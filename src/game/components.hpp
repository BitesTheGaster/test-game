#pragma once

#include "core/render/color.hpp"

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
};

struct Projectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 1.5F;
};

struct Weapon {
  float cooldown = 0.5F;
  float timer = 0.0F;
  float damage = 5.0F;
  int projectiles = 1;
  float speed = 12.0F;
  float life = 1.4F;
  int pierce = 0;
};

struct Xp {
  float value = 1.0F;
};

} // namespace game
