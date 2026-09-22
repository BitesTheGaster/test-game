#include "game/game.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace game {
namespace {

constexpr float kPi = 3.14159265358979F;
constexpr std::size_t kMaxEnemies = 4000;
constexpr float kSpawnDist = 11.0F;
constexpr float kSpawnTelegraph = 0.6F; // seconds a spawn marker is visible
constexpr float kPickupDist = 0.45F;
constexpr float kShieldRegenRate = 10.0F;   // HP/s once out of combat
constexpr float kShieldRegenDelay = 4.0F;   // seconds without damage
constexpr float kContactIframes = 0.55F;

constexpr float kEliteHpMul = 3.5F;
constexpr float kChampionHpMul = 5.0F;

// Trait pool: a growing count of traits is rolled for elite/champion enemies.
enum PickTrait : int {
  PickFast,
  PickArmored,
  PickRegenerating,
  PickExplosive,
  PickVenomous,
  PickVampiric,
  PickShielded,
  PickCount,
};

float length(float x, float y) {
  return std::sqrt(x * x + y * y);
}

core::render::Color eliteTint(core::render::Color c) {
  // Brighten toward white so elites read at a glance.
  c.r = 1.0F - (1.0F - c.r) * 0.45F;
  c.g = 1.0F - (1.0F - c.g) * 0.45F;
  c.b = 1.0F - (1.0F - c.b) * 0.45F;
  return c;
}

} // namespace

UpgradeEffectResult applyUpgrade(PlayerStats& stats, std::string_view effect, float value) {
  if (effect == "damage_mul") {
    stats.damageMul += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "cooldown_mul") {
    stats.cooldownMul += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "speed_mul") {
    stats.speedMul += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "pickup_mul") {
    stats.pickupMul += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "max_hp_add") {
    stats.maxHp += value;
    return {true, value, 0.0F}; // also heals for the same amount
  }
  if (effect == "regen_add") {
    stats.regen += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "proj_add") {
    stats.projAdd += static_cast<int>(value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "pierce_add") {
    stats.pierceAdd += static_cast<int>(value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "heal") {
    return {true, value, 0.0F};
  }
  if (effect == "defense_add") {
    stats.defense += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "lifesteal_add") {
    stats.lifesteal += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "shield_add") {
    stats.shieldMax += value;
    return {true, 0.0F, value}; // the picker tops the shield up to the new max
  }
  if (effect == "extra_choice") {
    stats.extraChoice += static_cast<int>(value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "reroll_add") {
    stats.rerollCharges += static_cast<int>(value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "fan") {
    // Top-tier spread: double the volley angle and (roughly) double fire rate.
    stats.spreadMul += value * 2.0F;
    stats.cooldownMul *= (1.0F / (1.0F + value));
    return {true, 0.0F, 0.0F};
  }
  if (effect == "thorns") {
    stats.thornsDmg += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "adrenaline") {
    stats.adrenaline = 1;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "black_hole") {
    stats.blackHole = 1;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "chain") {
    stats.chain = 1;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "blood_price") {
    stats.bloodPrice = 1;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ice_blood") {
    stats.iceBlood = 1;
    return {true, 0.0F, 0.0F};
  }
  return {false, 0.0F, 0.0F};
}

float mitigateDamage(float raw, float defense) {
  if (raw <= 0.0F) return 0.0F;
  // One defensive curve, two feels:
  //  - flat: every 5 points of defense silently eat 1 point of damage;
  //  - percent: overall reduction approaches 50% as defense grows.
  const float flat = std::floor(defense / 5.0F);
  const float percent = 0.5F * defense / (defense + 150.0F);
  const float after = (raw - flat) * (1.0F - percent);
  return std::max(0.0F, after);
}

float xpForLevel(int level) {
  const float l = static_cast<float>(level - 1);
  return 6.0F + 4.0F * l + 0.5F * l * (l + 1.0F);
}

Game::Game(const Content& content, std::uint32_t seed)
    : content_(content), rng_(seed), stacks_(content.upgrades.size(), 0) {
  reset();
}

void Game::reset() {
  registry_.clear();
  rng_.seed(1337); // deterministic restarts

  state_ = RunState::Playing;
  simTime_ = 0.0F;
  level_ = 1;
  xp_ = 0.0F;
  xpNext_ = xpForLevel(level_);
  kills_ = 0;
  spawnTimer_ = 0.5F; // short grace before the first telegraph
  iframes_ = 0.0F;
  stats_ = PlayerStats{};
  std::fill(stacks_.begin(), stacks_.end(), 0);
  choices_.clear();
  pending_.clear();
  labels_.clear();
  particles_.clear();
  particleCursor_ = 0;

  shield_ = 0.0F;
  shieldDelay_ = 0.0F;
  rerollsUsed_ = 0;
  milestoneOffer_ = false;
  poison_ = 0.0F;
  chainCounter_ = 0;
  bloodKills_ = 0;
  blackHoleTimer_ = 8.0F;
  adrenalineCd_ = 0.0F;
  adrenalineActive_ = false;

  player_ = registry_.create();
  registry_.emplace<Transform>(player_, 0.0F, 0.0F, 0.0F, 0.0F);
  registry_.emplace<Velocity>(player_);
  registry_.emplace<Radius>(player_, 0.35F);
  Sprite ps{};
  ps.color = {0.55F, 0.85F, 1.0F, 1.0F};
  ps.circle = true;
  registry_.emplace<Sprite>(player_, ps);
  registry_.emplace<Health>(player_, stats_.maxHp, stats_.maxHp);
  registry_.emplace<PlayerTag>(player_);

  // Starter weapon: a random starter-flagged weapon (wand/dagger/crossbow).
  weaponCount_ = 0;
  std::vector<int> starters;
  for (std::size_t i = 0; i < content_.weapons.size(); ++i) {
    if (content_.weapons[i].starter) {
      starters.push_back(static_cast<int>(i));
    }
  }
  if (starters.empty()) {
    for (std::size_t i = 0; i < content_.weapons.size(); ++i) {
      starters.push_back(static_cast<int>(i));
    }
  }
  if (!starters.empty()) {
    std::uniform_int_distribution<int> pick(0, static_cast<int>(starters.size()) - 1);
    addWeapon(starters[static_cast<std::size_t>(pick(rng_))]);
  }

  camX_ = 0.0F;
  camY_ = 0.0F;
  timestep_.reset();
}

void Game::advance(float frameDt, const FrameInput& input) {
  if (input.togglePause) {
    if (state_ == RunState::Playing) {
      state_ = RunState::Paused;
    } else if (state_ == RunState::Paused) {
      state_ = RunState::Playing;
    }
  }
  if (state_ == RunState::GameOver && input.restart) {
    reset();
    return;
  }
  if (state_ == RunState::LevelUp) {
    const std::size_t n = choices_.size();
    if (input.choose1 && n >= 1) chooseUpgrade(0);
    else if (input.choose2 && n >= 2) chooseUpgrade(1);
    else if (input.choose3 && n >= 3) chooseUpgrade(2);
    else if (input.choose4) {
      if (n >= 4) {
        chooseUpgrade(3);
      } else {
        reroll(); // with only three cards, key 4 is the free reroll
      }
    } else if (input.choose5 && n >= 5) {
      chooseUpgrade(4);
    }
    if (input.restart && state_ == RunState::LevelUp) {
      reroll(); // R key rerolls the current choices
    }
  }

  moveX_ = input.moveX;
  moveY_ = input.moveY;
  if (state_ == RunState::Playing) {
    timestep_.advance(frameDt, [&] { fixedUpdate(); });
  } else {
    timestep_.reset();
  }

  // Camera follows the player smoothly (frame-level).
  if (player_ != entt::null && registry_.valid(player_)) {
    const auto& t = registry_.get<Transform>(player_);
    constexpr float follow = 8.0F;
    camX_ += (t.x - camX_) * std::min(1.0F, follow * frameDt);
    camY_ += (t.y - camY_) * std::min(1.0F, follow * frameDt);
  }
}

void Game::fixedUpdate() {
  simTime_ += 1.0F / 60.0F;
  if (iframes_ > 0.0F) {
    iframes_ -= 1.0F / 60.0F;
  }

  movePlayer();
  buildSpatialHash();
  updateEnemies();
  fireWeapons();
  updateProjectiles();
  updateOrbitBlades();
  updateBombProjectiles();
  updateBoomerangProjectiles();
  updateBounceProjectiles();
  updateBeamEffects();
  updateSweepEffects();
  updateZoneEffects();
  updateChainLightning();
  updateNovaRing();
  updatePickups();

  // Regen.
  if (stats_.regen > 0.0F && player_ != entt::null && registry_.valid(player_)) {
    auto& hp = registry_.get<Health>(player_);
    hp.hp = std::min(hp.max, hp.hp + stats_.regen / 60.0F);
  }

  updateShield();
  updateUniqueEffects();

  processPendingSpawns();
  spawnWave();

  // Deferred destruction (command buffer convention).
  for (const auto e : destroyQueue_) {
    if (registry_.valid(e)) {
      registry_.destroy(e);
    }
  }
  destroyQueue_.clear();

  // Particles.
  for (auto& p : particles_) {
    if (p.life <= 0.0F) continue;
    p.life -= 1.0F / 60.0F;
    p.x += p.vx / 60.0F;
    p.y += p.vy / 60.0F;
    p.vx *= 0.92F;
    p.vy *= 0.92F;
  }

  // Level-up check.
  if (state_ == RunState::Playing && xp_ >= xpNext_) {
    enterLevelUp();
  }
}

void Game::movePlayer() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& t = registry_.get<Transform>(player_);
  auto& v = registry_.get<Velocity>(player_);

  float mx = moveX_;
  float my = moveY_;
  const float len = length(mx, my);
  if (len > 1.0F) {
    mx /= len;
    my /= len;
  }
  float speed = stats_.speed * stats_.speedMul;

  // Adrenaline: low HP gives a burst of speed and a brief invulnerability.
  if (stats_.adrenaline != 0) {
    const auto& hp = registry_.get<Health>(player_);
    const float frac = hp.max > 0.0F ? hp.hp / hp.max : 0.0F;
    if (frac > 0.0F && frac < 0.3F) {
      speed *= 1.6F;
      if (!adrenalineActive_ && adrenalineCd_ <= 0.0F) {
        adrenalineActive_ = true;
        adrenalineCd_ = 20.0F;
        iframes_ = std::max(iframes_, 1.0F);
        spawnParticles(t.x, t.y, {1.0F, 0.7F, 0.2F, 1.0F}, 10, 5.0F);
      }
    } else {
      adrenalineActive_ = false;
    }
  }

  v.x = mx * speed;
  v.y = my * speed;

  t.px = t.x;
  t.py = t.y;
  t.x += v.x / 60.0F;
  t.y += v.y / 60.0F;
}

void Game::buildSpatialHash() {
  scratchX_.clear();
  scratchY_.clear();
  scratchId_.clear();
  auto view = registry_.view<Transform>();
  for (const auto e : view) {
    const auto& t = view.get<Transform>(e);
    scratchX_.push_back(t.x);
    scratchY_.push_back(t.y);
    scratchId_.push_back(entt::to_integral(e));
  }
  hash_.build(scratchX_.data(), scratchY_.data(), scratchId_.data(), scratchX_.size());
}

void Game::updateEnemies() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const auto& pr = registry_.get<Radius>(player_);
  auto& php = registry_.get<Health>(player_);

  auto view = registry_.view<Transform, Velocity, Enemy, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);
    auto& en = view.get<Enemy>(e);
    const auto& r = view.get<Radius>(e);

    t.px = t.x;
    t.py = t.y;

    // Traits tick.
    auto* traits = registry_.try_get<EnemyTraits>(e);
    if (traits != nullptr) {
      if (traits->flags & TraitRegenerating) {
        auto& eh = registry_.get<Health>(e);
        eh.hp = std::min(eh.max, eh.hp + traits->regen / 60.0F);
      }
    }

    // Seek the player.
    float dx = pt.x - t.x;
    float dy = pt.y - t.y;
    const float dist = length(dx, dy);
    if (dist > 0.001F) {
      dx /= dist;
      dy /= dist;
    }

    // Separation from neighbours via spatial hash.
    float sepX = 0.0F;
    float sepY = 0.0F;
    const float queryR = r.r * 2.2F;
    hash_.forEachNear(t.x, t.y, queryR, [&](std::uint32_t id) {
      const auto other = static_cast<entt::entity>(id);
      if (other == e || !registry_.valid(other)) return;
      if (!registry_.all_of<Transform, Radius>(other)) return;
      const auto& ot = registry_.get<Transform>(other);
      const auto& orr = registry_.get<Radius>(other);
      const float ox = t.x - ot.x;
      const float oy = t.y - ot.y;
      const float d2 = ox * ox + oy * oy;
      const float minDist = (r.r + orr.r) * 0.9F;
      if (d2 < minDist * minDist && d2 > 0.0001F) {
        const float d = std::sqrt(d2);
        sepX += (ox / d) * (minDist - d);
        sepY += (oy / d) * (minDist - d);
      }
    });

    // Slowed enemies (ice blood) move at 45%.
    float effSpeed = en.speed;
    if (en.slowT > 0.0F) {
      en.slowT -= 1.0F / 60.0F;
      effSpeed *= 0.45F;
    }

    v.x = dx * effSpeed + sepX * 12.0F;
    v.y = dy * effSpeed + sepY * 12.0F;
    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    // Contact damage to the player.
    const float hitDist = pr.r + r.r;
    const float px = pt.x - t.x;
    const float py = pt.y - t.y;
    if (px * px + py * py < hitDist * hitDist && iframes_ <= 0.0F) {
      if (traits != nullptr) {
        if (traits->flags & TraitVampiric) {
          auto& eh = registry_.get<Health>(e);
          eh.hp = std::min(eh.max, eh.hp + en.touch * 0.5F);
        }
        if (traits->flags & TraitVenomous) {
          poison_ = 3.0F;
        }
      }
      if (stats_.iceBlood != 0) {
        en.slowT = 2.0F;
      }
      hurtPlayer(en.touch);
      iframes_ = kContactIframes;
      spawnParticles(pt.x, pt.y, {1.0F, 0.3F, 0.3F, 1.0F}, 6, 4.0F);
      if (php.hp <= 0.0F) {
        php.hp = 0.0F;
        state_ = RunState::GameOver;
      }
    }
  }
}

