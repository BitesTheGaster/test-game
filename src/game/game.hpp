#pragma once

#include "core/render/batcher.hpp"
#include "core/sim/fixed_timestep.hpp"
#include "core/sim/spatial_hash.hpp"
#include "game/components.hpp"
#include "game/content.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// Word-wraps `str` into lines of at most `maxChars` characters (word-based,
// single words longer than the limit overflow their own line). Shared between
// the level-up card renderer and unit tests.
std::vector<std::string> wrapWords(std::string_view str, std::size_t maxChars);

// Normalized input gathered by main() from SDL each frame.
struct FrameInput {
  float moveX = 0.0F; // -1..1
  float moveY = 0.0F; // -1..1, y-up
  bool choose1 = false;
  bool choose2 = false;
  bool choose3 = false;
  bool choose4 = false;
  bool choose5 = false; // picks a 5th card (from +1 choice items)
  bool restart = false;
  bool togglePause = false;
  bool testModeToggle = false; // T: open/close the weapon test mode
  bool heal = false;           // H: guaranteed 50% max-HP heal (cooldown-gated)
  bool bestiary = false;       // B: toggle the bestiary while paused
};

// Attack-speed formula: the final delay between shots is
//
//     delay = baseCooldown / (1 + totalFireRateBonus)
//
// where the bonus is the additive sum of every fire-rate source (global
// upgrades + per-weapon cards). Being additive in the denominator means
// stacking can never drive the delay to zero — it approaches 0 asymptotically
// (+100% halves the delay, +300% quarters it, ...).
inline float attackCooldown(float baseCooldown, float fireRateBonus) {
  return baseCooldown / std::max(0.05F, 1.0F + fireRateBonus);
}

// Mutable per-run player modifiers; upgraded through UpgradeDef::effect.
struct PlayerStats {
  float damageMul = 1.0F;
  // Additive fire-rate bonus. The final per-shot delay is
  // `base / (1 + fireRateBonus)` (see attackCooldown) — additive so stacking
  // can never reach infinite attack speed.
  float fireRateBonus = 0.0F;
  float speedMul = 1.0F;
  float pickupMul = 1.0F;
  float speed = 5.2F;
  float maxHp = 100.0F;
  float regen = 0.0F;
  int projAdd = 0;
  int pierceAdd = 0;

  // Defense: one curve yields both a flat and a percent reduction.
  float defense = 0.0F;
  // Lifesteal: chance-based, but it only rolls on a KILL (not on every hit).
  // L% = L% chance per slain enemy to heal `lifestealHeal` HP; at L >= 100 the
  // first point is guaranteed. Tying it to kills (instead of per-damage) is
  // what keeps a many-hit weapon (e.g. the Halo) from out-healing the fight:
  // the number of procs is bounded by kills, not by hit count.
  float lifesteal = 0.0F;
  int lifestealHeal = 1; // HP per proc (Vampiric Heart unique raises to 2)
  // Armor pierce: subtracted from an enemy's defense before mitigation, so
  // this is the "пробитие брони" stat — it is what lets a build cut through
  // the flat+percent defense curve that late/elite enemies grow into.
  float armorPierce = 0.0F;
  // Shield: regenerating damage buffer (see rules in game.hpp docs).
  float shieldMax = 0.0F;
  // XP gain multiplier (Scholar-style items).
  float xpMul = 1.0F;

  // Unique-item effects (each is a distinct mechanic):
  float spreadMul = 1.0F; // widens weapon volleys (fan item)
  // Inaccuracy from the Spreadshot unique: each projectile is offset by up to
  // this many radians (0.5236 = +/- 30 degrees).
  float aimJitter = 0.0F;
  int extraChoice = 0;    // +N level-up cards
  int rerollCharges = 0;  // +N extra rerolls (the base budget is already 1)
  float thornsDmg = 0.0F; // AoE burst around player on hit
  int adrenaline = 0;     // speed burst when HP is low
  int blackHole = 0;      // periodic enemy pull
  int chain = 0;          // every 3rd projectile hit chains lightning
  int bloodPrice = 0;     // every 20 kills, burst around player
  int iceBlood = 0;       // enemies that hit you get slowed
  int lastStand = 0;      // 1s of iframes when hit below 20% HP (cooldown)
  // Repulsion Field unique: enemies that strike the player are shoved away.
  float knockbackRetaliate = 0.0F;
  // Impact cards: multiplies every knockback the player deals (default 1).
  float knockbackMul = 1.0F;
};

