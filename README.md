# test-game

Vampire Survivors-like roguelike. C++20, SDL3 + OpenGL 3.3, CMake + vcpkg.

## Requirements

- CMake ≥ 3.26, Ninja
- GCC ≥ 13 / Clang ≥ 16 (C++20)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` environment variable set

## Build

```sh
export VCPKG_ROOT=~/vcpkg

# Configure + build + test in one command (workflow preset)
cmake --workflow --preset debug

# Or step by step
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets:

| Preset  | Purpose                          |
|---------|----------------------------------|
| debug   | Debug, fast build                |
| release | Release + LTO                    |
| asan    | Debug + AddressSanitizer + UBSan |
| tsan    | Debug + ThreadSanitizer          |

Run: `./build/debug/test-game` (Esc quits).

## Layout

```
src/core/   engine layer: ECS, renderer, sim, platform (no game knowledge)
src/game/   gameplay layer: components, systems, content
src/tests/  headless unit tests
assets/     sprites, fonts, audio
cmake/      warning/sanitizer/LTO modules
```
