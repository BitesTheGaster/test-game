cmake_minimum_required(VERSION 3.26)
project(test-game VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(LTO)

find_package(SDL3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)

# --- Core: engine layer (no game knowledge) ----------------------------------
add_library(game_core STATIC
  src/core/platform/window.cpp
  src/core/render/batcher.cpp
)
target_include_directories(game_core PUBLIC src)
target_link_libraries(game_core
  PUBLIC
    EnTT::EnTT
  PRIVATE
    SDL3::SDL3
    glad::glad
    game_warnings
    game_sanitizers
    game_lto
)

# --- Game: gameplay layer ----------------------------------------------------
add_library(game_lib STATIC
  src/game/game.cpp
  src/game/content.cpp
  src/game/profile.cpp
)
target_include_directories(game_lib PUBLIC src)
target_compile_definitions(game_lib
  PUBLIC GAME_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)
target_link_libraries(game_lib
  PUBLIC
    game_core
    tomlplusplus::tomlplusplus
  PRIVATE
    game_warnings
    game_sanitizers
    game_lto
)

# --- Executable --------------------------------------------------------------
add_executable(test-game src/main.cpp)
target_link_libraries(test-game PRIVATE game_lib SDL3::SDL3 game_warnings game_sanitizers game_lto)

# --- Tests (headless) --------------------------------------------------------
enable_testing()
add_subdirectory(tests)