struct UpgradeEffectResult {
  bool valid = false; // false => unknown effect id
  float heal = 0.0F;
  float shield = 0.0F; // instant shield granted on pick
};

// Applies one upgrade effect to stats. Shared between Game and unit tests.
UpgradeEffectResult applyUpgrade(PlayerStats& stats, std::string_view effect, float value);

// Defense formula: flat part (1 point per 5 defense) plus a percent part that
// approaches 50% as defense grows. Never makes damage negative.
float mitigateDamage(float raw, float defense);

// XP needed to advance from `level` to `level + 1`.
float xpForLevel(int level);

// AoE damage falloff: the more enemies a single blast catches, the less each
// one takes. 1 target = 100%, 2 = 90%, 3 = 81%, ... (0.9^(n-1)). Pierce reduces
// the falloff: at 20+ pierce there is no falloff at all. Clamped so a huge
// crowd still deals a floor of damage instead of underflowing.
float aoeFalloff(int enemiesHit, int pierce = 0);

// Enemy scaling curves, all pure so they can be unit-tested directly.
// Defense uses the same flat+percent curve as the player (mitigateDamage) and
// grows after a short grace period; higher tiers are tougher.
float enemyDefense(float simTime, int tier);
// Lifesteal resistance: 0.5 => the player's heal chance is halved.
float enemyLifestealResistance(float simTime, int tier, bool resistant);
// Knockback resistance: scales incoming push, grows with time.
float enemyKnockbackResistance(float simTime, int tier, bool resistant);
// How many traits a given tier rolls: elite = exactly 1, champions several,
// overlords even more. Grows a little with run time.
int traitsForTier(int tier, float simTime);

enum class RunState {
  Playing,
  LevelUp,
  Paused,
  GameOver,
};

// One level-up card: either an upgrade from content_ or a new weapon.
struct Choice {
  enum class Kind : std::uint8_t { Upgrade, Weapon } kind = Kind::Upgrade;
  int index = -1; // upgrade index or weapon index
};

class Game {
public:
  explicit Game(const Content& content, std::uint32_t seed = 1337);

  void reset();

  // Frame-level entry: input handling, fixed-step sim, camera.
  void advance(float frameDt, const FrameInput& input);

  void render(core::render::Batcher& batcher, float alpha);

  [[nodiscard]] RunState state() const { return state_; }
  [[nodiscard]] float simTime() const { return simTime_; }
  [[nodiscard]] int level() const { return level_; }
  [[nodiscard]] int kills() const { return kills_; }
  [[nodiscard]] float playerHp() const;
  [[nodiscard]] float playerMaxHp() const;
  [[nodiscard]] float shield() const { return shield_; }
  [[nodiscard]] float shieldMax() const { return stats_.shieldMax; }
  [[nodiscard]] float xp() const { return xp_; }
  [[nodiscard]] float xpNext() const { return xpNext_; }
  [[nodiscard]] std::size_t enemyCount() const;
  [[nodiscard]] const PlayerStats& stats() const { return stats_; }
  [[nodiscard]] PlayerStats& stats() { return stats_; }
  [[nodiscard]] const std::vector<Choice>& upgradeChoices() const { return choices_; }
  [[nodiscard]] int upgradeStacks(std::size_t upgradeIndex) const;
  [[nodiscard]] int rerollsUsed() const { return rerollsUsed_; }
  [[nodiscard]] bool milestoneOffer() const { return milestoneOffer_; }
  // True while the weapon test mode is open (T). Exposed for tests.
  [[nodiscard]] bool testMode() const { return testMode_; }
  // True while the opening 3-weapon pick is being offered.
  [[nodiscard]] bool choosingStarter() const { return choosingStarter_; }
  // True while the bestiary overlay is open (paused).
  [[nodiscard]] bool bestiaryOpen() const { return bestiaryOpen_; }

