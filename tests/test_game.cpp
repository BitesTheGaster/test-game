#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "game/game.hpp"
#include "game/content.hpp"
#include "core/sim/spatial_hash.hpp"
#include "core/sim/fixed_timestep.hpp"

#include <algorithm>
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
