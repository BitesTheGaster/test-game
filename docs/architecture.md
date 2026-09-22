# Architecture

How the game is put together: engine vs. gameplay split, the ECS, the fixed
timestep, rendering, and where each gameplay feature lives in the source.

## Overview

```
src/
├── core/     engine: window, timestep, spatial hash, batcher (GL), font
└── game/     gameplay: content loading, simulation, systems, input map
tests/        headless unit tests (Catch2)
assets/       data/ (TOML balance + content), sprites/ (optional, unwired)
tools/        gendocs.py -> regenerates docs/content.md from the TOML
docs/         mechanics.md, content.md, architecture.md
cmake/        warning / sanitizer / LTO helper modules
```

The engine layer (`src/core`) knows nothing about the game: it provides the
window, a 60 Hz fixed-timestep accumulator, an allocation-free spatial hash,
the instanced renderer and the bitmap font. Everything roguelike lives in
`src/game`.

## Gameplay layer

| File | Responsibility |
|------|----------------|
| `game.hpp` / `game.cpp` | The `Game` class: full simulation, systems order, level-ups, elite rolling, rendering of all entities |
| `components.hpp` | POD entity components: `Transform`, `Velocity`, `Radius`, `Sprite`, `Health`, `Enemy`, `EnemyTraits`, `Projectile`, `Xp`, `PlayerTag` |
| `content.hpp` / `content.cpp` | `Content` loader: parses `assets/data/*.toml` into `WeaponDef`, `EnemyDef`, `UpgradeDef` structs |
| `systems.hpp` / `systems.cpp` | Smaller gameplay systems (projectiles, pickups, particles) |
| `main.cpp` | SDL3 window, GL context, input mapping (WASD, 1–5, Esc, R), game loop |

### ECS (EnTT)

Entities are assembled from POD components in dense pools:

- `PlayerTag` + `Transform/Velocity/Radius/Sprite/Health`
- `Enemy` (speed, touch damage, slow timer) + `Transform/Velocity/Radius/
  Sprite/Health/Xp`, and `EnemyTraits` only on elites/champions
- Not-white-purposes projectiles get `Projectile` (damage, pierce, life)

Systems iterate `registry_.view<...>()` over the components they need.
Systems never mutate during iteration: hits, deaths and pickups are queued
into scratch lists and applied after the loop (deferred destroy / deferred
damage), so a view is never invalidated mid-iteration.

### Fixed timestep

- Simulation runs at **60 Hz** via an accumulator: `timestep_.advance(frameDt,
  fixedUpdate)` runs 0–2 sim steps per rendered frame (spiral-of-death valve).
- All gameplay math therefore assumes `1/60 s` per tick (e.g. trait regen
  divides by 60, spawn telegraphs count down 1/60 per tick).
- Input is sampled once per frame and laps over until the state machine
  consumes it (level-up keys, pause, restart).

### Spatial hash

A flat counting-sort grid, **allocation-free after warm-up**:

```
buildSpatialHash() -> scratch arrays of x/y/id -> hash_.build(...)
```

Used by enemy separation and projectile hits. Queries are
`forEachNear(x, y, radius, callback)`.

### Randomness

The whole run is driven by one `rng_` (`std::mt19937`-backed). Restarts seed
it with `1337` for determinism; `Game(content, seed)` exists for seeded
constructor use (tests). The starter weapon, every spawn roll, elite/champion
rolls, trait picks, card choices and rerolls all flow through this stream.

## Simulation order (per fixed tick)

1. `movePlayer()` — input → velocity, Adrenaline speed/invuln logic
2. `buildSpatialHash()`
3. `updateEnemies()` — seek player, separation, trait ticks (regen/slow),
   contact damage, vampiric/venomous/ice-blood on hit
4. `fireWeapons()` — per-slot cooldowns, shared nearest-enemy target, fans
5. `updateProjectiles()` — hits, pierce, chain (Storm Bolt), particle bursts
6. `updatePickups()` — XP magnetism and collection
7. Regen (`regen_add`), black hole timer, adrenaline cooldown, poison tick
8. `spawnWave()` — spawn interval ramp, weighted enemy pick, elite/champion
   rolls, pushes a `PendingSpawn` with a 0.6 s telegraph