  // Test/debug hooks.
  void grantXp(float amount);
  // Test helper: add weapon by index (bypasses normal level-up flow)
  void testAddWeapon(int defIndex) {
    starterChoicePending_ = false; // tests set up weapons directly
    addWeapon(defIndex);
  }
  // Test helper: drop owned weapons + their persistent entities (orbit blades).
  void testClearWeapons();
  // Test helper: place a stationary, high-HP enemy at a world position.
  void testSpawnEnemyAt(float x, float y);
  // Test helper: place an elite/champion/overlord enemy (tier 1..3) so tests
  // can inspect the guaranteed stat boosts, resistances and trait flags.
  void testSpawnTieredEnemyAt(float x, float y, int tier, std::uint32_t traits = 0);
  // Test helper: tier of every live Enemy (0 normal, 1 elite, 2 champion, 3
  // overlord). Order unspecified.
  [[nodiscard]] std::vector<int> testEnemyTiers() const;
  // Test helper: number of set trait flags for every live Enemy. Order
  // unspecified.
  [[nodiscard]] std::vector<int> testEnemyTraitCounts() const;
  // Test helper: freeze wave spawning so tests control the enemy pool exactly.
  void testDisableWaves() { wavesEnabled_ = false; }
  // Test helper: apply a per-weapon upgrade (same path as weapon Focus cards).
  void testAddWeaponUpgrade(int slot, std::string_view effect, float value) {
    applyWeaponEffect(slot, effect, value);
  }
  // Test helper: current HP of the first Enemy in the registry (-1 if none).
  [[nodiscard]] float testFirstEnemyHp() const;
  // Test helper: damage-aura radius/dps of the first Enemy carrying an aura
  // (0 if none), so the per-tier aura sizes can be asserted.
  [[nodiscard]] float testFirstAuraRadius() const;
  [[nodiscard]] float testFirstAuraDps() const;
  // Test helper: true when the first Enemy can shoot (has the Archer trait).
  [[nodiscard]] bool testFirstCanShoot() const;
  // Test helper: overwrite the first enemy's current HP (death-boundary tests).
  void testSetFirstEnemyHp(float hp);
  // Test helper: route a raw damage packet through applyEnemyDamage() on the
  // first enemy (so defense mitigation and the lethal rule are exercised).
  void testDamageFirstEnemy(float dmg);
  // Test helper: distance from the player to the first Enemy (-1 if none).
  [[nodiscard]] float testFirstEnemyDistToPlayer() const;
  // Test helper: current speed (velocity magnitude) of every live Enemy.
  [[nodiscard]] std::vector<float> testEnemySpeeds() const;
  // Test helper: current HP of every live Enemy (order unspecified — use for
  // sums / range checks, not positional assertions).
  [[nodiscard]] std::vector<float> testEnemyHps() const;
  // Test helper: contact radius of every live bounce projectile (the eternal
  // Void Orb grows its size with the projectile stat).
  [[nodiscard]] std::vector<float> testBounceRadii() const;
  // Test helper: the equipped weapon ids, oldest first (e.g. for test-mode
  // snapshot/restore assertions). Empty when no weapons are equipped.
  [[nodiscard]] std::vector<std::string> armedWeaponIds() const;
  // Test helper: angular position (radians) of the first orbit blade, so a
  // test can measure how fast the dagger spins. -1 if no blades exist.
  [[nodiscard]] float testOrbitBladeAngle() const;
  // Test helper: circular angular gaps (radians) between a slot's orbit
  // blades, sorted. All gaps equal 2*pi/count when blades are evenly spaced.
  [[nodiscard]] std::vector<float> testOrbitBladeGaps(int slot) const;
  // Test helper: current invulnerability window (seconds) and what a base
  // window becomes after the defense scaling.
  [[nodiscard]] float testIframes() const { return iframes_; }
  [[nodiscard]] float testIframeDuration(float base) const { return iframeDuration(base); }
  // Test helper: bestiary kill count / tier bitmask for a content enemy index.
  [[nodiscard]] int testBestiaryKills(int def) const {
    if (def < 0 || static_cast<std::size_t>(def) >= bestiaryKills_.size()) return 0;
    return bestiaryKills_[static_cast<std::size_t>(def)];
  }
  [[nodiscard]] int testBestiaryTiers(int def) const {
    if (def < 0 || static_cast<std::size_t>(def) >= bestiaryTiers_.size()) return 0;
    return static_cast<int>(bestiaryTiers_[static_cast<std::size_t>(def)]);
  }
  // Test helper: deal direct damage to the player through the normal path.
  void testHurtPlayer(float amount) { hurtPlayer(amount); }
  // Test helper: set the player's current HP (for low-HP unique tests).
  void testSetPlayerHp(float hp);
  // Test helper: kill the first live Enemy through the normal kill path (so
  // the bestiary records it). No-op when the registry has no enemies.
  void testKillFirstEnemy();
  // Highest tier slain this run (0 none, 1 elite, 2 champion, 3 overlord), and
  // the enemy def index that achieved it. Drives the bestiary's "strongest
  // kill" line.
  [[nodiscard]] int strongestKilledTier() const { return strongestKilledTier_; }
  [[nodiscard]] int strongestKilledDef() const { return strongestKilledDef_; }
  // Bitmask (1<<tier) of every tier killed at least once this run. Used both by
  // the bestiary and to unlock the elite/champion/overlord player outlines.
  [[nodiscard]] std::uint8_t tierKillMask() const { return tierKillMask_; }
  // --- Enemy-type retirement -------------------------------------------------
  // When an overlord of a given enemy type spawns, that type is "retired": it
  // no longer spawns, which stops the same overlord from repeating forever.
  // The 3 most recently unlocked types are exempt so the spawn pool can never
  // dry up (there is always something new to fight).
  void retireEnemyType(int def);
  [[nodiscard]] bool typeRetired(int def) const;
  // Full spawn-eligibility rule used by pickDef (unlocked AND not retired,
  // unless it is one of the 3 most recent types).
  [[nodiscard]] bool typeCanSpawn(int def) const;
  // Test helpers for the retirement rule.
  void testRetireType(int def) { retireEnemyType(def); }
  [[nodiscard]] bool testTypeRetired(int def) const { return typeRetired(def); }
  [[nodiscard]] bool testTypeCanSpawn(int def) const { return typeCanSpawn(def); }
  // Test helper: whether a def index is among the 3 most recently unlocked
  // types (the ones exempt from retirement).
  [[nodiscard]] bool testTypeIsRecent(int def) const;

