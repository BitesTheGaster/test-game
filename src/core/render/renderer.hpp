#pragma once

namespace core::render {

// Minimal renderer for milestone 0: clears the screen with a fixed color.
// Instanced sprite batching lands in milestone 1.
class Renderer {
public:
  void beginFrame(int width, int height) const;
  void endFrame() const;
};

} // namespace core::render