9. `processPendingSpawns()` — materializes enemies after their telegraph
10. Particle tick, camera update (frame-level, smooth follow)

## Level-up pipeline

`enterLevelUp()` → `buildChoices()` (weapon card → unique card → normal fill,
see `docs/mechanics.md`):

- **Milestone** levels (`level % 5 == 0`): pool = milestone upgrades whose
  `level ==` current level; shuffle, take 2; violet screen.
- **Weapon grant**: `pickWeaponGrant()` first checks **evolutions whose
  `prereqs` are all owned**, then the untouched normal weapons, shuffled.
- **Unique**: 45% roll from unpicked `kind = "unique"` cards.
- `reroll()` just re-runs `buildChoices()`; consumed counts live in
  `rerollsUsed_` vs `1 + stats_.rerollCharges`.

Upgrades apply through two stateless functions: `applyUpgrade(stats, effect,
value)` for player-wide effects (including the fan/thorns/adrenaline/black-
hole/chain/blood-price/ice-blood uniques) and `applyWeaponEffect(slot, ...)`
for `weapon`-tagged cards (`w_damage_add`, `w_proj_add`, `w_pierce_add`,
`w_cd_mul`). `stacks_[i]` counts each upgrade's stacks and enforces
`max_stacks`.

## Content pipeline

- `assets/data/*.toml` is the single source of balance truth
  (`weapons.toml`, `enemies.toml`, `upgrades.toml`).
- `Content` parses it at startup into packed POD arrays; gameplay never
  hard-codes a weapon/enemy name except the trait enum in `game.cpp`.
- **`tools/gendocs.py`** (stdlib `tomllib` only) re-derives all weapon,
  enemy and upgrade tables for `docs/content.md` — edit balance, regenerate
  docs, never hand-maintain the markdown.

## Rendering

```
Batcher (core)  -> one @instanced draw call per pass
                 -> world pass: shapes (circles cut in the fragment shader,
                    quads for rects)
                 -> screen pass: text via a 5x7 bitmap font
```

- **No textures required**: colors come from the `color` fields in TOML
  (`proj_color` for projectiles); `assets/sprites/` is reserved for a future
  atlas pipeline.
- Elites/champions: tint toward white, 1.35× radius, always-on HP bar.
- Elite/champion **name tags are drawn in the screen pass** with a
  world→screen projection so they stay legible and screen-aligned.
- Spawn telegraphs: pulsing rings in the world pass + edge dots in the
  screen pass.
- UI: XP bar, HP/shield bars, level, timer, kill count, and the level-up /
  milestone / game-over overlays.

## Key constants (game.cpp)

| Constant | Value | Meaning |
|----------|-------|---------|
| `kMaxWeapons` | 4 | Weapon slots |
| `kSpawnDist` | 11 | Spawn distance from player (units) |
| `kSpawnTelegraph` | 0.6 s | Telegraph duration before an enemy appears |
| `kShieldRegenRate` | 10 HP/s | Shield regen out of combat |
| `kShieldRegenDelay` | 4 s | Damage-free time before regen starts |
| `kContactIframes` | 0.55 s | Invulnerability after being hit |
| `kMaxEnemies` | — | Hard cap on live enemies (see `game.hpp`) |

## Testing

Headless Catch2 tests in `tests/test_game.cpp` construct a `Game` directly
(no SDL window) and assert on:

- `mitigateDamage` defense formula (flat + percent, clamping in 0)
- Defense/shield/lifesteal pipeline behavior
- Unique item effects (fan, thorns, adrenaline, black hole, chain,
  blood price, extra choice, reroll)
- Milestone offering at level 5
- Reroll budget accounting
- Projectile/weapon stat accumulation

Run everything (configure + build + test) with:

```sh
VCPKG_ROOT=$HOME/vcpkg cmake --workflow --preset debug
```

Presets: `debug`, `release` (+LTO), `asan`, `tsan`. The build is clean under
`-Wall -Wextra -Wpedantic` with warnings-as-errors enabled in the workflow.