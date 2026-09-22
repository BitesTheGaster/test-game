#pragma once

#include "core/render/batcher.hpp"
#include "core/sim/fixed_timestep.hpp"
#include "core/sim/spatial_hash.hpp"
#include "game/components.hpp"
#include "game/content.hpp"

#include <entt/entity/registry.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace game {

// Normalized input gathered by main() from SDL each frame.
struct FrameInput {
  float moveX = 0.0F; // -1..1
  float moveY = 0.0F; // -1..1, y-up
  bool choose1 = false;
  bool choose2 = false;
  bool choose3 = false;
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
};

struct UpgradeEffectResult {
  bool valid = false; // false => unknown effect id
  float heal = 0.0F;
};

// Applies one upgrade effect to stats. Shared between Game and unit tests.
UpgradeEffectResult applyUpgrade(PlayerStats& stats, std::string_view effect, float value);

// XP needed to advance from `level` to `level + 1`.
float xpForLevel(int level);

enum class RunState {
  Playing,
  LevelUp,
  Paused,
  GameOver,
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
  [[nodiscard]] float xp() const { return xp_; }
  [[nodiscard]] float xpNext() const { return xpNext_; }
  [[nodiscard]] std::size_t enemyCount() const;
  [[nodiscard]] const PlayerStats& stats() const { return stats_; }
  [[nodiscard]] const std::vector<int>& upgradeChoices() const { return choices_; }
  [[nodiscard]] int upgradeStacks(std::size_t upgradeIndex) const;

  // Test/debug hooks.
  void grantXp(float amount);

private:
  void fixedUpdate();
  void spawnWave();
  void fireWeapons();
  void movePlayer();
  void updateEnemies();
  void updateProjectiles();
  void updatePickups();
  void buildSpatialHash();
  void enterLevelUp();
  void chooseUpgrade(int slot);
  void spawnParticles(float x, float y, core::render::Color c, int count, float speed);

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
  float iframes_ = 0.0F;
  float moveX_ = 0.0F; // latched input for fixed steps
  float moveY_ = 0.0F;

  PlayerStats stats_;
  std::vector<int> stacks_;   // per-upgrade stack counts
  std::vector<int> choices_;  // 3 upgrade indices offered on level-up

  entt::entity player_ = entt::null;

  // Camera (frame-level, not simulated).
  float camX_ = 0.0F;
  float camY_ = 0.0F;
  float zoom_ = 48.0F;

  // Scratch buffers, reused every tick (no hot-path allocation).
  std::vector<float> scratchX_, scratchY_;
  std::vector<std::uint32_t> scratchId_;
  std::vector<entt::entity> destroyQueue_;

  // Particles: fixed ring buffer of PODs.
  struct Particle {
    float x, y, vx, vy, life, maxLife, size;
    core::render::Color color;
  };
  std::vector<Particle> particles_;
  static constexpr std::size_t kMaxParticles = 4096;
  std::size_t particleCursor_ = 0;
};

} // namespace game
