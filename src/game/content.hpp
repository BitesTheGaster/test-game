#pragma once

#include "core/render/color.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// Data-driven balance: everything here is loaded from assets/data/*.toml.

struct WeaponDef {
  std::string id;
  std::string name;
  std::string desc;
  float damage = 5.0F;
  float cooldown = 0.5F;
  int projectiles = 1;
  float projSpeed = 12.0F;
  float projLife = 1.4F;
  int pierce = 0;
  float spread = 0.16F; // radians between projectiles of a volley
  core::render::Color projColor{1.0F, 0.95F, 0.55F, 1.0F};
  bool starter = false;   // may be rolled as the run-start weapon
  std::vector<std::string> prereqs; // evolution prerequisites (weapon ids)
};

struct EnemyDef {
  std::string id;
  std::string name;
  float hp = 10.0F;
  float speed = 2.0F;
  float touch = 5.0F;
  float radius = 0.3F;
  float xp = 1.0F;
  float unlockAt = 0.0F; // seconds into the run
  float weight = 1.0F;   // relative spawn weight
  core::render::Color color{};
  bool circle = true;
};

struct UpgradeDef {
  std::string id;
  std::string name;
  std::string desc;
  std::string effect; // effect id understood by applyUpgrade()
  float value = 0.0F;
  int maxStacks = 5;
  std::string kind = "normal";  // "normal" | "unique" | "milestone"
  std::string weapon;           // non-empty => targets that owned weapon
  int level = 0;                // required level for milestone picks
};

struct Content {
  std::vector<WeaponDef> weapons;
  std::vector<EnemyDef> enemies;
  std::vector<UpgradeDef> upgrades;

  [[nodiscard]] const WeaponDef* weapon(std::string_view id) const;
  [[nodiscard]] const EnemyDef* enemy(std::string_view id) const;
};

// Loads weapons.toml, enemies.toml, upgrades.toml from `dir`.
// Throws std::runtime_error with a file path on malformed data.
Content loadContent(const std::filesystem::path& dir);

} // namespace game
