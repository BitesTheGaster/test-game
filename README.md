# test-game

Vampire Survivors-like roguelike. C++20, SDL3 + OpenGL 3.3, CMake + vcpkg.

Multi-weapon build-your-arsenal roguelike: 32 weapons (incl. 10 evolutions and
4 three-weapon super evolutions), 18 enemy types with elite/champion/overlord
traits, 178 upgrades (normal / unique / milestone), an opening **3-weapon pick**
instead of a fixed starter, three **active abilities** on `J` / `K` / `L` that
are live from the first second and gated only by cooldown, a **momentum kill
chain** that only pays while you are actively killing, an **adaptive tribunal
director** that opens champions when elites stop being a problem and overlords
when champions do, regenerating shield + defense + lifesteal, an H-key heal,
defense-scaled invulnerability frames, pierce that cancels AoE damage falloff,
knockback with an Impact multiplier, an Esc **bestiary**, a difficulty ramp that
accelerates after 6 minutes, an **in-game manual** on `F1`, and a **two-step
reset** in the main menu.

Every one of the 32 weapons has at least one exclusive unique card, and a test
fails the build if that stops being true — a weapon you can pick up and then
never improve is a dead slot.

## Documentation

- [docs/mechanics.md](docs/mechanics.md) — formulas (defense, shield, XP),
  level-ups, rerolls, evolutions, elites/traits, spawn & ramp curves
- [docs/content.md](docs/content.md) — all weapons, enemies, upgrades and
  manual pages (generated from `assets/data/*.toml` by `tools/gendocs.py`)
- [docs/architecture.md](docs/architecture.md) — ECS, timestep, systems
  order, rendering, content pipeline

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

| Key         | Action                                    |
|-------------|-------------------------------------------|
| WASD / arrows | Move (attacks fire automatically)         |
| J           | **Phase Dash** — teleport along your movement (or at the nearest enemy), with i-frames (5 s) |
| K           | **Overload** — radial damage + knockback (14 s) |
| L           | **Stasis** — the world slows to 35%, you do not (30 s) |
| H           | Guaranteed 50% max-HP heal (30 s cooldown) |
| 1 / 2 / 3 / 4 / 5 | Pick the matching card on level-up   |
| R           | Reroll the level-up choice (1 free per level, more with Second Chance) |
| Esc         | Pause / resume — overlays your character sheet |
| F1          | **Open / close the in-game manual** (works from the menu, a run and the pause screen) |
| B           | While paused: toggle the **bestiary** (kills, stats, elite+ variants) |
| R (after death) | Restart                              |
| T           | Toggle **weapon test mode** (live run only) |
| 1 / 2       | In test mode: previous / next weapon (all 32 incl. evolutions & supers) |
| 3           | In test mode: apply a maxed-out build boost (damage/projectiles/pierce/fire rate) |
| 4           | In test mode: toggle enemy waves |
| 5           | In test mode: exit back to the run — **which ends the run** |
| E / I / F / X / R | In test mode: item picker / immortality / difficulty clock / kill me / max every item |

