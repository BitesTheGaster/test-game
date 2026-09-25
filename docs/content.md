<!-- GENERATED FILE — do not edit by hand.
     Source: assets/data/*.toml, generator: tools/gendocs.py -->

# Content Reference

All numbers below are read directly from the TOML files in `assets/data/`.
Regenerate with:

```sh
python3 tools/gendocs.py
```

**Totals:** 32 weapons (18 base +
10 evolutions +
4 super evolutions),
18 enemies, 11 in-game manual pages.

### Base weapons

| Name | ID | Attack | Damage | Cooldown (s) | Projectiles | Proj. speed | Pierce | Spread (rad) | Starter | Traits | Description |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Arcane Wand | wand | projectile | 8 | 0.5 | 1 | 14 | 0 | 0.1 | yes |  | Reliable magic bolts. The balanced baseline. |
| Throwing Dagger | dagger | orbit | 5 | 0.4 | 3 | 0 | 0 | 0 | yes |  | Orbiting knives that carve up anything close. |
| Heavy Crossbow | crossbow | projectile | 20 | 2.2 | 1 | 22 | 4 | 0.18 | yes | homing | Slow, homing piercing bolt that punches through crowds. |
| Ember Sprayer | flame | cone | 4 | 0.35 | 1 | 0 | 0 | 0 |  |  | Cone of fire — instant damage in a wide arc. |
| Runic Hammer | hammer | bomb | 45 | 1.8 | 1 | 8 | 0 | 0.3 |  |  | Arcing bomb with massive explosion and knockback. |
| Storm Shuriken | shuriken | boomerang | 8 | 0.6 | 2 | 16 | 1 | 0.2 |  |  | Boomerang blades — hit going out AND coming back. |
| Void Orb | orb | bounce | 35 | 1.5 | 1 | 7 | 5 | 0 |  | eternal | One eternal orb that hunts forever. Projectiles grow it, not multiply. |
| Soul Scythe | scythe | sweep | 55 | 1.3 | 1 | 0 | 0 | 0 |  |  | Reaps a full circle of death around its nearest prey. |
| Solar Lance | beam | beam | 130 | 1.6 | 1 | 0 | 0 | 0 |  |  | Instant hitscan beam. Slow to swing, but it deletes a whole line at once. |
| Rail Rifle | railgun | projectile | 78 | 1.8 | 1 | 40 | 6 | 0 |  |  | One hypervelocity slug. Slow to load, punches through an entire rank. |
| Frost Shards | shard | projectile | 12 | 0.45 | 4 | 20 | 0 | 0.34 |  |  | A tight volley of fast shards that shreds whatever walks into it. |
| Siege Mortar | mortar | bomb | 62 | 2.4 | 1 | 9 | 0 | 0.22 |  | fused | Lobs a shell over the crowd. It ignores whatever it flies over and cooks where it lands. |
| Pinball Puck | pinball | bounce | 20 | 1.6 | 1 | 13 | 2 | 0 |  |  | A white-hot puck that keeps ricocheting between bodies until it burns out. |
| Grave Bell | lure | lure | 0 | 2.6 | 1 | 0 | 0 | 0 |  | taunt | Plants a bell that hauls the horde into its core. It fights from where it stands, not from where you stand. |
| Jackhammer Drill | drill | cone | 7 | 0.55 | 1 | 0 | 0 | 0 |  |  | A narrow, extremely fast cone of steel. Point blank, nothing survives it. |
| Shock Core | shockcore | nova | 20 | 2.2 | 1 | 0 | 0 | 0 |  |  | A ring of pressure that blows itself outward from where you are standing. |
| Barbed Whip | whip | sweep | 26 | 0.7 | 1 | 0 | 0 | 0 |  | lead-lash | Lashes the arc in front of you, whether or not anything is standing in it. |
| Tesla Coil | tesla | chain | 11 | 0.65 | 1 | 0 | 0 | 0 |  |  | Lightning that keeps jumping. One target is never enough. |

### Evolutions (A + B = C)

| Name | ID | Attack | Requires | Damage | Cooldown (s) | Projectiles | Pierce | Area | Traits | Description |
|---|---|---|---|---|---|---|---|---|---|---|
| Storm Caller | storm | chain | wand + crossbow | 15 | 0.45 | 1 | 0 | 0 |  | Wand + Crossbow. Lightning chains between enemies. |
| Void Nova | nova | nova | orb + hammer | 40 | 1.8 | 1 | 0 | 0 |  | Orb + Hammer. Expanding ring of destruction. |
| Inferno | inferno | inferno | flame + scythe | 60 | 1.6 | 1 | 0 | 0 |  | Sprayer + Scythe. Reaps a circle and leaves burning ground. |
| Pulsar | pulsar | pulsar | beam + shuriken | 12 | 0.9 | 1 | 2 | 0 |  | Shuriken + Lance. Light-chakram dragging a burning laser trail. |
| Radiant Halo | halo | halo | dagger + beam | 40 | 1 | 2 | 4 | 0 |  | Dagger + Lance. Blades of light orbit you, reaping all they touch. |
| Blizzard Rail | blizzard | chain | railgun + shard | 22 | 0.5 | 1 | 0 | 0 |  | Rail Rifle + Frost Shards. One slug comes apart mid-flight into a storm of shards that keeps jumping. |
| Ashfall | siege | inferno | mortar + lure | 72 | 1.8 | 1 | 0 | 0 |  | Siege Mortar + Grave Bell. The bell gathers the horde and the shells land inside the crowd it gathered. |
| Chaos Sphere | chaos | bounce | pinball + orb | 30 | 1.6 | 1 | 8 | 0 |  | Pinball + Void Orb. A screaming orb that never stops bouncing, until it wears itself out. |
| Sundering Core | sunder | nova | shockcore + scythe | 55 | 1.6 | 1 | 0 | 0 |  | Shock Core + Soul Scythe. A pressure ring wide enough to reach the far side of a horde, reaping everyone it crosses. |
| Tidal Lash | tidewhip | sweep | whip + shuriken | 34 | 0.55 | 1 | 0 | 0 | lead-lash | Barbed Whip + Storm Shuriken. A wide, fast lash that flings everything it touches back into the crowd. |

### Super evolutions (A + B + C)

| Name | ID | Attack | Requires | Damage | Cooldown (s) | Projectiles | Pierce | Area | Traits | Description |
|---|---|---|---|---|---|---|---|---|---|---|
| Void Gyre | vortex | vortex | dagger + scythe + orb | 40 | 1 | 3 | 4 | 0 |  | Dagger + Scythe + Void Orb. Suction zones circle you and drag prey into their cores. More projectiles = more AND bigger zones (up to a cap). |
| Prism Array | prism | prism | flame + beam + crossbow | 42 | 1.05 | 4 | 2 | 0 | ricochet | Sprayer + Lance + Crossbow. One locked beam per projectile, each on a different enemy — a crowd gets shredded from several angles at once. |
| Seraph Array | seraph | halo | drill + crossbow + whip | 60 | 0.9 | 3 | 6 | 0 |  | Drill + Crossbow + Barbed Whip. Heavy wings of light walk a slow circle around you, cutting and shoving everything they cross. |
| Event Horizon | eventhorizon | vortex | railgun + shockcore + flame | 48 | 1 | 4 | 6 | 0 |  | Rail Rifle + Shock Core + Ember Sprayer. Four gravity wells circle you, dragging the horde in and grinding it against the cores. |

---

## Enemies

| Name | ID | HP | Speed | Touch dmg | Radius | XP | Unlocks at (s) | Weight | Shape |
|---|---|---|---|---|---|---|---|---|---|
| Bat | bat | 8 | 4.2 | 5 | 0.3 | 1 | 0 | 6 | circle |
| Slime | slime | 16 | 1.5 | 6 | 0.4 | 1 | 8 | 5 | circle |
| Spider | spider | 10 | 5 | 6 | 0.28 | 2 | 20 | 4 | rect |
| Zombie | zombie | 34 | 2.1 | 11 | 0.38 | 2 | 25 | 4 | rect |
| Imp | imp | 14 | 5.8 | 8 | 0.26 | 2 | 35 | 4 | circle |
| Skeleton | skeleton | 28 | 3.4 | 9 | 0.34 | 3 | 45 | 4 | rect |
| Chest Mimic | mimic | 70 | 3 | 14 | 0.42 | 5 | 60 | 3 | rect |
| Wraith | wraith | 18 | 5.6 | 9 | 0.3 | 3 | 70 | 3 | circle |
| Ghost | ghost | 45 | 3 | 12 | 0.36 | 4 | 85 | 3 | circle |
| Harpy | harpy | 55 | 5 | 14 | 0.34 | 5 | 100 | 3 | circle |
| Charger | charger | 60 | 6.2 | 16 | 0.44 | 5 | 105 | 3 | rect |
| Brute | brute | 140 | 1.6 | 22 | 0.62 | 8 | 120 | 2 | circle |
| Stalker | stalker | 90 | 4.8 | 15 | 0.4 | 7 | 135 | 3 | circle |
| Abomination | abomination | 200 | 2.6 | 20 | 0.55 | 10 | 150 | 2 | circle |
| Golem | golem | 320 | 1.2 | 30 | 0.75 | 14 | 160 | 2 | rect |
| Juggernaut | juggernaut | 420 | 1.8 | 34 | 0.8 | 18 | 175 | 2 | rect |
| Oracle | oracle | 160 | 4.4 | 24 | 0.46 | 14 | 190 | 2 | circle |
| Reaper | reaper | 240 | 5.4 | 26 | 0.5 | 16 | 200 | 2 | circle |

---

## Upgrades

### Normal pool

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Description |
|---|---|---|---|---|---|---|---|
| Sharpened Bolts | damage | damage_mul | 0.15 | 6 |  |  | +15% damage |
| Battle Haste | haste | fire_rate | 0.09 | 5 |  |  | +9% fire rate |
| Split Shot | multi | proj_add | 1 | 4 |  |  | +1 projectile |
| Impact | impact | knockback_mul | 0.45 | 3 |  |  | +45% knockback from all your attacks and bursts |
| Swift Boots | boots | speed_mul | 0.12 | 2 |  |  | +12% move speed |
| Vigor | vigor | max_hp_add | 20 | 4 |  |  | +20 max HP and heal 20 |
| Soul Magnet | magnet | pickup_mul | 0.6 | 2 |  |  | +60% pickup range |
| Scholar | scholar | xp_mul | 0.12 | 3 |  |  | +12% experience gained |
| Lorekeeper | lorekeeper | xp_mul | 0.3 | 2 |  |  | +30% experience gained |
| Piercing Shots | pierce | pierce_add | 1 | 3 |  |  | +1 pierce |
| Blood Surge | surge_chain | momentum_damage | 1 | 2 |  |  | Kill chain: +1% damage per stack |
| Rampage | rampage | momentum_speed | 4 | 1 |  |  | Kill chain: +4% move speed per stack |
| Deep Reserves | deep_reserves | momentum_window | 2 | 1 |  |  | Kill chain: 2 more seconds before it goes cold |
| Kill Tempo | kill_tempo | momentum_rate | 0.5 | 3 |  |  | Kill chain: +0.5% fire rate per stack |
| Long Tail | long_tail | momentum_chain | 6 | 2 |  |  | Kill chain: 6 more stacks count toward the multipliers |
| Cull | cull | momentum_gain | 0.5 | 2 |  |  | Kill chain: +0.5 stacks per kill |
| Regeneration | regen | regen_add | 1 | 4 |  |  | +1 HP per second |
| Might | might | damage_mul | 0.25 | 4 |  |  | +25% damage |
| Quickdraw | quickdraw | fire_rate | 0.13 | 3 |  |  | +13% fire rate |
| Twin Cast | twin_shot | proj_add | 1 | 2 |  |  | +1 projectile |
| Windrunner | windrunner | speed_mul | 0.08 | 3 |  |  | +8% move speed |
| Iron Constitution | constitution | max_hp_add | 35 | 3 |  |  | +35 max HP and heal 35 |
| Bulwark | bulwark | max_hp_add | 50 | 2 |  |  | +50 max HP and heal 50 |
| Vitality | vitality | regen_add | 2 | 2 |  |  | +2 HP per second |
| Treasure Sense | treasure | pickup_mul | 1 | 1 |  |  | +100% pickup range |
| Harvester | harvester | pickup_mul | 0.8 | 1 |  |  | +80% pickup range |
| Garrote Bolts | garrote | pierce_add | 2 | 1 |  |  | +2 pierce |
| Battle Echoes | echoes | fire_rate | 0.19 | 2 |  |  | +19% fire rate |
| Barrage | barrage | proj_add | 2 | 1 |  |  | +2 projectiles |
| Fervor | fervor | damage_mul | 0.35 | 2 |  |  | +35% damage |
| Gale Steps | gale | speed_mul | 0.15 | 1 |  |  | +15% move speed |
| Heartwood | heartwood | max_hp_add | 70 | 1 |  |  | +70 max HP and heal 70 |
| Surge | surge | fire_rate | 0.25 | 1 |  |  | +25% fire rate |
| Stony Pledge | pledge | defense_add | 10 | 4 |  |  | +10 defense |
| Iron Conviction | conviction | defense_add | 25 | 2 |  |  | +25 defense |
| Minor Ward | ward_small | shield_add | 15 | 3 |  |  | +15 regenerating shield |
| Runic Ward | ward_great | shield_add | 30 | 2 |  |  | +30 regenerating shield |
| Aegis Flow | aegis_flow | shield_regen | 0.6 | 3 |  |  | Shield refills 60% faster, and refills right now |
| Quickdraw | quickdraw_ward | shield_delay | 1 | 2 |  |  | Shield starts refilling 1s sooner after you take a hit |
| Field Kit | field_kit | heal_pct | 0.4 | 2 |  |  | Patch yourself up: heal 40% of your maximum HP |
| Gilded Fangs | leech_gold | lifesteal_add | 4 | 3 |  |  | +4% lifesteal |
| Soulfeed | leech_soul | lifesteal_add | 6 | 2 |  |  | +6% lifesteal |
| Sunder Edge | sunder | armor_pierce_add | 15 | 4 |  |  | +15 armor pierce |
| Keen Sunder | sunder_great | armor_pierce_add | 40 | 2 |  |  | +40 armor pierce |
| Armor-Cracker | armor_cracker | armor_pierce_add | 80 | 1 |  |  | +80 armor pierce |
| Wand Focus | w_wand_power | w_damage_add | 8 | 3 | wand |  | Arcane Wand: +8 damage |
| Wand Channeling | w_wand_speed | w_fire_rate | 0.16 | 2 | wand |  | Arcane Wand: +16% fire rate |
| Dagger Honing | w_dagger_power | w_damage_add | 5 | 3 | dagger |  | Throwing Dagger: +5 damage each |
| Dagger Volley | w_dagger_volley | w_proj_add | 1 | 2 | dagger |  | Throwing Dagger: +1 projectile |
| Crossbow Wit | w_crossbow_power | w_damage_add | 12 | 2 | crossbow |  | Heavy Crossbow: +12 damage |
| Crossbow Drill | w_crossbow_pierce | w_pierce_add | 2 | 2 | crossbow |  | Heavy Crossbow: +2 pierce |
| Ember Intensity | w_flame_power | w_damage_add | 3 | 3 | flame |  | Ember Sprayer: +3 damage |
| Ember Horde | w_flame_volley | w_proj_add | 2 | 2 | flame |  | Ember Sprayer: +2 projectiles |
| Hammer Rune | w_hammer_power | w_damage_add | 20 | 2 | hammer |  | Runic Hammer: +20 damage |
| Hammer Wrath | w_hammer_pierce | w_pierce_add | 3 | 2 | hammer |  | Runic Hammer: +3 pierce |
| Shuriken Storm | w_shuriken_power | w_damage_add | 5 | 3 | shuriken |  | Storm Shuriken: +5 damage |
| Shuriken Cyclone | w_shuriken_speed | w_fire_rate | 0.19 | 2 | shuriken |  | Storm Shuriken: +19% fire rate |
| Rail Tuning | w_railgun_power | w_damage_add | 30 | 2 | railgun |  | Rail Rifle: +30 damage |
| Overcharged Rail | w_railgun_pierce | w_pierce_add | 2 | 2 | railgun |  | Rail Rifle: +2 pierce |
| Shard Whetstone | w_shard_power | w_damage_add | 5 | 3 | shard |  | Frost Shards: +5 damage |
| Shard Barrage | w_shard_volley | w_proj_add | 2 | 2 | shard |  | Frost Shards: +2 projectiles |
| Mortar Charge | w_mortar_power | w_damage_add | 25 | 2 | mortar |  | Siege Mortar: +25 damage |
| Barrage | w_mortar_volley | w_proj_add | 1 | 2 | mortar |  | Siege Mortar: +1 shell per shot |
| Puck Polish | w_pinball_power | w_damage_add | 10 | 3 | pinball |  | Pinball Puck: +10 damage |
| Hot Wheels | w_pinball_speed | w_fire_rate | 0.2 | 2 | pinball |  | Pinball Puck: +20% fire rate |
| Deeper Toll | w_lure_power | w_lure_power | 8 | 3 | lure |  | Grave Bell: +8 damage per second |
| Quick Chime | w_lure_rate | w_fire_rate | 0.18 | 2 | lure |  | Grave Bell: +18% fire rate |
| Drill Bit | w_drill_power | w_damage_add | 3 | 3 | drill |  | Jackhammer Drill: +3 damage per tick |
| Redline | w_drill_rate | w_fire_rate | 0.25 | 2 | drill |  | Jackhammer Drill: +25% fire rate |
| Deeper Charge | w_shockcore_power | w_nova_power | 10 | 3 | shockcore |  | Shock Core: +10 damage per tick |
| Quicken | w_shockcore_rate | w_fire_rate | 0.2 | 2 | shockcore |  | Shock Core: +20% fire rate |
| Barb Wire | w_whip_power | w_damage_add | 12 | 3 | whip |  | Barbed Whip: +12 damage |
| Long Lash | w_whip_wide | w_proj_add | 1 | 2 | whip |  | Barbed Whip: a 15% wider lash per stack |
| Coil Tuning | w_tesla_power | w_damage_add | 5 | 3 | tesla |  | Tesla Coil: +5 damage per jump |
| Arc Cascade | w_tesla_rate | w_fire_rate | 0.22 | 2 | tesla |  | Tesla Coil: +22% fire rate |
| Deep Cold | w_blizzard_power | w_damage_add | 8 | 2 | blizzard |  | Blizzard Rail: +8 damage per jump |
| Whiteout | w_blizzard_rate | w_fire_rate | 0.2 | 2 | blizzard |  | Blizzard Rail: +20% fire rate |
| Thermite | w_siege_power | w_damage_add | 30 | 2 | siege |  | Ashfall: +30 damage to the reap |
| Bombardment | w_siege_rate | w_fire_rate | 0.25 | 2 | siege |  | Ashfall: +25% fire rate |
| Unstable Core | w_chaos_power | w_damage_add | 15 | 2 | chaos |  | Chaos Sphere: +15 damage |
| Ricochet Tuning | w_chaos_rate | w_fire_rate | 0.2 | 2 | chaos |  | Chaos Sphere: +20% fire rate |
| Sunder Charge | w_sunder_power | w_nova_power | 20 | 2 | sunder |  | Sundering Core: +20 damage per tick |
| Faster Ring | w_sunder_rate | w_fire_rate | 0.2 | 2 | sunder |  | Sundering Core: +20% fire rate |
| Deep Current | w_tidewhip_power | w_damage_add | 16 | 2 | tidewhip |  | Tidal Lash: +16 damage |
| Spring Tide | w_tidewhip_rate | w_fire_rate | 0.22 | 2 | tidewhip |  | Tidal Lash: +22% fire rate |
| Wing Polish | w_seraph_power | w_damage_add | 25 | 2 | seraph |  | Seraph Array: +25 damage |
| Winged Host | w_seraph_wings | w_proj_add | 1 | 2 | seraph |  | Seraph Array: +1 wing |
| Singularity Tuning | w_horizon_power | w_damage_add | 20 | 2 | eventhorizon |  | Event Horizon: +20 damage |
| Deeper Well | w_horizon_wells | w_proj_add | 1 | 2 | eventhorizon |  | Event Horizon: +1 gravity well |
| Arsenal Core | u_arsenal_core | weapon_slot_add | 1 | 3 |  |  | +1 weapon slot (3 stacks max) |
| Eventide Hunger | w_orb_hunger | w_orb_grow | 1 | 2 | orb |  | Void Orb: finds the next victim 30% faster and loses far less per bounce |
| Long Arm | w_scythe_longarm | w_scythe_reach | 1 | 3 | scythe |  | Soul Scythe: +6% reap radius |
| Focused Burn | w_beam_focus | w_beam_lance | 1 | 3 | beam |  | Solar Lance: +6% range and +4% width |
| Arc Cascade | w_storm_cascade | w_chain_arc | 1 | 3 | storm |  | Storm Caller: +6% jump range and +5% damage per link |
| Rupture | w_nova_rupture | w_nova_wide | 1 | 3 | nova |  | Void Nova: +6% ring radius and +4% expansion speed |
| Pyre Spread | w_inferno_pyre | w_reap_wide | 1 | 3 | inferno |  | Inferno: +6% reap radius, burning ground lasts 0.5s longer |
| Long Cast | w_pulsar_longcast | w_boomerang_reach | 1 | 3 | pulsar |  | Pulsar: +7% flight range and +6% return speed |
| Pooled Ash | w_flame_pools | w_zone_pools | 1 | 2 | flame |  | Ember Sprayer: one more burning pool on the ground at a time |
| Ashfall Spread | w_inferno_pools | w_zone_pools | 1 | 2 | inferno |  | Inferno: one more burning pool on the ground at a time |
| Concussion Charge | w_hammer_concussion | w_bomb_blast | 1 | 3 | hammer |  | Runic Hammer: +8% blast radius and +6% knockback |
| Wide Shell | w_mortar_concussion | w_bomb_blast | 1 | 3 | mortar |  | Siege Mortar: +8% blast radius and +6% knockback |
| Echo Anchor | w_lure_anchor | w_lure_anchor | 1 | 2 | lure |  | Grave Bell: the beacon lasts 0.8s longer, and one more may be planted |
| Siege Chime | w_lure_anchor_siege | w_lure_anchor | 1 | 2 | siege |  | Ashfall: the beacon lasts 0.8s longer, and one more may be planted |
| Beam Lattice | w_prism_lattice | w_prism_lattice | 1 | 2 | prism |  | Prism Array: one more independent beam, +5% range |
| Long Wings | w_halo_wings | w_halo_wings | 1 | 3 | halo |  | Radiant Halo: +8% beam reach and 1.5 more shove per beam |
| Denser Gyre | w_vortex_core | w_vortex_core | 1 | 3 | vortex |  | Void Gyre: fatter core, wider orbit, 12% faster spin |
| Phase Mirror | phase_mirror | ability_dash | 0.8 | 3 |  |  | Phase Dash: +0.8 distance, +0.06s of invulnerability on arrival |
| Concussion Core | concussion_core | ability_burst | 0.8 | 3 |  |  | Overload: +0.8 radius, +30 damage, +1 knockback |
| Cryostasis | cryostasis | ability_slow | 1 | 3 |  |  | Stasis: +1s of duration, and the slowed world drops another 0.08x |

### Unique items (one-time, rule-changing)

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Description |
|---|---|---|---|---|---|---|---|
| Spreadshot | u_spreadshot | fan | 1 | 1 |  |  | Double volley spread and fire rate, but shots spray +/- 30 degrees |
| Ignited Carapace | u_thorns | thorns | 3 | 1 |  |  | Getting hit detonates a burst dealing 3x incoming damage |
| Gambler's Eye | u_extra_choice | extra_choice | 1 | 1 |  |  | +1 card in every future level-up choice |
| Second Chance | u_reroll | reroll_add | 1 | 1 |  |  | +1 free reroll per level-up (total two) |
| Adrenaline | u_adrenaline | adrenaline | 1 | 1 |  |  | Below 30% HP: +60% speed and a 1s invulnerability every 20s |
| Singularity | u_black_hole | black_hole | 1 | 1 |  |  | Every 12s, violently yanks nearby enemies toward you |
| Storm Bolt | u_chain | chain | 1 | 1 |  |  | Every 3rd projectile hit chains lightning to 3 nearby enemies |
| Blood Price | u_blood_price | blood_price | 1 | 1 |  |  | Every 20 kills detonates a burst around you |
| Cold Blood | u_ice_blood | ice_blood | 1 | 1 |  |  | Enemies that hit you are slowed for 2s |
| Last Stand | u_last_stand | last_stand | 1 | 1 |  |  | Taking a hit below 20% HP grants 1s of invulnerability (20s cooldown) |
| Repulsion Field | u_repulsion | knockback_retaliate | 6 | 1 |  |  | Enemies that strike you are violently knocked away |
| Vampiric Heart | u_vampiric_heart | lifesteal_heal | 2 | 1 |  |  | Lifesteal heals 2 HP per proc instead of 1 |
| Seeking Missiles | uw_wand_seeking | w_unique_homing | 1 | 1 | wand |  | Arcane Wand: bolts home onto the nearest enemy |
| Blade Vortex | uw_dagger_vortex | w_unique_vortex | 1 | 1 | dagger |  | Throwing Dagger: blades spin 2x faster in a 25% wider orbit |
| Fragmenting Bolt | uw_crossbow_fragment | w_unique_area | 1.2 | 1 | crossbow |  | Heavy Crossbow: bolts explode on impact for area damage |
| Hearthfire | uw_flame_hearthfire | w_unique_hearthfire | 1 | 1 | flame |  | Ember Sprayer: cone is 50% wider and 40% longer |
| Cataclysm | uw_hammer_cataclysm | w_unique_cataclysm | 1 | 1 | hammer |  | Runic Hammer: explosions 60% larger with heavier knockback |
| Return Tempest | uw_shuriken_tempest | w_unique_area | 2.5 | 1 | shuriken |  | Storm Shuriken: returning blades detonate a 2.5-area burst |
| Echo Detonation | uw_orb_echo | w_unique_area | 1.5 | 1 | orb |  | Void Orb: every bounce splashes half damage around the hit |
| Reaper's Harvest | uw_scythe_harvest | w_unique_harvest | 3 | 1 | scythe |  | Soul Scythe: sweeps restore 3 HP per kill |
| Bloodthirst | uw_bloodthirst | momentum_bloodthirst | 1 | 1 |  |  | Kill chain: twice the stacks, twice the length, 3s longer before it goes cold |
| Prism Lance | uw_beam_prism | w_unique_prism | 3 | 1 | beam |  | Solar Lance: three beams at once — forward, left and right |
| Thunderlord | uw_storm_thunderlord | w_unique_thunderlord | 4 | 1 | storm |  | Storm Caller: +4 chain jumps and no damage decay |
| Supernova | uw_nova_supernova | w_unique_supernova | 1 | 1 | nova |  | Void Nova: ring expands faster, wider, and hits harder |
| Everflame | uw_inferno_everflame | w_unique_everflame | 1 | 1 | inferno |  | Inferno: wider reap, burning ground lasts longer and burns harder |
| Arc Saw | uw_pulsar_arcsaw | w_unique_arcsaw | 1 | 1 | pulsar |  | Pulsar: the laser trail is 80% wider and deals 35% more damage |
| Magnetic Slug | uw_railgun_slug | w_unique_homing | 1 | 1 | railgun |  | Rail Rifle: the slug homes onto the nearest enemy |
| Deep Toll | uw_lure_bell | w_unique_bell | 1 | 1 | lure |  | Grave Bell: 35% harder pull, 20% wider core and reach, and one more bell at a time |
| Gravitic Field | uw_tesla_gravitic | w_unique_gravitic | 1 | 1 | tesla |  | Tesla Coil: jumps reach 50% further and stop decaying so hard |
| White Squall | uw_blizzard_storm | w_unique_thunderlord | 4 | 1 | blizzard |  | Blizzard Rail: +4 jumps and no damage decay |
| Molten Crater | uw_siege_molten | w_unique_molten | 1 | 1 | siege |  | Ashfall: the burning ground is 80% hotter, 25% wider and lasts much longer |
| Detonation Chain | uw_chaos_echo | w_unique_area | 1.6 | 1 | chaos |  | Chaos Sphere: every bounce splashes area damage around the hit |
| Event Collapse | uw_sunder_supernova | w_unique_supernova | 1 | 1 | sunder |  | Sundering Core: ring expands faster, wider, and hits harder |
| Undertow | uw_tidewhip_lash | w_unique_lash | 1 | 1 | tidewhip |  | Tidal Lash: a 35% wider lash that flings 40% harder and reaches further |
| Corona Mantle | uw_halo_corona | w_unique_corona | 1 | 1 | halo |  | Radiant Halo: beams shove for 6, 40% wider, 15% longer, +20% damage |
| Black Gyre | uw_vortex_gyre | w_unique_gyre | 1 | 1 | vortex |  | Void Gyre: 45% harder pull, 30% further reach, fatter core, ticks faster |
| Total Internal Reflection | uw_prism_refract | w_unique_refract | 1 | 1 | prism |  | Prism Array: one more independent beam, 40% longer ricochet, +15% range |
| Rime Lances | uw_shard_rime | w_unique_rime | 1 | 1 | shard |  | Frost Shards: +4 pierce and 40% longer flight, at 80% damage each |
| Siege Doctrine | uw_mortar_doctrine | w_unique_siege_doctrine | 1 | 1 | mortar |  | Siege Mortar: a 3-shell salvo on a double fuse, 35% wider blasts, 20% slower |
| Silver Skewer | uw_pinball_skewer | w_unique_skewer | 1 | 1 | pinball |  | Pinball Puck: +10 bounces, no damage decay, 30% longer reach per hop |
| Overdrive Bore | uw_drill_bore | w_unique_bore | 1 | 1 | drill |  | Jackhammer Drill: 60% wider bite, 40% longer reach, strikes far faster |
| Standing Discharge | uw_shockcore_discharge | w_unique_discharge | 1 | 1 | shockcore |  | Shock Core: the ring lingers, expands faster and re-strikes twice as fast |
| Barbed Chain | uw_whip_chainlash | w_unique_chainlash | 1 | 1 | whip |  | Barbed Whip: the lash goes all the way around, 30% further, 50% harder shove |
| Wingbeat | uw_seraph_wingbeat | w_unique_wingbeat | 1 | 1 | seraph |  | Seraph Array: the wings spin 60% faster, shove for 5 and burn 30% wider |
| Singularity | uw_horizon_singularity | w_unique_singularity | 1 | 1 | eventhorizon |  | Event Horizon: 50% harder pull, fatter core, further reach, denser ticks |
| Hollow Chamber | hollow_chamber | weapon_slot_add | 1 | 1 |  |  | ONE more weapon slot. Your arsenal holds eight |
| Combat Reflexes | u_ability_haste | ability_haste | 0.25 | 3 |  |  | All abilities recharge 25% faster |
| Heavy Hands | u_ability_might | ability_might | 1 | 2 |  |  | Overload: a wider blast that hits 30 harder and shoves 3 further |
| Phase Memory | u_ability_phase | ability_phase | 1 | 2 |  |  | Phase Dash: +1.2 distance and +0.2s of invulnerability on arrival |
| Deep Freeze | u_ability_stasis | ability_stasis | 1 | 2 |  |  | Stasis: +1s of duration, and the slowed world drops another 0.08x |
| Cascade | u_ability_echo | ability_echo | 0.4 | 1 |  |  | Every ability also fires a 40% Overload at the same spot |

### Milestones (every power-of-two level from 4 on)

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Description |
|---|---|---|---|---|---|---|---|
| Aegis | m4_aegis | shield_add | 45 | 1 |  | 4 | MILESTONE: +45 regenerating shield |
| Blood Pact | m4_pact | lifesteal_add | 6 | 1 |  | 4 | MILESTONE: +6% lifesteal |
| Tempest | m4_tempest | proj_add | 2 | 1 |  | 4 | MILESTONE: +2 projectiles to every weapon |
| Stone Mantle | m8_mantle | defense_add | 60 | 1 |  | 8 | MILESTONE: +60 defense |
| Frenzy | m8_frenzy | fire_rate | 0.5 | 1 |  | 8 | MILESTONE: +50% fire rate to every weapon |
| Reaper's Grasp | m8_grasp | pierce_add | 3 | 1 |  | 8 | MILESTONE: +3 pierce to every weapon |
| Crimson Crown | m16_crown | lifesteal_add | 8 | 1 |  | 16 | MILESTONE: +8% lifesteal |
| Titan Heart | m16_titan | max_hp_add | 120 | 1 |  | 16 | MILESTONE: +120 max HP and heal 120 |
| Overload | m16_overload | damage_mul | 0.6 | 1 |  | 16 | MILESTONE: +60% damage |
| Void Symbiosis | m32_void | damage_mul | 0.9 | 1 |  | 32 | MILESTONE: +90% damage |
| Bulwark of Ages | m32_bulwark | defense_add | 120 | 1 |  | 32 | MILESTONE: +120 defense |
| Hailstorm | m32_hail | proj_add | 4 | 1 |  | 32 | MILESTONE: +4 projectiles to every weapon |
| Immortal | m64_immortal | max_hp_add | 300 | 1 |  | 64 | MILESTONE: +300 max HP and heal 300 |
| Perfection | m64_perfect | fire_rate | 1 | 1 |  | 64 | MILESTONE: +100% fire rate to every weapon |
| Vanquisher | m64_vanquish | damage_mul | 1.5 | 1 |  | 64 | MILESTONE: +150% damage |
| Ascendant Skin | m128_ascend | defense_add | 200 | 1 |  | 128 | MILESTONE: +200 defense |
| Starlight Ward | m128_starlight | shield_add | 500 | 1 |  | 128 | MILESTONE: +500 regenerating shield |
| Overdrive | m128_overdrive | damage_mul | 2 | 1 |  | 128 | MILESTONE: +200% damage |

**Totals:** 178 upgrades (109 normal, 51 unique, 18 milestones).


---

### Upgrade kinds

| Kind       | When it is offered                                        |
|------------|-----------------------------------------------------------|
| normal     | Any level-up, subject to `max_stacks`                     |
| unique     | ~45% chance per level-up, one-time, violet card           |
| milestone  | Only on power-of-two levels (4, 8, 16, ...); separate 3-card pick of 2  |
| weapon     | Only while the named weapon is owned; buffs that slot     |

---

## In-game manual

Shown in the game with **F1** (main menu, live run, or the pause screen). **11 pages:**

- `controls` — CONTROLS (16 lines)
- `run` — THE RUN (17 lines)
- `levels` — LEVEL-UPS (21 lines)
- `weapons` — WEAPONS (16 lines)
- `evolutions` — EVOLUTIONS (21 lines)
- `abilities` — ABILITIES  J / K / L (23 lines)
- `stats` — YOUR STATS (30 lines)
- `cards` — CARDS (21 lines)
- `enemies` — ENEMIES (18 lines)
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

    SURVIVE. THE SPAWN RATE ACCELERATES
    AFTER SIX MINUTES AND NEVER STOPS.
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
    ORBITING BLADES   VOID GYRE, SERAPH
    EXPANDING RING    VOID NOVA, SUNDER
    TAUNTING BEACON   GRAVE BELL
    GROUND ZONE       EMBER, ASHFALL

    ORBITING BLADES ALSO GRIND THE
    INSIDE OF THEIR RING, SO NOTHING
    HUGS YOU FOR FREE.
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
    SERAPH ARRAY     DRILL + CROSSBOW + WHIP
    EVENT HORIZON    RAIL RIFLE + SHOCK + FLAME
```

### ABILITIES  J / K / L

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
               ALL DAMAGE. FLAT, THEN %.
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

    # THE MOMENTUM CHAIN
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

    # VIOLET UNIQUES
    ONE-TIME, RULE-CHANGING, RARE. EVERY
    WEAPON HAS AT LEAST ONE OF ITS OWN, SO
    A NEW WEAPON IS NEVER A DEAD SLOT. FIVE
    MORE RETUNE THE J/K/L ABILITIES - NONE
    OF THEM UNLOCKS AN ABILITY.

    # VIOLET MILESTONES
    EVERY POWER-OF-TWO LEVEL FROM 4 ON
    REPLACES THE WHOLE CHOICE WITH A
    SPECIAL SCREEN. PICK 2 OF 3.

    # THE POOL NEVER DRIES UP
    AN EXHAUSTED OR FULLY MAXED POOL FALLS
    BACK TO THE FIRST STILL-APPLICABLE
    CARD RATHER THAN LEAVING YOU STUCK.
```

### ENEMIES

```
    18 TYPES WALK IN AND DEAL CONTACT
    DAMAGE. THEY SCALE WITH RUN TIME:
    MORE HP, FASTER, AND HARDER HITS.

    # ELITES AND ABOVE
    ELITE    FROM 45S. ONE RANDOM TRAIT.
    CHAMPION OPENS WHEN ELITES STOP BEING
             A PROBLEM. SEVERAL TRAITS.
    OVERLORD OPENS WHEN CHAMPIONS DO.
             MANY TRAITS.

    THE TRIBUNAL DIRECTOR WATCHES HOW WELL
    YOU ARE DOING AND PROMOTES YOU WHEN
    YOU HAVE EARNED IT. KILL ONE OF EACH
    TIER TO UNLOCK ITS OUTLINE.

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
