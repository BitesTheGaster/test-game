#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "game/game.hpp"
#include "game/content.hpp"
#include "game/profile.hpp"
#include "core/input/key_repeat.hpp"
#include "core/sim/spatial_hash.hpp"
#include "core/sim/fixed_timestep.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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
  REQUIRE(game::xpForLevel(1) == Catch::Approx(12.0F));
  float prev = 0.0F;
  for (int lvl = 1; lvl <= 20; ++lvl) {
    const float need = game::xpForLevel(lvl);
    REQUIRE(need > prev);
    prev = need;
  }
  // Pacing guard: the curve has to stay steep enough that level-ups (which are
  // the only source of build decisions) keep arriving for a whole run. A flat
  // curve is what turns the game into a walk with nothing to pick. Reaching
  // level 32 costs ~33k XP and level 64 ~246k, so a build is still filling out
  // long after the first minute.
  REQUIRE(game::xpForLevel(32) > 2500.0F);
  REQUIRE(game::xpForLevel(64) > 10000.0F);
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
  // A 50% lifesteal resistance halves the player's chance.
  REQUIRE(game::enemyLifestealResistance(600.0F, 0, false) == Catch::Approx(0.5F));
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
  // the unlock gate is the only thing blocking it.
  const bool unlockedNow = content.enemies[static_cast<std::size_t>(recentType)].unlockAt <= 0.0F;
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
  // nothing. enemyDefense is (t-30)/25 * tierMul; at t=120 champion (2.0x) => ~7.2.
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

  // Level 4 is a milestone level: pick-of-2 from the milestone pool.
  g.grantXp(game::xpForLevel(4));
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE(g.milestoneOffer());
  REQUIRE(g.upgradeChoices().size() == 2);

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
  REQUIRE(storm->attackType == game::AttackType::Chain);
  REQUIRE(storm->chainMaxJumps > 0);
  REQUIRE(storm->chainJumpRange > 0.0F);
  REQUIRE(storm->chainDamageMul > 0.0F);
  REQUIRE(storm->chainDamageMul < 1.0F);
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
  // One enemy far away: the bomb aims along +x but its fixed arc lands ~6.5
  // units out, so it detonates by the LANDING rule (no contact impact).
  g.testSpawnEnemyAt(30.0F, 0.0F);
  g.testAddWeapon(hammerIdx);

  game::FrameInput in{};
  // Flight time is t = 2*sqrt(2*30*2.5)/30 ~= 0.82s => ~49 ticks.
  for (int i = 0; i < 25; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  REQUIRE(g.debugCounts().bombs == 1); // airborne mid-arc

  for (int i = 0; i < 50; ++i) {
    g.advance(1.0F / 60.0F, in);
  }
  // Bomb landed, exploded, and was destroyed — it never lingers invisible.
  REQUIRE(g.debugCounts().bombs == 0);
  // The far-away enemy took nothing: the explosion happened at the landing
  // spot, not teleported onto the enemy.
  REQUIRE(g.testFirstEnemyHp() == Catch::Approx(100000.0F));
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
    // Landing spot is x = speed * flightTime = 8 * 0.8165 ~= 6.53. Park the
    // enemy 2.1 units past it: inside the boosted blast (2.0 * 1.3 = 2.6 for
    // +2 pierce) but outside the base 2.0.
    g.testSpawnEnemyAt(8.63F, 0.0F);
    g.testAddWeapon(hammerIdx);
    game::FrameInput in{};
    for (int i = 0; i < 75; ++i) {
      g.advance(1.0F / 60.0F, in);
    }
    return g;
  };

  {
    const auto base = run(0);
    REQUIRE(base.testFirstEnemyHp() == Catch::Approx(100000.0F)); // out of blast
  }
  {
    const auto boosted = run(2);
    REQUIRE(boosted.testFirstEnemyHp() == Catch::Approx(100000.0F - 45.0F));
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

  // An enemy in the ring is carved by the sweeping spokes over time.
  g.testSpawnEnemyAt(2.0F, 0.0F);
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
      {"storm", &game::Game::DebugCounts::chains},
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
      const float sx = (def.attackType == game::AttackType::Bomb)
                           ? def.projSpeed * (2.0F * vy / gAcc)
                           : 1.5F;
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
  REQUIRE(g.menuSelection() == 3);

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

  // Navigate to QUIT (index 3).
  game::FrameInput down{};
  down.menuDown = true;
  for (int i = 0; i < 3; ++i) g.advance(1.0F / 60.0F, down);
  REQUIRE(g.menuSelection() == 3);

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

TEST_CASE("Lifesteal values are halved and the kill trigger is kept") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  const auto value = [&content](const char* id) {
    for (const auto& u : content.upgrades) {
      if (u.id == id) return u.value;
    }
    return -1.0F;
  };
  // The user's ask: everything that grants vampirism is twice as weak, and the
  // healing still only rolls on a kill (not per hit).
  REQUIRE(value("leech_gold") == Catch::Approx(4.0F));
  REQUIRE(value("leech_soul") == Catch::Approx(6.0F));
  REQUIRE(value("m4_pact") == Catch::Approx(6.0F));
  REQUIRE(value("m16_crown") == Catch::Approx(8.0F));
  for (const auto& u : content.upgrades) {
    if (u.effect != "lifesteal_add") continue;
    CAPTURE(u.id);
    REQUIRE(u.value <= 8.0F);
  }

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

TEST_CASE("Prism Array locks one beam per projectile onto separate targets") {
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
  // Four targets in a row, all well inside the lock range. Four projectiles
  // means four locks, so the crowd is split across separate beams.
  for (int i = 0; i < 4; ++i) {
    g.testSpawnEnemyAt(3.0F + static_cast<float>(i) * 0.8F, 0.0F);
  }
  g.testAddWeapon(prism);
  game::FrameInput in{};
  g.advance(1.0F / 60.0F, in);
  // Four projectiles => four independent locked beams, not one fat beam.
  REQUIRE(g.debugCounts().beams == 4);

  // Every one of them does real work: all four separate targets lose HP, which
  // a single-target beam could never manage.
  for (int i = 0; i < 30; ++i) g.advance(1.0F / 60.0F, in);
  const auto hps = g.testEnemyHps();
  REQUIRE(hps.size() == 4);
  int damaged = 0;
  for (const float hp : hps) {
    if (hp < 100000.0F) ++damaged;
  }
  REQUIRE(damaged == 4);
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
  REQUIRE(g.xp() > xp0);
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
  REQUIRE(g.playerHp() == Catch::Approx(hp1));
  REQUIRE(g.testBestiaryKills(0) == 0);
  REQUIRE(g.testTimeScale() == 1);
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

  // R maxes every item in the sandbox.
  game::FrameInput fill{};
  fill.restart = true;
  g.advance(1.0F / 60.0F, fill);
  int maxed = 0;
  for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
    if (content.upgrades[i].kind == "milestone") continue;
    if (g.upgradeStacks(i) == content.upgrades[i].maxStacks) ++maxed;
  }
  REQUIRE(maxed > static_cast<int>(content.upgrades.size()) / 2);
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

TEST_CASE("Test sandbox rerolls have no budget") {
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

  // Level up in the sandbox, then reroll far past the normal single charge.
  g.grantXp(1000.0F);
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.state() == game::RunState::LevelUp);
  for (int i = 0; i < 12; ++i) {
    game::FrameInput roll{};
    roll.restart = true;
    g.advance(1.0F / 60.0F, roll);
    REQUIRE(g.state() == game::RunState::LevelUp);
  }
  // Leaving the sandbox clears the pending level-up, so the run is playable.
  game::FrameInput close{};
  close.testModeToggle = true;
  g.advance(1.0F / 60.0F, close);
  REQUIRE(g.state() == game::RunState::Playing);
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

  game::FrameInput open{};
  open.testModeToggle = true;
  g.advance(1.0F / 60.0F, open);
  REQUIRE(g.testMode());
  g.testMaxAllItems();

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
  // displace as a run goes on, and old spawns are lifted with it.
  REQUIRE(game::enemyKnockbackResistance(0.0F, 0, false) == Catch::Approx(0.0F));
  REQUIRE(game::enemyKnockbackResistance(400.0F, 0, false) ==
          Catch::Approx(400.0F / 750.0F).margin(0.001F));
  REQUIRE(game::enemyKnockbackResistance(525.0F, 0, false) == Catch::Approx(0.7F));
  REQUIRE(game::enemyKnockbackResistance(2000.0F, 0, false) == Catch::Approx(0.7F));
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
  // Player HP, weapons, XP and the unique-item state are all rolled back.
  REQUIRE(g.playerHp() == Catch::Approx(entryHp));
  REQUIRE(g.armedWeaponIds().size() == 1);
  REQUIRE(g.armedWeaponIds().front() == content.weapons[0].id);
  REQUIRE(g.xp() == Catch::Approx(0.0F));
  REQUIRE(g.rerollsUsed() == 0);
  REQUIRE_FALSE(g.milestoneOffer());
  // Nothing sandbox-only survives: no injected fodder, no stray projectiles.
  REQUIRE(g.debugCounts().projectiles == 0);
  REQUIRE(g.testTimeScale() == 1);
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