void Game::fireWeapons() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  if (weaponCount_ <= 0) return;

  // Find the nearest enemy for targeting.
  float bestDist2 = 1e12F;
  float targetX = 0.0F;
  float targetY = 0.0F;
  bool found = false;
  auto enemies = registry_.view<Transform, Enemy>();
  for (const auto e : enemies) {
    const auto& t = enemies.get<Transform>(e);
    const float dx = t.x - pt.x;
    const float dy = t.y - pt.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < bestDist2) {
      bestDist2 = d2;
      targetX = t.x;
      targetY = t.y;
      found = true;
    }
  }

  if (!found) return; // No enemies - don't fire

  const float baseAngle = std::atan2(targetY - pt.y, targetX - pt.x);

  for (int i = 0; i < weaponCount_; ++i) {
    auto& w = weapons_[i];
    w.timer -= 1.0F / 60.0F;
    if (w.timer > 0.0F) continue;

    const float cooldown = w.cooldown * stats_.cooldownMul;
    const float damage = w.damage * stats_.damageMul;
    const int count = std::max(1, w.projectiles + stats_.projAdd);
    const int pierce = w.pierce + stats_.pierceAdd;

    switch (w.attackType) {
      case AttackType::Projectile: {
        const float spread = (count > 1) ? w.spread * stats_.spreadMul : 0.0F;
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * spread;
          const float angle = baseAngle + offset;
          const auto proj = registry_.create();
          registry_.emplace<Transform>(proj, pt.x, pt.y, pt.x, pt.y);
          registry_.emplace<Velocity>(proj, std::cos(angle) * w.speed, std::sin(angle) * w.speed);
          registry_.emplace<Radius>(proj, 0.14F);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(proj, s);
          registry_.emplace<Projectile>(proj, damage, pierce, w.life, w.area, w.strength, w.homing, w.bounces, 0);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Orbit: {
        // Orbit blades are created once in addWeapon() and persist
        // Just reset the cooldown timer here
        w.timer = cooldown;
        break;
      }
      case AttackType::Cone: {
        // Instant cone damage - no projectiles, just damage in cone area
        if (found) {
          const float coneAngle = w.coneAngle;
          const float coneRange = w.coneRange;
          const float halfAngle = coneAngle * 0.5F;
          auto view = registry_.view<Transform, Health, Radius, Enemy>();
          for (const auto e : view) {
            const auto& et = view.get<Transform>(e);
            const float dx = et.x - pt.x;
            const float dy = et.y - pt.y;
            const float dist2 = dx * dx + dy * dy;
            if (dist2 > coneRange * coneRange) continue;
            const float angleToEnemy = std::atan2(dy, dx);
            float diff = angleToEnemy - baseAngle;
            while (diff > kPi) diff -= 2.0F * kPi;
            while (diff < -kPi) diff += 2.0F * kPi;
            if (std::abs(diff) <= halfAngle) {
              applyEnemyDamage(e, damage * stats_.damageMul);
              const auto& er = view.get<Radius>(e); (void)er;
              spawnParticles(et.x, et.y, {1.0F, 0.5F, 0.1F, 1.0F}, 4, 3.0F);
            }
          }
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Bomb: {
        // Arcing projectile that explodes on impact
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * w.spread;
          const float angle = baseAngle + offset;
          // Calculate horizontal distance to target
          const float range = w.speed * w.life; // approximate range
          const float tx = pt.x + std::cos(angle) * range;
          const float ty = pt.y + std::sin(angle) * range;
          const auto bomb = registry_.create();
          registry_.emplace<Transform>(bomb, pt.x, pt.y, pt.x, pt.y);
          // Calculate initial velocity for parabolic arc
          // Horizontal velocity
          const float vx = std::cos(angle) * w.speed;
          // Vertical velocity to achieve bombArcHeight at midpoint
          // Using: H = vy^2 / (2*g) => vy = sqrt(2*g*H)
          const float gravity = 30.0F; // matches updateBombProjectiles
          const float vy = std::sqrt(2.0F * gravity * w.bombArcHeight);
          registry_.emplace<Velocity>(bomb, vx, vy);
          registry_.emplace<Radius>(bomb, 0.2F);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(bomb, s);
          BombProjectile bp{};
          bp.damage = damage;
          bp.explodeRadius = w.bombExplodeRadius;
          bp.knockback = w.bombKnockback;
          bp.life = w.life;
          bp.initialLife = w.life;
          bp.arcHeight = w.bombArcHeight;
          bp.startX = pt.x;
          bp.startY = pt.y;
          bp.targetX = tx;
          bp.targetY = ty;
          bp.fuse = w.bombFuse;
          bp.color = w.color;
          registry_.emplace<BombProjectile>(bomb, bp);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Boomerang: {
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * w.spread;
          const float angle = baseAngle + offset;
          const auto boom = registry_.create();
          registry_.emplace<Transform>(boom, pt.x, pt.y, pt.x, pt.y);
          registry_.emplace<Velocity>(boom, std::cos(angle) * w.speed, std::sin(angle) * w.speed);
          registry_.emplace<Radius>(boom, 0.16F);
          Sprite s{};
          s.color = w.color;
          s.circle = false; // rect for shuriken look
          registry_.emplace<Sprite>(boom, s);
          BoomerangProjectile bp{};
          bp.damage = damage;
          bp.pierce = pierce;
          bp.life = w.life;
          bp.maxRange = w.boomerangRange;
          bp.returnSpeed = w.boomerangReturnSpeed;
          bp.startX = pt.x;
          bp.startY = pt.y;
          bp.targetX = pt.x + std::cos(angle) * w.boomerangRange;
          bp.targetY = pt.y + std::sin(angle) * w.boomerangRange;
          bp.returning = false;
          bp.bounceCount = 0;
          bp.color = w.color;
          registry_.emplace<BoomerangProjectile>(boom, bp);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Bounce: {
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * w.spread;
          const float angle = baseAngle + offset;
          const auto proj = registry_.create();
          registry_.emplace<Transform>(proj, pt.x, pt.y, pt.x, pt.y);
          registry_.emplace<Velocity>(proj, std::cos(angle) * w.speed, std::sin(angle) * w.speed);
          registry_.emplace<Radius>(proj, 0.18F);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(proj, s);
          BounceProjectile bp{};
          bp.damage = damage;
          bp.pierce = pierce;
          bp.life = w.life;
          bp.maxBounces = w.bounceCount;
          bp.bounceRange = w.bounceRange;
          bp.damageMul = w.bounceDamageMul;
          bp.bounceCount = 0;
          bp.color = w.color;
          registry_.emplace<BounceProjectile>(proj, bp);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Beam: {
        if (found) {
          const float endX = pt.x + std::cos(baseAngle) * w.beamRange;
          const float endY = pt.y + std::sin(baseAngle) * w.beamRange;
          const auto beam = registry_.create();
          registry_.emplace<Transform>(beam, pt.x, pt.y, pt.x, pt.y);
          registry_.emplace<Radius>(beam, w.beamWidth * 0.5F);
          Sprite s{};
          s.color = w.color;
          s.circle = false;
          registry_.emplace<Sprite>(beam, s);
          BeamEffect be{};
          be.damage = damage;
          be.range = w.beamRange;
          be.width = w.beamWidth;
          be.duration = w.beamDuration;
          be.timer = w.beamDuration;
          be.startX = pt.x;
          be.startY = pt.y;
          be.endX = endX;
          be.endY = endY;
          be.color = w.color;
          registry_.emplace<BeamEffect>(beam, be);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Sweep: {
        // Sweep is centered on player, instant damage in arc
        const float sweepAngle = w.sweepAngle;
        const float sweepRadius = w.sweepRadius;
        const float halfAngle = sweepAngle * 0.5F;
        auto view = registry_.view<Transform, Health, Radius, Enemy>();
        for (const auto e : view) {
          const auto& et = view.get<Transform>(e);
          const float dx = et.x - pt.x;
          const float dy = et.y - pt.y;
          const float dist2 = dx * dx + dy * dy;
          if (dist2 > sweepRadius * sweepRadius) continue;
          const float angleToEnemy = std::atan2(dy, dx);
          // For sweep, we sweep from -halfAngle to +halfAngle around baseAngle
          // Actually sweep is 360 or 180 around player, so check if in arc
          float diff = angleToEnemy - baseAngle;
          while (diff > kPi) diff -= 2.0F * kPi;
          while (diff < -kPi) diff += 2.0F * kPi;
          if (std::abs(diff) <= halfAngle) {
            applyEnemyDamage(e, damage * stats_.damageMul);
            // Knockback
            auto* ev = registry_.try_get<Velocity>(e);
            if (ev) {
              const float pushAngle = angleToEnemy;
              ev->x += std::cos(pushAngle) * w.sweepKnockback;
              ev->y += std::sin(pushAngle) * w.sweepKnockback;
            }
            spawnParticles(et.x, et.y, {0.7F, 1.0F, 0.8F, 1.0F}, 6, 4.0F);
          }
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Chain: {
        // Chain lightning - find target and create chain effect
        if (found) {
          const auto chain = registry_.create();
          registry_.emplace<Transform>(chain, targetX, targetY, targetX, targetY);
          registry_.emplace<Radius>(chain, w.chainJumpRange);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(chain, s);
          ChainLightning cl{};
          cl.damage = damage;
          cl.maxJumps = w.chainMaxJumps;
          cl.jumpRange = w.chainJumpRange;
          cl.damageMul = w.chainDamageMul;
          cl.jumpsDone = 0;
          cl.timer = 0.0F;
          cl.color = w.color;
          registry_.emplace<ChainLightning>(chain, cl);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Nova: {
        // Nova - expanding ring from player
        const auto nova = registry_.create();
        registry_.emplace<Transform>(nova, pt.x, pt.y, pt.x, pt.y);
        registry_.emplace<Radius>(nova, w.novaMaxRadius);
        Sprite s{};
        s.color = w.color;
        s.circle = true;
        registry_.emplace<Sprite>(nova, s);
        NovaRing nr{};
        nr.damagePerTick = w.novaDamagePerTick;
        nr.maxRadius = w.novaMaxRadius;
        nr.expandSpeed = w.novaExpandSpeed;
        nr.tickRate = w.novaTickRate;
        nr.radius = 0.0F;
        nr.timer = 0.0F;
        nr.tickTimer = 0.0F;
        nr.color = w.color;
        registry_.emplace<NovaRing>(nova, nr);
        w.timer = cooldown;
        break;
      }
      case AttackType::Zone: {
        // Zone - create damage zone at target location
        if (found) {
          for (int p = 0; p < count; ++p) {
            const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * w.spread;
            const float angle = baseAngle + offset;
            const float zx = pt.x + std::cos(angle) * w.speed * 0.5F;
            const float zy = pt.y + std::sin(angle) * w.speed * 0.5F;
            const auto zone = registry_.create();
            registry_.emplace<Transform>(zone, zx, zy, zx, zy);
            registry_.emplace<Radius>(zone, w.zoneRadius);
            Sprite s{};
            s.color = w.color;
            s.circle = true;
            s.color.a = 0.3F;
            registry_.emplace<Sprite>(zone, s);
            ZoneEffect ze{};
            ze.dps = w.zoneDps;
            ze.radius = w.zoneRadius;
            ze.duration = w.zoneDuration;
            ze.tickRate = w.coneTickRate;
            ze.timer = 0.0F;
            ze.tickTimer = 0.0F;
            ze.color = w.color;
            registry_.emplace<ZoneEffect>(zone, ze);
          }
        }
        w.timer = cooldown;
        break;
      }
    }
  }
}

void Game::applyEnemyDamage(entt::entity e, float dmg) {
  if (!registry_.valid(e)) return;
  auto* eh = registry_.try_get<Health>(e);
  if (eh == nullptr || eh->hp <= 0.0F) return;
  auto* tr = registry_.try_get<EnemyTraits>(e);
  if (tr != nullptr && tr->shield > 0.0F) {
    const float absorbed = std::min(tr->shield, dmg);
    tr->shield -= absorbed;
    dmg -= absorbed;
    spawnParticles(registry_.get<Transform>(e).x, registry_.get<Transform>(e).y,
                   {0.5F, 0.8F, 1.0F, 1.0F}, 2, 2.0F);
  }
  eh->hp -= dmg;
  if (eh->hp <= 0.0F) {
    killEnemy(e);
  }
}

void Game::killEnemy(entt::entity e) {
  if (!registry_.valid(e)) return;
  const auto& t = registry_.get<Transform>(e);

  // Death: XP orb + counter; actual destroy deferred.
  const float xpValue = registry_.all_of<Xp>(e) ? registry_.get<Xp>(e).value : 1.0F;
  const auto orb = registry_.create();
  registry_.emplace<Transform>(orb, t.x, t.y, t.x, t.y);
  registry_.emplace<Velocity>(orb);
  registry_.emplace<Radius>(orb, 0.16F);
  Sprite s{};
  s.color = {0.4F, 1.0F, 0.5F, 1.0F};
  s.circle = true;
  registry_.emplace<Sprite>(orb, s);
  registry_.emplace<Xp>(orb, xpValue);
  destroyQueue_.push_back(e);
  ++kills_;

  // Blood price: every 20 kills detonates a burst around the player.
  ++bloodKills_;
  if (stats_.bloodPrice != 0 && bloodKills_ >= 20) {
    bloodKills_ = 0;
    if (player_ != entt::null && registry_.valid(player_)) {
      const auto& pt = registry_.get<Transform>(player_);
      std::vector<entt::entity> targets;
      auto pl = registry_.view<Transform, Health>();
      for (const auto other : pl) {
        if (other == player_) continue;
        const auto& ot = pl.get<Transform>(other);
        const float dx = ot.x - pt.x;
        const float dy = ot.y - pt.y;
        if (dx * dx + dy * dy < 1.6F * 1.6F) {
          targets.push_back(other);
        }
      }
      for (const auto other : targets) {
        applyEnemyDamage(other, 40.0F);
      }
      spawnParticles(pt.x, pt.y, {0.9F, 0.2F, 0.9F, 1.0F}, 14, 6.0F);
    }
  }

  // Explosive trait: the corpse detonates if the player is close.
  if (auto* tr = registry_.try_get<EnemyTraits>(e); tr != nullptr && (tr->flags & TraitExplosive)) {
    const auto& en = registry_.get<Enemy>(e);
    if (player_ != entt::null && registry_.valid(player_)) {
      const auto& pt = registry_.get<Transform>(player_);
      const float dx = pt.x - t.x;
      const float dy = pt.y - t.y;
      if (dx * dx + dy * dy < 1.2F * 1.2F) {
        hurtPlayer(en.touch * 1.2F);
      }
    }
    spawnParticles(t.x, t.y, {1.0F, 0.5F, 0.1F, 1.0F}, 12, 6.0F);
  }

  spawnParticles(t.x, t.y, {0.9F, 0.3F, 0.5F, 1.0F}, 8, 5.0F);
}

void Game::chainBolt(float x, float y, float dmg) {
  std::vector<entt::entity> targets;
  auto view = registry_.view<Transform, Health>();
  for (const auto e : view) {
    if (e == player_) continue;
    const auto& t = view.get<Transform>(e);
    const float dx = t.x - x;
    const float dy = t.y - y;
    if (dx * dx + dy * dy < 2.0F * 2.0F) {
      targets.push_back(e);
      if (targets.size() >= 3) break;
    }
  }
  for (const auto e : targets) {
    applyEnemyDamage(e, 0.5F * dmg);
    const auto& t = registry_.get<Transform>(e);
    spawnParticles(t.x, t.y, {0.5F, 0.9F, 1.0F, 1.0F}, 3, 3.0F);
  }
}

void Game::updateProjectiles() {
  auto view = registry_.view<Transform, Velocity, Projectile, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);
    auto& pr = view.get<Projectile>(e);
    const auto& r = view.get<Radius>(e);

    t.px = t.x;
    t.py = t.y;

    // Homing: steer toward the nearest enemy each tick.
    if (pr.homing) {
      float bestDist2 = 1e12F;
      float tx = t.x, ty = t.y;
      auto enemies = registry_.view<Transform, Enemy>();
      for (const auto oe : enemies) {
        const auto& ot = enemies.get<Transform>(oe);
        const float dx = ot.x - t.x;
        const float dy = ot.y - t.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) { bestDist2 = d2; tx = ot.x; ty = ot.y; }
      }
      if (bestDist2 < 1e11F) {
        const float a = std::atan2(ty - t.y, tx - t.x);
        const float cur = std::atan2(v.y, v.x);
        float diff = a - cur;
        while (diff >  kPi)  diff -= 2.0F * kPi;
        while (diff < -kPi)  diff += 2.0F * kPi;
        const float steer = std::clamp(diff, -0.12F, 0.12F);
        const float ca = cur + steer;
        v.x = std::cos(ca) * std::sqrt(v.x * v.x + v.y * v.y);
        v.y = std::sin(ca) * std::sqrt(v.x * v.x + v.y * v.y);
      }
    }

    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;
    pr.life -= 1.0F / 60.0F;
    if (pr.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    bool spent = false;
    hash_.forEachNear(t.x, t.y, r.r + 0.7F, [&](std::uint32_t id) {
      if (spent) return;
      const auto enemy = static_cast<entt::entity>(id);
      if (!registry_.valid(enemy) || !registry_.all_of<Enemy>(enemy)) return;
      auto* eh = registry_.try_get<Health>(enemy);
      if (eh == nullptr || eh->hp <= 0.0F) return;
      const auto& et = registry_.get<Transform>(enemy);
      const auto& er = registry_.get<Radius>(enemy);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = r.r + er.r;
      if (dx * dx + dy * dy > hitR * hitR) return;

      // Damage, then lifesteal off the damage actually dealt.
      const float hpBefore = eh->hp;
      applyEnemyDamage(enemy, pr.damage);
      const float dealt = hpBefore - eh->hp;
      if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
        auto& hp = registry_.get<Health>(player_);
        hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
      }

      // Knockback on hit.
      if (pr.strength > 0.0F && player_ != entt::null) {
        const auto& ptx = registry_.get<Transform>(player_);
        const float pushAngle = std::atan2(et.y - ptx.y, et.x - ptx.x);
        auto* ev = registry_.try_get<Velocity>(enemy);
        if (ev) {
          ev->x += std::cos(pushAngle) * pr.strength / 60.0F;
          ev->y += std::sin(pushAngle) * pr.strength / 60.0F;
        }
      }

      // Area splash.
      if (pr.area > 0.0F) {
        auto splashView = registry_.view<Transform, Health, Radius>();
        for (const auto se : splashView) {
          const auto& st = splashView.get<Transform>(se);
          const auto& sr = splashView.get<Radius>(se); (void)sr;
          const float sdx = st.x - et.x;
          const float sdy = st.y - et.y;
          if (sdx * sdx + sdy * sdy < pr.area * pr.area) {
            applyEnemyDamage(se, pr.damage * 0.5F);
          }
        }
        spawnParticles(et.x, et.y, {1.0F, 0.5F, 0.1F, 1.0F}, 16, 5.0F);
      }

      // Chain lightning on every third hit.
      if (stats_.chain != 0 && dealt > 0.0F) {
        ++chainCounter_;
        if (chainCounter_ % 3 == 0) {
          chainBolt(et.x, et.y, pr.damage);
        }
      }

      spawnParticles(et.x, et.y, {1.0F, 0.8F, 0.3F, 1.0F}, 3, 3.0F);
      --pr.pierce;

      // Bounce logic.
      if (pr.bounces > 0 && pr.bounceCount < pr.bounces) {
        ++pr.bounceCount;
        // Reflect velocity off the nearest enemy surface normal.
        const float nx = dx / hitR;
        const float ny = dy / hitR;
        const float dot = v.x * nx + v.y * ny;
        v.x = (v.x - 2.0F * dot * nx) * 0.7F;
        v.y = (v.y - 2.0F * dot * ny) * 0.7F;
        spent = true;
        return; // keep the projectile alive, don't destroy
      }

      if (pr.pierce < 0) {
        spent = true;
        destroyQueue_.push_back(e);
      }
    });
  }
}

void Game::updateOrbitBlades() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const float scaledDamageMul = stats_.damageMul;
  const int scaledPierceAdd = stats_.pierceAdd;
  auto view = registry_.view<Transform, OrbitBlade>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& ob = view.get<OrbitBlade>(e);

    t.px = t.x;
    t.py = t.y;

    ob.angle += ob.speed / 60.0F;
    t.x = pt.x + std::cos(ob.angle) * ob.radius;
    t.y = pt.y + std::sin(ob.angle) * ob.radius;

    // Check collision with enemies
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    for (const auto en : enemyView) {
      const auto& et = enemyView.get<Transform>(en);
      const auto& er = enemyView.get<Radius>(en);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = er.r + 0.18F;
      if (dx * dx + dy * dy > hitR * hitR) continue;

      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;

      const float damage = ob.damage * scaledDamageMul;
      const int pierce = ob.pierce + scaledPierceAdd; (void)pierce;
      const float hpBefore = eh->hp;
      applyEnemyDamage(en, damage);
      const float dealt = hpBefore - eh->hp;
      if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
        auto& hp = registry_.get<Health>(player_);
        hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
      }
      spawnParticles(et.x, et.y, {0.85F, 0.9F, 1.0F, 1.0F}, 2, 2.0F);
    }
  }
}

void Game::updateBombProjectiles() {
  auto view = registry_.view<Transform, Velocity, BombProjectile, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);
    auto& bp = view.get<BombProjectile>(e);
    const auto& r = view.get<Radius>(e);

    t.px = t.x;
    t.py = t.y;

    const float dt = 1.0F / 60.0F;
    bp.life -= dt;
    if (bp.life <= 0.0F) {
      // Explode on timeout
      explodeBomb(e, bp, t.x, t.y);
      destroyQueue_.push_back(e);
      continue;
    }

    // Proper parabolic arc: apply gravity
    // Gravity scaled to game units (world units per second^2)
    const float gravity = -30.0F; // tuned for game scale
    v.y += gravity * dt;
    t.x += v.x * dt;
    t.y += v.y * dt;

    // Check collision with enemies (explode on impact)
    bool hit = false;
    hash_.forEachNear(t.x, t.y, r.r + 1.0F, [&](std::uint32_t id) {
      if (hit) return;
      const auto enemy = static_cast<entt::entity>(id);
      if (!registry_.valid(enemy) || !registry_.all_of<Enemy>(enemy)) return;
      auto* eh = registry_.try_get<Health>(enemy);
      if (eh == nullptr || eh->hp <= 0.0F) return;
      const auto& et = registry_.get<Transform>(enemy);
      const auto& er = registry_.get<Radius>(enemy);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = r.r + er.r;
      if (dx * dx + dy * dy > hitR * hitR) return;

      hit = true;
      explodeBomb(e, bp, et.x, et.y);
      destroyQueue_.push_back(e);
    });
  }
}

void Game::explodeBomb(entt::entity, const BombProjectile& bp, float x, float y) {
  // Explosion damage in radius
  auto view = registry_.view<Transform, Health, Radius, Enemy>();
  for (const auto e : view) {
    const auto& t = view.get<Transform>(e);
    const float dx = t.x - x;
    const float dy = t.y - y;
    if (dx * dx + dy * dy < bp.explodeRadius * bp.explodeRadius) {
      applyEnemyDamage(e, bp.damage);
      // Knockback
      if (bp.knockback > 0.0F) {
        auto* ev = registry_.try_get<Velocity>(e);
        if (ev) {
          const float angle = std::atan2(t.y - y, t.x - x);
          ev->x += std::cos(angle) * bp.knockback;
          ev->y += std::sin(angle) * bp.knockback;
        }
      }
      spawnParticles(t.x, t.y, {1.0F, 0.5F, 0.1F, 1.0F}, 4, 3.0F);
    }
  }
  // Big explosion particles
  spawnParticles(x, y, {1.0F, 0.5F, 0.1F, 1.0F}, 20, 6.0F);
  spawnParticles(x, y, {1.0F, 1.0F, 0.5F, 1.0F}, 10, 4.0F);
}

void Game::updateBoomerangProjectiles() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);

  auto view = registry_.view<Transform, Velocity, BoomerangProjectile, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);
    auto& bp = view.get<BoomerangProjectile>(e);
    const auto& r = view.get<Radius>(e);

    t.px = t.x;
    t.py = t.y;

    bp.life -= 1.0F / 60.0F;
    if (bp.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    if (!bp.returning) {
      // Check if reached max range
      const float dx = t.x - bp.startX;
      const float dy = t.y - bp.startY;
      const float dist2 = dx * dx + dy * dy;
      if (dist2 >= bp.maxRange * bp.maxRange) {
        bp.returning = true;
      }
    }

    if (bp.returning) {
      // Return to player
      const float dx = pt.x - t.x;
      const float dy = pt.y - t.y;
      const float dist = std::sqrt(dx * dx + dy * dy);
      if (dist > 0.001F) {
        v.x = (dx / dist) * bp.returnSpeed;
        v.y = (dy / dist) * bp.returnSpeed;
      }
      // Check if reached player
      if (dist < 0.5F) {
        destroyQueue_.push_back(e);
        continue;
      }
    }

    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    // Collision with enemies
    bool spent = false;
    hash_.forEachNear(t.x, t.y, r.r + 0.7F, [&](std::uint32_t id) {
      if (spent) return;
      const auto enemy = static_cast<entt::entity>(id);
      if (!registry_.valid(enemy) || !registry_.all_of<Enemy>(enemy)) return;
      auto* eh = registry_.try_get<Health>(enemy);
      if (eh == nullptr || eh->hp <= 0.0F) return;
      const auto& et = registry_.get<Transform>(enemy);
      const auto& er = registry_.get<Radius>(enemy);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = r.r + er.r;
      if (dx * dx + dy * dy > hitR * hitR) return;

      const float hpBefore = eh->hp;
      applyEnemyDamage(enemy, bp.damage);
      const float dealt = hpBefore - eh->hp;
      if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
        auto& hp = registry_.get<Health>(player_);
        hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
      }

      spawnParticles(et.x, et.y, {0.6F, 0.9F, 1.0F, 1.0F}, 3, 3.0F);
      --bp.pierce;
      if (bp.pierce < 0) {
        spent = true;
        destroyQueue_.push_back(e);
      }
    });
  }
}

