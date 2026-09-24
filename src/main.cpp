#include "core/platform/window.hpp"
#include "core/render/batcher.hpp"
#include "game/game.hpp"
#include "game/content.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>

namespace {

game::FrameInput pollInput(bool& quit) {
  game::FrameInput in{};

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit = true;
      continue;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      switch (event.key.key) {
        case SDLK_ESCAPE: in.togglePause = true; break;
        case SDLK_1: in.choose1 = true; break;
        case SDLK_2: in.choose2 = true; break;
        case SDLK_3: in.choose3 = true; break;
        case SDLK_4: in.choose4 = true; break;
        case SDLK_5: in.choose5 = true; break;
        case SDLK_R: in.restart = true; break;
        case SDLK_T: in.testModeToggle = true; break;
        case SDLK_H: in.heal = true; break;
        case SDLK_B: in.bestiary = true; break;
        default: break;
      }
    }
  }

  const bool* keys = SDL_GetKeyboardState(nullptr);
  float x = 0.0F;
  float y = 0.0F;
  if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) x -= 1.0F;
  if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) x += 1.0F;
  if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) y += 1.0F;
  if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) y -= 1.0F;
  in.moveX = x;
  in.moveY = y;
  return in;
}

} // namespace

int main() {
  try {
    core::platform::WindowDesc desc{};
    desc.title = "test-game";
    desc.width = 1280;
    desc.height = 720;

    core::platform::Window window{desc};
    core::render::Batcher batcher;
    if (!batcher.init()) {
      std::cerr << "fatal: failed to init renderer\n";
      return 1;
    }

    const game::Content content = game::loadContent(GAME_ASSETS_DIR "/data");
    game::Game g{content};

    std::uint64_t previous = SDL_GetPerformanceCounter();
    bool quit = false;

    while (!quit) {
      const std::uint64_t now = SDL_GetPerformanceCounter();
      float dt = static_cast<float>(static_cast<double>(now - previous) /
                                    static_cast<double>(SDL_GetPerformanceFrequency()));
      previous = now;
      if (dt > 0.1F) dt = 0.1F;

      const game::FrameInput input = pollInput(quit);
      g.advance(dt, input);

      int pw = 0;
      int ph = 0;
      SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window.sdlWindow()), &pw, &ph);
      batcher.beginFrame(pw, ph, core::render::Color{0.05F, 0.02F, 0.06F, 1.0F});
      g.render(batcher, 1.0F);
      window.swap();
    }

    batcher.shutdown();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }
}
