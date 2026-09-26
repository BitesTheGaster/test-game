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
| `main.cpp` | SDL3 window, GL context, input mapping (WASD, 1–5, Esc, R, T, H, J/K/L, X), game loop |

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

Frame-level work runs before the accumulator: `updateAbilities()` consumes the
`J`/`K`/`L` presses (cooldown-gated, legal at any time — including on the
level-up screen's other keys), and `moveX_/moveY_` are latched for the tick.

1. `movePlayer()` — input → velocity, Adrenaline speed/invuln logic
2. `buildSpatialHash()`
3. `updateEnemies()` — seek player, separation, trait ticks (regen, archer
   shots, damage aura), contact damage, vampiric/venomous/ice-blood on hit.
   Runs on `dt = (1/60) * worldTimeScale_`, so Stasis slows the horde without
   touching the player
4. `fireWeapons()` — per-slot cooldowns, shared nearest-enemy target, fans,
   AoE crowd falloff. This is where each `AttackType` lives: `Lure` plants a
   `Lure` beacon, `Prism` walks one reflection path per locked beam, `Sweep`
   either reaps the prey's circle (scythe) or lashes the arc in front of the
   player (`sweep_lead > 0`, the whip)
5. `updateProjectiles()` — hits, pierce, chain (Storm Bolt), particle bursts
6. `updateOrbitBlades()` / `syncHaloBeams()` / `syncVortices()` — the persistent
   ring weapons. The orbit pass also runs the **interior whirl** (once per
   build, not per blade), which is what makes the inside of the dagger's ring
   dangerous instead of a blind spot
7. `updateLures()` — beacon life, the inward drag on everything in reach, and
   the kill core; also on the scaled clock
8. `updatePickups()` — XP magnetism and collection (scaled by `xpMul`)
9. Regen (`regen_add`), black hole timer, adrenaline cooldown, poison tick
10. `spawnWave()` — spawn interval ramp, weighted enemy pick, elite/champion/
    overlord rolls, pushes a `PendingSpawn` with a 0.6 s telegraph
11. `processPendingSpawns()` — materializes enemies after their telegraph
12. Particle tick, camera update (frame-level, smooth follow)

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
  `weapon = "<id>"` (the 21 weapon uniques) are only eligible while that
  weapon is equipped.
- `reroll()` just re-runs `buildChoices()`; consumed counts live in
  `rerollsUsed_` vs `1 + stats_.rerollCharges`.

Upgrades apply through two stateless functions: `applyUpgrade(stats, effect,
value)` for player-wide effects (including the fan/thorns/adrenaline/black-
hole/chain/blood-price/ice-blood uniques and the five `ability_*` cards) and
`applyWeaponEffect(slot, ...)` for `weapon`-tagged cards (`w_damage_add`,
`w_proj_add`, `w_pierce_add`, `w_fire_rate`, `w_lure_power`, `w_nova_power`,
plus the `w_unique_*` weapon uniques). `stacks_[i]` counts each upgrade's
stacks and enforces `max_stacks`.

### Active abilities

`Game::Ability` is a three-entry enum (`Blink`, `Burst`, `Stasis`) with
`kAbilityCount` and a `kBaseAbilityCd{5, 14, 30}` table, all public so the
tests and the HUD agree with the code. `updateAbilities()` reads the three
`FrameInput` flags, checks `abilityReady()` and calls `castBlink()`,
`castBurst()` or `castStasis()`. Every number they use lives in `PlayerStats`
(`blinkDist`, `blinkIframes`, `burstRadius`, `burstDamage`, `burstKnockback`,
`stasisDuration`, `stasisSlow`, `abilityCdMul`, `abilityEcho`), so the cards
retune the keys without the casts knowing anything about upgrades.

Stasis is a single `worldTimeScale_` float rather than a per-system flag: the
hostile systems multiply their own `dt` by it, `simTime_` keeps counting real
seconds (so the difficulty ramp is not stretched), and the snapshot saves and
restores both the timer and the scale.

## Content pipeline

- `assets/data/*.toml` is the single source of balance truth
  (`weapons.toml`, `enemies.toml`, `upgrades.toml`, `manual.toml`).
- `Content` parses it at startup into packed POD arrays; gameplay never
  hard-codes a weapon/enemy name except the trait enum in `game.cpp`.
- **`tools/gendocs.py`** (stdlib `tomllib` only) re-derives all weapon,
  enemy, upgrade and manual tables for `docs/content.md` — edit balance,
  regenerate docs, never hand-maintain the markdown.

### The in-game manual

`manual.toml` is data, not code, and it is loaded by the same pipeline into
`Content::manual` as `ManualPage { id, title, lines }`. It is deliberately the
*only* content file allowed to be absent — a stripped distribution must still
start, so `manualPage()` returning `nullptr` and an empty `Content::manual` are
both valid states. A **malformed** file throws instead, because a typo in the
manual is a content bug worth failing loudly on rather than rendering as a screen
full of `?`.

The loader validates every page on the way in:

- **non-empty** id and title, and at least one line;
- **unique ids**, because `Content::manualPage()` resolves by id and a duplicate
  would make the lookup ambiguous;
- **every character is drawable**, via `core::render::fontSupports()`
  (`batcher.hpp`). The 5×7 font covers ASCII 32–96 with lowercase folded to
  uppercase; anything else would render as `?`. Checking at *load* time turns a
  silent visual bug into a content-load failure with a file name in the message.

The renderer itself knows nothing about content: `renderManual()` draws a page
rail, a body and a hint bar, and `>` / `#` / `"  "` / `""` are the only markup.
Those prefixes are stripped before drawing, so they only ever make a line
*shorter* on screen — which is why the layout test only has to check the line
body, not the markup.

### Card/effect plumbing

`UpgradeDef::effect` is a string resolved in one of two free functions:

- `applyUpgrade(PlayerStats&, effect, value)` — global stats. Returns
  `{valid, heal, shield}`; `valid == false` means "unrecognised effect", which is
  a **content bug the tests catch**: one test walks every card with an empty
  `weapon` and requires `valid`, another arms every weapon in turn, takes every
  card scoped to it and requires the weapon's own numbers to have moved
  (`testWeaponSnapshot` is a comparable copy of the whole configurable stat
  block, so a new `WeaponSlot` field is covered without touching a test).
- `Game::applyWeaponEffect(slot, effect, value)` — per-weapon, dispatched on
  `def.weapon`. Void-returning, and the caller has already proved the weapon is
  held (`findWeaponSlot`).

The halo and vortex families are the one place where a weapon effect has to
**resync live entities** after editing a stat: `syncHaloBeams()` and
`syncVortices()` re-read the count and the geometry, because those weapons
maintain persistent components rather than spawning per shot. `w_proj_add` already
did this; the new halo/vortex uniques do it too, which is why picking one up
visibly changes the ring rather than silently not.

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
- Overlays are drawn in a fixed order, and the **manual is last and opaque**:
  it covers the HUD, the pause sheet, the bestiary and the sandbox alike.
- The **main menu is an exception, and has to be**. `advance()` treats the manual
  as the topmost modal, so from the menu it takes every keystroke — and if
  `render()` returned early on `menuOpen_` without drawing it, the keys would be
  live and the screen would never change. A modal that eats your input without
  drawing is the worst of both, and from the menu there was also no way back:
  the menu's own handler stops being called the moment the manual is open. So
  the menu draws first and the manual over the top of it.

## Overlay precedence

There are four things that can take the keyboard, and the order is fixed in
`advance()`:

1. **Manual** (`F1`) — topmost, opaque, eats the whole frame. Reachable from
   the main menu (a MANUAL row, or F1) as well as from a run and the pause
   sheet, and the page it was on survives a reset.
2. **Main menu** — swallows input until START or QUIT.
3. **Pause sheet** — a modal character sheet, with B toggling the bestiary.
4. **Gameplay** — the only state in which the simulation advances at all.

The same precedence drives key auto-repeat in `main.cpp`, which uses one
`KeyRepeat` shared by the menu, the test shop and the manual
(`g.menuOpen() || g.testShopOpen() || g.manualOpen()`) so holding an arrow
flips manual pages at the same rate it moves menu rows.

## Key constants (game.cpp)

| Constant | Value | Meaning |
|----------|-------|---------|
| `kBaseWeapons` / `kMaxWeapons` | 4 / 8 | Weapon slots, and the hard cap after 3 Arsenal Cores + Hollow Chamber |
| `kMaxSlotCards` | 4 | Hard cap on `weapon_slot_add` stacks, whatever content says |
| `kSpawnDist` | 11 | Base spawn distance from player (units); scales to 2× by 10:00 |
| `kSpawnTelegraph` | 0.6 s | Telegraph duration before an enemy appears |
| `kShieldRegenRate` | 10 HP/s | Shield regen out of combat, at 1.0× `shieldRegenMul` |
| `kShieldRegenDelay` | 4 s | Damage-free time before regen starts, before `shieldRegenDelay` |
| `kShieldRegenDelayFloor` | 0.5 s | Floor on the delay, so no build makes the pool permanent |
| `kContactIframes` | 0.18 s | Base invulnerability after being hit; scaled by `1 + defense/500` |
| `kOrbitInnerMul` | 0.35 | Share of one blade's damage the orbit's interior whirl pays |
| `kOrbitWindowHalf` | 0.42 rad | Half-width of the Blade Vortex's safe gap, measured from the first blade |
| `WaveEffect::kHookAngle` | 1.15 rad | How far off its own heading a hooking wave pushes: ~66°, mostly across the fan, a little along it |
| `kChainTelegraph` | 0.22 s | How long a chain bolt converges on its first target before it strikes |
| `kChainStrike` | 0.09 s | Ramp of the bolt's brightness as it lands |
| `kChainLinger` | 0.45 s | Quadratic fade of the arc after the last jump, so the bolt is never cut off mid-flash |
| `kBaseAbilityCd` | 5 / 14 / 30 s | Phase Dash / Overload / Stasis cooldowns, before `abilityCdMul` |
| `kEliteHpMin` / `kEliteHpMax` | 3.5 / 6.5 | Elite HP multiplier range (rolled per spawn) |
| `kChampionHpMin` / `kChampionHpMax` | 15 / 55 | Champion HP multiplier range |
| `kOverlordHpMin` / `kOverlordHpMax` | 70 / 450 | Overlord HP multiplier range |
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
  Prism Array beams, which ricochet once per pierce point
- The weapon test sandbox: full run snapshot/restore, the item picker,
  immortality, the difficulty-clock multiplier, and the three ways it refuses
  to be a cheat (no XP, no unlocks, leaving it kills the run)
- The roster shape: 18 base weapons, 10 evolutions, 4 supers, every
  prerequisite resolvable and every super built from three base weapons
- The second-wave mechanics: the dagger's interior whirl, the whip's lead lash
  versus the scythe's reap, the fused mortar arcing over the front rank, the
  Grave Bell's drag (inside reach dies, outside it does not), the Solar Lance's
  three-beam triad
- The three abilities: they are ready on tick one, the dash moves the player
  and the cooldown really gates a second press, Stasis measurably slows the
  world, the cards retune rather than unlock, and the sandbox rolls the
  cooldowns back
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
- The in-game manual: it loads, its ids are unique, every glyph is drawable,
  every shipped page fits one screen at the reference height, a build with no
  `manual.toml` still starts, and **F1 works from a run, the pause screen and
  the main menu** (including by selecting the MANUAL row)
- Progress reset: it takes two confirms, navigating away disarms it, the wipe
  clears the skin/outline/unlocks and marks the profile dirty, it is inert
  without a profile, and a wiped profile does not re-grant an outline on the
  next kill
- **Card coverage, as a rule rather than a hope**: every weapon in the roster
  has at least one `kind = "unique"` card, every card with no `weapon` resolves
  to a real effect, and every weapon-scoped card actually moves its own weapon's
  numbers when that weapon is armed
- The thin-mechanics cards: the chain's fire-rate/cap/fill axes, all three
  stackable ability cards (including their floors), the shield's rate and delay,
  the percentage heal, and the arsenal cap of 4 + slot cards
- Upgrade-pool shape: no dead stat family may outnumber the offensive core
- `xpForLevel` monotonicity and a pacing floor, so a run cannot max out early

Run everything (configure + build + test) with:

```sh
VCPKG_ROOT=$HOME/vcpkg cmake --workflow --preset debug
```

Presets: `debug`, `release` (+LTO), `asan`, `tsan`. The build is clean under
`-Wall -Wextra -Wpedantic` with warnings-as-errors enabled in the workflow.