void Game::updateBounceProjectiles() {
  auto view = registry_.view<Transform, Velocity, BounceProjectile, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);
    auto& bp = view.get<BounceProjectile>(e);
    const auto& r = view.get<Radius>(e);

    t.px = t.x;
    t.py = t.y;

    bp.life -= 1.0F / 60.0F;
    if (bp.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    bool spent = false;
    hash_.forEachNear(t.x, t.y, r.r + 0.7F, [&](std::uint32_t id) {
      if (spent) return;
      const auto enemy = static_cast<entt::entity>(id);
      if (!registry_.valid(enemy) || !registry_.all_of<Enemy>(enemy)) return;
      if (bp.bounceCount > 0) {
        // Check if we hit the same enemy twice in a row
        const auto& et = registry_.get<Transform>(enemy);
        const float dx = et.x - bp.lastHitX;
        const float dy = et.y - bp.lastHitY;
        if (dx * dx + dy * dy < 0.1F) return;
      }
      auto* eh = registry_.try_get<Health>(enemy);
      if (eh == nullptr || eh->hp <= 0.0F) return;
      const auto& et = registry_.get<Transform>(enemy);
      const auto& er = registry_.get<Radius>(enemy);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = r.r + er.r;
      if (dx * dx + dy * dy > hitR * hitR) return;

      const float scaledDamage = bp.damage * stats_.damageMul * std::powf(bp.damageMul, static_cast<float>(bp.bounceCount));
      const int pierce = bp.pierce + stats_.pierceAdd; (void)pierce;
      const float hpBefore = eh->hp;
      applyEnemyDamage(enemy, scaledDamage);
      const float dealt = hpBefore - eh->hp;
      if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
        auto& hp = registry_.get<Health>(player_);
        hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
      }

      // Find next bounce target
      bp.bounceCount++;
      bp.lastHitX = et.x;
      bp.lastHitY = et.y;

      if (bp.bounceCount >= bp.maxBounces) {
        spent = true;
        destroyQueue_.push_back(e);
        return;
      }

      // Find next enemy in range
      auto enemies = registry_.view<Transform, Enemy, Health>();
      float bestDist2 = bp.bounceRange * bp.bounceRange;
      float nextTx = 0.0F, nextTy = 0.0F;
      bool found = false;
      for (const auto oe : enemies) {
        if (oe == enemy) continue;
        const auto& ot = enemies.get<Transform>(oe);
        const auto* oeh = registry_.try_get<Health>(oe);
        if (!oeh || oeh->hp <= 0.0F) continue;
        const float cdx = ot.x - et.x;
        const float cdy = ot.y - et.y;
        const float cd2 = cdx * cdx + cdy * cdy;
        if (cd2 < bestDist2) {
          bestDist2 = cd2;
          nextTx = ot.x;
          nextTy = ot.y;
          found = true;
        }
      }
      if (found) {
        const float angle = std::atan2(nextTy - et.y, nextTx - et.x);
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        v.x = std::cos(angle) * speed;
        v.y = std::sin(angle) * speed;
      }

      spawnParticles(et.x, et.y, {0.75F, 0.45F, 1.0F, 1.0F}, 4, 3.0F);
    });
  }
}

