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
- **Esc** pauses the run and overlays a **character sheet**: level/XP, time,
  kills, HP/regen, shield/defense, damage/cooldown, speed/pickup/lifesteal and
  the full list of owned weapons with their numbers. Press **B** while paused
  to toggle the **bestiary** — every enemy type you have slain, with its base
  and current stats, its in-world appearance, its kill count and badges for
  the elite/champion/overlord variants you have beaten.
- Dying shows the game-over screen; **R** restarts (deterministic seed 1337).

## The player

| Stat | Meaning |
|------|---------|
| Max HP | Starts at 100, growable via `max_hp_add` (also heals by the same amount) |
| Regen | Flat HP per second (`regen_add`) |
| Defense | One stat that mitigates all damage (see below) |
| Shield | Absorbs damage point-for-point before HP, regenerates out of combat |
| Lifesteal | Per-hit chance to heal; see [Lifesteal & healing](#lifesteal--healing) |
| Heal (H) | Guaranteed 50% max-HP heal on a 30 s cooldown |
| Move speed | Base × `speedMul` |
| Damage / cooldown | Global multipliers applying to every weapon |
| Projectiles / pierce | Additive buffs applied to every weapon |

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

- `shieldMax` is raised by `shield_add` upgrades (normal, uniques and
  milestones all use it).
- Damage is absorbed by the shield **after** defense mitigation,
  point-for-point; HP only drops once the shield is empty.
- The shield **regenerates at 10 HP/s after 4 seconds without taking damage**.
  Taking any damage resets the delay.
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

## Weapons

- **4 weapon slots** to begin with, **7** at full Arsenal Core stacks. Each slot
  runs its own cooldown, damage, projectile count/speed/life/pierce/spread and
  projectile tint.
- Every weapon fires `projectiles` bolts in a fan of `spread` radians around
  the aim direction. The fan angle caps at a reasonable maximum so extra
  spread never wraps around backward.
- A weapon is never offered twice; owned weapons are removed from the grant
  pool.
- Each weapon has a distinct **role**: some are rapid spray (**Ember Sprayer**
  rakes a visible flame fan in an arc, dealing its damage instantly), some are
  piercing snipers (**Heavy Crossbow** bolts home and punch through crowds —
  a slow, heavy 2.2 s reload after the balance pass — and the **Solar Lance**
  is an instant hitscan line), some are area-splash
  (**Runic Hammer** lobs an arcing bomb that detonates where it lands,
  destroying itself), some bounce (orb), and some home toward the target
  (crossbow, shuriken, nova). The **Solar Lance** fires a wide beam across the
  whole arena and splits into one parallel beam per projectile. Check
  `content.md` for the per-weapon traits column.
- The **Soul Scythe** reaps a **full 360° circle around its nearest enemy**:
  the swing is centered on the target itself, so everything around that
  target takes the hit. A target-centered ring — visually and mechanically a
  world apart from the flame's forward-pointing cone.
- The **Void Orb** is **one eternal projectile**: it never expires and hunts
  forever, steering between enemies (or back to you when the arena is empty).
  Projectile upgrades do **not** spawn more orbs — they make the single orb
  **grow** (bigger contact radius). With no damage decay per bounce, every
  hit deals full damage.
- Weapon fields `area` (splash radius), `strength` (knockback), `homing` and
  `bounces` are honored: area deals half-damage in a radius on hit, strength
  pushes the struck enemy, homing steers the projectile toward the nearest
  foe each tick, and bounces reflect the projectile off the hit surface.

### AoE damage falloff

Any area-of-effect hit (cone, scythe reap, inferno, bomb blast, zone tick,
nova tick, beam row, boomerang trail/blast, orb splash) deals **slightly less
damage to each target the more targets it catches at once**:

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
pierce plus the global pierce buff) — cones, scythe reaps, infernos, bombs,
zones, novas, beams, boomerang trails/blasts and orb splashes.

### Evolutions & super evolutions (A + B = C)

Weapons can require other weapons. When you own **all** prerequisites, the
result becomes the **highest-priority** weapon offer, replacing the random
weapon grant:

- **Two-ingredient evolutions** (tag `EVOLUTION! (A+B)`): `storm` (Arcane Wand
  + Heavy Crossbow), `nova` (Void Orb + Runic Hammer), `inferno` (Ember Sprayer
  + Soul Scythe), `pulsar` (Solar Lance + Storm Shuriken) and `halo` (**Radiant
  Halo**: Throwing Dagger + Solar Lance). Radiant Halo spawns **persistent
  beams that orbit you**, reaping everything they sweep through — the beam's
  damage re-expressed as an always-on ring rather than a brief flash.
