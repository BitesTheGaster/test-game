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
| `components.hpp` | POD entity components: `Transform`, `Velocity`, `Radius`, `Sprite`, `Health`, `Enemy`, `EnemyTraits`, `EnemyShot`, `Projectile`, `Xp`, `PlayerTag` |
| `content.hpp` / `content.cpp` | `Content` loader: parses `assets/data/*.toml` into `WeaponDef`, `EnemyDef`, `UpgradeDef` structs |
| `systems.hpp` / `systems.cpp` | Smaller gameplay systems (projectiles, pickups, particles) |
| `main.cpp` | SDL3 window, GL context, input mapping (WASD, 1–5, Esc, R, T, H), game loop |

### ECS (EnTT)

Entities are assembled from POD components in dense pools:

- `PlayerTag` + `Transform/Velocity/Radius/Sprite/Health`
- `Enemy` (speed, touch damage, slow timer) + `Transform/Velocity/Radius/
  Sprite/Health/Xp`, plus `EnemyTraits` on every enemy (tier, defense,
  resistances, regen/shield, archer/aura params). Archers' projectiles are
  separate entities with `EnemyShot`.
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
constructor use (tests). The opening weapon pick, every spawn roll, elite/
champion rolls, trait picks, card choices and rerolls all flow through this
stream.

## Simulation order (per fixed tick)

1. `movePlayer()` — input → velocity, Adrenaline speed/invuln logic
2. `buildSpatialHash()`
3. `updateEnemies()` — seek player, separation, trait ticks (regen, archer
   shots, damage aura), contact damage, vampiric/venomous/ice-blood on hit
4. `fireWeapons()` — per-slot cooldowns, shared nearest-enemy target, fans,
   AoE crowd falloff
5. `updateProjectiles()` — hits, pierce, chain (Storm Bolt), particle bursts
6. `updatePickups()` — XP magnetism and collection (scaled by `xpMul`)
7. Regen (`regen_add`), black hole timer, adrenaline cooldown, poison tick
8. `spawnWave()` — spawn interval ramp, weighted enemy pick, elite/champion/
   overlord rolls, pushes a `PendingSpawn` with a 0.6 s telegraph
9. `processPendingSpawns()` — materializes enemies after their telegraph
10. Particle tick, camera update (frame-level, smooth follow)

## Level-up pipeline

A run opens with `enterStarterPick()` → `buildStarterChoices()`, which shuffles
the `starter = true` weapons and offers 3; `chooseUpgrade()` equips the pick
without spending a level-up. Normal level-ups run
`enterLevelUp()` → `buildChoices()` (weapon card → unique card → normal fill,
see `docs/mechanics.md`):

- **Milestone** levels (`level >= 4 && (level & (level-1)) == 0`, i.e. powers
  of two): pool = milestone upgrades whose `level ==` current level; shuffle,
  take 2; violet screen.
- **Weapon grant**: `pickWeaponGrant()` first checks **evolutions whose
  `prereqs` are all owned**, then the untouched normal weapons, shuffled.
- **Unique**: 45% roll from unpicked `kind = "unique"` cards; cards tagged
  `weapon = "<id>"` (the 13 weapon uniques) are only eligible while that
  weapon is equipped.
- `reroll()` just re-runs `buildChoices()`; consumed counts live in
  `rerollsUsed_` vs `1 + stats_.rerollCharges`.

Upgrades apply through two stateless functions: `applyUpgrade(stats, effect,
value)` for player-wide effects (including the fan/thorns/adrenaline/black-
hole/chain/blood-price/ice-blood uniques) and `applyWeaponEffect(slot, ...)`
for `weapon`-tagged cards (`w_damage_add`, `w_proj_add`, `w_pierce_add`,
`w_fire_rate`, plus the `w_unique_*` weapon uniques). `stacks_[i]` counts each
upgrade's stacks and enforces `max_stacks`.

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
- Elites/champions/overlords: tint toward white, larger radius, always-on HP
  bar, and a **coloured outline ring** (gold / orange / violet by tier) drawn
  as a ring of dots in the world pass. No floating name tags.