void Game::updateBeamEffects() {
  auto view = registry_.view<Transform, BeamEffect, Radius>();
  for (const auto e : view) {
    auto& be = view.get<BeamEffect>(e);

    be.timer -= 1.0F / 60.0F;
    if (be.timer <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    // Beam deals damage along its line each tick
    const float dx = be.endX - be.startX;
    const float dy = be.endY - be.startY;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001F) continue;

    // Damage enemies along the beam line
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    for (const auto en : enemyView) {
      const auto& et = enemyView.get<Transform>(en);
      const auto& er = enemyView.get<Radius>(en);
      // Distance from point to line segment
      const float lx = et.x - be.startX;
      const float ly = et.y - be.startY;
      const float proj = (lx * dx + ly * dy) / len;
      if (proj < 0.0F || proj > len) continue;
      const float closestX = be.startX + (dx / len) * proj;
      const float closestY = be.startY + (dy / len) * proj;
      const float ddx = et.x - closestX;
      const float ddy = et.y - closestY;
      const float distToLine = std::sqrt(ddx * ddx + ddy * ddy);
      if (distToLine > be.width * 0.5F + er.r) continue;

      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;

      const float hpBefore = eh->hp;
      applyEnemyDamage(en, be.damage * stats_.damageMul / 60.0F); // per-tick damage with scaling
      const float dealt = hpBefore - eh->hp;
      if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
        auto& hp = registry_.get<Health>(player_);
        hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
      }
      spawnParticles(et.x, et.y, {1.0F, 1.0F, 0.75F, 1.0F}, 2, 2.0F);
    }
  }
}

