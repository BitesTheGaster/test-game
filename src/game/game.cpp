#include "game/game.hpp"

namespace game {

void Game::update(double dt) {
  elapsed_ += dt;
}

} // namespace game
