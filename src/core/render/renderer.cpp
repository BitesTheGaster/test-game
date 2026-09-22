#include "core/render/renderer.hpp"

#include <glad/gl.h>

namespace core::render {

void Renderer::beginFrame(int width, int height) const {
  glViewport(0, 0, width, height);
  glClearColor(0.06F, 0.02F, 0.05F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::endFrame() const {
  // Nothing yet: flushing happens in Window::swap() during milestone 0.
}

} // namespace core::render
