#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace game {
namespace {

constexpr float kPi = 3.14159265358979F;
constexpr std::size_t kMaxEnemies = 4000;
constexpr float kSpawnDist = 9.0F;
constexpr float kPickupDist = 0.45F;

float length(float x, float y) {
  return std::sqrt(x * x + y * y);
}

} // namespace

UpgradeEffectResult applyUpgrade(PlayerStats& stats, std::string_view effect, float value) {
  if (effect == "damage_mul") {
    stats.damageMul += value;
    return {true, 0.0F};
  }
  if (effect == "cooldown_mul") {
    stats.cooldownMul = std::max(0.25F, stats.cooldownMul + value);
    return {true, 0.0F};
  }
  if (effect == "speed_mul") {
    stats.speedMul += value;
    return {true, 0.0F};
  }
  if (effect == "pickup_mul") {
    stats.pickupMul += value;
    return {true, 0.0F};
  }
  if (effect == "max_hp_add") {
    stats.maxHp += value;
    return {true, value}; // also heals for the same amount
  }
  if (effect == "regen_add") {
    stats.regen += value;
    return {true, 0.0F};
  }
  if (effect == "proj_add") {
    stats.projAdd += static_cast<int>(value);
    return {true, 0.0F};
  }
  if (effect == "pierce_add") {
    stats.pierceAdd += static_cast<int>(value);
    return {true, 0.0F};
  }
  if (effect == "heal") {
    return {true, value};
  }
  return {false, 0.0F};
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
  spawnTimer_ = 0.0F;
  iframes_ = 0.0F;
  stats_ = PlayerStats{};
  std::fill(stacks_.begin(), stacks_.end(), 0);
  choices_.clear();
  particles_.clear();
  particleCursor_ = 0;

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

  const WeaponDef* def = content_.weapons.empty() ? nullptr : &content_.weapons.front();
  Weapon w{};
  if (def != nullptr) {
    w.cooldown = def->cooldown;
    w.damage = def->damage;
    w.projectiles = def->projectiles;
    w.speed = def->projSpeed;
    w.life = def->projLife;
    w.pierce = def->pierce;
  }
  registry_.emplace<Weapon>(player_, w);

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
    if (input.choose1) chooseUpgrade(0);
    else if (input.choose2) chooseUpgrade(1);
    else if (input.choose3) chooseUpgrade(2);
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
  updatePickups();

  // Regen.
  if (stats_.regen > 0.0F && player_ != entt::null) {
    auto& hp = registry_.get<Health>(player_);
    hp.hp = std::min(hp.max, hp.hp + stats_.regen / 60.0F);
  }

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
  const float speed = stats_.speed * stats_.speedMul;
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
    const auto& en = view.get<Enemy>(e);
    const auto& r = view.get<Radius>(e);

    t.px = t.x;
    t.py = t.y;

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

    v.x = dx * en.speed + sepX * 12.0F;
    v.y = dy * en.speed + sepY * 12.0F;
    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    // Contact damage to the player.
    const float hitDist = pr.r + r.r;
    const float px = pt.x - t.x;
    const float py = pt.y - t.y;
    if (px * px + py * py < hitDist * hitDist && iframes_ <= 0.0F) {
      php.hp -= en.touch;
      iframes_ = 0.55F;
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
  auto& weapon = registry_.get<Weapon>(player_);
  const auto& pt = registry_.get<Transform>(player_);

  weapon.timer -= 1.0F / 60.0F;
  if (weapon.timer > 0.0F) return;

  // Find the nearest enemy (linear scan: fine at current scale).
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
  if (!found) {
    weapon.timer = 0.05F;
    return;
  }

  weapon.timer = weapon.cooldown * stats_.cooldownMul;
  const float damage = weapon.damage * stats_.damageMul;
  const int count = std::max(1, weapon.projectiles + stats_.projAdd);
  const int pierce = weapon.pierce + stats_.pierceAdd;

  float baseAngle = std::atan2(targetY - pt.y, targetX - pt.x);
  const float spread = (count > 1) ? (0.16F) : 0.0F;
  for (int i = 0; i < count; ++i) {
    const float offset = (static_cast<float>(i) - static_cast<float>(count - 1) * 0.5F) * spread;
    const float angle = baseAngle + offset;
    const auto p = registry_.create();
    registry_.emplace<Transform>(p, pt.x, pt.y, pt.x, pt.y);
    registry_.emplace<Velocity>(p, std::cos(angle) * weapon.speed, std::sin(angle) * weapon.speed);
    registry_.emplace<Radius>(p, 0.14F);
    Sprite s{};
    s.color = {1.0F, 0.95F, 0.55F, 1.0F};
    s.circle = true;
    registry_.emplace<Sprite>(p, s);
    registry_.emplace<Projectile>(p, damage, pierce, weapon.life);
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
      if (!registry_.valid(enemy) || !registry_.all_of<Enemy, Health>(enemy)) return;
      auto& eh = registry_.get<Health>(enemy);
      if (eh.hp <= 0.0F) return; // already dead this tick
      const auto& et = registry_.get<Transform>(enemy);
      const auto& er = registry_.get<Radius>(enemy);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = r.r + er.r;
      if (dx * dx + dy * dy > hitR * hitR) return;

      eh.hp -= pr.damage;
      spawnParticles(et.x, et.y, {1.0F, 0.8F, 0.3F, 1.0F}, 3, 3.0F);
      if (eh.hp <= 0.0F) {
        // Death: XP orb + counter; actual destroy deferred.
        const float xpValue = registry_.all_of<Xp>(enemy) ? registry_.get<Xp>(enemy).value : 1.0F;
        const auto orb = registry_.create();
        registry_.emplace<Transform>(orb, et.x, et.y, et.x, et.y);
        registry_.emplace<Velocity>(orb);
        registry_.emplace<Radius>(orb, 0.16F);
        Sprite s{};
        s.color = {0.4F, 1.0F, 0.5F, 1.0F};
        s.circle = true;
        registry_.emplace<Sprite>(orb, s);
        registry_.emplace<Xp>(orb, xpValue);
        destroyQueue_.push_back(enemy);
        ++kills_;
        spawnParticles(et.x, et.y, {0.9F, 0.3F, 0.5F, 1.0F}, 8, 5.0F);
      }

      --pr.pierce;
      if (pr.pierce < 0) {
        spent = true;
        destroyQueue_.push_back(e);
      }
    });
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

void Game::spawnWave() {
  if (content_.enemies.empty()) return;
  if (registry_.view<Enemy>().size() >= kMaxEnemies) return;

  spawnTimer_ -= 1.0F / 60.0F;
  if (spawnTimer_ > 0.0F) return;

  // Difficulty ramp: interval shrinks over time.
  spawnTimer_ = std::max(0.10F, 0.9F - simTime_ * 0.008F);

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
  const EnemyDef* def = nullptr;
  for (const auto& candidate : content_.enemies) {
    if (simTime_ < candidate.unlockAt) continue;
    roll -= candidate.weight;
    if (roll <= 0.0F) {
      def = &candidate;
      break;
    }
  }
  if (def == nullptr) return;

  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const float angle = unit(rng_) * 2.0F * kPi;
  const float x = pt.x + std::cos(angle) * kSpawnDist;
  const float y = pt.y + std::sin(angle) * kSpawnDist;

  // Gradual HP scaling keeps late runs challenging.
  const float hpScale = 1.0F + simTime_ / 90.0F;

  const auto e = registry_.create();
  registry_.emplace<Transform>(e, x, y, x, y);
  registry_.emplace<Velocity>(e);
  registry_.emplace<Radius>(e, def->radius);
  Sprite s{};
  s.color = def->color;
  s.circle = def->circle;
  registry_.emplace<Sprite>(e, s);
  registry_.emplace<Health>(e, def->hp * hpScale, def->hp * hpScale);
  registry_.emplace<Enemy>(e, def->speed, def->touch);
  registry_.emplace<Xp>(e, def->xp);
}

void Game::enterLevelUp() {
  state_ = RunState::LevelUp;
  choices_.clear();

  std::vector<int> eligible;
  eligible.reserve(content_.upgrades.size());
  for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
    if (stacks_[i] < content_.upgrades[i].maxStacks) {
      eligible.push_back(static_cast<int>(i));
    }
  }
  std::shuffle(eligible.begin(), eligible.end(), rng_);
  const std::size_t take = std::min<std::size_t>(3, eligible.size());
  choices_.assign(eligible.begin(), eligible.begin() + static_cast<std::ptrdiff_t>(take));
}