- **Three-ingredient super evolutions** (tag `SUPER EVOLUTION! (A+B+C)`):
  `vortex` (**Void Gyre**: Throwing Dagger + Soul Scythe + Void Orb) and
  `prism` (**Prism Array**: Ember Sprayer + Solar Lance + Heavy Crossbow).
  Neither is a re-skin of an existing evolution — each introduces a mechanic
  nothing else in the pool has:
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
    every shot.
- Evolutions and supers are **not** added to the normal weapon pool before the
  prerequisites are met.
- Picking one grants the evolved weapon **in addition to** the ingredients'
  slot usage (it occupies one of the 4 slots; you never lose the ingredients).

## Weapon slots

The arsenal starts at **4** weapon slots. That is a hard cap for most of a run:
the level-up offer stops appearing for weapons once you are full.

**Arsenal Core** (`+1 weapon slot`, **max 3 stacks**) is the only way past it.
Each stack adds one slot, so a fully-stacked build can hold **7** weapons.

## Level-ups

XP per level: `7 + 5·(level−1) + 0.85·(level−1)·level`. Overflow XP carries into
the next level; if it purchases another level immediately, another card choice
queues. Picked-up XP is scaled by the **XP multiplier** (`xpMul`): Scholar and
Lorekeeper cards add `+12%` and `+30%` respectively, stacking additively.

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
4. **Fallback** — if the pool is exhausted the game never bricks; the first
   still-stackable upgrade is offered instead.

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

**Weapon uniques** — thirteen weapons have one exclusive treasure card, offered
only while that weapon is equipped (the card carries `weapon = "<id>"`), so the
pool stays relevant to your loadout:

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
| Solar Lance | Prism Lance | Fires 3 parallel beams at once |
| Storm Caller | Thunderlord | +4 chain jumps and no damage decay |
| Void Nova | Supernova | Ring expands faster, wider, and hits harder |
| Inferno | Everflame | Wider reap; burning ground lasts longer and burns harder |
| Pulsar | Arc Saw | The laser trail is 80% wider and deals 35% more damage |

The newer Radiant Halo and the two super evolutions have no exclusive card;
they rely on their raw stats and synergies instead.

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
| `1` / `2` | Previous / next weapon — **all 16 including evolutions and supers** (`storm`, `nova`, `inferno`, `pulsar`, `halo`, `vortex`, `prism`) |
| `3` | Apply a **max build** boost: +100% damage, +4 projectiles, +3 pierce, +80% fire rate. Toggling it off keeps your item picks — it only undoes the boost |
| `4` | Toggle enemy waves (fodder bats spawn so every weapon has a target) |
| `5` | Exit back to the untouched run |
| `E` | Open/close the **item picker** |
| `I` | Toggle **immortality** (all incoming damage ignored, so you can stand in a horde) |
| `F` | Cycle the **difficulty clock**: 1× → 4× → 10× → 20× |
| `K` | **Kill the player** on demand (works through immortality — that is the point of a death switch) |
| `R` | **Max every item** instantly (milestones excluded) |

In the item picker, `↑`/`↓` (or `←`/`→`, or `W`/`S`) move the highlight and
`Enter` takes the highlighted card — one stack per press, up to the card's own
cap. The list shows each card's current stacks and flags cards that are
`[MAX]` or `[NOT ARMED]`.

Other sandbox rules worth knowing:

- **Level-ups work** in the sandbox (that is how the card flow gets exercised)
  but only **item** cards are offered — never weapons, because testing *one*
  weapon is the entire point. Rerolls are **unlimited** there.
- Switching weapons re-seeds a herd of weak test bats.
- Leaving the sandbox removes the sandbox's injected fodder (enemies that were
  already on the field are kept) and puts you back exactly where you stood.
- Dying inside the sandbox with `K` drops you on the normal game-over screen;
  `R` there restarts the run as usual.

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
  trickles, and packs grow every minute (up to ~9 enemies per pack).
- Every ~40–50 s a **horde burst** spawns a full ring of 10–24 enemies from
  every side at once, with telegraphs ringing the whole screen.

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
30–90 s  :  interval = max(0.45, 1.05 − (t−30)·0.01)   (→ 0.45 s)
t ≥ 90 s :  interval = max(0.12, 0.45 − (t−90)·0.004) (→ min 0.12 s)
```

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

| Roll | Becomes | Chance | HP | Touch | Speed | XP |
|------|---------|--------|----|-------|-------|----|
| t ≥ 45 s | Elite (tier 1) | 5% → 15% | ×4 | ×1.5 | ×1.15 | ×3 |
| t ≥ 120 s | Champion (tier 2) | ~1% → 3% | ×7 | ×2.5 | ×1.3 | ×5 |
| t ≥ 300 s | Overlord (tier 3) | ~0.4% → 1.2% | ×14 | ×4 | ×1.5 | ×10 |

Champions and overlords are deliberately **rarer** than in earlier builds: a
single unlucky fast tank should not decide a run.

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