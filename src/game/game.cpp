#include "game/game.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace game {
namespace {

constexpr float kPi = 3.14159265358979F;
constexpr std::size_t kMaxEnemies = 8000;
// Visible rows in the test sandbox's item list.
constexpr int kTestShopRows = 14;
constexpr float kSpawnDist = 11.0F;
constexpr float kSpawnTelegraph = 0.6F; // seconds a spawn marker is visible
constexpr float kPickupDist = 0.45F;
constexpr float kShieldRegenRate = 10.0F;   // HP/s once out of combat
constexpr float kShieldRegenDelay = 4.0F;   // seconds without damage
constexpr float kContactIframes = 0.18F;    // enemies connect more often

// The orbit ring's interior sweep. Blades only touch the circle itself, so this
// is what makes the inside of the dagger's ring dangerous to stand in; it is a
// reduced share of one blade's contact damage (see updateOrbitBlades).
constexpr float kOrbitInnerMul = 0.35F;

// Enemies die once their HP drops to (or below) this tiny epsilon. Defense
// mitigation and float rounding can otherwise leave a sliver of HP, so a hit
// that should be lethal leaves a "0 HP" enemy alive.
constexpr float kEnemyDeathEpsilon = 1.0e-3F;

// Base HP multiplier ranges per enemy tier. Rolled per spawn so each elite is
// tougher than the last; higher tiers are exponentially beefier so they never
// simply melt. Applied on top of the global time-based hp scale.
constexpr float kEliteHpMin = 5.0F;
constexpr float kEliteHpMax = 10.0F;
constexpr float kChampionHpMin = 25.0F;
constexpr float kChampionHpMax = 100.0F;
constexpr float kOverlordHpMin = 125.0F;
constexpr float kOverlordHpMax = 1000.0F;

// Trait pool. Elites roll exactly one; champions and overlords roll several
// (see traitsForTier).
enum PickTrait : int {
  PickFast,
  PickArmored,
  PickRegenerating,
  PickExplosive,
  PickVenomous,
  PickVampiric,
  PickShielded,
  PickHeavy,
  PickArcher,
  PickAura,
  PickResistant,
  PickCount,
};

// Base stat multipliers applied to EVERY elite-and-above (before trait rolls),
// so higher tiers are always beefier regardless of which traits they roll.
// HP is a range rolled per spawn.
struct TierBuffs {
  float hpMin;
  float hpMax;
  float touch;
  float speed;
  float xp;
};

TierBuffs tierBuffs(int tier) {
  switch (tier) {
    case 3: return {kOverlordHpMin, kOverlordHpMax, 4.0F, 1.5F, 10.0F};
    case 2: return {kChampionHpMin, kChampionHpMax, 2.5F, 1.3F, 5.0F};
    case 1: return {kEliteHpMin, kEliteHpMax, 1.5F, 1.15F, 3.0F};
    default: return {1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
  }
}

// Roll a concrete HP multiplier for a tier from its range.
float rollTierHpMul(const TierBuffs& b, std::mt19937& rng) {
  if (b.hpMax <= b.hpMin) return b.hpMin;
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);
  return b.hpMin + unit(rng) * (b.hpMax - b.hpMin);
}

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
  if (effect == "fire_rate") {
    stats.fireRateBonus += value;
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
  if (effect == "armor_pierce_add") {
    stats.armorPierce += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "lifesteal_heal") {
    // Vampiric Heart: each lifesteal proc heals 2 instead of 1.
    stats.lifestealHeal = static_cast<int>(value);
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
  if (effect == "weapon_slot_add") {
    // Arsenal Core: one more weapon slot per stack (3 stacks max in content).
    stats.weaponSlots += static_cast<int>(value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "fan") {
    // Top-tier spread: double the volley angle and add +100% fire rate
    // (which halves the delay under the 1/(1+bonus) formula). The same item
    // costs accuracy: every shot now deviates up to +/- 30 degrees.
    stats.spreadMul += value * 2.0F;
    stats.fireRateBonus += value;
    stats.aimJitter += value * 0.5236F; // 30 degrees
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
  if (effect == "xp_mul") {
    stats.xpMul += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "knockback_retaliate") {
    // Repulsion Field: enemies that damage you are shoved away.
    stats.knockbackRetaliate += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "knockback_mul") {
    // Impact: every knockback the player deals is stronger.
    stats.knockbackMul += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "last_stand") {
    stats.lastStand = 1;
    return {true, 0.0F, 0.0F};
  }
  // --- Momentum cards --------------------------------------------------------
  // They never touch damageMul/fireRateBonus directly: the chain is a runtime
  // meter, so a card only changes how fast it fills and how much each stack is
  // worth (see Game::updateMomentum).
  if (effect == "momentum_damage") {
    stats.momentumDamage += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "momentum_speed") {
    stats.momentumSpeed += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "momentum_window") {
    stats.momentumWindow += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "momentum_bloodthirst") {
    // Bloodthirst: twice the stacks per kill, twice the useful chain length
    // and a chain that forgives twice as long without a kill.
    stats.momentumGain += value;
    stats.momentumMax += static_cast<int>(value * 20.0F);
    stats.momentumWindow += value * 3.0F;
    return {true, 0.0F, 0.0F};
  }
  // --- Ability cards ---------------------------------------------------------
  // The three buttons exist in every run; these only decide how much they are
  // worth. Cooldown reduction is floored so no build can hold them all.
  if (effect == "ability_haste") {
    stats.abilityCdMul = std::max(0.35F, stats.abilityCdMul - value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ability_might") {
    stats.burstRadius += 0.8F;
    stats.burstDamage += 30.0F;
    stats.burstKnockback += 3.0F;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ability_phase") {
    stats.blinkDist += 1.2F;
    stats.blinkIframes += 0.2F;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ability_stasis") {
    stats.stasisDuration += 1.0F;
    stats.stasisSlow = std::max(0.15F, stats.stasisSlow - 0.08F);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ability_echo") {
    // Echo Chamber: every ability also throws a weakened Overload.
    stats.abilityEcho = 1;
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
  // The pacing lever of the whole game. Level-ups are picks, and picks are the
  // only thing that makes a run interesting, so the curve is deliberately
  // steep: with the old numbers a build was maxed out in ~4 minutes and the
  // remaining 20 were an empty walk. At this rate the core damage/fire-rate
  // cards are still worth taking at minute 10, and the big milestones (16, 32,
  // 64) land at 3 / 8 / 20+ minutes instead of 1 / 2 / 4.
  const float l = static_cast<float>(level - 1);
  return 12.0F + 9.0F * l + 2.6F * l * (l + 1.0F);
}

float aoeFalloff(int enemiesHit, int pierce) {
  const int n = std::clamp(enemiesHit, 1, 40);
  const float base = std::pow(0.9F, static_cast<float>(n - 1));
  // Pierce eats into the falloff: at 20 pierce a blast deals full damage to
  // every target it catches.
  const float t = std::clamp(static_cast<float>(pierce) / 20.0F, 0.0F, 1.0F);
  return base + (1.0F - base) * t;
}

float enemyDefense(float simTime, int tier) {
  // A short grace period keeps the opening minute free of mitigation, then
  // defense ramps so late waves shrug off a slice of every hit.
  const float base = std::max(0.0F, simTime - 30.0F) / 25.0F;
  const float tierMul = tier >= 3 ? 2.8F : tier == 2 ? 2.0F : tier == 1 ? 1.4F : 1.0F;
  return base * tierMul;
}

float enemyLifestealResistance(float simTime, int tier, bool resistant) {
  float res = std::min(0.75F, simTime / 1200.0F);
  res += tier >= 3 ? 0.35F : tier == 2 ? 0.25F : tier == 1 ? 0.15F : 0.0F;
  if (resistant) res += 0.5F;
  return std::min(1.0F, res);
}

float enemyKnockbackResistance(float simTime, int tier, bool resistant) {
  // Reaches 70% resistance for an ordinary enemy at 8:45, then tier and trait
  // bonuses push elites/champions/overlords towards the cap. 750s instead of
  // the old 900s makes the ramp visible inside a normal run.
  float res = std::min(0.7F, simTime / 750.0F);
  res += tier >= 3 ? 0.35F : tier == 2 ? 0.25F : tier == 1 ? 0.15F : 0.0F;
  if (resistant) res += 0.5F;
  return std::min(1.0F, res);
}

int traitsForTier(int tier, float simTime) {
  if (tier <= 1) return tier == 1 ? 1 : 0; // elite: exactly one bonus
  // Champions: 2 base, +1 after 4 minutes. Overlords: 4 base, +1 after 8.
  if (tier == 2) return 2 + (simTime >= 240.0F ? 1 : 0);
  return 4 + (simTime >= 480.0F ? 1 : 0);
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
  healCd_ = 0.0F;
  stats_ = PlayerStats{};
  std::fill(stacks_.begin(), stacks_.end(), 0);
  choices_.clear();
  pending_.clear();
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
  lastStandCd_ = 0.0F;

  // Bestiary progress resets with the run.
  bestiaryKills_.assign(content_.enemies.size(), 0);
  bestiaryTiers_.assign(content_.enemies.size(), 0);

  // Overlord retirements are per-run: a fresh run re-opens every type.
  retiredTypes_.assign(content_.enemies.size(), 0);
  strongestKilledTier_ = 0;
  strongestKilledDef_ = -1;
  tierKillMask_ = 0;
  // The adaptive tribunal director is per-run too: a fresh run starts with no
  // handling score, so the heavy tiers are shut again.
  for (int t = 0; t < 4; ++t) {
    tierPressure_[t] = 0.0F;
    tierGrace_[t] = 0.0F;
    tierOpen_[t] = t == 1;
  }
  tierBanner_.clear();
  tierBannerT_ = 0.0F;
  // The kill chain is per-run: every restart starts from zero.
  streak_ = 0;
  streakTimer_ = 0.0F;
  momentumDamageMul_ = 1.0F;
  momentumRate_ = 0.0F;
  momentumSpeedMul_ = 1.0F;
  // Seed the "3 most recent" exemption set. As more types unlock this is
  // recomputed in refreshRecentTypes().
  recentTypes_.assign(content_.enemies.size(), 0);
  refreshRecentTypes();

  // The opening 3-weapon pick replaces the old random starter weapon.
  starterChoicePending_ = true;
  choosingStarter_ = false;
  bestiaryOpen_ = false;

  // Profile unlocks are EARNED ACROSS RUNS, so a reset does not clear them —
  // but the run-local "already pushed" set does, so a new run re-reports any
  // tier earned in the previous one (harmless: unlockTier is idempotent).
  syncedUnlocks_ = 0;
  syncProfileUnlocks();

  // Weapon test mode is a session-level sandbox; a fresh run starts clean.
  testMode_ = false;
  testBoosted_ = false;
  testWeaponIdx_ = 0;
  savedWeaponCount_ = 0;
  savedStats_ = PlayerStats{};

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

  // No starter weapon is granted; advance() opens the three-weapon pick.
  weaponCount_ = 0;

  // Paint the freshly created player with the profile's skin/outline.
  applyProfileToPlayer();

  camX_ = 0.0F;
  camY_ = 0.0F;
  timestep_.reset();
}

void Game::syncProfileUnlocks() {
  if (profile_ == nullptr) return;
  // The sandbox NEVER grants cosmetics. Killing a champion there must not hand
  // out an outline that took a real run to earn — the whole point of the
  // sandbox is that nothing inside it counts, and the tier-kill mask is rolled
  // back on exit anyway, so simply not writing is enough.
  if (testMode_) return;
  // tierKillMask_ bit t is set once a tier-t enemy has died. Map that to the
  // profile's tier-1-based unlock bits and push only the new ones.
  for (int tier = 1; tier <= 3; ++tier) {
    const UnlockMask bit = static_cast<UnlockMask>(1u << (tier - 1));
    if ((tierKillMask_ & (1u << tier)) == 0u) continue;
    if ((syncedUnlocks_ & bit) != 0u) continue;
    if (profile_->unlockTier(tier)) profileDirty_ = true;
    syncedUnlocks_ = static_cast<UnlockMask>(syncedUnlocks_ | bit);
  }
}

bool Game::consumeProfileDirty() {
  const bool dirty = profileDirty_;
  profileDirty_ = false;
  return dirty;
}

core::render::Color Game::testPlayerColor() const {
  if (player_ == entt::null || !registry_.valid(player_)) return core::render::Color{};
  return registry_.get<Sprite>(player_).color;
}

void Game::applyProfileToPlayer() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& sprite = registry_.get<Sprite>(player_);
  if (profile_ != nullptr) {
    const auto& skins = skinPalette();
    const int skin = std::clamp(profile_->skin, 0, static_cast<int>(skins.size()) - 1);
    sprite.color = skins[static_cast<std::size_t>(skin)].color;
    // Resolve the outline color once here so the render path never has to
    // reach back into the profile.
    if (profile_->canUseOutline(profile_->outline) &&
        profile_->outline < static_cast<int>(outlinePalette().size())) {
      outlineColor_ = outlinePalette()[static_cast<std::size_t>(profile_->outline)].color;
    } else {
      outlineColor_ = core::render::Color{0.0F, 0.0F, 0.0F, 0.0F};
    }
  } else {
    sprite.color = core::render::Color{0.55F, 0.85F, 1.0F, 1.0F};
    outlineColor_ = core::render::Color{0.0F, 0.0F, 0.0F, 0.0F};
  }
}

void Game::updateMainMenu(const FrameInput& input) {
  constexpr int kRows = 4; // START, SKIN, OUTLINE, QUIT
  if (input.menuUp) {
    menuSelection_ = (menuSelection_ + kRows - 1) % kRows;
  } else if (input.menuDown) {
    menuSelection_ = (menuSelection_ + 1) % kRows;
  }
  if (profile_ == nullptr) {
    // Without a profile the cosmetic rows are inert, but START and QUIT must
    // still work (tests and headless callers may not attach one).
    if (input.menuConfirm && menuSelection_ == 0) menuOpen_ = false;
    if (input.menuConfirm && menuSelection_ == 3) quitRequested_ = true;
    return;
  }

  const int skins = static_cast<int>(skinPalette().size());
  const int outlines = static_cast<int>(outlinePalette().size());
  if (input.menuLeft || input.menuRight) {
    const int dir = input.menuRight ? 1 : -1;
    if (menuSelection_ == 1) { // SKIN
      profile_->skin = ((profile_->skin + dir) % skins + skins) % skins;
      profileDirty_ = true;
      applyProfileToPlayer();
    } else if (menuSelection_ == 2) { // OUTLINE
      // Cycle but skip locked styles so the player never lands on one they
      // cannot wear: step until an unlocked index is found (palette is tiny,
      // and "None" is index 0 so the loop always terminates).
      for (int step = 0; step < outlines; ++step) {
        profile_->outline = ((profile_->outline + dir) % outlines + outlines) % outlines;
        if (profile_->canUseOutline(profile_->outline)) break;
      }
      profileDirty_ = true;
      applyProfileToPlayer();
    }
  }
  if (!input.menuConfirm) return;
  switch (menuSelection_) {
    case 0: // START
      menuOpen_ = false;
      break;
    case 1: // SKIN — advance to the next colour directly on confirm too
      profile_->skin = (profile_->skin + 1) % skins;
      profileDirty_ = true;
      applyProfileToPlayer();
      break;
    case 2: // OUTLINE — same convenience
      for (int step = 0; step < outlines; ++step) {
        profile_->outline = (profile_->outline + 1) % outlines;
        if (profile_->canUseOutline(profile_->outline)) break;
      }
      profileDirty_ = true;
      applyProfileToPlayer();
      break;
    case 3: // QUIT
      quitRequested_ = true;
      break;
    default:
      break;
  }
}

void Game::advance(float frameDt, const FrameInput& input) {
  // The main menu is fully modal: it eats the whole frame and the simulation
  // does not advance behind it.
  if (menuOpen_) {
    updateMainMenu(input);
    timestep_.reset();
    return;
  }
  // Opening pick: the run starts weaponless and immediately offers three
  // starter weapons (unless a weapon is already equipped, e.g. in tests).
  if (state_ == RunState::Playing && starterChoicePending_ && weaponCount_ == 0) {
    enterStarterPick();
  }
  if (input.togglePause) {
    if (state_ == RunState::Playing) {
      state_ = RunState::Paused;
    } else if (state_ == RunState::Paused) {
      state_ = RunState::Playing;
      bestiaryOpen_ = false;
    }
  }
  // ESC from game over returns to the main menu (the menu is where a fresh
  // player starts, and R still restarts in place).
  if (state_ == RunState::GameOver && input.togglePause) {
    reset();
    menuOpen_ = true;
    return;
  }
  // While paused, B opens/closes the bestiary.
  if (state_ == RunState::Paused && input.bestiary) {
    bestiaryOpen_ = !bestiaryOpen_;
  }
  if (state_ == RunState::GameOver && input.restart) {
    reset();
    return;
  }
  // Weapon test mode: T opens/closes it, then the sandbox keys take over.
  if ((state_ == RunState::Playing || state_ == RunState::LevelUp) &&
      input.testModeToggle) {
    if (testMode_) {
      exitTestMode();
    } else if (state_ == RunState::Playing) {
      enterTestMode();
    }
  }
  if (testMode_) {
    // Sandbox switches work even while a level-up card screen is open.
    if (input.testShop) toggleTestShop();
    if (input.testInvuln) toggleTestInvuln();
    if (input.testTime) cycleTestTimeScale();
    if (input.testKill && state_ == RunState::Playing) testKillPlayer();
    if (input.restart) testMaxAllItems();
    if (testShopOpen_) {
      updateTestShop(input);
    } else if (state_ == RunState::Playing) {
      if (input.choose1) setTestWeapon(testWeaponIdx_ - 1);
      else if (input.choose2) setTestWeapon(testWeaponIdx_ + 1);
      else if (input.choose3) toggleTestBoost();
      else if (input.choose4) wavesEnabled_ = !wavesEnabled_;
      else if (input.choose5) exitTestMode();
    }
  }
  // J / K / L: the three active abilities. They are always live (no unlock, no
  // card) and only a cooldown stands in the way, so this is checked before the
  // level-up branch: spending a cooldown is legal at any time.
  if (state_ == RunState::Playing) {
    updateAbilities(input);
  }
  // H: guaranteed heal for 50% of max HP on a cooldown (replaces the old
  // single-use heal cards, which are no longer in the loot pool).
  if (state_ == RunState::Playing && input.heal && healCd_ <= 0.0F &&
      player_ != entt::null && registry_.valid(player_)) {
    auto& php = registry_.get<Health>(player_);
    if (php.hp > 0.0F && php.hp < php.max) {
      php.hp = std::min(php.max, php.hp + php.max * 0.5F);
      healCd_ = 30.0F;
      const auto& t = registry_.get<Transform>(player_);
      spawnParticles(t.x, t.y, {0.4F, 1.0F, 0.6F, 1.0F}, 18, 5.0F);
    }
  }
  if (state_ == RunState::LevelUp) {
    const std::size_t n = choices_.size();
    if (input.choose1 && n >= 1) chooseUpgrade(0);
    else if (input.choose2 && n >= 2) chooseUpgrade(1);
    else if (input.choose3 && n >= 3) chooseUpgrade(2);
    else if (input.choose4 && n >= 4) chooseUpgrade(3);
    else if (input.choose5 && n >= 5) chooseUpgrade(4);
    // R is *always* the reroll key on level-up (even with 4+ cards) — except
    // inside the sandbox, where it is the "max every item" cheat.
    if (input.restart && !testMode_) {
      reroll();
    }
  }

  moveX_ = input.moveX;
  moveY_ = input.moveY;
  if (testMode_ && testShopOpen_) {
    // The item list is driven by the same keys as movement, so stand still
    // while it is open instead of walking off with the cursor.
    moveX_ = 0.0F;
    moveY_ = 0.0F;
  }
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
  // The test sandbox can fast-forward the difficulty clock, which means running
  // the whole simulation step several times per rendered frame. Outside the
  // sandbox this is always exactly one step, so gameplay is untouched.
  if (!testMode_ || testTimeScale_ <= 1) {
    fixedStep();
    return;
  }
  for (int i = 0; i < testTimeScale_; ++i) fixedStep();
}

void Game::fixedStep() {
  simTime_ += 1.0F / 60.0F;
  // Recompute the chain multipliers first: a kill from the previous step has to
  // be able to make this step's shots stronger.
  updateMomentum();
  if (iframes_ > 0.0F) {
    iframes_ -= 1.0F / 60.0F;
  }
  if (healCd_ > 0.0F) {
    healCd_ -= 1.0F / 60.0F;
  }
  if (lastStandCd_ > 0.0F) {
    lastStandCd_ -= 1.0F / 60.0F;
  }
  // Ability cooldowns and the Stasis window tick with the simulation, and the
  // world-time multiplier is derived from them once per step so every system
  // that reads it this frame sees the same value.
  for (int i = 0; i < kAbilityCount; ++i) {
    if (abilityCd_[i] > 0.0F) abilityCd_[i] = std::max(0.0F, abilityCd_[i] - 1.0F / 60.0F);
  }
  if (stasis_ > 0.0F) {
    stasis_ = std::max(0.0F, stasis_ - 1.0F / 60.0F);
    worldTimeScale_ = std::max(0.1F, stats_.stasisSlow);
  } else {
    worldTimeScale_ = 1.0F;
  }

  movePlayer();
  buildSpatialHash();
  updateEnemies();
  fireWeapons();
  updateProjectiles();
  updateOrbitBlades();
  updateHaloBeams();
  updateVortices();
  updateBombProjectiles();
  updateBoomerangProjectiles();
  updateBounceProjectiles();
  updateBeamEffects();
  updateSweepEffects();
  updateZoneEffects();
  updateLures();
  updateChainLightning();
  updateNovaRing();
  updateEnemyShots();
  updatePickups();

  // Regen.
  if (stats_.regen > 0.0F && player_ != entt::null && registry_.valid(player_)) {
    auto& hp = registry_.get<Health>(player_);
    hp.hp = std::min(hp.max, hp.hp + stats_.regen / 60.0F);
  }

  updateShield();
  updateUniqueEffects();

  processPendingSpawns();
  updateTierDirector();
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

  // Level-up check. The sandbox can never reach it: grantXp() refuses to pay
  // out inside the sandbox, so the card screen is not something a tester can
  // farm picks from. A sandbox that was opened mid-level-up still unwinds.
  if (state_ == RunState::Playing && !testMode_ && xp_ >= xpNext_) {
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
  float speed = stats_.speed * stats_.speedMul * momentumSpeedMul_;

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
  // Stasis slows the world, not the player: every enemy timer, every step of
  // movement and every aura tick inside this function run on the slowed clock,
  // so one multiplier here covers the whole hostile side of the game.
  const float dt = (1.0F / 60.0F) * worldTimeScale_;

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
        eh.hp = std::min(eh.max, eh.hp + traits->regen * dt);
      }
      // Damage aura: continuous chip damage while the player stands inside.
      if (traits->auraRadius > 0.0F) {
        const float adx = pt.x - t.x;
        const float ady = pt.y - t.y;
        if (adx * adx + ady * ady < traits->auraRadius * traits->auraRadius) {
          damagePlayerDirect(traits->auraDps * dt);
        }
      }
      // Archer: periodic projectiles aimed at the player.
      if (traits->shootCooldown > 0.0F) {
        traits->shootTimer -= dt;
        if (traits->shootTimer <= 0.0F) {
          const float sdx = pt.x - t.x;
          const float sdy = pt.y - t.y;
          const float sd = length(sdx, sdy);
          if (sd < 9.0F && sd > 0.001F) {
            traits->shootTimer = traits->shootCooldown;
            const auto shot = registry_.create();
            constexpr float kShotSpeed = 7.5F;
            registry_.emplace<Transform>(shot, t.x, t.y, t.x, t.y);
            registry_.emplace<Velocity>(shot, sdx / sd * kShotSpeed, sdy / sd * kShotSpeed);
            registry_.emplace<Radius>(shot, 0.13F);
            Sprite ss{};
            ss.color = {1.0F, 0.5F, 0.2F, 1.0F};
            ss.circle = true;
            registry_.emplace<Sprite>(shot, ss);
            EnemyShot es{};
            es.damage = en.touch * 0.6F;
            es.life = 3.0F;
            registry_.emplace<EnemyShot>(shot, es);
          } else {
            traits->shootTimer = 0.25F; // out of range: retry shortly
          }
        }
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

    // Ranged enemies kite instead of charging: they hold a preferred stand-off
    // band, back away when the player closes in, close the gap when the player
    // runs away, and strafe sideways while inside the band. Without this an
    // archer simply walked into melee range, which wasted the whole point of
    // the Archer trait (and made ranged enemies strictly worse melee).
    if (traits != nullptr && traits->shootCooldown > 0.0F) {
      // The stand-off band is defined by its two edges: back off below the
      // inner one, hold position and strafe between them, and only close the
      // gap above the outer one.
      constexpr float kTooClose = 3.2F; // start backing off below this
      constexpr float kTooFar = 7.0F;   // close in above this
      if (dist > 0.001F) {
        float seekX = dx;
        float seekY = dy;
        if (dist < kTooClose) {
          // Too close: retreat directly away from the player.
          seekX = -dx;
          seekY = -dy;
        } else if (dist <= kTooFar && dist >= kTooClose) {
          // In the band: stop closing and strafe perpendicular so the player
          // has to keep moving to line up a shot.
          // Strafe direction flips slowly to avoid a perfectly static orbit.
          const float side = std::sin(simTime_ * 0.7F + t.x * 0.3F) >= 0.0F ? 1.0F : -1.0F;
          seekX = -dy * side;
          seekY = dx * side;
        }
        // Replace the seek vector with the kiting one.
        dx = seekX;
        dy = seekY;
        const float sl = length(dx, dy);
        if (sl > 0.001F) {
          dx /= sl;
          dy /= sl;
        }
      }
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
      // Enemies get extra clearance from the player character so they hover
      // just outside it instead of piling flush on top of the player.
      const float minDist = (r.r + orr.r) * 0.9F + (other == player_ ? 0.35F : 0.0F);
      if (d2 < minDist * minDist && d2 > 0.0001F) {
        const float d = std::sqrt(d2);
        sepX += (ox / d) * (minDist - d);
        sepY += (oy / d) * (minDist - d);
      }
    });

    // Slowed enemies (ice blood) move at 45%.
    float effSpeed = en.speed;
    if (en.slowT > 0.0F) {
      en.slowT -= dt;
      effSpeed *= 0.45F;
    }

    v.x = dx * effSpeed + sepX * 12.0F;
    v.y = dy * effSpeed + sepY * 12.0F;

    // Cap total speed: separation between tightly-packed enemies (the player
    // itself is a hash neighbour too) can otherwise fling an enemy at many
    // times its base speed — the "enemy suddenly accelerates into the player"
    // behaviour. Base speed is ~2, so 2.5x still allows fast trait bursts.
    const float maxSpeed = std::max(en.speed * 2.5F, 4.0F);
    const float sp2 = v.x * v.x + v.y * v.y;
    if (sp2 > maxSpeed * maxSpeed) {
      const float inv = maxSpeed / std::sqrt(sp2);
      v.x *= inv;
      v.y *= inv;
    }
    // Knockback shove: added on top of the clamped AI velocity, then decays
    // over a handful of ticks so hits genuinely push enemies around.
    v.x += en.kbX;
    v.y += en.kbY;
    constexpr float kKbDecay = 0.82F;
    en.kbX *= kKbDecay;
    en.kbY *= kKbDecay;
    if (std::abs(en.kbX) < 0.01F) en.kbX = 0.0F;
    if (std::abs(en.kbY) < 0.01F) en.kbY = 0.0F;
    t.x += v.x * dt;
    t.y += v.y * dt;

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
      retaliateKnockback(e);
      iframes_ = iframeDuration(kContactIframes);
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
  entt::entity bestEnemy = entt::null;
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
      bestEnemy = e;
      found = true;
    }
  }

  if (!found) {
    // No enemies: weapons idle-rev. Timers keep ticking so the first enemy
    // does not get dumped on with a full volley of instant shots.
    const float dt = 1.0F / 60.0F;
    for (int i = 0; i < weaponCount_; ++i) {
      auto& w = weapons_[i];
      w.timer -= dt;
      if (w.timer <= 0.0F) {
        w.timer = attackCooldown(w.cooldown, stats_.fireRateBonus + w.cdBonus + momentumRate_);
      }
    }
    return;
  }

  const float baseAngle = std::atan2(targetY - pt.y, targetX - pt.x);
  // Spreadshot's inaccuracy: each projectile deviates by up to +/- aimJitter.
  std::uniform_real_distribution<float> jitterUnit(-1.0F, 1.0F);

  for (int i = 0; i < weaponCount_; ++i) {
    auto& w = weapons_[i];
    w.timer -= 1.0F / 60.0F;
    if (w.timer > 0.0F) continue;

    const float cooldown = attackCooldown(w.cooldown, stats_.fireRateBonus + w.cdBonus + momentumRate_);
    const float damage = w.damage * stats_.damageMul * momentumDamageMul_;
    const int count = std::max(1, w.projectiles + stats_.projAdd);
    const int pierce = w.pierce + stats_.pierceAdd;

    switch (w.attackType) {
      case AttackType::Projectile: {
        const float spread = (count > 1) ? w.spread * stats_.spreadMul : 0.0F;
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * spread;
          const float jitter = stats_.aimJitter > 0.0F ? jitterUnit(rng_) * stats_.aimJitter : 0.0F;
          const float angle = baseAngle + offset + jitter;
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
          // More projectiles = a wider cone sweep.
          const float coneRange = w.coneRange * (1.0F + stats_.projAdd * 0.25F);
          const float halfAngle = coneAngle * 0.5F;
          auto view = registry_.view<Transform, Health, Radius, Enemy>();
          std::vector<entt::entity> hits;
          std::vector<float> hx;
          std::vector<float> hy;
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
              hits.push_back(e);
              hx.push_back(et.x);
              hy.push_back(et.y);
            }
          }
          // AoE falloff: a cone that catches a crowd deals less to each target.
          const float coneMul = aoeFalloff(static_cast<int>(hits.size()), pierce);
          for (std::size_t hi = 0; hi < hits.size(); ++hi) {
            applyEnemyDamage(hits[hi], damage * coneMul);
            spawnParticles(hx[hi], hy[hi], {1.0F, 0.5F, 0.1F, 1.0F}, 4, 3.0F);
          }
          // Visible flame fan along the aim direction so the cone reads on
          // screen (reuses the fading-wedge renderer).
          const auto se = registry_.create();
          registry_.emplace<Transform>(se, pt.x, pt.y, pt.x, pt.y);
          registry_.emplace<Radius>(se, coneRange);
          SweepEffect sw{};
          sw.radius = coneRange;
          sw.angle = coneAngle;
          sw.duration = 0.22F;
          sw.timer = 0.22F;
          sw.startAngle = baseAngle - halfAngle;
          sw.endAngle = baseAngle + halfAngle;
          sw.color = {1.0F, 0.55F, 0.10F, 1.0F};
          registry_.emplace<SweepEffect>(se, sw);
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
          // More projectiles = a taller arc; more pierce = a bigger blast.
          const float arcHeight = w.bombArcHeight * (1.0F + stats_.projAdd * 0.25F);
          const float vy = std::sqrt(2.0F * gravity * arcHeight);
          registry_.emplace<Velocity>(bomb, vx, vy);
          registry_.emplace<Radius>(bomb, 0.2F);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(bomb, s);
          BombProjectile bp{};
          bp.damage = damage;
          bp.pierce = pierce;
          // Pierce no longer extends bomb life: it scales the explosion
          // radius (+15% per point) so the weapon stays "one boom, gone".
          bp.explodeRadius = w.bombExplodeRadius * (1.0F + stats_.pierceAdd * 0.15F);
          bp.knockback = w.bombKnockback;
          bp.life = w.life;
          bp.initialLife = w.life;
          bp.arcHeight = arcHeight;
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
          bp.blastRadius = w.area;
          bp.color = w.color;
          registry_.emplace<BoomerangProjectile>(boom, bp);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Bounce: {
        // The Void Orb is ONE eternal projectile: projectile count does NOT
        // spawn more orbs — it GROWS the single orb instead. Once launched it
        // hunts forever (no life budget, no bounce budget); each cooldown tick
        // only re-syncs the orb's size with the current projectile stat.
        const float orbRadius =
            std::min(1.2F, 0.18F * (1.0F + static_cast<float>(count - 1) * 0.35F));
        if (w.liveBounce != entt::null && registry_.valid(w.liveBounce)) {
          if (auto* rr = registry_.try_get<Radius>(w.liveBounce); rr != nullptr) {
            rr->r = orbRadius;
          }
          w.timer = cooldown;
          break;
        }
        const auto proj = registry_.create();
        registry_.emplace<Transform>(proj, pt.x, pt.y, pt.x, pt.y);
        registry_.emplace<Velocity>(proj, std::cos(baseAngle) * w.speed,
                                    std::sin(baseAngle) * w.speed);
        registry_.emplace<Radius>(proj, orbRadius);
        Sprite s{};
        s.color = w.color;
        s.circle = true;
        registry_.emplace<Sprite>(proj, s);
        BounceProjectile bp{};
        bp.damage = damage;
        bp.pierce = pierce;
        bp.life = w.life;
        bp.maxBounces = w.bounceCount + stats_.pierceAdd;
        bp.bounceRange = w.bounceRange;
        bp.damageMul = w.bounceDamageMul;
        bp.bounceCount = 0;
        bp.splashRadius = w.area;
        bp.infinite = w.bounceInfinite;
        bp.color = w.color;
        registry_.emplace<BounceProjectile>(proj, bp);
        w.liveBounce = proj;
        w.timer = cooldown;
        break;
      }
      case AttackType::Beam: {
        if (found) {
          // The fan is the whole point of the beam, so it is laid out as a
          // readable wedge instead of a stack: the "Prism Lance" unique
          // (`beamSplit >= 3`) is exactly THREE beams aimed forward, left and
          // right, and projectile cards no longer multiply them (they used to
          // read as "+2 projectiles" because all of them overlapped on the
          // aim line with a 0.05 rad gap). Without the unique, extra
          // projectiles widen the same symmetric fan instead of stacking.
          const bool triad = w.beamSplit >= 3;
          const int beamCount = triad ? 3 : std::clamp(count, 1, 4);
          const float beamWidth = w.beamWidth * (1.0F + stats_.projAdd * 0.05F);
          constexpr float kTriadGap = 0.30F;  // ~17 degrees between the three
          constexpr float kFanGap = 0.18F;    // ~10 degrees per extra beam
          const float gap = triad ? kTriadGap : kFanGap;
          for (int p = 0; p < beamCount; ++p) {
            const float offset =
                (static_cast<float>(p) - static_cast<float>(beamCount - 1) * 0.5F) * gap;
            const float bAngle = baseAngle + offset;
            const float endX = pt.x + std::cos(bAngle) * w.beamRange;
            const float endY = pt.y + std::sin(bAngle) * w.beamRange;
            const auto beam = registry_.create();
            registry_.emplace<Transform>(beam, pt.x, pt.y, pt.x, pt.y);
            registry_.emplace<Radius>(beam, beamWidth * 0.5F);
            Sprite s{};
            s.color = w.color;
            s.circle = false;
            registry_.emplace<Sprite>(beam, s);
            BeamEffect be{};
            be.damage = w.damage; // base; updateBeamEffects scales by damageMul
            be.pierce = pierce;
            be.range = w.beamRange;
            be.width = beamWidth;
            be.duration = w.beamDuration;
            be.timer = w.beamDuration;
            be.startX = pt.x;
            be.startY = pt.y;
            be.endX = endX;
            be.endY = endY;
            be.color = w.color;
            registry_.emplace<BeamEffect>(beam, be);
          }
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Sweep: {
        // The scythe REAPS a circle around its prey: the swing is centered ON
        // the nearest enemy, carving up everything around that target.
        // A weapon with `sweep_lead` is a WHIP instead: the arc is centered on
        // the point in FRONT of the player and only covers `sweep_angle` of it,
        // so it lashes where the player is looking rather than where the prey
        // happens to stand — and it still swings with nothing to hit. This is
        // the mechanical difference between the two melee weapons, and it is
        // what lets the whip (and its evolution) cover the player's own front
        // while the scythe covers the far side of a crowd.
        const bool lead = w.sweepLead > 0.0F;
        const float sweepRadius = w.sweepRadius * (1.0F + stats_.projAdd * 0.15F);
        const float sx = lead ? pt.x + std::cos(baseAngle) * w.sweepLead : targetX;
        const float sy = lead ? pt.y + std::sin(baseAngle) * w.sweepLead : targetY;
        const float halfArc = lead ? std::min(kPi, w.sweepAngle * 0.5F) : kPi;
        auto view = registry_.view<Transform, Health, Radius, Enemy>();
        std::vector<entt::entity> hits;
        std::vector<float> hx;
        std::vector<float> hy;
        for (const auto e : view) {
          const auto& et = view.get<Transform>(e);
          const float dx = et.x - sx;
          const float dy = et.y - sy;
          const float dist2 = dx * dx + dy * dy;
          if (dist2 > sweepRadius * sweepRadius) continue;
          if (lead && dist2 > 1e-4F) {
            // Keep only the part of the circle the lash actually travels.
            const float ang = std::atan2(dy, dx);
            const float off = std::abs(std::remainderf(ang - baseAngle, 2.0F * kPi));
            if (off > halfArc) continue;
          }
          hits.push_back(e);
          hx.push_back(et.x);
          hy.push_back(et.y);
        }
        const float sweepMul = aoeFalloff(static_cast<int>(hits.size()), pierce);
        for (std::size_t hi = 0; hi < hits.size(); ++hi) {
          const entt::entity e = hits[hi];
          const float dx = hx[hi] - sx;
          const float dy = hy[hi] - sy;
          const float dist2 = dx * dx + dy * dy;
          applyEnemyDamage(e, damage * sweepMul);
          // "Reaper's Harvest" unique: scythe kills restore HP.
          if (w.uniqueHeal > 0.0F) {
            auto* eh = registry_.try_get<Health>(e);
            if (eh != nullptr && eh->hp <= 0.0F && registry_.valid(player_)) {
              auto& php = registry_.get<Health>(player_);
              php.hp = std::min(php.max, php.hp + w.uniqueHeal);
            }
          }
          // Knockback away from the reap center (dead center pushes along aim).
          float pushAngle = std::atan2(dy, dx);
          if (dist2 < 1e-4F) pushAngle = baseAngle;
          applyKnockback(e, pushAngle, w.sweepKnockback);
          spawnParticles(hx[hi], hy[hi], {0.7F, 1.0F, 0.8F, 1.0F}, 6, 4.0F);
        }
        // Visual: a full 360° fading ring around the target, or — for a whip —
        // the arc that was actually swept.
        const auto se = registry_.create();
        registry_.emplace<Transform>(se, sx, sy, sx, sy);
        registry_.emplace<Radius>(se, sweepRadius);
        SweepEffect sw{};
        sw.damage = w.damage; // instant damage stays in fireWeapons
        sw.radius = sweepRadius;
        sw.angle = w.sweepAngle;
        sw.knockback = w.sweepKnockback;
        sw.duration = 0.25F;
        sw.timer = 0.25F;
        sw.startAngle = lead ? baseAngle - halfArc : 0.0F;
        sw.endAngle = lead ? baseAngle + halfArc : 2.0F * kPi;
        sw.color = w.color;
        registry_.emplace<SweepEffect>(se, sw);
        w.timer = cooldown;
        break;
      }
      case AttackType::Chain: {
        // Chain lightning - strike the nearest enemy, then chain onward.
        if (found) {
          // The first jump originates from the target's own position, so hit
          // it immediately (otherwise a lone enemy would never take damage).
          if (bestEnemy != entt::null && registry_.valid(bestEnemy)) {
            applyEnemyDamage(bestEnemy, damage);
            const auto& bt = registry_.get<Transform>(bestEnemy);
            spawnParticles(bt.x, bt.y, {0.55F, 1.0F, 1.0F, 1.0F}, 6, 4.0F);
          }
          const auto chain = registry_.create();
          registry_.emplace<Transform>(chain, targetX, targetY, targetX, targetY);
          registry_.emplace<Radius>(chain, w.chainJumpRange);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(chain, s);
          ChainLightning cl{};
          cl.damage = w.damage; // base; updateChainLightning scales by damageMul
          cl.maxJumps = w.chainMaxJumps + stats_.pierceAdd;
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
        registry_.emplace<Radius>(nova, w.novaMaxRadius * (1.0F + stats_.projAdd * 0.1F));
        Sprite s{};
        s.color = w.color;
        s.circle = true;
        registry_.emplace<Sprite>(nova, s);
        NovaRing nr{};
        nr.damagePerTick = w.novaDamagePerTick;
        nr.pierce = pierce;
        nr.maxRadius = w.novaMaxRadius * (1.0F + stats_.projAdd * 0.1F);
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
          // More projectiles = a wider zone; more pierce = denser damage.
          const float zr = w.zoneRadius * (1.0F + stats_.projAdd * 0.2F);
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
            ze.dps = w.zoneDps + stats_.pierceAdd * 2.0F;
            ze.pierce = pierce;
            ze.radius = zr;
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
      case AttackType::Lure: {
        // Grave Bell: plant a beacon just short of the player's target. It does
        // no damage on impact at all — its value is the drag it applies
        // afterwards, so it is planted BEHIND the prey: the bell has to sit
        // between the player and the horde for anything to be funneled.
        if (found) {
          constexpr float kLead = 1.1F;
          const float bx = targetX - std::cos(baseAngle) * kLead;
          const float by = targetY - std::sin(baseAngle) * kLead;
          // Cap the number of live bells: a taunt that covers the screen is not
          // a taunt, it is a wall. The oldest bell gives way.
          int live = 0;
          entt::entity oldest = entt::null;
          float oldestAge = -1.0F;
          for (const auto be : registry_.view<Transform, Lure>()) {
            const auto& bl = registry_.get<Lure>(be);
            ++live;
            const float age = bl.maxLife - bl.life;
            if (oldest == entt::null || age > oldestAge) {
              oldest = be;
              oldestAge = age;
            }
          }
          if (live >= std::max(1, w.lureMaxBeacons) && oldest != entt::null) {
            destroyQueue_.push_back(oldest);
          }
          const auto bell = registry_.create();
          registry_.emplace<Transform>(bell, bx, by, bx, by);
          registry_.emplace<Radius>(bell, w.lureRadius);
          Sprite s{};
          s.color = w.color;
          s.circle = true;
          registry_.emplace<Sprite>(bell, s);
          Lure lu{};
          lu.damage = w.lureDps;
          lu.pierce = pierce;
          lu.radius = w.lureRadius;
          lu.reach = w.lureReach;
          lu.pull = w.lurePull;
          lu.life = w.lureDuration;
          lu.maxLife = w.lureDuration;
          lu.tickRate = w.lureTickRate;
          lu.tickTimer = 0.0F;
          lu.weaponIndex = i;
          lu.color = w.color;
          registry_.emplace<Lure>(bell, lu);
          spawnParticles(bx, by, w.color, 12, 3.0F);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Inferno: {
        // Inferno evolution (flame + scythe): reaps a full circle around the
        // nearest enemy AND leaves burning ground that keeps dealing damage.
        const float sweepRadius = w.sweepRadius * (1.0F + stats_.projAdd * 0.15F);
        const float ix = targetX;
        const float iy = targetY;
        auto iview = registry_.view<Transform, Health, Radius, Enemy>();
        std::vector<entt::entity> ihits;
        std::vector<float> ihx;
        std::vector<float> ihy;
        for (const auto e : iview) {
          const auto& et = iview.get<Transform>(e);
          const float dx = et.x - ix;
          const float dy = et.y - iy;
          const float dist2 = dx * dx + dy * dy;
          if (dist2 > sweepRadius * sweepRadius) continue;
          ihits.push_back(e);
          ihx.push_back(et.x);
          ihy.push_back(et.y);
        }
        const float infernoMul = aoeFalloff(static_cast<int>(ihits.size()), pierce);
        for (std::size_t hi = 0; hi < ihits.size(); ++hi) {
          const entt::entity e = ihits[hi];
          const float dx = ihx[hi] - ix;
          const float dy = ihy[hi] - iy;
          const float dist2 = dx * dx + dy * dy;
          applyEnemyDamage(e, damage * infernoMul);
          float pushAngle = std::atan2(dy, dx);
          if (dist2 < 1e-4F) pushAngle = baseAngle; // dead center: push along aim
          applyKnockback(e, pushAngle, w.sweepKnockback);
          spawnParticles(ihx[hi], ihy[hi], {1.0F, 0.5F, 0.1F, 1.0F}, 6, 4.0F);
        }
        // Visible full-circle ring of fire around the target.
        const auto se = registry_.create();
        registry_.emplace<Transform>(se, ix, iy, ix, iy);
        registry_.emplace<Radius>(se, sweepRadius);
        SweepEffect sw{};
        sw.damage = w.damage; // instant damage stays in fireWeapons
        sw.radius = sweepRadius;
        sw.angle = w.sweepAngle;
        sw.knockback = w.sweepKnockback;
        sw.duration = 0.25F;
        sw.timer = 0.25F;
        sw.startAngle = 0.0F;
        sw.endAngle = 2.0F * kPi;
        sw.color = {1.0F, 0.45F, 0.1F, 1.0F};
        registry_.emplace<SweepEffect>(se, sw);
        // Burning ground: persistent DPS zone at the reap center.
        const float zr = w.zoneRadius * (1.0F + stats_.projAdd * 0.2F);
        const auto zone = registry_.create();
        registry_.emplace<Transform>(zone, ix, iy, ix, iy);
        registry_.emplace<Radius>(zone, zr);
        Sprite zs{};
        zs.color = w.color;
        zs.color.a = 0.3F;
        zs.circle = true;
        registry_.emplace<Sprite>(zone, zs);
        ZoneEffect ze{};
        ze.dps = w.zoneDps + stats_.pierceAdd * 2.0F;
        ze.pierce = pierce;
        ze.radius = zr;
        ze.duration = w.zoneDuration;
        ze.tickRate = 0.1F;
        ze.timer = 0.0F;
        ze.tickTimer = 0.0F;
        ze.color = {1.0F, 0.5F, 0.1F, 1.0F};
        registry_.emplace<ZoneEffect>(zone, ze);
        w.timer = cooldown;
        break;
      }
      case AttackType::Pulsar: {
        // Pulsar evolution (beam + shuriken): a light-chakram that slices out
        // and back, dragging a burning laser trail. Enemies touching the trail
        // keep taking damage; the blade still pierces on contact.
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * w.spread;
          const float angle = baseAngle + offset;
          const auto boom = registry_.create();
          registry_.emplace<Transform>(boom, pt.x, pt.y, pt.x, pt.y);
          registry_.emplace<Velocity>(boom, std::cos(angle) * w.speed, std::sin(angle) * w.speed);
          registry_.emplace<Radius>(boom, 0.16F);
          Sprite s{};
          s.color = w.color;
          s.circle = false; // spinning blade look
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
          bp.blastRadius = w.area;
          bp.trailWidth = w.beamWidth;
          bp.trailDamage = damage; // trail tick damage (scaled by damageMul)
          bp.trailTick = 0.15F;
          bp.trailTimer = 0.0F;
          bp.color = w.color;
          registry_.emplace<BoomerangProjectile>(boom, bp);
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Halo: {
        // Halo beams are persistent entities created in addWeapon()/syncHaloBeams
        // and steered in updateHaloBeams(); firing only paces the cooldown.
        w.timer = cooldown;
        break;
      }
      case AttackType::Vortex: {
        // Vortex zones are persistent entities created in addWeapon()/
        // syncVortices and steered in updateVortices(); firing paces the
        // cooldown. Both the COUNT and the SIZE come from the projectile stat
        // (see syncVortices), so "+1 projectile" visibly thickens the ring.
        w.timer = cooldown;
        break;
      }
      case AttackType::Prism: {
        // Prism Array: instead of one beam toward one target, lock a SEPARATE
        // beam onto each of the N closest enemies. Every line is an independent
        // damage corridor, so a crowd is chewed from N angles at once.
        const int maxTargets = std::max(1, w.prismMaxTargets);
        const int beams = std::min(maxTargets, count);
        // Collect candidates by distance, then keep the closest `beams` of them.
        std::vector<std::pair<float, entt::entity>> cands;
        cands.reserve(static_cast<std::size_t>(beams));
        for (const auto e : registry_.view<Transform, Enemy>()) {
          const auto& et = registry_.get<Transform>(e);
          const float dx = et.x - pt.x;
          const float dy = et.y - pt.y;
          const float d2 = dx * dx + dy * dy;
          if (d2 > w.prismRange * w.prismRange) continue;
          cands.emplace_back(d2, e);
        }
        std::sort(cands.begin(), cands.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const std::size_t n = std::min<std::size_t>(cands.size(),
                                                     static_cast<std::size_t>(beams));
        for (std::size_t k = 0; k < n; ++k) {
          const auto& et = registry_.get<Transform>(cands[k].second);
          // --- Ricochet: pierce IS the number of reflections ------------------
          // The beam does not stop at the first body it burns through: it comes
          // off it and jumps to the next victim within reach, once per pierce,
          // drawing itself as a chain of segments. This is what makes the Prism
          // Array a super evolution instead of "several lasers", and it is why
          // the card that raises pierce visibly turns a beam into a zig-zag that
          // keeps chewing through a crowd.
          std::vector<entt::entity> hit;
          hit.push_back(cands[k].second);
          float lastX = et.x;
          float lastY = et.y;
          const int reflections = std::max(0, pierce);
          for (int r = 0; r < reflections; ++r) {
            entt::entity next = entt::null;
            float bestD2 = w.prismRicochet * w.prismRicochet;
            for (const auto e : registry_.view<Transform, Enemy>()) {
              if (std::find(hit.begin(), hit.end(), e) != hit.end()) continue;
              const auto& nt = registry_.get<Transform>(e);
              const float dx = nt.x - lastX;
              const float dy = nt.y - lastY;
              const float d2 = dx * dx + dy * dy;
              if (d2 >= bestD2) continue;
              bestD2 = d2;
              next = e;
            }
            if (next == entt::null) break; // nothing left to bounce to
            const auto& nt = registry_.get<Transform>(next);
            lastX = nt.x;
            lastY = nt.y;
            hit.push_back(next);
          }
          // One beam effect per segment of the path.
          float fromX = pt.x;
          float fromY = pt.y;
          for (std::size_t seg = 0; seg < hit.size(); ++seg) {
            const auto& ht = registry_.get<Transform>(hit[seg]);
            const auto beam = registry_.create();
            registry_.emplace<Transform>(beam, fromX, fromY, fromX, fromY);
            // Split the prism's colour across its shafts so the fan reads as
            // separate beams instead of one thick smear.
            const float hueMix =
                (static_cast<float>(k) + static_cast<float>(seg) * 0.3F) /
                static_cast<float>(std::max<std::size_t>(1, n));
            registry_.emplace<Radius>(beam, w.prismWidth * 0.5F);
            Sprite s{};
            s.color = w.color;
            s.color.g = std::clamp(w.color.g * (1.0F - 0.45F * hueMix), 0.0F, 1.0F);
            s.color.b = std::clamp(w.color.b + 0.35F * hueMix, 0.0F, 1.0F);
            s.circle = false;
            registry_.emplace<Sprite>(beam, s);
            BeamEffect be{};
            be.damage = w.damage; // base; updateBeamEffects scales by damageMul
            // A reflection is aimed at ONE specific body, so the segment must not
            // be softened by the crowd standing beside it.
            be.pierce = 20;
            be.range = w.prismRange;
            be.width = w.prismWidth;
            be.duration = w.beamDuration;
            be.timer = w.beamDuration;
            be.startX = fromX;
            be.startY = fromY;
            be.endX = ht.x;
            be.endY = ht.y;
            be.color = s.color;
            registry_.emplace<BeamEffect>(beam, be);
            fromX = ht.x;
            fromY = ht.y;
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
  const float hpBefore = eh->hp;
  const float raw = dmg; // pre-mitigation damage, used for the lethal check
  const bool hadShield = tr != nullptr && tr->shield > 0.0F;
  // Enemy defense runs through the SAME flat+percent curve as the player's
  // (mitigateDamage), and grows over the run so late enemies tank more.
  // Armor pierce subtracts from that defense first (clamped at 0), so a pierce
  // build cuts through the flat+percent curve instead of being flattened by it.
  if (tr != nullptr && tr->defense > 0.0F) {
    const float effectiveDefense = std::max(0.0F, tr->defense - stats_.armorPierce);
    if (effectiveDefense > 0.0F) {
      dmg = mitigateDamage(dmg, effectiveDefense);
    }
  }
  if (tr != nullptr && tr->shield > 0.0F) {
    const float absorbed = std::min(tr->shield, dmg);
    tr->shield -= absorbed;
    dmg -= absorbed;
    spawnParticles(registry_.get<Transform>(e).x, registry_.get<Transform>(e).y,
                   {0.5F, 0.8F, 1.0F, 1.0F}, 2, 2.0F);
  }
  eh->hp -= dmg;
  // A hit whose raw damage already covers the remaining HP always kills:
  // defense mitigation must never leave a "0 HP" enemy standing. Shields are
  // exempt so the Shielded trait still buys its buffer.
  const bool rawLethal = raw >= hpBefore && !hadShield;
  if (rawLethal || eh->hp <= kEnemyDeathEpsilon) {
    eh->hp = 0.0F;
    killEnemy(e);
  }
}

void Game::killEnemy(entt::entity e) {
  if (!registry_.valid(e)) return;
  const auto& t = registry_.get<Transform>(e);

  // Death: XP orb + counter; actual destroy deferred. The orb's colour (and
  // size) telegraphs its value: green -> cyan -> violet -> gold.
  const float xpValue = registry_.all_of<Xp>(e) ? registry_.get<Xp>(e).value : 1.0F;
  const auto orb = registry_.create();
  registry_.emplace<Transform>(orb, t.x, t.y, t.x, t.y);
  registry_.emplace<Velocity>(orb);
  registry_.emplace<Radius>(orb, 0.14F + std::min(0.12F, xpValue * 0.006F));
  Sprite s{};
  if (xpValue < 3.0F) {
    s.color = {0.40F, 1.00F, 0.50F, 1.0F}; // green: common
  } else if (xpValue < 8.0F) {
    s.color = {0.40F, 0.80F, 1.00F, 1.0F}; // cyan: uncommon
  } else if (xpValue < 15.0F) {
    s.color = {0.75F, 0.50F, 1.00F, 1.0F}; // violet: rare
  } else {
    s.color = {1.00F, 0.85F, 0.30F, 1.0F}; // gold: elite/champion
  }
  s.circle = true;
  registry_.emplace<Sprite>(orb, s);
  registry_.emplace<Xp>(orb, xpValue);
  destroyQueue_.push_back(e);
  ++kills_;
  // Momentum: every kill feeds the chain, so farming the same weak enemy still
  // works but the chain pays out the same for it.
  if (stats_.momentumGain > 0.0F) {
    streak_ += static_cast<int>(stats_.momentumGain);
  }
  streakTimer_ = 0.0F;

  // Bestiary: remember the type and which tier variant was slain.
  int slainTier = 0;
  if (const auto* tr = registry_.try_get<EnemyTraits>(e); tr != nullptr) {
    slainTier = std::clamp(static_cast<int>(tr->tier), 0, 3);
  }
  // Tier unlock mask + strongest-kill tracking (used by the bestiary and by
  // the elite/champion/overlord player-outline unlocks).
  tierKillMask_ = static_cast<std::uint8_t>(tierKillMask_ | (1u << slainTier));
  // Adaptive tribunal director: handling a tier is the currency that opens the
  // NEXT one. Champions eat this score, overlords eat the champion score, and
  // both bleed away again if the player stops keeping up.
  if (slainTier > 0 && slainTier < 4) {
    // A champion or overlord is worth more than a plain elite: clearing one
    // says more about the build than mowing down three fodder-tier elites.
    tierPressure_[slainTier] += slainTier == 1 ? 1.0F : 1.25F;
  }
  if (slainTier > strongestKilledTier_) {
    strongestKilledTier_ = slainTier;
    strongestKilledDef_ = registry_.try_get<Enemy>(e) != nullptr
                              ? registry_.get<Enemy>(e).def
                              : -1;
  }
  if (const auto* en = registry_.try_get<Enemy>(e);
      en != nullptr && en->def >= 0 &&
      static_cast<std::size_t>(en->def) < bestiaryKills_.size()) {
    const std::size_t di = static_cast<std::size_t>(en->def);
    bestiaryKills_[di]++;
    bestiaryTiers_[di] = static_cast<std::uint8_t>(bestiaryTiers_[di] | (1u << slainTier));
  }
  // A tier-t kill earns that tier's player outline for good (all runs).
  syncProfileUnlocks();

  // Lifesteal fires on the kill itself, using the slain enemy's lifesteal
  // resistance. This is the single point where healing can trigger, which is
  // what bounds procs by kills rather than by hits.
  tryLifestealOnKill(e);

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

// Lifesteal now rolls on a KILL, not on every hit. With lifesteal L, each
// slain enemy has an L% chance to heal `lifestealHeal` HP; at L >= 100 the
// first point is guaranteed and the excess (L - 100)% rolls a second point.
// The Vampiric Heart unique heals 2 per proc.
//
// Why kill-based: a many-hit weapon (Halo, Beam, chains) lands dozens of small
// hits per second, so per-hit lifesteal healed far faster than any enemy could
// kill the player — the exact opposite of the intended risk/reward. Bounding
// procs by KILLS instead of hits keeps vampirism strong (an overlord kill is
// still a big payoff) while making it scale with how much you actually clear.
// The slain enemy's lifesteal resistance scales the chance down (50% res
// halves it), so late/elite enemies are much harder to drain.
void Game::tryLifestealOnKill(entt::entity source) {
  if (stats_.lifesteal <= 0.0F) return;
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& hp = registry_.get<Health>(player_);
  if (hp.hp >= hp.max) return;
  float res = 0.0F;
  if (auto* tr = registry_.try_get<EnemyTraits>(source)) {
    res = tr->lifestealRes;
  }
  std::uniform_real_distribution<float> unit(0.0F, 100.0F);
  const float L = stats_.lifesteal * (1.0F - std::clamp(res, 0.0F, 1.0F));
  const float heal = stats_.lifestealHeal >= 2 ? 2.0F : 1.0F;
  int procs = 0;
  if (L >= 100.0F) {
    procs = 1;
    if (unit(rng_) < (L - 100.0F)) ++procs;
  } else if (unit(rng_) < L) {
    procs = 1;
  }
  if (procs > 0) {
    hp.hp = std::min(hp.max, hp.hp + heal * static_cast<float>(procs));
  }
}

float Game::displacementResistance(entt::entity e) const {
  // The stored value is the resistance the enemy spawned with (which already
  // includes the tier and trait bonuses). Taking the max with the CURRENT
  // time-based ramp also lifts old enemies into the late-game curve instead of
  // leaving early spawns permanently soft.
  float res = std::min(0.7F, simTime_ / 750.0F);
  if (const auto* tr = registry_.try_get<EnemyTraits>(e); tr != nullptr) {
    res = std::max(res, tr->knockbackRes);
  }
  return std::clamp(res, 0.0F, 1.0F);
}

float Game::continuousPullScale(entt::entity e) const {
  // 80% of the resistance applies to a sustained field, with a 20% floor. A
  // fully resistant overlord can still be nudged, but it cannot be corkscrewed
  // and deleted by an aura alone.
  return std::max(0.2F, 1.0F - 0.8F * displacementResistance(e));
}

// Applies knockback to an enemy, reduced by its knockback resistance.
void Game::applyKnockback(entt::entity e, float angle, float force) {
  // Store the shove as a decaying knockback velocity on the enemy. The AI seek
  // overwrites Velocity every tick, so writing it there would be a no-op; this
  // separate channel is added to movement in updateEnemies and decays away.
  auto* en = registry_.try_get<Enemy>(e);
  if (en == nullptr) return;
  const float f = force * stats_.knockbackMul * (1.0F - displacementResistance(e));
  en->kbX += std::cos(angle) * f;
  en->kbY += std::sin(angle) * f;
}

void Game::retaliateKnockback(entt::entity attacker) {
  if (stats_.knockbackRetaliate <= 0.0F) return;
  if (attacker == entt::null || !registry_.valid(attacker)) return;
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const auto& at = registry_.get<Transform>(attacker);
  // Shove the attacker directly away from the player.
  const float angle = std::atan2(at.y - pt.y, at.x - pt.x);
  applyKnockback(attacker, angle, stats_.knockbackRetaliate);
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

      // Damage. Lifesteal no longer rolls here — it fires on the kill itself
      // (see killEnemy), so a many-hit weapon cannot heal per tick. `dealt` is
      // still needed to know the hit actually landed (for chain lightning).
      const float hpBefore = eh->hp;
      applyEnemyDamage(enemy, pr.damage);
      const float dealt = hpBefore - eh->hp;

      // Knockback on hit.
      if (pr.strength > 0.0F && player_ != entt::null) {
        const auto& ptx = registry_.get<Transform>(player_);
        const float pushAngle = std::atan2(et.y - ptx.y, et.x - ptx.x);
        applyKnockback(enemy, pushAngle, pr.strength / 60.0F);
      }

      // Area splash (with crowd falloff).
      if (pr.area > 0.0F) {
        std::vector<entt::entity> splash;
        auto splashView = registry_.view<Transform, Health, Radius>();
        for (const auto se : splashView) {
          const auto& st = splashView.get<Transform>(se);
          const float sdx = st.x - et.x;
          const float sdy = st.y - et.y;
          if (sdx * sdx + sdy * sdy < pr.area * pr.area) {
            splash.push_back(se);
          }
        }
        const float splashMul = aoeFalloff(static_cast<int>(splash.size()), pr.pierce);
        for (const auto se : splash) {
          applyEnemyDamage(se, pr.damage * 0.5F * splashMul);
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
  auto view = registry_.view<Transform, OrbitBlade>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& ob = view.get<OrbitBlade>(e);

    // Derive damage/pierce/speed/radius from the owning weapon slot each tick
    // so upgrades (incl. the "Blade Vortex" unique) apply to existing blades.
    const bool owned = ob.weaponIndex >= 0 && ob.weaponIndex < weaponCount_;
    const float damage = owned ? weapons_[ob.weaponIndex].damage * stats_.damageMul * momentumDamageMul_
                               : ob.damage * stats_.damageMul * momentumDamageMul_;
    const int pierce =
        owned ? weapons_[ob.weaponIndex].pierce + stats_.pierceAdd
              : ob.pierce + stats_.pierceAdd;
    (void)pierce;
    const float speed = owned ? weapons_[ob.weaponIndex].orbitSpeed : ob.speed;
    const float radius = owned ? weapons_[ob.weaponIndex].orbitRadius : ob.radius;

    t.px = t.x;
    t.py = t.y;

    // Dagger spin scales with attack speed: faster fire rate = faster spin.
    ob.angle += (speed * std::max(0.5F, 1.0F + stats_.fireRateBonus + momentumRate_)) / 60.0F;
    t.x = pt.x + std::cos(ob.angle) * radius;
    t.y = pt.y + std::sin(ob.angle) * radius;

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

      applyEnemyDamage(en, damage);
      spawnParticles(et.x, et.y, {0.85F, 0.9F, 1.0F, 1.0F}, 2, 2.0F);
    }
  }

  // --- The whirl: the inside of the ring ---------------------------------------
  // Blades live ON a circle, so anything hugging the player (which, in a
  // survivors-like, is most of the horde once it closes) used to sit in a blind
  // spot and take literally nothing while the ring spun harmlessly overhead.
  // The spin now also grinds its way through the interior at a reduced rate.
  // This runs once per step for the whole build, not per blade: N blades means
  // N times the contact damage but only one whirl.
  {
    const int orbitSlot = [&] {
      for (int i = 0; i < weaponCount_; ++i) {
        if (weapons_[i].attackType == AttackType::Orbit) return i;
      }
      return -1;
    }();
    if (orbitSlot < 0) return;
    const auto& w = weapons_[orbitSlot];
    const float radius = w.orbitRadius;
    const float damage =
        w.damage * stats_.damageMul * momentumDamageMul_ * kOrbitInnerMul;
    std::vector<entt::entity> inside;
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    for (const auto en : enemyView) {
      const auto& et = enemyView.get<Transform>(en);
      const auto& er = enemyView.get<Radius>(en);
      const float dx = et.x - pt.x;
      const float dy = et.y - pt.y;
      // Strictly inside the blade band, so an enemy standing on the ring is
      // never paid twice for the same tick.
      const float limit = radius - (0.18F + er.r);
      if (limit <= 0.0F) continue;
      if (dx * dx + dy * dy > limit * limit) continue;
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;
      inside.push_back(en);
    }
    if (inside.empty()) return;
    // Same crowd falloff as every other area attack: standing in a horde is
    // still the best way to use the ring, it just is not a free damage bonus.
    const float mul = aoeFalloff(static_cast<int>(inside.size()), w.pierce + stats_.pierceAdd);
    for (const auto en : inside) {
      if (!registry_.valid(en)) continue;
      applyEnemyDamage(en, damage * mul);
    }
    if (inside.size() <= 8) {
      for (const auto en : inside) {
        if (!registry_.valid(en)) continue;
        const auto& et = registry_.get<Transform>(en);
        spawnParticles(et.x, et.y, {0.7F, 0.8F, 1.0F, 1.0F}, 1, 1.5F);
      }
    }
  }
}

void Game::updateHaloBeams() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const float dt = 1.0F / 60.0F;
  auto view = registry_.view<Transform, HaloBeam>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& hb = view.get<HaloBeam>(e);

    // Derive damage/pierce/spin/length from the owning weapon slot each tick so
    // upgrades (projectile count, damage, fire rate) apply to existing beams.
    const bool owned = hb.weaponIndex >= 0 && hb.weaponIndex < weaponCount_;
    const float damage = owned ? weapons_[hb.weaponIndex].damage * stats_.damageMul * momentumDamageMul_
                               : hb.damage * stats_.damageMul * momentumDamageMul_;
    const int pierce = owned ? weapons_[hb.weaponIndex].pierce + stats_.pierceAdd
                             : hb.pierce + stats_.pierceAdd;
    const float spin = owned ? weapons_[hb.weaponIndex].orbitSpeed : hb.spin;
    const float length = owned ? weapons_[hb.weaponIndex].beamRange : hb.length;
    const float width = owned ? weapons_[hb.weaponIndex].beamWidth : hb.width;
    const float knockback =
        owned ? weapons_[hb.weaponIndex].haloKnockback : hb.knockback;

    t.px = t.x;
    t.py = t.y;
    // Attack speed also spins the halo faster, like the dagger ring.
    hb.angle += spin * std::max(0.5F, 1.0F + stats_.fireRateBonus + momentumRate_) * dt;
    t.x = pt.x;
    t.y = pt.y;

    const float ex = pt.x + std::cos(hb.angle) * length;
    const float ey = pt.y + std::sin(hb.angle) * length;
    const float dx = ex - pt.x;
    const float dy = ey - pt.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001F) continue;

    // Damage enemies along the spoke (falloff when it sweeps a whole row).
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    std::vector<entt::entity> hits;
    std::vector<float> hx;
    std::vector<float> hy;
    for (const auto en : enemyView) {
      const auto& et = enemyView.get<Transform>(en);
      const auto& er = enemyView.get<Radius>(en);
      const float lx = et.x - pt.x;
      const float ly = et.y - pt.y;
      const float proj = (lx * dx + ly * dy) / len;
      if (proj < 0.0F || proj > len) continue;
      const float closestX = pt.x + (dx / len) * proj;
      const float closestY = pt.y + (dy / len) * proj;
      const float ddx = et.x - closestX;
      const float ddy = et.y - closestY;
      if (std::sqrt(ddx * ddx + ddy * ddy) > width * 0.5F + er.r) continue;
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;
      hits.push_back(en);
      hx.push_back(et.x);
      hy.push_back(et.y);
    }
    const float haloMul = aoeFalloff(static_cast<int>(hits.size()), pierce);
    for (std::size_t hi = 0; hi < hits.size(); ++hi) {
      auto* eh = registry_.try_get<Health>(hits[hi]);
      if (eh == nullptr) continue;
      applyEnemyDamage(hits[hi], damage * dt * haloMul);
      // The spoke shoves what it touches outward (super halo).
      if (knockback > 0.0F) {
        applyKnockback(hits[hi], hb.angle, knockback * dt);
      }
      spawnParticles(hx[hi], hy[hi], {1.0F, 1.0F, 0.75F, 1.0F}, 2, 2.0F);
    }
  }
}

// Vortex evolution: the suction zones circle the player and haul anything they
// touch toward their own core. Pull is applied EVERY tick (it is the defining
// mechanic); the damage only lands once the prey has been dragged inside the
// core radius, so the zones read as crushers rather than plain damage pools.
void Game::updateVortices() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const float dt = 1.0F / 60.0F;
  auto view = registry_.view<Transform, Vortex>();
  for (const auto e : view) {
    auto& t = registry_.get<Transform>(e);
    auto& vx = registry_.get<Vortex>(e);

    // Live values come from the owning weapon so upgrades apply as they land.
    const bool owned = vx.weaponIndex >= 0 && vx.weaponIndex < weaponCount_;
    auto& w = weapons_[owned ? vx.weaponIndex : 0];
    const float damage = (owned ? w.damage : vx.damage) * stats_.damageMul * momentumDamageMul_;
    const int pierce = (owned ? w.pierce : vx.pierce) + stats_.pierceAdd;
    const float pull = owned ? w.vortexPull : vx.pull;

    t.px = t.x;
    t.py = t.y;
    // Attack speed also whips the zones around faster, like the dagger ring.
    vx.angle += vx.spin * std::max(0.5F, 1.0F + stats_.fireRateBonus + momentumRate_) * dt;
    t.x = pt.x + std::cos(vx.angle) * vx.orbitRadius;
    t.y = pt.y + std::sin(vx.angle) * vx.orbitRadius;

    // --- Inward drag ---------------------------------------------------------
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    std::vector<entt::entity> caught;
    for (const auto en : enemyView) {
      auto& et = registry_.get<Transform>(en);
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;
      const float dx = t.x - et.x;
      const float dy = t.y - et.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 > vx.reach * vx.reach || d2 < 1e-6F) continue;
      const float d = std::sqrt(d2);
      // Drag scales down at the very edge of the reach so enemies are nudged
      // in rather than teleported, and clamp so nobody overshoots the core.
      const float edge = std::clamp(1.0F - (d - vx.radius) / std::max(0.001F, vx.reach - vx.radius), 0.35F, 1.0F);
      // Unlike a one-off shove, this force is applied every tick. It therefore
      // has to respect the enemy's live knockback resistance, otherwise late
      // bosses can never escape and the aura deletes the whole game by itself.
      const float step =
          std::min(pull * edge * continuousPullScale(en) * dt,
                   std::max(0.0F, d - vx.radius * 0.15F));
      et.x += (dx / d) * step;
      et.y += (dy / d) * step;
      caught.push_back(en);
    }

    // --- Core damage ---------------------------------------------------------
    vx.tickTimer -= dt;
    if (vx.tickTimer > 0.0F) continue;
    vx.tickTimer = 1.0F / std::max(0.02F, vx.tickRate);

    std::vector<entt::entity> inside;
    for (const auto en : caught) {
      if (!registry_.valid(en)) continue;
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;
      const auto& et = registry_.get<Transform>(en);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      if (dx * dx + dy * dy > vx.radius * vx.radius) continue;
      inside.push_back(en);
    }
    const float vortexMul = aoeFalloff(static_cast<int>(inside.size()), pierce);
    const float tickDamage = damage * std::max(0.02F, vx.tickRate) * vortexMul;
    for (const auto en : inside) {
      if (!registry_.valid(en)) continue;
      applyEnemyDamage(en, tickDamage);
      if (static_cast<int>(inside.size()) <= 4) {
        const auto& et = registry_.get<Transform>(en);
        spawnParticles(et.x, et.y, vx.color, 2, 2.5F);
      }
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

    // Contact detonation. A shell with a FUSE ignores the horde entirely and
    // only cooks where its arc lands: that is what makes a mortar a mortar
    // instead of a slower hammer — it lobs over the front rank and detonates on
    // the far side of the crowd the player is aiming past.
    bool hit = false;
    if (bp.fuse <= 0.0F) {
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
      });
    }
    if (hit) {
      // Explode where the bomb is — it has just moved into the enemy, so the
      // blast lands on the target (and the bomb entity is always removed).
      explodeBomb(e, bp, t.x, t.y);
      destroyQueue_.push_back(e);
      continue;
    }

    // Landing: once past the apex the bomb drops back to launch height —
    // that's where it detonates, so it never tunnels below the ground and
    // vanishes. The boom is always visible and always removes the bomb.
    if (v.y < 0.0F && t.y <= bp.startY) {
      explodeBomb(e, bp, t.x, t.y);
      destroyQueue_.push_back(e);
    }
  }
}

void Game::explodeBomb(entt::entity, const BombProjectile& bp, float x, float y) {
  // Explosion damage in radius, with crowd falloff.
  auto view = registry_.view<Transform, Health, Radius, Enemy>();
  std::vector<entt::entity> hits;
  std::vector<float> hx;
  std::vector<float> hy;
  for (const auto e : view) {
    const auto& t = view.get<Transform>(e);
    const float dx = t.x - x;
    const float dy = t.y - y;
    if (dx * dx + dy * dy < bp.explodeRadius * bp.explodeRadius) {
      hits.push_back(e);
      hx.push_back(t.x);
      hy.push_back(t.y);
    }
  }
  const float blastMul = aoeFalloff(static_cast<int>(hits.size()), bp.pierce);
  for (std::size_t hi = 0; hi < hits.size(); ++hi) {
    applyEnemyDamage(hits[hi], bp.damage * blastMul);
    // Knockback (reduced by the enemy's knockback resistance).
    if (bp.knockback > 0.0F) {
      const float angle = std::atan2(hy[hi] - y, hx[hi] - x);
      applyKnockback(hits[hi], angle, bp.knockback);
    }
    spawnParticles(hx[hi], hy[hi], {1.0F, 0.5F, 0.1F, 1.0F}, 4, 3.0F);
  }
  // Big explosion particles
  spawnParticles(x, y, {1.0F, 0.5F, 0.1F, 1.0F}, 20, 6.0F);
  spawnParticles(x, y, {1.0F, 1.0F, 0.5F, 1.0F}, 10, 4.0F);
}

void Game::updateBoomerangProjectiles() {
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
      // Return to the launch point, not the player's current position —
      // the start is stored in the projectile, so no per-frame look-up.
      const float dx = bp.startX - t.x;
      const float dy = bp.startY - t.y;
      const float dist = std::sqrt(dx * dx + dy * dy);
      if (dist > 0.001F) {
        v.x = (dx / dist) * bp.returnSpeed;
        v.y = (dy / dist) * bp.returnSpeed;
      }
      // Check if reached player
      if (dist < 0.5F) {
        // "Return Tempest" unique: detonate a burst at the return point.
        if (bp.blastRadius > 0.0F) {
          std::vector<entt::entity> blast;
          auto blastView = registry_.view<Transform, Health, Radius, Enemy>();
          for (const auto be : blastView) {
            const auto& bt = blastView.get<Transform>(be);
            const float bdx = bt.x - t.x;
            const float bdy = bt.y - t.y;
            if (bdx * bdx + bdy * bdy < bp.blastRadius * bp.blastRadius) {
              blast.push_back(be);
            }
          }
          const float blastMul = aoeFalloff(static_cast<int>(blast.size()), bp.pierce);
          for (const auto be : blast) {
            applyEnemyDamage(be, bp.damage * 2.0F * blastMul);
          }
          spawnParticles(t.x, t.y, {0.6F, 0.9F, 1.0F, 1.0F}, 16, 5.0F);
        }
        destroyQueue_.push_back(e);
        continue;
      }
    }

    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    // "Pulsar" evolution: the blade drags a burning laser trail. Every trail
    // tick, everything within trailWidth/2 of the segment swept this frame
    // takes damage (uses t.px/t.py = previous position).
    if (bp.trailWidth > 0.0F) {
      bp.trailTimer += 1.0F / 60.0F;
      if (bp.trailTimer >= bp.trailTick) {
        bp.trailTimer = 0.0F;
        const float halfW = bp.trailWidth * 0.5F;
        const float segx = t.x - t.px;
        const float segy = t.y - t.py;
        const float segLen2 = segx * segx + segy * segy;
        auto trailView = registry_.view<Transform, Health, Radius, Enemy>();
        std::vector<entt::entity> thits;
        std::vector<float> thp;
        for (const auto te : trailView) {
          const auto& tt = trailView.get<Transform>(te);
          const auto& tr = trailView.get<Radius>(te);
          // Distance from the enemy center to the swept segment.
          float d2 = 0.0F;
          if (segLen2 < 1e-6F) {
            const float qx = tt.x - t.px;
            const float qy = tt.y - t.py;
            d2 = qx * qx + qy * qy;
          } else {
            const float qx = tt.x - t.px;
            const float qy = tt.y - t.py;
            float fParam = (qx * segx + qy * segy) / segLen2;
            fParam = fParam < 0.0F ? 0.0F : (fParam > 1.0F ? 1.0F : fParam);
            const float cx = t.px + fParam * segx;
            const float cy = t.py + fParam * segy;
            const float cx2 = tt.x - cx;
            const float cy2 = tt.y - cy;
            d2 = cx2 * cx2 + cy2 * cy2;
          }
          const float hitR = halfW + tr.r;
          if (d2 > hitR * hitR) continue;
          auto* teh = registry_.try_get<Health>(te);
          if (teh == nullptr || teh->hp <= 0.0F) continue;
          thits.push_back(te);
          thp.push_back(teh->hp);
        }
        const float trailMul = aoeFalloff(static_cast<int>(thits.size()), bp.pierce);
        for (std::size_t ti = 0; ti < thits.size(); ++ti) {
          auto* teh = registry_.try_get<Health>(thits[ti]);
          if (teh == nullptr) continue;
          applyEnemyDamage(thits[ti], bp.trailDamage * trailMul);
        }
        spawnParticles(t.x, t.y, {0.55F, 1.0F, 1.0F, 0.6F}, 3, 2.0F);
      }
    }

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

      applyEnemyDamage(enemy, bp.damage);

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

    if (!bp.infinite) {
      bp.life -= 1.0F / 60.0F;
      if (bp.life <= 0.0F) {
        destroyQueue_.push_back(e);
        continue;
      }
    }

    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;

    if (bp.infinite) {
      // Eternal orb: keep hunting — steer toward the nearest live enemy (any
      // range); when none exist, drift back to the player so the orb never
      // flies off the arena forever. Its size is re-synced by fireWeapons.
      float hx = 0.0F, hy = 0.0F;
      if (player_ != entt::null && registry_.valid(player_)) {
        const auto& ppt = registry_.get<Transform>(player_);
        hx = ppt.x;
        hy = ppt.y;
      }
      float best2 = std::numeric_limits<float>::max();
      auto enemies = registry_.view<Transform, Enemy, Health>();
      for (const auto oe : enemies) {
        const auto& ot = enemies.get<Transform>(oe);
        const auto* oeh = registry_.try_get<Health>(oe);
        if (oeh == nullptr || oeh->hp <= 0.0F) continue;
        const float cdx = ot.x - t.x;
        const float cdy = ot.y - t.y;
        const float cd2 = cdx * cdx + cdy * cdy;
        if (cd2 < best2) {
          best2 = cd2;
          hx = ot.x;
          hy = ot.y;
        }
      }
      const float sdx = hx - t.x;
      const float sdy = hy - t.y;
      const float sdist = std::sqrt(sdx * sdx + sdy * sdy);
      if (sdist > 0.001F) {
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        v.x = (sdx / sdist) * speed;
        v.y = (sdy / sdist) * speed;
      }
    }

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

      const float scaledDamage =
          bp.damage * std::powf(bp.damageMul, static_cast<float>(bp.bounceCount));
      const int pierce = bp.pierce + stats_.pierceAdd; (void)pierce;
      applyEnemyDamage(enemy, scaledDamage);

      // "Echo Detonation" unique: every bounce splashes area damage (falloff).
      if (bp.splashRadius > 0.0F) {
        std::vector<entt::entity> splash;
        auto splashView = registry_.view<Transform, Health, Radius, Enemy>();
        for (const auto se : splashView) {
          if (se == enemy) continue;
          const auto& st = splashView.get<Transform>(se);
          const float sdx = st.x - et.x;
          const float sdy = st.y - et.y;
          if (sdx * sdx + sdy * sdy < bp.splashRadius * bp.splashRadius) {
            splash.push_back(se);
          }
        }
        const float echoMul = aoeFalloff(static_cast<int>(splash.size()), bp.pierce);
        for (const auto se : splash) {
          applyEnemyDamage(se, scaledDamage * 0.5F * echoMul);
        }
        spawnParticles(et.x, et.y, {0.75F, 0.45F, 1.0F, 1.0F}, 10, 4.0F);
      }

      // Find next bounce target
      bp.bounceCount++;
      bp.lastHitX = et.x;
      bp.lastHitY = et.y;

      // The eternal orb has no bounce budget and no re-target pass — the
      // steering above keeps it engaged forever.
      if (!bp.infinite) {
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

    // Damage enemies along the beam line (falloff when it catches a row).
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    std::vector<entt::entity> bline;
    std::vector<float> bhx;
    std::vector<float> bhy;
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
      bline.push_back(en);
      bhx.push_back(et.x);
      bhy.push_back(et.y);
    }
    const float beamMul = aoeFalloff(static_cast<int>(bline.size()), be.pierce);
    for (std::size_t bi = 0; bi < bline.size(); ++bi) {
      const entt::entity en = bline[bi];
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr) continue;
      applyEnemyDamage(en, be.damage * stats_.damageMul * momentumDamageMul_ / 60.0F * beamMul);
      spawnParticles(bhx[bi], bhy[bi], {1.0F, 1.0F, 0.75F, 1.0F}, 2, 2.0F);
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
      // Damage enemies in zone (falloff when many are caught at once).
      auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
      std::vector<entt::entity> zhits;
      std::vector<float> zhx;
      std::vector<float> zhy;
      for (const auto en : enemyView) {
        const auto& et = enemyView.get<Transform>(en);
        const auto& er = enemyView.get<Radius>(en);
        const float dx = et.x - t.x;
        const float dy = et.y - t.y;
        if (dx * dx + dy * dy < (ze.radius + er.r) * (ze.radius + er.r)) {
          auto* eh = registry_.try_get<Health>(en);
          if (eh == nullptr || eh->hp <= 0.0F) continue;
          zhits.push_back(en);
          zhx.push_back(et.x);
          zhy.push_back(et.y);
        }
      }
      const float zoneMul = aoeFalloff(static_cast<int>(zhits.size()), ze.pierce);
      const float dmg = ze.dps * ze.tickRate * stats_.damageMul * momentumDamageMul_;
      for (std::size_t zi = 0; zi < zhits.size(); ++zi) {
        auto* eh = registry_.try_get<Health>(zhits[zi]);
        if (eh == nullptr) continue;
        applyEnemyDamage(zhits[zi], dmg * zoneMul);
        spawnParticles(zhx[zi], zhy[zi], {1.0F, 0.4F, 0.15F, 1.0F}, 2, 2.0F);
      }
    }
  }
}

// Grave Bell (lure). Two things happen every tick, and only the second one is
// damage: enemies inside `reach` are DRAGGED toward the bell, and enemies inside
// the core are cut up on a fixed tick. The drag is what makes the weapon work
// — a bell planted a few steps ahead of the player walks the whole horde into a
// two-unit circle, which is a far better crowd-control tool than any amount of
// extra damage would be. Like every other continuous field it respects the
// enemy's live knockback resistance, so a late overlord can still shrug it off.
void Game::updateLures() {
  const float dt = (1.0F / 60.0F) * worldTimeScale_;
  auto view = registry_.view<Transform, Lure>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& lu = view.get<Lure>(e);
    const bool owned = lu.weaponIndex >= 0 && lu.weaponIndex < weaponCount_;

    lu.life -= dt;
    if (lu.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }
    t.px = t.x;
    t.py = t.y;

    // --- Taunt: walk everything in reach toward the bell ----------------------
    auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
    std::vector<entt::entity> caught;
    for (const auto en : enemyView) {
      auto& et = enemyView.get<Transform>(en);
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;
      const float dx = t.x - et.x;
      const float dy = t.y - et.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 > lu.reach * lu.reach || d2 < 1e-6F) continue;
      const float d = std::sqrt(d2);
      // Ease off at the very rim so the horde is coaxed in rather than yanked,
      // and never overshoot the core.
      const float edge = std::clamp(
          1.0F - (d - lu.radius) / std::max(0.001F, lu.reach - lu.radius), 0.25F, 1.0F);
      const float step =
          std::min(lu.pull * edge * continuousPullScale(en) * dt,
                   std::max(0.0F, d - lu.radius * 0.1F));
      et.x += (dx / d) * step;
      et.y += (dy / d) * step;
      caught.push_back(en);
    }

    // --- Kill core ------------------------------------------------------------
    lu.tickTimer += dt;
    if (lu.tickTimer < lu.tickRate) continue;
    const float tickSpan = lu.tickTimer;
    lu.tickTimer = 0.0F;
    std::vector<entt::entity> inside;
    for (const auto en : caught) {
      if (!registry_.valid(en)) continue;
      auto* eh = registry_.try_get<Health>(en);
      if (eh == nullptr || eh->hp <= 0.0F) continue;
      const auto& et = registry_.get<Transform>(en);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      if (dx * dx + dy * dy > lu.radius * lu.radius) continue;
      inside.push_back(en);
    }
    if (inside.empty()) continue;
    // A bell that pulls thirty enemies in should not out-damage the rest of the
    // arsenal thirtyfold, so the core uses the same crowd falloff as a bomb.
    const float mul = aoeFalloff(static_cast<int>(inside.size()), lu.pierce);
    const float baseDamage = owned ? weapons_[lu.weaponIndex].lureDps : lu.damage;
    const float dmg = baseDamage * tickSpan * stats_.damageMul * momentumDamageMul_;
    for (const auto en : inside) {
      if (!registry_.valid(en)) continue;
      applyEnemyDamage(en, dmg * mul);
    }
    if (inside.size() <= 6) {
      for (const auto en : inside) {
        if (!registry_.valid(en)) continue;
        const auto& et = registry_.get<Transform>(en);
        spawnParticles(et.x, et.y, {0.75F, 0.5F, 1.0F, 1.0F}, 2, 2.0F);
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
    const float damage = cl.damage * stats_.damageMul * momentumDamageMul_ * std::powf(cl.damageMul, static_cast<float>(cl.jumpsDone));
    auto* eh = registry_.try_get<Health>(nextTarget);
    if (eh && eh->hp > 0.0F) {
      applyEnemyDamage(nextTarget, damage);

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
      // Damage enemies in the current ring (falloff for a full ring).
      auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
      std::vector<entt::entity> nhits;
      std::vector<float> nhx;
      std::vector<float> nhy;
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
          nhits.push_back(en);
          nhx.push_back(et.x);
          nhy.push_back(et.y);
        }
      }
      const float novaMul = aoeFalloff(static_cast<int>(nhits.size()), nr.pierce);
      for (std::size_t ni = 0; ni < nhits.size(); ++ni) {
        auto* eh = registry_.try_get<Health>(nhits[ni]);
        if (eh == nullptr) continue;
        applyEnemyDamage(nhits[ni], nr.damagePerTick * stats_.damageMul * momentumDamageMul_ * novaMul);
        spawnParticles(nhx[ni], nhy[ni], {0.5F, 0.3F, 1.0F, 1.0F}, 4, 3.0F);
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
    // Living enemies also carry an Xp component (awarded on death) but are
    // never pickups: without this guard every enemy that gets within magnet
    // range was vacuumed into the player and destroyed without dying.
    if (registry_.all_of<Enemy>(e)) continue;
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
      // XP is routed through grantXp() so the sandbox's "no experience" rule
      // is enforced in exactly one place.
      grantXp(registry_.get<Xp>(e).value * stats_.xpMul);
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
          // Black Hole is a continuous field too: it respects the same live
          // displacement resistance as the Void Gyre instead of teleporting
          // even a fully resistant overlord.
          const float step = 1.1F * continuousPullScale(e);
          t.x += (dx / d) * step;
          t.y += (dy / d) * step;
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
    if (testMode_ && testInvuln_) {
      // Immortal: the DoT keeps its timer but does no damage.
    } else {
      auto& hp = registry_.get<Health>(player_);
      hp.hp -= 3.0F / 60.0F;
      if (hp.hp <= 0.0F) {
        hp.hp = 0.0F;
        state_ = RunState::GameOver;
      }
    }
  }
}

void Game::currentScales(float& hp, float& speed, float& touch) const {
  // Growth is piecewise: after the 6-minute mark enemies develop even faster
  // (steeper HP and speed ramps), so the late game keeps escalating.
  if (simTime_ <= 360.0F) {
    hp = 1.0F + simTime_ / 70.0F;
    speed = 1.0F + simTime_ / 600.0F;
  } else {
    hp = 1.0F + 360.0F / 70.0F + (simTime_ - 360.0F) / 45.0F;
    speed = 1.0F + 360.0F / 600.0F + (simTime_ - 360.0F) / 300.0F;
  }
  hp = std::min(hp, 30.0F);
  speed = std::min(speed, 2.4F);
  // Contact damage also creeps up so late enemies hit harder.
  touch = 1.0F + simTime_ / 1500.0F;
}

// Overlord retirement: once an overlord of type X has spawned, X is retired
// and stops appearing, so the run does not keep throwing the same boss at you.
// The 3 most recently unlocked types are exempt (refreshRecentTypes) which
// guarantees the pool never empties; if somehow every type is retired we fall
// back to the exempt set, and ultimately to everything unlocked.
void Game::refreshRecentTypes() {
  recentTypes_.assign(content_.enemies.size(), 0);
  if (content_.enemies.empty()) return;
  // Find the indices of the 3 highest unlock_at values.
  std::vector<std::size_t> order(content_.enemies.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::partial_sort(order.begin(), order.end(), order.end(),
                    [this](std::size_t a, std::size_t b) {
                      return content_.enemies[a].unlockAt > content_.enemies[b].unlockAt;
                    });
  const std::size_t take = std::min<std::size_t>(3, order.size());
  for (std::size_t k = 0; k < take; ++k) recentTypes_[order[k]] = 1;
}

void Game::retireEnemyType(int def) {
  if (def < 0 || static_cast<std::size_t>(def) >= retiredTypes_.size()) return;
  retiredTypes_[static_cast<std::size_t>(def)] = 1;
}

bool Game::typeRetired(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= retiredTypes_.size()) return false;
  return retiredTypes_[static_cast<std::size_t>(def)] != 0;
}

bool Game::testTypeIsRecent(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= recentTypes_.size()) return false;
  return recentTypes_[static_cast<std::size_t>(def)] != 0;
}

bool Game::typeCanSpawn(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= content_.enemies.size()) return false;
  if (simTime_ < content_.enemies[static_cast<std::size_t>(def)].unlockAt) return false;
  if (!typeRetired(def)) return true;
  // Retired, but exempt because it is one of the 3 newest types.
  return testTypeIsRecent(def);
}

void Game::spawnWave() {
  if (!wavesEnabled_) return;
  if (content_.enemies.empty()) return;
  if (registry_.view<Enemy>().size() >= kMaxEnemies) return;

  std::uniform_real_distribution<float> unit(0.0F, 1.0F);

  // Difficulty scalars used by both the regular spawn and horde bursts.
  float hpScale;
  float speedScale;
  float touchScale;
  currentScales(hpScale, speedScale, touchScale);

  // Spawns walk in from farther away each minute, capping at 2x the base
  // distance by 10:00; after that the distance no longer changes.
  const float spawnDist = kSpawnDist * std::min(2.0F, 1.0F + simTime_ / 600.0F);

  // Weighted pick among unlocked, non-retired enemy types (regular spawn +
  // horde bursts). If retirement ever emptied the pool, fall back so the
  // game can never stop spawning.
  auto pickDef = [&]() -> int {
    float totalWeight = 0.0F;
    for (const auto& def : content_.enemies) {
      if (typeCanSpawn(static_cast<int>(&def - content_.enemies.data()))) {
        totalWeight += def.weight;
      }
    }
    if (totalWeight <= 0.0F) {
      for (const auto& def : content_.enemies) {
        if (simTime_ >= def.unlockAt) totalWeight += def.weight;
      }
      if (totalWeight <= 0.0F) return -1;
    }
    float roll = unit(rng_) * totalWeight;
    for (std::size_t i = 0; i < content_.enemies.size(); ++i) {
      const auto& candidate = content_.enemies[i];
      if (simTime_ < candidate.unlockAt) continue;
      if (!typeCanSpawn(static_cast<int>(i))) continue;
      roll -= candidate.weight;
      if (roll <= 0.0F) return static_cast<int>(i);
    }
    return -1;
  };

  // Periodic horde bursts tick every frame (not per normal spawn) so their
  // cadence stays a flat ~40-50s. A full ring of enemies closes in from every
  // side — this is what makes it a horde game rather than a trickle shooter.
  if (player_ != entt::null && registry_.valid(player_)) {
    hordeTimer_ -= 1.0F / 60.0F;
    // Announce the ring a beat before it lands. A horde that only shows up as
    // 20 telegraphs is noise; one that is called out is a decision (hold the
    // line, or disengage and rebuild the chain).
    if (hordeTimer_ > 0.0F && hordeTimer_ <= 2.0F && tierBannerT_ < 1.5F) {
      showBanner("HORDE INCOMING", 2.0F);
    }
    if (hordeTimer_ <= 0.0F) {
      hordeTimer_ = 40.0F + unit(rng_) * 10.0F;
      const int hordeDef = pickDef();
      if (hordeDef >= 0) {
        const auto& hpt = registry_.get<Transform>(player_);
        const int hordeSize = std::min(24, 10 + static_cast<int>(simTime_ / 30.0F));
        for (int m = 0; m < hordeSize; ++m) {
          const float angle =
              (static_cast<float>(m) / static_cast<float>(hordeSize)) * 2.0F * kPi +
              (unit(rng_) - 0.5F) * 0.4F;
          PendingSpawn pending{};
          pending.x = hpt.x + std::cos(angle) * spawnDist;
          pending.y = hpt.y + std::sin(angle) * spawnDist;
          pending.t = kSpawnTelegraph;
          pending.def = hordeDef;
          pending.hpMul = std::max(1.0F, hpScale);
          pending.touchMul = touchScale;
          pending.speedMul = speedScale;
          pending.xpMul = 1.0F;
          pending.traits = TraitNone;
          pending.tier = 0;
          pending_.push_back(pending);
        }
      }
    }
  }

  spawnTimer_ -= 1.0F / 60.0F;
  if (spawnTimer_ > 0.0F) return;

  // Difficulty ramp: comfortable start, then spawns accelerate hard so the
  // screen fills with hordes. The floor is 0.20s between packs (was 0.12s):
  // 8 packs a second is a treadmill the player cannot fight, while 5 packs a
  // second of 5-6 enemies each is a screen they can actually push through, and
  // it is the difference between a run that lasts 25 minutes and one that ends
  // in a build-completion screen.
  if (simTime_ < 30.0F) {
    spawnTimer_ = 1.2F - simTime_ * 0.005F;
  } else if (simTime_ < 90.0F) {
    spawnTimer_ = std::max(0.50F, 1.05F - (simTime_ - 30.0F) * 0.0092F);
  } else {
    spawnTimer_ = std::max(0.20F, 0.50F - (simTime_ - 90.0F) * 0.003F);
  }

  const int defIndex = pickDef();
  if (defIndex < 0) return;

  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);

  // Pack spawning grows into hordes: every minute adds another chance to
  // include an extra member, so late runs face clusters instead of singles.
  int packSize = 1;
  const int extraRolls = std::min(7, 1 + static_cast<int>(simTime_ / 70.0F));
  for (int k = 0; k < extraRolls; ++k) {
    if (unit(rng_) < 0.65F) ++packSize;
  }
  packSize = std::min(9, packSize);

  // Cluster center: a random angle at spawn distance. Subsequent pack
  // members spawn close to this center so enemies approach together.
  const float centerAngle = unit(rng_) * 2.0F * kPi;

  for (int member = 0; member < packSize; ++member) {
    const float angle = centerAngle
        + (packSize > 1 ? (static_cast<float>(member) - static_cast<float>(packSize - 1) * 0.5F) * 0.35F : 0.0F)
        + (member > 0 ? (unit(rng_) - 0.5F) * 0.5F : 0.0F);
    const float x = pt.x + std::cos(angle) * spawnDist;
    const float y = pt.y + std::sin(angle) * spawnDist;

    // Per-member elite/champion/overlord rolls (each enemy rolls
    // independently). Elites open at 45s and creep up with time. Champions and
    // overlords are NOT on a clock: the adaptive tribunal director opens them
    // once the previous tier is being handled easily, and the chance scales with
    // how far above that line the player is.
    //
    // Their POWER is deliberately unchanged — an overlord is meant to be the
    // difficulty spike, not a soft one. Only the timing follows the player.
    const float eliteChance = tierSpawnChance(1);
    const float champChance = tierSpawnChance(2);
    const float overlordChance = tierSpawnChance(3);
    bool memberElite = unit(rng_) < eliteChance;
    bool memberChampion = unit(rng_) < champChance;
    const bool memberOverlord = unit(rng_) < overlordChance;
    if (memberChampion) memberElite = true;
    if (memberOverlord) {
      memberElite = true;
      memberChampion = true;
    }

    float mHpMul = hpScale;
    float mTouchMul = touchScale;
    float mSpeedMul = speedScale;
    float mXpMul = 1.0F;
    std::uint32_t mTraits = TraitNone;
    std::uint8_t mTier = 0;
    if (memberElite) {
      mTier = memberOverlord ? 3 : (memberChampion ? 2 : 1);
      // Every elite-and-above gets ALL of its stats boosted so it cannot be
      // deleted instantly; stronger tiers are boosted more.
      const TierBuffs buffs = tierBuffs(mTier);
      mHpMul *= rollTierHpMul(buffs, rng_);
      mTouchMul *= buffs.touch;
      mSpeedMul *= buffs.speed;
      mXpMul = buffs.xp;
      const int traitCount = traitsForTier(mTier, simTime_);
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
          case PickHeavy: mTouchMul *= 2.0F; break;
          case PickArcher: mTraits |= TraitArcher; break;
          case PickAura: mTraits |= TraitAura; break;
          case PickResistant: mTraits |= TraitResistant; break;
          default: break;
        }
      }
    } else {
      // Over time, more and more of the ordinary (tier 0) enemies learn to
      // shoot. The chance ramps from 0 at ~60s to a 25% cap by ~7 minutes, so
      // the early game stays melee-only and a late horde genuinely mixes
      // ranged threats into the swarm. This is separate from the elite trait
      // roll above, which is how "elite archers" have always worked.
      const float rangedChance =
          simTime_ >= 60.0F ? std::min(0.25F, (simTime_ - 60.0F) / 1320.0F) : 0.0F;
      if (rangedChance > 0.0F && unit(rng_) < rangedChance) {
        mTraits |= TraitArcher;
      }
    }

    PendingSpawn pending{};
    pending.x = x;
    pending.y = y;
    pending.t = kSpawnTelegraph;
    pending.def = defIndex;
    // An overlord retires its own type: it will not spawn again this run, so
    // the same boss cannot repeat forever. The 3 newest types stay eligible.
    if (memberOverlord) {
      retireEnemyType(defIndex);
      refreshRecentTypes();
    }
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
  // Champions are noticeably larger than elites; overlords larger still.
  const float radius =
      def.radius * (p.tier >= 3 ? 2.0F : (p.tier == 2 ? 1.6F : (p.tier == 1 ? 1.35F : 1.0F)));

  const auto e = registry_.create();
  registry_.emplace<Transform>(e, p.x, p.y, p.x, p.y);
  registry_.emplace<Velocity>(e);
  registry_.emplace<Radius>(e, radius);
  Sprite s{};
  s.color = elite ? eliteTint(def.color) : def.color;
  s.circle = def.circle;
  registry_.emplace<Sprite>(e, s);
  registry_.emplace<Health>(e, hp, hp);
  Enemy en{};
  en.speed = def.speed * p.speedMul;
  en.touch = def.touch * p.touchMul;
  en.def = p.def;
  registry_.emplace<Enemy>(e, en);
  registry_.emplace<Xp>(e, def.xp * p.xpMul);

  // Every enemy carries traits so defense/resistances can scale with run time,
  // even for normal (tier 0) enemies.
  EnemyTraits tr{};
  tr.flags = p.traits;
  tr.tier = p.tier;
  tr.defense = enemyDefense(simTime_, p.tier);
  const bool resistant = (p.traits & TraitResistant) != 0;
  if (resistant) tr.defense += 40.0F; // a strong-resistance variant
  tr.lifestealRes = enemyLifestealResistance(simTime_, p.tier, resistant);
  tr.knockbackRes = enemyKnockbackResistance(simTime_, p.tier, resistant);
  if (p.traits & TraitRegenerating) tr.regen = hp * 0.02F;
  if (p.traits & TraitShielded) tr.shield = hp * 0.5F;
  if (p.traits & TraitArcher) {
    tr.shootCooldown = std::max(0.9F, 2.4F - simTime_ / 500.0F);
    tr.shootTimer = 0.4F;
  }
  if (p.traits & TraitAura) {
    // Aura size and damage scale hard with the tier: an elite's aura is a
    // small nuisance, a champion's covers real ground you must walk out of,
    // and an overlord's is a zone you cannot stand in at all. Radius grows
    // more than damage so the threat is *space control*, not just chip damage.
    switch (p.tier) {
      case 3:
        tr.auraRadius = 4.2F;
        tr.auraDps = 18.0F + simTime_ / 40.0F;
        break;
      case 2:
        tr.auraRadius = 2.9F;
        tr.auraDps = 11.0F + simTime_ / 45.0F;
        break;
      default:
        tr.auraRadius = 1.7F;
        tr.auraDps = 5.0F + simTime_ / 50.0F;
        break;
    }
  }
  registry_.emplace<EnemyTraits>(e, tr);
}

// --- Momentum (the kill chain) -----------------------------------------------
// Most of this game's power is flat: a card you take at level 12 is worth the
// same at level 12 and at level 40, so a finished build has nothing left to do
// except walk. Momentum is the one thing that cannot be hoarded — it is a meter
// that only pays while you are actively killing, and it is what makes stepping
// INTO the horde the correct move instead of the reckless one.
void Game::updateMomentum() {
  constexpr float dt = 1.0F / 60.0F;
  if (streak_ > 0) {
    streakTimer_ += dt;
    if (streakTimer_ > std::max(0.5F, stats_.momentumWindow)) {
      streak_ = 0; // the chain went cold
    }
  } else {
    streakTimer_ = 0.0F;
  }
  const float n = static_cast<float>(std::clamp(streak_, 0, std::max(0, stats_.momentumMax)));
  momentumDamageMul_ = 1.0F + n * stats_.momentumDamage * 0.01F;
  momentumRate_ = n * stats_.momentumRate * 0.01F;
  momentumSpeedMul_ = 1.0F + n * stats_.momentumSpeed * 0.01F;
}

void Game::breakMomentum() {
  if (streak_ <= 0) return;
  streak_ = std::max(0, streak_ / 2 - 2);
  streakTimer_ = 0.0F;
}

void Game::enterLevelUp() {
  state_ = RunState::LevelUp;
  choosingStarter_ = false;
  starterChoicePending_ = false;
  rerollsUsed_ = 0;
  buildChoices();
}

// --- Adaptive tribunal director ----------------------------------------------
// The heavy tiers answer to the player, not to the clock. Champions wait until
// elites are routine, overlords wait until champions are, and both shut again
// if the run stops keeping up. Killing the tier is what feeds the score, so a
// high-damage build unlocks the next tribunal early and a struggling one never
// sees it — the difficulty curve follows the build instead of the clock.

bool Game::tierUnlocked(int tier) const {
  switch (tier) {
    case 0: return true;
    case 1: return tierOpen_[1] && simTime_ >= 45.0F;
    case 2: return tierOpen_[2];
    case 3: return tierOpen_[3];
    default: return false;
  }
}

float Game::tierSpawnChance(int tier) const {
  if (!tierUnlocked(tier)) return 0.0F;
  if (tier == 1) {
    return std::min(0.15F, 0.05F + simTime_ * 0.0005F);
  }
  if (tier == 2) {
    // The further above the "elites are routine" line the player is, the more
    // champions show up — up to the old late-game cap, so an overwhelming
    // build still gets a real fight instead of an endless stream.
    const float over = tierPressure_[1] - kChampionPressure;
    return std::clamp(over * 0.006F, 0.0F, 0.05F);
  }
  if (tier == 3) {
    const float over = tierPressure_[2] - kOverlordPressure;
    return std::clamp(over * 0.003F, 0.0F, 0.02F);
  }
  return 0.0F;
}

void Game::updateTierDirector() {
  constexpr float dt = 1.0F / 60.0F;
  if (tierBannerT_ > 0.0F) {
    tierBannerT_ -= dt;
    if (tierBannerT_ <= 0.0F) tierBanner_.clear();
  }
  for (int t = 1; t < 4; ++t) {
    // The handling score bleeds away, so only *recent* form counts: a build
    // that was strong ten minutes ago does not keep the overlords coming.
    tierPressure_[t] = std::max(0.0F, tierPressure_[t] - dt / kPressureWindow);
    if (tierGrace_[t] > 0.0F) tierGrace_[t] = std::max(0.0F, tierGrace_[t] - dt);
  }
  const bool champWant =
      simTime_ >= kChampionMinTime && tierPressure_[1] >= kChampionPressure;
  const bool lordWant = tierOpen_[2] && simTime_ >= kOverlordMinTime &&
                        tierPressure_[2] >= kOverlordPressure;
  const auto apply = [this](int tier, bool want, const char* label) {
    if (want) {
      // Announce it once, on the edge of the switch: this is the game's way of
      // saying "you outgrew the last tier".
      if (!tierOpen_[tier]) {
        tierBanner_ = std::string(label) + " TRIBUNAL OPEN";
        tierBannerT_ = 4.0F;
      }
      tierOpen_[tier] = true;
      tierGrace_[tier] = kTierGrace;
    } else if (tierGrace_[tier] <= 0.0F) {
      tierOpen_[tier] = false;
    }
  };
  apply(2, champWant, "CHAMPION");
  apply(3, lordWant, "OVERLORD");
}

// --- Active abilities (J / K / L) ---------------------------------------------
//
// Weapons decide what a build does to the field; abilities decide what the
// player does about the field. All three exist from the first second of a run,
// need no card, and are gated purely by a cooldown, so the interesting decision
// is never "do I have it" but "is now the moment". Each one answers a different
// problem a survivors-like run keeps running into:
//
//   J Phase Dash  - "there are six of them on me and I cannot outrun them"
//   K Overload    - "I am surrounded and my weapons cannot reach through"
//   L Stasis      - "I need three more seconds of shooting at this thing"

void Game::updateAbilities(const FrameInput& input) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  if (input.abilityBlink && abilityReady(Ability::Blink)) castBlink();
  if (input.abilityBurst && abilityReady(Ability::Burst)) castBurst(1.0F);
  if (input.abilitySlow && abilityReady(Ability::Stasis)) castStasis();
}

void Game::castBlink() {
  abilityCd_[static_cast<int>(Ability::Blink)] = abilityCooldown(Ability::Blink);
  auto& t = registry_.get<Transform>(player_);
  // Direction: where the player is walking. Standing still, dash *at* the
  // nearest enemy instead — the common panic case is "I am cornered", and a
  // dash that only works while a movement key is held is not an answer to it.
  float dx = moveX_;
  float dy = moveY_;
  const float inputLen = length(dx, dy);
  if (inputLen > 0.05F) {
    dx /= inputLen;
    dy /= inputLen;
  } else {
    dx = 0.0F;
    dy = 0.0F;
    float best = 1e12F;
    auto view = registry_.view<Transform, Enemy>();
    for (const auto e : view) {
      const auto& et = view.get<Transform>(e);
      const float ddx = et.x - t.x;
      const float ddy = et.y - t.y;
      const float d2 = ddx * ddx + ddy * ddy;
      if (d2 < best) {
        best = d2;
        dx = ddx;
        dy = ddy;
      }
    }
    const float d = length(dx, dy);
    if (d > 0.001F) {
      dx /= d;
      dy /= d;
    } else {
      dx = 1.0F;
      dy = 0.0F;
    }
  }
  // Leave a trail of afterimages along the path, then land on the far side.
  const float dist = stats_.blinkDist;
  for (int i = 1; i <= 4; ++i) {
    const float f = static_cast<float>(i) / 5.0F;
    spawnParticles(t.x + dx * dist * f, t.y + dy * dist * f, {0.45F, 0.85F, 1.0F, 1.0F}, 3,
                   1.5F);
  }
  t.px = t.x;
  t.py = t.y;
  t.x += dx * dist;
  t.y += dy * dist;
  // A dash that cannot be interrupted is not a dodge.
  iframes_ = std::max(iframes_, stats_.blinkIframes);
  spawnParticles(t.x, t.y, {0.6F, 0.95F, 1.0F, 1.0F}, 14, 4.0F);
  // The Echo Chamber unique rides on every ability.
  if (stats_.abilityEcho != 0) castBurst(0.4F);
}

void Game::castBurst(float damageScale) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  if (damageScale >= 1.0F) {
    abilityCd_[static_cast<int>(Ability::Burst)] = abilityCooldown(Ability::Burst);
  }
  auto& t = registry_.get<Transform>(player_);
  const float radius = stats_.burstRadius;
  const float damage =
      stats_.burstDamage * damageScale * stats_.damageMul * momentumDamageMul_;
  auto view = registry_.view<Transform, Health, Radius, Enemy>();
  std::vector<entt::entity> hits;
  for (const auto e : view) {
    const auto& et = view.get<Transform>(e);
    const auto& er = view.get<Radius>(e);
    const float dx = et.x - t.x;
    const float dy = et.y - t.y;
    if (dx * dx + dy * dy > (radius + er.r) * (radius + er.r)) continue;
    hits.push_back(e);
  }
  // Same falloff rule as every other blast in the game, so a crowd is still
  // worth using it on but not worth spamming into.
  const float falloff = aoeFalloff(static_cast<int>(hits.size()), stats_.pierceAdd);
  for (const auto e : hits) {
    const auto& et = registry_.get<Transform>(e);
    const float dx = et.x - t.x;
    const float dy = et.y - t.y;
    const float d = length(dx, dy);
    applyEnemyDamage(e, damage * falloff);
    if (d > 0.001F) {
      applyKnockback(e, std::atan2(dy, dx), stats_.burstKnockback);
    }
    spawnParticles(et.x, et.y, {1.0F, 0.85F, 0.5F, 1.0F}, 4, 4.0F);
  }
  // A bright expanding ring so the player can see exactly what it covered.
  const auto ring = registry_.create();
  registry_.emplace<Transform>(ring, t.x, t.y, t.x, t.y);
  registry_.emplace<Radius>(ring, radius);
  SweepEffect sw{};
  sw.radius = radius;
  sw.angle = 6.2832F;
  sw.duration = 0.35F;
  sw.timer = 0.35F;
  sw.startAngle = 0.0F;
  sw.endAngle = 6.2832F;
  sw.color = {1.0F, 0.85F, 0.45F, 1.0F};
  registry_.emplace<SweepEffect>(ring, sw);
  spawnParticles(t.x, t.y, {1.0F, 0.8F, 0.4F, 1.0F}, 18, 6.0F);
}

void Game::castStasis() {
  abilityCd_[static_cast<int>(Ability::Stasis)] = abilityCooldown(Ability::Stasis);
  stasis_ = stats_.stasisDuration;
  const auto& t = registry_.get<Transform>(player_);
  spawnParticles(t.x, t.y, {0.5F, 0.7F, 1.0F, 1.0F}, 26, 5.0F);
  if (stats_.abilityEcho != 0) castBurst(0.4F);
}

void Game::testTriggerAbility(Ability a) {
  FrameInput in{};
  switch (a) {
    case Ability::Blink: in.abilityBlink = true; break;
    case Ability::Burst: in.abilityBurst = true; break;
    case Ability::Stasis: in.abilitySlow = true; break;
  }
  updateAbilities(in);
}


void Game::enterStarterPick() {
  state_ = RunState::LevelUp;
  choosingStarter_ = true;
  starterChoicePending_ = false;
  rerollsUsed_ = 0;
  milestoneOffer_ = false;
  buildStarterChoices();
}

void Game::buildStarterChoices() {
  choices_.clear();
  std::vector<int> starters;
  for (std::size_t i = 0; i < content_.weapons.size(); ++i) {
    if (content_.weapons[i].starter) starters.push_back(static_cast<int>(i));
  }
  // Fall back to the first few weapons if the content has no starter flags.
  for (std::size_t i = 0; starters.size() < 3 && i < content_.weapons.size(); ++i) {
    const int idx = static_cast<int>(i);
    if (std::find(starters.begin(), starters.end(), idx) == starters.end()) {
      starters.push_back(idx);
    }
  }
  std::shuffle(starters.begin(), starters.end(), rng_);
  for (const int idx : starters) {
    if (choices_.size() >= 3) break;
    choices_.push_back({Choice::Kind::Weapon, idx});
  }
}

void Game::buildChoices() {
  choices_.clear();
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);

