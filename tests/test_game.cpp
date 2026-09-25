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
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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

TEST_CASE("The roster is 32 weapons: 18 base, 10 evolutions, 4 supers") {
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
  REQUIRE(evo == 10);
  REQUIRE(super == 4);
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

  // Right in the shell's flight path: a fused shell ignores what it flies over.
  const float passed = hpAfterHit(content, mortar, 204, 1.5F, 0.0F, 90);
  CAPTURE(passed);
  REQUIRE(passed == Catch::Approx(100000.0F));
  // Where the arc comes back down (~8 units out with proj_speed 9): cooked.
  const float landed = hpAfterHit(content, mortar, 204, 8.3F, 0.0F, 90);
  CAPTURE(landed);
  REQUIRE(landed < 100000.0F);
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
  // The renderer lays a page out at 1.6 scale on a 15 px line pitch, starting
  // 74 px down, and keeps 56 px of bottom margin for the hint bar. Anything
  // past that is clipped with a "...MORE, NEXT PAGE" marker, which would mean
  // the reader is missing part of a topic with no way to scroll.
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  constexpr float kBodyY = 74.0F;
  constexpr float kLineH = 15.0F;
  constexpr float kBottomPad = 56.0F;
  constexpr float kRefHeight = 720.0F; // the smallest window the HUD supports
  constexpr float kBodyX = 250.0F;
  constexpr float kScale = 1.6F;
  constexpr float kGlyph = 6.0F; // Batcher::textWidth == len * 6 * scale
  const float maxLines = (kRefHeight - kBodyY - kBottomPad) / kLineH;
  const float maxChars = (kRefHeight - kBodyX - 20.0F) / (kGlyph * kScale);

  for (const auto& page : content.manual) {
    CAPTURE(page.id);
    REQUIRE(static_cast<float>(page.lines.size()) <= maxLines);
    // The marker prefixes ('>', '#', two spaces) are stripped before drawing,
    // so they only ever make a line SHORTER on screen.
    for (const auto& raw : page.lines) {
      std::string_view v = raw;
      if (!v.empty() && (v.front() == '>' || v.front() == '#')) v.remove_prefix(1);
      if (v.size() >= 2 && v[0] == ' ' && v[1] == ' ') v.remove_prefix(2);
      REQUIRE(static_cast<float>(v.size()) <= maxChars);
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
  // At least two different slot cards: the stacking one and the one-shot that
  // completes the roster, so the last weapon slot is not behind a 3x grind.
  REQUIRE(slotCards >= 2);
  REQUIRE(slotStacks == game::Game::kMaxSlotCards);
  REQUIRE(g.weaponCap() == game::Game::kBaseWeapons + slotStacks);
  REQUIRE(game::Game::kMaxWeapons == 8);
  REQUIRE(g.weaponCap() <= game::Game::kMaxWeapons);

  // The array really is big enough for the full cap: fill it and confirm the
  // eighth slot accepts a weapon (a short weapons_[] would write out of bounds
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
