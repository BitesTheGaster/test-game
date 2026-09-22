#pragma once

#include "core/render/color.hpp"

#include <cstdint>

namespace game {

// All components are POD: they live in dense EnTT pools and never allocate.

struct Transform {
  float x = 0.0F;
  float y = 0.0F;
  float px = 0.0F; // previous tick position (for render interpolation)
  float py = 0.0F;
};

struct Velocity {
  float x = 0.0F;
  float y = 0.0F;
};

struct Radius {
  float r = 0.1F;
};

struct Sprite {
  core::render::Color color{};
  bool circle = true;
};

struct Health {
  float hp = 1.0F;
  float max = 1.0F;
};

struct PlayerTag {};

struct Enemy {
  float speed = 2.0F;
  float touch = 5.0F;
  float slowT = 0.0F; // >0 => slowed for this many seconds
};

// Bit flags for elite/champion enemy traits (non-POD-free: plain POD).
enum TraitFlag : std::uint32_t {
  TraitNone = 0,
  TraitFast = 1u << 0,
  TraitArmored = 1u << 1,
  TraitRegenerating = 1u << 2,
  TraitExplosive = 1u << 3,
  TraitVenomous = 1u << 4,
  TraitVampiric = 1u << 5,
  TraitShielded = 1u << 6,
};

struct EnemyTraits {
  std::uint32_t flags = TraitNone;
  std::uint8_t tier = 0; // 0 normal, 1 elite, 2 champion
  float regen = 0.0F;    // HP per second
  float shield = 0.0F;   // absorbs damage point-for-point
};

struct Projectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 1.5F;
};

struct Xp {
  float value = 1.0F;
};

} // namespace game
