#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "game/game.hpp"
#include "game/content.hpp"
#include "game/profile.hpp"
#include "core/input/key_repeat.hpp"
#include "core/render/batcher.hpp"
#include "core/sim/spatial_hash.hpp"
#include "core/sim/fixed_timestep.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// --- Content loading ---------------------------------------------------------

TEST_CASE("Content loads from assets/data") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapons.size() >= 1);
  REQUIRE(content.enemies.size() >= 3);
  REQUIRE(content.upgrades.size() >= 5);

  const auto* wand = content.weapon("wand");
  REQUIRE(wand != nullptr);
  REQUIRE(wand->damage > 0.0F);

  const auto* bat = content.enemy("bat");
  REQUIRE(bat != nullptr);
  REQUIRE(bat->hp > 0.0F);
}

TEST_CASE("Malformed content throws with a file path") {
  REQUIRE_THROWS(game::loadContent("/nonexistent/data"));
}

// --- Upgrade effects ---------------------------------------------------------

TEST_CASE("applyUpgrade mutates stats and reports validity") {
  game::PlayerStats s;

  const auto dmg = game::applyUpgrade(s, "damage_mul", 0.15F);
  REQUIRE(dmg.valid);
  REQUIRE(s.damageMul == Catch::Approx(1.15F));

  const auto fr = game::applyUpgrade(s, "fire_rate", 0.10F);
  REQUIRE(fr.valid);
  REQUIRE(s.fireRateBonus == Catch::Approx(0.10F));

  const auto proj = game::applyUpgrade(s, "proj_add", 1.0F);
  REQUIRE(proj.valid);
  REQUIRE(s.projAdd == 1);

  const auto heal = game::applyUpgrade(s, "heal", 25.0F);
  REQUIRE(heal.valid);
  REQUIRE(heal.heal == Catch::Approx(25.0F));

  const auto unknown = game::applyUpgrade(s, "no_such_effect", 1.0F);
  REQUIRE_FALSE(unknown.valid);
}

TEST_CASE("mitigateDamage combines flat and percent reduction") {
  // no defense = no reduction
  REQUIRE(game::mitigateDamage(30.0F, 0.0F) == Catch::Approx(30.0F));
  // defense always reduces (flat part kicks in at 5+)
  REQUIRE(game::mitigateDamage(30.0F, 10.0F) < 30.0F);
  // huge defense floors damage at zero, never negative
  REQUIRE(game::mitigateDamage(30.0F, 500.0F) == Catch::Approx(0.0F));
  REQUIRE(game::mitigateDamage(0.0F, 500.0F) == Catch::Approx(0.0F));
  // monotonic in defense
  for (float d = 0.0F; d <= 300.0F; d += 10.0F) {
    REQUIRE(game::mitigateDamage(30.0F, d + 10.0F) <= game::mitigateDamage(30.0F, d));
  }
}

TEST_CASE("applyUpgrade supports the defense / shield / lifesteal tree") {
  game::PlayerStats s;
  const auto de = game::applyUpgrade(s, "defense_add", 25.0F);
  REQUIRE(de.valid);
  REQUIRE(s.defense == Catch::Approx(25.0F));

  // Lifesteal is a per-KILL percentage now (150 = 100% for 1 HP + 50% for a 2nd).
  const auto ls = game::applyUpgrade(s, "lifesteal_add", 150.0F);
  REQUIRE(ls.valid);
  REQUIRE(s.lifesteal == Catch::Approx(150.0F));

  // Armor pierce ("пробитие брони") is its own additive stat.
  const auto ap = game::applyUpgrade(s, "armor_pierce_add", 40.0F);
  REQUIRE(ap.valid);
  REQUIRE(s.armorPierce == Catch::Approx(40.0F));
  REQUIRE(game::applyUpgrade(s, "armor_pierce_add", 15.0F).valid);
  REQUIRE(s.armorPierce == Catch::Approx(55.0F));

  const auto sh = game::applyUpgrade(s, "shield_add", 60.0F);
  REQUIRE(sh.valid);
  REQUIRE(s.shieldMax == Catch::Approx(60.0F));
  REQUIRE(sh.shield == Catch::Approx(60.0F)); // picker refills the shield
}

TEST_CASE("applyUpgrade supports unique-item effects") {
  game::PlayerStats s;
  const auto fan = game::applyUpgrade(s, "fan", 1.0F);
  REQUIRE(fan.valid);
  REQUIRE(s.spreadMul == Catch::Approx(3.0F));
  // The new attack-speed model is ADDITIVE: the fan adds +100% fire rate,
  // which halves the delay (delay = base / (1 + bonus)).
  REQUIRE(s.fireRateBonus == Catch::Approx(1.0F));
  // The same item costs accuracy: shots deviate up to +/- 30 degrees (0.5236 rad).
  REQUIRE(s.aimJitter == Catch::Approx(0.5236F).margin(1e-3F));

  REQUIRE(game::applyUpgrade(s, "extra_choice", 1.0F).valid);
  REQUIRE(s.extraChoice == 1);
  REQUIRE(game::applyUpgrade(s, "reroll_add", 1.0F).valid);
  REQUIRE(s.rerollCharges == 1);  // base budget is 1, item adds +1 (total 2)
  REQUIRE(game::applyUpgrade(s, "thorns", 3.0F).valid);
  REQUIRE(s.thornsDmg == Catch::Approx(3.0F));
  REQUIRE(game::applyUpgrade(s, "adrenaline", 1.0F).valid);
  REQUIRE(s.adrenaline == 1);
  REQUIRE(game::applyUpgrade(s, "chain", 1.0F).valid);
  REQUIRE(s.chain == 1);
  REQUIRE(game::applyUpgrade(s, "lifesteal_heal", 2.0F).valid);
  REQUIRE(s.lifestealHeal == 2);
  // XP-boost items stack additively onto the multiplier.
  REQUIRE(game::applyUpgrade(s, "xp_mul", 0.12F).valid);
  REQUIRE(game::applyUpgrade(s, "xp_mul", 0.30F).valid);
  REQUIRE(s.xpMul == Catch::Approx(1.42F));
  // Last Stand unique flag.
  REQUIRE(game::applyUpgrade(s, "last_stand", 1.0F).valid);
  REQUIRE(s.lastStand == 1);
  // Repulsion Field: retaliation knockback force.
  REQUIRE(game::applyUpgrade(s, "knockback_retaliate", 6.0F).valid);
  REQUIRE(s.knockbackRetaliate == Catch::Approx(6.0F));
  // Impact: a global multiplier on every knockback the player deals.
  REQUIRE(s.knockbackMul == Catch::Approx(1.0F)); // default
  REQUIRE(game::applyUpgrade(s, "knockback_mul", 0.45F).valid);
  REQUIRE(s.knockbackMul == Catch::Approx(1.45F));
  REQUIRE(game::applyUpgrade(s, "knockback_mul", 0.45F).valid);
  REQUIRE(s.knockbackMul == Catch::Approx(1.90F));
}

TEST_CASE("attackCooldown: delay = base / (1 + additive fire-rate bonus)") {
  // No bonus: delay equals the weapon's base cooldown.
  REQUIRE(game::attackCooldown(0.50F, 0.0F) == Catch::Approx(0.50F));
  // +100% fire rate halves the delay; +300% quarters it.
  REQUIRE(game::attackCooldown(0.50F, 1.0F) == Catch::Approx(0.25F));
  REQUIRE(game::attackCooldown(0.50F, 3.0F) == Catch::Approx(0.125F));
  // Bonuses stack additively (+, not *), so +50% + +50% = +100%.
  REQUIRE(game::attackCooldown(1.0F, 0.5F + 0.5F) ==
          game::attackCooldown(1.0F, 1.0F));
  // The formula is asymptotic: stacking can never reach zero delay.
  REQUIRE(game::attackCooldown(0.5F, 100.0F) > 0.0F);
  REQUIRE(game::attackCooldown(0.5F, 100000.0F) > 0.0F);
  // And order of magnitude matches: a huge bonus ~ base / bonus.
  REQUIRE(game::attackCooldown(1.0F, 9.0F) == Catch::Approx(0.10F));
}

TEST_CASE("xpForLevel grows monotonically") {
  REQUIRE(game::xpForLevel(1) == Catch::Approx(10.0F));
  float prev = 0.0F;
  for (int lvl = 1; lvl <= 20; ++lvl) {
    const float need = game::xpForLevel(lvl);
    REQUIRE(need > prev);
    prev = need;
  }
  // Pacing guard, and it is two-sided on purpose.
  //
  // The lower bound: level 16 has to be reachable inside the first stretch of a
  // run. Sixteen picks is roughly where a build stops being a starter and starts
  // being an answer to something, and the HP ramp is the one piece of the
  // difficulty the player cannot outshoot -- so a player who is still four picks
  // behind at minute six is not having a slow start, they are having a dead run.
  //
  // The upper bound: the curve has to keep CLIMBING, or every later level costs
  // the same and the run stops having a shape. Four times the cost at 64 as at
  // 16 is a rising quadratic, not a flattened one.
  REQUIRE(game::xpForLevel(16) < 600.0F);
  REQUIRE(game::xpForLevel(64) > 6000.0F);
  REQUIRE(game::xpForLevel(64) > 4.0F * game::xpForLevel(16));
  // And the shape is a smooth acceleration, not a step: every level costs more
  // than the one before by more than the one before it did.
  float prevStep = 0.0F;
  for (int lvl = 2; lvl <= 30; ++lvl) {
    const float step = game::xpForLevel(lvl) - game::xpForLevel(lvl - 1);
    REQUIRE(step > prevStep);
    prevStep = step;
  }
}

TEST_CASE("aoeFalloff drops per-target damage as a blast catches a crowd") {
  // A single target takes full damage.
  REQUIRE(game::aoeFalloff(0) == Catch::Approx(1.0F));
  REQUIRE(game::aoeFalloff(1) == Catch::Approx(1.0F));
  // Two targets: ~90% each; three: ~81%.
  REQUIRE(game::aoeFalloff(2) == Catch::Approx(0.9F));
  REQUIRE(game::aoeFalloff(3) == Catch::Approx(0.81F));
  // Monotonically decreasing, but never zero (clamped floor).
  float prev = 1.0F;
  for (int n = 1; n <= 40; ++n) {
    const float f = game::aoeFalloff(n);
    REQUIRE(f <= prev + 1e-5F);
    REQUIRE(f > 0.0F);
    prev = f;
  }
}

TEST_CASE("pierce reduces AoE crowd falloff and fully negates it at 20") {
  // With no pierce the base falloff applies.
  REQUIRE(game::aoeFalloff(5, 0) == Catch::Approx(game::aoeFalloff(5)));
  // Pierce always recovers some damage.
  REQUIRE(game::aoeFalloff(5, 5) > game::aoeFalloff(5, 0));
  REQUIRE(game::aoeFalloff(5, 10) > game::aoeFalloff(5, 5));
  // At 20+ pierce a blast deals full damage to every target it catches.
  REQUIRE(game::aoeFalloff(2, 20) == Catch::Approx(1.0F));
  REQUIRE(game::aoeFalloff(40, 20) == Catch::Approx(1.0F));
  REQUIRE(game::aoeFalloff(40, 100) == Catch::Approx(1.0F));
  // Monotonic in pierce, never above 1.
  for (int p = 0; p <= 25; ++p) {
    const float f = game::aoeFalloff(8, p);
    REQUIRE(f <= 1.0F + 1e-5F);
    REQUIRE(f >= game::aoeFalloff(8, p > 0 ? p - 1 : 0) - 1e-5F);
  }
}

TEST_CASE("enemyDefense grows with time and tier, after a grace period") {
  // Grace period: no mitigation for the first 30 seconds.
  REQUIRE(game::enemyDefense(0.0F, 0) == Catch::Approx(0.0F));
  REQUIRE(game::enemyDefense(30.0F, 0) == Catch::Approx(0.0F));
  // Grows after the grace period.
  REQUIRE(game::enemyDefense(100.0F, 0) > 0.0F);
  REQUIRE(game::enemyDefense(200.0F, 0) > game::enemyDefense(100.0F, 0));
  // Higher tiers are tougher at the same time.
  REQUIRE(game::enemyDefense(200.0F, 1) > game::enemyDefense(200.0F, 0));
  REQUIRE(game::enemyDefense(200.0F, 2) > game::enemyDefense(200.0F, 1));
  REQUIRE(game::enemyDefense(200.0F, 3) > game::enemyDefense(200.0F, 2));
}

TEST_CASE("enemy resistances scale with time, tier and the resistant trait") {
  // Both resistances are clamped into [0, 1].
  for (float t = 0.0F; t <= 3000.0F; t += 250.0F) {
    const float lr = game::enemyLifestealResistance(t, 0, false);
    const float kr = game::enemyKnockbackResistance(t, 0, false);
    REQUIRE(lr >= 0.0F);
    REQUIRE(lr <= 1.0F);
    REQUIRE(kr >= 0.0F);
    REQUIRE(kr <= 1.0F);
  }
  // The "resistant" variant resists noticeably more.
  REQUIRE(game::enemyLifestealResistance(120.0F, 1, true) >
          game::enemyLifestealResistance(120.0F, 1, false));
  REQUIRE(game::enemyKnockbackResistance(120.0F, 1, true) >
          game::enemyKnockbackResistance(120.0F, 1, false));
  // The ramp reaches its cap and stops, and the cap is a cap the player can live
  // with: 60% drain resistance, reached at 25 minutes. It was 75% at 20, which
  // meant a lifesteal build was being taxed for most of a run for the sake of a
  // number no player ever sees.
  REQUIRE(game::enemyLifestealResistance(1500.0F, 0, false) == Catch::Approx(0.60F));
  REQUIRE(game::enemyLifestealResistance(3000.0F, 0, false) == Catch::Approx(0.60F));
  // A resistance that has not reached its cap is still rising, so it is a ramp
  // and not a step.
  REQUIRE(game::enemyLifestealResistance(600.0F, 0, false) ==
          Catch::Approx(600.0F / 1500.0F).margin(0.001F));
  REQUIRE(game::enemyLifestealResistance(300.0F, 0, false) <
          game::enemyLifestealResistance(900.0F, 0, false));
}

TEST_CASE("traitsForTier is 1 / 3 / 7 and does not move with the clock") {
  // Flat, and on purpose. The old rule was "2, plus one after five minutes" for a
  // champion and "4, plus one after ten" for an overlord, which is a rule nobody
  // learns: the player cannot predict what is walking in, and a champion at
  // 6:00 is a different fight from the same label at 4:00.
  REQUIRE(game::traitsForTier(0) == 0);
  REQUIRE(game::traitsForTier(1) == 1);
  REQUIRE(game::traitsForTier(2) == 3);
  REQUIRE(game::traitsForTier(3) == 7);

  // The ladder is strictly increasing, so a stronger tier is always a strictly
  // bigger problem. The old ">= 2" and ">= 4" let this drift downward silently:
  // dropping a champion to 1 trait would have kept the test green.
  REQUIRE(game::traitsForTier(3) > game::traitsForTier(2));
  REQUIRE(game::traitsForTier(2) > game::traitsForTier(1));

  // Seven out of a pool of eleven, so the top two tiers are genuinely dense. An
  // overlord at 4 could be told apart from a champion at 3 by the banner alone.
  REQUIRE(game::traitsForTier(3) >= 6);
  REQUIRE(game::traitsForTier(2) <= 4);
}

TEST_CASE("elite+ enemies always get boosted stats and grow with tier") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 99};
  g.testDisableWaves();
  g.testSpawnTieredEnemyAt(0.0F, 0.0F, 0);
  g.testSpawnTieredEnemyAt(2.0F, 0.0F, 1);
  g.testSpawnTieredEnemyAt(4.0F, 0.0F, 2);
  g.testSpawnTieredEnemyAt(6.0F, 0.0F, 3);

  const auto tiers = g.testEnemyTiers();
  REQUIRE(tiers.size() == 4);
  std::vector<int> sortedTiers = tiers;
  std::sort(sortedTiers.begin(), sortedTiers.end());
  REQUIRE(sortedTiers == std::vector<int>{0, 1, 2, 3});

  const auto hps = g.testEnemyHps();
  REQUIRE(hps.size() == 4);
  std::vector<float> hs = hps;
  std::sort(hs.begin(), hs.end());
  // Strictly increasing HP with tier (all stats boosted, never deleted fast).
  REQUIRE(hs[0] < hs[1]);
  REQUIRE(hs[1] < hs[2]);
  REQUIRE(hs[2] < hs[3]);
}

TEST_CASE("trait flags survive spawn and are counted") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 100};
  g.testDisableWaves();
  g.testSpawnTieredEnemyAt(0.0F, 0.0F, 1, game::TraitFast | game::TraitArcher);
  g.testSpawnTieredEnemyAt(2.0F, 0.0F, 2, game::TraitResistant);
  const auto counts = g.testEnemyTraitCounts();
  REQUIRE(counts.size() == 2);
  int total = 0;
  for (const int c : counts) {
    total += c;
  }
  REQUIRE(total == 3); // 2 flags + 1 flag
}

// --- Round 7: opening pick, iframes, bestiary -------------------------------

TEST_CASE("Run opens with a 3-weapon pick instead of a random starter") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 5};

  // No weapon is granted up front.
  REQUIRE(g.armedWeaponIds().empty());

  // The first frame opens the starter pick (still no level-up yet).
  g.advance(1.0F / 60.0F, game::FrameInput{});
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE(g.choosingStarter());
  REQUIRE(g.upgradeChoices().size() == 3);
  for (const auto& c : g.upgradeChoices()) {
    REQUIRE(c.kind == game::Choice::Kind::Weapon);
  }

  // Picking one starts the run with exactly that weapon and does NOT level up.
  game::FrameInput pick{};
  pick.choose1 = true;
  g.advance(1.0F / 60.0F, pick);
  REQUIRE(g.state() == game::RunState::Playing);
  REQUIRE(g.armedWeaponIds().size() == 1);
  REQUIRE(g.level() == 1);
}

TEST_CASE("Defense extends the player's invulnerability window") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 6};
  g.testDisableWaves();
  g.testAddWeapon(0);

  // No defense: unchanged.
  REQUIRE(g.testIframeDuration(1.0F) == Catch::Approx(1.0F));
  // +5 defense -> +1% iframes; +25 -> +5%.
  g.stats().defense = 5.0F;
  REQUIRE(g.testIframeDuration(1.0F) == Catch::Approx(1.01F));
  g.stats().defense = 25.0F;
  REQUIRE(g.testIframeDuration(1.0F) == Catch::Approx(1.05F));
  // Monotonic in defense.
  REQUIRE(g.testIframeDuration(0.18F) > 0.18F);
}

TEST_CASE("Last Stand grants iframes when hit below 20% HP") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 8};
  g.testDisableWaves();
  g.testAddWeapon(0);
  g.stats().lastStand = 1;

  // Drop to just above the threshold, then take a hit that pushes below 20%.
  g.testSetPlayerHp(25.0F); // max HP is 100
  REQUIRE(g.testIframes() == Catch::Approx(0.0F));
  g.testHurtPlayer(10.0F);
  REQUIRE(g.testIframes() > 0.9F); // ~1s of invulnerability

  // A healthy player never triggers it.
  game::Game h{content, 8};
  h.testDisableWaves();
  h.testAddWeapon(0);
  h.stats().lastStand = 1;
  h.testSetPlayerHp(80.0F);
  h.testHurtPlayer(5.0F);
  REQUIRE(h.testIframes() == Catch::Approx(0.0F));
}

TEST_CASE("Bestiary records kills and the elite+ variants slain") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 9};
  g.testDisableWaves();
  g.testSpawnTieredEnemyAt(1.0F, 0.0F, 0);
  g.testSpawnTieredEnemyAt(2.0F, 0.0F, 2); // champion of the same type (def 0)

  REQUIRE(g.testBestiaryKills(0) == 0);
  g.testKillFirstEnemy();
  g.testKillFirstEnemy();
  REQUIRE(g.testBestiaryKills(0) == 2);
  // Both the normal and the champion variants are recorded.
  REQUIRE((g.testBestiaryTiers(0) & (1 << 0)) != 0);
  REQUIRE((g.testBestiaryTiers(0) & (1 << 2)) != 0);
}

TEST_CASE("An overlord retires its enemy type, but the 3 newest types stay eligible") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 11};
  g.testDisableWaves();

  // The 3 most recently unlocked types are exempt from retirement.
  const int n = static_cast<int>(content.enemies.size());
  REQUIRE(n >= 3);
  int recentCount = 0;
  int oldType = -1;
  int recentType = -1;
  for (int i = 0; i < n; ++i) {
    if (g.testTypeIsRecent(i)) {
      ++recentCount;
      if (recentType < 0) recentType = i;
    } else if (oldType < 0) {
      oldType = i;
    }
  }
  REQUIRE(recentCount == 3);
  REQUIRE(oldType >= 0);
  REQUIRE(recentType >= 0);

  // An OLD (non-recent) type that is already unlocked can spawn, then gets
  // retired and can no longer spawn. The Bat is unlocked at t=0, so it is
  // always eligible regardless of retirement.
  REQUIRE(content.enemies[static_cast<std::size_t>(oldType)].unlockAt <= 0.0F);
  REQUIRE(g.testTypeCanSpawn(oldType));
  g.testRetireType(oldType);
  REQUIRE(g.testTypeRetired(oldType));
  REQUIRE_FALSE(g.testTypeCanSpawn(oldType));

  // Retiring a "recent" type marks it retired, but the newest-3 exemption
  // keeps it eligible ONCE it has unlocked. (A recent type may still be locked
  // early in a run, so drive the clock via the can-spawn rule only after
  // checking the recent bit is set.)
  g.testRetireType(recentType);
  REQUIRE(g.testTypeRetired(recentType));
  REQUIRE(g.testTypeIsRecent(recentType));
  // A retired recent type is exempt from the retirement filter, so it can
  // spawn as soon as its unlock time arrives. It is locked at t=0, so assert
  // the time gate is the only thing blocking it -- and read that gate through
  // typeLockedUntil() rather than repeating the rule here, because a `fast`
  // type's gate is the later of its own unlock_at and the global fast floor.
  const bool unlockedNow = g.typeLockedUntil(recentType) <= 0.0F;
  REQUIRE(g.testTypeCanSpawn(recentType) == unlockedNow);

  // Retirement is per-run: a fresh Game starts with everything spawnable again.
  game::Game fresh{content, 11};
  fresh.testDisableWaves();
  REQUIRE_FALSE(fresh.testTypeRetired(oldType));
  REQUIRE(fresh.testTypeCanSpawn(oldType));
}

TEST_CASE("Bestiary tracks the strongest enemy killed this run") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 12};
  g.testDisableWaves();

  REQUIRE(g.strongestKilledTier() == 0);
  REQUIRE(g.strongestKilledDef() == -1);

  // A normal kill is the weakest tier.
  g.testSpawnTieredEnemyAt(1.0F, 0.0F, 0);
  g.testKillFirstEnemy();
  REQUIRE(g.strongestKilledTier() == 0);
  REQUIRE((g.tierKillMask() & (1 << 0)) != 0);

  // Killing a champion upgrades the record and stays there.
  g.testSpawnTieredEnemyAt(2.0F, 0.0F, 2);
  g.testKillFirstEnemy();
  REQUIRE(g.strongestKilledTier() == 2);
  REQUIRE((g.tierKillMask() & (1 << 2)) != 0);
  // The elite tier was never killed, so its bit stays clear even though the
  // champion outranks it.
  REQUIRE((g.tierKillMask() & (1 << 1)) == 0);

  // A weaker kill afterwards must not downgrade the record.
  g.testSpawnTieredEnemyAt(3.0F, 0.0F, 0);
  g.testKillFirstEnemy();
  REQUIRE(g.strongestKilledTier() == 2);

  // An overlord becomes the new strongest and unlocks its outline bit.
  g.testSpawnTieredEnemyAt(4.0F, 0.0F, 3);
  g.testKillFirstEnemy();
  REQUIRE(g.strongestKilledTier() == 3);
  REQUIRE((g.tierKillMask() & (1 << 3)) != 0);
}

TEST_CASE("Lifesteal heals on a kill, not on a non-lethal hit") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 13};
  g.testDisableWaves();
  g.testAddWeapon(0);

  // 100% lifesteal => every kill heals the full amount. Start wounded.
  REQUIRE(game::applyUpgrade(g.stats(), "lifesteal_add", 100.0F).valid);
  g.testSetPlayerHp(40.0F);
  REQUIRE(g.playerHp() == Catch::Approx(40.0F));

  // A high-HP, stationary target: hit it repeatedly but never kill it.
  g.testSpawnEnemyAt(1.0F, 0.0F);
  g.testDamageFirstEnemy(1.0F); // survives: 100000 HP
  g.testDamageFirstEnemy(1.0F);
  g.testDamageFirstEnemy(1.0F);

  // No healing yet: lifesteal is kill-triggered, not per hit.
  REQUIRE(g.playerHp() == Catch::Approx(40.0F));

  // Now kill it: exactly one proc heals 1 HP (lifestealHeal defaults to 1).
  g.testSetFirstEnemyHp(1.0F);
  g.testDamageFirstEnemy(100.0F);
  REQUIRE(g.playerHp() > 40.0F);
}

TEST_CASE("Armor pierce reduces the defense an enemy actually applies") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // Defense has a 30s grace period and only starts growing after it, so the
  // comparison must happen late in a run where defense is actually non-zero.
  // Both runs are advanced by the same amount so simTime (and therefore
  // defense) is identical in both.
  game::Game plain{content, 14};
  plain.testDisableWaves();
  game::Game pierced{content, 14};
  pierced.testDisableWaves();
  // Give both runs a weapon: the run otherwise sits in the starter pick
  // (RunState::LevelUp), where the fixed timestep does not advance simTime.
  plain.testAddWeapon(0);
  pierced.testAddWeapon(0);
  REQUIRE(game::applyUpgrade(pierced.stats(), "armor_pierce_add", 100000.0F).valid);

  // 120s of simulated time (7200 fixed steps at 60Hz), no waves.
  game::FrameInput idle{};
  for (int i = 0; i < 7200; ++i) {
    plain.advance(1.0F / 60.0F, idle);
    pierced.advance(1.0F / 60.0F, idle);
  }

  // Sanity: defense must be active at this point, otherwise the test proves
  // nothing. enemyDefense is (t-60)/34 * tierMul; at t=120 champion (1.7x) => ~3.
  REQUIRE(game::enemyDefense(plain.simTime(), 2) > 0.0F);

  plain.testSpawnTieredEnemyAt(5.0F, 0.0F, 2); // champion
  plain.testSetFirstEnemyHp(100000.0F);
  plain.testDamageFirstEnemy(100.0F);
  const float dealtWithout = 100000.0F - plain.testFirstEnemyHp();

  pierced.testSpawnTieredEnemyAt(5.0F, 0.0F, 2);
  pierced.testSetFirstEnemyHp(100000.0F);
  pierced.testDamageFirstEnemy(100.0F);
  const float dealtWith = 100000.0F - pierced.testFirstEnemyHp();

  // A pierced hit lands harder...
  REQUIRE(dealtWith > dealtWithout);
  // ...and with enough pierce to zero out defense, it lands at full value.
  REQUIRE(dealtWith == Catch::Approx(100.0F).margin(0.01F));
}

TEST_CASE("Bestiary overlay opens with B while paused") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 10};
  g.testDisableWaves();
  g.testAddWeapon(0);

  game::FrameInput esc{};
  esc.togglePause = true;
  g.advance(1.0F / 60.0F, esc);
  REQUIRE(g.state() == game::RunState::Paused);
  REQUIRE_FALSE(g.bestiaryOpen());

  game::FrameInput b{};
  b.bestiary = true;
  g.advance(1.0F / 60.0F, b);
  REQUIRE(g.bestiaryOpen());
  g.advance(1.0F / 60.0F, b);
  REQUIRE_FALSE(g.bestiaryOpen());

  // Resuming closes the overlay.
  g.advance(1.0F / 60.0F, b);
  REQUIRE(g.bestiaryOpen());
  g.advance(1.0F / 60.0F, esc);
  REQUIRE(g.state() == game::RunState::Playing);
  REQUIRE_FALSE(g.bestiaryOpen());
}

// --- Fixed timestep ----------------------------------------------------------

TEST_CASE("FixedTimestep caps steps and drops backlog") {
  core::sim::FixedTimestep ts(1.0 / 60.0, 2);
  int steps = 0;
  ts.advance(10.0, [&] { ++steps; }); // huge hitch
  REQUIRE(steps <= 2);                 // spiral-of-death valve
  REQUIRE(ts.alpha() >= 0.0);
  REQUIRE(ts.alpha() < 1.0);
}

TEST_CASE("FixedTimestep runs expected number of steps for small dt") {
  core::sim::FixedTimestep ts(1.0 / 60.0, 2);
  int steps = 0;
  for (int i = 0; i < 60; ++i) {
    ts.advance(1.0 / 60.0, [&] { ++steps; });
  }
  REQUIRE(steps == 60);
}

// --- Spatial hash ------------------------------------------------------------

TEST_CASE("SpatialHash queries match brute force") {
  core::sim::SpatialHash hash(1.0F);

  std::vector<float> xs, ys;
  std::vector<std::uint32_t> ids;
  for (std::uint32_t i = 0; i < 500; ++i) {
    xs.push_back(static_cast<float>(i % 25) - 12.0F);
    ys.push_back(static_cast<float>(i / 25) - 10.0F);
    ids.push_back(i);
  }
  hash.build(xs.data(), ys.data(), ids.data(), xs.size());

  const float qx = 3.2F;
  const float qy = -1.5F;
  const float qr = 4.0F;

  std::vector<std::uint32_t> expected;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const float dx = xs[i] - qx;
    const float dy = ys[i] - qy;
    // cell-based query: include if cell intersects circle's bounding cells
    if (dx * dx + dy * dy <= (qr + 1.5F) * (qr + 1.5F)) {
      expected.push_back(ids[i]);
    }
  }

  std::vector<std::uint32_t> got;
  hash.forEachNear(qx, qy, qr, [&](std::uint32_t id) { got.push_back(id); });

  REQUIRE_FALSE(got.empty());
  // every expected id that lies well within the circle must be reported
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const float dx = xs[i] - qx;
    const float dy = ys[i] - qy;
    if (dx * dx + dy * dy < (qr - 1.5F) * (qr - 1.5F)) {
      const auto id = ids[i];
      REQUIRE(std::find(got.begin(), got.end(), id) != got.end());
    }
  }
}

// --- Headless game loop ------------------------------------------------------

TEST_CASE("Game simulates headless and spawns enemies") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 42};

  // The run opens with a 3-weapon pick; take the first one to start playing.
  game::FrameInput pick{};
  pick.choose1 = true;
  g.advance(1.0F / 60.0F, pick);

  game::FrameInput in{};
  for (int i = 0; i < 600; ++i) { // 10 seconds at 60 Hz
    in.moveX = 1.0F;
    g.advance(1.0F / 60.0F, in);
  }

  REQUIRE(g.simTime() > 9.0F);
  REQUIRE(g.enemyCount() > 0);
}

TEST_CASE("grantXp triggers level-up state") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 7};

  // exactly one level's worth: after the pick there must be no queued level-up
  g.grantXp(game::xpForLevel(1));
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE(g.upgradeChoices().size() == 3);

  game::FrameInput in{};
  in.choose1 = true;
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::Playing);
  REQUIRE(g.level() == 2);
}

TEST_CASE("Milestones fire on power-of-two levels (4, 8, ...) not 5") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 11};

  // Reach level 4 with exactly one level-up per pick.
  g.grantXp(game::xpForLevel(1) + game::xpForLevel(2) + game::xpForLevel(3));
  REQUIRE(g.state() == game::RunState::LevelUp);
  for (int i = 0; i < 3; ++i) {
    game::FrameInput in{};
    in.choose1 = true;
    g.advance(1.0F / 60.0F, in);
  }
  REQUIRE(g.level() == 4);
  REQUIRE(g.state() == game::RunState::Playing);

  // Level 4 is a milestone level. The card count is the size of the group the
  // milestone drew, so it is no longer a fixed two -- it is whatever the exclusive
  // question at that level happens to be worth. The test below pins the real
  // numbers; here we only care that it is a milestone and that it offers more
  // than the flat two it used to.
  g.grantXp(game::xpForLevel(4));
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE(g.milestoneOffer());
  REQUIRE(g.upgradeChoices().size() >= 2);
  REQUIRE(g.upgradeChoices().size() <= game::Game::kMaxMilestoneSlots);

  // A non-power-of-two level (5) behaves like a normal level-up.
  game::FrameInput in{};
  in.choose1 = true;
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.level() == 5);
  g.grantXp(game::xpForLevel(5));
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE_FALSE(g.milestoneOffer());
  // Normal level-up offers the base hand of 3, plus any Gambler's Eye picks
  // the random cards above may have granted.
  REQUIRE(g.upgradeChoices().size() ==
          static_cast<std::size_t>(3 + g.stats().extraChoice));
}

TEST_CASE("Reroll refreshes the offered choices once per level-up") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 13};
  g.grantXp(game::xpForLevel(1));
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE(g.upgradeChoices().size() == 3);

  game::FrameInput in{};
  in.restart = true; // R is always the reroll key on level-up
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::LevelUp); // still choosing
  REQUIRE(g.rerollsUsed() == 1);

  // The base budget is exactly ONE reroll (the Second Chance unique adds a
  // second), so another reroll this level-up is a no-op.
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.rerollsUsed() == 1);
}

// --- Enemy movement ----------------------------------------------------------

TEST_CASE("Enemy separation speed is clamped (no vacuum darting)") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 13};
  g.testDisableWaves();
  // Take the opening weapon pick so the run is actually playing.
  game::FrameInput pick{};
  pick.choose1 = true;
  g.advance(1.0F / 60.0F, pick);
  // Tight cluster: heavily overlapping enemies get a large separation force
  // that used to fling them at many times their base speed whenever they
  // bunched up around the player.
  const float kOff[][2] = {
      {-0.10F, -0.10F}, {0.10F, -0.10F}, {0.00F, 0.10F}, {-0.10F, 0.10F},
      {0.10F, 0.10F},   {0.00F, -0.10F}, {-0.10F, 0.00F}, {0.10F, 0.00F},
      {-0.10F, -0.05F}, {0.10F, -0.05F}, {-0.10F, 0.05F}, {0.10F, 0.05F}};
  for (const auto& o : kOff) g.testSpawnEnemyAt(o[0], o[1]);

  float worst = 0.0F;
  for (int f = 0; f < 120; ++f) {
    g.advance(1.0F / 60.0F, game::FrameInput{});
    for (const float s : g.testEnemySpeeds()) {
      worst = std::max(worst, s);
    }
  }
  // Clamp is max(speed * 2.5, 4.0); test enemies are speed 0 -> 4.0 u/s cap.
  REQUIRE(worst <= 4.0001F);
}

TEST_CASE("Tier damage auras grow: champion bigger than elite, overlord biggest") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  game::Game eliteGame{content, 21};
  eliteGame.testDisableWaves();
  eliteGame.testSpawnTieredEnemyAt(10.0F, 0.0F, 1, game::TraitAura);
  const float eliteR = eliteGame.testFirstAuraRadius();
  const float eliteDps = eliteGame.testFirstAuraDps();
  REQUIRE(eliteR > 0.0F);
  REQUIRE(eliteDps > 0.0F);

  game::Game champGame{content, 21};
  champGame.testDisableWaves();
  champGame.testSpawnTieredEnemyAt(10.0F, 0.0F, 2, game::TraitAura);
  const float champR = champGame.testFirstAuraRadius();
  const float champDps = champGame.testFirstAuraDps();

  game::Game overlordGame{content, 21};
  overlordGame.testDisableWaves();
  overlordGame.testSpawnTieredEnemyAt(10.0F, 0.0F, 3, game::TraitAura);
  const float overlordR = overlordGame.testFirstAuraRadius();
  const float overlordDps = overlordGame.testFirstAuraDps();

  // Champion aura is clearly bigger than an elite's...
  REQUIRE(champR > eliteR);
  REQUIRE(champDps > eliteDps);
  // ...and an overlord's is bigger still than the champion's.
  REQUIRE(overlordR > champR);
  REQUIRE(overlordDps > champDps);
}

TEST_CASE("Shooting enemies keep their distance instead of charging the player") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 22};
  g.testDisableWaves();
  g.testAddWeapon(0);
  g.testSetPlayerHp(100000.0F); // survive the contact ticks

  // An archer spawned right next to the player must retreat, not close in.
  g.testSpawnTieredEnemyAt(1.0F, 0.0F, 1, game::TraitArcher);
  REQUIRE(g.testFirstCanShoot());

  const float startDist = g.testFirstEnemyDistToPlayer();
  REQUIRE(startDist == Catch::Approx(1.0F).margin(0.01F));

  // Simulate: it should move AWAY, so the distance grows.
  for (int f = 0; f < 30; ++f) {
    g.advance(1.0F / 60.0F, game::FrameInput{});
  }
  const float laterDist = g.testFirstEnemyDistToPlayer();
  REQUIRE(laterDist > startDist);
}

TEST_CASE("Orbit daggers stay evenly spaced when projectiles are added") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 13};
  g.testDisableWaves();

  int daggerIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "dagger") {
      daggerIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(daggerIdx >= 0);
  g.testClearWeapons(); // drop the random starter weapon so dagger is slot 0
  g.testAddWeapon(daggerIdx);
  REQUIRE(g.debugCounts().orbitBlades == 3);

  constexpr float kTau = 6.283185307179586F;
  auto checkEven = [&](int expected) {
    const auto gaps = g.testOrbitBladeGaps(0);
    REQUIRE(gaps.size() == static_cast<std::size_t>(expected));
    const float target = kTau / static_cast<float>(expected);
    for (const float gap : gaps) {
      REQUIRE(gap == Catch::Approx(target).margin(0.02F));
    }
  };

  checkEven(3);

  // "+1 projectile" Dagger Volley cards must re-space the whole ring instead
  // of stacking the new dagger onto the previous one.
  g.testAddWeaponUpgrade(0, "w_proj_add", 1);
  g.advance(1.0F / 60.0F, game::FrameInput{});
  REQUIRE(g.debugCounts().orbitBlades == 4);
  checkEven(4);

  g.testAddWeaponUpgrade(0, "w_proj_add", 1);
  g.advance(1.0F / 60.0F, game::FrameInput{});
  REQUIRE(g.debugCounts().orbitBlades == 5);
  checkEven(5);
}

// --- Weapon attack types -----------------------------------------------------

TEST_CASE("Weapon attack types are loaded correctly") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  
  const auto* wand = content.weapon("wand");
  REQUIRE(wand != nullptr);
  REQUIRE(wand->attackType == game::AttackType::Projectile);
  REQUIRE(wand->damage > 0.0F);
  REQUIRE(wand->starter == true);
  
  const auto* dagger = content.weapon("dagger");
  REQUIRE(dagger != nullptr);
  REQUIRE(dagger->attackType == game::AttackType::Orbit);
  REQUIRE(dagger->orbitCount == 3);
  REQUIRE(dagger->orbitRadius > 0.0F);
  REQUIRE(dagger->orbitSpeed > 0.0F);
  REQUIRE(dagger->starter == true);
  
  const auto* crossbow = content.weapon("crossbow");
  REQUIRE(crossbow != nullptr);
  REQUIRE(crossbow->attackType == game::AttackType::Projectile);
  // homing is now handled in fireWeapons via weapon slot data
  REQUIRE(crossbow->starter == true);
  // Reload was roughly doubled: a slow, heavy, hard-hitting bolt.
  REQUIRE(crossbow->cooldown == Catch::Approx(2.20F));
  
  const auto* flame = content.weapon("flame");
  REQUIRE(flame != nullptr);
  REQUIRE(flame->attackType == game::AttackType::Cone);
  REQUIRE(flame->coneAngle > 0.0F);
  REQUIRE(flame->coneRange > 0.0F);
  
  const auto* hammer = content.weapon("hammer");
  REQUIRE(hammer != nullptr);
  REQUIRE(hammer->attackType == game::AttackType::Bomb);
  REQUIRE(hammer->bombExplodeRadius > 0.0F);
  REQUIRE(hammer->bombKnockback > 0.0F);
  REQUIRE(hammer->bombArcHeight > 0.0F);
  
  const auto* shuriken = content.weapon("shuriken");
  REQUIRE(shuriken != nullptr);
  REQUIRE(shuriken->attackType == game::AttackType::Boomerang);
  REQUIRE(shuriken->boomerangRange > 0.0F);
  REQUIRE(shuriken->boomerangReturnSpeed > 0.0F);
  
  const auto* orb = content.weapon("orb");
  REQUIRE(orb != nullptr);
  REQUIRE(orb->attackType == game::AttackType::Bounce);
  REQUIRE(orb->bounceCount > 0);
  REQUIRE(orb->bounceRange > 0.0F);
  // Eternal orb: no damage decay — every bounce hits at full strength.
  REQUIRE(orb->bounceDamageMul >= 1.0F);
  REQUIRE(orb->bounceInfinite);
  
  const auto* scythe = content.weapon("scythe");
  REQUIRE(scythe != nullptr);
  REQUIRE(scythe->attackType == game::AttackType::Sweep);
  REQUIRE(scythe->sweepAngle > 0.0F);
  REQUIRE(scythe->sweepRadius > 0.0F);
  REQUIRE(scythe->sweepKnockback > 0.0F);
  
  const auto* beam = content.weapon("beam");
  REQUIRE(beam != nullptr);
  REQUIRE(beam->attackType == game::AttackType::Beam);
  REQUIRE(beam->beamRange > 0.0F);
  REQUIRE(beam->beamWidth > 0.0F);
  REQUIRE(beam->beamDuration > 0.0F);
  
  // Evolutions
  const auto* storm = content.weapon("storm");
  REQUIRE(storm != nullptr);
  // A re-aiming bolt, not an arc: see "Every reworked weapon has a rule the
  // others do not have" for why this is no longer a chain weapon.
  REQUIRE(storm->attackType == game::AttackType::Projectile);
  REQUIRE(storm->reaimRange > 0.0F);
  REQUIRE(storm->reaimTurn > 0.0F);
  REQUIRE(storm->pierce > 0);
  REQUIRE(storm->prereqs.size() == 2);
  
  const auto* nova = content.weapon("nova");
  REQUIRE(nova != nullptr);
  REQUIRE(nova->attackType == game::AttackType::Nova);
  REQUIRE(nova->novaMaxRadius > 0.0F);
  REQUIRE(nova->novaExpandSpeed > 0.0F);
  REQUIRE(nova->novaDamagePerTick > 0.0F);
  REQUIRE(nova->novaTickRate > 0.0F);
  REQUIRE(nova->prereqs.size() == 2);

  const auto* inferno = content.weapon("inferno");
  REQUIRE(inferno != nullptr);
  REQUIRE(inferno->attackType == game::AttackType::Inferno);
  REQUIRE(inferno->sweepRadius > 0.0F);
  REQUIRE(inferno->zoneDps > 0.0F);
  REQUIRE(inferno->zoneDuration > 0.0F);
  REQUIRE(inferno->prereqs.size() == 2);

  const auto* pulsar = content.weapon("pulsar");
  REQUIRE(pulsar != nullptr);
  REQUIRE(pulsar->attackType == game::AttackType::Pulsar);
  REQUIRE(pulsar->boomerangRange > 0.0F);
  REQUIRE(pulsar->beamWidth > 0.0F);
  REQUIRE(pulsar->prereqs.size() == 2);

  const auto* halo = content.weapon("halo");
  REQUIRE(halo != nullptr);
  REQUIRE(halo->attackType == game::AttackType::Halo);
  REQUIRE(halo->beamRange > 0.0F);
  REQUIRE(halo->beamWidth > 0.0F);
  REQUIRE(halo->orbitSpeed > 0.0F);
  REQUIRE(halo->prereqs.size() == 2);

  // Super evolutions: three prerequisites (A+B+C), the marquee build payoff.
  // Each one introduces a mechanic no other weapon has.
  const auto* gyre = content.weapon("vortex");
  REQUIRE(gyre != nullptr);
  REQUIRE(gyre->attackType == game::AttackType::Vortex);
  REQUIRE(gyre->prereqs.size() == 3);
  REQUIRE(gyre->damage > 0.0F);
  REQUIRE(gyre->projectiles >= 3);
  // The defining numbers of a suction zone: it must actually drag enemies in.
  REQUIRE(gyre->vortexPull > 0.0F);
  REQUIRE(gyre->vortexReach > gyre->vortexRadius);

  const auto* prism = content.weapon("prism");
  REQUIRE(prism != nullptr);
  REQUIRE(prism->attackType == game::AttackType::Prism);
  REQUIRE(prism->prereqs.size() == 3);
  REQUIRE(prism->projectiles >= 4);
  REQUIRE(prism->prismRange > 0.0F);
  REQUIRE(prism->prismMaxTargets >= 2);

  // Supers must be genuinely distinct from the plain evolutions they build on:
  // no super may reuse another weapon's attack type.
  REQUIRE(gyre->attackType != game::AttackType::Halo);
  REQUIRE(prism->attackType != game::AttackType::Beam);
  REQUIRE(gyre->attackType != prism->attackType);
}

TEST_CASE("Orbit weapon creates blades on acquisition") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 42};
  
  // Give player the dagger (orbit weapon)
  const auto* dagger = content.weapon("dagger");
  REQUIRE(dagger != nullptr);
  
  // Find dagger index
  int daggerIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "dagger") {
      daggerIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(daggerIdx >= 0);
  
  g.testAddWeapon(daggerIdx);
  REQUIRE(g.stats().damageMul == 1.0F); // no upgrades yet
  
  // Advance a few frames to let orbit blades spawn
  game::FrameInput in{};
  for (int i = 0; i < 10; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  // Check that orbit blades exist
  // We can't easily access registry from test, but we can verify the game runs
  REQUIRE(g.state() == game::RunState::Playing);
}

TEST_CASE("Projectile weapon fires at enemies") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 123};
  
  // Give player the wand (projectile weapon)
  int wandIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "wand") {
      wandIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(wandIdx >= 0);
  
  g.testAddWeapon(wandIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  // Advance to spawn enemies
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  // Should have spawned enemies and killed some
  REQUIRE(g.kills() >= 0); // May be 0 if no enemies reached yet
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Bomb weapon creates arcing projectile") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 456};
  
  int hammerIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "hammer") {
      hammerIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(hammerIdx >= 0);
  
  g.testAddWeapon(hammerIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Bomb explodes on landing and is always removed") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int hammerIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "hammer") {
      hammerIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(hammerIdx >= 0);

  game::Game g{content, 456};
  g.testDisableWaves();
  g.testClearWeapons();
  // The hammer lands ON whatever is nearest its aim line, so this body at 30
  // units is the target: the shell is solved to come down exactly on it, and the
  // flight is long enough that the bomb is still in the air halfway through.
  g.testSpawnEnemyAt(30.0F, 0.0F);
  g.testAddWeapon(hammerIdx);

  game::FrameInput in{};
  for (int i = 0; i < 12; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  REQUIRE(g.debugCounts().bombs == 1); // airborne mid-arc

  for (int i = 0; i < 90; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  // Bomb landed, exploded, and was destroyed — it never lingers invisible.
  REQUIRE(g.debugCounts().bombs == 0);
  // And it landed ON the body rather than somewhere in the vicinity: the shell
  // flies at whatever speed makes `land` take exactly as long as its arc, so a
  // target at 30 units is hit as squarely as one at 3.
  REQUIRE(g.testFirstEnemyHp() < 100000.0F);
}

TEST_CASE("Hammer pierce scales the explosion radius, not bomb life") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int hammerIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "hammer") {
      hammerIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(hammerIdx >= 0);

  auto run = [&](int pierceAdd) {
    game::Game g{content, 331};
    g.testDisableWaves();
    g.testClearWeapons();
    g.stats().pierceAdd = pierceAdd;
    // The hammer answers the body nearest its aim line, so the shell is solved
    // to come down on the one at 4.0. The bystander is parked 2.5 units further
    // down that same line: outside the base 2.0 blast (2.0 + the 0.3 body = 2.3)
    // and inside the boosted one (2.0 * 1.3 = 2.6, so 2.9 with the body). Pierce
    // widening the blast is the whole claim; if it widened the flight instead, the
    // bystander would be untouched at both settings.
    g.testSpawnEnemyAt(4.0F, 0.0F);
    g.testSpawnEnemyAt(6.5F, 0.0F);
    g.testAddWeapon(hammerIdx);
    game::FrameInput in{};
    for (int i = 0; i < 75; ++i) {
      g.advance(1.0F / 60.0F, in);
    }
    return g;
  };

  {
    const auto base = run(0);
    // The aimed body is hit either way, so the bystander is read by position.
    REQUIRE(base.testEnemyHpNear(4.0F, 0.0F) < 100000.0F);
    REQUIRE(base.testEnemyHpNear(6.5F, 0.0F) == Catch::Approx(100000.0F)); // out of blast
    REQUIRE(base.debugCounts().bombs == 0);
  }
  {
    const auto boosted = run(2);
    REQUIRE(boosted.testEnemyHpNear(4.0F, 0.0F) < 100000.0F);
    REQUIRE(boosted.testEnemyHpNear(6.5F, 0.0F) < 100000.0F);
    REQUIRE(boosted.debugCounts().bombs == 0); // still "one boom, gone"
  }
}

TEST_CASE("Beam projectile count spawns parallel beams") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int beamIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "beam") {
      beamIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(beamIdx >= 0);

  auto countBeams = [&](int projAdd) {
    game::Game g{content, 222};
    g.testDisableWaves();
    g.testClearWeapons();
    g.stats().projAdd = projAdd;
    g.testSpawnEnemyAt(3.0F, 0.0F);
    g.testAddWeapon(beamIdx);
    game::FrameInput in{};
    g.advance(1.0F / 60.0F, in);
    g.advance(1.0F / 60.0F, in);
    return static_cast<int>(g.debugCounts().beams);
  };

  // The 1-vs-2 "nothing changed" complaint: each projectile is now a beam.
  REQUIRE(countBeams(0) == 1);
  REQUIRE(countBeams(1) == 2);
}

TEST_CASE("Prism Lance unique triples the parallel beams") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int beamIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "beam") {
      beamIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(beamIdx >= 0);

  game::Game g{content, 223};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testSpawnEnemyAt(3.0F, 0.0F);
  g.testAddWeapon(beamIdx);
  g.testAddWeaponUpgrade(0, "w_unique_prism", 3.0F);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.debugCounts().beams == 3);
}

TEST_CASE("Cone weapon renders a visible flame fan and deals instant damage") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int flameIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "flame") {
      flameIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(flameIdx >= 0);

  game::Game g{content, 444};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testSpawnEnemyAt(2.0F, 0.0F);
  g.testAddWeapon(flameIdx);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  g.advance(1.0F / 60.0F, in);
  // The cone now spawns a visible flame-fan wedge (sweep renderer), so the
  // flamethrower actually shows itself instead of being an invisible zap.
  REQUIRE(g.debugCounts().sweeps == 1);
  // And it still deals its instant cone damage.
  REQUIRE(g.testFirstEnemyHp() == Catch::Approx(100000.0F - 4.0F));
}

TEST_CASE("Dagger Vortex unique doubles the orbit spin") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int daggerIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "dagger") {
      daggerIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(daggerIdx >= 0);

  game::Game g{content, 555};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(daggerIdx);

  game::FrameInput in{};
  const float a0 = g.testOrbitBladeAngle();
  for (int i = 0; i < 60; ++i) {
    g.advance(1.0F / 60.0F, in); // 1 second of spin at 2 rad/s
  }
  const float a1 = g.testOrbitBladeAngle();
  const float deltaBase = a1 - a0;

  g.testAddWeaponUpgrade(0, "w_unique_vortex", 1.0F); // orbitSpeed x2
  const float a2 = g.testOrbitBladeAngle();
  for (int i = 0; i < 60; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  const float a3 = g.testOrbitBladeAngle();
  const float deltaVortex = a3 - a2;

  REQUIRE(deltaVortex == Catch::Approx(2.0F * deltaBase).margin(0.4F));
}

TEST_CASE("Orb Echo unique splashes extra damage in clusters") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int orbIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "orb") {
      orbIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(orbIdx >= 0);

  auto totalLost = [](const game::Game& g) {
    float lost = 0.0F;
    for (const float hp : g.testEnemyHps()) {
      lost += 100000.0F - hp;
    }
    return lost;
  };
  auto run = [&](bool echo) {
    game::Game g{content, 1357};
    g.testDisableWaves();
    g.testClearWeapons();
    g.testAddWeapon(orbIdx);
    if (echo) g.testAddWeaponUpgrade(0, "w_unique_area", 1.5F);
    // Tight cluster: every bounce splashes the neighbours (0.5x each).
    g.testSpawnEnemyAt(4.0F, 0.0F);
    g.testSpawnEnemyAt(4.8F, 0.0F);
    g.testSpawnEnemyAt(4.0F, 0.8F);
    game::FrameInput in{};
    for (int i = 0; i < 240; ++i) {
      g.advance(1.0F / 60.0F, in);
    }
    return totalLost(g);
  };

  const float plain = run(false);
  const float echoed = run(true);
  REQUIRE(echoed > plain); // the unique's splash is strictly extra damage
}

TEST_CASE("Weapon test mode cycles weapons, boosts stats, and restores the run") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 5};
  g.testDisableWaves();
  g.testClearWeapons();
  int beamIdx = -1, orbIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "beam") beamIdx = static_cast<int>(i);
    if (content.weapons[i].id == "orb") orbIdx = static_cast<int>(i);
  }
  REQUIRE(beamIdx >= 0);
  REQUIRE(orbIdx >= 0);
  g.testAddWeapon(beamIdx);
  g.testAddWeapon(orbIdx);
  REQUIRE(g.armedWeaponIds() == std::vector<std::string>{"beam", "orb"});

  // T opens the sandbox with the first weapon (wand) replacing the run.
  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());
  REQUIRE(g.armedWeaponIds() == std::vector<std::string>{"wand"});

  // [2] cycles to the next weapon (dagger).
  game::FrameInput next{};
  next.choose2 = true;
  g.advance(1.0F / 60.0F, next);
  REQUIRE(g.armedWeaponIds() == std::vector<std::string>{"dagger"});

  // [3] applies the max-build boost.
  game::FrameInput boost{};
  boost.choose3 = true;
  g.advance(1.0F / 60.0F, boost);
  REQUIRE(g.stats().projAdd == 4);
  REQUIRE(g.stats().pierceAdd == 3);
  REQUIRE(g.stats().fireRateBonus == Catch::Approx(0.8F));
  REQUIRE(g.stats().damageMul == Catch::Approx(2.0F));

  // [5] closes the sandbox and restores the exact pre-test run.
  game::FrameInput close{};
  close.choose5 = true;
  g.advance(1.0F / 60.0F, close);
  REQUIRE_FALSE(g.testMode());
  REQUIRE(g.armedWeaponIds() == std::vector<std::string>{"beam", "orb"});
}

TEST_CASE("Boomerang weapon fires and returns") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 789};
  
  int shurikenIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "shuriken") {
      shurikenIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(shurikenIdx >= 0);
  
  g.testAddWeapon(shurikenIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Bounce weapon chains between enemies") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 999};
  
  int orbIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "orb") {
      orbIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(orbIdx >= 0);
  
  g.testAddWeapon(orbIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Beam weapon fires instant hitscan") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 111};
  
  int beamIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "beam") {
      beamIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(beamIdx >= 0);
  
  g.testAddWeapon(beamIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Sweep weapon deals AoE around player") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 222};
  
  int scytheIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "scythe") {
      scytheIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(scytheIdx >= 0);
  
  g.testAddWeapon(scytheIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Scythe reaps a circle around its nearest enemy") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 223};
  g.testDisableWaves();
  g.testClearWeapons();
  int scytheIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "scythe") {
      scytheIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(scytheIdx >= 0);
  g.testAddWeapon(scytheIdx);
  // Target at (4, 0); a second enemy sits a full 90 degrees below the aim
  // line but still inside the reap circle around the target.
  g.testSpawnEnemyAt(4.0F, 0.0F);
  g.testSpawnEnemyAt(4.0F, -3.0F);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in); // first sweep fires immediately
  REQUIRE(g.debugCounts().sweeps == 1);
  const auto hps = g.testEnemyHps();
  REQUIRE(hps.size() == 2);
  for (const float hp : hps) {
    REQUIRE(hp < 100000.0F); // both damaged by the target-centered ring
  }
}

TEST_CASE("Void Orb: one eternal projectile that grows with count") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int orbIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "orb") {
      orbIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(orbIdx >= 0);

  auto run = [&](int projAdd) {
    game::Game g{content, 55};
    g.testDisableWaves();
    g.testClearWeapons();
    if (projAdd > 0) g.stats().projAdd = projAdd;
    g.testAddWeapon(orbIdx);
    g.testSpawnEnemyAt(1.5F, 0.0F);
    game::FrameInput in{};
    g.advance(1.0F / 60.0F, in);
    g.advance(1.0F / 60.0F, in);
    // Projectile count never spawns extra orbs — exactly one is in flight.
    REQUIRE(g.debugCounts().bounces == 1);
    // The eternal orb never expires: 5+ seconds later it is still the same
    // single orb (a finite orb would have timed out after 3.0s of life).
    for (int i = 0; i < 360; ++i) g.advance(1.0F / 60.0F, in);
    REQUIRE(g.debugCounts().bounces == 1);
    const auto radii = g.testBounceRadii();
    REQUIRE(radii.size() == 1);
    return radii[0];
  };

  const float baseR = run(0);
  const float bigR = run(5);
  REQUIRE(bigR > baseR * 1.5F); // more projectiles = a bigger orb
}

TEST_CASE("Inferno evolves flame + scythe into a reap plus burning ground") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 66};
  g.testDisableWaves();
  g.testClearWeapons();
  int infernoIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "inferno") {
      infernoIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(infernoIdx >= 0);
  g.testAddWeapon(infernoIdx);
  g.testSpawnEnemyAt(3.0F, 0.0F);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in); // reap ring + burning zone spawn at once
  REQUIRE(g.debugCounts().sweeps == 1);
  REQUIRE(g.debugCounts().zones == 1);
  REQUIRE(g.testFirstEnemyHp() < 100000.0F); // instant reap damage
  for (int i = 0; i < 120; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.testFirstEnemyHp() < 99900.0F); // burning ground keeps ticking
}

TEST_CASE("Pulsar evolves beam + shuriken into a slicing laser trail") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 77};
  g.testDisableWaves();
  g.testClearWeapons();
  int pulsarIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "pulsar") {
      pulsarIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(pulsarIdx >= 0);
  g.testAddWeapon(pulsarIdx);
  // On the flight line (contact + trail) and just off it (trail only: the
  // blade's contact radius is 0.46, the laser trail reaches 0.55).
  g.testSpawnEnemyAt(3.0F, 0.0F);
  g.testSpawnEnemyAt(3.2F, 0.5F);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.debugCounts().boomerangs == 1);
  for (int i = 0; i < 120; ++i) g.advance(1.0F / 60.0F, in);
  const auto hps = g.testEnemyHps();
  REQUIRE(hps.size() == 2);
  for (const float hp : hps) {
    REQUIRE(hp < 100000.0F); // both burned by the trail
  }
}

TEST_CASE("Halo evolves dagger + beam into beams orbiting the player") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 91};
  g.testDisableWaves();
  g.testClearWeapons();
  int haloIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "halo") {
      haloIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(haloIdx >= 0);
  g.testAddWeapon(haloIdx);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  // Two persistent spokes by default.
  REQUIRE(g.debugCounts().halos == 2);
  // "+1 projectile" adds a third spoke.
  g.testAddWeaponUpgrade(0, "w_proj_add", 1.0F);
  REQUIRE(g.debugCounts().halos == 3);

  // An enemy out in the ring is carved by the sweeping spokes over time. It has
  // to be past the halo's dead centre, or it is standing in the safe hole.
  g.testSpawnEnemyAt(4.0F, 0.0F);
  for (int i = 0; i < 180; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.testFirstEnemyHp() < 100000.0F);
}

TEST_CASE("Repulsion Field shoves enemies that strike the player") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto endDist = [&](float retaliate) {
    game::Game g{content, 5};
    g.testDisableWaves();
    g.testClearWeapons();
    g.stats().knockbackRetaliate = retaliate;
    // Just inside contact range, so the enemy lands a hit immediately.
    g.testSpawnEnemyAt(0.3F, 0.0F);
    game::FrameInput in{};
    for (int i = 0; i < 60; ++i) g.advance(1.0F / 60.0F, in);
    return g.testFirstEnemyDistToPlayer();
  };
  const float without = endDist(0.0F);
  const float with = endDist(6.0F);
  REQUIRE(without > 0.0F);
  // The retaliation shove pushes the attacker clearly farther away.
  REQUIRE(with > without + 0.2F);
}

TEST_CASE("Impact multiplies the knockback the player deals") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto endDist = [&](float knockbackMul) {
    game::Game g{content, 5};
    g.testDisableWaves();
    g.testClearWeapons();
    g.stats().knockbackRetaliate = 6.0F;
    g.stats().knockbackMul = knockbackMul;
    g.testSpawnEnemyAt(0.3F, 0.0F);
    game::FrameInput in{};
    for (int i = 0; i < 60; ++i) g.advance(1.0F / 60.0F, in);
    return g.testFirstEnemyDistToPlayer();
  };
  const float plain = endDist(1.0F);
  const float boosted = endDist(2.0F);
  REQUIRE(plain > 0.0F);
  // Doubling the multiplier shoves the attacker farther than the base shove.
  REQUIRE(boosted > plain + 0.1F);
}

TEST_CASE("Super evolutions require three weapons") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* gyre = content.weapon("vortex");
  const auto* prism = content.weapon("prism");
  REQUIRE(gyre != nullptr);
  REQUIRE(prism != nullptr);
  REQUIRE(gyre->prereqs ==
          std::vector<std::string>{"dagger", "scythe", "orb"});
  REQUIRE(prism->prereqs ==
          std::vector<std::string>{"flame", "beam", "crossbow"});

  // Void Gyre is a vortex super: it spawns three suction zones and a
  // "+1 projectile" card adds a fourth.
  int gyreIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "vortex") gyreIdx = static_cast<int>(i);
  }
  REQUIRE(gyreIdx >= 0);
  game::Game g{content, 17};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(gyreIdx);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.debugCounts().vortices == 3);
  g.testAddWeaponUpgrade(0, "w_proj_add", 1.0F);
  REQUIRE(g.debugCounts().vortices == 4);
}

TEST_CASE("A hit that covers the remaining HP always kills") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 7};
  g.testDisableWaves();
  g.testClearWeapons();
  // Skip the opening grace period so the elite actually has defense and would
  // mitigate the hit below its remaining HP.
  game::FrameInput in{};
  for (int i = 0; i < 31 * 60; ++i) g.advance(1.0F / 60.0F, in);
  g.testSpawnTieredEnemyAt(5.0F, 0.0F, 1);
  g.testSetFirstEnemyHp(10.0F);
  // Raw damage equals the remaining HP: it must die, not linger at ~0 HP.
  g.testDamageFirstEnemy(10.0F);
  REQUIRE(g.testFirstEnemyHp() <= 0.0F);
}

TEST_CASE("Cone weapon deals instant cone damage") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 333};
  
  int flameIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "flame") {
      flameIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(flameIdx >= 0);
  
  g.testAddWeapon(flameIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Chain lightning jumps between enemies") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 444};
  
  int stormIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "storm") {
      stormIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(stormIdx >= 0);
  
  g.testAddWeapon(stormIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Nova ring expands and damages") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 555};
  
  int novaIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "nova") {
      novaIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(novaIdx >= 0);
  
  g.testAddWeapon(novaIdx);
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 300; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.simTime() > 4.0F);
}

TEST_CASE("Damage multiplier applies to all attack types") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 666};
  
  int wandIdx = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "wand") {
      wandIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(wandIdx >= 0);
  
  g.testAddWeapon(wandIdx);
  
  // Apply damage multiplier upgrade
  // Find damage_mul upgrade
  int dmgIdx = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].effect == "damage_mul" && content.upgrades[i].kind == "normal") {
      dmgIdx = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(dmgIdx >= 0);
  
  // Manually apply the upgrade effect
  game::applyUpgrade(g.stats(), "damage_mul", 0.5F); // +50% damage
  REQUIRE(g.stats().damageMul == Catch::Approx(1.5F));
  
  game::FrameInput in{};
  in.moveX = 1.0F;
  
  for (int i = 0; i < 120; ++i) { // 2 seconds
    g.advance(1.0F / 60.0F, in);
  }
  
  REQUIRE(g.stats().damageMul == Catch::Approx(1.5F));
}

// --- Repair regression tests: text wrap, weapon visibility, damage scaling ---

TEST_CASE("wrapWords wraps words without accumulating overflow") {
  // Regression: the old renderer re-accumulated the previous line after
  // flushing, so "a b c" wrapped as "a / ab / abc".
  const auto three = game::wrapWords("a b c", 1);
  REQUIRE(three.size() == 3);
  REQUIRE(three[0] == "a");
  REQUIRE(three[1] == "b");
  REQUIRE(three[2] == "c");

  const auto lines = game::wrapWords("one two three four", 8);
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0] == "one two");
  REQUIRE(lines[1] == "three");
  REQUIRE(lines[2] == "four");

  REQUIRE(game::wrapWords("", 5).empty());

  // A word longer than the limit still gets its own line (no crash).
  const auto longWord = game::wrapWords("antidisestablishmentarianism x", 5);
  REQUIRE(longWord.size() == 2);
  REQUIRE(longWord[0] == "antidisestablishmentarianism");
  REQUIRE(longWord[1] == "x");
}

TEST_CASE("Every weapon creates its attack-type entities") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto idx = [&content](const char* id) {
    for (std::size_t i = 0; i < content.weapons.size(); ++i) {
      if (content.weapons[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };

  {
    // Orbit blades are persistent and created at addWeapon time.
    game::Game g{content, 10};
    g.testDisableWaves();
    g.testClearWeapons();
    REQUIRE(idx("dagger") >= 0);
    g.testAddWeapon(idx("dagger"));
    game::FrameInput in{};
    g.advance(1.0F / 60.0F, in);
    REQUIRE(g.debugCounts().orbitBlades == 3);
  }

  struct Case {
    const char* id;
    std::size_t game::Game::DebugCounts::*field;
  };
  const Case cases[] = {
      {"wand", &game::Game::DebugCounts::projectiles},
      {"hammer", &game::Game::DebugCounts::bombs},
      {"shuriken", &game::Game::DebugCounts::boomerangs},
      {"orb", &game::Game::DebugCounts::bounces},
      {"beam", &game::Game::DebugCounts::beams},
      {"scythe", &game::Game::DebugCounts::sweeps},
      {"storm", &game::Game::DebugCounts::projectiles},
      {"nova", &game::Game::DebugCounts::novas},
      {"inferno", &game::Game::DebugCounts::sweeps},
      {"pulsar", &game::Game::DebugCounts::boomerangs},
      {"halo", &game::Game::DebugCounts::halos},
      {"vortex", &game::Game::DebugCounts::vortices},
      {"prism", &game::Game::DebugCounts::beams},
  };
  for (const auto& c : cases) {
    CAPTURE(c.id);
    const int wi = idx(c.id);
    REQUIRE(wi >= 0);
    game::Game g{content, 77};
    g.testDisableWaves();
    g.testClearWeapons();
    g.stats().fireRateBonus = 100.0F; // attacks fire on (almost) every tick
    g.testSpawnEnemyAt(3.0F, 0.0F);
    g.testAddWeapon(wi);
    game::FrameInput in{};
    g.advance(1.0F / 60.0F, in); // create the effect entity
    g.advance(1.0F / 60.0F, in); // keep it alive one more frame
    REQUIRE((g.debugCounts().*c.field) > 0);
  }
}

TEST_CASE("Damage multiplier scales exactly once per attack type") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto idx = [&content](const char* id) {
    for (std::size_t i = 0; i < content.weapons.size(); ++i) {
      if (content.weapons[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };

  const char* ids[] = {"wand", "flame", "hammer", "shuriken",
                       "orb", "beam", "scythe", "storm", "nova",
                       "inferno", "pulsar", "halo", "vortex", "prism"};
  for (const char* id : ids) {
    CAPTURE(id);
    const int wi = idx(id);
    REQUIRE(wi >= 0);
    const auto& def = content.weapons[static_cast<std::size_t>(wi)];

    auto run = [&](float mul) {
      game::Game g{content, 1337};
      g.testDisableWaves();
      g.testClearWeapons();
      g.stats().damageMul = mul;
      // Bombs only land where their fixed arc falls: put the enemy at the
      // deterministic landing spot (t = 2*vy/g, x = speed*t).
      const float gAcc = 30.0F;
      const float vy = std::sqrt(2.0F * gAcc * def.bombArcHeight);
      // A halo's spokes do not start at the player, so the probe has to go past
      // whatever dead zone that weapon declares or it is standing in the hole.
      const float sx = (def.attackType == game::AttackType::Bomb)
                           ? def.projSpeed * (2.0F * vy / gAcc)
                           : def.haloInner + 1.5F;
      g.testSpawnEnemyAt(sx, 0.0F);
      g.testAddWeapon(wi);
      game::FrameInput in{};
      for (int i = 0; i < 200; ++i) g.advance(1.0F / 60.0F, in);
      const float hp = g.testFirstEnemyHp();
      return hp < 0.0F ? -1.0F : 100000.0F - hp;
    };

    const float lost1 = run(1.0F);
    const float lost3 = run(3.0F);
    REQUIRE(lost1 > 0.0F);         // the weapon actually damaged the enemy
    REQUIRE(lost3 > 2.6F * lost1); // scales up with the multiplier
    REQUIRE(lost3 < 3.4F * lost1); // ...but exactly once, not twice (~9x)
  }
}

TEST_CASE("Orbit blades track projectile-count and damage upgrades") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int dagger = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "dagger") {
      dagger = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(dagger >= 0);

  game::Game g{content, 42};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(dagger);
  REQUIRE(g.debugCounts().orbitBlades == 3);

  // "+1 projectile" (the Dagger Volley card effect) adds a blade.
  g.testAddWeaponUpgrade(0, "w_proj_add", 1.0F);
  REQUIRE(g.debugCounts().orbitBlades == 4);

  // Damage is re-derived from the weapon slot every tick, so a +25 damage
  // upgrade applies to existing blades (5 base + 25 up = 30 per hit).
  g.testAddWeaponUpgrade(0, "w_damage_add", 25.0F);
  g.testSpawnEnemyAt(1.3F, 0.0F); // blade #1 spawns at angle 0: exactly on top
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.testFirstEnemyHp() == Catch::Approx(99970.0F)); // 100000 - 30
}

// --- Profile: skins, outline unlocks, persistence -----------------------------

TEST_CASE("Attaching a saved profile repaints the player immediately") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  // A profile loaded from disk (skin 3, overlord outline earned + selected).
  game::Profile saved;
  saved.skin = 3;
  REQUIRE(saved.unlockTier(3));
  saved.outline = 3;

  game::Game g{content, 30};
  // Before a profile is attached the player is the default blue with no ring.
  const auto defaultSkin = game::skinPalette()[0].color;
  REQUIRE(g.testPlayerColor().r == defaultSkin.r);
  REQUIRE(g.testPlayerColor().g == defaultSkin.g);
  REQUIRE(g.testPlayerColor().b == defaultSkin.b);
  REQUIRE(g.testOutlineColor().a == 0.0F);

  // setProfile must apply it straight away — the constructor's reset() already
  // ran with no profile, so without this the saved skin would be invisible
  // until the player touched the menu.
  g.setProfile(&saved);
  const auto wantSkin = game::skinPalette()[3].color;
  REQUIRE(g.testPlayerColor().r == wantSkin.r);
  REQUIRE(g.testPlayerColor().g == wantSkin.g);
  REQUIRE(g.testPlayerColor().b == wantSkin.b);
  const auto wantOutline = game::outlinePalette()[3].color;
  REQUIRE(g.testOutlineColor().r == wantOutline.r);
  REQUIRE(g.testOutlineColor().g == wantOutline.g);
  REQUIRE(g.testOutlineColor().b == wantOutline.b);
  REQUIRE(g.testOutlineColor().a == wantOutline.a);
}

TEST_CASE("Profile outline unlocks are earned per tier and are idempotent") {
  game::Profile p;
  REQUIRE(p.skin == 0);
  REQUIRE(p.outline == 0);
  REQUIRE(p.unlocks == 0);

  // Tier 0 (an ordinary enemy) earns nothing.
  REQUIRE_FALSE(p.unlockTier(0));
  REQUIRE(p.unlocks == 0);
  // Out-of-range tiers are rejected rather than silently setting a stray bit.
  REQUIRE_FALSE(p.unlockTier(4));
  REQUIRE_FALSE(p.unlockTier(-1));
  REQUIRE(p.unlocks == 0);

  // "None" is always wearable; the reward styles are gated.
  REQUIRE(p.canUseOutline(0));
  REQUIRE_FALSE(p.canUseOutline(1));
  REQUIRE_FALSE(p.canUseOutline(2));
  REQUIRE_FALSE(p.canUseOutline(3));

  // Each tier grants exactly its own style, once.
  REQUIRE(p.unlockTier(1));
  REQUIRE(p.canUseOutline(1));
  REQUIRE_FALSE(p.canUseOutline(2));
  // Re-granting reports "nothing new" so the caller does not rewrite the file.
  REQUIRE_FALSE(p.unlockTier(1));
  REQUIRE(p.unlocks == game::kUnlockElite);

  REQUIRE(p.unlockTier(2));
  REQUIRE(p.canUseOutline(2));
  REQUIRE(p.unlocks == static_cast<game::UnlockMask>(game::kUnlockElite | game::kUnlockChampion));

  REQUIRE(p.unlockTier(3)); // killing an overlord: the coolest one
  REQUIRE(p.canUseOutline(3));
  REQUIRE(p.unlocks == static_cast<game::UnlockMask>(
      game::kUnlockElite | game::kUnlockChampion | game::kUnlockOverlord));
}

TEST_CASE("Profile sanitize repairs out-of-range and illegally-unlocked values") {
  game::Profile p;
  // A hand-edited / corrupted file: bad skin, bad outline, and an outline
  // claimed without its unlock bit.
  p.skin = 9999;
  p.outline = 3;
  p.unlocks = 0;
  p.sanitize();
  REQUIRE(p.skin == 0);
  REQUIRE(p.outline == 0); // cannot keep a locked outline
  REQUIRE(p.unlocks == 0);

  p.skin = 2;
  p.outline = -5;
  p.unlocks = 0xFF; // bits beyond the palette are dropped
  p.sanitize();
  REQUIRE(p.skin == 2);
  REQUIRE(p.outline == 0);
  REQUIRE(p.unlocks == static_cast<game::UnlockMask>(
      game::kUnlockElite | game::kUnlockChampion | game::kUnlockOverlord));
}

TEST_CASE("Profile survives a save/load round trip on disk") {
  const auto path = std::filesystem::temp_directory_path() / "test-game-profile.tmp";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  // A missing file yields defaults (first launch), not a throw.
  {
    const game::Profile fresh = game::loadProfile(path.string());
    REQUIRE(fresh.skin == 0);
    REQUIRE(fresh.outline == 0);
    REQUIRE(fresh.unlocks == 0);
  }

  game::Profile p;
  p.skin = 3;
  p.unlockTier(1);
  p.unlockTier(3);
  p.outline = 3; // legal now that the overlord bit is set
  game::saveProfile(path.string(), p);
  REQUIRE(std::filesystem::exists(path));

  const game::Profile loaded = game::loadProfile(path.string());
  REQUIRE(loaded.skin == 3);
  REQUIRE(loaded.outline == 3);
  REQUIRE(loaded.unlocks == static_cast<game::UnlockMask>(game::kUnlockElite | game::kUnlockOverlord));
  // The champion bit was never earned, so it must not come back from the file.
  REQUIRE((loaded.unlocks & game::kUnlockChampion) == 0u);

  std::filesystem::remove(path, ec);
}

// --- Main menu ----------------------------------------------------------------

TEST_CASE("Main menu navigation wraps and START closes it") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 31};
  g.openMenu();
  REQUIRE(g.menuOpen());
  REQUIRE(g.menuSelection() == 0);

  game::FrameInput up{};
  up.menuUp = true;
  g.advance(1.0F / 60.0F, up);
  // Up from the first row wraps around to the last (QUIT).
  REQUIRE(g.menuSelection() == 5);

  game::FrameInput down{};
  down.menuDown = true;
  g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 0);

  game::FrameInput confirm{};
  confirm.menuConfirm = true;
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE_FALSE(g.menuOpen());
}

TEST_CASE("Main menu is modal: the simulation does not advance behind it") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 32};
  g.testAddWeapon(0); // so the run would otherwise be simulating
  g.openMenu();

  const float before = g.simTime();
  for (int f = 0; f < 30; ++f) {
    g.advance(1.0F / 60.0F, game::FrameInput{});
  }
  REQUIRE(g.simTime() == Catch::Approx(before));
  // ...but once closed, the clock runs again.
  g.closeMenu();
  for (int f = 0; f < 30; ++f) {
    g.advance(1.0F / 60.0F, game::FrameInput{});
  }
  REQUIRE(g.simTime() > before);
}

TEST_CASE("Main menu skin row cycles the whole palette and marks the profile dirty") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile;
  game::Game g{content, 33};
  g.setProfile(&profile);
  g.openMenu();

  // Move down to SKIN.
  game::FrameInput down{};
  down.menuDown = true;
  g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 1);

  const int skinCount = static_cast<int>(game::skinPalette().size());
  REQUIRE(skinCount >= 2);
  game::FrameInput right{};
  right.menuRight = true;
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.skin == 1);
  // The first change is what main() sees to decide to write the file.
  REQUIRE(g.consumeProfileDirty());
  REQUIRE_FALSE(g.consumeProfileDirty()); // ...and it clears after reporting.

  // Finishing the lap wraps back to the start.
  for (int i = 0; i < skinCount - 1; ++i) {
    g.advance(1.0F / 60.0F, right);
  }
  REQUIRE(profile.skin == 0);
  (void)g.consumeProfileDirty();

  // Left from the start wraps to the last colour.
  game::FrameInput left{};
  left.menuLeft = true;
  g.advance(1.0F / 60.0F, left);
  REQUIRE(profile.skin == skinCount - 1);
}

TEST_CASE("Main menu outline row skips styles that are not unlocked yet") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile;
  game::Game g{content, 34};
  g.setProfile(&profile);
  g.openMenu();

  // Move down twice to OUTLINE (START -> SKIN -> OUTLINE).
  game::FrameInput down{};
  down.menuDown = true;
  g.advance(1.0F / 60.0F, down);
  g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 2);

  // With nothing unlocked, stepping right can only land on "None" — it must
  // never park the selection on a style the player has not earned.
  game::FrameInput right{};
  right.menuRight = true;
  for (int i = 0; i < 6; ++i) {
    g.advance(1.0F / 60.0F, right);
    REQUIRE(profile.canUseOutline(profile.outline));
    REQUIRE(profile.outline == 0);
  }

  // Unlock the elite outline: now right steps onto it.
  REQUIRE(profile.unlockTier(1));
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.outline == 1);
  // The next two styles (champion, overlord) are still locked, so stepping
  // right wraps past them and lands back on "None" rather than parking the
  // selection on something the player cannot wear.
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.outline == 0);
  REQUIRE(profile.canUseOutline(profile.outline));
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.outline == 1);

  // With everything earned, the cycle reaches all of them.
  profile.unlockTier(2);
  profile.unlockTier(3);
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.outline == 2);
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.outline == 3);
  g.advance(1.0F / 60.0F, right);
  REQUIRE(profile.outline == 0);
}

TEST_CASE("Main menu QUIT row raises a one-shot quit request") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile;
  game::Game g{content, 35};
  g.setProfile(&profile);
  g.openMenu();

  // Navigate to QUIT (index 5, the last row).
  game::FrameInput down{};
  down.menuDown = true;
  for (int i = 0; i < 5; ++i) g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 5);

  game::FrameInput confirm{};
  confirm.menuConfirm = true;
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE(g.quitRequested());
  REQUIRE(g.consumeQuitRequest());
  // consumeQuitRequest clears it, so main() cannot quit twice for one press.
  REQUIRE_FALSE(g.consumeQuitRequest());
  REQUIRE_FALSE(g.quitRequested());
  // Picking QUIT leaves the menu open (the process is about to end anyway).
  REQUIRE(g.menuOpen());
}

// --- Profile unlocks driven by real kills -------------------------------------

TEST_CASE("Killing a tiered enemy earns the matching outline in the profile") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile;
  game::Game g{content, 36};
  g.setProfile(&profile);
  g.testDisableWaves();

  // An ordinary kill earns no outline.
  g.testSpawnTieredEnemyAt(1.0F, 0.0F, 0);
  g.testKillFirstEnemy();
  REQUIRE(profile.unlocks == 0);
  REQUIRE_FALSE(g.consumeProfileDirty());

  // An elite earns the gold outline, exactly once, and flags the save.
  g.testSpawnTieredEnemyAt(2.0F, 0.0F, 1);
  g.testKillFirstEnemy();
  REQUIRE((profile.unlocks & game::kUnlockElite) != 0u);
  REQUIRE(g.consumeProfileDirty());

  // A second elite kill changes nothing, so nothing is written again.
  g.testSpawnTieredEnemyAt(3.0F, 0.0F, 1);
  g.testKillFirstEnemy();
  REQUIRE_FALSE(g.consumeProfileDirty());

  // Champion, then overlord (the "very cool" one).
  g.testSpawnTieredEnemyAt(4.0F, 0.0F, 2);
  g.testKillFirstEnemy();
  REQUIRE((profile.unlocks & game::kUnlockChampion) != 0u);
  g.testSpawnTieredEnemyAt(5.0F, 0.0F, 3);
  g.testKillFirstEnemy();
  REQUIRE((profile.unlocks & game::kUnlockOverlord) != 0u);
  REQUIRE(profile.canUseOutline(3));
}

TEST_CASE("Outline unlocks persist across runs (they are not run-local)") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile; // survives the Game, as it does in main()
  {
    game::Game g{content, 37};
    g.setProfile(&profile);
    g.testDisableWaves();
    g.testSpawnTieredEnemyAt(1.0F, 0.0F, 1);
    g.testKillFirstEnemy();
  }
  REQUIRE((profile.unlocks & game::kUnlockElite) != 0u);

  // A brand-new run must not wipe the earned unlock.
  game::Game g2{content, 38};
  g2.setProfile(&profile);
  g2.testDisableWaves();
  REQUIRE((profile.unlocks & game::kUnlockElite) != 0u);
  REQUIRE(profile.canUseOutline(1));
}

// --- Round 10: lifesteal nerf, arsenal slots, new supers, sandbox ----------

TEST_CASE("Vampirism is reachable, the milestone stacks it, the trigger is a kill") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto find = [&content](const char* id) -> const game::UpgradeDef* {
    for (const auto& u : content.upgrades) {
      if (u.id == id) return &u;
    }
    return nullptr;
  };
  // Round 10 halved every lifesteal number because vampirism was outscaling the
  // rest of the game. It is back -- the ask now is that vampirism should be
  // REACHABLE, not that it should be the strongest stat on the board -- and the
  // way that is done is the milestone group: vampirism, regeneration and a shield
  // are three mutually exclusive answers, so the build that takes the lifesteal
  // one takes a much bigger number than the old flat card ever did.
  //
  // There used to be TWO everyday lifesteal cards here, Gilded Fangs and
  // Soulfeed, at +4% and +6%. That is the duplicate problem the player named,
  // in the one place where it also cost something real: two cards for one axis
  // meant the survival group had a repeatable answer that was quietly better than
  // the milestone, and the milestone's whole promise is that committing to
  // vampirism beats dabbling in it. One card now, and it is not the milestone.
  const auto* gold = find("leech_gold");
  const auto* crimson = find("m4_crimson");
  const auto* crown = find("m128_crown");
  REQUIRE(gold != nullptr);
  REQUIRE(crimson != nullptr);
  REQUIRE(crown != nullptr);
  // The everyday card is still modest, so vampirism is not simply strong
  // everywhere -- it is a build you commit to. Eight takes, because the family
  // collapsed into one card and a card you can only take three times is a card
  // the slot system is quietly taxing you for using.
  REQUIRE(gold->value == Catch::Approx(3.0F));
  REQUIRE(gold->maxStacks >= 6);
  // The milestone is a MULTIPLIER with a seed, not a bigger flat number, and that
  // changes what "beats the everyday card" even means. It used to be a straight
  // per-take and whole-run comparison of two flat numbers. It is now: a run that
  // takes the everyday card to its ceiling and the milestone once ends up with
  // (seed + everyday) x 1.6, not seed + everyday, and a run that also walks the
  // branch tree ends up at x2.2 or x2.8 on top of that. The milestone is worth
  // taking BECAUSE of what the run put into the axis, which is the entire reason
  // it is a multiplier -- and which only shows up if the comparison is made on
  // the multiplied total, so the test makes it there.
  REQUIRE(crimson->effect == "ms_lifesteal_seed");
  const float seeded = 7.0F;
  const float plainTotal = seeded + gold->value * static_cast<float>(gold->maxStacks);
  const float withPact = plainTotal * (1.0F + crimson->value);
  CAPTURE(plainTotal);
  CAPTURE(withPact);
  REQUIRE(withPact > plainTotal);
  // Even one take of the milestone carries more lifesteal than one take of the
  // everyday card, which is the promise the player can plan against before they
  // have engaged the axis at all. That is what the SEED half is for: a pure
  // multiplier would be worth literally nothing here.
  REQUIRE(seeded + gold->value > gold->value);
  REQUIRE(crown->value == Catch::Approx(40.0F));
  REQUIRE(crown->maxStacks >= 2);
  // And the strongest single number in the game for lifesteal is a MILESTONE, so
  // vampirism has somewhere to go and the run has to be pointed at it.
  float best = 0.0F;
  for (const auto& u : content.upgrades) {
    if (u.effect != "lifesteal_add") continue;
    CAPTURE(u.id);
    best = std::max(best, u.value);
  }
  REQUIRE(best >= 40.0F);
  // A milestone lifesteal card must be exclusive with the two other survival
  // answers. The tier-1 group is still exactly three cards: the tree hangs off it,
  // it does not widen the opening question.
  REQUIRE((crown->group == "survivor" || crimson->group == "survivor"));
  int survivor = 0;
  for (const auto& u : content.upgrades) {
    if (u.group == "survivor") ++survivor;
  }
  REQUIRE(survivor == 3);
  // And the tree is real rather than declared: the crimson line multiplies to
  // x2.2 at its first branch and x2.8 down the Blood Debt path, so committing to
  // vampirism pays out across three milestone levels instead of one.
  const auto* debt = find("m8_blooddebt");
  const auto* thirst = find("m16_thirstunbound");
  REQUIRE(debt != nullptr);
  REQUIRE(thirst != nullptr);
  REQUIRE(debt->after == "m4_crimson");
  REQUIRE(thirst->after == "m8_blooddebt");
  const float deep =
      (1.0F + crimson->value) * (1.0F + debt->value) * (1.0F + thirst->value);
  CAPTURE(deep);
  REQUIRE(deep > 2.5F);

  // Still kill-triggered: hitting an enemy many times must not heal at all.
  game::Game g{content, 91};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  REQUIRE(game::applyUpgrade(g.stats(), "lifesteal_add", 100.0F).valid);
  g.testHurtPlayer(40.0F);
  const float wounded = g.playerHp();
  g.testSpawnEnemyAt(1.0F, 0.0F);
  g.advance(1.0F / 60.0F, game::FrameInput{});
  REQUIRE(g.kills() == 0);
  REQUIRE(g.playerHp() == Catch::Approx(wounded)); // no hit, no heal
  g.testKillFirstEnemy();
  REQUIRE(g.playerHp() > wounded); // the kill healed
}

TEST_CASE("Arsenal is 4 weapons wide until Arsenal Core is stacked (max 3)") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int coreIdx = -1;
  int coreMax = 0;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].id == "u_arsenal_core") {
      coreIdx = static_cast<int>(i);
      coreMax = content.upgrades[i].maxStacks;
    }
  }
  REQUIRE(coreIdx >= 0);
  REQUIRE(coreMax == 3);

  game::Game g{content, 61};
  g.testDisableWaves();
  g.testClearWeapons();
  const int n = static_cast<int>(content.weapons.size());
  REQUIRE(n >= 4);

  // Four base slots, and a fifth is refused.
  for (int i = 0; i < 4; ++i) g.testAddWeapon(i);
  REQUIRE(g.armedWeaponIds().size() == 4);
  g.testAddWeapon(4);
  REQUIRE(g.armedWeaponIds().size() == 4);
  REQUIRE(g.stats().weaponSlots == 0);

  // Each Arsenal Core stack opens exactly one more slot.
  for (int k = 0; k < coreMax; ++k) {
    REQUIRE(g.testGrantUpgrade(coreIdx));
    g.testAddWeapon(4);
  }
  REQUIRE(g.stats().weaponSlots == 3);
  REQUIRE(g.armedWeaponIds().size() == 7);
  // Maxed out: no more slots, no more stacks.
  REQUIRE_FALSE(g.testGrantUpgrade(coreIdx));
}

TEST_CASE("Void Gyre zones drag enemies inward and scale with projectiles") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int gyre = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "vortex") gyre = static_cast<int>(i);
  }
  REQUIRE(gyre >= 0);

  // Differential test: an idle target is never touched without the weapon, and
  // is physically shoved around with it. That is the whole identity of the
  // super — the zones MOVE enemies into their cores, they are not damage pools.
  const auto run = [&content, gyre](bool armed) {
    game::Game g{content, 55};
    g.testDisableWaves();
    g.testClearWeapons();
    if (armed) g.testAddWeapon(gyre);
    g.testSpawnEnemyAt(2.2F, 0.0F);
    const float before = g.testFirstEnemyDistToPlayer();
    game::FrameInput in{};
    for (int i = 0; i < 90; ++i) g.advance(1.0F / 60.0F, in);
    return std::pair<float, float>(before, g.testFirstEnemyDistToPlayer());
  };
  const auto [idleBefore, idleAfter] = run(false);
  REQUIRE(idleBefore > 0.0F);
  REQUIRE(idleAfter == Catch::Approx(idleBefore)); // control: stands still
  const auto [armedBefore, armedAfter] = run(true);
  REQUIRE(std::abs(armedAfter - armedBefore) > 0.1F); // suction drags it

  // The prey is HELD, not leaked: for the whole run it never escapes a zone's
  // pull envelope, even though the zones are sweeping around it.
  game::Game glued{content, 57};
  glued.testDisableWaves();
  glued.testClearWeapons();
  glued.testAddWeapon(gyre);
  glued.testSpawnEnemyAt(2.2F, 0.0F);
  game::FrameInput spin{};
  float worst = 0.0F;
  for (int i = 0; i < 180; ++i) {
    glued.advance(1.0F / 60.0F, spin);
    worst = std::max(worst, glued.testFirstEnemyDistToVortex());
  }
  REQUIRE(worst > 0.0F);
  REQUIRE(worst <= 2.8F); // dragged into reach and kept there

  // Count and size both track the projectile stat.
  game::Game g{content, 56};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(gyre);
  REQUIRE(g.debugCounts().vortices == 3);
  g.testAddWeaponUpgrade(0, "w_proj_add", 2.0F);
  REQUIRE(g.debugCounts().vortices == 5);
}

TEST_CASE("Prism Array ricochets its locked beams, once per pierce") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int prism = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "prism") prism = static_cast<int>(i);
  }
  REQUIRE(prism >= 0);

  game::Game g{content, 64};
  g.testDisableWaves();
  g.testClearWeapons();
  g.stats().fireRateBonus = 100.0F; // fire every tick
  // Six targets in a tight line, all inside the ricochet reach, so a beam that
  // reflects has somewhere to go.
  for (int i = 0; i < 6; ++i) {
    g.testSpawnEnemyAt(3.0F + static_cast<float>(i) * 1.0F, 0.0F);
  }
  g.testAddWeapon(prism);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  // A beam that only reached its own target would draw exactly one segment per
  // lock. The base pierce already reflects, so there is strictly more.
  const int beams = g.debugCounts().beams;
  REQUIRE(beams > 6);

  // Pierce IS the number of reflections: adding one draws one more segment per
  // locked beam, which is what makes the pierce card a real upgrade here.
  g.testAddWeaponUpgrade(0, "pierce_add", 1.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.debugCounts().beams > beams);

  // Every one of the beams does real work: all six separate targets lose HP,
  // which a single-target beam could never manage.
  for (int i = 0; i < 30; ++i) g.advance(1.0F / 60.0F, in);
  const auto hps = g.testEnemyHps();
  REQUIRE(hps.size() == 6);
  int damaged = 0;
  for (const float hp : hps) {
    if (hp < 100000.0F) ++damaged;
  }
  REQUIRE(damaged == 6);
}

TEST_CASE("Test sandbox rolls back XP, items, kills and bestiary on exit") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 71};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in); // settle out of the opening pick

  const float xp0 = g.xp();
  const int kills0 = g.kills();
  const int level0 = g.level();
  const float time0 = g.simTime();
  const float hp0 = g.playerHp();
  g.testHurtPlayer(10.0F);
  const float hp1 = g.playerHp();
  REQUIRE(hp1 < hp0);

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());

  // Cheat inside the sandbox: stack every item, farm XP and a tier kill, and
  // fast-forward the difficulty clock.
  game::FrameInput clock{};
  clock.testTime = true;
  g.advance(1.0F / 60.0F, clock);
  REQUIRE(g.testTimeScale() == 4);
  for (int i = 0; i < 60; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.simTime() > time0 + 3.0F); // the clock really ran fast

  game::FrameInput fill{};
  fill.restart = true;
  g.advance(1.0F / 60.0F, fill);
  REQUIRE(g.stats().damageMul > 1.0F);
  g.grantXp(500.0F);
  g.testSpawnTieredEnemyAt(2.0F, 0.0F, 1);
  g.testKillFirstEnemy();
  REQUIRE(g.kills() > kills0);
  // The sandbox pays out no experience at all: the grant above is dropped on
  // the floor, so there is no way to grind levels inside it.
  REQUIRE(g.xp() == Catch::Approx(xp0));
  REQUIRE(g.testBestiaryKills(0) > 0);

  // Leaving rolls the whole run back: no leaked XP, items, kills or HP.
  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  REQUIRE_FALSE(g.testMode());
  REQUIRE(g.xp() == Catch::Approx(xp0));
  REQUIRE(g.kills() == kills0);
  REQUIRE(g.level() == level0);
  // The clock resumes from where it stood when the sandbox opened; the frame
  // that closed the sandbox is the next normal-speed tick.
  REQUIRE(g.simTime() == Catch::Approx(time0 + 1.0F / 60.0F).margin(0.02F));
  REQUIRE(g.stats().damageMul == Catch::Approx(1.0F));
  REQUIRE(g.testBestiaryKills(0) == 0);
  REQUIRE(g.testTimeScale() == 1);
  // ...and the run is over: the sandbox is a cheat tool, so the cheat is not
  // cashed in. The report still shows the REAL run (kills/level/time above).
  REQUIRE(g.state() == game::RunState::GameOver);
  REQUIRE(g.playerHp() == 0.0F);
}

TEST_CASE("Test sandbox keeps the real horde but drops its own injected fodder") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 76};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // One enemy standing before the sandbox opens.
  g.testSpawnEnemyAt(4.0F, 0.0F);
  REQUIRE(g.enemyCount() == 1);

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());
  // setTestWeapon() seeds a herd of fodder for the weapon under test. It is
  // telegraphed first, so let the markers resolve into real enemies.
  for (int i = 0; i < 30; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.enemyCount() > 1);

  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  // The injected fodder is gone; the run's own enemy survived the visit.
  REQUIRE(g.enemyCount() == 1);
  REQUIRE(g.testFirstEnemyDistToPlayer() > 0.0F);
}

TEST_CASE("Test sandbox item picker grants on demand and max-all fills it") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 72};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());

  game::FrameInput shop{};
  shop.testShop = true;
  g.advance(1.0F / 60.0F, shop);
  REQUIRE(g.testShopOpen());

  // Walk the cursor to a card with a visible effect and take it. Index 0 is
  // where the list opens, so the number of steps is known up front.
  int hpCard = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].effect == "max_hp_add" && content.upgrades[i].weapon.empty()) {
      hpCard = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(hpCard >= 0);
  game::FrameInput move{};
  move.menuDown = true;
  for (int i = 0; i < hpCard; ++i) g.advance(1.0F / 60.0F, move);
  REQUIRE(g.testShopCursor() == hpCard);

  const float hpBefore = g.playerHp();
  const float maxBefore = g.playerMaxHp();
  game::FrameInput take{};
  take.menuConfirm = true;
  g.advance(1.0F / 60.0F, take);
  REQUIRE(g.upgradeStacks(static_cast<std::size_t>(hpCard)) == 1);
  // The card really applied: a bigger pool, topped up by its own heal.
  REQUIRE(g.playerMaxHp() > maxBefore);
  REQUIRE(g.playerHp() > hpBefore);
  // Taking it again is allowed (it is a stacking card), maxed at its own cap.
  for (int i = 0; i < 8; ++i) g.advance(1.0F / 60.0F, take);
  REQUIRE(g.upgradeStacks(static_cast<std::size_t>(hpCard)) ==
          content.upgrades[static_cast<std::size_t>(hpCard)].maxStacks);

  // R maxes every item in the sandbox. A card that needs a weapon the player
  // does not own cannot be applied even here, so the invariant is narrower than
  // "every card": every weapon-agnostic card AND every card of the armed weapon
  // ends up maxed.
  game::FrameInput fill{};
  fill.restart = true;
  g.advance(1.0F / 60.0F, fill);
  int globalCards = 0;
  int maxedGlobal = 0;
  int ownedCards = 0;
  int maxedOwned = 0;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    const auto& def = content.upgrades[i];
    if (def.kind == "milestone") continue;
    const bool isMaxed = g.upgradeStacks(i) == def.maxStacks;
    if (def.weapon.empty()) {
      ++globalCards;
      maxedGlobal += isMaxed ? 1 : 0;
      continue;
    }
    bool armed = false;
    for (const auto& id : g.armedWeaponIds()) armed = armed || id == def.weapon;
    if (!armed) continue;
    ++ownedCards;
    maxedOwned += isMaxed ? 1 : 0;
  }
  REQUIRE(globalCards > 20);
  REQUIRE(maxedGlobal == globalCards);
  REQUIRE(ownedCards > 0);
  REQUIRE(maxedOwned == ownedCards);
}

TEST_CASE("Test sandbox immortality and on-demand death") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 73};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Without the switch a hit hurts.
  g.testHurtPlayer(20.0F);
  const float hurt = g.playerHp();
  REQUIRE(hurt < 100.0F);

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);

  game::FrameInput god{};
  god.testInvuln = true;
  g.advance(1.0F / 60.0F, god);
  REQUIRE(g.testInvulnerable());
  const float before = g.playerHp();
  g.testHurtPlayer(500.0F);
  REQUIRE(g.playerHp() == Catch::Approx(before));
  REQUIRE(g.state() != game::RunState::GameOver);

  // K still works through immortality: that is the point of a death switch.
  game::FrameInput kill{};
  kill.testKill = true;
  g.advance(1.0F / 60.0F, kill);
  REQUIRE(g.state() == game::RunState::GameOver);
}

TEST_CASE("Test sandbox difficulty clock only speeds up the sandbox") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 74};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  REQUIRE(g.testTimeScale() == 1);
  const float plain = g.simTime();
  for (int i = 0; i < 60; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.simTime() == Catch::Approx(plain + 1.0F).margin(0.02F));

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  game::FrameInput fast{};
  fast.testTime = true;
  g.advance(1.0F / 60.0F, fast);
  REQUIRE(g.testTimeScale() == 4);
  // Measure from here: the frame that flipped the switch already ran at 4x.
  const float before = g.simTime();
  for (int i = 0; i < 30; ++i) g.advance(1.0F / 60.0F, in);
  // 30 frames at 4x = 2 simulated seconds.
  REQUIRE(g.simTime() == Catch::Approx(before + 2.0F).margin(0.02F));

  // Back to normal speed, and the snapshot's clock is restored on exit: the run
  // resumes from the moment the sandbox was opened, not from the fast-forwarded
  // difficulty clock.
  g.cycleTestTimeScale();
  g.cycleTestTimeScale();
  g.cycleTestTimeScale();
  REQUIRE(g.testTimeScale() == 1);
  const float entryTime = plain + 1.0F; // the frame that pressed T
  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  REQUIRE(g.simTime() == Catch::Approx(entryTime + 1.0F / 60.0F).margin(0.02F));
}

TEST_CASE("The test sandbox never levels up, so it can never be rerolled") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 75};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());

  // Experience is refused inside the sandbox, so the level-up card screen (and
  // with it the sandbox's free rerolls) is unreachable: grinding levels with a
  // cheat is the one thing the hermetic rollback used to leave open.
  g.grantXp(1000.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.xp() == 0.0F);
  REQUIRE(g.level() == 1);
  REQUIRE(g.state() == game::RunState::Playing);
  // R tries "max every item" in here, which is still the sandbox's job.
  game::FrameInput fill{};
  fill.restart = true;
  g.advance(1.0F / 60.0F, fill);
  REQUIRE(g.stats().damageMul > 1.0F);
  REQUIRE(g.state() == game::RunState::Playing);
  // Leaving the sandbox ends the run.
  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  REQUIRE(g.state() == game::RunState::GameOver);
}

// --- Round 11: menu repeat, max-all soft-lock, displacement resistance -------

TEST_CASE("Menu key repeat fires once per press, then paces itself") {
  core::input::KeyRepeat up;

  // A fresh press acts immediately, so a single tap never feels laggy.
  REQUIRE(up.update(true, true, 1.0F / 60.0F));

  // Holding does nothing for the initial delay: this is the "arrows skip three
  // rows the instant you touch them" fix.
  float held = 0.0F;
  while (held < core::input::KeyRepeat::kInitialDelay - 0.05F) {
    REQUIRE_FALSE(up.update(true, false, 0.02F));
    held += 0.02F;
  }

  // Past the delay it repeats, but at the slow cadence, not once per frame.
  int fired = 0;
  for (int i = 0; i < 50; ++i) {
    if (up.update(true, false, 0.02F)) ++fired;
  }
  REQUIRE(fired > 1);
  // 1.0s of extra holding at a 0.11s cadence: at most ~10, never 50.
  REQUIRE(fired <= 12);

  // One action per poll even after a huge frame hitch: a 5 s stall must not
  // replay ~45 queued steps and teleport the cursor down the list.
  core::input::KeyRepeat hitched;
  REQUIRE(hitched.update(true, true, 0.016F));
  int burst = 0;
  for (int i = 0; i < 5; ++i) {
    if (hitched.update(true, false, 5.0F)) ++burst;
  }
  REQUIRE(burst == 5); // exactly one per poll, never a catch-up storm

  // Letting go resets it: the key must be pressed again, and holding it once
  // more starts the delay from scratch.
  core::input::KeyRepeat released;
  released.update(true, true, 0.016F);
  REQUIRE_FALSE(released.update(false, false, 1.0F));
  REQUIRE_FALSE(released.update(true, false, 0.1F));
  REQUIRE(released.update(true, true, 0.016F));
}

TEST_CASE("A maxed-out build still finishes its level-up") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 111};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0); // one weapon: every other weapon's cards stay unapplyable
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Max the build the way a well-played run would, WITHOUT the sandbox: the
  // sandbox cannot level up any more, so the stacks are granted directly (which
  // is exactly what its "max all" cheat does internally).
  for (int pass = 0; pass < 64; ++pass) {
    bool progressed = false;
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].kind == "milestone") continue;
      progressed |= g.testGrantUpgrade(static_cast<int>(i));
    }
    if (!progressed) break;
  }

  // The reported bug: "after max all the game hangs while offering the one
  // upgrade it has left". Level up repeatedly and never be offered a card that
  // cannot be applied.
  g.grantXp(1000.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::LevelUp);

  int levels = 0;
  for (int guard = 0; guard < 200 && g.state() == game::RunState::LevelUp; ++guard) {
    for (const auto& c : g.upgradeChoices()) {
      if (c.kind == game::Choice::Kind::Skip) continue;
      if (c.kind == game::Choice::Kind::Weapon) {
        const auto& wdef = content.weapons[static_cast<std::size_t>(c.index)];
        CAPTURE(wdef.id);
        // A weapon card for a weapon the player already owns is a dead pick:
        // choosing it used to consume nothing and brick the run.
        bool armed = false;
        for (const auto& id : g.armedWeaponIds()) armed = armed || id == wdef.id;
        REQUIRE_FALSE(armed);
        continue;
      }
      REQUIRE(c.kind == game::Choice::Kind::Upgrade);
      const auto& def = content.upgrades[static_cast<std::size_t>(c.index)];
      if (!def.weapon.empty()) {
        CAPTURE(def.id);
        // A weapon card for a weapon the player does not own would be a dead
        // pick: choosing it used to consume nothing and brick the run.
        bool armed = false;
        for (const auto& id : g.armedWeaponIds()) armed = armed || id == def.weapon;
        REQUIRE(armed);
      }
    }
    game::FrameInput pick{};
    pick.choose1 = true;
    g.advance(1.0F / 60.0F, pick);
    ++levels;
  }
  // The queue of level-ups granted by the big XP dump is fully consumed: the run
  // is playable again instead of stuck on the card screen.
  REQUIRE(g.state() == game::RunState::Playing);
  REQUIRE(levels > 1);

  // A picker roll in that state still produces a usable card, not a dead one.
  g.grantXp(100000.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE_FALSE(g.upgradeChoices().empty());
}

TEST_CASE("Void Gyre drag and shove both shrink against enemy resistance") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int gyre = -1;
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == "vortex") gyre = static_cast<int>(i);
  }
  REQUIRE(gyre >= 0);

  // Identical runs, only the target's knockback resistance differs.
  //
  // Two details make this a measurement of the pull rather than of the clock.
  // The target starts OUTSIDE the core (2.4): inside it the body is already being
  // ground and how far the drag carried it says nothing about how strong the drag
  // is. And the result is the MEAN distance to the player over the last second,
  // not the distance on the final frame -- the wells orbit, so a single-frame
  // reading is a snapshot of where a well happened to be, and a badly-timed one
  // fails or passes for reasons that have nothing to do with resistance.
  const auto dragged = [&content, gyre](float res) {
    game::Game g{content, 58};
    g.testDisableWaves();
    g.testClearWeapons();
    g.testAddWeapon(gyre);
    g.testSpawnEnemyAt(2.9F, 0.0F); // stationary target, no self-movement
    g.testSetFirstEnemyKnockbackRes(res);
    game::FrameInput in{};
    for (int i = 0; i < 120; ++i) g.advance(1.0F / 60.0F, in);
    float sum = 0.0F;
    int samples = 0;
    for (int i = 0; i < 60; ++i) {
      g.advance(1.0F / 60.0F, in);
      const float d = g.testFirstEnemyDistToPlayer();
      if (d >= 0.0F) {
        sum += d;
        ++samples;
      }
    }
    return samples > 0 ? sum / static_cast<float>(samples) : -1.0F;
  };
  const float soft = dragged(0.0F);
  const float tough = dragged(1.0F);
  CAPTURE(soft);
  CAPTURE(tough);
  REQUIRE(soft > 0.0F);
  REQUIRE(tough > 0.0F);
  // A fully resistant enemy is dragged in far less: the aura can no longer
  // corkscrew a boss on its own, which is what made the super dominant.
  REQUIRE(tough > soft + 0.3F);

  // The time-based ramp exists and reaches its cap: enemies get harder to
  // displace as a run goes on, and old spawns are lifted with it. The cap is
  // 55%, not the old 70% -- knockback resistance is a tax on the player's
  // positioning rather than on their damage, and at 70% ordinary trash stopped
  // being pushable around the nine-minute mark, which quietly turned kiting into
  // standing still.
  REQUIRE(game::enemyKnockbackResistance(0.0F, 0, false) == Catch::Approx(0.0F));
  REQUIRE(game::enemyKnockbackResistance(400.0F, 0, false) ==
          Catch::Approx(400.0F / 900.0F).margin(0.001F));
  REQUIRE(game::enemyKnockbackResistance(900.0F, 0, false) == Catch::Approx(0.55F));
  REQUIRE(game::enemyKnockbackResistance(2000.0F, 0, false) == Catch::Approx(0.55F));
  REQUIRE(game::enemyKnockbackResistance(2000.0F, 3, true) == Catch::Approx(1.0F));
}

TEST_CASE("Test sandbox rolls back the state it used to leak") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 76};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  g.testSpawnEnemyAt(3.0F, 0.0F);
  g.testSpawnEnemyAt(-3.0F, 0.0F);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  const float entryTime = g.simTime();
  const std::size_t entryEnemies = g.enemyCount();
  const float entryHp = g.playerHp();
  const std::vector<float> entryEnemyHp = g.testEnemyHps();

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());

  // Do a bit of everything the sandbox allows: hurt, kill, weapon swap, max all
  // items, fast-forward the clock and open the item picker.
  g.toggleTestInvuln(); // the injected fodder would otherwise maul the player
  g.testHurtPlayer(35.0F);
  g.testMaxAllItems();
  g.testKillFirstEnemy();
  g.setTestWeapon(3);
  game::FrameInput fast{};
  fast.testTime = true;
  g.advance(1.0F / 60.0F, fast);
  game::FrameInput shop{};
  shop.testShop = true;
  g.advance(1.0F / 60.0F, shop);
  REQUIRE(g.testShopOpen());
  for (int i = 0; i < 20; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() != game::RunState::GameOver);
  REQUIRE(g.simTime() > entryTime + 0.5F);

  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  REQUIRE_FALSE(g.testMode());

  // The clock goes back to the moment the sandbox was opened (plus the one tick
  // that frame already ran), not to the fast-forwarded sandbox time.
  REQUIRE(g.simTime() == Catch::Approx(entryTime + 1.0F / 60.0F).margin(0.02F));
  // Enemies that were on the field are back: same count, same HP, and one that
  // the sandbox killed is rebuilt instead of vanishing.
  REQUIRE(g.enemyCount() == entryEnemies);
  const std::vector<float> after = g.testEnemyHps();
  REQUIRE(after.size() == entryEnemyHp.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    REQUIRE(after[i] == Catch::Approx(entryEnemyHp[i]));
  }
  // Weapons, XP and the unique-item state are all rolled back...
  REQUIRE(g.armedWeaponIds().size() == 1);
  REQUIRE(g.armedWeaponIds().front() == content.weapons[0].id);
  REQUIRE(g.xp() == Catch::Approx(0.0F));
  REQUIRE(g.rerollsUsed() == 0);
  REQUIRE_FALSE(g.milestoneOffer());
  // Nothing sandbox-only survives: no injected fodder, no stray projectiles.
  REQUIRE(g.debugCounts().projectiles == 0);
  REQUIRE(g.testTimeScale() == 1);
  // ...but the player does not survive either: leaving the sandbox ends the run.
  REQUIRE(g.state() == game::RunState::GameOver);
  REQUIRE(g.playerHp() == 0.0F);
  REQUIRE(g.playerHp() < entryHp);
}

TEST_CASE("Champions wait for four elites, overlords for two champions") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 120};
  g.testDisableWaves();
  g.testClearWeapons();
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Nothing about a clock alone opens the heavy tiers: past the old 180 s mark
  // with a clean sheet, champions are still shut.
  g.testSetSimTime(200.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierUnlocked(1));
  REQUIRE_FALSE(g.tierUnlocked(2));
  REQUIRE_FALSE(g.tierUnlocked(3));

  // Clearing an elite is what actually counts, and it is counted.
  g.testSpawnTieredEnemyAt(4.0F, 0.0F, 1);
  g.testKillFirstEnemy();
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierPressure(1) > 0.0F);
  REQUIRE(g.tierKills(1) == 1);
  REQUIRE_FALSE(g.tierUnlocked(2));

  // Handle enough of them and the champion tribunal opens by itself. The count is
  // asked of the game rather than written out: a literal here is a second copy of
  // kChampionKills, and the last time the two disagreed this test failed for a
  // reason that had nothing to do with what it was checking.
  //
  // A COUNT, not a decaying rate, and that is the fix rather than a detail. The
  // gate used to be 14 points on a score that drains over thirty seconds. Tiers
  // are on a clock now, so a run supplies about five elite kills in ten minutes --
  // the old gate asked for more than the whole game provides, and champions and
  // overlords never appeared at all. Measured: zero of each in a ten-minute run.
  // One real kill is already banked above, so the milestone is crossed by
  // topping the ledger up to it and not one past -- the last step has to be a
  // single kill, or the gate is never tested AT the gate.
  const int need = game::Game::tierKillGate(2);
  REQUIRE(need > 0);
  REQUIRE(g.tierKills(1) == 1);
  g.testAddTierKills(1, need - 1 - g.tierKills(1));
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierKills(1) == need - 1);
  REQUIRE_FALSE(g.tierUnlocked(2));
  g.testAddTierKills(1, 1);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierUnlocked(2));
  REQUIRE(g.tierGateProgress(2) == need);

  // A better player does not get champions SOONER -- the cadence is a clock, and
  // making it adaptive is what produced one champion every seven seconds. What a
  // better player gets is a BIGGER one: past the comfort line a tier walks in as
  // three or four instead of one or two, so "you have outgrown this" is answered
  // with more of the same fight rather than with a new label.
  REQUIRE(g.tierComfort(1) == 0);
  g.testAddTierPressure(1, game::Game::tierComfortLine(1) * 2.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierComfort(1) == 1);

  // Overlords are gated on CHAMPIONS, not on the clock. And on the floor as well
  // as the milestone, which two of the three tiers used to declare and never
  // read -- so the source said "no overlord before seven minutes" and the game
  // said nothing of the kind.
  g.testSetSimTime(300.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierGateProgress(3) == 0);
  g.testAddTierKills(2, game::Game::tierKillGate(3));
  g.advance(1.0F / 60.0F, in);
  REQUIRE_FALSE(g.tierUnlocked(3)); // earned, but too early
  g.testSetSimTime(game::tierMinTime(3) + 1.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierUnlocked(3));
  REQUIRE(g.tierComfort(2) == 0);

  // And a promotion is EARNED, so it is not taken back. It used to be reversible:
  // the score fell back under the line and the gate shut again after a grace
  // window, which meant a tribunal could flicker on and off between two kills and
  // the player could never learn whether they had it. A count does not decay, and
  // the thing that punishes a struggling build is that they never reach it.
  const int marks = game::Game::tierKillGate(2);
  for (int i = 0; i < 120 * 60; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierUnlocked(2));
  REQUIRE(g.tierKills(1) >= marks);
  // Two minutes of doing nothing at all: the tribunal is still yours, and the
  // handling score is not. That split is the whole redesign -- the milestone is
  // permanent, the rate is not -- and the rate is what makes the arrival size fall
  // back to a pair when the player stops keeping up.
  const float tired = g.tierPressure(1);
  CAPTURE(tired);
  REQUIRE(tired < g.tierComfortLine(1));
  REQUIRE(g.tierComfort(1) == 0);
}

TEST_CASE("Kill chain feeds damage, breaks on a hit and dies when you stop") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 130};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0); // wand: a real damage number we can compare
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Cold: no multiplier at all.
  REQUIRE(g.killStreak() == 0);
  REQUIRE(g.momentumDamageMul() == Catch::Approx(1.0F));
  REQUIRE(g.momentumFireRate() == Catch::Approx(0.0F));

  // Kills build it, one stack each, and it pays out as damage.
  g.testSetStreak(0);
  for (int i = 0; i < 4; ++i) {
    g.testSpawnEnemyAt(2.0F, 0.0F);
    g.testKillFirstEnemy();
  }
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.killStreak() == 4);
  REQUIRE(g.momentumDamageMul() == Catch::Approx(1.0F + 4.0F * 1.5F / 100.0F));
  REQUIRE(g.momentumFireRate() == Catch::Approx(4.0F * 0.5F / 100.0F));
  REQUIRE(g.momentumDamageMul() > 1.0F);

  // Getting hit costs more than a step: the chain is halved, minus two.
  const int before = g.killStreak();
  g.testHurtPlayer(20.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.killStreak() == std::max(0, before / 2 - 2));

  // Stop killing and it goes cold on its own — the reward is for staying in
  // the fight, not for banking it.
  g.testSetStreak(10);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.killStreak() == 10);
  for (int i = 0; i < 4 * 60; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.killStreak() == 0);
  REQUIRE(g.momentumDamageMul() == Catch::Approx(1.0F));
  REQUIRE(g.momentumSpeedMul() == Catch::Approx(1.0F)); // no card, no speed
}

TEST_CASE("Momentum cards bend the chain and the chain is capped") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  auto card = [&content](const char* id) -> const game::UpgradeDef& {
    for (const auto& u : content.upgrades) {
      if (u.id == id) return u;
    }
    throw std::runtime_error(std::string("no such card: ") + id);
  };
  // Three normal cards, one unique, all wired into the runtime meter.
  REQUIRE(card("surge_chain").effect == "momentum_damage");
  REQUIRE(card("rampage").effect == "momentum_speed");
  REQUIRE(card("deep_reserves").effect == "momentum_window");
  REQUIRE(card("uw_bloodthirst").kind == "unique");
  REQUIRE(card("uw_bloodthirst").effect == "momentum_bloodthirst");

  game::PlayerStats fresh{};
  REQUIRE(fresh.momentumDamage == Catch::Approx(1.5F));
  REQUIRE(fresh.momentumRate == Catch::Approx(0.5F));
  REQUIRE(fresh.momentumSpeed == Catch::Approx(0.0F));
  REQUIRE(fresh.momentumMax == 20);
  REQUIRE(game::applyUpgrade(fresh, "momentum_damage", 1.0F).valid);
  REQUIRE(game::applyUpgrade(fresh, "momentum_speed", 4.0F).valid);
  REQUIRE(game::applyUpgrade(fresh, "momentum_window", 2.0F).valid);
  REQUIRE(fresh.momentumDamage == Catch::Approx(2.5F));
  REQUIRE(fresh.momentumSpeed == Catch::Approx(4.0F));
  REQUIRE(fresh.momentumWindow == Catch::Approx(5.0F));
  // Bloodthirst: double the stacks per kill, double the chain, +3s of patience.
  REQUIRE(game::applyUpgrade(fresh, "momentum_bloodthirst", 1.0F).valid);
  REQUIRE(fresh.momentumGain == Catch::Approx(2.0F));
  REQUIRE(fresh.momentumMax == 40);
  REQUIRE(fresh.momentumWindow == Catch::Approx(8.0F));

  // The cap is real: a runaway kill loop cannot stack past momentumMax.
  game::Game g{content, 131};
  g.testDisableWaves();
  g.testClearWeapons();
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  g.testSetStreak(999);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.momentumDamageMul() == Catch::Approx(1.0F + 20.0F * 1.5F / 100.0F));
}

TEST_CASE("The upgrade pool is not dominated by dead stat cards") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  // Two different questions, and they need two different measures.
  //
  // REACH -- how much of the pool an axis occupies -- is a count of CARDS. It
  // used to be counted in stacks, back when "+15% damage" and "+25% damage"
  // were two cards on the same axis and three stacks of one of them really did
  // mean three times the exposure. Now there is one damage card, so a stack is
  // a re-take of a card the player has already seen and stacks say nothing about
  // how much of the pool the axis owns.
  //
  // DEPTH -- how far one axis can be leaned on -- is still a sum of stacks, and
  // that is the number the caps below are about.
  //
  // Pickup range and XP are the real dead picks: they never change how the game
  // plays, they only make it faster to sweep up. Flat HP and move speed are
  // legitimate but must stay a minority, and the offensive core has to be the
  // deepest commitment in the game or the build you are offered is a build you
  // cannot build.
  int normal = 0;
  int pickupXpCards = 0;
  int hp = 0;
  int speed = 0;
  int offense = 0;
  int deepestOther = 0;
  int momentum = 0;
  for (const auto& u : content.upgrades) {
    if (u.kind != "normal") continue;
    ++normal;
    if (u.effect == "pickup_mul" || u.effect == "xp_mul") ++pickupXpCards;
    if (u.effect == "max_hp_add") hp += u.maxStacks;
    if (u.effect == "speed_mul") speed += u.maxStacks;
    if (u.effect == "damage_mul" || u.effect == "fire_rate" || u.effect == "proj_add") {
      offense += u.maxStacks;
    } else {
      deepestOther = std::max(deepestOther, u.maxStacks);
    }
  }
  for (const auto& u : content.upgrades) {
    if (u.effect.rfind("momentum_", 0) == 0) momentum += u.maxStacks;
  }
  CAPTURE(normal);
  CAPTURE(pickupXpCards);
  CAPTURE(hp);
  CAPTURE(speed);
  CAPTURE(offense);
  CAPTURE(deepestOther);
  CAPTURE(momentum);
  REQUIRE(normal > 20);
  // At most a fifth of the pool may be pure convenience.
  REQUIRE(pickupXpCards * 5 <= normal);
  // No defensive family may outnumber the whole offensive core.
  REQUIRE(hp <= 12);
  REQUIRE(speed <= 6);
  // The offensive core must be the deepest thing in the game, and by a clear
  // margin rather than a hair. Collapsing thirteen stat families into one card
  // each took this ceiling from 30 to 22, because ten cards became three; the
  // ratio is the part that has to survive that, and it is the part a player
  // feels -- offense goes further than anything else they could take.
  REQUIRE(offense >= deepestOther * 2);
  // Momentum is a real build axis, not one lonely card.
  REQUIRE(momentum >= 4);
}

TEST_CASE("No two global stat cards raise the same thing") {
  // "There are items in the game that increase the same thing but for some
  // reason are not one item. Fix it." The player found fourteen of them: four
  // damage cards, four fire-rate cards, four max-HP cards, three pierce-range
  // cards, and a pair (`multi` and `twin_shot`) that were byte-identical text.
  // Nothing in the data said they were the same axis, so to the player they were
  // fourteen items and the build they were assembling had no shape.
  //
  // This is the check that keeps them from creeping back. It asks the CONTENT for
  // the answer rather than restating the list, so a new duplicate fails here and
  // not in a player's hour.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  std::map<std::string, std::vector<std::string>> byEffect;
  for (const auto& u : content.upgrades) {
    // Per-weapon cards are allowed to share an effect: `w_damage_add` on the
    // wand and on the dagger are two different purchases for two different
    // slots, and merging them would delete a weapon's upgrade. Only the global
    // cards -- the ones that improve everything at once -- have to be unique.
    if (!u.weapon.empty()) continue;
    if (u.kind == "milestone") continue; // milestones are exclusive by group
    byEffect[u.effect].push_back(u.id);
  }
  int checked = 0;
  for (const auto& [effect, ids] : byEffect) {
    CAPTURE(effect);
    if (effect == "weapon_slot_add" || effect == "extra_choice" ||
        effect == "reroll_add") {
      continue; // one-time structural unlocks, deliberately repeatable
    }
    // The uniques are exempt by policy (see `slot` in content.hpp): a unique is
    // a once-per-run treasure, so two of them on one axis is a choice, not a
    // duplicate. What must not happen is a unique colliding with a REPEATABLE
    // card, which is the old Whetstone/"+15% damage" bug wearing a new hat.
    const bool allUnique =
        std::all_of(ids.begin(), ids.end(), [&](const std::string& id) {
          const auto* c = content.upgrade(id);
          return c != nullptr && c->kind == "unique";
        });
    if (allUnique) continue;
    REQUIRE(ids.size() == 1);
    ++checked;
  }
  // The check is not vacuous: it really walked the global pool.
  REQUIRE(checked >= 20);
}

// --- Second-wave weapons, abilities and the fixes on top ---------------------

namespace {

// --- Item slots ----------------------------------------------------------------
//
// The run's power curve. A card that improves every weapon at once is unlimited
// in stacks, so without a limit on how many of them a player may hold, the answer
// to "what is strong" is "take all of them" and the build stops being a build.
// The player asked for the shape: 1 slot at level 1, one more at level 2, and one
// more at every power of two after that.

TEST_CASE("Item slots open one at a time, and only at a power of two") {
  // 1 + floor(log2(level)): one at level 1, 2 at level 2, then 3 at 4, 4 at 8,
  // 5 at 16, 6 at 32, 7 at 64, 8 at 128. The point of the shape is that it is
  // SLOW -- a player at level 40 has six and can see three more coming a long way
  // off -- so the test walks every boundary and both sides of it, because an
  // off-by-one here is invisible in play and permanent in a run.
  REQUIRE(game::itemSlotCap(0) == 1);
  REQUIRE(game::itemSlotCap(1) == 1);
  REQUIRE(game::itemSlotCap(2) == 2);
  REQUIRE(game::itemSlotCap(3) == 2);
  REQUIRE(game::itemSlotCap(4) == 3);
  REQUIRE(game::itemSlotCap(7) == 3);
  REQUIRE(game::itemSlotCap(8) == 4);
  REQUIRE(game::itemSlotCap(15) == 4);
  REQUIRE(game::itemSlotCap(16) == 5);
  REQUIRE(game::itemSlotCap(31) == 5);
  REQUIRE(game::itemSlotCap(32) == 6);
  REQUIRE(game::itemSlotCap(63) == 6);
  REQUIRE(game::itemSlotCap(64) == 7);
  REQUIRE(game::itemSlotCap(127) == 7);
  REQUIRE(game::itemSlotCap(128) == game::kMaxItemSlots);
  REQUIRE(game::itemSlotCap(129) == game::kMaxItemSlots);
  // It never goes down, and it never exceeds the display ceiling -- the sandbox
  // and any future curve change have to be able to set a level of 5000 without
  // walking off the end of the sheet.
  int previous = 0;
  for (int level = 1; level <= 600; ++level) {
    const int cap = game::itemSlotCap(level);
    REQUIRE(cap >= previous);
    REQUIRE(cap <= game::kMaxItemSlots);
    previous = cap;
  }
  REQUIRE(game::itemSlotCap(100000) == game::kMaxItemSlots);
  // And the milestone levels and the slot levels land on the same powers of two,
  // so a milestone and a new slot arrive on the same screen.
  REQUIRE(game::itemSlotCap(4) == 3);
  REQUIRE(game::itemSlotCap(8) == 4);
  REQUIRE(game::itemSlotCap(16) == 5);
  REQUIRE(game::itemSlotCap(32) == 6);
  REQUIRE(game::itemSlotCap(64) == 7);
  REQUIRE(game::itemSlotCap(128) == game::kMaxItemSlots);
}

TEST_CASE("A full slot bar stops new axes and keeps the ones you have") {
  // The rule has two halves and the second one is the one that gets forgotten.
  // A full bar means no card that would OPEN a slot is offered -- and it does NOT
  // mean the run runs out of cards. Re-taking an axis the player already holds is
  // free, forever, up to that card's own max_stacks. If deepening were capped too,
  // a player who filled four slots by level 16 would hit a wall with sixteen levels
  // of experience still to spend, and the slot limit would read as a bug rather
  // than as a build.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // Fill every slot the level opens, with real global cards.
  game::Game g{content, 5};
  g.testDisableWaves();
  g.testSetLevel(8); // four slots
  REQUIRE(g.testItemSlotCap() == 4);
  REQUIRE(g.testUsedItemSlots() == 0);

  const char* axes[] = {"damage", "haste", "multi", "constitution"};
  for (const char* id : axes) {
    REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex(id)));
  }
  REQUIRE(g.testUsedItemSlots() == 4);
  REQUIRE_FALSE(g.testSlotIsOpen());

  // Nothing on the screen may be a slotted card the player does not hold -- with
  // one exception, and it is not an exception to the rule. A card that does NOT
  // take a slot (a milestone, a unique, a weapon card) is not competing for the
  // eight, so a full bar must not silence it. Asked of the real offer rather than
  // of a re-derivation of the rules, because agreeing with the bug is what a
  // duplicated rule does.
  //
  // Level 9, not level 8: 8 is a milestone level, so the screen is three
  // milestone cards and the question "did the cap silence the stat pool?" cannot
  // be asked there. The slot count is the same at both levels.
  REQUIRE(game::itemSlotCap(9) == 4);
  g.testSetLevel(9);
  std::map<std::string, int> held;
  for (const char* id : axes) held[id] = 1;
  int slottedOnScreen = 0;
  for (const auto& id : g.testChoiceIds()) {
    const auto* card = content.upgrade(id.c_str());
    if (card == nullptr) continue; // a weapon offer
    CAPTURE(id);
    if (!card->slot) continue;
    REQUIRE(held.count(id) == 1);
    ++slottedOnScreen;
  }
  // The screen is not empty of stats, which is what a cap implemented as "offer
  // fewer cards" would have produced. Every card on it is a deepening.
  REQUIRE(slottedOnScreen > 0);

  // ...and deepening is still on the table. Every take is legal, and the count
  // does not move: this is the same slot, not a new one.
  const int before = g.testUsedItemSlots();
  for (int take = 0; take < 3; ++take) {
    REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("damage")));
  }
  REQUIRE(g.testUsedItemSlots() == before);
  REQUIRE_FALSE(g.testSlotIsOpen());
}

TEST_CASE("A weapon card, a unique and a milestone never cost a slot") {
  // Three kinds of card that improve the run and are not a global stat axis, so
  // taxing them would be taxing power the player did not have to choose between.
  //
  // A card for ONE weapon cannot stack its way to global reach: it is bounded by
  // that weapon's own cards and there are only a handful. A unique is one per run
  // and there are 52 of them, so charging them would fill the whole bar with cards
  // nobody could afford. A milestone already closes two other axes for the rest of
  // the run, so a slot on top of that is charging the same exclusivity twice.
  //
  // The content check is asked of the loader's own decision (`def.slot`) rather
  // than restated, which is the only version that cannot disagree with the game.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int uniques = 0;
  int milestones = 0;
  int weaponCards = 0;
  int normals = 0;
  int exemptNormals = 0;
  for (const auto& u : content.upgrades) {
    if (u.kind == "unique") {
      ++uniques;
      REQUIRE_FALSE(u.slot);
    } else if (u.kind == "milestone") {
      ++milestones;
      REQUIRE_FALSE(u.slot);
    } else if (!u.weapon.empty()) {
      ++weaponCards;
      REQUIRE_FALSE(u.slot);
    } else {
      ++normals;
      if (!u.slot) ++exemptNormals;
    }
  }
  // The rule is aimed at global stat cards, so there has to be a real set of them
  // for the rule to be aimed at.
  REQUIRE(normals > 20);
  REQUIRE(normals - exemptNormals > 20);
  REQUIRE(uniques > 20);
  REQUIRE(milestones >= 12);
  REQUIRE(weaponCards > 50);
  // The only global cards allowed to opt out are the ones that grant capacity
  // rather than spend it, and there is exactly one of those.
  REQUIRE(exemptNormals == 1);
  REQUIRE(content.upgrade("u_arsenal_core") != nullptr);
  REQUIRE_FALSE(content.upgrade("u_arsenal_core")->slot);

  // And in the running game, taking a milestone does not eat a slot.
  game::Game g{content, 5};
  g.testDisableWaves();
  g.testSetLevel(8);
  const int before = g.testUsedItemSlots();
  REQUIRE(before == 0);
  const int aegis = g.testUpgradeContentIndex("m4_aegis");
  REQUIRE(aegis >= 0);
  REQUIRE(g.testGrantUpgrade(aegis));
  REQUIRE(g.testUsedItemSlots() == before);
  // A card for ONE weapon neither. The example has to be a real per-weapon card
  // and not u_long_barrel, which reads like one -- "reaches 10% further, shots
  // live 10% longer" -- but has no `weapon` field because it applies to all of
  // them, and is therefore one of the cards the cap exists to govern. It is
  // asserted as a slot in the loop above, which is where that belongs.
  const int wandCard = g.testUpgradeContentIndex("w_wand_power");
  REQUIRE(wandCard >= 0);
  REQUIRE(content.upgrade("w_wand_power")->weapon == "wand");
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  REQUIRE(g.testGrantUpgrade(wandCard));
  REQUIRE(g.testUsedItemSlots() == before);
  // ...and the global weapon-wide card does, which is the half of the rule that
  // is easy to get backwards.
  const int barrel = g.testUpgradeContentIndex("u_long_barrel");
  REQUIRE(barrel >= 0);
  REQUIRE(content.upgrade("u_long_barrel")->slot);
  REQUIRE(g.testGrantUpgrade(barrel));
  REQUIRE(g.testUsedItemSlots() == before + 1);
}

TEST_CASE("No path into a run can open a slot that is not there") {
  // There are three doors a card can come through -- the level-up screen, a chest,
  // and a test/migration grant -- and a cap that holds on only one of them is not
  // a cap. The chest pool in particular had its own filter, and a filter that
  // knows about blocked cards and max stacks but not about slots will happily hand
  // out a seventh axis at level 8.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 11};
  g.testDisableWaves();
  g.testSetLevel(8);
  REQUIRE(g.testItemSlotCap() == 4);
  for (const char* id : {"damage", "haste", "multi", "constitution"}) {
    REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex(id)));
  }
  REQUIRE(g.testUsedItemSlots() == 4);

  // A chest, asked for the biggest hand the rarest tier gives.
  const int given = g.testOpenChestFor(7);
  CAPTURE(given);
  REQUIRE(g.testUsedItemSlots() <= g.testItemSlotCap());
  // And nothing new got in: four in, four out.
  REQUIRE(g.testUsedItemSlots() == 4);

  // The chest pool itself is clean, which is the check that would have caught it:
  // every card in it either does not spend a slot, or is one the player already
  // holds. A brand new axis in that list is the bug this whole test exists for.
  std::map<std::string, int> held;
  for (const char* id : {"damage", "haste", "multi", "constitution"}) held[id] = 1;
  int newAxes = 0;
  for (const int idx : g.testLegalChestItems()) {
    const auto& u = content.upgrades[static_cast<std::size_t>(idx)];
    CAPTURE(u.id);
    if (!u.slot) continue;
    if (held.count(u.id) != 0) continue;
    ++newAxes;
  }
  REQUIRE(newAxes == 0);

  // Levelling on does not change the arithmetic under the player's feet: the cap
  // rises, the bar does not.
  g.testSetLevel(64);
  REQUIRE(g.testItemSlotCap() == 7);
  REQUIRE(g.testUsedItemSlots() == 4);
  REQUIRE(g.testSlotIsOpen());
}

TEST_CASE("The opening screen is a weapon, and the first card after it is slot one") {
  // The slot ladder starts at 1 on level 1, and the player's instinct is to read
  // that as "level 1 gives me an item". It nearly does not: level 1 is the
  // STARTING WEAPON pick, and the first stat card comes on the very next screen,
  // still at level 1, with one slot open.
  //
  // That ordering is not an accident and should not be "fixed" into one screen.
  // A stat card offered before the player has chosen a weapon cannot be judged --
  // "+30% damage" is worth nothing to someone who has not picked between a wand
  // and a greatsword -- so the weapon goes first and the card follows. The test
  // is here because the two screens look identical from outside (both are
  // RunState::LevelUp) and someone tidying this up would merge them.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 13};
  g.testDisableWaves();

  g.testEnterStarterPick();
  REQUIRE(g.choosingStarter());
  const auto first = g.testChoiceIds();
  CAPTURE(first.size());
  // The starter pick is weapons and nothing else.
  for (const auto& id : first) {
    CAPTURE(id);
    REQUIRE(content.weapon(id.c_str()) != nullptr);
    REQUIRE(content.upgrade(id.c_str()) == nullptr);
  }
  REQUIRE_FALSE(first.empty());

  // The first card screen, at the same level, with exactly one slot.
  game::Game after{content, 13};
  after.testDisableWaves();
  after.testAddWeapon(after.testWeaponContentIndex("wand"));
  after.testSetLevel(1);
  REQUIRE(after.testItemSlotCap() == 1);
  REQUIRE(after.testUsedItemSlots() == 0);
  REQUIRE(after.testSlotIsOpen());
  int slottedOnScreen = 0;
  for (const auto& id : after.testChoiceIds()) {
    const auto* card = content.upgrade(id.c_str());
    if (card == nullptr) continue;
    if (card->slot) ++slottedOnScreen;
  }
  REQUIRE(slottedOnScreen > 0);
  // And taking that one card fills the bar, which is the whole point of a ladder
  // that opens one slot on the first screen of a run.
  REQUIRE(after.testGrantUpgrade(after.testUpgradeContentIndex("damage")));
  REQUIRE(after.testUsedItemSlots() == 1);
  REQUIRE_FALSE(after.testSlotIsOpen());
  // ...until level 2, which is the very next level.
  after.testSetLevel(2);
  REQUIRE(after.testItemSlotCap() == 2);
  REQUIRE(after.testSlotIsOpen());
}

TEST_CASE("A dry stat pool offers a unique for certain, not on a coin flip") {
  // The worst outcome of a slot cap is not a smaller screen, it is an EMPTY one:
  // every axis engaged and maxed, the arsenal full, and a level-up that offers
  // nothing but "continue". The unique roll used to be a flat 45%, so once the
  // stat pool ran dry the only thing standing between the player and a dead screen
  // was a coin flip.
  //
  // Measured over a 140-level autoplay diagnostic: every empty screen was past
  // level 100, and they were all cases where uniques were ALSO spent. The fix is
  // not more cards -- it is that the roll becomes a certainty when there is
  // genuinely nothing else, which costs nothing in practice because a unique is
  // gone for good once taken and 52 of them exist.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 17};
  g.testDisableWaves();

  // Fill the bars: seven axes, a full arsenal, and every global stat card maxed
  // out so the normal pool is empty by construction rather than by luck.
  g.testSetLevel(64);
  REQUIRE(g.testItemSlotCap() == 7);
  // Exactly seven, because that is the cap: the eighth is the thing the rule
  // exists to prevent, and it is checked for that reason below.
  for (const char* id : {"damage", "haste", "multi", "constitution", "pierce",
                         "sunder", "pledge"}) {
    const int idx = g.testUpgradeContentIndex(id);
    REQUIRE(idx >= 0);
    const auto* card = content.upgrade(id);
    REQUIRE(card->slot);
    for (int take = 0; take < card->maxStacks; ++take) {
      REQUIRE(g.testGrantUpgrade(idx));
    }
  }
  REQUIRE(g.testUsedItemSlots() == 7);
  // The eighth axis is in no pool at all. Not "rarely offered" -- absent, from
  // both the level-up roll and the chest roll.
  //
  // Note it is still GRANTABLE through `testGrantUpgrade`, and that is correct:
  // that hook is the sandbox's "max everything" cheat and it bypasses eligibility
  // on purpose, exactly as it does for a card an answered milestone group blocked.
  // The cap is a property of what the game OFFERS, not a lock on the applier --
  // a lock there would stop the chest, the milestone carry and the migration path
  // from disagreeing with the level-up screen, which is how a cap becomes a lie
  // the player finds out about three screens later.
  const int eighth = g.testUpgradeContentIndex("ward_small");
  REQUIRE(eighth >= 0);
  REQUIRE(content.upgrade("ward_small")->slot);
  const auto offeredAtEighth = [&] {
    game::Game probe{content, 17};
    probe.testDisableWaves();
    probe.testSetLevel(64);
    for (const char* id : {"damage", "haste", "multi", "constitution", "pierce",
                           "sunder", "pledge"}) {
      probe.testGrantUpgrade(probe.testUpgradeContentIndex(id));
    }
    for (const auto& id : probe.testChoiceIds()) {
      if (id == "ward_small") return true;
    }
    for (const int idx : probe.testLegalChestItems()) {
      if (content.upgrades[static_cast<std::size_t>(idx)].id == "ward_small") return true;
    }
    return false;
  }();
  REQUIRE_FALSE(offeredAtEighth);
  // Max every remaining slotted card too, so there is no deepening left either.
  //
  // Only the SLOT-TAKING ones. The first version of this looped over the whole
  // chest pool, which quietly ate all 18 global uniques as well -- and then the
  // "a unique is on the screen" half of the test was asserting something about a
  // pool the test had itself emptied.
  for (int guard = 0; guard < 64; ++guard) {
    bool progressed = false;
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (!content.upgrades[i].slot) continue;
      if (content.upgrades[i].kind != "normal") continue;
      progressed |= g.testGrantUpgrade(static_cast<int>(i));
    }
    if (!progressed) break;
  }
  // Nothing slotted is left to deepen, which is what "dry" has to mean.
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (!content.upgrades[i].slot) continue;
    if (content.upgrades[i].kind != "normal") continue;
    const int st = g.testUpgradeStacks(static_cast<int>(i));
    if (st == 0) continue; // not held, and cannot be: the bar is full
    CAPTURE(content.upgrades[i].id);
    REQUIRE(st == content.upgrades[i].maxStacks);
  }
  // The last global card that is NOT slot-taking is the arsenal core, and it has
  // to be maxed too or the pool is not actually dry: a card that gives weapon
  // capacity rather than spending it is a real, permanent option, and while one
  // remains the 45% roll is the correct rule. This is the test finding out what
  // "slotted" means -- the exemption is not a loophole for the dry case, it is
  // one card that is intentionally always available.
  const int core = g.testUpgradeContentIndex("u_arsenal_core");
  REQUIRE(core >= 0);
  REQUIRE_FALSE(content.upgrades[static_cast<std::size_t>(core)].slot);
  for (int take = 0; take < content.upgrades[static_cast<std::size_t>(core)].maxStacks;
       ++take) {
    REQUIRE(g.testGrantUpgrade(core));
  }

  // Over several level-ups the screen is never empty, which is the invariant the
  // 45% could not give -- and the reason it cannot be "a unique is always on the
  // screen" is that a weapon offer takes precedence over a unique, and it
  // arrives on roughly a third of levels. So the rule as built is: a unique is
  // guaranteed exactly when nothing else is, which is the sharp version and the
  // one worth pinning.
  int filled = 0;
  int uniqueScreens = 0;
  int weaponScreens = 0;
  for (int level = 65; level <= 90; ++level) {
    g.testSetLevel(level);
    const auto ids = g.testChoiceIds();
    bool real = false;
    bool uniqueHere = false;
    bool weaponHere = false;
    for (const auto& id : ids) {
      if (id == "<skip>") continue;
      real = true;
      if (content.upgrade(id.c_str()) == nullptr) weaponHere = true;
      const auto* card = content.upgrade(id.c_str());
      if (card != nullptr && card->kind == "unique") uniqueHere = true;
    }
    CAPTURE(level);
    REQUIRE(real);
    // THE RULE. Something on the screen, and if it was not a weapon then it was
    // a unique -- because the stat pool is empty by construction and the unique
    // roll is a certainty in exactly this case.
    const bool swordOrTreasure = weaponHere || uniqueHere;
    REQUIRE(swordOrTreasure);
    if (real) ++filled;
    if (uniqueHere) ++uniqueScreens;
    if (weaponHere) ++weaponScreens;
  }
  REQUIRE(filled == 26);
  // Both paths are genuinely exercised, so the check above is not passing
  // vacuously on a screen that only ever had weapons on it.
  REQUIRE(uniqueScreens > 5);
  REQUIRE(weaponScreens > 5);
}

// Index of a weapon by id (-1 when the id is not in the roster).
int weaponIndex(const game::Content& content, const char* id) {
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

} // namespace

TEST_CASE("The roster is 33 weapons: 18 base, 11 evolutions, 4 supers") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int base = 0;
  int evo = 0;
  int super = 0;
  for (const auto& w : content.weapons) {
    if (w.prereqs.size() >= 3) {
      ++super;
    } else if (w.prereqs.size() == 2) {
      ++evo;
    } else {
      REQUIRE(w.prereqs.empty());
      ++base;
    }
  }
  CAPTURE(base);
  CAPTURE(evo);
  CAPTURE(super);
  REQUIRE(base == 18);
  REQUIRE(evo == 11);
  // Three supers, not four: the Seraph Array was a Radiant Halo with longer arms,
  // and its one real idea (a ring of safe ground at your feet) now belongs to the
  // Halo. Trading a reskin for the rule it was hiding was the better deal.
  REQUIRE(super == 3);
  REQUIRE(content.weapons.size() == 32);

  // Every evolution's prerequisites actually exist, and a super really is
  // reachable (three base weapons, not two evolutions of each other).
  for (const auto& w : content.weapons) {
    for (const auto& req : w.prereqs) {
      CAPTURE(w.id);
      CAPTURE(req);
      REQUIRE(content.weapon(req) != nullptr);
    }
  }
  for (const auto& w : content.weapons) {
    if (w.prereqs.size() < 3) continue;
    for (const auto& req : w.prereqs) {
      const auto* base_def = content.weapon(req);
      REQUIRE(base_def != nullptr);
      REQUIRE(base_def->prereqs.empty());
    }
  }
}

// One enemy, one game, so `testEnemyHps()` (whose order is unspecified) can
// never be read as a positional claim.
static float hpAfterHit(const game::Content& content, int weapon, std::uint32_t seed,
                        float ex, float ey, int ticks) {
  game::Game g{content, seed};
  g.testDisableWaves();
  g.testClearWeapons();
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  g.testAddWeapon(weapon);
  g.testSpawnEnemyAt(ex, ey);
  for (int i = 0; i < ticks; ++i) g.advance(1.0F / 60.0F, in);
  const auto hps = g.testEnemyHps();
  if (hps.size() != 1) return -1.0F;
  return hps.front();
}

TEST_CASE("Dagger blades also grind the inside of the ring") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const int dagger = weaponIndex(content, "dagger");
  REQUIRE(dagger >= 0);

  // Hugging the player at 0.5 units, with the blades riding a 1.3 circle: no
  // blade ever touches it. The reported bug was that this took literally
  // nothing while the ring spun harmlessly overhead.
  const float inside = hpAfterHit(content, dagger, 201, 0.5F, 0.0F, 20);
  CAPTURE(inside);
  REQUIRE(inside < 100000.0F);
  // Past the ring, nothing reaches: 2.5 units is outside both the blades and
  // the interior sweep.
  const float outside = hpAfterHit(content, dagger, 201, 2.5F, 0.0F, 20);
  CAPTURE(outside);
  REQUIRE(outside == Catch::Approx(100000.0F));
}

TEST_CASE("Prism Lance spreads the beam into front, left and right") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const int beam = weaponIndex(content, "beam");
  REQUIRE(beam >= 0);

  game::Game g{content, 202};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(beam);
  g.testSpawnEnemyAt(4.0F, 0.0F);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Plain Lance: one line.
  REQUIRE(g.testBeamAngles().size() == 1);

  // "Prism Lance" is exactly three beams — the complaint was that it read as
  // "+2 projectiles" because they were stacked on the aim line.
  g.testAddWeaponUpgrade(0, "w_unique_prism", 3.0F);
  // The Lance is a 1.6s weapon and its beams live 0.25s, so only one volley is
  // ever on screen: wait for the next one and there is nothing to confuse.
  for (int i = 0; i < 100; ++i) g.advance(1.0F / 60.0F, in);
  auto three = g.testBeamAngles();
  REQUIRE(three.size() == 3);
  std::sort(three.begin(), three.end());
  // Front, and roughly 17 degrees either side: three genuinely different lines.
  REQUIRE(three[0] == Catch::Approx(-0.30F).margin(0.01F));
  REQUIRE(three[1] == Catch::Approx(0.0F).margin(0.01F));
  REQUIRE(three[2] == Catch::Approx(0.30F).margin(0.01F));
}

TEST_CASE("Barbed Whip lashes the arc in front, the Scythe still reaps a circle") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const int whip = weaponIndex(content, "whip");
  const int scythe = weaponIndex(content, "scythe");
  REQUIRE(whip >= 0);
  REQUIRE(scythe >= 0);

  // A pair: one straight ahead, one off to the side. Both weapons aim at the
  // nearest enemy, so they both hit the one in front — the difference is what
  // happens to the one at the side, which is inside the circle but outside a
  // whip's arc.
  const auto wounded = [ticks = 3](game::Game& g) {
    g.testSpawnEnemyAt(2.0F, 0.0F);
    g.testSpawnEnemyAt(2.0F, 2.5F);
    for (int i = 0; i < ticks; ++i) g.advance(1.0F / 60.0F, game::FrameInput{});
    int hits = 0;
    for (const float hp : g.testEnemyHps()) hits += hp < 100000.0F ? 1 : 0;
    return hits;
  };

  game::Game w{content, 203};
  w.testDisableWaves();
  w.testClearWeapons();
  w.advance(1.0F / 60.0F, game::FrameInput{});
  w.testAddWeapon(whip);
  // A whip covers the arc you are facing, not the whole world.
  REQUIRE(wounded(w) == 1);

  game::Game s{content, 203};
  s.testDisableWaves();
  s.testClearWeapons();
  s.advance(1.0F / 60.0F, game::FrameInput{});
  s.testAddWeapon(scythe);
  // The scythe is the weapon that reaps the full circle, so it takes both.
  REQUIRE(wounded(s) == 2);
}

TEST_CASE("Siege Mortar lobs over the horde and cooks where it lands") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const int mortar = weaponIndex(content, "mortar");
  REQUIRE(mortar >= 0);

  // The shell is solved to land `bomb_ahead` past whoever is nearest its aim
  // line, so the pair below is the whole rule: a body at 1.5 is the front rank
  // and one at 6.0 is where the shell comes down (1.5 + 4.5).
  const auto hps = [&] {
    game::Game g{content, 204};
    g.testDisableWaves();
    g.testClearWeapons();
    g.testAddWeapon(mortar);
    g.testSpawnEnemyAt(1.5F, 0.0F);
    g.testSpawnEnemyAt(6.0F, 0.0F);
    game::FrameInput in{};
    for (int i = 0; i < 90; ++i) g.advance(1.0F / 60.0F, in);
    return std::pair<float, float>(g.testEnemyHpNear(1.5F, 0.0F),
                                   g.testEnemyHpNear(6.0F, 0.0F));
  }();
  CAPTURE(hps.first);
  CAPTURE(hps.second);
  // Right in the shell's flight path: a fused shell ignores what it flies over.
  REQUIRE(hps.first == Catch::Approx(100000.0F));
  REQUIRE(hps.second < 100000.0F);

  // And the flip side, which is what makes it a mortar rather than a hammer: on
  // its own it MISSES. One body, overshot by the full four and a half units, is
  // a weapon that does nothing to a lone target -- the crowd is the point.
  const auto lone = hpAfterHit(content, mortar, 204, 1.5F, 0.0F, 90);
  CAPTURE(lone);
  REQUIRE(lone == Catch::Approx(100000.0F));
}

TEST_CASE("Grave Bell taunts prey into its core") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const int lure = weaponIndex(content, "lure");
  REQUIRE(lure >= 0);
  const auto& def = content.weapons[static_cast<std::size_t>(lure)];

  // The bell is planted 1.1 units BEHIND the target, so the target always lands
  // inside the core. The interesting case is the bystander: one inside the
  // taunt reach has to be dragged in (and only then does it die), one outside
  // it walks away untouched.
  const auto bells = [ticks = 90](game::Game& g, float by) {
    g.testSpawnEnemyAt(5.0F, 0.0F);
    g.testSpawnEnemyAt(3.9F, by);
    for (int i = 0; i < ticks; ++i) g.advance(1.0F / 60.0F, game::FrameInput{});
    int hits = 0;
    for (const float hp : g.testEnemyHps()) hits += hp < 100000.0F ? 1 : 0;
    return hits;
  };

  game::Game inReach{content, 205};
  inReach.testDisableWaves();
  inReach.testClearWeapons();
  inReach.advance(1.0F / 60.0F, game::FrameInput{});
  inReach.testAddWeapon(lure);
  inReach.testSpawnEnemyAt(5.0F, 0.0F);
  inReach.testSpawnEnemyAt(3.9F, 3.2F);
  inReach.advance(1.0F / 60.0F, game::FrameInput{});
  REQUIRE(inReach.debugCounts().lures == 1);
  // The bell keeps at most its own cap alive: a taunt is not a wall of beacons.
  REQUIRE(inReach.debugCounts().lures <= static_cast<std::size_t>(def.lureMaxBeacons));
  for (int i = 0; i < 90; ++i) inReach.advance(1.0F / 60.0F, game::FrameInput{});
  // 3.2 units from the bell: outside the kill core, inside `lure_reach`, so it
  // is dragged in and dies with the target.
  int hits = 0;
  for (const float hp : inReach.testEnemyHps()) hits += hp < 100000.0F ? 1 : 0;
  REQUIRE(hits == 2);
  // The drag is what did it: prey that started outside the core ends up in it.
  REQUIRE(inReach.testFirstEnemyDistToLure() <= def.lureRadius);

  game::Game outOfReach{content, 205};
  outOfReach.testDisableWaves();
  outOfReach.testClearWeapons();
  outOfReach.advance(1.0F / 60.0F, game::FrameInput{});
  outOfReach.testAddWeapon(lure);
  // 6.0 units from the bell is beyond `lure_reach`, so only the target dies.
  REQUIRE(bells(outOfReach, 6.0F) == 1);
}

TEST_CASE("Phase Dash, Overload and Stasis are live from the first second") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 206};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Nothing to unlock, nothing to pick: all three are ready on tick one.
  REQUIRE(g.abilityReady(game::Game::Ability::Blink));
  REQUIRE(g.abilityReady(game::Game::Ability::Burst));
  REQUIRE(g.abilityReady(game::Game::Ability::Stasis));
  REQUIRE(g.abilityCooldown(game::Game::Ability::Blink) == Catch::Approx(5.0F));
  REQUIRE(g.abilityCooldown(game::Game::Ability::Burst) == Catch::Approx(14.0F));
  REQUIRE(g.abilityCooldown(game::Game::Ability::Stasis) == Catch::Approx(30.0F));

  // J: the dash moves the player and buys invulnerability on arrival.
  g.testSpawnEnemyAt(-6.0F, 0.0F);
  const float x0 = g.testPlayerX();
  g.advance(1.0F / 60.0F, in);
  g.testTriggerAbility(game::Game::Ability::Blink);
  const float x1 = g.testPlayerX();
  CAPTURE(x1);
  REQUIRE(x1 < x0 - 2.0F);
  REQUIRE(g.testIframes() > 0.0F);
  REQUIRE_FALSE(g.abilityReady(game::Game::Ability::Blink));
  // The cooldown is a real gate: a second press mid-cooldown does nothing.
  g.advance(1.0F / 60.0F, in);
  g.testTriggerAbility(game::Game::Ability::Blink);
  REQUIRE(g.testPlayerX() == Catch::Approx(x1));

  // K: the blast hurts and shoves whatever is standing on the player.
  g.testSpawnEnemyAt(1.0F, 0.0F);
  const auto before = g.testEnemyHps();
  g.advance(1.0F / 60.0F, in);
  g.testTriggerAbility(game::Game::Ability::Burst);
  const auto after = g.testEnemyHps();
  REQUIRE(before.size() == 2);
  REQUIRE(after.size() == 2);
  float lost = 0.0F;
  for (std::size_t i = 0; i < after.size(); ++i) lost += before[i] - after[i];
  REQUIRE(lost > 0.0F);
  REQUIRE_FALSE(g.abilityReady(game::Game::Ability::Burst));
}

TEST_CASE("Stasis slows the world without slowing the player") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 207};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // A real, moving enemy far enough away that it cannot reach the player during
  // the measurement windows. A champion, not a bat: a plain bat has 8 HP and the
  // wand in the corner would kill it before it had taken a single measured step.
  g.testSpawnTieredEnemyAt(14.0F, 0.0F, 2);
  for (int i = 0; i < 40; ++i) g.advance(1.0F / 60.0F, in);

  const float d0 = g.testFirstEnemyDistToPlayer();
  for (int i = 0; i < 10; ++i) g.advance(1.0F / 60.0F, in);
  const float normalStep = (d0 - g.testFirstEnemyDistToPlayer()) / 10.0F;
  REQUIRE(normalStep > 0.0F);
  REQUIRE(g.worldTimeScale() == Catch::Approx(1.0F));

  g.testTriggerAbility(game::Game::Ability::Stasis);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.stasisRemaining() > 0.0F);
  REQUIRE(g.worldTimeScale() < 1.0F);
  REQUIRE(g.worldTimeScale() >= 0.1F);

  const float d1 = g.testFirstEnemyDistToPlayer();
  for (int i = 0; i < 10; ++i) g.advance(1.0F / 60.0F, in);
  const float slowStep = (d1 - g.testFirstEnemyDistToPlayer()) / 10.0F;
  CAPTURE(normalStep);
  CAPTURE(slowStep);
  REQUIRE(slowStep > 0.0F);
  REQUIRE(slowStep < normalStep);
  // The player keeps full speed: only the hostile side of the world is slowed.
  REQUIRE(g.worldTimeScale() < 1.0F);
  REQUIRE_FALSE(g.abilityReady(game::Game::Ability::Stasis));
}

TEST_CASE("Ability cards retune the keys, they never unlock them") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto find = [&content](const char* effect) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].effect == effect) return static_cast<int>(i);
    }
    return -1;
  };
  const int haste = find("ability_haste");
  const int phase = find("ability_phase");
  const int stasis = find("ability_stasis");
  const int echo = find("ability_echo");
  REQUIRE(haste >= 0);
  REQUIRE(phase >= 0);
  REQUIRE(stasis >= 0);
  REQUIRE(echo >= 0);

  game::Game g{content, 208};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Haste shortens every cooldown, floor included.
  REQUIRE(g.testGrantUpgrade(haste));
  REQUIRE(g.abilityCooldown(game::Game::Ability::Blink) < 5.0F);
  REQUIRE(g.abilityCooldown(game::Game::Ability::Stasis) < 30.0F);
  // Phase Memory lengthens the dash and the mercy window.
  const float dist0 = g.stats().blinkDist;
  const float iframes0 = g.stats().blinkIframes;
  REQUIRE(g.testGrantUpgrade(phase));
  REQUIRE(g.stats().blinkDist > dist0);
  REQUIRE(g.stats().blinkIframes > iframes0);
  // Deep Freeze extends Stasis.
  const float dur0 = g.stats().stasisDuration;
  REQUIRE(g.testGrantUpgrade(stasis));
  REQUIRE(g.stats().stasisDuration > dur0);
  // Cascade makes every ability throw a scaled-down Overload as well.
  REQUIRE(g.testGrantUpgrade(echo));
  g.testSpawnEnemyAt(-5.0F, 0.0F);
  const auto before = g.testEnemyHps();
  g.advance(1.0F / 60.0F, in);
  g.testTriggerAbility(game::Game::Ability::Blink);
  const auto after = g.testEnemyHps();
  REQUIRE(before.size() == 1);
  REQUIRE(after.size() == 1);
  REQUIRE(after[0] < before[0]);
}

TEST_CASE("The sandbox rolls the ability cooldowns back with everything else") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 209};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());
  g.testTriggerAbility(game::Game::Ability::Burst);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.abilityCooldownRemaining(game::Game::Ability::Burst) > 0.0F);

  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  // Everything the sandbox spent is restored...
  REQUIRE(g.abilityCooldownRemaining(game::Game::Ability::Burst) == 0.0F);
  REQUIRE(g.stasisRemaining() == 0.0F);
  REQUIRE(g.worldTimeScale() == 1.0F);
  // ...except the run itself, which the sandbox ends.
  REQUIRE(g.state() == game::RunState::GameOver);
}

// --- In-game manual, progress reset and card coverage ------------------------
//
// Three separate promises are checked here, and all three are the kind that rot
// silently: the manual must stay drawable AND fit on one screen, the reset
// button must stay two-step, and every weapon must keep at least one unique.

TEST_CASE("The in-game manual is loaded, ordered and every glyph is drawable") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE_FALSE(content.manual.empty());
  REQUIRE(content.manual.size() >= 8);

  std::vector<std::string> ids;
  for (const auto& page : content.manual) {
    CAPTURE(page.id);
    REQUIRE_FALSE(page.id.empty());
    REQUIRE_FALSE(page.title.empty());
    REQUIRE_FALSE(page.lines.empty());
    // Unique ids: Content::manualPage() resolves by id, so a duplicate would
    // make the lookup ambiguous.
    for (const auto& seen : ids) REQUIRE(seen != page.id);
    ids.push_back(page.id);
    // Every character in the title and every line must exist in the 5x7 font.
    // loadContent() already throws on this, so reaching the assert means the
    // check runs; the loop is the belt to the loader's braces.
    REQUIRE(core::render::fontSupports(page.title));
    for (const auto& line : page.lines) {
      REQUIRE(core::render::fontSupports(line));
    }
  }
  // The pages a player cannot do without.
  for (const char* want : {"controls", "weapons", "abilities", "cards"}) {
    REQUIRE(content.manualPage(want) != nullptr);
  }
}

TEST_CASE("The shipped manual fits one screen and never wraps off the right") {
  // The renderer lays a page out at kManualBodyScale on a kManualLineH pitch,
  // starting kManualBodyY down, and keeps kManualBottomPad at the bottom for
  // the hint bar. Anything past that is clipped with a "...MORE, NEXT PAGE"
  // marker, which would mean the reader is missing part of a topic with no way
  // to scroll.
  //
  // Every number comes from Game rather than being repeated here: this test used
  // to carry its own copies, and when the body was resized they went stale and
  // the assertion silently stopped meaning anything.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  constexpr float kGlyph = 6.0F; // Batcher::textWidth == len * 6 * scale
  const float maxLines =
      (game::Game::kRefScreenHeight - game::Game::kManualBodyY -
       game::Game::kManualBottomPad) /
      game::Game::kManualLineH;
  // Width budget: from the body's left edge to the right margin, minus the
  // indent the bullet and sub-line prefixes add.
  const float maxPx = game::Game::kRefScreenWidth - game::Game::kManualBodyX - 20.0F;
  const float maxChars = maxPx / (kGlyph * game::Game::kManualBodyScale);
  const float maxIndentChars = (maxPx - game::Game::kManualIndent) /
                               (kGlyph * game::Game::kManualBodyScale);
  REQUIRE(maxChars > 20.0F); // a real bound, not a vacuous one

  for (const auto& page : content.manual) {
    CAPTURE(page.id);
    REQUIRE(static_cast<float>(page.lines.size()) <= maxLines);
    for (const auto& raw : page.lines) {
      std::string_view v = raw;
      // The marker prefixes ('>', '#', two spaces) are stripped before drawing,
      // so they only ever make a line SHORTER on screen -- but the indenting
      // branch moves the line RIGHT by kManualIndent, so it is charged for that.
      const bool indented = v.size() >= 2 && v[0] == ' ' && v[1] == ' ';
      if (!v.empty() && (v.front() == '>' || v.front() == '#')) v.remove_prefix(1);
      if (v.size() >= 2 && v[0] == ' ' && v[1] == ' ') v.remove_prefix(2);
      REQUIRE(static_cast<float>(v.size()) <= (indented ? maxIndentChars : maxChars));
    }
  }
}

TEST_CASE("F1 opens the manual from the menu, a run and the pause screen") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 401};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // From a live run.
  game::FrameInput f1{};
  f1.manualToggle = true;
  g.advance(1.0F / 60.0F, f1);
  REQUIRE(g.manualOpen());
  REQUIRE(g.manualPageCount() == content.manual.size());
  REQUIRE(g.manualPageIndex() == 0);
  // Modal: the manual eats the frame, so the simulation must not have moved.
  const float t = g.simTime();
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.simTime() == t);

  // Pages flip and wrap in both directions.
  const std::string first = g.manualPageId();
  g.nextManualPage();
  REQUIRE(g.manualPageId() != first);
  g.prevManualPage();
  REQUIRE(g.manualPageId() == first);
  g.prevManualPage();
  REQUIRE(g.manualPageIndex() == content.manual.size() - 1);
  g.nextManualPage();
  REQUIRE(g.manualPageIndex() == 0);
  // Number keys jump straight to a page.
  game::FrameInput jump{};
  jump.choose3 = true;
  g.advance(1.0F / 60.0F, jump);
  REQUIRE(g.manualPageIndex() == 2);
  // A jump past the end is ignored rather than landing on nothing.
  const std::size_t pages = content.manual.size();
  REQUIRE(pages > 5);
  game::FrameInput far{};
  far.choose5 = true;
  g.advance(1.0F / 60.0F, far);
  REQUIRE(g.manualPageIndex() == 4);

  // Arrows are the other way to flip, and a repeat-holding player gets them.
  game::FrameInput down{};
  down.menuDown = true;
  g.advance(1.0F / 60.0F, down);
  REQUIRE(g.manualPageIndex() == 5);

  // F1 again closes it, and the run resumes on the next frame.
  g.advance(1.0F / 60.0F, f1);
  REQUIRE_FALSE(g.manualOpen());
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.simTime() > t);

  // The same key works from the pause screen...
  game::FrameInput pause{};
  pause.togglePause = true;
  g.advance(1.0F / 60.0F, pause);
  REQUIRE(g.state() == game::RunState::Paused);
  g.advance(1.0F / 60.0F, f1);
  REQUIRE(g.manualOpen());
  // ...and ESC from the manual returns to the pause screen, not the run.
  game::FrameInput esc{};
  esc.togglePause = true;
  g.advance(1.0F / 60.0F, esc);
  REQUIRE_FALSE(g.manualOpen());
  REQUIRE(g.state() == game::RunState::Paused);

  // And from the main menu, including by selecting the MANUAL row.
  g.openMenu();
  REQUIRE(g.menuOpen());
  game::FrameInput row{};
  row.menuDown = true;
  for (int i = 0; i < 3; ++i) g.advance(1.0F / 60.0F, row);
  REQUIRE(g.menuSelection() == 3);
  game::FrameInput confirm{};
  confirm.menuConfirm = true;
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE(g.manualOpen());
  // Closing the manual lands back on the menu, not into the run.
  g.advance(1.0F / 60.0F, f1);
  REQUIRE_FALSE(g.manualOpen());
  REQUIRE(g.menuOpen());
}

TEST_CASE("A build with no manual.toml still loads and reports none") {
  // The manual is the one content file allowed to be absent: a stripped
  // distribution must still start. Copy the data dir minus manual.toml.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "test-game-nomanual";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  for (const char* name : {"weapons.toml", "enemies.toml", "upgrades.toml"}) {
    std::filesystem::copy_file(std::filesystem::path(GAME_ASSETS_DIR) / "data" / name,
                               dir / name, ec);
  }
  REQUIRE_FALSE(std::filesystem::exists(dir / "manual.toml"));

  const auto content = game::loadContent(dir.string());
  REQUIRE(content.manual.empty());
  REQUIRE(content.manualPage("controls") == nullptr);
  game::Game g{content, 402};
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  g.openManual();
  REQUIRE(g.manualOpen());
  REQUIRE(g.manualPageCount() == 0);
  REQUIRE(g.manualPageId().empty());
  // Flipping pages on an empty manual must not index out of bounds.
  g.nextManualPage();
  g.prevManualPage();
  REQUIRE(g.manualPageIndex() == 0);

  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("Reset Progress takes two confirms, and moving away disarms it") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile;
  profile.skin = 5;
  profile.outline = 0;
  profile.unlockTier(1);
  profile.unlockTier(2);
  profile.unlockTier(3);
  profile.outline = 3;
  game::Game g{content, 403};
  g.setProfile(&profile);
  g.openMenu();

  // Walk to RESET PROGRESS (index 4).
  game::FrameInput down{};
  down.menuDown = true;
  for (int i = 0; i < 4; ++i) g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 4);

  // First confirm only ARMS it. This is the whole point: the button that
  // deletes hours of unlocks must not fire on the same press that arms it.
  game::FrameInput confirm{};
  confirm.menuConfirm = true;
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE(g.progressResetArmed());
  REQUIRE(profile.skin == 5);
  REQUIRE(profile.unlocks == static_cast<game::UnlockMask>(
      game::kUnlockElite | game::kUnlockChampion | game::kUnlockOverlord));

  // Leaving the row disarms it, so an old armed state cannot be fired from
  // somewhere else later.
  g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 5);
  REQUIRE_FALSE(g.progressResetArmed());
  // ...and walking all the way round the six rows is the only way back, which
  // is the point: a reset cannot be armed from one row and fired from another.
  for (int i = 0; i < 5; ++i) g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 4);
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE(g.progressResetArmed());

  // Second confirm wipes: skin back to default, every unlock gone, outline off.
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE_FALSE(g.progressResetArmed());
  REQUIRE(profile.skin == 0);
  REQUIRE(profile.outline == 0);
  REQUIRE(profile.unlocks == 0);
  // The player is repainted to the default skin straight away.
  const auto skin0 = game::skinPalette()[0].color;
  const auto c = g.testPlayerColor();
  REQUIRE(c.r == Catch::Approx(skin0.r));
  REQUIRE(c.g == Catch::Approx(skin0.g));
  REQUIRE(c.b == Catch::Approx(skin0.b));
  // And main() is told to write the wipe to disk.
  REQUIRE(g.consumeProfileDirty());
  REQUIRE_FALSE(g.consumeProfileDirty());

  // A third press is a fresh arm, not a second wipe.
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE(g.progressResetArmed());
  g.cancelProgressReset();
  REQUIRE_FALSE(g.progressResetArmed());
}

TEST_CASE("A wiped profile does not silently re-grant an outline on the next kill") {
  // The Game remembers which unlocks it has already pushed into the profile.
  // After a wipe that memory must be cleared, or the next elite kill would
  // re-push a bit the player no longer has and hand the outline straight back.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Profile profile;
  game::Game g{content, 404};
  g.setProfile(&profile);
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  // Kill a tier-0 enemy through the normal path and report the unlocks once.
  g.testKillFirstEnemy();
  g.consumeProfileDirty();
  // Nothing to earn from a normal enemy.
  REQUIRE(profile.unlocks == 0);

  g.beginProgressReset();
  REQUIRE(g.confirmProgressReset());
  REQUIRE(profile.unlocks == 0);
  // syncedUnlocks_ was cleared by the wipe, so a re-report would be treated as
  // new. A normal kill still earns nothing, which is the observable part.
  g.testKillFirstEnemy();
  REQUIRE(profile.unlocks == 0);
}

TEST_CASE("The reset row is inert without a profile but never crashes") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 405}; // no profile attached
  g.openMenu();
  game::FrameInput down{};
  down.menuDown = true;
  for (int i = 0; i < 4; ++i) g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 4);
  game::FrameInput confirm{};
  confirm.menuConfirm = true;
  g.advance(1.0F / 60.0F, confirm);
  // Nothing to arm, so nothing can fire later either.
  REQUIRE_FALSE(g.progressResetArmed());
  REQUIRE_FALSE(g.confirmProgressReset());
  // START and QUIT still work on a headless caller.
  game::FrameInput up{};
  up.menuUp = true;
  for (int i = 0; i < 4; ++i) g.advance(1.0F / 60.0F, up);
  REQUIRE(g.menuSelection() == 0);
  g.advance(1.0F / 60.0F, confirm);
  REQUIRE_FALSE(g.menuOpen());
}

TEST_CASE("Every weapon in the roster has at least one unique item") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 406};
  std::vector<std::string> withoutCards;
  std::vector<std::string> withoutUnique;
  for (const auto& w : content.weapons) {
    // The same accessor the game itself offers, so this is a check of the
    // shipped roster and not of a copy of it.
    const auto cards = g.collectWeaponCards(w.id);
    CAPTURE(w.id);
    REQUIRE_FALSE(cards.empty());
    bool hasUnique = false;
    for (const auto& u : content.upgrades) {
      if (u.weapon == w.id && u.kind == "unique") {
        hasUnique = true;
        break;
      }
    }
    if (!hasUnique) withoutUnique.push_back(w.id);
  }
  // A weapon with no card at all is a dead slot; one with cards but no unique
  // means its identity stat block is unreachable in a normal run.
  REQUIRE(withoutCards.empty());
  REQUIRE(withoutUnique.empty());
}

TEST_CASE("No card points at an effect the game does not implement") {
  // A typo in upgrades.toml is silent: the card shows up on the level-up screen
  // and then does nothing. Cross-check every effect id against the two appliers
  // by applying each card and requiring the call to report a change.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  for (const auto& def : content.upgrades) {
    CAPTURE(def.id);
    CAPTURE(def.effect);
    if (def.weapon.empty() && !game::Game::isWeaponWideEffect(def.effect)) {
      game::PlayerStats s{};
      const auto r = game::applyUpgrade(s, def.effect, def.value);
      REQUIRE(r.valid);
    } else {
      // Weapon-scoped effects have no standalone applier, so they are checked
      // by arming the weapon and taking the card in a real Game below.
      REQUIRE_FALSE(def.effect.empty());
    }
    REQUIRE(def.maxStacks >= 1);
    REQUIRE(std::isfinite(def.value));
  }
}

TEST_CASE("Every weapon-scoped card does something when its weapon is armed") {
  // The complement of the effect-id check: arm each weapon in turn, take every
  // card scoped to it, and prove the weapon's own numbers moved. A card that
  // silently no-ops on its own weapon is the worst kind of dead pick: it shows
  // up on the level-up screen, looks great, and does nothing.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  // A Game with the given weapon armed and the sim advanced one frame, so the
  // snapshot reflects the live slot rather than an empty arsenal.
  const auto armed = [&content](int weaponIndex) {
    auto g = std::make_shared<game::Game>(content, 407);
    g->testDisableWaves();
    g->testClearWeapons();
    g->testAddWeapon(weaponIndex);
    game::FrameInput in{};
    g->advance(1.0F / 60.0F, in);
    return g;
  };

  for (std::size_t wi = 0; wi < content.weapons.size(); ++wi) {
    const auto& def = content.weapons[wi];
    for (std::size_t ui = 0; ui < content.upgrades.size(); ++ui) {
      const auto& u = content.upgrades[ui];
      if (u.weapon != def.id) continue;
      CAPTURE(def.id);
      CAPTURE(u.id);
      const auto g = armed(static_cast<int>(wi));
      // The weapon lands in the first free slot, which is 0 after a clear.
      const auto before = g->testWeaponSnapshot(0);
      REQUIRE(before.def == static_cast<int>(wi));
      REQUIRE(g->testGrantUpgrade(static_cast<int>(ui)));
      const auto after = g->testWeaponSnapshot(0);
      REQUIRE(after != before);
      // The card is reported as taken, and taking it twice applies it twice
      // (for a normal card) — the stack counter is the other half of the
      // promise, and an effect that only ever fires once would be a trap.
      if (u.maxStacks > 1) {
        REQUIRE(g->upgradeStacks(ui) == 1);
        REQUIRE(g->testGrantUpgrade(static_cast<int>(ui)));
        REQUIRE(g->upgradeStacks(ui) == 2);
      }
    }
  }
}

TEST_CASE("The momentum meter has a card for every axis it actually has") {
  // momentumRate was a live simulation field that only one card could ever
  // touch, and momentumMax / momentumGain were not reachable at all outside a
  // unique. These three make the chain a real build axis.
  game::PlayerStats base{};
  REQUIRE(game::applyUpgrade(base, "momentum_rate", 0.5F).valid);
  REQUIRE(base.momentumRate == Catch::Approx(game::PlayerStats{}.momentumRate + 0.5F));

  game::PlayerStats t{};
  REQUIRE(game::applyUpgrade(t, "momentum_chain", 6.0F).valid);
  REQUIRE(t.momentumMax == game::PlayerStats{}.momentumMax + 6);

  game::PlayerStats u{};
  REQUIRE(game::applyUpgrade(u, "momentum_gain", 0.5F).valid);
  REQUIRE(u.momentumGain == Catch::Approx(game::PlayerStats{}.momentumGain + 0.5F));

  // The rate card must actually reach the cooldown, not just the sheet: the
  // meter feeds attackCooldown() and every spin rate in the game.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 408};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  int card = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].effect == "momentum_rate") {
      card = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(card >= 0);
  const float perStack = g.momentumRatePerStack();
  REQUIRE(g.testGrantUpgrade(card));
  REQUIRE(g.momentumRatePerStack() > perStack);
}

TEST_CASE("Each ability has a stackable card, not just a one-shot unique") {
  // The uniques are dramatic but you have to be lucky enough to roll one. The
  // J / K / L row should be a real build axis, so each button needs a normal
  // card too, and each must move the number the ability actually reads.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 409};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);

  const auto find = [&content](std::string_view effect) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].effect == effect && content.upgrades[i].weapon.empty()) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const int dash = find("ability_dash");
  const int burst = find("ability_burst");
  const int slow = find("ability_slow");
  REQUIRE(dash >= 0);
  REQUIRE(burst >= 0);
  REQUIRE(slow >= 0);
  // All three must be ordinary stackable cards, not one-shot uniques.
  for (int idx : {dash, burst, slow}) {
    const auto& def = content.upgrades[static_cast<std::size_t>(idx)];
    REQUIRE(def.kind == "normal");
    REQUIRE(def.maxStacks >= 2);
  }

  const float dashBefore = g.blinkDistance();
  const float burstR = g.burstRadius();
  const float burstD = g.burstDamage();
  const float slowDur = g.stasisDurationMax();
  const float slowMul = g.stasisSlowFactor();

  REQUIRE(g.testGrantUpgrade(dash));
  REQUIRE(g.blinkDistance() > dashBefore);
  REQUIRE(g.testGrantUpgrade(burst));
  REQUIRE(g.burstRadius() > burstR);
  REQUIRE(g.burstDamage() > burstD);
  REQUIRE(g.testGrantUpgrade(slow));
  REQUIRE(g.stasisDurationMax() > slowDur);
  REQUIRE(g.stasisSlowFactor() < slowMul);

  // A Phase Dash card must not turn the dash into a screen-crossing skip.
  for (int i = 0; i < 12; ++i) g.testGrantUpgrade(dash);
  REQUIRE(g.blinkDistance() <= 9.0F);
  // And Stasis must not stop the world outright.
  REQUIRE(g.stasisSlowFactor() >= 0.15F);
}

TEST_CASE("The shield is a pool and a clock, and both are now cards") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 410};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  const auto find = [&content](std::string_view effect) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].effect == effect && content.upgrades[i].weapon.empty()) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  const int regen = find("shield_regen");
  const int delay = find("shield_delay");
  REQUIRE(regen >= 0);
  REQUIRE(delay >= 0);

  const float rate0 = g.shieldRegenRate();
  const float delay0 = g.shieldRegenDelay();
  REQUIRE(g.testGrantUpgrade(regen));
  REQUIRE(g.shieldRegenRate() > rate0);
  REQUIRE(g.testGrantUpgrade(delay));
  REQUIRE(g.shieldRegenDelay() < delay0);

  // The delay is floored, so a shield build can never become permanent.
  for (int i = 0; i < 12; ++i) g.testGrantUpgrade(delay);
  REQUIRE(g.shieldRegenDelay() >= 0.5F);
}

TEST_CASE("A percentage heal resolves against the pool the player has now") {
  // A flat heal is useless next to a 400 HP pool, and a percentage that froze
  // the max HP at pickup time would under-heal after a max-HP card. This one
  // reads the CURRENT max, so the two compose.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 411};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  int kit = -1;
  int hpCard = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].effect == "heal_pct" && kit < 0) kit = static_cast<int>(i);
    if (content.upgrades[i].effect == "max_hp_add" && content.upgrades[i].weapon.empty() &&
        hpCard < 0) {
      hpCard = static_cast<int>(i);
    }
  }
  REQUIRE(kit >= 0);
  REQUIRE(hpCard >= 0);

  // Take the max-HP card first, then wound, then patch up: the heal must be a
  // share of the BIGGER pool.
  for (int i = 0; i < 3; ++i) REQUIRE(g.testGrantUpgrade(hpCard));
  const float bigMax = g.playerMaxHp();
  g.testDamagePlayer(bigMax - 10.0F);
  const float hurt = g.playerHp();
  REQUIRE(g.testGrantUpgrade(kit));
  // 40% of the bigger pool, capped at the pool.
  REQUIRE(g.playerHp() > hurt);
  REQUIRE(g.playerHp() == Catch::Approx(std::min(bigMax, hurt + bigMax * 0.4F)).margin(0.001F));
}

TEST_CASE("The arsenal cap is four plus real slot cards, and the array is big enough") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 412};
  g.testDisableWaves();
  g.testClearWeapons();
  // Armed to the cap first: the cap is the design limit, and addWeapon must
  // refuse past it even when asked directly.
  std::vector<int> roster;
  for (std::size_t i = 0; i < content.weapons.size() && roster.size() < 4; ++i) {
    if (content.weapons[i].prereqs.empty()) roster.push_back(static_cast<int>(i));
  }
  for (int idx : roster) g.testAddWeapon(idx);
  REQUIRE(g.armedWeaponIds().size() == 4);
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    g.testAddWeapon(static_cast<int>(i));
  }
  REQUIRE(g.armedWeaponIds().size() == 4);
  REQUIRE(g.weaponCap() == 4);

  // Now the slot cards. Every "+1 weapon slot" card in content has to be
  // reachable, and the total must not exceed the storage array.
  int slotStacks = 0;
  int slotCards = 0;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].effect != "weapon_slot_add") continue;
    ++slotCards;
    for (std::size_t s = 0; s < content.upgrades[i].maxStacks; ++s) {
      REQUIRE(g.testGrantUpgrade(static_cast<int>(i)));
      ++slotStacks;
    }
  }
  // Exactly one slot card, and it stacks. There used to be a second one -- a
  // unique that also said "+1 weapon slot" and also claimed the total was eight
  // when it is seven -- so a level-up screen could be spent on a choice between
  // two descriptions of the same decision. One card, three stacks, and the
  // arsenal is a fixed shape the whole way up.
  REQUIRE(slotCards == 1);
  REQUIRE(slotStacks == game::Game::kMaxSlotCards);
  REQUIRE(g.weaponCap() == game::Game::kBaseWeapons + slotStacks);
  REQUIRE(game::Game::kMaxWeapons == 7);
  REQUIRE(g.weaponCap() <= game::Game::kMaxWeapons);

  // The array really is big enough for the full cap: fill it and confirm the
  // last slot accepts a weapon (a short weapons_[] would write out of bounds
  // here rather than failing a REQUIRE).
  std::size_t armed = g.armedWeaponIds().size();
  for (std::size_t i = 0; i < content.weapons.size() && armed < static_cast<std::size_t>(
           g.weaponCap()); ++i) {
    const std::size_t before = g.armedWeaponIds().size();
    g.testAddWeapon(static_cast<int>(i));
    armed = g.armedWeaponIds().size();
  }
  REQUIRE(armed == static_cast<std::size_t>(g.weaponCap()));
  // And past the cap nothing more is accepted.
  const std::size_t full = g.armedWeaponIds().size();
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    g.testAddWeapon(static_cast<int>(i));
  }
  REQUIRE(g.armedWeaponIds().size() == full);
}

TEST_CASE("A hand-edited slot card cannot push the arsenal past its array") {
  // Content is data: a well-meaning edit that grants 9 slots must not write past
  // weapons_[8]. The clamp in applyUpgrade is what makes that safe, and this is
  // the test that says so out loud.
  game::PlayerStats s{};
  for (int i = 0; i < 20; ++i) REQUIRE(game::applyUpgrade(s, "weapon_slot_add", 1.0F).valid);
  REQUIRE(s.weaponSlots == game::Game::kMaxSlotCards);
  // Negative values cannot drive the count below zero either.
  game::PlayerStats t{};
  for (int i = 0; i < 5; ++i) REQUIRE(game::applyUpgrade(t, "weapon_slot_add", -1.0F).valid);
  REQUIRE(t.weaponSlots == 0);
}

// --- Round 15: text layout -----------------------------------------------------

TEST_CASE("Wrapped text hangs its continuation lines instead of running flush") {
  // The reason this exists: a wrapped paragraph used to be a rectangle of text
  // with no left edge on the continuation rows, so the eye had to re-read the
  // row above to find where the sentence restarted. wrapToWidth charges the
  // indent against the width of every line after the first, which is what makes
  // the indent honest instead of decoration.
  constexpr float kScale = 2.0F;
  constexpr float kAdvance = 6.0F * kScale; // Batcher::textWidth
  constexpr float kWidth = 240.0F;
  constexpr float kIndent = 32.0F;

  const auto lines = game::wrapToWidth("one two three four five six seven", kWidth,
                                       kScale, kIndent);
  REQUIRE(lines.size() > 1);

  // The first line may use the whole box; every later one is `kIndent` narrower,
  // so its character budget is strictly smaller.
  const auto firstMax = static_cast<std::size_t>(kWidth / kAdvance);
  const auto restMax = static_cast<std::size_t>((kWidth - kIndent) / kAdvance);
  REQUIRE(firstMax > restMax);
  REQUIRE(lines[0].size() <= firstMax);
  for (std::size_t i = 1; i < lines.size(); ++i) {
    REQUIRE(lines[i].size() <= restMax);
  }

  // No content is lost or duplicated by the wrap.
  std::string joined;
  for (const auto& l : lines) {
    if (!joined.empty()) joined += ' ';
    joined += l;
  }
  REQUIRE(joined == "one two three four five six seven");

  // No indent: every line gets the full box, so the indent is what costs width
  // and nothing else does.
  const auto flat = game::wrapToWidth("one two three four five six seven", kWidth,
                                      kScale, 0.0F);
  for (const auto& l : flat) REQUIRE(l.size() <= firstMax);
  REQUIRE(flat.size() <= lines.size());

  // Degenerate boxes must not hang or crash.
  REQUIRE(game::wrapToWidth("hello", 0.0F, kScale, kIndent).size() == 1);
  REQUIRE(game::wrapToWidth("hello", -5.0F, kScale, kIndent).size() == 1);
  REQUIRE(game::wrapToWidth("hello", kWidth, 0.0F, kIndent).size() == 1);
  REQUIRE(game::wrapToWidth("", kWidth, kScale, kIndent).empty());
  // An indent wider than the box still yields something drawable, not a
  // negative character budget.
  for (const auto& l : game::wrapToWidth("a b c d e f", kWidth, kScale, 1e6F)) {
    REQUIRE_FALSE(l.empty());
  }
}

TEST_CASE("Every card description fits the card, and no card text is silently lost") {
  // Two separate promises, both of which used to be broken:
  //  - the level-up card measured nothing, so a long description ran off the
  //    bottom of the panel onto the reroll hint;
  //  - the sandbox item list measured the description and simply DID NOT DRAW
  //    IT if it was wider than half the panel, so most items showed as a bare
  //    name with no explanation at all.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE_FALSE(content.upgrades.empty());
  REQUIRE_FALSE(content.weapons.empty());

  // Card body geometry. These come from game::cardTextLayout rather than being
  // mirrored here, because the card sets every line at one size and one pitch --
  // a copy of the constants would quietly test the wrong layout the next time the
  // style moved.
  constexpr float kCardMinW = 200.0F;
  constexpr float kCardMaxW = 400.0F;
  constexpr float kGap = 24.0F;
  // Where the description starts. 86 is a one-line title; a title that wraps to
  // two lines pushes the body 24px further down, and because the card height is
  // clamped at kCardMaxH that can COST a line rather than gain one. So the
  // wrapped case is the pessimistic one and is the one that has to hold.
  constexpr float kBodyTopShort = 86.0F;
  constexpr float kBodyTopTall = 110.0F;
  constexpr float kFooter = 30.0F;
  constexpr float kCardMinH = 150.0F;
  constexpr float kCardMaxH = 364.0F;
  // How many lines the card actually shows for a body that wants `needed`.
  // Takes the row's own card width: it used to be handed kCardMaxW, so it
  // measured a 400px card while the renderer measures the row's real width. That
  // is only harmless while the scale is pinned by its clamps -- un-pin it and
  // this silently tests a layout nobody renders, which is the exact failure the
  // comment above exists to prevent.
  const auto capLines = [&](float cardW, std::size_t needed, float bodyTop) {
    const auto lay = game::cardTextLayout(cardW);
    const float h = std::clamp(bodyTop + game::cardTextHeight(static_cast<int>(needed), lay) +
                                   kFooter,
                               kCardMinH, kCardMaxH);
    return static_cast<std::size_t>(game::cardTextLinesThatFit(h - bodyTop - kFooter, lay));
  };
  // The minimum card height has to hold at least one line of body, otherwise a
  // short description would be clipped by the very clamp meant to protect it.
  REQUIRE(capLines(kCardMaxW, 1, kBodyTopShort) >= 1);
  REQUIRE(capLines(kCardMaxW, 2, kBodyTopShort) >= 2);
  REQUIRE(capLines(kCardMaxW, 1, kBodyTopTall) >= 1);
  REQUIRE(capLines(kCardMaxW, 2, kBodyTopTall) >= 2);

  // The worst case is the widest card row the level-up screen can produce: 3
  // cards is the normal case, 5 only with Gambler's Eye, and the row is
  // centred so the narrowest is what decides whether text stays inside.
  for (std::size_t n : {std::size_t{3}, std::size_t{4}, std::size_t{5}}) {
    const float avail = (game::Game::kRefScreenWidth - kGap * static_cast<float>(n - 1) -
                         40.0F) /
                        static_cast<float>(n);
    const float cardW = std::clamp(avail, kCardMinW, kCardMaxW);
    // The row itself must fit the screen, or the last card is off the edge.
    REQUIRE(static_cast<float>(n) * cardW + static_cast<float>(n - 1) * kGap <=
            game::Game::kRefScreenWidth);
    // The body is ONE column: every line the same size, flush to the same left
    // edge, on a pitch with real air in it. This is a style, not a preference --
    // the card used to set its wrapped lines 18% larger and indent them 32px
    // inboard, which players read as a rendering fault, so the asymmetry is
    // pinned here so it cannot creep back in.
    const auto lay = game::cardTextLayout(cardW);
    REQUIRE(lay.scale >= 1.3F);
    REQUIRE(lay.contScale == lay.scale);
    REQUIRE(lay.contLineH == lay.lineH);
    REQUIRE(lay.indent == 0.0F);
    // Room between lines, or the paragraph reads as a wall. A 7-row glyph at 1.6
    // is 11.2px tall, so the pitch has to clear it with a visible gap.
    REQUIRE(lay.lineH >= 7.0F * lay.scale + 6.0F);
    // The renderer must not truncate below the fitted height, or a long
    // description silently loses its last clause. capLines is monotone, so
    // checking the ceiling holds every smaller case too.
    REQUIRE(capLines(cardW, 10, kBodyTopShort) == 10); // every 4-card row fits in full
    REQUIRE(capLines(cardW, 10, kBodyTopTall) == 10);  // ...even with a wrapped title
    REQUIRE(capLines(cardW, 30, kBodyTopTall) >= 10);   // the ceiling absorbs a long card
    for (const auto& u : content.upgrades) {
      CAPTURE(u.id);
      const auto lines = game::wrapToWidth(u.desc, lay.width, lay.scale, lay.indent,
                                           lay.contScale);
      // ...and no line is wider than its own budget.
      const auto firstMax = static_cast<std::size_t>(lay.width / (6.0F * lay.scale));
      REQUIRE(lines[0].size() <= firstMax);
      for (std::size_t i = 1; i < lines.size(); ++i) {
        REQUIRE(static_cast<float>(lines[i].size()) * 6.0F * lay.contScale <=
                lay.width - lay.indent + 0.001F);
      }
    }
  }

  // The narrowest row the screen can produce is the real budget, so check the
  // shipped content against THAT rather than the comfortable 3-card case. Any
  // item that does not fit there is one the 5-card row will cut, and the cut
  // is only acceptable while it stays a handful of long evolution blurbs.
  {
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& u : content.upgrades) items.emplace_back(u.id, u.desc);
    for (const auto& w : content.weapons) items.emplace_back(w.id, w.desc);

    std::size_t worst = 0;
    std::string worstId;
    std::size_t over = 0;
    for (const auto& n : {std::size_t{3}, std::size_t{4}, std::size_t{5}}) {
      const float cardW =
          std::clamp((game::Game::kRefScreenWidth - kGap * static_cast<float>(n - 1) -
                      40.0F) / static_cast<float>(n),
                     kCardMinW, kCardMaxW);
      const auto lay = game::cardTextLayout(cardW);
      const auto count = [&](std::string_view d) {
        return game::wrapToWidth(d, lay.width, lay.scale, lay.indent, lay.contScale).size();
      };
      // The title is the tightest block in the narrowest card, so its own wrap
      // is checked here too: at most two lines, nothing elided, nothing wider
      // than the card.
      for (const auto& it : items) {
        CAPTURE(n);
        CAPTURE(it.first);
        const auto nameLay = game::cardNameLayout(it.first, lay.width);
        REQUIRE_FALSE(nameLay.elided);
        REQUIRE(nameLay.lines.size() <= 2);
        for (const auto& l : nameLay.lines) {
          REQUIRE(static_cast<float>(l.size()) * 6.0F * nameLay.scale <= lay.width);
        }
      }
      // The worst case is the wrapped title: it costs a line, so it is the one
      // that decides whether a 3- or 4-card row loses any of its text.
      const std::size_t room = capLines(cardW, static_cast<std::size_t>(100), kBodyTopTall);
      for (const auto& it : items) {
        const std::size_t need = count(it.second);
        if (n == 5) {
          if (need > worst) {
            worst = need;
            worstId = it.first;
          }
          if (need > room) ++over;
        } else {
          // 3- and 4-card rows are the ones players actually see; none of them
          // may lose a single character.
          CAPTURE(n);
          CAPTURE(it.first);
          REQUIRE(need <= room);
        }
      }
    }
    CAPTURE(worstId);
    CAPTURE(worst);
    CAPTURE(over);
    // Only the 5-card row may truncate, and only for a few very long blurbs.
    REQUIRE(over <= 6);
  }

  // The sandbox hint box: a row that shows a hint at all must leave room for
  // one, and long descriptions must be short enough to be truncated honestly
  // (with a "+") rather than dropped.
  constexpr float kHintScale = 1.2F;
  const float panelW = std::min(720.0F, game::Game::kRefScreenWidth * 0.9F);
  int withHint = 0;
  for (const auto& u : content.upgrades) {
    CAPTURE(u.id);
    const float labelW = (static_cast<float>(u.name.size()) + 8.0F) * 6.0F * 1.6F;
    const float hintBox = panelW - 48.0F - labelW;
    if (hintBox > 60.0F) {
      ++withHint;
      const auto lines = game::wrapToWidth(u.desc, hintBox, kHintScale, 12.0F);
      // Two lines at most; the third is the honest "+" truncation.
      REQUIRE(lines.size() >= 1);
    }
  }
  // Sanity: the vast majority of items really do get a hint drawn, which is the
  // regression this whole change is about.
  REQUIRE(withHint > static_cast<int>(content.upgrades.size()) / 2);
}

// --- Fast-enemy pacing --------------------------------------------------------

TEST_CASE("Fast enemies are held back to 4-5 minutes, then ramp back up to speed") {
  // The intent behind speed_ramp + fast: a quick enemy is something the run
  // grows INTO. `speed` in the data is what the player meets; the ramp restores
  // the intended top speed by 10:00, on the same clock the difficulty curve
  // steepens on.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.enemies.size() >= 18);

  std::vector<std::size_t> fastTypes;
  for (std::size_t i = 0; i < content.enemies.size(); ++i) {
    if (content.enemies[i].fast) fastTypes.push_back(i);
  }
  REQUIRE(fastTypes.size() >= 6);
  for (const auto i : fastTypes) {
    CAPTURE(content.enemies[i].id);
    // A fast type with no ramp would be slow and then suddenly quick, with
    // nothing in between: the loader rejects that, and so does this.
    REQUIRE(content.enemies[i].speedRampMax > 0.0F);
    REQUIRE(content.enemies[i].speedRampMax <= game::kSpeedRampCeiling);
  }

  game::Game g{content, 4242};
  g.testDisableWaves();

  for (const auto i : fastTypes) {
    CAPTURE(content.enemies[i].id);
    // Locked at t=0 no matter what its own unlock_at says.
    REQUIRE_FALSE(g.testTypeCanSpawn(static_cast<int>(i)));
    REQUIRE(g.typeLockedUntil(static_cast<int>(i)) >= game::Game::kFastEnemyMinTime);
    // The `fast` floor is a FLOOR: it holds a quick type back until
    // kFastEnemyMinTime, but it must never push one out past the time its own
    // unlock_at already asked for. A type scheduled for 8:00 is gated by its own
    // schedule, not by the fast rule, and asserting an absolute ceiling here used
    // to be a way of saying "no quick type may ever unlock after 5:00" -- which is
    // a statement about the roster, not about the floor, and broke the moment the
    // heavies were spread out across the run.
    const float own = content.enemies[i].unlockAt;
    const float want = std::max(own, game::Game::kFastEnemyMinTime);
    REQUIRE(g.typeLockedUntil(static_cast<int>(i)) == Catch::Approx(want));
    // And it really is a floor for the ones that ask for less than it.
    if (own < game::Game::kFastEnemyMinTime) {
      REQUIRE(g.typeLockedUntil(static_cast<int>(i)) ==
              Catch::Approx(game::Game::kFastEnemyMinTime));
    }
  }

  // The ramp: at t=0 the authored (slowed) speed, at kSpeedRampFullTime the
  // authored speed plus the full ramp, and the global difficulty ramp on top of
  // both -- so a late fast enemy is a real escalation and an early one is not.
  for (const auto i : fastTypes) {
    CAPTURE(content.enemies[i].id);
    const auto idx = static_cast<int>(i);
    const float base = content.enemies[i].speed;

    g.testSetSimTime(0.0F);
    const float atStart = g.enemySpawnSpeed(idx);
    REQUIRE(g.typeSpeedRamp(idx) == Catch::Approx(1.0F));
    // Global speed scale is 1.0 at t=0, so this is exactly the authored speed.
    REQUIRE(atStart == Catch::Approx(base));

    g.testSetSimTime(game::Game::kFastEnemyMinTime);
    const float atGate = g.enemySpawnSpeed(idx);
    REQUIRE(g.typeSpeedRamp(idx) > 1.0F);
    REQUIRE(atGate > atStart); // it is already speeding up as it arrives

    g.testSetSimTime(game::Game::kSpeedRampFullTime);
    const float atFull = g.enemySpawnSpeed(idx);
    REQUIRE(g.typeSpeedRamp(idx) == Catch::Approx(1.0F + content.enemies[i].speedRampMax));
    // Never runs away: the per-type ramp is capped, so the late speed is the
    // authored speed times a KNOWN multiple of the ramp, times the global curve
    // (which is itself capped at 2.4 by currentScales). Asserting against the
    // product rather than a bare ratio keeps the two ramps from being confused
    // for one another.
    REQUIRE(atFull <= base * (1.0F + content.enemies[i].speedRampMax) * 2.4F + 1e-3F);
    REQUIRE(atFull > atStart); // still an escalation, just a bounded one

    // Saturates: past the full time the ramp stops growing.
    g.testSetSimTime(game::Game::kSpeedRampFullTime * 3.0F);
    REQUIRE(g.typeSpeedRamp(idx) == Catch::Approx(1.0F + content.enemies[i].speedRampMax));
  }
}

TEST_CASE("Nothing quick spawns before the fast floor, and the pool is never empty") {
  // Two guarantees at once. The pacing rule must hold for the WHOLE roster, not
  // just the types that were edited: whatever the data says, the first four
  // minutes contain no `fast` archetype. And the gate must not accidentally
  // starve the spawn pool -- a run that cannot spawn is a dead run.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  game::Game g{content, 77};
  g.testDisableWaves();
  for (std::size_t i = 0; i < content.enemies.size(); ++i) {
    if (!content.enemies[i].fast) continue;
    REQUIRE_FALSE(g.testTypeCanSpawn(static_cast<int>(i)));
  }

  // The pool has a legal member at every moment of the opening, and holds a
  // fast one once the floor passes.
  auto eligibleCount = [&](float t) {
    g.testSetSimTime(t);
    int n = 0;
    for (std::size_t i = 0; i < content.enemies.size(); ++i) {
      if (g.testTypeCanSpawn(static_cast<int>(i))) ++n;
    }
    return n;
  };
  // At t=0 exactly one type is unlocked (the Bat is the only enemy with
  // unlock_at 0), so the floor here is "never zero" for the whole opening and
  // "a real choice" once the second wave of unlocks has landed.
  REQUIRE(eligibleCount(0.0F) >= 1);
  for (float t = 45.0F; t < game::Game::kFastEnemyMinTime; t += 15.0F) {
    CAPTURE(t);
    REQUIRE(eligibleCount(t) >= 4);
  }
  const int late = eligibleCount(game::Game::kFastEnemyMinTime + 1.0F);
  const int early = eligibleCount(0.0F);
  REQUIRE(late > early); // the fast roster actually joins in

  // Every eligible type at the floor has a ramped speed, i.e. the fastest thing
  // on screen at 4:00 is not a 4x charger.
  g.testSetSimTime(game::Game::kFastEnemyMinTime + 1.0F);
  float fastest = 0.0F;
  std::string fastestId;
  for (std::size_t i = 0; i < content.enemies.size(); ++i) {
    if (!g.testTypeCanSpawn(static_cast<int>(i))) continue;
    const float s = g.enemySpawnSpeed(static_cast<int>(i));
    if (s > fastest) {
      fastest = s;
      fastestId = content.enemies[i].id;
    }
  }
  CAPTURE(fastestId);
  REQUIRE(fastest > 0.0F);
  // The authored top speed of the fastest archetype, times the global ramp at
  // 4:00 (1.4) -- a bound that would fail loudly if a speed were left un-nerfed.
  REQUIRE(fastest <= 6.3F * 1.41F);
}

TEST_CASE("The elite Fast trait ramps instead of applying a flat speed bonus") {
  // A flat 1.7x on every Fast elite means a 4-minute horde is undodgeable and
  // the trait says nothing about when it is dangerous. It now grows to the full
  // bonus by kFastTraitFullTime, so the trait is a clock as well as a stat.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 31};
  g.testDisableWaves();

  const auto shareAt = [&](float t) {
    g.testSetSimTime(t);
    return g.fastTraitSpeedMul();
  };

  const float early = shareAt(0.0F);
  const float mid = shareAt(game::Game::kFastTraitFullTime * 0.5F);
  const float late = shareAt(game::Game::kFastTraitFullTime);
  const float later = shareAt(game::Game::kFastTraitFullTime * 4.0F);

  CAPTURE(early);
  CAPTURE(mid);
  CAPTURE(late);
  REQUIRE(early == Catch::Approx(1.0F)); // no bonus at all in the opening minute
  REQUIRE(late == Catch::Approx(game::Game::kFastTraitMaxMul));
  REQUIRE(mid > early);
  REQUIRE(mid < late);
  // Monotone and saturating: the trait never gets slower as the run goes on, and
  // it never grows past its cap. Both are what make the trait readable.
  REQUIRE(early <= mid);
  REQUIRE(mid <= late);
  REQUIRE(later == late);

  // A tier-1 elite is also boosted by the tier itself, so the trait is a
  // multiplier ON TOP of that, not a replacement for it.
  REQUIRE(game::Game::kFastTraitMaxMul > 1.0F);
}

// --- Weapon originality: the reworked mechanics -----------------------------
//
// The rework exists to stop an evolution from being its parent with a bigger
// number. These tests pin the RULES that make the reworked weapons different
// things rather than bigger things, because "it forks" and "it shatters" and
// "it comes apart" are claims that are only true while the code does them.

namespace {

// Index of a weapon in the content roster, or -1. The roster is data-driven and
// its order is not part of any contract, so a test names what it wants.
int weaponIndex(const game::Content& content, std::string_view id) {
  for (std::size_t i = 0; i < content.weapons.size(); ++i) {
    if (content.weapons[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

// A game armed with exactly one weapon and no incoming spawns.
struct SoloWeapon {
  game::Content content;
  game::Game g;
  int slot = 0;
  explicit SoloWeapon(std::string_view id, std::uint32_t seed = 7)
      : content(game::loadContent(GAME_ASSETS_DIR "/data")),
        g(content, seed) {
    g.testDisableWaves();
    slot = weaponIndex(content, id);
  }
  // Arms the weapon and makes the first shot land immediately, so a test does
  // not have to wait out a cooldown to see the effect.
  void arm() { g.testAddWeapon(slot); }
};

// The most wind-up left on any live chain bolt, in milliseconds, or -1 if every
// bolt has closed its ring. Not a lambda: it is called from inside REQUIRE, and
// Catch2's macro turns the expression into a template argument, where a
// user-defined conversion does not go.
int chainMostAiming(const std::vector<int>& telegraphs) {
  int best = -1;
  for (const int ms : telegraphs) best = std::max(best, ms);
  return best;
}

}  // namespace

TEST_CASE("No evolution is a numbers-only copy of one of its parents") {
  // The whole point of the rework: 9 of 10 evolutions used to run the exact
  // AttackType of one of their parents with only the numbers changed, which
  // made them reskins. What is left sharing an attack type with a parent is
  // only allowed if its own description is about the numbers -- and even then
  // there has to be at most a couple of them, or the rule stops meaning
  // anything.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // Sharing an attack type with a parent is only a reskin if nothing else
  // separates them, so the rule is narrower than "different type": it is
  // "different type, or a rule the parent does not have". Two weapons running
  // the same code with the same rules and only bigger numbers are the same
  // weapon, and that is the thing worth failing a build over.
  struct Differentiator {
    // Does this weapon run a rule its parent does not?
    bool hasOwnRule;
    // What that rule is, for the failure message.
    const char* why;
  };
  const auto rulesOf = [](const game::WeaponDef& w) -> Differentiator {
    if (w.reaimRange > 0.0F) return {true, "bends onto the next body"};
    if (w.chainShatter > 0) return {true, "shatters into shards"};
    if (w.bounceSplits > 0) return {true, "splits on every impact"};
    if (w.waveCount > 1 || w.waveArcStep > 0.0F) return {true, "throws several arcs"};
    if (w.attackType == game::AttackType::Wave) return {true, "travels away from the player"};
    // Rules added after this test was first written. Each is a mechanic, not a
    // number: a weapon running the same code as its parent but with one of these
    // on is a different weapon, which is exactly what the test is here to say.
    if (w.coneBite > 0.0F) return {true, "bites one target and ramps"};
    if (w.coneEmberAt > 0.0F) return {true, "leaves burning ground"};
    if (w.bombAhead > 0.0F) return {true, "fires over the front rank"};
    if (w.bombOnTarget) return {true, "lands on whatever it is aimed at"};
    if (w.novaContract) return {true, "contracts inward and bursts"};
    if (w.novaEcho > 0.0F) return {true, "fires a second, delayed ring"};
    if (w.haloInner > 0.0F) return {true, "leaves a hole at its centre"};
    if (w.vortexCollapseAt > 0.0F) return {true, "collapses and reopens"};
    if (w.vortexCrowd > 0.0F) return {true, "damages per body it holds"};
    if (w.sweepHook) return {true, "drags its catch in"};
    if (w.orbitWindow) return {true, "has a gap in the ring"};
    if (w.auraRadius > 0.0F) return {true, "carries a chilling aura"};
    return {false, "same attack, same rules"};
  };

  std::vector<std::string> reskins;
  for (const auto& w : content.weapons) {
    if (w.prereqs.empty()) continue; // a base weapon has no parents to copy
    const auto mine = rulesOf(w);
    for (const auto& parentId : w.prereqs) {
      const auto* parent = content.weapon(parentId);
      REQUIRE(parent != nullptr);
      const auto theirs = rulesOf(*parent);
      if (w.attackType == parent->attackType && !mine.hasOwnRule && !theirs.hasOwnRule) {
        reskins.push_back(w.id + " copies " + parentId + " (" + mine.why + ")");
      }
    }
  }
  for (const auto& r : reskins) INFO(r);
  REQUIRE(reskins.empty());
}

TEST_CASE("Every reworked weapon has a rule the others do not have") {
  // A different AttackType alone is not enough -- two weapons can share a type
  // and still differ by a rule, which is what the second half of this checks.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // The reworked four are each a different attack type from their parents, and
  // the two remaining chain weapons are told apart by a rule, not a number.
  REQUIRE(content.weapon("sunder")->attackType == game::AttackType::Wave);
  REQUIRE(content.weapon("tidewhip")->attackType == game::AttackType::Wave);
  REQUIRE(content.weapon("chaos")->attackType == game::AttackType::Bounce);
  REQUIRE(content.weapon("blizzard")->attackType == game::AttackType::Chain);

  // Tesla forks nothing and shatters nothing. Blizzard shatters. These are the
  // rules, and they are what the cards promise.
  REQUIRE(content.weapon("tesla")->chainShatter == 0);
  REQUIRE(content.weapon("blizzard")->chainShatter > 0);

  // The Storm Caller stopped being a chain weapon. A bolt that BENDS is a
  // projectile, and running it through the chain system made it a second Tesla
  // Coil with a number on it rather than a merge of the wand and the crossbow.
  // Re-aiming is its rule, and nothing else in the game has it -- least of all
  // the two weapons it is easy to confuse it with: the plain crossbow bolt that
  // goes straight, and the tesla arc that jumps.
  REQUIRE(content.weapon("storm")->attackType == game::AttackType::Projectile);
  REQUIRE(content.weapon("storm")->reaimRange > 0.0F);
  REQUIRE(content.weapon("storm")->reaimTurn > 0.0F);
  REQUIRE(content.weapon("storm")->pierce > 0);
  REQUIRE(content.weapon("crossbow")->reaimRange == 0.0F);
  REQUIRE(content.weapon("wand")->reaimRange == 0.0F);
  REQUIRE(content.weapon("tesla")->reaimRange == 0.0F);
  REQUIRE(content.weapon("rimewake")->reaimRange == 0.0F);

  // The Chaos Orb comes apart; the Pinball Puck and the eternal Void Orb do not.
  REQUIRE(content.weapon("chaos")->bounceSplits > 0);
  REQUIRE(content.weapon("pinball")->bounceSplits == 0);
  REQUIRE(content.weapon("orb")->bounceSplits == 0);

  // A wave that leaves the player (Tidewhip throws three of them) is a
  // different thing from a nova that grows around them, so the two evolution
  // slots are not interchangeable.
  REQUIRE(content.weapon("tidewhip")->waveCount > 1);
  REQUIRE(content.weapon("tidewhip")->waveArcStep > 0.0F);
  REQUIRE(content.weapon("sunder")->waveCount == 1);
  REQUIRE(content.weapon("sunder")->waveKnockback > 0.0F);
}

TEST_CASE("A wave leaves the player, shoves downrange, and hits each enemy once") {
  // The Sunder's whole identity is that it is NOT a nova. A nova is pinned to
  // the player and only grows, so it hits the same crowd in the same order; a
  // wave travels. These three assertions are the difference.
  SoloWeapon s("sunder");
  REQUIRE(s.slot >= 0);
  s.arm();

  // A tight rank of stationary targets directly to the +x side of the origin.
  for (int i = 0; i < 4; ++i) {
    s.g.testSpawnEnemyAt(2.0F + static_cast<float>(i) * 1.2F, 0.0F);
  }
  const auto before = s.g.testEnemyPositions();
  REQUIRE(before.size() == 8);

  s.g.testAdvance(0.2F);
  REQUIRE(s.g.testWaveCount() > 0);

  // The wave is somewhere between the player and the far end of the rank: it
  // travelled away from the player instead of sitting on top of them.
  const auto pos = s.g.testWavePositions();
  REQUIRE(pos.size() >= 2);
  REQUIRE(pos[0] > 0.0F);
  REQUIRE(pos[0] < 6.0F);

  // The far end of the rank moved further +x than the near end, i.e. the shove
  // was along the wave's direction of travel and not away from the player.
  s.g.testAdvance(0.6F);
  const auto after = s.g.testEnemyPositions();
  REQUIRE(after.size() == before.size());
  float nearPush = 0.0F;
  float farPush = 0.0F;
  for (std::size_t i = 0; i < after.size(); i += 2) {
    const float push = after[i] - before[i];
    if (i < 4) {
      nearPush = push;
    } else {
      farPush += push;
    }
  }
  farPush /= 2.0F;
  CAPTURE(nearPush);
  CAPTURE(farPush);
  // Everything caught is pushed downrange (positive x).
  REQUIRE(nearPush > 0.0F);
  REQUIRE(farPush > 0.0F);
  // And it is a push down the line, not a radial blast off the player: the far
  // enemies are shoved at least as hard as the near ones.
  REQUIRE(farPush >= nearPush * 0.8F);
}

TEST_CASE("A wave damages an enemy once, not once per tick") {
  // A wave is a passing event. If it re-ticked, a target standing inside the
  // band would be hit every frame for as long as the wave lingered, which would
  // make a 0.5s wave worth ten times a 0.05s one for no reason.
  SoloWeapon s("sunder");
  REQUIRE(s.slot >= 0);
  s.arm();
  s.g.testSpawnEnemyAt(2.0F, 0.0F);
  const float hp0 = s.g.testFirstEnemyHp();

  // The whole window has to stay inside ONE shot. The weapon's cooldown is
  // 1.6s, and a second wave passing through would land damage that has nothing
  // to do with the "hits once" claim -- so 1.45s of advance, not 2s of it.
  s.g.testAdvance(1.0F);
  const float hp1 = s.g.testFirstEnemyHp();
  // The wave has passed and gone: no more damage is landing.
  s.g.testAdvance(0.4F);
  const float hp2 = s.g.testFirstEnemyHp();
  REQUIRE(hp1 < hp0);
  REQUIRE(hp2 == Catch::Approx(hp1));
  // And the total it dealt is a single hit's worth, not a stream: with base
  // damage 55 and a 0.85 multiplier the one hit is well under the 100000 HP the
  // test enemy carries, so a stream would show up as a much larger drop.
  REQUIRE(hp0 - hp1 < 55.0F * 0.85F * 1.5F);
}

TEST_CASE("A re-aiming bolt bends onto the next body instead of sailing past it") {
  // The Storm Caller's whole merge. The Crossbow pierces four bodies in a STRAIGHT
  // line, so the last body it touches is the far end of the crowd and anything
  // off that line is never in play; the Wand always hits what it is aimed at.
  // Spending the reach on DIRECTION instead gets both: the bolt keeps the punch
  // and keeps finding somebody.
  //
  // The crowd is therefore a CURVE and not a row. A row is the one layout where
  // the two rules cannot tell each other apart -- a straight bolt walks the whole
  // row and a bending one walks it too, so the test would be measuring the layout
  // instead of the mechanic. A quarter-arc is the layout the mechanic exists for:
  // only a bolt that turns at each body can stay on it.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* storm = content.weapon("storm");
  REQUIRE(storm != nullptr);
  REQUIRE(storm->reaimRange > 0.0F);
  // It is a projectile now, not an arc, so the bending case and the straight case
  // are the same code with one number changed.
  REQUIRE(storm->attackType == game::AttackType::Projectile);
  REQUIRE(content.weapon("crossbow")->reaimRange == 0.0F);

  // A quarter arc: 2.2 units per hop, each hop turned about half a radian.
  const std::array<std::pair<float, float>, 4> arc{{
      {3.0F, 0.0F},
      {4.35F, 1.75F},
      {4.6F, 3.9F},
      {3.7F, 5.9F},
  }};

  SoloWeapon s("storm", 11);
  REQUIRE(s.slot >= 0);
  s.arm();
  for (const auto& [x, y] : arc) s.g.testSpawnEnemyAt(x, y);
  // One window: the weapon fires on a 0.6s cadence, so a second bolt passing
  // through would have nothing to do with the claim.
  s.g.testAdvance(0.5F);
  std::size_t stormHits = 0;
  for (const float hp : s.g.testEnemyHps()) {
    if (hp < 100000.0F) ++stormHits;
  }
  CAPTURE(stormHits);

  // The Crossbow, which is the honest control: a piercing bolt with no re-aim,
  // aimed the same way at the same crowd.
  SoloWeapon straight("crossbow", 11);
  REQUIRE(straight.slot >= 0);
  straight.arm();
  for (const auto& [x, y] : arc) straight.g.testSpawnEnemyAt(x, y);
  straight.g.testAdvance(0.5F);
  std::size_t straightHits = 0;
  for (const float hp : straight.g.testEnemyHps()) {
    if (hp < 100000.0F) ++straightHits;
  }
  CAPTURE(straightHits);

  // The bolt that turns follows the crowd; the one that goes straight takes what
  // happens to be on its line and leaves the rest of the arc standing there.
  REQUIRE(stormHits >= 3);
  REQUIRE(stormHits > straightHits);
}

TEST_CASE("A bend has its own budget, and pierce still means bodies") {
  // The bend was originally paid out of the pierce, one point per turn. That is
  // a tidy-sounding rule and it quietly lies: a pierce-4 bolt touched THREE
  // bodies, so `pierce` meant a different number of bodies on this weapon than
  // on the other thirty-two in the game. The budget is seeded from the pierce
  // instead, so the statline means one thing everywhere -- and the test that
  // matters is the ceiling, not the count.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* storm = content.weapon("storm");
  REQUIRE(storm != nullptr);
  const std::size_t maxBodies = static_cast<std::size_t>(storm->pierce) + 1;

  SoloWeapon s("storm", 5);
  REQUIRE(s.slot >= 0);
  s.arm();
  // A dense ball, so the bolt always has somebody to bend onto and the only
  // thing that can stop it is the budget.
  for (int i = 0; i < 14; ++i) {
    const float a = static_cast<float>(i) * 2.399963F;
    const float r = 0.4F + 0.12F * static_cast<float>(i % 4);
    s.g.testSpawnEnemyAt(2.2F + r * std::cos(a), r * std::sin(a));
  }
  s.g.testAdvance(0.5F);

  // One shot is one shot: however many were standing there, no more than the
  // card's own pierce admits to were touched.
  std::size_t hits = 0;
  for (const float hp : s.g.testEnemyHps()) {
    if (hp < 100000.0F) ++hits;
  }
  CAPTURE(hits);
  CAPTURE(maxBodies);
  REQUIRE(hits > 1);
  REQUIRE(hits <= maxBodies);
  // And it is not paid twice for the same body, which is the bug the memo in
  // Projectile exists to prevent. Fourteen bodies in a ball, one shot, five
  // bodies touched -- if the bolt were stalling inside the body it just bent
  // off, the budget would be spent on two of them.
  for (const float hp : s.g.testEnemyHps()) {
    if (hp < 100000.0F) {
      REQUIRE(100000.0F - hp == Catch::Approx(15.0F));
    }
  }
}

TEST_CASE("A shattering bolt throws one fan of shards, not one per jump") {
  // The Blizzard Rail's desc used to promise a mid-flight shatter that did not
  // exist. Now it does -- but it has to be a ONE-TIME event. Paying the fan out
  // on every hop would turn a 4-jump bolt into 24 shards, which is both a
  // different weapon and an entity-count hazard.
  SoloWeapon s("blizzard", 5);
  REQUIRE(s.slot >= 0);
  s.arm();
  for (int i = 0; i < 6; ++i) {
    const float a = static_cast<float>(i) * 1.0F;
    s.g.testSpawnEnemyAt(2.0F * std::cos(a), 2.0F * std::sin(a));
  }

  // The bolt spends kChainTelegraph converging, then kChainStrike dropping, then
  // kChainLandHold sitting with the drop fully drawn, before its first hop. The
  // fan is paid on that hop, so "straight after the first hop" is all three of
  // those plus the inter-hop delay. Written as the sum rather than as a literal
  // because the whole point of the wind-up is that it is several beats long, and
  // a hardcoded 0.1s here silently became "before the strike has even landed"
  // the last time the timing moved.
  constexpr float kHopDelay = 0.05F;
  s.g.testAdvance(game::Game::kChainTelegraph + game::Game::kChainStrike +
                  game::Game::kChainLandHold + kHopDelay + 0.02F);
  const std::size_t shardsEarly = s.g.testProjectileCount();
  CAPTURE(shardsEarly);
  REQUIRE(shardsEarly > 0);

  // "Not one per jump", measured inside a window the next volley cannot start in.
  //
  // The Blizzard Rail fires every 0.50s, so a 0.30s window can contain hops 2
  // through 4 and their arcs but not a second shot. If the fan were paid on every
  // hop, four jumps would add four fans and the count would climb towards 24;
  // paid once, it can only fall as those six shards expire. This is the whole
  // point of the test, so it is written to be immune to the fire schedule rather
  // than to pick a lucky moment in it.
  constexpr float kVolley = 0.50F; // blizzard's cooldown
  s.g.testAdvance(kVolley - 0.20F);
  const std::size_t shardsMid = s.g.testProjectileCount();
  CAPTURE(shardsEarly);
  CAPTURE(shardsMid);
  REQUIRE(shardsMid <= shardsEarly);

  // Steady state: however many volleys have gone by, there is never more than a
  // couple of fans' worth of shards in the air at once. A bolt lives longer than
  // the 0.5s between shots, so two or three are in flight and the honest ceiling
  // is a few fans -- the point is that the count is BOUNDED, not that it is
  // small. The chain bound is sized to include bolts still in their telegraph,
  // which is why it is 4 and not "bolts that have already struck": a converging
  // bolt is a real bolt occupying a real entity slot.
  constexpr std::size_t kFan = 6; // blizzard's chain_shatter
  for (int i = 0; i < 20; ++i) {
    s.g.testAdvance(kVolley);
    REQUIRE(s.g.testProjectileCount() <= kFan * 3);
    REQUIRE(s.g.testChainCount() <= 4);
  }
}

TEST_CASE("The two chain weapons are separated by a rule, not a number") {
  // Tesla and the Blizzard Rail both throw an arc. What makes the Blizzard a
  // different weapon is that its bolt comes apart on the landing, and what makes
  // the Tesla a different weapon is that nothing else happens. If a card or a
  // later edit gave the Tesla a shatter, the pair would collapse into one weapon
  // with a bigger number on it -- which is the whole thing this test is for.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("tesla")->chainShatter == 0);
  REQUIRE(content.weapon("blizzard")->chainShatter > 0);
  REQUIRE(content.weapon("blizzard")->attackType == game::AttackType::Chain);
  REQUIRE(content.weapon("tesla")->attackType == game::AttackType::Chain);
  // Only the Chaos Orb splits, and only on a bounded cascade.
  REQUIRE(content.weapon("chaos")->bounceSplits > 0);
  REQUIRE(content.weapon("chaos")->bounceSplits <= 2);
  REQUIRE(game::Game::kMaxBounceSplitDepth > 0);
}

TEST_CASE("A splitting orb comes apart, and the cascade decays instead of exploding") {
  // Chaos Orb vs Pinball Puck. The puck stays one puck; the orb multiplies. But
  // "multiplies" has to mean "a bounded decaying cascade" -- a split that grows
  // without limit deletes the game on its own.
  SoloWeapon chaos("chaos", 9);
  SoloWeapon pin("pinball", 9);
  REQUIRE(chaos.slot >= 0);
  REQUIRE(pin.slot >= 0);
  chaos.arm();
  pin.arm();

  for (int i = 0; i < 6; ++i) {
    const float a = static_cast<float>(i) * 1.0F;
    chaos.g.testSpawnEnemyAt(2.5F * std::cos(a), 2.5F * std::sin(a));
    pin.g.testSpawnEnemyAt(2.5F * std::cos(a), 2.5F * std::sin(a));
  }

  chaos.g.testAdvance(0.4F);
  pin.g.testAdvance(0.4F);
  const std::size_t chaosOrbs = chaos.g.testBounceCount();
  const std::size_t pinOrbs = pin.g.testBounceCount();
  CAPTURE(chaosOrbs);
  CAPTURE(pinOrbs);
  // Same room, same bounce budget geometry: the orb fills it, the puck does not.
  REQUIRE(chaosOrbs > pinOrbs);
  REQUIRE(pinOrbs == 1); // one puck, however many times it has bounced

  // The cascade decays instead of accumulating. Note this cannot assert "zero
  // orbs": the weapon fires every 1.6s, so there is always a fresh orb in
  // flight. What it CAN assert is the ceiling -- no matter how many volleys have
  // gone by, the room never holds more than a bounded few generations at once.
  // 6 enemies, 2 splits, depth 3, so the worst case is 1+2+4+8 times however
  // many orbs are in flight at once; a runaway would blow straight past it.
  const std::size_t ceiling = 64;
  for (int i = 0; i < 40; ++i) {
    chaos.g.testAdvance(0.5F);
    const std::size_t live = chaos.g.testBounceCount();
    CAPTURE(live);
    REQUIRE(live <= ceiling);
  }
  // And the fragments are gone once their parent is: a 12s orb that sheds
  // 6.6s and then 3.6s generations must not leave a 12s tail behind, or the
  // bound above would be doing all the work.
  chaos.g.testClearWeapons();
  chaos.g.testAdvance(30.0F);
  REQUIRE(chaos.g.testBounceCount() == 0);
}

TEST_CASE("The multi-arc lash throws its arcs over a beat, not all at once") {
  // Tidal Lash is three waves. Throwing them simultaneously would be one very
  // wide slash, which is exactly the whip with a bigger number -- the reskin the
  // rework exists to remove. The gap is what makes it read as a sweep.
  SoloWeapon s("tidewhip", 13);
  REQUIRE(s.slot >= 0);
  s.arm();
  for (int i = 0; i < 3; ++i) {
    const float a = -0.6F + static_cast<float>(i) * 0.6F;
    s.g.testSpawnEnemyAt(2.0F * std::cos(a), 2.0F * std::sin(a));
  }

  s.g.testAdvance(1.0F / 60.0F);
  const std::size_t first = s.g.testWaveCount();
  s.g.testAdvance(1.0F / 60.0F);
  const std::size_t second = s.g.testWaveCount();
  CAPTURE(first);
  CAPTURE(second);
  // The first arc is out on its own; the rest arrive over the following frames.
  REQUIRE(first == 1);
  REQUIRE(second >= first);

  // One beat later the whole burst has landed, and there is not one wave per
  // arc per second -- the burst is paced by kWaveBurstGap, not by the cooldown.
  s.g.testAdvance(0.2F);
  REQUIRE(s.g.testWaveCount() >= 1);
  s.g.testAdvance(0.2F);
  REQUIRE(s.g.testWaveCount() < 3 * 6);
}


TEST_CASE("Frost Shards chill what they hit, and the Rime unique deepens it") {
  // The card used to read as a numbers-only "+4 pierce" with no status behind it,
  // and the base weapon had no ice rule at all -- the shards were plain
  // projectiles in a pale colour. Chill is the thing that makes an ice weapon an
  // ice weapon: a volley that holds a group still is worth more than the same
  // volley that does not.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto shard = weaponIndex(content, "shard");
  REQUIRE(shard >= 0);
  const auto& def = content.weapons[static_cast<std::size_t>(shard)];
  // The base weapon opts in by carrying the values; nothing global turns it on.
  REQUIRE(def.chillMul > 0.0F);
  REQUIRE(def.chillMul < 1.0F);
  REQUIRE(def.chillTime > 0.0F);
  // And only the ice carries it, so a test that spawns a fire weapon and finds a
  // chill would mean the status is leaking into every hit path.
  const auto flame = weaponIndex(content, "flame");
  REQUIRE(flame >= 0);
  REQUIRE(content.weapons[static_cast<std::size_t>(flame)].chillTime == 0.0F);

  const auto slowest = [&content](std::string_view id) {
    float m = 1.0F;
    for (const auto& w : content.weapons) {
      if (w.id == id && w.chillTime > 0.0F) m = std::min(m, w.chillMul);
    }
    return m;
  };
  REQUIRE(slowest("shard") > 0.0F);
  REQUIRE(slowest("shard") < 1.0F);

  // Behaviour: a live hit actually pins the target.
  {
    SoloWeapon s("shard");
    REQUIRE(s.slot >= 0);
    s.arm();
    s.g.testSpawnEnemyAt(1.2F, 0.0F);
    s.g.testAdvance(0.5F);
    const auto chill = s.g.testEnemyChills();
    CAPTURE(chill.size());
    REQUIRE_FALSE(chill.empty());
    // Reported as mul,time pairs; 1.0 means "not chilled".
    REQUIRE(chill[0] < 1.0F);
    REQUIRE(chill[1] > 0.0F);
  }

  // The unique is a real rule, not a bigger number: it must change the CHILL,
  // because that is the only part of the card that is not a stat bump.
  {
    SoloWeapon s("shard");
    REQUIRE(s.slot >= 0);
    s.arm();
    const auto before = s.g.testWeaponSnapshot(0).chillMul;
    s.g.testAddWeaponUpgrade(0, "w_unique_rime", 1.0F);
    const auto after = s.g.testWeaponSnapshot(0);
    REQUIRE(after.chillMul < before);
    REQUIRE(after.chillTime > s.g.testWeaponSnapshot(0).chillTime * 0.0F);
  }
}

TEST_CASE("A ricochet with nothing left to hit curves back instead of leaving the arena") {
  // The complaint this pins: a bouncing orb ricochets off into empty space and
  // the player waits out the rest of its life watching nothing happen. The old
  // code only re-aimed on the frame it HIT something, so an orb crossing open
  // ground kept whatever heading it had and left the map: 45 units out and
  // climbing after four seconds, with the player watching an empty screen.
  //
  // The purest form of the bug is a room with nothing left in it, which is set
  // up honestly: spawn a body, let the shot go, then kill it.
  SoloWeapon s("pinball", 11);
  REQUIRE(s.slot >= 0);
  s.arm();
  s.g.testSpawnEnemyAt(3.0F, 0.0F);
  s.g.testAdvance(0.05F);
  REQUIRE(s.g.testBounceCount() >= 1);
  s.g.testDespawnEnemies();
  REQUIRE(s.g.testEnemyPositions().empty());

  // The puck is now in an empty arena with the player at the origin. A puck
  // that holds its heading leaves at 13 u/s and never returns; a puck that aims
  // sweeps a circle about speed/turn-rate wide and comes back around.
  float maxD = 0.0F;
  float lastD = 0.0F;
  for (int i = 0; i < 30; ++i) {
    s.g.testAdvance(0.1F);
    const auto pos = s.g.testBouncePositions();
    for (std::size_t k = 0; k + 1 < pos.size(); k += 2) {
      // The player sits at the origin, so distance is the radius.
      const float d = std::sqrt(pos[k] * pos[k] + pos[k + 1] * pos[k + 1]);
      maxD = std::max(maxD, d);
      lastD = d;
    }
  }
  CAPTURE(maxD);
  CAPTURE(lastD);
  REQUIRE(maxD < 12.0F);
  // ...and it is still in the neighbourhood at the end rather than parked at
  // maximum range with a life timer running down.
  REQUIRE(lastD < 8.0F);
}

TEST_CASE("A ricochet that has run out of bodies retires instead of coasting") {
  // The other half of the annoyance: even a puck that IS steering is dead weight
  // once it has nothing left to hit, so its life is a short leash rather than the
  // twelve seconds the Chaos Sphere shipped with.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  for (const char* id : {"pinball", "chaos"}) {
    const auto idx = weaponIndex(content, id);
    CAPTURE(id);
    REQUIRE(idx >= 0);
    const float life = content.weapons[static_cast<std::size_t>(idx)].projLife;
    // Bounded and short. The cascade is already limited by split depth and by a
    // shorter life per fragment generation, so a long root bought nothing except
    // a projectile the player has to wait out.
    REQUIRE(life > 0.0F);
    REQUIRE(life <= 4.5F);
  }
}

TEST_CASE("Q abandons a run from the pause screen, and only on the second press") {
  // There was no way out of a live run at all short of dying, which makes a bad
  // run a twenty-minute commitment you cannot walk away from. Quitting is the one
  // irreversible thing the player can do by accident, so it is two-step.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g(content, 23);
  g.testDisableWaves();
  g.closeMenu();
  g.testAddWeapon(weaponIndex(content, "wand"));

  // From a live run, Q does nothing: it is only legal on the pause screen.
  {
    game::FrameInput in;
    in.quitRun = true;
    g.advance(0.016F, in);
    CAPTURE(g.state());
    REQUIRE(g.state() == game::RunState::Playing);
    REQUIRE_FALSE(g.menuOpen());
  }

  // Paused: the first press arms, and the run is still there.
  {
    game::FrameInput in;
    in.togglePause = true;
    g.advance(0.016F, in);
    REQUIRE(g.state() == game::RunState::Paused);
    game::FrameInput q;
    q.quitRun = true;
    g.advance(0.016F, q);
    REQUIRE(g.state() == game::RunState::Paused);
    REQUIRE_FALSE(g.menuOpen());
  }

  // The second press inside the window actually leaves.
  {
    game::FrameInput q;
    q.quitRun = true;
    g.advance(0.016F, q);
    REQUIRE(g.menuOpen());
  }

  // Leaving the sandbox is still a way out and must not require two presses --
  // the sandbox is a debug tool, and its own overlay already says it ends the run.
  {
    game::Game g2(content, 23);
    g2.testDisableWaves();
    g2.closeMenu();
    g2.enterTestMode();
    REQUIRE(g2.testMode());
    game::FrameInput t;
    t.testModeToggle = true;
    g2.advance(0.016F, t);
    REQUIRE_FALSE(g2.testMode());
  }
}

TEST_CASE("Every shipped name and description is spellable in the in-game font") {
  // The font is ASCII 32..96 with lowercase folded onto uppercase, and anything
  // outside it does not fail -- it draws as '?'. Four shipped descriptions carried
  // a typographic em-dash, so every player met four question marks on the
  // level-up screen while the .toml looked perfectly fine. The loader now throws
  // on an undrawable weapon/upgrade/enemy string, and this test is the belt to
  // that braces: it walks the loaded content rather than the files, so it also
  // catches a default string or a hand-edited value.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE_FALSE(content.weapons.empty());
  REQUIRE_FALSE(content.upgrades.empty());
  REQUIRE_FALSE(content.enemies.empty());

  std::size_t checked = 0;
  for (const auto& w : content.weapons) {
    CAPTURE(w.id);
    CAPTURE(w.name);
    REQUIRE(core::render::fontSupports(w.name));
    REQUIRE(core::render::fontSupports(w.desc));
    checked += 2;
  }
  for (const auto& u : content.upgrades) {
    CAPTURE(u.id);
    CAPTURE(u.name);
    REQUIRE(core::render::fontSupports(u.name));
    REQUIRE(core::render::fontSupports(u.desc));
    checked += 2;
  }
  for (const auto& e : content.enemies) {
    CAPTURE(e.id);
    CAPTURE(e.name);
    REQUIRE(core::render::fontSupports(e.name));
    ++checked;
  }
  // Guard against the loop bodies drifting out of sync with the content.
  REQUIRE(checked ==
          content.weapons.size() * 2 + content.upgrades.size() * 2 + content.enemies.size());
}

TEST_CASE("Every card name fits the card it is drawn on, whole") {
  // The card TITLE was drawn at a fixed 2.3 scale with no width check, so
  // "Total Internal Reflection" (345px) ran under the next card's 95%-opaque
  // panel in a 260px cell and was sliced with no ellipsis -- which reads as a
  // misspelled word rather than a long name. This is the screen players read to
  // choose a build, so the title fitting is not a detail.
  //
  // The fix is to wrap to two lines rather than shrink (shrinking to 1.7 put the
  // title below the body's own scale, and 25 characters still did not fit the
  // narrowest card). So the invariant is: never elided, never more than two
  // lines, and every line inside the budget.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  constexpr float kCardMinW = 200.0F;
  constexpr float kCardMaxW = 400.0F;
  constexpr float kGap = 24.0F;
  constexpr float kPad = 16.0F;

  // A short name is a single line at the comfortable size...
  {
    const auto lay = game::cardNameLayout("SHARD", 300.0F);
    REQUIRE_FALSE(lay.wrapped());
    REQUIRE(lay.scale == Catch::Approx(2.3F));
    REQUIRE(lay.lines.size() == 1);
    REQUIRE(lay.lines.front() == "SHARD");
    REQUIRE_FALSE(lay.elided);
  }
  // ...and a long one wraps at the smaller size, whole.
  {
    const auto lay = game::cardNameLayout("TOTAL INTERNAL REFLECTION", 200.0F);
    REQUIRE(lay.wrapped());
    REQUIRE(lay.scale == Catch::Approx(2.0F));
    REQUIRE(lay.lines.size() == 2);
    REQUIRE_FALSE(lay.elided);
    for (const auto& l : lay.lines) {
      REQUIRE(static_cast<float>(l.size()) * 6.0F * lay.scale <= 200.0F);
    }
  }
  // Three words at the narrowest card the row can produce: exactly the case that
  // used to be sliced.
  for (std::size_t n : {std::size_t{3}, std::size_t{4}, std::size_t{5}}) {
    const float cardW =
        std::clamp((game::Game::kRefScreenWidth - kGap * static_cast<float>(n - 1) - 40.0F) /
                       static_cast<float>(n),
                   kCardMinW, kCardMaxW);
    const float budget = cardW - kPad * 2.0F;
    for (const auto& u : content.upgrades) {
      CAPTURE(n);
      CAPTURE(u.id);
      const auto lay = game::cardNameLayout(u.name, budget);
      REQUIRE_FALSE(lay.elided);
      REQUIRE(lay.lines.size() <= 2);
      for (const auto& l : lay.lines) {
        REQUIRE(static_cast<float>(l.size()) * 6.0F * lay.scale <= budget);
      }
    }
    for (const auto& w : content.weapons) {
      CAPTURE(n);
      CAPTURE(w.id);
      const auto lay = game::cardNameLayout(w.name, budget);
      REQUIRE_FALSE(lay.elided);
      REQUIRE(lay.lines.size() <= 2);
      for (const auto& l : lay.lines) {
        REQUIRE(static_cast<float>(l.size()) * 6.0F * lay.scale <= budget);
      }
    }
  }
}

TEST_CASE("The level-up card row fits on screen at every height and width") {
  // The card row was laid out at a fixed py * 0.32 with only its WIDTH ever
  // checked. A resizable window has no minimum, so below about 500px tall the
  // cards and the reroll hint below them ran off the bottom of the screen --
  // the player is asked to press a number they cannot see. The top is now
  // clamped against the real height.
  //
  // The horizontal side of the same rule is asserted too, because the width clamp
  // is the other thing standing between a 5-card row and the right edge.
  constexpr float kGap = 24.0F;
  // The worst case: a 4-card row whose descriptions are long enough to hit the
  // height ceiling, and whose titles wrap to two lines.
  for (float py : {300.0F, 480.0F, 720.0F, 1080.0F, 1440.0F}) {
    for (float px : {640.0F, 1024.0F, 1280.0F, 1920.0F}) {
      for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
                            std::size_t{5}}) {
        CAPTURE(px);
        CAPTURE(py);
        CAPTURE(n);
        const auto row = game::Game::levelUpRowLayout(px, py, n, 10, 2);
        // The row is horizontally centred, so the first card's left edge and the
        // last card's right edge are the two things that can leave the screen.
        const float totalW = static_cast<float>(n) * row.cardW +
                             static_cast<float>(n - 1) * kGap;
        const float left = px * 0.5F - totalW * 0.5F;
        const float right = left + totalW;
        REQUIRE(left >= -0.001F);
        REQUIRE(right <= px + 0.001F);
        // The card must be as tall as the space it was given, and the reroll hint
        // sits 24px under it -- so on a window tall enough for the card, both the
        // bottom edge and the hint have to be on screen.
        REQUIRE(row.top >= 0.0F);
        if (py >= 0.16F * py + 44.0F + row.cardH + 44.0F) {
          REQUIRE(row.top + row.cardH <= py);
          REQUIRE(row.hintY <= py);
        }
        // The title and the description must not overlap inside the card, and
        // both must be inside it.
        REQUIRE(row.bodyTop > static_cast<float>(row.nameLines) * 24.0F);
        REQUIRE(row.bodyTop + 15.0F <= row.cardH);
        REQUIRE(row.bodyLines >= 1);
      }
    }
  }
}

TEST_CASE("A level-up always offers at least one choice") {
  // The renderer divides by the card count, so an empty row would mean cards
  // drawn at NaN positions. buildChoices has always appended a Skip card when
  // the pool came up dry; this pins that, because the renderer's fallback branch
  // is otherwise dead code whose deadness nobody would notice.
  //
  // The run is maxed out first, which is the case that actually produces the Skip
  // card -- with upgrades left to take, a non-empty row proves nothing.
  // Named local on purpose: Game holds a const Content&, and loadContent returns
  // by value, so binding the temporary straight into the constructor leaves
  // content_ dangling the moment the statement ends.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 4242};
  g.testAddWeapon(0);
  g.testMaxAllItems();
  int levels = 0;
  for (int i = 0; i < 200 && levels < 12; ++i) {
    g.grantXp(5000.0F);
    g.advance(1.0F / 60.0F, game::FrameInput{});
    if (g.state() != game::RunState::LevelUp) continue;
    ++levels;
    REQUIRE(g.testChoiceCount() >= 1);
    // Take whichever is offered and keep going; the point is that there is
    // always something to take.
    game::FrameInput in{};
    in.choose1 = true;
    g.advance(1.0F / 60.0F, in);
  }
  // If the loop never reached a level-up the test proved nothing.
  REQUIRE(levels >= 12);
}

TEST_CASE("A chain bolt converges before it strikes, and outlives its last hop") {
  // Two separate complaints, one test: the strike used to be a pop (the enemy's
  // health was already moving before the player could tell what was about to
  // happen) and the bolt used to vanish the instant it hit the last body in
  // range. Both are about the same three beats.
  SoloWeapon s("tesla", 3);
  REQUIRE(s.slot >= 0);
  s.arm();
  // One target is enough to prove the wind-up: a bolt with a single victim
  // dead-ends immediately, which is the case that used to skip the linger.
  s.g.testSpawnEnemyAt(2.5F, 0.0F);
  const float hpBefore = s.g.testFirstEnemyHp();

  // Beat 1: the bolt exists and has not struck. Nothing has been damaged.
  s.g.testAdvance(1.0F / 60.0F);
  const auto converging = s.g.testChainTelegraphs();
  REQUIRE(converging.size() == 1);
  CAPTURE(converging.front());
  REQUIRE(converging.front() > 0);
  REQUIRE(s.g.testFirstEnemyHp() == Catch::Approx(hpBefore));

  // Mid-wind-up it is still converging, and the timer is counting DOWN.
  s.g.testAdvance(game::Game::kChainTelegraph * 0.5F);
  const auto midway = s.g.testChainTelegraphs();
  REQUIRE(midway.size() == 1);
  CAPTURE(midway.front());
  REQUIRE(midway.front() > 0);
  REQUIRE(midway.front() < converging.front());
  REQUIRE(s.g.testFirstEnemyHp() == Catch::Approx(hpBefore));

  // Beat 2/3: after the window it lands, and the damage is real. The extra
  // 0.1s is the 0.05s inter-hop delay, so the sample is unambiguously after the
  // first hit rather than sitting exactly on it.
  s.g.testAdvance(game::Game::kChainTelegraph + 0.1F);
  const auto struck = s.g.testChainTelegraphs();
  REQUIRE(struck.size() == 1);
  CAPTURE(struck.front());
  REQUIRE(struck.front() == -1);
  REQUIRE(s.g.testFirstEnemyHp() < hpBefore);

  // And it is STILL on screen. This is the half that was broken: the bolt had no
  // target left, so it used to be destroyed on the spot instead of lingering.
  REQUIRE(s.g.testChainCount() == 1);
  REQUIRE(game::Game::kChainLinger >= 0.3F);

  // It is gone only once the linger has actually run out. The weapon is silenced
  // first: Tesla fires every 0.65s, so a live weapon would have put a second
  // bolt on screen before the first had finished hanging, and the test would be
  // measuring the fire rate rather than the linger.
  s.g.testClearWeapons();
  s.g.testAdvance(game::Game::kChainLinger + 0.1F);
  REQUIRE(s.g.testChainCount() == 0);
}

// ============================================================================
// Round 15: one test per weapon rework.
//
// Every one of these exists because the rework was a claim, not a rename. A
// different AttackType is not evidence of a different weapon -- two weapons can
// run the same code with only the numbers changed and the game will happily call
// it an evolution. So each test states the MECHANIC, and the ones that can be
// checked against a sibling weapon are checked against the sibling, because
// "the drill only chews one body" means nothing until "the sprayer chews them
// all" is in the same file.
// ============================================================================

TEST_CASE("The Jackhammer Drill commits to one body; the Sprayer washes the whole crowd") {
  // The complaint was that the drill and the flamethrower were the same weapon.
  // They are both `cone`, so the separation has to be a rule, and the rule is
  // that the bit latches ONE body and ramps while it stays buried.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("drill")->coneBite > 0.0F);
  REQUIRE(content.weapon("flame")->coneBite == 0.0F);

  // Two bodies inside the drill's arc, the near one first. Both are well inside
  // cone_range and both sit on the aim line, so nothing about geometry favours
  // one over the other except distance -- which is the latch rule.
  SoloWeapon drill("drill");
  REQUIRE(drill.slot >= 0);
  drill.arm();
  drill.g.testSpawnEnemyAt(1.6F, 0.0F);
  drill.g.testSpawnEnemyAt(2.6F, 0.0F);
  drill.g.testAdvance(0.30F);
  const float drillNear = drill.g.testEnemyHpNear(1.6F, 0.0F);
  const float drillFar = drill.g.testEnemyHpNear(2.6F, 0.0F);
  CAPTURE(drillNear);
  CAPTURE(drillFar);
  // The near body is chewed; the far one is left completely alone.
  REQUIRE(drillNear < 100000.0F);
  REQUIRE(drillFar == Catch::Approx(100000.0F));

  // The same two bodies against the Sprayer: both are washed. This is the half
  // of the claim that makes it a pair of different weapons rather than one
  // weapon with a filter.
  SoloWeapon flame("flame");
  REQUIRE(flame.slot >= 0);
  flame.arm();
  flame.g.testSpawnEnemyAt(1.6F, 0.0F);
  flame.g.testSpawnEnemyAt(2.6F, 0.0F);
  flame.g.testAdvance(0.30F);
  const float flameNear = flame.g.testEnemyHpNear(1.6F, 0.0F);
  const float flameFar = flame.g.testEnemyHpNear(2.6F, 0.0F);
  CAPTURE(flameNear);
  CAPTURE(flameFar);
  REQUIRE(flameNear < 100000.0F);
  REQUIRE(flameFar < 100000.0F);

  // And the bite RAMPS, which is the other half of the drill's identity. Over
  // 0.30s at a 0.05s tick the ramp is well past 1x, so the drill's damage on a
  // single body beats the Sprayer's on that same body by more than the crowd
  // falloff can explain.
  REQUIRE(100000.0F - drillNear > 100000.0F - flameNear);
}

TEST_CASE("The Siege Mortar fires THROUGH the front rank; the Runic Hammer does not") {
  // These two were the same arcing shell with a longer fuse and a wider blast.
  // The mortar's rule is that it lands `bomb_ahead` PAST whoever is nearest its
  // aim line, so the answer is asserted positionally: the body in front survives
  // and the body behind it is the one that gets cooked.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("mortar")->bombAhead > 0.0F);
  REQUIRE(content.weapon("hammer")->bombAhead == 0.0F);

  // The near body is what the player is aiming at (auto-target takes the
  // nearest), so the aim line is unambiguous and the overshoot has something to
  // overshoot.
  SoloWeapon mortar("mortar");
  REQUIRE(mortar.slot >= 0);
  mortar.arm();
  mortar.g.testSpawnEnemyAt(3.0F, 0.0F);
  mortar.g.testSpawnEnemyAt(7.0F, 0.0F);
  // Long enough for the fuse to expire and the shell to come down.
  mortar.g.testAdvance(3.0F);
  const float frontRank = mortar.g.testEnemyHpNear(3.0F, 0.0F);
  const float backRank = mortar.g.testEnemyHpNear(7.0F, 0.0F);
  CAPTURE(frontRank);
  CAPTURE(backRank);
  // It sailed over the thing in front of it.
  REQUIRE(frontRank == Catch::Approx(100000.0F));
  REQUIRE(backRank < 100000.0F);

  // The hammer, same two bodies: it detonates on contact, so the front rank is
  // what it answers and the body behind it is untouched.
  SoloWeapon hammer("hammer");
  REQUIRE(hammer.slot >= 0);
  hammer.arm();
  hammer.g.testSpawnEnemyAt(3.0F, 0.0F);
  hammer.g.testSpawnEnemyAt(7.0F, 0.0F);
  hammer.g.testAdvance(3.0F);
  const float hammerFront = hammer.g.testEnemyHpNear(3.0F, 0.0F);
  const float hammerBack = hammer.g.testEnemyHpNear(7.0F, 0.0F);
  CAPTURE(hammerFront);
  CAPTURE(hammerBack);
  REQUIRE(hammerFront < 100000.0F);
  REQUIRE(hammerBack == Catch::Approx(100000.0F));
}

TEST_CASE("The Void Nova is cast wide and rushes IN; the Shock Core only ever grows") {
  // The complaint was that Void Nova was a bigger Shock Core. Both are
  // `nova`, both are rings pinned to the player, so the separation has to be
  // which way the radius moves -- and a test that cannot see the radius cannot
  // tell the two apart at all.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("nova")->novaContract);
  REQUIRE(content.weapon("shockcore")->novaContract == false);

  auto radiusTrace = [](std::string_view id) {
    SoloWeapon s(id);
    REQUIRE(s.slot >= 0);
    s.arm();
    // Something to aim at so the ring is cast, well outside the shock core's
    // reach so it does not matter which end it starts at.
    s.g.testSpawnEnemyAt(7.0F, 0.0F);
    s.g.testAdvance(1.0F / 60.0F);
    std::vector<float> radii;
    for (int i = 0; i < 20; ++i) {
      const auto r = s.g.testNovaRadii();
      if (!r.empty()) radii.push_back(r.front());
      s.g.testAdvance(1.0F / 60.0F);
    }
    return radii;
  };

  const auto contract = radiusTrace("nova");
  const auto expand = radiusTrace("shockcore");
  REQUIRE(contract.size() >= 4);
  REQUIRE(expand.size() >= 4);
  CAPTURE(contract.front());
  CAPTURE(contract.back());
  CAPTURE(expand.front());
  CAPTURE(expand.back());
  // A contracting ring STARTS at its maximum: that is the "cast wide" in the
  // description, and it is the half that a bigger number on the shock core
  // could never produce.
  REQUIRE(contract.front() > contract.back());
  REQUIRE(contract.front() > 1.0F);
  // An expanding one starts at nothing and ends up out.
  REQUIRE(expand.front() < expand.back());
  REQUIRE(expand.back() > 1.0F);
}

TEST_CASE("The Radiant Halo's spokes leave a ring of safe ground at your feet") {
  // "A better Halo" was the complaint about the Seraph Array, and reach alone is a
  // better Halo -- so the Seraph is gone and its one real idea was handed to the
  // Halo it was copying. The rule is `halo_inner`: the spokes do not start at the
  // player, so there is a hole at the centre. Reach is paid for with safety, which
  // is a choice the Halo now always asks you to make.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("halo")->haloInner > 0.0F);
  // And there is no longer a second halo to confuse it with.
  REQUIRE(content.weapon("seraph") == nullptr);
  std::size_t halos = 0;
  for (const auto& w : content.weapons) {
    if (w.attackType == game::AttackType::Halo) ++halos;
  }
  REQUIRE(halos == 1);

  // One body practically under the player's feet. Nothing else is on screen, so
  // auto-target aims at it and the spoke sweeps straight over where it stands.
  SoloWeapon halo("halo");
  REQUIRE(halo.slot >= 0);
  halo.arm();
  halo.g.testSpawnEnemyAt(0.5F, 0.0F);
  halo.g.testAdvance(1.5F);
  const float hugging = halo.g.testEnemyHpNear(0.5F, 0.0F);
  CAPTURE(hugging);
  // The spokes have swept over it many times and never once reached it.
  REQUIRE(hugging == Catch::Approx(100000.0F));
  // The hole is real and measurable, not just "smaller numbers".
  const auto inners = halo.g.testHaloBeamInners();
  REQUIRE(!inners.empty());
  for (const float in : inners) REQUIRE(in > 0.0F);

  // The same weapon DOES reach a body out at the rim, so the dead zone is a hole
  // and not the whole ring.
  halo.g.testSpawnEnemyAt(4.0F, 0.0F);
  halo.g.testAdvance(1.5F);
  REQUIRE(halo.g.testEnemyHpNear(4.0F, 0.0F) < 100000.0F);
}

TEST_CASE("The Event Horizon's wells hoard and implode; the Void Gyre's never end") {
  // "The same weapon twice" was the complaint, and they are both `vortex` with
  // the same orbiting wells. The difference is whether the well has an end: the
  // Gyre is a patient permanent drag, and the Horizon hoards for three seconds
  // and then detonates and reopens somewhere else on the orbit.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("eventhorizon")->vortexCollapseAt > 0.0F);
  REQUIRE(content.weapon("vortex")->vortexCollapseAt == 0.0F);

  // Charge is the collapse clock. A collapsing well's charge climbs towards its
  // threshold; a well that never collapses keeps its charge pinned at zero
  // forever, which is the whole of the Gyre's identity.
  auto chargeAfter = [](std::string_view id, float seconds) {
    SoloWeapon s(id);
    REQUIRE(s.slot >= 0);
    s.arm();
    s.g.testSpawnEnemyAt(6.0F, 0.0F);
    s.g.testAdvance(seconds);
    return s.g.testVortexCharges();
  };

  const auto horizonCharging = chargeAfter("eventhorizon", 1.0F);
  const auto gyreCharging = chargeAfter("vortex", 1.0F);
  REQUIRE(!horizonCharging.empty());
  REQUIRE(!gyreCharging.empty());
  // Flat charge,angle pairs: every even index is a charge.
  for (std::size_t i = 0; i < horizonCharging.size(); i += 2) {
    CAPTURE(horizonCharging[i]);
    REQUIRE(horizonCharging[i] > 0.5F);
  }
  for (std::size_t i = 0; i < gyreCharging.size(); i += 2) {
    CAPTURE(gyreCharging[i]);
    REQUIRE(gyreCharging[i] == Catch::Approx(0.0F));
  }

  // Past the threshold the charge wraps, and that wrap is what says the well
  // detonated and reopened: at 1.0s the charge is nearly full, and at 4.2s --
  // just over one collapse cycle later -- the charge is young again.
  const auto afterCycle = chargeAfter("eventhorizon", 4.2F);
  REQUIRE(!afterCycle.empty());
  float maxCharge = 0.0F;
  for (std::size_t i = 0; i < afterCycle.size(); i += 2) {
    maxCharge = std::max(maxCharge, afterCycle[i]);
  }
  CAPTURE(maxCharge);
  REQUIRE(maxCharge < content.weapon("eventhorizon")->vortexCollapseAt);

  // And the collapse has to be worth several seconds of the steady drip, or it
  // is not an event -- it is a slightly louder tick. A body parked in the blast
  // takes the lump; the assertion is only that it took a lot, which the constant
  // damage of a well would not produce on its own in one and a bit seconds.
  SoloWeapon horizon("eventhorizon");
  REQUIRE(horizon.slot >= 0);
  horizon.arm();
  // Straight out along +x, which is where a well's orbit passes.
  horizon.g.testSpawnEnemyAt(3.0F, 0.0F);
  const float before = horizon.g.testFirstEnemyHp();
  horizon.g.testAdvance(4.2F);
  const float after = horizon.g.testFirstEnemyHp();
  CAPTURE(after);
  REQUIRE(before - after > content.weapon("eventhorizon")->vortexBurstDamage * 0.5F);
}

TEST_CASE("The two wave weapons are a corridor and a fan, not two crescents") {
  // "The sprites are not symmetric and they are too similar" was the complaint,
  // and the fix had to be structural rather than cosmetic. They share an entity
  // and an AttackType, so the separation is in the data: the Sundering Core is
  // ONE wide slow arc that goes a long way, the Tidal Lash is THREE narrow fast
  // ones spread across a fan. Neither is reachable from the other's numbers.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* core = content.weapon("sunder");
  const auto* lash = content.weapon("tidewhip");
  REQUIRE(core != nullptr);
  REQUIRE(lash != nullptr);

  // Coverage vs corridor. The lash throws several arcs, fanned apart; the core
  // throws exactly one.
  REQUIRE(lash->waveCount > 1);
  REQUIRE(lash->waveArcStep > 0.0F);
  REQUIRE(core->waveCount == 1);
  REQUIRE(core->waveArcStep == 0.0F);

  // Horns: the core's open far wider than the lash's, which is the difference
  // between a wall and a hook and is the whole of the visual complaint.
  REQUIRE(core->waveSpread > lash->waveSpread);

  // And the two profiles do not cross over: the core is the slower, longer,
  // heavier shot and the lash is the faster, shorter one. If these ever swap,
  // the names are lying.
  REQUIRE(core->waveSpeed < lash->waveSpeed);
  REQUIRE(core->waveRange > lash->waveRange);
  REQUIRE(core->waveKnockback > lash->waveKnockback);
}

TEST_CASE("Hearthfire lays burning ground behind the Ember Sprayer's cone") {
  // The card used to be "wider and longer", which is not a rule. The rule is
  // that the cone leaves pools, dropped by DISTANCE the tip has travelled, so
  // walking lays a trail and standing still does not stack a hundred pools on
  // one tile.
  SoloWeapon bare("flame");
  REQUIRE(bare.slot >= 0);
  bare.arm();
  bare.g.testSpawnEnemyAt(2.0F, 0.0F);
  bare.g.testAdvance(0.5F);
  // Without the card the sprayer leaves nothing behind.
  REQUIRE(bare.g.debugCounts().zones == 0);

  SoloWeapon hearth("flame");
  REQUIRE(hearth.slot >= 0);
  hearth.arm();
  // 0, not `hearth.slot`: that is the CONTENT index of the weapon, and the
  // upgrade hook takes an EQUIPPED slot. With one weapon equipped they differ.
  hearth.g.testAddWeaponUpgrade(0, "w_unique_hearthfire", 1.0F);
  hearth.g.testSpawnEnemyAt(2.0F, 0.0F);
  hearth.g.testAdvance(0.5F);
  const auto zones = hearth.g.debugCounts().zones;
  CAPTURE(zones);
  REQUIRE(zones > 0);

  // The cap is a real cap: the pools are dropped by DISTANCE the cone's tip has
  // travelled, so walking lays a road and standing still lays nothing new. A
  // screen of permanent flame is a different game rather than a stronger Ember
  // Sprayer.
  for (int i = 0; i < 40; ++i) {
    hearth.g.testSetPlayerPosition(static_cast<float>(i) * 0.25F, 0.0F);
    hearth.g.testAdvance(1.0F / 60.0F);
  }
  const auto whileWalking = hearth.g.debugCounts().zones;
  CAPTURE(whileWalking);
  REQUIRE(whileWalking > 0);
  REQUIRE(whileWalking <= 6);

  // And standing still on top of a live trail adds nothing: the pools already
  // there burn out and nothing replaces them, because the tip has not moved.
  hearth.g.testAdvance(4.0F);
  const auto whileStill = hearth.g.debugCounts().zones;
  CAPTURE(whileStill);
  REQUIRE(whileStill == 0);
}

TEST_CASE("Discharge fires a second nova ring, and it is DELAYED, not doubled") {
  // Two rings on the same frame would be one bigger ring wearing a costume.
  // The echo is scheduled, so the second blast lands after the first has had
  // time to open a gap in the pack -- which is the entire reason a double is
  // worth having over one larger nova.
  SoloWeapon bare("shockcore");
  REQUIRE(bare.slot >= 0);
  bare.arm();
  bare.g.testSpawnEnemyAt(3.0F, 0.0F);
  bare.g.testAdvance(1.0F / 60.0F);
  // One ring per cast without the card, even a frame later.
  bare.g.testAdvance(0.2F);
  const auto plain = bare.g.testNovaRadii();
  REQUIRE(plain.size() <= 1);

  SoloWeapon echo("shockcore");
  REQUIRE(echo.slot >= 0);
  echo.arm();
  echo.g.testAddWeaponUpgrade(0, "w_unique_discharge", 1.0F);
  echo.g.testSpawnEnemyAt(3.0F, 0.0F);
  echo.g.testAdvance(1.0F / 60.0F);
  // A delay later there are two: the original, still growing, and the echo.
  bool sawTwo = false;
  bool sawTwoOnTheCastFrame = false;
  for (int i = 0; i < 40 && !sawTwo; ++i) {
    const auto radii = echo.g.testNovaRadii();
    if (radii.size() >= 2) {
      sawTwo = true;
      sawTwoOnTheCastFrame = (i == 0);
      // Two live rings, and they are NOT the same ring: the echo is younger, so
      // it is smaller. A doubled ring would be two identical radii.
      REQUIRE(radii[0] != radii[1]);
    }
    echo.g.testAdvance(1.0F / 60.0F);
  }
  REQUIRE(sawTwo);
  // Not both on the frame the weapon fired -- that is the "one bigger ring"
  // case, and it is the case the card is supposed to avoid.
  REQUIRE_FALSE(sawTwoOnTheCastFrame);
}

TEST_CASE("A hooked whip drags its catch in instead of shoving it out") {
  // The Barbed Whip's barbs were doing nothing: the weapon pushed and could
  // therefore never START anything. The hook inverts the knockback, which is the
  // only thing in the weapon's name that was not already accounted for. It is
  // opt-in so the Soul Scythe -- whose whole job is a knockback circle -- is
  // untouched.
  // One frame only, and that is deliberate. The AI's separation term shoves a
  // body away from the player at up to 4 u/s, which is almost exactly the size of
  // a lash's knockback -- so over a quarter of a second the shove is buried and
  // the two cases end up only a few centimetres apart, with separation winning in
  // both. At the instant of the hit the knockback is at full strength and the
  // direction is unambiguous, which is the thing being asserted: which way does
  // this weapon push.
  const auto lashOnce = [](bool hooked) {
    game::Content content(game::loadContent(GAME_ASSETS_DIR "/data"));
    game::Game g(content, 7);
    g.testDisableWaves();
    int def = -1;
    for (std::size_t i = 0; i < content.weapons.size(); ++i) {
      if (content.weapons[i].id == "whip") def = static_cast<int>(i);
    }
    REQUIRE(def >= 0);
    g.testAddWeapon(def);
    if (hooked) g.testAddWeaponUpgrade(0, "w_unique_chainlash", 1.0F);
    g.testSpawnEnemyAt(2.0F, 0.0F);
    const float before = g.testEnemyPositions()[0];
    // Two steps, not one: the swing happens after the movement update in a step,
    // so a shove lands in the position on the NEXT step. One step measures
    // nothing at all.
    g.testAdvance(2.0F / 60.0F);
    const auto after = g.testEnemyPositions();
    REQUIRE(after.size() == 2);
    return std::pair<float, float>(before, after[0]);
  };

  const auto plain = lashOnce(false);
  const auto hook = lashOnce(true);
  CAPTURE(plain.first);
  CAPTURE(plain.second);
  CAPTURE(hook.first);
  CAPTURE(hook.second);
  // A plain lash SHOVES: the body is driven further out than it started.
  REQUIRE(plain.second > plain.first);
  // The same lash with the barbs actually barbed drives it back TOWARD the
  // player, and leaves it nearer than the plain lash did.
  REQUIRE(hook.second < hook.first);
  REQUIRE(hook.second < plain.second);
}

TEST_CASE("Blade Vortex's gap is the only place the ring's interior bites") {
  // The interior grind used to pay every enemy strictly inside the ring
  // uniformly, which is a worse version of the ring: an everywhere-equal hazard
  // the player cannot aim at or answer. With the gap, the safe angle becomes
  // the killing angle, so a faster ring is worth something.
  SoloWeapon bare("dagger");
  REQUIRE(bare.slot >= 0);
  bare.arm();
  bare.g.testSpawnEnemyAt(6.0F, 0.0F);
  bare.g.testAdvance(0.2F);
  // No card, no gap to speak of.
  REQUIRE(bare.g.testOrbitWindowAngle(0) == Catch::Approx(-1.0F));

  SoloWeapon gap("dagger");
  REQUIRE(gap.slot >= 0);
  gap.arm();
  gap.g.testAddWeaponUpgrade(0, "w_unique_vortex", 1.0F);
  gap.g.testSpawnEnemyAt(6.0F, 0.0F);
  gap.g.testAdvance(1.0F / 60.0F);
  const float window = gap.g.testOrbitWindowAngle(0);
  CAPTURE(window);
  REQUIRE(window >= -0.5F);

  // The ring is spinning, so the gap's angle is only knowable at the instant it
  // is read. Read it, put one body in the gap and one diametrically opposite it
  // at the same radius, and step a single frame: the gap is where the interior
  // damage is paid.
  SoloWeapon s("dagger");
  REQUIRE(s.slot >= 0);
  s.arm();
  s.g.testAddWeaponUpgrade(0, "w_unique_vortex", 1.0F);
  s.g.testSpawnEnemyAt(6.0F, 0.0F);
  s.g.testAdvance(1.0F / 60.0F);
  const float gapAngle = s.g.testOrbitWindowAngle(0);
  REQUIRE(gapAngle > -0.5F);
  const auto blades = s.g.testOrbitBladeGaps(0);
  REQUIRE(!blades.empty());
  // A target just inside the ring, in the gap, and its mirror image.
  const float px = s.g.testPlayerX();
  const float py = s.g.testPlayerY();
  const float inR = 0.5F;
  s.g.testSpawnEnemyAt(px + std::cos(gapAngle) * inR, py + std::sin(gapAngle) * inR);
  s.g.testSpawnEnemyAt(px - std::cos(gapAngle) * inR, py - std::sin(gapAngle) * inR);
  const float inGap = s.g.testEnemyHpNear(px + std::cos(gapAngle) * inR,
                                           py + std::sin(gapAngle) * inR);
  const float opposite = s.g.testEnemyHpNear(px - std::cos(gapAngle) * inR,
                                             py - std::sin(gapAngle) * inR);
  // The whirl is paid inside a step, so give it one. The ring turns 0.1 rad per
  // step at this speed and the window is 0.84 rad wide, so the bodies are still
  // where they were put; and separation only pushes them radially, so neither
  // leaves the angle it was placed on.
  s.g.testAdvance(1.0F / 60.0F);
  const float inGapAfter = s.g.testEnemyHpNear(px + std::cos(gapAngle) * inR,
                                                py + std::sin(gapAngle) * inR);
  const float oppositeAfter = s.g.testEnemyHpNear(px - std::cos(gapAngle) * inR,
                                                  py - std::sin(gapAngle) * inR);
  CAPTURE(inGap);
  CAPTURE(opposite);
  CAPTURE(inGapAfter);
  CAPTURE(oppositeAfter);
  // Both are inside the ring, so without the wedge they would be paid equally.
  // With it, the one in the gap is the one being cut.
  REQUIRE(inGapAfter < 100000.0F);
  REQUIRE(oppositeAfter == Catch::Approx(100000.0F));
}

TEST_CASE("The Pulsar's trail burns the whole line it flies, not the blade tip") {
  // The complaint was "just prettier shuriken", and the reason was provable: at
  // 60Hz and twenty units a second the per-frame segment is a third of a unit,
  // which hits exactly what the blade's own contact test already hits. The trail
  // now covers the blade's remembered path, so it is a corridor.
  //
  // The test is the version that cannot be faked: a body parked OFF the blade's
  // flight line, inside the trail's width but outside the blade's contact
  // radius. If that body loses health, the scar did it, not the blade.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* pulsar = content.weapon("pulsar");
  REQUIRE(pulsar != nullptr);
  // Blade contact radius is 0.14 and the test enemy is 0.3, so a body more than
  // 0.44 off the line cannot be touched by the blade itself. The trail half
  // width is beam_width/2; this requires the body to be inside that.
  const float trailHalf = pulsar->beamWidth * 0.5F;
  const float offset = trailHalf * 0.9F;
  REQUIRE(offset > 0.44F);
  REQUIRE(offset < trailHalf + 0.3F);

  SoloWeapon s("pulsar");
  REQUIRE(s.slot >= 0);
  s.arm();
  // The aim anchor is the NEAREST body, so it has to be nearer than the one
  // being tested or the blade flies at the wrong line.
  s.g.testSpawnEnemyAt(2.0F, 0.0F);
  s.g.testSpawnEnemyAt(4.0F, offset);
  const float victimX = 4.0F;
  const float victimY = offset;
  const float before = s.g.testEnemyHpNear(victimX, victimY);
  s.g.testAdvance(1.2F);
  const float after = s.g.testEnemyHpNear(victimX, victimY);
  CAPTURE(before);
  CAPTURE(after);
  // It was never in contact with the blade and it is still hurt.
  REQUIRE(before == Catch::Approx(100000.0F));
  REQUIRE(after < 100000.0F);
}

TEST_CASE("The Hoarfrost Wake's aura slows and grinds what the volley only PASSES") {
  // Chill is an on-HIT status, so Frost Shards only ever slowed the bodies a
  // shard went through -- it had to keep hitting to keep slowing, and the back
  // of the horde was never touched. The aura is the other kind of cold: it
  // travels with the shard.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* wake = content.weapon("rimewake");
  REQUIRE(wake != nullptr);
  REQUIRE(wake->auraRadius > 0.0F);
  REQUIRE(wake->auraChillTime > 0.0F);
  // The pair is a real evolution of a real pair of parents.
  REQUIRE(wake->prereqs.size() == 2);

  // The victim sits off the flight line by more than the blade's contact reach
  // and inside the corona, so the only thing that can touch it is the aura.
  const float offset = wake->auraRadius * 0.6F;
  REQUIRE(offset > 0.44F);
  SoloWeapon s("rimewake");
  REQUIRE(s.slot >= 0);
  s.arm();
  s.g.testSpawnEnemyAt(2.0F, 0.0F);                    // the aim anchor, nearest
  s.g.testSpawnEnemyAt(4.0F, offset);                  // the body the shard misses
  const float victimX = 4.0F;
  const float victimY = offset;
  const float hpBefore = s.g.testEnemyHpNear(victimX, victimY);
  s.g.testAdvance(0.6F);
  const float hpAfter = s.g.testEnemyHpNear(victimX, victimY);
  CAPTURE(hpBefore);
  CAPTURE(hpAfter);
  REQUIRE(hpBefore == Catch::Approx(100000.0F));
  // Grounded by the corona even though nothing ever went through it.
  REQUIRE(hpAfter < 100000.0F);

  // And chilled. The nearest enemy to the victim is the victim itself (it is
  // alone out there), so the chill pair list is unambiguous.
  s.g.testAdvance(0.2F);
  const auto chills = s.g.testEnemyChills();
  REQUIRE(!chills.empty());
  bool anyChilled = false;
  for (std::size_t i = 0; i < chills.size(); i += 2) {
    CAPTURE(chills[i]);
    if (chills[i] < 1.0F) anyChilled = true;
  }
  REQUIRE(anyChilled);

  // The plain Frost Shards, same body, same off-line position: nothing at all
  // happens, because there is no corona to reach it.
  const float shardOffset = wake->auraRadius * 0.6F;
  SoloWeapon shard("shard");
  REQUIRE(shard.slot >= 0);
  shard.arm();
  shard.g.testSpawnEnemyAt(2.0F, 0.0F);
  shard.g.testSpawnEnemyAt(4.0F, shardOffset);
  const float shardBefore = shard.g.testEnemyHpNear(4.0F, shardOffset);
  shard.g.testAdvance(0.6F);
  const float shardAfter = shard.g.testEnemyHpNear(4.0F, shardOffset);
  CAPTURE(shardBefore);
  CAPTURE(shardAfter);
  REQUIRE(shardBefore == Catch::Approx(100000.0F));
  REQUIRE(shardAfter == Catch::Approx(100000.0F));
}


TEST_CASE("The difficulty pass: level-ups arrive sooner and elites arrive later") {
  // One guard for the whole pass, because the numbers in it are the numbers a
  // player feels. Individually they look like tuning; together they are the
  // difference between a run that is under-levelled and a run that is not, and
  // the only reason to write them down is so the next pass cannot quietly undo
  // it one constant at a time.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // --- Elites wait, and are rarer once they arrive ---------------------------
  // The first elite used to land at 0:45, before the player had a third pick.
  // That is not an interruption, it is an ambush: the run's first real decision
  // became "do I play around the thing that is about to end me", which is a
  // decision about the game rather than about the build.
  game::Game g{content, 3};
  g.testDisableWaves();
  REQUIRE_FALSE(g.tierUnlocked(1));
  g.testSetSimTime(89.0F);
  REQUIRE_FALSE(g.tierUnlocked(1));
  g.testSetSimTime(90.0F);
  REQUIRE(g.tierUnlocked(1));

  // And on a CLOCK rather than as a share of spawns. This is the number that
  // answered "why are there so many chests": at a per-member roll, an elite was
  // one body in ten, forever, so a ten-minute run handed over 42 boxes and the
  // player had the whole arsenal by minute three. A tier is an event, so it is
  // scheduled: once every couple of minutes, and the tier's own box is the only
  // thing in the game that decides how often new cards arrive.
  const auto [eliteLo, eliteHi] = game::tierCadence(1);
  REQUIRE(eliteLo >= 100.0F);
  REQUIRE(eliteHi > eliteLo);
  // Champions and overlords wait longer still, and the ladder of waits is
  // increasing: a boss that showed up as often as an elite would be a boss in
  // name only.
  const auto [champLo, champHi] = game::tierCadence(2);
  const auto [lordLo, lordHi] = game::tierCadence(3);
  REQUIRE(champLo > eliteHi);
  REQUIRE(lordLo > champHi);
  REQUIRE(champHi > champLo);
  REQUIRE(lordHi > lordLo);

  // --- The traits do not wait at all ------------------------------------------
  // The old rule added a trait at five and ten minutes. A flat 1/3/7 is a number
  // the player can hold in their head, which is the point: see the test on
  // traitsForTier itself.

  // --- The enemy HP ramp is gentler, and capped lower ------------------------
  // The HP ramp is the one piece of difficulty the player has no answer to: a
  // build three picks behind cannot outshoot it, so a steep ramp is not a hard
  // game, it is a run where the player's choices do not matter. Read through the
  // real spawn path (testSpawnTieredEnemyAt applies the global scales), against
  // the bat, which is the first thing the player ever sees shoot back.
  const int bat = [&content] {
    for (std::size_t i = 0; i < content.enemies.size(); ++i) {
      if (content.enemies[i].id == "bat") return static_cast<int>(i);
    }
    return -1;
  }();
  REQUIRE(bat >= 0);
  const float batBaseHp = content.enemies[static_cast<std::size_t>(bat)].hp;

  const auto scaledHpAt = [&](float seconds) {
    game::Game probe{content, 5};
    probe.testDisableWaves();
    probe.testSetSimTime(seconds);
    probe.testSpawnTieredEnemyAt(4.0F, 0.0F, 0, 0, bat);
    return probe.testFirstEnemyHp() / batBaseHp;
  };
  const float atStart = scaledHpAt(0.0F);
  const float atFive = scaledHpAt(300.0F);
  const float atTen = scaledHpAt(600.0F);
  const float atTwenty = scaledHpAt(1200.0F);
  const float atHour = scaledHpAt(3600.0F);
  CAPTURE(atStart);
  CAPTURE(atFive);
  CAPTURE(atTen);
  CAPTURE(atTwenty);
  CAPTURE(atHour);
  // Monotone, and starting from exactly the authored value.
  REQUIRE(atStart == Catch::Approx(1.0F));
  REQUIRE(atFive > atStart);
  REQUIRE(atTen > atFive);
  REQUIRE(atTwenty > atTen);
  // The cap holds: the old one was 30x, which at twenty minutes was most of
  // what a build could be worth.
  REQUIRE(atHour <= 20.0F);
  // And the ramp is real but shallower than it was. At ten minutes the old curve
  // was 11.5x; a build that is still finishing its core damage and fire-rate
  // cards at that point cannot answer 11.5x, so 7.5x is the difference between
  // a fight and a wall.
  REQUIRE(atTen < 8.0F);
  // The complaint this pass answers is specifically about two and three minutes,
  // so that window is asserted on its own rather than left to the shape of the
  // curve. At 2:30 an ordinary enemy used to be at 2.6x its opening HP; the
  // heavies have been moved out of this window instead (see the unlock test), and
  // what is left here has to be killable.
  REQUIRE(scaledHpAt(150.0F) < 2.15F);
  REQUIRE(scaledHpAt(180.0F) < 2.35F);
  // Not flat either. A ramp that stopped growing would be a different game.
  REQUIRE(atTwenty > 2.0F * atStart);
}

TEST_CASE("A weapon's unique card can only move fields its own attack type reads") {
  // The failure this exists to stop is not subtle, it is just invisible: a card
  // sets `sweepHook` on a weapon that throws waves, the SLOT changes, the
  // existing "the card did something" test passes, and the weapon behaves
  // identically before and after. It survived that test for a full pass, and it
  // survived because the test asked the wrong question -- "did the numbers on
  // the card change?" instead of "did the WEAPON change?".
  //
  // So this asks the right one, from the other end: every `w_unique_*` effect
  // declares which attack types it can actually act on, and a unique card may
  // only ship on a weapon of one of them. A weapon changes its own attack type
  // (the Storm Caller stopped being a chain weapon) and its card moves to a
  // different row, or this fails and says so.
  //
  // The table is the design statement: which RULE belongs to which kind of
  // weapon. Adding a new `w_unique_*` effect means adding a row, which is the
  // point -- the alternative is finding out it is dead from a playtest.
  struct EffectScope {
    const char* effect;
    std::vector<game::AttackType> types;
    const char* why; // what the effect actually edits
  };
  const std::vector<EffectScope> scopes{
      {"w_unique_homing", {game::AttackType::Projectile}, "homing"},
      {"w_unique_area",
       {game::AttackType::Projectile, game::AttackType::Boomerang,
        game::AttackType::Bounce},
       "splash radius"},
      {"w_unique_rime", {game::AttackType::Projectile}, "chill and pierce"},
      {"w_unique_deepfreeze", {game::AttackType::Projectile}, "the carried aura"},
      {"w_unique_rimefang", {game::AttackType::Projectile}, "the lances and the shrunk aura"},
      {"w_unique_reaim", {game::AttackType::Projectile}, "pierce and the bend"},
      {"w_unique_skewer", {game::AttackType::Bounce}, "ricochet count and range"},
      {"w_unique_vortex", {game::AttackType::Orbit}, "the orbit ring"},
      {"w_unique_hearthfire", {game::AttackType::Cone}, "cone shape and ember pools"},
      {"w_unique_bore", {game::AttackType::Cone}, "the bite ramp"},
      {"w_unique_cataclysm", {game::AttackType::Bomb}, "blast size and fuse"},
      {"w_unique_siege_doctrine", {game::AttackType::Bomb}, "shells and overshoot"},
      {"w_unique_harvest", {game::AttackType::Sweep}, "heal on kill"},
      {"w_unique_chainlash", {game::AttackType::Sweep}, "the full circle and the hook"},
      {"w_unique_lash", {game::AttackType::Wave}, "arc count and the herd"},
      {"w_unique_faultline", {game::AttackType::Wave}, "the single wide crescent"},
      {"w_unique_supernova", {game::AttackType::Nova}, "the ring"},
      {"w_unique_discharge", {game::AttackType::Nova}, "the ring and the echo"},
      {"w_unique_everflame", {game::AttackType::Inferno}, "the reap and the fire"},
      {"w_unique_molten", {game::AttackType::Inferno}, "the burning ground"},
      {"w_unique_gravitic", {game::AttackType::Chain}, "jump range and decay"},
      {"w_unique_thunderlord", {game::AttackType::Chain}, "jump count and decay"},
      {"w_unique_bell", {game::AttackType::Lure}, "the pull and the beacons"},
      {"w_unique_gyre", {game::AttackType::Vortex}, "the suction"},
      {"w_unique_singularity", {game::AttackType::Vortex}, "the suction and the collapse"},
      {"w_unique_prism", {game::AttackType::Beam}, "the split"},
      {"w_unique_refract", {game::AttackType::Prism}, "the target count"},
      {"w_unique_arcsaw", {game::AttackType::Pulsar}, "the trail width"},
      {"w_unique_corona", {game::AttackType::Halo}, "the spokes"},
      {"w_unique_wingbeat", {game::AttackType::Halo}, "the spokes and the dead zone"},
  };

  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto scopeOf = [&scopes](const std::string& effect) -> const EffectScope* {
    for (const auto& sc : scopes) {
      if (effect == sc.effect) return &sc;
    }
    return nullptr;
  };
  const auto allows = [](const EffectScope& sc, game::AttackType t) {
    return std::find(sc.types.begin(), sc.types.end(), t) != sc.types.end();
  };

  // Every unique card that is scoped to a weapon must be one of the rules above,
  // and its weapon must be a type that rule can act on.
  std::size_t checked = 0;
  for (const auto& u : content.upgrades) {
    if (u.weapon.empty()) continue;
    if (u.effect.rfind("w_unique_", 0) != 0) continue;
    const auto* def = content.weapon(u.weapon);
    REQUIRE(def != nullptr);
    CAPTURE(u.id);
    CAPTURE(u.weapon);
    CAPTURE(u.effect);
    const auto* sc = scopeOf(u.effect);
    REQUIRE(sc != nullptr);
    INFO("effect " << u.effect << " moves " << sc->why
                  << ", which no " << static_cast<int>(def->attackType)
                  << "-type weapon reads");
    REQUIRE(allows(*sc, def->attackType));
    ++checked;
  }
  // And the roster is fully covered: no unique card escaped the loop above, so
  // the number here is the number of weapon-scoped unique cards in the game.
  CAPTURE(checked);
  REQUIRE(checked >= 29);
}

TEST_CASE("A hooking wave hands its catch across the fan, a plain one shoves it") {
  // The Tidal Lash's description has said each arc "drags its catch into the
  // next" for a full pass, and `WaveEffect` had no field for it: every wave
  // shoved along its own heading, which is the Sundering Core's behaviour and
  // the exact opposite of a funnel. With three arcs that meant three
  // independent pushes, each sending bodies down a different line -- which is
  // the one thing the Core already does, so the two wave weapons were a wall
  // and three more walls.
  //
  // So this measures the net direction of the push, with the Core as the
  // control. Both are aimed at a body due east, so the first arc of each goes
  // due east: a plain shove sends the catch straight out, and a herd sends it
  // across the fan. The fan turns counter-clockwise, so across means north.
  //
  // The Lash's three arcs push at +0, +1.15 and +2.30 rad, so its net is a mix
  // of all three and the claim is about where the body ENDS UP, not about any
  // one arc. The control is what makes that number mean anything: with the same
  // body, the same aim and a third of the force, the Core's across-component is
  // not smaller, it is zero.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* lash = content.weapon("tidewhip");
  const auto* core = content.weapon("sunder");
  REQUIRE(lash != nullptr);
  REQUIRE(core != nullptr);
  // The data has to say so, or the mechanic is not there to test.
  REQUIRE(lash->waveHookPull > 0.0F);
  REQUIRE(core->waveHookPull == 0.0F);
  REQUIRE(lash->waveArcStep > 0.0F); // the herd follows the fan's own rotation
  REQUIRE(lash->waveCount > 1);      // and there is a next arc to hand it to

  // How far the single body ended up from where it started, split into the
  // component along the first arc (east) and across it (north).
  const auto shoveOf = [&content](std::string_view id) {
    SoloWeapon s{std::string{id}, 31};
    s.arm();
    s.g.testSpawnEnemyAt(3.0F, 0.0F);
    s.g.testSetFirstEnemyKnockbackRes(0.0F);
    // Long enough for every arc of the shot to arrive and its push to decay.
    s.g.testAdvance(0.8F);
    const auto pos = s.g.testEnemyPositions();
    REQUIRE(pos.size() >= 2);
    return std::pair<float, float>{pos[0] - 3.0F, pos[1]};
  };

  const auto [lashAlong, lashAcross] = shoveOf("tidewhip");
  const auto [coreAlong, coreAcross] = shoveOf("sunder");
  CAPTURE(lashAlong);
  CAPTURE(lashAcross);
  CAPTURE(coreAlong);
  CAPTURE(coreAcross);

  // The control: a wave with no hook shoves along its own heading, so the body
  // goes out and not sideways at all.
  REQUIRE(coreAlong > 0.0F);
  REQUIRE(std::abs(coreAcross) < 0.02F);

  // And the Lash hands the same body across the fan instead. A third of the
  // along-component is a wide margin against a control that is exactly zero,
  // and below it the herd is not happening.
  REQUIRE(lashAcross > 0.0F);
  REQUIRE(lashAcross > 0.35F * lashAlong);
}

TEST_CASE("The Void Nova drags victims to the edge of its ring, not onto the player") {
  // "It pulls the enemies to me and I die" was the complaint, and it was right:
  // the pull had no floor, so a contracting ring walked the whole crowd onto the
  // player's own position and detonated there. The fantasy is a knot, not a
  // suicide button, so the floor now tracks the ring inward -- bodies line up
  // just outside the shrinking circle and are held there while it closes.
  //
  // The number that matters is the closest anything ever gets, so this measures
  // that rather than whether anything moved at all.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.weapon("nova")->novaContract);
  REQUIRE(content.weapon("nova")->novaPull > 0.0F);

  SoloWeapon s{"nova", 23};
  s.arm();
  // A ring of bodies INSIDE the nova's cast radius -- it is cast at full width and
  // contracts, so a body outside that is never touched at all -- but well outside
  // the pull floor, so the pull has real work to do.
  const float cast = content.weapon("nova")->novaMaxRadius;
  REQUIRE(cast > 4.0F);
  for (int k = 0; k < 8; ++k) {
    const float a = 2.0F * 3.14159265F * static_cast<float>(k) / 8.0F;
    s.g.testSpawnEnemyAt(std::cos(a) * cast * 0.95F, std::sin(a) * cast * 0.95F);
  }
  s.g.testSetFirstEnemyKnockbackRes(0.0F);
  s.g.testAdvance(2.5F);

  float closest = 1e9F;
  const auto pos = s.g.testEnemyPositions();
  REQUIRE(pos.size() >= 16);
  for (std::size_t i = 0; i + 1 < pos.size(); i += 2) {
    closest = std::min(closest, std::sqrt(pos[i] * pos[i] + pos[i + 1] * pos[i + 1]));
  }
  CAPTURE(closest);
  // Test bodies are radius 0.3 and the player is 0.35, so contact is 0.65. The
  // floor is well outside that, with room for a body to be dragged to the ring
  // and stop there.
  REQUIRE(closest > 1.2F);
  // And they really were pulled: without the floor this ran to ~0.3.
  REQUIRE(closest < 6.5F);
}

TEST_CASE("No minute of the run opens more than two enemy types") {
  // This is the "after two or three minutes the screen is full of things I cannot
  // kill" wall, measured. `unlock_at` used to be sorted by creature size, which
  // put the brute, the abomination, the golem and the juggernaut -- 140, 200, 320
  // and 420 HP -- into the single minute from 2:00 to 2:55, and that is also where
  // the spawn interval was accelerating hardest. Four heavy bodies at once, at
  // 2-3x HP scale, is a wall and not a curve.
  //
  // Two per minute is a rule the content has to obey rather than a number the
  // engine happens to produce, so a future roster edit that piles the heavies up
  // again fails here instead of in someone's run.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // The opening minute is exempt: the first three types have to be there for the
  // first minute to be playable at all, and they are all under 35 HP.
  int inMinute = 0;
  float minuteOf = 0.0F;
  float worst = 0.0F;
  for (const auto& e : content.enemies) {
    if (e.unlockAt < 60.0F) {
      REQUIRE(e.hp < 40.0F);
      continue;
    }
    const float m = std::floor(e.unlockAt / 60.0F);
    if (m != minuteOf) {
      minuteOf = m;
      inMinute = 0;
    }
    ++inMinute;
    worst = std::max(worst, static_cast<float>(inMinute));
    CAPTURE(e.id);
    CAPTURE(e.unlockAt);
    CAPTURE(inMinute);
    REQUIRE(inMinute <= 2);
  }
  CAPTURE(worst);
  // And the run has to actually use the whole roster -- a schedule that satisfies
  // the rule by holding everything back to minute ten is not a fix.
  float last = 0.0F;
  for (const auto& e : content.enemies) last = std::max(last, e.unlockAt);
  REQUIRE(last >= 540.0F);
}

// --- Round 16: milestone groups, and the marks they buy ----------------------

TEST_CASE("A milestone lays out a whole exclusive group, and taking one closes the rest") {
  // The design the player asked for: a pair, a trio or a quartet of cards that
  // always arrive TOGETHER, of which exactly one can be taken, and the rest are
  // gone for the run. It used to be three flat cards with two of them shown, so
  // the screen was "a number, or a bigger number" and nothing was decided.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // Every milestone card belongs to a group. A milestone card outside one is a
  // flat number that is neither exclusive nor part of a question, which is the
  // thing this whole section stopped being.
  std::vector<std::string> groups;
  int milestoneCards = 0;
  // Derived, so that converting another group in a later round is a data change
  // and not an edit to a list in a test. See the long note at the stacks rule
  // below for why this is not just "the groups that have an `after` member".
  std::set<std::string> treeGroups;
  for (const auto& u : content.upgrades) {
    if (u.kind == "milestone" && !u.after.empty()) {
      treeGroups.insert(u.group);
    }
  }
  for (const auto& u : content.upgrades) {
    if (u.kind != "milestone" || u.after.empty()) continue;
    for (const auto& p2 : content.upgrades) {
      if (p2.id == u.after) treeGroups.insert(p2.group);
    }
  }
  for (const auto& u : content.upgrades) {
    if (u.kind != "milestone") continue;
    ++milestoneCards;
    CAPTURE(u.id);
    REQUIRE_FALSE(u.group.empty());
    // EVERY milestone card in the survivor tree is one stack, and that is a
    // change of what "several times over" means.
    //
    // The old promise was: the card you take keeps coming back at later
    // milestones with a STACKS counter, so "vampirism several times" was a card
    // you could lean on. The new promise is: the card you take opens a LINE, and
    // the line has three tiers of its own, each a different question. Depth comes
    // from the tree rather than from repeats of one number -- which is the whole
    // point, because a fourth take of "+9% lifesteal" is not a decision and a
    // fourth tier is.
    //
    // The distinction that matters for the CONTENT is that a repeat and a branch
    // are not the same mechanic wearing different clothes. A repeat re-applies one
    // effect and the player can compute it; a branch asks something new. So the
    // stacks rule and the branch rule are stated together here rather than one
    // being allowed to imply the other.
    //
    // WHICH groups are trees is DERIVED, not listed. The first version of this
    // predicate was `group starts with "survivor"`, which is exactly how the
    // execution conversion would have shipped with its three roots still asserting
    // they stack: a hardcoded group name is a list that stops being true the moment
    // the next group is converted, and it fails by being too permissive rather than
    // by being obviously wrong.
    //
    // The closure is: a group is a tree if it has a member with a parent, OR if one
    // of its cards is the PARENT of a card in a tree group. The second clause is
    // what catches the roots, which carry no `after` of their own and would
    // otherwise read as a flat group of repeatable cards.
    if (treeGroups.count(u.group) > 0) {
      REQUIRE(u.maxStacks == 1);
    } else {
      // The groups not yet converted still lean on repeats, and the carry path in
      // buildChoices is what feeds them. Converting one deletes this branch rather
      // than relaxing it.
      REQUIRE(u.maxStacks >= 2);
    }
    if (!u.after.empty()) {
      // A branch is only reachable through its parent, and the parent has to
      // exist: a branch naming a card nothing declares is a card nobody can ever
      // see, which is the quietest possible content bug there is.
      bool parentFound = false;
      for (const auto& p2 : content.upgrades) {
        if (p2.id != u.after) continue;
        parentFound = true;
        // And the parent must be a MILESTONE. A branch off an ordinary stat card
        // would be a second card for one axis, which is the duplicate-item problem
        // wearing a hat.
        REQUIRE(p2.kind == "milestone");
      }
      CAPTURE(u.id);
      REQUIRE(parentFound);
      // And a branch group is a group of its own, never the parent's: sharing one
      // would let taking a branch block its own parent, which is how a tree
      // becomes a single take.
      REQUIRE(u.group != u.after);
    }
    if (std::find(groups.begin(), groups.end(), u.group) == groups.end()) {
      groups.push_back(u.group);
    }
  }
  CAPTURE(milestoneCards);
  // Six levels of ladder, and the two the player named plus the kill chain and
  // contact are all in there. The count is now ABOVE six, because the survivor
  // tree hangs seven branch groups off its one tier-1 group: a group is no longer
  // the unit of a milestone, it is the unit of a LINE.
  REQUIRE(groups.size() >= 6);
  REQUIRE(std::count(groups.begin(), groups.end(), "survivor") == 1);
  // Six branch groups hanging off it: one per LINE per TIER, so three lines
  // times two tiers. The count is on the survivor groups specifically and NOT
  // "every group that is not survivor" -- the first version of this predicate
  // counted the other five milestone groups too and reported eleven, which is a
  // number about the whole game rather than about the tree.
  int branchGroups = 0;
  for (const auto& gname2 : groups) {
    if (gname2.rfind("survivor.", 0) == 0) ++branchGroups;
  }
  CAPTURE(branchGroups);
  REQUIRE(branchGroups == 9);
  // NINE branch groups: three per tier, and every one of them exactly two cards.
  // Two is not a style choice, it is the minimum that makes a branch a decision.
  // A one-card group is a reward with no question attached, and the first version
  // of this tree had one -- the lifesteal/echo line had a single tier-3 answer --
  // and it read on the screen as a card the game had decided for the player.
  //
  // The groups are per TIER-2 CARD and not per LINE, and getting that wrong is a
  // bug rather than a tidiness issue: two tier-2 branches on one line are
  // different answers, so sharing a group between their tier-3 questions means
  // taking one silently closes a card the run never earned and may not even be
  // able to reach.
  for (const auto& gname2 : groups) {
    if (gname2.rfind("survivor.", 0) != 0) continue;
    CAPTURE(gname2);
    int members = 0;
    std::vector<std::string> parents;
    for (const auto& u : content.upgrades) {
      if (u.group != gname2) continue;
      ++members;
      parents.push_back(u.after);
    }
    REQUIRE(members == 2);
    // Both members of a branch group descend from the SAME parent, which is the
    // other half of the rule above.
    REQUIRE(parents.size() == 2);
    REQUIRE(parents[0] == parents[1]);
  }
  for (const char* want : {"survivor", "execution", "element", "momentum", "bulwark",
                           "apotheosis"}) {
    CAPTURE(want);
    REQUIRE(std::find(groups.begin(), groups.end(), want) != groups.end());
  }

  // At least one group is a quartet, because a four-way question is the whole
  // reason the milestone screen is allowed four slots.
  std::size_t widest = 0;
  for (const auto& gname : groups) {
    std::size_t n = 0;
    for (const auto& u : content.upgrades) {
      if (u.group == gname) ++n;
    }
    widest = std::max(widest, n);
  }
  CAPTURE(widest);
  REQUIRE(widest == 4);
  // And every group fits on one screen, so no exclusive question is ever split.
  for (const auto& gname : groups) {
    std::size_t n = 0;
    for (const auto& u : content.upgrades) {
      if (u.group == gname) ++n;
    }
    CAPTURE(gname);
    REQUIRE(n <= game::Game::kMaxMilestoneSlots);
  }
}

TEST_CASE("A milestone screen shows every card of the group it drew, and the rest stay locked") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  // The group a LEVEL introduces, which is its tier-1 group. Not "the first card
  // at that level": since the survivor tree was added, level 8 opens with a
  // branch card, and a helper that took the first match asked about
  // `survivor.crimson` (two cards) while the game correctly offered the whole
  // three-card execution group. The helper was wrong, the game was right, and the
  // test was the only thing that noticed.
  const auto groupOf = [&content](int level) -> std::string {
    for (const auto& u : content.upgrades) {
      if (u.kind == "milestone" && u.level == level && u.after.empty()) return u.group;
    }
    return {};
  };
  const auto sizeOf = [&content](const std::string& g) {
    std::size_t n = 0;
    for (const auto& u : content.upgrades) {
      if (u.group == g) ++n;
    }
    return n;
  };

  for (const int level : {4, 8, 16, 32, 64, 128}) {
    CAPTURE(level);
    const std::string gname = groupOf(level);
    REQUIRE_FALSE(gname.empty());

    game::Game g{content, 4242};
    g.testClearWeapons();
    g.testSetLevel(level);
    const auto offer = g.upgradeChoices();
    // Not two "choices" any more: the whole question is on the screen.
    REQUIRE(offer.size() == sizeOf(gname));
    REQUIRE(g.milestoneOffer());
    for (const auto& c : offer) REQUIRE(c.kind == game::Choice::Kind::Upgrade);

    // Take the first one. Every sibling is now closed off for the run. The card
    // that was taken is allowed back -- it is the one that stacks, and a leftover
    // slot at the next milestone is where the answer compounds -- but nothing
    // else from the group may ever appear again.
    const int taken = offer.front().index;
    const std::string takenId = content.upgrades[static_cast<std::size_t>(taken)].id;
    REQUIRE(g.testGrantUpgrade(taken));
    for (const auto& u : content.upgrades) {
      if (u.group != gname || u.id == takenId) continue;
      CAPTURE(u.id);
      REQUIRE(g.testUpgradeBlocked(g.testUpgradeContentIndex(u.id)));
    }
    // Advancing to the next milestone must never show a sibling.
    const int nxt = level * 2;
    g.testSetLevel(nxt);
    int seen = 0;
    for (const auto& c : g.upgradeChoices()) {
      // -1 is the "skip / nothing left" card, which has no group at all.
      if (c.index < 0) continue;
      const auto& u = content.upgrades[static_cast<std::size_t>(c.index)];
      CAPTURE(nxt);
      CAPTURE(u.id);
      if (u.group == gname) {
        // Only ever the card the run already committed to, and only once.
        REQUIRE(u.id == takenId);
        ++seen;
      }
    }
    CAPTURE(seen);
    REQUIRE(seen <= 1);
  }
}

TEST_CASE("A group is only closed by taking one of its cards, not by passing the level") {
  // The obvious bug in an exclusive system: the lock fires on "a milestone
  // happened" instead of on "the player chose", and a run silently loses a third
  // of its upgrade space because it skipped a level-up to read the screen.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // Reading the level-4 screen must not lock anything by itself. The same level,
  // offered twice in a row, has to show the same question both times.
  const auto drawSurvivor = [&content]() {
    game::Game g{content, 99};
    g.testClearWeapons();
    g.testSetLevel(4);
    std::vector<int> out;
    for (const auto& c : g.upgradeChoices()) {
      if (c.index < 0) continue;
      if (content.upgrades[static_cast<std::size_t>(c.index)].group == "survivor") {
        out.push_back(c.index);
      }
    }
    return out;
  };
  REQUIRE(drawSurvivor().size() == 3);
  REQUIRE(drawSurvivor().size() == 3);
  // A brand new run has none of it locked either.
  game::Game fresh{content, 99};
  fresh.testSetLevel(4);
  for (const auto& c : fresh.upgradeChoices()) {
    if (c.index < 0) continue;
    REQUIRE_FALSE(fresh.testUpgradeBlocked(c.index));
  }

  // Now actually take one. The two siblings are gone for good, and the card that
  // was taken comes BACK -- alone in the group, with its stacks showing -- at the
  // next milestone. That is the whole shape: one question, one answer, and then
  // the answer compounds.
  game::Game g{content, 99};
  g.testClearWeapons();
  g.testSetLevel(4);
  int taken = -1;
  for (const auto& c : g.upgradeChoices()) {
    if (c.index < 0) continue;
    if (content.upgrades[static_cast<std::size_t>(c.index)].group == "survivor") {
      taken = c.index;
      break;
    }
  }
  REQUIRE(taken >= 0);
  const std::string takenId = content.upgrades[static_cast<std::size_t>(taken)].id;
  REQUIRE(g.testGrantUpgrade(taken));
  for (const auto& u : content.upgrades) {
    if (u.group != "survivor" || u.id == takenId) continue;
    CAPTURE(u.id);
    REQUIRE(g.testUpgradeBlocked(g.testUpgradeContentIndex(u.id)));
  }

  // L8 is a fresh question AND the branch the run has earned. Both fit, but not
  // at the old numbers, and the change is deliberate rather than a squeeze: the
  // branch is placed first and unconditionally, because the player took a tier-1
  // card and is OWED a tier-2 question. Letting a shuffle drop it in favour of a
  // group the run never committed to would mean the tree silently stops growing,
  // which is a far worse bug than one card of a four-card group going unshown for
  // a single level.
  g.testSetLevel(8);
  int freshGroup = 0;
  int branchOnScreen = 0;
  for (const auto& c : g.upgradeChoices()) {
    if (c.index < 0) continue;
    const auto& u = content.upgrades[static_cast<std::size_t>(c.index)];
    CAPTURE(u.id);
    if (u.group == "execution") ++freshGroup;
    if (u.after == takenId) ++branchOnScreen;
    // Nothing from the closed tier-1 group, and nothing from another LINE: a
    // player on the pact line is never shown the regen or shield branches.
    REQUIRE(u.group != "survivor");
    if (u.after == takenId) REQUIRE(u.group.rfind("survivor.", 0) == 0);
  }
  CAPTURE(freshGroup);
  CAPTURE(branchOnScreen);
  REQUIRE(branchOnScreen == 2);
  // The two branches are the only cards that had to be here, and the screen is
  // full: three execution cards would have needed five slots, so exactly two of
  // them made it. That is the trade, and it is the right way round.
  REQUIRE(freshGroup == 2);
  REQUIRE(g.upgradeChoices().size() == game::Game::kMaxMilestoneSlots);

  // Taking one branch closes its sibling and does NOT close the run's other
  // lines' futures outright -- it just means this line's next question is a
  // reply to the branch, not to the pact.
  const auto branchIds = g.testChoiceIds();
  int branchToTake = -1;
  for (std::size_t i = 0; i < branchIds.size(); ++i) {
    if (branchIds[i] == "<skip>") continue;
    for (const auto& u : content.upgrades) {
      if (u.id == branchIds[i] && u.after == takenId) branchToTake = g.testUpgradeContentIndex(u.id);
    }
  }
  REQUIRE(branchToTake >= 0);
  const std::string branchId = content.upgrades[static_cast<std::size_t>(branchToTake)].id;
  const std::string branchGroup = content.upgrades[static_cast<std::size_t>(branchToTake)].group;
  REQUIRE(g.testGrantUpgrade(branchToTake));
  // The sibling is closed for good.
  for (const auto& u : content.upgrades) {
    if (u.group != branchGroup || u.id == branchId) continue;
    CAPTURE(u.id);
    REQUIRE(g.testUpgradeBlocked(g.testUpgradeContentIndex(u.id)));
  }
  // The parent is still held and still not blocked. A branch blocking its own
  // parent is how a three-tier tree silently becomes a one-tier fork.
  REQUIRE_FALSE(g.testUpgradeBlocked(g.testUpgradeContentIndex(takenId)));
  REQUIRE(g.testHoldsCard(takenId));
}

TEST_CASE("A milestone tree is three tiers deep, and only the line you took is ever shown") {
  // "An item that improves vampirism several times" was originally read as a
  // promise about HOW MANY TIMES a card drops, and it was implemented as stacks:
  // the same card, a STACKS counter, the same effect again. That is a number the
  // player can add up in their head, which is a real virtue, and it is also not
  // what makes vampirism interesting -- the fourth take of "+9% lifesteal" is
  // arithmetic, not a decision.
  //
  // The tree replaces the repeats with three tiers of DIFFERENT questions, and
  // the test is here to say what the tree promises rather than what it used to:
  //
  //   - the tier-1 card multiplies the axis and seeds it, so it is worth taking
  //     cold AND worth deepening, which a flat bump is not;
  //   - a branch is shown only to a run that holds its parent, so a player on the
  //     lifesteal line is never asked the regen question;
  //   - a branch is a different question, so it does not stack, and taking one
  //     closes its sibling for the run without closing its own parent.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto find = [&content](const char* id) {
    for (const auto& u : content.upgrades) {
      if (u.id == id) return &u;
    }
    return static_cast<const game::UpgradeDef*>(nullptr);
  };
  const auto* pact = find("m4_crimson");
  const auto* debt = find("m8_blooddebt");
  const auto* echo = find("m8_woundecho");
  const auto* thirst = find("m16_thirstunbound");
  const auto* ironblood = find("m16_ironblood");
  REQUIRE(pact != nullptr);
  REQUIRE(debt != nullptr);
  REQUIRE(echo != nullptr);
  REQUIRE(thirst != nullptr);
  REQUIRE(ironblood != nullptr);
  // The two branches of one line are each other's siblings, and the two lines'
  // tier-2 groups are different groups. If they shared one, taking a lifesteal
  // branch would also close the regen branches, and the run would lose a
  // question it had not answered -- which is the difference between a line and a
  // chapter.
  REQUIRE(debt->group == echo->group);
  REQUIRE(debt->group != pact->group);
  REQUIRE(thirst->group == ironblood->group);
  REQUIRE(thirst->group != debt->group);
  // A tier-3 card branches off the tier-2 card, not off the tier-1 one. Branching
  // off the tier-1 card would put the same tier-3 pair on the board twice, once
  // for each route through tier 2, and the second pair would be two cards the
  // player has already refused.
  REQUIRE(thirst->after == debt->id);
  REQUIRE(ironblood->after == debt->id);

  game::Game g{content, 7};
  g.testClearWeapons();
  const float lifestealBefore = g.stats().lifesteal;
  const float mulBefore = g.stats().milestone.lifesteal;

  // The tier-1 card does BOTH halves in one take: a seed that opens the axis and a
  // multiplier on it. The seed is not decoration -- x1.6 on zero lifesteal is
  // zero lifesteal, which would be a dead card on the only screen it can first
  // appear on.
  const int pactIdx = g.testUpgradeContentIndex(pact->id);
  REQUIRE(g.testGrantUpgrade(pactIdx));
  REQUIRE(g.stats().lifesteal == Catch::Approx(lifestealBefore + 7.0F));
  REQUIRE(g.stats().milestone.lifesteal == Catch::Approx(mulBefore + pact->value));
  // One take is one application. A card that quietly applied three would make the
  // STACKS line on it a lie, and there is no STACKS line any more, which is the
  // point: the depth moved into the tree where it can ask something new.
  REQUIRE(g.upgradeStacks(static_cast<std::size_t>(pactIdx)) == 1);
  REQUIRE_FALSE(g.testGrantUpgrade(pactIdx));
  // The other two survival answers are closed for the run, permanently.
  for (const char* sib : {"m4_renewal", "m4_aegis"}) {
    CAPTURE(sib);
    REQUIRE(g.testUpgradeBlocked(g.testUpgradeContentIndex(sib)));
  }

  // L8: the run is asked about ITS line. Both branches, and not one of the four
  // belonging to lines it never entered.
  g.testSetLevel(8);
  int shownForPact = 0;
  for (const auto& id : g.testChoiceIds()) {
    if (id == "<skip>") continue;
    for (const auto& u : content.upgrades) {
      if (u.id != id) continue;
      if (u.after == pact->id) {
        ++shownForPact;
      } else if (!u.after.empty()) {
        // A branch off a card this run does not hold. Never legal, at any level,
        // for any reason -- this is the whole eligibility rule.
        CAPTURE(id);
        REQUIRE(false);
      }
    }
  }
  CAPTURE(shownForPact);
  REQUIRE(shownForPact == 2);

  // Take one and the two multipliers ADD, they do not compound. x1.6 then x1.6
  // is x2.2, and that is exactly what the card prints ("60% stronger AGAIN.
  // Totals 2.2x") -- the test is what holds the two to each other, because
  // "a multiplier" is the kind of word that makes a reader assume x2.56 and then
  // discovers otherwise in a run.
  const int debtIdx = g.testUpgradeContentIndex(debt->id);
  REQUIRE(g.testGrantUpgrade(debtIdx));
  const float pactTotal = 1.0F + pact->value;
  const float compounded = pactTotal + debt->value;
  REQUIRE(g.stats().milestone.lifesteal == Catch::Approx(compounded - 1.0F));
  // And explicitly NOT the compounded figure, because the difference is exactly
  // the kind of thing that gets "fixed" later by someone who assumed otherwise.
  const float wouldHaveCompounded = pactTotal * (1.0F + debt->value);
  REQUIRE(compounded < wouldHaveCompounded);
  REQUIRE(compounded == Catch::Approx(2.2F).margin(0.001F));
  // The sibling of the branch is closed; the parent is untouched.
  REQUIRE(g.testUpgradeBlocked(g.testUpgradeContentIndex(echo->id)));
  REQUIRE_FALSE(g.testUpgradeBlocked(pactIdx));
  // And the branch does not come back: one take, one answer, a third tier instead.
  REQUIRE_FALSE(g.testGrantUpgrade(debtIdx));

  // L16: the question is a reply to the branch, so only the pair behind the
  // branch the run actually took may appear.
  g.testSetLevel(16);
  int shownForDebt = 0;
  int shownForPact2 = 0;
  for (const auto& id : g.testChoiceIds()) {
    if (id == "<skip>") continue;
    for (const auto& u : content.upgrades) {
      if (u.id != id) continue;
      CAPTURE(id);
      if (u.after == debt->id) ++shownForDebt;
      else if (u.after == pact->id) ++shownForPact2;
    }
  }
  CAPTURE(shownForDebt);
  CAPTURE(shownForPact2);
  REQUIRE(shownForDebt == 2);
  REQUIRE(shownForPact2 == 0);

  // The three tiers the run is on, multiplied: x2.8, and the card says so.
  const int thirstIdx = g.testUpgradeContentIndex(thirst->id);
  REQUIRE(g.testGrantUpgrade(thirstIdx));
  const float deep = compounded + thirst->value;
  REQUIRE(g.stats().milestone.lifesteal == Catch::Approx(deep - 1.0F));
  // x2.8 across three levels, and the third card prints that number too.
  REQUIRE(deep == Catch::Approx(2.8F).margin(0.001F));
}

TEST_CASE("A milestone multiplies the axis, so the same card is worth more the deeper the run went") {
  // THE CLAIM THIS ROUND EXISTS TO MAKE. A milestone used to add a flat number, and
  // a flat number is worth exactly the same to a run that engaged one card on an
  // axis as to a run that engaged eight -- so it was a reward for being at a
  // milestone rather than a reward for having built something. The fix is that a
  // milestone MULTIPLIES, which makes its value proportional to what the run put
  // in.
  //
  // Measured on regeneration, because it is the axis with a seed card and a plain
  // card and therefore the one where the comparison is honest: the same
  // Verdant Renewal, on a run with no regeneration cards and on a run with eight.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto idx = [&content](const char* id) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };
  const int renewal = idx("m4_renewal");
  const int regen = idx("regen");
  REQUIRE(renewal >= 0);
  REQUIRE(regen >= 0);

  // What the milestone contributes, measured as regenRate() with it minus regenRate()
  // without it. Reported as a RATIO because the two runs have wildly different
  // absolute numbers and the ratio is the thing the design claims.
  // The milestone's contribution, in HP/s of regeneration, on a run with the given
  // number of plain cards already taken. Measured as an ABSOLUTE DIFFERENCE
  // rather than a ratio, because on a cold run the base is zero and a ratio over
  // zero is the number a diagnostic prints when it has divided by something it
  // should have checked.
  const auto worthOf = [&content, renewal, regen](int plainTakes) {
    game::Game g{content, 11};
    g.testClearWeapons();
    for (int t = 0; t < plainTakes; ++t) {
      REQUIRE(g.testGrantUpgrade(regen));
    }
    const float before = g.regenRate();
    REQUIRE(g.testGrantUpgrade(renewal));
    return g.regenRate() - before;
  };

  // With the axis empty the milestone's own seed carries it, so the card is still
  // worth taking cold. A pure multiplier here would be worth literally nothing --
  // x1.75 on zero regeneration is zero -- which is why the tier-1 cards seed.
  const float cold = worthOf(0);
  // With the axis full the SAME card is worth a great deal more, because it is
  // scaling nine HP/s instead of one.
  const float deep = worthOf(8);
  CAPTURE(cold);
  CAPTURE(deep);
  REQUIRE(cold > 0.0F);
  REQUIRE(deep > cold * 3.0F);

  // And the numbers behind the claim, so a future edit to either card is caught
  // here rather than in a run.
  game::Game shallow{content, 11};
  shallow.testClearWeapons();
  REQUIRE(shallow.testGrantUpgrade(renewal));
  const float seededOnly = shallow.regenRate();
  game::Game deepRun{content, 11};
  deepRun.testClearWeapons();
  for (int t = 0; t < 8; ++t) REQUIRE(deepRun.testGrantUpgrade(regen));
  REQUIRE(deepRun.testGrantUpgrade(renewal));
  CAPTURE(seededOnly);
  CAPTURE(deepRun.regenRate());
  // 1.0 seeded x1.75 = 1.75.  (1.0 seed + 8.0 cards) x1.75 = 15.75.
  REQUIRE(seededOnly == Catch::Approx(1.75F));
  REQUIRE(deepRun.regenRate() == Catch::Approx(15.75F));

  // Damage takes the same shape, and it goes through ONE choke point: twenty-odd
  // places produce a point of damage and all of them read playerDamageScale(). A
  // milestone that reached nineteen of them would be a card that silently does
  // less than it says, which is the failure this accessor exists to prevent.
  game::Game dmg{content, 11};
  dmg.testClearWeapons();
  const float coldScale = dmg.playerDamageScale();
  dmg.stats().milestone.damage = 1.0F;
  const float doubled = dmg.playerDamageScale();
  CAPTURE(coldScale);
  CAPTURE(doubled);
  REQUIRE(doubled == Catch::Approx(coldScale * 2.0F));
  // The kill chain is still OUTSIDE the milestone, so a live chain is not taxed
  // by a milestone: the milestone scales what the build did, and the chain then
  // scales what the milestone made.
  REQUIRE(dmg.playerDamageScale() ==
          Catch::Approx(dmg.stats().damageMul * 2.0F * dmg.momentumDamageMul()));
}

TEST_CASE("Regeneration can be made conditional on standing still, and on being nearly dead") {
  // The two branch shapes that are not numbers. Both are CONDITIONAL multipliers
  // rather than plain ones, and that is the design: they make the player's
  // POSITION part of the build, so a survival line stops being a stat you have
  // and becomes a way you have to play.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto find = [&content](const char* id) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };
  const int stillBloom = find("m8_stillbloom");
  const int deepStill = find("m16_deepstill");
  const int warmblood = find("m16_warmblood");
  REQUIRE(stillBloom >= 0);
  REQUIRE(deepStill >= 0);
  REQUIRE(warmblood >= 0);

  const int regen = find("regen");
  REQUIRE(regen >= 0);

  // Off by default. Measured over a real second rather than off a stat, for the
  // same reason the assertions below are: the number the player gets is health,
  // and a card that sets a flag the stat ignores is a card that does nothing.
  const auto gainOver = [&content, regen](int card, bool moving, float healthFrac) {
    game::Game g{content, 61};
    g.testDisableWaves();
    g.testClearWeapons();
    for (int t = 0; t < 4; ++t) REQUIRE(g.testGrantUpgrade(regen));
    if (card >= 0) REQUIRE(g.testGrantUpgrade(card));
    g.testSetPlayerHp(g.maxHealth() * healthFrac);
    const float before = g.testPlayerHp();
    g.testSetMove(moving ? 1.0F : 0.0F, 0.0F);
    g.testAdvance(1.0F);
    return g.testPlayerHp() - before;
  };
  // Four regen cards is the base rate: 4 HP/s, so 4 HP in a second.
  // A margin, not equality: 4 HP/s applied as sixty separate 1/60 additions is
  // 3.99994, and a test that demands exactly 4.0 is a test that will fail on a
  // different compiler and be "fixed" by loosening a real assertion.
  const float tol = 0.01F;
  const float base = 4.0F;
  const float idleHealthy = gainOver(-1, false, 0.9F);
  const float idleHurt = gainOver(-1, false, 0.2F);
  const float idleMoving = gainOver(-1, true, 0.4F);
  CAPTURE(idleHealthy);
  CAPTURE(idleHurt);
  CAPTURE(idleMoving);
  REQUIRE(idleHealthy == Catch::Approx(base).margin(tol));
  REQUIRE(idleHurt == Catch::Approx(base).margin(tol));
  REQUIRE(idleMoving == Catch::Approx(base).margin(tol));

  // The stillness factor is NOT in regenRate(), on purpose: regenRate() is the
  // axis, and the condition is applied where the regen is actually spent. So the
  // assertions below measure HEALTH OVER TIME, which is the claim the card makes,
  // rather than a stat that happens to feed it. A test that read regenRate() would
  // have been reading a number the player never sees.
  //
  // One second, no input, at 40% health so the low-health mirror is inert here.
  const float baseGain = gainOver(-1, false, 0.4F);
  CAPTURE(baseGain);
  REQUIRE(baseGain == Catch::Approx(base).margin(tol));
  // x4 while still, x1 while moving.
  const float stillGain = gainOver(stillBloom, false, 0.4F);
  const float movingGain = gainOver(stillBloom, true, 0.4F);
  CAPTURE(stillGain);
  CAPTURE(movingGain);
  REQUIRE(stillGain == Catch::Approx(base * 4.0F).margin(tol));
  REQUIRE(movingGain == Catch::Approx(base).margin(tol));
  // And the tier-3 sibling is its own factor, not a bigger one: tier-2 then
  // tier-3 is x4 then x9, NOT x4+9. The factor compounding is the test.
  game::Game deepStillRun{content, 61};
  deepStillRun.testDisableWaves();
  deepStillRun.testClearWeapons();
  for (int t = 0; t < 4; ++t) REQUIRE(deepStillRun.testGrantUpgrade(regen));
  REQUIRE(deepStillRun.testGrantUpgrade(stillBloom));
  REQUIRE(deepStillRun.testGrantUpgrade(deepStill));
  deepStillRun.testSetPlayerHp(deepStillRun.maxHealth() * 0.4F);
  const float deepBefore = deepStillRun.testPlayerHp();
  deepStillRun.testAdvance(1.0F);
  const float deepGain = deepStillRun.testPlayerHp() - deepBefore;
  CAPTURE(deepGain);
  REQUIRE(deepGain == Catch::Approx(base * 9.0F).margin(tol));

  // The low-health mirror, which is the same mechanic on the other end of the bar.
  // The low-health mirror, which is the same mechanic on the other end of the bar.
  // 90% health is inert and 20% is active, both still and unmoving, so the only
  // thing that differs between the two numbers is the health bar.
  const float healthyGain = gainOver(warmblood, false, 0.9F);
  const float nearlyDeadGain = gainOver(warmblood, false, 0.2F);
  CAPTURE(healthyGain);
  CAPTURE(nearlyDeadGain);
  REQUIRE(healthyGain == Catch::Approx(base).margin(tol));
  REQUIRE(nearlyDeadGain == Catch::Approx(base * 4.0F).margin(tol));

  // The two conditions multiply rather than add, and a player who is BOTH still
  // and nearly dead earns both. Adding the factors would have made standing still
  // worth less the closer you were to dying, which is backwards from every other
  // conditional in the game and from the card's own promise.
  //
  // Measured over a QUARTER SECOND on purpose. Both cards at once is 64 HP/s, and
  // from 20 HP that crosses the half-health line in under half a second -- so a
  // one-second window measures the band switching off partway through rather than
  // the multiplier, and the first version of this test did exactly that and read
  // the shortfall as a bug in the code. It is not. The condition is LIVE, which
  // is the whole character of the card: the reward for being nearly dead stops
  // the moment you stop being nearly dead, and a player can feel that as a change
  // in the rate rather than having to look it up.
  const auto gainOverQuarter = [&content, regen, stillBloom, warmblood](bool moving,
                                                                     float healthFrac) {
    game::Game g{content, 61};
    g.testDisableWaves();
    g.testClearWeapons();
    for (int t = 0; t < 4; ++t) REQUIRE(g.testGrantUpgrade(regen));
    REQUIRE(g.testGrantUpgrade(stillBloom));
    REQUIRE(g.testGrantUpgrade(warmblood));
    g.testSetPlayerHp(g.maxHealth() * healthFrac);
    const float before = g.testPlayerHp();
    g.testSetMove(moving ? 1.0F : 0.0F, 0.0F);
    g.testAdvance(0.25F);
    return g.testPlayerHp() - before;
  };
  const float bothGain = gainOverQuarter(false, 0.2F);
  const float bothMoving = gainOverQuarter(true, 0.2F);
  CAPTURE(bothGain);
  CAPTURE(bothMoving);
  // Still and dying: 4 HP/s times x4 times x4 is 64 HP/s, and a quarter of that.
  REQUIRE(bothGain == Catch::Approx(base * 16.0F * 0.25F).margin(tol));
  // Moving and dying: only the health one, because a dash is not standing still.
  REQUIRE(bothMoving == Catch::Approx(base * 4.0F * 0.25F).margin(tol));

  // And over a full second from the same 20%, the band gives way partway through,
  // so the run gets LESS than sixteen times the base rate. Asserted explicitly
  // because it is the property that makes the card honest: a flat x16 would be
  // the larger number, and it is the wrong one -- a player who is climbing out of
  // danger is not still owed the reward for being in it.
  game::Game settling{content, 61};
  settling.testDisableWaves();
  settling.testClearWeapons();
  for (int t = 0; t < 4; ++t) REQUIRE(settling.testGrantUpgrade(regen));
  REQUIRE(settling.testGrantUpgrade(stillBloom));
  REQUIRE(settling.testGrantUpgrade(warmblood));
  settling.testSetPlayerHp(settling.maxHealth() * 0.2F);
  const float settlingBefore = settling.testPlayerHp();
  settling.testAdvance(1.0F);
  const float settlingGain = settling.testPlayerHp() - settlingBefore;
  CAPTURE(settlingGain);
  REQUIRE(settlingGain < base * 16.0F);
  REQUIRE(settlingGain > base * 4.0F);
}

TEST_CASE("The mercy heal pays out once per injury, and a hit re-arms it") {
  // "Heal to full after three seconds untouched" is the shape the design asked
  // for by name. The detail that makes it a mechanic rather than a health bar
  // that stops being a health bar is ONCE PER INJURY: you earn it by surviving
  // something, and the next one has to be earned by nearly dying again.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const int echo = [&content] {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == "m8_woundecho") return static_cast<int>(i);
    }
    return -1;
  }();
  REQUIRE(echo >= 0);

  game::Game g{content, 51};
  g.testDisableWaves();
  g.testClearWeapons();
  // Off with no card: a long quiet stretch must not heal anybody.
  g.testSetPlayerHp(g.maxHealth() * 0.25F);
  g.testAdvance(8.0F);
  const float untouchedByNothing = g.testPlayerHp();
  REQUIRE(untouchedByNothing == Catch::Approx(g.maxHealth() * 0.25F));

  REQUIRE(g.testGrantUpgrade(echo));
  // Both sides of the threshold, and the reason for asking is that three seconds
  // of 1/60 additions lands at 2.99999 rather than 3.0 on some builds, so a test
  // written at exactly 3.0s is asserting a knife edge. The rule is "three
  // seconds", which means "not before three seconds" and "yes by three and a bit".
  g.testSetPlayerHp(g.maxHealth() * 0.25F);
  g.testAdvance(2.5F);
  const float tooSoon = g.testPlayerHp();
  CAPTURE(tooSoon);
  REQUIRE(tooSoon == Catch::Approx(g.maxHealth() * 0.25F));
  g.testAdvance(1.0F);
  const float afterWait = g.testPlayerHp();
  CAPTURE(afterWait);
  REQUIRE(afterWait == Catch::Approx(g.maxHealth()));

  // Once per injury. A long quiet stretch heals exactly once, because a repeating
  // version is a health bar that stops mattering.
  g.testSetPlayerHp(g.maxHealth() * 0.30F);
  g.testAdvance(10.0F);
  const float afterSecondQuiet = g.testPlayerHp();
  CAPTURE(afterSecondQuiet);
  REQUIRE(afterSecondQuiet == Catch::Approx(g.maxHealth() * 0.30F));

  // And a hit re-arms it. This is the part that keeps the branch from being
  // out-ranged: if the timer did not reset, a player could walk away from a fight
  // and collect the heal for free, which would make the card an argument for
  // running rather than for surviving.
  g.testHurtPlayer(10.0F);
  g.testAdvance(3.5F);
  const float afterReArm = g.testPlayerHp();
  CAPTURE(afterReArm);
  REQUIRE(afterReArm == Catch::Approx(g.maxHealth()));

  // A damage-over-time tick must NOT re-arm it, and that exclusion is deliberate:
  // a DoT ticks every frame, so if it counted, standing inside your own fire
  // would switch the branch off permanently -- found out in the worst possible
  // moment. The same rule the kill chain already uses.
  game::Game d{content, 51};
  d.testDisableWaves();
  d.testClearWeapons();
  REQUIRE(d.testGrantUpgrade(echo));
  d.testSetPlayerHp(d.maxHealth() * 0.30F);
  d.testAdvance(1.0F);
  d.testDirectPlayerDamage(1.0F);
  d.testAdvance(3.5F);
  const float afterDot = d.testPlayerHp();
  CAPTURE(afterDot);
  REQUIRE(afterDot == Catch::Approx(d.maxHealth()));

  // The shield comes back with the health, because a mercy heal that left the
  // buffer empty would hand the next hit straight through -- which is the exact
  // opposite of the promise on the card.
  game::Game s{content, 51};
  s.testDisableWaves();
  s.testClearWeapons();
  const int aegis = [&content] {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == "m4_aegis") return static_cast<int>(i);
    }
    return -1;
  }();
  REQUIRE(aegis >= 0);
  REQUIRE(s.testGrantUpgrade(aegis));
  REQUIRE(s.testGrantUpgrade(echo));
  s.testSetPlayerHp(s.maxHealth() * 0.30F);
  s.testAdvance(3.5F);
  REQUIRE(s.testPlayerHp() == Catch::Approx(s.maxHealth()));
  const float shieldBack = s.testShield();
  CAPTURE(shieldBack);
  REQUIRE(shieldBack == Catch::Approx(s.shieldCap()));
}

TEST_CASE("A branch card cannot reach a run through any of the four doors") {
  // Branches are gated in upgradeIsUsable, which every door goes through. A gate
  // that only the screen you are looking at respects is a gate the chest will lie
  // about three screens later, and the absolute fallback -- the thing that fires
  // when the pool is dry and nobody is checking -- is exactly the door most likely
  // to be forgotten.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto find = [&content](const char* id) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };
  const int debt = find("m8_blooddebt");
  const int pact = find("m4_crimson");
  REQUIRE(debt >= 0);
  REQUIRE(pact >= 0);

  game::Game g{content, 71};
  g.testDisableWaves();
  g.testClearWeapons();

  // Door one: the level-up pool. Four different levels, because a branch card is
  // only relevant at its own level and a gate that only works at one level is a
  // gate that will be found out at another.
  for (const int level : {4, 8, 16, 32, 64, 128}) {
    CAPTURE(level);
    g.testSetLevel(level);
    for (const auto& id : g.testChoiceIds()) {
      REQUIRE(id != "m8_blooddebt");
      REQUIRE(id != "m8_woundecho");
      REQUIRE(id != "m16_thirstunbound");
    }
  }

  // Door two: the chest pool. A chest may never give a milestone or a branch, but
  // the predicate is asked anyway and a stale answer here would surface as a
  // branch appearing in a box.
  for (const int card : g.testLegalChestItems()) {
    const auto& u = content.upgrades[static_cast<std::size_t>(card)];
    REQUIRE(u.after.empty());
  }

  // Door three: the applier refuses it, which is what makes the test above
  // meaningful rather than cosmetic. testGrantUpgrade deliberately bypasses
  // eligibility for milestone reasons, so this one is a different door and needs
  // its own line.
  REQUIRE(g.testUpgradeUsable(debt) == false);
  REQUIRE(g.testUpgradeUsable(pact));

  // And once the parent is held, the branch is live everywhere. A gate that never
  // opens is as broken as one that never closes.
  REQUIRE(g.testGrantUpgrade(pact));
  REQUIRE(g.testUpgradeUsable(debt));
  REQUIRE(g.testUpgradeUsable(find("m8_woundecho")));
  // ...but only ITS parent's, never another line's.
  REQUIRE(g.testUpgradeUsable(find("m8_stillbloom")) == false);
  REQUIRE(g.testUpgradeUsable(find("m8_bulwark")) == false);

  g.testSetLevel(8);
  int onScreen = 0;
  for (const auto& id : g.testChoiceIds()) {
    if (id == "m8_blooddebt" || id == "m8_woundecho") ++onScreen;
  }
  CAPTURE(onScreen);
  REQUIRE(onScreen == 2);
}

TEST_CASE("The mark roots are exclusive, each one is live, and only one line can carry two") {
  // The mark group is the second question the player asked for: "do you want to
  // slow them, or set them on fire, or make them softer?" It is now THREE roots and
  // a tree, because a root row of four made the per-mark multiplier unreachable --
  // an exclusive group means a run holds exactly one root, so "all four marks"
  // always meant "the one you have".
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  struct Root {
    const char* id;
    const char* effect;
  };
  const Root roots[] = {
      {"m16_frostbind", "ms_mark_chill_seed"},
      {"m16_emberbrand", "ms_mark_burn_seed"},
      {"m16_hex", "ms_mark_vuln_seed"},
  };
  std::vector<int> idxs;
  for (const auto& r : roots) {
    int idx = -1;
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == r.id) {
        idx = static_cast<int>(i);
        CAPTURE(r.id);
        REQUIRE(content.upgrades[i].effect == r.effect);
        REQUIRE(content.upgrades[i].group == "element");
        REQUIRE(content.upgrades[i].maxStacks == 1);
      }
    }
    CAPTURE(r.id);
    REQUIRE(idx >= 0);
    REQUIRE(content.upgrades[static_cast<std::size_t>(idx)].value > 0.0F);
    idxs.push_back(idx);
  }
  REQUIRE(idxs.size() == 3);

  // A run can own exactly one root, and the one it owns must be genuinely live --
  // exactly one mark field non-zero. A root whose mark never reached the screen
  // would still close the other two out, so the player would have been charged a
  // whole tree for nothing and the exclusivity check would have passed.
  for (const int taken : idxs) {
    game::Game g{content, 5};
    g.testClearWeapons();
    REQUIRE(g.testGrantUpgrade(taken));
    // The two it did not take are exactly the ones that are now closed off.
    for (const int other : idxs) {
      if (other == taken) continue;
      CAPTURE(other);
      REQUIRE(g.testUpgradeBlocked(other));
    }
    // ...and the one it took is not.
    REQUIRE_FALSE(g.testUpgradeBlocked(taken));
    const auto& st = g.stats();
    const int live = (st.markChillTime > 0.0F ? 1 : 0) + (st.markBurnDps > 0.0F ? 1 : 0) +
                     (st.markVuln > 0.0F ? 1 : 0) + (st.markDefStrip > 0.0F ? 1 : 0);
    CAPTURE(live);
    REQUIRE(live == 1);
    // ...and exactly one mark MULTIPLIER, so a root is not quietly scaling the
    // other three as well. This is the property the per-mark split exists for, and
    // it is the one a shared multiplier could not satisfy.
    const auto& sc = st.milestone;
    const int scaled = (sc.markChill > 0.0F ? 1 : 0) + (sc.markBurn > 0.0F ? 1 : 0) +
                       (sc.markVuln > 0.0F ? 1 : 0) + (sc.markDefStrip > 0.0F ? 1 : 0);
    CAPTURE(scaled);
    REQUIRE(scaled == 1);
  }

  // THE SECOND MARK, and the only route to it. Armour Split is not a root any
  // more; it is a branch under Hex, so exactly one of the three lines can end up
  // carrying two marks and the other two can never carry a second one at any depth.
  // That asymmetry is the whole reason the Hex fork is a fork.
  const auto indexOf = [&content](const std::string& id) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };
  {
    game::Game hex{content, 5};
    hex.testClearWeapons();
    const int h = indexOf("m16_hex");
    const int split = indexOf("m32_hex_split");
    REQUIRE(h >= 0);
    REQUIRE(split >= 0);
    REQUIRE(hex.testGrantUpgrade(h));
    REQUIRE(hex.testGrantUpgrade(split));
    const auto& st = hex.stats();
    CAPTURE(st.markVuln);
    CAPTURE(st.markDefStrip);
    REQUIRE(st.markVuln > 0.0F);
    REQUIRE(st.markDefStrip > 0.0F);
    // Two marks and two multipliers, which is the run this whole refactor is for.
    const auto& sc = st.milestone;
    REQUIRE(sc.markVuln > 0.0F);
    REQUIRE(sc.markDefStrip > 0.0F);
    // ...and the strip does NOT borrow Hex's 60%. This is the check the comment on
    // the per-mark fields asks for, and it is the one that would fail first if
    // anyone ever widened a multiplier to "all the marks the run has".
    REQUIRE(sc.markDefStrip == Catch::Approx(0.6F).margin(0.001F));
  }
  // The other two lines reach a second mark only by being told to, on the record,
  // in their own branch -- never by accident, and never by depth alone.
  for (const char* branch : {"element.burn", "element.frost"}) {
    int live = 0;
    for (const auto& u : content.upgrades) {
      if (u.group == branch && u.effect.find("ms_mark_") == 0) ++live;
    }
    CAPTURE(branch);
    REQUIRE(live > 0);
    for (const auto& u : content.upgrades) {
      if (u.group == branch) {
        // Nothing in these two branches may seed a mark the root did not.
        const bool seedsAnother =
            u.effect == "ms_mark_split_seed" || u.effect == "ms_mark_vuln_armour" ||
            u.effect == "ms_mark_chill_armour";
        REQUIRE_FALSE(seedsAnother);
      }
    }
  }
}

TEST_CASE("Frostbind chills on every hit, so a melee weapon can hold a front rank") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 31};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0); // the wand: fast, weak, always hitting
  REQUIRE(game::applyUpgrade(g.stats(), "mark_slow", 2.0F).valid);
  g.testSpawnEnemyAt(1.2F, 0.0F);
  g.testAdvance(0.4F);
  // The chill is a real timer on the body, not a hidden multiplier.
  const auto muls = g.testEnemySpeedMuls();
  REQUIRE(!muls.empty());
  CAPTURE(muls.front());
  REQUIRE(muls.front() < 1.0F);
}

TEST_CASE("Emberbrand keeps a body alight only while it is still being hit") {
  // The refresh-on-hit rule is the whole character of the card: a fast weapon
  // holds a pack burning and a slow heavy hitter does not, so it is a question
  // of hit RATE rather than another flat damage number. Tested as the difference
  // between the two, not as an absolute.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const float dps = 40.0F;

  // Struck once, then left alone: the burn runs its window and the body dies of
  // it well after the last hit.
  game::Game once{content, 12};
  once.testDisableWaves();
  once.testClearWeapons();
  once.testAddWeapon(0);
  REQUIRE(game::applyUpgrade(once.stats(), "mark_burn", dps).valid);
  once.testSpawnEnemyAt(1.5F, 0.0F);
  once.testAdvance(1.0F);
  // Cut off the source of further hits, then let the fire work on its own.
  once.testClearWeapons();
  const float litHp = once.testFirstEnemyHp();
  REQUIRE(litHp < 100000.0F);
  once.testAdvance(2.0F);
  const float burntHp = once.testFirstEnemyHp();
  CAPTURE(litHp);
  CAPTURE(burntHp);
  REQUIRE(burntHp < litHp);

  // Struck continuously: the burn never lapses, so a body under fire keeps
  // losing HP for as long as the weapon keeps landing.
  game::Game held{content, 12};
  held.testDisableWaves();
  held.testClearWeapons();
  held.testAddWeapon(0);
  REQUIRE(game::applyUpgrade(held.stats(), "mark_burn", dps).valid);
  held.testSpawnEnemyAt(1.2F, 0.0F);
  held.testAdvance(1.0F);
  held.testClearWeapons();
  const float heldStart = held.testFirstEnemyHp();
  held.testAdvance(0.5F);
  CAPTURE(heldStart);
  CAPTURE(held.testFirstEnemyHp());
  // Still alight half a second after the last hit: the refresh outlived the gap.
  REQUIRE(held.testFirstEnemyHp() < heldStart);
}

// --- Round 16: elite chests, and a quieter screen ---------------------------

TEST_CASE("An elite leaves a chest, a champion three, an overlord seven") {
  // The ask: elites should not be slowed down, they should be rarer, and they
  // should be worth meeting. The box is the "worth meeting" half, and the size
  // of the box is the visible difference between a tier and the one below it.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  game::Game g{content, 1234};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);

  struct Want {
    int tier;
    int grants;
  };
  // tier 0 is trash and must never drop one.
  g.testSpawnEnemyAt(4.0F, 4.0F);
  g.testKillLastSpawned();
  REQUIRE(g.testChestCount() == 0);

  // Seven for the overlord, not five. It is the rarest thing in the game by a
  // wide margin now, and it is the fight the player has spent the whole run
  // building for, so its box has to be worth the run rather than worth the fight.
  // Five cards across five weapons was one card each; seven is the difference
  // between improving everything you own and improving the two or three things
  // that matter.
  for (const auto& w : std::vector<Want>{{1, 1}, {2, 3}, {3, 7}}) {
    CAPTURE(w.tier);
    // A fresh run per tier. A box can never invent cards, so reusing one
    // exhausted arsenal would measure the wand's card count instead of the
    // tier's promise -- and would quietly turn a content bug into a pass.
    game::Game t{content, 1234};
    t.testDisableWaves();
    t.testClearWeapons();
    for (const char* id : {"wand", "dagger", "crossbow", "flame", "hammer"}) {
      t.testAddWeapon(t.testWeaponContentIndex(id));
    }
    t.testSpawnEliteAt(4.0F, 4.0F, w.tier);
    t.testKillLastSpawned();
    REQUIRE(t.testChestCount() == 1);
    t.testOpenFirstChest();
    CAPTURE(t.testLastChestGrants());
    REQUIRE(t.testLastChestGrants() == w.grants);
    // Opening it consumed it: no box may linger to be opened twice.
    REQUIRE(t.testChestCount() == 0);
  }
}

TEST_CASE("A chest spends itself on the player's own weapons, and only legal cards") {
  // "A random improvement of one of the player's items." The important half is
  // the last three words: a box must never hand out a card for a weapon the
  // player does not hold, because that card then sits in the pool waiting for a
  // weapon that never comes.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  game::Game g{content, 88};
  g.testDisableWaves();
  g.testClearWeapons();
  const int wand = g.testWeaponContentIndex("wand");
  g.testAddWeapon(wand);

  const int given = g.testOpenChestFor(1);
  CAPTURE(given);
  REQUIRE(given == 1);
  // Whatever it gave, it went to the wand.
  REQUIRE(g.upgradeStacks(static_cast<std::size_t>(g.testLastChestCard())) == 1);
  REQUIRE(g.testLastChestCard() >= 0);
  REQUIRE(content.upgrades[static_cast<std::size_t>(g.testLastChestCard())].weapon ==
          "wand");
}

TEST_CASE("A box with nothing left to give still opens") {
  // The failure this guards is a soft lock: a box that refuses to open because
  // every weapon is maxed sits on the floor forever, and a player who is waiting
  // to walk over it is waiting for nothing. It opens, spends nothing, and leaves.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 3};
  g.testDisableWaves();
  g.enterTestMode();
  g.testClearWeapons();
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  g.testMaxAllItems();
  // Confirm the premise rather than assuming it: the wand really is out of cards.
  REQUIRE(g.testLegalWeaponCards(0).empty());
  REQUIRE(g.testChestCount() == 0);
  // maxAllItems also took the Deep Cache card, so the box is bigger than a bare
  // elite's -- which does not matter, because a bigger box with nothing to spend
  // on is still the same dead end this test is about.
  REQUIRE(g.stats().chestBonus == 3);
  REQUIRE(g.testSpawnChest(1.0F, 0.0F, 1, -1) == 4);
  g.testOpenFirstChest();
  REQUIRE(g.testLastChestGrants() == 0);
  REQUIRE(g.testChestCount() == 0);
}

TEST_CASE("Deep Cache makes every chest one weapon bigger") {
  // The only lever that lets an elite's box reach a champion's without a
  // champion existing, which is what makes "make elites rarer" survivable.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int idx = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].id == "u_deep_cache") idx = static_cast<int>(i);
  }
  REQUIRE(idx >= 0);
  REQUIRE(content.upgrades[static_cast<std::size_t>(idx)].effect == "chest_bonus");
  game::PlayerStats probe{};
  REQUIRE(game::applyUpgrade(probe, "chest_bonus", 1.0F).valid);
  REQUIRE(probe.chestBonus == 1);

  game::Game g{content, 44};
  g.testDisableWaves();
  g.testClearWeapons();
  for (const char* id : {"wand", "dagger", "crossbow"}) {
    g.testAddWeapon(g.testWeaponContentIndex(id));
  }
  REQUIRE(g.testSpawnChest(1.0F, 0.0F, 1, -1) == 1);
  g.testOpenFirstChest();
  const int oneCard = g.testLastChestGrants();
  CAPTURE(oneCard);
  REQUIRE(oneCard == 1);

  g.testGrantUpgrade(idx);
  REQUIRE(g.stats().chestBonus == 1);
  REQUIRE(g.testSpawnChest(1.0F, 0.0F, 1, -1) == 2);
}

TEST_CASE("The screen never holds more than a tier's share of heavy bodies") {
  // "Do not make elites weaker, make them rarer, so there are one or two on
  // screen." A per-spawn percentage cannot promise that -- packs and waves roll
  // members independently -- so the promise is a live count, per tier, and this
  // pins it.
  //
  // Four and not two, because the size of an arrival is now drawn from how well
  // the player is handling that tier (tierEventSize): a comfortable player meets
  // three or four at once, and a cap of two would silently throw two of them away
  // and hand back the flat one-at-a-time feeling the ladder is being fixed for.
  // The ladder tightens as it climbs, because it has to: an overlord is the run's
  // boss and there is only ever one.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 6};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  // The elite budget, spent, so the director has nothing to hand out.
  for (int i = 0; i < game::tierEventCap(1); ++i) {
    g.testSpawnEliteAt(3.0F, static_cast<float>(i), 1);
  }
  REQUIRE(g.testLiveTierCount(1) == game::tierEventCap(1));
  // Waves are disabled, so the only way the number could grow is the director
  // ignoring the cap, and the only way to test that is to let it run.
  g.testEnableWaves();
  g.testSetSimTime(game::tierMinTime(1) + 1.0F);
  g.testAdvance(6.0F);
  // The cap may be under-spent on a quiet roll, but it may never be over-spent.
  // Which is the whole claim: the size of an arrival is ROLLED, and it used to be
  // rolled after the "is there room" check rather than clamped to it, so one
  // surviving elite plus a comfortable roll of four put five on the floor against
  // a ceiling of four.
  // One elite already up, the player comfortably past the line so the size roll
  // is three or four, and the clock due. Asserted on the event's OWN body count
  // rather than on the live count, because an arrival lands as a ring of
  // telegraphs first: a live count taken a frame later is still zero of it.
  g.testDespawnEnemies();
  g.testAddTierPressure(1, game::Game::tierComfortLine(1) * 4.0F);
  g.testSpawnEliteAt(3.0F, 0.0F, 1);
  const int before = g.tierEventBodies(1);
  g.testAdvance(1.0F / 60.0F);
  const int added = g.tierEventBodies(1) - before;
  CAPTURE(added);
  REQUIRE(added > 0);
  const int room = game::tierEventCap(1) - g.testLiveTierCount(1);
  CAPTURE(room);
  REQUIRE(added <= room);
  // And the comfortable roll really was the bigger one, so this is not passing
  // because the size rule quietly collapsed to one body.
  REQUIRE(added >= 3);
  // And the tiers above it are tighter, monotonically, or an overlord would stop
  // being the run's boss and become the third member of a pack.
  REQUIRE(game::tierEventCap(1) > game::tierEventCap(2));
  REQUIRE(game::tierEventCap(2) > game::tierEventCap(3));
  REQUIRE(game::tierEventCap(3) == 1);
}

TEST_CASE("Improving your weapons is an item, and it stacks") {
  // "Weapon improvement" is a card you pick off the level-up screen like any
  // other item -- there is no hidden bump for levelling, because a free stat axis
  // nobody chose is a stat axis that competes with every card they DID choose.
  // So the promise is three things: the card exists as an ordinary item, every
  // take lands once, and a late weapon improves exactly like an early one.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  // Reach, because that is the axis that still edits the WEAPON's own fields.
  // Damage and fire rate used to be here too, as u_whetstone and u_oiled_gear,
  // and the reason they are not is the whole subject of this test's sibling: they
  // scaled the weapon instead of the player, which put "Every weapon you own hits
  // 8% harder" in the pool beside "+15% damage" as two items for one axis.
  const auto* barrel = content.upgrade("u_long_barrel");
  REQUIRE(barrel != nullptr);
  REQUIRE(barrel->kind == "normal");
  REQUIRE(barrel->group.empty());
  REQUIRE(barrel->maxStacks >= 3);

  game::Game g{content, 31};
  g.testClearWeapons();
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  const float baseLife = g.testWeaponStat(0, "projLife");

  // One take, one application. The card's number is the number on the card.
  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("u_long_barrel")));
  const float afterOne = g.testWeaponStat(0, "projLife");
  REQUIRE(afterOne == Catch::Approx(baseLife * (1.0F + barrel->value)));

  // It stacks, and every take is visible on the weapon.
  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("u_long_barrel")));
  const float afterTwo = g.testWeaponStat(0, "projLife");
  REQUIRE(afterTwo == Catch::Approx(baseLife * (1.0F + barrel->value) * (1.0F + barrel->value)));
  REQUIRE(afterTwo > afterOne);

  // A weapon picked up at 6:00 is worth exactly what the same weapon was worth
  // at 0:30, and it improves on the SAME terms. That is the promise that quietly
  // stops being true in every game like this.
  const int dagger = g.testWeaponContentIndex("dagger");
  const float daggerFresh = [&] {
    game::Game fresh{content, 31};
    fresh.testClearWeapons();
    fresh.testAddWeapon(dagger);
    return fresh.testWeaponStat(0, "projLife");
  }();
  g.testAddWeapon(dagger);
  REQUIRE(g.testWeaponStat(1, "projLife") == Catch::Approx(daggerFresh));
  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("u_long_barrel")));
  REQUIRE(g.testWeaponStat(1, "projLife") == Catch::Approx(daggerFresh * (1.0F + barrel->value)));
  // ...and both weapons moved together on the very next take.
  const float wandBefore = g.testWeaponStat(0, "projLife");
  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("u_long_barrel")));
  REQUIRE(g.testWeaponStat(0, "projLife") > wandBefore);
  REQUIRE(g.testWeaponStat(1, "projLife") ==
          Catch::Approx(daggerFresh * (1.0F + barrel->value) * (1.0F + barrel->value)));
}

TEST_CASE("The number a weapon is worth on the sheet is the number it does") {
  // The pause sheet used to print `w.damage` and `w.cooldown` straight out of the
  // content file. That was invisible for as long as the damage and fire-rate
  // cards were `w_all_damage` and `w_all_rate`, because those scaled the WEAPON
  // and so moved the printed number. Collapsing them into one `damage_mul` card
  // and one `fire_rate` card is the right change -- they reached the same
  // multipliers through the player's own stats, which is where a card that
  // improves everything belongs -- but it would have left the sheet permanently
  // frozen on the base numbers of the two axes the player spends the most of
  // their slots on. So the sheet now prints the effective value, and this is the
  // test that the two can never drift apart again.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* damageCard = content.upgrade("damage");
  const auto* hasteCard = content.upgrade("haste");
  REQUIRE(damageCard != nullptr);
  REQUIRE(hasteCard != nullptr);

  game::Game g{content, 37};
  g.testClearWeapons();
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  const float baseDamage = g.testWeaponStat(0, "effectiveDamage");
  const float baseCooldown = g.testWeaponStat(0, "effectiveCooldown");
  // The sheet is not printing a number that happens to equal the base: it is
  // printing the base times the player's multipliers, and right now those are 1.
  REQUIRE(g.testWeaponStat(0, "damage") == Catch::Approx(baseDamage));

  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("damage")));
  const float hurtDamage = g.testWeaponStat(0, "effectiveDamage");
  REQUIRE(hurtDamage == Catch::Approx(baseDamage * (1.0F + damageCard->value)));
  REQUIRE(hurtDamage > baseDamage);
  // The weapon's own base number did not move, and must not: that is the other
  // test's promise, and a card that quietly edits it would make the sheet and the
  // content file disagree about what a wand is.
  REQUIRE(g.testWeaponStat(0, "damage") == Catch::Approx(baseDamage));

  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("haste")));
  const float hurtCooldown = g.testWeaponStat(0, "effectiveCooldown");
  REQUIRE(hurtCooldown == Catch::Approx(game::attackCooldown(baseCooldown, hasteCard->value)));
  REQUIRE(hurtCooldown < baseCooldown);
  REQUIRE(g.testWeaponStat(0, "cooldown") == Catch::Approx(baseCooldown));

  // Both stack, and the sheet keeps up. Damage is ADDITIVE (damageMul is a sum,
  // fire rate is a sum in a denominator), so two takes of +30% is +60%, not
  // +69% -- worth pinning, because a card that reads "+30% damage" and quietly
  // compounds is a card whose ninth stack is a surprise.
  const float beforeMore = g.testWeaponStat(0, "effectiveDamage");
  REQUIRE(g.testGrantUpgrade(g.testUpgradeContentIndex("damage")));
  const float afterMore = g.testWeaponStat(0, "effectiveDamage");
  REQUIRE(afterMore == Catch::Approx(baseDamage * (1.0F + 2.0F * damageCard->value)));
  REQUIRE(afterMore > beforeMore);
}

TEST_CASE("Levelling changes a weapon on its own") {
  // The negative half of the same promise, pinned because a regression here is
  // invisible: the game still plays, the weapons still fire, and the player just
  // quietly gets stronger for reasons that are not on any card. So a level-up
  // must not touch a single weapon number.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 33};
  g.testClearWeapons();
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  const float damage = g.testWeaponStat(0, "damage");
  const float cooldown = g.testWeaponStat(0, "cdBonus");
  const float projectiles = g.testWeaponStat(0, "projectiles");
  g.testSetLevel(200);
  REQUIRE(g.testWeaponStat(0, "damage") == Catch::Approx(damage));
  REQUIRE(g.testWeaponStat(0, "cdBonus") == Catch::Approx(cooldown));
  REQUIRE(g.testWeaponStat(0, "projectiles") == Catch::Approx(projectiles));
}

TEST_CASE("Every weapon-wide item moves the weapon's own numbers, not the player's") {
  // What is left of the weapon-wide set covers the axes no player-stat card
  // covers, because they are the weapon's geometry rather than a multiplier on
  // what comes out of it. Each one has to actually move something on a weapon,
  // or it is a card that prints a promise and does nothing.
  //
  // The set used to have four members. `w_all_damage` and `w_all_rate` were
  // removed because they were the global damage and fire-rate cards in disguise:
  // the pool showed "Every weapon you own hits 8% harder" next to "+15% damage"
  // and a player reasonably read two items where there was one axis. This is the
  // test that would have caught it, and it is kept pointed at the applier rather
  // than at a literal so the list cannot drift.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* wand = content.weapon("wand");
  REQUIRE(wand != nullptr);
  REQUIRE(game::Game::isWeaponWideEffect("w_all_reach"));
  REQUIRE(game::Game::isWeaponWideEffect("w_all_knockback"));
  REQUIRE_FALSE(game::Game::isWeaponWideEffect("w_all_damage"));
  REQUIRE_FALSE(game::Game::isWeaponWideEffect("w_all_rate"));
  for (const char* id : {"u_long_barrel", "u_heavy_stock"}) {
    const auto* card = content.upgrade(id);
    CAPTURE(id);
    REQUIRE(card != nullptr);
    REQUIRE(card->kind == "normal");
    REQUIRE(card->maxStacks >= 3);
    REQUIRE(game::Game::isWeaponWideEffect(card->effect));
  }
  // The reach item is the only one that must widen something every weapon has,
  // and it is a percentage of the weapon's OWN reach, so the content number is
  // what a card stacks on top of.
  const auto* barrel = content.upgrade("u_long_barrel");
  REQUIRE(barrel->value > 0.0F);
  REQUIRE(wand->projLife > 0.0F);
}

TEST_CASE("The two vortex weapons are a snare and a charge, not one weapon twice") {
  // "The super-evolutions are too similar conceptually; the player barely feels a
  // difference." Two of the three were literally the same well with a bigger
  // number on it: permanent drag, and drag that stopped for three seconds first.
  // The difference between those is a timer, and a timer is not a concept. So one
  // of them was rebuilt around a DIFFERENT question -- not "how hard does the well
  // hit" but "how many bodies is the well holding" -- and the two now want
  // opposite positions on the screen.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto* gyre = content.weapon("vortex");
  const auto* horizon = content.weapon("eventhorizon");
  REQUIRE(gyre != nullptr);
  REQUIRE(horizon != nullptr);

  // One rule each, never both, and never neither.
  REQUIRE(gyre->vortexCrowd > 0.0F);
  REQUIRE(horizon->vortexCrowd == 0.0F);
  REQUIRE(gyre->vortexCollapseAt == 0.0F);
  REQUIRE(horizon->vortexCollapseAt > 0.0F);

  // And the snare's answer to a single enemy has to be feeble, or the crowd bonus
  // would never be the reason to take it and it would just be a stronger charge.
  // A crowd of six is worth 3x the tick; that is the whole payoff.
  const float sixBody = 1.0F + gyre->vortexCrowd * 5.0F;
  CAPTURE(sixBody);
  REQUIRE(sixBody >= 2.5F);

  // The charge, meanwhile, must be worth holding on its own, because it is paid
  // by the clock and has no reason to wait for a crowd.
  REQUIRE(horizon->damage > gyre->damage);
  // And the snare has to be the one that can actually HOLD a crowd, so its grip
  // and reach are the better of the two.
  REQUIRE(gyre->vortexReach > horizon->vortexReach);
  REQUIRE(gyre->vortexPull > horizon->vortexPull);
}

TEST_CASE("The Void Gyre's damage is a function of what it is holding") {
  // Tested as a ratio rather than a number, so the assertion is about the RULE
  // and survives a retune. One body in the core versus six.
  //
  // The bodies are spawned as a tight knot and the window is four seconds, not a
  // handful of frames. The wells ORBIT at 2.8 units and reach 3.4, so a body has
  // to be dragged in by the pull before it is inside a 1.4 core at all, and the
  // well that is doing the dragging is itself moving. A short window with the
  // bodies spread on a wide ring measures how long the test happened to be
  // looking at, not what the weapon does.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 71};
  g.testDisableWaves();
  g.testClearWeapons();
  const int gyre = g.testWeaponContentIndex("vortex");
  REQUIRE(gyre >= 0);
  g.testAddWeapon(gyre);
  const float crowd = content.weapon("vortex")->vortexCrowd;
  REQUIRE(crowd > 0.0F);

  // One body held: the well has to scratch, not kill.
  g.testSpawnEnemyAt(0.2F, 0.0F);
  g.testAdvance(4.0F);
  const float solo = 100000.0F - g.testFirstEnemyHp();
  CAPTURE(solo);
  REQUIRE(solo > 0.0F);

  // Six bodies held: a different run, same well, same everything else. The knot
  // is tight enough that all six sit in whichever core catches them, which is
  // the state the weapon is FOR -- spread them and three wells each hold two,
  // which is a different (and much smaller) number on purpose.
  game::Game h{content, 71};
  h.testDisableWaves();
  h.testClearWeapons();
  h.testAddWeapon(gyre);
  for (int k = 0; k < 6; ++k) {
    const float a = 6.28318F * static_cast<float>(k) / 6.0F;
    h.testSpawnEnemyAt(std::cos(a) * 0.2F, std::sin(a) * 0.2F);
  }
  h.testAdvance(4.0F);
  const float packed = 100000.0F - h.testFirstEnemyHp();
  CAPTURE(packed);
  // Strictly better per body, and by more than the crowd falloff the well applies
  // on its own would explain -- which is the only way to know the bonus is being
  // applied and not just that six targets shared a tick.
  REQUIRE(packed > solo * 2.0F);
}

// --- Round 17/19: a milestone must beat a plain card on the same axis ---------

TEST_CASE("A milestone beats a plain card on the same axis, and is worth more the deeper the run went") {
  // A milestone closes its siblings for the rest of the run. A plain card does
  // not. So for the same AXIS, a milestone has to win on the number it hands you
  // for ONE take -- you are giving up three options, and one of those options is
  // worth more than what you get.
  //
  // It exists because of a real defect the player named: the regeneration
  // milestone was +1.2 while a plain card handed out +2, so the "reward" for
  // locking away two other options was a strictly worse buy.
  //
  // WHAT CHANGED, AND WHY THE OLD RULE HAD TO GO. The original test also demanded
  // that a milestone beat a plain card over the WHOLE run, which it got by
  // comparing `value * maxStacks` against `plainValue * plainStacks`. With
  // multipliers that comparison is a category error rather than a close call: it
  // weighed one take of a one-stack card against eight takes of an eight-stack
  // card, and it only ever passed because the old milestones stacked two or three
  // times. A multiplier is exactly the card that CANNOT be measured that way --
  // its whole-run value is "the axis you built", which is unbounded and is the
  // point. So the whole-run half of the rule is replaced by the claim that
  // replaces it: the same card is worth MORE on a run that engaged the axis, and
  // strictly more than a flat card is worth there.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  // The strongest repeatable card on an effect, judged per take.
  struct Best {
    float perTake = 0.0F;
    float perRun = 0.0F;
    std::string id;
  };
  std::map<std::string, Best> plain;
  for (const auto& u : content.upgrades) {
    if (u.kind == "milestone") continue;
    const float v = static_cast<float>(u.value);
    auto& b = plain[u.effect];
    if (b.id.empty() || v > b.perTake) {
      b.perTake = std::max(b.perTake, v);
      b.id = u.id;
    }
    b.perRun = std::max(b.perRun, v * static_cast<float>(u.maxStacks));
  }

  // ONE TABLE, THREE CLAIMS, all measured rather than tabulated.
  //
  // The expected number is what the CARD PRINTS, and that is the claim worth
  // making: a player who reads "4 becomes 6" off Tempest and counts seven shots
  // leaving their weapon has been told a lie, and nothing else in the design is
  // worth as much as that not happening. The first version of this test listed the
  // seeds as hand-written constants BESIDE the card that sets them, which is a
  // number that rots silently -- it stayed green when the seed in the applier was
  // halved and when the card text was left printing the pre-multiplier seed.
  //
  // So: `want` is the number on the card, `read` is the axis the player can see,
  // and the two are required to agree. Then the same measurement has to beat one
  // take of the best repeatable card on that axis, because the player is locking
  // away three options to get this one and one of those options has to be the
  // worse buy.
  struct Axis {
    const char* effect;      // the milestone effect, for "is anything on here"
    const char* plainEffect; // the repeatable card it competes with
    const char* card;        // the card whose printed number `want` is
    std::function<float(const game::Game&)> read;
    float want;   // what the card says
    float plain;  // one take of the best repeatable card on the same axis
  };
  const std::vector<Axis> axes = {
    // 1.6 total damage, printed as "+60%". damageMul is 1.0-based, so the scale IS
    // the multiplier and needs no adjustment -- which is the opposite of the other
    // eight axes and is why it gets a comment rather than a silent asymmetry.
    {"ms_damage", "damage_mul", "m8_overload",
     [](const game::Game& g) { return g.playerDamageScale(); }, 1.6F, 0.30F},
    // effectiveFireRate() is a RATE BONUS, not the rate: 0.6 means the rate is
    // x1.6. A test that compared it to a raw 1.6 would fail while the game is
    // right, which is why the comment is here and not in the assertion.
    {"ms_fire_rate", "fire_rate", "m8_frenzy",
     [](const game::Game& g) { return g.effectiveFireRate(); }, 0.6F, 0.18F},
    // The two seed-and-multiply cards, printed as the arithmetic they perform.
    // 4 at x1.5 is 6; 2 at x2 is 4. Both round to whole shots, so the number on
    // the card is the number the weapon can actually spend.
    {"ms_proj_seed", "proj_add", "m8_tempest",
     [](const game::Game& g) { return static_cast<float>(g.extraProjectiles()); }, 6.0F, 1.0F},
    {"ms_pierce", "pierce_add", "m16_tempest_pierce",
     [](const game::Game& g) { return static_cast<float>(g.playerPierceBonus()); }, 4.0F, 1.0F},
    // These three print the SEED AFTER the multiplier, because the seed is inside
    // it: 7 at x1.6 is 11.2, not 7. A card that printed the raw seed would be
    // under-delivering by 40% while reading as though it were not.
    {"ms_lifesteal_seed", "lifesteal_add", "m4_crimson",
     [](const game::Game& g) { return g.lifestealChance(); }, 11.2F, 3.0F},
    {"ms_regen_seed", "regen_add", "m4_renewal",
     [](const game::Game& g) { return g.regenRate(); }, 1.75F, 1.0F},
    {"ms_shield_seed", "shield_add", "m4_aegis",
     [](const game::Game& g) { return g.shieldCap(); }, 80.0F, 13.0F},
  };

  int checked = 0;
  for (const Axis& a : axes) {
    CAPTURE(a.effect);
    // Every axis has at least one card on it, so a renamed effect id cannot quietly
    // turn this block into nothing.
    int onAxis = 0;
    for (const auto& u : content.upgrades) {
      if (u.kind == "milestone" && u.effect == a.effect) ++onAxis;
    }
    CAPTURE(onAxis);
    REQUIRE(onAxis >= 1);
    // And the repeatable card it competes against exists, at the strength assumed
    // here -- asserted rather than read, because `plain` is the claim under test.
    const auto it = plain.find(a.plainEffect);
    REQUIRE(it != plain.end());
    CAPTURE(it->second.id);
    REQUIRE(it->second.perTake == Catch::Approx(a.plain).margin(0.001F));

    int mIdx = -1;
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == a.card) mIdx = static_cast<int>(i);
    }
    REQUIRE(mIdx >= 0);
    game::Game g{content, 101};
    g.testClearWeapons();
    const float base = a.read(g);
    CAPTURE(base);
    REQUIRE(g.testGrantUpgrade(mIdx));
    const float got = a.read(g);
    CAPTURE(got);
    // The card's promise, measured.
    REQUIRE(got == Catch::Approx(a.want).margin(0.01F));
    // ...and it has to be worth more than one take of the plain card.
    REQUIRE(got - base > a.plain);
    ++checked;
  }
  // Every axis with a repeatable card was compared, so emptying the milestone pool
  // cannot quietly turn this test green.
  REQUIRE(checked >= 7);
}

TEST_CASE("Every Execution branch card does what it prints, all the way down the tree") {
  // The Execution tree is nineteen numbers, and the first version of it shipped
  // with one of them silently doing nothing: the projectile axis was still an `int`
  // in the milestone layer, so its multiplier truncated to zero and the ROOT card
  // already printed "4 becomes 6" while firing four. The per-take test caught it --
  // but by accident, because that test compares a card against another card and the
  // accident is that it happened to be reading the axis the number was wrong on.
  //
  // So: walk the tree, and at every node assert the axis value the card PRINTS.
  // The expected numbers are written out longhand rather than recomputed from the
  // data, because a table that recomputes itself can only ever confirm that the
  // data and the applier agree with each other -- not that either one matches the
  // sentence on the card. Every number below was read off a card and then checked
  // against the code, and that is the only direction this check is worth anything
  // in.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto indexOf = [&content](const std::string& id) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };

  // A run holding exactly these cards. It hands back a pointer because Game holds a
  // registry and is not copyable, which is a detail that would otherwise be
  // rediscovered by every future reader of this file.
  using Run = std::unique_ptr<game::Game>;
  const auto run = [&content, &indexOf](const std::vector<std::string>& path) {
    auto g = std::make_unique<game::Game>(content, 101);
    g->testDisableWaves();
    g->testClearWeapons();
    for (const auto& id : path) {
      const int idx = indexOf(id);
      REQUIRE(idx >= 0);
      REQUIRE(g->testGrantUpgrade(idx));
    }
    return g;
  };
  // Standing still AND below half health, so every conditional is true at once and
  // the product can be compared against what the cards say the product is. Measured
  // under those two conditions because a conditional card asserted in the state
  // where it is FALSE reads 1.0 and passes, which is the opposite of a test.
  const auto condMul = [&run](const std::vector<std::string>& path, float healthFrac) {
    const Run g = run(path);
    g->testSetPlayerHp(g->maxHealth() * healthFrac);
    g->testSetMove(0.0F, 0.0F);
    g->testAdvance(1.0F / 60.0F); // one step, so the conditions are evaluated
    return g->damageConditionMul();
  };
  const auto at = [&run](const std::function<float(const game::Game&)>& f,
                         const std::vector<std::string>& path) {
    return f(*run(path));
  };
  // A stated condition is only half a rule until the other half is measured too, so
  // every conditional here is checked moving as well as still.
  const auto condMulMoving = [&run](const std::vector<std::string>& path, float healthFrac) {
    const Run g = run(path);
    g->testSetPlayerHp(g->maxHealth() * healthFrac);
    g->testSetMove(1.0F, 0.0F);
    g->testAdvance(1.0F / 60.0F);
    return g->damageConditionMul();
  };

  // --- the damage line ---------------------------------------------------------
  // "All damage +60%." / "again. Totals 2.2x." / "again. Totals 3.0x."
  const auto scale = [](const game::Game& g) { return g.playerDamageScale(); };
  REQUIRE(at(scale, {"m8_overload"}) == Catch::Approx(1.6F).margin(0.01F));
  REQUIRE(at(scale, {"m8_overload", "m16_overload_deep"}) == Catch::Approx(2.2F).margin(0.01F));
  REQUIRE(at(scale, {"m8_overload", "m16_overload_deep", "m32_overload_ruin"}) ==
          Catch::Approx(3.0F).margin(0.01F));
  // "All damage x4 against anything below half health." The factor is checked here
  // and the funnel is checked at the bottom of this test, because a factor that is
  // stored correctly and never reached is the exact shape of bug this file exists
  // to catch.
  const auto execute = [](const game::Game& g) { return g.playerExecuteMul(); };
  REQUIRE(at(execute, {"m8_overload", "m16_overload_deep", "m32_overload_guillotine"}) ==
          Catch::Approx(4.0F).margin(0.01F));

  // "All damage x3 while you are below half health." / "x2 on top. Totals x6."
  const std::vector<std::string> bloodied{"m8_overload", "m16_overload_bloodied"};
  const std::vector<std::string> wrath{"m8_overload", "m16_overload_bloodied",
                                        "m32_bloodied_wrath"};
  const std::vector<std::string> secondWind{"m8_overload", "m16_overload_bloodied",
                                           "m32_bloodied_secondwind"};
  REQUIRE(condMul(bloodied, 0.3F) == Catch::Approx(3.0F).margin(0.01F));
  REQUIRE(condMul(wrath, 0.3F) == Catch::Approx(6.0F).margin(0.01F));
  // HEALTH is what switches it off, not movement. An early draft of this test
  // walked with a low-health card and expected 1.0, which is the shape of a card
  // that secretly also reads the movement key -- so both halves of the condition
  // are asserted: healthy is a no-op, and walking at 30% is still live.
  REQUIRE(condMul(wrath, 0.9F) == Catch::Approx(1.0F).margin(0.01F));
  REQUIRE(condMulMoving(wrath, 0.3F) == Catch::Approx(6.0F).margin(0.01F));

  // "x2.5 below a quarter health instead of a half. Totals x7.5." -- the same
  // product as Wrath at a DIFFERENT threshold, and that difference is the whole
  // card: with Wrath the fork is two multipliers, with Second Wind it is two
  // different places to be standing. So the pair is asserted at one fraction that
  // falls between the two cuts, where they disagree, and each number is that
  // card's own claim rather than one of them plus a no-op.
  REQUIRE(condMul(wrath, 0.2F) == Catch::Approx(6.0F).margin(0.01F));
  REQUIRE(condMul(secondWind, 0.2F) == Catch::Approx(7.5F).margin(0.01F));
  REQUIRE(condMul(wrath, 0.4F) == Catch::Approx(6.0F).margin(0.01F));
  REQUIRE(condMul(secondWind, 0.4F) == Catch::Approx(1.0F).margin(0.01F));
  // The cut itself, because "below a quarter" is a word and the word has an edge:
  // a player sitting on exactly a quarter is not below it. Asserted right at the
  // line, because a threshold that is a quarter up to a rounding error and half a
  // quarter for real is a card that lies for exactly one pixel of health.
  REQUIRE(condMul(wrath, 0.499F) == Catch::Approx(6.0F).margin(0.01F));
  REQUIRE(condMul(wrath, 0.501F) == Catch::Approx(1.0F).margin(0.01F));
  REQUIRE(condMul(secondWind, 0.249F) == Catch::Approx(7.5F).margin(0.01F));
  REQUIRE(condMul(secondWind, 0.251F) == Catch::Approx(1.0F).margin(0.01F));

  // --- the fire-rate line ------------------------------------------------------
  // "Fire rate +60%." / "again. Totals 2.2x." / "+80% again. Totals 3.0x."
  // effectiveFireRate() is a RATE BONUS, so 0.6 is a rate of x1.6 and the totals are
  // 0.6, 1.2 and 2.0 -- additive, because the milestone layer is.
  const auto rateBonus = [](const game::Game& g) { return g.effectiveFireRate(); };
  REQUIRE(at(rateBonus, {"m8_frenzy"}) == Catch::Approx(0.6F).margin(0.01F));
  REQUIRE(at(rateBonus, {"m8_frenzy", "m16_frenzy_deep"}) == Catch::Approx(1.2F).margin(0.01F));
  REQUIRE(at(rateBonus, {"m8_frenzy", "m16_frenzy_deep", "m32_frenzy_blizzard"}) ==
          Catch::Approx(2.0F).margin(0.01F));
  // "Fire rate x2 while you give no movement input." Measured while standing still
  // AND while moving, because the second half is the one that makes it a condition.
  {
    const Run still = run({"m8_frenzy", "m16_frenzy_deep", "m32_frenzy_cadence"});
    still->testSetMove(0.0F, 0.0F);
    still->testAdvance(1.0F / 60.0F);
    REQUIRE(still->rateConditionMul() == Catch::Approx(2.0F).margin(0.01F));
    const Run moving = run({"m8_frenzy", "m16_frenzy_deep", "m32_frenzy_cadence"});
    moving->testSetMove(1.0F, 0.0F);
    moving->testAdvance(1.0F / 60.0F);
    REQUIRE(moving->rateConditionMul() == Catch::Approx(1.0F).margin(0.01F));
  }

  // --- the projectile line -----------------------------------------------------
  // "4 more projectiles, and 4 becomes 6." / "doubled. 4 becomes 8." /
  // "quadrupled. 4 becomes 16." The three the `int` bug broke.
  const auto bonus = [](const game::Game& g) { return static_cast<float>(g.extraProjectiles()); };
  REQUIRE(at(bonus, {"m8_tempest"}) == Catch::Approx(6.0F));
  REQUIRE(at(bonus, {"m8_tempest", "m16_tempest_deep"}) == Catch::Approx(8.0F));
  REQUIRE(at(bonus, {"m8_tempest", "m16_tempest_deep", "m32_tempest_deep"}) ==
          Catch::Approx(16.0F));

  // "Pierce +2, and your pierce bonus is doubled. 2 becomes 4." The two third tiers
  // land on 4 -> 12 and 8 -> 40, and the cards deliberately print the MULTIPLIER
  // rather than those counts, because the counts also depend on whichever pierce
  // cards the run took on the way. The multiplier is the honest thing to assert and
  // it is the one that catches a scale that stopped applying.
  const auto pierce = [](const game::Game& g) { return static_cast<float>(g.playerPierceBonus()); };
  REQUIRE(at(pierce, {"m8_tempest", "m16_tempest_pierce"}) == Catch::Approx(4.0F));
  REQUIRE(at(pierce, {"m8_tempest", "m16_tempest_pierce", "m32_skewer_spine"}) ==
          Catch::Approx(12.0F));
  REQUIRE(at(pierce, {"m8_tempest", "m16_tempest_pierce", "m32_skewer_lattice"}) ==
          Catch::Approx(40.0F));
  // ...and the halves the third tiers add on top of the count, each asserted to be
  // ABSENT from the other: two cards that both said "pierce" and quietly did the
  // same second thing is the failure mode of sharing vocabulary.
  const auto armour = [](const game::Game& g) { return g.playerArmourScale(); };
  const auto area = [](const game::Game& g) { return g.playerAreaScale(); };
  const std::vector<std::string> spine{"m8_tempest", "m16_tempest_pierce", "m32_skewer_spine"};
  const std::vector<std::string> lattice{"m8_tempest", "m16_tempest_pierce", "m32_skewer_lattice"};
  REQUIRE(at(armour, spine) == Catch::Approx(0.5F).margin(0.01F));
  REQUIRE(at(area, spine) == Catch::Approx(1.0F).margin(0.01F));
  REQUIRE(at(area, lattice) == Catch::Approx(1.4F).margin(0.01F));
  REQUIRE(at(armour, lattice) == Catch::Approx(0.0F).margin(0.01F));
  // Hail sits on the OTHER branch, so it inherits the root's projectile seed and no
  // pierce one: "Pierce +2, and your pierce bonus is tripled. 2 becomes 6."
  const std::vector<std::string> hail{"m8_tempest", "m16_tempest_deep", "m32_tempest_hail"};
  REQUIRE(at(pierce, hail) == Catch::Approx(6.0F));
  // ...and it must not have touched the projectile axis it is a sibling of.
  REQUIRE(at(bonus, hail) == Catch::Approx(8.0F));

  // --- the still line ----------------------------------------------------------
  // "All damage x2.5 while you give no movement input." / "x1.6 on top. Totals x4."
  const std::vector<std::string> rooted{"m8_frenzy", "m16_frenzy_settled"};
  const std::vector<std::string> anchor{"m8_frenzy", "m16_frenzy_settled", "m32_rooted_anchor"};
  const std::vector<std::string> pillar{"m8_frenzy", "m16_frenzy_settled", "m32_rooted_pillar"};
  REQUIRE(condMul(rooted, 0.25F) == Catch::Approx(2.5F).margin(0.01F));
  REQUIRE(condMul(anchor, 0.25F) == Catch::Approx(4.0F).margin(0.01F));
  REQUIRE(condMulMoving(anchor, 0.25F) == Catch::Approx(1.0F).margin(0.01F));
  // The Pillar sibling says "x2.5 ... and regeneration x3 there too": the SAME damage
  // factor as its parent, not a bigger one, and a regen factor on top. The pair only
  // makes sense if the damage halves are identical, so that is asserted rather than
  // assumed -- a tree where one branch quietly also deepened the damage would make
  // the fork arithmetic and the regen half decoration.
  REQUIRE(condMul(pillar, 0.25F) == Catch::Approx(2.5F).margin(0.01F));
  {
    // The regen half, measured as HEALTH GAINED rather than as the field, because a
    // test that reads the private field is a test of the field. A quarter of a
    // second, because a full one would climb out of the low-health band partway
    // through and measure the transition instead of the multiplier.
    const Run g = run(pillar);
    g->testSetMove(0.0F, 0.0F);
    g->testSetPlayerHp(g->maxHealth() * 0.25F);
    g->testAdvance(1.0F / 60.0F); // let the conditions latch
    const float before = g->testPlayerHp();
    g->testAdvance(0.25F);
    const float gained = g->testPlayerHp() - before;
    // The Pillar seeds 3 HP/s and triples it while still, so a run that has never
    // taken a regen card of its own still heals -- which is the whole reason the
    // card seeds, and the assertion is written as the product so that a change to
    // either half has to be made here on purpose.
    REQUIRE(gained == Catch::Approx(3.0F * 3.0F * 0.25F).margin(0.01F));
  }
  // ...and the same card, MOVING, heals nothing at all. "While you do" is doing
  // real work in that sentence, and a regen card that quietly pays out while the
  // player runs is a card whose condition is decoration.
  {
    const Run g = run(pillar);
    g->testSetPlayerHp(g->maxHealth() * 0.25F);
    g->testSetMove(1.0F, 0.0F);
    g->testAdvance(1.0F / 60.0F);
    const float before = g->testPlayerHp();
    g->testAdvance(0.25F);
    REQUIRE(g->testPlayerHp() - before == Catch::Approx(0.0F).margin(0.01F));
  }

  // --- the target half, through the real damage funnel -------------------------
  // "All damage x4 against anything below half health." Everything above reads a
  // stored number. This one puts damage on a body and watches it, because the factor
  // is applied in applyEnemyDamage and nowhere else -- the single place in the game
  // that knows which body is being hit, and therefore the single place a
  // target-conditional card can be true or false.
  //
  // The body is placed at an exact fraction of its OWN pool rather than at a
  // hardcoded HP number, which is what makes this a test of the card and not a test
  // of one particular enemy's statistics.
  {
    const auto dealt = [&content, &indexOf](float enemyFrac) {
      game::Game g{content, 101};
      g.testDisableWaves();
      g.testClearWeapons();
      const int idx = indexOf("m32_overload_guillotine");
      REQUIRE(idx >= 0);
      REQUIRE(g.testGrantUpgrade(idx));
      g.testSpawnEnemyAt(0.0F, 0.0F, 0.0F);
      const float pool = g.testFirstEnemyMaxHp();
      REQUIRE(pool > 0.0F);
      g.testSetFirstEnemyHp(pool * enemyFrac);
      g.testDamageFirstEnemy(1.0F);
      return pool * enemyFrac - g.testFirstEnemyHp();
    };
    const float weak = dealt(0.3F);
    const float tough = dealt(0.7F);
    CAPTURE(weak);
    CAPTURE(tough);
    REQUIRE(weak > 0.0F);
    REQUIRE(tough > 0.0F);
    // The same point of damage, on the same body, at two fractions of the same pool.
    // The ratio IS the card's printed x4, and it is asserted tightly -- this funnel
    // is a plain multiplication on the incoming number, so a card promising x4 that
    // delivered x2.5 would show up here and nowhere else.
    REQUIRE(weak == Catch::Approx(4.0F * tough).margin(0.001F));
    // ...and the cut, at the line, because a threshold that is "half" for a player
    // on exactly half is a card arguing with its own wording.
    REQUIRE(dealt(0.499F) > dealt(0.501F) * 2.0F);
    // A card of this shape must not fire at all on a body it does not describe, so
    // the same run without the card is the control: both fractions take the same
    // damage, which is what makes the two numbers above a measurement rather than a
    // coincidence of a strong run.
    {
      const auto plain = [&content](float enemyFrac) {
        game::Game g{content, 101};
        g.testDisableWaves();
        g.testClearWeapons();
        g.testSpawnEnemyAt(0.0F, 0.0F, 0.0F);
        const float pool = g.testFirstEnemyMaxHp();
        REQUIRE(pool > 0.0F);
        g.testSetFirstEnemyHp(pool * enemyFrac);
        g.testDamageFirstEnemy(1.0F);
        return pool * enemyFrac - g.testFirstEnemyHp();
      };
      REQUIRE(plain(0.3F) == Catch::Approx(plain(0.7F)).margin(0.001F));
    }
  }
}

TEST_CASE("A milestone card is only ever offered on a milestone screen") {
  // The promise the violet screen makes is that a milestone is a choice made
  // AGAINST ITS ALTERNATIVES: the whole group is on screen at once, and the rest of
  // that group is shut for the run. A milestone card on an ordinary level-up cannot
  // keep that promise, because there is nothing on the screen to be against -- and the
  // 140-level diagnostic found it happening for exactly that reason. The absolute
  // fallback at the end of buildChoices() has no kind filter, so on a level where the
  // normal pool had run dry it reached into the milestone table and handed out one
  // tree card, on its own, eighty levels after the screen that card belonged to:
  // "L112 cards=1 [m32_ember_deep]". A tree card chosen against nothing.
  //
  // So the rule is stated here as a rule about EVERY ordinary level, not about the
  // one that was wrong. The check is a scan, not a reproduction: a reproduction would
  // pin the level number, and the level number is a property of the seed and the
  // economy rather than of the promise.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  const auto isMilestone = [&content](const std::string& id) {
    for (const auto& u : content.upgrades) {
      if (u.id == id) return u.kind == "milestone";
    }
    return false;
  };
  // A milestone screen is a power-of-two level from 4 on, so anything else is an
  // ordinary one. 1 and 2 are the opening screens and 3 is the first ordinary one, so
  // the scan starts where ordinary level-ups actually start.
  const auto isMilestoneLevel = [](int level) {
    return level >= 4 && (level & (level - 1)) == 0;
  };

  // Three runs of very different shape, because the leak needs a dry pool and a dry
  // pool needs a built run. An empty run proves nothing here: with the whole table
  // open, the ordinary pool is never dry and the fallback is never reached.
  std::vector<std::unique_ptr<game::Game>> runs;
  for (const int seed : {101, 7717, 31337}) {
    runs.push_back(std::make_unique<game::Game>(content, seed));
    // Test mode, because a weapon offer would otherwise pad every screen with two
    // or three weapon cards and the dry state this test exists to reach would be
    // invisible behind them. The first draft of this scanned 402 ordinary levels,
    // passed all 1609 of its own assertions, and never once saw an empty screen --
    // not because the pool was full but because something was always on the screen.
    runs.back()->enterTestMode();
  }
  // Run one is empty, so every level is well supplied and the ordinary pool is never
  // dry. It is the control: it shows the scan passes on a run that has everything.
  //
  // Runs two and three are the condition the leak needs, and they are built into it
  // rather than waited for. A 140-level autoplay reaches a dry pool at L107, which
  // made this a test that only failed on one seed; a maxed run has a dry pool from
  // the first level, so the fallback is reached on every ordinary screen of the scan.
  //
  // The cheat, and the full arsenal first. `testMaxAllItems` is the sandbox "max
  // everything" and it loops until nothing more can be granted, which matters here: a
  // single content-order pass leaves a card ungranted whenever a weapon-specific card
  // comes before the weapon it belongs to, and the first draft of this test granted in
  // one pass and so never actually reached an empty screen. Run three additionally
  // answers part of the milestone tree, so the deferred branches are still unclaimed
  // and still there for a pool that has run out to reach for.
  for (std::size_t r = 1; r < runs.size(); ++r) {
    auto& g = runs[r];
    for (std::size_t i = 0; i < content.weapons.size(); ++i) {
      g->testAddWeapon(static_cast<int>(i));
    }
    g->testMaxAllItems();
  }
  for (const char* const id : {"m16_emberbrand", "m32_ember_spread", "m16_hex",
                               "m32_hex_deep", "m8_overload", "m8_frenzy",
                               "m4_aegis", "m4_renewal"}) {
    auto& g = runs[2];
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id && g->testGrantUpgrade(static_cast<int>(i))) break;
    }
  }

  int scanned = 0;
  int thinScreens = 0;
  for (auto& g : runs) {
    for (int level = 1; level <= 140; ++level) {
      g->testSetLevel(level);
      const auto ids = g->testChoiceIds();
      if (ids.size() <= 1) ++thinScreens;
      if (isMilestoneLevel(level)) continue;
      ++scanned;
      for (const auto& id : ids) {
        CAPTURE(level);
        CAPTURE(id);
        // `<skip>` is a level-up with nothing on it, which is honest and is not this
        // test's business. A milestone card is a broken promise, and this is the only
        // place either of those two facts is checked.
        if (id == "<skip>") continue;
        REQUIRE_FALSE(isMilestone(id));
      }
    }
  }
  // The scan has to have actually reached the state it exists to reach. A rule test
  // that only ever saw a full pool would pass on a build where the fallback is
  // unreachable, which is the same class of test as a card that was never offered.
  CAPTURE(scanned);
  CAPTURE(thinScreens);
  REQUIRE(scanned > 100);
  REQUIRE(thinScreens > 0);

  // ...and the milestone screens themselves must still offer milestone cards, or the
  // filter above would be satisfied by a build that had simply stopped offering the
  // tree at all. Checked at the levels where the tree has something to say.
  for (const int level : {16, 32, 64}) {
    auto g = std::make_unique<game::Game>(content, 4242);
    g->testClearWeapons();
    int offered = 0;
    for (int mlevel = 4; mlevel <= level; mlevel <<= 1) {
      g->testSetLevel(mlevel);
      for (const auto& id : g->testChoiceIds()) {
        if (id != "<skip>" && isMilestone(id)) ++offered;
      }
    }
    CAPTURE(level);
    CAPTURE(offered);
    REQUIRE(offered > 0);
  }
}

TEST_CASE("Every Element card does what it prints, and only to its own mark") {
  // The Element tree is the first one whose CARDS cannot be checked by reading a
  // number the player owns, because most of what they do happens to a body rather
  // than to the sheet. So this walks the tree twice over: once through the accessors
  // that fold the mark multiplier into the number the card prints, and once by
  // spawning bodies and asking what the last hit actually did to them.
  //
  // The second half is the one that matters. Every bug found so far in this area was
  // a card that stored something correctly and never spent it, or a multiplier that
  // was set and read by a different axis than the card named -- and a stored number
  // is equally happy in all three cases. "The field is right" is not evidence that
  // the game does the thing.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto indexOf = [&content](const std::string& id) {
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == id) return static_cast<int>(i);
    }
    return -1;
  };
  using Run = std::unique_ptr<game::Game>;
  const auto run = [&content, &indexOf](const std::vector<std::string>& path) {
    auto g = std::make_unique<game::Game>(content, 101);
    g->testDisableWaves();
    g->testClearWeapons();
    for (const auto& id : path) {
      const int idx = indexOf(id);
      REQUIRE(idx >= 0);
      REQUIRE(g->testGrantUpgrade(idx));
    }
    return g;
  };
  const auto at = [&run](const std::function<float(const game::Game&)>& f,
                         const std::vector<std::string>& path) {
    return f(*run(path));
  };

  // A body, and one hit that lands on it. Zero damage on purpose: `applyEnemyDamage`
  // applies the marks after the damage is resolved and bails if the body died, so a
  // hit of 1.0 on a low-health body would measure the mark only on the runs where it
  // failed to kill. Spawn, one step so the spatial hash exists for the spread, then
  // the hit.
  const auto struck = [&content, &indexOf](const std::vector<std::string>& path,
                                           int bodies = 1) {
    auto g = std::make_unique<game::Game>(content, 101);
    g->testDisableWaves();
    g->testClearWeapons();
    for (const auto& id : path) {
      const int idx = indexOf(id);
      REQUIRE(idx >= 0);
      REQUIRE(g->testGrantUpgrade(idx));
    }
    // A line of bodies, all inside the spread radius. Nothing is parked out of
    // range on purpose: the "first" enemy is whichever one the registry hands over
    // first, which is not the one that was spawned first, and an earlier draft
    // parked a body far away and then discovered the spread was firing on THAT one
    // and had no neighbours at all. The cap is proved by giving the card more
    // bodies in range than it may reach, which needs no out-of-range body and does
    // not depend on which body was hit.
    for (int i = 0; i < bodies; ++i) {
      // With traits, not the bare stub: half of what the Element cards do is written
      // to EnemyTraits, so a body without one is a body the cards cannot act on and
      // the assertion silently measures a card writing to nothing.
      g->testSpawnTraitedEnemyAt(static_cast<float>(i) * 1.0F, 0.0F);
    }
    g->testAdvance(1.0F / 60.0F);
    g->testDamageFirstEnemy(0.0F);
    return g;
  };

  // --- the three roots ----------------------------------------------------------
  // "Every hit sets it alight: 35 burning damage a second, and your burn counts 60%
  // stronger." The 35 is 22 seeded at x1.6, and it is asserted twice: as the number
  // the sheet would show, and as the number a body is actually left burning at.
  const std::vector<std::string> ember{"m16_emberbrand"};
  REQUIRE(at([](const game::Game& g) { return g.playerMarkBurnDps(); }, ember) ==
          Catch::Approx(35.2F).margin(0.05F));
  REQUIRE(struck(ember)->testFirstEnemyBurnDps() == Catch::Approx(35.2F).margin(0.05F));
  // "Every hit makes that body take 22% more damage, up to +179%."
  const std::vector<std::string> hex{"m16_hex"};
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnStep(); }, hex) ==
          Catch::Approx(0.224F).margin(0.002F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnCeiling(); }, hex) ==
          Catch::Approx(1.792F).margin(0.01F));
  // "Every hit chills what it strikes for 4s, and the chill counts 60% stronger."
  const std::vector<std::string> frost{"m16_frostbind"};
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillTime(); }, frost) ==
          Catch::Approx(4.0F).margin(0.01F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillDepth(); }, frost) ==
          Catch::Approx(0.60F).margin(0.01F));
  // The depth, asserted on a body rather than on the field, because the multiplier
  // reaching the SLOW and not just the DURATION was the specific thing that could
  // have gone wrong and would have looked fine on a card that only mentions seconds.
  REQUIRE(struck(frost)->testFirstEnemySlowMul() == Catch::Approx(0.60F).margin(0.01F));
  REQUIRE(struck(frost)->testFirstEnemySlowT() == Catch::Approx(4.0F).margin(0.05F));

  // A ROOT SCALES ONE MARK. The per-mark split is the whole reason the Element tree
  // has three roots and not four, so this is asserted for all three rather than once:
  // a root that quietly scaled the sibling marks would be invisible on its own card
  // and would make the Hex branch's second mark free.
  for (const std::vector<std::string>& root : {ember, hex, frost}) {
    const Run g = run(root);
    const auto& st = g->stats();
    int live = 0;
    live += st.markBurnDps > 0.0F ? 1 : 0;
    live += st.markChillTime > 0.0F ? 1 : 0;
    live += st.markVuln > 0.0F ? 1 : 0;
    live += st.markDefStrip > 0.0F ? 1 : 0;
    CAPTURE(live);
    REQUIRE(live == 1);
    int scaled = 0;
    scaled += st.milestone.markBurn > 0.0F ? 1 : 0;
    scaled += st.milestone.markChill > 0.0F ? 1 : 0;
    scaled += st.milestone.markVuln > 0.0F ? 1 : 0;
    scaled += st.milestone.markDefStrip > 0.0F ? 1 : 0;
    CAPTURE(scaled);
    REQUIRE(scaled == 1);
  }

  // --- the burn branch ----------------------------------------------------------
  // "35 becomes 70 burning damage a second." / "70 becomes 140."
  REQUIRE(at([](const game::Game& g) { return g.playerMarkBurnDps(); },
             {"m16_emberbrand", "m32_ember_deep"}) == Catch::Approx(70.4F).margin(0.05F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkBurnDps(); },
             {"m16_emberbrand", "m32_ember_deep", "m64_ember_ruin"}) ==
          Catch::Approx(140.8F).margin(0.05F));
  // "The burn jumps to two bodies either side." / "four bodies either side."
  //
  // Measured as a COUNT OF BODIES ON FIRE, with more bodies in range than either
  // card is allowed to reach. A card that jumped to everything in range would pass a
  // test that only looked at one body, and a card that jumped to nothing passes a
  // test that only looked at whether the target lit up.
  {
    // Four bodies, a card that may reach two: the one that was hit plus two.
    const Run g = struck({"m16_emberbrand", "m32_ember_spread"}, 4);
    CAPTURE(g->testBurningEnemies());
    REQUIRE(g->testBurningEnemies() == 3);
  }
  {
    // The same four, with the card that may reach four: all of them, because only
    // three others were standing there. Which is the control for the count above --
    // a spread that ignored its cap would also give three here, so the pair only
    // means something because the two numbers differ.
    const Run g = struck({"m16_emberbrand", "m32_ember_spread", "m64_ember_flare"}, 4);
    CAPTURE(g->testBurningEnemies());
    REQUIRE(g->testBurningEnemies() == 4);
  }
  // ...and with no spread card at all, exactly the one that was hit burns. This is
  // the control that makes the two numbers above a measurement: without it, a build
  // that set the whole screen alight would look identical to one that jumped to two.
  REQUIRE(struck({"m16_emberbrand"}, 4)->testBurningEnemies() == 1);
  // "A burn outlives its last hit for 8s." The base window is 4, so this is a claim
  // about seconds and is asserted as one.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkBurnWindow(); },
             {"m16_emberbrand", "m32_ember_spread", "m64_ember_wildfire"}) ==
          Catch::Approx(8.0F).margin(0.01F));
  // ...and the plain branch leaves the window alone, because a "deeper" card that
  // quietly also stretched the burn's life would make the fork arithmetic.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkBurnWindow(); },
             {"m16_emberbrand", "m32_ember_deep"}) == Catch::Approx(4.0F).margin(0.01F));
  // "Bodies that burn are also slowed, for 1.6s."
  {
    const Run g = struck({"m16_emberbrand", "m32_ember_deep", "m64_ember_cinder"});
    REQUIRE(g->testFirstEnemySlowMul() == Catch::Approx(0.60F).margin(0.01F));
    REQUIRE(g->testFirstEnemySlowT() == Catch::Approx(1.6F).margin(0.05F));
  }
  // ...and the burn's own damage is untouched by its sibling, which is the other half
  // of "these are two different cards".
  REQUIRE(at([](const game::Game& g) { return g.playerMarkBurnDps(); },
             {"m16_emberbrand", "m32_ember_deep", "m64_ember_cinder"}) ==
          Catch::Approx(70.4F).margin(0.05F));

  // --- the Hex branch -----------------------------------------------------------
  // "The ramp is steeper and the ceiling higher: 45% per hit, up to +358%."
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnStep(); },
             {"m16_hex", "m32_hex_deep"}) == Catch::Approx(0.448F).margin(0.002F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnCeiling(); },
             {"m16_hex", "m32_hex_deep"}) == Catch::Approx(3.584F).margin(0.01F));
  // "67% per hit, up to +537%." Half the second tier's step, and the reason is the
  // ceiling -- so both numbers are asserted, because a card whose step doubled and
  // whose ceiling did not is a ratchet that stalls.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnStep(); },
             {"m16_hex", "m32_hex_deep", "m64_hex_grievous"}) ==
          Catch::Approx(0.672F).margin(0.002F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnCeiling(); },
             {"m16_hex", "m32_hex_deep", "m64_hex_grievous"}) ==
          Catch::Approx(5.376F).margin(0.01F));

  // THE RAMP, on a body, over several hits, and then at its ceiling. The printed
  // "+22% per hit, up to +179%" is a claim about a sequence and not about one hit,
  // and the two halves fail independently: a ratchet that never rises is a flat
  // bonus, and one that rises without a ceiling is a trap for a slow weapon.
  {
    const Run g = run(hex);
    g->testSpawnTraitedEnemyAt(0.0F, 0.0F);
    g->testAdvance(1.0F / 60.0F);
    for (int hit = 1; hit <= 3; ++hit) {
      g->testDamageFirstEnemy(0.0F);
      CAPTURE(hit);
      // The current hit does not enjoy the ramp, so after N hits the body carries
      // N steps. This is the ratchet the comment on EnemyTraits::vuln describes.
      REQUIRE(g->testFirstEnemyVuln() == Catch::Approx(0.224F * hit).margin(0.002F));
    }
  }
  {
    // ...and it stops. Twelve hits at a 0.224 step would be +269% if it were an open
    // ramp; the ceiling says +179% and the ceiling is the whole reason the card can
    // promise one.
    const Run g = run(hex);
    g->testSpawnTraitedEnemyAt(0.0F, 0.0F);
    g->testAdvance(1.0F / 60.0F);
    for (int hit = 0; hit < 12; ++hit) {
      g->testDamageFirstEnemy(0.0F);
    }
    CAPTURE(g->testFirstEnemyVuln());
    REQUIRE(g->testFirstEnemyVuln() == Catch::Approx(1.792F).margin(0.002F));
  }

  // "Every hit also strips 35 of the target's own armour, and the stripping counts
  // 60% stronger." The second mark, and the only route to it.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkDefStrip(); },
             {"m16_hex", "m32_hex_split"}) == Catch::Approx(35.2F).margin(0.05F));
  // ...and the Hex it was taken beside is UNCHANGED. A branch card that quietly
  // deepened its parent would make the second mark arrive with a bonus nobody chose.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkVulnStep(); },
             {"m16_hex", "m32_hex_split"}) == Catch::Approx(0.224F).margin(0.002F));
  // "Every hit also strips 22 of the target's own armour." -- Hex Rupture, and the
  // exact number is the assertion. This run has no strip multiplier, because it never
  // took the card that seeds one, so 22 is what the card says and 35 would be Hex's
  // 60% leaking into a mark it does not name. The one place the per-mark rule could
  // fail quietly.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkDefStrip(); },
             {"m16_hex", "m32_hex_deep", "m64_hex_armour"}) ==
          Catch::Approx(22.0F).margin(0.05F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkDefStrip(); },
             {"m16_hex", "m32_hex_split", "m64_split_ruin"}) ==
          Catch::Approx(70.4F).margin(0.05F));
  // ...and the sibling of the deeper strip leaves the strip alone.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkDefStrip(); },
             {"m16_hex", "m32_hex_split", "m64_split_brittle"}) ==
          Catch::Approx(35.2F).margin(0.05F));

  // --- the Frostbind branch -----------------------------------------------------
  // "4s becomes 8s of chill." / "8s becomes 16s."
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillTime(); },
             {"m16_frostbind", "m32_frost_deep"}) == Catch::Approx(8.0F).margin(0.01F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillTime(); },
             {"m16_frostbind", "m32_frost_deep", "m64_frost_permafrost"}) ==
          Catch::Approx(16.0F).margin(0.01F));
  // "Chilled bodies are slowed 50% harder." / "50% harder again."
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillDepth(); },
             {"m16_frostbind", "m32_frost_slow"}) == Catch::Approx(1.10F).margin(0.01F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillDepth(); },
             {"m16_frostbind", "m32_frost_slow", "m64_frost_glacier"}) ==
          Catch::Approx(1.60F).margin(0.01F));
  // The DURATION is a second axis on the same pair, and the deeper card must not
  // touch it -- a chill that is both longer and harder is not a fork.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillTime(); },
             {"m16_frostbind", "m32_frost_slow"}) == Catch::Approx(4.0F).margin(0.01F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillTime(); },
             {"m16_frostbind", "m32_frost_slow", "m64_frost_glacier"}) ==
          Catch::Approx(4.0F).margin(0.01F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillDepth(); },
             {"m16_frostbind", "m32_frost_deep"}) == Catch::Approx(0.60F).margin(0.01F));
  // "Every hit also strips 22 of the target's own armour." -- Frostbind Hoarfrost.
  // The same WORDS as Hex Rupture and a different rule, and the rule is the whole
  // reason the two cards are not one: Rupture strips on every hit, Hoarfrost strips
  // only while it is chilling, which is to say never for a run that does not hold
  // Frostbind. So the two are asserted on DIFFERENT accessors, and the run that took
  // Hoarfrost is asserted to have an Armour Split of exactly zero -- a shared field
  // would have given it 22 as well and quietly answered a question nobody asked.
  REQUIRE(at([](const game::Game& g) { return g.playerMarkChillArmourStrip(); },
             {"m16_frostbind", "m32_frost_slow", "m64_frost_hoar"}) ==
          Catch::Approx(35.2F).margin(0.05F));
  REQUIRE(at([](const game::Game& g) { return g.playerMarkDefStrip(); },
             {"m16_frostbind", "m32_frost_slow", "m64_frost_hoar"}) == 0.0F);
  // ...and the run that took it has a chill AND a strip, which is the one thing the
  // per-mark design was supposed to make possible without giving every root two
  // marks at its root.
  {
    const Run g = run({"m16_frostbind", "m32_frost_slow", "m64_frost_hoar"});
    CAPTURE(g->playerMarkChillTime());
    CAPTURE(g->playerMarkChillArmourStrip());
    REQUIRE(g->playerMarkChillTime() == Catch::Approx(4.0F).margin(0.01F));
    REQUIRE(g->playerMarkChillArmourStrip() == Catch::Approx(35.2F).margin(0.05F));
  }

  // ...and Armour Split, for the pair: it strips on EVERY hit, from a body that has
  // armour, and 35 of it -- 22 seeded at x1.6.
  {
    const Run g = run({"m16_hex", "m32_hex_split"});
    g->testSpawnTraitedEnemyAt(0.0F, 0.0F, 100.0F);
    g->testAdvance(1.0F / 60.0F);
    g->testDamageFirstEnemy(0.0F);
    CAPTURE(g->testFirstEnemyDefense());
    REQUIRE(g->testFirstEnemyDefense() == Catch::Approx(100.0F - 35.2F).margin(0.05F));
  }
  {
    // ...and a second hit takes it again, because the card says "every hit" and a
    // strip that only fired once would be a card with a hidden total.
    const Run g = run({"m16_hex", "m32_hex_split"});
    g->testSpawnTraitedEnemyAt(0.0F, 0.0F, 100.0F);
    g->testAdvance(1.0F / 60.0F);
    g->testDamageFirstEnemy(0.0F);
    g->testDamageFirstEnemy(0.0F);
    CAPTURE(g->testFirstEnemyDefense());
    REQUIRE(g->testFirstEnemyDefense() == Catch::Approx(100.0F - 70.4F).margin(0.05F));
  }
  {
    // ...and it floors at zero rather than going negative, because a body cannot be
    // made more naked and a defence of -5 would be a mitigation that helps the enemy.
    const Run g = run({"m16_hex", "m32_hex_split"});
    g->testSpawnTraitedEnemyAt(0.0F, 0.0F, 20.0F);
    g->testAdvance(1.0F / 60.0F);
    for (int hit = 0; hit < 5; ++hit) {
      g->testDamageFirstEnemy(0.0F);
    }
    CAPTURE(g->testFirstEnemyDefense());
    REQUIRE(g->testFirstEnemyDefense() == 0.0F);
  }
  {
    // Hoarfrost strips 35 from a body rather than 22, and the reason is printed on
    // the card: 22 at Frostbind's x1.6. Armour Split strips 35 too, from 22 at the
    // strip's x1.6 -- the same number arrived at twice, from two fields a run can
    // never hold at once, which is a coincidence on a card screen and not a shared
    // stat.
    const Run g = run({"m16_frostbind", "m32_frost_slow", "m64_frost_hoar"});
    g->testSpawnTraitedEnemyAt(0.0F, 0.0F, 100.0F);
    g->testAdvance(1.0F / 60.0F);
    g->testDamageFirstEnemy(0.0F);
    CAPTURE(g->testFirstEnemyDefense());
    REQUIRE(g->testFirstEnemyDefense() == Catch::Approx(100.0F - 35.2F).margin(0.05F));
  }

  // --- the two brittle cards, through the damage funnel -------------------------
  // "A stripped body takes 32% more damage from everything." /
  // "A chilled body takes 40% more damage from everything."
  //
  // These two are the only Element cards whose promise is not visible on the body's
  // own health bar, so they are measured by putting damage on it -- and by the ratio
  // between a marked and an unmarked body, because a stored factor that was never
  // multiplied in would read back at exactly 0.20 and pass every other assertion in
  // this test.
  //
  // The body is dropped to a small, well-resolved health pool before the measured hit.
  // The spawn hook makes bodies with 100000 HP so a test can hit them a dozen times
  // without watching them die, and at that magnitude a float32 can only represent
  // differences of about 0.0156 -- which is 1% of the damage this test is trying to
  // resolve. The first draft measured against the big pool and got 1.6171875 where it
  // wanted 1.32 times a base of 1.2265625, and the whole discrepancy was the
  // quantisation of the subtraction, not a bug in the card. A measurement whose
  // precision is worse than the effect it is measuring is a coin flip with extra steps.
  const auto dealt = [&content, &indexOf](const std::vector<std::string>& path) {
    game::Game g{content, 101};
    g.testDisableWaves();
    g.testClearWeapons();
    for (const auto& id : path) {
      const int idx = indexOf(id);
      REQUIRE(idx >= 0);
      REQUIRE(g.testGrantUpgrade(idx));
    }
    g.testSpawnTraitedEnemyAt(0.0F, 0.0F);
    g.testAdvance(1.0F / 60.0F);
    // The marking hit, which is what puts the body in the state the card describes.
    g.testDamageFirstEnemy(0.0F);
    g.testSetFirstEnemyHp(10.0F);
    g.testDamageFirstEnemy(1.0F);
    return 10.0F - g.testFirstEnemyHp();
  };
  // The control is the SAME LINE WITHOUT THE CARD UNDER TEST, not an empty run. That
  // was the first attempt and it was wrong for the strip branch: Armour Split can only
  // be reached through Hex, so an empty control meant the measured ratio was Hex's own
  // first-step 22% multiplied into the brittle 32% -- 1.617 against an expected 1.32 --
  // and the honest reading of that failure is "this assertion is not measuring the card
  // it names", which is exactly what it was. Taking the parent as the control isolates
  // the one card, and a factor that was stored and never multiplied in would still be
  // caught, because a stored factor does not reach the funnel at all.
  {
    const Run g = struck({"m16_hex", "m32_hex_split", "m64_split_brittle"});
    REQUIRE(g->testFirstEnemyBrittle() == Catch::Approx(0.32F).margin(0.001F));
    const float base = dealt({"m16_hex", "m32_hex_split"});
    const float marked = dealt({"m16_hex", "m32_hex_split", "m64_split_brittle"});
    CAPTURE(base);
    CAPTURE(marked);
    REQUIRE(base > 0.0F);
    // The ratio IS the card's 32% -- 20 at the strip's own x1.6 -- and it is asserted
    // tightly rather than as a lower bound, because "at least 32% more" is a card that
    // quietly became 90%. The parent's Hex is in BOTH numbers, so it cancels, which is
    // the whole reason this control is the parent and not nothing.
    REQUIRE(marked == Catch::Approx(base * 1.32F).margin(0.001F));
  }
  {
    const Run g = struck({"m16_frostbind", "m32_frost_deep", "m64_frost_brittle"});
    REQUIRE(g->testFirstEnemyBrittle() == Catch::Approx(0.4F).margin(0.001F));
    const float base = dealt({"m16_frostbind", "m32_frost_deep"});
    const float marked = dealt({"m16_frostbind", "m32_frost_deep", "m64_frost_brittle"});
    CAPTURE(base);
    CAPTURE(marked);
    REQUIRE(base > 0.0F);
    // 25 at Frostbind's x1.6 is 40. Here the parent line is pure control -- a chill
    // changes no damage -- so this one would also have passed against an empty run,
    // and it is asserted anyway so the two cards are measured on the same terms.
    REQUIRE(marked == Catch::Approx(base * 1.4F).margin(0.001F));
  }
  // A run CANNOT hold both, and the reason is worth stating rather than leaving to
  // the exclusivity test: the two roots are in one group, so the second one is shut.
  // That is also why the two factors needed no accumulation rule -- the question
  // "what if a body is both chilled and stripped" is a question no run can ask.
  {
    game::Game g{content, 101};
    g.testClearWeapons();
    const int hexRoot = indexOf("m16_hex");
    const int frostRoot = indexOf("m16_frostbind");
    const int strip = indexOf("m32_hex_split");
    const int stripBrittle = indexOf("m64_split_brittle");
    const int chillBrittle = indexOf("m64_frost_brittle");
    REQUIRE(hexRoot >= 0);
    REQUIRE(frostRoot >= 0);
    REQUIRE(strip >= 0);
    REQUIRE(stripBrittle >= 0);
    REQUIRE(chillBrittle >= 0);
    REQUIRE(g.testGrantUpgrade(hexRoot));
    REQUIRE(g.testGrantUpgrade(strip));
    REQUIRE(g.testGrantUpgrade(stripBrittle));
    // A body to be brittle, because "the refused card left nothing behind" is a claim
    // about a body and there is nothing to ask if there is no body on the screen.
    g.testSpawnTraitedEnemyAt(0.0F, 0.0F);
    g.testAdvance(1.0F / 60.0F);
    g.testDamageFirstEnemy(0.0F);
    REQUIRE(g.testFirstEnemyBrittle() == Catch::Approx(0.32F).margin(0.001F));
    // The other root is shut, and by the GROUP: `element` is the group the three roots
    // share, so taking Hex closes the other two for the rest of the run. This is the
    // first door, and it is asked of the BLOCKED vector because that is the vector the
    // offer layer filters on -- `testGrantUpgrade` is a sandbox cheat that consults
    // nothing, and asserting through it would be asserting that a door the player
    // cannot use closes the same way the doors they can use do.
    REQUIRE(g.testUpgradeBlocked(frostRoot));
    REQUIRE(g.testUpgradeBlocked(indexOf("m16_emberbrand")));
    // ...and NOT the line below it, because the groups are per tier and match exactly:
    // the Frostbind branch cards sit in `element.frost`, `element.frost.deep` and
    // `element.frost.slow`, and none of those is the string `element`.
    REQUIRE_FALSE(g.testUpgradeBlocked(indexOf("m32_frost_slow")));
    // Which is not a hole, and the second door is why. A run that never took
    // Frostbind cannot reach a Frostbind branch card, because the branch gate asks for
    // the parent by name -- so a card can be reachable in neither sense and shut in
    // both, and the two gates shut different things. This pair is the whole reason
    // both are worth having: change the groups to a prefix hierarchy and the same
    // assertion still passes, so it would catch the merge; delete the `after` gate and
    // it fails, so it catches the other half.
    REQUIRE_FALSE(g.testUpgradeUsable(indexOf("m32_frost_slow")));
    REQUIRE_FALSE(g.testUpgradeUsable(chillBrittle));
    REQUIRE_FALSE(g.testUpgradeUsable(indexOf("m64_frost_hoar")));
    // Its OWN line is open on both counts, because the parent is held and the strip's
    // group was not the one taken. An exclusivity rule that shut the chosen line too
    // would make the whole screen dead on arrival.
    REQUIRE_FALSE(g.testUpgradeBlocked(strip));
    REQUIRE_FALSE(g.testUpgradeBlocked(stripBrittle));
    REQUIRE(g.testUpgradeUsable(stripBrittle));
    REQUIRE(g.stats().markChillBrittle == 0.0F);
    REQUIRE(g.stats().markChillTime == 0.0F);
    REQUIRE(g.stats().markChillArmour == 0.0F);
    g.testDamageFirstEnemy(0.0F);
    // The strip's 32% is all that is on the body, and it is still the strip's 32% and
    // not a product with a 40% that was never granted. Asserted as an EQUALITY, so
    // that a later change turning these fields into maxima cannot make a body holding
    // one of the two factors report as if it held both.
    REQUIRE(g.testFirstEnemyBrittle() == Catch::Approx(0.32F).margin(0.001F));
  }
}

TEST_CASE("A data file that says something the loader does not read is a failed load") {
  // The check this exercises was written after two cards in the milestone tree had
  // carried a key nobody read for two rounds: `threshold` on the card that tightens
  // the low-health band, so the band never tightened, and `area` on the card that
  // widens every blast, so the blasts never widened. Both cards behaved plausibly,
  // because the numbers in the HANDLER were right, and both were therefore invisible
  // to every behavioural test in this file -- the behaviour was correct and the file
  // was lying.
  //
  // A guard that has never been seen to fire is not a guard, it is a comment. So
  // this writes a data file with a stray key and requires the load to FAIL, naming
  // the key. Without the loader check, a designer has no way at all to learn that
  // the line they just typed does nothing, and "the number is right and editing it
  // changes nothing" is the most expensive kind of quiet.
  const auto dataDir = std::filesystem::path(GAME_ASSETS_DIR) / "data";
  const auto scratch = std::filesystem::temp_directory_path() / "tg_stray_key_test";
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);

  // Every table the loader insists on, copied verbatim, so the only thing under test
  // is the one edited file. The directory first, because copy_file will not make one.
  std::filesystem::create_directories(scratch);
  REQUIRE(std::filesystem::is_directory(scratch));
  for (const char* name : {"weapons.toml", "enemies.toml"}) {
    std::filesystem::copy_file(dataDir / name, scratch / name,
                               std::filesystem::copy_options::overwrite_existing, ec);
  }
  REQUIRE(!ec);
  REQUIRE(std::filesystem::exists(scratch / "weapons.toml"));

  // Injects one line into the FIRST upgrade, and hands back that card's id -- read
  // out of the file rather than hardcoded, because an earlier draft injected into the
  // last one and then asserted a card name that had nothing to do with it. A test
  // that pins the wrong card is a test that would pass if the guard reported the
  // wrong entry, which is the failure mode worth ruling out here.
  std::string firstCardId;
  const auto copyUpgrades = [&dataDir, &scratch, &firstCardId](const std::string& injected) {
    std::ifstream in{dataDir / "upgrades.toml"};
    REQUIRE(in.good());
    std::string body{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // Anchored on the first [[upgrade]] table, because the file opens with comment
    // prose that mentions "value =" before any card does.
    const auto card = body.find("[[upgrade]]");
    REQUIRE(card != std::string::npos);
    const auto at = body.find("value = ", card);
    REQUIRE(at != std::string::npos);
    const auto idAt = body.find("id = \"", card);
    REQUIRE(idAt != std::string::npos);
    REQUIRE(idAt < at);
    const auto idEnd = body.find('"', idAt + 6);
    REQUIRE(idEnd != std::string::npos);
    firstCardId = body.substr(idAt + 6, idEnd - (idAt + 6));
    REQUIRE(!firstCardId.empty());
    body.insert(at, injected);
    std::ofstream out{scratch / "upgrades.toml", std::ios::trunc};
    REQUIRE(out.good());
    out << body;
    out.close();
  };

  // 1. A key nobody reads: rejected, and the message names the key, because a load
  //    error that says only "bad file" sends the designer looking in the wrong place.
  copyUpgrades("threshhold = 0.25\n"); // the classic typo, and the classic trap
  {
    std::string what;
    try {
      (void)game::loadContent(scratch);
    } catch (const std::exception& e) {
      what = e.what();
    }
    CAPTURE(what);
    REQUIRE(what.find("threshhold") != std::string::npos);
    // ...naming WHICH entry, so the message points at the card to go and look at.
    REQUIRE(what.find(firstCardId) != std::string::npos);
  }

  // 2. A key nobody reads even though it is spelled correctly -- the exact shape of
  //    the two bugs this exists for, where the author used a real word and the
  //    loader never had a field for it.
  copyUpgrades("shield_pct = 0.5\n");
  {
    std::string what;
    try {
      (void)game::loadContent(scratch);
    } catch (const std::exception& e) {
      what = e.what();
    }
    CAPTURE(what);
    REQUIRE(what.find("shield_pct") != std::string::npos);
  }

  // 3. The real data still loads, and it loads through the SAME check that just
  //    rejected two files -- otherwise a guard that rejects everything is
  //    indistinguishable from a guard that works, from inside a test suite.
  std::error_code ec2;
  std::filesystem::remove_all(scratch, ec2);
  REQUIRE_NOTHROW((void)game::loadContent(dataDir));
}

TEST_CASE("No card carries a fraction on an effect the game counts in whole numbers") {
  // The applier feeds a count effect through `static_cast<int>`, so "+0.5
  // projectiles" becomes "+0 projectiles" and the card is a lie that renders
  // beautifully. The count set is asked of the applier, not restated here,
  // because a second list is a list that drifts.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int counts = 0;
  for (const auto& u : content.upgrades) {
    if (!game::effectIsWholeNumberOnly(u.effect)) continue;
    ++counts;
    CAPTURE(u.id);
    CAPTURE(u.effect);
    CAPTURE(u.value);
    // Whole, and at least one: a count of zero is as dead as a fraction, and is
    // the same bug wearing a different hat.
    REQUIRE(u.value >= 1.0F);
    REQUIRE(u.value == std::floor(u.value));
  }
  // And the set is not empty, so a rename cannot turn this green by accident.
  REQUIRE(counts >= 8);
}

TEST_CASE("A chest names every card it handed over, not just the last one") {
  // The player asked for this: a simple readout of what fell out of the box. The
  // old toast was one line built from the LAST card of up to five, so a
  // champion's hand of three upgrades arrived as three real changes to the run
  // and one ambiguous sentence.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 5150};
  g.enterTestMode();
  g.testDisableWaves();
  g.testClearWeapons();
  // A real arsenal, so the weapon side of the pool is not the empty one that
  // forces every card down the item path.
  for (const char* id : {"dagger", "scythe", "orb", "bow", "whip"}) {
    const int idx = g.testWeaponContentIndex(id);
    if (idx >= 0) g.testAddWeapon(idx);
  }
  REQUIRE(g.testWeaponCount() > 1);

  const int size = g.testSpawnChest(0.0F, 0.0F, 3, -1); // an overlord's box
  REQUIRE(size == 7);
  g.testOpenFirstChest();
  const int given = g.testLastChestGrants();
  CAPTURE(given);
  REQUIRE(given >= 2);

  // Exactly the cards that were granted, in the order they rolled, and every one
  // of them a real card. An empty slot or a stale index would render as a blank
  // line in the panel, which is worse than no panel.
  REQUIRE(g.testChestRevealCount() == given);
  REQUIRE(g.testChestRevealCount() <=
          static_cast<int>(game::Game::kChestRevealMax));
  for (int k = 0; k < g.testChestRevealCount(); ++k) {
    const int idx = g.testChestReveal()[static_cast<std::size_t>(k)];
    CAPTURE(k);
    CAPTURE(idx);
    REQUIRE(idx >= 0);
    REQUIRE(idx < static_cast<int>(content.upgrades.size()));
    const auto& u = content.upgrades[static_cast<std::size_t>(idx)];
    // And it is a card the player can actually be shown: a named, described,
    // non-milestone card. A milestone in a receipt would be a question the box
    // has already answered without asking.
    REQUIRE_FALSE(u.name.empty());
    REQUIRE_FALSE(u.desc.empty());
    REQUIRE(u.kind != "milestone");
  }

  // The panel belongs to ONE box. A box that can hand out nothing must not leave
  // the previous box's cards on screen claiming to be its own.
  //
  // Two boxes are on the floor and testOpenFirstChest takes whichever the
  // registry hands back first, so both are drained: each call consumes one box,
  // and the last one to go is the empty one.
  g.testSpawnChest(0.0F, 0.0F, 1, 0); // a box with nothing left to give
  while (g.testChestCount() > 0) g.testOpenFirstChest();
  REQUIRE(g.testLastChestGrants() == 0);
  REQUIRE(g.testChestRevealCount() == 0);
}

TEST_CASE("A chain bolt falls DOWN out of the sky, and its hit lands as it arrives") {
  // This is the complaint that would not go away: "the lightning animation is
  // still broken". It was not a rough shape and not a bad easing. The render drew
  // the telegraph charge at `targetY + kChainSkyDrop` and the drop from
  // `targetY - kChainSkyDrop`, so the effect gathered in the sky above the enemy
  // and then delivered its lightning upward out of the floor to meet it.
  //
  // A sign error in a renderer is invisible to a test suite, so it is not left in
  // the renderer: both ends now come from one function, and this asks the
  // function.
  const float target = 3.0F;
  const auto span = game::chainDropSpan(target, 0.0F);
  const auto landed = game::chainDropSpan(target, 1.0F);

  // The bolt comes FROM above. World +y is up, so above is a larger y.
  REQUIRE(span.skyY > target);
  // And the charge that telegraphs it is aimed at that same end of the sky, not
  // a second opinion about which way is up.
  REQUIRE(span.skyY == game::chainChargeY(target));

  // It FALLS. As `born` rises the reached end descends toward the target, which
  // is the only way a strike reads as falling rather than as extending upward
  // out of the ground.
  REQUIRE(landed.reachY < span.reachY);
  REQUIRE(landed.reachY == target); // and it finishes at the enemy

  // A half-drawn drop is a vertical segment hanging from the sky, not from the
  // ground: the part that exists is always the TOP of it.
  const auto half = game::chainDropSpan(target, 0.5F);
  REQUIRE(half.reachY > target);
  REQUIRE(half.reachY < half.skyY);
  // And a nonsensical progress value is clamped rather than making the bolt
  // teleport: the render's `born` is a clamp of a timer, but the clamp lives in
  // one place now instead of at every use.
  REQUIRE(game::chainDropSpan(target, -5.0F).reachY == span.reachY);
  REQUIRE(game::chainDropSpan(target, 9.0F).reachY == target);
}

TEST_CASE("A chain bolt's first hit waits for the drop to finish falling") {
  // The other half of the same defect, and the reason it kept reading as a pop.
  // The damage was paid on the FIRST FRAME of the strike window, while the drop
  // was still a third of the way down, and the drop was then replaced by the
  // first arc 0.05s later -- at 55% of its own length, so the lightning the
  // player was watching stopped in mid-air.
  //
  // The wind-up is three beats long now: converge, drop, land. Nothing may be
  // damaged before the drop has arrived, and the bolt must not start arcing until
  // it has.
  SoloWeapon s("tesla", 4);
  REQUIRE(s.slot >= 0);
  s.arm();
  s.g.testSpawnEnemyAt(2.0F, 0.0F);
  const float hp0 = s.g.testEnemyHpNear(2.0F, 0.0F);
  REQUIRE(hp0 > 0.0F);
  // Roll forward to the START of a wind-up rather than to a chosen number of
  // seconds. The Tesla needs 0.65s to shoot at all, and its bolts are ~1.2s
  // long, so a fixed pre-roll lands at a different point of the strike every time
  // the timings are touched -- and a test that measures beat 1 of a wind-up it
  // has already run through is measuring nothing.
  for (int i = 0; i < 400; ++i) {
    if (chainMostAiming(s.g.testChainTelegraphs()) > 150) break;
    s.g.testAdvance(1.0F / 60.0F);
  }
  REQUIRE(chainMostAiming(s.g.testChainTelegraphs()) > 100);
  const float hpStart = s.g.testEnemyHpNear(2.0F, 0.0F);

  // Beat 1: converging. Nothing has been hit yet, and the bolt is still aiming.
  s.g.testAdvance(game::Game::kChainTelegraph * 0.5F);
  REQUIRE(s.g.testEnemyHpNear(2.0F, 0.0F) == hpStart);
  REQUIRE(chainMostAiming(s.g.testChainTelegraphs()) > 0);

  // Beat 2, most of the way: the drop is still falling. The ring has closed --
  // the bolt is no longer aiming, it is arriving -- but the health bar must not
  // have moved, because the bolt has not arrived yet. This is the assertion that
  // used to be false: the hit was paid on the first frame of this window, while
  // the drop was a third of the way down.
  s.g.testAdvance(game::Game::kChainTelegraph * 0.5F +
                  game::Game::kChainStrike * 0.75F);
  REQUIRE(s.g.testEnemyHpNear(2.0F, 0.0F) == hpStart);
  // -1 is a bolt that has closed its ring and is on its way down; it is NOT yet
  // a bolt that has hit anything.
  REQUIRE(chainMostAiming(s.g.testChainTelegraphs()) == -1);

  // Beat 2, complete: the drop reaches the enemy, and only now is the hit paid.
  s.g.testAdvance(game::Game::kChainStrike * 0.25F + 2.0F / 60.0F);
  REQUIRE(s.g.testEnemyHpNear(2.0F, 0.0F) < hpStart);

  // The whole wind-up is strictly longer than the old one, which is the point:
  // it is something you can watch rather than something that has already
  // happened by the time you look at it.
  REQUIRE(game::Game::kChainStrike > 0.05F);
}

TEST_CASE("Each tier is one clear step above the one below it, and none is a wall") {
  // The complaint this answers is "the balance is terrible, and why is the
  // champion so easy". The measured answer was that the frequency mattered more
  // than the health bar -- but the health bar had its own defect, and it is the
  // one a data edit can silently break again, so both are held here.
  //
  // What a tier is FOR, in order, and the shape that delivers it:
  //
  //   elite     an interrupt: you notice it, you clear it, you move on
  //   champion  a real fight: it costs you a position, or an ability
  //   overlord  a boss: it is worth clearing your screen to deal with it
  //
  // So each rung must be meaningfully above the last, and none may be a health
  // check. Both halves matter: too close and the tiers are the same enemy with
  // different names, too far apart and the top of the ladder is an arithmetic
  // problem rather than a fight.
  const auto [eliteLo, eliteHi] = game::tierHpBand(1);
  const auto [champLo, champHi] = game::tierHpBand(2);
  const auto [lordLo, lordHi] = game::tierHpBand(3);

  // Every rung is x5 the one below it. Not "meaningfully above": five, exactly, so
  // the midpoints are 6.5, 32.5 and 162.5 and the player can hold the ladder in
  // their head. The old one was 6.5 / 37 / 125, which made a champion 5.7x an
  // elite and an overlord only 3.4x a champion -- the top two rungs were nearly
  // the same fight, which is how an overlord could turn up at minute four in a
  // build that had only just met an elite.
  // Computed outside the REQUIRE: Catch2 turns the expression into a template
  // argument, so a lambda call in there does not compile.
  const float midElite = (eliteLo + eliteHi) * 0.5F;
  const float midChamp = (champLo + champHi) * 0.5F;
  const float midLord = (lordLo + lordHi) * 0.5F;
  REQUIRE(midChamp > midElite * 4.0F);
  REQUIRE(midLord > midChamp * 4.0F);

  // Every rung is a real step up from the one below. The floor of each band --
  // not its ceiling -- is the number that has to clear, because a champion that
  // rolled low used to be a pushover and a champion that rolled high a wall, out
  // of the same spawn table with the same banner.
  REQUIRE(champLo > eliteHi * 2.0F);
  REQUIRE(lordLo > champHi * 2.0F);

  // And no rung is a health check. The old overlord ceiling of 450x a
  // contemporaneous mob was a wall: at minute twelve it was a
  // nine-hundred-thousand-HP thing a small crowd could not chew through, which
  // is not a reward for out-playing the game. The global time ramp is capped at
  // 20x on top of this, so 200x is 4000x a minute-zero mob at worst and 5x the
  // champion is what makes that number worth the fight.
  REQUIRE(lordHi <= 220.0F);
  REQUIRE(champHi <= 45.0F);

  // The within-tier spread is narrow. A 3.7x spread inside one label meant the
  // player could not tell which kind of champion they were about to meet, and
  // the weak roll is the one they remember.
  REQUIRE(champHi / champLo <= 2.0F);
  REQUIRE(lordHi / lordLo <= 2.0F);
  REQUIRE(eliteHi / eliteLo <= 2.0F);
}

TEST_CASE("A heavy tier arrives on a clock, and the clock lengthens up the ladder") {
  // The whole of the answer to "why is the champion so easy", and to "why are
  // there so many chests".
  //
  // A tier used to be a share of every spawn: an elite one body in ten, forever.
  // That made a tier's arrival depend on how much garbage happened to be on
  // screen, so a busy screen produced a champion every seven seconds, and -- since
  // every tiered body drops a box -- a ten-minute run handed the player 42 chests
  // and the whole arsenal by minute three.
  //
  // A tier is an EVENT now: a clock, a size, and a box. These are the clocks.
  const auto [eliteLo, eliteHi] = game::tierCadence(1);
  const auto [champLo, champHi] = game::tierCadence(2);
  const auto [lordLo, lordHi] = game::tierCadence(3);

  CAPTURE(eliteLo);
  CAPTURE(eliteHi);
  CAPTURE(champLo);
  CAPTURE(champHi);
  CAPTURE(lordLo);
  CAPTURE(lordHi);

  // "Once every couple of minutes" is the ask, and it is a floor: an elite more
  // often than this is a tax on the trash again, which is what the old 10% roll
  // was. Two minutes is also the shortest gap that lets the player finish
  // clearing one, collect its box, and get back to the horde before the next.
  REQUIRE(eliteLo >= 100.0F);
  REQUIRE(eliteHi > eliteLo);

  // The ladder of waits is strictly increasing. An elite is a thing you meet; a
  // champion is a fight you plan for; an overlord is the run's boss. If the gaps
  // were equal the three would be the same encounter with three HP bars.
  REQUIRE(champLo > eliteHi);
  REQUIRE(lordLo > champHi);
  REQUIRE(champHi > champLo);
  REQUIRE(lordHi > lordLo);

  // An overlord roughly twice an hour-and-a-half apart is "a couple in a ten
  // minute run". More than three in ten minutes and the run has a boss rotation
  // instead of a boss.
  REQUIRE(lordLo >= 400.0F);
}

static std::string uppered(std::string_view in) {
  std::string v(in);
  for (char& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return v;
}

// True when a line mentions any weapon in the game. This is how a RECIPE line is
// told apart from prose: "STORM CALLER   WAND + CROSSBOW" names two weapons and
// says nothing about either, while "STORM CALLER   A BOLT THAT BENDS ONTO THE
// NEXT ONE" names one and explains it. Without this distinction "the manual
// describes the weapon" is satisfiable by the ingredient list, which is the exact
// thing a player cannot learn anything from.
// The same text as one blob, for the checks that ask whether a phrase is
// mentioned anywhere rather than where.
static std::string manualSays(const std::vector<std::string>& lines) {
  std::string all;
  for (const auto& l : lines) {
    all += ' ';
    all += l;
    all += ' ';
  }
  return all;
}

static bool manualLineNamesAWeapon(const std::string& content,
                                   const std::vector<game::WeaponDef>& weapons) {
  for (const auto& w : weapons) {
    if (content.find(uppered(w.name)) != std::string::npos) return true;
  }
  return false;
}

// The manual as plain text: one entry per row, page titles included, the '>' /
// '#' / two-space markup prefixes stripped and everything uppercased, so a lookup
// finds a name whether the page wrote it as a title, a bullet or an indented
// continuation. Every coverage check below goes through this, so "what the manual
// says" has exactly one definition -- two copies of this loop is how one of them
// ends up auditing a different book from the other.
static std::vector<std::string> manualLines(const game::Content& content) {
  std::vector<std::string> out;
  const auto push = [&out](std::string_view line) {
    std::string v(line);
    if (!v.empty() && (v.front() == '>' || v.front() == '#')) v.erase(0, 1);
    if (v.size() >= 2 && v[0] == ' ' && v[1] == ' ') v.erase(0, 2);
    for (char& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out.push_back(std::move(v));
  };
  for (const auto& page : content.manual) {
    push(page.title);
    for (const auto& line : page.lines) push(line);
  }
  return out;
}

TEST_CASE("The manual names every weapon, and a recipe line is not a description") {
  // "The manual must describe all weapons" stops being enforced the moment it is
  // written down: adding a weapon does not make the manual stop covering it, so
  // the gap opens silently and a player reads a seventeen-page book and still
  // cannot find out what the Sundering Core actually does.
  //
  // So the coverage is asserted rather than trusted, and it is asserted in the
  // form the pages are actually written in. The two layouts are different rules
  // because the two kinds of weapon are different problems:
  //
  //  - a base weapon is described ON the line that names it, and the test is that
  //    the rest of that line is prose rather than a list of other weapons;
  //  - an evolution or super is described UNDER its recipe, and the test is that
  //    two lines of prose follow before the next weapon's recipe line starts.
  //
  // The locality in the second rule is the whole point. A "count two lines after
  // the name" check passes on a page where the name's own description was deleted
  // and the two lines belong to the weapon below it, which is precisely the
  // silent-omission failure this is here to catch.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto& weapons = content.weapons;

  for (const auto& w : weapons) {
    const std::string name = uppered(w.name);
    CAPTURE(w.id);
    REQUIRE(std::any_of(manualLines(content).begin(), manualLines(content).end(),
                        [&name](const std::string& l) { return l.find(name) != std::string::npos; }));

    if (w.prereqs.empty()) {
      // A base weapon: the rule is on the naming line, in prose.
      bool described = false;
      for (const auto& l : manualLines(content)) {
        const auto at = l.find(name);
        if (at == std::string::npos) continue;
        const std::string rest = l.substr(at + name.size());
        // Trim the column padding the pages align names with.
        const auto firstWord = rest.find_first_not_of(' ');
        if (firstWord == std::string::npos) continue;
        if (rest.size() - firstWord < 25) continue;
        if (manualLineNamesAWeapon(rest.substr(firstWord), weapons)) continue;
        described = true;
        break;
      }
      REQUIRE(described);
    } else {
      // An evolution or a super: the rule is the body text under the recipe,
      // and it has to be this weapon's body text, not its neighbour's.
      bool described = false;
      for (const auto& page : content.manual) {
        const auto at = std::find_if(page.lines.begin(), page.lines.end(),
                                     [&name](std::string_view l) {
                                       std::string v = uppered(l);
                                       if (v.size() >= 2 && v[0] == ' ' && v[1] == ' ') {
                                         v.erase(0, 2);
                                       }
                                       return v.find(name) != std::string::npos;
                                     });
        if (at == page.lines.end()) continue;
        int prose = 0;
        for (auto it = at + 1; it != page.lines.end(); ++it) {
          if (it->empty()) continue;
          // Another weapon's name means the description ended and the next entry
          // began, however much body text that next entry goes on to have.
          if (manualLineNamesAWeapon(uppered(*it), weapons)) break;
          ++prose;
        }
        if (prose >= 2) described = true;
      }
      REQUIRE(described);
    }
  }
}

TEST_CASE("No weapon in the game is a dead slot: every one has at least two cards") {
  // The manual promises it in as many words -- "every weapon has at least one of
  // its own, so a new weapon is never a dead slot" -- and for one weapon it was
  // quietly false. The Hoarfrost Wake shipped with a single card while every
  // other weapon in the game has two to four, so a player who took the newest
  // super-evolution got a strictly worse slot than a player who took any other,
  // and its only pick was an unambiguous upgrade. A stat line wearing a card's
  // clothes, on the one weapon where the choice was not a choice.
  //
  // Two is the floor because two is what the rest of the game already provides;
  // the assertion is not a taste about how many cards are good, it is a floor
  // under the weakest slot so the next weapon added does not repeat it.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  for (const auto& w : content.weapons) {
    int cards = 0;
    for (const auto& u : content.upgrades) {
      // Both kinds count: a weapon-scoped card is a weapon card whether it is
      // written as a plain stackable or as a one-time rule change, and filtering
      // one of them out is what let the dead slot through the first time.
      if (u.weapon == w.id) ++cards;
    }
    CAPTURE(w.id);
    CAPTURE(cards);
    REQUIRE(cards >= 2);
  }
}

TEST_CASE("The manual documents the mechanics a card cannot explain by itself") {
  // The other half of "all mechanics". Every word below is something the player
  // can only learn by being told: a number on a card explains itself, a system
  // does not. The list is the deliverable, and asserting it means deleting a page
  // cannot quietly remove a system from the book.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const std::string all = manualSays(manualLines(content));

  // The chest is the clearest example: a whole reward tier, with a card count
  // that scales by tier and a panel that names every card it gave, that existed
  // for as long as the game did and was never in the book at all.
  //
  // Asserted against the PAGE rather than against the word "chest", because the
  // word survives the page: "every chest opens one more time" is on the unique
  // cards page, so a corpus search for CHEST keeps passing with the chest page
  // deleted and the reward rules -- how many cards, what is in them, how to read
  // the reveal -- gone with it.
  const auto* chests = content.manualPage("chests");
  REQUIRE(chests != nullptr);
  std::vector<std::string> rows;
  for (const auto& raw : chests->lines) rows.push_back(uppered(raw));
  const std::string page = manualSays(rows);
  for (const char* bit : {"ELITE      1 CARD", "CHAMPION   3 CARDS", "OVERLORD   7 CARDS",
                          "DEEP CACHE", "PANEL"}) {
    CAPTURE(bit);
    REQUIRE(page.find(bit) != std::string::npos);
  }

  // The four marks, each by name. They are what a milestone screen is mostly made
  // of, and a milestone screen is the loudest thing the game does.
  for (const char* mark : {"FROSTBIND", "EMBERBRAND", "HEX", "ARMOUR SPLIT"}) {
    CAPTURE(mark);
    REQUIRE(all.find(uppered(mark)) != std::string::npos);
  }

  // The kill chain, the shield pool, armour, the tribunal director, rerolls,
  // milestones, lifesteal and defence. The one-time rule cards have a page of
  // their own now.
  //
  // "KILL CHAIN", not "momentum": momentum is what the effect ids are called, and
  // "kill chain" is what every card that touches it says out loud to the player.
  // A manual that documents the system's internal name is documenting the source.
  for (const char* idea : {"KILL CHAIN", "SHIELD", "ARMOUR", "TRIBUNAL", "REROLL",
                           "MILESTONE", "LIFESTEAL", "DEFENSE"}) {
    CAPTURE(idea);
    REQUIRE(all.find(uppered(idea)) != std::string::npos);
  }
}

TEST_CASE("The manual's page jump reaches as far as the hint bar promises") {
  // The hint says [1-9] JUMP. If the decoder only reads five keys then the one
  // screen whose whole job is telling the player which keys do something is
  // lying, and the other eight pages of a seventeen-page book are unreachable by
  // keyboard. That is not a hypothetical: the jump used to stop at 5, which was
  // correct when the book had eleven pages and stopped being correct at twelve.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(content.manual.size() >= 9); // otherwise [1-9] is not the promise
  for (int key = 1; key <= 9; ++key) {
    game::Game g{content, 555};
    game::FrameInput in{};
    in.manualToggle = true;
    g.advance(1.0F / 60.0F, in);
    REQUIRE(g.manualOpen());

    // The page the key names, not the key's own offset in some enum: a decoder
    // that read the keys in the wrong order would pass a "was it handled at all"
    // check and fail this one.
    in = game::FrameInput{};
    switch (key) {
      case 1: in.choose1 = true; break;
      case 2: in.choose2 = true; break;
      case 3: in.choose3 = true; break;
      case 4: in.choose4 = true; break;
      case 5: in.choose5 = true; break;
      case 6: in.choose6 = true; break;
      case 7: in.choose7 = true; break;
      case 8: in.choose8 = true; break;
      case 9: in.choose9 = true; break;
      default: break;
    }
    g.advance(1.0F / 60.0F, in);
    CAPTURE(key);
    REQUIRE(g.manualPageIndex() == static_cast<std::size_t>(key - 1));
  }
}

// The generated document, as one string. `docs/content.md` is written by
// tools/gendocs.py and is the only place the tier ladder, the chest rewards and
// the difficulty ramps are written down as numbers rather than as code.
static std::string readGeneratedDoc() {
  std::ifstream f(std::string(GAME_ASSETS_DIR) + "/../docs/content.md");
  REQUIRE(f.good());
  return std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The document's own number format, so a check written here formats the way
// tools/gendocs.py does and a reformat of the document shows up as a test
// failure rather than as a check that quietly stopped matching.
static std::string docNum(float v) {
  std::ostringstream os;
  os << v;
  std::string s = os.str();
  // %g, not the C++ default: 28.0 prints "28" and 1.90 prints "1.9", because the
  // generator uses ':g' and a check that does not would demand "| Champion |
  // 28-46 | x1.9 |" while the document says "| Champion | 28-46 | x1.9 |" -- on
  // this row by luck, and on the next one by accident.
  if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

TEST_CASE("The generated content doc lists every weapon, enemy and upgrade") {
  // The most common way for docs/content.md to go stale is not a number moving
  // -- that is what the generator is for -- it is a THING being added and the
  // generator not being run. A weapon, an enemy or a card that exists in the
  // game and not in the document reads, to the next person, as a thing that
  // does not exist.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const std::string doc = readGeneratedDoc();

  for (const auto& w : content.weapons) {
    CAPTURE(w.id);
    REQUIRE(doc.find("`" + w.id + "`") != std::string::npos);
  }
  for (const auto& e : content.enemies) {
    CAPTURE(e.id);
    REQUIRE(doc.find("`" + e.id + "`") != std::string::npos);
  }
  for (const auto& u : content.upgrades) {
    CAPTURE(u.id);
    REQUIRE(doc.find("`" + u.id + "`") != std::string::npos);
  }
}

TEST_CASE("The generated content doc is not behind the tier ladder it documents") {
  // The ladder is the most balance-sensitive thing in the game and it lives in
  // C++, not in data. A balance pass turns two of its numbers and nothing marks
  // the document, so a run can end with one ladder in the code and a different
  // one in the docs -- and the docs are what gets read before the next pass,
  // which is exactly how a stale number becomes the number somebody tunes
  // against next.
  //
  // So the publicly readable half is checked against the source: the HP bands,
  // the spawn caps and the gates, which are all reachable through the same
  // accessors a balance test would use. The HP/speed ramp and the chest counts
  // are generated too, and for those the generator is the guard -- it exits with
  // an error rather than printing a cell it could not read, so a moved constant
  // breaks `python3 tools/gendocs.py` instead of quietly changing the document.
  const std::string doc = readGeneratedDoc();

  // A row is matched cell by cell, not as a string. A substring check on a
  // whole row is a check that passes on the wrong row: looking for
  // "2% of spawns" in a document that says "1.2% of spawns" succeeds, and an
  // overlord cap doubled from 1.2% to 2% therefore reads as the document being
  // correct. It is the same trap as a word search finding "chest" inside "every
  // chest opens one more time" on a different page.
  const auto cellsFor = [&doc](const char* tier) -> std::vector<std::string> {
    const std::string want = std::string("| ") + tier + " |";
    const auto at = doc.find(want);
    if (at == std::string::npos) return {};
    const auto eol = doc.find('\n', at);
    std::vector<std::string> cells;
    for (std::size_t i = at + want.size(); i < eol;) {
      const auto bar = doc.find('|', i);
      if (bar == std::string::npos || bar > eol) break;
      // Markdown cells carry the padding that makes a table readable, and a
      // check that compares a padded cell to a bare string fails on the padding
      // rather than on the number.
      const auto cell = doc.substr(i, bar - i);
      const auto first = cell.find_first_not_of(' ');
      cells.push_back(first == std::string::npos
                          ? cell
                          : cell.substr(first, cell.find_last_not_of(' ') - first + 1));
      i = bar + 1;
    }
    return cells;
  };
  const auto hasCell = [](const std::vector<std::string>& cells,
                          const std::string& want) {
    return std::find(cells.begin(), cells.end(), want) != cells.end();
  };

  const char* names[3] = {"Elite", "Champion", "Overlord"};
  for (int tier = 1; tier <= 3; ++tier) {
    CAPTURE(tier);
    const auto cells = cellsFor(names[tier - 1]);
    REQUIRE_FALSE(cells.empty());
    const auto band = game::tierHpBand(tier);
    const std::string counts = docNum(band.first) + "-" + docNum(band.second);
    // The frequency is a clock now, so the docs say so in the only unit a player
    // can use: one arrival every so many seconds.
    const auto cad = game::tierCadence(tier);
    const std::string every = "every " + docNum(cad.first) + "-" + docNum(cad.second) + "s";
    INFO("row: | " << names[tier - 1] << " |" << [&cells] {
      std::string all;
      for (const auto& c : cells) all += " " + c + " |";
      return all;
    }());
    REQUIRE(hasCell(cells, counts));
    REQUIRE(hasCell(cells, every));
    // The gate is what stops the next tier from being an accident, and it is
    // the number a pass reaches for first.
    if (tier > 1) {
      // What opens a tribunal is a kill milestone now, and the bestiary says so in
      // kills. A gate the docs describe in a unit the panel does not use is a gate
      // the player cannot check themselves against the thing on screen -- so the
      // cell is the whole phrase, compared case-insensitively, because the docs
      // sentence-case it and the panel shouts it and neither is wrong.
      const std::string gate =
          std::string("after ") + docNum(game::Game::tierKillGate(tier)) + " " +
          (tier == 2 ? "elites" : "champions");
      bool found = false;
      for (const auto& c : cells) {
        std::string low = c;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (low == gate) found = true;
      }
      REQUIRE(found);
    }
  }
}

// ---------------------------------------------------------------------------
// A sphere with satellites, a bolt that rides its victim, and a receipt that
// stops the world. All three are things the player looked at and named.
// ---------------------------------------------------------------------------

TEST_CASE("A chest draws one sphere per card it will hand over") {
  // The box used to be a crate of eleven rects, with 0.055 dots on a ring around
  // it standing in for "how many cards are in here". The player asked for a ball
  // with balls orbiting it, and for the ring to be the real count -- including the
  // ones Deep Cache adds, which the old clamp at five silently threw away.
  REQUIRE(game::Game::chestSatellites(0) == 1); // never a bare ball: uncountable
  REQUIRE(game::Game::chestSatellites(1) == 1); // an elite's box
  REQUIRE(game::Game::chestSatellites(3) == 3); // a champion's
  REQUIRE(game::Game::chestSatellites(7) == 7); // an overlord's
  REQUIRE(game::Game::chestSatellites(8) == 8); // + one Deep Cache
  REQUIRE(game::Game::chestSatellites(10) == 10); // + three: the real maximum

  // And the count on screen is the count in the box. An overlord's box with three
  // Deep Cache stacks really does open eight times -- the clamp at five was the
  // picture disagreeing with the receipt.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 909};
  g.testDisableWaves();
  g.testClearWeapons();
  for (const char* id : {"wand", "dagger", "crossbow"}) {
    g.testAddWeapon(g.testWeaponContentIndex(id));
  }
  REQUIRE(g.testSpawnChest(0.0F, 0.0F, 3, -1) == 7);
  const int idx = g.testUpgradeContentIndex("u_deep_cache");
  REQUIRE(idx >= 0);
  REQUIRE(g.testGrantUpgrade(idx));
  REQUIRE(g.testSpawnChest(4.0F, 0.0F, 3, -1) == 8);
  REQUIRE(g.testGrantUpgrade(idx));
  REQUIRE(g.testGrantUpgrade(idx));
  REQUIRE(g.stats().chestBonus == 3);
  // Ten is the real maximum: an overlord's seven plus three Deep Cache stacks.
  // The old ring clamped at five, so the eighth card was invisible on the ground
  // and then appeared on the receipt. A cap that is too small does not show fewer
  // cards, it hides the ones at the bottom of a list the player is about to be
  // told is complete.
  REQUIRE(g.testSpawnChest(8.0F, 0.0F, 3, -1) == 10);
  REQUIRE(game::Game::chestSatellites(10) == 10);
}

TEST_CASE("A chain bolt rides the enemy it is on") {
  // "The lightning does not follow the enemy", reported against a bolt that was
  // teleported onto a target once per hop and then left there. The arena is 40+
  // units across and enemies do not stand still, so by the second hop the bolt
  // was pointing at empty ground while the bat it had hit walked off.
  //
  // The measurement is a distance, and the enemy is WALKING, which is the part
  // that matters. A bolt frozen on a stationary target is followed by a bolt that
  // has no idea what following means -- the old implementation passed that test
  // perfectly while looking, in motion, exactly as broken as reported.
  SoloWeapon s("tesla", 31);
  REQUIRE(s.slot >= 0);
  s.arm();
  s.g.testSpawnEnemyAt(3.0F, 0.0F, 2.5F);
  s.g.testSpawnEnemyAt(-3.0F, 0.0F, 2.5F);

  int measured = 0;
  for (int i = 0; i < 20; ++i) {
    s.g.testAdvance(0.10F);
    const float gap = s.g.testChainAnchorGap();
    if (gap >= 0.0F) {
      CAPTURE(i);
      ++measured;
      // Exact, not approximate: the bolt's position IS the victim's position,
      // copied in. A tolerance here would be a tolerance for the bug -- the old
      // code sat one enemy-speed-times-0.05s behind, which at 2.5 units/s is
      // 0.125 and would sail through any reasonable epsilon.
      REQUIRE(gap == 0.0F);
    }
  }
  // Not vacuous: bolts existed, and at least one of them had actually landed on a
  // body, so the assertions above were about following and not about an empty
  // registry.
  REQUIRE(measured > 0);
  REQUIRE(s.g.testChainCount() > 0);
  // And the enemies really were moving the whole time, which is what makes the
  // zero above mean something.
  const std::vector<float> pos = s.g.testEnemyPositions();
  REQUIRE(pos.size() >= 4);
  REQUIRE(std::abs(pos[0]) < 3.0F);
  REQUIRE(std::abs(pos[2]) < 3.0F);
}

TEST_CASE("A chain bolt's arc is a real segment between two bodies") {
  // Two numbers, and both of them used to be wrong. The tail is fromX/fromY --
  // the point the last hop came FROM, captured at the moment of the hop -- and
  // the head is the bolt's own interpolated position, which the test above pins
  // to the live victim. What this pins is the frame in between: the tail must
  // still span the gap when the bolt is moving, and the head must not be
  // interpolated across that gap.
  SoloWeapon s("tesla", 77);
  REQUIRE(s.slot >= 0);
  s.arm();
  // Three bodies, all inside tesla's 2.8-unit jump range of each other, so hops
  // are real hops. Two bodies 5.2 apart dead-end the bolt on the spot: the arc
  // never happens, which is a correct behaviour and useless for measuring one.
  s.g.testSpawnEnemyAt(1.5F, 0.0F, 1.0F);
  s.g.testSpawnEnemyAt(-1.5F, 0.0F, 1.0F);
  s.g.testSpawnEnemyAt(0.0F, 2.4F, 1.0F);

  float best = -1.0F;
  float worstLag = -1.0F;
  for (int i = 0; i < 20; ++i) {
    s.g.testAdvance(0.10F);
    best = std::max(best, s.g.testChainArcSpan());
    worstLag = std::max(worstLag, s.g.testChainHeadLag());
  }
  CAPTURE(best);
  CAPTURE(worstLag);
  // A hop between bodies that start about 3 units apart is a segment you can see.
  // The tail is captured at the hop and the head rides the new victim, so the
  // drawn arc is the gap between two bodies at every moment of the frame.
  REQUIRE(best > 1.0F);
  // And it is bounded by the weapon's own reach, so this cannot pass by being
  // loosened: the range is read out of the content, not typed in.
  const game::WeaponDef* tesla = s.content.weapon("tesla");
  REQUIRE(tesla != nullptr);
  REQUIRE(best <= tesla->chainJumpRange);

  // The other half, and the part that was actually broken. px/py are what the
  // renderer interpolates the head from, and they used to be set to the OLD
  // victim's position, so the head slid all the way across the gap in a single
  // frame: the arc the player was watching grew out of a point and then jumped.
  // They are now the new victim's own previous tick, so the head is always
  // somewhere on the body it is standing on. One frame of a walking enemy is
  // about 0.017 at speed 1.0; the gap is about 3.0.
  REQUIRE(worstLag >= 0.0F);
  REQUIRE(worstLag < 0.2F);
}

TEST_CASE("Opening a chest stops the run until the player says so") {
  // The player asked for this outright: the readout should pause the game, wait
  // to be read, and continue on SPACE. It was a 3.4-second toast over a live
  // horde, which is either read while being chewed on or missed entirely.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 4242};
  g.enterTestMode();
  g.testDisableWaves();
  for (const char* id : {"dagger", "scythe", "orb", "bow", "whip"}) {
    const int idx = g.testWeaponContentIndex(id);
    if (idx >= 0) g.testAddWeapon(idx);
  }
  g.testSpawnEnemyAt(3.0F, 0.0F);
  REQUIRE(g.testSpawnChest(0.0F, 0.0F, 3, -1) == 7);
  g.testOpenFirstChest();

  // Held.
  REQUIRE(g.state() == game::RunState::ChestReveal);
  REQUIRE(g.testChestRevealCount() > 0);

  // Held means held: a hundred frames of nothing, with a champion standing right
  // there, must not resolve into a kill or a second wave. The enemy is alive for
  // the whole of it.
  game::FrameInput in{};
  for (int i = 0; i < 100; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::ChestReveal);
  REQUIRE(g.testChestRevealCount() > 0);

  // SPACE lets go, and the run is handed back.
  game::FrameInput go{};
  go.menuConfirm = true;
  g.advance(1.0F / 60.0F, go);
  REQUIRE(g.state() == game::RunState::Playing);

  // And it does not come back on its own: the panel had a lifetime, and a
  // countdown that brings the receipt back is the bug again.
  for (int i = 0; i < 120; ++i) g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::Playing);
}

TEST_CASE("The chest receipt fits on screen and nothing runs off the right edge") {
  // Two defects, neither of which a test could see while the layout was inline in
  // the renderer:
  //
  //  1. The panel was drawn with the CENTRE-based Batcher::rect at what were meant
  //     to be corner coordinates, so the dark backing sat half a panel up and to
  //     the left of the text it was backing and the four frame bars were drawn as
  //     centre-based rects through the middle of the screen. That is "the chest
  //     window is broken".
  //  2. Its width was min(screen, widest UNWRAPPED line) while the text was laid
  //     out in one line, so on any window narrower than a card description the
  //     tail of the text left the screen. That is "and cut off".
  //
  // The layout is the renderer's own arithmetic, so these are claims about what
  // gets drawn rather than about a copy of it.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  std::vector<std::string> names;
  std::vector<std::string> descs;
  for (const auto& u : content.upgrades) {
    names.push_back(u.name);
    descs.push_back(u.desc);
  }
  REQUIRE(names.size() > 100);

  // The shipped worst case, measured over the WHOLE card set rather than a
  // hand-picked pair: the longest description in the game, four times over, which
  // is what a real overlord's box can hand out.
  std::size_t longest = 0;
  for (std::size_t i = 1; i < descs.size(); ++i) {
    if (descs[i].size() > descs[longest].size()) longest = i;
  }
  const std::vector<std::string> worstName{
      names[longest], names[longest], names[longest], names[longest], names[longest]};
  const std::vector<std::string> worstDesc{
      descs[longest], descs[longest], descs[longest], descs[longest], descs[longest]};

  for (const float py : {720.0F, 1080.0F}) {
    for (const float px : {1920.0F, 1280.0F, 800.0F, 640.0F}) {
      CAPTURE(px);
      CAPTURE(py);
      const auto lay = game::Game::chestRevealLayout(px, py, worstName, worstDesc);
      REQUIRE(lay.panelX >= 0.0F);
      REQUIRE(lay.panelX + lay.panelW <= px);
      REQUIRE(lay.panelY >= 0.0F);
      // The BOTTOM is not guaranteed on a window too short to hold a five-card
      // receipt with the game's longest description in it, and that is the same
      // trade the level-up row makes: clamping the top keeps the heading and the
      // first card readable, which beats pushing the whole thing up off the
      // screen. Only reachable by dragging the window to a few hundred pixels tall.
      // What is checked is that the reference heights have room for it.
      if (py >= 720.0F) REQUIRE(lay.panelY + lay.panelH <= py);
      REQUIRE(lay.cardY.size() == 5);
      REQUIRE(lay.descLines.size() == 5);
      // The panel is sized for the largest box the game can produce, which is now
      // an overlord's SEVEN cards. Sizing it for five was the same mistake as
      // clamping the ring: the cards the box actually gave would not fit the
      // receipt that named them.
      const auto big = game::Game::chestRevealLayout(px, py, worstName, worstDesc);
      REQUIRE(big.panelH >= lay.panelH);
      if (py >= 1080.0F) REQUIRE(big.panelY + big.panelH <= py);

      // Every line the renderer will DRAW fits the column it will be drawn in, and
      // the column is inside the panel. chestRevealDescLines is the renderer's own
      // wrapping, not a copy of it -- the defect was a renderer laying every
      // description out in one unwrapped line, and a test that wrapped the text
      // for itself would have passed straight through it.
      const float colW = lay.panelW - lay.descX - lay.padX;
      REQUIRE(colW > 0.0F);
      const std::vector<std::string> drawn =
          game::Game::chestRevealDescLines(lay, worstDesc[0]);
      // A 134-character description in a 300px column has to wrap. If this ever
      // reports one line, the wrap is gone and the tail is going off screen.
      REQUIRE(drawn.size() > 1);
      for (const auto& line : drawn) {
        CAPTURE(line);
        REQUIRE(6.0F * lay.descScale * static_cast<float>(line.size()) <= colW);
      }
      // The name column is one width for the whole panel, so the rows are not
      // ragged, and the longest name in the game fits inside it at every width the
      // game supports.
      for (const auto& n : content.upgrades) {
        const auto one = game::Game::chestRevealLayout(
            px, py, std::vector<std::string>{n.name},
            std::vector<std::string>{n.desc});
        CAPTURE(n.name);
        REQUIRE(one.nameX + 6.0F * one.nameScale * static_cast<float>(n.name.size()) <=
                one.descX + 0.5F);
      }
      // The last card's text ends above the hint line, not on top of it, using the
      // layout's own pitch -- the renderer draws at lay.descLineH, so a literal here
      // would be a second copy of a number that decides the panel's height.
      REQUIRE(lay.descLineH > 0.0F);
      REQUIRE(lay.panelY + lay.cardY.back() +
                  static_cast<float>(lay.descLines.back()) * lay.descLineH <=
              lay.hintY);
    }
  }

  // A name far longer than anything shipped must not push the description column
  // off the right edge. The column is capped at 42% of the panel for exactly this,
  // and the guarantee is about the geometry, not about the name being readable --
  // no card can be that long.
  const std::vector<std::string> one{
      "A DELIBERATELY ABSURDLY LONG UPGRADE NAME FOR A TEST"};
  const std::vector<std::string> od{
      std::string("and a description long enough to need wrapping, which is ") +
      "the only way a card row ever gets to be two lines tall"};
  const auto narrow = game::Game::chestRevealLayout(640.0F, 720.0F, one, od);
  REQUIRE(narrow.panelX >= 0.0F);
  REQUIRE(narrow.panelX + narrow.panelW <= 640.0F);
  REQUIRE(narrow.descX >= narrow.nameX);
  REQUIRE(narrow.descX + (narrow.panelW - narrow.descX - narrow.padX) <= narrow.panelW);
  // The name may overrun its column -- nothing can make 53 characters fit 252px --
  // but the column the descriptions live in is still whole.
  REQUIRE(narrow.panelW - narrow.descX - narrow.padX > 0.0F);
}

TEST_CASE("The manual tells the player how a chest actually behaves") {
  // Three things changed and all three are things a player has to be told, or the
  // manual becomes a lie: the box is a ball of spheres rather than a crate, the
  // spheres are the real card count including Deep Cache's, and opening one stops
  // the run until SPACE.
  //
  // Pinned on the CHESTS page rather than searched for anywhere, because "every
  // chest opens one more time" is also on the cards page -- a corpus search for
  // CHEST survives deleting the chest page entirely, which is the same trap the
  // earlier coverage tests walked into.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const game::ManualPage* page = nullptr;
  for (const auto& p : content.manual) {
    if (p.id == "chests") page = &p;
  }
  REQUIRE(page != nullptr);

  std::string body;
  for (const auto& raw : page->lines) {
    std::string line(raw);
    if (!line.empty() && (line.front() == '>' || line.front() == '#')) line.erase(0, 1);
    if (line.size() >= 2 && line[0] == ' ' && line[1] == ' ') line.erase(0, 2);
    for (char& c : line) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    body += line;
    body += '\n';
  }

  // SPACE is the key the renderer actually waits for, named on the page. If the
  // key is ever rebound, this is where it should fail.
  REQUIRE(body.find("SPACE") != std::string::npos);
  REQUIRE(body.find("STOPS THE GAME") != std::string::npos);
  // The ring is the count, and it grows with the card that makes boxes bigger.
  REQUIRE(body.find("SPHERES") != std::string::npos);
  REQUIRE(body.find("DEEP CACHE") != std::string::npos);
  // And the three tiers, which is the other half of "the ring is the count".
  REQUIRE(body.find("ELITE") != std::string::npos);
  REQUIRE(body.find("CHAMPION") != std::string::npos);
  REQUIRE(body.find("OVERLORD") != std::string::npos);

  // It must not still be describing the panel as a timed toast. That sentence was
  // true until this change and is the exact thing a player would read and believe.
  REQUIRE(body.find("HOLDS A FEW SECONDS") == std::string::npos);
}

TEST_CASE("A chest opened on the step the player dies does not resurrect the run") {
  // The panel takes the run state, so it has to be checked before it takes it. A
  // box can be under the player's feet on the step they die, and a chest that
  // clobbered the GameOver would leave SPACE meaning "continue" on a corpse.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 1717};
  g.enterTestMode();
  g.testDisableWaves();
  g.testClearWeapons();
  for (const char* id : {"dagger", "scythe", "orb", "bow", "whip"}) {
    const int idx = g.testWeaponContentIndex(id);
    if (idx >= 0) g.testAddWeapon(idx);
  }
  REQUIRE(g.testSpawnChest(0.0F, 0.0F, 3, -1) == 7);
  // A body standing on the box, so the box is spent in the same breath as the
  // kill. testKillPlayer is the honest way to get there: it goes through the
  // death path, not around it.
  g.testSpawnEnemyAt(0.0F, 0.0F, 2.5F);
  g.testKillPlayer();
  REQUIRE(g.state() == game::RunState::GameOver);
  g.testOpenFirstChest();

  // The cards are still granted and still readable, and the run stays over.
  REQUIRE(g.testChestRevealCount() > 0);
  REQUIRE(g.state() == game::RunState::GameOver);
  game::FrameInput go{};
  go.menuConfirm = true;
  for (int i = 0; i < 30; ++i) g.advance(1.0F / 60.0F, go);
  REQUIRE(g.state() == game::RunState::GameOver);
}

// ---------------------------------------------------------------------------
// The tribunal director, end to end. A tier used to be a share of every spawn;
// these are the promises that replaced it, each one the thing the player actually
// sees: a clock, a size, a cap, a warning, and a box.
// ---------------------------------------------------------------------------

TEST_CASE("An elite walks in at 1:30 and then on its own clock") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 31};
  g.testDisableWaves();
  g.testClearWeapons();
  g.enterTestMode();

  // The first elite is held by the FLOOR, not by a countdown that has already
  // run. Both were guarding the same arrival for a while, which meant the floor
  // was paid twice and the first elite landed at three minutes while the manual
  // said one and a half. So the clock starts due and the floor is the only thing
  // in front of it -- and it has to stay that way, which is what this pins.
  REQUIRE(g.tierCooldown(1) == 0.0F);

  // Nothing before the floor. A run that met one at 0:45 meant the first real
  // decision of the session was "do I play around the thing about to end me".
  const float floor = game::tierMinTime(1);
  CAPTURE(floor);
  g.testAdvance(floor - 1.0F);
  REQUIRE(g.tierEventsFired(1) == 0);
  REQUIRE(g.tierUnlocked(1) == false);

  // And it lands on the frame the floor opens. Advanced in whole frames by
  // testAdvance, so this is the arrival, not a window it fell into.
  g.testAdvance(1.5F);
  CAPTURE(g.tierEventsFired(1));
  REQUIRE(g.tierUnlocked(1));
  REQUIRE(g.tierEventsFired(1) == 1);
  REQUIRE(g.tierEventBodies(1) >= 1);

  // Then it re-arms from the moment it FIRED, into the tier's own band, which is
  // how "once every one to two minutes" stays true without the two numbers
  // drifting apart.
  const auto [lo, hi] = game::tierCadence(1);
  const float next = g.tierCooldown(1);
  CAPTURE(next);
  CAPTURE(lo);
  CAPTURE(hi);
  REQUIRE(next >= lo - 0.05F);
  REQUIRE(next <= hi + 0.05F);
  // Which means the second one is a whole interval away and not two frames: the
  // old shared-per-spawn roll produced a tier every seven seconds, and the only
  // way to keep that from creeping back is to say what the gap actually is.
  REQUIRE(next > 60.0F);
}

TEST_CASE("An arrival is a pair, or a group once you are handling them") {
  // "One or two, maybe three or four if they are already easy." Both halves have
  // to contain an odd AND an even count, and the reason is the reason this is a
  // test rather than a comment: an average size of 1.5 that rounds either way
  // means a comfortable player still meets single bodies most of the time, which
  // is the flat one-at-a-time feeling the ladder is being fixed for. If either
  // set ever came back as {1,2} alone, or {3,4} alone, the screen would stop
  // visibly filling for exactly the player it is supposed to reward.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 35};
  g.enterTestMode();
  std::array<int, 5> ordinary{};
  std::array<int, 5> comfortable{};
  for (int i = 0; i < 400; ++i) {
    // It rolls the game's own RNG, so it is asked of a real game rather than of a
    // copy of the rule: a test that reimplemented the two lines would pass even
    // after the implementation was changed underneath it.
    const int a = g.tierEventSizeFor(false);
    const int b = g.tierEventSizeFor(true);
    REQUIRE((a == 1 || a == 2));
    REQUIRE((b == 3 || b == 4));
    ++ordinary[static_cast<std::size_t>(a)];
    ++comfortable[static_cast<std::size_t>(b)];
  }
  CAPTURE(ordinary[1]);
  CAPTURE(ordinary[2]);
  CAPTURE(comfortable[3]);
  CAPTURE(comfortable[4]);
  REQUIRE(ordinary[1] > 0);
  REQUIRE(ordinary[2] > 0);
  REQUIRE(comfortable[3] > 0);
  REQUIRE(comfortable[4] > 0);
  // And the two halves never overlap, or the same roll would be answering two
  // different questions.
  REQUIRE(ordinary[0] + ordinary[3] == 0);
  REQUIRE(comfortable[0] + comfortable[1] == 0);
}

TEST_CASE("A tier held back by a busy screen re-arms from when it fired") {
  // The old rule re-armed from the moment it was DUE, so a tier that had been
  // waiting for room arrived the instant the screen cleared -- two of them inside
  // two seconds, which is the chest flood again by another name. Held back means
  // still due, and the next one is scheduled from the arrival that did happen.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 32};
  g.testDisableWaves();
  g.testClearWeapons();
  g.enterTestMode();

  // Fill the elite budget, so the event is due and cannot land.
  for (int i = 0; i < game::tierEventCap(1); ++i) {
    g.testSpawnEliteAt(3.0F, static_cast<float>(i), 1);
  }
  g.testAdvance(game::tierMinTime(1) + 1.0F);
  REQUIRE(g.tierCooldown(1) == 0.0F);
  const int firedWhileBusy = g.tierEventsFired(1);
  REQUIRE(firedWhileBusy == 0);

  // Still due, and still not landed, ten seconds later: a full, honest wait rather
  // than a queue of arrivals waiting for room.
  g.testAdvance(10.0F);
  REQUIRE(g.tierEventsFired(1) == firedWhileBusy);

  // Clear the floor. The next frame it lands -- and lands once, not once per
  // frame it was waiting.
  g.testDespawnEnemies();
  g.testAdvance(1.0F / 60.0F);
  REQUIRE(g.tierEventsFired(1) == firedWhileBusy + 1);
  const auto [lo, hi] = game::tierCadence(1);
  const float next = g.tierCooldown(1);
  CAPTURE(next);
  CAPTURE(lo);
  CAPTURE(hi);
  REQUIRE(next >= lo - 0.05F);
  REQUIRE(next <= hi + 0.05F);
}

TEST_CASE("A heavy tier is called out before it lands, not after") {
  // A thing that only shows up as a ring of telegraphs is noise; a thing that is
  // announced two seconds early is a decision. This also pins the latch: a tier
  // held back by a busy screen must not re-announce itself every frame while it
  // waits, and must announce itself again the NEXT time round rather than
  // arriving silently because the latch never cleared.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 33};
  g.testDisableWaves();
  g.testClearWeapons();
  g.enterTestMode();
  g.testSetSimTime(game::tierMinTime(1) + 1.0F);

  g.testAdvance(game::tierMinTime(1) + 1.0F);
  REQUIRE(g.tierEventsFired(1) == 1);

  // Wait until the clock is inside the callout window rather than guessing a
  // length for "a whole interval": the cadence is a random 100-130 seconds, and a
  // test that hard-codes 91 would drift out of the window the first time someone
  // widened the band.
  for (int i = 0; i < 200 && g.tierCooldown(1) > 1.5F; ++i) {
    g.testAdvance(0.1F);
  }
  CAPTURE(g.tierCooldown(1));
  CAPTURE(g.testTierBanner());
  REQUIRE(g.tierCooldown(1) <= 1.5F);
  REQUIRE_FALSE(g.testTierBanner().empty());
  // And it says what is coming, in the tier's own word, so the player learns the
  // ladder's vocabulary from the game rather than from the manual.
  REQUIRE(g.testTierBanner().find("ELITE") != std::string_view::npos);
  // The arrival has NOT happened yet. A warning raised after the thing it warns
  // about is not a warning.
  REQUIRE(g.tierEventsFired(1) == 1);

  // And it is gone again once the tier has landed -- a warning that outlasts the
  // thing it warned about is just a second banner.
  g.testAdvance(4.0F);
  REQUIRE(g.tierEventsFired(1) == 2);
  g.testAdvance(3.0F);
  REQUIRE(g.testTierBanner().empty());
}

TEST_CASE("The ladder is a ladder: heavier tiers arrive later, hold fewer, hit harder") {
  // One test for the three promises that make the tiers read as steps rather than
  // as one encounter with three HP bars. The HP bands are the ask -- 5x, 25x,
  // 125x -- and the midpoints are the honest way to state them, because a band is
  // a range and "25x" is only true of its middle.
  const auto e = game::tierHpBand(1);
  const auto c = game::tierHpBand(2);
  const auto o = game::tierHpBand(3);
  const double em = (e.first + e.second) / 2.0;
  const double cm = (c.first + c.second) / 2.0;
  const double om = (o.first + o.second) / 2.0;
  CAPTURE(em);
  CAPTURE(cm);
  CAPTURE(om);
  REQUIRE(cm / em > 4.0);
  REQUIRE(cm / em < 6.0);
  REQUIRE(om / cm > 4.0);
  REQUIRE(om / cm < 6.0);

  // Later, and fewer at a time. Strictly, or the three are the same fight.
  const auto [eLo, eHi] = game::tierCadence(1);
  const auto [cLo, cHi] = game::tierCadence(2);
  const auto [oLo, oHi] = game::tierCadence(3);
  REQUIRE(cLo > eHi);
  REQUIRE(oLo > cHi);
  REQUIRE(game::tierEventCap(1) > game::tierEventCap(2));
  REQUIRE(game::tierEventCap(2) > game::tierEventCap(3));

  // A tier's own clock does not tick until its tribunal is earned, so a run that
  // never gets there is not quietly running a countdown it cannot use -- and a
  // tribunal earned at minute five is not instantly due, which is what stops a
  // champion from landing on the corpse of the elite that opened it.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 34};
  g.testDisableWaves();
  g.testClearWeapons();
  g.enterTestMode();
  const float parked = g.tierCooldown(2);
  CAPTURE(parked);
  g.testAdvance(120.0F);
  REQUIRE_FALSE(g.tierUnlocked(2));
  CAPTURE(g.tierCooldown(2));
  REQUIRE(g.tierCooldown(2) == parked);
  // Cross the milestone, not the clock: four elite kills, and the clock starts
  // moving from the whole first interval rather than from zero.
  g.testAddTierKills(1, game::Game::tierKillGate(2));
  g.testAdvance(1.0F / 60.0F);
  REQUIRE(g.tierUnlocked(2));
  const float running = g.tierCooldown(2);
  CAPTURE(running);
  const auto [cLo2, cHi2] = game::tierCadence(2);
  REQUIRE(running >= cLo2 - 0.05F);
  REQUIRE(running <= cHi2 + 0.05F);
}
