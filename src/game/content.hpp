#pragma once

#include "core/render/color.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// Data-driven balance: everything here is loaded from assets/data/*.toml.

enum class AttackType : std::uint8_t {
  Projectile,   // standard aimed projectile(s) with spread
  Orbit,        // blades orbit player, deal contact damage
  Cone,         // instant AoE cone in aim direction
  Bomb,         // slow arcing projectile with gravity, explodes on impact
  Boomerang,    // projectile goes out to max range, returns to player
  Bounce,       // projectile bounces between enemies
  Beam,         // instant hitscan line
  Sweep,        // melee arc around player
  Zone,         // creates persistent damage zone on ground
  Chain,        // lightning jumps between enemies (evolution)
  Nova          // expanding ring from player (evolution)
};

struct WeaponDef {
  std::string id;
  std::string name;
  std::string desc;
  AttackType attackType = AttackType::Projectile;
  float damage = 5.0F;
  float cooldown = 0.5F;
  int projectiles = 1;        // count for projectile/orbit/boomerang
  float projSpeed = 12.0F;    // speed for projectile/boomerang/bomb
  float projLife = 1.4F;      // lifetime for projectile/boomerang/bomb
  int pierce = 0;             // projectile/bounce
  float spread = 0.16F;       // radians between projectiles (projectile)
  core::render::Color projColor{1.0F, 0.95F, 0.55F, 1.0F};
  bool starter = false;
  std::vector<std::string> prereqs;

  // Cone
  float coneAngle = 0.8F;     // radians
  float coneRange = 2.5F;     // world units
  float coneTickRate = 0.1F;  // damage ticks per second

  // Orbit
  float orbitRadius = 1.2F;   // distance from player
  float orbitSpeed = 2.0F;    // radians per second
  int orbitCount = 2;         // number of blades

  // Bomb
  float bombArcHeight = 2.0F;     // vertical arc height
  float bombExplodeRadius = 1.5F; // explosion radius
  float bombKnockback = 3.0F;     // knockback force
  float bombFuse = 0.0F;          // 0 = explode on impact, >0 = delay

  // Boomerang
  float boomerangRange = 4.0F;    // max distance before return
  float boomerangReturnSpeed = 1.5F; // return speed multiplier

  // Bounce
  int bounceCount = 3;          // max bounces
  float bounceRange = 2.5F;     // search radius for next target
  float bounceDamageMul = 0.7F; // damage multiplier per bounce

  // Beam
  float beamRange = 8.0F;       // max range
  float beamWidth = 0.3F;       // line thickness
  float beamDuration = 0.15F;   // visual persist time

  // Sweep
  float sweepAngle = 3.14F;     // radians (PI = 180°, 2PI = 360°)
  float sweepRadius = 2.0F;     // reach
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
};

struct EnemyDef {
  std::string id;
  std::string name;
  float hp = 10.0F;
  float speed = 2.0F;
  float touch = 5.0F;
  float radius = 0.3F;
  float xp = 1.0F;
  float unlockAt = 0.0F;
  float weight = 1.0F;
  core::render::Color color{};
  bool circle = true;
};

struct UpgradeDef {
  std::string id;
  std::string name;
  std::string desc;
  std::string effect;
  float value = 0.0F;
  int maxStacks = 5;
  std::string kind = "normal";
  std::string weapon;
  int level = 0;
};

struct Content {
  std::vector<WeaponDef> weapons;
  std::vector<EnemyDef> enemies;
  std::vector<UpgradeDef> upgrades;

  [[nodiscard]] const WeaponDef* weapon(std::string_view id) const;
  [[nodiscard]] const EnemyDef* enemy(std::string_view id) const;
};

Content loadContent(const std::filesystem::path& dir);

} // namespace game