  // Debug introspection for tests: how many live entities each attack type has.
  struct DebugCounts {
    std::size_t projectiles = 0;
    std::size_t orbitBlades = 0;
    std::size_t halos = 0;
    std::size_t bombs = 0;
    std::size_t boomerangs = 0;
    std::size_t bounces = 0;
    std::size_t beams = 0;
    std::size_t sweeps = 0;
    std::size_t zones = 0;
    std::size_t chains = 0;
    std::size_t novas = 0;
  };
  [[nodiscard]] DebugCounts debugCounts() const;
  [[nodiscard]] std::size_t debugEnemyCount() const;

private:
  // Owned weapons (fixed slots, no allocation on the hot path).
  struct WeaponSlot {
    int def = -1;
    AttackType attackType = AttackType::Projectile;
    float cooldown = 0.5F;
    float timer = 0.0F;
    float damage = 5.0F;
    int projectiles = 1;
    float speed = 12.0F;
    float life = 1.4F;
    int pierce = 0;
    float spread = 0.16F;
    core::render::Color color{1.0F, 0.95F, 0.55F, 1.0F};

    // Cone
    float coneAngle = 0.8F;
    float coneRange = 2.5F;
    float coneTickRate = 0.1F;
    float coneTimer = 0.0F;        // for continuous cone damage

    // Orbit
    float orbitRadius = 1.2F;
    float orbitSpeed = 2.0F;
    int orbitCount = 2;
    float orbitAngle = 0.0F;       // current rotation angle

    // Bomb
    float bombArcHeight = 2.0F;
    float bombExplodeRadius = 1.5F;
    float bombKnockback = 3.0F;
    float bombFuse = 0.0F;

    // Boomerang
    float boomerangRange = 4.0F;
    float boomerangReturnSpeed = 1.5F;

    // Bounce
    int bounceCount = 3;
    float bounceRange = 2.5F;
    float bounceDamageMul = 0.7F;
    bool bounceInfinite = false; // eternal orb: no life/bounce budget
    entt::entity liveBounce = entt::null; // the one eternal orb (single, grows)

    // Beam
    float beamRange = 8.0F;
    float beamWidth = 0.3F;
    float beamDuration = 0.15F;

    // Halo (evolution)
    float haloKnockback = 0.0F;

    // Sweep
    float sweepAngle = 3.14F;
    float sweepRadius = 2.0F;
    float sweepKnockback = 2.0F;

    // Zone
    float zoneRadius = 1.2F;
    float zoneDuration = 4.0F;
    float zoneDps = 15.0F;
    int zoneMaxPools = 3;

    // Chain (evolution)
    float chainJumpRange = 2.5F;
    int chainMaxJumps = 4;
    float chainDamageMul = 0.6F;

