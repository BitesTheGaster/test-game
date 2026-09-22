#pragma once

#include <string>

namespace core::platform {

struct WindowDesc {
  std::string title = "test-game";
  int width = 1280;
  int height = 720;
  bool vsync = true;
};

// Owns SDL_Init, the SDL window and the OpenGL 3.3 core context.
// Non-copyable, RAII: shuts everything down in the destructor.
class Window {
public:
  explicit Window(const WindowDesc& desc);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  void swap() const;
  [[nodiscard]] bool pollExit() const;
  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }

private:
  void* window_ = nullptr;   // SDL_Window*
  void* glContext_ = nullptr; // SDL_GLContext
  int width_ = 0;
  int height_ = 0;
};

} // namespace core::platform
