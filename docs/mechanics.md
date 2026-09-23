# Game Mechanics

This document describes exactly how the game plays: the formulas behind
defense, the shield, the level-up system (rerolls, unique items, milestones,
evolutions), elite/champion enemies and their traits, and the spawn/difficulty
curves. Tables of all content are generated into
[`content.md`](content.md) from `assets/data/*.toml` by
`tools/gendocs.py`.

## Run flow

- A run starts at level 1 with **one random starter weapon** (one of the
  `starter = true` entries in `weapons.toml`: Arcane Wand, Throwing Dagger or
  Heavy Crossbow).
- Enemies spawn off-screen with a **telegraph** (see [Spawning](#spawning)),
  walk toward the player and deal contact damage.
- Every weapon fires **automatically** at the nearest enemy. All owned weapons
  share the same target so the whole arsenal focuses the biggest threat.
- Killing enemies drops XP gems. Leveling up opens a card choice (see
  [Level-ups](#level-ups)).
- **Esc** pauses the run and overlays a **character sheet**: level/XP, time,
  kills, HP/regen, shield/defense, damage/cooldown, speed/pickup/lifesteal and
  the full list of owned weapons with their numbers.
- Dying shows the game-over screen; **R** restarts (deterministic seed 1337).

## The player

| Stat | Meaning |
|------|---------|
| Max HP | Starts at 100, growable via `max_hp_add` (also heals by the same amount) |
| Regen | Flat HP per second (`regen_add`) |
| Defense | One stat that mitigates all damage (see below) |
| Shield | Absorbs damage point-for-point before HP, regenerates out of combat |
| Lifesteal | Per-hit chance to heal; see [Lifesteal & healing](#lifesteal--healing) |
| Move speed | Base × `speedMul` |
| Damage / cooldown | Global multipliers applying to every weapon |
| Projectiles / pierce | Additive buffs applied to every weapon |

**Cross-weapon couplings** — stats are not siloed per weapon:

| Stat | Also affects |
|------|--------------|
| Cooldown / attack speed | **Dagger orbit spin** — faster fire rate makes blades rotate faster (`orbitSpeed / cooldownMul`) |
| Projectiles | **Cone range** (+25%/proj), **Bomb arc height** (+25%), **Beam width** (+15%), **Scythe arc** (+15%), **Nova radius** (+10%), **Zone radius** (+20%) |
| Pierce | **Bounce bounces**, **Chain jumps** (one more each), **Bomb knockback** (+0.5), **Zone DPS** (+2) |

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

- Heal upgrades (`heal`) restore flat HP instantly.
- **Lifesteal is chance-based**: a damaging hit with lifesteal `L` has an
  `L%` chance to heal **1 HP** (capped at max HP). At `L ≥ 100` the first
  point is guaranteed and the excess `(L − 100)%` rolls for a **second**
  point — so 150 lifesteal means 100% for 1 HP and 50% for another.
- The **Vampiric Heart** unique makes every lifesteal proc heal **2 HP**
  instead of 1.
- Regen adds flat HP every fixed tick (0.0556 HP/s per point).

## Weapons

- Up to **4 weapon slots** (`kMaxWeapons`). Each slot runs its own cooldown,
  damage, projectile count/speed/life/pierce/spread and projectile tint.
- Every weapon fires `projectiles` bolts in a fan of `spread` radians around
  the aim direction. The fan angle caps at a reasonable maximum so extra
  spread never wraps around backward.
- A weapon is never offered twice; owned weapons are removed from the grant
  pool.
- Each weapon has a distinct **role**: some are rapid spray (flame, ember),
  some are piercing snipers (crossbow, scythe), some are area-splash (hammer,
  nova), some bounce (orb), and some home toward the target (crossbow, shuriken,
  nova). Check `content.md` for the per-weapon traits column.
- Weapon fields `area` (splash radius), `strength` (knockback), `homing` and
  `bounces` are honored: area deals half-damage in a radius on hit, strength
  pushes the struck enemy, homing steers the projectile toward the nearest
  foe each tick, and bounces reflect the projectile off the hit surface.

### Evolutions (A + B = C)

Two weapons are **evolutions**: `storm` (Arcane Wand + Spark Spitter) and
`nova` (Void Orb + Soul Scythe). They are defined as ordinary weapons with a
`requires = [a, b]` list:

- While you own **both** prerequisites, the evolution becomes the **highest
  priority** weapon offer, replacing the random weapon grant.
- Evolutions are **not** added to the normal weapon pool before the
  prerequisites are met.
- Picking the evolution grants the evolved weapon **in addition to** the two
  prerequisites' slot usage (it occupies one of the 4 slots; you do not lose
  the ingredients).

## Level-ups

XP per level: `6 + 4·(level−1) + 0.5·(level−1)·level`. Overflow XP carries into
the next level; if it purchases another level immediately, another card choice
queues.

### Choice generation

Each level-up shows **3 cards, plus** one extra per `extraChoice`
(Gambler's Eye). The cards are filled in this order:

1. **Weapon card** — one card may be a new weapon, with probability
   `(1 − weaponCount/4) · 0.5` (rarer as you fill the 4 slots). When an
   evolution's prerequisites are owned, this card is always that evolution.
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
- The reroll hint is shown under the cards when free rerolls remain.

### Unique items (one-time, rule-changing)

Uniques are violet cards, offered at ~45% per level-up, and each can be taken
once. See [`content.md`](content.md) for the exact list; the rules they bend:

| Unique | Changes the rules |
|--------|-------------------|
| Spreadshot | Doubles the volley spread and halves all weapon cooldowns (≈ double fire rate) |
| Ignited Carapace | Getting hit detonates a burst dealing **3× incoming damage** to enemies nearby |
| Gambler's Eye | +1 card in every future level-up |
| Second Chance | +1 free reroll per level-up (total 2) |
| Adrenaline | Below 30% HP: +60% move speed and 1s invulnerability every 20s |
| Singularity | Every 12s yanks nearby enemies toward you |
| Storm Bolt | Every 3rd projectile hit chains lightning to 3 nearby enemies |
| Blood Price | Every 20 kills detonates a burst around you |
| Cold Blood | Enemies that hit you are slowed for 2s (45% speed) |
| Vampiric Heart | Lifesteal procs heal 2 HP instead of 1 |

### Milestones

Every level divisible by 5 (5, 10, 15, 20, 25, 30 — currently) replaces the
normal choice with a **violet milestone screen**: the pool contains only the
3 milestone cards for that exact level and you pick **2 of 3**. All
milestones are strong and live **outside** the normal pool (they can never
appear as ordinary card offers). With no milestone content for that level the
card UI behaves like a normal level-up.

## Enemies

All enemy stats live in `enemies.toml`: HP, speed, contact damage, radius,
XP, `unlock_at` (seconds into the run when the type enters the spawn pool),
relative `weight`, and `color`/`shape`.

### Spawning

- Spawns happen **11 world units away** from the player (off-screen at
  typical zoom) at a random angle — enemies materialize outside the visible
  area, never on top of you.
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

- Enemy HP also scales globally: every enemy's HP is multiplied by
  `1 + t/70`, so a 2-minute run faces ×2.71 the base HP, a 4-minute run
  ×4.43.
- Enemy **move speed** drifts up over the run (`×1` → `×1.35` at ~3.5 min)
  so late waves stay threatening even for a leveled arsenal.
- Up to **8000 enemies** can be alive at once; the cap protects the frame
  rate rather than throttling spawns.

### Elites & champions

From elapsed time `t`:

| Roll | Becomes | Chance | HP | Touch | XP |
|------|---------|--------|----|-------|----|
| t ≥ 45 s | Elite | 5% → 15% (creeps up) | ×4 | ×1.5 | ×3 |
| t ≥ 120 s | Champion | ~2% → 5% (creeps up) | ×7 | ×2.5 | ×5 |

- Champions fight harder than elites: they also move **×1.3 faster** than
  their base speed and spawn **1.6× larger** (elites are 1.35×).
- Elites are visually **tinted toward white** and always show their enlarged
  hitbox, a HP bar, and a **name tag** with their trait list above them.
- Champions get a distinctive gold-ish tag of their own.

**Trait pool** (compiled in `game.cpp`, `PickTrait`): `fast, armored,
regenerating, explosive, venomous, vampiric, shielded`. The number of traits
rolled grows with time — `min(4, 1 + t/90)`, and a champion gets **+1 more**:

| Trait | Effect |
|-------|--------|
| Fast | ×1.7 move speed |
| Armored | ×2.5 HP and ×1.3 touch damage |
| Regenerating | Regenerates 2% of max HP per second |
| Explosive | Counts down; detonates near the player |
| Venomous | Hitting the player applies 3 s poison (damage over time) |
| Vampiric | Heals 50% of its touch damage when it hits you |
| Shielded | Gains a shield equal to 50% of max HP (absorbs damage first) |

Elites never roll duplicate traits, and their name tag lists the applied
traits so the player can react.

### Contact damage

Hits go through the standard mitigation/shield pipeline, then grant the
player 0.30 s of contact invulnerability — enemies connect noticeably more
often, so positioning matters. Tougher enemies survive hits because the ramp
multiplies their HP — nothing is one-shotted out of its weight class.

## Combat helpers

- **Thorns/Blood Price chain** bursts exclude the player entity from their
  area loops (no self-damage, no double counting).
- **Storm Bolt** chains on every 3rd projectile hit, bouncing to 3 nearest
  enemies.

## Death & restart

Death ends the run. `R` restarts with a deterministic seed (1337) so a ruined
run is reproducible. Restarting re-rolls the starter weapon from the same pool.