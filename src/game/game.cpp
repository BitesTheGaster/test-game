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
// How long a tribunal banner lives, in seconds. One constant for both the timer
// and the fade-in, because the fade has to be measured against the lifetime it
// is fading from.
constexpr float kTierBannerLife = 4.0F;
// Visible rows in the test sandbox's item list.
constexpr int kTestShopRows = 14;
constexpr float kSpawnDist = 11.0F;
constexpr float kSpawnTelegraph = 0.6F; // seconds a spawn marker is visible
constexpr float kPickupDist = 0.45F;
constexpr float kContactIframes = 0.18F;    // enemies connect more often

// The orbit ring's interior sweep. Blades only touch the circle itself, so this
// is what makes the inside of the dagger's ring dangerous to stand in; it is a
// reduced share of one blade's contact damage (see updateOrbitBlades).
constexpr float kOrbitInnerMul = 0.35F;

// How wide, in radians, the Blade Vortex gap is -- half-angle, so the full
// opening is twice this. Sized off the ring rather than hard-coded to a blade
// count: with two blades a gap narrower than a blade's own shadow would never
// fit an enemy, and with six a wide one would overlap its neighbours and the
// "gap" would stop being a gap.
constexpr float kOrbitWindowHalf = 0.42F;

// The on-hit marks' two fixed numbers. A card supplies the SCALE (seconds of
// chill, burn damage per second, how much hex a hit adds) and these supply the
// shape, so four cards can never drift into four unrelated behaviours by each
// picking its own constants.
constexpr float kMarkChillMul = 0.60F;   // Frostbind's chill depth
constexpr float kMarkBurnWindow = 4.0F;   // seconds a single hit keeps a body lit
// How many hits' worth of hex one card is worth. The card sets the per-hit step
// and this is what turns it into a ceiling, so "Hex" reads as "+12% damage taken
// per hit, up to 96%" -- a ratchet with a visible end rather than an open ramp.
constexpr float kMarkVulnHits = 8.0F;

// How close a contracting nova will drag a victim to the player. The Void Nova's
// whole fantasy is a knot of bodies pulled into a knot and then detonated, and
// its pull used to obey no floor at all: it dragged everything right onto the
// player's own position, so the fantasy was delivered and then the player died of
// it before the burst ever landed. Enemies now line up just OUTSIDE the incoming
// ring instead, which is the same picture -- a ring of bodies closing in -- with
// the player standing in the middle of it alive.
constexpr float kNovaPullFloor = 1.6F;

// How far above the first target a chain bolt is drawn coming down from. A
// lightning strike that starts at the victim's own centre is just a dot; the
// vertical drop is what makes it read as a strike.
constexpr float kChainSkyDrop = 4.2F;

// Deterministic per-point jitter for a jagged bolt, in [-1, 1]. Derived from the
// entity handle and the step index so the bolt is a different shape every shot
// but does not shimmer between frames within one shot (which would look like
// noise, not lightning).
float boltJitter(unsigned seed) {
  unsigned h = seed * 2654435761U;
  h ^= h >> 15U;
  h *= 2246822519U;
  h ^= h >> 13U;
  return static_cast<float>(h & 0xFFFFU) / 32767.5F - 1.0F;
}

// Enemies die once their HP drops to (or below) this tiny epsilon. Defense
// mitigation and float rounding can otherwise leave a sliver of HP, so a hit
// that should be lethal leaves a "0 HP" enemy alive.
constexpr float kEnemyDeathEpsilon = 1.0e-3F;

