#include "core/platform/window.hpp"
#include "core/render/renderer.hpp"
#include "game/game.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>

int main() {
  try {
    core::platform::WindowDesc desc{};
    desc.title = "test-game";
    desc.width = 1280;
    desc.height = 720;

    core::platform::Window window{desc};
    core::render::Renderer renderer;
    game::Game game;

    std::uint64_t previous = SDL_GetPerformanceCounter();

    while (!window.pollExit()) {
      const std::uint64_t now = SDL_GetPerformanceCounter();
      const double dt = static_cast<double>(now - previous) /
                        static_cast<double>(SDL_GetPerformanceFrequency());
      previous = now;

      game.update(dt);

      renderer.beginFrame(window.width(), window.height());
      renderer.endFrame();
      window.swap();
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }
}