  // Milestones arrive on every power-of-two level from 4 on (4, 8, 16, 32, ...).
  const bool milestone = level_ >= 4 && (level_ & (level_ - 1)) == 0;
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

  // Weapon offers are rarer now, but arrive as a CHOICE OF SEVERAL (2 up to
  // the number of free slots) instead of a single card. The sandbox skips them
  // entirely: its whole point is testing ONE weapon, and a second one would
  // pollute every observation.
  const int cap = weaponCap();
  if (!testMode_ && weaponCount_ < cap) {
    const float grantChance =
        (1.0F - static_cast<float>(weaponCount_) / static_cast<float>(cap)) * 0.32F;
    if (unit(rng_) < grantChance) {
      std::vector<int> grants = collectWeaponGrants();
      if (!grants.empty()) {
        int offerN = 2 + (unit(rng_) < 0.5F ? 1 : 0);
        offerN = std::min(offerN, cap - weaponCount_);
        offerN = std::min<int>(offerN, static_cast<int>(grants.size()));
        for (int k = 0; k < offerN; ++k) {
          choices_.push_back({Choice::Kind::Weapon, grants[static_cast<std::size_t>(k)]});
        }
      }
    }
  }

  // A unique treasure card can replace the weapon offer.
  if (choices_.empty() || choices_.front().kind == Choice::Kind::Upgrade) {
    std::vector<int> uniques;
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      const auto& u = content_.upgrades[i];
      if (u.kind != "unique" || stacks_[i] >= u.maxStacks) continue;
      // A weapon's unique item only makes sense if that weapon is equipped.
      if (!u.weapon.empty() && findWeaponSlot(u.weapon) < 0) continue;
      uniques.push_back(static_cast<int>(i));
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

  // Absolute fallback so a level-up never bricks silently. Only cards that can
  // ACTUALLY be applied are eligible: a weapon-specific card whose weapon is not
  // equipped used to be picked here, and choosing it silently failed, leaving
  // the player on the level-up screen forever.
  if (choices_.empty()) {
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      if (stacks_[i] >= content_.upgrades[i].maxStacks) continue;
      if (!upgradeIsUsable(static_cast<int>(i))) continue;
      choices_.push_back({Choice::Kind::Upgrade, static_cast<int>(i)});
      break;
    }
  }
  // Everything is maxed (or nothing left is usable): offer an explicit "continue"
  // card so the level-up always has a working way out.
  if (choices_.empty()) {
    choices_.push_back({Choice::Kind::Skip, -1});
  }
}

