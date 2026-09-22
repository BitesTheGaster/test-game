#pragma once

namespace core::sim {

// Fixed simulation timestep with a "spiral of death" valve:
// at most `maxSteps` steps per frame, backlog is dropped afterwards.
class FixedTimestep {
public:
  explicit FixedTimestep(double step = 1.0 / 60.0, int maxSteps = 2)
      : step_(step), maxSteps_(maxSteps) {}

  template <class F>
  void advance(double frameDt, F&& stepFn) {
    if (frameDt > 0.25) {
      frameDt = 0.25; // clamp huge hitches (window drag, debugger, ...)
    }
    accumulator_ += frameDt;
    int executed = 0;
    while (accumulator_ >= step_ && executed < maxSteps_) {
      stepFn();
      accumulator_ -= step_;
      ++executed;
    }
    if (accumulator_ >= step_) {
      accumulator_ = 0.0; // drop backlog instead of spiraling
    }
    alpha_ = accumulator_ / step_;
  }

  [[nodiscard]] double alpha() const { return alpha_; }
  [[nodiscard]] double step() const { return step_; }

  void reset() {
    accumulator_ = 0.0;
    alpha_ = 0.0;
  }

private:
  double step_;
  double accumulator_ = 0.0;
  double alpha_ = 0.0;
  int maxSteps_;
};

} // namespace core::sim