void Game::chooseUpgrade(int slot) {
  if (slot < 0 || static_cast<std::size_t>(slot) >= choices_.size()) return;
  const int idx = choices_[static_cast<std::size_t>(slot)];
  const auto& def = content_.upgrades[static_cast<std::size_t>(idx)];

  const auto result = applyUpgrade(stats_, def.effect, def.value);
  if (!result.valid) return;
  ++stacks_[static_cast<std::size_t>(idx)];

  if (result.heal > 0.0F && player_ != entt::null && registry_.valid(player_)) {
    auto& hp = registry_.get<Health>(player_);
    hp.max = stats_.maxHp;
    hp.hp = std::min(hp.max, hp.hp + result.heal);
  } else if (player_ != entt::null && registry_.valid(player_)) {
    registry_.get<Health>(player_).max = stats_.maxHp;
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

void Game::grantXp(float amount) {
  xp_ += amount;
  if (state_ == RunState::Playing && xp_ >= xpNext_) {
    enterLevelUp();
  }
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

  const float ix = camX_; // interpolation of camera-independent entities:
  (void)ix;
  const float lerp = alpha;

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

  // Enemies (with tiny HP bar when damaged).
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
      if (h.hp < h.max) {
        const float frac = h.hp / h.max;
        const float w = r.r * 2.0F;
        b.rect(x, y + r.r + 0.15F, w, 0.08F, Color{0.1F, 0.1F, 0.1F, 0.8F});
        b.rect(x - w * 0.5F * (1.0F - frac), y + r.r + 0.15F, w * frac, 0.08F,
               Color{0.9F, 0.25F, 0.25F, 1.0F});
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

  // Player (blinks while invulnerable).
  if (player_ != entt::null && registry_.valid(player_)) {
    const auto& t = registry_.get<Transform>(player_);
    const auto& r = registry_.get<Radius>(player_);
    const auto& s = registry_.get<Sprite>(player_);
    const float x = t.px + (t.x - t.px) * lerp;
    const float y = t.py + (t.y - t.py) * lerp;
    Color c = s.color;
    if (iframes_ > 0.0F && static_cast<int>(iframes_ * 20.0F) % 2 == 0) {
      c.a = 0.4F;
    }
    b.circle(x, y, r.r, c);
    b.circle(x, y, r.r * 0.55F, Color{1.0F, 1.0F, 1.0F, 0.85F});
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

  // XP bar (top edge) + level.
  const float xpFrac = xpNext_ > 0.0F ? std::min(1.0F, xp_ / xpNext_) : 0.0F;
  b.rectTopLeft(0.0F, 0.0F, px, 8.0F, Color{0.08F, 0.08F, 0.12F, 1.0F});
  b.rectTopLeft(0.0F, 0.0F, px * xpFrac, 8.0F, gold);
  b.text(14.0F, 38.0F, 2.0F, gold, "LV " + std::to_string(level_));

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

  // Level-up overlay.
  if (state_ == RunState::LevelUp) {
    b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.0F, 0.0F, 0.0F, 0.6F});
    const std::string title = "LEVEL UP! CHOOSE 1/2/3";
    b.text(px * 0.5F - b.textWidth(4.0F, title) * 0.5F, py * 0.18F, 4.0F, gold, title);

    const float cardW = 320.0F;
    const float cardH = 170.0F;
    const float gap = 30.0F;
    const float totalW = static_cast<float>(choices_.size()) * cardW +
                         static_cast<float>(choices_.size() - 1) * gap;
    float x = px * 0.5F - totalW * 0.5F;
    for (std::size_t i = 0; i < choices_.size(); ++i) {
      const auto& def = content_.upgrades[static_cast<std::size_t>(choices_[i])];
      const float y = py * 0.34F;
      b.rectTopLeft(x, y, cardW, cardH, Color{0.12F, 0.10F, 0.16F, 0.95F});
      b.rectTopLeft(x, y, cardW, 4.0F, gold);
      const std::string key = "[" + std::to_string(i + 1) + "]";
      b.text(x + 16.0F, y + 16.0F, 3.0F, gold, key);
      b.text(x + 16.0F, y + 52.0F, 2.5F, white, def.name);
      b.text(x + 16.0F, y + 84.0F, 2.0F, Color{0.8F, 0.8F, 0.85F, 1.0F}, def.desc);
      const std::string stacks =
          "STACKS " + std::to_string(upgradeStacks(static_cast<std::size_t>(choices_[i]))) +
          "/" + std::to_string(def.maxStacks);
      b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, Color{0.6F, 0.6F, 0.7F, 1.0F}, stacks);
      x += cardW + gap;
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