bool Game::upgradeIsUsable(int upgradeIndex) const {
  if (upgradeIndex < 0 ||
      static_cast<std::size_t>(upgradeIndex) >= content_.upgrades.size()) {
    return false;
  }
  const auto& def = content_.upgrades[static_cast<std::size_t>(upgradeIndex)];
  return def.weapon.empty() || findWeaponSlot(def.weapon) >= 0;
}

void Game::completeLevelUp() {
  choices_.clear();
  xp_ -= xpNext_;
  ++level_;
  xpNext_ = xpForLevel(level_);

  if (xp_ >= xpNext_) {
    enterLevelUp(); // queued level-ups
  } else {
    state_ = RunState::Playing;
  }
}

void Game::chooseUpgrade(int slot) {
  if (slot < 0 || static_cast<std::size_t>(slot) >= choices_.size()) return;
  const auto choice = choices_[static_cast<std::size_t>(slot)];

  // Opening pick: grant the weapon and start the run without leveling up.
  if (choosingStarter_) {
    if (choice.kind != Choice::Kind::Weapon) return;
    addWeapon(choice.index);
    choosingStarter_ = false;
    starterChoicePending_ = false;
    choices_.clear();
    state_ = RunState::Playing;
    return;
  }

  if (choice.kind == Choice::Kind::Skip) {
    completeLevelUp();
    return;
  }

  if (choice.kind == Choice::Kind::Weapon) {
    addWeapon(choice.index);
  } else {
    // A card can go stale (e.g. its weapon left the arsenal between the roll
    // and the pick). Never consume the level-up in that case: rebuild so the
    // player gets a card that actually works instead of being stuck.
    if (!applyUpgradeAt(choice.index)) {
      buildChoices();
      return;
    }
  }

  completeLevelUp();
}