// Base HP multiplier ranges per enemy tier. Rolled per spawn so each elite is
// tougher than the last; higher tiers are exponentially beefier so they never
// simply melt. Applied on top of the global time-based hp scale.
// These came down across the board. An elite is meant to be the thing that
// interrupts a good run, not the thing that ends it: at 5-10x base HP an elite
// of an ordinary trash type was already a health bar, and a champion at 25-100x
// was a wall. The bands are still far enough apart to read at a glance (a
// champion is visibly a different proposition from an elite) but they no longer
// out-scale everything the player can own by minute three.
constexpr float kEliteHpMin = 3.5F;
constexpr float kEliteHpMax = 6.5F;
constexpr float kChampionHpMin = 15.0F;
constexpr float kChampionHpMax = 55.0F;
constexpr float kOverlordHpMin = 70.0F;
constexpr float kOverlordHpMax = 450.0F;

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
    // Touch and speed are down too. The XP multiplier is deliberately NOT: an
    // elite is a reward before it is a threat, and if killing it is worse value
    // than killing three pieces of trash then the player is right to ignore it.
    case 3: return {kOverlordHpMin, kOverlordHpMax, 2.8F, 1.30F, 10.0F};
    case 2: return {kChampionHpMin, kChampionHpMax, 1.9F, 1.18F, 5.0F};
    case 1: return {kEliteHpMin, kEliteHpMax, 1.25F, 1.08F, 3.0F};
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
  if (effect == "heal_pct") {
    // Percentage of CURRENT max HP, resolved here so the card keeps working
    // after a later max-HP card raises the ceiling. `heal` stays as the flat
    // form for milestone-sized top-ups where a fraction of a big pool would be
    // far too small to matter.
    return {true, stats.maxHp * value, 0.0F};
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
  if (effect == "shield_regen") {
    // Refill speed. Multiplicative on the base 10 HP/s so the card reads as
    // "+X% shield regen" and never needs a rebalance when the base moves.
    stats.shieldRegenMul += value;
    return {true, 0.0F, value}; // and tops the pool up, so the card feels now
  }
  if (effect == "shield_delay") {
    // Shortens the out-of-combat wait. Cards subtract; the floor in
    // updateShield() keeps the pool from becoming permanent.
    stats.shieldRegenDelay -= value;
    return {true, 0.0F, 0.0F};
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
    // One more weapon slot per stack, floored at zero and capped at the storage
    // array's headroom. The clamp is what makes weaponCap()'s own clamp a
    // belt-and-braces check rather than the only thing between a hand-edited
    // content file and a buffer overrun.
    stats.weaponSlots = std::clamp(stats.weaponSlots + static_cast<int>(value), 0,
                                   Game::kMaxSlotCards);
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
  // --- On-hit marks (the "element" milestone group) ---------------------------
  // Four of them, and they are mutually exclusive by construction: the cards that
  // grant them are all in one group, so a run can only ever own one. That is why
  // they are allowed to be blunt -- each one is the whole answer to a question
  // rather than one line in a long list of multipliers.
  if (effect == "mark_slow") {
    // Frostbind: seconds of chill per hit. A deeper chill than the ice weapons
    // would ever give, and a shorter one, so a fast melee build can hold a front
    // rank in place without turning into the Frost Shards build.
    stats.markChillTime += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "mark_burn") {
    // Emberbrand: damage per second, refreshed on every hit. Refreshing rather
    // than accumulating is what makes it a question of hit RATE, which is a
    // different build from every other lifesteal/regen answer in the game.
    stats.markBurnDps += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "mark_vuln") {
    // Hex: +damage taken per hit, and the ceiling. Both are on the card, because
    // the per-hit step without a ceiling is a trap for a slow weapon and the
    // ceiling without a step is a flat bonus.
    stats.markVuln += value;
    stats.markVulnMax = std::max(stats.markVulnMax, value * kMarkVulnHits);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "mark_defstrip") {
    // Armour Split: flat defence removed per hit. No cap needed -- the enemy's
    // own defence is the cap, and hitting something with none is a no-op rather
    // than a wasted effect.
    stats.markDefStrip += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "chest_bonus") {
    // One more weapon per box. It is on a card rather than baked into the tier
    // numbers because the count IS the reward: making the elite's box bigger is
    // the whole point of making elites rarer, and this is the lever that lets a
    // player push the elite's box up to a champion's without a champion existing.
    stats.chestBonus += static_cast<int>(value);
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
  if (effect == "momentum_rate") {
    // % fire rate per chain stack. This is the axis the whole meter exists for
    // and it had no card at all, so a chain built purely for damage was the only
    // way to play it.
    stats.momentumRate += value;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "momentum_chain") {
    // How long the chain may grow before the extra stacks stop counting. Reaching
    // it is a pure DPS milestone, and it is the only way to make a wide,
    // slow-clearing build worth chaining with.
    stats.momentumMax += static_cast<int>(value);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "momentum_gain") {
    // Stacks per kill. Makes short, dense waves feed the meter faster than one
    // huge chaff horde, which is the opposite of Bloodthirst's trade.
    stats.momentumGain += value;
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
  // --- Stackable ability cards ------------------------------------------------
  // The one-shot uniques above are dramatic; these are the ordinary level-up
  // cards that let a build actually scale the buttons, so the J/K/L row is a
  // real build axis instead of a novelty one has to hope to roll once. `value`
  // is the per-stack magnitude and each one is floored where a floor matters.
  if (effect == "ability_dash") {
    // Phase Dash: a longer jump, a touch more landing invulnerability. Capped
    // because a blink that crosses the screen is not a dash, it is a skip
    // button — it would delete positioning, which is the only thing the dash
    // is for.
    stats.blinkDist = std::min(9.0F, stats.blinkDist + value);
    stats.blinkIframes = std::min(1.2F, stats.blinkIframes + value * 0.08F);
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ability_burst") {
    // Overload: wider, harder, and it shoves harder so the knockback is still a
    // defensive tool at the radius the extra damage now covers.
    stats.burstRadius += value;
    stats.burstDamage += value * 37.5F;
    stats.burstKnockback += value * 1.25F;
    return {true, 0.0F, 0.0F};
  }
  if (effect == "ability_slow") {
    // Stasis: longer and deeper, with the same 0.15 floor as the unique so the
    // world never grinds to a halt outright.
    stats.stasisDuration += value;
    stats.stasisSlow = std::max(0.15F, stats.stasisSlow - value * 0.08F);
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
  // only thing that makes a run interesting.
  //
  // This curve was made deliberately steep a few rounds back, on the reasoning
  // that a build maxed out in four minutes makes the other twenty an empty walk.
  // That was half right, and it overshot: the same steepness that keeps picks
  // coming late also keeps the player BEHIND for the whole first half of a run,
  // because the fight is on before the build is. Being under-levelled is not a
  // slow start, it is a dead run -- the player never gets the tools that answer
  // what is hitting them.
  //
  // So this is about 0.62x of the old curve, uniformly: reaching level 16 costs
  // 3.0k instead of 4.8k, level 32 costs 20k instead of 33k. The shape is
  // deliberately unchanged -- still a rising quadratic, not a flattened one --
  // because a flat curve would deliver every level in the same length of time
  // and the run would stop having a shape at all.
  const float l = static_cast<float>(level - 1);
  return 10.0F + 6.5F * l + 1.55F * l * (l + 1.0F);
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
  // A grace period keeps the opening free of mitigation, then defense ramps so
  // late waves shrug off a slice of every hit.
  //
  // The grace is a full minute now and the ramp is a third slower, for the same
  // reason the XP curve came down: mitigation is a tax on every hit forever, so
  // it punishes a build that is still coming together far more than it punishes
  // a finished one. An enemy that takes 10% off every swing from minute two is
  // not a difficulty curve, it is an invisible tax on the first ten picks.
  const float base = std::max(0.0F, simTime - 60.0F) / 34.0F;
  const float tierMul = tier >= 3 ? 2.2F : tier == 2 ? 1.7F : tier == 1 ? 1.25F : 1.0F;
  return base * tierMul;
}

float enemyLifestealResistance(float simTime, int tier, bool resistant) {
  // Caps and rates both down. Drain is one of the strongest things a build can
  // own, and a resistance curve that reaches 75% on ordinary trash quietly taxes
  // every lifesteal build for the whole run rather than making elites the thing
  // you build drain for.
  float res = std::min(0.60F, simTime / 1500.0F);
  res += tier >= 3 ? 0.35F : tier == 2 ? 0.25F : tier == 1 ? 0.15F : 0.0F;
  if (resistant) res += 0.5F;
  return std::min(1.0F, res);
}

float enemyKnockbackResistance(float simTime, int tier, bool resistant) {
  // Reaches 55% resistance for an ordinary enemy at 13:45 instead of 70% at
  // 8:45, then tier and trait bonuses push elites towards the cap. Knockback is
  // a positioning tool, so resistance is a tax on the player's control rather
  // than on their damage: at the old rate an ordinary enemy stopped being
  // pushable halfway through a run, which quietly turned "walk backwards and
  // kite" into "stand still and hope".
  float res = std::min(0.55F, simTime / 900.0F);
  res += tier >= 3 ? 0.35F : tier == 2 ? 0.25F : tier == 1 ? 0.15F : 0.0F;
  if (resistant) res += 0.5F;
  return std::min(1.0F, res);
}

int traitsForTier(int tier, float simTime) {
  if (tier <= 1) return tier == 1 ? 1 : 0; // elite: exactly one bonus
  // Champions: 2 base, +1 after 5 minutes. Overlords: 4 base, +1 after 10.
  // Both extra traits moved later. A champion with three traits at the four
  // minute mark is not a champion, it is a boss with a champion's label, and the
  // player has no way to have built for it yet.
  if (tier == 2) return 2 + (simTime >= 300.0F ? 1 : 0);
  return 4 + (simTime >= 600.0F ? 1 : 0);
}

Game::Game(const Content& content, std::uint32_t seed)
    : content_(content),
      rng_(seed),
      stacks_(content.upgrades.size(), 0),
      blocked_(content.upgrades.size(), 0) {
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
  // A restart has to forget which milestone groups the last run closed off,
  // exactly as it forgets the stacks. Leaving the lock behind would silently
  // shrink the milestone groups of the next run -- and shrink them differently
  // each time, which is the worst version of that bug.
  std::fill(blocked_.begin(), blocked_.end(), 0);
  choices_.clear();
  pending_.clear();
  particles_.clear();
  particleCursor_ = 0;

  // The manual is not run state and deliberately survives a reset — a player who
  // was partway through the weapon list should not lose their place. The page
  // INDEX is clamped here anyway, because the content it indexes into is what a
  // reset could plausibly change, and out of range is the one value the renderer
  // cannot recover from.
  if (manualPage_ >= content_.manual.size()) manualPage_ = 0;

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
  // All five flags, not the two that were here: testInvuln_, testTimeScale_ and
  // testShopOpen_ were left stale across a reset. Nothing broke, because every
  // reader is gated on testMode_ (now false) and enterTestModeImpl happens to
  // re-initialise them -- but that masking is incidental, and a new consumer
  // reading one of the three would have inherited the previous run's sandbox
  // state.
  testMode_ = false;
  testBoosted_ = false;
  testInvuln_ = false;
  testShopOpen_ = false;
  testTimeScale_ = 1;
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
  // Moving off RESET PROGRESS disarms it: the "press Enter again to wipe"
  // window belongs to the row you armed it on, not to wherever you drifted next.
  if (resetArmed_ && (input.menuUp || input.menuDown || input.menuLeft || input.menuRight)) {
    resetArmed_ = false;
  }
  if (input.menuUp) {
    menuSelection_ = (menuSelection_ + kMenuRows - 1) % kMenuRows;
  } else if (input.menuDown) {
    menuSelection_ = (menuSelection_ + 1) % kMenuRows;
  }

  // F1 opens the manual from the menu. Handled before the profile check because
  // the manual is not a profile feature and must work in a headless caller too.
  if (input.manualToggle) {
    openManual();
    return;
  }

  if (profile_ == nullptr) {
    // Without a profile the cosmetic rows are inert, but START, MANUAL, RESET
    // and QUIT must still work (tests and headless callers may not attach one).
    if (input.menuConfirm) {
      if (menuSelection_ == kMenuStart) {
        menuOpen_ = false;
      } else if (menuSelection_ == kMenuManual) {
        openManual();
      } else if (menuSelection_ == kMenuReset) {
        // Nothing to wipe without a profile, so there is nothing to arm either.
        resetArmed_ = false;
      } else if (menuSelection_ == kMenuQuit) {
        quitRequested_ = true;
      }
    }
    return;
  }

  const int skins = static_cast<int>(skinPalette().size());
  const int outlines = static_cast<int>(outlinePalette().size());
  if (input.menuLeft || input.menuRight) {
    const int dir = input.menuRight ? 1 : -1;
    if (menuSelection_ == kMenuSkin) {
      profile_->skin = ((profile_->skin + dir) % skins + skins) % skins;
      profileDirty_ = true;
      applyProfileToPlayer();
    } else if (menuSelection_ == kMenuOutline) {
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
    case kMenuStart:
      menuOpen_ = false;
      break;
    case kMenuSkin: // advance to the next colour directly on confirm too
      profile_->skin = (profile_->skin + 1) % skins;
      profileDirty_ = true;
      applyProfileToPlayer();
      break;
    case kMenuOutline: // same convenience
      for (int step = 0; step < outlines; ++step) {
        profile_->outline = (profile_->outline + 1) % outlines;
        if (profile_->canUseOutline(profile_->outline)) break;
      }
      profileDirty_ = true;
      applyProfileToPlayer();
      break;
    case kMenuManual:
      openManual();
      break;
    case kMenuReset:
      // Two-step. The first Enter only arms; the wipe needs the next one.
      if (resetArmed_) {
        confirmProgressReset();
      } else {
        beginProgressReset();
      }
      break;
    case kMenuQuit:
      quitRequested_ = true;
      break;
    default:
      break;
  }
}

void Game::beginProgressReset() {
  resetArmed_ = true;
}

bool Game::confirmProgressReset() {
  resetArmed_ = false;
  if (profile_ == nullptr) return false;
  // Wipe to first-launch defaults. Assigning a fresh Profile is the whole
  // operation — there is no second copy of the progress to hunt down.
  *profile_ = Profile{};
  // syncedUnlocks_ is the set of bits we already reported to the profile. After
  // a wipe it must be cleared, or the next kill would re-push an unlock that
  // the player no longer has and silently re-grant it.
  syncedUnlocks_ = 0;
  profileDirty_ = true;
  // Repaint: the default skin is a different colour than whatever was worn.
  applyProfileToPlayer();
  return true;
}

void Game::openManual() {
  manualOpen_ = true;
  if (manualPage_ >= content_.manual.size()) manualPage_ = 0;
}

void Game::closeManual() {
  manualOpen_ = false;
}

void Game::nextManualPage() {
  if (content_.manual.empty()) return;
  manualPage_ = (manualPage_ + 1) % content_.manual.size();
}

void Game::prevManualPage() {
  if (content_.manual.empty()) return;
  manualPage_ = (manualPage_ + content_.manual.size() - 1) % content_.manual.size();
}

std::string Game::manualPageId() const {
  if (manualPage_ >= content_.manual.size()) return {};
  return content_.manual[manualPage_].id;
}

void Game::updateManual(const FrameInput& input) {
  if (input.manualToggle || input.togglePause) {
    closeManual();
    return;
  }
  // B is the bestiary's key while paused; inside the manual it is "back", which
  // is what a player who learned the pause screen will press first.
  if (input.bestiary || input.restart) {
    closeManual();
    return;
  }
  if (input.menuDown) nextManualPage();
  if (input.menuUp) prevManualPage();
  if (input.menuLeft) prevManualPage();
  if (input.menuRight) nextManualPage();
  // Number keys jump straight to a page: a player who knows the page list
  // should not have to flip through eight screens to get there.
  const int jump = (input.choose1 ? 0 : input.choose2 ? 1 : input.choose3 ? 2
                : input.choose4 ? 3 : input.choose5 ? 4 : -1);
  if (jump >= 0) {
    if (static_cast<std::size_t>(jump) < content_.manual.size()) {
      manualPage_ = static_cast<std::size_t>(jump);
    }
  }
}

void Game::advance(float frameDt, const FrameInput& input) {
  // The manual is the topmost overlay and wins over everything below it,
  // including the main menu, so F1 always does the obvious thing.
  if (manualOpen_) {
    updateManual(input);
    timestep_.reset();
    return;
  }
  // The main menu is fully modal: it eats the whole frame and the simulation
  // does not advance behind it.
  if (menuOpen_) {
    updateMainMenu(input);
    timestep_.reset();
    return;
  }
  // F1 from a live run, the pause screen or a level-up screen. The run is not
  // advanced this frame: reading the manual should not cost you the horde.
  if (input.manualToggle) {
    // Opening the manual disarms an armed abandon. It used not to, so the flow
    // "arm Q, open the manual, close it" left a destructive action primed with
    // its red hint still showing -- and this branch returns before the cancel
    // list below ever gets a look at the input.
    quitArmed_ = false;
    openManual();
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
      // Pausing disarms the quit, so the arming window belongs to the pause you
      // are looking at rather than surviving an unpause/pause cycle.
      quitArmed_ = false;
    } else if (state_ == RunState::Paused) {
      state_ = RunState::Playing;
      bestiaryOpen_ = false;
      quitArmed_ = false;
    }
  }
  // Q on the pause screen abandons the run. Two presses, because throwing away a
  // twenty-minute run on a stray keypress is unforgivable and there was, until
  // now, no way out of a run at all short of dying on purpose.
  if (state_ == RunState::Paused && input.quitRun) {
    if (quitArmed_) {
      quitArmed_ = false;
      bestiaryOpen_ = false;
      testShopOpen_ = false;
      // reset() rebuilds the registry and clears stats and weapons outright, so
      // it supersedes everything the sandbox would have restored. Calling
      // exitTestMode() here too was pure waste -- and worse, exitTestModeImpl
      // sets state_ = GameOver on its way out, which reset() then undoes.
      reset();
      menuOpen_ = true;
      return;
    }
    quitArmed_ = true;
  }
  // Anything else the player decides on the pause screen cancels the armed quit,
  // so a player who changes their mind and does something else does not lose the
  // run to a later Q. (F1 is not in this list because it is handled above and
  // returns, which is exactly why it had to be cleared there.)
  if (quitArmed_ && state_ == RunState::Paused && input.togglePause) {
    quitArmed_ = false;
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
  //
  // Only Playing. The condition used to also accept LevelUp on the strength of
  // a comment claiming "sandbox switches work even while a level-up card screen
  // is open" -- but that state is unreachable while the sandbox is on: the
  // sandbox can only be entered from Playing, and while it is on nothing can
  // reach LevelUp (grantXp returns early, the level-up trigger is gated on
  // !testMode_, and enterTestMode arms a weapon so the starter pick never
  // fires). It was a branch that looked like a feature and did nothing.
  if (state_ == RunState::Playing && input.testModeToggle) {
    if (testMode_) {
      exitTestMode();
    } else {
      enterTestMode();
    }
  }
  if (testMode_) {
    // The switches that are not about the run itself (shop, invulnerability,
    // difficulty clock, max-all) stay live on any screen, so a player does not
    // have to un-pause the game to flip one. The ones that drive the world --
    // killing the player, changing weapon -- need the world to be running.
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
  updateWaveEffects();
  updateNovaRing();
  updateEnemyShots();
  updatePickups();
  // Fixed step, like every other timer in here.
  updateChestToast(1.0F / 60.0F);

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

    // Chilled/slowed enemies move at `slowMul` of their own speed. The multiplier
    // is stored on the enemy rather than being a constant so that a deeper chill
    // replaces a shallower one instead of both fighting over one number.
    float effSpeed = en.speed;
    if (en.slowT > 0.0F) {
      en.slowT -= dt;
      effSpeed *= en.slowMul;
    }
    // Burning. Ticked here, next to the chill, for the same reason: it is a
    // per-second effect on a body and the only place that owns a body's clock.
    // `mark = false` because this damage came FROM the burn and must not set the
    // burn again, or a lit body would be permanently re-ignited and would also
    // hex itself every tick for free.
    if (en.burnT > 0.0F) {
      en.burnT -= dt;
      if (en.burnDps > 0.0F) {
        applyEnemyDamage(e, en.burnDps * dt, false);
        if (!registry_.valid(e)) break;
      }
      if (en.burnT <= 0.0F) en.burnDps = 0.0F;
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
        en.slowMul = 0.45F;
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

    // --- Draining a shot that is still in progress ---------------------------
    // These two are NOT cooldowns. A three-arc whip and a double-pulse nova are
    // each ONE attack that pays out over the following beat, and the whole point
    // is that they do NOT recharge in between: the second arc is part of the same
    // swing, and the echo ring lands while the first ring is still growing.
    //
    // They used to sit BELOW the cooldown gate, under a comment saying they were
    // drained before the cooldown was consulted -- which was not true. So the
    // follow-up waited for the weapon's own cooldown to expire first: a
    // three-arc Tidal Lash whose last two arcs were really two more separate
    // attacks, and a Discharge echo that arrived two seconds after the ring it
    // was supposed to double had already finished. Both cards did nothing.
    //
    // Draining first also gets the ordering right for free: a shot that is mid
    // burst spends its frames finishing the burst, and only fires again once the
    // burst is over AND the timer has expired.
    if (w.attackType == AttackType::Wave && w.waveBurstLeft > 0) {
      if (w.waveBurstTimer > 0.0F) {
        w.waveBurstTimer -= 1.0F / 60.0F;
        continue;
      }
      w.waveBurstTimer = kWaveBurstGap;
      const float burstDmg = w.damage * stats_.damageMul * momentumDamageMul_;
      spawnWaveCrescent(w, w.waveBurstAngle, burstDmg, w.pierce + stats_.pierceAdd);
      --w.waveBurstLeft;
      w.waveBurstAngle += w.waveArcStep;
      // Deliberately does NOT touch `w.timer`: the rest of the burst is part of
      // the same shot, and recharging between arcs would turn a three-arc whip
      // into three separate attacks.
      continue;
    }

    if (w.attackType == AttackType::Nova && w.novaEchoesLeft > 0) {
      if (w.novaEchoTimer > 0.0F) {
        w.novaEchoTimer -= 1.0F / 60.0F;
        continue;
      }
      if (player_ == entt::null || !registry_.valid(player_)) {
        w.novaEchoesLeft = 0;
      } else {
        const auto& ept = registry_.get<Transform>(player_);
        spawnNovaRing(ept.x, ept.y, w, w.pierce + stats_.pierceAdd);
      }
      --w.novaEchoesLeft;
      w.novaEchoTimer = w.novaEcho;
      continue;
    }

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
          Projectile pr{};
          pr.damage = damage;
          pr.pierce = pierce;
          pr.life = w.life;
          pr.area = w.area;
          pr.strength = w.strength;
          pr.homing = w.homing;
          pr.bounces = w.bounces;
          // The ice carries its status onto every shot it fires; a weapon with
          // no chill leaves both at zero and the hit path skips it entirely.
          pr.chillMul = w.chillMul;
          pr.chillTime = w.chillTime;
          pr.auraRadius = w.auraRadius;
          pr.auraDps = w.auraDps;
          pr.auraTick = w.auraTick;
          pr.auraTimer = 0.0F;
          pr.auraChillMul = w.auraChillMul;
          pr.auraChillTime = w.auraChillTime;
          pr.reaimRange = w.reaimRange;
          pr.reaimTurn = w.reaimTurn;
          // One bend per body the bolt is allowed to touch. See the note on
          // `reaimLeft`: the bend is NOT paid out of the pierce, because that
          // would make `pierce` mean "a third fewer bodies" on this weapon and
          // exactly what it means on the other thirty-two.
          pr.reaimLeft = w.reaimRange > 0.0F ? pierce + 1 : 0;
          registry_.emplace<Projectile>(proj, pr);
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
          std::vector<float> hdist;
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
              hdist.push_back(dist2);
            }
          }
          // NEAREST FIRST, and this has to be a real sort rather than a comment
          // claiming the registry hands them over in range order -- it does not,
          // and the drill's whole rule is "the bit latches the NEAREST body in
          // the arc". Without the sort it latched whichever enemy the entity
          // store happened to yield first, so the same two bodies could be
          // drilled in either order depending on spawn order, and the weapon's
          // description was a lie.
          const auto byRange = [&hdist](std::size_t a, std::size_t b) {
            return hdist[a] < hdist[b];
          };
          std::vector<std::size_t> order(hits.size());
          for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
          std::sort(order.begin(), order.end(), byRange);
          if (!order.empty() && order.front() != 0) {
            std::vector<entt::entity> sortedE;
            std::vector<float> sortedX;
            std::vector<float> sortedY;
            sortedE.reserve(hits.size());
            sortedX.reserve(hits.size());
            sortedY.reserve(hits.size());
            for (const std::size_t i : order) {
              sortedE.push_back(hits[i]);
              sortedX.push_back(hx[i]);
              sortedY.push_back(hy[i]);
            }
            hits.swap(sortedE);
            hx.swap(sortedX);
            hy.swap(sortedY);
          }

          if (w.coneBite > 0.0F) {
            // --- The drill's bite --------------------------------------------
            // The Jackhammer Drill was "a cone" and so was the Ember Sprayer, so
            // one of them had to stop being a cone. The bite is the difference:
            // the bit commits to ONE body in the arc and chews it, and the longer
            // it stays buried the deeper it goes. The Sprayer washes a crowd
            // evenly and is therefore better at trash; the drill is the answer to
            // something with armour, and a real mistake against a swarm.
            //
            // So: keep only the latched target, and re-latch when it dies or
            // steps out of the arc. `hits` is already in range order, so the
            // first entry is the nearest.
            //
            // Two different ways to lose the latch, and they are treated
            // differently on purpose. If the target DIED, the bit carries its ramp
            // straight onto the next body -- that is what makes drilling a queue of
            // heavies worth so much more than drilling one, and it is the Bore
            // card's whole rule. If the target merely walked out of the arc, the
            // ramp restarts, because the bit never actually went deep on anything.
            const bool latchAlive = w.coneLatch != entt::null && registry_.valid(w.coneLatch);
            const bool stillLatched =
                latchAlive && std::find(hits.begin(), hits.end(), w.coneLatch) != hits.end();
            if (stillLatched) {
              ++w.coneBiteTicks;
            } else {
              w.coneLatch = hits.empty() ? entt::null : hits.front();
              if (w.coneLatch == entt::null) {
                w.coneBiteTicks = 0;
              } else if (latchAlive) {
                // Left the arc without dying: it was not drilled through.
                w.coneBiteTicks = 1;
              } else {
                // Drilled through: keep the depth. `>= 1` covers the case where
                // the slot was empty on the very first tick.
                w.coneBiteTicks = std::max(1, w.coneBiteTicks);
              }
            }
            if (w.coneLatch != entt::null) {
              const auto latchIt = std::find(hits.begin(), hits.end(), w.coneLatch);
              const std::size_t hi = static_cast<std::size_t>(latchIt - hits.begin());
              // The ramp is a multiple of the base, capped, so a bite can never
              // become the answer to everything by out-scaling the rest of the kit.
              const float ramp = std::min(
                  std::max(1.0F, w.coneBiteMax),
                  1.0F + static_cast<float>(w.coneBiteTicks - 1) * w.coneBite);
              // No crowd falloff: the bite hit exactly one body, which is the
              // entire point of committing to it.
              applyEnemyDamage(w.coneLatch, damage * ramp);
              spawnParticles(hx[hi], hy[hi], {0.88F, 0.92F, 1.0F, 1.0F}, 4, 3.0F);
            }
          } else {
            // AoE falloff: a cone that catches a crowd deals less to each target.
            const float coneMul = aoeFalloff(static_cast<int>(hits.size()), pierce);
            for (std::size_t hi = 0; hi < hits.size(); ++hi) {
              applyEnemyDamage(hits[hi], damage * coneMul);
              spawnParticles(hx[hi], hy[hi], {1.0F, 0.5F, 0.1F, 1.0F}, 4, 3.0F);
            }
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
          sw.color = w.coneBite > 0.0F
                         ? core::render::Color{0.88F, 0.92F, 1.0F, 1.0F}
                         : core::render::Color{1.0F, 0.55F, 0.10F, 1.0F};
          registry_.emplace<SweepEffect>(se, sw);

          // --- Hearthfire: scorch the ground the cone has swept -------------
          // Pools are dropped by DISTANCE the tip has travelled, not on a timer,
          // so walking with the sprayer lays a continuous trail and standing
          // still does not stack a hundred pools on one tile. The tip is where the
          // cone actually reaches, which is the point: the trail is in front of
          // you, where you are about to be.
          if (w.coneEmberAt > 0.0F) {
            const float tipX = pt.x + std::cos(baseAngle) * coneRange * 0.75F;
            const float tipY = pt.y + std::sin(baseAngle) * coneRange * 0.75F;
            if (w.coneEmberWalked <= 0.0F) {
              w.coneEmberWalked = w.coneEmberAt; // drop the first one immediately
            }
            const float dx = tipX - w.coneEmberLastX;
            const float dy = tipY - w.coneEmberLastY;
            w.coneEmberWalked += std::sqrt(dx * dx + dy * dy);
            w.coneEmberLastX = tipX;
            w.coneEmberLastY = tipY;
            // A while-loop, not an if: crossing a whole screen in one tick must
            // not silently drop a single pool and leave a dotted line.
            while (w.coneEmberWalked >= w.coneEmberAt) {
              w.coneEmberWalked -= w.coneEmberAt;
              // Cap the live pools. An uncapped trail is a fire map, which is a
              // different weapon: it removes the reason to keep moving.
              if (w.coneEmberLive >= 6) {
                entt::entity oldest = entt::null;
                float oldestAge = -1.0F;
                for (const auto ze : registry_.view<ZoneEffect>()) {
                  const auto& z = registry_.get<ZoneEffect>(ze);
                  if (z.weaponIndex != i) continue;
                  const float age = z.duration - z.timer;
                  if (oldest == entt::null || age > oldestAge) {
                    oldest = ze;
                    oldestAge = age;
                  }
                }
                if (oldest != entt::null) {
                  destroyQueue_.push_back(oldest);
                  --w.coneEmberLive;
                }
              }
              const auto pool = registry_.create();
              registry_.emplace<Transform>(pool, tipX, tipY, tipX, tipY);
              registry_.emplace<Radius>(pool, w.coneEmberRadius);
              Sprite ps{};
              ps.color = w.color;
              ps.color.a = 0.3F;
              ps.circle = true;
              registry_.emplace<Sprite>(pool, ps);
              ZoneEffect pz{};
              pz.dps = w.damage * 0.35F;
              pz.pierce = pierce;
              pz.radius = w.coneEmberRadius;
              pz.duration = w.coneEmberDuration;
              pz.tickRate = std::max(0.05F, w.coneTickRate);
              // Age, not remaining life. `updateZoneEffects` counts the timer UP
              // and destroys the zone when it reaches `duration`, so seeding the
              // timer with the duration made every pool born already dead: the
              // card created a scorch, it was reaped on the very next frame, and
              // the Ember Sprayer's whole Hearthfire rule did nothing at all.
              pz.timer = 0.0F;
              pz.color = w.color;
              pz.weaponIndex = i;
              registry_.emplace<ZoneEffect>(pool, pz);
              ++w.coneEmberLive;
            }
          }
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Bomb: {
        // Arcing projectile that explodes on impact
        const float gravity = 30.0F; // matches updateBombProjectiles
        // More projectiles = a taller arc; more pierce = a bigger blast.
        const float arcHeight = w.bombArcHeight * (1.0F + stats_.projAdd * 0.25F);
        // A shell thrown over an arc comes back down to its launch height after a
        // fixed flight time, whatever it was aimed at: t = sqrt(8H/g). Solving for
        // that ONCE per shot is what makes the landing point a decision instead of
        // an accident of the ballistics.
        //
        // It used to be an accident. The horizontal speed was always `proj_speed`
        // and the arc height alone decided where the shell ended up, so
        // `bomb_ahead` computed a target point, stored it on the shell, and then
        // never used it -- the mortar's "land four and a half units past the front
        // rank" was a number in a struct that no code read. The two bomb weapons
        // really were the same arcing shell with different arc heights, which is
        // exactly what they looked like.
        const float flightTime = std::sqrt(8.0F * arcHeight / gravity);
        for (int p = 0; p < count; ++p) {
          const float offset = (static_cast<float>(p) - static_cast<float>(count - 1) * 0.5F) * w.spread;
          const float angle = baseAngle + offset;
          const float dirX = std::cos(angle);
          const float dirY = std::sin(angle);
          // Where an uninterested shell lands: its own ballistic range, which is
          // what every bomb without a landing rule has always done.
          float land = w.speed * flightTime;
          if (w.bombAhead > 0.0F || w.bombOnTarget) {
            // Whoever is nearest the aim line, in front of the player and inside
            // the shell's own reach. A shell is not a laser, so a body a little
            // off the line still counts.
            float bestAlong = -1.0F;
            for (const auto e : registry_.view<Transform, Health, Enemy>()) {
              auto* eh = registry_.try_get<Health>(e);
              if (eh == nullptr || eh->hp <= 0.0F) continue;
              const auto& et = registry_.get<Transform>(e);
              const float dx = et.x - pt.x;
              const float dy = et.y - pt.y;
              const float along = dx * dirX + dy * dirY;
              if (along <= 0.0F) continue;
              const float perp = std::abs(dx * dirY - dy * dirX);
              if (perp > w.bombExplodeRadius) continue;
              if (bestAlong < 0.0F || along < bestAlong) bestAlong = along;
            }
            if (bestAlong > 0.0F) {
              // A hammer lands ON the body; a mortar lands this far PAST it, which
              // is the whole difference between them. Opposites, not degrees: one
              // answers the thing in front of you, the other answers the horde
              // behind it and treats the front rank as something to fire through.
              land = w.bombOnTarget ? bestAlong : bestAlong + w.bombAhead;
            }
          }
          const float tx = pt.x + dirX * land;
          const float ty = pt.y + dirY * land;
          const auto bomb = registry_.create();
          registry_.emplace<Transform>(bomb, pt.x, pt.y, pt.x, pt.y);
          // Solve the launch velocity for THAT landing point: cover `land` in
          // `flightTime`, and arc `arcHeight` high at the midpoint.
          const float vx = dirX * (land / flightTime);
          const float vy = gravity * flightTime * 0.5F;
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
        bp.splits = w.bounceSplits;
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
          // A hooked whip drags its catch IN instead of shoving it away. The two
          // are opposites, not degrees, and the barbs are the only thing in the
          // weapon's name that was not already accounted for: a whip that only
          // pushes is a weapon you can never start anything with. Opt-in
          // (`sweep_hook`) so the Soul Scythe -- whose entire job is a knockback
          // circle around its prey -- is untouched.
          if (w.sweepHook) {
            pushAngle = std::atan2(-dy, -dx);
            // Dead center: nothing to pull toward, so fall back to dragging
            // straight back along the aim rather than leaving it alone.
            if (dist2 < 1e-4F) pushAngle = baseAngle + kPi;
          }
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
          // The first strike is NOT applied here. It used to be, on the same line
          // that created the bolt, which meant the damage landed at the instant
          // the bolt was born and the ring that is supposed to warn about it came
          // after the fact. The bolt now owns its own landing hit
          // (ChainLightning::pendingFirst), so the sequence is: converge, land,
          // arc on. A lone enemy still takes damage, because the landing is not
          // the same event as arcing onward.
          spawnChainBolt(
              targetX, targetY, w,
              bestEnemy == entt::null ? 0U : static_cast<std::uint32_t>(bestEnemy));
        }
        w.timer = cooldown;
        break;
      }
      case AttackType::Wave: {
        // A crescent that leaves the player and travels. With waveCount == 1
        // that is a single outgoing slash; with more, the arcs are laid down
        // one after another over a beat (see the burst drain in fireWeapons),
        // so a three-arc whip reads as the tide coming around you rather than
        // as one flat simultaneous wall.
        if (w.waveBurstLeft <= 0) {
          w.waveBurstLeft = std::max(1, w.waveCount);
          w.waveBurstTimer = 0.0F;
          w.waveBurstAngle = baseAngle;
        }
        spawnWaveCrescent(w, w.waveBurstAngle, damage, pierce);
        --w.waveBurstLeft;
        w.waveBurstAngle += w.waveArcStep;
        w.timer = cooldown;
        break;
      }
      case AttackType::Nova: {
        // A nova is a ring that starts at the player and either grows out of it
        // (the Shock Core) or is cast wide and rushes back in (the Void Nova).
        // Both are the same entity; `novaContract` only decides which end it
        // starts at and which way `radius` moves.
        spawnNovaRing(pt.x, pt.y, w, pierce);
        // Discharge: the second ring is scheduled here rather than spawned, so it
        // is a genuine DELAY and not two rings on the same frame. The echo lands
        // after the first blast has had time to open a gap in the pack, which is
        // the whole reason a double is worth having over one bigger ring.
        if (w.novaEcho > 0.0F) {
          w.novaEchoesLeft = 1;
          w.novaEchoTimer = w.novaEcho;
        }
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
        float ix = targetX;
        float iy = targetY;
        // Siege Mortar (mortar + lure): the shells land on the BELL. Its whole
        // pitch is "the bell gathers the horde and the shells fall inside the
        // crowd it gathered", and without this the Ashfall was just a bigger
        // Inferno with no connection to the Grave Bell its recipe names. So
        // when a bell is standing, the barrage is centred on the bell instead
        // of on the nearest enemy -- which turns the pair into one machine
        // (gather, then cook) instead of two weapons that happen to coexist.
        if (w.infernoBindsToLure) {
          float bestBell2 = 0.0F;
          bool haveBell = false;
          for (const auto e : registry_.view<Transform, Lure>()) {
            const auto& lt = registry_.get<Transform>(e);
            const float d2 = (lt.x - pt.x) * (lt.x - pt.x) + (lt.y - pt.y) * (lt.y - pt.y);
            if (!haveBell || d2 < bestBell2) {
              haveBell = true;
              bestBell2 = d2;
              ix = lt.x;
              iy = lt.y;
            }
          }
        }
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
        ze.fromAbove = w.zoneFromAbove;
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

// Pins an enemy to `mul` of its own speed for `time` seconds. The stronger of
// two sources wins, and a weaker one still refreshes the timer, so a stream of
// light hits holds a chill that one heavy hit would have dropped.
void Game::applyChill(entt::entity e, float mul, float time) {
  if (!registry_.valid(e) || mul <= 0.0F || time <= 0.0F) return;
  auto* en = registry_.try_get<Enemy>(e);
  if (en == nullptr) return;
  if (mul <= en->slowMul || en->slowT <= 0.0F) en->slowMul = mul;
  en->slowT = std::max(en->slowT, time);
}

void Game::applyMarks(entt::entity e, float towardX, float towardY) {
  if (!registry_.valid(e)) return;
  // Frostbind: a hit chills, so a melee build gets an ice weapon's control for
  // free. The chill is shallower than a deep freeze on purpose -- this is meant
  // to be the everyday version of slow, not a second copy of the ice cards.
  if (stats_.markChillTime > 0.0F) {
    applyChill(e, kMarkChillMul, stats_.markChillTime);
  }
  // Emberbrand: a hit lights it up. Refreshed rather than accumulated, so the
  // weapon you field decides whether a target stays alight, and a slow heavy
  // hitter is honestly worse at this than a fast one.
  if (stats_.markBurnDps > 0.0F) {
    if (auto* en = registry_.try_get<Enemy>(e); en != nullptr) {
      en->burnDps = std::max(en->burnDps, stats_.markBurnDps);
      en->burnT = std::max(en->burnT, kMarkBurnWindow);
    }
  }
  // Hex: this body is softer than it was. Applied AFTER this hit is resolved, so
  // the ramp costs the first strike nothing and every strike after it more.
  if (stats_.markVuln > 0.0F) {
    if (auto* tr = registry_.try_get<EnemyTraits>(e); tr != nullptr) {
      tr->vuln = std::min(tr->vuln + stats_.markVuln, stats_.markVulnMax);
    }
  }
  // Armour Split: the target's own mitigation is taken off, permanently, a piece
  // at a time. It only bites if the target has any left -- a naked bat cannot be
  // made more naked -- which is what makes it a card for the late game, where the
  // roster finally grows armour to strip.
  if (stats_.markDefStrip > 0.0F) {
    if (auto* tr = registry_.try_get<EnemyTraits>(e); tr != nullptr) {
      tr->defense = std::max(0.0F, tr->defense - stats_.markDefStrip);
    }
  }
  (void)towardX;
  (void)towardY;
}

void Game::applyEnemyDamage(entt::entity e, float dmg, bool mark) {
  if (!registry_.valid(e)) return;
  auto* eh = registry_.try_get<Health>(e);
  if (eh == nullptr || eh->hp <= 0.0F) return;
  auto* tr = registry_.try_get<EnemyTraits>(e);
  const float hpBefore = eh->hp;
  // A hexed body is hit harder, and the multiplier is read BEFORE the mark is
  // applied again -- so the ramp is a ratchet that the current hit does not
  // enjoy, only the ones after it.
  if (tr != nullptr && tr->vuln > 0.0F) dmg *= 1.0F + tr->vuln;
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
  // The marks land on a hit that was actually dealt, not on an attempt. A body
  // that is already dead does not need to be hexed, and applying them before the
  // lethal check would make the last hit of every kill do free work.
  if (mark && eh->hp > 0.0F) {
    if (player_ != entt::null && registry_.valid(player_)) {
      const auto& ptx = registry_.get<Transform>(player_);
      applyMarks(e, ptx.x, ptx.y);
    }
  }
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

  // A body that dies while frozen comes apart. This is the ice weapon's actual
  // identity rather than a colour: the chill is not just a damage multiplier
  // that happens to be small, it is the setup for a burst. A frost shard that
  // only slowed things would read as a worse crossbow; one that leaves a corpse
  // that bursts into a fan of ice reads as ice, and it rewards the player for
  // chilling first and killing second instead of swapping one number for another.
  if (const auto* en = registry_.try_get<Enemy>(e); en != nullptr && en->slowT > 0.0F) {
    spawnShatterBurst(t.x, t.y, 0.55F * stats_.damageMul);
    spawnParticles(t.x, t.y, {0.70F, 0.94F, 1.0F, 1.0F}, 10, 4.5F);
  }

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
  // An elite-and-above leaves a box behind. It is not guaranteed on every elite
  // death -- a run that farms elites should not be showered -- but the roll is
  // generous enough that an elite is normally worth walking over, because the
  // box is the only reason an elite is a reward rather than just a tax.
  if (const auto* tr = registry_.try_get<EnemyTraits>(e); tr != nullptr &&
      tr->tier > 0) {
    // Guaranteed, not rolled. Elites are capped at two on screen now, so a box
    // per elite is a steady trickle rather than a flood, and a player who kills
    // an elite and watches nothing come out of it concludes the reward is
    // unreliable -- which is worse than a slightly too generous one.
    spawnChest(t.x, t.y, static_cast<int>(tr->tier));
    lastChestTimer_ = std::max(lastChestTimer_, 1.4F);
    lastChestGrants_ = 0;
  }
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

    // --- Re-aim steering -----------------------------------------------------
    // Runs AFTER homing, so a card that hands both to one weapon resolves the
    // conflict the same way every frame: the bend wins, because the bend is the
    // thing the weapon was built around and homing is the thing that was added.
    //
    // The bend is held rather than kicked (see Projectile::hasReaim), so this is
    // a few lines of steering that run for as many steps as the corner takes.
    if (pr.hasReaim && pr.reaimLeft > 0) {
      const float want = std::atan2(pr.reaimY - t.y, pr.reaimX - t.x);
      const float cur = std::atan2(v.y, v.x);
      float diff = want - cur;
      while (diff >  kPi) diff -= 2.0F * kPi;
      while (diff < -kPi) diff += 2.0F * kPi;
      if (std::abs(diff) < 0.02F) {
        // Arrived, or the target moved off the heading. Either way there is
        // nothing left to steer at; the next body it touches picks a new one.
        pr.hasReaim = false;
      } else {
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        const float bent = cur + std::clamp(diff, -pr.reaimTurn, pr.reaimTurn);
        v.x = std::cos(bent) * speed;
        v.y = std::sin(bent) * speed;
      }
    }

    t.x += v.x / 60.0F;
    t.y += v.y / 60.0F;
    pr.life -= 1.0F / 60.0F;
    if (pr.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    // --- The carried aura ----------------------------------------------------
    // A corona, not a status: while the shard is in the air it drags a bubble of
    // cold with it, and everything inside that bubble is chilled and ground down
    // whether the shard ever went through it or not. This is the difference
    // between "ice that hits" and "a line of ice you walk into slowly", and it
    // has to be a separate tick from the body hit -- otherwise the aura would
    // only ever pay for the enemies the shard was aimed at, which is the one
    // thing it is not for.
    if (pr.auraRadius > 0.0F) {
      pr.auraTimer += 1.0F / 60.0F;
      if (pr.auraTimer >= std::max(0.02F, pr.auraTick)) {
        pr.auraTimer = 0.0F;
        const float reach = pr.auraRadius;
        std::vector<entt::entity> auraHits;
        for (const auto en : registry_.view<Transform, Health, Radius, Enemy>()) {
          auto* eh = registry_.try_get<Health>(en);
          if (eh == nullptr || eh->hp <= 0.0F) continue;
          const auto& et = registry_.get<Transform>(en);
          const auto& er = registry_.get<Radius>(en);
          const float dx = et.x - t.x;
          const float dy = et.y - t.y;
          const float hitR = reach + er.r;
          if (dx * dx + dy * dy > hitR * hitR) continue;
          auraHits.push_back(en);
        }
        if (!auraHits.empty()) {
          // The same crowd falloff every other area attack uses: an aura that
          // ignores it would make every dense pack a damage jackpot and the
          // weapon's own answer would be "stand in the thick of it".
          const float mul = aoeFalloff(static_cast<int>(auraHits.size()), pr.pierce);
          const float dmg = pr.auraDps * std::max(0.02F, pr.auraTick) * mul *
                            stats_.damageMul * momentumDamageMul_;
          for (const auto en : auraHits) {
            if (!registry_.valid(en)) continue;
            applyEnemyDamage(en, dmg);
            if (pr.auraChillTime > 0.0F) {
              applyChill(en, pr.auraChillMul, pr.auraChillTime);
            }
          }
          if (auraHits.size() <= 6) {
            for (const auto en : auraHits) {
              if (!registry_.valid(en)) continue;
              const auto& et = registry_.get<Transform>(en);
              spawnParticles(et.x, et.y, {0.65F, 0.88F, 1.0F, 1.0F}, 1, 1.2F);
            }
          }
        }
      }
    }

    bool spent = false;
    hash_.forEachNear(t.x, t.y, r.r + 0.7F, [&](std::uint32_t id) {
      if (spent) return;
      const auto enemy = static_cast<entt::entity>(id);
      if (!registry_.valid(enemy) || !registry_.all_of<Enemy>(enemy)) return;
      auto* eh = registry_.try_get<Health>(enemy);
      if (eh == nullptr || eh->hp <= 0.0F) return;
      // A BENDING bolt is still inside the body it just left when the next step
      // starts, so it has to be told what it has already paid. A straight bolt
      // is outside that reach by then, which is why this costs it nothing.
      if (pr.reaimedAlready(static_cast<int>(enemy))) return;
      const auto& et = registry_.get<Transform>(enemy);
      const auto& er = registry_.get<Radius>(enemy);
      const float dx = et.x - t.x;
      const float dy = et.y - t.y;
      const float hitR = r.r + er.r;
      if (dx * dx + dy * dy > hitR * hitR) return;

      pr.rememberReaimed(static_cast<int>(enemy));

      // Damage. Lifesteal no longer rolls here — it fires on the kill itself
      // (see killEnemy), so a many-hit weapon cannot heal per tick. `dealt` is
      // still needed to know the hit actually landed (for chain lightning).
      const float hpBefore = eh->hp;
      applyEnemyDamage(enemy, pr.damage);
      const float dealt = hpBefore - eh->hp;
      // Chill, for the weapons that carry it. A piercing shard hits five bodies,
      // so the status has to be applied per hit here rather than once on spawn.
      if (pr.chillTime > 0.0F) applyChill(enemy, pr.chillMul, pr.chillTime);

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

      // --- Re-aim (the Storm Caller) -----------------------------------------
      // The bolt does not carry on through: it BENDS. The crossbow's pierce
      // becomes the storm's steering, so the weapon inherits the wand's promise
      // -- it does not sail past the pack -- while keeping the crossbow's
      // punch-through, which is the part that makes the bend cost something.
      //
      // Three things are deliberate here:
      //  * The bend is BOUNDED, by a budget seeded from the pierce -- not
      //    unbounded, so a bolt dropped into a crowd of forty still stops when
      //    its own statline says it has run out.
      //  * The turn is limited (`reaimTurn`), not a snap to the new heading. A
      //    perfect seeker would make the crossbow half of the pairing pointless;
      //    a limited one can still lose a fast body that is already peeling away.
      //  * Every body already paid is off the table, so the bolt cannot double up
      //    on the one it is standing in or bank off the one it is trying to leave.
      if (pr.reaimRange > 0.0F && pr.reaimTurn > 0.0F && pr.reaimLeft > 0) {
        float bestDist2 = pr.reaimRange * pr.reaimRange;
        entt::entity next = entt::null;
        for (const auto oe : registry_.view<Transform, Health, Enemy>()) {
          // Everything this bolt has already paid is off the table, not just the
          // body it is standing in. A bolt that bent at one body and is now
          // leaning back toward it would otherwise bank off the same target
          // twice in a row and read as a stutter.
          if (pr.reaimedAlready(static_cast<int>(oe))) continue;
          auto* oh = registry_.try_get<Health>(oe);
          if (oh == nullptr || oh->hp <= 0.0F) continue;
          const auto& ot = registry_.get<Transform>(oe);
          const float odx = ot.x - t.x;
          const float ody = ot.y - t.y;
          const float od2 = odx * odx + ody * ody;
          // Strictly better than the best so far, and only if it is genuinely in
          // reach -- `bestDist2` starts AT the range, so a tie loses.
          if (od2 < bestDist2) {
            bestDist2 = od2;
            next = oe;
          }
        }
        if (next != entt::null) {
          const auto& ot = registry_.get<Transform>(next);
          // Hand the bend to the steering block above rather than applying it
          // here. The heading it leaves on is the old one -- it will be turned
          // over the next few steps, at the corner rate the data card states.
          pr.reaimX = ot.x;
          pr.reaimY = ot.y;
          pr.hasReaim = true;
          --pr.reaimLeft;
          // A crack of light along the course it just left, so the bend is
          // something you can see rather than something the numbers do.
          const float heading = std::atan2(v.y, v.x);
          for (int sIdx = 0; sIdx < 3; ++sIdx) {
            const float back = 0.18F + 0.14F * static_cast<float>(sIdx);
            spawnParticles(t.x - std::cos(heading) * back, t.y - std::sin(heading) * back,
                           {0.65F, 1.0F, 1.0F, 1.0F}, 2, 3.0F);
          }
        }
      }

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
    // Blade Vortex: find the gap in the ring. The blades are re-spaced evenly, so
    // the midpoint between any blade and its neighbour IS a gap -- taking the
    // first owned blade and stepping halfway is exact, not approximate, and it
    // costs one scan of entities the loop is already touching.
    bool hasWindow = false;
    float gapAngle = 0.0F;
    if (w.orbitWindow) {
      const int blades = std::max(1, w.projectiles + stats_.projAdd);
      for (const auto e : registry_.view<OrbitBlade>()) {
        if (registry_.get<OrbitBlade>(e).weaponIndex != orbitSlot) continue;
        gapAngle = registry_.get<OrbitBlade>(e).angle + kPi / static_cast<float>(blades);
        hasWindow = true;
        break;
      }
      // No blade to measure from (should not happen, the slot owns a ring): fall
      // back to the full interior rather than silently deleting the whirl.
      hasWindow = hasWindow && blades > 1;
    }
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
      if (hasWindow) {
        // Only the gap gets the interior damage. This is the whole point of the
        // card: without it the hub is a uniform everywhere-equal grind you cannot
        // aim, which is a worse version of the ring. With it, the safe angle
        // becomes the killing angle, so a faster ring is worth something.
        const float ang = std::atan2(dy, dx);
        if (std::abs(std::remainderf(ang - gapAngle, 2.0F * kPi)) > kOrbitWindowHalf) continue;
      }
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
    const float inner = owned ? weapons_[hb.weaponIndex].haloInner : hb.inner;

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
    // The spoke is a segment, not a ray, once it has an inner dead zone. Clamping
    // the far end too matters: a `haloInner` larger than the reach would otherwise
    // produce a negative-length segment whose dot products still pass `proj > 0`
    // and damage a body behind the player.
    const float near = std::min(inner, len * 0.9F);
    const float far = std::max(near + 0.05F, len);

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
      if (proj < near || proj > far) continue;
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
    // Collapse settings are read live like everything else, so a card that adds
    // them lands on wells that are already spinning rather than on the next
    // weapon. A well with collapseAt <= 0 never charges and never bursts, which
    // is what keeps the Void Gyre a patient permanent drag.
    const float collapseAt = owned ? w.vortexCollapseAt : vx.collapseAt;
    const float burstDamage = owned ? w.vortexBurstDamage : vx.burstDamage;
    const float burstRadius = owned ? w.vortexBurstRadius : vx.burstRadius;

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

    // --- Collapse clock -------------------------------------------------------
    // Accumulated EVERY frame, not on the damage tick, because `collapseAt` is in
    // seconds and "hoards for three seconds" has to mean three seconds. It used
    // to ride the damage tick's `continue` above, which made the real clock
    // `collapseAt / tickRate` long -- thirty seconds for a weapon whose card says
    // three -- and then it was also paid `dt` per tick rather than per frame on
    // top of that. Two unit errors stacked, and the implosion was effectively
    // never seen in a run.
    if (collapseAt > 0.0F) {
      vx.charge += dt;
      if (vx.charge >= collapseAt) {
        vx.charge = 0.0F;
        // Reopen somewhere else. Not at a random angle: a well that can land on
        // top of the player every time is a different weapon from one that keeps
        // its distance, and the orbit radius is the player's only control over
        // that. Half a turn ahead is enough to read as "it moved" without ever
        // being unpredictable.
        vx.angle += kPi;
        t.x = pt.x + std::cos(vx.angle) * vx.orbitRadius;
        t.y = pt.y + std::sin(vx.angle) * vx.orbitRadius;
        const float rBurst = burstRadius > 0.0F ? burstRadius : vx.radius * 2.2F;
        // The collapse is worth several seconds of the steady grind, or it is not
        // an event -- it is a slightly louder tick.
        const float dmg = burstDamage > 0.0F
                              ? burstDamage * stats_.damageMul * momentumDamageMul_
                              : damage * 3.0F * stats_.damageMul * momentumDamageMul_;
        std::vector<entt::entity> collapsed;
        for (const auto en : registry_.view<Transform, Health, Radius, Enemy>()) {
          auto* eh = registry_.try_get<Health>(en);
          if (eh == nullptr || eh->hp <= 0.0F) continue;
          const auto& et = registry_.get<Transform>(en);
          const auto& er = registry_.get<Radius>(en);
          const float dx = et.x - t.x;
          const float dy = et.y - t.y;
          if (dx * dx + dy * dy > (rBurst + er.r) * (rBurst + er.r)) continue;
          collapsed.push_back(en);
        }
        // A collapse that catches the whole knot is the reward for having gathered
        // it, so no falloff here: the pull did the crowd control and the burst
        // collects.
        for (const auto en : collapsed) {
          if (!registry_.valid(en)) continue;
          applyEnemyDamage(en, dmg);
          const auto& et = registry_.get<Transform>(en);
          spawnParticles(et.x, et.y, {vx.color.r, vx.color.g, vx.color.b, 1.0F}, 8, 5.0F);
        }
        // The implosion itself: a ring of debris at the well's old position, which
        // is what sells the collapse as an EVENT rather than as the regular tick
        // being louder for a frame.
        for (int k = 0; k < 14; ++k) {
          const float a = 2.0F * kPi * static_cast<float>(k) / 14.0F;
          spawnParticles(t.x + std::cos(a) * rBurst * 0.5F,
                          t.y + std::sin(a) * rBurst * 0.5F,
                          {vx.color.r, vx.color.g, vx.color.b, 1.0F}, 3, 6.0F);
        }
      }
    }

    // --- Core damage ---------------------------------------------------------
    // `tickRate` is an INTERVAL in seconds here, exactly as it is on the zone, the
    // lure and the nova. It used to be treated as a rate -- the timer was reset to
    // `1 / tickRate` -- which turned a tenth of a second into ten seconds between
    // damage ticks. Both vortex weapons were dealing a hundredth of the damage
    // their numbers describe, which is most of why they read as "the same weapon
    // twice" and did not feel like weapons at all.
    vx.tickTimer -= dt;
    if (vx.tickTimer > 0.0F) continue;
    const float tickSpan = std::max(0.02F, vx.tickRate);
    vx.tickTimer = tickSpan;

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

    // A shell always goes off where its arc brings it back down to launch
    // height, and nothing else detonates it early.
    //
    // There used to be a contact test here as well: a fuse-less shell popped the
    // instant it overlapped a body. That was the right idea for a shell whose
    // landing point had never been decided -- "explodes on impact" had to mean
    // SOMETHING, and touching a body was the only candidate. It is now strictly
    // worse than the landing rule, because the launch velocity is now solved for
    // a chosen landing point: the Runic Hammer already flies to the body it was
    // aimed at, and the overlap test fires when the shell is still half a unit in
    // the air on the way down. The blast lands in roughly the right place, but
    // the spot it lands in depends on how fast the shell happens to be falling,
    // so "lands on the thing you aimed at" becomes a claim about ballistics
    // rather than a rule. The landing rule is the whole answer now: a hammer
    // answers the thing in front of you, a mortar answers the horde behind it,
    // and both of them say so on the data card.
    //
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
    // tick, everything within trailWidth/2 of the blade's whole remembered path
    // takes damage.
    //
    // The path, not this frame's segment. At 60Hz and twenty units a second the
    // per-frame segment is a third of a unit long, which hits exactly what the
    // blade's own contact test already hits -- so the trail was pure decoration
    // and the weapon was, as it looked, a prettier shuriken. Remembering the last
    // few positions is what makes the trail a swath the whole line burns at once,
    // and that swath is the Pulsar's actual identity.
    if (bp.trailWidth > 0.0F) {
      // Push this frame's position, keeping the buffer newest-first by index.
      bp.trailPathX[bp.trailHead] = t.x;
      bp.trailPathY[bp.trailHead] = t.y;
      bp.trailHead = (bp.trailHead + 1) % BoomerangProjectile::kTrailSamples;
      if (bp.trailCount < BoomerangProjectile::kTrailSamples) ++bp.trailCount;

      bp.trailTimer += 1.0F / 60.0F;
      if (bp.trailTimer >= bp.trailTick) {
        bp.trailTimer = 0.0F;
        const float halfW = bp.trailWidth * 0.5F;
        const int n = bp.trailCount;
        auto trailView = registry_.view<Transform, Health, Radius, Enemy>();
        std::vector<entt::entity> thits;
        for (const auto te : trailView) {
          const auto& tt = trailView.get<Transform>(te);
          const auto& tr = trailView.get<Radius>(te);
          const float hitR = halfW + tr.r;
          const float hitR2 = hitR * hitR;
          // Nearest approach to the polyline through the remembered path. Any
          // single segment close enough counts, which is a corridor rather than
          // a moving dot.
          bool onTrail = false;
          for (int s = 0; s + 1 < n && !onTrail; ++s) {
            // Walk from oldest to newest so the arc is contiguous. `head` is the
            // next slot to write, so the oldest live sample is the one just
            // before it.
            const int i0 = (bp.trailHead - n + s + BoomerangProjectile::kTrailSamples * 2) %
                           BoomerangProjectile::kTrailSamples;
            const int i1 = (i0 + 1) % BoomerangProjectile::kTrailSamples;
            const float ax = bp.trailPathX[i0];
            const float ay = bp.trailPathY[i0];
            const float segx = bp.trailPathX[i1] - ax;
            const float segy = bp.trailPathY[i1] - ay;
            const float segLen2 = segx * segx + segy * segy;
            const float qx = tt.x - ax;
            const float qy = tt.y - ay;
            float d2 = 0.0F;
            if (segLen2 < 1e-6F) {
              d2 = qx * qx + qy * qy;
            } else {
              float fParam = (qx * segx + qy * segy) / segLen2;
              fParam = fParam < 0.0F ? 0.0F : (fParam > 1.0F ? 1.0F : fParam);
              const float cx2 = qx - fParam * segx;
              const float cy2 = qy - fParam * segy;
              d2 = cx2 * cx2 + cy2 * cy2;
            }
            if (d2 <= hitR2) onTrail = true;
          }
          if (!onTrail && n == 1) {
            // One sample and no segment: still hit whatever the blade is on top
            // of, so the trail does not silently do nothing on its first tick.
            const float qx = tt.x - t.x;
            const float qy = tt.y - t.y;
            onTrail = qx * qx + qy * qy <= hitR2;
          }
          if (!onTrail) continue;
          auto* teh = registry_.try_get<Health>(te);
          if (teh == nullptr || teh->hp <= 0.0F) continue;
          thits.push_back(te);
        }
        const float trailMul = aoeFalloff(static_cast<int>(thits.size()), bp.pierce);
        for (const auto te : thits) {
          if (!registry_.valid(te)) continue;
          applyEnemyDamage(te, bp.trailDamage * trailMul);
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
    } else if (bp.bounceCount < bp.maxBounces) {
      // A finite ricochet aims ITSELF every frame, not only on the frames it
      // hits something. This used to live inside the collision callback, which
      // meant an orb crossing open ground never re-aimed at all: it kept the
      // heading it happened to have, sailed off the map, and the player watched
      // nothing happen for the rest of a multi-second life. That is the whole
      // complaint -- a bouncing orb you cannot get rid of and that is not
      // hitting anything.
      //
      // A target inside ricochet range is a real bounce and snaps the heading.
      // Anything further away only nudges it (kBounceReturnTurn), which keeps
      // the arc reading as a puck searching for the next body rather than as a
      // homing missile, and turns it back toward the player when the room is
      // empty.
      float inX = 0.0F, inY = 0.0F;
      float in2 = bp.bounceRange * bp.bounceRange;
      bool inRange = false;
      float anyX = 0.0F, anyY = 0.0F;
      float any2 = std::numeric_limits<float>::max();
      bool haveAny = false;
      // Do not aim at the body just hit, or the orb grinds against it instead of
      // ricocheting away -- the same rule the collision test enforces.
      const float skipX = bp.lastHitX;
      const float skipY = bp.lastHitY;
      const bool skip = bp.bounceCount > 0;
      for (const auto oe : registry_.view<Transform, Enemy, Health>()) {
        const auto& ot = registry_.get<Transform>(oe);
        const auto* oeh = registry_.try_get<Health>(oe);
        if (oeh == nullptr || oeh->hp <= 0.0F) continue;
        if (skip) {
          const float sdx = ot.x - skipX;
          const float sdy = ot.y - skipY;
          if (sdx * sdx + sdy * sdy < 0.1F) continue;
        }
        const float cdx = ot.x - t.x;
        const float cdy = ot.y - t.y;
        const float cd2 = cdx * cdx + cdy * cdy;
        if (cd2 < in2) {
          in2 = cd2;
          inX = ot.x;
          inY = ot.y;
          inRange = true;
        }
        if (cd2 < any2) {
          any2 = cd2;
          anyX = ot.x;
          anyY = ot.y;
          haveAny = true;
        }
      }
      // The player is the last resort: an orb with nowhere to go comes back into
      // the fight instead of to the edge of the world.
      float goalX = inX, goalY = inY;
      if (!inRange) {
        if (haveAny) {
          goalX = anyX;
          goalY = anyY;
        } else if (player_ != entt::null && registry_.valid(player_)) {
          const auto& ppt = registry_.get<Transform>(player_);
          goalX = ppt.x;
          goalY = ppt.y;
        }
      }
      const float gdx = goalX - t.x;
      const float gdy = goalY - t.y;
      if (gdx * gdx + gdy * gdy > 0.01F) {
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        if (speed > 1e-3F) {
          const float want = std::atan2(gdy, gdx);
          const float have = std::atan2(v.y, v.x);
          float delta = want - have;
          // Wrap to (-pi, pi] so a target behind the orb does not spin it the
          // long way round.
          constexpr float kPi = 3.14159265F;
          while (delta > kPi) delta -= 2.0F * kPi;
          while (delta < -kPi) delta += 2.0F * kPi;
          const float step =
              std::clamp(delta, -kBounceTurn * (1.0F / 60.0F), kBounceTurn * (1.0F / 60.0F));
          const float a = have + step;
          v.x = std::cos(a) * speed;
          v.y = std::sin(a) * speed;
        }
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

      // Split: this is what separates a Chaos Orb from a Pinball Puck. The puck
      // is one ball that ricochets; the orb COMES APART on every impact, so one
      // shot fills the room with fragments that each go on to hit and split
      // again. The cascade is bounded three ways -- depth, bounce budget and a
      // shorter life per generation -- so it decays instead of multiplying into
      // a screen full of orbs that delete the game by themselves.
      if (bp.splits > 0 && !bp.splitUsed && bp.depth < kMaxBounceSplitDepth) {
        // Paid out once per piece. A fragment that broke again on every bounce
        // would multiply each generation by its remaining bounces rather than
        // by two, and the total would be thousands of orbs from a single shot.
        bp.splitUsed = true;
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        // Fragments carry ON in the direction the parent was travelling, fanned
        // around it. Using the offset as an absolute heading would send them all
        // off at a fixed world angle regardless of where the orb came from,
        // which is not what coming apart looks like.
        const float heading = speed > 1e-3F ? std::atan2(v.y, v.x) : 0.0F;
        for (int k = 0; k < bp.splits; ++k) {
          // A fan of fragments rather than a symmetric ring: a ring looks like
          // a particle effect, a fan looks like something coming apart.
          const float spreadAngle = (static_cast<float>(k) -
                                     static_cast<float>(bp.splits - 1) * 0.5F) * 0.7F;
          const float a = heading + spreadAngle;
          const auto child = registry_.create();
          registry_.emplace<Transform>(child, et.x, et.y, et.x, et.y);
          registry_.emplace<Velocity>(child, std::cos(a) * speed, std::sin(a) * speed);
          // Fragments are visibly smaller than the parent, which is what makes
          // the split legible at a glance.
          registry_.emplace<Radius>(child, r.r * 0.62F);
          Sprite cs{};
          cs.color = bp.color;
          cs.circle = true;
          registry_.emplace<Sprite>(child, cs);
          BounceProjectile cbp = bp;
          cbp.depth = bp.depth + 1;
          cbp.splits = bp.splits;
          cbp.splitUsed = false; // a fresh piece is allowed its own break
          // A fragment's life is a share of the parent's AND a hard ceiling.
          // Without the ceiling the root orb's 12s life would dominate and the
          // whole cascade would stay in the air for as long as the root, which
          // is not a decaying cascade -- it is a slow-motion multiplication.
          cbp.life = std::min(kMaxSplitLife, std::max(0.25F, bp.life * 0.45F));
          cbp.maxBounces = std::max(1, bp.maxBounces - 1);
          cbp.damage = bp.damage * 0.55F;
          // The fragment must not be locked to the parent as its last victim, or
          // every generation would refuse the enemy it was born on.
          cbp.bounceCount = 0;
          cbp.lastHitX = et.x;
          cbp.lastHitY = et.y;
          registry_.emplace<BounceProjectile>(child, cbp);
        }
      }

      // The eternal orb has no bounce budget and no re-target pass — the
      // steering above keeps it engaged forever.
      if (!bp.infinite && bp.bounceCount >= bp.maxBounces) {
        spent = true;
        destroyQueue_.push_back(e);
        return;
      }
      // No aiming here: a finite ricochet re-aims itself every frame in the
      // pass above. Doing it only on the frame of a hit is what let an orb sail
      // off the arena, and doing it in BOTH places is how that hid for so long —
      // the copy that used to live here was correct and still unreachable for
      // every orb that was not touching anything.

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

    // Beat 1: converge. Nothing is damaged yet; the renderer is drawing a ring
    // shrinking onto the target. The bolt only starts hopping when the ring
    // closes, so the strike is something you watch arrive rather than something
    // that has already happened by the time you look.
    if (cl.telegraph > 0.0F) {
      cl.telegraph -= 1.0F / 60.0F;
      continue;
    }
    // Beat 2: the sky drop draws itself down. The bolt is already damaging
    // during it -- the ring is the warning, not the damage -- and the renderer
    // scales the drop's brightness by this.
    if (cl.strike < 1.0F) {
      cl.strike = std::min(1.0F, cl.strike + (1.0F / 60.0F) / kChainStrike);
    }

    // Pay the strike the bolt was aimed at. See ChainLightning::pendingFirst:
    // this used to be done by fireWeapons the instant the bolt was created, so
    // the damage always landed before the player could see anything. A fork
    // carries nothing, so only the trunk pays.
    if (cl.pendingFirst != 0) {
      const entt::entity first{cl.pendingFirst};
      cl.pendingFirst = 0;
      if (registry_.valid(first) && registry_.all_of<Health, Enemy>(first)) {
        auto& fh = registry_.get<Health>(first);
        if (fh.hp > 0.0F) {
          const float damage = cl.damage * stats_.damageMul * momentumDamageMul_;
          applyEnemyDamage(first, damage);
          const auto& ft = registry_.get<Transform>(first);
          spawnParticles(ft.x, ft.y, {0.55F, 1.0F, 1.0F, 1.0F}, 8, 4.0F);
          // Shatter on the landing hit, which is where a slug coming apart reads
          // best: the first thing the player saw was the fan.
          if (cl.shatter > 0) {
            spawnChainShatter(cl, ft.x, ft.y, 0.0F, cl.damage * stats_.damageMul *
                                                     momentumDamageMul_ * 0.5F);
            cl.shatter = 0; // one fan per slug
          }
        }
      }
    }

    // Out of jumps: stop dealing damage, but hang around so the bolt is actually
    // visible. The whole traversal is 0.05s per hop, so without this the effect
    // is over before the player has registered it.
    if (cl.jumpsDone >= cl.maxJumps) {
      cl.linger -= 1.0F / 60.0F;
      if (cl.linger <= 0.0F) {
        destroyQueue_.push_back(e);
      }
      continue;
    }

    cl.timer += 1.0F / 60.0F;
    if (cl.timer < 0.05F) continue; // small delay between jumps
    cl.timer = 0.0F;

    // Find next target. The enemy this bolt just left is only taken if there is
    // nothing else in range: a pocket of two enemies would otherwise make the
    // bolt strobe between them forever, and "it keeps hitting the same two" is
    // not what lightning is supposed to look like.
    float bestDist2 = cl.jumpRange * cl.jumpRange;
    float bestRepeat2 = cl.jumpRange * cl.jumpRange;
    entt::entity nextTarget = entt::null;
    entt::entity repeatTarget = entt::null;
    auto enemies = registry_.view<Transform, Health, Enemy>();
    for (const auto oe : enemies) {
      if (oe == e) continue; // shouldn't happen
      const auto& ot = enemies.get<Transform>(oe);
      const auto* oeh = registry_.try_get<Health>(oe);
      if (!oeh || oeh->hp <= 0.0F) continue;
      const float dx = ot.x - t.x;
      const float dy = ot.y - t.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 >= cl.jumpRange * cl.jumpRange) continue;
      if (entt::to_entity(oe) == cl.lastTarget) {
        if (d2 < bestRepeat2) {
          bestRepeat2 = d2;
          repeatTarget = oe;
        }
        continue;
      }
      if (d2 < bestDist2) {
        bestDist2 = d2;
        nextTarget = oe;
      }
    }
    if (nextTarget == entt::null) nextTarget = repeatTarget;

    if (nextTarget == entt::null) {
      // Ran out of things to arc to, which is the COMMON case: an arc dead-ends
      // against empty ground well before it uses up maxJumps. This used to
      // destroy the bolt on the spot, which is why a bolt into a scattered crowd
      // vanished the instant it hit the last body -- the exact moment the player
      // most wanted to see it. Instead the bolt is marked spent and finishes its
      // linger like any other end, which is the whole of "it disappears too
      // early": dead-ending was bypassing the linger entirely.
      cl.jumpsDone = cl.maxJumps;
      cl.linger -= 1.0F / 60.0F;
      if (cl.linger <= 0.0F) {
        destroyQueue_.push_back(e);
      }
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

      // Shatter: the slug comes apart on the impact it just made. Paid out
      // once -- the count is cleared as it is spent -- so a long-jumping bolt
      // cannot throw a fan of shards on every single hop. The trunk normally
      // spends it on its landing (above, where there is no incoming direction to
      // point the fan along, so it bursts radially instead), which leaves this
      // as the fallback for a fork.
      if (cl.shatter > 0) {
        // Heading of the segment just travelled, so the fan carries on the way
        // the bolt was already going rather than spraying every which way.
        float heading = 0.0F;
        const float segX = targetT.x - t.x;
        const float segY = targetT.y - t.y;
        if (std::abs(segX) + std::abs(segY) > 1e-4F) {
          heading = std::atan2(segY, segX);
        }
        const float shardDamage =
            cl.damage * stats_.damageMul * momentumDamageMul_ *
            std::powf(cl.damageMul, static_cast<float>(cl.jumpsDone)) * 0.5F;
        spawnChainShatter(cl, targetT.x, targetT.y, heading, shardDamage);
        cl.shatter = 0; // one fan per slug
      }
    }

    // Move chain lightning position to target. px/py are the previous-tick
    // position used for render interpolation, and they were never written here:
    // the renderer was lerping from (0, 0), so every bolt was drawn streaking
    // in from the world origin.
    t.px = t.x;
    t.py = t.y;
    const auto& targetT = registry_.get<Transform>(nextTarget);
    t.x = targetT.x;
    t.y = targetT.y;
    cl.jumpsDone++;
    cl.lastTarget = entt::to_entity(nextTarget);
  }
}

// Travelling crescents. Each wave moves along its own heading and hits every
// enemy it passes exactly once, shoving it along the direction of travel -- so
// the crowd ends up displaced downrange instead of just taking damage, which is
// the thing a player-centred nova can never do.
void Game::updateWaveEffects() {
  auto view = registry_.view<Transform, WaveEffect, Radius>();
  for (const auto e : view) {
    auto& t = view.get<Transform>(e);
    auto& wv = view.get<WaveEffect>(e);

    t.px = t.x;
    t.py = t.y;
    const float step = wv.speed / 60.0F;
    t.x += std::cos(wv.angle) * step;
    t.y += std::sin(wv.angle) * step;
    wv.travelled += step;
    wv.life -= 1.0F / 60.0F;

    if (wv.travelled >= wv.range || wv.life <= 0.0F) {
      destroyQueue_.push_back(e);
      continue;
    }

    const float dmg = wv.damage * stats_.damageMul * momentumDamageMul_;
    auto enemies = registry_.view<Transform, Health, Enemy>();
    std::vector<entt::entity> hits;
    for (const auto oe : enemies) {
      const auto& ot = enemies.get<Transform>(oe);
      const auto* oeh = registry_.try_get<Health>(oe);
      if (!oeh || oeh->hp <= 0.0F) continue;
      // A wave hits each enemy ONCE. The band is as long as it is wide, so
      // without this a target standing in the path would be struck on every
      // single frame until the wave moved past it.
      if (wv.alreadyHit(entt::to_entity(oe))) continue;
      const float dx = ot.x - t.x;
      const float dy = ot.y - t.y;
      // A crescent, not a disc: the hit box is the wave's path swept back into
      // a band, so what it has already passed is not re-hit.
      const float along = dx * std::cos(wv.angle) + dy * std::sin(wv.angle);
      if (along > 0.0F) continue; // only the half behind the front
      const float across = -dx * std::sin(wv.angle) + dy * std::cos(wv.angle);
      if (std::abs(across) > wv.width * 0.5F) continue;
      if (along < -wv.width) continue;
      hits.push_back(oe);
    }
    for (const auto oe : hits) {
      const auto& ot = registry_.get<Transform>(oe);
      applyEnemyDamage(oe, dmg);
      // Shove along the wave's own heading, not away from the player: a wave
      // is a push, and where it pushes things is the point. A HOOKING wave
      // pushes across the fan instead of along it (see WaveEffect::hookPull),
      // which is the difference between three arcs that each clear their own
      // line and three arcs that hand the same catch down the line.
      const bool hooking = wv.hookPull > 0.0F;
      const float pushAngle =
          wv.angle + (hooking ? wv.hookSide * WaveEffect::kHookAngle : 0.0F);
      applyKnockback(oe, pushAngle, hooking ? wv.knockback * wv.hookPull : wv.knockback);
      wv.rememberHit(entt::to_entity(oe));
      spawnParticles(ot.x, ot.y, {wv.color.r, wv.color.g, wv.color.b, 1.0F}, 4, 3.0F);
    }
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

    // The end condition follows the sign of expandSpeed rather than assuming the
    // ring grows. A contracting ring STARTS at maxRadius, so the old
    // "radius >= maxRadius means it is done" test fired on frame one and deleted
    // the Void Nova before it moved -- which is why this cannot just be a
    // negative number on a positive-only check.
    const bool contracting = nr.expandSpeed < 0.0F;
    const bool finished = contracting ? (nr.radius <= 0.0F) : (nr.radius >= nr.maxRadius);

    if (finished) {
      if (!nr.burstDone) {
        // The centre detonation. `burstDone` is what makes it one hit: without it
        // the ring would sit inside its own core radius for several ticks and pay
        // the burst again each one, which is a different weapon than the one
        // described.
        nr.burstDone = true;
        const float burst = nr.burstDamage * stats_.damageMul * momentumDamageMul_;
        // The blast is at the player, because that is where the ring arrived.
        auto enemyView = registry_.view<Transform, Health, Radius, Enemy>();
        std::vector<entt::entity> bhits;
        for (const auto en : enemyView) {
          auto* eh = registry_.try_get<Health>(en);
          if (eh == nullptr || eh->hp <= 0.0F) continue;
          const auto& et = enemyView.get<Transform>(en);
          const float dx = et.x - pt.x;
          const float dy = et.y - pt.y;
          // Generous: the ring dragged them in, so the payoff has to cover the
          // knot it actually made rather than the empty ground it was cast over.
          if (dx * dx + dy * dy > nr.maxRadius * nr.maxRadius) continue;
          bhits.push_back(en);
        }
        const float burstMul = aoeFalloff(static_cast<int>(bhits.size()), nr.pierce);
        for (const auto en : bhits) {
          if (!registry_.valid(en)) continue;
          applyEnemyDamage(en, burst * burstMul);
          const auto& et = registry_.get<Transform>(en);
          spawnParticles(et.x, et.y, {nr.color.r, nr.color.g, nr.color.b, 1.0F}, 6, 4.0F);
        }
      }
      destroyQueue_.push_back(e);
      continue;
    }

    // The inward drag, every frame and not on the damage tick. A ring that only
    // pulls when it deals damage cannot gather anybody: at 0.12s per tick and
    // 4 units a second that is less than half a unit of travel per hit, which is
    // not enough to move a rank. This is the same reason the vortex applies its
    // pull continuously, and it is the whole of what makes the contracting ring
    // worth anything over the expanding one.
    if (nr.pull > 0.0F) {
      for (const auto en : registry_.view<Transform, Health, Radius, Enemy>()) {
        auto* eh = registry_.try_get<Health>(en);
        if (eh == nullptr || eh->hp <= 0.0F) continue;
        auto& et = registry_.get<Transform>(en);
        const auto& er = registry_.get<Radius>(en);
        const float dx = pt.x - et.x;
        const float dy = pt.y - et.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        // Only what the ring has already swept past: inside the band it damages,
        // outside it just shoves. Pulling the whole map inward would delete every
        // positioning problem the rest of the roster exists to pose.
        if (d > nr.radius + er.r || d < 1e-4F) continue;
        // The floor is what stops a contracting ring from being a suicide button.
        // It tracks the ring's own radius inward, so the victim is dragged to the
        // edge of the shrinking circle and then held there while the ring closes
        // on it -- a wall of bodies facing inward, with the burst going off in
        // the gap. Once the ring itself is inside the floor the floor takes over
        // and the pull simply stops, which is the moment the burst is due.
        const float floorR = std::max(nr.radius, kNovaPullFloor);
        const float step = std::min(nr.pull * continuousPullScale(en) / 60.0F,
                                    std::max(0.0F, d - floorR - er.r * 0.5F));
        et.x += (dx / d) * step;
        et.y += (dy / d) * step;
      }
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

// --- Elite chests -------------------------------------------------------------
// The tier's gift. An elite opens one box, a champion three, an overlord five,
// plus whatever the Deep Cache card adds, and the box spends itself on the
// player's OWN weapons -- one legal card each, preferring a weapon that has none
// yet. So the run gets rarer elites precisely so that meeting one is worth
// something, and the size of the box is the visible difference between a tier and
// the one below it.
std::vector<int> Game::legalWeaponCards(int weaponSlot) const {
  std::vector<int> out;
  if (weaponSlot < 0 || weaponSlot >= weaponCount_) return out;
  const std::string& id = content_.weapons[static_cast<std::size_t>(weapons_[weaponSlot].def)].id;
  for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
    const auto& u = content_.upgrades[i];
    if (u.weapon != id) continue;
    if (blocked_[i]) continue;
    if (stacks_[i] >= u.maxStacks) continue;
    out.push_back(static_cast<int>(i));
  }
  return out;
}

int Game::openChest(int grants) {
  if (grants <= 0) return 0;
  int given = 0;
  for (int n = 0; n < grants; ++n) {
    // Weapons that still have a card left. A weapon with nothing left is skipped
    // rather than counted, so a seven-weapon arsenal does not waste a third of a
    // champion's gift on nothing.
    std::vector<int> slots;
    for (int s = 0; s < weaponCount_; ++s) {
      if (!legalWeaponCards(s).empty()) slots.push_back(s);
    }
    if (slots.empty()) break;
    // Prefer a weapon this box has not touched yet. Without that, a chest picks
    // uniformly and stacks the first weapon's damage card four times, which is
    // the same thing a level-up card would have done and much less interesting.
    std::vector<int> fresh;
    for (const int s : slots) {
      if (legalWeaponCards(s).size() > 1 || given == 0) fresh.push_back(s);
    }
    if (!fresh.empty()) slots = fresh;
    const int slot = slots[static_cast<std::size_t>(rng_() % slots.size())];
    const auto cards = legalWeaponCards(slot);
    if (cards.empty()) continue;
    const int card = cards[static_cast<std::size_t>(rng_() % cards.size())];
    if (applyUpgradeAt(card)) {
      ++given;
      // The HUD wants to say what came out of the box.
      lastChestCard_ = card;
    }
  }
  return given;
}

entt::entity Game::spawnChest(float x, float y, int tier) {
  // How many weapons this tier's box improves. The numbers are the reward: an
  // elite is one card, a champion is a whole hand, an overlord is most of the
  // arsenal in one pickup.
  int base = 1;
  if (tier == 2) base = 3;
  if (tier >= 3) base = 5;
  // NOT clamped to the arsenal size. A box is N CARDS, and openChest spends them
  // across the weapons the player holds, spreading before repeating. Clamping
  // here instead would quietly downgrade a champion's gift to an elite's for
  // every early run -- and the early run is exactly when a champion is hardest
  // to kill and the box is most worth having.
  const int grants = base + stats_.chestBonus;
  if (grants <= 0) return entt::null;

  const auto box = registry_.create();
  registry_.emplace<Transform>(box, x, y, x, y);
  registry_.emplace<Velocity>(box);
  registry_.emplace<Radius>(box, 0.30F);
  Sprite s{};
  s.color = tier >= 3 ? core::render::Color{1.00F, 0.80F, 0.25F, 1.0F}
         : tier == 2 ? core::render::Color{0.75F, 0.50F, 1.00F, 1.0F}
                      : core::render::Color{0.45F, 0.85F, 1.00F, 1.0F};
  s.circle = true;
  registry_.emplace<Sprite>(box, s);
  Chest c{};
  c.grants = grants;
  c.tier = tier;
  registry_.emplace<Chest>(box, c);
  return box;
}

void Game::updatePickups() {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& pt = registry_.get<Transform>(player_);
  const float magnet = 2.0F * stats_.pickupMul;

  // The view cannot ask for "Xp OR Chest", so it asks for what they share and
  // the predicate below filters. That is not a stylistic choice: widening this
  // view to every moving body once made the magnet drag ENEMY PROJECTILES into
  // the player and delete them, which quietly gutted sixteen weapons' worth of
  // tests. A pickup is an orb or a box, and nothing else moves toward the player
  // because it is closer to the player.
  auto view = registry_.view<Transform, Velocity, Radius>();
  for (const auto e : view) {
    // Living enemies also carry an Xp component (awarded on death) but are
    // never pickups: without this guard every enemy that gets within magnet
    // range was vacuumed into the player and destroyed without dying.
    if (registry_.all_of<Enemy>(e)) continue;
    if (!registry_.all_of<Xp>(e) && !registry_.all_of<Chest>(e)) continue;
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
      // A chest spends itself here, on the player's own weapons, and then gets
      // out of the way. It is a pickup like any other so the player never has to
      // press a key for a reward.
      if (const auto* c = registry_.try_get<Chest>(e); c != nullptr) {
        const int given = openChest(c->grants);
        // A box with nothing left to give still opens, and says so, rather than
        // sitting on the floor forever waiting for a weapon to have a card again.
        spawnParticles(t.x, t.y, {1.0F, 0.85F, 0.35F, 1.0F}, 12, 5.0F);
        if (given > 0) {
          lastChestGrants_ = given;
          lastChestTimer_ = kChestToastTime;
        }
        destroyQueue_.push_back(e);
        continue;
      }
      // XP is routed through grantXp() so the sandbox's "no experience" rule
      // is enforced in exactly one place. A body that is neither an orb nor a
      // chest is nothing the player collects, so it is simply left alone.
      if (const auto* x = registry_.try_get<Xp>(e); x != nullptr) {
        grantXp(x->value * stats_.xpMul);
        destroyQueue_.push_back(e);
      }
    }
  }
}

// The chest toast is a timer like every other transient HUD message, so it is
// ticked in the same place the banner is.
void Game::updateChestToast(float dt) {
  if (lastChestTimer_ > 0.0F) lastChestTimer_ = std::max(0.0F, lastChestTimer_ - dt);
}

void Game::updateShield() {
  if (stats_.shieldMax <= 0.0F) return;
  if (shieldDelay_ > 0.0F) {
    shieldDelay_ -= 1.0F / 60.0F;
    return;
  }
  const float rate = shieldRegenRate();
  shield_ = std::min(stats_.shieldMax, shield_ + rate / 60.0F);
}

// How long the shield waits after a hit before it starts refilling again.
// Shortened by shield_delay cards, floored so a pool is never permanent.
float Game::shieldRegenDelay() const {
  return std::max(kShieldRegenDelayFloor, kShieldRegenDelay + stats_.shieldRegenDelay);
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
  // Growth is piecewise: past the 7-minute mark enemies develop faster, so the
  // late game keeps escalating.
  //
  // Every number here is lower than it was and the knee is a minute later. The
  // reason is the same as the XP curve: the HP ramp is the one piece of the
  // difficulty that the player has NO answer to. A build that is three picks
  // behind cannot outshoot a 12x enemy, and at the old rates a ten-minute run
  // put ordinary bats at nearly ten times the HP of the first minute while the
  // player's damage was still mostly on the cards. The shape is intact -- it is
  // still a rising ramp that steepens -- it just stops outrunning the build.
  // The first leg is deliberately shallow. The complaint this answers is
  // "after two or three minutes the screen is so dense I cannot kill what is on
  // it", and at /95 a bat had 2.6x its opening HP by 2:30 while the player's
  // damage was still arriving on level-up cards. The heavies are now spread
  // across the run as well (see enemies.toml), so the early leg no longer has to
  // carry the whole difficulty on its own: at 2:30 an ordinary enemy is barely
  // doubled, and the threat comes from what has actually walked in.
  constexpr float kHpKnee = 480.0F;
  if (simTime_ <= kHpKnee) {
    hp = 1.0F + simTime_ / 145.0F;
    speed = 1.0F + simTime_ / 780.0F;
  } else {
    hp = 1.0F + kHpKnee / 145.0F + (simTime_ - kHpKnee) / 62.0F;
    speed = 1.0F + kHpKnee / 780.0F + (simTime_ - kHpKnee) / 380.0F;
  }
  hp = std::min(hp, 20.0F);
  speed = std::min(speed, 2.05F);
  // Contact damage also creeps up so late enemies hit harder.
  touch = 1.0F + simTime_ / 2000.0F;
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

float Game::fastTraitSpeedMul() const {
  return 1.0F + (kFastTraitMaxMul - 1.0F) *
                    std::min(1.0F, simTime_ / kFastTraitFullTime);
}

float Game::typeSpeedRamp(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= content_.enemies.size()) {
    return 1.0F;
  }
  const float max = content_.enemies[static_cast<std::size_t>(def)].speedRampMax;
  return 1.0F + max * std::min(1.0F, simTime_ / kSpeedRampFullTime);
}

float Game::enemySpawnSpeed(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= content_.enemies.size()) {
    return 0.0F;
  }
  float hpScale = 1.0F;
  float speedScale = 1.0F;
  float touchScale = 1.0F;
  currentScales(hpScale, speedScale, touchScale);
  return content_.enemies[static_cast<std::size_t>(def)].speed * typeSpeedRamp(def) *
         speedScale;
}

float Game::typeLockedUntil(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= content_.enemies.size()) {
    return 0.0F;
  }
  const auto& d = content_.enemies[static_cast<std::size_t>(def)];
  // A `fast` archetype is gated by the later of its own unlock time and the
  // global fast-enemy floor, so the data only has to say "this is a sprinter"
  // and the pacing rule lives in one place.
  return d.fast ? std::max(d.unlockAt, kFastEnemyMinTime) : d.unlockAt;
}

bool Game::typeCanSpawn(int def) const {
  if (def < 0 || static_cast<std::size_t>(def) >= content_.enemies.size()) return false;
  if (simTime_ < typeLockedUntil(def)) return false;
  if (!typeRetired(def)) return true;
  // Retired, but exempt because it is one of the 3 newest types.
  return testTypeIsRecent(def);
}

// Spawns one chain bolt. The arc itself is the Tesla Coil's whole identity, and
// the one thing that distinguishes another chain weapon is what the bolt DOES at
// each hop rather than how many hops it has: the Blizzard Rail shatters into a
// fan of flat shards on its landing (see spawnChainShatter).
void Game::spawnChainBolt(float x, float y, const WeaponSlot& w,
                          std::uint32_t fromTarget) {
  const auto chain = registry_.create();
  registry_.emplace<Transform>(chain, x, y, x, y);
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
  cl.lastTarget = fromTarget;
  cl.linger = kChainLinger;
  // Every bolt converges first, and every bolt owns its own landing hit. There
  // used to be a second kind of chain bolt -- a "fork", born mid-arc off a body
  // that was already being struck, with no wind-up and no strike owed -- but the
  // only weapon that forked was the Storm Caller, and it stopped being a chain
  // weapon when it became a bolt that bends. So there is one kind of bolt now,
  // and it behaves the same way every time it is spawned.
  cl.telegraph = kChainTelegraph;
  cl.strike = 0.0F;
  cl.pendingFirst = fromTarget;
  cl.shatter = w.chainShatter;
  cl.shatterSpeed = w.chainShatterSpeed;
  cl.shatterSpread = w.chainShatterSpread;
  registry_.emplace<ChainLightning>(chain, cl);
}

// The Blizzard Rail slug coming apart. Every shard is a plain fast projectile
// aimed along the direction the bolt was travelling, spread into a fan. They
// are projectiles and not bolts on purpose: the fragments fly flat and stop
// where they stop, where a fork would arc on toward a new victim. That
// difference is the whole read of "it shattered".
void Game::spawnChainShatter(const ChainLightning& cl, float x, float y,
                             float heading, float damage) {
  const int n = std::max(0, cl.shatter);
  const int count = std::max(1, n);
  for (int k = 0; k < n; ++k) {
    // Fan centred on the bolt's own heading, so the burst goes where the strike
    // was already going instead of spraying in every direction at once.
    const float offset = (static_cast<float>(k) - static_cast<float>(count - 1) * 0.5F) *
                         std::max(0.05F, cl.shatterSpread);
    const float a = heading + offset;
    const auto sh = registry_.create();
    registry_.emplace<Transform>(sh, x, y, x, y);
    registry_.emplace<Velocity>(sh, std::cos(a) * cl.shatterSpeed,
                                 std::sin(a) * cl.shatterSpeed);
    // Fragments are visibly smaller than a bolt, which is most of what makes
    // the shatter legible at a glance.
    registry_.emplace<Radius>(sh, 0.12F);
    Sprite s{};
    s.color = cl.color;
    registry_.emplace<Sprite>(sh, s);
    Projectile p{};
    p.damage = damage;
    // A fixed, short life rather than a share of the bolt's reach: these are
    // pieces of a slug, not a second slug.
    p.life = 0.55F;
    p.pierce = cl.maxJumps > 0 ? 1 : 0;
    registry_.emplace<Projectile>(sh, p);
  }
}

// A burst of ice fragments thrown from a frozen corpse. A ring, not the
// Blizzard's directional fan: a dead body has no heading, and a symmetric spray
// is what coming apart looks like anyway.
void Game::spawnShatterBurst(float x, float y, float damage) {
  constexpr int kShards = 6;
  constexpr float kShardSpeed = 7.0F;
  for (int k = 0; k < kShards; ++k) {
    const float a = (static_cast<float>(k) / static_cast<float>(kShards)) * 2.0F * kPi;
    const auto sh = registry_.create();
    registry_.emplace<Transform>(sh, x, y, x, y);
    registry_.emplace<Velocity>(sh, std::cos(a) * kShardSpeed, std::sin(a) * kShardSpeed);
    registry_.emplace<Radius>(sh, 0.11F);
    Sprite s{};
    s.color = {0.70F, 0.92F, 1.0F, 1.0F};
    s.circle = true;
    registry_.emplace<Sprite>(sh, s);
    Projectile p{};
    p.damage = damage;
    // Short and single-target. The point is a punctuation mark on a kill, not a
    // second volley: a corpse that cleared the room would make the ice weapon
    // scale off kills rather than off what the player aims at.
    p.life = 0.4F;
    registry_.emplace<Projectile>(sh, p);
  }
}

// Spawns one nova ring at (x, y). Every ring in the game goes through here, which
// is the only reason the expanding Shock Core and the contracting Void Nova cannot
// drift apart: they are the same call with different fields, so a change to one is
// automatically a change to the other.
void Game::spawnNovaRing(float x, float y, const WeaponSlot& w, int pierce) {
  const float maxR = w.novaMaxRadius * (1.0F + stats_.projAdd * 0.1F);
  const auto nova = registry_.create();
  registry_.emplace<Transform>(nova, x, y, x, y);
  registry_.emplace<Radius>(nova, maxR);
  Sprite s{};
  s.color = w.color;
  s.circle = true;
  registry_.emplace<Sprite>(nova, s);
  NovaRing nr{};
  nr.damagePerTick = w.novaDamagePerTick;
  nr.pierce = pierce;
  nr.maxRadius = maxR;
  // A negative speed is how "inward" is spelled: one ring type, one integrator,
  // and no branch in the update for which way it travels.
  nr.expandSpeed = w.novaContract ? -w.novaExpandSpeed : w.novaExpandSpeed;
  // Cast at full radius, not at zero -- that is the whole visual difference
  // between a wall arriving and a knot arriving.
  nr.radius = w.novaContract ? maxR : 0.0F;
  nr.tickRate = w.novaTickRate;
  nr.timer = 0.0F;
  nr.tickTimer = 0.0F;
  nr.pull = w.novaContract ? w.novaPull : 0.0F;
  nr.burstDone = !w.novaContract || w.novaBurstDamage <= 0.0F;
  nr.burstDamage = w.novaBurstDamage;
  nr.color = w.color;
  registry_.emplace<NovaRing>(nova, nr);
}

// Spawns one travelling crescent at `angle`.
void Game::spawnWaveCrescent(const WeaponSlot& w, float angle, float damage, int pierce) {
  const auto pt = registry_.get<Transform>(player_);
  const auto we = registry_.create();
  registry_.emplace<Transform>(we, pt.x, pt.y, pt.x, pt.y);
  registry_.emplace<Radius>(we, w.waveWidth * 0.5F);
  Sprite s{};
  s.color = w.color;
  s.circle = true;
  registry_.emplace<Sprite>(we, s);
  WaveEffect wv{};
  wv.damage = damage;
  wv.speed = w.waveSpeed;
  wv.width = w.waveWidth;
  wv.spread = w.waveSpread;
  wv.angle = angle;
  wv.knockback = w.waveKnockback;
  // The herd goes the way the NEXT arc is actually thrown, which is the sign of
  // the fan's step -- not whichever perpendicular happened to be cheaper.
  wv.hookPull = w.waveHookPull;
  wv.hookSide = w.waveArcStep >= 0.0F ? 1.0F : -1.0F;
  wv.travelled = 0.0F;
  wv.range = w.waveRange;
  wv.life = w.waveRange / std::max(0.5F, w.waveSpeed) + 0.2F;
  wv.color = w.color;
  registry_.emplace<WaveEffect>(we, wv);
  (void)pierce; // a wave hits each enemy once; crowd falloff is applied per hit
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

  // Per-type acceleration on top of the global ramp. A fast archetype is
  // authored slow (that is what the player meets in the first minute) and gains
  // speed over the run, saturating at kSpeedRampFullTime. Types with no ramp
  // get exactly 1.0 and are unaffected, so this is a no-op for the whole roster
  // except the types that ask for it.
  const auto rampFor = [&](int def) { return typeSpeedRamp(def); };

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
      // Retirement emptied the pool, so fall back to every time-unlocked type
      // regardless of retirement. This has to use the SAME gate as the first
      // pass: weighting a type here that the selection loop below then skips
      // would leave `roll` never reaching zero and silently stop ALL spawns.
      for (const auto& def : content_.enemies) {
        if (simTime_ >= typeLockedUntil(static_cast<int>(&def - content_.enemies.data()))) {
          totalWeight += def.weight;
        }
      }
      if (totalWeight <= 0.0F) return -1;
    }
    float roll = unit(rng_) * totalWeight;
    for (std::size_t i = 0; i < content_.enemies.size(); ++i) {
      const auto& candidate = content_.enemies[i];
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
          pending.speedMul = speedScale * rampFor(hordeDef);
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
  // The third leg used to start at 1:30 and bottom out at 0.20s, so the run went
  // from one pack a second to five inside the two minutes where the heavy enemies
  // were also arriving. The floor is 0.30s now and the leg starts at 2:30, which
  // is the other half of the same fix: the screen fills later AND the things in
  // it can be killed when it does.
  if (simTime_ < 30.0F) {
    spawnTimer_ = 1.2F - simTime_ * 0.005F;
  } else if (simTime_ < 150.0F) {
    spawnTimer_ = std::max(0.58F, 1.05F - (simTime_ - 30.0F) * 0.0039F);
  } else {
    spawnTimer_ = std::max(0.30F, 0.58F - (simTime_ - 150.0F) * 0.0021F);
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
    // The screen's elite budget for this spawn. Counted ONCE per pack rather
    // than per member, so a pack of three cannot spend three slots, and the
    // budget is handed out in tier order: an overlord is worth more than a
    // champion, which is worth more than an elite, so when there is only room
    // for one the roll keeps the expensive one.
    int tierBudget = kLiveTierCap - liveTierCount();
    if (tierBudget < 0) tierBudget = 0;
    const float eliteChance = tierSpawnChance(1);
    const float champChance = tierSpawnChance(2);
    const float overlordChance = tierSpawnChance(3);
    const bool memberOverlord = tierBudget > 0 && unit(rng_) < overlordChance;
    if (memberOverlord) --tierBudget;
    const bool memberChampion =
        tierBudget > 0 && !memberOverlord && unit(rng_) < champChance;
    if (memberChampion) --tierBudget;
    // An elite is the common tier, so it is the one that fills any leftover room
    // -- including all of it, when a pack is big. That is the point: the trash
    // becomes elite instead of the elites becoming trash.
    bool memberElite = false;
    if (!memberOverlord && !memberChampion && tierBudget > 0) {
      const int room = std::min(tierBudget, 3);
      // A pack may take more than one slot when the screen is empty, which is
      // the only way an early pack of three can be the "one or two on screen"
      // moment rather than three separate minutes of one elite.
      memberElite = room > 0 && unit(rng_) < std::max(eliteChance, 0.34F);
      if (memberElite) --tierBudget;
    }
    if (memberChampion || memberOverlord) memberElite = true;

    float mHpMul = hpScale;
    float mTouchMul = touchScale;
    float mSpeedMul = speedScale * rampFor(defIndex);
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
          // The Fast trait is a RAMP, not a flat 1.7x. A flat bonus applied the
          // moment a horde spawns makes a 4-minute ring of elites undodgeable,
          // and it makes the trait worthless to read: there is no difference
          // between a 2-minute and a 9-minute Fast elite. Growing to the full
          // bonus by kFastTraitFullTime means a Fast elite is a genuine threat
          // only once the run is built to handle one.
          case PickFast:
            mSpeedMul *= fastTraitSpeedMul();
            mTraits |= TraitFast;
            break;
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
    // 1:30 instead of 0:45. The first elite used to arrive before the player
    // had a third pick, which meant the run's first real decision was "do I
    // play around the thing that is about to end me".
    case 1: return tierOpen_[1] && simTime_ >= 90.0F;
    case 2: return tierOpen_[2];
    case 3: return tierOpen_[3];
    default: return false;
  }
}

int Game::liveTierCount() const {
  int n = 0;
  auto view = registry_.view<Enemy, EnemyTraits>();
  for (const auto e : view) {
    if (view.get<EnemyTraits>(e).tier > 0) ++n;
  }
  return n;
}

int Game::testLiveTierCount() const { return liveTierCount(); }

float Game::tierSpawnChance(int tier) const {
  if (!tierUnlocked(tier)) return 0.0F;
  if (tier == 1) {
    // Starts rarer AND tops out lower. At the old 15% one in seven spawns was an
    // elite, which stops being an event and becomes a tax on the trash.
    return std::min(0.10F, 0.025F + simTime_ * 0.0003F);
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
        tierBannerT_ = kTierBannerLife;
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
    // A milestone is a GROUP, not a pair of cards. Every still-available card of
    // the group is laid out on the screen at once, and taking one of them closes
    // the rest of the group for the rest of the run.
    //
    // This is what makes a milestone a decision instead of a lottery. It used to
    // be three flat stat cards of which two were shown, which meant the screen
    // said "damage, or more damage, or yet more damage" and the only real choice
    // was which number was bigger. Now the members of a group are different axes
    // of the run -- vampirism against regen against a shield, damage against fire
    // rate against reach -- and the screen can be as wide as the group is, so a
    // four-way group gets four slots (kMaxMilestoneSlots) instead of two.
    //
    // Two groups may share a screen when both are narrow, and picking from one
    // does NOT close the other: that is what lets the player choose the subject
    // as well as the card, and it leaves a group with a spare choice to be
    // revisited at the next milestone instead of quietly vanishing.
    std::vector<std::string> groupNames;
    std::vector<int> ungrouped;
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      const auto& u = content_.upgrades[i];
      if (u.kind != "milestone" || u.level != level_) continue;
      if (stacks_[i] >= u.maxStacks || blocked_[i]) continue;
      if (u.group.empty()) {
        ungrouped.push_back(static_cast<int>(i));
      } else if (std::find(groupNames.begin(), groupNames.end(), u.group) ==
                 groupNames.end()) {
        groupNames.push_back(u.group);
      }
    }

    std::vector<int> offer;
    const auto membersOf = [this, level = level_](const std::string& name) {
      std::vector<int> out;
      for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
        const auto& u = content_.upgrades[i];
        if (u.kind != "milestone" || u.level != level) continue;
        if (u.group != name) continue;
        if (stacks_[i] >= u.maxStacks || blocked_[i]) continue;
        out.push_back(static_cast<int>(i));
      }
      return out;
    };

    std::shuffle(groupNames.begin(), groupNames.end(), rng_);
    std::shuffle(ungrouped.begin(), ungrouped.end(), rng_);
    for (const auto& name : groupNames) {
      if (offer.size() >= kMaxMilestoneSlots) break;
      for (const int idx : membersOf(name)) {
        if (offer.size() >= kMaxMilestoneSlots) break;
        offer.push_back(idx);
      }
    }
    // A group-less milestone card is its own group of one.
    for (const int idx : ungrouped) {
      if (offer.size() >= kMaxMilestoneSlots) break;
      offer.push_back(idx);
    }

    if (!offer.empty()) {
      std::shuffle(offer.begin(), offer.end(), rng_);
      milestoneOffer_ = true;
      choices_.reserve(offer.size());
      for (const int idx : offer) choices_.push_back({Choice::Kind::Upgrade, idx});
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
      if (u.kind != "unique" || stacks_[i] >= u.maxStacks || blocked_[i]) continue;
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
    // Locked out by an earlier milestone pick. A group card is a milestone card,
    // so this cannot fire today -- but the lock is the rule, and the rule should
    // be the thing every pool goes through rather than a guard on one path.
    if (blocked_[i]) continue;
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
      if (stacks_[i] >= content_.upgrades[i].maxStacks || blocked_[i]) continue;
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
    // `grants` is how many times the effect lands. It is a separate field from
    // `value` on purpose: "lifesteal, three times over" and "lifesteal +18" are
    // the same number but only one of them tells the player what is about to
    // happen, and a milestone that grants several applications is the whole
    // point of the group it belongs to.
    const int times = std::max(1, def.grants);
    for (int k = 0; k < times; ++k) {
      result = applyUpgrade(stats_, def.effect, def.value);
      if (!result.valid) return false;
    }
  } else {
    // Weapon-targeted upgrade (e.g. a weapon's personal Focus card).
    const int weaponSlot = findWeaponSlot(def.weapon);
    if (weaponSlot < 0) return false;
    for (int k = 0; k < std::max(1, def.grants); ++k) {
      applyWeaponEffect(weaponSlot, def.effect, def.value);
    }
    result.valid = true;
  }
  ++stacks_[static_cast<std::size_t>(upgradeIndex)];

  // Close the rest of the group. This is the exclusive half of the promise: the
  // card you took is now on the board, and its siblings can never be offered
  // again this run, at a milestone or anywhere else. The card itself stays
  // available if it stacks, so a group of one-stack cards is a one-time decision
  // and a group with a stacking member can be leaned on.
  if (!def.group.empty()) {
    for (std::size_t i = 0; i < content_.upgrades.size(); ++i) {
      if (i == static_cast<std::size_t>(upgradeIndex)) continue;
      if (content_.upgrades[i].group == def.group) blocked_[i] = 1;
    }
  }

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
  // Two guards, deliberately: weaponCap() is the design cap and kMaxWeapons is
  // the array bound. Anything that could push weaponCount_ past the array is a
  // memory bug, not a balance question.
  if (weaponCount_ >= weaponCap() || weaponCount_ >= kMaxWeapons) return;
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
  w.coneBite = def.coneBite;
  w.coneBiteMax = def.coneBiteMax;
  w.coneLatch = entt::null;
  w.coneBiteTicks = 0;
  w.coneEmberAt = def.coneEmberAt;
  w.coneEmberRadius = def.coneEmberRadius;
  w.coneEmberDuration = def.coneEmberDuration;
  w.coneEmberWalked = 0.0F;
  w.coneEmberLive = 0;
  w.coneEmberLastX = 0.0F;
  w.coneEmberLastY = 0.0F;

  // Orbit
  w.orbitRadius = def.orbitRadius;
  w.orbitSpeed = def.orbitSpeed;
  w.orbitCount = def.orbitCount;
  w.orbitWindow = def.orbitWindow;
  w.orbitAngle = 0.0F;

  // Bomb
  w.bombArcHeight = def.bombArcHeight;
  w.bombExplodeRadius = def.bombExplodeRadius;
  w.bombKnockback = def.bombKnockback;
  w.bombFuse = def.bombFuse;
  w.bombAhead = def.bombAhead;
  w.bombOnTarget = def.bombOnTarget;
  w.reaimRange = def.reaimRange;
  w.reaimTurn = def.reaimTurn;

  // Boomerang
  w.boomerangRange = def.boomerangRange;
  w.boomerangReturnSpeed = def.boomerangReturnSpeed;

  // Bounce
  w.bounceCount = def.bounceCount;
  w.bounceRange = def.bounceRange;
  w.bounceDamageMul = def.bounceDamageMul;
  w.bounceInfinite = def.bounceInfinite;
  w.bounceSplits = def.bounceSplits;
  w.liveBounce = entt::null;

  // Beam
  w.beamRange = def.beamRange;
  w.beamWidth = def.beamWidth;
  w.beamDuration = def.beamDuration;

  // Halo
  w.haloKnockback = def.haloKnockback;
  w.haloInner = def.haloInner;

  // Sweep
  w.sweepAngle = def.sweepAngle;
  w.sweepRadius = def.sweepRadius;
  w.sweepKnockback = def.sweepKnockback;

  // Zone
  w.zoneRadius = def.zoneRadius;
  w.zoneDuration = def.zoneDuration;
  w.zoneDps = def.zoneDps;
  w.zoneMaxPools = def.zoneMaxPools;
  w.zoneFromAbove = def.zoneFromAbove;

  // Chain
  w.chainJumpRange = def.chainJumpRange;
  w.chainMaxJumps = def.chainMaxJumps;
  w.chainDamageMul = def.chainDamageMul;
  w.chainShatter = def.chainShatter;
  w.chainShatterSpeed = def.chainShatterSpeed;
  w.chainShatterSpread = def.chainShatterSpread;
  w.infernoBindsToLure = def.infernoBindsToLure;

  // Wave
  w.waveSpeed = def.waveSpeed;
  w.waveRange = def.waveRange;
  w.waveWidth = def.waveWidth;
  w.waveKnockback = def.waveKnockback;
  w.waveDamageMul = def.waveDamageMul;
  w.waveCount = def.waveCount;
  w.waveArcStep = def.waveArcStep;
  w.waveHookPull = def.waveHookPull;
  w.waveSpread = def.waveSpread;
  w.waveBurstLeft = 0;
  w.waveBurstTimer = 0.0F;
  w.waveBurstAngle = 0.0F;

  // Nova
  w.novaMaxRadius = def.novaMaxRadius;
  w.novaExpandSpeed = def.novaExpandSpeed;
  w.novaDamagePerTick = def.novaDamagePerTick;
  w.novaTickRate = def.novaTickRate;
  w.novaContract = def.novaContract;
  w.novaPull = def.novaPull;
  w.novaBurstDamage = def.novaBurstDamage;
  w.novaEcho = def.novaEcho;
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
  w.vortexCollapseAt = def.vortexCollapseAt;
  w.vortexBurstDamage = def.vortexBurstDamage;
  w.vortexBurstRadius = def.vortexBurstRadius;

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
  w.chillMul = def.chillMul;
  w.chillTime = def.chillTime;
  w.auraRadius = def.auraRadius;
  w.auraDps = def.auraDps;
  w.auraTick = def.auraTick;
  w.auraChillMul = def.auraChillMul;
  w.auraChillTime = def.auraChillTime;

  // Round-3 additions.
  w.cdBonus = 0.0F;
  w.sweepLead = def.sweepLead;
  w.sweepHook = def.sweepHook;
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
    hb.inner = w.haloInner;
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
    vx.collapseAt = w.vortexCollapseAt;
    vx.charge = 0.0F;
    vx.burstDamage = w.vortexBurstDamage;
    vx.burstRadius = w.vortexBurstRadius;
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
    // Throwing Dagger: the ring stops being a solid wall and becomes a HUB with
    // one safe angle in it. Anything standing in the gap is held where it is and
    // cut continuously, so the card's rule is that the safe spot is also the
    // killing spot -- which is the only thing that would make a faster ring worth
    // having, because now you have to be able to stand somewhere inside it.
    w.orbitSpeed *= 2.0F;
    w.orbitRadius *= 1.25F;
    w.orbitWindow = true;
  } else if (effect == "w_unique_hearthfire") {
    // Hearthfire: the Sprayer's cone starts LEAVING EMBERS on the ground it
    // washes. A cone that only ever damages what is standing in it right now is
    // a cone you have to keep pointing at things; a cone that scorches where it
    // has been is a trail you lay down and then walk away from. The wider, longer
    // reach is the same card's other half, but the pools are the rule.
    w.coneRange *= 1.5F;
    w.coneAngle *= 1.4F;
    w.coneEmberAt = 0.9F;      // one pool per this many world units travelled
    w.coneEmberRadius = 1.15F;
    w.coneEmberDuration = 2.6F;
  } else if (effect == "w_unique_cataclysm") {
    // Cataclysm: a shell that detonates ONCE and is gone, so the card's job is to
    // make that one boom worth the wait. The stacked fuse is the rule: the shells
    // no longer arrive as a volley you can walk out of between shots, they arrive
    // together, and the bigger radius is the payoff rather than the point.
    w.bombFuse += 0.3F;
    w.cooldown *= 1.15F;
    w.bombExplodeRadius *= 1.6F;
    w.bombKnockback *= 1.5F;
  } else if (effect == "w_unique_prism") {
    w.beamSplit += static_cast<int>(value);
  } else if (effect == "w_unique_molten") {
    w.zoneDps *= 1.8F;
    w.zoneDuration += 2.0F;
    w.zoneRadius *= 1.25F;
  } else if (effect == "w_unique_thunderlord") {
    // CHAIN-ONLY, and deliberately so. This used to be the card for three chain
    // weapons, one of which stopped being a chain weapon when the Storm Caller
    // became a bolt that bends. For a month the Storm's copy of it edited two
    // numbers the weapon never reads: the card showed up on the level-up screen,
    // read beautifully, and did nothing at all. The split is what stops that --
    // see `w_unique_reaim` for the bolt, and the effect table in the test that
    // fails the build when a `w_unique_*` card lands on a type it cannot move.
    w.chainMaxJumps += static_cast<int>(value);
    w.chainDamageMul = 1.0F;
  } else if (effect == "w_unique_reaim") {
    // The Storm Caller's own card, and the one thing its merge is FOR: the bolt
    // spends its pierce on direction, so a pierce point is both one more body it
    // may touch and one more body it may bend onto. The card's `value` is read
    // as chain-jump points and halved, because "+4 chain jumps" in those units
    // is "+2 bodies" in these -- which is the same promise the old card was
    // making, finally told in the currency the weapon actually spends.
    //
    // The other two are the numbers that decide how well it keeps that promise:
    // how far ahead it can see for the next body, and how hard it corners once
    // it has picked one. A longer sight line is what lets it cross a gap the
    // crowd left; a harder corner is what lets it keep up with a sprinter.
    w.pierce += static_cast<int>(value) / 2;
    w.reaimRange *= 1.35F;
    w.reaimTurn *= 1.45F;
  } else if (effect == "w_unique_faultline") {
    // The Sundering Core is a WIDE crescent shoved away from the player at a
    // crawl: a wall, not a lash. So its card makes it more of a wall. It had
    // been sharing the Void Nova's `w_unique_supernova` and its description
    // verbatim -- "ring expands faster, wider, and hits harder", on a weapon
    // that throws no ring and no Nova-shaped anything. A copy-pasted desc is
    // usually a copy-pasted effect underneath it, and here it was.
    //
    // Nothing here adds a second crescent: that is the Tidal Lash's whole
    // identity (three arcs, evenly stepped), and handing it to the Core would
    // collapse the one distinction the two wave weapons have.
    w.waveWidth *= 1.5F;
    w.waveRange *= 1.35F;
    w.waveSpeed *= 1.25F;
    w.waveDamageMul *= 1.6F;
    w.waveSpread *= 1.2F;
    w.waveKnockback *= 1.3F;
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
    // Tidal Lash. This card was four SWEEP fields and a `sweepHook` -- the
    // Barbed Whip's best trick, drag-the-catch-in and all -- sitting on a weapon
    // that throws WAVES. A wave reads no sweep field at all, so every edit was
    // invisible: the Tidal Lash's own card was the deadest pick in the pool
    // while reading like the strongest one in it.
    //
    // What it does now is buy more of the thing the weapon already IS. Coverage
    // is the Lash's identity against the Sundering Core's single wall, so: one
    // more arc, thrown further apart, reaching further and biting harder -- and
    // a stronger herd, because the hook is what makes an extra arc worth having
    // rather than a fourth independent shove. The Core's card
    // (`w_unique_faultline`) goes the opposite way, making its ONE crescent
    // wider; between them, the two wave weapons' cards are as opposed as the
    // weapons themselves.
    w.waveCount += static_cast<int>(value);
    w.waveArcStep += 0.18F;
    w.waveRange *= 1.30F;
    w.waveWidth *= 1.20F;
    w.waveDamageMul *= 1.25F;
    w.waveKnockback *= 1.35F;
    w.waveHookPull *= 1.30F;
  } else if (effect == "w_unique_corona") {
    // --- Uniques for the evolutions and supers that had no card at all -------
    // Halo, Void Gyre and Prism Array carried zero weapon-scoped cards, so
    // owning one of them meant its whole stat block was untouchable past the
    // global stats. Each unique below edits the parameters that weapon's
    // identity is actually made of, not a generic damage number.
    // Radiant Halo: the beams get real shoving power, so the ring stops being a
    // purely passive damage dealer and starts holding ground. Length and width
    // go up with it so the extra knockback is delivered, not swallowed.
    w.haloKnockback += 6.0F;
    w.beamWidth *= 1.4F;
    w.beamRange *= 1.15F;
    w.damage *= 1.2F;
    syncHaloBeams(slotIndex);
  } else if (effect == "w_unique_gyre") {
    // Void Gyre: harder pull, longer reach, a fatter core. Pull is capped
    // because past this the ring stops being a zone and becomes a wall that
    // pins every late-game boss in place forever.
    w.vortexPull = std::min(14.0F, w.vortexPull * 1.45F);
    w.vortexReach *= 1.3F;
    w.vortexRadius *= 1.2F;
    w.vortexTickRate *= 0.8F;
    syncVortices(slotIndex);
  } else if (effect == "w_unique_refract") {
    // Prism Array: one more independent beam corridor, and each of them
    // ricochets further off enemies. Max targets is capped at 6 so the screen
    // never becomes a solid sheet of beams.
    w.prismMaxTargets = std::min(6, w.prismMaxTargets + 1);
    w.prismRicochet = std::min(4.0F, w.prismRicochet * 1.4F);
    w.prismRange *= 1.15F;
  } else if (effect == "w_unique_deepfreeze") {
    // Hoarfrost Wake: the aura stops being a bubble around each shard and becomes
    // the corridor itself. Every shot fires a second, much wider shell that lags
    // behind the volley rather than riding with it, so the cold is a lane with
    // walls and the shards only have to carry the freezing. A bigger aura radius
    // alone would have been strictly a bigger aura.
    w.projectiles += 1;
    w.auraRadius = std::max(w.auraRadius, w.auraRadius * 1.55F);
    w.auraDps *= 1.35F;
    w.auraChillMul = std::min(0.45F, w.auraChillMul * 0.85F);
    w.auraChillTime *= 1.3F;
    // The direct hit gets weaker on purpose: once the lane itself is doing the
    // work, the shard is a delivery mechanism, and a shard that also hits hard is
    // the same weapon with more of everything.
    w.damage *= 0.85F;
  } else if (effect == "w_unique_rime") {
    // Frost Shards: the volley stops being a spray of ice and becomes a line of
    // lances that FREEZE. The chill is the point, not a stat line: the pierce
    // and the extra flight time are what let one lance keep chilling new bodies
    // instead of re-chilling the one in front of it, so the payoff is that the
    // horde arrives at you walking. Damage comes down to pay for it.
    w.pierce += 4;
    w.life *= 1.4F;
    w.damage *= 0.8F;
    w.chillMul = std::max(0.35F, w.chillMul * 0.6F);
    w.chillTime *= 1.6F;
  } else if (effect == "w_unique_siege_doctrine") {
    // Siege Mortar: a three-shell salvo on a longer fuse, so the whole salvo
    // lands together instead of dribbling out one shell at a time. The cooldown
    // goes up to keep the shell count honest — a mortar that fires six every
    // two seconds is a different weapon, not a better one.
    w.projectiles += 2;
    w.bombFuse *= 2.0F;
    w.bombExplodeRadius *= 1.35F;
    w.bombArcHeight *= 1.15F;
    w.cooldown *= 1.2F;
  } else if (effect == "w_unique_skewer") {
    // Pinball Puck: a skewed silver ball that never loses its bite. Bounce
    // count is capped so the puck cannot be left ricocheting forever inside
    // one room with the horde.
    w.bounceCount = std::min(24, w.bounceCount + 10);
    w.bounceDamageMul = 1.0F;
    w.bounceRange *= 1.3F;
  } else if (effect == "w_unique_bore") {
    // Jackhammer Drill: the bit stops being a narrow jab and starts drilling
    // THROUGH. The card's rule is the ramp -- the bite gets deeper the longer it
    // stays buried, and it keeps its ramp the instant the previous target dies,
    // so drilling a queue of heavies is worth far more than drilling one. The
    // width and reach are the same card's other half.
    w.coneBite = 0.20F;
    w.coneBiteMax = std::max(w.coneBiteMax, 5.0F);
    w.coneAngle *= 1.6F;
    w.coneRange *= 1.4F;
    w.coneTickRate *= 0.6F;
    w.damage *= 1.15F;
  } else if (effect == "w_unique_discharge") {
    // Shock Core: the ring stops being a single pulse and goes off TWICE. The
    // outward blast is unchanged in character -- it still expands, it still
    // shoves the crowd into open ground -- and then a second, faster ring is cast
    // on top of it a beat later, after the first has thinned the pack. That is
    // the rule: a discharge is one pulse, this makes it a double. Charging
    // outward is left alone, so the card cannot be mistaken for the Void Nova's
    // inward pull that it visually sits next to.
    w.novaMaxRadius *= 1.35F;
    w.novaExpandSpeed *= 1.4F;
    w.novaTickRate *= 0.5F;
    w.novaDamagePerTick *= 1.5F;
    w.novaEcho = 0.35F;   // seconds until the second ring, 0 = single pulse
  } else if (effect == "w_unique_chainlash") {
    // Barbed Whip: the lash goes all the way around AND the barbs bite -- the
    // hook from the Lash card, on the Chainlash card. The lead is kept (not
    // zeroed) so it still centres in front of the player instead of becoming
    // a reaper that also swings at whatever is behind you. The two cards are
    // deliberately complementary rather than two versions of a wider arc, so
    // taking both is a build and taking one is a build.
    w.sweepAngle = 6.2832F;
    w.sweepRadius *= 1.3F;
    w.sweepKnockback *= 1.5F;
    w.sweepHook = true;
  } else if (effect == "w_unique_wingbeat") {
    // Seraph Array: the wings beat faster, shove much harder and burn wider --
    // and the hole at your feet CLOSES, which is the card's real rule. Until now
    // the Seraph's dead zone was permanent and free, so the card had nothing to
    // decide: take the reach and keep the safety, or neither. Closing the hole
    // turns that into a choice -- the wings become a real guard again, at the
    // cost of the reach they were bought for. `haloInner` is clamped just under
    // the beam range so the spoke can never invert into a segment pointing the
    // wrong way.
    w.haloKnockback += 5.0F;
    w.beamWidth *= 1.3F;
    w.orbitSpeed *= 1.6F;
    w.damage *= 1.2F;
    w.haloInner = std::max(0.0F, w.haloInner * 0.35F);
    syncHaloBeams(slotIndex);
  } else if (effect == "w_unique_singularity") {
    // Event Horizon: the last word in crowd control. Pull is capped for the
    // same reason Void Gyre's is — a hard pin that never releases would delete
    // every positioning problem the rest of the roster exists to pose.
    //
    // The rule the card adds is the CLOCK, not the size: the wells collapse twice
    // as often and each implosion hits far harder, so the weapon stops being a
    // steady drag and becomes a thing that gathers and then detonates. Charging
    // faster without hitting harder would just be a shorter fuse on the same
    // weapon, and a longer one would make it strictly worse.
    w.vortexPull = std::min(14.0F, w.vortexPull * 1.5F);
    w.vortexRadius *= 1.35F;
    w.vortexReach *= 1.25F;
    w.vortexTickRate *= 0.75F;
    if (w.vortexCollapseAt > 0.0F) {
      w.vortexCollapseAt *= 0.5F;
      w.vortexBurstDamage = std::max(w.vortexBurstDamage,
                                     w.vortexBurstDamage > 0.0F ? w.vortexBurstDamage * 1.8F
                                                                  : w.damage * 3.0F * 1.8F);
      w.vortexBurstRadius *= 1.2F;
    }
    syncVortices(slotIndex);
  } else if (effect == "w_orb_grow") {
    // --- Stackable cards for the weapons that only had their one unique ------
    // These are the identity parameters that no card used to touch, so a Void
    // Orb or a Solar Lance could be picked up and never scale again. Each one
    // is a normal (stackable) card, which is what a build with a favourite
    // weapon needs most: a second, then a third, then a fourth reason to keep
    // it.
    // The orb's size comes from the projectile stat, not from a field, so this
    // buys what it can: it reacquires the next victim faster and decays far
    // less per bounce. Both matter, because a single eternal orb that loses a
    // third of its power every hop is a weapon that dies in ten seconds.
    w.bounceRange *= 1.3F;
    w.bounceDamageMul = std::max(w.bounceDamageMul, 0.9F);
  } else if (effect == "w_scythe_reach") {
    w.sweepRadius *= 1.0F + value * 0.06F;
  } else if (effect == "w_beam_lance") {
    w.beamRange *= 1.0F + value * 0.06F;
    w.beamWidth *= 1.0F + value * 0.04F;
  } else if (effect == "w_chain_arc") {
    w.chainJumpRange *= 1.0F + value * 0.06F;
    w.chainDamageMul = std::min(1.0F, w.chainDamageMul + value * 0.05F);
  } else if (effect == "w_nova_wide") {
    w.novaMaxRadius *= 1.0F + value * 0.06F;
    w.novaExpandSpeed *= 1.0F + value * 0.04F;
  } else if (effect == "w_reap_wide") {
    w.sweepRadius *= 1.0F + value * 0.06F;
    w.zoneDuration += value * 0.5F;
  } else if (effect == "w_boomerang_reach") {
    w.boomerangRange *= 1.0F + value * 0.07F;
    w.boomerangReturnSpeed *= 1.0F + value * 0.06F;
  } else if (effect == "w_zone_pools") {
    // More lingering fire on the ground at once. The pool cap is applied in the
    // Zone bookkeeping, so this can never leak entities.
    w.zoneMaxPools += static_cast<int>(value);
  } else if (effect == "w_bomb_blast") {
    w.bombExplodeRadius *= 1.0F + value * 0.08F;
    w.bombKnockback *= 1.0F + value * 0.06F;
  } else if (effect == "w_lure_anchor") {
    // The bell survives longer and one more may be planted at a time, so the
    // taunt is a persistent engine instead of a one-shot.
    w.lureDuration += value * 0.8F;
    w.lureMaxBeacons += 1;
  } else if (effect == "w_prism_lattice") {
    // Prism Array: one more beam corridor per stack, on top of the global
    // projectile card (which the prism ignores, since its count is its own
    // field). Capped at 6 so the screen never fills with beams.
    w.prismMaxTargets = std::min(6, w.prismMaxTargets + static_cast<int>(value));
    w.prismRange *= 1.0F + value * 0.05F;
  } else if (effect == "w_halo_wings") {
    // Radiant Halo: the ring reaches further and its beams shove a little more,
    // so the two halo-family weapons stop at exactly one card each.
    w.beamRange *= 1.0F + value * 0.08F;
    w.haloKnockback += value * 1.5F;
    syncHaloBeams(slotIndex);
  } else if (effect == "w_vortex_core") {
    // Void Gyre: a fatter core, a wider orbit and a faster spin. The orbit
    // grows sub-linearly with stacks so the ring cannot creep to the screen
    // edge, and syncVortices re-reads all three immediately.
    w.vortexRadius *= 1.0F + value * 0.08F;
    w.vortexOrbit *= 1.0F + value * 0.07F;
    w.vortexOrbitSpeed *= 1.0F + value * 0.12F;
    syncVortices(slotIndex);
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
    shieldDelay_ = shieldRegenDelay();
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
    shieldDelay_ = shieldRegenDelay();
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
  savedBlocked_ = blocked_;
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
  blocked_ = savedBlocked_;
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
  // Leave the sandbox on the way out. It used not to, so the death screen came
  // up wearing the sandbox keymap -- and with testMode_ still true, the two keys
  // that would have got you out of it (T and [5] CLOSE) were both dead, leaving
  // only R and the Q Q abandon. The run is over; the sandbox has no business
  // still being open on top of it.
  if (testMode_) {
    testMode_ = false;
    testShopOpen_ = false;
  }
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
    // What the card actually does, right-aligned and wrapped into the space
    // left over by the name. This used to measure the description and simply
    // not draw it if it was wider than half the panel, so every long item in
    // the picker showed as a bare name with no explanation at all.
    const float nameW = b.textWidth(1.6F, label);
    const float hintBox = panelW - 48.0F - nameW;
    if (hintBox > 60.0F) {
      // The hint is right-aligned, so the indent goes on the RIGHT of the box:
      // wrapped lines hang toward the panel edge and the first line, which sits
      // against the name, is the one that gets the full width.
      const float hintX = x0 + panelW - 20.0F;
      const auto hintLines =
          wrapToWidth(def.desc, hintBox, 1.2F, 12.0F);
      const Color hintCol = usable ? Color{0.55F, 0.55F, 0.65F, 1.0F} : maxed;
      // Two lines at most: the row pitch is 19px, so a third would land on
      // top of the next row's name.
      int drawn = 0;
      for (const auto& hl : hintLines) {
        if (drawn >= 2) break;
        const float lx = hintX - b.textWidth(1.2F, hl) - (drawn == 0 ? 0.0F : 12.0F);
        b.text(lx, y + (drawn == 0 ? 3.0F : 1.0F), 1.2F, hintCol, hl);
        ++drawn;
      }
      if (hintLines.size() > 2) {
        // Say that there is more rather than silently cutting it off.
        b.text(hintX - 12.0F, y + 1.0F, 1.2F, hintCol, "+");
      }
    }
  }
}

void Game::testSpawnEliteAt(float x, float y, int tier) {
  testSpawnEnemyAt(x, y);
  const entt::entity e = lastTestSpawn_;
  registry_.emplace_or_replace<EnemyTraits>(e, EnemyTraits{});
  registry_.get<EnemyTraits>(e).tier = static_cast<std::uint8_t>(std::max(1, tier));
}

void Game::testKillLastSpawned() {
  if (lastTestSpawn_ == entt::null || !registry_.valid(lastTestSpawn_)) return;
  killEnemy(lastTestSpawn_);
  lastTestSpawn_ = entt::null;
}

int Game::testSpawnChest(float x, float y, int tier, int grants) {
  const auto box = spawnChest(x, y, tier);
  if (box == entt::null) return 0;
  // `grants` < 0 means "whatever the tier promised", which is the interesting
  // case; a non-negative one overrides it so a test can set up a box of a known
  // size without needing an arsenal that can absorb it.
  if (grants >= 0) registry_.get<Chest>(box).grants = grants;
  return registry_.get<Chest>(box).grants;
}

int Game::testWeaponContentIndex(std::string_view id) const {
  for (std::size_t i = 0; i < content_.weapons.size(); ++i) {
    if (content_.weapons[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

void Game::testOpenFirstChest() {
  auto view = registry_.view<Chest>();
  if (view.empty()) return;
  const entt::entity e = *view.begin();
  const auto& t = registry_.get<Transform>(e);
  const int given = openChest(registry_.get<Chest>(e).grants);
  if (given > 0) {
    lastChestGrants_ = given;
    lastChestTimer_ = kChestToastTime;
  }
  spawnParticles(t.x, t.y, {1.0F, 0.85F, 0.35F, 1.0F}, 12, 5.0F);
  registry_.destroy(e);
}

int Game::testChestCount() const {
  int n = 0;
  auto view = registry_.view<Chest>();
  for (const auto e : view) {
    (void)e;
    ++n;
  }
  return n;
}

void Game::testSpawnEnemyAt(float x, float y) {
  const auto e = registry_.create();
  lastTestSpawn_ = e;
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

void Game::testDespawnEnemies() {
  std::vector<entt::entity> dead;
  for (const auto e : registry_.view<Enemy>()) dead.push_back(e);
  for (const auto e : dead) {
    if (registry_.valid(e)) registry_.destroy(e);
  }
}

void Game::testSpawnTieredEnemyAt(float x, float y, int tier, std::uint32_t traits,
                                  int def) {
  const int t = std::clamp(tier, 0, 3);
  const int d = std::clamp(def, 0, static_cast<int>(content_.enemies.size()) - 1);
  const TierBuffs buffs = tierBuffs(t);
  // The global time ramps are applied here too, because this hook stands in for
  // a real spawn and a spawn at minute eight IS scaled. Leaving them off made
  // every "what does an enemy look like" test silently run at minute zero, which
  // is exactly the kind of gap the difficulty curves can then drift through.
  float hpScale = 1.0F;
  float speedScale = 1.0F;
  float touchScale = 1.0F;
  currentScales(hpScale, speedScale, touchScale);
  PendingSpawn p{};
  p.x = x;
  p.y = y;
  p.t = 0.0F;
  p.def = d;
  p.hpMul = hpScale * rollTierHpMul(buffs, rng_);
  p.touchMul = touchScale * buffs.touch;
  p.speedMul = speedScale * buffs.speed;
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

void Game::testSetPlayerPosition(float x, float y) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& t = registry_.get<Transform>(player_);
  t.px = x;
  t.py = y;
  t.x = x;
  t.y = y;
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

void Game::testDamagePlayer(float amount) {
  if (player_ == entt::null || !registry_.valid(player_)) return;
  auto& hp = registry_.get<Health>(player_);
  // Leave 1 HP: a test that is measuring a heal must not be racing the lethal
  // rule, and a 0 HP player is a GameOver state that would swallow the rest of
  // the test's frames.
  hp.hp = std::max(1.0F, hp.hp - amount);
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

std::vector<float> Game::testEnemySpeedMuls() const {
  std::vector<float> out;
  auto view = registry_.view<Enemy>();
  out.reserve(enemyCount() * 2);
  for (const auto e : view) {
    const auto& en = view.get<Enemy>(e);
    out.push_back(en.slowT > 0.0F ? en.slowMul : 1.0F);
    out.push_back(en.slowT);
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

std::size_t Game::testChainCount() const {
  return registry_.view<ChainLightning>().size();
}

std::size_t Game::testWaveCount() const {
  return registry_.view<WaveEffect>().size();
}

std::size_t Game::testBounceCount() const {
  return registry_.view<BounceProjectile>().size();
}

std::size_t Game::testProjectileCount() const {
  return registry_.view<Projectile>().size();
}

std::vector<float> Game::testBombPositions() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Transform, BombProjectile>()) {
    const auto& t = registry_.get<Transform>(e);
    out.push_back(t.x);
    out.push_back(t.y);
  }
  return out;
}

std::vector<float> Game::testNovaRadii() const {
  std::vector<float> out;
  for (const auto e : registry_.view<NovaRing>()) {
    out.push_back(registry_.get<NovaRing>(e).radius);
  }
  return out;
}

std::vector<float> Game::testHaloBeamInners() const {
  std::vector<float> out;
  for (const auto e : registry_.view<HaloBeam>()) {
    out.push_back(registry_.get<HaloBeam>(e).inner);
  }
  return out;
}

std::vector<float> Game::testVortexCharges() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Vortex>()) {
    const auto& vx = registry_.get<Vortex>(e);
    out.push_back(vx.charge);
    out.push_back(vx.angle);
  }
  return out;
}

std::vector<float> Game::testBoomerangPositions() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Transform, BoomerangProjectile>()) {
    const auto& t = registry_.get<Transform>(e);
    out.push_back(t.x);
    out.push_back(t.y);
  }
  return out;
}

float Game::testEnemyHpNear(float x, float y) const {
  entt::entity best = entt::null;
  float bestD2 = 1e12F;
  for (const auto e : registry_.view<Transform, Health, Enemy>()) {
    const auto& t = registry_.get<Transform>(e);
    const float dx = t.x - x;
    const float dy = t.y - y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < bestD2) {
      bestD2 = d2;
      best = e;
    }
  }
  if (best == entt::null) return -1.0F;
  return registry_.get<Health>(best).hp;
}

float Game::testOrbitWindowAngle(int slot) const {
  if (slot < 0 || slot >= weaponCount_) return -1.0F;
  const auto& w = weapons_[slot];
  if (w.attackType != AttackType::Orbit || !w.orbitWindow) return -1.0F;
  const int blades = std::max(1, w.projectiles + stats_.projAdd);
  if (blades < 2) return -1.0F;
  for (const auto e : registry_.view<OrbitBlade>()) {
    if (registry_.get<OrbitBlade>(e).weaponIndex == slot) {
      return registry_.get<OrbitBlade>(e).angle + kPi / static_cast<float>(blades);
    }
  }
  return -1.0F;
}

std::vector<int> Game::testChainTelegraphs() const {
  // Milliseconds of wind-up left, rounded, so a test can assert "this bolt has not
  // struck yet" without depending on the exact frame the timer happened to land
  // on. A landed bolt is -1.
  std::vector<int> out;
  for (const auto e : registry_.view<ChainLightning>()) {
    const auto& cl = registry_.get<ChainLightning>(e);
    out.push_back(cl.telegraph > 0.0F ? static_cast<int>(cl.telegraph * 1000.0F) : -1);
  }
  return out;
}

std::vector<float> Game::testWavePositions() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Transform, WaveEffect>()) {
    const auto& t = registry_.get<Transform>(e);
    out.push_back(t.x);
    out.push_back(t.y);
  }
  return out;
}

std::vector<float> Game::testEnemyPositions() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Transform, Enemy>()) {
    const auto& t = registry_.get<Transform>(e);
    out.push_back(t.x);
    out.push_back(t.y);
  }
  return out;
}

std::vector<float> Game::testBouncePositions() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Transform, BounceProjectile>()) {
    const auto& t = registry_.get<Transform>(e);
    out.push_back(t.x);
    out.push_back(t.y);
  }
  return out;
}

std::vector<float> Game::testEnemyChills() const {
  std::vector<float> out;
  for (const auto e : registry_.view<Enemy>()) {
    const auto& en = registry_.get<Enemy>(e);
    // 1.0 is reported for an enemy that is not chilled, so a test reads "is it
    // slowed" without having to know the unchilled default.
    out.push_back(en.slowT > 0.0F ? en.slowMul : 1.0F);
    out.push_back(en.slowT);
  }
  return out;
}

void Game::testAdvance(float seconds) {
  // Fixed 1/60 steps, same as fixedStep(). A test that only watched entity
  // counts right after adding a weapon would never see the fire path run, and
  // the whole point of these hooks is to watch a cascade start.
  const int steps = static_cast<int>(std::max(0.0F, seconds) * 60.0F);
  for (int i = 0; i < steps; ++i) {
    fixedStep();
  }
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

std::vector<std::string> wrapToWidth(std::string_view str, float maxWidth, float scale,
                                    float indent, float contScale) {
  if (maxWidth <= 0.0F || scale <= 0.0F) return {std::string(str)};
  // The 5x7 bitmap font advances 6*scale pixels per character, so a pixel
  // budget maps onto a character count. The indent eats width on every line
  // after the first, which is why the loop narrows the budget as it goes
  // rather than subtracting once up front.
  //
  // `contScale` is the size the CONTINUATION lines will be drawn at. When they
  // are drawn larger than the first line (which is the card's style) the wrap
  // has to measure them at that larger size, or every one of them overflows the
  // card by exactly the size of the bump. Zero or negative means "the same as
  // the first line", which is the plain single-size case.
  const float cs = contScale > 0.0F ? contScale : scale;
  // The character budget of line `idx`, which shrinks by one indent for every
  // line after the first. It has to be a function of the line index rather than
  // a single number: a word that does not fit the line it is on gets moved to
  // the NEXT line, and that line is the narrower one.
  const auto budgetFor = [&](std::size_t idx) {
    const float advance = 6.0F * (idx == 0 ? scale : cs);
    return static_cast<std::size_t>(
        std::max(1.0F, (maxWidth - (idx == 0 ? 0.0F : indent)) / advance));
  };
  std::vector<std::string> lines;
  std::string current;
  std::string_view rest(str);
  std::size_t maxChars = budgetFor(0);
  while (!rest.empty()) {
    const std::size_t space = rest.find(' ');
    std::string_view word = rest.substr(0, space);
    if (space != std::string_view::npos) rest.remove_prefix(space + 1);
    else rest = std::string_view();
    // Start a new line only if the word cannot share this one.
    if (!current.empty() && current.size() + 1 + word.size() > maxChars) {
      lines.push_back(current);
      current.clear();
      maxChars = budgetFor(lines.size());
    }
    // A word too long for a whole line is hard-broken rather than allowed to
    // run off the panel. wrapWords() deliberately does NOT do this (its own test
    // pins that contract), but there the limit is a character count for a list
    // of short identifiers, whereas here it is a pixel box on a card: an
    // over-long word ("invulnerability" is 15 characters and a 4-card row leaves
    // room for 14) would otherwise put a character outside the card.
    while (word.size() > maxChars) {
      if (!current.empty()) {
        lines.push_back(current);
        current.clear();
        maxChars = budgetFor(lines.size());
      }
      lines.emplace_back(word.substr(0, maxChars));
      word.remove_prefix(maxChars);
      maxChars = budgetFor(lines.size());
    }
    if (word.empty()) continue;
    if (!current.empty()) current += ' ';
    current.append(word);
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

// The card body's hanging indent. Twice the card's 16px left padding, so a
// wrapped continuation line sits visibly inboard of the first line instead of
// blending into it.
constexpr float kCardIndent = 32.0F;

// The card body is set at a FIXED scale, not derived from the card width.
//
// Deriving it was tried: clamp((cardW - 64) / 26, 1.3, 1.6). For every card
// width the level-up row can actually produce -- 200 to 400 -- that expression
// is 5.2 to 12.9, so it always clamped to the maximum and the "derivation" was a
// constant wearing a formula. The premise behind it was wrong anyway: a
// narrower card does not need smaller text to keep a readable measure, because
// the card being narrower already shortens the measure. At a fixed scale the
// first line holds (cardW - 32) / 9.6 characters -- about 20 on a 5-card row and
// about 38 on a 3-card row -- which is the range the derivation was trying to
// manufacture in the first place.
//
// The value is deliberately modest. Blowing the body text up to fill the card
// was tried and it was the wrong instinct: bigger is not more readable once the
// measure drops to a dozen characters per line, and it made the card a wall of
// type. The wrapped lines are where the emphasis belongs, not the first.
constexpr float kCardDescScale = 1.6F;

// Horizontal padding and body line pitch of the card, and how much louder the
// wrapped lines speak than the first one.
constexpr float kCardPadX = 16.0F;
constexpr float kCardLineH = 15.0F;
constexpr float kCardContScaleMul = 1.18F;
constexpr float kCardContLineMul = 1.45F;

CardTextLayout cardTextLayout(float cardW) {
  CardTextLayout lay;
  lay.width = cardW - kCardPadX * 2.0F;
  lay.scale = kCardDescScale;
  // Wrapped lines are set LARGER than the first, not smaller, and are pushed
  // down a little further than the normal pitch: same indent, more ink.
  lay.contScale = lay.scale * kCardContScaleMul;
  lay.lineH = kCardLineH;
  lay.contLineH = kCardLineH * kCardContLineMul;
  lay.indent = kCardIndent;
  return lay;
}

float cardTextHeight(int lines, const CardTextLayout& lay) {
  if (lines <= 0) return 0.0F;
  if (lines == 1) return lay.lineH;
  return lay.lineH + static_cast<float>(lines - 1) * lay.contLineH;
}

// The level-up card row's geometry, as one function so it can be tested. The
// renderer calls this, so a test that checks "the row fits on screen" is
// checking the renderer's own arithmetic rather than a copy of it.
Game::LevelUpRow Game::levelUpRowLayout(float px, float py, std::size_t n,
                                        std::size_t tallestDescLines,
                                        std::size_t tallestNameLines) {
  LevelUpRow out;
  if (n == 0) {
    out.top = py * 0.32F;
    out.cardH = 176.0F;
    out.hintY = py * 0.66F;
    out.bodyLines = 4;
    return out;
  }
  const float gap = 24.0F;
  const float avail =
      (px - gap * static_cast<float>(n - 1) - 40.0F) / static_cast<float>(n);
  // Width: the 200px floor is a legibility PREFERENCE, not a constraint. It was
  // applied as a clamp, which meant a 3-card row on a 640px-wide window -- 184px
  // each after the gaps and the side margin -- drew at 200 and put 4px of the
  // first and last card off each edge of the screen. So the preference is capped
  // by what the row's share of the window actually is. Below the floor the card
  // simply wraps into more lines; the body scale is a constant, so a narrow card
  // is still legible.
  out.cardW = std::min(std::clamp(avail, 200.0F, 400.0F), avail);
  const CardTextLayout lay = cardTextLayout(out.cardW);
  out.nameLines = static_cast<int>(std::max<std::size_t>(tallestNameLines, 1));
  // Where the description starts: below the [1] key badge (52), plus the title
  // block, plus 10px of air. For a one-line title that is the 86 this has always
  // been, so nothing moves unless a title actually wraps.
  const float footer = 30.0F;
  out.bodyTop = 52.0F + 24.0F * static_cast<float>(out.nameLines) + 10.0F;
  // The height ceiling is above what any reachable row actually needs, not a
  // tight fit: the worst case is a 4-card row with a two-line title, which wants
  // 110 + 211 + 30 = 351. At 340 it lost a line of a long description, so the
  // ceiling was silently truncating cards the players DO see. Being a ceiling
  // rather than a target, raising it costs nothing for a short description --
  // the row is still as tall as its own tallest body.
  out.cardH = std::clamp(
      out.bodyTop + cardTextHeight(static_cast<int>(tallestDescLines), lay) + footer,
      150.0F, 364.0F);
  out.bodyLines = cardTextLinesThatFit(out.cardH - out.bodyTop - footer, lay);
  // The row's TOP is clamped so the whole card plus the reroll hint below it
  // stays on screen. It used to be a fixed py * 0.32, which needs a window of
  // at least (cardH + 68) / 0.68 -- 503px for a 4-card row -- and the window is
  // resizable with no minimum, so a short window pushed the cards and the hint
  // clean off the bottom. Only the width was ever tested. The outer max stops
  // the clamp from shoving the row into the title on a window too short to hold
  // it at all, which then clips the bottom -- the lesser of the two evils, and
  // only reachable by dragging the window to about 300px tall.
  out.top = std::max(py * 0.16F + 44.0F, std::min(py * 0.32F, py - out.cardH - 44.0F));
  out.hintY = out.top + out.cardH + 24.0F;
  return out;
}

int cardTextLinesThatFit(float avail, const CardTextLayout& lay) {
  if (avail < lay.lineH) return 1;
  return 1 + static_cast<int>((avail - lay.lineH) / lay.contLineH);
}

// Greedy word-wrap: renders str under `lay`, indenting every continuation line
// by lay.indent and drawing it at lay.contScale. Draws at most `maxLines` lines
// (-1 for all of them) and returns the number drawn, so a caller with a fixed
// box can clamp instead of letting the text run off the bottom of it.
//
// When the clamp actually bites, the last visible line gets an ellipsis. A
// description that stops mid-sentence with no marker reads as a complete
// thought, which is worse than a slightly longer panel.
static int renderWrappedText(core::render::Batcher& b, float x, float y,
                             const CardTextLayout& lay, core::render::Color c,
                             std::string_view str, int maxLines = -1) {
  // The continuation lines are set at their own scale, so the wrap has to be
  // MEASURED at that scale too. Measuring them at the first line's size is what
  // makes a line quietly overflow the card by exactly the size of the bump.
  const auto lines = wrapToWidth(str, lay.width, lay.scale, lay.indent, lay.contScale);
  const bool clipped = maxLines >= 0 && static_cast<std::size_t>(maxLines) < lines.size();
  const int limit = maxLines < 0 ? static_cast<int>(lines.size()) : maxLines;
  int drawn = 0;
  for (; drawn < limit && drawn < static_cast<int>(lines.size()); ++drawn) {
    const std::string_view text = lines[static_cast<std::size_t>(drawn)];
    const bool cont = drawn > 0;
    const float s = cont ? lay.contScale : lay.scale;
    const float lineX = x + (cont ? lay.indent : 0.0F);
    // A continuation line gets its own budget: it is indented, so it has less
    // room to work with than the first line did.
    const float lineMax = lay.width - (cont ? lay.indent : 0.0F);
    if (clipped && drawn == limit - 1) {
      // Make room for "..." on the final line, trimming characters from the
      // end of the text if the line is already full.
      constexpr std::string_view kDots = "...";
      const float dotsW = static_cast<float>(kDots.size()) * 6.0F * s;
      std::size_t keep = text.size();
      while (keep > 0 && static_cast<float>(keep) * 6.0F * s + dotsW > lineMax) {
        --keep;
      }
      std::string last(text.substr(0, keep));
      last.append(kDots);
      b.text(lineX, y, s, c, last);
      y += cont ? lay.contLineH : lay.lineH;
      continue;
    }
    b.text(lineX, y, s, c, text);
    y += cont ? lay.contLineH : lay.lineH;
  }
  return drawn;
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
    case AttackType::Wave: return "wave";
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

// The card's title, wrapped to two lines rather than sliced or shrunk.
CardNameLayout cardNameLayout(std::string_view name, float maxWidth) {
  CardNameLayout out;
  if (name.empty()) return out;
  // The 5x7 font advances 6*scale pixels per character, so a pixel budget maps
  // onto a character count. The comfortable size is tried first, and only if the
  // name will not fit on one line there does it drop to the wrapped size.
  constexpr float kNameScale = 2.3F;
  constexpr float kNameWrapScale = 2.0F;
  if (static_cast<float>(name.size()) * 6.0F * kNameScale <= maxWidth) {
    out.scale = kNameScale;
    out.lines = {std::string(name)};
    return out;
  }
  out.scale = kNameWrapScale;
  out.lines = wrapToWidth(name, maxWidth, kNameWrapScale, 0.0F, kNameWrapScale);
  // A title is one to three words, so two lines is enough for every shipped
  // name. A third line would mean the wrap point is inside a single word (a very
  // long name with no spaces), which reads worse than an ellipsis, so cap it
  // here and let the caller mark the tail.
  if (out.lines.size() > 2) {
    out.lines.resize(2);
    std::string& last = out.lines.back();
    while (static_cast<float>(last.size() + 3) * 6.0F * kNameWrapScale > maxWidth &&
           !last.empty()) {
      last.pop_back();
    }
    last += "...";
    out.elided = true;
  }
  return out;
}

// The card's title. It was drawn at a fixed 2.3 with no width check, and
// "Total Internal Reflection" is 345px in a 260px cell -- so the tail of the
// name ran under the next card's 95%-opaque panel with no ellipsis, which reads
// as a misspelled word rather than a long name. Stepping the scale down to 1.7
// was the first attempt and it was also wrong: that is below the body's own
// first-line scale, so a long name stopped reading as a title at all, and 25
// characters still did not fit the narrowest card. Wrapping keeps the whole
// name at a size that is still larger than the body.
void Game::drawCardName(core::render::Batcher& b, std::string_view name, float x, float y,
                        float maxWidth, const core::render::Color& accent,
                        const core::render::Color& fallback) {
  const CardNameLayout lay = cardNameLayout(name, maxWidth);
  float ly = y;
  for (std::size_t i = 0; i < lay.lines.size(); ++i) {
    const bool last = i + 1 == lay.lines.size();
    b.text(x, ly, lay.scale, lay.elided && last ? accent : fallback, lay.lines[i]);
    ly += lay.lineH;
  }
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
  // Shield shows its refill rate and its delay, because a shield build is only
  // half a pool and half a clock: 400 HP that trickles back at 10/s is a very
  // different weapon from 400 HP that trickles back at 30/s.
  rows.push_back("SHIELD " + std::to_string(static_cast<int>(shield_)) + "/" +
                 std::to_string(static_cast<int>(stats_.shieldMax)) + " +" +
                 std::to_string(static_cast<int>(shieldRegenRate())) + "/S AFTER " +
                 fit1(shieldRegenDelay()) + "S");
  rows.push_back("DEFENSE " + std::to_string(static_cast<int>(stats_.defense)));
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
  const std::string title = "PLAYER - PAUSED  [ESC] RESUME  [B] BESTIARY";
  // The panel is sized to its CONTENT, not to a literal. It used to be a fixed
  // 560px, which is narrower than its own title: "PLAYER - PAUSED [ESC] RESUME
  // [B] BESTIARY" is 774px at scale 3.0, so 238px of the title hung off the
  // right edge of its own panel on every single pause, and the ability row (600px
  // at base stats, 792px once the cooldown and echo cards are taken) hung off it
  // too. Measuring is the only version of this that cannot go stale the next time
  // a row gets longer.
  float contentW = b.textWidth(3.0F, title);
  for (const auto& r : rows) {
    if (!r.empty()) contentW = std::max(contentW, b.textWidth(2.0F, r));
  }
  const float panelW = std::min(px - 24.0F, std::max(560.0F, contentW + padX * 2.0F + 16.0F));
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
  b.text(panelX + padX, panelY + 14.0F, 3.0F, gold, title);

  // Rows are clipped to the panel. The height is already clamped to the window,
  // so on a short window the list used to run off the bottom of its own
  // backdrop and continue over the world with no marker at all. Anything that
  // does not fit is counted and said out loud rather than silently drawn.
  float y = panelY + 52.0F;
  const float clipY = panelY + panelH - 12.0F;
  std::size_t skipped = 0;
  for (const auto& r : rows) {
    const float step = r.empty() ? 10.0F : lineH;
    if (y + step > clipY) {
      ++skipped;
      continue;
    }
    if (r.empty()) {
      y += step;
      continue;
    }
    b.text(panelX + padX, y, 2.0F, dim, r);
    y += step;
  }
  if (skipped > 0) {
    b.text(panelX + padX, y, 1.6F, core::render::Color{1.0F, 0.6F, 0.5F, 1.0F},
           "+" + std::to_string(skipped) + " MORE LINES - RESIZE THE WINDOW");
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
  // Six text rows: roman+name at +8, traits/XP/gate at +26, the live gate status
  // at +38, the stat line at +52, the resistance line at +66, then up to four
  // wrapped pool lines from +80 at a 12px pitch -- which lands the last one at
  // +116 and a 19px line, so 132 is the tight fit. It used to be 112, which is
  // exactly why the stat line and the gate status ended up sharing a slot.
  const float tribH = 132.0F;
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
      // Its OWN row, at tribY + 38. This used to be drawn at tribY + 40 -- the
      // exact slot the stat line below uses -- so the two overwrote each other
      // and the gate progress, which is the whole point of the adaptive
      // director, was never once visible on screen. The stat line was drawn
      // second, so it always won.
      b.text(x + 14.0F, tribY + 38.0F, 1.25F,
             open ? tb.color : Color{dim.r, dim.g, dim.b, 0.7F}, buf);
    }

    std::snprintf(buf, sizeof(buf), "HP x%.0f-%.0f  DMG x%.1f  SPD x%.2f  DEF %d",
                  static_cast<double>(buffs.hpMin), static_cast<double>(buffs.hpMax),
                  static_cast<double>(buffs.touch), static_cast<double>(buffs.speed),
                  static_cast<int>(enemyDefense(simTime_, tb.tier)));
    b.text(x + 14.0F, tribY + 52.0F, 1.25F, statBlue, buf);

    // Resistances at the current run time (they grow with it, so this is the
    // live number, not a constant).
    std::snprintf(buf, sizeof(buf), "LIFE RES %d%%  KB RES %d%%  (PIERCE %d)",
                  static_cast<int>(enemyLifestealResistance(simTime_, tb.tier, false) * 100.0F),
                  static_cast<int>(enemyKnockbackResistance(simTime_, tb.tier, false) * 100.0F),
                  static_cast<int>(stats_.armorPierce));
    b.text(x + 14.0F, tribY + 66.0F, 1.25F, resOrange, buf);

    // The full ability pool, word-wrapped. The wrap is 56 characters, not 40:
    // at 40 the eleven trait names needed four lines and the fourth -- which is
    // "RESIST +40 DEF" -- was silently dropped by the 3-line cap, so every
    // panel ended mid-list and the player never learned the pool included a
    // defence trait. 56 fits the list in three lines at this column width, and
    // the cap is now 4 so a future trait does not vanish without a trace.
    std::string pool;
    for (const char* n : kTraitNames) {
      if (!pool.empty()) pool += "  ";
      pool += n;
    }
    const std::vector<std::string> lines = wrapWords(pool, 56);
    float ly = tribY + 80.0F;
    for (std::size_t li = 0; li < lines.size() && li < 4; ++li) {
      b.text(x + 14.0F, ly, 1.15F, violet, lines[li]);
      ly += 12.0F;
    }
    // Say so when the cap actually bites, rather than pretending the list ends.
    if (lines.size() > 4) {
      b.text(x + 14.0F, ly, 1.15F, core::render::Color{violet.r, violet.g, violet.b, 0.6F},
             "+ MORE");
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

// ---------------------------------------------------------------------------
// In-game manual.
//
// The same documentation docs/ ships, trimmed to the 5x7 bitmap font's ASCII
// range and split into one-screen pages. Layout: a page list down the left, the
// current page's lines on the right, the hint bar at the bottom.
//
// A page is allowed to be taller than the screen — the content is data, and a
// long page should read as "this topic is long" rather than silently lose its
// last lines off the bottom. The test asserts every page fits, so the shipped
// manual is always complete; the clamp here is the safety net for a future
// contributor who adds one that does not.
// ---------------------------------------------------------------------------
void Game::renderManual(core::render::Batcher& b, float px, float py) {
  using core::render::Color;
  const Color gold{1.0F, 0.85F, 0.4F, 1.0F};
  const Color dim{0.66F, 0.66F, 0.76F, 1.0F};
  const Color white{1.0F, 1.0F, 1.0F, 1.0F};
  const Color violet{0.75F, 0.5F, 1.0F, 1.0F};
  const Color bullet{0.55F, 0.9F, 1.0F, 1.0F};

  b.rectTopLeft(0.0F, 0.0F, px, py, Color{0.04F, 0.035F, 0.08F, 0.97F});

  if (content_.manual.empty()) {
    // A build with no manual.toml must still say something honest.
    const std::string none = "NO MANUAL IN THIS BUILD";
    b.text(px * 0.5F - b.textWidth(3.0F, none) * 0.5F, py * 0.45F, 3.0F, dim, none);
    const std::string hint = "[F1] CLOSE";
    b.text(px * 0.5F - b.textWidth(1.8F, hint) * 0.5F, py - 40.0F, 1.8F, dim, hint);
    return;
  }

  // Bound-checked, because everything below indexes by it. openManual() clamps
  // it and the page controls only ever move it within range, so the empty case is
  // the only way to get here out of bounds — but "should not happen" is not
  // "cannot", and this is the difference between a wrong page and a read past
  // the end of a vector.
  if (manualPage_ >= content_.manual.size()) {
    b.text(px * 0.5F - b.textWidth(2.4F, "NO PAGE") * 0.5F, py * 0.45F, 2.4F, dim,
           "NO PAGE");
    b.text(px * 0.5F - b.textWidth(1.8F, "[F1] CLOSE") * 0.5F, py - 40.0F, 1.8F, dim,
           "[F1] CLOSE");
    return;
  }
  const ManualPage& page = content_.manual[manualPage_];
  const std::size_t total = content_.manual.size();
  const std::size_t n = manualPage_ + 1;

  // --- Title -----------------------------------------------------------------
  const std::string title = "MANUAL  " + std::to_string(n) + "/" + std::to_string(total);
  b.text(px * 0.5F - b.textWidth(3.4F, title) * 0.5F, 20.0F, 3.4F, gold, title);
  const float subW = std::min(px * 0.5F, 460.0F);
  for (int k = 0; k <= 40; ++k) {
    const float t = static_cast<float>(k) / 40.0F;
    b.circle(px * 0.5F - subW * 0.5F + subW * t, 54.0F, 1.2F,
             Color{gold.r, gold.g, gold.b, 0.30F});
  }

  // --- Page list (left rail) -------------------------------------------------
  {
    const float listX = 26.0F;
    const float listY0 = 74.0F;
    const float rowH = 22.0F;
    // The rail is sized to its own labels and is forbidden from reaching the
    // body column. It used to be a hardcoded 196px, which page 6's
    // "6  ABILITIES  J / K / L" overran by 33px -- so the selected page's own
    // highlight bar stopped short of its own label, and the label's tail landed
    // 3px short of the text it was supposed to be separating from.
    const float railBudget = kManualBodyX - listX - 14.0F;
    float railW = 0.0F;
    for (std::size_t i = 0; i < total; ++i) {
      railW = std::max(railW, b.textWidth(1.6F, std::to_string(i + 1) + "  " +
                                                        content_.manual[i].title));
    }
    railW = std::clamp(railW + 10.0F, 90.0F, railBudget);
    for (std::size_t i = 0; i < total; ++i) {
      const float y = listY0 + static_cast<float>(i) * rowH;
      const bool sel = (i == manualPage_);
      const auto& p = content_.manual[i];
      // Highlight bar behind the active page.
      if (sel) {
        b.rectTopLeft(listX - 8.0F, y - 4.0F, railW, 18.0F,
                      Color{gold.r, gold.g, gold.b, 0.16F});
        b.circle(listX - 12.0F, y + 4.0F, 3.5F, gold);
      }
      std::string label = std::to_string(i + 1) + "  " + p.title;
      if (b.textWidth(1.6F, label) > railW - 6.0F) {
        while (!label.empty() && b.textWidth(1.6F, label + "...") > railW - 6.0F) {
          label.pop_back();
        }
        label += "...";
      }
      b.text(listX, y, 1.6F, sel ? gold : dim, label);
    }
  }

  // --- Page body (right) -----------------------------------------------------
  {
    const float bodyX = kManualBodyX;
    const float bodyY0 = kManualBodyY;
    const float lineH = kManualLineH;
    // The body must fit the gap between the rail and the hint bar.
    const float maxLines =
        std::max(1.0F, (py - bodyY0 - kManualBottomPad) / lineH);
    const std::size_t shown = std::min<std::size_t>(page.lines.size(),
                                                    static_cast<std::size_t>(maxLines));
    for (std::size_t i = 0; i < shown; ++i) {
      std::string_view raw = page.lines[i];
      const float y = bodyY0 + static_cast<float>(i) * lineH;
      // Leading markers are markup, not text: '>' is a highlighted bullet, '#' is
      // a sub-heading, and "  " is an indent under the bullet above.
      if (!raw.empty() && raw.front() == '>') {
        b.text(bodyX, y, kManualBodyScale, gold, ">");
        b.text(bodyX + kManualIndent, y, kManualBodyScale, bullet, raw.substr(1));
      } else if (!raw.empty() && raw.front() == '#') {
        b.text(bodyX, y, kManualBodyScale, violet, raw.substr(1));
      } else if (raw.size() >= 2 && raw[0] == ' ' && raw[1] == ' ') {
        b.text(bodyX + kManualIndent, y, kManualBodyScale, dim, raw.substr(2));
      } else if (raw.empty()) {
        b.circle(bodyX + 4.0F, y + 5.0F, 1.0F, Color{dim.r, dim.g, dim.b, 0.35F});
      } else {
        b.text(bodyX, y, kManualBodyScale, white, raw);
      }
    }
    // Overflow marker, so a clipped page says it is clipped. It used to read
    // "...MORE, NEXT PAGE", which is the wrong instruction twice over: the next
    // page is a different topic, not a continuation, and there is no in-page
    // scroll for "more" to refer to. What is true is that the text ends here.
    if (page.lines.size() > shown) {
      const float y = bodyY0 + static_cast<float>(shown) * lineH;
      b.text(bodyX, y, 1.6F, Color{0.95F, 0.55F, 0.45F, 1.0F}, "...ENDS HERE - NO SCROLL");
    }
  }

  // --- Hint bar --------------------------------------------------------------
  {
    // The jump range is what the DECODER can do, not how many pages exist. Only
    // choose1..choose5 are read, so advertising "[1-11]" and then silently
    // ignoring 6..11 is a lie on the one screen whose whole job is telling the
    // player what the keys do. Clamped to the five that actually work.
    constexpr std::size_t kJumpPages = 5;
    const std::size_t jumpable = std::min(total, kJumpPages);
    const std::string hint =
        "[F1] CLOSE   [UP/DOWN] PAGE   [1-" + std::to_string(jumpable) + "] JUMP";
    b.text(px * 0.5F - b.textWidth(1.5F, hint) * 0.5F, py - 30.0F, 1.5F,
           Color{0.5F, 0.5F, 0.6F, 1.0F}, hint);
  }
}

// The armed weapon's focus cards and exclusive uniques, for the page that
// lists them. Pulled out so the weapon tests can assert on the exact same set
// the game shows.
std::vector<std::string> Game::collectWeaponCards(std::string_view weaponId) const {
  std::vector<std::string> out;
  for (const auto& u : content_.upgrades) {
    if (u.weapon == weaponId) {
      out.push_back(u.name);
    }
  }
  return out;
}

bool Game::WeaponSnapshot::operator==(const WeaponSnapshot& other) const {
  // Field-by-field rather than memcmp: the struct is a flat bag of scalars with
  // padding, so memcmp would read the padding bytes and report spurious
  // differences. Every field a card can move is listed; a new WeaponSlot field
  // must be added to the snapshot above as well, or it will slip through the
  // "did this card do anything" test unnoticed.
  return def == other.def && attackType == other.attackType && cooldown == other.cooldown &&
         damage == other.damage && projectiles == other.projectiles &&
         speed == other.speed && life == other.life && pierce == other.pierce &&
         spread == other.spread && coneAngle == other.coneAngle &&
         coneRange == other.coneRange && coneTickRate == other.coneTickRate &&
         coneBite == other.coneBite && coneBiteMax == other.coneBiteMax &&
         coneEmberAt == other.coneEmberAt && coneEmberRadius == other.coneEmberRadius &&
         coneEmberDuration == other.coneEmberDuration &&
         orbitRadius == other.orbitRadius && orbitSpeed == other.orbitSpeed &&
         orbitCount == other.orbitCount && orbitWindow == other.orbitWindow &&
         waveSpread == other.waveSpread && bombArcHeight == other.bombArcHeight &&
         bombExplodeRadius == other.bombExplodeRadius &&
         bombKnockback == other.bombKnockback && bombFuse == other.bombFuse &&
         bombAhead == other.bombAhead && bombOnTarget == other.bombOnTarget &&
         reaimRange == other.reaimRange && reaimTurn == other.reaimTurn &&
         boomerangRange == other.boomerangRange &&
         boomerangReturnSpeed == other.boomerangReturnSpeed &&
         bounceCount == other.bounceCount && bounceRange == other.bounceRange &&
         bounceDamageMul == other.bounceDamageMul &&
         bounceInfinite == other.bounceInfinite && beamRange == other.beamRange &&
         beamWidth == other.beamWidth && beamDuration == other.beamDuration &&
         // chill was in the snapshot but not in the comparison, so the Rime
         // Lances' whole effect -- the only thing that card really does -- was
         // invisible to the "did this card do anything" test.
         chillMul == other.chillMul && chillTime == other.chillTime &&
         auraRadius == other.auraRadius && auraDps == other.auraDps &&
         auraTick == other.auraTick && auraChillMul == other.auraChillMul &&
         auraChillTime == other.auraChillTime &&
         haloKnockback == other.haloKnockback && haloInner == other.haloInner &&
         sweepAngle == other.sweepAngle &&
         sweepRadius == other.sweepRadius && sweepKnockback == other.sweepKnockback &&
         zoneRadius == other.zoneRadius && zoneDuration == other.zoneDuration &&
         zoneDps == other.zoneDps && zoneMaxPools == other.zoneMaxPools &&
         zoneFromAbove == other.zoneFromAbove &&
         chainJumpRange == other.chainJumpRange && chainMaxJumps == other.chainMaxJumps &&
         chainDamageMul == other.chainDamageMul &&
         chainShatter == other.chainShatter && bounceSplits == other.bounceSplits &&
         waveSpeed == other.waveSpeed && waveRange == other.waveRange &&
         waveWidth == other.waveWidth && waveKnockback == other.waveKnockback &&
         waveDamageMul == other.waveDamageMul && waveCount == other.waveCount &&
         waveArcStep == other.waveArcStep && waveHookPull == other.waveHookPull &&
         novaMaxRadius == other.novaMaxRadius &&
         novaExpandSpeed == other.novaExpandSpeed &&
         novaDamagePerTick == other.novaDamagePerTick &&
         novaTickRate == other.novaTickRate && novaContract == other.novaContract &&
         novaPull == other.novaPull && novaBurstDamage == other.novaBurstDamage &&
         novaEcho == other.novaEcho && area == other.area &&
         strength == other.strength && homing == other.homing && bounces == other.bounces &&
         cdBonus == other.cdBonus && sweepLead == other.sweepLead &&
         sweepHook == other.sweepHook &&
         uniqueHeal == other.uniqueHeal && beamSplit == other.beamSplit &&
         vortexRadius == other.vortexRadius && vortexReach == other.vortexReach &&
         vortexPull == other.vortexPull && vortexOrbit == other.vortexOrbit &&
         vortexOrbitSpeed == other.vortexOrbitSpeed &&
         vortexTickRate == other.vortexTickRate &&
         vortexCollapseAt == other.vortexCollapseAt &&
         vortexBurstDamage == other.vortexBurstDamage &&
         vortexBurstRadius == other.vortexBurstRadius &&
         prismRange == other.prismRange &&
         prismWidth == other.prismWidth && prismMaxTargets == other.prismMaxTargets &&
         prismRicochet == other.prismRicochet && lureRadius == other.lureRadius &&
         lureReach == other.lureReach && lurePull == other.lurePull &&
         lureDps == other.lureDps && lureDuration == other.lureDuration &&
         lureTickRate == other.lureTickRate && lureMaxBeacons == other.lureMaxBeacons;
}

Game::WeaponSnapshot Game::testWeaponSnapshot(int slotIndex) const {
  WeaponSnapshot s{};
  if (slotIndex < 0 || slotIndex >= weaponCount_) return s;
  const auto& w = weapons_[static_cast<std::size_t>(slotIndex)];
  s.def = w.def;
  s.attackType = static_cast<int>(w.attackType);
  s.cooldown = w.cooldown;
  s.damage = w.damage;
  s.projectiles = w.projectiles;
  s.speed = w.speed;
  s.life = w.life;
  s.pierce = w.pierce;
  s.spread = w.spread;
  s.coneAngle = w.coneAngle;
  s.coneRange = w.coneRange;
  s.coneTickRate = w.coneTickRate;
  s.coneBite = w.coneBite;
  s.coneBiteMax = w.coneBiteMax;
  s.coneEmberAt = w.coneEmberAt;
  s.coneEmberRadius = w.coneEmberRadius;
  s.coneEmberDuration = w.coneEmberDuration;
  s.orbitRadius = w.orbitRadius;
  s.orbitSpeed = w.orbitSpeed;
  s.orbitCount = w.orbitCount;
  s.orbitWindow = w.orbitWindow;
  s.waveSpread = w.waveSpread;
  s.bombArcHeight = w.bombArcHeight;
  s.bombExplodeRadius = w.bombExplodeRadius;
  s.bombKnockback = w.bombKnockback;
  s.bombFuse = w.bombFuse;
  s.bombAhead = w.bombAhead;
  s.bombOnTarget = w.bombOnTarget;
  s.reaimRange = w.reaimRange;
  s.reaimTurn = w.reaimTurn;
  s.boomerangRange = w.boomerangRange;
  s.boomerangReturnSpeed = w.boomerangReturnSpeed;
  s.bounceCount = w.bounceCount;
  s.bounceRange = w.bounceRange;
  s.bounceDamageMul = w.bounceDamageMul;
  s.bounceInfinite = w.bounceInfinite;
  s.beamRange = w.beamRange;
  s.beamWidth = w.beamWidth;
  s.beamDuration = w.beamDuration;
  s.chillMul = w.chillMul;
  s.chillTime = w.chillTime;
  s.auraRadius = w.auraRadius;
  s.auraDps = w.auraDps;
  s.auraTick = w.auraTick;
  s.auraChillMul = w.auraChillMul;
  s.auraChillTime = w.auraChillTime;
  s.haloKnockback = w.haloKnockback;
  s.haloInner = w.haloInner;
  s.sweepAngle = w.sweepAngle;
  s.sweepRadius = w.sweepRadius;
  s.sweepKnockback = w.sweepKnockback;
  s.zoneRadius = w.zoneRadius;
  s.zoneDuration = w.zoneDuration;
  s.zoneDps = w.zoneDps;
  s.zoneMaxPools = w.zoneMaxPools;
  s.zoneFromAbove = w.zoneFromAbove;
  s.chainJumpRange = w.chainJumpRange;
  s.chainMaxJumps = w.chainMaxJumps;
  s.chainDamageMul = w.chainDamageMul;
  s.chainShatter = w.chainShatter;
  s.bounceSplits = w.bounceSplits;
  s.waveSpeed = w.waveSpeed;
  s.waveRange = w.waveRange;
  s.waveWidth = w.waveWidth;
  s.waveKnockback = w.waveKnockback;
  s.waveDamageMul = w.waveDamageMul;
  s.waveCount = w.waveCount;
  s.waveArcStep = w.waveArcStep;
  s.waveHookPull = w.waveHookPull;
  s.novaMaxRadius = w.novaMaxRadius;
  s.novaExpandSpeed = w.novaExpandSpeed;
  s.novaDamagePerTick = w.novaDamagePerTick;
  s.novaTickRate = w.novaTickRate;
  s.novaContract = w.novaContract;
  s.novaPull = w.novaPull;
  s.novaBurstDamage = w.novaBurstDamage;
  s.novaEcho = w.novaEcho;
  s.area = w.area;
  s.strength = w.strength;
  s.homing = w.homing;
  s.bounces = w.bounces;
  s.cdBonus = w.cdBonus;
  s.sweepLead = w.sweepLead;
  s.sweepHook = w.sweepHook;
  s.uniqueHeal = w.uniqueHeal;
  s.beamSplit = w.beamSplit;
  s.vortexRadius = w.vortexRadius;
  s.vortexReach = w.vortexReach;
  s.vortexPull = w.vortexPull;
  s.vortexOrbit = w.vortexOrbit;
  s.vortexOrbitSpeed = w.vortexOrbitSpeed;
  s.vortexTickRate = w.vortexTickRate;
  s.vortexCollapseAt = w.vortexCollapseAt;
  s.vortexBurstDamage = w.vortexBurstDamage;
  s.vortexBurstRadius = w.vortexBurstRadius;
  s.prismRange = w.prismRange;
  s.prismWidth = w.prismWidth;
  s.prismMaxTargets = w.prismMaxTargets;
  s.prismRicochet = w.prismRicochet;
  s.lureRadius = w.lureRadius;
  s.lureReach = w.lureReach;
  s.lurePull = w.lurePull;
  s.lureDps = w.lureDps;
  s.lureDuration = w.lureDuration;
  s.lureTickRate = w.lureTickRate;
  s.lureMaxBeacons = w.lureMaxBeacons;
  return s;
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
  const float rowY0 = py * 0.54F;
  const float rowH = 38.0F;
  const float rowX = px * 0.5F - 170.0F;
  const float rowW = 340.0F;

  struct Row {
    const char* label;
  };
  const Row rows[] = {
      {"START RUN"},
      {"SKIN"},
      {"OUTLINE"},
      {"MANUAL"},
      {"RESET PROGRESS"},
      {"QUIT"},
  };
  static_assert(std::size(rows) == static_cast<std::size_t>(kMenuRows),
                "the row table and the kMenu* indices must stay in step");

  const auto& skins = skinPalette();
  const auto& outlines = outlinePalette();
  const Color danger{1.0F, 0.42F, 0.35F, 1.0F};
  for (int i = 0; i < kMenuRows; ++i) {
    const float y = rowY0 + static_cast<float>(i) * rowH;
    const bool sel = (i == menuSelection_);
    const bool armedReset = resetArmed_ && (i == kMenuReset);
    // Selection bar: a filled rounded-ish block behind the active row.
    if (sel) {
      b.rectTopLeft(rowX - 18.0F, y - 8.0F, rowW + 36.0F, 34.0F,
                    armedReset ? Color{danger.r, danger.g, danger.b, 0.18F}
                               : Color{gold.r, gold.g, gold.b, 0.14F});
      // Caret marker.
      b.circle(rowX - 28.0F, y + 4.0F, 5.0F, armedReset ? danger : gold);
    }
    const Color labelCol = armedReset ? danger : (sel ? gold : dim);
    // The label itself becomes the question once armed, so the destructive
    // state is readable without a second line of small print.
    b.text(rowX, y, 2.2F, labelCol,
           armedReset ? "RESET? PRESS AGAIN" : rows[static_cast<std::size_t>(i)].label);

    // Value on the right of the row, with the left/right hint.
    std::string value;
    if (i == kMenuSkin && profile_ != nullptr) {
      const int skin = std::clamp(profile_->skin, 0, static_cast<int>(skins.size()) - 1);
      value = std::string("< ") + skins[static_cast<std::size_t>(skin)].name + " >";
    } else if (i == kMenuOutline) {
      if (profile_ == nullptr) {
        value = "< unavailable >";
      } else if (profile_->canUseOutline(profile_->outline) &&
                 profile_->outline < static_cast<int>(outlines.size())) {
        value = std::string("< ") + outlines[static_cast<std::size_t>(profile_->outline)].name +
                " >";
      } else {
        value = "< locked >";
      }
    } else if (i == kMenuManual) {
      const std::size_t pages = content_.manual.size();
      value = pages == 0 ? "< no manual in build >"
                         : "< " + std::to_string(pages) + " pages >  OR [F1] >";
    } else if (i == kMenuReset) {
      if (profile_ == nullptr) {
        value = "< no profile >";
      } else if (armedReset) {
        // Deliberately EMPTY while armed. "RESET? PRESS AGAIN" already ends at
        // 707px and "< ENTER = WIPE ALL >" started at 618px, so the two
        // overwrote each other across 90px on the one row that deletes saved
        // progress. The label is the instruction now; the value would only
        // repeat it on top of itself.
      } else {
        value = "< 2-STEP >";
      }
    }
    if (!value.empty()) {
      b.text(rowX + rowW - b.textWidth(1.6F, value), y + 2.0F, 1.6F,
             armedReset ? danger : (sel ? white : dim), value);
    }
  }

  // A one-line warning under the rows while the wipe is armed. Cheap insurance:
  // this button is the only thing in the game that deletes earned progress.
  if (resetArmed_) {
    const std::string warn =
        "THIS WIPES YOUR SKIN AND EVERY UNLOCKED OUTLINE. IT CANNOT BE UNDONE.";
    b.text(px * 0.5F - b.textWidth(1.4F, warn) * 0.5F,
           rowY0 + static_cast<float>(kMenuRows) * rowH + 4.0F, 1.4F, danger, warn);
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
      // Anchored to the OUTLINE row, not to a hardcoded index. It said
      // rowY0 + 4*rowH, which is RESET PROGRESS -- two rows below the thing the
      // hint is about -- so the requirement for the outline you cannot use yet
      // read as if it belonged to the button that wipes your unlocks. The 18px
      // drop clears the next row's 38px pitch: the line ends at +33 and the row
      // below starts at +38.
      b.text(px * 0.5F - b.textWidth(1.5F, hint) * 0.5F,
             rowY0 + static_cast<float>(kMenuOutline) * rowH + 18.0F, 1.5F,
             Color{0.85F, 0.55F, 0.35F, 1.0F}, hint);
    }
  }

  // Footer: controls + version-ish tagline.
  const std::string footer = "[W/S or UP/DOWN] SELECT   [A/D or LEFT/RIGHT] CHANGE   [ENTER] CONFIRM";
  b.text(px * 0.5F - b.textWidth(1.4F, footer) * 0.5F, py - 40.0F, 1.4F,
         Color{0.5F, 0.5F, 0.6F, 1.0F}, footer);
  const std::string manualHint = "[F1] OPEN THE MANUAL AT ANY TIME";
  b.text(px * 0.5F - b.textWidth(1.2F, manualHint) * 0.5F, py - 22.0F, 1.2F,
         Color{0.42F, 0.45F, 0.55F, 1.0F}, manualHint);
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
    // ...except the manual, which advance() treats as the topmost modal overlay
    // and which therefore DOES take input while the menu is open. It used to be
    // skipped here, so the MANUAL row and F1 both set manualOpen_, advance()
    // routed every keystroke to updateManual(), and the screen never changed:
    // the keys were live and the manual was invisible. A modal that eats your
    // input without drawing is the worst of both, and from the menu there was no
    // way back either — the menu's own handler was no longer being called.
    if (manualOpen_) {
      renderManual(b, static_cast<float>(b.fbWidth()), static_cast<float>(b.fbHeight()));
    }
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

  // Chests. A plain dot would be mistaken for a fat XP orb, so the box is drawn
  // as one: a dark square lid, a lighter body, and a ring of the tier's colour
  // whose TICKS equal the number of weapons it will improve. A champion's box is
  // literally bigger than an elite's, so the size of the reward is readable from
  // across the screen before the player has to reach it.
  {
    auto view = registry_.view<Transform, Sprite, Chest, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& s = view.get<Sprite>(e);
      const auto& c = view.get<Chest>(e);
      const auto& r = view.get<Radius>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      const float bob = 0.05F * std::sin(simTime_ * 4.0F);
      // Halo on the ground, so a box on the far side of a pack still reads as
      // something to walk toward.
      Color glow = s.color;
      glow.a = 0.14F + 0.06F * std::sin(simTime_ * 4.0F);
      b.circle(x, y, r.r * 2.4F, glow);
      const float w = r.r * 2.0F;
      Color lid{0.14F, 0.13F, 0.20F, 1.0F};
      b.rect(x - w * 0.5F, y - w * 0.9F + bob, w, w * 0.45F, lid);
      b.rect(x - w * 0.42F, y - w * 0.42F + bob, w * 0.84F, w * 0.84F, s.color);
      Color band{0.16F, 0.15F, 0.22F, 1.0F};
      b.rect(x - w * 0.10F, y - w * 0.42F + bob, w * 0.20F, w * 0.84F, band);
      // One tick per weapon this box will improve.
      const int ticks = std::clamp(c.grants, 1, 5);
      for (int k = 0; k < ticks; ++k) {
        const float a = (static_cast<float>(k) / static_cast<float>(ticks)) * 2.0F * kPi +
                        simTime_ * 0.9F;
        Color tick = s.color;
        tick.a = 0.9F;
        b.circle(x + std::cos(a) * r.r * 1.5F, y + std::sin(a) * r.r * 1.5F, 0.055F,
                 tick);
      }
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
      // A chilled enemy is visibly frosted. The tint is the only feedback the
      // player gets that a status is on the field -- without it, "slowed" is an
      // invisible multiplier and the ice weapons read as plain damage.
      const auto* en = registry_.try_get<Enemy>(e);
      const bool chilled = en != nullptr && en->slowT > 0.0F;
      Color body = s.color;
      if (chilled) {
        // Lerp toward ice blue by how hard the chill is, so a 30% chill and a
        // deep freeze are distinguishable at a glance and the effect visibly
        // decays instead of snapping off.
        const float depth =
            std::clamp((1.0F - en->slowMul) / 0.65F, 0.0F, 1.0F) *
            std::clamp(en->slowT * 3.0F, 0.0F, 1.0F);
        constexpr Color kIce{0.62F, 0.90F, 1.0F, 1.0F};
        body.r += (kIce.r - body.r) * depth;
        body.g += (kIce.g - body.g) * depth;
        body.b += (kIce.b - body.b) * depth;
      }
      // Burning reads as heat, the same way the chill reads as cold, and for the
      // same reason: a damage-over-time the player cannot see is just an invisible
      // multiplier. The pulse is tied to the burn's own remaining time rather than
      // to the clock, so a freshly-lit body flares and a nearly-out one gutters.
      const bool burning = en != nullptr && en->burnT > 0.0F;
      if (burning) {
        const float heat = std::clamp(en->burnT / 2.0F, 0.0F, 1.0F);
        const float flick = 0.82F + 0.18F * std::sin(simTime_ * 17.0F + static_cast<float>(e) * 0.7F);
        constexpr Color kEmber{1.0F, 0.42F, 0.12F, 1.0F};
        const float k = heat * flick;
        body.r += (kEmber.r - body.r) * k;
        body.g += (kEmber.g - body.g) * k;
        body.b += (kEmber.b - body.b) * k;
      }
      if (s.circle) b.circle(x, y, r.r, body);
      else b.rect(x, y, r.r * 2.0F, r.r * 2.0F, body);
      // A hexed body is outlined, because the hex is a stack: the player needs to
      // be able to see it build without watching the damage numbers. The outline
      // is a fraction of the elite ring so the two never read as the same thing.
      if (const auto* tr = registry_.try_get<EnemyTraits>(e);
          tr != nullptr && tr->vuln > 0.0F) {
        const float depth = std::clamp(tr->vuln, 0.0F, 1.0F);
        constexpr Color kHex{0.85F, 0.40F, 1.0F, 1.0F};
        Color ring = kHex;
        ring.a = 0.30F + 0.45F * depth;
        constexpr int kHexDots = 12;
        for (int k = 0; k < kHexDots; ++k) {
          const float a =
              (static_cast<float>(k) / static_cast<float>(kHexDots)) * 2.0F * kPi +
              simTime_ * 1.4F;
          b.circle(x + std::cos(a) * r.r * 1.09F, y + std::sin(a) * r.r * 1.09F, 0.06F,
                   ring);
        }
      }
      // A hard freeze gets a ring, borrowing the elite outline's language so
      // "this one is pinned" reads at the same glance as "this one is tough".
      if (chilled && en->slowMul <= 0.55F) {
        constexpr Color kFrostRing{0.70F, 0.94F, 1.0F, 0.55F};
        constexpr float kDot = 0.07F;
        constexpr int kRingDots = 20;
        for (int k = 0; k < kRingDots; ++k) {
          const float a = (static_cast<float>(k) / static_cast<float>(kRingDots)) * 2.0F * kPi;
          b.circle(x + std::cos(a) * r.r * 1.14F, y + std::sin(a) * r.r * 1.14F, kDot,
                   kFrostRing);
        }
      }

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
      // The carried aura, drawn. Without this the weapon is invisible: the shots
      // are the same specks the Frost Shards threw, and the one thing the player
      // has to be able to see is the bubble that is actually doing the slowing.
      const auto& pr = view.get<Projectile>(e);
      if (pr.auraRadius > 0.0F) {
        Color wash = s.color;
        wash.a = 0.09F;
        b.circle(x, y, pr.auraRadius, wash);
        Color edge = s.color;
        edge.a = 0.20F;
        b.circle(x, y, pr.auraRadius * 0.72F, edge);
      }
      if (s.circle) {
        if (pr.reaimRange > 0.0F) {
          // A short comet tail, and only on the bolt that bends. The whole point
          // of the weapon is that its heading CHANGES, and a bare circle moving in
          // a straight line is drawn identically before a bend, during one and
          // after one -- so the mechanic is real and invisible. The tail is the
          // course it just flew, which is exactly the thing a bend changes, and
          // it costs one interpolated segment because the previous position is
          // already on the transform.
          const float sx = t.px + (t.x - t.px) * lerp;
          const float sy = t.py + (t.y - t.py) * lerp;
          for (int k = 1; k <= 4; ++k) {
            const float f = 1.0F - static_cast<float>(k) / 5.0F;
            Color c = s.color;
            c.a *= 0.30F * f;
            b.circle(sx + (x - sx) * f, sy + (y - sy) * f, r.r * 0.72F * f, c);
          }
        }
        b.circle(x, y, r.r, s.color);
      } else {
        b.rect(x, y, r.r * 2.0F, r.r * 2.0F, s.color);
      }
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
      // The drawn spoke is a SEGMENT, not a ray. A halo with a dead zone has to
      // show that hole or the player has no way to tell that the safe ring is
      // safe -- it would read as a plain halo and the whole point would be
      // invisible. Clamped exactly as the damage test clamps it, so what is drawn
      // is what is hit.
      const float near = std::min(hb.inner, len * 0.9F);
      const float far = std::max(near + 0.05F, len);
      const float sx0 = t.x + std::cos(hb.angle) * near;
      const float sy0 = t.y + std::sin(hb.angle) * near;
      const float ex = t.x + std::cos(hb.angle) * far;
      const float ey = t.y + std::sin(hb.angle) * far;
      Color glow = hb.color;
      glow.a = 0.16F;
      Color core = hb.color;
      core.a = 0.95F;
      const int steps = std::max(
          1, static_cast<int>((far - near) / std::max(0.03F, hb.width * 0.15F)));
      for (int s = 0; s <= steps; ++s) {
        const float fr = static_cast<float>(s) / static_cast<float>(steps);
        const float sx = sx0 + (ex - sx0) * fr;
        const float sy = sy0 + (ey - sy0) * fr;
        b.circle(sx, sy, hb.width * 0.52F, glow);
        b.circle(sx, sy, hb.width * 0.30F, core);
      }
      // Bright hub at the inner end of the spoke, wherever that turns out to be.
      b.circle(sx0, sy0, hb.width * 0.7F, core);
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
      Color rim = vx.color;
      rim.a = 0.45F;
      constexpr int kRim = 18;
      for (int k = 0; k < kRim; ++k) {
        const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(kRim);
        b.circle(x + std::cos(a) * vx.radius, y + std::sin(a) * vx.radius, 0.06F, rim);
      }
      if (vx.collapseAt > 0.0F) {
        // A well with an END looks completely different from one without, and the
        // difference is the whole point of the weapon: the Void Gyre is a soft
        // patient disc you can stand next to, and the Event Horizon is a hard
        // bright ring that is visibly winding shut. The player can see the timer
        // on the weapon instead of having to remember it.
        const float chargeF =
            vx.collapseAt > 0.0F ? std::clamp(vx.charge / vx.collapseAt, 0.0F, 1.0F) : 0.0F;
        // The rim goes hard and bright and closes in on the centre as it charges.
        const float cr = vx.radius * (1.0F - 0.80F * chargeF);
        Color close = vx.color;
        close.a = 0.35F + 0.55F * chargeF;
        b.circle(x, y, std::max(0.05F, cr), close);
        // Radial tics that crowd inward with the charge, so the closing is a
        // direction of travel rather than just a shrinking outline.
        constexpr int kTics = 12;
        for (int k = 0; k < kTics; ++k) {
          const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(kTics) +
                          chargeF * 2.2F;
          const float rr = vx.radius * (1.0F - 0.72F * chargeF);
          Color tic = vx.color;
          tic.a = 0.30F + 0.60F * chargeF;
          b.circle(x + std::cos(a) * rr, y + std::sin(a) * rr,
                   0.05F + 0.03F * chargeF, tic);
        }
        // The eye only opens once the charge is nearly done, which is the tell
        // that the burst is a moment away.
        Color eye = vx.color;
        eye.a = 0.25F + 0.75F * chargeF * chargeF;
        b.circle(x, y, 0.05F + 0.13F * chargeF * chargeF, eye);
      } else {
        // The Gyre: a soft bright centre it never loses, because this well never
        // ends.
        Color eye = vx.color;
        eye.a = 0.85F;
        b.circle(x, y, 0.13F, eye);
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
      if (bp.trailWidth > 0.0F && bp.trailCount > 1) {
        // Drawn along the same remembered path the damage uses, so what the player
        // sees IS what is being hit. Drawing only the last frame's sliver (which is
        // what this did) is part of why the trail read as decoration.
        Color glow = bp.color;
        glow.a = 0.09F;
        Color trailCore = bp.color;
        trailCore.a = 0.42F;
        const int n = bp.trailCount;
        for (int s = 0; s < n; ++s) {
          const int i = (bp.trailHead - n + s + BoomerangProjectile::kTrailSamples * 2) %
                        BoomerangProjectile::kTrailSamples;
          // Older samples fade, so the trail has a direction: brightest at the
          // blade, dying out behind it.
          const float age = static_cast<float>(n - s) / static_cast<float>(n);
          const float lx = bp.trailPathX[i];
          const float ly = bp.trailPathY[i];
          Color g = glow;
          Color c = trailCore;
          g.a *= age;
          c.a *= age;
          b.circle(lx, ly, bp.trailWidth * 0.62F, g);
          b.circle(lx, ly, bp.trailWidth * 0.34F, c);
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
      // A pool whose fire ARRIVED is drawn as a shaft standing on it: the Ember
      // Sprayer burns the ground where the cone went, while Ashfall's shells come
      // down on the Grave Bell. Same entity, same numbers, and the two read as
      // one weapon unless something says which way the fire arrived. The impact
      // ring at the top is the tell -- it is the shell, still up there.
      if (ze.fromAbove) {
        const float shaftH = ze.radius * 2.4F;
        constexpr int kShaft = 9;
        for (int k = 0; k < kShaft; ++k) {
          const float f = static_cast<float>(k) / static_cast<float>(kShaft - 1);
          // Narrowing as it rises, so it reads as a column and not as a pillar.
          const float rr = ze.radius * (1.0F - f * 0.62F) * pulse;
          const float yy = t.y + f * shaftH;
          Color col = ze.color;
          // Densest at the base, where the fire actually is.
          col.a = (0.20F - 0.10F * f) * fade;
          b.circle(t.x, yy, rr, col);
        }
        // The impact: a bright ring up at the top of the shaft, contracting as
        // the pool ages, which is the shell that is still falling.
        const float fall = 1.0F - fade;
        const float ir = ze.radius * (0.30F + 0.55F * fall) * pulse;
        Color hot = ze.color;
        hot.a = 0.70F * fade * (0.45F + 0.55F * fall);
        b.circle(t.x, t.y + shaftH, ir * 0.42F, hot);
        constexpr int kDrop = 14;
        for (int k = 0; k < kDrop; ++k) {
          const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(kDrop);
          b.circle(t.x + std::cos(a) * ir, t.y + shaftH + std::sin(a) * ir, 0.07F, hot);
        }
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

  // Chain lightning: a bolt that strikes down from the sky onto the first
  // target, then arcs from enemy to enemy. Drawn as a run of small circles with
  // a deterministic perpendicular jitter, because a straight line between two
  // enemies reads as a laser sight and a jagged one reads as lightning.
  //
  // The strike is in three beats, and the order is the whole point of it:
  //   1. telegraph -- a transparent ring converges on the target. Nothing has
  //      damaged yet. This is the only warning, and without it the first thing
  //      the player saw was the enemy's health bar already moving.
  //   2. strike    -- the drop draws itself down out of the sky over
  //      kChainStrike, so the bolt arrives instead of appearing.
  //   3. arc + hang -- enemy to enemy, then a linger (kChainLinger) in which the
  //      bolt stays where it ended and fades. A bolt into a scattered crowd used
  //      to end the instant it hit the last body; dead-ending bypassed the linger
  //      altogether, which is the "it vanishes too early" the linger was meant to
  //      prevent in the first place.
  {
    auto view = registry_.view<Transform, ChainLightning>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& cl = view.get<ChainLightning>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;

      // --- Beat 1: the charge gathers in the SKY above the target. -----------
      //
      // It used to converge on the target's own centre, as three shrinking rings
      // and a crosshair pinned to the ground under them. That was the first thing
      // the player ever saw of a chain weapon and it read as a floor decal stuck
      // to the victim's feet -- the one thing a lightning strike must not look
      // like, and far worse in a crowd, where a dozen bodies wore a dozen decals.
      //
      // So the convergence happens where the bolt is going to come FROM: up at the
      // top of the drop. The charge is still unmistakably aimed -- it tightens, it
      // brightens, its bead creeps down the drop as the moment arrives -- but it
      // hangs in the air, and the enemy itself gets nothing until the bolt lands.
      if (cl.telegraph > 0.0F) {
        // 1 at the start of the window, 0 at the moment of the strike. Squared,
        // so the charge spends most of its time loose and then tightens the last
        // stretch quickly -- a linear shrink reads as slow and even, which is the
        // least alarming thing a telegraph can look like.
        const float open = std::clamp(cl.telegraph / kChainTelegraph, 0.0F, 1.0F);
        const float f = 1.0F - open * open;
        const float chargeY = y + kChainSkyDrop;
        // Three rings closing at different rates reads as depth; one reads as a
        // circle being scaled. Drawn UP HERE around the charge, not on the floor
        // around the victim.
        for (int k = 0; k < 3; ++k) {
          const float lag = 1.0F - static_cast<float>(k) * 0.16F;
          const float rf = std::max(0.0F, (f - static_cast<float>(k) * 0.12F) * lag);
          Color c = cl.color;
          c.a = (0.08F + 0.30F * f) * (1.0F - static_cast<float>(k) * 0.28F);
          b.circle(x, chargeY, 0.22F + 0.95F * (1.0F - rf), c);
        }
        // The gathering charge: a bright bead that creeps down the drop as the
        // moment arrives, so the eye follows something that is ABOUT to fall on
        // that specific body rather than something sitting on the floor.
        const float beadY = chargeY - (chargeY - y) * 0.22F * f;
        Color glow = cl.color;
        glow.a = 0.30F * (0.35F + 0.65F * f);
        Color core = cl.color;
        core.a = 0.45F + 0.55F * f;
        b.circle(x, beadY, 0.20F + 0.10F * f, glow);
        b.circle(x, beadY, 0.09F + 0.05F * f, core);
        continue;
      }

      // Fades over the linger, and a bolt that has run out of jumps is dimmer
      // than one that is still travelling. The fade is quadratic so it drops off
      // gently and then goes quickly -- linear made the last third of a bolt look
      // like it was being switched off.
      const float left = std::clamp(cl.linger / kChainLinger, 0.0F, 1.0F);
      const float alive =
          cl.jumpsDone >= cl.maxJumps ? (left * left) * 0.65F : 1.0F;
      // The drop grows into existence over kChainStrike, so it arrives rather
      // than appearing. A bolt that is still travelling keeps the full width.
      const float born = cl.jumpsDone == 0 ? std::clamp(cl.strike, 0.0F, 1.0F) : 1.0F;
      // A bolt that has not moved yet is still a bolt that came from the sky:
      // draw the vertical drop above the first target.
      if (cl.jumpsDone == 0) {
        const float topY = y - kChainSkyDrop;
        // The drop reaches further down as it lands, so it reads as falling
        // rather than as a fixed decoration that switched on.
        const float reach = topY + (y - topY) * born;
        const int steps = 9;
        for (int k = 0; k <= steps; ++k) {
          const float f = static_cast<float>(k) / static_cast<float>(steps);
          const float jx = boltJitter(static_cast<unsigned>(e) + static_cast<unsigned>(k)) *
                            0.16F * (1.0F - f);
          const float bxx = x + jx;
          const float byy = topY + (reach - topY) * f;
          Color glow = cl.color;
          glow.a = 0.30F * alive;
          Color core = cl.color;
          core.a = alive;
          b.circle(bxx, byy, 0.17F, glow);
          b.circle(bxx, byy, 0.07F, core);
        }
        // Impact: a short radial star at the point of contact, blooming only once
        // the drop has actually reached it. It used to be two concentric rings
        // centred on the enemy -- the same floor decal as the telegraph, and for
        // the same reason it read as a marker rather than as a hit.
        Color flash = cl.color;
        flash.a = 0.30F * alive * born;
        b.circle(x, y, 0.30F, flash);
        for (int arm = 0; arm < 6; ++arm) {
          const float a = static_cast<float>(arm) * (kPi / 3.0F);
          const float r0 = 0.22F;
          const float r1 = 0.22F + 0.40F * born;
          b.circle(x + std::cos(a) * r0, y + std::sin(a) * r0, 0.05F, flash);
          b.circle(x + std::cos(a) * r1, y + std::sin(a) * r1, 0.05F, flash);
        }
      } else {
        // Enemy-to-enemy arc. The bolt's previous tick position is where the
        // last hop ended, so the last hop's segment is drawn here. This is also
        // why updateChainLightning writes t.px/t.py: before that they stayed at
        // zero and the renderer drew every arc flying in from the world origin.
        const float ax = t.px;
        const float ay = t.py;
        const float dx = x - ax;
        const float dy = y - ay;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.05F) {
          const float ux = dx / len;
          const float uy = dy / len;
          // Perpendicular, so the wobble bows out of the line rather than
          // stretching it.
          const float px = -uy;
          const float py = ux;
          const int steps = 10;
          for (int k = 0; k <= steps; ++k) {
            const float f = static_cast<float>(k) / static_cast<float>(steps);
            // Zero at both ends, biggest mid-span: a bolt that leaves and
            // arrives at a point, not one that has kinks at the enemies.
            const float bow = std::sin(f * kPi) * 0.30F;
            const float j = boltJitter(static_cast<unsigned>(e) * 31U +
                                        static_cast<unsigned>(cl.jumpsDone) * 7U +
                                        static_cast<unsigned>(k)) * bow;
            const float bx = ax + dx * f + px * j;
            const float by = ay + dy * f + py * j;
            Color glow = cl.color;
            glow.a = 0.26F * alive;
            Color core = cl.color;
            core.a = 0.9F * alive;
            b.circle(bx, by, 0.16F, glow);
            b.circle(bx, by, 0.065F, core);
          }
        }
      }
      b.circle(x, y, 0.22F, Color{cl.color.r, cl.color.g, cl.color.b, 0.40F * alive});
      b.circle(x, y, 0.11F, Color{cl.color.r, cl.color.g, cl.color.b, alive});
    }
  }

  // Travelling waves (Sunder / Tidal Lash): a crescent, drawn as a real arc.
  //
  // This used to be a blob smeared BACKWARD from a bright point, which is
  // asymmetric about the direction of travel -- it looked like a comet, not like
  // a wave, and it was the single clearest thing saying "these two weapons are the
  // same". The shape now is sampled across the arc from one horn to the other,
  // each sample pushed back by a PARABOLA in the cross-axis coordinate, so the
  // nose leads at the wave's actual position and the two horns trail evenly
  // behind it. Mirror the picture and it is unchanged, which is what symmetry
  // means here. `spread` is what makes the Sundering Core's wide slow wall and the
  // Tidal Lash's narrow fast rake read as different objects rather than the same
  // one with a different speed.
  {
    auto view = registry_.view<Transform, WaveEffect, Radius>();
    for (const auto e : view) {
      const auto& t = view.get<Transform>(e);
      const auto& wv = view.get<WaveEffect>(e);
      const float x = t.px + (t.x - t.px) * lerp;
      const float y = t.py + (t.y - t.py) * lerp;
      const float fade = std::clamp(wv.life * 2.0F, 0.0F, 1.0F);
      // Depth of the arc is the same as the damage band's depth, so the drawn
      // crescent and the hit box are the same object.
      const float depth = wv.width;
      const float spread = std::clamp(wv.spread, 0.15F, kPi);
      // A crescent is a FILLED shape, not a stroke, so it is drawn as one: a
      // body of overlapping circles running down the middle of the lune, plus a
      // bright rim along its leading edge. The old version drew a single row of
      // 11 circles along the outer arc, and at that spacing the inside of the
      // curve came out scalloped and the horns came out as loose dots -- which is
      // exactly the "strange circles" complaint. The sample count below is set by
      // the worst case on the shape, which is the horns: the curve is moving
      // fastest and the body is thinnest there, so that is where the spacing has
      // to be tightest. kSamples is depth-independent for the same reason.
      constexpr int kSamples = 41;  // odd, so there is a sample exactly on the nose
      const auto bodyAt = [&](float u, float out, Color c, float scale) {
        // u = 0 is the nose; |u| = 1 are the horns. Parabolic in u, so the shape
        // is symmetric about u = 0 by construction rather than by hand. The lune
        // tapers toward both horns, which is what makes the tips read as points.
        const float taper = 1.0F - u * u * 0.72F;
        const float thickness = depth * 0.46F * taper;
        const float dist = depth * (1.0F - u * u) - thickness * 0.5F;
        const float a = wv.angle + u * spread;
        b.circle(x + std::cos(a) * dist, y + std::sin(a) * dist,
                 thickness * 0.5F * out * scale, c);
      };
      // The body. Two passes so the middle of the lune is solid rather than
      // translucent where the circles only just overlap.
      Color body = wv.color;
      body.a = 0.16F * fade;
      for (int pass = 0; pass < 2; ++pass) {
        for (int k = 0; k < kSamples; ++k) {
          const float u =
              static_cast<float>(k) / static_cast<float>(kSamples - 1) * 2.0F - 1.0F;
          bodyAt(u, 1.0F, body, 1.0F);
        }
        body.a += 0.10F * fade;
      }
      // The leading rim: the bright edge that shows which way the crescent is
      // travelling, sampled on the outer parabola rather than the mid-line.
      Color rim = wv.color;
      rim.a = 0.55F * fade;
      for (int k = 0; k < kSamples; ++k) {
        const float u =
            static_cast<float>(k) / static_cast<float>(kSamples - 1) * 2.0F - 1.0F;
        const float dist = depth * (1.0F - u * u);
        const float a = wv.angle + u * spread;
        b.circle(x + std::cos(a) * dist, y + std::sin(a) * dist,
                 depth * 0.055F * (1.0F - std::abs(u) * 0.45F), rim);
      }
      // A small hot point on the nose itself, so the direction of travel is
      // readable at a glance even while the crescent is faint.
      Color tip = wv.color;
      tip.a = 0.85F * fade;
      b.circle(x, y, depth * 0.13F, tip);
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

  // The manual hint, in the same left-hand key column as the heal indicator --
  // which is the one place on screen that is already "things you can press". F1 is
  // a real key and nobody guesses it, but the old version was 1.5x grey text in
  // the bottom-right corner, fighting the [Q] QUIT line for space on a narrow
  // window and vanishing against a bright background. It is a touch larger, full
  // contrast, and on a panel so it reads at any size -- still a small hint, not a
  // banner, because the manual being findable is not worth a permanent corner of
  // the screen.
  if (!manualOpen_) {
    const std::string hint = "[F1] MANUAL";
    const float hintScale = 1.7F;
    const float hintW = b.textWidth(hintScale, hint);
    b.rectTopLeft(8.0F, 84.0F, hintW + 14.0F, 22.0F, Color{0.06F, 0.07F, 0.10F, 0.72F});
    b.text(15.0F, 87.0F, hintScale, Color{0.78F, 0.82F, 0.92F, 1.0F}, hint);
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
    // The fade-in is measured from the banner's OWN lifetime, not from a 4.0
    // literal duplicated here. Both current sources happen to be <= 4.0s so
    // there is no live bug today, but the two copies were free to disagree and
    // a longer banner would have produced a negative alpha -- an invisible text
    // draw rather than a visible failure.
    const float a = std::min(1.0F, (kTierBannerLife - tierBannerT_) / 0.3F) *
                    std::min(1.0F, tierBannerT_ / 1.0F);
    Color fade = tierGlow;
    fade.a = a;
    b.text(px * 0.5F - b.textWidth(2.6F, tierBanner_) * 0.5F, py * 0.5F - 150.0F, 2.6F,
           fade, tierBanner_);
  }

  // Chest toast. A box spends itself the instant it is touched, so without this
  // the player sees their weapon quietly improve and has no way of telling which
  // one, or how many. The number is the whole reward, so it is what is written.
  if (lastChestTimer_ > 0.0F && lastChestGrants_ > 0 && lastChestCard_ >= 0) {
    const auto& card = content_.upgrades[static_cast<std::size_t>(lastChestCard_)];
    const std::string what = card.name;
    const std::string line =
        "CHEST: " + std::to_string(lastChestGrants_) +
        (lastChestGrants_ == 1 ? " WEAPON UPGRADED" : " WEAPONS UPGRADED") + " - " + what;
    const float a = std::min(1.0F, lastChestTimer_ / 0.6F);
    Color fade{1.0F, 0.85F, 0.35F, a};
    b.text(px * 0.5F - b.textWidth(2.2F, line) * 0.5F, py * 0.5F - 176.0F, 2.2F, fade,
           line);
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

    // The range is built from the real card count. Hardcoding "/4" meant a
    // 5-card row (or the moment extraChoice ever gains a second stack) said
    // "CHOOSE 1/2/3/4" while a fifth key was sitting there working.
    const auto chooseRange = [](std::size_t count) {
      std::string s = "CHOOSE 1";
      for (std::size_t i = 2; i <= count; ++i) s += "/" + std::to_string(i);
      return s;
    };
    // A milestone counts its own cards rather than claiming two. The screen used
    // to say "CHOOSE 1/2" on a four-card question, which is the one place a
    // player is told a lie while making the run's biggest decision.
    const std::string title =
        choosingStarter_ ? "CHOOSE A STARTING WEAPON"
        : milestoneOffer_ ? "MILESTONE! " + chooseRange(choices_.size())
        : "LEVEL UP! " + chooseRange(choices_.size());
    b.text(px * 0.5F - b.textWidth(4.0F, title) * 0.5F, py * 0.16F, 4.0F,
           (milestoneOffer_ && !choosingStarter_) ? violet : gold, title);

    const std::size_t n = choices_.size();
    // Fallbacks for the "no choices at all" case, which buildChoices makes
    // impossible (it always appends a Skip card when the pool is empty) and a
    // test now pins. They are here because the arithmetic below divides by n:
    // a renderer that divides by a size it has not checked will happily draw
    // cards at NaN positions if a future path ever reaches LevelUp with an
    // empty row.
    float cardH = 176.0F;
    float cardTop = py * 0.32F;
    float bodyTop = 86.0F;
    int bodyLines = 4;
    if (n > 0) {
      const float gap = 24.0F;
      const float avail = (px - gap * static_cast<float>(n - 1) - 40.0F) /
                          static_cast<float>(n);
      // Clamp the WIDTH rather than the count: a 5-card row must still fit the
      // screen, so a minimum card width can never push the row off the right.
      const float cardW = std::clamp(avail, 200.0F, 400.0F);
      const CardTextLayout lay = cardTextLayout(cardW);
      // The row is as tall as its TALLEST body, not a fixed height: a 3-card
      // row of one-liners should not be a metre of empty panel, and a 5-card
      // row of two-liners should not be a truncated wall of text. Every card in
      // the row gets the same height so the footers line up.
      const auto lineCount = [&](const Choice& c) {
        std::string_view desc = "Every upgrade you can use is already maxed.";
        if (c.kind == Choice::Kind::Upgrade) {
          desc = content_.upgrades[static_cast<std::size_t>(c.index)].desc;
        } else if (c.kind == Choice::Kind::Weapon) {
          desc = content_.weapons[static_cast<std::size_t>(c.index)].desc;
        }
        return wrapToWidth(desc, lay.width, lay.scale, lay.indent, lay.contScale).size();
      };
      // A card's title can need two lines in a narrow card, and when it does the
      // description has to start lower or the two blocks overlap. The row uses
      // one body offset for every card so the descriptions still line up, so
      // this is the TALLEST title in the row, not each card's own.
      const auto nameLines = [&](const Choice& c) -> std::size_t {
        if (c.kind == Choice::Kind::Upgrade) {
          return cardNameLayout(content_.upgrades[static_cast<std::size_t>(c.index)].name,
                                lay.width)
              .lines.size();
        }
        if (c.kind == Choice::Kind::Weapon) {
          return cardNameLayout(content_.weapons[static_cast<std::size_t>(c.index)].name,
                                lay.width)
              .lines.size();
        }
        return 1; // the Skip card's "NOTHING LEFT" always fits
      };
      std::size_t tallest = 1;
      std::size_t tallestName = 1;
      for (const auto& c : choices_) {
        tallest = std::max(tallest, lineCount(c));
        tallestName = std::max(tallestName, nameLines(c));
      }
      const LevelUpRow row = levelUpRowLayout(px, py, n, tallest, tallestName);
      cardH = row.cardH;
      bodyTop = row.bodyTop;
      cardTop = row.top;
      bodyLines = row.bodyLines;
      const float totalW = static_cast<float>(n) * cardW + static_cast<float>(n - 1) * gap;
      float x = px * 0.5F - totalW * 0.5F;
      for (std::size_t i = 0; i < n; ++i) {
        const auto& choice = choices_[i];
        const float y = cardTop;
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
          renderWrappedText(b, x + 16.0F, y + bodyTop, lay,
                            Color{0.85F, 0.85F, 0.9F, 1.0F},
                            "Every upgrade you can use is already maxed.", bodyLines);
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, accent, "PRESS 1 TO CONTINUE");
        } else if (choice.kind == Choice::Kind::Upgrade) {
          const auto& def = content_.upgrades[static_cast<std::size_t>(choice.index)];
          drawCardName(b, def.name, x + 16.0F, y + 52.0F, lay.width, accent, white);
          renderWrappedText(b, x + 16.0F, y + bodyTop, lay,
                            Color{0.85F, 0.85F, 0.9F, 1.0F}, def.desc, bodyLines);
          const std::string stacks =
              "STACKS " + std::to_string(upgradeStacks(static_cast<std::size_t>(choice.index))) +
              "/" + std::to_string(def.maxStacks);
          b.text(x + 16.0F, y + cardH - 30.0F, 1.5F, Color{0.6F, 0.6F, 0.7F, 1.0F}, stacks);
        } else {
          const auto& def = content_.weapons[static_cast<std::size_t>(choice.index)];
          drawCardName(b, def.name, x + 16.0F, y + 52.0F, lay.width, accent, white);
          renderWrappedText(b, x + 16.0F, y + bodyTop, lay,
                            Color{0.85F, 0.85F, 0.9F, 1.0F}, def.desc, bodyLines);
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
    // rerolls are still available this level. Anchored to the bottom of the
    // card row rather than a fixed screen fraction, so a tall 5-card row
    // cannot grow up and swallow it.
    const int rerollsLeft = 1 + stats_.rerollCharges - rerollsUsed_;
    const std::string hint = rerollsLeft > 0
                                 ? "[R] REROLL x" + std::to_string(rerollsLeft)
                                 : "[R] REROLL NONE LEFT";
    const float hintY = n > 0 ? cardTop + cardH + 24.0F : py * 0.66F;
    b.text(px * 0.5F - b.textWidth(2.0F, hint) * 0.5F, hintY, 2.0F,
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
    // The way out. Drawn on the pause screen and nowhere else, because that is
    // the only place it is legal from: a run you cannot leave is a run you can
    // only end by dying, and the player should not have to work that out.
    const std::string quit = quitArmed_ ? "[Q] PRESS AGAIN TO QUIT TO MENU"
                                        : "[Q] QUIT TO MENU";
    const Color quitCol = quitArmed_ ? Color{1.0F, 0.45F, 0.4F, 1.0F}
                                      : Color{0.6F, 0.62F, 0.7F, 1.0F};
    b.text(px * 0.5F - b.textWidth(2.0F, quit) * 0.5F, py - 40.0F, 2.0F, quitCol, quit);
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
    // The item picker is a full-height panel that used to be drawn AFTER this
    // header, so it covered the weapon name completely -- and the name is the
    // one thing the picker itself does not show, since [1]/[2] (the keys that
    // change weapon) are dead while the list is open. When the picker is up the
    // header moves above its top edge instead.
    const float headerDrop = testShopOpen_ ? -0.30F : 0.0F;
    b.text(px * 0.5F - b.textWidth(3.0F, title) * 0.5F, py * 0.70F + headerDrop, 3.0F, gold,
           title);
    const std::string boost = testBoosted_ ? "BOOST ON" : "BOOST OFF";
    const std::string waves = wavesEnabled_ ? "WAVES ON" : "WAVES OFF";
    const std::string god = testInvuln_ ? "GOD ON" : "GOD OFF";
    const std::string clock = "CLOCK X" + std::to_string(testTimeScale_);
    const std::string status = boost + "   " + waves + "   " + god + "   " + clock;
    b.text(px * 0.5F - b.textWidth(2.0F, status) * 0.5F, py * 0.74F + headerDrop, 2.0F,
           Color{0.75F, 0.85F, 1.0F, 1.0F}, status);
    // The keymap is filtered by what actually works right now. The 1-5 keys and
    // X all drive the world, and the world is only driven while Playing, so
    // advertising the full keymap over the pause sheet listed six keys that
    // silently did nothing.
    const Color key{0.8F, 0.8F, 0.85F, 1.0F};
    const Color deadKey{0.8F, 0.8F, 0.85F, 0.35F};
    const bool live = state_ == RunState::Playing;
    if (live) {
      const std::string rowA =
          "[1] PREV   [2] NEXT   [3] MAX BUILD   [4] WAVES   [5] CLOSE";
      b.text(px * 0.5F - b.textWidth(1.6F, rowA) * 0.5F, py * 0.775F + headerDrop, 1.6F, key,
             rowA);
    } else {
      const std::string rowA = "PAUSED - [ESC] RESUME TO USE [1]-[5] AND [X]";
      b.text(px * 0.5F - b.textWidth(1.6F, rowA) * 0.5F, py * 0.775F + headerDrop, 1.6F,
             deadKey, rowA);
    }
    // E, I, F and R stay live on every screen, so they are never greyed.
    const std::string rowB = live ? "[E] ITEMS   [I] GOD   [F] CLOCK   [X] KILL   [R] MAX ALL"
                                 : "[E] ITEMS   [I] GOD   [F] CLOCK   [R] MAX ALL";
    b.text(px * 0.5F - b.textWidth(1.6F, rowB) * 0.5F, py * 0.805F + headerDrop, 1.6F, key,
           rowB);
    const std::string noteA = "NO XP - NO SKINS - LEAVING THE SANDBOX ENDS THE RUN";
    b.text(px * 0.5F - b.textWidth(1.4F, noteA) * 0.5F, py * 0.84F + headerDrop, 1.4F,
           Color{0.95F, 0.55F, 0.45F, 1.0F}, noteA);
    if (testShopOpen_) renderTestShop(b, px, py);
  }

  // The manual is drawn LAST and opaque: it is the topmost overlay, so it covers
  // the HUD, the pause sheet, the bestiary and the sandbox alike.
  if (manualOpen_) renderManual(b, px, py);

  b.flush();
}

} // namespace game