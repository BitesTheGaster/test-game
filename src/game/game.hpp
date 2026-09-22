#pragma once

namespace game {

// Owns the per-frame game state. Milestone 0: skeleton only.
class Game {
public:
  void update(double dt);
  [[nodiscard]] double elapsed() const { return elapsed_; }

private:
  double elapsed_ = 0.0;
};

} // namespace game