// Applies one upgrade card. Returns false when the effect is unknown or when a
// weapon-targeted card's weapon is no longer equipped (in both cases the level
// up is left pending rather than silently consumed).
bool Game::applyUpgradeAt(int upgradeIndex) {
  if (upgradeIndex < 0 || static_cast<std::size_t>(upgradeIndex) >= content_.upgrades.size()) {
    return false;
  }
  const auto& def = content_.upgrades[static_cast<std::size_t>(upgradeIndex)];

  UpgradeEffectResult result{false, 0.0F, 0.0F};
  if (def.weapon.empty()) {
    result = applyUpgrade(stats_, def.effect, def.value);
    if (!result.valid) return false;
  } else {
    // Weapon-targeted upgrade (e.g. a weapon's personal Focus card).
    const int weaponSlot = findWeaponSlot(def.weapon);
    if (weaponSlot < 0) return false;
    applyWeaponEffect(weaponSlot, def.effect, def.value);
    result.valid = true;
  }
  ++stacks_[static_cast<std::size_t>(upgradeIndex)];

  // Global "+1 projectile" upgrades also add orbit blades / halo spokes /
  // vortex zones.
  if (def.effect == "proj_add") {
    for (int s = 0; s < weaponCount_; ++s) {
      if (weapons_[s].attackType == AttackType::Orbit) {
        syncOrbitBlades(s);
      } else if (weapons_[s].attackType == AttackType::Halo) {
        syncHaloBeams(s);
      } else if (weapons_[s].attackType == AttackType::Vortex) {
        syncVortices(s);
      }
    }
  }

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
  return true;
}

