#pragma once

#include "core/render/batcher.hpp"
#include "core/sim/fixed_timestep.hpp"
#include "core/sim/spatial_hash.hpp"
#include "game/components.hpp"
#include "game/content.hpp"
#include "game/profile.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// The game's title, and the single line the main menu puts under it.
inline constexpr const char* kGameName = "TEST GAME";
inline constexpr const char* kGameSubtitle = "";

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
  bool testShop = false;       // E: open/close the sandbox item picker
  bool testInvuln = false;     // I: toggle sandbox immortality
  bool testKill = false;       // X: kill the player on demand
  bool testTime = false;       // F: cycle the sandbox difficulty timer
  // Note: `restart` (R) is repurposed INSIDE the sandbox as "max every item".
  bool heal = false;           // H: guaranteed 50% max-HP heal (cooldown-gated)
  bool bestiary = false;       // B: toggle the bestiary while paused
  // --- Active abilities (always available, cooldown-gated) --------------------
  // Three buttons, no menu, no cards required: the build decides how they are
  // tuned, the player decides when to spend them. See the "Active abilities"
  // section in docs/mechanics.md.
  bool abilityBlink = false; // J: Phase Dash — teleport along your movement
  bool abilityBurst = false; // K: Overload — knock everything around you away
  bool abilitySlow = false;  // L: Stasis — the world slows down, you do not
  // --- Main menu --------------------------------------------------------------
  bool menuUp = false;    // Up / W
  bool menuDown = false;  // Down / S
  bool menuLeft = false;  // Left / A
  bool menuRight = false; // Right / D
  bool menuConfirm = false; // Enter / Space: activate the highlighted row
  // F1: open/close the in-game manual. Legal from the main menu, from a run and
  // from the pause screen, because a manual you can only read at the start is a
  // manual nobody reads.
  bool manualToggle = false;
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
  // Shield refill SPEED and how long the "out of combat" wait before it starts.
  // Both used to be bare constants, which made a shield build a one-card trick:
  // the pool grew but the rate and the downtime did not, so a burst build
  // simply re-took the same chunk of HP five seconds later. Cards can now scale
  // the rate and shorten the delay (kShieldRegenRate / kShieldRegenDelay are the
  // 1.0x / 0.0s baselines).
  float shieldRegenMul = 1.0F;
  float shieldRegenDelay = 0.0F;
  // XP gain multiplier (Scholar-style items).
  float xpMul = 1.0F;

  // Unique-item effects (each is a distinct mechanic):
  float spreadMul = 1.0F; // widens weapon volleys (fan item)
  // Inaccuracy from the Spreadshot unique: each projectile is offset by up to
  // this many radians (0.5236 = +/- 30 degrees).
  float aimJitter = 0.0F;
  int extraChoice = 0;    // +N level-up cards
  int rerollCharges = 0;  // +N extra rerolls (the base budget is already 1)
  // Extra weapon slots. The arsenal starts at kBaseWeapons (4); the "Arsenal
  // Core" card adds one slot per stack (3 stacks) and the "Hollow Chamber"
  // unique adds the last one, so a fully stacked build holds kMaxWeapons (8).
  int weaponSlots = 0;
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
  // --- Momentum ----------------------------------------------------------------
  // The kill chain is the game's dynamic layer: it is NOT a flat stat you buy
  // and forget, it is a meter that fills while you keep killing and collapses
  // the moment you stop or get hit. The values below are what one card can add
  // to it; see Game::updateMomentum for how the meter turns into multipliers.
  float momentumDamage = 1.5F;  // % damage per stack (default 1.5 => +30% at cap)
  float momentumRate = 0.5F;    // % fire rate per stack
  float momentumSpeed = 0.0F;   // % move speed per stack (needs a card to matter)
  int momentumMax = 20;         // chain length that counts
  float momentumWindow = 3.0F;  // seconds of not killing before the chain drops
  float momentumGain = 1.0F;    // stacks per kill
  // --- Active abilities (J / K / L) -------------------------------------------
  // Three buttons that exist in every run from the first second — no unlock, no
  // card, no level. They are what the player reaches for when a build is done
  // growing and the fight has to be solved with positioning instead of numbers,
  // and they are the answer to "the run got boring": something to spend.
  // Cooldowns live in the Game (see abilityCooldownRemaining); these fields only
  // hold what the cards change.
  float abilityCdMul = 1.0F;   // multiplies all three cooldowns (cards lower it)
  float blinkDist = 3.2F;      // Phase Dash: teleport distance
  float blinkIframes = 0.35F;  // seconds of invulnerability the dash grants
  float burstRadius = 3.0F;    // Overload: blast radius
  float burstDamage = 45.0F;   // Overload: damage before damageMul
  float burstKnockback = 8.0F; // Overload: shove strength
  float stasisDuration = 2.5F; // Stasis: window length
  float stasisSlow = 0.35F;    // Stasis: enemy time multiplier while it is up
  int abilityEcho = 0;         // 1 = every ability also fires a 40% Overload
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
  enum class Kind : std::uint8_t {
    Upgrade,
    Weapon,
    // Fallback card shown when every real upgrade is maxed or inapplicable.
    // Without it a level-up screen can be left with no usable card and the run
    // is stuck forever.
    Skip,
  };
  Kind kind = Kind::Upgrade;
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
  // Sandbox switches.
  [[nodiscard]] bool testInvulnerable() const { return testMode_ && testInvuln_; }
  [[nodiscard]] int testTimeScale() const { return testMode_ ? testTimeScale_ : 1; }
  [[nodiscard]] bool testShopOpen() const { return testMode_ && testShopOpen_; }
  [[nodiscard]] int testShopCursor() const { return testShopCursor_; }
  // True while the opening 3-weapon pick is being offered.
  [[nodiscard]] bool choosingStarter() const { return choosingStarter_; }
  // True while the bestiary overlay is open (paused).
  [[nodiscard]] bool bestiaryOpen() const { return bestiaryOpen_; }

  // --- Profile (skins / outline unlocks) -------------------------------------
  // The profile is OWNED BY THE CALLER (main() loads it from disk and keeps it
  // alive for the process lifetime). Game only mutates it in place — it never
  // touches the filesystem — so tests can hand in a plain local object and
  // assert on the results.
  // Attaching a profile repaints the player immediately: the constructor's
  // reset() already ran with no profile, so a saved skin would otherwise not
  // show up until the player changed it in the menu.
  void setProfile(Profile* profile) {
    profile_ = profile;
    syncedUnlocks_ = 0;
    applyProfileToPlayer();
  }
  [[nodiscard]] Profile* profile() const { return profile_; }
  // Test helper: the player's rendered body colour, so tests can assert that
  // the profile's skin actually reaches the Sprite.
  [[nodiscard]] core::render::Color testPlayerColor() const;
  // Test helper: the resolved outline colour (alpha 0 when no outline is worn).
  [[nodiscard]] core::render::Color testOutlineColor() const { return outlineColor_; }
  // Pushes newly earned tier unlocks (from kills) into the profile. Called
  // automatically on every kill; safe to call again.
  void syncProfileUnlocks();
  // True when the profile changed since the last consumeProfileDirty(), so
  // main() knows it should write it back to disk. Returns and clears the flag.
  bool consumeProfileDirty();

  // --- Main menu --------------------------------------------------------------
  // The menu is a Game-level overlay so it can reuse the same renderer. Tests
  // never open it, so `advance()` behaves exactly as before by default.
  void openMenu() { menuOpen_ = true; }
  void closeMenu() { menuOpen_ = false; }
  [[nodiscard]] bool menuOpen() const { return menuOpen_; }
  // Highlighted row, exposed for tests and for the renderer.
  [[nodiscard]] int menuSelection() const { return menuSelection_; }
  // Set when the player picks QUIT in the menu; main() watches this to break
  // its loop. Cleared by consumeQuitRequest().
  [[nodiscard]] bool quitRequested() const { return quitRequested_; }
  bool consumeQuitRequest() {
    const bool q = quitRequested_;
    quitRequested_ = false;
    return q;
  }

  // --- In-game manual (F1) ---------------------------------------------------
  // The same documentation docs/ ships, trimmed to what the bitmap font can
  // draw and split into pages. It is a Game-level overlay so it can be opened
  // from the main menu, from a live run and from the pause screen.
  void openManual();
  void closeManual();
  [[nodiscard]] bool manualOpen() const { return manualOpen_; }
  [[nodiscard]] std::size_t manualPageIndex() const { return manualPage_; }
  [[nodiscard]] std::size_t manualPageCount() const { return content_.manual.size(); }
  void nextManualPage();
  void prevManualPage();
  // Page id shown right now, or "" when the build ships no manual.
  [[nodiscard]] std::string manualPageId() const;

  // --- Progress reset (main menu) --------------------------------------------
  // Wipes the attached profile back to first-launch defaults: default skin, no
  // outline, no earned unlocks, and repaints the player. Marks the profile
  // dirty so main() persists the wipe.
  //
  // Two-step on purpose. `beginProgressReset()` arms it and returns true; the
  // actual wipe only happens on `confirmProgressReset()`, which must be a
  // SEPARATE call. A destructive action that fires on the same Enter that
  // selected it is one stray keypress away from wiping hours of unlocks.
  void beginProgressReset();
  [[nodiscard]] bool progressResetArmed() const { return resetArmed_; }
  // Wipes and disarms. Returns false when nothing was armed.
  bool confirmProgressReset();
  void cancelProgressReset() { resetArmed_ = false; }

  // --- Defense clock ----------------------------------------------------------
  // A shield is a pool AND a clock. These are the 1.0x rate and the 0.0s
  // baseline; the shield_add cards grow the pool, shield_regen and shield_delay
  // change these two, and the character sheet prints the result.
  static constexpr float kShieldRegenRate = 10.0F;  // HP/s once out of combat
  static constexpr float kShieldRegenDelay = 4.0F;   // seconds without damage
  // The delay is floored so no build can make the pool permanent.
  static constexpr float kShieldRegenDelayFloor = 0.5F;

  // --- Read-only build readouts ----------------------------------------------
  // The numbers a card is supposed to move, exposed so tests (and the
  // character sheet) can read the same value the simulation does rather than
  // re-deriving it from the card's effect id.
  // % fire rate the chain grants per live stack (see momentumFireRate() for the
  // value the simulation actually applies, which is stacks * this).
  [[nodiscard]] float momentumRatePerStack() const { return stats_.momentumRate; }
  [[nodiscard]] float blinkDistance() const { return stats_.blinkDist; }
  [[nodiscard]] float burstRadius() const { return stats_.burstRadius; }
  [[nodiscard]] float burstDamage() const { return stats_.burstDamage; }
  [[nodiscard]] float stasisDurationMax() const { return stats_.stasisDuration; }
  [[nodiscard]] float stasisSlowFactor() const { return stats_.stasisSlow; }
  // Shield refill HP/s and the seconds it waits after a hit before starting.
  // The delay is floored at kShieldRegenDelayFloor, so no build can make the
  // pool permanent. Exposed (not inlined into updateShield) so the character
  // sheet and the tests read the same number the simulation does.
  [[nodiscard]] float shieldRegenRate() const {
    return kShieldRegenRate * std::max(0.1F, stats_.shieldRegenMul);
  }
  [[nodiscard]] float shieldRegenDelay() const;
  // Base arsenal size + any "+1 weapon slot" cards. Weapons cannot be added
  // past this, and the level-up offer stops appearing once it is full. Clamped
  // to kMaxWeapons so a hand-edited content file cannot overflow the array.
  [[nodiscard]] int weaponCap() const {
    return std::min(kMaxWeapons, kBaseWeapons + std::max(0, stats_.weaponSlots));
  }
  // The arsenal starts at 4 weapons. Slot cards add more: "Arsenal Core" gives
  // +1 per stack (3 stacks) and "Hollow Chamber" is the one-shot +1 that takes
  // the build to the full eight. Public so a test can assert the shipped cap
  // against the storage array rather than hard-coding a number.
  static constexpr int kBaseWeapons = 4;
  static constexpr int kMaxWeapons = 8;
  // Hard cap on "+1 weapon slot" stacks, whatever the content says. Content can
  // ship fewer (today: 3 + 1); it can never ship more than this.
  static constexpr int kMaxSlotCards = kMaxWeapons - kBaseWeapons;

  // Test/debug hooks.
  void grantXp(float amount);
  // Test helper: add weapon by index (bypasses normal level-up flow)
  void testAddWeapon(int defIndex) {
    starterChoicePending_ = false; // tests set up weapons directly
    addWeapon(defIndex);
  }
  // Test helper: a comparable copy of one weapon slot's whole CONFIGURABLE stat
  // block, so a test can assert that taking a card actually MOVED something
  // instead of silently doing nothing. Comparing two snapshots with == covers
  // every field at once, so a new WeaponSlot field needs no test edit. Pure
  // runtime state (timers, the live bounce handle, the nova radius) is left out
  // on purpose: it ticks on its own, so including it would make every
  // comparison fail for reasons that have nothing to do with the card.
  struct WeaponSnapshot {
    int def = 0;
    int attackType = 0;
    float cooldown = 0;
    float damage = 0;
    int projectiles = 0;
    float speed = 0;
    float life = 0;
    int pierce = 0;
    float spread = 0;
    float coneAngle = 0;
    float coneRange = 0;
    float coneTickRate = 0;
    float orbitRadius = 0;
    float orbitSpeed = 0;
    int orbitCount = 0;
    float bombArcHeight = 0;
    float bombExplodeRadius = 0;
    float bombKnockback = 0;
    float bombFuse = 0;
    float boomerangRange = 0;
    float boomerangReturnSpeed = 0;
    int bounceCount = 0;
    float bounceRange = 0;
    float bounceDamageMul = 0;
    bool bounceInfinite = 0;
    float beamRange = 0;
    float beamWidth = 0;
    float beamDuration = 0;
    float haloKnockback = 0;
    float sweepAngle = 0;
    float sweepRadius = 0;
    float sweepKnockback = 0;
    float zoneRadius = 0;
    float zoneDuration = 0;
    float zoneDps = 0;
    int zoneMaxPools = 0;
    float chainJumpRange = 0;
    int chainMaxJumps = 0;
    float chainDamageMul = 0;
    float novaMaxRadius = 0;
    float novaExpandSpeed = 0;
    float novaDamagePerTick = 0;
    float novaTickRate = 0;
    float area = 0;
    float strength = 0;
    bool homing = 0;
    int bounces = 0;
    float cdBonus = 0;
    float sweepLead = 0;
    float uniqueHeal = 0;
    int beamSplit = 0;
    float vortexRadius = 0;
    float vortexReach = 0;
    float vortexPull = 0;
    float vortexOrbit = 0;
    float vortexOrbitSpeed = 0;
    float vortexTickRate = 0;
    float prismRange = 0;
    float prismWidth = 0;
    int prismMaxTargets = 0;
    float prismRicochet = 0;
    float lureRadius = 0;
    float lureReach = 0;
    float lurePull = 0;
    float lureDps = 0;
    float lureDuration = 0;
    float lureTickRate = 0;
    int lureMaxBeacons = 0;
    [[nodiscard]] bool operator==(const WeaponSnapshot& other) const;
    [[nodiscard]] bool operator!=(const WeaponSnapshot& other) const {
      return !(*this == other);
    }
  };
  [[nodiscard]] WeaponSnapshot testWeaponSnapshot(int slotIndex) const;
  // Test helper: drop owned weapons + their persistent entities (orbit blades).
  void testClearWeapons();
  // Test helper: place a stationary, high-HP enemy at a world position.
  void testSpawnEnemyAt(float x, float y);
  // Test helper: place an elite/champion/overlord enemy (tier 1..3) so tests
  // can inspect the guaranteed stat boosts, resistances and trait flags.
  // `def` selects the content enemy type (default: the first one).
  void testSpawnTieredEnemyAt(float x, float y, int tier, std::uint32_t traits = 0,
                              int def = 0);
  // Test helper: tier of every live Enemy (0 normal, 1 elite, 2 champion, 3
  // overlord). Order unspecified.
  [[nodiscard]] std::vector<int> testEnemyTiers() const;
  // Test helper: number of set trait flags for every live Enemy. Order
  // unspecified.
  [[nodiscard]] std::vector<int> testEnemyTraitCounts() const;
  // Test hook: freeze wave spawning so tests control the enemy pool exactly.
  void testDisableWaves() { wavesEnabled_ = false; }
  // Test hook: jump the run clock, so time-gated systems (spawn tiers, scaling
  // curves) can be tested without simulating minutes of real time.
  void testSetSimTime(float seconds) { simTime_ = seconds; }
  // --- Weapon test sandbox (T) — public controls, used by input + tests ------
  void enterTestMode();
  void exitTestMode();
  void setTestWeapon(int defIndex);
  void toggleTestBoost() { toggleTestBoost_(); }
  void toggleTestInvuln() { testInvuln_ = !testInvuln_; }
  // Difficulty-timer presets: 1x, 4x, 10x, 20x, then back to 1x.
  void cycleTestTimeScale();
  void toggleTestShop();
  // Test hook: grant one stack of an upgrade straight into the sandbox.
  bool testGrantUpgrade(int index) { return grantTestUpgrade(index); }
  // Test hook: the sandbox "max everything" cheat.
  void testMaxAllItems();
  // Test hook: put the player at death's door through the normal damage path.
  void testKillPlayer();
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
  // Test helper: force the first enemy's knockback resistance (and give it the
  // EnemyTraits component it does not have by default). Lets a test compare how
  // hard a shove or a suction field moves a soft enemy versus a boss.
  void testSetFirstEnemyKnockbackRes(float res);
  // --- Momentum introspection (HUD + tests) -----------------------------------
  [[nodiscard]] int killStreak() const { return streak_; }
  [[nodiscard]] float killStreakTimer() const { return streakTimer_; }
  [[nodiscard]] float momentumDamageMul() const { return momentumDamageMul_; }
  [[nodiscard]] float momentumFireRate() const { return momentumRate_; }
  [[nodiscard]] float momentumSpeedMul() const { return momentumSpeedMul_; }
  // Test hook: set the chain directly.
  void testSetStreak(int streak) {
    streak_ = std::max(0, streak);
    streakTimer_ = 0.0F;
  }
  // Test helper: route a raw damage packet through applyEnemyDamage() on the
  // first enemy (so defense mitigation and the lethal rule are exercised).
  void testDamageFirstEnemy(float dmg);
  // Test helper: wound the PLAYER by `amount` HP, bypassing defense (the point
  // is to set up a heal test, not to measure mitigation). Clamped so the run
  // never ends from a test's own bookkeeping.
  void testDamagePlayer(float amount);
  // Test helper: distance from the player to the first Enemy (-1 if none).
  [[nodiscard]] float testFirstEnemyDistToPlayer() const;
  // Test helper: distance from the first Enemy to the NEAREST live Vortex zone
  // (-1 if there is no enemy or no zone). This is what proves the suction zones
  // actually drag prey into their cores.
  [[nodiscard]] float testFirstEnemyDistToVortex() const;
  // Test helper: distance from the first enemy to the NEAREST planted Lure bell
  // (-1 if there is no enemy or no bell). This is what proves the Grave Bell
  // actually taunts prey into its core instead of just sitting there.
  [[nodiscard]] float testFirstEnemyDistToLure() const;
  // Test helpers: the player's world position (Blink moves it).
  [[nodiscard]] float testPlayerX() const;
  [[nodiscard]] float testPlayerY() const;
  // Test helper: the aim direction of every live beam, in radians. Used to prove
  // the Solar Lance's "Prism Lance" unique really spreads into front/left/right
  // instead of stacking three beams on the same line.
  [[nodiscard]] std::vector<float> testBeamAngles() const;
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
  // Test helper: the names of every card in content that is scoped to this
  // weapon id (focus cards and weapon uniques alike), in content order. Lets a
  // test assert the "every weapon has cards" rule against the same roster the
  // game offers.
  [[nodiscard]] std::vector<std::string> collectWeaponCards(std::string_view weaponId) const;
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
  // --- Adaptive tribunal director (test + HUD introspection) -----------------
  // True while a tier is allowed to spawn at all. Tier 0 always, tier 1 from
  // 45s on, tier 2 once elites are routine, tier 3 once champions are.
  [[nodiscard]] bool tierUnlocked(int tier) const;
  // Per-member spawn chance for a tier right now (0 while the tier is shut).
  [[nodiscard]] float tierSpawnChance(int tier) const;
  // Decaying "handling" score for a tier (elite/champion scores drive the
  // next tier's gate).
  [[nodiscard]] float tierPressure(int tier) const {
    return tier >= 0 && tier < 4 ? tierPressure_[tier] : 0.0F;
  }
  // Test hook: feed the director directly, so the champion/overlord gates can be
  // tested without simulating minutes of real kills.
  void testAddTierPressure(int tier, float amount) {
    if (tier > 0 && tier < 4) tierPressure_[tier] += amount;
  }
  void testSetTierOpen(int tier, bool open) {
    if (tier > 0 && tier < 4) tierOpen_[tier] = open;
  }
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
    std::size_t vortices = 0;
    std::size_t lures = 0;
  };
  [[nodiscard]] DebugCounts debugCounts() const;
  [[nodiscard]] std::size_t debugEnemyCount() const;

  // --- Active abilities (J / K / L) -------------------------------------------
  // Every run has all three buttons, from the first second, with no unlock and
  // no card. They cost nothing but a cooldown, so the interesting decision is
  // never "do I have it" but "is now the moment" — which is exactly what a
  // finished build needs. Each answers a different problem a run keeps hitting:
  // J gets you out of a hug, K clears what your weapons cannot reach through,
  // L buys the three seconds a big enemy needs to die.
  enum class Ability : int { Blink = 0, Burst = 1, Stasis = 2 };
  static constexpr int kAbilityCount = 3;
  // Base cooldowns in seconds, scaled by PlayerStats::abilityCdMul.
  static constexpr float kBaseAbilityCd[kAbilityCount] = {5.0F, 14.0F, 30.0F};
  [[nodiscard]] float abilityCooldown(Ability a) const {
    return kBaseAbilityCd[static_cast<int>(a)] * stats_.abilityCdMul;
  }
  // Seconds left on an ability (0 = ready).
  [[nodiscard]] float abilityCooldownRemaining(Ability a) const {
    return abilityCd_[static_cast<int>(a)];
  }
  [[nodiscard]] bool abilityReady(Ability a) const {
    return abilityCd_[static_cast<int>(a)] <= 0.0F;
  }
  // Enemy time multiplier right now (1.0 unless Stasis is up).
  [[nodiscard]] float worldTimeScale() const { return worldTimeScale_; }
  [[nodiscard]] float stasisRemaining() const { return stasis_; }
  // Test hook: cast an ability through the normal input path.
  void testTriggerAbility(Ability a);

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

    // Vortex (evolution): rotating suction zones around the player.
    float vortexRadius = 1.3F;   // damage core radius
    float vortexReach = 2.6F;    // how far out enemies start getting pulled in
    float vortexPull = 4.0F;     // pull strength (world units / sec)
    float vortexOrbit = 2.6F;    // distance of the zone from the player
    float vortexOrbitSpeed = 1.8F;
    float vortexTickRate = 0.1F;

    // Prism (evolution): one locked beam per projectile, up to a hard cap.
    float prismRange = 9.0F;
    float prismWidth = 0.45F;
    int prismMaxTargets = 6;
    float prismRicochet = 4.0F;  // range of a reflected beam's next jump

    // Lure: planted beacon that drags enemies into its kill core.
    float lureRadius = 1.4F;
    float lureReach = 3.6F;
    float lurePull = 5.0F;
    float lureDps = 14.0F;
    float lureDuration = 5.0F;
    float lureTickRate = 0.15F;
    int lureMaxBeacons = 2;
  };
  // The storage arrays below hold the fully-stacked weaponCap(), which the
  // public constants above bound.

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
  // One simulation step. fixedUpdate() may run this several times per frame
  // when the test sandbox's difficulty clock is fast-forwarded.
  void fixedStep();
  void spawnWave();
  void processPendingSpawns();
  void fireWeapons();
  void movePlayer();
  void updateEnemies();
  void updateProjectiles();
  void updateOrbitBlades();
  void updateHaloBeams();
  void updateVortices();
  void updateBombProjectiles();
  void updateBoomerangProjectiles();
  void updateBounceProjectiles();
  void updateBeamEffects();
  void updateSweepEffects();
  void updateZoneEffects();
  void updateLures();
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
  // Applies one upgrade card to the run (stats, stack count, per-weapon cards,
  // HP/shield top-up). Shared by the level-up picker and the test item list.
  bool applyUpgradeAt(int upgradeIndex);
  void reroll();
  // Consumes the pending level-up (spends its XP, advances the level and either
  // queues the next one or resumes play). Shared by every kind of pick.
  void completeLevelUp();
  // True when the card can actually be applied right now. Weapon-specific
  // cards are unusable while their weapon is not equipped.
  [[nodiscard]] bool upgradeIsUsable(int upgradeIndex) const;
  void addWeapon(int defIndex);
  void syncOrbitBlades(int slot); // add orbit blades up to the current count
  void syncHaloBeams(int slot);   // add halo beams up to the current count
  void syncVortices(int slot);    // add vortex zones up to the current count
  // weaponCap() (public, above) is the limit addWeapon enforces: weapons cannot
  // be added past it, and the level-up offer stops appearing once it is full.
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
  // Knockback resistance AT THIS MOMENT. Tier/trait resistance is stored when
  // the enemy spawns, but the time-based part keeps growing for enemies that
  // were already on the field when the run started scaling.
  [[nodiscard]] float displacementResistance(entt::entity e) const;
  // Continuous fields (Void Gyre drag, Black Hole) use the same resistance but
  // keep a small floor, so a maxed enemy is hard to pin rather than immune to
  // crowd control outright.
  [[nodiscard]] float continuousPullScale(entt::entity e) const;
  // Repulsion Field unique: shove an enemy that just damaged the player.
  void retaliateKnockback(entt::entity attacker);
  void explodeBomb(entt::entity bomb, const BombProjectile& bp, float x, float y);
  void hurtPlayer(float amount);
  void damagePlayerDirect(float amount); // no thorns trigger (DoT auras)
  void spawnParticles(float x, float y, core::render::Color c, int count, float speed);
  void renderPlayerStats(core::render::Batcher& b, float px, float py);
  // Main menu overlay: title, the five rows (START / SKIN / OUTLINE / MANUAL /
  // RESET PROGRESS / QUIT) and the live skin+outline preview.
  void renderMainMenu(core::render::Batcher& b, float px, float py);
  // Handles one frame of menu input (navigation + activation). Runs before any
  // gameplay input so the menu is fully modal.
  void updateMainMenu(const FrameInput& input);
  // In-game manual overlay: one page at a time, page list on the left, the
  // page's own lines on the right.
  void renderManual(core::render::Batcher& b, float px, float py);
  // Handles one frame of manual input (page flipping, closing). The manual is
  // modal too: it eats the frame so a page flip never also moves the player.
  void updateManual(const FrameInput& input);
  // Skin + outline applied to the player Sprite, and the ring drawn around it.
  void applyProfileToPlayer();
  // Bestiary overlay (paused, B): discovered enemy types, their stats,
  // appearance, kill counts and which elite+ variants have been slain.
  void renderBestiary(core::render::Batcher& b, float px, float py);
  // Current global enemy scaling (shared by spawning and the bestiary).
  void currentScales(float& hp, float& speed, float& touch) const;
  // Player invulnerability window, nudged up ~1% per 5 defense.
  [[nodiscard]] float iframeDuration(float base) const;
  // Weapon test mode (T): cycle every weapon incl. evolutions, apply a boosted
  // build to see stat couplings, toggle waves, close to restore the run.
  // The entry/exit bodies are private; the thin public wrappers live with the
  // other test hooks above.
  void enterTestModeImpl();
  void exitTestModeImpl();
  void toggleTestBoost_();
  void spawnTestFodder();
  // Test sandbox: the "pick any item" list ([E], arrows, Enter).
  void updateTestShop(const FrameInput& input);
  void renderTestShop(core::render::Batcher& b, float px, float py);
  // Test sandbox: grant one stack of an upgrade without going through the
  // level-up roll. Returns false when the item is already maxed.
  bool grantTestUpgrade(int upgradeIndex);

  WeaponSlot weapons_[kMaxWeapons];
  int weaponCount_ = 0;
  // Weapon test mode state: snapshot of the real run while testing.
  bool testMode_ = false;
  bool testBoosted_ = false;
  // Stats snapshot taken the moment the "max build" boost was switched on, so
  // switching it back off only undoes the boost and keeps any item picks.
  PlayerStats testBoostBase_;
  int testWeaponIdx_ = 0;
  // --- Sandbox switches (all reset when the mode is left) --------------------
  bool testInvuln_ = false;   // immortality: incoming damage is ignored
  int testTimeScale_ = 1;     // difficulty-timer multiplier (1 / 4 / 10 / 20)
  bool testShopOpen_ = false; // the "pick any item" list
  int testShopCursor_ = 0;    // highlighted row in that list
  int testShopScroll_ = 0;    // first visible row (list is longer than the screen)
  // Full snapshot of the run. The sandbox is completely hermetic: anything that
  // happens inside it (XP, levels, kills, bestiary entries, outline unlocks,
  // item stacks, HP/shield) is rolled back when the mode is closed.
  int savedWeaponCount_ = 0;
  WeaponSlot savedWeapons_[kMaxWeapons];
  // Enemies already on the field when the sandbox opened. Everything spawned
  // after that is sandbox fodder and is removed on exit; the original horde is
  // kept so the run is not handed back a suspiciously empty arena. The full
  // component state is stored too: a sandbox weapon can kill, damage, displace
  // or knock the tier off a real enemy, and the run has to come back untouched.
  struct SavedEnemyState {
    entt::entity entity = entt::null;
    Transform transform{};
    Velocity velocity{};
    Radius radius{};
    Health health{};
    Enemy enemy{};
    EnemyTraits traits{};
    Sprite sprite{};
    Xp xp{};
  };
  std::vector<SavedEnemyState> savedEnemies_;
  // Every entity that existed when the sandbox opened. Anything else that
  // appears (XP orbs, drops, enemy shots, effect entities) is sandbox-only and
  // is deleted on exit instead of leaking into the real run.
  std::vector<entt::entity> savedEntityIds_;
  // Per-enemy spawn bookkeeping so a real enemy killed inside the sandbox can be
  // recreated with its original stats instead of simply vanishing.
  struct SavedSpawnState {
    int def = 0;
    int tier = 0;
    float hpMul = 1.0F;
    float touchMul = 1.0F;
    float speedMul = 1.0F;
    float xpMul = 1.0F;
    std::uint32_t traitFlags = TraitNone;
  };
  std::vector<SavedSpawnState> savedEnemySpawns_;
  float savedPlayerX_ = 0.0F;
  float savedPlayerY_ = 0.0F;
  PlayerStats savedStats_;
  std::vector<int> savedStacks_;
  std::vector<int> savedBestiaryKills_;
  std::vector<std::uint8_t> savedBestiaryTiers_;
  float savedXp_ = 0.0F;
  float savedXpNext_ = 0.0F;
  int savedLevel_ = 1;
  int savedKills_ = 0;
  float savedHp_ = 0.0F;
  float savedHpMax_ = 0.0F;
  float savedShield_ = 0.0F;
  float savedShieldDelay_ = 0.0F;
  float savedIframes_ = 0.0F;
  float savedHealCd_ = 0.0F;
  float savedSimTime_ = 0.0F;
  bool savedWaves_ = true;
  bool savedChoosingStarter_ = false;
  int savedStrongestTier_ = 0;
  int savedStrongestDef_ = -1;
  std::uint8_t savedTierKillMask_ = 0;
  float savedTierPressure_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  bool savedTierOpen_[4] = {false, true, false, false};
  float savedTierGrace_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  std::string savedTierBanner_;
  float savedTierBannerT_ = 0.0F;
  int savedStreak_ = 0;
  float savedStreakTimer_ = 0.0F;
  float savedAbilityCd_[kAbilityCount] = {0.0F, 0.0F, 0.0F};
  float savedStasis_ = 0.0F;
  float savedWorldTimeScale_ = 1.0F;
  std::vector<PendingSpawn> pending_;
  std::vector<PendingSpawn> savedPending_;
  // --- Rest of the run state that also mutates inside the sandbox ----------
  float savedSpawnTimer_ = 0.0F;
  float savedHordeTimer_ = 0.0F;
  bool savedStarterChoicePending_ = false;
  float savedLastStandCd_ = 0.0F;
  std::vector<std::uint8_t> savedRetiredTypes_;
  std::vector<std::uint8_t> savedRecentTypes_;
  int savedRerollsUsed_ = 0;
  bool savedMilestoneOffer_ = false;
  std::vector<Choice> savedChoices_;
  float savedPoison_ = 0.0F;
  int savedChainCounter_ = 0;
  int savedBloodKills_ = 0;
  float savedBlackHoleTimer_ = 8.0F;
  float savedAdrenalineCd_ = 0.0F;
  bool savedAdrenalineActive_ = false;
  bool savedBestiaryOpen_ = false;
  UnlockMask savedSyncedUnlocks_ = 0;
  bool savedProfileDirty_ = false;
  float savedMoveX_ = 0.0F;
  float savedMoveY_ = 0.0F;
  float savedCamX_ = 0.0F;
  float savedCamY_ = 0.0F;
  float savedZoom_ = 48.0F;
  std::vector<Particle> savedParticles_;
  std::size_t savedParticleCursor_ = 0;
  std::mt19937 savedRng_{};

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

  // --- Adaptive tribunal director --------------------------------------------
  // The heavy tiers are NOT on a timer: champions only start showing up once the
  // player is handling ELITES easily, and overlords once champions are routine.
  // `tierPressure_` is a decaying "how well is this tier being handled" score:
  // every kill of that tier adds weight, the score bleeds away over
  // kPressureWindow seconds, and the next tier is gated on the current one.
  static constexpr float kPressureWindow = 30.0F;
  // Elite kills (weighted 1.0) needed inside the window to call elites routine.
  static constexpr float kChampionPressure = 7.0F;
  // Champion kills needed inside the window to call champions routine.
  static constexpr float kOverlordPressure = 6.0F;
  // Even a great player waits this long: the first minute is for the build.
  static constexpr float kChampionMinTime = 90.0F;
  static constexpr float kOverlordMinTime = 240.0F;
  // Once earned, a tier stays open for a while so the director does not
  // flicker on and off between two kills.
  static constexpr float kTierGrace = 20.0F;
  float tierPressure_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  bool tierOpen_[4] = {false, true, false, false};
  float tierGrace_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  // Transient HUD banner ("CHAMPION TRIBUNAL OPEN", "HORDE INCOMING"). One
  // channel is enough: the newest message replaces the old one.
  std::string tierBanner_;
  float tierBannerT_ = 0.0F;
  void showBanner(std::string text, float seconds) {
    tierBanner_ = std::move(text);
    tierBannerT_ = seconds;
  }

  // Per-tick update of the director (decay + open/close with hysteresis).
  void updateTierDirector();

  // --- Momentum (the kill chain) ---------------------------------------------
  // Every kill adds to the chain; the chain dies if you stop killing for
  // `momentumWindow` seconds or if something hits you. While it is alive the
  // player is measurably stronger, which is what turns "walk away and let the
  // aura tick" into "stay in the middle of the horde".
  int streak_ = 0;
  float streakTimer_ = 0.0F;
  // Multipliers derived from the chain once per step; applied to every damage,
  // cooldown and movement calculation instead of being folded into PlayerStats
  // so the character sheet keeps showing the flat build.
  float momentumDamageMul_ = 1.0F;
  float momentumRate_ = 0.0F;
  float momentumSpeedMul_ = 1.0F;

  // Active ability state (see the Ability enum above).
  float abilityCd_[kAbilityCount] = {0.0F, 0.0F, 0.0F};
  // Stasis window remaining, and the enemy time multiplier it produces. The
  // player and every weapon keep running at full speed while this is up: only
  // the world slows down, which is what makes it readable at a glance.
  float stasis_ = 0.0F;
  float worldTimeScale_ = 1.0F;
  void updateMomentum();
  // Called when something actually lands a hit: the chain takes the hit with
  // you. Halved, minus two, and floored at zero.
  void breakMomentum();

  void updateAbilities(const FrameInput& input);
  void castBlink();
  void castBurst(float damageScale);
  void castStasis();

  // --- Profile / main menu state ---------------------------------------------
  // Not owned: main() owns the Profile and keeps it alive. Null in tests that
  // do not care, in which case skins stay at the default and nothing is
  // unlocked.
  Profile* profile_ = nullptr;
  bool profileDirty_ = false;
  // Unlock bits already pushed into the profile, so syncProfileUnlocks() only
  // reports genuinely new unlocks.
  UnlockMask syncedUnlocks_ = 0;
  bool menuOpen_ = false;
  // 0 START, 1 SKIN, 2 OUTLINE, 3 MANUAL, 4 RESET PROGRESS, 5 QUIT.
  // Named so the tests and the renderer cannot drift apart on the numbering.
  static constexpr int kMenuRows = 6;
  static constexpr int kMenuStart = 0;
  static constexpr int kMenuSkin = 1;
  static constexpr int kMenuOutline = 2;
  static constexpr int kMenuManual = 3;
  static constexpr int kMenuReset = 4;
  static constexpr int kMenuQuit = 5;
  int menuSelection_ = kMenuStart;
  bool quitRequested_ = false;
  // Armed by the first confirm on RESET PROGRESS; the wipe needs a second one.
  bool resetArmed_ = false;
  // In-game manual overlay.
  bool manualOpen_ = false;
  std::size_t manualPage_ = 0;
  // The selected outline is drawn as a ring around the player; this caches the
  // resolved color so render() does not touch the profile pointer.
  core::render::Color outlineColor_{0.0F, 0.0F, 0.0F, 0.0F};

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
