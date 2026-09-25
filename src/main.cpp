#include "core/platform/window.hpp"
#include "core/render/batcher.hpp"
#include "game/game.hpp"
#include "game/content.hpp"
#include "game/profile.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Resolves the runtime data directory (the folder containing *.toml content
// files, i.e. "<assets>/data").
//
// Portability contract: the game must run from a copy of the project on any
// machine without the binary containing a path minted on the build machine
// (the old configure-time absolute GAME_ASSETS_DIR leaked e.g. /home/<user>,
// which broke "give the build to a friend"). Assets are therefore located
// dynamically:
//   1. relative to the running executable (SDL base path),
//   2. relative to the current working directory (run from the repo root),
//   3. the configure-time define as a last-resort fallback.
std::filesystem::path findDataDir() {
  std::error_code ec;

  // Normalise so ".." elements are resolved; keeps the probe robust against
  // build trees that nest the binary (e.g. build/debug/bin/…).
  auto candidate = [&](std::filesystem::path p) -> std::filesystem::path {
    p = std::filesystem::weakly_canonical(std::filesystem::absolute(p), ec);
    return p;
  };

  std::vector<std::filesystem::path> candidates;
  if (const char* base = SDL_GetBasePath()) {
    candidates.emplace_back(candidate(std::filesystem::path(base) / "assets" / "data"));
    // CMake build trees (Ninja/Make debug|release) place the binary under
    // build/<preset>/ while assets live at the project root.
    candidates.emplace_back(candidate(std::filesystem::path(base) / ".." / "assets" / "data"));
    candidates.emplace_back(candidate(std::filesystem::path(base) / ".." / ".." / "assets" / "data"));
  }
  candidates.emplace_back(candidate("assets/data"));    // run from the repo root
  candidates.emplace_back(candidate("../assets/data"));  // run from a build depth-1 dir
  candidates.emplace_back(candidate("../../assets/data")); // run from a build depth-2 dir
  candidates.emplace_back(candidate(GAME_ASSETS_DIR "/data")); // configure-time last resort

  for (const auto& c : candidates) {
    if (std::filesystem::is_directory(c, ec)) return c;
  }
  throw std::runtime_error(
      "could not locate assets/data (probed exe-relative and CWD-relative "
      "locations, then the configure-time GAME_ASSETS_DIR). Copy the whole "
      "project directory — the game resolves assets relative to the binary.");
}

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
        // Main menu navigation + confirm.
        case SDLK_UP: in.menuUp = true; break;
        case SDLK_DOWN: in.menuDown = true; break;
        case SDLK_LEFT: in.menuLeft = true; break;
        case SDLK_RIGHT: in.menuRight = true; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: in.menuConfirm = true; break;
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
  // W/A/S/D double as menu arrows so the game is fully mouse/keyboard-less.
  if (y > 0.0F) in.menuUp = true;
  if (y < 0.0F) in.menuDown = true;
  if (x < 0.0F) in.menuLeft = true;
  if (x > 0.0F) in.menuRight = true;
  return in;
}

} // namespace

int main() {
  try {
    core::platform::WindowDesc desc{};
    desc.title = game::kGameName;
    desc.width = 1280;
    desc.height = 720;

    core::platform::Window window{desc};
    core::render::Batcher batcher;
    if (!batcher.init()) {
      std::cerr << "fatal: failed to init renderer\n";
      return 1;
    }

    const std::filesystem::path dataDir = findDataDir();
    const game::Content content = game::loadContent(dataDir);

    // Player profile (skins + earned outline unlocks) lives next to the data
    // dir so a copied build keeps its progress with it. Missing file => first
    // launch defaults.
    const std::string profilePath = (dataDir / game::kProfileFileName).string();
    game::Profile profile = game::loadProfile(profilePath);

    game::Game g{content};
    g.setProfile(&profile);
    g.openMenu(); // start on the main menu, not mid-run

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
      // Persist the profile whenever a kill unlocked an outline or the menu
      // changed a selection. consumeProfileDirty() clears the flag.
      if (g.consumeProfileDirty()) {
        game::saveProfile(profilePath, profile);
      }
      if (g.consumeQuitRequest()) {
        quit = true;
      }

      int pw = 0;
      int ph = 0;
      SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window.sdlWindow()), &pw, &ph);
      batcher.beginFrame(pw, ph, core::render::Color{0.05F, 0.02F, 0.06F, 1.0F});
      g.render(batcher, 1.0F);
      window.swap();
    }

    // Final safety write so the last unlock is never lost on quit.
    game::saveProfile(profilePath, profile);

    batcher.shutdown();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }
}