void Game::updateSweepEffects() {
  auto view = registry_.view<Transform, SweepEffect, Radius>();
  for (const auto e : view) {
    auto& se = view.get<SweepEffect>(e);

    se.timer -= 1.0F / 60.0F;
    if (se.timer <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    // Sweep effect is created at player position, deals damage once
    // Actually we handle damage in fireWeapons for instant sweep
    // This is just for visual
  }
}

void Game::updateZoneEffects() {
  auto view = registry_.view<Transform, ZoneEffect, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& ze = view.get<ZoneEffect>(e);

    ze.timer += 1.0F / 60.0F;
    ze.tickTimer += 1.0F / 60.0F;

    if (ze.timer >= ze.duration) {
      destroyQueue_.push_back(e);
      continue;
    }

    if (ze.tickTimer >= ze.tickRate) {
      ze.tickTimer = 0.0F;
      // Damage enemies in zone
      auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
      for (const auto en : enemyView) {
        const auto& et = enemyView.get<Transform>(en);
        const auto& er = enemyView.get<Radius>(en);
        const float dx = et.x - t.x;
        const float dy = et.y - t.y;
        if (dx * dx + dy * dy < (ze.radius + er.r) * (ze.radius + er.r)) {
          auto* eh = registry_.try_get<Health>(en);
          if (eh == nullptr || eh->hp <= 0.0F) continue;

          const float dmg = ze.dps * ze.tickRate * stats_.damageMul;
          const float hpBefore = eh->hp;
          applyEnemyDamage(en, dmg);
          const float dealt = hpBefore - eh->hp;
          if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
            auto& hp = registry_.get<Health>(player_);
            hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
          }
          spawnParticles(et.x, et.y, {1.0F, 0.4F, 0.15F, 1.0F}, 2, 2.0F);
        }
      }
    }
  }
}

void Game::updateChainLightning() {
  auto view = registry_.view<Transform, ChainLightning, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& cl = view.get<ChainLightning>(e);

    if (cl.jumpsDone >= cl.maxJumps) {
      destroyQueue_.push_back(e);
      continue;
    }

    cl.timer += 1.0F / 60.0F;
    if (cl.timer < 0.05F) continue; // small delay between jumps
    cl.timer = 0.0F;

    // Find next target
    float bestDist2 = cl.jumpRange * cl.jumpRange;
    entt::entity nextTarget = entt::null;
    auto enemies = registry_.view<Transform, Health, Enemy>();
    for (const auto oe : enemies) {
      if (oe == e) continue; // shouldn't happen
      const auto& ot = enemies.get<Transform>(oe);
      const auto* oeh = registry_.try_get<Health>(oe);
      if (!oeh || oeh->hp <= 0.0F) continue;
      const float dx = ot.x - t.x;
      const float dy = ot.y - t.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 < bestDist2) {
        bestDist2 = d2;
        nextTarget = oe;
      }
    }

    if (nextTarget == entt::null) {
      destroyQueue_.push_back(e);
      continue;
    }

    // Damage the target
    const float damage = cl.damage * stats_.damageMul * std::powf(cl.damageMul, static_cast<float>(cl.jumpsDone));
    auto* eh = registry_.try_get<Health>(nextTarget);
    if (eh && eh->hp > 0.0F) {
      const float hpBefore = eh->hp;
      applyEnemyDamage(nextTarget, damage);
      const float dealt = hpBefore - eh->hp;
      if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
        auto& hp = registry_.get<Health>(player_);
        hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
      }

      const auto& targetT = registry_.get<Transform>(nextTarget);
      spawnParticles(targetT.x, targetT.y, {0.55F, 1.0F, 1.0F, 1.0F}, 8, 4.0F);

      // Arc from current position to target
      spawnParticles(t.x, t.y, {0.55F, 1.0F, 1.0F, 0.5F}, 3, 2.0F);
      spawnParticles(targetT.x, targetT.y, {0.55F, 1.0F, 1.0F, 0.5F}, 3, 2.0F);
    }

    // Move chain lightning position to target
    const auto& targetT = registry_.get<Transform>(nextTarget);
    t.x = targetT.x;
    t.y = targetT.y;
    cl.jumpsDone++;
  }
}

void Game::updateNovaRing() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);

  auto view = registry_.view<Transform, NovaRing, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& nr = view.get<NovaRing>(e);

    // Follow player
    t.x = pt.x;
    t.y = pt.y;

    nr.timer += 1.0F / 60.0F;
    nr.tickTimer += 1.0F / 60.0F;
    nr.radius += nr.expandSpeed / 60.0F;

    if (nr.radius >= nr.maxRadius) {
      destroyQueue_.push_back(e);
      continue;
    }

    if (nr.tickTimer >= nr.tickRate) {
      nr.tickTimer = 0.0F;
      // Damage enemies in current ring
      auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
      for (const auto en : enemyView) {
        const auto& et = enemyView.get<Transform>(en);
        const auto& er = enemyView.get<Radius>(en);
        const float dx = et.x - pt.x;
        const float dy = et.y - pt.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        // Check if in current ring (thin ring)
        if (std::abs(dist - nr.radius) < er.r + 0.3F) {
          auto* eh = registry_.try_get<Health>(en);
          if (eh == nullptr || eh->hp <= 0.0F) continue;

          const float hpBefore = eh->hp;
          applyEnemyDamage(en, nr.damagePerTick * stats_.damageMul);
          const float dealt = hpBefore - eh->hp;
          if (dealt > 0.0F && stats_.lifesteal > 0.0F && player_ != entt::null) {
            auto& hp = registry_.get<Health>(player_);
            hp.hp = std::min(hp.max, hp.hp + dealt * stats_.lifesteal);
          }
          spawnParticles(et.x, et.y, {0.5F, 0.3F, 1.0F, 1.0F}, 4, 3.0F);
        }
      }
    }
  }
}

void Game::updatePickups() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& pt = registry_.get<Transform>(player_);
  const float magnet = 2.0F * stats_.pickupMul;

  auto view = registry_.view<Transform, Velocity, Xp, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);

    t.px = t.x;
    t.py = t.y;

    const float dx = pt.x - t.x;
    const float dy = pt.y - t.y;
    const float dist = length(dx, dy);

    if (dist < magnet && dist > 0.001F) {
      const float pull = 14.0F;
      v.x = (dx / dist) * pull;
      v.y = (dy / dist) * pull;
    } else {
      v.x *= 0.9F;
      v.y *= 0.9F;
    }
    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    if (dist < kPickupDist) {
      xp_ += registry_.get<Xp>(e).value;
      destroyQueue_.push_back(e);
    }
  }
}

void Game::updateShield() {
  if (stats_.shieldMax <= 0.0F) return;
  if (shieldDelay_ > 0.0F) {
    shieldDelay_ -= 1.0F / 60.0F;
    return;
  }
  shield_ = std::min(stats_.shieldMax, shield_ + kShieldRegenRate / 60.0F);
}

void Game::updateUniqueEffects() {
  if (stats_.blackHole != 0 && player_ != entt::null && registry_.valid(player_)) {
    blackHoleTimer_ -= 1.0F / 60.0F;
    if (blackHoleTimer_ <= 0.0F) {
      blackHoleTimer_ = 12.0F;
      const auto& pt = registry_.get<Transform>(player_);
      auto view = registry_.view<Transform, Enemy>();
      for (const auto e : view) {
        auto& t = view.get<Transform>(e);
        const float dx = pt.x - t.x;
        const float dy = pt.y - t.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < 3.0F * 3.0F && d2 > 0.0001F) {
          const float d = std::sqrt(d2);
          t.x += (dx / d) * 1.1F;
          t.y += (dy / d) * 1.1F;
          spawnParticles(t.x, t.y, {0.6F, 0.4F, 1.0F, 1.0F}, 1, 2.0F);
        }
      }
      spawnParticles(pt.x, pt.y, {0.6F, 0.4F, 1.0F, 1.0F}, 10, 4.0F);
    }
  }
  if (stats_.adrenaline != 0 && adrenalineCd_ > 0.0F) {
    adrenalineCd_ -= 1.0F / 60.0F;
  }

  // Venomous contact poison: a small defenseless DoT.
  if (poison_ > 0.0F && player_ != entt::null && registry_.valid(player_)) {
    poison_ -= 1.0F / 60.0F;
    auto& hp = registry_.get<Health>(player_);
    hp.hp -= 3.0F / 60.0F;
    if (hp.hp <= 0.0F) {
      hp.hp = 0.0F;
      state_ = RunState::GameOver;
    }
  }
}

