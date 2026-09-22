#include <catch2/catch_test_macros.hpp>

#include "game/game.hpp"

TEST_CASE("Game accumulates elapsed time") {
  game::Game g;
  REQUIRE(g.elapsed() == 0.0);

  g.update(0.016);
  g.update(0.016);
  REQUIRE(g.elapsed() > 0.031);
  REQUIRE(g.elapsed() < 0.033);
}