void Game::reroll() {
  if (state_ != RunState::LevelUp) return;
  if (choosingStarter_) return; // no rerolls on the opening weapon pick
  // The sandbox rolls without a budget so a tester can hunt for a specific
  // card as long as they like. Outside it, the normal charges apply.
  if (!testMode_ && rerollsUsed_ >= 1 + stats_.rerollCharges) return;
  ++rerollsUsed_;
  buildChoices();
}

void Game::addWeapon(int defIndex) {
  if (weaponCount_ >= weaponCap()) return;
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
  w.bounceInfinite = def.bounceInfinite;
  w.liveBounce = entt::null;

  // Beam
  w.beamRange = def.beamRange;
  w.beamWidth = def.beamWidth;
  w.beamDuration = def.beamDuration;

  // Halo
  w.haloKnockback = def.haloKnockback;

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

  // Vortex
  w.vortexRadius = def.vortexRadius;
  w.vortexReach = def.vortexReach;
  w.vortexPull = def.vortexPull;
  w.vortexOrbit = def.vortexOrbit;
  w.vortexOrbitSpeed = def.vortexOrbitSpeed;
  w.vortexTickRate = def.vortexTickRate;

  // Prism
  w.prismRange = def.prismRange;
  w.prismWidth = def.prismWidth;
  w.prismMaxTargets = def.prismMaxTargets;
  w.prismRicochet = def.prismRicochet;

  // Lure
  w.lureRadius = def.lureRadius;
  w.lureReach = def.lureReach;
  w.lurePull = def.lurePull;
  w.lureDps = def.lureDps;
  w.lureDuration = def.lureDuration;
  w.lureTickRate = def.lureTickRate;
  w.lureMaxBeacons = def.lureMaxBeacons;

  // General projectile fields
  w.area = 0.0F;
  w.strength = 0.0F;
  w.homing = def.homing;
  w.bounces = 0;

  // Round-3 additions.
  w.cdBonus = 0.0F;
  w.sweepLead = def.sweepLead;
  w.uniqueHeal = 0.0F;
  w.beamSplit = 0;

  // Create orbit blades if this is an orbit weapon. The blade count tracks
  // w.projectiles + stats_.projAdd so "+1 projectile" upgrades add blades.
  if (w.attackType == AttackType::Orbit && player_ != entt::null && registry_.valid(player_)) {
    syncOrbitBlades(weaponCount_ - 1);
  }
  // Halo evolution: persistent rotating beams, count tracks projectiles.
  if (w.attackType == AttackType::Halo && player_ != entt::null && registry_.valid(player_)) {
    syncHaloBeams(weaponCount_ - 1);
  }
  // Vortex evolution: persistent suction zones, count AND size track
  // projectiles (see syncVortices).
  if (w.attackType == AttackType::Vortex && player_ != entt::null && registry_.valid(player_)) {
    syncVortices(weaponCount_ - 1);
  }
}

void Game::syncOrbitBlades(int slot) {
  if (slot < 0 || slot >= weaponCount_) return;
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& w = weapons_[slot];
  if (w.attackType != AttackType::Orbit) return;

  const int desired = std::max(1, w.projectiles + stats_.projAdd);

  // Collect this slot's blades, remembering the first blade's angle as the
  // rotation phase so the ring keeps its orientation when re-laid out.
  std::vector<entt::entity> blades;
  float phase = 0.0F;
  bool havePhase = false;
  for (const auto e : registry_.view<OrbitBlade>()) {
    auto& ob = registry_.get<OrbitBlade>(e);
    if (ob.weaponIndex == slot) {
      if (!havePhase) {
        phase = ob.angle;
        havePhase = true;
      }
      blades.push_back(e);
    }
  }

  // Grow (or shrink) the ring to the desired blade count.
  while (static_cast<int>(blades.size()) < desired) {
    const auto blade = registry_.create();
    registry_.emplace<Transform>(blade, 0.0F, 0.0F, 0.0F, 0.0F);
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
    ob.angle = 0.0F;
    ob.pierce = w.pierce;
    ob.color = w.color;
    ob.weaponIndex = slot;
    registry_.emplace<OrbitBlade>(blade, ob);
    blades.push_back(blade);
  }
  while (static_cast<int>(blades.size()) > desired) {
    destroyQueue_.push_back(blades.back());
    blades.pop_back();
  }

  // Re-space ALL blades evenly around the circle so a newly added dagger
  // cannot bunch up next to the previous one. The ring phase is snapped to
  // the nearest even multiple of `step` so one blade always sits exactly on
  // the +X axis after a re-sync (predictable ring, plain rotation continues).
  const float step = 2.0F * kPi / static_cast<float>(desired);
  phase = std::round(phase / step) * step;
  const auto& pt = registry_.get<Transform>(player_);
  for (int i = 0; i < desired; ++i) {
    const auto e = blades[static_cast<std::size_t>(i)];
    auto& ob = registry_.get<OrbitBlade>(e);
    ob.angle = phase + static_cast<float>(i) * step;
    auto& t = registry_.get<Transform>(e);
    t.px = pt.x;
    t.py = pt.y;
    t.x = pt.x + std::cos(ob.angle) * w.orbitRadius;
    t.y = pt.y + std::sin(ob.angle) * w.orbitRadius;
  }
}

void Game::syncHaloBeams(int slot) {
  if (slot < 0 || slot >= weaponCount_) return;
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& w = weapons_[slot];
  if (w.attackType != AttackType::Halo) return;

  const int desired = std::max(1, w.projectiles + stats_.projAdd);

  std::vector<entt::entity> beams;
  float phase = 0.0F;
  bool havePhase = false;
  for (const auto e : registry_.view<HaloBeam>()) {
    auto& hb = registry_.get<HaloBeam>(e);
    if (hb.weaponIndex == slot) {
      if (!havePhase) {
        phase = hb.angle;
        havePhase = true;
      }
      beams.push_back(e);
    }
  }

  while (static_cast<int>(beams.size()) < desired) {
    const auto beam = registry_.create();
    registry_.emplace<Transform>(beam, 0.0F, 0.0F, 0.0F, 0.0F);
    registry_.emplace<Radius>(beam, w.beamWidth * 0.5F);
    Sprite s{};
    s.color = w.color;
    s.circle = false;
    registry_.emplace<Sprite>(beam, s);
    HaloBeam hb{};
    hb.damage = w.damage;
    hb.pierce = w.pierce;
    hb.length = w.beamRange;
    hb.width = w.beamWidth;
    hb.angle = 0.0F;
    hb.spin = w.orbitSpeed;
    hb.knockback = w.haloKnockback;
    hb.weaponIndex = slot;
    hb.color = w.color;
    registry_.emplace<HaloBeam>(beam, hb);
    beams.push_back(beam);
  }
  while (static_cast<int>(beams.size()) > desired) {
    destroyQueue_.push_back(beams.back());
    beams.pop_back();
  }

  // Re-space evenly around the player, preserving the ring's phase.
  const float step = 2.0F * kPi / static_cast<float>(desired);
  phase = std::round(phase / step) * step;
  const auto& pt = registry_.get<Transform>(player_);
  for (int i = 0; i < desired; ++i) {
    const auto e = beams[static_cast<std::size_t>(i)];
    auto& hb = registry_.get<HaloBeam>(e);
    hb.angle = phase + static_cast<float>(i) * step;
    auto& t = registry_.get<Transform>(e);
    t.px = pt.x;
    t.py = pt.y;
    t.x = pt.x;
    t.y = pt.y;
  }
}

// The Vortex ring mirrors the Halo ring, except that its COUNT and SIZE both
// come from the projectile stat: every extra projectile adds a whole new
// suction zone AND fattens the existing ones, so the ring thickens as the
// build scales instead of just adding more circles.
void Game::syncVortices(int slot) {
  if (slot < 0 || slot >= weaponCount_) return;
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& w = weapons_[slot];
  if (w.attackType != AttackType::Vortex) return;

  const int desired = std::max(1, w.projectiles + stats_.projAdd);

  std::vector<entt::entity> zones;
  float phase = 0.0F;
  bool havePhase = false;
  for (const auto e : registry_.view<Vortex>()) {
    auto& vx = registry_.get<Vortex>(e);
    if (vx.weaponIndex == slot) {
      if (!havePhase) {
        phase = vx.angle;
        havePhase = true;
      }
      zones.push_back(e);
    }
  }

  // Size growth is deliberately sub-linear and capped. Projectile upgrades
  // should thicken the ring, not turn it into one screen-wide vacuum that holds
  // every late-game boss in place forever.
  const float scale = std::min(1.55F, 1.0F + 0.09F * static_cast<float>(desired - 1));
  const float core = w.vortexRadius * scale;
  const float orbit = w.vortexOrbit * std::min(1.35F, 1.0F + 0.07F * static_cast<float>(desired - 1));

  while (static_cast<int>(zones.size()) < desired) {
    const auto z = registry_.create();
    registry_.emplace<Transform>(z, 0.0F, 0.0F, 0.0F, 0.0F);
    registry_.emplace<Radius>(z, core);
    Sprite s{};
    s.color = w.color;
    s.color.a = 0.22F;
    s.circle = true;
    registry_.emplace<Sprite>(z, s);
    Vortex vx{};
    vx.damage = w.damage;
    vx.pierce = w.pierce;
    vx.radius = core;
    vx.reach = w.vortexReach * scale;
    vx.pull = w.vortexPull;
    vx.orbitRadius = orbit;
    vx.spin = w.vortexOrbitSpeed;
    vx.angle = 0.0F;
    vx.tickRate = w.vortexTickRate;
    vx.tickTimer = 0.0F;
    vx.weaponIndex = slot;
    vx.color = w.color;
    registry_.emplace<Vortex>(z, vx);
    zones.push_back(z);
  }
  while (static_cast<int>(zones.size()) > desired) {
    destroyQueue_.push_back(zones.back());
    zones.pop_back();
  }

  // Re-space evenly around the player, keeping the ring's phase.
  const float step = 2.0F * kPi / static_cast<float>(desired);
  phase = std::round(phase / step) * step;
  const auto& pt = registry_.get<Transform>(player_);
  for (int i = 0; i < desired; ++i) {
    const auto e = zones[static_cast<std::size_t>(i)];
    auto& vx = registry_.get<Vortex>(e);
    vx.angle = phase + static_cast<float>(i) * step;
    vx.radius = core;
    vx.reach = w.vortexReach * scale;
    vx.orbitRadius = orbit;
    if (auto* rr = registry_.try_get<Radius>(e); rr != nullptr) rr->r = core;
    auto& t = registry_.get<Transform>(e);
    t.px = pt.x;
    t.py = pt.y;
    t.x = pt.x;
    t.y = pt.y;
  }
}

void Game::applyWeaponEffect(int slotIndex, std::string_view effect, float value) {
  if (slotIndex < 0 || slotIndex >= weaponCount_) return;
  auto& w = weapons_[slotIndex];
  if (effect == "w_damage_add") {
    w.damage += value;
  } else if (effect == "w_proj_add") {
    w.projectiles += static_cast<int>(value);
    if (w.attackType == AttackType::Orbit) {
      syncOrbitBlades(slotIndex);
    } else if (w.attackType == AttackType::Halo) {
      syncHaloBeams(slotIndex);
    } else if (w.attackType == AttackType::Vortex) {
      syncVortices(slotIndex);
    }
  } else if (effect == "w_pierce_add") {
    w.pierce += static_cast<int>(value);
  } else if (effect == "w_fire_rate") {
    w.cdBonus += value;
  } else if (effect == "w_unique_homing") {
    w.homing = true;
  } else if (effect == "w_unique_area") {
    w.area = value;
  } else if (effect == "w_unique_vortex") {
    w.orbitSpeed *= 2.0F;
    w.orbitRadius *= 1.25F;
  } else if (effect == "w_unique_hearthfire") {
    w.coneRange *= 1.5F;
    w.coneAngle *= 1.4F;
  } else if (effect == "w_unique_cataclysm") {
    w.bombExplodeRadius *= 1.6F;
    w.bombKnockback *= 1.5F;
  } else if (effect == "w_unique_prism") {
    w.beamSplit += static_cast<int>(value);
  } else if (effect == "w_unique_molten") {
    w.zoneDps *= 1.8F;
    w.zoneDuration += 2.0F;
    w.zoneRadius *= 1.25F;
  } else if (effect == "w_unique_thunderlord") {
    w.chainMaxJumps += static_cast<int>(value);
    w.chainDamageMul = 1.0F;
  } else if (effect == "w_unique_supernova") {
    w.novaExpandSpeed *= 1.8F;
    w.novaMaxRadius *= 1.4F;
    w.novaDamagePerTick *= 1.6F;
    w.novaTickRate *= 0.7F;
  } else if (effect == "w_unique_harvest") {
    w.uniqueHeal += value;
  } else if (effect == "w_unique_everflame") {
    w.sweepRadius *= 1.25F;
    w.zoneDuration += 3.0F;
    w.zoneDps *= 1.5F;
  } else if (effect == "w_unique_arcsaw") {
    w.beamWidth *= 1.8F;
    w.damage *= 1.35F;
  } else if (effect == "w_lure_power") {
    // The bell's damage lives in lureDps, not damage, so a plain damage card
    // would be a dead pick on it.
    w.lureDps += value;
    w.lureRadius += value * 0.01F;
  } else if (effect == "w_nova_power") {
    // Same story for the ring: novaDamagePerTick is the real number.
    w.novaDamagePerTick += value;
  } else if (effect == "w_unique_bell") {
    w.lurePull *= 1.35F;
    w.lureRadius *= 1.2F;
    w.lureReach *= 1.2F;
    w.lureMaxBeacons += 1;
  } else if (effect == "w_unique_gravitic") {
    w.chainJumpRange *= 1.5F;
    w.chainDamageMul = std::max(w.chainDamageMul, 0.85F);
  } else if (effect == "w_unique_lash") {
    w.sweepRadius *= 1.35F;
    w.sweepKnockback *= 1.4F;
    w.sweepLead += 0.4F;
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

std::vector<int> Game::collectWeaponGrants() {
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
  std::shuffle(evolutions.begin(), evolutions.end(), rng_);
  std::shuffle(normals.begin(), normals.end(), rng_);
  std::vector<int> out;
  out.reserve(evolutions.size() + normals.size());
  // Evolutions are the marquee offer, so they always lead the list.
  for (const int idx : evolutions) out.push_back(idx);
  for (const int idx : normals) out.push_back(idx);
  return out;
}

float Game::iframeDuration(float base) const {
  // ~1% longer invulnerability per 5 points of defense (defense / 500).
  return base * (1.0F + stats_.defense / 500.0F);
}

void Game::hurtPlayer(float amount) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  // Sandbox immortality: incoming damage is dropped entirely, so a tester can
  // park themselves in a horde and watch a weapon work.
  if (testMode_ && testInvuln_) return;
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
    // The chain takes the hit with you. This is the whole tension of the
    // system: standing in the horde is the only way to keep it fed, and it is
    // also the only way to lose it. (Aura/DoT ticks go through
    // damagePlayerDirect and deliberately do NOT break it — a damage-over-time
    // aura would otherwise zero the meter every frame.)
    breakMomentum();
    if (php.hp <= 0.0F) {
      php.hp = 0.0F;
      state_ = RunState::GameOver;
    }
  }

  // Last Stand unique: taking a hit below 20% HP grants 1s of iframes.
  if (stats_.lastStand != 0 && lastStandCd_ <= 0.0F && php.hp > 0.0F &&
      php.hp < php.max * 0.2F) {
    lastStandCd_ = 20.0F;
    iframes_ = std::max(iframes_, iframeDuration(1.0F));
    const auto& pt = registry_.get<Transform>(player_);
    spawnParticles(pt.x, pt.y, {1.0F, 0.9F, 0.4F, 1.0F}, 16, 5.0F);
  }
}

// Same mitigation/shield path as hurtPlayer, but without the thorns proc.
// Used by continuous damage auras so they don't fire thorns every frame.
void Game::damagePlayerDirect(float amount) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  if (testMode_ && testInvuln_) return;
  auto& php = registry_.get<Health>(player_);
  if (php.hp <= 0.0F) return;
  float dmg = mitigateDamage(amount, stats_.defense);
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
  // Last Stand unique also triggers on aura/DoT damage.
  if (stats_.lastStand != 0 && lastStandCd_ <= 0.0F && php.hp > 0.0F &&
      php.hp < php.max * 0.2F) {
    lastStandCd_ = 20.0F;
    iframes_ = std::max(iframes_, iframeDuration(1.0F));
    const auto& pt = registry_.get<Transform>(player_);
    spawnParticles(pt.x, pt.y, {1.0F, 0.9F, 0.4F, 1.0F}, 16, 5.0F);
  }
}

void Game::updateEnemyShots() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  const auto& pr = registry_.get<Radius>(player_);
  // Enemy fire is part of the world: Stasis slows it like everything else the
  // hostile side owns, so a paused moment is a real reprieve and not a
  // cosmetic trick.
  const float dt = (1.0F / 60.0F) * worldTimeScale_;
  auto view = registry_.view<Transform, Velocity, EnemyShot, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& v = view.get<Velocity>(e);
    auto& es = view.get<EnemyShot>(e);
    const auto& r = view.get<Radius>(e);
    t.px = t.x;
    t.py = t.y;
    es.life -= dt;
    if (es.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }
    t.x += v.x * dt;
    t.y += v.y * dt;
    const float dx = pt.x - t.x;
    const float dy = pt.y - t.y;
    const float hitR = pr.r + r.r;
    if (dx * dx + dy * dy < hitR * hitR) {
      hurtPlayer(es.damage);
      spawnParticles(t.x, t.y, es.color, 5, 3.0F);
      destroyQueue_.push_back(e);
    }
  }
}

void Game::grantXp(float amount) {
  // The sandbox pays out NO experience at all: no XP orbs, no level-ups, no
  // free cards. Everything the sandbox is for (trying a weapon, watching its
  // numbers, maxing a build) works without a single level, and a sandbox that
  // cannot level up cannot be used to farm levels either. XP is a real-run
  // reward and stays that way.
  if (testMode_) return;
  xp_ += amount;
  if (state_ == RunState::Playing && xp_ >= xpNext_) {
    enterLevelUp();
  }
}

void Game::testClearWeapons() {
  weaponCount_ = 0;
  starterChoicePending_ = false; // test sandbox: no opening pick
  // Collect first, destroy after: destroying an entity while an EnTT view over
  // that same component is being walked is undefined behaviour, and an entity
  // can carry several of the types below (an area projectile is also a
  // Projectile), so the list is de-duplicated before anything is removed.
  std::vector<entt::entity> doomed;
  const auto collect = [&doomed](auto&& view) {
    for (const auto e : view) {
      if (std::find(doomed.begin(), doomed.end(), e) == doomed.end()) doomed.push_back(e);
    }
  };
  collect(registry_.view<OrbitBlade>());
  collect(registry_.view<HaloBeam>());
  collect(registry_.view<Vortex>());
  // Drop every live projectile/effect so the eternal orb or in-flight blades
  // never linger into the next test (or the restored run) after a swap.
  collect(registry_.view<Projectile>());
  collect(registry_.view<BombProjectile>());
  collect(registry_.view<BoomerangProjectile>());
  collect(registry_.view<BounceProjectile>());
  collect(registry_.view<BeamEffect>());
  collect(registry_.view<SweepEffect>());
  collect(registry_.view<ZoneEffect>());
  collect(registry_.view<Lure>());
  collect(registry_.view<ChainLightning>());
  collect(registry_.view<NovaRing>());
  // A deferred death queued for one of these entities would double-destroy it
  // later in the frame, so it is dropped from the queue as well.
  destroyQueue_.erase(
      std::remove_if(destroyQueue_.begin(), destroyQueue_.end(),
                     [&doomed](entt::entity e) {
                       return std::find(doomed.begin(), doomed.end(), e) != doomed.end();
                     }),
      destroyQueue_.end());
  for (const auto e : doomed) {
    if (registry_.valid(e)) registry_.destroy(e);
  }
}

void Game::testSetPlayerHp(float hp) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  registry_.get<Health>(player_).hp = hp;
}

void Game::testKillFirstEnemy() {
  auto view = registry_.view<Enemy>();
  for (const auto e : view) {
    // Skip anything already queued for destruction (kills are deferred).
    if (std::find(destroyQueue_.begin(), destroyQueue_.end(), e) != destroyQueue_.end()) {
      continue;
    }
    killEnemy(e);
    return;
  }
}

void Game::enterTestMode() { enterTestModeImpl(); }
void Game::exitTestMode() { exitTestModeImpl(); }
void Game::cycleTestTimeScale() {
  testTimeScale_ = testTimeScale_ >= 20 ? 1 : (testTimeScale_ == 1 ? 4 : (testTimeScale_ == 4 ? 10 : 20));
}
void Game::toggleTestShop() {
  testShopOpen_ = !testShopOpen_;
  if (testShopOpen_) {
    testShopCursor_ = 0;
    testShopScroll_ = 0;
  }
}