void Game::spawnWave() {
  if (content_.enemies.empty()) return;
  if (registry_.view<Enemy>().size() >= kMaxEnemies) return;

  spawnTimer_ -= 1.0F / 60.0F;
  if (spawnTimer_ > 0.0F) return;

  // Two-phase ramp: gentle through the first minute, aggressive afterwards.
  if (simTime_ < 45.0F) {
    spawnTimer_ = 1.4F - simTime_ * 0.004F;
  } else {
    spawnTimer_ = std::max(0.22F, 1.22F - (simTime_ - 45.0F) * 0.02F);
  }

  // Weighted pick among unlocked enemy types.
  float totalWeight = 0.0F;
  for (const auto& def : content_.enemies) {
    if (simTime_ >= def.unlockAt) {
      totalWeight += def.weight;
    }
  }
  if (totalWeight <= 0.0F) return;

  std::uniform_real_distribution<float> unit(0.0F, 1.0F);
  float roll = unit(rng_) * totalWeight;
  int defIndex = -1;
  for (std::size_t i = 0; i < content_.enemies.size(); ++i) {
    const auto& candidate = content_.enemies[i];
    if (simTime_ < candidate.unlockAt) continue;
    roll -= candidate.weight;
    if (roll <= 0.0F) {
      defIndex = static_cast<int>(i);
      break;
    }
  }
  if (defIndex < 0) return;

  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);

  // Gradual HP scaling (ramps from ~1 minute onward).
  const float hpScale = 1.0F + simTime_ / 90.0F;

  // Elite / champion rolls. Trait count grows with time (1 + t/90, max 4).
  bool elite = false;
  bool champion = false;
  if (simTime_ >= 60.0F && unit(rng_) < 0.05F) elite = true;
  if (simTime_ >= 150.0F && unit(rng_) < 0.02F) champion = true;
  if (champion) elite = true;

  float hpMul = hpScale; (void)hpMul;
  float touchMul = 1.0F; (void)touchMul;
  float speedMul = 1.0F; (void)speedMul;
  float xpMul = 1.0F; (void)xpMul;
  std::uint32_t traits = TraitNone; (void)traits;
  std::uint8_t tier = 0; (void)tier;
  if (elite) {
    tier = champion ? 2 : 1;
    hpMul *= champion ? kChampionHpMul : kEliteHpMul;
    touchMul = champion ? 2.0F : 1.5F;
    xpMul = champion ? 5.0F : 3.0F;
    const int traitCount =
        std::min(4, 1 + static_cast<int>(simTime_ / 90.0F)) + (champion ? 1 : 0);
    std::array<bool, PickCount> used{};
    for (int k = 0; k < traitCount; ++k) {
      int pick = static_cast<int>(unit(rng_) * static_cast<float>(PickCount));
      int guard = static_cast<int>(PickCount);
      while (guard-- > 0 && used[static_cast<std::size_t>(pick)]) {
        pick = (pick + 1) % static_cast<int>(PickCount);
      }
      used[static_cast<std::size_t>(pick)] = true;
      switch (pick) {
        case PickFast: speedMul *= 1.7F; break;
        case PickArmored: hpMul *= 2.5F; touchMul *= 1.3F; break;
        case PickRegenerating: traits |= TraitRegenerating; break;
        case PickExplosive: traits |= TraitExplosive; break;
        case PickVenomous: traits |= TraitVenomous; break;
        case PickVampiric: traits |= TraitVampiric; break;
        case PickShielded: traits |= TraitShielded; break;
        default: break;
      }
    }
  }

  // Pack spawning: after ~90s, groups of 2-3 enemies arrive together.
  const int packSize = (simTime_ >= 90.0F && unit(rng_) < 0.40F)
                       ? static_cast<int>(2.0F + unit(rng_) * 1.5F)
                       : 1;

  // Cluster center: a random angle at spawn distance. Subsequent pack
  // members spawn close to this center so enemies approach together.
  const float centerAngle = unit(rng_) * 2.0F * kPi;

  for (int member = 0; member < packSize; ++member) {
    const float angle = centerAngle
        + (packSize > 1 ? (static_cast<float>(member) - static_cast<float>(packSize - 1) * 0.5F) * 0.35F : 0.0F)
        + (member > 0 ? (unit(rng_) - 0.5F) * 0.5F : 0.0F);
    const float x = pt.x + std::cos(angle) * kSpawnDist;
    const float y = pt.y + std::sin(angle) * kSpawnDist;

    // Per-member elite/champion rolls (each enemy rolls independently).
    bool memberElite = false;
    bool memberChampion = false;
    if (simTime_ >= 60.0F && unit(rng_) < 0.05F) memberElite = true;
    if (simTime_ >= 150.0F && unit(rng_) < 0.02F) memberChampion = true;
    if (memberChampion) memberElite = true;

    float mHpMul = hpScale;
    float mTouchMul = 1.0F;
    float mSpeedMul = 1.0F;
    float mXpMul = 1.0F;
    std::uint32_t mTraits = TraitNone;
    std::uint8_t mTier = 0;
    if (memberElite) {
      mTier = memberChampion ? 2 : 1;
      mHpMul *= memberChampion ? kChampionHpMul : kEliteHpMul;
      mTouchMul = memberChampion ? 2.0F : 1.5F;
      mXpMul = memberChampion ? 5.0F : 3.0F;
      const int traitCount =
          std::min(4, 1 + static_cast<int>(simTime_ / 90.0F)) + (memberChampion ? 1 : 0);
      std::array<bool, PickCount> used{};
      for (int k = 0; k < traitCount; ++k) {
        int pick = static_cast<int>(unit(rng_) * static_cast<float>(PickCount));
        int guard = static_cast<int>(PickCount);
        while (guard-- > 0 && used[static_cast<std::size_t>(pick)]) {
          pick = (pick + 1) % static_cast<int>(PickCount);
        }
        used[static_cast<std::size_t>(pick)] = true;
        switch (pick) {
          case PickFast: mSpeedMul *= 1.7F; break;
          case PickArmored: mHpMul *= 2.5F; mTouchMul *= 1.3F; break;
          case PickRegenerating: mTraits |= TraitRegenerating; break;
          case PickExplosive: mTraits |= TraitExplosive; break;
          case PickVenomous: mTraits |= TraitVenomous; break;
          case PickVampiric: mTraits |= TraitVampiric; break;
          case PickShielded: mTraits |= TraitShielded; break;
          default: break;
        }
      }
    }

    PendingSpawn pending{};
    pending.x = x;
    pending.y = y;
    pending.t = kSpawnTelegraph;
    pending.def = defIndex;
    pending.hpMul = std::max(1.0F, mHpMul);
    pending.touchMul = mTouchMul;
    pending.speedMul = mSpeedMul;
    pending.xpMul = mXpMul;
    pending.traits = mTraits;
    pending.tier = mTier;
    pending_.push_back(pending);
  }
}

void Game::processPendingSpawns() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    it->t -= 1.0F / 60.0F;
    if (it->t <= 0.0F) {
      spawnEnemy(*it);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void Game::spawnEnemy(const PendingSpawn& p) {
  const auto& def = content_.enemies[static_cast<std::size_t>(p.def)];
  const bool elite = p.tier > 0;
  const float hp = def.hp * p.hpMul;
  const float radius = def.radius * (elite ? 1.35F : 1.0F);

  const auto e = registry_.create();
  registry_.emplace<Transform>(e, p.x, p.y, p.x, p.y);
  registry_.emplace<Velocity>(e);
  registry_.emplace<Radius>(e, radius);
  Sprite s{};
  s.color = elite ? eliteTint(def.color) : def.color;
  s.circle = def.circle;
  registry_.emplace<Sprite>(e, s);
  registry_.emplace<Health>(e, hp, hp);
  registry_.emplace<Enemy>(e, def.speed * p.speedMul, def.touch * p.touchMul);
  registry_.emplace<Xp>(e, def.xp * p.xpMul);
  if (elite) {
    EnemyTraits tr{};
    tr.flags = p.traits;
    tr.tier = p.tier;
    if (p.traits & TraitRegenerating) tr.regen = hp * 0.02F;
    if (p.traits & TraitShielded) tr.shield = hp * 0.5F;
    registry_.emplace<EnemyTraits>(e, tr);
  }
}

void Game::enterLevelUp() {
  state_ = RunState::LevelUp;
  rerollsUsed_ = 0;
  buildChoices();
}

void Game::buildChoices() {
  choices_.clear();
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);

  const bool milestone = (level_ % 5 == 0);
  if (milestone) {
    std::vector<int> pool;
    pool.reserve(content_.upgrades.size());
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      const auto& u = content_.upgrades[i];
      if (u.kind == "milestone" && u.level == level_ && stacks_[i] < u.maxStacks) {
        pool.push_back(static_cast<int>(i));
      }
    }
    if (!pool.empty()) {
      std::shuffle(pool.begin(), pool.end(), rng_);
      milestoneOffer_ = true;
      const std::size_t take = std::min<std::size_t>(2, pool.size());
      choices_.reserve(take);
      for (std::size_t k = 0; k < take; ++k) {
        choices_.push_back({Choice::Kind::Upgrade, pool[k]});
      }
      return;
    }
    // No defined milestone for this exact level: behave like a normal level-up.
  }
  milestoneOffer_ = false;

  const int baseChoices = 3 + stats_.extraChoice;

  // One of the cards can be a NEW WEAPON (rarer the more weapons you have).
  if (weaponCount_ < kMaxWeapons) {
    const float grantChance =
        (1.0F - static_cast<float>(weaponCount_) / static_cast<float>(kMaxWeapons)) * 0.5F;
    if (unit(rng_) < grantChance) {
      const int w = pickWeaponGrant();
      if (w >= 0) {
        choices_.push_back({Choice::Kind::Weapon, w});
      }
    }
  }

  // A unique treasure card can replace the weapon offer.
  if (choices_.empty() || choices_.front().kind == Choice::Kind::Upgrade) {
    std::vector<int> uniques;
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      const auto& u = content_.upgrades[i];
      if (u.kind == "unique" && stacks_[i] < u.maxStacks) {
        uniques.push_back(static_cast<int>(i));
      }
    }
    if (!uniques.empty() && unit(rng_) < 0.45F) {
      std::shuffle(uniques.begin(), uniques.end(), rng_);
      choices_.push_back({Choice::Kind::Upgrade, uniques[0]});
    }
  }

  // Normal pool fills the remaining cards.
  std::vector<int> normals;
  normals.reserve(content_.upgrades.size());
  for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
    const auto& u = content_.upgrades[i];
    if (u.kind != "normal") continue;
    if (stacks_[i] >= u.maxStacks) continue;
    if (!u.weapon.empty() && findWeaponSlot(u.weapon) < 0) continue;
    normals.push_back(static_cast<int>(i));
  }
  std::shuffle(normals.begin(), normals.end(), rng_);

  const std::size_t want = static_cast<std::size_t>(std::max(1, baseChoices));
  std::size_t fill = want > choices_.size() ? want - choices_.size() : 0;
  fill = std::min(fill, normals.size());
  for (std::size_t k = 0; k < fill; ++k) {
    choices_.push_back({Choice::Kind::Upgrade, normals[k]});
  }

  // Absolute fallback so a level-up never bricks silently.
  if (choices_.empty()) {
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      if (stacks_[i] < content_.upgrades[i].maxStacks) {
        choices_.push_back({Choice::Kind::Upgrade, static_cast<int>(i)});
        break;
      }
    }
  }
}

void Game::chooseUpgrade(int slot) {
  if (slot < 0 || static_cast<std::size_t>(slot) >= choices_.size()) return;
  const auto choice = choices_[static_cast<std::size_t>(slot)];

  if (choice.kind == Choice::Kind::Weapon) {
    addWeapon(choice.index);
  } else {
    const auto& def = content_.upgrades[static_cast<std::size_t>(choice.index)];

    UpgradeEffectResult result{false, 0.0F, 0.0F};
    if (def.weapon.empty()) {
      result = applyUpgrade(stats_, def.effect, def.value);
      if (!result.valid) return;
    } else {
      // Weapon-targeted upgrade (e.g. a weapon's personal Focus card).
      const int weaponSlot = findWeaponSlot(def.weapon);
      if (weaponSlot < 0) return;
      applyWeaponEffect(weaponSlot, def.effect, def.value);
      result.valid = true;
    }
    ++stacks_[static_cast<std::size_t>(choice.index)];

    if (player_ != entt::null && registry_.valid(player_)) {
      auto& hp = registry_.get<Health>(player_);
      hp.max = stats_.maxHp;
      if (result.heal > 0.0F) {
        hp.hp = std::min(hp.max, hp.hp + result.heal);
      }
    }
    if (result.shield > 0.0F) {
      shield_ = stats_.shieldMax; // refill on pickup
    }
  }

  xp_ -= xpNext_;
  ++level_;
  xpNext_ = xpForLevel(level_);

  if (xp_ >= xpNext_) {
    enterLevelUp(); // queued level-ups
  } else {
    state_ = RunState::Playing;
  }
}

void Game::reroll() {
  if (state_ != RunState::LevelUp) return;
  if (rerollsUsed_ >= 1 + stats_.rerollCharges) return;
  ++rerollsUsed_;
  buildChoices();
}

