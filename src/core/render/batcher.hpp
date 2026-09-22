#pragma once

#include "core/render/color.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace core::render {

// Instanced colored-shape renderer (GL 3.3 core).
// One draw call per flush(); circles are cut in the fragment shader.
// Textures are not required: everything is drawn as colored rects/circles.
class Batcher {
public:
  bool init();
  void shutdown();

  void beginFrame(int fbWidth, int fbHeight, Color clearColor);

  // World space: y-up, `zoom` = pixels per world unit.
  void setWorldView(float centerX, float centerY, float zoom);
  // Screen space: pixels, origin top-left, y-down.
  void setScreenView();

  // Center-based rect in current view coordinates.
  void rect(float x, float y, float w, float h, Color c);
  // Corner-based rect (convenient for screen-space UI).
  void rectTopLeft(float x, float y, float w, float h, Color c);
  void circle(float x, float y, float radius, Color c);
  // Top-left anchored text, one pixel-block = `scale` units.
  void text(float x, float y, float scale, Color c, std::string_view str);
  [[nodiscard]] float textWidth(float scale, std::string_view str) const;

  void flush();

  [[nodiscard]] int fbWidth() const { return fbW_; }
  [[nodiscard]] int fbHeight() const { return fbH_; }

private:
  void push(float cx, float cy, float halfW, float halfH, float shape, Color c);

  unsigned program_ = 0;
  unsigned vao_ = 0;
  unsigned quadVbo_ = 0;
  unsigned instVbo_ = 0;
  int uViewHalf_ = -1;

  std::vector<float> data_; // 9 floats per instance

  float cx_ = 0.0F;
  float cy_ = 0.0F;
  float zoom_ = 48.0F;
  bool yDown_ = false;
  int fbW_ = 1;
  int fbH_ = 1;
};

} // namespace core::render
