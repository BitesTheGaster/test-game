#pragma once

#include "core/render/batcher.hpp"
#include "core/sim/fixed_timestep.hpp"
#include "core/sim/spatial_hash.hpp"
#include "game/components.hpp"
#include "game/content.hpp"

#include <entt/entity/registry.hpp>

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
};

// Mutable per-run player modifiers; upgraded through UpgradeDef::effect.
struct PlayerStats {
  float damageMul = 1.0F;
  float cooldownMul = 1.0F;
  float speedMul = 1.0F;
  float pickupMul = 1.0F;
  float speed = 5.2F;
  float maxHp = 100.0F;
  float regen = 0.0F;
  int projAdd = 0;
  int pierceAdd = 0;

  // Defense: one curve yields both a flat and a percent reduction.
  float defense = 0.0F;
  // Lifesteal: chance-based. L% = L% chance per damaging hit to heal
  // `lifestealHeal` HP; at L >= 100 the first point is guaranteed.
  float lifesteal = 0.0F;
  int lifestealHeal = 1; // HP per proc (Vampiric Heart unique raises to 2)
  // Shield: regenerating damage buffer (see rules in game.hpp docs).
  float shieldMax = 0.0F;

  // Unique-item effects (each is a distinct mechanic):
  float spreadMul = 1.0F; // widens weapon volleys (fan item)
  int extraChoice = 0;    // +N level-up cards
  int rerollCharges = 1;  // +N free rerolls per level-up (1 base)
  float thornsDmg = 0.0F; // AoE burst around player on hit
  int adrenaline = 0;     // speed burst when HP is low
  int blackHole = 0;      // periodic enemy pull
  int chain = 0;          // every 3rd projectile hit chains lightning
  int bloodPrice = 0;     // every 20 kills, burst around player
  int iceBlood = 0;       // enemies that hit you get slowed
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

  // Test/debug hooks.
  void grantXp(float amount);
  // Test helper: add weapon by index (bypasses normal level-up flow)
  void testAddWeapon(int defIndex) { addWeapon(defIndex); }
  // Test helper: drop owned weapons + their persistent entities (orbit blades).
  void testClearWeapons();
  // Test helper: place a stationary, high-HP enemy at a world position.
  void testSpawnEnemyAt(float x, float y);
  // Test helper: freeze wave spawning so tests control the enemy pool exactly.
  void testDisableWaves() { wavesEnabled_ = false; }
  // Test helper: apply a per-weapon upgrade (same path as weapon Focus cards).
  void testAddWeaponUpgrade(int slot, std::string_view effect, float value) {
    applyWeaponEffect(slot, effect, value);
  }
  // Test helper: current HP of the first Enemy in the registry (-1 if none).
  [[nodiscard]] float testFirstEnemyHp() const;
  // Test helper: current speed (velocity magnitude) of every live Enemy.
  [[nodiscard]] std::vector<float> testEnemySpeeds() const;
  // Test helper: circular angular gaps (radians) between a slot's orbit
  // blades, sorted. All gaps equal 2*pi/count when blades are evenly spaced.
  [[nodiscard]] std::vector<float> testOrbitBladeGaps(int slot) const;

  // Debug introspection for tests: how many live entities each attack type has.
  struct DebugCounts {
    std::size_t projectiles = 0;
    std::size_t orbitBlades = 0;
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

    // Beam
    float beamRange = 8.0F;
    float beamWidth = 0.3F;
    float beamDuration = 0.15F;

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
  };
  static constexpr int kMaxWeapons = 4;

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

  // Elite name labels collected during the world pass (drawn in screen pass).
  struct NameLabel {
    float x = 0.0F;
    float y = 0.0F;
    core::render::Color c{1.0F, 1.0F, 1.0F, 1.0F};
    std::string name;
  };

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
  void updateBombProjectiles();
  void updateBoomerangProjectiles();
  void updateBounceProjectiles();
  void updateBeamEffects();
  void updateSweepEffects();
  void updateZoneEffects();
  void updateChainLightning();
  void updateNovaRing();
  void updatePickups();
  void updateShield();
  void updateUniqueEffects();
  void buildSpatialHash();
  void enterLevelUp();
  void buildChoices();
  void chooseUpgrade(int slot);
  void reroll();
  void addWeapon(int defIndex);
  void syncOrbitBlades(int slot); // add orbit blades up to the current count
  void applyWeaponEffect(int slotIndex, std::string_view effect, float value);
  int findWeaponSlot(std::string_view weaponId) const;
  bool ownsWeapon(int defIndex) const;
  int pickWeaponGrant(); // -1 when nothing new to offer
  void spawnEnemy(const PendingSpawn& p);
  void applyEnemyDamage(entt::entity e, float dmg);
  void killEnemy(entt::entity e);
  void chainBolt(float x, float y, float dmg);
  void tryLifesteal(); // chance-based heal on a damaging hit
  void explodeBomb(entt::entity bomb, const BombProjectile& bp, float x, float y);
  void hurtPlayer(float amount);
  void spawnParticles(float x, float y, core::render::Color c, int count, float speed);
  void renderPlayerStats(core::render::Batcher& b, float px, float py);

  WeaponSlot weapons_[kMaxWeapons];
  int weaponCount_ = 0;
  std::vector<PendingSpawn> pending_;
  std::vector<NameLabel> labels_;

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
