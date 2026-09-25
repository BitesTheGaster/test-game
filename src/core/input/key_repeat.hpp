#pragma once

namespace core::input {

// Edge-triggered input with keyboard-style repeat.  A fresh press fires
// immediately; keeping the key down then waits before the first repeat and
// continues at a slower cadence.  This keeps menu navigation from skipping
// several rows in one frame while still allowing a held key to move through a
// long list comfortably.
class KeyRepeat {
public:
  static constexpr float kInitialDelay = 0.35F;
  static constexpr float kRepeatInterval = 0.11F;

  // `pressed` is the non-repeated key-down edge for this frame.  `held` is the
  // current physical key state.  The edge takes precedence so a key pressed
  // and released between polls still produces exactly one action.
  bool update(bool held, bool pressed, float deltaSeconds) {
    if (pressed) {
      held_ = true;
      nextRepeat_ = kInitialDelay;
      return true;
    }
    if (!held) {
      held_ = false;
      nextRepeat_ = kInitialDelay;
      return false;
    }

    // Do not carry a large frame hitch into a burst of menu actions.  Consume
    // at most one repeat per poll, then schedule the next one from now.
    if (nextRepeat_ > 0.0F) {
      nextRepeat_ -= deltaSeconds;
      if (nextRepeat_ > 0.0F) return false;
    }
    nextRepeat_ = kRepeatInterval;
    return true;
  }

  void reset() {
    held_ = false;
    nextRepeat_ = kInitialDelay;
  }

private:
  bool held_ = false;
  float nextRepeat_ = kInitialDelay;
};

} // namespace core::input