- `TraitAura` enemies draw a faint pulsing disc; `TraitArcher` shots are drawn
  with a soft glow in the world pass.
- Spawn telegraphs: pulsing rings in the world pass + edge dots in the
  screen pass.
- UI: XP bar, HP/shield bars, H-heal cooldown, level, timer, kill count, and
  the level-up / milestone / game-over overlays.

## Key constants (game.cpp)

| Constant | Value | Meaning |
|----------|-------|---------|
| `kMaxWeapons` | 5 | Weapon slots |
| `kSpawnDist` | 11 | Base spawn distance from player (units); scales to 2× by 10:00 |
| `kSpawnTelegraph` | 0.6 s | Telegraph duration before an enemy appears |
| `kShieldRegenRate` | 10 HP/s | Shield regen out of combat |
| `kShieldRegenDelay` | 4 s | Damage-free time before regen starts |
| `kContactIframes` | 0.18 s | Base invulnerability after being hit; scaled by `1 + defense/500` |
| `kEliteHpMin` / `kEliteHpMax` | 5 / 10 | Elite HP multiplier range (rolled per spawn) |
| `kChampionHpMin` / `kChampionHpMax` | 25 / 100 | Champion HP multiplier range |
| `kOverlordHpMin` / `kOverlordHpMax` | 125 / 1000 | Overlord HP multiplier range |
| `kMaxEnemies` | — | Hard cap on live enemies (see `game.hpp`) |

## Testing

Headless Catch2 tests in `tests/test_game.cpp` construct a `Game` directly
(no SDL window) and assert on:

- `mitigateDamage` defense formula (flat + percent, clamping in 0)
- `aoeFalloff`, `enemyDefense`, `enemyLifestealResistance`,
  `enemyKnockbackResistance` and `traitsForTier` pure helpers
- Defense/shield/lifesteal pipeline behavior
- Elite/champion/overlord spawn stat boosts and trait-flag counts
- Unique item effects (fan, thorns, adrenaline, black hole, chain,
  blood price, extra choice, reroll, XP multiplier, Last Stand, Repulsion Field)
- Knockback: Repulsion Field retaliation and the Impact multiplier, plus the
  decaying-impulse push landing on the enemy
- Opening 3-weapon pick, defense-scaled iframes, Last Stand low-HP iframes
- Halo spokes (Radiant Halo) and super-evolution prerequisites
- Void Gyre suction zones (count/size track the projectile stat) and the
  Prism Array multi-target beam locks
- The weapon test sandbox: full run snapshot/restore, the item picker, unlimited
  rerolls, immortality and the difficulty-clock multiplier
- The lethal-hit rule (a hit covering the remaining HP always kills)
- Bestiary kill/tier tracking and the B overlay toggle
- Milestone offering at level 5
- Reroll budget accounting
- Projectile/weapon stat accumulation
- Menu key repeat (`core::input::KeyRepeat`): fires on the press, waits out the
  initial delay, then paces itself and never bursts after a frame hitch
- A maxed-out build still completing its level-up (the no-applicable-card
  fallback, the "nothing left, continue" card and the skip/recovery path)
- Displacement resistance: Void Gyre drag and every shove shrink against an
  enemy's live knockback resistance
- The adaptive tribunal director: champions gated on elite handling, overlords
  on champion handling, both reversible, both with hysteresis
- The momentum kill chain: it feeds on kills, pays out as damage/fire rate, is
  halved by a hit, goes cold on a timer and respects its cap
- Upgrade-pool shape: no dead stat family may outnumber the offensive core
- `xpForLevel` monotonicity and a pacing floor, so a run cannot max out early

Run everything (configure + build + test) with:

```sh
VCPKG_ROOT=$HOME/vcpkg cmake --workflow --preset debug
```

Presets: `debug`, `release` (+LTO), `asan`, `tsan`. The build is clean under
`-Wall -Wextra -Wpedantic` with warnings-as-errors enabled in the workflow.