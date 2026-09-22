#include "core/platform/window.hpp"

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <stdexcept>

namespace core::platform {

Window::Window(const WindowDesc& desc) : width_(desc.width), height_(desc.height) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  auto* window = SDL_CreateWindow(desc.title.c_str(),
                                  desc.width,
                                  desc.height,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
  }
  window_ = window;

  auto* context = SDL_GL_CreateContext(window);
  if (!context) {
    SDL_DestroyWindow(window);
    window_ = nullptr;
    SDL_Quit();
    throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
  }
  glContext_ = context;

  if (!SDL_GL_MakeCurrent(window, context)) {
    throw std::runtime_error(std::string("SDL_GL_MakeCurrent failed: ") + SDL_GetError());
  }
  if (desc.vsync) {
    SDL_GL_SetSwapInterval(1);
  }

  const int version = gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress));
  if (version == 0) {
    throw std::runtime_error("gladLoadGL failed");
  }
}

Window::~Window() {
  if (glContext_) {
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(glContext_));
  }
  if (window_) {
    SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
  }
  SDL_Quit();
}

void Window::swap() const {
  SDL_GL_SwapWindow(static_cast<SDL_Window*>(window_));
}

bool Window::pollExit() const {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN &&
        event.key.key == SDLK_ESCAPE) {
      return true;
    }
  }
  return false;
}

} // namespace core::platform