void Game::enterTestModeImpl() {
  if (state_ != RunState::Playing) return;
  // Snapshot the ENTIRE run. The sandbox is hermetic: everything that happens
  // inside it is rolled back on exit, so grinding fodder for XP, picking
  // items, fast-forwarding the clock or farming tier kills can never leak back
  // into the real run.
  savedWeaponCount_ = weaponCount_;
  for (int i = 0; i < savedWeaponCount_; ++i) savedWeapons_[i] = weapons_[i];
  // Snapshot EVERY live entity id: whatever is not in this list was born inside
  // the sandbox and is removed again on exit.
  savedEntityIds_.clear();
  for (const auto e : registry_.view<entt::entity>()) savedEntityIds_.push_back(e);
  savedEnemies_.clear();
  savedEnemySpawns_.clear();
  for (const auto e : registry_.view<Enemy>()) {
    if (registry_.try_get<Transform>(e) == nullptr) continue;
    SavedEnemyState s;
    s.entity = e;
    s.transform = registry_.get<Transform>(e);
    if (const auto* v = registry_.try_get<Velocity>(e); v != nullptr) s.velocity = *v;
    if (const auto* r = registry_.try_get<Radius>(e); r != nullptr) s.radius = *r;
    s.health = registry_.get<Health>(e);
    s.enemy = registry_.get<Enemy>(e);
    if (const auto* t = registry_.try_get<EnemyTraits>(e); t != nullptr) s.traits = *t;
    if (const auto* sp = registry_.try_get<Sprite>(e); sp != nullptr) s.sprite = *sp;
    if (const auto* xp = registry_.try_get<Xp>(e); xp != nullptr) s.xp = *xp;
    savedEnemies_.push_back(s);

    // Remember how the enemy was rolled so it can be rebuilt if the sandbox
    // manages to kill it. Killing is irreversible for an entity id.
    SavedSpawnState ss;
    ss.def = registry_.get<Enemy>(e).def;
    ss.tier = registry_.try_get<EnemyTraits>(e) != nullptr
                  ? registry_.get<EnemyTraits>(e).tier
                  : 0;
    ss.hpMul = registry_.get<Health>(e).max;
    const auto& body = registry_.get<Enemy>(e);
    ss.speedMul = body.speed;
    ss.touchMul = body.touch;
    ss.xpMul = registry_.try_get<Xp>(e) != nullptr ? registry_.get<Xp>(e).value : 1.0F;
    if (const auto* t = registry_.try_get<EnemyTraits>(e); t != nullptr) {
      ss.traitFlags = t->flags;
    }
    savedEnemySpawns_.push_back(ss);
  }
  if (player_ != entt::null && registry_.valid(player_)) {
    const auto& t = registry_.get<Transform>(player_);
    savedPlayerX_ = t.x;
    savedPlayerY_ = t.y;
  }
  savedStats_ = stats_;
  savedStacks_ = stacks_;
  savedBestiaryKills_ = bestiaryKills_;
  savedBestiaryTiers_ = bestiaryTiers_;
  savedXp_ = xp_;
  savedXpNext_ = xpNext_;
  savedLevel_ = level_;
  savedKills_ = kills_;
  savedShield_ = shield_;
  savedShieldDelay_ = shieldDelay_;
  savedIframes_ = iframes_;
  savedHealCd_ = healCd_;
  savedSimTime_ = simTime_;
  savedWaves_ = wavesEnabled_;
  savedChoosingStarter_ = choosingStarter_;
  savedStrongestTier_ = strongestKilledTier_;
  savedStrongestDef_ = strongestKilledDef_;
  savedTierKillMask_ = tierKillMask_;
  for (int t = 0; t < 4; ++t) {
    savedTierPressure_[t] = tierPressure_[t];
    savedTierOpen_[t] = tierOpen_[t];
    savedTierGrace_[t] = tierGrace_[t];
  }
  savedTierBanner_ = tierBanner_;
  savedTierBannerT_ = tierBannerT_;
  savedStreak_ = streak_;
  savedStreakTimer_ = streakTimer_;
  for (int i = 0; i < kAbilityCount; ++i) savedAbilityCd_[i] = abilityCd_[i];
  savedStasis_ = stasis_;
  savedWorldTimeScale_ = worldTimeScale_;
  savedPending_ = pending_;
  savedSpawnTimer_ = spawnTimer_;
  savedHordeTimer_ = hordeTimer_;
  savedStarterChoicePending_ = starterChoicePending_;
  savedLastStandCd_ = lastStandCd_;
  savedRetiredTypes_ = retiredTypes_;
  savedRecentTypes_ = recentTypes_;
  savedRerollsUsed_ = rerollsUsed_;
  savedMilestoneOffer_ = milestoneOffer_;
  savedChoices_ = choices_;
  savedPoison_ = poison_;
  savedChainCounter_ = chainCounter_;
  savedBloodKills_ = bloodKills_;
  savedBlackHoleTimer_ = blackHoleTimer_;
  savedAdrenalineCd_ = adrenalineCd_;
  savedAdrenalineActive_ = adrenalineActive_;
  savedBestiaryOpen_ = bestiaryOpen_;
  savedSyncedUnlocks_ = syncedUnlocks_;
  savedProfileDirty_ = profileDirty_;
  savedMoveX_ = moveX_;
  savedMoveY_ = moveY_;
  savedCamX_ = camX_;
  savedCamY_ = camY_;
  savedZoom_ = zoom_;
  savedParticles_ = particles_;
  savedParticleCursor_ = particleCursor_;
  savedRng_ = rng_;
  if (player_ != entt::null && registry_.valid(player_)) {
    const auto& h = registry_.get<Health>(player_);
    savedHp_ = h.hp;
    savedHpMax_ = h.max;
  }
  testBoosted_ = false;
  testInvuln_ = false;
  testTimeScale_ = 1;
  testShopOpen_ = false;
  testShopCursor_ = 0;
  testShopScroll_ = 0;
  testMode_ = true;
  setTestWeapon(0); // starts on the first weapon (wand)
}

void Game::exitTestModeImpl() {
  if (!testMode_) return;
  // Nothing may stay paused in LevelUp when the run takes over again.
  state_ = RunState::Playing;
  testClearWeapons(); // drop the sandbox weapon + its persistent entities
  // The sandbox's own fodder is not part of the real run either: leaving a herd
  // of injected test spawns, XP orbs, drops or effect entities behind would be
  // the last bit of cheat leakage, and the new ones would immediately maul the
  // restored player. Everything that was NOT alive when the sandbox opened is
  // destroyed eagerly (not via destroyQueue_) so no contact tick can land this
  // frame. Ids are collected first: destroying while an EnTT view is being
  // walked is not allowed.
  std::vector<entt::entity> sandboxOnly;
  for (const auto e : registry_.view<entt::entity>()) {
    if (std::find(savedEntityIds_.begin(), savedEntityIds_.end(), e) == savedEntityIds_.end()) {
      sandboxOnly.push_back(e);
    }
  }
  for (const auto e : sandboxOnly) registry_.destroy(e);
  // Deferred deaths queued during the sandbox would otherwise fire against the
  // restored run later on and delete entities the player owns.
  destroyQueue_.clear();
  // Enemies that were on the field when the sandbox opened are put back exactly
  // as they were: same position, HP, tier, traits and pending knockback. One
  // that the sandbox managed to kill is rebuilt from its snapshot.
  for (std::size_t i = 0; i < savedEnemies_.size(); ++i) {
    SavedEnemyState& s = savedEnemies_[i];
    const SavedSpawnState* spawn = i < savedEnemySpawns_.size() ? &savedEnemySpawns_[i] : nullptr;
    if (!registry_.valid(s.entity)) {
      if (spawn == nullptr) continue; // no rebuild data: better gone than a ghost
      const auto fresh = registry_.create();
      registry_.emplace<Transform>(fresh);
      registry_.emplace<Velocity>(fresh);
      registry_.emplace<Radius>(fresh, s.radius);
      registry_.emplace<Health>(fresh, s.health);
      registry_.emplace<Enemy>(fresh, s.enemy);
      registry_.emplace<EnemyTraits>(fresh, s.traits);
      registry_.emplace<Sprite>(fresh, s.sprite);
      registry_.emplace<Xp>(fresh, s.xp);
      s.entity = fresh;
    }
    registry_.emplace_or_replace<Transform>(s.entity, s.transform);
    registry_.emplace_or_replace<Velocity>(s.entity, s.velocity);
    registry_.emplace_or_replace<Radius>(s.entity, s.radius);
    registry_.emplace_or_replace<Health>(s.entity, s.health);
    registry_.emplace_or_replace<Enemy>(s.entity, s.enemy);
    registry_.emplace_or_replace<EnemyTraits>(s.entity, s.traits);
    registry_.emplace_or_replace<Sprite>(s.entity, s.sprite);
    registry_.emplace_or_replace<Xp>(s.entity, s.xp);
  }
  pending_ = savedPending_;
  weaponCount_ = savedWeaponCount_;
  for (int i = 0; i < weaponCount_; ++i) weapons_[i] = savedWeapons_[i];
  stats_ = savedStats_;
  stacks_ = savedStacks_;
  bestiaryKills_ = savedBestiaryKills_;
  bestiaryTiers_ = savedBestiaryTiers_;
  xp_ = savedXp_;
  xpNext_ = savedXpNext_;
  level_ = savedLevel_;
  kills_ = savedKills_;
  shield_ = savedShield_;
  shieldDelay_ = savedShieldDelay_;
  iframes_ = savedIframes_;
  healCd_ = savedHealCd_;
  simTime_ = savedSimTime_;
  spawnTimer_ = savedSpawnTimer_;
  hordeTimer_ = savedHordeTimer_;
  wavesEnabled_ = savedWaves_;
  choosingStarter_ = savedChoosingStarter_;
  starterChoicePending_ = savedStarterChoicePending_;
  strongestKilledTier_ = savedStrongestTier_;
  strongestKilledDef_ = savedStrongestDef_;
  tierKillMask_ = savedTierKillMask_;
  for (int t = 0; t < 4; ++t) {
    tierPressure_[t] = savedTierPressure_[t];
    tierOpen_[t] = savedTierOpen_[t];
    tierGrace_[t] = savedTierGrace_[t];
  }
  tierBanner_ = savedTierBanner_;
  tierBannerT_ = savedTierBannerT_;
  streak_ = savedStreak_;
  streakTimer_ = savedStreakTimer_;
  for (int i = 0; i < kAbilityCount; ++i) abilityCd_[i] = savedAbilityCd_[i];
  stasis_ = savedStasis_;
  worldTimeScale_ = savedWorldTimeScale_;
  momentumDamageMul_ = 1.0F;
  momentumRate_ = 0.0F;
  momentumSpeedMul_ = 1.0F;
  lastStandCd_ = savedLastStandCd_;
  retiredTypes_ = savedRetiredTypes_;
  recentTypes_ = savedRecentTypes_;
  rerollsUsed_ = savedRerollsUsed_;
  milestoneOffer_ = savedMilestoneOffer_;
  choices_ = savedChoices_;
  poison_ = savedPoison_;
  chainCounter_ = savedChainCounter_;
  bloodKills_ = savedBloodKills_;
  blackHoleTimer_ = savedBlackHoleTimer_;
  adrenalineCd_ = savedAdrenalineCd_;
  adrenalineActive_ = savedAdrenalineActive_;
  bestiaryOpen_ = savedBestiaryOpen_;
  syncedUnlocks_ = savedSyncedUnlocks_;
  profileDirty_ = savedProfileDirty_;
  moveX_ = savedMoveX_;
  moveY_ = savedMoveY_;
  camX_ = savedCamX_;
  camY_ = savedCamY_;
  zoom_ = savedZoom_;
  particles_ = savedParticles_;
  particleCursor_ = savedParticleCursor_;
  rng_ = savedRng_;
  if (player_ != entt::null && registry_.valid(player_)) {
    auto& h = registry_.get<Health>(player_);
    h.max = savedHpMax_;
    h.hp = savedHp_;
    auto& t = registry_.get<Transform>(player_);
    t.px = savedPlayerX_;
    t.py = savedPlayerY_;
    t.x = savedPlayerX_;
    t.y = savedPlayerY_;
  }
  testMode_ = false;
  testBoosted_ = false;
  testInvuln_ = false;
  testTimeScale_ = 1;
  testShopOpen_ = false;
  // Rebuild persistent entities for the restored arsenal.
  for (int s = 0; s < weaponCount_; ++s) {
    if (weapons_[s].attackType == AttackType::Orbit) {
      syncOrbitBlades(s);
    } else if (weapons_[s].attackType == AttackType::Halo) {
      syncHaloBeams(s);
    } else if (weapons_[s].attackType == AttackType::Vortex) {
      syncVortices(s);
    }
  }
  // Release the snapshot buffers: a long session that toggles the sandbox a lot
  // should not keep a second copy of every entity alive.
  savedEntityIds_.clear();
  savedEnemies_.clear();
  savedEnemySpawns_.clear();
  savedChoices_.clear();
  savedPending_.clear();
  savedParticles_.clear();

  // Leaving the sandbox ends the run. The snapshot above is what makes the
  // sandbox fair — nothing inside it survives — but a cheat tool that hands the
  // run back untouched is also a cheat tool: max-all weapons, a fast-forwarded
  // clock and an immortal test are exactly the "easy run" a player should not
  // be able to cash in. So the run is restored (so the death screen reports the
  // REAL kills, level and time) and then the player is killed for real.
  if (player_ != entt::null && registry_.valid(player_)) {
    auto& h = registry_.get<Health>(player_);
    h.hp = 0.0F;
    state_ = RunState::GameOver;
    iframes_ = 0.0F;
  }
}

void Game::setTestWeapon(int defIndex) {
  const int n = static_cast<int>(content_.weapons.size());
  if (n <= 0) return;
  testWeaponIdx_ = ((defIndex % n) + n) % n;
  testClearWeapons();
  addWeapon(testWeaponIdx_);
  spawnTestFodder(); // fresh targets so every weapon has something to hit
}

void Game::toggleTestBoost_() {
  if (!testMode_) return;
  if (!testBoosted_) {
    // A buffed build that makes every stat coupling obvious at a glance.
    testBoosted_ = true;
    testBoostBase_ = stats_;
    stats_.damageMul += 1.0F;
    stats_.projAdd += 4;
    stats_.pierceAdd += 3;
    stats_.fireRateBonus += 0.8F;
  } else {
    testBoosted_ = false;
    // Only the boost comes off — items picked in the sandbox stay put.
    stats_ = testBoostBase_;
  }
  for (int s = 0; s < weaponCount_; ++s) {
    if (weapons_[s].attackType == AttackType::Orbit) {
      syncOrbitBlades(s);
    } else if (weapons_[s].attackType == AttackType::Halo) {
      syncHaloBeams(s);
    } else if (weapons_[s].attackType == AttackType::Vortex) {
      syncVortices(s);
    }
  }
}

void Game::spawnTestFodder() {
  if (content_.enemies.empty()) return;
  if (player_ == entt::null || !registry_.valid(player_)) return;
  const auto& pt = registry_.get<Transform>(player_);
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);
  constexpr int kHerd = 10;  for (int m = 0; m < kHerd; ++m) {
    const float angle =
        (static_cast<float>(m) / static_cast<float>(kHerd)) * 2.0F * kPi +
        (unit(rng_) - 0.5F) * 0.5F;
    PendingSpawn pending{};
    pending.x = pt.x + std::cos(angle) * 6.5F;
    pending.y = pt.y + std::sin(angle) * 6.5F;
    pending.t = 0.4F; // short telegraph so the herd rushes in fast
    pending.def = 0;  // cheapest fodder enemy
    pending.hpMul = 1.0F;
    pending.touchMul = 1.0F;
    pending.speedMul = 1.0F;
    pending.xpMul = 1.0F;
    pending.traits = TraitNone;
    pending.tier = 0;
    pending_.push_back(pending);
  }
}

// --- Test sandbox: item picker, immortality, difficulty clock, suicide -----

// Grants one stack of an upgrade. This is the shared "take this card" path: the
// level-up picker uses it, and so does the sandbox's item list — so a tester
// can grab any card at all and see exactly what a real pick would do.
bool Game::grantTestUpgrade(int upgradeIndex) {
  if (upgradeIndex < 0 || static_cast<std::size_t>(upgradeIndex) >= content_.upgrades.size()) {
    return false;
  }
  if (stacks_[static_cast<std::size_t>(upgradeIndex)] >=
      content_.upgrades[static_cast<std::size_t>(upgradeIndex)].maxStacks) {
    return false;
  }
  return applyUpgradeAt(upgradeIndex);
}

// The sandbox "max everything" cheat: keeps granting stacks until every item
// is full. Milestone cards are excluded because they are level-gated rewards,
// not something a tester can meaningfully stack.
void Game::testMaxAllItems() {
  if (!testMode_) return;
  for (int pass = 0; pass < 64; ++pass) {
    bool progressed = false;
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      if (content_.upgrades[i].kind == "milestone") continue;
      progressed |= grantTestUpgrade(static_cast<int>(i));
    }
    if (!progressed) break;
  }
}

void Game::testKillPlayer() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  registry_.get<Health>(player_).hp = 0.0F;
  state_ = RunState::GameOver;
}

void Game::updateTestShop(const FrameInput& input) {
  if (!testMode_ || !testShopOpen_) return;
  const int n = static_cast<int>(content_.upgrades.size());
  if (n <= 0) return;
  if (input.menuUp) {
    testShopCursor_ = (testShopCursor_ - 1 + n) % n;
  } else if (input.menuDown) {
    testShopCursor_ = (testShopCursor_ + 1) % n;
  }
  if (input.menuLeft) {
    testShopCursor_ = (testShopCursor_ - 1 + n) % n;
  } else if (input.menuRight) {
    testShopCursor_ = (testShopCursor_ + 1) % n;
  }
  // Keep the highlighted row inside the visible window.
  if (testShopCursor_ < testShopScroll_) testShopScroll_ = testShopCursor_;
  if (testShopCursor_ >= testShopScroll_ + kTestShopRows) {
    testShopScroll_ = testShopCursor_ - kTestShopRows + 1;
  }
  if (input.menuConfirm) grantTestUpgrade(testShopCursor_);
}

void Game::renderTestShop(core::render::Batcher& b, float px, float py) {
  using core::render::Color;
  const Color panel{0.07F, 0.06F, 0.11F, 0.97F};
  const Color edge{0.45F, 0.35F, 0.85F, 1.0F};
  const Color dim{0.75F, 0.75F, 0.85F, 1.0F};
  const Color hot{1.0F, 0.88F, 0.45F, 1.0F};
  const Color maxed{0.45F, 0.45F, 0.55F, 1.0F};

  const float panelW = std::min(720.0F, px * 0.9F);
  const float panelH = std::min(py * 0.86F, 60.0F + static_cast<float>(kTestShopRows) * 19.0F);
  const float x0 = px * 0.5F - panelW * 0.5F;
  const float y0 = py * 0.5F - panelH * 0.5F;
  b.rectTopLeft(x0, y0, panelW, panelH, panel);
  b.rectTopLeft(x0, y0, panelW, 3.0F, edge);

  const std::string title = "TEST ITEMS - [E] CLOSE   [R] MAX EVERYTHING";
  b.text(x0 + 18.0F, y0 + 14.0F, 2.4F, hot, title);
  const std::string sub = "UP/DOWN SELECT   ENTER TAKE ONE STACK   (ALL CHANGES ARE ROLLED BACK ON EXIT)";
  b.text(x0 + 18.0F, y0 + 36.0F, 1.4F, maxed, sub);

  const float rowH = 19.0F;
  const float listY = y0 + 54.0F;
  for (int r = 0; r < kTestShopRows; ++r) {
    const int idx = testShopScroll_ + r;
    if (idx < 0 || static_cast<std::size_t>(idx) >= content_.upgrades.size()) break;
    const auto& def = content_.upgrades[static_cast<std::size_t>(idx)];
    const int have = stacks_[static_cast<std::size_t>(idx)];
    const bool full = have >= def.maxStacks;
    const bool sel = idx == testShopCursor_;
    const float y = listY + static_cast<float>(r) * rowH;
    if (sel) {
      b.rectTopLeft(x0 + 10.0F, y - 2.0F, panelW - 20.0F, rowH, Color{0.22F, 0.19F, 0.36F, 1.0F});
    }
    const Color col = full ? maxed : (sel ? hot : dim);
    // Weapon-specific Focus cards are only usable while that weapon is armed.
    bool usable = true;
    if (!def.weapon.empty() && findWeaponSlot(def.weapon) < 0) usable = false;
    std::string tag = usable ? "" : " [NOT ARMED]";
    if (full) tag = " [MAX]";
    const std::string label = def.name + "  " + std::to_string(have) + "/" +
                              std::to_string(def.maxStacks) + tag;
    b.text(x0 + 20.0F, y + 1.0F, 1.6F, col, label);
    // A short hint of what the card actually does, right-aligned.
    const std::string hint = def.desc;
    const float hw = b.textWidth(1.2F, hint);
    if (hw < panelW * 0.5F) {
      b.text(x0 + panelW - 20.0F - hw, y + 3.0F, 1.2F,
             usable ? Color{0.55F, 0.55F, 0.65F, 1.0F} : maxed, hint);
    }
  }
}

void Game::testSpawnEnemyAt(float x, float y) {
  const auto e = registry_.create();
  registry_.emplace<Transform>(e, x, y, x, y);
  registry_.emplace<Velocity>(e);
  registry_.emplace<Radius>(e, 0.3F);
  registry_.emplace<Health>(e, 100000.0F, 100000.0F);
  Enemy en{};
  en.speed = 0.0F; // stationary target for deterministic assertions
  en.touch = 0.0F; // no contact damage
  registry_.emplace<Enemy>(e, en);
  Sprite s{};
  s.color = {1.0F, 0.25F, 0.25F, 1.0F};
  s.circle = true;
  registry_.emplace<Sprite>(e, s);
  registry_.emplace<Xp>(e, 1.0F);
}

void Game::testSpawnTieredEnemyAt(float x, float y, int tier, std::uint32_t traits,
                                  int def) {
  const int t = std::clamp(tier, 0, 3);
  const int d = std::clamp(def, 0, static_cast<int>(content_.enemies.size()) - 1);
  const TierBuffs buffs = tierBuffs(t);
  PendingSpawn p{};
  p.x = x;
  p.y = y;
  p.t = 0.0F;
  p.def = d;
  p.hpMul = rollTierHpMul(buffs, rng_);
  p.touchMul = buffs.touch;
  p.speedMul = buffs.speed;
  p.xpMul = buffs.xp;
  p.traits = traits;
  p.tier = static_cast<std::uint8_t>(t);
  spawnEnemy(p);
}

std::vector<int> Game::testEnemyTiers() const {
  std::vector<int> out;
  auto view = registry_.view<Enemy, EnemyTraits>();
  for (const auto e : view) {
    out.push_back(static_cast<int>(view.get<EnemyTraits>(e).tier));
  }
  return out;
}

std::vector<int> Game::testEnemyTraitCounts() const {
  std::vector<int> out;
  auto view = registry_.view<Enemy, EnemyTraits>();
  for (const auto e : view) {
    std::uint32_t f = view.get<EnemyTraits>(e).flags;
    int c = 0;
    while (f != 0u) {
      c += static_cast<int>(f & 1u);
      f >>= 1u;
    }
    out.push_back(c);
  }
  return out;
}

Game::DebugCounts Game::debugCounts() const {
  DebugCounts c;
  c.projectiles = registry_.view<Projectile>().size();
  c.orbitBlades = registry_.view<OrbitBlade>().size();
  c.halos = registry_.view<HaloBeam>().size();
  c.bombs = registry_.view<BombProjectile>().size();
  c.boomerangs = registry_.view<BoomerangProjectile>().size();
  c.bounces = registry_.view<BounceProjectile>().size();
  c.beams = registry_.view<BeamEffect>().size();
  c.sweeps = registry_.view<SweepEffect>().size();
  c.zones = registry_.view<ZoneEffect>().size();
  c.chains = registry_.view<ChainLightning>().size();
  c.novas = registry_.view<NovaRing>().size();
  c.vortices = registry_.view<Vortex>().size();
  c.lures = registry_.view<Lure>().size();
  return c;
}

float Game::testFirstEnemyHp() const {
  auto view = registry_.view<Health, Enemy>();
  for (const auto e : view) {
    return view.get<Health>(e).hp;
  }
  return -1.0F;
}

void Game::testSetFirstEnemyHp(float hp) {
  auto view = registry_.view<Health, Enemy>();
  for (const auto e : view) {
    view.get<Health>(e).hp = hp;
    return;
  }
}

void Game::testSetFirstEnemyKnockbackRes(float res) {
  auto view = registry_.view<Enemy>();
  for (const auto e : view) {
    if (auto* t = registry_.try_get<EnemyTraits>(e); t != nullptr) {
      t->knockbackRes = std::clamp(res, 0.0F, 1.0F);
    } else {
      EnemyTraits fresh{};
      fresh.knockbackRes = std::clamp(res, 0.0F, 1.0F);
      registry_.emplace<EnemyTraits>(e, fresh);
    }
    return;
  }
}

void Game::testDamageFirstEnemy(float dmg) {
  auto view = registry_.view<Health, Enemy>();
  for (const auto e : view) {
    applyEnemyDamage(e, dmg);
    return;
  }
}

float Game::testFirstEnemyDistToPlayer() const {
  if (player_ == entt::null || !registry_.valid(player_)) return -1.0F;
  const auto& pt = registry_.get<Transform>(player_);
  auto view = registry_.view<Transform, Enemy>();
  for (const auto e : view) {
    const auto& t = view.get<Transform>(e);
    const float dx = t.x - pt.x;
    const float dy = t.y - pt.y;
    return std::sqrt(dx * dx + dy * dy);
  }
  return -1.0F;
}

float Game::testFirstEnemyDistToVortex() const {
  auto enemyView = registry_.view<Transform, Enemy>();
  for (const auto en : enemyView) {
    const auto& et = registry_.get<Transform>(en);
    float best = -1.0F;
    for (const auto vz : registry_.view<Transform, Vortex>()) {
      const auto& zt = registry_.get<Transform>(vz);
      const float dx = et.x - zt.x;
      const float dy = et.y - zt.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (best < 0.0F || d < best) best = d;
    }
    return best;
  }
  return -1.0F;
}

float Game::testFirstEnemyDistToLure() const {
  auto enemyView = registry_.view<Transform, Enemy>();
  for (const auto en : enemyView) {
    const auto& et = registry_.get<Transform>(en);
    float best = -1.0F;
    for (const auto lb : registry_.view<Transform, Lure>()) {
      const auto& lt = registry_.get<Transform>(lb);
      const float dx = et.x - lt.x;
      const float dy = et.y - lt.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (best < 0.0F || d < best) best = d;
    }
    return best;
  }
  return -1.0F;
}

float Game::testPlayerX() const {
  if (player_ == entt::null || !registry_.valid(player_)) return 0.0F;
  return registry_.get<Transform>(player_).x;
}

float Game::testPlayerY() const {
  if (player_ == entt::null || !registry_.valid(player_)) return 0.0F;
  return registry_.get<Transform>(player_).y;
}

std::vector<float> Game::testBeamAngles() const {
  std::vector<float> out;
  for (const auto e : registry_.view<BeamEffect>()) {
    const auto& be = registry_.get<BeamEffect>(e);
    out.push_back(std::atan2(be.endY - be.startY, be.endX - be.startX));
  }
  return out;
}

std::size_t Game::debugEnemyCount() const {
  return registry_.view<Enemy>().size();
}

float Game::testFirstAuraRadius() const {
  for (const auto e : registry_.view<EnemyTraits>()) {
    const auto& t = registry_.get<EnemyTraits>(e);
    if (t.auraRadius > 0.0F) return t.auraRadius;
  }
  return 0.0F;
}