    // Nova (evolution)
    float novaMaxRadius = 4.0F;
    float novaExpandSpeed = 3.0F;
    float novaDamagePerTick = 25.0F;
    float novaTickRate = 0.15F;
    float novaRadius = 0.0F;       // current radius
    float novaTimer = 0.0F;        // tick timer
    bool novaActive = false;       // whether nova is expanding

    // General projectile fields (for projectile/boomerang/bounce)
    float area = 0.0F;
    float strength = 0.0F;
    bool homing = false;
    int bounces = 0;

    // Per-weapon fire-rate bonus (additive; see attackCooldown()).
    float cdBonus = 0.0F;
    // Sweep: the arc is centered this far in front of the player (scythe).
    float sweepLead = 0.0F;
    // Weapon-unique modifiers.
    float uniqueHeal = 0.0F; // scythe "Reaper's Harvest": heal HP per kill
    int beamSplit = 0;       // beam "Prism Lance": beam count multiplier
  };
  static constexpr int kMaxWeapons = 5;

  // Pending spawn telegraphs (enemies walk in after a short warning).
  struct PendingSpawn {
    float x = 0.0F;
    float y = 0.0F;
    float t = 0.0F;
    int def = 0;
    float hpMul = 1.0F;
    float touchMul = 1.0F;
    float speedMul = 1.0F;
    float xpMul = 1.0F;
    std::uint32_t traits = TraitNone;
    std::uint8_t tier = 0;
  };

  // Elite name labels are no longer drawn; elites/champions/overlords are
  // marked by a coloured outline instead (see render()).

  // Particles: fixed ring buffer of PODs.
  struct Particle {
    float x, y, vx, vy, life, maxLife, size;
    core::render::Color color;
  };

  void fixedUpdate();
  void spawnWave();
  void processPendingSpawns();
  void fireWeapons();
  void movePlayer();
  void updateEnemies();
  void updateProjectiles();
  void updateOrbitBlades();
  void updateHaloBeams();
  void updateBombProjectiles();
  void updateBoomerangProjectiles();
  void updateBounceProjectiles();
  void updateBeamEffects();
  void updateSweepEffects();
  void updateZoneEffects();
  void updateChainLightning();
  void updateNovaRing();
  void updateEnemyShots();
  void updatePickups();
  void updateShield();
  void updateUniqueEffects();
  void buildSpatialHash();
  void enterLevelUp();
  // Opening pick: the run begins with no weapon, offering three starter
  // weapons (wand/dagger/crossbow) instead of a random one.
  void enterStarterPick();
  void buildStarterChoices();
  void buildChoices();
  void chooseUpgrade(int slot);
  void reroll();
  void addWeapon(int defIndex);
  void syncOrbitBlades(int slot); // add orbit blades up to the current count
  void syncHaloBeams(int slot);   // add halo beams up to the current count
  void applyWeaponEffect(int slotIndex, std::string_view effect, float value);
  int findWeaponSlot(std::string_view weaponId) const;
  bool ownsWeapon(int defIndex) const;
  // Collects every new weapon the player could be offered (evolutions first,
  // then normals), shuffled. Used to offer several weapon cards at once.
  std::vector<int> collectWeaponGrants();
  void spawnEnemy(const PendingSpawn& p);
  // Recomputes recentTypes_: the 3 highest-unlock_at enemy types, which are
  // exempt from overlord retirement so the spawn pool never runs dry.
  void refreshRecentTypes();
  void applyEnemyDamage(entt::entity e, float dmg);
  void killEnemy(entt::entity e);
  void chainBolt(float x, float y, float dmg);
  // Lifesteal now rolls on a KILL (not per hit) so a many-hit weapon cannot
  // out-heal the fight; see the definition for the full rationale.
  void tryLifestealOnKill(entt::entity source);
  void applyKnockback(entt::entity e, float angle, float force);
  // Repulsion Field unique: shove an enemy that just damaged the player.
  void retaliateKnockback(entt::entity attacker);
  void explodeBomb(entt::entity bomb, const BombProjectile& bp, float x, float y);
  void hurtPlayer(float amount);
  void damagePlayerDirect(float amount); // no thorns trigger (DoT auras)
  void spawnParticles(float x, float y, core::render::Color c, int count, float speed);
  void renderPlayerStats(core::render::Batcher& b, float px, float py);
  // Bestiary overlay (paused, B): discovered enemy types, their stats,
  // appearance, kill counts and which elite+ variants have been slain.
  void renderBestiary(core::render::Batcher& b, float px, float py);
  // Current global enemy scaling (shared by spawning and the bestiary).
  void currentScales(float& hp, float& speed, float& touch) const;
  // Player invulnerability window, nudged up ~1% per 5 defense.
  [[nodiscard]] float iframeDuration(float base) const;
  // Weapon test mode (T): cycle every weapon incl. evolutions, apply a boosted
  // build to see stat couplings, toggle waves, close to restore the run.
  void enterTestMode();
  void exitTestMode();
  void setTestWeapon(int defIndex);
  void toggleTestBoost();
  void spawnTestFodder();

