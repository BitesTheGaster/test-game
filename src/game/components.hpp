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
  float area = 0.0F;      // explosion radius on impact
  float strength = 0.0F;  // knockback force on hit
  bool homing = false;    // homes toward nearest enemy each tick
  int bounces = 0;        // wall bounces remaining after impact
  int bounceCount = 0;    // how many bounces have happened
};

// Orbiting blades (dagger, etc.)
struct OrbitBlade {
  float damage = 1.0F;
  float radius = 1.2F;     // orbit distance from player
  float speed = 2.0F;      // radians per second
  float angle = 0.0F;      // current angle
  int pierce = 0;
  int weaponIndex = -1;    // owning weapon slot (-1 = orphaned)
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Arcing bomb projectile (hammer)
struct BombProjectile {
  float damage = 1.0F;
  float explodeRadius = 1.5F;
  float knockback = 3.0F;
  float life = 2.0F;
  float initialLife = 2.0F;
  float arcHeight = 2.0F;
  float startX = 0.0F;
  float startY = 0.0F;
  float targetX = 0.0F;
  float targetY = 0.0F;
  float fuse = 0.0F;       // 0 = explode on impact
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Boomerang projectile (shuriken)
struct BoomerangProjectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 3.0F;
  float maxRange = 4.0F;
  float returnSpeed = 1.5F;
  float startX = 0.0F;
  float startY = 0.0F;
  float targetX = 0.0F;
  float targetY = 0.0F;
  bool returning = false;
  int bounceCount = 0;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Bouncing projectile (orb)
struct BounceProjectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 2.5F;
  int maxBounces = 3;
  float bounceRange = 2.5F;
  float damageMul = 0.7F;
  int bounceCount = 0;
  float lastHitX = 0.0F;
  float lastHitY = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Instant beam (beam)
struct BeamEffect {
  float damage = 1.0F;
  float range = 8.0F;
  float width = 0.3F;
  float duration = 0.15F;
  float timer = 0.0F;
  float startX = 0.0F;
  float startY = 0.0F;
  float endX = 0.0F;
  float endY = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Melee sweep (scythe)
struct SweepEffect {
  float damage = 1.0F;
  float radius = 2.0F;
  float angle = 3.14F;      // radians
  float knockback = 2.0F;
  float duration = 0.2F;
  float timer = 0.0F;
  float startAngle = 0.0F;
  float endAngle = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Damage zone on ground (ember/zone)
struct ZoneEffect {
  float dps = 15.0F;
  float radius = 1.2F;
  float duration = 4.0F;
  float timer = 0.0F;
  float tickTimer = 0.0F;
  float tickRate = 0.1F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Chain lightning (storm evolution)
struct ChainLightning {
  float damage = 1.0F;
  int maxJumps = 4;
  float jumpRange = 2.5F;
  float damageMul = 0.6F;
  int jumpsDone = 0;
  float timer = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Expanding nova ring (nova evolution)
struct NovaRing {
  float damagePerTick = 25.0F;
  float maxRadius = 4.0F;
  float expandSpeed = 3.0F;
  float tickRate = 0.15F;
  float radius = 0.0F;
  float timer = 0.0F;
  float tickTimer = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct Xp {
  float value = 1.0F;
};

} // namespace game
