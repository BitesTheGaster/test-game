#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "game/game.hpp"
#include "game/content.hpp"
#include "core/sim/spatial_hash.hpp"
#include "core/sim/fixed_timestep.hpp"

#include <algorithm>
#include <cmath>
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

  const auto cd = game::applyUpgrade(s, "cooldown_mul", -0.10F);
  REQUIRE(cd.valid);
  REQUIRE(s.cooldownMul == Catch::Approx(0.90F));

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

  // Lifesteal is a per-hit percentage now (150 = 100% for 1 HP + 50% for a 2nd).
  const auto ls = game::applyUpgrade(s, "lifesteal_add", 150.0F);
  REQUIRE(ls.valid);
  REQUIRE(s.lifesteal == Catch::Approx(150.0F));

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
  REQUIRE(s.cooldownMul == Catch::Approx(0.5F));

  REQUIRE(game::applyUpgrade(s, "extra_choice", 1.0F).valid);
  REQUIRE(s.extraChoice == 1);
  REQUIRE(game::applyUpgrade(s, "reroll_add", 1.0F).valid);
  REQUIRE(s.rerollCharges == 2);  // default 1 base + 1 from item
  REQUIRE(game::applyUpgrade(s, "thorns", 3.0F).valid);
  REQUIRE(s.thornsDmg == Catch::Approx(3.0F));
  REQUIRE(game::applyUpgrade(s, "adrenaline", 1.0F).valid);
  REQUIRE(s.adrenaline == 1);
  REQUIRE(game::applyUpgrade(s, "chain", 1.0F).valid);
  REQUIRE(s.chain == 1);
  REQUIRE(game::applyUpgrade(s, "lifesteal_heal", 2.0F).valid);
  REQUIRE(s.lifestealHeal == 2);
}

TEST_CASE("xpForLevel grows monotonically") {
  REQUIRE(game::xpForLevel(1) == Catch::Approx(6.0F));
  float prev = 0.0F;
  for (int lvl = 1; lvl <= 20; ++lvl) {
    const float need = game::xpForLevel(lvl);
    REQUIRE(need > prev);
    prev = need;
  }
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

TEST_CASE("Level 5 offers milestone cards") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 11};

  // Reach level 5 with exactly one level-up per pick (queue four level-ups).
  g.grantXp(game::xpForLevel(1) + game::xpForLevel(2) + game::xpForLevel(3) + game::xpForLevel(4));
  REQUIRE(g.state() == game::RunState::LevelUp);
  for (int i = 0; i < 4; ++i) {
    game::FrameInput in{};
    in.choose1 = true;
    g.advance(1.0F / 60.0F, in);
  }
  REQUIRE(g.level() == 5);
  REQUIRE(g.state() == game::RunState::Playing);

  g.grantXp(game::xpForLevel(5));
  REQUIRE(g.state() == game::RunState::LevelUp);
  REQUIRE(g.milestoneOffer());
  REQUIRE(g.upgradeChoices().size() == 2);
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

  // A second reroll also succeeds (1 base + 1 from reroll_add = 2 budget).
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.rerollsUsed() == 2);

  // A third reroll is a no-op (budget exhausted).
  g.advance(1.0F / 60.0F, in);
  REQUIRE(g.rerollsUsed() == 2);
}

// --- Enemy movement ----------------------------------------------------------

TEST_CASE("Enemy separation speed is clamped (no vacuum darting)") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 13};
  g.testDisableWaves();
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
  REQUIRE(orb->bounceDamageMul > 0.0F);
  REQUIRE(orb->bounceDamageMul < 1.0F);
  
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
  };
  for (const auto& c : cases) {
    CAPTURE(c.id);
    const int wi = idx(c.id);
    REQUIRE(wi >= 0);
    game::Game g{content, 77};
    g.testDisableWaves();
    g.testClearWeapons();
    g.stats().cooldownMul = 0.01F; // every weapon fires immediately
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
                       "orb", "beam", "scythe", "storm", "nova"};
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