float Game::testFirstAuraDps() const {
  for (const auto e : registry_.view<EnemyTraits>()) {
    const auto& t = registry_.get<EnemyTraits>(e);
    if (t.auraRadius > 0.0F) return t.auraDps;
  }
  return 0.0F;
}

bool Game::testFirstCanShoot() const {
  for (const auto e : registry_.view<EnemyTraits>()) {
    const auto& t = registry_.get<EnemyTraits>(e);
    if (t.shootCooldown > 0.0F) return true;
  }
  return false;
}

std::vector<float> Game::testEnemySpeeds() const {
  std::vector<float> out;
  auto view = registry_.view<Transform, Velocity, Enemy>();
  out.reserve(enemyCount());
  for (const auto e : view) {
    const auto& v = view.get<Velocity>(e);
    out.push_back(std::sqrt(v.x * v.x + v.y * v.y));
  }
  return out;
}

std::vector<std::string> Game::armedWeaponIds() const {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(weaponCount_));
  for (int i = 0; i < weaponCount_; ++i) {
    out.push_back(content_.weapons[static_cast<std::size_t>(weapons_[i].def)].id);
  }
  return out;
}

std::vector<float> Game::testEnemyHps() const {
  std::vector<float> out;
  auto view = registry_.view<Health, Enemy>();
  for (const auto e : view) {
    out.push_back(view.get<Health>(e).hp);
  }
  return out;
}

std::vector<float> Game::testBounceRadii() const {
  std::vector<float> out;
  auto view = registry_.view<BounceProjectile, Radius>();
  for (const auto e : view) {
    out.push_back(view.get<Radius>(e).r);
  }
  return out;
}

float Game::testOrbitBladeAngle() const {
  for (const auto e : registry_.view<OrbitBlade>()) {
    return registry_.get<OrbitBlade>(e).angle;
  }
  return -1.0F;
}

std::vector<float> Game::testOrbitBladeGaps(int slot) const {
  std::vector<float> angles;
  for (const auto e : registry_.view<OrbitBlade>()) {
    const auto& ob = registry_.get<OrbitBlade>(e);
    if (ob.weaponIndex == slot) angles.push_back(ob.angle);
  }
  std::sort(angles.begin(), angles.end());
  std::vector<float> gaps;
  if (angles.size() < 2) return gaps;
  for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
    gaps.push_back(angles[i + 1] - angles[i]);
  }
  // Wrap-around gap between the last and first blade.
  gaps.push_back(angles.front() + 2.0F * kPi - angles.back());
  return gaps;
}


std::vector<std::string> wrapWords(std::string_view str, std::size_t maxChars) {
  std::vector<std::string> lines;
  if (maxChars == 0) maxChars = 1;
  std::string current;
  std::string_view rest(str);
  while (!rest.empty()) {
    const std::size_t space = rest.find(' ');
    const std::string_view word = rest.substr(0, space);
    if (space != std::string_view::npos) rest.remove_prefix(space + 1);
    else rest = std::string_view();
    if (current.empty()) {
      current = std::string(word);
    } else if (current.size() + 1 + word.size() > maxChars) {
      lines.push_back(current);
      current = std::string(word);
    } else {
      current += ' ';
      current.append(word);
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

// Greedy word-wrap: renders str in lines that fit maxWidth pixels.
static void renderWrappedText(core::render::Batcher& b, float x, float y,
                              float scale, core::render::Color c,
                              std::string_view str, float maxWidth,
                              float lineHeight) {
  // The 5x7 bitmap font advances 6*scale pixels per character, so the pixel
  // limit maps 1:1 onto a character limit (textWidth() is size()*6*scale).
  const std::size_t maxChars =
      maxWidth > 0.0F ? static_cast<std::size_t>(maxWidth / (6.0F * scale))
                      : str.size();
  for (const auto& line : wrapWords(str, maxChars)) {
    b.text(x, y, scale, c, line);
    y += lineHeight;
  }
}

// Human-readable labels for attack patterns (ASCII only - font is 32..96).
static const char* attackTypeName(AttackType t) {
  switch (t) {
    case AttackType::Projectile: return "shot";
    case AttackType::Orbit: return "orbit";
    case AttackType::Cone: return "cone";
    case AttackType::Bomb: return "bomb";
    case AttackType::Boomerang: return "boomerang";
    case AttackType::Bounce: return "bounce";
    case AttackType::Beam: return "beam";
    case AttackType::Sweep: return "sweep";
    case AttackType::Zone: return "zone";
    case AttackType::Chain: return "chain";
    case AttackType::Nova: return "nova";
    case AttackType::Inferno: return "inferno";
    case AttackType::Pulsar: return "pulsar";
    case AttackType::Halo: return "halo";
    case AttackType::Vortex: return "vortex";
    case AttackType::Prism: return "prism";
    case AttackType::Lure: return "beacon";
  }
  return "?";
}

static std::string fit1(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
  return buf;
}

// Character sheet shown when the run is paused (ESC).
void Game::renderPlayerStats(core::render::Batcher& b, float px, float py) {
  const core::render::Color gold{1.0F, 0.85F, 0.4F, 1.0F};
  const core::render::Color dim{0.72F, 0.72F, 0.82F, 1.0F};
  const core::render::Color teal{0.4F, 0.9F, 0.9F, 1.0F};

  std::vector<std::string> rows;
  rows.push_back(std::string(kGameName) + "   LV " + std::to_string(level_) + "      XP " +
                 std::to_string(static_cast<int>(xp_)) + "/" +
                 std::to_string(static_cast<int>(xpNext_)));
  rows.push_back("TIME " + std::to_string(static_cast<int>(simTime_)) + "S     KILLS " +
                 std::to_string(kills_));
  float hpNow = 0.0F;
  float hpMax = stats_.maxHp;
  if (player_ != entt::null && registry_.valid(player_)) {
    const auto& h = registry_.get<Health>(player_);
    hpNow = h.hp;
    hpMax = h.max;
  }
  rows.push_back("HP " + std::to_string(static_cast<int>(hpNow)) + "/" +
                 std::to_string(static_cast<int>(hpMax)) + "    REGEN " +
                 fit1(stats_.regen) + "/S");
  rows.push_back("SHIELD " + std::to_string(static_cast<int>(shield_)) + "/" +
                 std::to_string(static_cast<int>(stats_.shieldMax)) + "  DEFENSE " +
                 std::to_string(static_cast<int>(stats_.defense)));
  rows.push_back("DAMAGE X" + fit1(stats_.damageMul) + "    FIRE RATE +" +
                 std::to_string(static_cast<int>(stats_.fireRateBonus * 100.0F)) + "%");
  rows.push_back("CHAIN x" + std::to_string(streak_) + "/" +
                 std::to_string(std::max(1, stats_.momentumMax)) + "  +" +
                 std::to_string(static_cast<int>(stats_.momentumDamage)) + "% DMG/STACK  +" +
                 std::to_string(static_cast<int>(stats_.momentumRate)) + "% RATE/STACK");
  std::string speedRow = "SPEED X" + fit1(stats_.speedMul) + "     PICKUP X" +
                         fit1(stats_.pickupMul);
  if (stats_.lifesteal > 0.0F) {
    // Lifesteal rolls per KILL, not per hit — label it so that is not a
    // surprise: "+12% per kill".
    speedRow += "   LIFE " + std::to_string(static_cast<int>(stats_.lifesteal)) + "%/KILL";
    if (stats_.lifestealHeal >= 2) speedRow += " X2";
  }
  rows.push_back(speedRow);
  rows.push_back("PROJECTILES +" + std::to_string(stats_.projAdd) + "   PIERCE +" +
                 std::to_string(stats_.pierceAdd));
  {
    // The three buttons, with what the build has done to them. Printed even at
    // their base numbers: they are always available, and a player who has never
    // noticed them should find the reminder where the rest of the build lives.
    const auto secs = [](float v) { return fit1(v); };
    std::string abil = "J DASH " + secs(stats_.blinkDist) + "U   K BURST " +
                       std::to_string(static_cast<int>(stats_.burstDamage)) + "/" +
                       secs(stats_.burstRadius) + "U   L STASIS " + secs(stats_.stasisDuration) +
                       "S X" + std::to_string(static_cast<int>(stats_.stasisSlow * 100.0F)) + "%";
    if (stats_.abilityCdMul < 0.999F) {
      abil += "   CD X" + std::to_string(static_cast<int>(stats_.abilityCdMul * 100.0F)) + "%";
    }
    if (stats_.abilityEcho != 0) abil += "  ECHO";
    rows.push_back(abil);
  }
  if (stats_.armorPierce > 0.0F) {
    rows.push_back("ARMOR PIERCE " + std::to_string(static_cast<int>(stats_.armorPierce)));
  }
  if (stats_.extraChoice > 0) {
    rows.push_back("+1 CARD PER LEVEL-UP");
  }
  if (stats_.rerollCharges > 1) {
    rows.push_back("+1 REROLL PER LEVEL-UP");
  }
  if (stats_.weaponSlots > 0) {
    rows.push_back("ARSENAL CORE X" + std::to_string(stats_.weaponSlots) +
                   "  (+" + std::to_string(stats_.weaponSlots) + " SLOT)");
  }
  // What the player is currently wearing, so the persistent cosmetics are
  // discoverable from inside a run (and not only on the main menu).
  if (profile_ != nullptr) {
    const auto& skins = skinPalette();
    const auto& outlines = outlinePalette();
    const int skin = std::clamp(profile_->skin, 0, static_cast<int>(skins.size()) - 1);
    const bool outlined = profile_->canUseOutline(profile_->outline) &&
                          profile_->outline < static_cast<int>(outlines.size());
    rows.push_back(std::string("SKIN ") + skins[static_cast<std::size_t>(skin)].name +
                   "   OUTLINE " +
                   (outlined ? outlines[static_cast<std::size_t>(profile_->outline)].name
                             : "NONE"));
  }

  rows.push_back(""); // spacer
  rows.push_back("WEAPONS " + std::to_string(weaponCount_) + "/" +
                 std::to_string(weaponCap()) + ":");
  for (int i = 0; i < weaponCount_; ++i) {
    const auto& w = weapons_[i];
    const auto& def = content_.weapons[static_cast<std::size_t>(w.def)];
    rows.push_back(def.name + " [" + attackTypeName(w.attackType) + "] D" +
                   std::to_string(static_cast<int>(w.damage)) + " N" +
                   std::to_string(w.projectiles) + " CD" + fit1(w.cooldown) + "S");
  }

  const float lineH = 17.0F;
  const float padX = 24.0F;
  const float panelW = 560.0F;
  const float panelX = px * 0.5F - panelW * 0.5F;
  const float panelY = 40.0F;

  // Measure the content height first so the backdrop fits, then draw text.
  float yEnd = panelY + 52.0F;
  for (const auto& r : rows) {
    yEnd += r.empty() ? 10.0F : lineH;
  }
  const float panelH = std::min((yEnd - panelY) + 22.0F, py - 70.0F);
  b.rectTopLeft(panelX, panelY, panelW, panelH, core::render::Color{0.08F, 0.07F, 0.12F, 0.92F});
  b.rectTopLeft(panelX, panelY, panelW, 4.0F, teal);
  const std::string title = "PLAYER - PAUSED  [ESC] RESUME  [B] BESTIARY";
  b.text(panelX + padX, panelY + 14.0F, 3.0F, gold, title);

  float y = panelY + 52.0F;
  for (const auto& r : rows) {
    if (r.empty()) {
      y += 10.0F;
      continue;
    }
    b.text(panelX + padX, y, 2.0F, dim, r);
    y += lineH;
  }
}

void Game::renderBestiary(core::render::Batcher& b, float px, float py) {
  using core::render::Color;
  const Color gold{1.0F, 0.85F, 0.4F, 1.0F};
  const Color dim{0.72F, 0.72F, 0.82F, 1.0F};
  const Color white{1.0F, 1.0F, 1.0F, 1.0F};
  const Color violet{0.75F, 0.5F, 1.0F, 1.0F};

  b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.05F, 0.04F, 0.09F, 0.94F});
  const std::string title = "BESTIARY   [B] BACK   [ESC] RESUME";
  b.text(px * 0.5F - b.textWidth(3.0F, title) * 0.5F, 18.0F, 3.0F, gold, title);

  float hpS = 1.0F;
  float spS = 1.0F;
  float tchS = 1.0F;
  currentScales(hpS, spS, tchS);
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "CURRENT ENEMY SCALING: HP x%.1f   SPEED x%.2f   DAMAGE x%.2f",
                static_cast<double>(hpS), static_cast<double>(spS), static_cast<double>(tchS));
  b.text(px * 0.5F - b.textWidth(1.8F, buf) * 0.5F, 54.0F, 1.8F, violet, buf);

  // Strongest enemy killed this run (the headline the player cares about).
  {
    const char* tierName = "NONE";
    Color tierCol = dim;
    switch (strongestKilledTier_) {
      case 3: tierName = "OVERLORD"; tierCol = Color{0.85F, 0.35F, 1.0F, 1.0F}; break;
      case 2: tierName = "CHAMPION"; tierCol = Color{1.0F, 0.45F, 0.10F, 1.0F}; break;
      case 1: tierName = "ELITE";    tierCol = Color{1.0F, 0.85F, 0.20F, 1.0F}; break;
      default: break;
    }
    std::string line = "STRONGEST KILL THIS RUN: ";
    line += tierName;
    if (strongestKilledDef_ >= 0 &&
        static_cast<std::size_t>(strongestKilledDef_) < content_.enemies.size()) {
      line += " - " + content_.enemies[static_cast<std::size_t>(strongestKilledDef_)].name;
    }
    b.text(px * 0.5F - b.textWidth(2.0F, line) * 0.5F, 74.0F, 2.0F, tierCol, line);
  }

  // --- TRIBUNALS --------------------------------------------------------------
  // The three elite+ tiers are the game's "tribunals": each one escalates how
  // many random abilities a spawn rolls, on top of its fixed stat buffs. This
  // panel is the reference the player reads before choosing between an
  // armour-pierce build (armour) and a lifesteal build (life-steal resist).
  const Color eliteGold{1.0F, 0.85F, 0.20F, 1.0F};
  const Color champOrange{1.0F, 0.45F, 0.10F, 1.0F};
  const Color overlordViolet{0.85F, 0.35F, 1.0F, 1.0F};
  const Color statBlue{0.60F, 0.78F, 0.92F, 1.0F};
  const Color resOrange{1.0F, 0.72F, 0.45F, 1.0F};

  // Display names for the shared trait pool, in PickTrait order.
  static const char* const kTraitNames[] = {
      "FAST x1.7",  "ARMORED x2.5 HP", "REGEN",     "EXPLOSIVE", "VENOM",
      "VAMPIRIC",  "SHIELDED",        "HEAVY x2 DMG", "SHOOT",   "DAMAGE AURA",
      "RESIST +40 DEF",
  };

  struct Tribunal {
    int tier;
    const char* name;
    const char* roman;
    const char* gate; // how this tier is unlocked
    Color color;
  };
  const Tribunal tribunals[] = {
      {1, "ELITE",    "I",   "FROM 45s",            eliteGold},
      {2, "CHAMPION", "II",  "WHEN ELITES ARE EASY", champOrange},
      {3, "OVERLORD", "III", "WHEN CHAMPIONS ARE",  overlordViolet},
  };

  b.text(24.0F, 100.0F, 1.8F, gold, "TRIBUNALS");
  const float tribW = (px - 48.0F) / 3.0F;
  const float tribY = 122.0F;
  const float tribH = 112.0F;
  for (std::size_t ti = 0; ti < 3; ++ti) {
    const auto& tb = tribunals[ti];
    const float x = 24.0F + static_cast<float>(ti) * tribW;
    const bool slain = (tierKillMask_ & (1u << tb.tier)) != 0u;
    const bool open = tierUnlocked(tb.tier);
    // Panel backdrop; a slain tribunal gets a brighter border strip so the
    // player can see at a glance which ones they have actually faced.
    b.rectTopLeft(x, tribY, tribW - 10.0F, tribH,
                  Color{tb.color.r, tb.color.g, tb.color.b, slain ? 0.13F : 0.06F});
    b.rectTopLeft(x, tribY, 4.0F, tribH, tb.color);

    b.text(x + 14.0F, tribY + 8.0F, 1.9F, tb.color,
           std::string(tb.roman) + ". " + tb.name);
    const int traitCount = traitsForTier(tb.tier, simTime_);
    const TierBuffs buffs = tierBuffs(tb.tier);
    std::snprintf(buf, sizeof(buf), "%d TRAIT%s   XP x%.0f   %s",
                  traitCount, traitCount == 1 ? "" : "S",
                  static_cast<double>(buffs.xp), tb.gate);
    b.text(x + 14.0F, tribY + 26.0F, 1.25F, dim, buf);
    // Live gate status: closed tiers show how far the player is from opening
    // them, which is the whole point of the adaptive director.
    if (tb.tier >= 2) {
      const float have = tierPressure_[tb.tier - 1];
      const float need = tb.tier == 2 ? kChampionPressure : kOverlordPressure;
      const int pct = static_cast<int>(std::clamp(have / need, 0.0F, 1.0F) * 100.0F);
      std::snprintf(buf, sizeof(buf), "%s  %d%%", open ? "OPEN" : "LOCKED", pct);
      b.text(x + 14.0F, tribY + 40.0F, 1.25F,
             open ? tb.color : Color{dim.r, dim.g, dim.b, 0.7F}, buf);
    }

    std::snprintf(buf, sizeof(buf), "HP x%.0f-%.0f  DMG x%.1f  SPD x%.2f  DEF %d",
                  static_cast<double>(buffs.hpMin), static_cast<double>(buffs.hpMax),
                  static_cast<double>(buffs.touch), static_cast<double>(buffs.speed),
                  static_cast<int>(enemyDefense(simTime_, tb.tier)));
    b.text(x + 14.0F, tribY + 40.0F, 1.25F, statBlue, buf);

    // Resistances at the current run time (they grow with it, so this is the
    // live number, not a constant).
    std::snprintf(buf, sizeof(buf), "LIFE RES %d%%  KB RES %d%%  (PIERCE %d)",
                  static_cast<int>(enemyLifestealResistance(simTime_, tb.tier, false) * 100.0F),
                  static_cast<int>(enemyKnockbackResistance(simTime_, tb.tier, false) * 100.0F),
                  static_cast<int>(stats_.armorPierce));
    b.text(x + 14.0F, tribY + 54.0F, 1.25F, resOrange, buf);

    // The full ability pool, word-wrapped, with a "(rolls N)" note so the
    // player knows how many they will actually get.
    std::string pool;
    for (const char* n : kTraitNames) {
      if (!pool.empty()) pool += "  ";
      pool += n;
    }
    const std::vector<std::string> lines = wrapWords(pool, 40);
    float ly = tribY + 68.0F;
    for (std::size_t li = 0; li < lines.size() && li < 3; ++li) {
      b.text(x + 14.0F, ly, 1.15F, violet, lines[li]);
      ly += 12.0F;
    }

    // Slain badge, bottom-right of the panel.
    b.text(x + tribW - 78.0F, tribY + tribH - 16.0F, 1.3F,
           slain ? tb.color : Color{0.4F, 0.4F, 0.45F, 1.0F},
           slain ? "SLAIN" : "UNFACED");
  }

  // --- Discovered enemy types --------------------------------------------------
  b.text(24.0F, tribY + tribH + 12.0F, 1.8F, gold, "ENEMIES");

  std::vector<int> found;
  for (std::size_t i = 0; i < bestiaryKills_.size(); ++i) {
    if (bestiaryKills_[i] > 0) found.push_back(static_cast<int>(i));
  }
  if (found.empty()) {
    const std::string none = "NO ENEMIES SLAIN YET";
    b.text(px * 0.5F - b.textWidth(2.5F, none) * 0.5F, tribY + tribH + 50.0F, 2.5F, dim, none);
    return;
  }

  const float startX = 24.0F;
  const float startY = tribY + tribH + 32.0F;
  // Four lines per entry: name/kills, base stats, live-scaled stats with
  // defense, and the resistance line the player needs for build planning.
  const float rowH = 62.0F;
  // How many rows actually fit between the list header and the bottom of the
  // window. Deriving it from the viewport (instead of hardcoding 6) keeps every
  // discovered type reachable on short windows, and lets tall ones show more.
  const float listBottom = py - 46.0F; // room for the "+N more" overflow note
  const int perCol = std::clamp(static_cast<int>((listBottom - startY) / rowH), 2, 10);
  const bool twoCols = found.size() > static_cast<std::size_t>(perCol);
  const int colCount = twoCols ? 2 : 1;
  const float colW = twoCols ? (px - 60.0F) * 0.5F : px - 60.0F;
  const std::size_t shown =
      std::min<std::size_t>(found.size(), static_cast<std::size_t>(perCol * colCount));

  for (std::size_t fi = 0; fi < shown; ++fi) {
    const int col = static_cast<int>(fi) / perCol;
    const int row = static_cast<int>(fi) % perCol;
    const int idx = found[fi];
    const auto& def = content_.enemies[static_cast<std::size_t>(idx)];
    const float x = startX + static_cast<float>(col) * colW;
    const float y = startY + static_cast<float>(row) * rowH;

    // Appearance swatch: matches the enemy's in-world shape and colour.
    Color swatch = def.color;
    swatch.a = 1.0F;
    if (def.circle) {
      b.circle(x + 16.0F, y + 16.0F, 12.0F, swatch);
    } else {
      b.rectTopLeft(x + 4.0F, y + 4.0F, 24.0F, 24.0F, swatch);
    }

    b.text(x + 40.0F, y, 1.9F, white, def.name);
    // Base stats, including armor-relevant defense at tier 0 and the radius.
    std::snprintf(buf, sizeof(buf), "HP %d  SPD %.1f  DMG %d  XP %d  R %.2f",
                  static_cast<int>(def.hp), static_cast<double>(def.speed),
                  static_cast<int>(def.touch), static_cast<int>(def.xp),
                  static_cast<double>(def.radius));
    b.text(x + 40.0F, y + 18.0F, 1.3F, dim, buf);
    // Live-scaled stats (what this type actually is right now).
    std::snprintf(buf, sizeof(buf), "NOW  HP %d  DMG %d  ARMOR %d",
                  static_cast<int>(def.hp * hpS), static_cast<int>(def.touch * tchS),
                  static_cast<int>(enemyDefense(simTime_, 0)));
    b.text(x + 40.0F, y + 31.0F, 1.3F, statBlue, buf);

    // Resistances + how much of the armor the player's pierce currently
    // removes (the actionable number for a lifesteal vs. pierce build).
    std::snprintf(buf, sizeof(buf),
                  "LIFE RES %d%%   KB RES %d%%   PIERCE IGNORES %d",
                  static_cast<int>(enemyLifestealResistance(simTime_, 0, false) * 100.0F),
                  static_cast<int>(enemyKnockbackResistance(simTime_, 0, false) * 100.0F),
                  static_cast<int>(stats_.armorPierce));
    b.text(x + 40.0F, y + 44.0F, 1.3F, resOrange, buf);

    std::snprintf(buf, sizeof(buf), "KILLS %d", bestiaryKills_[static_cast<std::size_t>(idx)]);
    b.text(x + colW - 92.0F, y, 1.4F, gold, buf);

    // Tier badges: only the elite+ variants actually slain are lit.
    const std::uint8_t tiers = bestiaryTiers_[static_cast<std::size_t>(idx)];
    float bx = x + colW - 84.0F;
    const float by = y + 18.0F;
    if ((tiers & (1u << 1)) != 0u) { b.text(bx, by, 1.6F, eliteGold, "E"); bx += 18.0F; }
    if ((tiers & (1u << 2)) != 0u) { b.text(bx, by, 1.6F, champOrange, "C"); bx += 18.0F; }
    if ((tiers & (1u << 3)) != 0u) { b.text(bx, by, 1.6F, overlordViolet, "O"); }
  }

  // Never let a discovered type vanish without saying so: if the list had to
  // stop early (a very short window with many types), show how many are hidden
  // rather than silently dropping them.
  if (shown < found.size()) {
    const std::string more = "+" + std::to_string(found.size() - shown) +
                             " MORE DISCOVERED (RESIZE THE WINDOW TO SEE THEM)";
    b.text(startX, startY + static_cast<float>(perCol) * rowH + 6.0F, 1.5F, dim, more);
  }
}

