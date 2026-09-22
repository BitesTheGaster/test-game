# test-game

Vampire Survivors-like roguelike. C++20, SDL3 + OpenGL 3.3, CMake + vcpkg.

## Requirements

- CMake ≥ 3.26, Ninja
- GCC ≥ 13 / Clang ≥ 16 (C++20)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` environment variable set
- System packages for vcpkg builds: `autoconf autoconf-archive automake libtool`

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

Run: `./build/debug/test-game`.

## Controls

| Key            | Action                          |
|----------------|---------------------------------|
| WASD / arrows  | Move (attacks fire automatically) |
| 1 / 2 / 3      | Pick an upgrade on level-up     |
| Esc            | Pause / resume                  |
| R              | Restart after death             |

## Assets: where to put textures

The renderer currently draws **colored shapes only** (circles/rects) — no
textures are required to play. If you want to add sprites later:

```
assets/
├── data/          # balance/content (TOML) — loaded at startup
│   ├── weapons.toml
│   ├── enemies.toml
│   └── upgrades.toml
└── sprites/       # <- put PNG textures here (NOT wired up yet)
    ├── player.png
    ├── enemies/
    └── items/
```

Suggested conventions for `assets/sprites/`:
- PNG with alpha, power-of-two sizes (32x32, 64x64) for future atlas packing
- One sprite per file for now; atlas pipeline comes in a later milestone
- Sprite color in gameplay comes from `color` fields in `enemies.toml` —
  textures, when added, will tint with the same color

Until texture support lands, enemy/player appearance is defined by the
`color` and `shape` fields in TOML files — edit them to restyle the game
without recompiling.

## Content (data-driven)

Balance lives in `assets/data/*.toml`, loaded at startup — no rebuild needed:

- `weapons.toml` — damage, cooldown, projectile count/speed, pierce
- `enemies.toml` — hp, speed, contact damage, radius, xp, spawn time (`unlock_at`), weight, color, shape
- `upgrades.toml` — level-up choices: `effect` id + `value` + `max_stacks`

Effects understood by the game: `damage_mul`, `cooldown_mul`, `speed_mul`,
`pickup_mul`, `max_hp_add`, `regen_add`, `proj_add`, `pierce_add`, `heal`.

## Layout

```
src/core/   engine layer: batcher (GL instancing), spatial hash, timestep, window
src/game/   gameplay layer: components, systems, TOML content loader
tests/      headless unit tests (Catch2)
assets/     data/ (TOML balance), sprites/ (textures, optional)
cmake/      warning/sanitizer/LTO modules
```

## Architecture notes

- **ECS (EnTT)**: all components are POD in dense pools; systems iterate views
- **Fixed timestep**: 60 Hz sim with accumulator, max 2 steps/frame (spiral-of-death valve)
- **Spatial hash**: flat counting-sort grid, allocation-free after warm-up;
  used for enemy separation and projectile hits
- **Deferred destroy**: systems queue entities, destroyed at end of tick
- **Renderer**: single instanced draw call per pass (world / screen),
  circles cut in fragment shader; one 5x7 bitmap font for all text
