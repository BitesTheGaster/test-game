# Game Mechanics

This document describes exactly how the game plays: the formulas behind
defense, the shield, the level-up system (rerolls, unique items, milestones,
evolutions), elite/champion enemies and their traits, and the spawn/difficulty
curves. Tables of all content are generated into
[`content.md`](content.md) from `assets/data/*.toml` by
`tools/gendocs.py`.

## Run flow

- A run starts at level 1 with **no weapon equipped**. The first frame opens a
  **starting-weapon pick** offering 3 cards drawn from the `starter = true`
  entries in `weapons.toml` (Arcane Wand, Throwing Dagger or Heavy Crossbow).
  Picking one equips it and starts the run **without** spending a level-up.
- Enemies spawn off-screen with a **telegraph** (see [Spawning](#spawning)),
  walk toward the player and deal contact damage.
- Every weapon fires **automatically** at the nearest enemy. All owned weapons
  share the same target so the whole arsenal focuses the biggest threat.
- Killing enemies drops XP gems. Leveling up opens a card choice (see
  [Level-ups](#level-ups)).
- The three active abilities (`J`/`K`/`L`, see
  [Active abilities](#active-abilities-j--k--l)) are live from this first
  frame — no unlock, no card, only a cooldown.
- **Esc** pauses the run and overlays a **character sheet**: level/XP, time,
  kills, HP/regen, shield/defense, damage/cooldown, speed/pickup/lifesteal,
  the `J`/`K`/`L` ability row and the full list of owned weapons with their
  numbers. Press **B** while paused to toggle the **bestiary** — every enemy
  type you have slain, with its base and current stats, its in-world
  appearance, its kill count and badges for the elite/champion/overlord
  variants you have beaten.
- Dying shows the game-over screen; **R** restarts (deterministic seed 1337).

## The main menu

Six rows: **START RUN**, **SKIN**, **OUTLINE**, **MANUAL**, **RESET PROGRESS**,
**QUIT**. The row indices are named constants (`kMenuStart` … `kMenuQuit`) with
a `static_assert` tying them to the render row table, so a row cannot be inserted
without the code noticing.

- **SKIN** / **OUTLINE** cycle with ←/→ or on Enter, skipping outlines the profile
  has not unlocked. Both mark the profile dirty so `main()` persists them.
- **MANUAL** opens the in-game manual (see below). **F1** does the same, from the
  menu, from a live run and from the pause screen alike.

### In-game manual (F1)

`assets/data/manual.toml` is the same documentation this repo ships in `docs/`,
trimmed to what the 5×7 bitmap font can draw (ASCII 32–96 only: no lowercase, no
accents, no box drawing) and split into one-`[[page]]`-per-screen chunks. It is
loaded into `Content::manual` alongside the weapons and enemies.

- The manual is a **topmost modal overlay**. It is checked before the menu in
  `advance()`, so it eats the frame the simulation would otherwise get, and it is
  drawn **last and opaque** in `render()`, so it covers the HUD, the pause sheet,
  the bestiary and the sandbox alike.
- **Missing file is tolerated** (a stripped distribution must still start, and the
  menu row reads `< no manual in build >`); a **malformed file throws**, because
  a typo in the manual is a content bug worth failing loudly on.
- `loadContent()` **rejects any character the font cannot draw**
  (`core::render::fontSupports`), so a stray em dash fails the content load
  instead of rendering as a screen full of question marks.
- Line markup: a leading `>` is stripped and drawn as a highlighted bullet, a
  leading `#` as a sub-heading, two leading spaces as an indent, and `""` is a
  spacer. The prefixes only ever make a line *shorter* on screen.
- Arrows and **1**–**5** flip pages, both wrapping. A test asserts every shipped
  page fits one screen at the reference 720 px height, so a page can never grow
  into a "...MORE, NEXT PAGE" marker that hides half a topic with no way to see
  it.

### Reset progress

The only thing in the game that deletes earned progress, so it is **two-step**:

1. **Enter** on RESET PROGRESS calls `beginProgressReset()` — it *arms* the
   wipe and repaints the row (`RESET? PRESS AGAIN`, `ENTER = WIPE ALL`, plus a
   one-line warning). Nothing is deleted.
2. **Enter** again calls `confirmProgressReset()`: `*profile_ = Profile{}`,
   `syncedUnlocks_ = 0`, `profileDirty_ = true`, `applyProfileToPlayer()`.

Any menu navigation (↑ ↓ ← →) **disarms** it, so a wipe cannot be armed on one
row and fired from another. Without an attached profile the row is inert and
cannot be armed at all. `syncedUnlocks_` is cleared on the wipe because the Game
remembers which unlocks it has already pushed into the profile; leaving that
memory behind would let the next elite kill re-push a bit the player no longer
has and hand the outline straight back.

## The player

| Stat | Meaning |
|------|---------|
| Max HP | Starts at 100, growable via `max_hp_add` (also heals by the same amount) |
| Regen | Flat HP per second (`regen_add`) |
| Defense | One stat that mitigates all damage (see below) |
| Shield | Absorbs damage point-for-point before HP; the pool **and** its refill rate/delay are both cards |
| Lifesteal | Per-**kill** chance to heal; see [Lifesteal & healing](#lifesteal--healing) |
| Heal (H) | Guaranteed 50% max-HP heal on a 30 s cooldown |
| Weapon slots | 4 to start, 8 fully stacked; see [Weapon slots](#weapon-slots) |
| Move speed | Base × `speedMul` |
| Damage / cooldown | Global multipliers applying to every weapon |
| Projectiles / pierce | Additive buffs applied to every weapon |
| **Kill chain** | The one stat that cannot be hoarded; see [Momentum](#momentum) |

**Cross-weapon couplings** — stats are not siloed per weapon:

| Stat | Also affects |
|------|--------------|
| Fire rate | **Dagger orbit spin** — blades rotate at `orbitSpeed · max(0.5, 1 + fireRateBonus)` |
| Projectiles | **Cone range** (+25%/proj), **Bomb arc height** (+25%), **Beam count & width** (each projectile is a parallel beam; width +5%/proj), **Halo spokes** (+1 spoke), **Scythe arc radius** (+15%), **Nova radius** (+10%), **Zone radius** (+20%) |
| Pierce | **Bomb blast radius** (+15%), **Bounce bounces**, **Chain jumps** (one more each), **Zone DPS** (+2) |
| Knockback | **Impact** cards multiply every knockback the player deals — bombs, scythe/inferno bursts, Radiant Halo spokes and Repulsion Field retaliation |

### Fire rate (additive, never zero)

Attack speed is an **additive bonus**, not a multiplier on the cooldown:
every `fire_rate` upgrade adds a positive percentage and the final delay
between shots is

```text
final delay = weapon delay / (1 + total fire-rate bonus)   # floor 0.05 s
```

So stacking can approach but never reach zero cooldown: +25% fire rate fires
20% more often (`1/1.25`), +100% fires twice as often (`1/2.0`). Global
`fire_rate` (player level-ups, Frenzy/Perfection milestones) and per-weapon
`w_fire_rate` (Wand Channeling, Shuriken Cyclone) bonuses all add into the
same `1 + bonus` denominator. The Spreadshot unique doubles its bonus the same
way (+100% fire rate).

### Momentum

Everything else on this page is **flat**: a card you take at level 12 is worth
exactly as much at level 40, so a finished build has nothing left to do but
walk. Momentum is the one thing that cannot be banked.

- **Every kill adds a stack** to the chain (`momentumGain`, 1 by default).
- Each stack is worth **+1.5% damage** and **+0.5% fire rate**, up to a cap of
  **20** stacks — a live ceiling of **+30% damage / +10% fire rate** that has to
  be re-earned every single time.
- The chain goes **cold 3 seconds after your last kill**. A bar under the kill
  counter shows exactly how much patience is left.
- **A real hit costs you the chain**: it is halved, minus two, floored at zero.
  Damage-over-time and auras deliberately do *not* break it, so an enemy damage
  aura is not an instant chain-killer.
- The multipliers are applied at runtime to every damage, cooldown and movement
  calculation (they are *not* folded into the flat stats, so the character
  sheet still shows the build you actually own next to the live chain).

The point is a decision, not a bonus: standing *in* the horde is the only way to
keep the chain fed and also the only way to lose it. Seven cards bend it, and
between them they cover **every** axis the meter has:

| Card | Kind | What it moves |
| --- | --- | --- |
| **Blood Surge** | normal, 2 | +1% damage per stack |
| **Rampage** | normal, 1 | +4% move speed per stack |
| **Deep Reserves** | normal, 1 | +2 s of patience |
| **Kill Tempo** | normal, 3 | +0.5% fire rate per stack |
| **Long Tail** | normal, 2 | +6 stacks that count toward the cap |
| **Cull** | normal, 2 | +0.5 stacks per kill |
| **Bloodthirst** | unique | twice the stacks per kill, double the cap, +3 s of patience |

**Kill Tempo** is the one that was missing for longest: `momentumRate` was a live
simulation field from the first commit and no card could ever raise it, so a
chain build was structurally forced to be a damage build no matter how it was
assembled. **Long Tail** and **Cull** are the other two gaps — the cap and the
fill rate were reachable only through a unique, which meant a chain build either
had to be lucky or had to settle for the 20-stack default.

### Defense

Damage mitigation is a single curve with two stacked components:

```text
flat    = floor(defense / 5)          # every 5 defense eats 1 point
percent = 0.5 * defense / (defense + 150)
damage  = max(0, (raw − flat) * (1 − percent))
```

So defense always helps, scales smoothly, and can never block more than 50%
+ the flat component. At 150 defense: 30 flat + 25% percent ≈ 47.5% total on
large hits.

### Shield

A shield is a **pool and a clock**, and both halves are now cards. Until this
round only the pool was: `shieldMax` grew while the refill stayed hard-coded at
10 HP/s after a hard-coded 4 s wait, so a burst build simply re-took the same
chunk of HP five seconds later and the shield cards read as dead picks.

- `shieldMax` is raised by `shield_add` upgrades (normal, uniques and
  milestones all use it).
- Damage is absorbed by the shield **after** defense mitigation,
  point-for-point; HP only drops once the shield is empty.
- The shield **regenerates at `kShieldRegenRate` (10 HP/s) × `shieldRegenMul`
  after `shieldRegenDelay()` seconds without damage**. Taking any damage resets
  the delay. Both numbers are on the character sheet.
- `shield_regen` cards (**Aegis Flow**, 3 stacks, +60% each) raise the rate and
  also top the pool up, so the card feels immediate.
- `shield_delay` cards (**Quickdraw**, 2 stacks, −1 s each) shorten the wait.
- The delay is floored at **0.5 s**, so no build can make the pool permanent.
- Picking a `shield_add` card refills the shield to its new max.
- The current shield is drawn as a bar under the player's HP bar.

### Lifesteal & healing

- **H** triggers a **guaranteed heal for 50% of max HP** on a 30 s cooldown
  (shown next to the HP bar as `[H] HEAL READY` / `[H] HEAL nS`). This replaces
  the old single-use heal cards, which are no longer in the loot pool.
- **Lifesteal is kill-based**: every enemy **you kill** has an `L%` chance to
  heal **1 HP** (capped at max HP). At `L ≥ 100` the first point is guaranteed
  and the excess `(L − 100)%` rolls for a **second** point — so 150 lifesteal
  means 100% for 1 HP and 50% for another. It deliberately does *not* roll on
  ordinary hits: at high fire rates per-hit vampirism healed faster than any
  enemy could die, which made the stat a flat auto-heal instead of a reward for
  finishing things off.
- The raw numbers are **deliberately small** — `+4`, `+6` and `+8` per card
  rather than the `+8` / `+12` / `+16` they used to be. Lifesteal is a nice
  bonus for a kill-heavy build, not a replacement for avoiding damage.
- **Enemy lifesteal resistance** scales the chance down: the proc chance is
  multiplied by `1 − resistance`. Resistance grows with run time, is higher for
  tougher tiers, and the `resistant` trait adds a large chunk (a 50% resistance
  halves the chance).
- The **Vampiric Heart** unique makes every lifesteal proc heal **2 HP**
  instead of 1. This is the one card left at full strength — it scales the
  *flat* heal rather than the chance, and halving it would just duplicate the
  default behaviour.
- Regen adds flat HP every fixed tick (0.0556 HP/s per point).
- **Field Kit** (`heal_pct`, 2 stacks) heals **40% of your *current* max HP** the
  moment you take it. It resolves against the live pool rather than freezing the
  value at pickup time, so it still composes with a later max-HP card. This is
  the only card that heals, and it exists because `heal` was a fully implemented
  effect that no card in the game could reach.

## Weapons

- **4 weapon slots** to begin with, **8** at full stack. Each slot runs its own
  cooldown, damage, projectile count/speed/life/pierce/spread and projectile
  tint.
- Every weapon fires `projectiles` bolts in a fan of `spread` radians around
  the aim direction. The fan angle caps at a reasonable maximum so extra
  spread never wraps around backward.
- A weapon is never offered twice; owned weapons are removed from the grant
  pool.
- Each weapon has a distinct **role**: some are rapid spray (**Ember Sprayer**
  rakes a visible flame fan in an arc, dealing its damage instantly), some are
  piercing snipers (**Heavy Crossbow** bolts home and punch through crowds —
  a slow, heavy 2.2 s reload after the balance pass — and the **Solar Lance**
  is an instant hitscan line, 130 damage on a 1.6 s reload), some are
  area-splash (**Runic Hammer** lobs an arcing bomb that detonates on contact,
  destroying itself), some bounce (orb), and some home toward the target
  (crossbow, shuriken, nova). Check `content.md` for the per-weapon traits
  column.
- The **Solar Lance** fires a wide hitscan line across the whole arena and
  **fans** with your projectile count — up to four symmetric beams. The
  **Prism Lance** unique is not "+2 projectiles": it fires **exactly three**
  beams spread 0.30 rad apart, i.e. **forward, left and right**, so it covers a
  cone instead of stacking three lines on the same aim.
- Two melee weapons, two different shapes:
  - The **Soul Scythe** reaps a **full 360° circle around its nearest enemy**:
    the swing is centered on the target itself, so everything around that
    target takes the hit. A target-centered ring — visually and mechanically a
    world apart from the flame's forward-pointing cone.
  - The **Barbed Whip** (and its evolution, the **Tidal Lash**) instead lashes
    the arc **in front of the player**: the swing is centered 1.8–2.2 units
    along the aim line and only covers `sweep_angle` of it, so it hits what
    you are facing whether or not anything is standing there — and it still
    swings at empty air. A whip is the answer to a horde that is already on
    top of you; a scythe is the answer to one that is standing around a brute.
- The **Grave Bell** plants a **beacon behind your target** and does no damage
  on impact at all. Its whole value is what it does afterwards: everything
  inside `lure_reach` is dragged toward the core every tick (easing off at the
  rim, so the horde is coaxed in rather than yanked) and only takes damage once
  it has been hauled inside `lure_radius`. It is a turret that plays on the
  *other* side of the crowd, not a projectile aimed at the player's feet. At
  most `lure_max_beacons` bells live at once; the oldest one gives way.
- A **fused** bomb (`bomb_fuse > 0`) does **not** detonate on contact. The
  **Siege Mortar** therefore arcs clean over the front rank and cooks where the
  parabola brings it back down — a ranged weapon wearing a hammer's clothes is
  not a mortar, and the fuse is the whole difference.
- The **Void Orb** is **one eternal projectile**: it never expires and hunts
  forever, steering between enemies (or back to you when the arena is empty).
  Projectile upgrades do **not** spawn more orbs — they make the single orb
  **grow** (bigger contact radius). With no damage decay per bounce, every
  hit deals full damage.
- The **Throwing Dagger** blades live *on* their orbit circle, so anything
  hugging the player used to sit in a blind spot and take literally nothing
  while the ring spun overhead. The spin now also grinds through the interior:
  once per step, every enemy strictly inside the blade band takes `0.35×` one
  blade's contact damage (with the usual AoE falloff). N blades still means N×
  the contact damage — the whirl is once per build, not once per blade.
- Weapon fields `area` (splash radius), `strength` (knockback), `homing` and
  `bounces` are honored: area deals half-damage in a radius on hit, strength
  pushes the struck enemy, homing steers the projectile toward the nearest
  foe each tick, and bounces reflect the projectile off the hit surface.

### AoE damage falloff

Any area-of-effect hit (cone, scythe reap, whip lash, inferno, bomb blast, zone
tick, nova tick, beam row, boomerang trail/blast, orb splash, lure core,
Overload) deals **slightly less damage to each target the more targets it
catches at once**:

```text
multiplier = 0.9^(n − 1)   # n = enemies hit, clamped to 1..40
```

So one enemy takes 100%, two take ~90% each, three ~81% each, and so on. The
count is gathered **before** damage is applied so every target in the same
blast receives the same multiplier. Single-target hits (chain-lightning jumps,
direct projectile impacts) are unaffected.

**Pierce reduces this falloff.** The multiplier blends toward `1.0` by
`clamp(pierce / 20, 0, 1)`, so at **20+ pierce** an AoE hit deals full damage
to every target it catches. Every area source passes its own pierce (weapon
pierce plus the global pierce buff) — cones, scythe reaps, whip lashes,
infernos, bombs, zones, novas, beams, boomerang trails/blasts, orb splashes,
lure cores and the Overload.

### Evolutions & super evolutions (A + B = C)

The roster is **32 weapons**: **18** that can be rolled as ordinary weapon
cards, **10** two-ingredient evolutions and **4** three-ingredient supers.
Weapons can require other weapons. When you own **all** prerequisites, the
result becomes the **highest-priority** weapon offer, replacing the random
weapon grant:

- **Two-ingredient evolutions** (tag `EVOLUTION! (A+B)`): `storm` (Arcane Wand
  + Heavy Crossbow), `nova` (Void Orb + Runic Hammer), `inferno` (Ember Sprayer
  + Soul Scythe), `pulsar` (Solar Lance + Storm Shuriken), `halo` (**Radiant
  Halo**: Throwing Dagger + Solar Lance), `blizzard` (**Blizzard Rail**: Rail
  Rifle + Frost Shards), `siege` (**Ashfall**: Siege Mortar + Grave Bell),
  `chaos` (**Chaos Sphere**: Pinball Puck + Void Orb), `sunder` (**Sundering
  Core**: Shock Core + Soul Scythe) and `tidewhip` (**Tidal Lash**: Barbed Whip
  + Storm Shuriken). Radiant Halo spawns **persistent beams that orbit you**,
  reaping everything they sweep through — the beam's damage re-expressed as an
  always-on ring rather than a brief flash. `chaos` is deliberately *not* the
  Void Orb's eternal bounce: it burns out after ~14 ricochets, which is what
  makes "one more bounce" a real decision.
- **Three-ingredient super evolutions** (tag `SUPER EVOLUTION! (A+B+C)`):
  `vortex` (**Void Gyre**: Throwing Dagger + Soul Scythe + Void Orb),
  `prism` (**Prism Array**: Ember Sprayer + Solar Lance + Heavy Crossbow),
  `seraph` (**Seraph Array**: Jackhammer Drill + Heavy Crossbow + Barbed Whip)
  and `eventhorizon` (**Event Horizon**: Rail Rifle + Shock Core + Ember
  Sprayer). None is a re-skin of an existing evolution — each introduces a
  mechanic nothing else in the pool has:
  - **Void Gyre** spawns **suction zones** that circle the player. Anything
    caught in a zone's outer reach is **dragged inward** toward its core every
    tick, and only takes damage once it has been hauled inside. Both the
    **number** and the **size** of the zones scale with your projectile count:
    every `+1 projectile` adds a whole new zone *and* fattens the existing ones,
    so a late build is a corkscrew of overlapping crushers rather than more
    circles.
  - **Prism Array** does not fire one beam at one target. It locks a
    **separate beam onto each of the N nearest enemies** (up to 6), so a crowd
    is chewed from several angles at once and one big brute no longer eats
    every shot. Each locked beam then **ricochets**: on hitting its victim it
    jumps to the nearest enemy it has not already hit within
    `prism_ricochet`, up to **pierce** reflections. Pierce *is* the reflection
    budget, so a pierce card visibly turns one straight beam into a zig-zag
    that walks down a line of enemies — which is the point, since the report
    was that ricochets needed to actually be driven by pierce.
  - **Seraph Array** is the halo taken to weapon-grade numbers: wide, slow
    wings that cut and knock everything they cross.
  - **Event Horizon** is four independent gravity wells, each with its own
    core, orbiting fast enough to corkscrew a crowd.
- Evolutions and supers are **not** added to the normal weapon pool before the
  prerequisites are met.
- Picking one grants the evolved weapon **in addition to** the ingredients'
  slot usage (it occupies one of the arsenal's slots; you never lose the
  ingredients).

## Weapon slots

The arsenal starts at **4** weapon slots. That is a hard cap for most of a run:
the level-up offer stops appearing for weapons once you are full.

Two cards get past it, both using the same `weapon_slot_add` effect:

- **Arsenal Core** (`+1 weapon slot`, **normal, 3 stacks**) — the grind.
- **Hollow Chamber** (`+1 weapon slot`, **unique, 1 stack**) — the one-shot that
  hands you the last slot.

A fully-stacked build holds **8** weapons, which is `Game::kMaxWeapons` and the
size of the `weapons_` storage array. `weaponSlots` is clamped to
`kMaxSlotCards` at the point the effect is applied, and `weaponCap()` clamps
again at the point it is read, so a hand-edited `upgrades.toml` that granted 40
slots still cannot write past the array. `addWeapon()` additionally refuses past
`kMaxWeapons` outright — the array bound is a memory-safety check, not a balance
question.

## Level-ups

XP per level: `12 + 9·(level−1) + 2.6·(level−1)·level`. Overflow XP carries into
the next level; if it purchases another level immediately, another card choice
queues. Picked-up XP is scaled by the **XP multiplier** (`xpMul`): Scholar and
Lorekeeper cards add `+12%` and `+30%` respectively, stacking additively.

**This curve is the pacing of the game.** Level-ups are picks, and picks are the
only thing that makes a run interesting, so it is deliberately steep: level 32
costs ~33 000 XP and level 64 ~246 000. With the old, much cheaper curve a build
was maxed out about four minutes in and everything after that was an empty walk.

### Choice generation

Each level-up shows **3 cards, plus** one extra per `extraChoice`
(Gambler's Eye). The cards are filled in this order:

1. **Weapon cards** — one level-up may roll new weapons. The chance is
   `(1 − weaponCount/5) · 0.32` (rarer than before). When it fires you are
   offered a **choice of 2–3 weapon cards at once** (2, or 3 half the time),
   clamped to the free slots and to the number of available weapons. When an
   evolution's prerequisites are owned, that evolution (or super evolution) is
   always the first card offered.
2. **Unique card** — if no weapon card was rolled, a **45%** chance to offer
   a random unpicked unique item.
3. **Normal pool** — all remaining slots are filled from non-consumed normal
   upgrades. Weapon-tagged cards (`weapon = "wand"` etc.) only appear while
   that weapon is owned.
4. **Fallback** — if the pool is exhausted the game never bricks: the first
   still-**applicable** upgrade is offered (weapon-tagged cards for weapons the
   player does not own are skipped, since picking one could not do anything).
   If literally nothing is left, a **"nothing left, continue"** card completes
   the level-up. A pick that cannot be applied is never allowed to eat the
   level-up — that used to leave the run stuck on the card screen forever.

### Reroll

- Each level-up grants **1 free reroll** by default, plus one more per
  `rerollCharges` (Second Chance → 2 total).
- Rerolling rebuilds the whole choice set from scratch (including a fresh
  weapon/unique roll).
- **Keys:** `1`–`5` pick the matching card. **`R` always rerolls** the
  level-up choice while free rerolls remain; it does nothing once the budget
  is spent.
- The reroll hint is shown under the cards and reports how many rerolls are
  left this level (`[R] REROLL xN`, or `NONE LEFT`).

### Unique items (one-time, rule-changing)

Uniques are violet cards, offered at ~45% per level-up, and each can be taken
once. See [`content.md`](content.md) for the exact list; the rules they bend:

| Unique | Changes the rules |
|--------|-------------------|
| Spreadshot | Doubles the volley spread and fire rate, but every shot sprays ±30° off aim |
| Ignited Carapace | Getting hit detonates a burst dealing **3× incoming damage** to enemies nearby |
| Gambler's Eye | +1 card in every future level-up |
| Second Chance | +1 free reroll per level-up (total 2) |
| Adrenaline | Below 30% HP: +60% move speed and 1s invulnerability every 20s |
| Singularity | Every 12s yanks nearby enemies toward you |
| Storm Bolt | Every 3rd projectile hit chains lightning to 3 nearby enemies |
| Blood Price | Every 20 kills detonates a burst around you |
| Cold Blood | Enemies that hit you are slowed for 2s (45% speed) |
| Vampiric Heart | Lifesteal procs heal 2 HP instead of 1 |
| Last Stand | Taking a hit below 20% HP grants 1s of invulnerability (20s cooldown) |
| Repulsion Field | Enemies that strike you are violently knocked away |
| Combat Reflexes | All three abilities (`J`/`K`/`L`) recharge 25% faster (3 stacks, floor 0.35×) |
| Heavy Hands | Overload: a wider blast that hits 30 harder and shoves 3 further |
| Phase Memory | Phase Dash: +1.2 distance and +0.2 s of invulnerability on arrival |
| Deep Freeze | Stasis: +1 s of duration, and the slowed world drops another 0.08× |
| Cascade | Every ability also fires a 40% Overload at the same spot |

**Weapon uniques** — **every one of the 32 weapons** has an exclusive treasure
card, offered only while that weapon is equipped (the card carries
`weapon = "<id>"`), so the pool stays relevant to your loadout:

| Weapon | Unique | Effect |
|--------|--------|--------|
| Arcane Wand | Seeking Missiles | Bolts home onto the nearest enemy |
| Throwing Dagger | Blade Vortex | Blades spin 2× faster in a 25% wider orbit |
| Heavy Crossbow | Fragmenting Bolt | Bolts explode on impact for area damage |
| Ember Sprayer | Hearthfire | Cone is 50% wider and 40% longer |
| Runic Hammer | Cataclysm | Explosions 60% larger with heavier knockback |
| Storm Shuriken | Return Tempest | Returning blades detonate a burst |
| Void Orb | Echo Detonation | Every bounce splashes half damage around the hit |
| Soul Scythe | Reaper's Harvest | Sweeps restore 3 HP per kill |
| Solar Lance | Prism Lance | Fires **3 beams at once — forward, left and right** |
| Storm Caller | Thunderlord | +4 chain jumps and no damage decay |
| Void Nova | Supernova | Ring expands faster, wider, and hits harder |
| Inferno | Everflame | Wider reap; burning ground lasts longer and burns harder |
| Pulsar | Arc Saw | The laser trail is 80% wider and deals 35% more damage |
| Rail Rifle | Magnetic Slug | The slug homes onto the nearest enemy |
| Grave Bell | Deep Toll | 35% harder pull, 20% wider reach, one more bell at a time |
| Tesla Coil | Gravitic Field | Jumps reach 50% further and stop decaying so hard |
| Blizzard Rail | White Squall | +4 jumps and no damage decay |
| Ashfall | Molten Crater | The burning ground is 80% hotter, 25% wider and lasts much longer |
| Chaos Sphere | Detonation Chain | Every bounce splashes area damage around the hit |
| Sundering Core | Event Collapse | Ring expands faster, wider, and hits harder |
| Tidal Lash | Undertow | A 35% wider lash that flings 40% harder and reaches further |
| **Radiant Halo** | Corona Mantle | Beams shove for 6, 40% wider, 15% longer, +20% damage |
| **Void Gyre** | Black Gyre | 45% harder pull, 30% further reach, fatter core, denser ticks |
| **Prism Array** | Total Internal Reflection | One more independent beam, 40% longer ricochet, +15% range |
| **Frost Shards** | Rime Lances | +4 pierce and 40% longer flight, at 80% damage each |
| **Siege Mortar** | Siege Doctrine | A 3-shell salvo on a double fuse, 35% wider blasts, 20% slower |
| **Pinball Puck** | Silver Skewer | +10 bounces, no damage decay, 30% longer reach per hop |
| **Jackhammer Drill** | Overdrive Bore | 60% wider bite, 40% longer reach, strikes far faster |
| **Shock Core** | Standing Discharge | The ring lingers, expands faster and re-strikes twice as fast |
| **Barbed Whip** | Barbed Chain | The lash goes all the way around, 30% further, 50% harder shove |
| **Seraph Array** | Wingbeat | The wings spin 60% faster, shove for 5 and burn 30% wider |
| **Event Horizon** | Singularity | 50% harder pull, fatter core, further reach, denser ticks |

The bolded rows are new. Eleven weapons used to ship with **no** card at all —
Radiant Halo, Void Gyre, Prism Array, Frost Shards, Siege Mortar, Pinball Puck,
Jackhammer Drill, Shock Core, Barbed Whip, Seraph Array and Event Horizon — which
meant picking one up froze its entire stat block: every card that could improve
it was a global stat that hit all eight weapons at once, so there was never a
reason to draw it again. Each new unique edits the parameter that weapon's
identity is *made of* (the halo's knockback, the gyre's pull, the prism's beam
count) rather than a flat damage number, because a +20% damage unique is the
same card 32 times over.

Every one of these is now covered by a test that walks the whole roster and
fails if any weapon is missing its unique — the rule is enforced by CI, not by
good intentions.

### Focus cards for thin weapons

An exclusive unique is a single pick. Once it is taken, a weapon whose only card
was that unique has no reason to appear in the level-up pool again, so eleven
weapons also ship a **normal, stackable** card aimed at an identity parameter
that no card had ever touched:

| Weapon | Card | What it moves |
|--------|------|---------------|
| Void Orb | Eventide Hunger (2) | 30% longer reach per hop, and the per-bounce decay floors at 0.9× |
| Soul Scythe | Long Arm (3) | +6% reap radius |
| Solar Lance | Focused Burn (3) | +6% range, +4% width |
| Storm Caller | Arc Cascade (3) | +6% jump range, +5% damage per link |
| Void Nova | Rupture (3) | +6% ring radius, +4% expansion speed |
| Inferno | Pyre Spread (3) | +6% reap radius, +0.5 s of burning ground |
| Pulsar | Long Cast (3) | +7% flight range, +6% return speed |
| Ember Sprayer | Pooled Ash (2) | One more burning pool at a time |
| Inferno | Ashfall Spread (2) | One more burning pool at a time |
| Runic Hammer | Concussion Charge (3) | +8% blast radius, +6% knockback |
| Siege Mortar | Wide Shell (3) | +8% blast radius, +6% knockback |
| Grave Bell | Echo Anchor (2) | +0.8 s of beacon, one more bell at a time |
| Ashfall | Siege Chime (2) | +0.8 s of beacon, one more bell at a time |
| Radiant Halo | Long Wings (3) | +8% beam reach, +1.5 shove per beam |
| Void Gyre | Denser Gyre (3) | Fatter core, wider orbit, 12% faster spin |
| Prism Array | Beam Lattice (2) | One more independent beam, +5% range |

**Beam Lattice** is the interesting one. The prism's beam count lives in
`prismMaxTargets`, a field of its own, so the global `proj_add` card that
grows the halo and the gyre **never touched it**. A card that silently does
nothing on the weapon it is scoped to is worse than no card at all, so the
weapon-scoped card test arms every weapon in turn, takes every card scoped to
it, and requires the weapon's own numbers to actually move.

### Milestones

Every **power-of-two level from 4 on** (4, 8, 16, 32, 64, 128, …) replaces
the normal choice with a **violet milestone screen**: the pool contains only
the 3 milestone cards for that exact level and you pick **2 of 3**. All
milestones are strong and live **outside** the normal pool (they can never
appear as ordinary card offers). Reaching a milestone level with no milestone
content for it behaves like a normal level-up.

### Weapon test mode

Press **T** during a live run to open the weapon sandbox. The sandbox is a
**hermetic** sandbox: it snapshots the entire run — weapons, stats, item
stacks, XP, level, kills, bestiary entries, HP, shield, iframes and the
difficulty clock — and rolls **all** of it back when you leave. Nothing you do
inside can leak out, so you cannot farm XP, unlock an outline, or buff your
real build by testing.

| Key | Action |
|-----|--------|
| `T` | Toggle test mode on/off (also works from a level-up screen) |
| `1` / `2` | Previous / next weapon — **all 32 including evolutions and supers** (`storm`, `nova`, `inferno`, `pulsar`, `halo`, `blizzard`, `siege`, `chaos`, `sunder`, `tidewhip`, `vortex`, `prism`, `seraph`, `eventhorizon`) |
| `3` | Apply a **max build** boost: +100% damage, +4 projectiles, +3 pierce, +80% fire rate. Toggling it off keeps your item picks — it only undoes the boost |
| `4` | Toggle enemy waves (fodder bats spawn so every weapon has a target) |
| `5` | Exit back to the untouched run |
| `E` | Open/close the **item picker** |
| `I` | Toggle **immortality** (all incoming damage ignored, so you can stand in a horde) |
| `F` | Cycle the **difficulty clock**: 1× → 4× → 10× → 20× |
| `X` | **Kill the player** on demand (works through immortality — that is the point of a death switch) |
| `R` | **Max every item** instantly (milestones excluded) |

In the item picker, `↑`/`↓` (or `←`/`→`, or `W`/`S`) move the highlight and
`Enter` takes the highlighted card — one stack per press, up to the card's own
cap. The list shows each card's current stacks and flags cards that are
`[MAX]` or `[NOT ARMED]`. A card for a weapon you are **not** holding cannot be
applied at all, so "max every item" fills the weapon-agnostic cards and the
cards of whatever you happen to be holding, and stops there.

Other sandbox rules worth knowing:

- **The sandbox is a dead end, and that is the point.** It pays **no XP**, it
  never pays a **skin or outline** unlock, and **leaving it ends the run**: the
  snapshot is restored first (so the death report shows the real run you
  actually played), and *then* the player is dropped to 0 HP on the normal
  game-over screen. A sandbox that could be opened, farmed and closed cleanly
  was a cheat, not a test rig.
- **Level-ups work** in the sandbox (that is how the card flow gets exercised)
  but only **item** cards are offered — never weapons, because testing *one*
  weapon is the entire point. Since no XP is paid, you never level up there at
  all and so can never reroll; `R` is the "max every item" cheat instead.
- Switching weapons re-seeds a herd of weak test bats.
- Leaving the sandbox removes the sandbox's injected fodder (enemies that were
  already on the field are kept) and puts you back exactly where you stood —
  before, as noted, the run ends.
- `X` inside the sandbox drops you on the normal game-over screen; `R` there
  restarts the run as usual.

## Active abilities (J / K / L)

Three abilities, always live from the first second of a run, with no menu, no
unlock and no card. Only a **cooldown** stands in the way, so a build changes how
they feel but never whether they exist. The HUD shows all three with their
remaining cooldown, and so does the character sheet.

| Key | Ability | Cooldown | What it does |
|-----|---------|----------|--------------|
| `J` | **Phase Dash** | 5 s | Teleport `blinkDist` (3.2) units along your movement — or *at* the nearest enemy if you are standing still, because a dash that only works while a movement key is held is not an answer to being cornered. Grants `blinkIframes` (0.35 s) of invulnerability on arrival, so it is a real dodge. |
| `K` | **Overload** | 14 s | A radial burst: `burstDamage` (45) to everything within `burstRadius` (3.0), knocked `burstKnockback` (8) units away, using the same crowd falloff as every other blast. |
| `L` | **Stasis** | 30 s | For `stasisDuration` (2.5 s) the world runs at `stasisSlow` (0.35×): enemies and their projectiles tick on a scaled clock, **you do not**. The player is not slowed by their own stasis. |

Notes:

- Stasis scales the *hostile* side of the world only. `updateEnemies` and
  `updateEnemyShots` multiply their local `dt` by the world scale; the player,
  the weapons and the ability cooldowns run at full speed, and `simTime_` keeps
  counting real seconds so the difficulty ramp is not stretched by it.
- Cooldowns are multiplied by `abilityCdMul`, which cards only ever push
  **down** (floor **0.35×**). `abilityCdMul` never reaches zero, so no build
  gets to spam all three keys at once.
- **Eight** ability cards retune them (never unlock them). Five are one-shot
  uniques you have to be lucky enough to roll: **Combat Reflexes** (`-25%`
  cooldown, 3 stacks), **Heavy Hands** (a bigger, harder Overload), **Phase
  Memory** (a longer dash and a longer mercy window), **Deep Freeze** (longer,
  deeper Stasis) and **Cascade** (every ability also fires a 40% Overload at the
  same spot).
- The other three are **normal, stackable** cards, one per button, so the J/K/L
  row is a real build axis instead of a novelty you hope to draw once:
  **Phase Mirror** (`ability_dash`, 3 stacks, +0.8 distance and +0.06 s of
  landing invulnerability), **Concussion Core** (`ability_burst`, 3 stacks, +0.8
  radius / +30 damage / +1 knockback) and **Cryostasis** (`ability_slow`, 3
  stacks, +1 s and another −0.08× on the world clock).
- Two of those are floored so a maxed build cannot delete the button's purpose:
  `blinkDist` caps at **9.0** (a dash that crosses the screen is a skip button,
  and the dash exists for positioning) and `stasisSlow` at **0.15** (the world
  never grinds to a halt outright).
- The sandbox snapshots the ability cooldowns and the stasis timer along with
  everything else, so testing a dash cannot leave the real run mid-cooldown.

## Enemies

All enemy stats live in `enemies.toml`: HP, speed, contact damage, radius,
XP, `unlock_at` (seconds into the run when the type enters the spawn pool),
relative `weight`, and `color`/`shape`.

### Spawning

- Spawns happen **11 world units away** from the player at run start (off-screen
  at typical zoom) at a random angle — enemies materialize outside the visible
  area, never on top of you. The distance grows **linearly to 2× (22 units) by
  the 10-minute mark**, then stays there for the rest of the run.
- Every spawn is preceded by a **0.6 s telegraph**: a pulsing ring at the
  spawn point (visible when on-screen), plus a small dot at the screen edge
  pointing at the spawn for off-screen spawns.
- Enemy type is picked by **weighted roll among unlocked types** (`simTime >=
  unlockAt`), so the roster rotates in over time.
- From ~45 s enemies arrive in **clusters** instead of one-at-a-time
  trickles, and packs grow over the run (up to ~8 enemies per pack).
- Every ~40–50 s a **horde burst** spawns a full ring of 10–24 enemies from
  every side at once, with telegraphs ringing the whole screen. The ring is
  called out with a `HORDE INCOMING` banner 2 s before it lands, so the player
  gets to decide whether to hold the line or disengage.

### Enemy separation

- A spatial hash separates enemies from each other **and from the player**.
  Enemies hold a small buffer around the player character so they hover just
  outside it instead of piling flush on top of it.
- After separation, enemy velocities are **clamped** (at most `max(speed·2.5,
  4.0)` units/s), so a dense huddle can never fling members at absurd speeds
  toward the player — no vacuum "suction" into the player.

### Difficulty ramp

Three phases on the spawn interval, tuned so the first half-minute is a
warm-up and difficulty bites hard from ~90 s:

```text
t < 30 s :  interval = 1.2 − t·0.005         (1.20 s → 1.05 s)
30–90 s  :  interval = max(0.50, 1.05 − (t−30)·0.0092)  (→ 0.50 s)
t ≥ 90 s :  interval = max(0.20, 0.50 − (t−90)·0.003)   (→ min 0.20 s)
```

The 0.20 s floor is deliberate: at 0.12 s the game threw around thirty enemies
a second at the player, which no build can answer and which just ends the run
early. Five packs a second keeps the screen full while still being something
you can fight your way out of.

- Enemy HP also scales globally. Up to the 6-minute mark it is multiplied by
  `1 + t/70`; **after 6 minutes the ramp steepens** (`+ (t−360)/45`), so the
  late game escalates faster. Capped at ×30.
- Enemy **move speed** drifts up over the run (`1 + t/600` → `×1.6` at 6 min),
  and **after 6 minutes it too accelerates** (`+ (t−360)/300`, capped at
  ×2.4) so late waves stay threatening even for a leveled arsenal.
- Enemy **contact damage** creeps up as `1 + t/1500`, so late hits land harder.
- Enemy **defense** grows over time (see below) so late enemies shrug off a
  slice of every hit.
- Up to **8000 enemies** can be alive at once; the cap protects the frame
  rate rather than throttling spawns.

### Elites, champions & overlords

Elite-and-above enemies are rolled per spawn from elapsed time `t`. Every one
of them gets **all** of its base stats boosted (so they never simply melt), on
top of the traits they roll:

| Roll | Becomes | Gate | HP | Touch | Speed | XP |
|------|---------|------|----|-------|-------|----|
| t ≥ 45 s | Elite (tier 1) | on the clock | ×4 | ×1.5 | ×1.15 | ×3 |
| t ≥ 90 s | Champion (tier 2) | **once elites are routine** | ×7 | ×2.5 | ×1.3 | ×5 |
| t ≥ 240 s | Overlord (tier 3) | **once champions are routine** | ×14 | ×4 | ×1.5 | ×10 |

Elites open on a timer (5% per spawn, creeping to 15%). **Champions and
overlords are not on a clock at all** — they answer to how well the player is
doing. See [Adaptive tribunal director](#adaptive-tribunal-director).

- Tougher tiers are **larger** (elite ×1.35, champion ×1.6, overlord ×2.0).
- Strength is shown by a **coloured outline** around the enemy — elite **gold**,
  champion **orange**, overlord **violet** — replacing the old glow and
  floating name tags. They always show an enlarged HP bar.
- **Trait count** grows with tier and time: an elite rolls **exactly one**
  trait, a champion **2** (+1 after 4 min), an overlord **4** (+1 after 8 min).
- Every elite-and-above also has **stronger defenses and resistances**: their
  defense, lifesteal resistance and knockback resistance all scale with tier
  (see below). The `resistant` trait pushes those even higher.

**Trait pool** (compiled in `game.cpp`, `PickTrait`): `fast, armored,
regenerating, explosive, venomous, vampiric, shielded, heavy, archer, aura,
resistant`. Traits never repeat on the same enemy:

| Trait | Effect |
|-------|--------|
| Fast | ×1.7 move speed |
| Armored | ×2.5 HP and ×1.3 touch damage |
| Regenerating | Regenerates 2% of max HP per second |
| Explosive | Counts down; detonates near the player |
| Venomous | Hitting the player applies 3 s poison (damage over time) |
| Vampiric | Heals 50% of its touch damage when it hits you |
| Shielded | Gains a shield equal to 50% of max HP (absorbs damage first) |
| Heavy | ×2 touch damage — a hard-hitting bruiser |
| Archer | Shoots aimed projectiles at the player periodically |
| Aura | Burns the player with a damage aura while they stand inside it |
| Resistant | Strong lifesteal + knockback resistance and extra defense |

### Adaptive tribunal director

The heavy tiers are gated on **performance, not time**. A per-tier *handling
score* rises by 1.0 per elite killed (1.25 per champion) and bleeds away at
`1/30` per second, so only recent form counts:

| Tier | Opens when | Score needed | Fed by |
|------|-----------|--------------|--------|
| Champion | elite pressure ≥ 7 **and** t ≥ 90 s | 7 | elite kills |
| Overlord | champion pressure ≥ 6 **and** t ≥ 240 s | 6 | champion kills |

- The **spawn chance** then scales with how far *above* the line the player is:
  `clamp((pressure − threshold) · 0.006, 0, 5%)` for champions and
  `· 0.003, 0, 2%` for overlords. A dominant build gets a real fight, a
  merely competent one gets an occasional champion.
- Gates are **hysteretic**: once earned a tier stays open for a 20 s grace
  window, so a dry patch of five seconds does not slam the door shut. After the
  grace the tier shuts again and the run gets easier — a struggling player is
  never buried.
- Opening a tier announces it with a **HUD banner** (`CHAMPION TRIBUNAL OPEN`),
  and the bestiary shows each gate's live progress as a percentage.
- The tiers' **power is unchanged**: only their timing follows the player.
- Elites stay on the clock, so a fresh run still meets its first one at 45 s.

### Enemy defense & resistances

Enemies run through the **same flat+percent defense curve as the player**
(`mitigateDamage`) and gain defense as the run goes on:

```text
defense   = max(0, t − 30) / 25 · tierMul     # tierMul: 1 / 1.4 / 2.0 / 2.8
lifestealRes = min(0.75, t/1200) + tierBonus + (resistant ? 0.5 : 0)
knockbackRes = min(0.70, t/900)  + tierBonus + (resistant ? 0.5 : 0)
```

- The 30-second grace period keeps the opening minute free of mitigation.
- **Knockback** from bombs, the scythe reap, inferno, Radiant Halo spokes
  and Repulsion Field retaliation is scaled by `1 − knockbackRes`, so
  late/elite enemies get pushed around less and less. Every shove is stored as
  a decaying impulse on the enemy (`kbX`/`kbY`, ×0.82 per tick) that is added
  on top of its steering velocity, so the push actually lands before the AI
  re-clamps its movement. **Impact** cards multiply the whole shove.
- **Lifesteal** proc chance is scaled by `1 − lifestealRes`; a 50% resistance
  halves the player's chance to heal.
- **Lethal hits always kill.** If a hit's raw damage already covers the
  target's remaining HP, it dies even when defense mitigation would leave a
  fraction behind — no more "0 HP" enemies that are technically still alive.
  Shields are exempt: the Shielded trait still absorbs its buffer first.

### Contact damage

Hits go through the standard mitigation/shield pipeline, then grant the
player **0.18 s** of contact invulnerability, extended by **~1% per 5 points
of defense** (`base × (1 + defense / 500)`). Tougher enemies survive hits
because the ramp multiplies their HP — nothing is one-shotted out of its
weight class. The **Last Stand** unique grants a full 1 s of iframes when a
hit lands while you are below 20% HP (20 s cooldown).

## Combat helpers

- **Thorns/Blood Price chain** bursts exclude the player entity from their
  area loops (no self-damage, no double counting).
- **Storm Bolt** chains on every 3rd projectile hit, bouncing to 3 nearest
  enemies.

## Death & restart

Death ends the run. `R` restarts with a deterministic seed (1337) so a ruined
run is reproducible. Restarting re-opens the starting-weapon pick.