void Game::addWeapon(int defIndex) {
  if (weaponCount_ >= kMaxWeapons) return;
  if (defIndex < 0 || static_cast<std::size_t>(defIndex) >= content_.weapons.size()) return;
  const auto& def = content_.weapons[static_cast<std::size_t>(defIndex)];
  auto& w = weapons_[weaponCount_++];
  w.def = defIndex;
  w.attackType = def.attackType;
  w.cooldown = def.cooldown;
  w.timer = 0.0F;
  w.damage = def.damage;
  w.projectiles = def.projectiles;
  w.speed = def.projSpeed;
  w.life = def.projLife;
  w.pierce = def.pierce;
  w.spread = def.spread;
  w.color = def.projColor;

  // Cone
  w.coneAngle = def.coneAngle;
  w.coneRange = def.coneRange;
  w.coneTickRate = def.coneTickRate;
  w.coneTimer = 0.0F;

  // Orbit
  w.orbitRadius = def.orbitRadius;
  w.orbitSpeed = def.orbitSpeed;
  w.orbitCount = def.orbitCount;
  w.orbitAngle = 0.0F;

  // Bomb
  w.bombArcHeight = def.bombArcHeight;
  w.bombExplodeRadius = def.bombExplodeRadius;
  w.bombKnockback = def.bombKnockback;
  w.bombFuse = def.bombFuse;

  // Boomerang
  w.boomerangRange = def.boomerangRange;
  w.boomerangReturnSpeed = def.boomerangReturnSpeed;

  // Bounce
  w.bounceCount = def.bounceCount;
  w.bounceRange = def.bounceRange;
  w.bounceDamageMul = def.bounceDamageMul;

  // Beam
  w.beamRange = def.beamRange;
  w.beamWidth = def.beamWidth;
  w.beamDuration = def.beamDuration;

  // Sweep
  w.sweepAngle = def.sweepAngle;
  w.sweepRadius = def.sweepRadius;
  w.sweepKnockback = def.sweepKnockback;

  // Zone
  w.zoneRadius = def.zoneRadius;
  w.zoneDuration = def.zoneDuration;
  w.zoneDps = def.zoneDps;
  w.zoneMaxPools = def.zoneMaxPools;

  // Chain
  w.chainJumpRange = def.chainJumpRange;
  w.chainMaxJumps = def.chainMaxJumps;
  w.chainDamageMul = def.chainDamageMul;

  // Nova
  w.novaMaxRadius = def.novaMaxRadius;
  w.novaExpandSpeed = def.novaExpandSpeed;
  w.novaDamagePerTick = def.novaDamagePerTick;
  w.novaTickRate = def.novaTickRate;
  w.novaRadius = 0.0F;
  w.novaTimer = 0.0F;
  w.novaActive = false;

  // General projectile fields
  w.area = 0.0F;
  w.strength = 0.0F;
  w.homing = false;
  w.bounces = 0;

  // Create orbit blades if this is an orbit weapon
  if (w.attackType == AttackType::Orbit && player_ != entt::null && registry_.valid(player_)) {
    const auto& pt = registry_.get<Transform>(player_);
    for (int b = 0; b < w.orbitCount; ++b) {
      const float angle = (static_cast<float>(b) * 2.0F * kPi / static_cast<float>(w.orbitCount));
      const auto blade = registry_.create();
      const float bx = pt.x + std::cos(angle) * w.orbitRadius;
      const float by = pt.y + std::sin(angle) * w.orbitRadius;
      registry_.emplace<Transform>(blade, bx, by, bx, by);
      registry_.emplace<Velocity>(blade);
      registry_.emplace<Radius>(blade, 0.18F);
      Sprite s{};
      s.color = w.color;
      s.circle = false;
      registry_.emplace<Sprite>(blade, s);
      OrbitBlade ob{};
      ob.damage = w.damage;
      ob.radius = w.orbitRadius;
      ob.speed = w.orbitSpeed;
      ob.angle = angle;
      ob.pierce = w.pierce;
      ob.color = w.color;
      registry_.emplace<OrbitBlade>(blade, ob);
    }
  }
}

void Game::applyWeaponEffect(int slotIndex, std::string_view effect, float value) {
  if (slotIndex < 0 || slotIndex >= weaponCount_) return;
  auto& w = weapons_[slotIndex];
  if (effect == "w_damage_add") {
    w.damage += value;
  } else if (effect == "w_proj_add") {
    w.projectiles += static_cast<int>(value);
  } else if (effect == "w_pierce_add") {
    w.pierce += static_cast<int>(value);
  } else if (effect == "w_cd_mul") {
    w.cooldown *= (1.0F + value);
  }
}

int Game::findWeaponSlot(std::string_view weaponId) const {
  for (int i = 0; i < weaponCount_; ++i) {
    const auto& def = content_.weapons[static_cast<std::size_t>(weapons_[i].def)];
    if (def.id == weaponId) {
      return i;
    }
  }
  return -1;
}

bool Game::ownsWeapon(int defIndex) const {
  for (int i = 0; i < weaponCount_; ++i) {
    if (weapons_[i].def == defIndex) return true;
  }
  return false;
}

int Game::pickWeaponGrant() {
  std::vector<int> evolutions;
  std::vector<int> normals;
  for (std::size_t i = 0; i < content_.weapons.size(); ++i) {
    if (ownsWeapon(static_cast<int>(i))) continue;
    const auto& def = content_.weapons[i];
    bool ready = true;
    for (const auto& req : def.prereqs) {
      if (findWeaponSlot(req) < 0) {
        ready = false;
        break;
      }
    }
    if (!def.prereqs.empty()) {
      if (ready) evolutions.push_back(static_cast<int>(i));
    } else {
      normals.push_back(static_cast<int>(i));
    }
  }
  if (!evolutions.empty()) {
    std::shuffle(evolutions.begin(), evolutions.end(), rng_);
    return evolutions.front();
  }
  if (!normals.empty()) {
    std::shuffle(normals.begin(), normals.end(), rng_);
    return normals.front();
  }
  return -1;
}

void Game::hurtPlayer(float amount) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& php = registry_.get<Health>(player_);
  if (php.hp <= 0.0F) return;

  // Thorns: getting hit detonates a burst around the player.
  if (stats_.thornsDmg > 0.0F) {
    const auto& pt = registry_.get<Transform>(player_);
    std::vector<entt::entity> targets;
    auto view = registry_.view<Transform, Health>();
    for (const auto e : view) {
      if (e == player_) continue;
      const auto& ot = view.get<Transform>(e);
      const float dx = ot.x - pt.x;
      const float dy = ot.y - pt.y;
      if (dx * dx + dy * dy < 1.4F * 1.4F) {
        targets.push_back(e);
      }
    }
    for (const auto e : targets) {
      applyEnemyDamage(e, amount * stats_.thornsDmg);
    }
    spawnParticles(pt.x, pt.y, {0.9F, 0.4F, 0.2F, 1.0F}, 8, 4.0F);
  }

  float dmg = mitigateDamage(amount, stats_.defense);

  // Regenerating shield absorbs the remainder first.
  if (shield_ > 0.0F) {
    const float absorbed = std::min(shield_, dmg);
    shield_ -= absorbed;
    dmg -= absorbed;
    shieldDelay_ = kShieldRegenDelay;
  }

  if (dmg > 0.0F) {
    php.hp -= dmg;
    if (php.hp <= 0.0F) {
      php.hp = 0.0F;
      state_ = RunState::GameOver;
    }
  }
}

void Game::grantXp(float amount) {
  xp_ += amount;
  if (state_ == RunState::Playing && xp_ >= xpNext_) {
    enterLevelUp();
  }
}


// Greedy word-wrap: renders str in lines that fit maxWidth pixels.
static void renderWrappedText(core::render::Batcher& b, float x, float y,
                              float scale, core::render::Color c,
                              std::string_view str, float maxWidth,
                              float lineHeight) {
  std::string current;
  auto flush = [&]() {
    if (!current.empty()) {
      b.text(x, y, scale, c, current);
      y += lineHeight;
      current.clear();
    }
  };
  // Manual word split to avoid <sstream>/<random> overload ambiguity.
  std::string_view rest(str);
  while (!rest.empty()) {
    const std::size_t space = rest.find(' ');
    const std::string_view word = rest.substr(0, space);
    if (space != std::string_view::npos) rest.remove_prefix(space + 1);
    else rest = std::string_view();
    const std::string trial = current.empty()
        ? std::string(word) : current + " " + std::string(word);
    if (b.textWidth(scale, trial) > maxWidth && !current.empty()) {
      flush();
    }
    current = trial;
  }
  flush();
}

void Game::spawnParticles(float x, float y, core::render::Color c, int count, float speed) {
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);
  for (int i = 0; i < count; ++i) {
    const float angle = unit(rng_) * 2.0F * kPi;
    const float mag = speed * (0.4F + unit(rng_) * 0.6F);
    Particle p{};
    p.x = x;
    p.y = y;
    p.vx = std::cos(angle) * mag;
    p.vy = std::sin(angle) * mag;
    p.maxLife = p.life = 0.25F + unit(rng_) * 0.3F;
    p.size = 0.05F + unit(rng_) * 0.07F;
    p.color = c;
    if (particles_.size() < kMaxParticles) {
      particles_.push_back(p);
    } else {
      particles_[particleCursor_ % kMaxParticles] = p;
      ++particleCursor_;
    }
  }
}

float Game::playerHp() const {
  if (player_ == entt::null || !registry_.valid(player_)) return 0.0F;
  return registry_.get<Health>(player_).hp;
}

float Game::playerMaxHp() const {
  if (player_ == entt::null || !registry_.valid(player_)) return stats_.maxHp;
  return registry_.get<Health>(player_).max;
}

std::size_t Game::enemyCount() const {
  return registry_.view<Enemy>().size();
}

int Game::upgradeStacks(std::size_t upgradeIndex) const {
  return upgradeIndex < stacks_.size() ? stacks_[upgradeIndex] : 0;
}

