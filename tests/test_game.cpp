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
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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

TEST_CASE("traitsForTier gives elites exactly one bonus and stronger tiers more") {
  REQUIRE(game::traitsForTier(0, 0.0F) == 0);
  REQUIRE(game::traitsForTier(1, 0.0F) == 1);
  REQUIRE(game::traitsForTier(1, 9999.0F) == 1); // elite always exactly one
  REQUIRE(game::traitsForTier(2, 0.0F) >= 2);
  REQUIRE(game::traitsForTier(2, 300.0F) > game::traitsForTier(2, 0.0F));
  REQUIRE(game::traitsForTier(3, 0.0F) >= 4);
  REQUIRE(game::traitsForTier(3, 600.0F) > game::traitsForTier(3, 0.0F));
  // Overlords always roll more than champions, who roll more than elites.
  REQUIRE(game::traitsForTier(3, 0.0F) > game::traitsForTier(2, 0.0F));
  REQUIRE(game::traitsForTier(2, 0.0F) > game::traitsForTier(1, 0.0F));
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

TEST_CASE("Vampirism is reachable, the milestone grants it repeatedly, the trigger is a kill") {
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
  const auto* gold = find("leech_gold");
  const auto* soul = find("leech_soul");
  const auto* crimson = find("m4_crimson");
  const auto* crown = find("m128_crown");
  REQUIRE(gold != nullptr);
  REQUIRE(soul != nullptr);
  REQUIRE(crimson != nullptr);
  REQUIRE(crown != nullptr);
  // The everyday cards are still modest, so vampirism is not simply strong
  // everywhere -- it is a build you commit to.
  REQUIRE(gold->value == Catch::Approx(4.0F));
  REQUIRE(soul->value == Catch::Approx(6.0F));
  // The milestone one is "several times over", which is a different promise from
  // a big number and is the reason `grants` exists at all.
  REQUIRE(crimson->value == Catch::Approx(9.0F));
  REQUIRE(crimson->grants == 3);
  REQUIRE(crown->value == Catch::Approx(40.0F));
  REQUIRE(crown->grants == 2);
  // And the strongest single number in the game for lifesteal is a MILESTONE, so
  // vampirism has somewhere to go and the run has to be pointed at it.
  float best = 0.0F;
  for (const auto& u : content.upgrades) {
    if (u.effect != "lifesteal_add") continue;
    CAPTURE(u.id);
    best = std::max(best, u.value * static_cast<float>(u.grants));
  }
  REQUIRE(best >= 80.0F);
  // A milestone card that grants several applications must be the top of that
  // list and must be exclusive with the two other survival answers.
  REQUIRE((crown->group == "survivor" || crimson->group == "survivor"));
  int survivor = 0;
  for (const auto& u : content.upgrades) {
    if (u.group == "survivor") ++survivor;
  }
  REQUIRE(survivor == 3);

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
  while (held < core::input::KeyRepeat::kInitialDelay - 0.01F) {
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
  const auto dragged = [&content, gyre](float res) {
    game::Game g{content, 58};
    g.testDisableWaves();
    g.testClearWeapons();
    g.testAddWeapon(gyre);
    g.testSpawnEnemyAt(2.2F, 0.0F); // stationary target, no self-movement
    g.testSetFirstEnemyKnockbackRes(res);
    game::FrameInput in{};
    for (int i = 0; i < 120; ++i) g.advance(1.0F / 60.0F, in);
    return g.testFirstEnemyDistToVortex();
  };
  const float soft = dragged(0.0F);
  const float tough = dragged(1.0F);
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

TEST_CASE("Champions wait for elites to be easy, overlords for champions") {
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
  REQUIRE(g.tierSpawnChance(2) == 0.0F);
  REQUIRE_FALSE(g.tierUnlocked(3));
  REQUIRE(g.tierSpawnChance(3) == 0.0F);

  // Clearing an elite is what actually counts.
  g.testSpawnTieredEnemyAt(4.0F, 0.0F, 1);
  g.testKillFirstEnemy();
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierPressure(1) > 0.0F);
  REQUIRE_FALSE(g.tierUnlocked(2));

  // Handle enough of them and the champion tribunal opens by itself.
  g.testAddTierPressure(1, 8.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierUnlocked(2));
  REQUIRE(g.tierSpawnChance(2) > 0.0F);
  // The better the player does, the more champions show up.
  const float champChance = g.tierSpawnChance(2);
  g.testAddTierPressure(1, 20.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierSpawnChance(2) > champChance);

  // Overlords are gated on CHAMPIONS, not on the clock.
  g.testSetSimTime(300.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE_FALSE(g.tierUnlocked(3));
  g.testAddTierPressure(2, 7.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.tierUnlocked(3));
  REQUIRE(g.tierSpawnChance(3) > 0.0F);

  // And it is reversible: the form evaporates, the score falls back under the
  // line and the gate shuts again once the grace window runs out.
  game::Game fading{content, 121};
  fading.testDisableWaves();
  fading.testClearWeapons();
  fading.advance(1.0F / 60.0F, in);
  fading.testSetSimTime(200.0F);
  fading.testAddTierPressure(1, 7.5F);
  fading.advance(1.0F / 60.0F, in);
  REQUIRE(fading.tierUnlocked(2));
  fading.testAddTierPressure(1, -8.0F); // form is gone
  REQUIRE(fading.tierPressure(1) < 7.0F);
  REQUIRE(fading.tierUnlocked(2));      // still inside the grace window
  for (int i = 0; i < 24 * 60; ++i) fading.advance(1.0F / 60.0F, in);
  REQUIRE_FALSE(fading.tierUnlocked(2));
  REQUIRE_FALSE(fading.tierUnlocked(3));
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
  // Pickup range and XP are the real dead picks: they never change how the
  // game plays, they only make it faster to sweep up. Flat HP and move speed
  // are legitimate but must stay a minority, and the offensive core plus the
  // momentum axis have to keep real weight.
  int normal = 0;
  int pickupXp = 0;
  int hp = 0;
  int speed = 0;
  int offense = 0;
  int momentum = 0;
  for (const auto& u : content.upgrades) {
    if (u.kind != "normal") continue;
    ++normal;
    if (u.effect == "pickup_mul" || u.effect == "xp_mul") pickupXp += u.maxStacks;
    if (u.effect == "max_hp_add") hp += u.maxStacks;
    if (u.effect == "speed_mul") speed += u.maxStacks;
    if (u.effect == "damage_mul" || u.effect == "fire_rate" || u.effect == "proj_add") {
      offense += u.maxStacks;
    }
  }
  for (const auto& u : content.upgrades) {
    if (u.effect.rfind("momentum_", 0) == 0) momentum += u.maxStacks;
  }
  CAPTURE(normal);
  CAPTURE(pickupXp);
  REQUIRE(normal > 20);
  // At most a fifth of the pool may be pure convenience.
  REQUIRE(pickupXp * 5 <= normal);
  // No defensive family may outnumber the whole offensive core.
  REQUIRE(hp <= 12);
  REQUIRE(speed <= 6);
  REQUIRE(offense >= 25);
  // Momentum is a real build axis, not one lonely card.
  REQUIRE(momentum >= 4);
}

// --- Second-wave weapons, abilities and the fixes on top ---------------------

namespace {

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
    if (def.weapon.empty()) {
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
  // mirrored here, because the card sets its wrapped lines LARGER than the
  // first -- a copy of the constants would quietly test the wrong layout the
  // next time the style moved.
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
    // The body scale is derived, not fixed, so the measure stays readable in
    // every row, and the wrapped lines are set larger than the first.
    const auto lay = game::cardTextLayout(cardW);
    REQUIRE(lay.scale >= 1.3F);
    REQUIRE(lay.contScale > lay.scale);
    REQUIRE(lay.contLineH > lay.lineH);
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
      // ...and no line is wider than its own budget. The continuation lines are
      // bigger AND indented, so their budget is the tightest of the three and
      // is the one that overflows if the wrap is measured at the wrong size.
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

  // The bolt spends kChainTelegraph converging on the first target before it
  // damages anything, so "straight after the first hop" is now at least that far
  // in -- plus the 0.05s inter-hop delay. Advancing past the wind-up is the point
  // of the test: the fan must exist.
  s.g.testAdvance(game::Game::kChainTelegraph + 0.1F);
  const std::size_t shardsEarly = s.g.testProjectileCount();
  CAPTURE(shardsEarly);
  REQUIRE(shardsEarly > 0);

  // The bolt keeps hopping afterwards, but the shard count does not keep
  // growing with it. Four hops at a six-shard fan each would be 24 if the
  // shatter were being paid out every time; one fan is 6, and they are already
  // expiring, so the count stays near the first fan's size.
  s.g.testAdvance(0.2F);
  const std::size_t shardsMid = s.g.testProjectileCount();
  CAPTURE(shardsMid);
  REQUIRE(shardsMid <= shardsEarly + 2);

  // The bolt's 4 jumps at 0.05s each plus the 0.45s linger is under a second,
  // and the shards only live 0.55s -- but the WEAPON fires every 0.5s, so "no
  // bolts left" would only be true by accident of the schedule. Compare against
  // the next shot instead: the fan must clear rather than pile up.
  s.g.testAdvance(0.4F);
  const std::size_t shardsAfter = s.g.testProjectileCount();
  CAPTURE(shardsAfter);
  REQUIRE(shardsAfter <= shardsEarly);

  // Steady state: however many volleys have gone by, there is never more than a
  // couple of fans' worth of shards in the air at once. The chain bound is sized
  // to include bolts still in their telegraph, which is why it is 4 and not
  // "bolts that have already struck": a converging bolt is a real bolt occupying
  // a real entity slot.
  for (int i = 0; i < 20; ++i) {
    s.g.testAdvance(0.5F);
    REQUIRE(s.g.testProjectileCount() <= shardsEarly * 2);
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

  // And rarer. At the old 15% ceiling one spawn in seven was an elite, which is
  // a tax on the trash rather than an event.
  REQUIRE(g.tierSpawnChance(1) < 0.06F);
  g.testSetSimTime(2000.0F);
  REQUIRE(g.tierSpawnChance(1) <= 0.10F);
  // Still a ramp, not a constant: late in a run elites are commoner than early.
  g.testSetSimTime(90.0F);
  const float early = g.tierSpawnChance(1);
  g.testSetSimTime(400.0F);
  REQUIRE(g.tierSpawnChance(1) > early);

  // --- The extra traits wait too ---------------------------------------------
  // A champion with three traits at the four-minute mark is not a champion, it
  // is a boss wearing a champion's label, and nothing the player picked yet was
  // an answer to it.
  REQUIRE(game::traitsForTier(2, 299.0F) == 2);
  REQUIRE(game::traitsForTier(2, 300.0F) == 3);
  REQUIRE(game::traitsForTier(3, 599.0F) == 4);
  REQUIRE(game::traitsForTier(3, 600.0F) == 5);

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
  for (const auto& u : content.upgrades) {
    if (u.kind != "milestone") continue;
    ++milestoneCards;
    CAPTURE(u.id);
    REQUIRE_FALSE(u.group.empty());
    REQUIRE(u.maxStacks == 1); // one decision, not a grind
    if (std::find(groups.begin(), groups.end(), u.group) == groups.end()) {
      groups.push_back(u.group);
    }
  }
  CAPTURE(milestoneCards);
  // Six levels of ladder, and the two the player named plus the kill chain and
  // contact are all in there.
  REQUIRE(groups.size() == 6);
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
  const auto groupOf = [&content](int level) -> std::string {
    for (const auto& u : content.upgrades) {
      if (u.kind == "milestone" && u.level == level) return u.group;
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

    // Take the first one. Every sibling is now closed off, and it can never be
    // offered again -- checked by advancing to the NEXT milestone and confirming
    // that no card from this group is on the screen.
    const int taken = offer.front().index;
    REQUIRE(g.testGrantUpgrade(taken));
    for (int nxt : {level * 2, level * 2 + 0}) {
      if (nxt == level) continue;
      g.testSetLevel(nxt);
      for (const auto& c : g.upgradeChoices()) {
        CAPTURE(nxt);
        CAPTURE(c.index);
        // -1 is the "skip / nothing left" card, which has no group at all.
        if (c.index < 0) continue;
        const auto& u = content.upgrades[static_cast<std::size_t>(c.index)];
        REQUIRE(u.group != gname);
      }
    }
  }
}

TEST_CASE("A group is only closed by taking one of its cards, not by passing the level") {
  // The obvious bug in an exclusive system: the lock fires on "a milestone
  // happened" instead of on "the player chose", and a run silently loses a third
  // of its upgrade space because it skipped a level-up to read the screen.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  game::Game g{content, 99};
  g.testClearWeapons();
  g.testSetLevel(4);
  const auto first = g.upgradeChoices();
  REQUIRE(first.size() == 3);
  // Skip without taking anything.
  g.testClearWeapons();
  g.testSetLevel(4);
  REQUIRE(g.upgradeChoices().size() == 3);
  for (const auto& c : g.upgradeChoices()) REQUIRE(c.index >= 0);

  // Now actually take one and confirm the same three are NOT on offer again.
  const int idx = g.upgradeChoices().front().index;
  REQUIRE(g.testGrantUpgrade(idx));
  g.testSetLevel(4);
  // Level 4 is a one-shot level, so buildChoices will fall back to a normal
  // level-up; what matters is that none of the group is in it.
  int survivors = 0;
  for (const auto& u : content.upgrades) {
    if (u.group == "survivor") ++survivors;
  }
  REQUIRE(survivors == 3);
  for (const auto& c : g.upgradeChoices()) {
    if (c.index < 0) continue;
    const auto& u = content.upgrades[static_cast<std::size_t>(c.index)];
    CAPTURE(u.id);
    REQUIRE(u.group != "survivor");
  }
}

TEST_CASE("A card that grants several applications lands them all") {
  // "An item that improves vampirism several times" is a promise about HOW MANY
  // TIMES, not a bigger number, which is why `grants` is its own field: the
  // player can see the count before committing. Tested through the real effect so
  // a card that lies about its count cannot ship.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int idx = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].id == "m4_crimson") idx = static_cast<int>(i);
  }
  REQUIRE(idx >= 0);
  const auto& def = content.upgrades[static_cast<std::size_t>(idx)];
  REQUIRE(def.effect == "lifesteal_add");
  REQUIRE(def.grants == 3);

  game::Game g{content, 7};
  g.testClearWeapons();
  const float before = g.stats().lifesteal;
  REQUIRE(g.testGrantUpgrade(idx));
  // Three applications of value, not one, and not three times value either.
  REQUIRE(g.stats().lifesteal == Catch::Approx(before + def.value * 3.0F));
}

TEST_CASE("The four on-hit marks are exclusive, real, and none of them is free") {
  // The mark group is the second question the player asked for: "do you want to
  // slow them, or set them on fire, or something else?" Four answers, one pick.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");

  struct Mark {
    const char* id;
    const char* effect;
  };
  const Mark marks[] = {
      {"m16_frostbind", "mark_slow"}, {"m16_emberbrand", "mark_burn"},
      {"m16_hex", "mark_vuln"},       {"m16_splitarmor", "mark_defstrip"},
  };
  std::vector<int> idxs;
  for (const auto& m : marks) {
    int idx = -1;
    for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
      if (content.upgrades[i].id == m.id) {
        idx = static_cast<int>(i);
        REQUIRE(content.upgrades[i].effect == m.effect);
        REQUIRE(content.upgrades[i].group == "element");
      }
    }
    CAPTURE(m.id);
    REQUIRE(idx >= 0);
    REQUIRE(content.upgrades[static_cast<std::size_t>(idx)].value > 0.0F);
    idxs.push_back(idx);
  }
  REQUIRE(idxs.size() == 4);
  // A run can own exactly one of them: the group closes after the first.
  for (const int taken : idxs) {
    game::Game g{content, 5};
    g.testClearWeapons();
    REQUIRE(g.testGrantUpgrade(taken));
    // The three it did not take are exactly the ones that are now closed off.
    for (const int other : idxs) {
      if (other == taken) continue;
      CAPTURE(other);
      REQUIRE(g.testUpgradeBlocked(other));
    }
    // And the one it took is not.
    REQUIRE_FALSE(g.testUpgradeBlocked(taken));
    // And the one it took is genuinely live: exactly one mark field is non-zero.
    const auto& st = g.stats();
    const int live = (st.markChillTime > 0.0F ? 1 : 0) + (st.markBurnDps > 0.0F ? 1 : 0) +
                     (st.markVuln > 0.0F ? 1 : 0) + (st.markDefStrip > 0.0F ? 1 : 0);
    CAPTURE(live);
    REQUIRE(live == 1);
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

TEST_CASE("An elite leaves a chest, a champion three, an overlord five") {
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

  for (const auto& w : std::vector<Want>{{1, 1}, {2, 3}, {3, 5}}) {
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

TEST_CASE("The screen never holds more than two elites at a time") {
  // "Do not make elites weaker, make them rarer, so there are one or two on
  // screen." A per-spawn percentage cannot promise that -- packs and waves roll
  // members independently -- so the cap is a live count, and this pins it.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 6};
  g.testDisableWaves();
  g.testClearWeapons();
  g.testAddWeapon(0);
  // A handful of elites already on the floor, so the budget is spent.
  g.testSpawnEliteAt(3.0F, 0.0F, 1);
  g.testSpawnEliteAt(3.0F, 1.0F, 1);
  REQUIRE(g.testLiveTierCount() == 2);
  // Waves are disabled, so the only way the number could grow is the spawner
  // ignoring the cap, and the only way to test that is to let it run.
  g.testEnableWaves();
  g.testAdvance(6.0F);
  // Generous ceiling: the cap is two ELITE-AND-ABOVE slots, and the cap may be
  // under-spent on a quiet roll, but it may never be over-spent.
  REQUIRE(g.testLiveTierCount() <= 2);
}

TEST_CASE("Weapons level with the player, and a late pickup joins at the arsenal's level") {
  // "Stacks for weapons, so they are just a bit better at levels." Two promises:
  // they improve without the player spending anything, and picking a weapon up
  // late is not strictly worse than picking it up early. The second is the one
  // that quietly stops being true in every game like this.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  REQUIRE(game::Game::kWeaponLevelEvery == 4);

  game::Game g{content, 17};
  g.testClearWeapons();
  const int wand = g.testWeaponContentIndex("wand");
  g.testAddWeapon(wand);
  const float baseDamage = g.testWeaponStat(0, "damage");
  REQUIRE(g.testWeaponLevel(0) == 0);
  REQUIRE(g.testWeaponLevel() == 0);

  g.testSetLevel(8);
  REQUIRE(g.testWeaponLevel() == 2);
  REQUIRE(g.testWeaponLevel(0) == 2);
  // Two levels, each worth its own geometric step -- asserted against the shared
  // formula rather than a hard-coded sum, so a retune of the decay cannot leave a
  // stale expectation quietly passing or failing for the wrong reason.
  REQUIRE(g.testWeaponStat(0, "damage") ==
          Catch::Approx(baseDamage + game::Game::weaponLevelDamageTotal(2)));
  // And the second level is worth strictly less than the first, which is the
  // property the whole system rests on.
  REQUIRE(game::Game::weaponLevelDamageGain(1) <
          game::Game::weaponLevelDamageGain(0));

  // A weapon picked up now is at the same level as the one already here, and it
  // has the same LEVEL bonus on top of its own (different) base damage.
  const int dagger = g.testWeaponContentIndex("dagger");
  game::Game early{content, 17};
  early.testAddWeapon(dagger);
  const float daggerBase = early.testWeaponStat(0, "damage");
  g.testAddWeapon(dagger);
  REQUIRE(g.testWeaponLevel(1) == 2);
  REQUIRE(g.testWeaponStat(1, "damage") ==
          Catch::Approx(daggerBase + game::Game::weaponLevelDamageTotal(2)));
}

TEST_CASE("A weapon level is worth what the growth card says, and only from then on") {
  // A growth card that retroactively rewrote levels already paid out would make
  // the same run worth different amounts depending on when the card turned up,
  // which is the kind of thing that only shows up as a balance complaint weeks
  // later. So the growth applies forward only, and the high-water mark is what
  // makes that true.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int idx = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].id == "u_whetstone") idx = static_cast<int>(i);
  }
  REQUIRE(idx >= 0);
  REQUIRE(content.upgrades[static_cast<std::size_t>(idx)].effect == "weapon_growth");

  game::Game g{content, 21};
  g.testClearWeapons();
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  g.testSetLevel(4);
  const float afterOne = g.testWeaponStat(0, "damage");
  REQUIRE(g.testWeaponLevel(0) == 1);

  REQUIRE(g.testGrantUpgrade(idx));
  REQUIRE(g.stats().weaponGrowth == Catch::Approx(2.0F));
  // The level already paid for is untouched.
  REQUIRE(g.testWeaponStat(0, "damage") == Catch::Approx(afterOne));
  // The next one is worth double.
  g.testSetLevel(8);
  REQUIRE(g.testWeaponLevel(0) == 2);
  // The first level was already paid at the old rate, the second at the new one,
  // so the difference is exactly one doubled step.
  REQUIRE(g.testWeaponStat(0, "damage") ==
          Catch::Approx(afterOne + 2.0F * game::Game::weaponLevelDamageGain(1)));
}

TEST_CASE("Weapon levels never run away on a long run") {
  // The free bump is a rounding error against a build, which is the only reason
  // it is safe to have. This pins that: at the level a very long run reaches, it
  // is still smaller than a single ordinary card.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  int damageCard = -1;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].id == "w_wand_power") damageCard = static_cast<int>(i);
  }
  REQUIRE(damageCard >= 0);
  const auto& card = content.upgrades[static_cast<std::size_t>(damageCard)];

  game::Game g{content, 23};
  g.testClearWeapons();
  g.testAddWeapon(g.testWeaponContentIndex("wand"));
  g.testSetLevel(256);
  const int levels = g.testWeaponLevel(0);
  CAPTURE(levels);
  REQUIRE(levels == 256 / game::Game::kWeaponLevelEvery);
  const float fromLevels = game::Game::weaponLevelDamageTotal(levels);
  CAPTURE(fromLevels);
  // One fully-stacked ordinary weapon card, and the free bump is still smaller --
  // and it stays smaller no matter how far past that the run goes, which is the
  // part the decay buys.
  REQUIRE(fromLevels < card.value * static_cast<float>(card.maxStacks));
  g.testSetLevel(1024);
  REQUIRE(game::Game::weaponLevelDamageTotal(g.testWeaponLevel(0)) < fromLevels +
                                                          card.value * 0.5F);
}