void Game::renderMainMenu(core::render::Batcher& b, float px, float py) {
  using core::render::Color;
  // Opaque backdrop: the menu is the app's first screen, not a pause blit.
  b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.06F, 0.04F, 0.10F, 1.0F});

  const Color white{1.0F, 1.0F, 1.0F, 1.0F};
  const Color gold{1.0F, 0.85F, 0.35F, 1.0F};
  const Color dim{0.62F, 0.62F, 0.72F, 1.0F};
  const Color violet{0.75F, 0.5F, 1.0F, 1.0F};

  // --- Title block ------------------------------------------------------------
  const float titleScale = 7.0F;
  b.text(px * 0.5F - b.textWidth(titleScale, kGameName) * 0.5F, py * 0.13F, titleScale,
         gold, kGameName);
  // No tagline: the title is the whole brand. The slot stays so a future
  // subtitle only has to set kGameSubtitle.
  if (kGameSubtitle[0] != '\0') {
    b.text(px * 0.5F - b.textWidth(1.8F, kGameSubtitle) * 0.5F, py * 0.13F + 62.0F, 1.8F,
           violet, kGameSubtitle);
  }

  // A thin rule under the title, drawn as a row of dots (screen space: px).
  {
    const float ruleW = std::min(px * 0.55F, 520.0F);
    const int n = 60;
    for (int k = 0; k <= n; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(n);
      b.circle(px * 0.5F - ruleW * 0.5F + ruleW * t, py * 0.13F + 88.0F, 1.5F,
               Color{gold.r, gold.g, gold.b, 0.35F});
    }
  }

  // --- Player preview (skin + outline) ---------------------------------------
  // Draws the actual player colours so the menu is an honest preview of the
  // run you are about to start.
  {
    const float cx = px * 0.5F;
    const float cy = py * 0.40F;
    Color body{0.55F, 0.85F, 1.0F, 1.0F};
    Color ring{0.0F, 0.0F, 0.0F, 0.0F};
    if (profile_ != nullptr) {
      const auto& skins = skinPalette();
      const int skin = std::clamp(profile_->skin, 0, static_cast<int>(skins.size()) - 1);
      body = skins[static_cast<std::size_t>(skin)].color;
      if (profile_->outline < static_cast<int>(outlinePalette().size()) &&
          profile_->canUseOutline(profile_->outline)) {
        ring = outlinePalette()[static_cast<std::size_t>(profile_->outline)].color;
      }
    }
    // NOTE: this whole overlay is drawn in SCREEN space, so every radius here
    // is in PIXELS, not world units. The preview is scaled up from the
    // in-world player radius (0.35 world units at the gameplay zoom) purely so
    // it reads as a character portrait.
    constexpr float kPreviewR = 34.0F;
    b.circle(cx, cy, kPreviewR, body);
    b.circle(cx, cy, kPreviewR * 0.55F, Color{1.0F, 1.0F, 1.0F, 0.85F});
    if (ring.a > 0.0F) {
      const float pulse = 1.0F + 0.06F * std::sin(simTime_ * 3.5F);
      const float rr = kPreviewR * 1.22F * pulse;
      constexpr int kDots = 24;
      for (int k = 0; k < kDots; ++k) {
        const float a = (static_cast<float>(k) / static_cast<float>(kDots)) * 2.0F * kPi;
        b.circle(cx + std::cos(a) * rr, cy + std::sin(a) * rr, 3.0F, ring);
      }
    }
  }

  // --- Menu rows --------------------------------------------------------------
  const float rowY0 = py * 0.56F;
  const float rowH = 42.0F;
  const float rowX = px * 0.5F - 170.0F;
  const float rowW = 340.0F;

  struct Row {
    const char* label;
  };
  const Row rows[] = {
      {"START RUN"},
      {"SKIN"},
      {"OUTLINE"},
      {"QUIT"},
  };

  const auto& skins = skinPalette();
  const auto& outlines = outlinePalette();
  for (int i = 0; i < 4; ++i) {
    const float y = rowY0 + static_cast<float>(i) * rowH;
    const bool sel = (i == menuSelection_);
    const Color labelCol = sel ? gold : dim;
    // Selection bar: a filled rounded-ish block behind the active row.
    if (sel) {
      b.rectTopLeft(rowX - 18.0F, y - 8.0F, rowW + 36.0F, 34.0F,
                    Color{gold.r, gold.g, gold.b, 0.14F});
      // Caret marker.
      b.circle(rowX - 28.0F, y + 4.0F, 5.0F, gold);
    }
    b.text(rowX, y, 2.2F, labelCol, rows[static_cast<std::size_t>(i)].label);

    // Value on the right of the row, with the left/right hint.
    std::string value;
    if (i == 1 && profile_ != nullptr) {
      const int skin = std::clamp(profile_->skin, 0, static_cast<int>(skins.size()) - 1);
      value = std::string("< ") + skins[static_cast<std::size_t>(skin)].name + " >";
    } else if (i == 2) {
      if (profile_ == nullptr) {
        value = "< unavailable >";
      } else if (profile_->canUseOutline(profile_->outline) &&
                 profile_->outline < static_cast<int>(outlines.size())) {
        value = std::string("< ") + outlines[static_cast<std::size_t>(profile_->outline)].name +
                " >";
      } else {
        value = "< locked >";
      }
    }
    if (!value.empty()) {
      b.text(rowX + rowW - b.textWidth(1.6F, value), y + 2.0F, 1.6F,
             sel ? white : dim, value);
    }
  }

  // Locked-outline hint line: tells the player exactly what is still to earn.
  if (profile_ != nullptr) {
    std::string hint;
    for (std::size_t i = 1; i < outlines.size(); ++i) {
      if (!profile_->canUseOutline(static_cast<int>(i))) {
        hint = std::string("LOCKED: ") + outlines[i].requirement;
        break;
      }
    }
    if (!hint.empty()) {
      b.text(px * 0.5F - b.textWidth(1.5F, hint) * 0.5F, rowY0 + 4.0F * rowH + 18.0F, 1.5F,
             Color{0.85F, 0.55F, 0.35F, 1.0F}, hint);
    }
  }

  // Footer: controls + version-ish tagline.
  const std::string footer = "[W/S or UP/DOWN] SELECT   [A/D or LEFT/RIGHT] CHANGE   [ENTER] CONFIRM";
  b.text(px * 0.5F - b.textWidth(1.4F, footer) * 0.5F, py - 40.0F, 1.4F,
         Color{0.5F, 0.5F, 0.6F, 1.0F}, footer);
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
  // The main menu replaces the world entirely — no point rendering the game
  // behind an opaque screen.
  if (menuOpen_) {
    b.setScreenView();
    renderMainMenu(b, static_cast<float>(b.fbWidth()), static_cast<float>(b.fbHeight()));
    b.flush();
    return;
  }
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

      // Strength is shown by a coloured OUTLINE around the enemy (elite gold,
      // champion orange, overlord violet) — no glow, no floating text. The
      // outline follows the enemy's shape (ring for circles, frame for squares)
      // and is drawn thick so it reads at a glance.
      if (tr != nullptr && tr->tier > 0) {
        Color ring;
        if (tr->tier >= 3) {
          ring = {0.85F, 0.35F, 1.0F, 0.95F};
        } else if (tr->tier == 2) {
          ring = {1.0F, 0.45F, 0.10F, 0.95F};
        } else {
          ring = {1.0F, 0.85F, 0.20F, 0.95F};
        }
        const float rr = r.r * 1.16F;
        constexpr float kDot = 0.085F;
        if (s.circle) {
          constexpr int kRingDots = 24;
          for (int k = 0; k < kRingDots; ++k) {
            const float a = (static_cast<float>(k) / static_cast<float>(kRingDots)) * 2.0F * kPi;
            b.circle(x + std::cos(a) * rr, y + std::sin(a) * rr, kDot, ring);
          }
        } else {
          // Square perimeter: dots along each of the four edges.
          constexpr int kPerSide = 7;
          for (int side = 0; side < 4; ++side) {
            for (int k = 0; k <= kPerSide; ++k) {
              const float f = -rr + (2.0F * rr) * (static_cast<float>(k) / static_cast<float>(kPerSide));
              float dx = 0.0F;
              float dy = 0.0F;
              if (side == 0) { dx = f; dy = -rr; }
              else if (side == 1) { dx = f; dy = rr; }
              else if (side == 2) { dx = -rr; dy = f; }
              else { dx = rr; dy = f; }
              b.circle(x + dx, y + dy, kDot, ring);
            }
          }
        }
      }
      // Damage aura (TraitAura): faint pulsing disc around the enemy.
      if (tr != nullptr && tr->auraRadius > 0.0F) {
        b.circle(x, y, tr->auraRadius, Color{1.0F, 0.25F, 0.25F, 0.10F});
      }
    }
  }

  // Enemy shots (TraitArcher projectiles).
  {
    auto view = registry_.view<Transform, Sprite, EnemyShot, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& s = view.get<Sprite>(e);
      const auto& r = view.get<Radius>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      b.circle(x, y, r.r * 1.6F, Color{s.color.r, s.color.g, s.color.b, 0.30F});
      b.circle(x, y, r.r, s.color);
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

  // Orbit blades (daggers): streak + bright core.
  {
    auto view = registry_.view<Transform, OrbitBlade>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& ob = view.get<OrbitBlade>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      b.rect(x, y, 0.36F, 0.10F, ob.color);
      b.circle(x, y, 0.11F, ob.color);
    }
  }

  // Halo beams (evolution): bright spokes of light rotating around the player.
  {
    auto view = registry_.view<Transform, HaloBeam>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& hb = view.get<HaloBeam>(e);
      const float len = hb.length;
      const float ex = t.x + std::cos(hb.angle) * len;
      const float ey = t.y + std::sin(hb.angle) * len;
      Color glow = hb.color;
      glow.a = 0.16F;
      Color core = hb.color;
      core.a = 0.95F;
      const int steps = std::max(1, static_cast<int>(len / std::max(0.03F, hb.width * 0.15F)));
      for (int s = 0; s <= steps; ++s) {
        const float fr = static_cast<float>(s) / static_cast<float>(steps);
        const float sx = t.x + (ex - t.x) * fr;
        const float sy = t.y + (ey - t.y) * fr;
        b.circle(sx, sy, hb.width * 0.52F, glow);
        b.circle(sx, sy, hb.width * 0.30F, core);
      }
      // Bright hub where the spoke meets the player.
      b.circle(t.x, t.y, hb.width * 0.7F, core);
    }
  }

  // Vortex zones (super evolution): a hazy suction disc with a bright core and
  // a ring of orbiting debris, so the pull direction reads at a glance.
  {
    auto view = registry_.view<Transform, Vortex>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& vx = view.get<Vortex>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      // Outer "reach" halo: where the drag starts.
      Color reach = vx.color;
      reach.a = 0.07F;
      b.circle(x, y, vx.reach, reach);
      // Damage core.
      Color core = vx.color;
      core.a = 0.16F;
      b.circle(x, y, vx.radius, core);
      // Inward-spiralling motes: each one is drawn on a shrinking radius with a
      // counter-rotating angle, which reads as suction.
      constexpr int kMotes = 10;
      for (int k = 0; k < kMotes; ++k) {
        const float f = static_cast<float>(k) / static_cast<float>(kMotes);
        const float a = vx.angle * 1.7F - f * 4.2F;
        const float rr = vx.radius * (1.0F - f * 0.85F);
        Color mote = vx.color;
        mote.a = 0.30F + 0.45F * (1.0F - f);
        b.circle(x + std::cos(a) * rr, y + std::sin(a) * rr, 0.07F, mote);
      }
      // Bright centre.
      Color eye = vx.color;
      eye.a = 0.85F;
      b.circle(x, y, 0.13F, eye);
      Color rim = vx.color;
      rim.a = 0.45F;
      constexpr int kRim = 18;
      for (int k = 0; k < kRim; ++k) {
        const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(kRim);
        b.circle(x + std::cos(a) * vx.radius, y + std::sin(a) * vx.radius, 0.06F, rim);
      }
    }
  }

  // Bombs (hammer): body + faint shadow beneath to sell the arc.
  {
    auto view = registry_.view<Transform, BombProjectile, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& bp = view.get<BombProjectile>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      Color shadow = bp.color;
      shadow.r *= 0.35F;
      shadow.g *= 0.35F;
      shadow.b *= 0.35F;
      shadow.a = 0.45F;
      b.circle(x, y - 0.30F, 0.14F, shadow);
      b.circle(x, y, 0.17F, bp.color);
    }
  }

  // Boomerangs (shuriken / pulsar): rectangular blades; the pulsar evolution
  // also drags a glowing laser trail along the segment it just swept.
  {
    auto view = registry_.view<Transform, BoomerangProjectile, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& bp = view.get<BoomerangProjectile>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      if (bp.trailWidth > 0.0F) {
        Color glow = bp.color;
        glow.a = 0.10F;
        Color trailCore = bp.color;
        trailCore.a = 0.55F;
        const float len = std::sqrt((t.x - t.px) * (t.x - t.px) + (t.y - t.py) * (t.y - t.py));
        const int steps = std::max(1, static_cast<int>(len / std::max(0.03F, bp.trailWidth * 0.15F)));
        for (int s = 0; s <= steps; ++s) {
          const float fr = static_cast<float>(s) / static_cast<float>(steps);
          const float lx = t.px + (t.x - t.px) * fr;
          const float ly = t.py + (t.y - t.py) * fr;
          b.circle(lx, ly, bp.trailWidth * 0.52F, glow);
          b.circle(lx, ly, bp.trailWidth * 0.30F, trailCore);
        }
      }
      b.rect(x, y, 0.34F, 0.14F, bp.color);
      b.circle(x, y, 0.07F, bp.color);
    }
  }

  // Bouncing orbs (Void Orb): renders at its grown contact radius.
  {
    auto view = registry_.view<Transform, BounceProjectile, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& bp = view.get<BounceProjectile>(e);
      const auto& r = view.get<Radius>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      b.circle(x, y, r.r, bp.color);
      b.circle(x, y, r.r * 0.5F, Color{1.0F, 1.0F, 1.0F, 0.85F});
    }
  }

  // Beams: tight bright core + faint glow sampled densely along start->end.
  {
    auto view = registry_.view<BeamEffect>();
    for (const auto e : view) {
      const auto& be = view.get<BeamEffect>(e);
      const float fade = be.duration > 0.001F ? be.timer / be.duration : 0.0F;
      Color glow = be.color;
      glow.a = 0.16F * fade;
      Color core = be.color;
      core.a = 1.0F * fade;
      const float dx = be.endX - be.startX;
      const float dy = be.endY - be.startY;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.001F) continue;
      // Dense sampling (0.15x width per step) keeps the beam a solid line
      // instead of a string of soft blobs.
      const int steps = std::max(1, static_cast<int>(len / std::max(0.03F, be.width * 0.15F)));
      for (int s = 0; s <= steps; ++s) {
        const float fr = static_cast<float>(s) / static_cast<float>(steps);
        const float sx = be.startX + dx * fr;
        const float sy = be.startY + dy * fr;
        b.circle(sx, sy, be.width * 0.52F, glow);
        b.circle(sx, sy, be.width * 0.30F, core);
      }
    }
  }

  // Sweep arcs (scythe): fading wedge + bright swept edge.
  {
    auto view = registry_.view<Transform, SweepEffect, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& se = view.get<SweepEffect>(e);
      const float fade = se.duration > 0.001F ? se.timer / se.duration : 0.0F;
      Color fill = se.color;
      fill.a = 0.14F * fade;
      Color edge = se.color;
      edge.a = 0.70F * fade;
      const float span = se.endAngle - se.startAngle;

      const int spokes = 10;
      for (int k = 0; k <= spokes; ++k) {
        const float a = se.startAngle + span * static_cast<float>(k) / static_cast<float>(spokes);
        const float ex = t.x + std::cos(a) * se.radius;
        const float ey = t.y + std::sin(a) * se.radius;
        const int steps = std::max(1, static_cast<int>(se.radius / 0.12F));
        for (int s = 0; s <= steps; ++s) {
          const float fr = static_cast<float>(s) / static_cast<float>(steps);
          b.circle(t.x + (ex - t.x) * fr, t.y + (ey - t.y) * fr, 0.07F, fill);
        }
      }

      const int arcSteps = std::max(1, static_cast<int>(std::abs(span) * se.radius / 0.10F));
      for (int k = 0; k <= arcSteps; ++k) {
        const float a = se.startAngle + span * static_cast<float>(k) / static_cast<float>(arcSteps);
        b.circle(t.x + std::cos(a) * se.radius, t.y + std::sin(a) * se.radius, 0.08F, edge);
      }
    }
  }

  // Zones: pulsing disc + circumference highlight (lifelong AoE pools).
  {
    auto view = registry_.view<Transform, ZoneEffect>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& ze = view.get<ZoneEffect>(e);
      const float fade = ze.duration > 0.001F ? (ze.duration - ze.timer) / ze.duration : 0.0F;
      const float pulse = 1.0F + 0.05F * std::sin(simTime_ * 8.0F);
      Color disc = ze.color;
      disc.a = 0.18F * fade;
      b.circle(t.x, t.y, ze.radius * pulse, disc);
      Color rim = ze.color;
      rim.a = 0.55F * fade;
      const int n = 24;
      for (int k = 0; k < n; ++k) {
        const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(n);
        b.circle(t.x + std::cos(a) * ze.radius * pulse,
                 t.y + std::sin(a) * ze.radius * pulse, 0.07F, rim);
      }
    }
  }

  // Grave Bell beacons (lure): a wide, faint "taunt" disc, a bright kill core,
  // and motes walking inward from the rim so the drag direction is obvious.
  {
    auto view = registry_.view<Transform, Lure>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& lu = view.get<Lure>(e);
      const float fade = lu.maxLife > 0.001F ? std::clamp(lu.life / lu.maxLife, 0.0F, 1.0F) : 0.0F;
      // The reach: a very soft wash, so the player can see what it grabs.
      Color reach = lu.color;
      reach.a = 0.06F * fade;
      b.circle(t.x, t.y, lu.reach, reach);
      // The kill core: a rim that pulses on the damage tick.
      const float pulse = 1.0F + 0.08F * std::sin(simTime_ * 9.0F);
      Color core = lu.color;
      core.a = 0.20F * fade;
      b.circle(t.x, t.y, lu.radius * pulse, core);
      Color rim = lu.color;
      rim.a = 0.60F * fade;
      constexpr int kBellRim = 20;
      for (int k = 0; k < kBellRim; ++k) {
        const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(kBellRim);
        b.circle(t.x + std::cos(a) * lu.radius * pulse,
                 t.y + std::sin(a) * lu.radius * pulse, 0.07F, rim);
      }
      // Inward motes: the taunt, drawn.
      constexpr int kMotes = 8;
      for (int k = 0; k < kMotes; ++k) {
        const float f = (static_cast<float>(k) + 0.5F) / static_cast<float>(kMotes);
        const float a = 2.4F * static_cast<float>(k) - simTime_ * 1.3F;
        const float rr = lu.reach * (1.0F - f);
        Color mote = lu.color;
        mote.a = (0.10F + 0.35F * f) * fade;
        b.circle(t.x + std::cos(a) * rr, t.y + std::sin(a) * rr, 0.06F, mote);
      }
      Color eye = lu.color;
      eye.a = 0.85F * fade;
      b.circle(t.x, t.y, 0.15F, eye);
    }
  }

  // Chain lightning: halo + core at the current jump position.
  {
    auto view = registry_.view<Transform, ChainLightning>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& cl = view.get<ChainLightning>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      b.circle(x, y, 0.22F, Color{cl.color.r, cl.color.g, cl.color.b, 0.40F});
      b.circle(x, y, 0.11F, cl.color);
    }
  }

  // Nova rings: expanding dotted circumference over a faint wash.
  {
    auto view = registry_.view<Transform, NovaRing>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& nr = view.get<NovaRing>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      if (nr.radius <= 0.0F) continue;
      Color wash = nr.color;
      wash.a = 0.10F;
      b.circle(x, y, nr.radius, wash);
      Color edge = nr.color;
      edge.a = 0.85F;
      const int n = std::max(16, static_cast<int>(nr.radius * 12.0F));
      for (int k = 0; k < n; ++k) {
        const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(n);
        b.circle(x + std::cos(a) * nr.radius, y + std::sin(a) * nr.radius, 0.09F, edge);
      }
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
    // Earned outline: a dotted ring around the player, in the style of the
    // tier that unlocked it (gold = elite, ember = champion, violet =
    // overlord). This is a reward the player keeps forever, so it is drawn
    // under the poison/iframe tint but over the body, and pulses gently.
    if (outlineColor_.a > 0.0F) {
      const float pulse = 1.0F + 0.06F * std::sin(simTime_ * 3.5F);
      const float rr = r.r * 1.22F * pulse;
      constexpr int kRingDots = 20;
      for (int k = 0; k < kRingDots; ++k) {
        const float a = (static_cast<float>(k) / static_cast<float>(kRingDots)) * 2.0F * kPi +
                        simTime_ * 0.6F;
        b.circle(x + std::cos(a) * rr, y + std::sin(a) * rr, 0.075F, outlineColor_);
      }
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

  // Heal indicator (H). Shows READY or the remaining cooldown.
  {
    const bool ready = healCd_ <= 0.0F;
    const std::string label = ready ? "[H] HEAL READY"
                                    : "[H] HEAL " + std::to_string(static_cast<int>(healCd_ + 0.999F)) + "S";
    b.text(14.0F, 66.0F, 1.6F, ready ? Color{0.4F, 1.0F, 0.6F, 1.0F}
                                     : Color{0.55F, 0.6F, 0.6F, 1.0F},
           label);
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

  // Momentum chain, right under the kill counter. The bar is the time left
  // before the chain goes cold, so the player can literally watch a pause cost
  // them the bonus they were relying on.
  if (streak_ > 0) {
    const float cap = static_cast<float>(std::max(1, stats_.momentumMax));
    const float frac = std::min(1.0F, static_cast<float>(streak_) / cap);
    const std::string chain = "CHAIN x" + std::to_string(streak_) + "  +" +
                              std::to_string(static_cast<int>((momentumDamageMul_ - 1.0F) * 100.0F)) +
                              "% DMG";
    // Warm at the start of the chain, hot at the cap.
    const Color heat{0.6F + 0.4F * frac, 0.85F - 0.35F * frac, 0.35F - 0.25F * frac, 1.0F};
    b.text(px - b.textWidth(1.8F, chain) - 14.0F, 36.0F, 1.8F, heat, chain);
    const float barW = 120.0F;
    const float left = std::clamp(streakTimer_ / std::max(0.5F, stats_.momentumWindow), 0.0F, 1.0F);
    b.rectTopLeft(px - barW - 14.0F, 50.0F, barW, 4.0F, Color{0.35F, 0.30F, 0.35F, 0.8F});
    b.rectTopLeft(px - barW * left - 14.0F, 50.0F, barW * left, 4.0F, heat);
  }

  // Active abilities (bottom-left): key, name and the seconds left. Stasis also
  // reports how deep in stasis the world currently is, so the slow is visible
  // on the HUD instead of only being felt.
  {
    static const char* kKeys[3] = {"J", "K", "L"};
    static const char* kNames[3] = {"DASH", "OVERLOAD", "STASIS"};
    const float rowY = py - 42.0F;
    for (int i = 0; i < kAbilityCount; ++i) {
      const auto a = static_cast<Ability>(i);
      const float left = abilityCooldownRemaining(a);
      const bool ready = left <= 0.0F;
      const bool active = (a == Ability::Stasis && stasis_ > 0.0F);
      const float x = 14.0F + static_cast<float>(i) * 168.0F;
      std::string label = std::string("[") + kKeys[i] + "] " + kNames[i];
      if (active) {
        label += " " + std::to_string(static_cast<int>(stasis_ * 10.0F + 0.5F) / 10);
      } else if (!ready) {
        label += " " + std::to_string(static_cast<int>(left + 0.999F)) + "S";
      }
      const Color tint = active   ? Color{0.6F, 0.85F, 1.0F, 1.0F}
                          : ready  ? Color{0.55F, 1.0F, 0.7F, 1.0F}
                                   : Color{0.5F, 0.52F, 0.58F, 1.0F};
      b.text(x, rowY, 1.7F, tint, label);
      // Cooldown bar: full when ready, draining as it comes back.
      const float full = abilityCooldown(a);
      const float frac = ready ? 1.0F : std::clamp(1.0F - left / std::max(0.01F, full), 0.0F, 1.0F);
      b.rectTopLeft(x, rowY + 20.0F, 150.0F, 3.0F, Color{0.18F, 0.18F, 0.22F, 0.8F});
      b.rectTopLeft(x, rowY + 20.0F, 150.0F * frac, 3.0F, tint);
    }
  }

  // Tribunal banner: the adaptive director announces a newly opened tier under
  // the timer, so the player learns the rule at the moment it fires.
  if (!tierBanner_.empty() && tierBannerT_ > 0.0F) {
    const Color tierGlow = tierBanner_[0] == 'O' ? Color{0.85F, 0.45F, 1.0F, 1.0F}
                                               : Color{1.0F, 0.55F, 0.25F, 1.0F};
    // Fade in over the first 0.3 s, hold, then fade out over the last second.
    const float a = std::min(1.0F, (4.0F - tierBannerT_) / 0.3F) *
                    std::min(1.0F, tierBannerT_ / 1.0F);
    Color fade = tierGlow;
    fade.a = a;
    b.text(px * 0.5F - b.textWidth(2.6F, tierBanner_) * 0.5F, py * 0.5F - 150.0F, 2.6F,
           fade, tierBanner_);
  }

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

    const std::string title = choosingStarter_ ? "CHOOSE A STARTING WEAPON"
                              : milestoneOffer_ ? "MILESTONE! CHOOSE 1/2"
                              : choices_.size() > 3 ? "LEVEL UP! CHOOSE 1/2/3/4"
                                                     : "LEVEL UP! CHOOSE 1/2/3";
    b.text(px * 0.5F - b.textWidth(4.0F, title) * 0.5F, py * 0.16F, 4.0F,
           (milestoneOffer_ && !choosingStarter_) ? violet : gold, title);

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
        else if (choice.kind == Choice::Kind::Skip) accent = Color{0.55F, 0.6F, 0.7F, 1.0F};
        else if (content_.upgrades[static_cast<std::size_t>(choice.index)].kind == "unique") {
          accent = gold;
        }
        b.rectTopLeft(x, y, cardW, 4.0F, accent);

        const std::string key = "[" + std::to_string(i + 1) + "]";
        b.text(x + 16.0F, y + 16.0F, 3.0F, accent, key);

        if (choice.kind == Choice::Kind::Skip) {
          b.text(x + 16.0F, y + 52.0F, 2.3F, white, "NOTHING LEFT");
          renderWrappedText(b, x + 16.0F, y + 80.0F, 1.7F,
                            Color{0.85F, 0.85F, 0.9F, 1.0F},
                            "Every upgrade you can use is already maxed.", cardW - 32.0F, 14.0F);
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, accent, "PRESS 1 TO CONTINUE");
        } else if (choice.kind == Choice::Kind::Upgrade) {
          const auto& def = content_.upgrades[static_cast<std::size_t>(choice.index)];
          b.text(x + 16.0F, y + 52.0F, 2.3F, white, def.name);
          renderWrappedText(b, x + 16.0F, y + 80.0F, 1.7F,
                            Color{0.85F, 0.85F, 0.9F, 1.0F}, def.desc,
                            cardW - 32.0F, 14.0F);
          const std::string stacks =
              "STACKS " + std::to_string(upgradeStacks(static_cast<std::size_t>(choice.index))) +
              "/" + std::to_string(def.maxStacks);
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, Color{0.6F, 0.6F, 0.7F, 1.0F}, stacks);
        } else {
          const auto& def = content_.weapons[static_cast<std::size_t>(choice.index)];
          b.text(x + 16.0F, y + 52.0F, 2.3F, white, def.name);
          renderWrappedText(b, x + 16.0F, y + 80.0F, 1.7F,
                            Color{0.85F, 0.85F, 0.9F, 1.0F}, def.desc,
                            cardW - 32.0F, 14.0F);
          const std::string tag =
              def.prereqs.empty()
                  ? "NEW WEAPON"
                  : (def.prereqs.size() >= 3 ? "SUPER EVOLUTION! (A+B+C)"
                                             : "EVOLUTION! (A+B)");
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, teal, tag);
        }
        x += cardW + gap;
      }
    }

    // Reroll hint (R is always the reroll key on level-up) showing how many
    // rerolls are still available this level.
    const int rerollsLeft = 1 + stats_.rerollCharges - rerollsUsed_;
    const std::string hint = rerollsLeft > 0
                                 ? "[R] REROLL x" + std::to_string(rerollsLeft)
                                 : "[R] REROLL NONE LEFT";
    b.text(px * 0.5F - b.textWidth(2.0F, hint) * 0.5F, py * 0.66F, 2.0F,
           rerollsLeft > 0 ? Color{0.7F, 0.7F, 0.8F, 1.0F}
                           : Color{0.45F, 0.45F, 0.5F, 1.0F},
           hint);
  }

  // Pause overlay: ESC pauses and lets you read your character sheet, or B
  // toggles the bestiary of everything you have slain.
  if (state_ == RunState::Paused) {
    b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.0F, 0.0F, 0.0F, 0.5F});
    if (bestiaryOpen_) {
      renderBestiary(b, px, py);
    } else {
      renderPlayerStats(b, px, py);
    }
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

  // Weapon test mode overlay: shows the armed weapon and the sandbox controls.
  if (testMode_) {
    b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.0F, 0.0F, 0.0F, 0.18F});
    std::string wname = "NO WEAPON";
    std::string wtag = "";
    if (weaponCount_ >= 1) {
      const auto& def = content_.weapons[static_cast<std::size_t>(weapons_[0].def)];
      wname = def.name;
      wtag = def.prereqs.empty() ? ""
                                 : (def.prereqs.size() >= 3 ? " (SUPER)" : " (EVO)");
    }
    const std::string title =
        "WEAPON TEST " + std::to_string(testWeaponIdx_ + 1) + "/" +
        std::to_string(content_.weapons.size()) + " - " + wname + wtag;
    b.text(px * 0.5F - b.textWidth(3.0F, title) * 0.5F, py * 0.70F, 3.0F, gold, title);
    const std::string boost = testBoosted_ ? "BOOST ON" : "BOOST OFF";
    const std::string waves = wavesEnabled_ ? "WAVES ON" : "WAVES OFF";
    const std::string god = testInvuln_ ? "GOD ON" : "GOD OFF";
    const std::string clock = "CLOCK X" + std::to_string(testTimeScale_);
    const std::string status = boost + "   " + waves + "   " + god + "   " + clock;
    b.text(px * 0.5F - b.textWidth(2.0F, status) * 0.5F, py * 0.74F, 2.0F,
           Color{0.75F, 0.85F, 1.0F, 1.0F}, status);
    // Split over two rows so the whole keymap fits even in a narrow window.
    const Color key{0.8F, 0.8F, 0.85F, 1.0F};
    const std::string rowA =
        "[1] PREV   [2] NEXT   [3] MAX BUILD   [4] WAVES   [5] CLOSE";
    b.text(px * 0.5F - b.textWidth(1.6F, rowA) * 0.5F, py * 0.775F, 1.6F, key, rowA);
    const std::string rowB =
        "[E] ITEMS   [I] GOD   [F] CLOCK   [X] KILL   [R] MAX ALL";
    b.text(px * 0.5F - b.textWidth(1.6F, rowB) * 0.5F, py * 0.805F, 1.6F, key, rowB);
    const std::string noteA = "NO XP - NO SKINS - LEAVING THE SANDBOX ENDS THE RUN";
    b.text(px * 0.5F - b.textWidth(1.4F, noteA) * 0.5F, py * 0.84F, 1.4F,
           Color{0.95F, 0.55F, 0.45F, 1.0F}, noteA);
    if (testShopOpen_) renderTestShop(b, px, py);
  }

  b.flush();
}

} // namespace game