void Game::render(core::render::Batcher& b, float alpha) {
  using core::render::Color;

  const auto px = static_cast<float>(b.fbWidth());
  const auto py = static_cast<float>(b.fbHeight());

  // --- World pass -----------------------------------------------------------
  b.setWorldView(camX_, camY_, zoom_);

  // Ground grid.
  const float halfW = (px * 0.5F) / zoom_;
  const float halfH = (py * 0.5F) / zoom_;
  const Color gridColor{0.16F, 0.10F, 0.16F, 1.0F};
  const float cell = 4.0F;
  const float gx0 = std::floor((camX_ - halfW) / cell) * cell;
  const float gy0 = std::floor((camY_ - halfH) / cell) * cell;
  for (float gx = gx0; gx <= camX_ + halfW; gx += cell) {
    b.rect(gx, camY_, 0.03F, halfH * 2.0F, gridColor);
  }
  for (float gy = gy0; gy <= camY_ + halfH; gy += cell) {
    b.rect(camX_, gy, halfW * 2.0F, 0.03F, gridColor);
  }

  const float lerp = alpha;

  // Pending spawn telegraphs: pulsing rings where enemies are about to appear.
  for (const auto& p : pending_) {
    const auto& def = content_.enemies[static_cast<std::size_t>(p.def)];
    const float pulse = 1.0F + 0.25F * std::sin(p.t * 18.0F);
    Color ring = def.color;
    ring.a = 0.16F;
    b.circle(p.x, p.y, def.radius * 2.6F * pulse, ring);
    Color core = def.color;
    core.a = 0.6F;
    b.circle(p.x, p.y, def.radius * 0.5F, core);
  }

  // XP orbs.
  {
    auto view = registry_.view<Transform, Sprite, Xp, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& s = view.get<Sprite>(e);
      const auto& r = view.get<Radius>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      if (s.circle) b.circle(x, y, r.r, s.color);
      else b.rect(x, y, r.r * 2.0F, r.r * 2.0F, s.color);
    }
  }

  // Enemies (with tiny HP bar when damaged or when elite).
  {
    auto view = registry_.view<Transform, Sprite, Enemy, Radius, Health>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& s = view.get<Sprite>(e);
      const auto& r = view.get<Radius>(e);
      const auto& h = view.get<Health>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      if (s.circle) b.circle(x, y, r.r, s.color);
      else b.rect(x, y, r.r * 2.0F, r.r * 2.0F, s.color);

      const auto* tr = registry_.try_get<EnemyTraits>(e);
      if (h.hp < h.max || (tr != nullptr && tr->tier > 0)) {
        const float frac = h.hp / h.max;
        const float w = r.r * 2.0F;
        b.rect(x, y + r.r + 0.15F, w, 0.08F, Color{0.1F, 0.1F, 0.1F, 0.8F});
        b.rect(x - w * 0.5F * (1.0F - frac), y + r.r + 0.15F, w * frac, 0.08F,
               Color{0.9F, 0.25F, 0.25F, 1.0F});
      }

      // Elite/champion glow aura instead of a text label —
      // a pulsing ring makes them identifiable at a glance.
      if (tr != nullptr && tr->tier > 0) {
        const float pulse = 1.0F + 0.20F * std::sin(simTime_ * 6.0F);
        const float glowR = r.r * 1.6F * pulse;
        Color glow = tr->tier >= 2 ? Color{1.0F, 0.40F, 0.10F, 0.35F}
                                    : Color{1.0F, 0.85F, 0.20F, 0.30F};
        b.circle(x, y, glowR, glow);
        // Brighter inner ring.
        b.circle(x, y, r.r * 1.15F * pulse,
                 tr->tier >= 2 ? Color{1.0F, 0.55F, 0.20F, 0.55F}
                               : Color{1.0F, 0.95F, 0.40F, 0.50F});
        labels_.push_back({x, y,
                           tr->tier >= 2 ? Color{1.0F, 0.55F, 0.35F, 1.0F}
                                         : Color{1.0F, 0.9F, 0.5F, 1.0F},
                           tr->tier >= 2 ? "CHAMPION" : "ELITE"});
      }
    }
  }

  // Projectiles.
  {
    auto view = registry_.view<Transform, Sprite, Projectile, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& s = view.get<Sprite>(e);
      const auto& r = view.get<Radius>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      if (s.circle) b.circle(x, y, r.r, s.color);
      else b.rect(x, y, r.r * 2.0F, r.r * 2.0F, s.color);
    }
  }

  // Player (blinks while invulnerable, tinted green while poisoned).
  if (player_ != entt::null && registry_.valid(player_)) {
    const auto& t = registry_.get<Transform>(player_);
    const auto& r = registry_.get<Radius>(player_);
    const auto& s = registry_.get<Sprite>(player_);
    const float x = t.px + (t.x - t.px) * lerp;
    const float y = t.py + (t.y - t.py) * lerp;
    Color c = s.color;
    if (poison_ > 0.0F) {
      c = {c.r * 0.6F + 0.3F, c.g * 0.8F, c.b * 0.6F, 1.0F};
    }
    if (iframes_ > 0.0F && static_cast<int>(iframes_ * 20.0F) % 2 == 0) {
      c.a = 0.4F;
    }
    b.circle(x, y, r.r, c);
    if (poison_ <= 0.0F) {
      b.circle(x, y, r.r * 0.55F, Color{1.0F, 1.0F, 1.0F, 0.85F});
    }
  }

  // Particles.
  for (const auto& p : particles_) {
    if (p.life <= 0.0F) continue;
    Color c = p.color;
    c.a = p.life / p.maxLife;
    b.circle(p.x, p.y, p.size, c);
  }

  b.flush();

  // --- Screen pass ----------------------------------------------------------
  b.setScreenView();

  const Color white{1.0F, 1.0F, 1.0F, 1.0F};
  const Color gold{1.0F, 0.85F, 0.35F, 1.0F};

  // HP bar (top-left).
  const float hpFrac = playerMaxHp() > 0.0F ? playerHp() / playerMaxHp() : 0.0F;
  b.rectTopLeft(14.0F, 14.0F, 240.0F, 18.0F, Color{0.1F, 0.05F, 0.05F, 0.9F});
  b.rectTopLeft(14.0F, 14.0F, 240.0F * hpFrac, 18.0F, Color{0.85F, 0.2F, 0.25F, 1.0F});
  b.text(18.0F, 15.0F, 2.0F, white,
         "HP " + std::to_string(static_cast<int>(playerHp())) + "/" +
             std::to_string(static_cast<int>(playerMaxHp())));

  // Shield bar (under HP).
  if (stats_.shieldMax > 0.0F) {
    const float frac = stats_.shieldMax > 0.0F ? shield_ / stats_.shieldMax : 0.0F;
    b.rectTopLeft(14.0F, 34.0F, 240.0F, 7.0F, Color{0.05F, 0.1F, 0.2F, 0.9F});
    b.rectTopLeft(14.0F, 34.0F, 240.0F * frac, 7.0F, Color{0.35F, 0.65F, 1.0F, 1.0F});
  }

  // XP bar (top edge) + level.
  const float xpFrac = xpNext_ > 0.0F ? std::min(1.0F, xp_ / xpNext_) : 0.0F;
  b.rectTopLeft(0.0F, 0.0F, px, 8.0F, Color{0.08F, 0.08F, 0.12F, 1.0F});
  b.rectTopLeft(0.0F, 0.0F, px * xpFrac, 8.0F, gold);
  b.text(14.0F, 48.0F, 2.0F, gold, "LV " + std::to_string(level_));

  // Timer (top center).
  {
    const int total = static_cast<int>(simTime_);
    const std::string timer = (total / 60 < 10 ? "0" : "") + std::to_string(total / 60) + ":" +
                              (total % 60 < 10 ? "0" : "") + std::to_string(total % 60);
    b.text(px * 0.5F - b.textWidth(3.0F, timer) * 0.5F, 14.0F, 3.0F, white, timer);
  }

  // Kills (top-right).
  {
    const std::string kills = "KILLS " + std::to_string(kills_);
    b.text(px - b.textWidth(2.0F, kills) - 14.0F, 16.0F, 2.0F, white, kills);
  }

  // Elite / champion name tags.
  for (const auto& l : labels_) {
    const float sx = (l.x - camX_) * zoom_ + px * 0.5F;
    const float sy = py * 0.5F - (l.y - camY_) * zoom_;
    const float w = b.textWidth(1.6F, l.name);
    b.text(sx - w * 0.5F, sy - 20.0F, 1.6F, l.c, l.name);
  }
  labels_.clear();

  // Off-screen spawn warnings: a small dot at the screen edge marks where an
  // enemy is about to show up (the in-world ring only covers on-screen spots).
  for (const auto& p : pending_) {
    const float sx = (p.x - camX_) * zoom_ + px * 0.5F;
    const float sy = py * 0.5F - (p.y - camY_) * zoom_;
    if (sx >= -2.0F && sx <= px + 2.0F && sy >= -2.0F && sy <= py + 2.0F) continue;
    const float cxp = std::clamp(sx, 8.0F, px - 8.0F);
    const float cyp = std::clamp(sy, 8.0F, py - 8.0F);
    const auto& def = content_.enemies[static_cast<std::size_t>(p.def)];
    b.rectTopLeft(cxp - 3.0F, cyp - 3.0F, 6.0F, 6.0F, def.color);
  }

  // Level-up overlay.
  if (state_ == RunState::LevelUp) {
    b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.0F, 0.0F, 0.0F, 0.6F});

    const Color teal{0.4F, 0.9F, 0.9F, 1.0F};
    const Color violet{0.8F, 0.55F, 1.0F, 1.0F};

    const std::string title = milestoneOffer_ ? "MILESTONE! CHOOSE 1/2"
                              : choices_.size() > 3 ? "LEVEL UP! CHOOSE 1/2/3/4"
                                                     : "LEVEL UP! CHOOSE 1/2/3";
    b.text(px * 0.5F - b.textWidth(4.0F, title) * 0.5F, py * 0.16F, 4.0F,
           milestoneOffer_ ? violet : gold, title);

    const std::size_t n = choices_.size();
    if (n > 0) {
      const float gap = 24.0F;
      float cardW = std::min(340.0F, (px - gap * static_cast<float>(n - 1) - 40.0F) /
                                         static_cast<float>(n));
      cardW = std::max(200.0F, cardW);
      const float cardH = 176.0F;
      const float totalW = static_cast<float>(n) * cardW + static_cast<float>(n - 1) * gap;
      float x = px * 0.5F - totalW * 0.5F;
      for (std::size_t i = 0; i < n; ++i) {
        const auto& choice = choices_[i];
        const float y = py * 0.32F;
        b.rectTopLeft(x, y, cardW, cardH, Color{0.12F, 0.10F, 0.16F, 0.95F});

        Color accent = gold;
        if (milestoneOffer_) accent = violet;
        else if (choice.kind == Choice::Kind::Weapon) accent = teal;
        else if (content_.upgrades[static_cast<std::size_t>(choice.index)].kind == "unique") {
          accent = gold;
        }
        b.rectTopLeft(x, y, cardW, 4.0F, accent);

        const std::string key = "[" + std::to_string(i + 1) + "]";
        b.text(x + 16.0F, y + 16.0F, 3.0F, accent, key);

        if (choice.kind == Choice::Kind::Upgrade) {
          const auto& def = content_.upgrades[static_cast<std::size_t>(choice.index)];
          b.text(x + 16.0F, y + 52.0F, 2.3F, white, def.name);
          renderWrappedText(b, x + 16.0F, y + 80.0F, 1.7F,
                            Color{0.85F, 0.85F, 0.9F, 1.0F}, def.desc,
                            cardW - 32.0F, 10.0F);
          const std::string stacks =
              "STACKS " + std::to_string(upgradeStacks(static_cast<std::size_t>(choice.index))) +
              "/" + std::to_string(def.maxStacks);
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, Color{0.6F, 0.6F, 0.7F, 1.0F}, stacks);
        } else {
          const auto& def = content_.weapons[static_cast<std::size_t>(choice.index)];
          b.text(x + 16.0F, y + 52.0F, 2.3F, white, def.name);
          renderWrappedText(b, x + 16.0F, y + 80.0F, 1.7F,
                            Color{0.85F, 0.85F, 0.9F, 1.0F}, def.desc,
                            cardW - 32.0F, 10.0F);
          const std::string tag = def.prereqs.empty() ? "NEW WEAPON"
                                                       : "EVOLUTION! (A+B)";
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, teal, tag);
        }
        x += cardW + gap;
      }
    }

    // Reroll hint (only while key 4 is not needed to pick a 4th card).
    if (choices_.size() <= 3 && rerollsUsed_ < 1 + stats_.rerollCharges) {
      const std::string hint = "[4] REROLL (free once per level)";
      b.text(px * 0.5F - b.textWidth(2.0F, hint) * 0.5F, py * 0.66F, 2.0F,
             Color{0.7F, 0.7F, 0.8F, 1.0F}, hint);
    }
  }

  // Pause overlay.
  if (state_ == RunState::Paused) {
    b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.0F, 0.0F, 0.0F, 0.5F});
    const std::string pauseText = "PAUSED - ESC TO RESUME";
    b.text(px * 0.5F - b.textWidth(4.0F, pauseText) * 0.5F, py * 0.45F, 4.0F, white, pauseText);
  }

  // Game over overlay.
  if (state_ == RunState::GameOver) {
    b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.1F, 0.0F, 0.0F, 0.7F});
    const std::string dead = "YOU DIED";
    b.text(px * 0.5F - b.textWidth(6.0F, dead) * 0.5F, py * 0.30F, 6.0F,
           Color{0.9F, 0.2F, 0.2F, 1.0F}, dead);
    const std::string stats = "TIME " + std::to_string(static_cast<int>(simTime_)) + "S  KILLS " +
                              std::to_string(kills_) + "  LEVEL " + std::to_string(level_);
    b.text(px * 0.5F - b.textWidth(3.0F, stats) * 0.5F, py * 0.48F, 3.0F, white, stats);
    const std::string restart = "PRESS R TO RESTART";
    b.text(px * 0.5F - b.textWidth(3.0F, restart) * 0.5F, py * 0.60F, 3.0F, gold, restart);
  }

  b.flush();
}

} // namespace game