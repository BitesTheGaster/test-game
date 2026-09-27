<!-- GENERATED FILE — do not edit by hand.
     Source: assets/data/*.toml, generator: tools/gendocs.py -->

# Content Reference

All numbers below are read directly from the TOML files in `assets/data/`.
Regenerate with:

```sh
python3 tools/gendocs.py
```

**Totals:** 32 weapons (18 base +
11 evolutions +
3 super evolutions),
18 enemies, 18 in-game manual pages.

### Base weapons

| Name | ID | Attack | Damage | Cooldown (s) | Projectiles | Pierce | Starter | Rule | Description |
|---|---|---|---|---|---|---|---|---|---|
| Arcane Wand | `wand` | projectile | 9 | 0.34 | 1 | 0 | yes |  | Quick magic bolts. The baseline done properly: fast, flat, always hits. |
| Throwing Dagger | `dagger` | orbit | 5 | 0.4 | 3 | 0 | yes | orbit_count 3, orbit_radius 1.3, orbit_speed 3 | Orbiting knives that carve up anything close. |
| Heavy Crossbow | `crossbow` | projectile | 20 | 2.2 | 1 | 4 | yes | homing | Slow, homing piercing bolt that punches through crowds. |
| Ember Sprayer | `flame` | cone | 4 | 0.35 | 1 | 0 |  | cone_angle 1, cone_range 2.8, cone_tick_rate 0.08 | Cone of fire - instant damage in a wide arc. |
| Runic Hammer | `hammer` | bomb | 45 | 1.8 | 1 | 0 |  | bomb_arc_height 2.5, bomb_explode_radius 2, bomb_knockback 5, lands on target | Arcing bomb with massive explosion and knockback. |
| Storm Shuriken | `shuriken` | boomerang | 8 | 0.6 | 2 | 1 |  | boomerang_range 4.5, boomerang_return_speed 1.8 | Boomerang blades - hit going out AND coming back. |
| Void Orb | `orb` | bounce | 35 | 1.5 | 1 | 5 |  | bounce_count 4, bounce_damage_mul 1, eternal, bounce_range 3 | One eternal orb that hunts forever. Projectiles grow it, not multiply. |
| Soul Scythe | `scythe` | sweep | 55 | 1.3 | 1 | 0 |  | sweep_angle 6.2832, sweep_knockback 3, sweep_radius 4.6 | Reaps a full circle of death around its nearest prey. |
| Solar Lance | `beam` | beam | 130 | 1.6 | 1 | 0 |  | beam_duration 0.25, beam_range 12, beam_width 0.6 | Instant hitscan beam. Slow to swing, but it deletes a whole line at once. |
| Rail Rifle | `railgun` | projectile | 78 | 1.8 | 1 | 6 |  |  | One hypervelocity slug. Slow to load, punches through an entire rank. |
| Frost Shards | `shard` | projectile | 12 | 0.45 | 4 | 0 |  | chill_mul 0.7, chill_time 1.2 | A tight volley of fast shards that shreds and chills whatever walks into it. |
| Siege Mortar | `mortar` | bomb | 62 | 2.4 | 1 | 0 |  | bomb_ahead 4.5, bomb_arc_height 3.2, bomb_explode_radius 2.6, bomb_fuse 0.35, bomb_knockback 6 | Fires over the crowd. The shell sails past the front rank on purpose and cooks whatever is behind it. |
| Pinball Puck | `pinball` | bounce | 20 | 1.6 | 1 | 2 |  | bounce_count 6, bounce_damage_mul 0.85, bounce_range 3.5 | A white-hot puck that keeps ricocheting between bodies until it burns out. |
| Grave Bell | `lure` | lure | 0 | 2.6 | 1 | 0 |  | lure_dps 18, lure_duration 5, lure_max_beacons 2, lure_pull 5.5, lure_radius 1.5, lure_reach 4.2, lure_tick_rate 0.15 | Plants a bell that hauls the horde into its core. It fights from where it stands, not from where you stand. |
| Jackhammer Drill | `drill` | cone | 7 | 0.55 | 1 | 0 |  | cone_angle 0.45, cone_bite 0.28, cone_bite_max 3, cone_range 3.4, cone_tick_rate 0.05 | Bites one target and chews through it. The deeper it stays buried, the harder it bites - useless on a swarm, lethal on a heavy. |
| Shock Core | `shockcore` | nova | 20 | 2.2 | 1 | 0 |  | nova_damage_per_tick 14, nova_expand_speed 6, nova_max_radius 3.6, nova_tick_rate 0.1 | A ring of pressure that blows itself outward from where you are standing. |
| Barbed Whip | `whip` | sweep | 26 | 0.7 | 1 | 0 |  | sweep_angle 1.9, sweep_knockback 3, sweep_lead 1.8, sweep_radius 2.8 | Lashes the arc in front of you, whether or not anything is standing in it. |
| Tesla Coil | `tesla` | chain | 11 | 0.65 | 1 | 0 |  | chain_damage_mul 0.6, chain_jump_range 2.8, chain_max_jumps 3 | Lightning that keeps jumping. One target is never enough. |

### Evolutions (A + B = C)

| Name | ID | Attack | Requires | Damage | Cooldown (s) | Projectiles | Pierce | Rule | Description |
|---|---|---|---|---|---|---|---|---|---|
| Storm Caller | `storm` | projectile | `wand` + `crossbow` | 15 | 0.6 | 1 | 4 | reaim_range 4.2, reaim_turn 0.22 | Wand + Crossbow. The wand's bolt keeps the crossbow's punch: it drives into a body and bends onto the next one, so it never leaves the pack. |
| Void Nova | `nova` | nova | `orb` + `hammer` | 40 | 1.8 | 1 | 0 | nova_burst_damage 90, nova_contract, nova_damage_per_tick 35, nova_expand_speed 4, nova_max_radius 5, nova_pull 7, nova_tick_rate 0.12 | Orb + Hammer. A ring cast wide that rushes back in, dragging the horde to a knot, then detonates on top of them. |
| Inferno | `inferno` | inferno | `flame` + `scythe` | 60 | 1.6 | 1 | 0 | sweep_angle 6.2832, sweep_knockback 4, sweep_radius 4.2, zone_dps 30, zone_duration 2.5, zone_radius 2 | Sprayer + Scythe. Reaps a circle and leaves burning ground. |
| Pulsar | `pulsar` | pulsar | `beam` + `shuriken` | 12 | 0.9 | 1 | 5 | beam_width 1.3, boomerang_range 6.5, boomerang_return_speed 2.4 | Shuriken + Lance. A light-chakram that burns the WHOLE LINE it flies, out and back, not just the blade tip - it carves corridors. |
| Radiant Halo | `halo` | halo | `dagger` + `beam` | 40 | 1 | 2 | 4 | beam_range 6, beam_width 0.5, halo_inner 2.2, halo_knockback 2.4, orbit_speed 2.6 | Dagger + Lance. Blades of light walk a slow circle around you - with a ring of safe ground at your feet. They cut what they cross, not what hugs you. |
| Blizzard Rail | `blizzard` | chain | `railgun` + `shard` | 22 | 0.5 | 1 | 0 | chain_damage_mul 0.65, chain_jump_range 3.4, chain_max_jumps 4, chain_shatter 6 | Rail Rifle + Frost Shards. The slug shatters on contact into a fan of shards, each one hopping on its own. |
| Ashfall | `siege` | inferno | `mortar` + `lure` | 72 | 1.8 | 1 | 0 | binds to the bell, sweep_angle 6.2832, sweep_knockback 5, sweep_radius 3.8, zone_dps 34, zone_duration 3, falls from above, zone_radius 2.4 | Siege Mortar + Grave Bell. The barrage falls on the BELL, so the horde it gathered is the horde it cooks. |
| Chaos Sphere | `chaos` | bounce | `pinball` + `orb` | 30 | 1.6 | 1 | 8 | bounce_count 7, bounce_damage_mul 0.9, bounce_range 3.8, bounce_splits 2 | Pinball + Void Orb. It comes apart: each impact throws fragments, and each fragment breaks again. |
| Sundering Core | `sunder` | wave | `shockcore` + `scythe` | 55 | 1.6 | 1 | 0 | wave_count 1, wave_damage_mul 0.85, wave_knockback 6, wave_range 9, wave_speed 7.5, wave_spread 1.25, wave_width 3.4 | Shock Core + Soul Scythe. One huge crescent tears out of you and keeps going, shoving the whole front rank downrange. |
| Tidal Lash | `tidewhip` | wave | `whip` + `shuriken` | 34 | 0.55 | 1 | 0 | sweep_lead 2.2, wave_arc_step 1.1, wave_count 3, wave_damage_mul 0.7, wave_hook_pull 0.85, wave_knockback 5, wave_range 5.5, wave_speed 9, wave_spread 0.55, wave_width 2.6 | Barbed Whip + Storm Shuriken. Three narrow hooks come around in a fan, each one dragging its catch into the next. |
| Hoarfrost Wake | `rimewake` | projectile | `shard` + `orb` | 9 | 0.55 | 3 | 3 | aura_chill_mul 0.62, aura_chill_time 0.9, aura_dps 26, aura_radius 1.5, aura_tick 0.1, chill_mul 0.55, chill_time 1.4 | Frost Shards + Void Orb. The shards drag a freezing, grinding bubble behind them - everything the volley PASSES walks in slow and comes apart, hit or not. |

### Super evolutions (A + B + C)

| Name | ID | Attack | Requires | Damage | Cooldown (s) | Projectiles | Pierce | Rule | Description |
|---|---|---|---|---|---|---|---|---|---|
| Void Gyre | `vortex` | vortex | `dagger` + `scythe` + `orb` | 26 | 1 | 3 | 6 | vortex_crowd 0.75, vortex_orbit 1.6, vortex_orbit_speed 1.7, vortex_pull 8, vortex_radius 2.4, vortex_reach 3.4, vortex_tick_rate 0.1 | Dagger + Scythe + Void Orb. Suction wells hold what they catch, and their damage is a function of HOW MANY they hold - feeble alone, enormous in a knot. More projectiles means more wells. |
| Prism Array | `prism` | prism | `flame` + `beam` + `crossbow` | 42 | 1.05 | 4 | 2 | beam_duration 0.18, prism_max_targets 5, prism_range 11, prism_ricochet 4.5, prism_width 0.5 | Sprayer + Lance + Crossbow. One locked beam per projectile, each on a different enemy - a crowd gets shredded from several angles at once. |
| Event Horizon | `eventhorizon` | vortex | `railgun` + `shockcore` + `flame` | 48 | 1 | 4 | 6 | vortex_burst_damage 110, vortex_burst_radius 3.4, vortex_collapse_at 3, vortex_orbit 3, vortex_orbit_speed 1.9, vortex_pull 4, vortex_radius 1.5, vortex_reach 3.2, vortex_tick_rate 0.1 | Rail Rifle + Shock Core + Ember Sprayer. Four wells circle you, swallow the horde, then COLLAPSE - each implosion is a blast and the well reopens across the orbit. A flat tick, on a clock. |

---

## Enemies

| Name | ID | HP | Speed | Speed ramp | Fast | Touch dmg | Radius | XP | Unlocks at | Weight | Shape |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Bat | `bat` | 8 | 2.4 | 0.75 |  | 5 | 0.3 | 1 | 0s | 6 | circle |
| Slime | `slime` | 16 | 1.5 | 0 |  | 6 | 0.4 | 1 | 8s | 5 | circle |
| Spider | `spider` | 10 | 2.9 | 0.72 | yes | 6 | 0.28 | 2 | 4:00 | 4 | rect |
| Zombie | `zombie` | 34 | 2.1 | 0 |  | 11 | 0.38 | 2 | 25s | 4 | rect |
| Imp | `imp` | 14 | 3.4 | 0.71 | yes | 8 | 0.26 | 2 | 4:00 | 4 | circle |
| Skeleton | `skeleton` | 28 | 3.4 | 0 |  | 9 | 0.34 | 3 | 45s | 4 | rect |
| Chest Mimic | `mimic` | 70 | 3 | 0 |  | 14 | 0.42 | 5 | 1:00 | 3 | rect |
| Wraith | `wraith` | 18 | 3.2 | 0.75 | yes | 9 | 0.3 | 3 | 4:00 | 3 | circle |
| Ghost | `ghost` | 45 | 3 | 0 |  | 12 | 0.36 | 4 | 1:25 | 3 | circle |
| Harpy | `harpy` | 55 | 2.9 | 0.72 | yes | 14 | 0.34 | 5 | 4:00 | 3 | circle |
| Charger | `charger` | 60 | 3.6 | 0.72 | yes | 16 | 0.44 | 5 | 4:00 | 3 | rect |
| Brute | `brute` | 140 | 1.6 | 0 |  | 22 | 0.62 | 8 | 3:00 | 2 | circle |
| Stalker | `stalker` | 90 | 2.8 | 0.71 | yes | 15 | 0.4 | 7 | 4:30 | 3 | circle |
| Abomination | `abomination` | 200 | 2.6 | 0 |  | 20 | 0.55 | 10 | 5:00 | 2 | circle |
| Golem | `golem` | 320 | 1.2 | 0 |  | 30 | 0.75 | 14 | 7:00 | 2 | rect |
| Juggernaut | `juggernaut` | 420 | 1.8 | 0 |  | 34 | 0.8 | 18 | 9:30 | 2 | rect |
| Oracle | `oracle` | 160 | 2.6 | 0.69 | yes | 24 | 0.46 | 14 | 6:30 | 2 | circle |
| Reaper | `reaper` | 240 | 3.1 | 0.74 | yes | 26 | 0.5 | 16 | 8:00 | 2 | circle |

---

## Difficulty ramp

Three things rise over a run: how often a pack arrives, how big the
enemies are, and how hard they hit. The first is a three-leg curve, the
other two are a shallow leg that steepens at a knee.

### Pack interval

```text
t < 0:30  : interval = 1.2 - t*0.005
0:30-2:30 : interval = max(0.58, 1.05 - (t-0:30)*0.0039)
t >= 2:30 : interval = max(0.3, 0.58 - (t-2:30)*0.0021)
```

The floor is 0.3s -- about 3 packs a second, and a
pack is several bodies by the time the run gets there. At 0.12s the game
threw around thirty enemies a second at the player, which no build can
answer and which just ends the run early.

The third leg used to start at 1:30 and bottom out at 0.20s, so the run
went from one pack a second to five inside the two minutes where the heavy
enemies were also arriving. Moving the leg later AND raising the floor is
the same fix from both ends: the screen fills later, and the things in it
can be killed when it does.

### Enemy stats

| Stat | Shallow leg | After the knee | Cap |
|------|-------------|----------------|-----|
| HP | `1 + t/145` | `+ (t-480)/62` | x20 |
| Speed | `1 + t/780` | `+ (t-480)/380` | x2.05 |
| Contact | `1 + t/2000` | -- | uncapped |

The knee is at 8:00. Every number here is lower than it was and the
knee is a minute later, for the same reason the XP curve is: **the HP ramp
is the one piece of difficulty the player has no answer to.** A build three
picks behind cannot outshoot a doubled enemy, so the ramp has to stop
outrunning the build.

The shape is intact -- a rising ramp that steepens -- it just stops being
the whole difficulty. The heavies are now spread across the run (see
`enemies.toml`), so the early leg does not have to carry all of it: at 2:30
an ordinary enemy is barely doubled and the threat is whatever has actually
walked in.

---

## The elite ladder

An elite and above is a trash mob with a body count problem. Its stats are
a multiplier on whatever trash archetype it rolled, so a champion is always
the same fight wearing a different body. HP is a range, rolled per spawn.

**Each tier is x5 the one below it.** 6.5x, 32.5x, 162.5x on the midpoints,
which is a number you can hold in your head: each rung is a different fight,
not a bigger version of the last one.

| Tier | HP x trash | Touch | Speed | XP | Traits | Opens at | Gate | Arrives |
|------|-----------|-------|-------|----|--------|----------|------|---------|
| Elite | 5-8 | x1.25 | x1.08 | x3 | 1 | 1:30 | the clock alone | every 100-130s |
| Champion | 25-40 | x1.9 | x1.18 | x5 | 3 | 2:00 | after 4 elites | every 250-320s |
| Overlord | 125-200 | x2.8 | x1.3 | x10 | 7 | 7:00 | after 2 champions | every 470-590s |

**The frequency is the balance here, not the HP.** A tier used to be a share
of every spawn, which made it a tax on the trash rather than an event: an
elite was one body in ten, forever, so a busy screen produced a champion
every seven seconds -- and since every tiered body drops a box, a ten-minute
run handed over 42 chests and the player had the whole arsenal by minute
three. A tier is on a clock now, and the box is the only thing in the game
that decides how often you are handed new cards.

An arrival is **one or two bodies**, or **three or four** once you are far
enough past that tier to be handling it rather than meeting it. A player who
has outgrown a tier gets more of the same fight rather than a new label,
which is the only answer that keeps the tier's identity readable. Per tier, at
most 4, 2 and 1 alive at once: an overlord
is the run's boss and there is only ever one.

A tribunal is a **milestone you have reached**, not a mood you are in. Kill
4 elites and champions are yours for the rest of the run; kill
2 champions and overlords are. It does not close again. A
struggling build simply never gets there, which is the punishment, and it is a
thing the player can see and count towards in the bestiary.

The gate used to be a decaying score instead, calibrated against a supply of
elite kills that a clock-driven elite no longer produces -- so the two tiers
above the elite were unreachable, and an unreachable gate is worse than a wrong
one, because the curve that was supposed to be adapting was simply absent.
Each tier's clock starts when its tribunal opens, so a champion earned at
minute nine does not land on the same frame.

The XP multiplier is deliberately NOT scaled down with the HP. An elite is a
reward before it is a threat, and if killing one is worse value than killing
three pieces of trash then the player is right to ignore it -- which is
exactly how a promotion stops being a promotion.

---

## Chests

A tiered enemy drops a chest where it died. Walk over it: a chest is a
pickup, never a button. What is inside is cards that improve the weapons
you already own and the build around them -- never a new weapon, and never
a level-up, because a corpse must not be able to ask you a question you
have to answer. The box leans on whichever weapon it has touched least, so
a hand never lands four times on the same gun.

| Dropped by | Cards | What it is |
|------------|-------|------------|
| Elite | 1 | One card. The small change. |
| Champion | 3 | A whole hand. |
| Overlord | 7 | Most of the arsenal in one pickup. |

`Deep Cache` adds one more card to every box in the game. The count is
deliberately NOT clamped to the size of the arsenal: clamping here would
quietly downgrade a champion's gift to an elite's for every early run, and
the early run is exactly when a champion is hardest to kill and the box is
most worth having.


---

## Upgrades

### Normal pool

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Group | After | Slot | Description |
|---|---|---|---|---|---|---|---|---|---|---|
| Whetstone | `damage` | damage_mul | 0.3 | 8 |  |  |  |  | yes | +30% damage |
| Battle Haste | `haste` | fire_rate | 0.18 | 8 |  |  |  |  | yes | +18% fire rate |
| Split Shot | `multi` | proj_add | 1 | 6 |  |  |  |  | yes | +1 projectile |
| Impact | `impact` | knockback_mul | 0.45 | 3 |  |  |  |  | yes | +45% knockback from all your attacks and bursts |
| Soul Magnet | `magnet` | pickup_mul | 0.3 | 8 |  |  |  |  | yes | +30% pickup range |
| Scholar | `scholar` | xp_mul | 0.12 | 8 |  |  |  |  | yes | +12% experience gained |
| Piercing Shots | `pierce` | pierce_add | 1 | 6 |  |  |  |  | yes | +1 pierce |
| Blood Surge | `surge_chain` | momentum_damage | 1 | 2 |  |  |  |  | yes | Kill chain: +1% damage per stack |
| Rampage | `rampage` | momentum_speed | 4 | 1 |  |  |  |  | yes | Kill chain: +4% move speed per stack |
| Deep Reserves | `deep_reserves` | momentum_window | 2 | 1 |  |  |  |  | yes | Kill chain: 2 more seconds before it goes cold |
| Kill Tempo | `kill_tempo` | momentum_rate | 0.5 | 3 |  |  |  |  | yes | Kill chain: +0.5% fire rate per stack |
| Long Tail | `long_tail` | momentum_chain | 6 | 2 |  |  |  |  | yes | Kill chain: 6 more stacks count toward the multipliers |
| Cull | `cull` | momentum_gain | 0.5 | 2 |  |  |  |  | yes | Kill chain: +0.5 stacks per kill |
| Regeneration | `regen` | regen_add | 1 | 8 |  |  |  |  | yes | +1 HP per second |
| Windrunner | `windrunner` | speed_mul | 0.08 | 6 |  |  |  |  | yes | +8% move speed |
| Iron Constitution | `constitution` | max_hp_add | 45 | 8 |  |  |  |  | yes | +45 max HP and heal 45 |
| Stony Pledge | `pledge` | defense_add | 12 | 8 |  |  |  |  | yes | +12 defense |
| Runic Ward | `ward_small` | shield_add | 13 | 8 |  |  |  |  | yes | +13 regenerating shield |
| Aegis Flow | `aegis_flow` | shield_regen | 0.6 | 3 |  |  |  |  | yes | Shield refills 60% faster, and refills right now |
| Quickdraw | `quickdraw_ward` | shield_delay | 1 | 2 |  |  |  |  | yes | Shield starts refilling 1s sooner after you take a hit |
| Field Kit | `field_kit` | heal_pct | 0.4 | 2 |  |  |  |  | yes | Patch yourself up: heal 40% of your maximum HP |
| Gilded Fangs | `leech_gold` | lifesteal_add | 3 | 8 |  |  |  |  | yes | +3% lifesteal |
| Sunder Edge | `sunder` | armor_pierce_add | 28 | 8 |  |  |  |  | yes | +28 armor pierce |
| Wand Focus | `w_wand_power` | w_damage_add | 8 | 3 | wand |  |  |  | yes | Arcane Wand: +8 damage |
| Wand Channeling | `w_wand_speed` | w_fire_rate | 0.16 | 2 | wand |  |  |  | yes | Arcane Wand: +16% fire rate |
| Dagger Honing | `w_dagger_power` | w_damage_add | 5 | 3 | dagger |  |  |  | yes | Throwing Dagger: +5 damage each |
| Dagger Volley | `w_dagger_volley` | w_proj_add | 1 | 2 | dagger |  |  |  | yes | Throwing Dagger: +1 projectile |
| Crossbow Wit | `w_crossbow_power` | w_damage_add | 12 | 2 | crossbow |  |  |  | yes | Heavy Crossbow: +12 damage |
| Crossbow Drill | `w_crossbow_pierce` | w_pierce_add | 2 | 2 | crossbow |  |  |  | yes | Heavy Crossbow: +2 pierce |
| Ember Intensity | `w_flame_power` | w_damage_add | 3 | 3 | flame |  |  |  | yes | Ember Sprayer: +3 damage |
| Ember Horde | `w_flame_volley` | w_proj_add | 2 | 2 | flame |  |  |  | yes | Ember Sprayer: +2 projectiles |
| Hammer Rune | `w_hammer_power` | w_damage_add | 20 | 2 | hammer |  |  |  | yes | Runic Hammer: +20 damage |
| Hammer Wrath | `w_hammer_pierce` | w_pierce_add | 3 | 2 | hammer |  |  |  | yes | Runic Hammer: +3 pierce |
| Shuriken Storm | `w_shuriken_power` | w_damage_add | 5 | 3 | shuriken |  |  |  | yes | Storm Shuriken: +5 damage |
| Shuriken Cyclone | `w_shuriken_speed` | w_fire_rate | 0.19 | 2 | shuriken |  |  |  | yes | Storm Shuriken: +19% fire rate |
| Rail Tuning | `w_railgun_power` | w_damage_add | 30 | 2 | railgun |  |  |  | yes | Rail Rifle: +30 damage |
| Overcharged Rail | `w_railgun_pierce` | w_pierce_add | 2 | 2 | railgun |  |  |  | yes | Rail Rifle: +2 pierce |
| Shard Whetstone | `w_shard_power` | w_damage_add | 5 | 3 | shard |  |  |  | yes | Frost Shards: +5 damage |
| Shard Barrage | `w_shard_volley` | w_proj_add | 2 | 2 | shard |  |  |  | yes | Frost Shards: +2 projectiles |
| Mortar Charge | `w_mortar_power` | w_damage_add | 25 | 2 | mortar |  |  |  | yes | Siege Mortar: +25 damage |
| Barrage | `w_mortar_volley` | w_proj_add | 1 | 2 | mortar |  |  |  | yes | Siege Mortar: +1 shell per shot |
| Puck Polish | `w_pinball_power` | w_damage_add | 10 | 3 | pinball |  |  |  | yes | Pinball Puck: +10 damage |
| Hot Wheels | `w_pinball_speed` | w_fire_rate | 0.2 | 2 | pinball |  |  |  | yes | Pinball Puck: +20% fire rate |
| Deeper Toll | `w_lure_power` | w_lure_power | 8 | 3 | lure |  |  |  | yes | Grave Bell: +8 damage per second |
| Quick Chime | `w_lure_rate` | w_fire_rate | 0.18 | 2 | lure |  |  |  | yes | Grave Bell: +18% fire rate |
| Drill Bit | `w_drill_power` | w_damage_add | 3 | 3 | drill |  |  |  | yes | Jackhammer Drill: +3 damage per tick |
| Redline | `w_drill_rate` | w_fire_rate | 0.25 | 2 | drill |  |  |  | yes | Jackhammer Drill: +25% fire rate |
| Deeper Charge | `w_shockcore_power` | w_nova_power | 10 | 3 | shockcore |  |  |  | yes | Shock Core: +10 damage per tick |
| Quicken | `w_shockcore_rate` | w_fire_rate | 0.2 | 2 | shockcore |  |  |  | yes | Shock Core: +20% fire rate |
| Barb Wire | `w_whip_power` | w_damage_add | 12 | 3 | whip |  |  |  | yes | Barbed Whip: +12 damage |
| Long Lash | `w_whip_wide` | w_proj_add | 1 | 2 | whip |  |  |  | yes | Barbed Whip: a 15% wider lash per stack |
| Coil Tuning | `w_tesla_power` | w_damage_add | 5 | 3 | tesla |  |  |  | yes | Tesla Coil: +5 damage per jump |
| Arc Cascade | `w_tesla_rate` | w_fire_rate | 0.22 | 2 | tesla |  |  |  | yes | Tesla Coil: +22% fire rate |
| Deep Cold | `w_blizzard_power` | w_damage_add | 8 | 2 | blizzard |  |  |  | yes | Blizzard Rail: +8 damage per jump |
| Whiteout | `w_blizzard_rate` | w_fire_rate | 0.2 | 2 | blizzard |  |  |  | yes | Blizzard Rail: +20% fire rate |
| Thermite | `w_siege_power` | w_damage_add | 30 | 2 | siege |  |  |  | yes | Ashfall: +30 damage to the reap |
| Bombardment | `w_siege_rate` | w_fire_rate | 0.25 | 2 | siege |  |  |  | yes | Ashfall: +25% fire rate |
| Unstable Core | `w_chaos_power` | w_damage_add | 15 | 2 | chaos |  |  |  | yes | Chaos Sphere: +15 damage |
| Ricochet Tuning | `w_chaos_rate` | w_fire_rate | 0.2 | 2 | chaos |  |  |  | yes | Chaos Sphere: +20% fire rate |
| Sunder Charge | `w_sunder_power` | w_nova_power | 20 | 2 | sunder |  |  |  | yes | Sundering Core: +20 damage per tick |
| Faster Ring | `w_sunder_rate` | w_fire_rate | 0.2 | 2 | sunder |  |  |  | yes | Sundering Core: +20% fire rate |
| Deep Current | `w_tidewhip_power` | w_damage_add | 16 | 2 | tidewhip |  |  |  | yes | Tidal Lash: +16 damage |
| Spring Tide | `w_tidewhip_rate` | w_fire_rate | 0.22 | 2 | tidewhip |  |  |  | yes | Tidal Lash: +22% fire rate |
| Singularity Tuning | `w_horizon_power` | w_damage_add | 20 | 2 | eventhorizon |  |  |  | yes | Event Horizon: +20 damage |
| Deeper Well | `w_horizon_wells` | w_proj_add | 1 | 2 | eventhorizon |  |  |  | yes | Event Horizon: +1 gravity well |
| Arsenal Core | `u_arsenal_core` | weapon_slot_add | 1 | 3 |  |  |  |  | no | +1 weapon slot |
| Long Barrel | `u_long_barrel` | w_all_reach | 0.1 | 5 |  |  |  |  | yes | Reaches 10% further and its shots live 10% longer |
| Heavy Stock | `u_heavy_stock` | w_all_knockback | 0.2 | 4 |  |  |  |  | yes | Throws what it hits 20% further |
| Deep Cache | `u_deep_cache` | chest_bonus | 1 | 3 |  |  |  |  | yes | Every chest opens one more time |
| Eventide Hunger | `w_orb_hunger` | w_orb_grow | 1 | 2 | orb |  |  |  | yes | Void Orb: finds the next victim 30% faster and loses far less per bounce |
| Long Arm | `w_scythe_longarm` | w_scythe_reach | 1 | 3 | scythe |  |  |  | yes | Soul Scythe: +6% reap radius |
| Focused Burn | `w_beam_focus` | w_beam_lance | 1 | 3 | beam |  |  |  | yes | Solar Lance: +6% range and +4% width |
| Arc Cascade | `w_storm_cascade` | w_chain_arc | 1 | 3 | storm |  |  |  | yes | Storm Caller: +6% jump range and +5% damage per link |
| Rupture | `w_nova_rupture` | w_nova_wide | 1 | 3 | nova |  |  |  | yes | Void Nova: +6% ring radius and +4% expansion speed |
| Pyre Spread | `w_inferno_pyre` | w_reap_wide | 1 | 3 | inferno |  |  |  | yes | Inferno: +6% reap radius, burning ground lasts 0.5s longer |
| Long Cast | `w_pulsar_longcast` | w_boomerang_reach | 1 | 3 | pulsar |  |  |  | yes | Pulsar: +7% flight range and +6% return speed |
| Pooled Ash | `w_flame_pools` | w_zone_pools | 1 | 2 | flame |  |  |  | yes | Ember Sprayer: one more burning pool on the ground at a time |
| Ashfall Spread | `w_inferno_pools` | w_zone_pools | 1 | 2 | inferno |  |  |  | yes | Inferno: one more burning pool on the ground at a time |
| Concussion Charge | `w_hammer_concussion` | w_bomb_blast | 1 | 3 | hammer |  |  |  | yes | Runic Hammer: +8% blast radius and +6% knockback |
| Wide Shell | `w_mortar_concussion` | w_bomb_blast | 1 | 3 | mortar |  |  |  | yes | Siege Mortar: +8% blast radius and +6% knockback |
| Echo Anchor | `w_lure_anchor` | w_lure_anchor | 1 | 2 | lure |  |  |  | yes | Grave Bell: the beacon lasts 0.8s longer, and one more may be planted |
| Siege Chime | `w_lure_anchor_siege` | w_lure_anchor | 1 | 2 | siege |  |  |  | yes | Ashfall: the beacon lasts 0.8s longer, and one more may be planted |
| Beam Lattice | `w_prism_lattice` | w_prism_lattice | 1 | 2 | prism |  |  |  | yes | Prism Array: one more independent beam, +5% range |
| Long Wings | `w_halo_wings` | w_halo_wings | 1 | 3 | halo |  |  |  | yes | Radiant Halo: +8% beam reach and 1.5 more shove per beam |
| Denser Gyre | `w_vortex_core` | w_vortex_core | 1 | 3 | vortex |  |  |  | yes | Void Gyre: fatter core, wider orbit, 12% faster spin |
| Phase Mirror | `phase_mirror` | ability_dash | 0.8 | 3 |  |  |  |  | yes | Phase Dash: +0.8 distance, +0.06s of invulnerability on arrival |
| Concussion Core | `concussion_core` | ability_burst | 0.8 | 3 |  |  |  |  | yes | Overload: +0.8 radius, +30 damage, +1 knockback |
| Cryostasis | `cryostasis` | ability_slow | 1 | 3 |  |  |  |  | yes | Stasis: +1s of duration, and the slowed world drops another 0.08x |

### Unique items (one-time, rule-changing)

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Group | After | Slot | Description |
|---|---|---|---|---|---|---|---|---|---|---|
| Spreadshot | `u_spreadshot` | fan | 1 | 1 |  |  |  |  | no | Double volley spread and fire rate, but shots spray +/- 30 degrees |
| Ignited Carapace | `u_thorns` | thorns | 3 | 1 |  |  |  |  | no | Getting hit detonates a burst dealing 3x incoming damage |
| Gambler's Eye | `u_extra_choice` | extra_choice | 1 | 1 |  |  |  |  | no | +1 card in every future level-up choice |
| Second Chance | `u_reroll` | reroll_add | 1 | 1 |  |  |  |  | no | +1 free reroll at every level-up |
| Adrenaline | `u_adrenaline` | adrenaline | 1 | 1 |  |  |  |  | no | Below 30% HP: +60% speed and a 1s invulnerability every 20s |
| Singularity | `u_black_hole` | black_hole | 1 | 1 |  |  |  |  | no | Every 12s, violently yanks nearby enemies toward you |
| Storm Bolt | `u_chain` | chain | 1 | 1 |  |  |  |  | no | Every 3rd projectile hit chains lightning to 3 nearby enemies |
| Blood Price | `u_blood_price` | blood_price | 1 | 1 |  |  |  |  | no | Every 20 kills detonates a burst around you |
| Cold Blood | `u_ice_blood` | ice_blood | 1 | 1 |  |  |  |  | no | Enemies that hit you are slowed for 2s |
| Last Stand | `u_last_stand` | last_stand | 1 | 1 |  |  |  |  | no | Dropping below 20% HP grants a second of invulnerability |
| Repulsion Field | `u_repulsion` | knockback_retaliate | 6 | 1 |  |  |  |  | no | Enemies that strike you are violently knocked away |
| Vampiric Heart | `u_vampiric_heart` | lifesteal_heal | 2 | 1 |  |  |  |  | no | Lifesteal heals 2 HP per proc instead of 1 |
| Seeking Missiles | `uw_wand_seeking` | w_unique_homing | 1 | 1 | wand |  |  |  | no | Arcane Wand: bolts home onto the nearest enemy |
| Blade Vortex | `uw_dagger_vortex` | w_unique_vortex | 1 | 1 | dagger |  |  |  | no | Throwing Dagger: blades spin 2x faster in a 25% wider orbit |
| Fragmenting Bolt | `uw_crossbow_fragment` | w_unique_area | 1.2 | 1 | crossbow |  |  |  | no | Heavy Crossbow: bolts explode on impact for area damage |
| Hearthfire | `uw_flame_hearthfire` | w_unique_hearthfire | 1 | 1 | flame |  |  |  | no | Ember Sprayer: cone is 50% wider and 40% longer |
| Cataclysm | `uw_hammer_cataclysm` | w_unique_cataclysm | 1 | 1 | hammer |  |  |  | no | Runic Hammer: explosions 60% larger with heavier knockback |
| Return Tempest | `uw_shuriken_tempest` | w_unique_area | 2.5 | 1 | shuriken |  |  |  | no | Storm Shuriken: returning blades detonate a 2.5-area burst |
| Echo Detonation | `uw_orb_echo` | w_unique_area | 1.5 | 1 | orb |  |  |  | no | Void Orb: every bounce splashes half damage around the hit |
| Reaper's Harvest | `uw_scythe_harvest` | w_unique_harvest | 3 | 1 | scythe |  |  |  | no | Soul Scythe: sweeps restore 3 HP per kill |
| Bloodthirst | `uw_bloodthirst` | momentum_bloodthirst | 1 | 1 |  |  |  |  | no | Kill chain: twice the stacks, twice the length, 3s longer before it goes cold |
| Prism Lance | `uw_beam_prism` | w_unique_prism | 3 | 1 | beam |  |  |  | no | Solar Lance: three beams at once - forward, left and right |
| Thunderlord | `uw_storm_thunderlord` | w_unique_reaim | 4 | 1 | storm |  |  |  | no | Storm Caller: bolts pierce 2 further, see further for the next body, and corner harder |
| Supernova | `uw_nova_supernova` | w_unique_supernova | 1 | 1 | nova |  |  |  | no | Void Nova: ring expands faster, wider, and hits harder |
| Everflame | `uw_inferno_everflame` | w_unique_everflame | 1 | 1 | inferno |  |  |  | no | Inferno: wider reap, burning ground lasts longer and burns harder |
| Arc Saw | `uw_pulsar_arcsaw` | w_unique_arcsaw | 1 | 1 | pulsar |  |  |  | no | Pulsar: the laser trail is 80% wider and deals 35% more damage |
| Magnetic Slug | `uw_railgun_slug` | w_unique_homing | 1 | 1 | railgun |  |  |  | no | Rail Rifle: the slug homes onto the nearest enemy |
| Deep Toll | `uw_lure_bell` | w_unique_bell | 1 | 1 | lure |  |  |  | no | Grave Bell: 35% harder pull, 20% wider core and reach, and one more bell at a time |
| Gravitic Field | `uw_tesla_gravitic` | w_unique_gravitic | 1 | 1 | tesla |  |  |  | no | Tesla Coil: jumps reach 50% further and stop decaying so hard |
| White Squall | `uw_blizzard_storm` | w_unique_thunderlord | 4 | 1 | blizzard |  |  |  | no | Blizzard Rail: +4 jumps and no damage decay |
| Molten Crater | `uw_siege_molten` | w_unique_molten | 1 | 1 | siege |  |  |  | no | Ashfall: the burning ground is 80% hotter, 25% wider and lasts much longer |
| Detonation Chain | `uw_chaos_echo` | w_unique_area | 1.6 | 1 | chaos |  |  |  | no | Chaos Sphere: every bounce splashes area damage around the hit |
| Fault Line | `uw_sunder_faultline` | w_unique_faultline | 1 | 1 | sunder |  |  |  | no | Sundering Core: the crescent widens into a wall, reaches further, and hits harder |
| Undertow | `uw_tidewhip_lash` | w_unique_lash | 1 | 1 | tidewhip |  |  |  | no | Tidal Lash: a fourth arc, thrown wider, herding its catch harder into the next |
| Wingbeat | `uw_halo_wingbeat` | w_unique_wingbeat | 1 | 1 | halo |  |  |  | no | Radiant Halo: the spokes spin 60% faster, shove harder, and the safe ring at your feet closes in |
| Corona Mantle | `uw_halo_corona` | w_unique_corona | 1 | 1 | halo |  |  |  | no | Radiant Halo: beams shove for 6, 40% wider, 15% longer, +20% damage |
| Black Gyre | `uw_vortex_gyre` | w_unique_gyre | 1 | 1 | vortex |  |  |  | no | Void Gyre: 45% harder pull, 30% further reach, fatter core, ticks faster |
| Total Internal Reflection | `uw_prism_refract` | w_unique_refract | 1 | 1 | prism |  |  |  | no | Prism Array: one more independent beam, 40% longer ricochet, +15% range |
| Rime Lances | `uw_shard_rime` | w_unique_rime | 1 | 1 | shard |  |  |  | no | Frost Shards: lances freeze what they pass through, +4 pierce, 40% longer flight |
| Siege Doctrine | `uw_mortar_doctrine` | w_unique_siege_doctrine | 1 | 1 | mortar |  |  |  | no | Siege Mortar: a 3-shell salvo on a double fuse, 35% wider blasts, 20% slower |
| Silver Skewer | `uw_pinball_skewer` | w_unique_skewer | 1 | 1 | pinball |  |  |  | no | Pinball Puck: +10 bounces, no damage decay, 30% longer reach per hop |
| Overdrive Bore | `uw_drill_bore` | w_unique_bore | 1 | 1 | drill |  |  |  | no | Jackhammer Drill: 60% wider bite, 40% longer reach, strikes far faster |
| Standing Discharge | `uw_shockcore_discharge` | w_unique_discharge | 1 | 1 | shockcore |  |  |  | no | Shock Core: the ring lingers, expands faster and re-strikes twice as fast |
| Barbed Chain | `uw_whip_chainlash` | w_unique_chainlash | 1 | 1 | whip |  |  |  | no | Barbed Whip: the lash goes all the way around, 30% further, 50% harder shove |
| Singularity | `uw_horizon_singularity` | w_unique_singularity | 1 | 1 | eventhorizon |  |  |  | no | Event Horizon: 50% harder pull, fatter core, further reach, denser ticks |
| Combat Reflexes | `u_ability_haste` | ability_haste | 0.25 | 3 |  |  |  |  | no | All abilities recharge 25% faster |
| Heavy Hands | `u_ability_might` | ability_might | 1 | 2 |  |  |  |  | no | Overload: a wider blast that hits 30 harder and shoves 3 further |
| Phase Memory | `u_ability_phase` | ability_phase | 1 | 2 |  |  |  |  | no | Phase Dash: +1.2 distance and +0.2s of invulnerability on arrival |
| Deep Freeze | `u_ability_stasis` | ability_stasis | 1 | 2 |  |  |  |  | no | Stasis: +1s of duration, and the slowed world drops another 0.08x |
| Cascade | `u_ability_echo` | ability_echo | 0.4 | 1 |  |  |  |  | no | Every ability also fires a 40% Overload at the same spot |
| Deep Freeze | `uw_rimewake_deepfreeze` | w_unique_deepfreeze | 1 | 1 | rimewake |  |  |  | no | Hoarfrost Wake: a wider second corona sweeps the lane, chill on hit deepens, 20% more shards |
| Rimefang | `uw_rimewake_rimefang` | w_unique_rimefang | 1 | 1 | rimewake |  |  |  | no | Hoarfrost Wake: the volley narrows into piercing lances and the corona shrinks, so the lane cuts what it touches instead of slowing it |

### Milestones (every power-of-two level from 4 on)

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Group | After | Slot | Description |
|---|---|---|---|---|---|---|---|---|---|---|
| Crimson Pact | `m4_crimson` | ms_lifesteal_seed | 0.6 | 1 |  | 4 | survivor |  | yes | Lifesteal 11% per kill. Everything else you add to lifesteal counts 60% stronger. |
| Verdant Renewal | `m4_renewal` | ms_regen_seed | 0.75 | 1 |  | 4 | survivor |  | yes | Regeneration 1.75 HP/s. Everything else you add to regeneration counts 75% stronger. |
| Aegis | `m4_aegis` | ms_shield_seed | 0.6 | 1 |  | 4 | survivor |  | yes | Shield 50, filled at once. Your whole pool is 60% larger. |
| Blood Debt | `m8_blooddebt` | ms_lifesteal | 0.6 | 1 |  | 8 | survivor.crimson | m4_crimson | yes | Lifesteal 60% stronger again. Totals 2.2x. |
| Wound Echo | `m8_woundecho` | mercy_heal | 3 | 1 |  | 8 | survivor.crimson | m4_crimson | yes | Survive 3 seconds untouched and heal to full, once per injury. |
| Stillness Bloom | `m8_stillbloom` | regen_stillness | 3 | 1 |  | 8 | survivor.renewal | m4_renewal | yes | Regeneration x4 while you give no movement input. |
| Deep Roots | `m8_deeproots` | ms_regen | 0.75 | 1 |  | 8 | survivor.renewal | m4_renewal | yes | Regeneration 75% stronger again. Totals 2.5x. |
| Bulwark | `m8_bulwark` | ms_shield | 0.6 | 1 |  | 8 | survivor.aegis | m4_aegis | yes | Shield pool 60% larger again. Totals 2.2x. |
| Riposte | `m8_riposte` | ms_defense | 1 | 1 |  | 8 | survivor.aegis | m4_aegis | yes | Defence doubled. |
| Thirst Unbound | `m16_thirstunbound` | ms_lifesteal | 0.6 | 1 |  | 16 | survivor.crimson.debt | m8_blooddebt | yes | Lifesteal 60% stronger again. Totals 2.8x, and the payout scales with it. |
| Ironblood | `m16_ironblood` | lifesteal_uncapped | 1 | 1 |  | 16 | survivor.crimson.debt | m8_blooddebt | yes | Lifesteal chance cannot be reduced by enemy resistance. |
| Red Mend | `m16_redmend` | mercy_heal | 2 | 1 |  | 16 | survivor.crimson.echo | m8_woundecho | yes | Two seconds untouched is enough. Heals to full, once per injury. |
| Warmblood | `m16_warmblood` | regen_low | 3 | 1 |  | 16 | survivor.crimson.echo | m8_woundecho | yes | Regeneration x4 while you are below half health. |
| Deep Stillness | `m16_deepstill` | regen_stillness | 8 | 1 |  | 16 | survivor.renewal.still | m8_stillbloom | yes | Regeneration x9 while you give no movement input. |
| Verdant Heart | `m16_verdantheart` | ms_max_hp | 0.5 | 1 |  | 16 | survivor.renewal.still | m8_stillbloom | yes | Total health 50% larger, and regeneration scales with it. |
| Verdant Earth | `m16_verdantearth` | ms_regen | 0.75 | 1 |  | 16 | survivor.renewal.deep | m8_deeproots | yes | Regeneration 75% stronger again. Totals 3.1x. |
| Green Reservoir | `m16_greenreservoir` | regen_of_max | 0.02 | 1 |  | 16 | survivor.renewal.deep | m8_deeproots | yes | Regeneration also counts 2% of your maximum health every second. |
| Aegis Prism | `m16_aegisprism` | ms_shield | 0.6 | 1 |  | 16 | survivor.aegis.bulwark | m8_bulwark | yes | Shield pool 60% larger again. Totals 2.8x. |
| Iron Vow | `m16_ironvow` | shield_delay | 4 | 1 |  | 16 | survivor.aegis.bulwark | m8_bulwark | yes | Shield pool 60% larger, and the 4 second wait before it refills is gone. |
| Stone Vow | `m16_stonevow` | ms_defense | 2 | 1 |  | 16 | survivor.aegis.riposte | m8_riposte | yes | Defence tripled. |
| Riposte Call | `m16_ripostecall` | ms_knockback | 1 | 1 |  | 16 | survivor.aegis.riposte | m8_riposte | yes | Defence tripled, and knockback you deal is doubled with it. |
| Overload | `m8_overload` | ms_damage | 0.6 | 1 |  | 8 | execution |  | yes | All damage +60%. |
| Frenzy | `m8_frenzy` | ms_fire_rate | 0.6 | 1 |  | 8 | execution |  | yes | Fire rate +60%. |
| Tempest | `m8_tempest` | ms_proj_seed | 0.5 | 1 |  | 8 | execution |  | yes | Every weapon fires 4 more projectiles, and 4 becomes 6. |
| Overdrive | `m16_overload_deep` | ms_damage | 0.6 | 1 |  | 16 | execution.dmg | m8_overload | yes | All damage +60% again. Totals 2.2x. |
| Bloodied | `m16_overload_bloodied` | dmg_low | 2 | 1 |  | 16 | execution.dmg | m8_overload | yes | All damage x3 while you are below half health. |
| Fever | `m16_frenzy_deep` | ms_fire_rate | 0.6 | 1 |  | 16 | execution.rate | m8_frenzy | yes | Fire rate +60% again. Totals 2.2x. |
| Rooted | `m16_frenzy_settled` | dmg_still | 1.5 | 1 |  | 16 | execution.rate | m8_frenzy | yes | All damage x2.5 while you give no movement input. |
| Tempest Deep | `m16_tempest_deep` | ms_proj | 0.5 | 1 |  | 16 | execution.proj | m8_tempest | yes | Your extra projectiles are doubled. 4 becomes 8. |
| Skewer | `m16_tempest_pierce` | ms_pierce | 1 | 1 |  | 16 | execution.proj | m8_tempest | yes | Pierce +2, and your pierce bonus is doubled. 2 becomes 4. |
| Ruin | `m32_overload_ruin` | ms_damage | 0.8 | 1 |  | 32 | execution.dmg.deep | m16_overload_deep | yes | All damage +80% again. Totals 3.0x. |
| Guillotine | `m32_overload_guillotine` | ms_execute | 3 | 1 |  | 32 | execution.dmg.deep | m16_overload_deep | yes | All damage x4 against anything below half health. |
| Wrath | `m32_bloodied_wrath` | dmg_low_deeper | 1 | 1 |  | 32 | execution.dmg.low | m16_overload_bloodied | yes | All damage x2 while you are below half health, on top of the x3. Totals x6. |
| Second Wind | `m32_bloodied_secondwind` | dmg_low_tighter | 1.5 | 1 |  | 32 | execution.dmg.low | m16_overload_bloodied | yes | All damage x2.5 below a quarter health instead of a half. Totals x7.5. |
| Blizzard | `m32_frenzy_blizzard` | ms_fire_rate | 0.8 | 1 |  | 32 | execution.rate.deep | m16_frenzy_deep | yes | Fire rate +80% again. Totals 3.0x. |
| Cadence | `m32_frenzy_cadence` | ms_still_rate | 1 | 1 |  | 32 | execution.rate.deep | m16_frenzy_deep | yes | Fire rate x2 while you give no movement input. |
| Anchor | `m32_rooted_anchor` | dmg_still_deeper | 0.6 | 1 |  | 32 | execution.rate.still | m16_frenzy_settled | yes | All damage x1.6 while you give no movement input, on top of the x2.5. |
| Pillar | `m32_rooted_pillar` | ms_still_survival | 2 | 1 |  | 32 | execution.rate.still | m16_frenzy_settled | yes | All damage x2.5 while you give no movement input, and you regenerate 3 HP/s while you do, tripled. |
| Tempest Ruin | `m32_tempest_deep` | ms_proj | 2 | 1 |  | 32 | execution.proj.deep | m16_tempest_deep | yes | Your extra projectiles are quadrupled. 4 becomes 16. |
| Hail | `m32_tempest_hail` | ms_pierce | 2 | 1 |  | 32 | execution.proj.deep | m16_tempest_deep | yes | Pierce +2, and your pierce bonus is tripled. 2 becomes 6. |
| Spine | `m32_skewer_spine` | ms_pierce_armour | 1 | 1 |  | 32 | execution.proj.pierce | m16_tempest_pierce | yes | Pierce +2, and your pierce bonus is tripled. Half a body's armour stops mattering. |
| Lattice | `m32_skewer_lattice` | ms_pierce_area | 3 | 1 |  | 32 | execution.proj.pierce | m16_tempest_pierce | yes | Pierce +6, and your pierce bonus is multiplied fivefold. Every blast is 40% wider. |
| Frostbind | `m16_frostbind` | mark_slow | 2.5 | 3 |  | 16 | element |  | yes | Every hit chills what it strikes for 2.5s |
| Emberbrand | `m16_emberbrand` | mark_burn | 22 | 3 |  | 16 | element |  | yes | Every hit sets it alight: 22 burning damage a second |
| Hex | `m16_hex` | mark_vuln | 0.14 | 3 |  | 16 | element |  | yes | Every hit makes that body take 14% more damage, up to +84% |
| Armour Split | `m16_splitarmor` | mark_defstrip | 22 | 3 |  | 16 | element |  | yes | Every hit strips 22 of the target's own armour, for good |
| Bloodthirst | `m32_bloodthirst` | momentum_bloodthirst | 2 | 2 |  | 32 | momentum |  | yes | The kill chain feeds twice as fast, lasts twice as long, and its top end lifesteals |
| Momentum | `m32_haste` | momentum_rate | 2 | 2 |  | 32 | momentum |  | yes | The kill chain grants +2% fire rate per stack, up to +60% |
| Slaughter | `m32_slaughter` | momentum_damage | 8 | 2 |  | 32 | momentum |  | yes | The kill chain grants +8% damage per stack, up to +240% |
| Stone Mantle | `m64_mantle` | defense_add | 120 | 2 |  | 64 | bulwark |  | yes | Armour +120 |
| Barbed Skin | `m64_thorns` | thorns | 40 | 2 |  | 64 | bulwark |  | yes | Anything that touches you bursts for 40 damage around you |
| Repulsion | `m64_repulse` | knockback_retaliate | 40 | 2 |  | 64 | bulwark |  | yes | Anything that touches you is thrown 40 units clear, and every knockback you deal is 60% harder |
| Ascendant Skin | `m128_ascend` | defense_add | 300 | 2 |  | 128 | apotheosis |  | yes | Armour +300 |
| Crimson Crown | `m128_crown` | lifesteal_add | 40 | 2 |  | 128 | apotheosis |  | yes | Lifesteal +40% and regeneration +8 HP/s |
| Vanquisher | `m128_vanquish` | damage_mul | 1.8 | 2 |  | 128 | apotheosis |  | yes | All damage +180% |
| Perfection | `m128_perfect` | fire_rate | 1.2 | 2 |  | 128 | apotheosis |  | yes | Fire rate +120%, so everything you own fires more than twice as often |

### Milestone groups (mutually exclusive for the run)

| Group | Members | N |
|---|---|---|
| apotheosis | Ascendant Skin, Crimson Crown, Perfection, Vanquisher | 4 |
| bulwark | Barbed Skin, Repulsion, Stone Mantle | 3 |
| element | Armour Split, Emberbrand, Frostbind, Hex | 4 |
| execution | Frenzy, Overload, Tempest | 3 |
| execution.dmg | Bloodied, Overdrive | 2 |
| execution.dmg.deep | Guillotine, Ruin | 2 |
| execution.dmg.low | Second Wind, Wrath | 2 |
| execution.proj | Skewer, Tempest Deep | 2 |
| execution.proj.deep | Hail, Tempest Ruin | 2 |
| execution.proj.pierce | Lattice, Spine | 2 |
| execution.rate | Fever, Rooted | 2 |
| execution.rate.deep | Blizzard, Cadence | 2 |
| execution.rate.still | Anchor, Pillar | 2 |
| momentum | Bloodthirst, Momentum, Slaughter | 3 |
| survivor | Aegis, Crimson Pact, Verdant Renewal | 3 |
| survivor.aegis | Bulwark, Riposte | 2 |
| survivor.aegis.bulwark | Aegis Prism, Iron Vow | 2 |
| survivor.aegis.riposte | Riposte Call, Stone Vow | 2 |
| survivor.crimson | Blood Debt, Wound Echo | 2 |
| survivor.crimson.debt | Ironblood, Thirst Unbound | 2 |
| survivor.crimson.echo | Red Mend, Warmblood | 2 |
| survivor.renewal | Deep Roots, Stillness Bloom | 2 |
| survivor.renewal.deep | Green Reservoir, Verdant Earth | 2 |
| survivor.renewal.still | Deep Stillness, Verdant Heart | 2 |

### Milestone branches (offered only to a run holding the parent)

| Parent | Branches | N |
|---|---|---|
| `m16_frenzy_deep` | Blizzard (`m32_frenzy_blizzard`, group `execution.rate.deep`); Cadence (`m32_frenzy_cadence`, group `execution.rate.deep`) | 2 |
| `m16_frenzy_settled` | Anchor (`m32_rooted_anchor`, group `execution.rate.still`); Pillar (`m32_rooted_pillar`, group `execution.rate.still`) | 2 |
| `m16_overload_bloodied` | Second Wind (`m32_bloodied_secondwind`, group `execution.dmg.low`); Wrath (`m32_bloodied_wrath`, group `execution.dmg.low`) | 2 |
| `m16_overload_deep` | Guillotine (`m32_overload_guillotine`, group `execution.dmg.deep`); Ruin (`m32_overload_ruin`, group `execution.dmg.deep`) | 2 |
| `m16_tempest_deep` | Hail (`m32_tempest_hail`, group `execution.proj.deep`); Tempest Ruin (`m32_tempest_deep`, group `execution.proj.deep`) | 2 |
| `m16_tempest_pierce` | Lattice (`m32_skewer_lattice`, group `execution.proj.pierce`); Spine (`m32_skewer_spine`, group `execution.proj.pierce`) | 2 |
| `m4_aegis` | Bulwark (`m8_bulwark`, group `survivor.aegis`); Riposte (`m8_riposte`, group `survivor.aegis`) | 2 |
| `m4_crimson` | Blood Debt (`m8_blooddebt`, group `survivor.crimson`); Wound Echo (`m8_woundecho`, group `survivor.crimson`) | 2 |
| `m4_renewal` | Deep Roots (`m8_deeproots`, group `survivor.renewal`); Stillness Bloom (`m8_stillbloom`, group `survivor.renewal`) | 2 |
| `m8_blooddebt` | Ironblood (`m16_ironblood`, group `survivor.crimson.debt`); Thirst Unbound (`m16_thirstunbound`, group `survivor.crimson.debt`) | 2 |
| `m8_bulwark` | Aegis Prism (`m16_aegisprism`, group `survivor.aegis.bulwark`); Iron Vow (`m16_ironvow`, group `survivor.aegis.bulwark`) | 2 |
| `m8_deeproots` | Green Reservoir (`m16_greenreservoir`, group `survivor.renewal.deep`); Verdant Earth (`m16_verdantearth`, group `survivor.renewal.deep`) | 2 |
| `m8_frenzy` | Fever (`m16_frenzy_deep`, group `execution.rate`); Rooted (`m16_frenzy_settled`, group `execution.rate`) | 2 |
| `m8_overload` | Bloodied (`m16_overload_bloodied`, group `execution.dmg`); Overdrive (`m16_overload_deep`, group `execution.dmg`) | 2 |
| `m8_riposte` | Riposte Call (`m16_ripostecall`, group `survivor.aegis.riposte`); Stone Vow (`m16_stonevow`, group `survivor.aegis.riposte`) | 2 |
| `m8_stillbloom` | Deep Stillness (`m16_deepstill`, group `survivor.renewal.still`); Verdant Heart (`m16_verdantheart`, group `survivor.renewal.still`) | 2 |
| `m8_tempest` | Skewer (`m16_tempest_pierce`, group `execution.proj`); Tempest Deep (`m16_tempest_deep`, group `execution.proj`) | 2 |
| `m8_woundecho` | Red Mend (`m16_redmend`, group `survivor.crimson.echo`); Warmblood (`m16_warmblood`, group `survivor.crimson.echo`) | 2 |

**Totals:** 196 upgrades (88 normal, 52 unique, 56 milestones), 143 of which take an item slot.


---

### Upgrade kinds

| Kind       | When it is offered                                                     |
|------------|-----------------------------------------------------------------------|
| normal     | Any level-up, subject to `max_stacks`                                  |
| unique     | One-time, violet card. Either global, or scoped to one weapon          |
| milestone  | Only on power-of-two levels (4, 8, 16, ...); replaces the whole choice |

There is no separate weapon kind. A weapon card is a card with a `Weapon` set: it
is only drawn while that weapon is equipped, and it edits that slot alone. Every
weapon in the game has at least two of them, so no weapon is a dead slot -- see
`No weapon in the game is a dead slot` in `tests/test_game.cpp`, which fails the
build if a weapon is added with fewer.

---

## In-game manual

Shown in the game with **F1** (main menu, live run, or the pause screen). **18 pages:**

- `controls` — CONTROLS (17 lines)
- `run` — THE RUN (18 lines)
- `levels` — LEVEL-UPS (21 lines)
- `weapons` — WEAPONS (19 lines)
- `every weapon` — EVERY WEAPON (22 lines)
- `every evolution` — EVERY EVOLUTION (33 lines)
- `every super` — EVERY SUPER (18 lines)
- `chests` — CHESTS (22 lines)
- `the unique rules` — THE UNIQUE RULES (23 lines)
- `the four marks` — THE FOUR MARKS (19 lines)
- `evolutions` — EVOLUTIONS (21 lines)
- `abilities` — ABILITIES JKL (23 lines)
- `stats` — YOUR STATS (32 lines)
- `cards` — CARDS (33 lines)
- `milestones` — MILESTONES (39 lines)
- `enemies` — ENEMIES (30 lines)
- `sandbox` — TEST SANDBOX (19 lines)
- `profile` — PROFILE (17 lines)

### CONTROLS

```
    > WASD / ARROWS   MOVE
    > WEAPONS FIRE THEMSELVES

    J    PHASE DASH - BLINK
    K    OVERLOAD - RADIAL BLAST
    L    STASIS - SLOW THE WORLD
    H    HEAL 50% OF MAX HP

    1-5  PICK A CARD ON LEVEL-UP
    R    REROLL THE CARD CHOICE
    SPACE  CONTINUE AFTER A CHEST OPENS
    ESC  PAUSE / CHARACTER SHEET
    B    BESTIARY (WHILE PAUSED)
    F1   THIS MANUAL
    T    WEAPON TEST SANDBOX

    R    RESTART AFTER DEATH
```

### THE RUN

```
    A RUN IS A LONG FIGHT AGAINST THE CLOCK.

    > YOU START WITH NO WEAPON.
    > THE FIRST SCREEN OFFERS 3 STARTERS.
    > PICK ONE AND THE RUN BEGINS.

    EVERY WEAPON FIRES AUTOMATICALLY AT
    THE NEAREST ENEMY. ALL OWNED WEAPONS
    SHARE THAT TARGET, SO THE WHOLE
    ARSENAL FOCUSES THE BIGGEST THREAT.

    KILLS DROP XP GEMS. XP LEVELS YOU UP.
    EACH LEVEL-UP OFFERS CARDS - SEE THE
    CARDS PAGE.

    SURVIVE. PACKS COME FASTER AFTER HALF
    A MINUTE, AGAIN AFTER TWO AND A HALF,
    AND NEVER STOP.
```

### LEVEL-UPS

```
    EVERY LEVEL-UP OFFERS A CHOICE.

    # THE THREE SOURCES
    > UPGRADE   STATS AND WEAPON CARDS
    > WEAPON    A NEW WEAPON FOR A SLOT
    > UNIQUE    A VIOLET ONE-TIME TREASURE

    YOU GET 1 FREE REROLL PER LEVEL-UP.
    PRESS R TO SPEND IT AND REDRAW.

    # IF NOTHING APPLIES
    A NORMAL LEVEL-UP ALWAYS HAS AT LEAST
    ONE USABLE CARD. A LEVEL-UP WITH NO
    LEGAL CARD STILL COMPLETES, SO THE RUN
    CAN NEVER GET STUCK ON THE SCREEN.

    # WEAPON SLOTS
    YOU START WITH 4 SLOTS. ARSENAL CORE
    ADDS UP TO 3 MORE, AND HOLLOW CHAMBER
    IS THE EIGHTH. THE WEAPON OFFER STOPS
    ONCE THE ARSENAL IS FULL.
```

### WEAPONS

```
    32 WEAPONS: 18 BASE, 10 EVOLUTIONS,
    4 SUPER EVOLUTIONS.

    # EVERY WEAPON IS A DIFFERENT SHAPE
    AIMED PROJECTILE  CROSSBOW, RAIL RIFLE
    INSTANT CONE      EMBER SPRAYER, DRILL
    AREA BOMB         RUNIC HAMMER, MORTAR
    HITSCAN BEAM      SOLAR LANCE
    ORBITING BLADES   VOID GYRE, HALO
    EXPANDING RING    VOID NOVA, SUNDER
    TAUNTING BEACON   GRAVE BELL
    GROUND ZONE       INFERNO, ASHFALL

    ORBITING BLADES ALSO GRIND THE
    INSIDE OF THEIR RING, SO NOTHING
    HUGS YOU FOR FREE.

    THE NEXT THREE PAGES NAME ALL 32 AND
    GIVE THE ONE RULE EACH ONE PLAYS BY.
```

### EVERY WEAPON

```
    # THE 18 BASE WEAPONS
    EACH LINE IS THE ONE RULE THAT MAKES IT DIFFERENT. THE NUMBERS GO UP;
    THE RULE NEVER DOES.

    ARCANE WAND       FAST FLAT BOLTS AT THE NEAREST ENEMY. THE BASELINE, DONE RIGHT.
    THROWING DAGGER   ORBITING KNIVES THAT ALSO GRIND THE INSIDE OF THEIR OWN RING.
    HEAVY CROSSBOW    SLOW HOMING BOLT THAT PIERCES FOUR AND KEEPS GOING.
    EMBER SPRAYER     INSTANT CONE OF FIRE. NO PROJECTILE, SO NOTHING TO MISS.
    RUNIC HAMMER      ARCING BOMB. BIG BLAST, BIG KNOCKBACK, SLOW TO REARM.
    STORM SHURIKEN    BOOMERANG. HITS GOING OUT AND COMING BACK.
    VOID ORB          ONE ETERNAL ORB THAT HUNTS. PROJECTILES GROW IT, NOT COUNT.
    SOUL SCYTHE       REAPS A FULL CIRCLE AROUND ITS NEAREST PREY.
    SOLAR LANCE       HITSCAN BEAM. SLOW TO SWING, DELETES A WHOLE LINE AT ONCE.
    RAIL RIFLE        ONE HYPERVELOCITY SLUG. PUNCHES THROUGH AN ENTIRE RANK.
    FROST SHARDS      A TIGHT VOLLEY OF FAST SHARDS. SHREDS, AND CHILLS.
    SIEGE MORTAR      FIRES OVER THE CROWD ON PURPOSE AND COOKS WHAT IS BEHIND IT.
    PINBALL PUCK      A WHITE-HOT PUCK THAT RICOCHETS BETWEEN BODIES UNTIL IT BURNS OUT.
    GRAVE BELL        PLANTS A BELL THAT HAULS THE HORDE IN, AND FIGHTS WHERE IT STANDS.
    JACKHAMMER DRILL  BITES ONE TARGET AND CHEWS. THE DEEPER IT STAYS BURIED, THE HARDER IT BITES.
    SHOCK CORE        A RING OF PRESSURE THAT BLOWS OUTWARD FROM WHERE YOU ARE STANDING.
    BARBED WHIP       LASHES THE ARC IN FRONT OF YOU, WHETHER OR NOT ANYTHING IS IN IT.
    TESLA COIL        LIGHTNING THAT KEEPS JUMPING. ONE TARGET IS NEVER ENOUGH.
```

### EVERY EVOLUTION

```
    # THE 10 TWO-INGREDIENT EVOLUTIONS
    OWN BOTH INGREDIENTS AND THE RESULT BECOMES THE NEXT WEAPON OFFER. IT REPLACES
    THE RANDOM WEAPON GRANT RATHER THAN JOINING IT.

    STORM CALLER      WAND + CROSSBOW
                      THE BOLT KEEPS THE PUNCH-THROUGH BUT SPENDS IT ON STEERING. IT DRIVES
                      INTO A BODY AND BENDS ONTO THE NEXT ONE, SO IT NEVER LEAVES THE PACK.
    VOID NOVA         ORB + HAMMER
                      A RING CAST WIDE THAT RUSHES BACK IN, DRAGGING THE HORDE TO A KNOT,
                      THEN DETONATES ON TOP OF THEM.
    INFERNO           FLAME + SCYTHE
                      REAPS A CIRCLE AND LEAVES BURNING GROUND BEHIND IT.
    PULSAR            BEAM + SHURIKEN
                      A LIGHT CHAKRAM THAT BURNS THE WHOLE LINE IT FLIES, OUT AND BACK, NOT
                      JUST THE BLADE TIP. IT CARVES CORRIDORS.
    RADIANT HALO      DAGGER + BEAM
                      BLADES OF LIGHT WALK A SLOW CIRCLE AROUND YOU, WITH A RING OF SAFE
                      GROUND AT YOUR FEET. THEY CUT WHAT THEY CROSS, NOT WHAT HUGS YOU.
    BLIZZARD RAIL     RAIL RIFLE + FROST SHARDS
                      THE SLUG SHATTERS ON CONTACT INTO A FAN OF SHARDS, AND EACH SHARD
                      THEN HOPS ON ITS OWN.
    ASHFALL           MORTAR + GRAVE BELL
                      THE BARRAGE FALLS ON THE BELL, SO THE HORDE IT GATHERED IS THE
                      HORDE IT COOKS.
    CHAOS SPHERE      PINBALL + VOID ORB
                      IT COMES APART. EACH IMPACT THROWS FRAGMENTS, AND EACH FRAGMENT
                      BREAKS AGAIN.
    SUNDERING CORE    SHOCK CORE + SOUL SCYTHE
                      ONE HUGE CRESCENT TEARS OUT OF YOU AND KEEPS GOING, SHOVING THE
                      WHOLE FRONT RANK DOWNRANGE.
    TIDAL LASH        WHIP + SHURIKEN
                      THREE NARROW HOOKS COME AROUND IN A FAN, EACH ONE DRAGGING ITS
                      CATCH INTO THE NEXT.
```

### EVERY SUPER

```
    # THE 4 SUPER EVOLUTIONS
    THREE INGREDIENTS INSTEAD OF TWO. A SUPER IS NOT A LARGER EVOLUTION: EACH ONE IS A
    THIRD THING THAT NEITHER OF ITS PARENTS COULD DO ALONE.

    VOID GYRE         DAGGER + SCYTHE + VOID ORB
                      SUCTION WELLS HOLD WHAT THEY CATCH, AND THEIR DAMAGE IS A FUNCTION
                      OF HOW MANY THEY HOLD. FEEBLE ALONE, ENORMOUS IN A KNOT. MORE
                      PROJECTILES MEANS MORE WELLS.
    PRISM ARRAY       FLAME + LANCE + CROSSBOW
                      ONE LOCKED BEAM PER PROJECTILE, EACH ON A DIFFERENT ENEMY. A CROWD
                      GETS SHREDDED FROM SEVERAL ANGLES AT ONCE.
    EVENT HORIZON     RAIL RIFLE + SHOCK CORE + EMBER SPRAYER
                      FOUR WELLS CIRCLE YOU, SWALLOW THE HORDE, THEN COLLAPSE. EACH
                      IMPLOSION IS A BLAST AND THE WELL REOPENS ACROSS THE ORBIT.
    HOARFROST WAKE    FROST SHARDS + VOID ORB
                      THE SHARDS DRAG A FREEZING, GRINDING BUBBLE BEHIND THEM.
                      EVERYTHING THE VOLLEY PASSES WALKS IN SLOW AND COMES APART, HIT
                      OR NOT.
```

### CHESTS

```
    EVERY ELITE, CHAMPION AND OVERLORD DROPS ONE. WALK OVER IT - A CHEST IS A
    PICKUP, NEVER A BUTTON. IT IS A GLOWING BALL WITH SMALLER BALLS ORBITING IT,
    ONE PER CARD INSIDE, AND DEEP CACHE PUTS MORE BALLS IN THE RING.

    # WHAT IS INSIDE
    CARDS THAT IMPROVE THE WEAPONS YOU OWN AND THE BUILD AROUND THEM. NOT LEVEL-UPS
    AND NOT NEW WEAPONS: A CORPSE MUST NOT BE ABLE TO ASK YOU A QUESTION.
    THE BOX LEANS ON THE WEAPON IT HAS TOUCHED LEAST, SO A HAND NEVER LANDS ON ONE
    GUN FOUR TIMES.

    # HOW MANY
    > ELITE      1 CARD.    THE SMALL CHANGE.
    > CHAMPION   3 CARDS.   A WHOLE HAND.
    > OVERLORD   7 CARDS.   MOST OF THE ARSENAL AT ONCE.
    DEEP CACHE ADDS ONE MORE CARD TO EVERY BOX IN THE GAME. THE SPHERES AROUND
    THE BOX ARE THE CARDS IT WILL GIVE YOU, SO COUNT THEM BEFORE YOU TOUCH IT.

    # READING IT
    OPENING A BOX STOPS THE GAME AND PUTS A PANEL ON SCREEN NAMING EVERY CARD IT
    GAVE, IN THE COLOUR OF THE TIER THAT DROPPED IT. PRESS SPACE WHEN YOU HAVE
    READ IT AND THE RUN CARRIES ON. THE CARDS ARE ALREADY YOURS; THE PANEL IS
    ONLY SO YOU CAN SEE WHAT WENT WHERE.
```

### THE UNIQUE RULES

```
    THESE ARE NOT BIGGER NUMBERS. EACH ONE CHANGES HOW SOMETHING WORKS, AND YOU GET
    IT EXACTLY ONCE.

    IGNITED CARAPACE  GETTING HIT DETONATES A BURST FOR 3X THE DAMAGE YOU TOOK.
    SPREADSHOT        DOUBLE VOLLEY SPREAD AND FIRE RATE, BUT SHOTS SPRAY WIDE.
    GAMBLERS EYE      +1 CARD IN EVERY FUTURE LEVEL-UP CHOICE.
    SECOND CHANCE     +1 FREE REROLL AT EVERY LEVEL-UP.
    ARSENAL CORE      +1 WEAPON SLOT.
    DEEP CACHE        EVERY CHEST OPENS ONE MORE TIME.
    SINGULARITY       EVERY 12S, VIOLENTLY YANKS NEARBY ENEMIES TOWARD YOU.
    STORM BOLT        EVERY 3RD PROJECTILE HIT CHAINS LIGHTNING TO 3 NEARBY ENEMIES.
    BLOOD PRICE       EVERY 20 KILLS DETONATES A BURST AROUND YOU.
    COLD BLOOD        ENEMIES THAT HIT YOU ARE SLOWED FOR 2S.
    LAST STAND        DROPPING BELOW 20% HP GRANTS A SECOND OF INVULNERABILITY.
    ADRENALINE        BELOW 30% HP: +60% SPEED, AND 1S INVULNERABLE EVERY 20S.
    REPULSION FIELD   ENEMIES THAT STRIKE YOU ARE VIOLENTLY KNOCKED AWAY.
    VAMPIRIC HEART    LIFESTEAL HEALS 2 HP PER PROC INSTEAD OF 1.

    # THE FOUR THAT TOUCH EVERY WEAPON YOU OWN
    WHETSTONE         ALL OF THEM HIT 8% HARDER.
    OILED GEARS        ALL OF THEM FIRE 7% FASTER.
    LONG BARREL       ALL OF THEM REACH 10% FURTHER, AND THEIR SHOTS LIVE 10% LONGER.
    HEAVY STOCK       EVERYTHING ALL OF THEM HIT IS THROWN 20% FURTHER.
```

### THE FOUR MARKS

```
    EVERY HIT YOU LAND CARRIES ALL FOUR. THEY STACK ON A BODY, SO SOMETHING YOU KEEP
    BEATING FALLS APART INSTEAD OF FIGHTING BACK AT FULL STRENGTH.

      FROSTBIND     EVERY HIT CHILLS WHAT IT STRIKES FOR 2.5S.
      EMBERBRAND    EVERY HIT SETS IT ALIGHT FOR 22 BURNING DAMAGE A SECOND.
      HEX           EVERY HIT MAKES THAT BODY TAKE 14% MORE DAMAGE, UP TO +84%.
      ARMOUR SPLIT  EVERY HIT STRIPS 22 OF ITS OWN ARMOUR, AND IT STAYS STRIPPED.

    # WHERE THEY COME FROM
    THEY ARE A MILESTONE GROUP, WHICH MEANS
    ALL FOUR APPEAR ON THE SAME SCREEN AND
    TAKING ONE CLOSES THE OTHER THREE FOR
    THE REST OF THE RUN. THE ONE YOU TOOK
    KEEPS STACKING; THE OTHERS KEEP DROPPING.

    A MILESTONE SCREEN REPLACES THE WHOLE
    CHOICE AT EVERY POWER-OF-TWO LEVEL FROM 4
    ON, AND CAN CARRY TWO GROUPS AT ONCE - SO
    YOU PICK BOTH THE SUBJECT AND THE CARD.
```

### EVOLUTIONS

```
    OWN ALL THE INGREDIENTS AND THE RESULT
    BECOMES THE NEXT WEAPON OFFER - IT
    REPLACES THE RANDOM WEAPON GRANT.

    # TWO INGREDIENTS
    STORM        WAND + CROSSBOW
    NOVA         ORB + HAMMER
    INFERNO      FLAME + SCYTHE
    PULSAR       LANCE + SHURIKEN
    HALO         DAGGER + LANCE
    BLIZZARD     RAIL RIFLE + FROST SHARDS
    ASHFALL      MORTAR + GRAVE BELL
    CHAOS        PINBALL + ORB
    SUNDER       SHOCK CORE + SCYTHE
    TIDAL LASH   WHIP + SHURIKEN

    # THREE INGREDIENTS = SUPER EVOLUTION
    VOID GYRE        DAGGER + SCYTHE + ORB
    PRISM ARRAY      FLAME + LANCE + CROSSBOW
    EVENT HORIZON    RAIL RIFLE + SHOCK + FLAME
    HOARFROST WAKE   FROST SHARDS + ORB
```

### ABILITIES JKL

```
    THREE BUTTONS, LIVE FROM THE FIRST
    SECOND OF EVERY RUN. NO UNLOCK, NO
    CARD - ONLY A COOLDOWN STANDS IN THE
    WAY. A BUILD CHANGES HOW THEY FEEL,
    NEVER WHETHER THEY EXIST.

    # J  PHASE DASH - 5S
    TELEPORT ALONG YOUR MOVEMENT, OR AT
    THE NEAREST ENEMY IF YOU STAND STILL.
    GRANTS INVULNERABILITY ON ARRIVAL.

    # K  OVERLOAD - 14S
    A RADIAL BLAST. DAMAGE AND KNOCKBACK
    TO EVERYTHING WITHIN REACH.

    # L  STASIS - 30S
    THE WORLD RUNS AT 35% SPEED FOR A FEW
    SECONDS. YOU DO NOT. ENEMIES AND THEIR
    SHOTS TICK ON A SLOWED CLOCK.

    NORMAL CARDS SCALE ALL THREE, SO A BUILD
    CAN GROUND OUT THE COOLDOWNS INSTEAD OF
    WAITING TO ROLL THE RIGHT UNIQUE.
```

### YOUR STATS

```
    # SURVIVAL
    MAX HP     RAISED BY CARDS, HEALS YOU
               BY THE SAME AMOUNT.
    REGEN      FLAT HP EVERY SECOND.
    DEFENSE    ONE NUMBER THAT MITIGATES
               DAMAGE. FLAT, THEN %. ARMOUR
               PIERCE CARDS STRIP IT OFF A
               BODY FOR GOOD, PERMANENTLY.
    SHIELD     ABSORBS BEFORE HP AND
               REGENERATES OUT OF COMBAT.
               A POOL AND A CLOCK: CARDS
               GROW THE POOL, SPEED UP THE
               REFILL AND SHORTEN THE WAIT.
    LIFESTEAL  CHANCE TO HEAL ON A KILL,
               NOT ON EVERY HIT.

    # OFFENCE
    DAMAGE     MULTIPLIES EVERY WEAPON.
    FIRE RATE  ADDITIVE - A HIGHER BONUS
               DIVIDES THE DELAY.
    PIERCE     EXTRA TARGETS PER BOLT, AND
               IT CANCELS AREA FALLOFF.
    PROJECTILES  ONE CARD SERVES EVERY
               WEAPON AT ONCE.

    # THE KILL CHAIN
    KEEP KILLING AND A METER FILLS. STOP
    OR GET HIT AND IT COLLAPSES. EACH STACK
    ADDS DAMAGE AND FIRE RATE. CARDS CAN
    ALSO SCALE THE RATE PER STACK, HOW LONG
    THE CHAIN MAY GROW, AND HOW MANY
    STACKS EACH KILL ADDS.
```

### CARDS

```
    # NORMAL CARDS
    STAT AND WEAPON CARDS. STACKABLE, AND
    A WEAPON CARD ONLY APPEARS WHILE THAT
    WEAPON IS EQUIPPED.

    # ITEM SLOTS
    THE PLAIN STAT CARDS SHARE A BAR THAT
    OPENS ONE SLOT AT A TIME:

    L1:1 L2:2 L4:3 L8:4 L16:5 L32:6 L64:7 L128:8

    ENGAGING A NEW AXIS SPENDS A SLOT. RE-
    TAKING A CARD YOU ALREADY HOLD IS FREE:
    THE BAR LIMITS HOW MANY AXES YOU ENGAGE,
    NEVER HOW DEEP YOU LEAN INTO ONE. WEAPON
    CARDS, TREASURES AND MILESTONES COST NO SLOT.

    # VIOLET UNIQUES
    ONE-TIME, RULE-CHANGING, RARE. EVERY
    WEAPON HAS AT LEAST ONE OF ITS OWN, SO
    A NEW WEAPON IS NEVER A DEAD SLOT. FIVE
    MORE RETUNE THE J/K/L ABILITIES - NONE
    OF THEM UNLOCKS AN ABILITY.

    # VIOLET MILESTONES
    A WHOLE SCREEN AT EVERY POWER-OF-TWO
    LEVEL FROM 4 ON, PICKED IN EXCLUSIVE
    GROUPS. SEE THE MILESTONES PAGE.

    # THE POOL NEVER DRIES UP
    WHEN EVERY AXIS IS SPENT AND MAXED A
    TREASURE IS OFFERED FOR CERTAIN, SO A
    LEVEL-UP IS NEVER JUST A CONTINUE.
```

### MILESTONES

```
    # WHAT THEY ARE
    EVERY POWER-OF-TWO LEVEL FROM 4 ON REPLACES
    THE WHOLE LEVEL-UP CHOICE WITH A VIOLET
    SCREEN OF FOUR CARDS. A CARD DRAWN TWICE IN A
    RUN IS DEAD; A MILESTONE DRAWN TWICE IS A
    PITY. THE FOURTH SLOT IS ALWAYS A CONTINUE.

    # GROUPS CLOSE. PERMANENTLY.
    CARDS ARRIVE IN GROUPS AND THE WHOLE GROUP
    IS ON SCREEN AT ONCE, SO YOU CHOOSE AGAINST
    THE ALTERNATIVES RATHER THAN AGAINST
    NOTHING. TAKE ONE AND THE REST OF THAT GROUP
    IS SHUT FOR THE WHOLE RUN - NOT GREYED OUT
    UNTIL NEXT TIME. SHUT.

    # A MILESTONE MULTIPLIES ITS AXIS
    RATHER THAN ADDING A FLAT NUMBER TO IT, SO IT
    IS WORTH MORE THE MORE OF THAT AXIS YOU BUILT.
    A FLAT NUMBER IS WORTH THE SAME TO A RUN THAT
    TOOK ONE CARD ON AN AXIS AS TO ONE THAT TOOK
    EIGHT: A REWARD FOR BEING AT A LEVEL, NOT FOR
    HAVING BUILT SOMETHING.

    IT ALSO SEEDS THE AXIS, BECAUSE A PURE
    MULTIPLIER ON ZERO IS ZERO - SO IT IS WORTH
    SOMETHING EVEN ON A RUN THAT NEVER TOUCHED
    THE AXIS, AND IT PRINTS THE TOTAL YOU GET.

    # THE DEEP ONES ARE BRANCHES
    A TREE, THREE TIERS: TAKE ONE, GET ASKED A
    FOLLOW-UP, TAKE THAT, GET ASKED AGAIN. A
    BRANCH ONLY APPEARS IF YOU TOOK THE CARD
    ABOVE IT, SO YOU ARE NEVER SHOWN THE REGEN
    QUESTION ON THE VAMPIRE LINE. AND A BRANCH
    ASKS A DIFFERENT QUESTION INSTEAD OF
    REPEATING THE FIRST ONE, BECAUSE THE FOURTH
    TAKE OF A FLAT BONUS IS ARITHMETIC. EVERY
    BRANCH IS A TWO-WAY QUESTION, AND THE SIDE
    YOU DID NOT TAKE IS SHUT FOR THE RUN.
```

### ENEMIES

```
    18 TYPES WALK IN AND DEAL CONTACT
    DAMAGE. THEY SCALE WITH RUN TIME:
    MORE HP, FASTER, AND HARDER HITS.

    # ELITES AND ABOVE
    THEY ARE EVENTS, NOT A SHARE OF THE PACK.
    EACH WALKS IN ON ITS OWN CLOCK:

    ELITE     FROM 1:30, THEN EVERY 1:40-2:10.
              ONE TRAIT.  HP x5.  BOX OF 1.
    CHAMPION  AFTER 4 ELITES, THEN EVERY
              4:10-5:20.  THREE TRAITS.
              HP x25.  BOX OF 3.
    OVERLORD  AFTER 2 CHAMPIONS, THEN EVERY
              7:50-9:50.  SEVEN TRAITS.
              HP x125.  BOX OF 7.

    AN ARRIVAL IS ONE OR TWO BODIES, OR
    THREE OR FOUR ONCE YOU ARE HANDLING
    THAT TIER RATHER THAN MEETING IT. AT
    MOST 4, 2 AND 1 ALIVE AT A TIME. EVERY
    TIERED BODY DROPS A BOX.

    THE TRIBUNAL DIRECTOR PROMOTES YOU BY
    COUNTING KILLS, AND IT NEVER TAKES IT
    BACK. KILL ONE OF EACH TIER TO UNLOCK
    ITS OUTLINE.

    PRESS B WHILE PAUSED FOR THE FULL
    BESTIARY, INCLUDING TRAITS AND SCALING.
```

### TEST SANDBOX

```
    PRESS T MID-RUN TO OPEN IT.

    # WHAT IT DOES
    > SNAPSHOTS THE WHOLE RUN AND ROLLS
    > IT BACK WHEN YOU LEAVE.
    > CYCLES ALL 32 WEAPONS, EVOLUTIONS
    > AND SUPERS INCLUDED.
    > MAX BUILD BOOST, ITEM PICKER,
    > IMMORTALITY, A DIFFICULTY CLOCK.

    1/2 WEAPON   3 MAX BUILD   4 WAVES
    5 CLOSE      E ITEMS      I GOD
    F CLOCK      X KILL ME    R MAX ALL

    # IT IS NOT A CHEAT
    IT PAYS NO XP. IT NEVER PAYS A SKIN OR
    OUTLINE UNLOCK. AND LEAVING IT ENDS THE
    RUN - THE SNAPSHOT IS RESTORED, THEN
    YOU ARE DROPPED TO 0 HP.
```

### PROFILE

```
    THE MAIN MENU KEEPS YOUR SKIN AND THE
    OUTLINES YOU HAVE EARNED. IT IS SAVED
    NEXT TO THE GAME DATA AS A PLAIN TEXT
    FILE, SO A COPIED BUILD KEEPS ITS
    PROGRESS WITH IT.

    OUTLINES UNLOCK BY KILLING ONE ELITE,
    ONE CHAMPION AND ONE OVERLORD - EVER,
    ACROSS ALL RUNS.

    THE MAIN MENU ALSO HAS A RESET
    PROGRESS ROW, WHICH WIPES THE SKIN AND
    EVERY UNLOCK. IT ASKS FIRST.

    THE FULL MECHANICS REFERENCE, WITH
    EVERY FORMULA AND NUMBER, LIVES IN
    DOCS/MECHANICS.MD IN THE PROJECT.
```