The test sandbox is a **hermetic** rig, not a cheat: it pays no XP, never pays a
skin or outline unlock, and leaving it drops you on the game-over screen. See
[docs/mechanics.md](docs/mechanics.md#weapon-test-mode).

## The manual (`F1`)

The same documentation this repo ships in `docs/`, trimmed to what the 5×7
bitmap font can draw and split into 11 one-screen pages in
`assets/data/manual.toml`. It is a topmost modal overlay, so it works from the
main menu, mid-run and from the pause screen. Arrows and `1`–`5` flip pages.

The loader rejects any character the font cannot draw, so a stray em dash fails
the content load instead of rendering as a screen full of `?`. A **missing**
`manual.toml` is tolerated (the game still starts); a malformed one throws.

## The main menu

| Row | What it does |
|-----|--------------|
| START RUN | Begin a run |
| SKIN | Cycle your player colour |
| OUTLINE | Cycle unlocked outline styles (locked ones are skipped) |
| MANUAL | Open the in-game manual (same as `F1`) |
| RESET PROGRESS | **Wipes your skin and every unlocked outline.** Two-step: the first Enter only arms it, the second wipes. Navigating away disarms it. |
| QUIT | Leave |

## Assets: where to put textures

The renderer currently draws **colored shapes only** (circles/rects) — no
textures are required to play. If you want to add sprites later:

```
assets/
├── data/          # balance/content (TOML) — loaded at startup
│   ├── weapons.toml
│   ├── enemies.toml
│   ├── upgrades.toml
│   └── manual.toml   # in-game manual pages (optional — a missing file is fine)
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

- `weapons.toml` — damage, cooldown, projectile count/speed/pierce/spread,
  `starter` flags, per-attack fields (`beam_*`, `nova_*`, `lure_*`, `sweep_*`,
  `vortex_*`, `prism_*`, …) and `requires` evolution pairs
- `enemies.toml` — hp, speed, contact damage, radius, xp, spawn time
  (`unlock_at`), weight, color, shape
- `upgrades.toml` — level-up cards: `kind` (`normal` / `unique` / `milestone`),
  `effect` id + `value`, `max_stacks`, optional `weapon` tag and milestone `level`
- `manual.toml` — the in-game manual, one `[[page]]` per screen
  (`id` / `title` / `lines`). A leading `>` marks a highlighted bullet, `#` a
  sub-heading, two spaces an indent, `""` a spacer. The loader rejects any
  character the bitmap font cannot draw.

**Global effects** (cards with no `weapon` tag): `damage_mul`, `fire_rate`
(additive: final delay = base / (1 + bonus)), `speed_mul`, `pickup_mul`,
`max_hp_add`, `regen_add`, `proj_add`, `pierce_add`, `heal`, `heal_pct`
(percentage of the *current* max HP), `defense_add`, `armor_pierce_add`,
`lifesteal_add`, `lifesteal_heal`, `shield_add`, `shield_regen`,
`shield_delay`, `xp_mul`, `knockback_mul` (Impact), `weapon_slot_add`, the six
momentum effects (`momentum_damage`, `momentum_speed`, `momentum_window`,
`momentum_rate`, `momentum_chain`, `momentum_gain`, `momentum_bloodthirst`),
the eight ability effects (`ability_haste`, `ability_might`, `ability_phase`,
`ability_stasis`, `ability_echo` and the three stackable `ability_dash`,
`ability_burst`, `ability_slow`), and the unique-item effects (`fan`, `thorns`,
`extra_choice`, `reroll_add`, `adrenaline`, `black_hole`, `chain`,
`blood_price`, `ice_blood`, `last_stand`, `knockback_retaliate`).

**Per-weapon effects** (cards with a `weapon` tag): `w_damage_add`,
`w_proj_add`, `w_pierce_add`, `w_fire_rate`, `w_lure_power`, `w_nova_power`, and
one identity-scaling card per thin weapon (`w_orb_grow`, `w_scythe_reach`,
`w_beam_lance`, `w_chain_arc`, `w_nova_wide`, `w_reap_wide`,
`w_boomerang_reach`, `w_zone_pools`, `w_bomb_blast`, `w_lure_anchor`,
`w_prism_lattice`, `w_halo_wings`, `w_vortex_core`).

**Weapon uniques** (one per weapon, `kind = "unique"`): `w_unique_homing`,
`w_unique_area`, `w_unique_vortex`, `w_unique_hearthfire`, `w_unique_cataclysm`,
`w_unique_harvest`, `w_unique_prism`, `w_unique_thunderlord`,
`w_unique_supernova`, `w_unique_molten`, `w_unique_everflame`,
`w_unique_arcsaw`, `w_unique_bell`, `w_unique_gravitic`, `w_unique_lash`,
`w_unique_corona`, `w_unique_gyre`, `w_unique_refract`, `w_unique_rime`,
`w_unique_siege_doctrine`, `w_unique_skewer`, `w_unique_bore`,
`w_unique_discharge`, `w_unique_chainlash`, `w_unique_wingbeat`,
`w_unique_singularity`.

A typo in an effect id is a silent dead pick, so a test walks **every** card:
global ones must resolve in `applyUpgrade`, and weapon-scoped ones must actually
move that weapon's own numbers when the weapon is armed.

Regenerate the content reference docs after touching the TOML:

```sh
python3 tools/gendocs.py
```

## Layout

```
src/core/   engine layer: batcher (GL instancing), spatial hash, timestep, window
src/game/   gameplay layer: components, systems, TOML content loader
tests/      headless unit tests (Catch2)
assets/     data/ (TOML balance), sprites/ (textures, optional)
tools/      gendocs.py (regenerates docs/content.md from the TOML)
docs/       mechanics.md, content.md, architecture.md
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