  WeaponSlot weapons_[kMaxWeapons];
  int weaponCount_ = 0;
  // Weapon test mode state: snapshot of the real run while testing.
  bool testMode_ = false;
  bool testBoosted_ = false;
  int testWeaponIdx_ = 0;
  int savedWeaponCount_ = 0;
  WeaponSlot savedWeapons_[kMaxWeapons];
  PlayerStats savedStats_;
  std::vector<PendingSpawn> pending_;

  const Content& content_;
  entt::registry registry_;
  core::sim::SpatialHash hash_{1.0F};
  core::sim::FixedTimestep timestep_{1.0 / 60.0, 2};
  std::mt19937 rng_;

  RunState state_ = RunState::Playing;
  float simTime_ = 0.0F;
  int level_ = 1;
  float xp_ = 0.0F;
  float xpNext_ = 6.0F;
  int kills_ = 0;
  float spawnTimer_ = 0.0F;
  float hordeTimer_ = 120.0F;  // first horde burst arrives at ~2 minutes
  bool wavesEnabled_ = true; // tests may freeze spawning for determinism
  float iframes_ = 0.0F;
  float healCd_ = 0.0F; // H heal cooldown remaining
  bool starterChoicePending_ = false; // show the opening 3-weapon pick
  bool choosingStarter_ = false;      // currently in that opening pick
  bool bestiaryOpen_ = false;         // bestiary overlay (paused)
  float lastStandCd_ = 0.0F;          // low-HP iframe unique cooldown
  std::vector<int> bestiaryKills_;    // kills per content enemy index
  std::vector<std::uint8_t> bestiaryTiers_; // bitmask of tiers killed
  // Enemy types retired by an overlord spawn (bit per content enemy index).
  // The 3 most recently unlocked types stay eligible regardless, so the pool
  // can never empty out.
  std::vector<std::uint8_t> retiredTypes_;
  // Bit per type: is this one of the 3 most recently unlocked? Recomputed as
  // more types unlock over the run.
  std::vector<std::uint8_t> recentTypes_;
  // Strongest enemy killed this run: the tier and the enemy def that reached it.
  int strongestKilledTier_ = 0;
  int strongestKilledDef_ = -1;
  // Bit per tier (1<<tier) of tiers killed at least once this run.
  std::uint8_t tierKillMask_ = 0;
  float moveX_ = 0.0F; // latched input for fixed steps
  float moveY_ = 0.0F;

  // Shield state (regen delay resets whenever damage is absorbed).
  float shield_ = 0.0F;
  float shieldDelay_ = 0.0F;

  // Unique-item state.
  int rerollsUsed_ = 0; // free rerolls consumed this level-up
  bool milestoneOffer_ = false;
  float poison_ = 0.0F; // venomous DoT timer on the player
  int chainCounter_ = 0;
  int bloodKills_ = 0;
  float blackHoleTimer_ = 8.0F;
  float adrenalineCd_ = 0.0F;
  bool adrenalineActive_ = false;

  PlayerStats stats_;
  std::vector<int> stacks_;    // per-upgrade stack counts
  std::vector<Choice> choices_; // level-up cards

  entt::entity player_ = entt::null;

  // Camera (frame-level, not simulated).
  float camX_ = 0.0F;
  float camY_ = 0.0F;
  float zoom_ = 48.0F;

  // Scratch buffers, reused every tick (no hot-path allocation).
  std::vector<float> scratchX_, scratchY_;
  std::vector<std::uint32_t> scratchId_;
  std::vector<entt::entity> destroyQueue_;

  std::vector<Particle> particles_;
  static constexpr std::size_t kMaxParticles = 4096;
  std::size_t particleCursor_ = 0;
};

} // namespace game
