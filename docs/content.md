<!-- GENERATED FILE — do not edit by hand.
     Source: assets/data/*.toml, generator: tools/gendocs.py -->

# Content Reference

All numbers below are read directly from the TOML files in `assets/data/`.
Regenerate with:

```sh
python3 tools/gendocs.py
```

**Totals:** 13 weapons (9 base +
4 evolutions), 18 enemies.

### Base weapons

| Name | ID | Damage | Cooldown (s) | Projectiles | Proj. speed | Pierce | Spread (rad) | Starter | Traits | Description |
|---|---|---|---|---|---|---|---|---|---|---|
| Arcane Wand | wand | 8 | 0.5 | 1 | 14 | 0 | 0.1 | yes |  | Reliable magic bolts. The balanced baseline. |
| Throwing Dagger | dagger | 5 | 0.4 | 3 | 0 | 0 | 0 | yes |  | Orbiting knives that carve up anything close. |
| Heavy Crossbow | crossbow | 20 | 1.2 | 1 | 22 | 4 | 0.18 | yes | homing | Slow, homing piercing bolt that punches through crowds. |
| Ember Sprayer | flame | 4 | 0.35 | 1 | 0 | 0 | 0 |  |  | Cone of fire — instant damage in a wide arc. |
| Runic Hammer | hammer | 45 | 1.8 | 1 | 8 | 0 | 0.3 |  |  | Arcing bomb with massive explosion and knockback. |
| Storm Shuriken | shuriken | 8 | 0.6 | 2 | 16 | 1 | 0.2 |  |  | Boomerang blades — hit going out AND coming back. |
| Void Orb | orb | 35 | 1.5 | 1 | 7 | 5 | 0 |  |  | One eternal orb that hunts forever. Projectiles grow it, not multiply. |
| Soul Scythe | scythe | 55 | 1.3 | 1 | 0 | 0 | 0 |  |  | Reaps a full circle of death around its nearest prey. |
| Solar Lance | beam | 90 | 2.5 | 1 | 0 | 0 | 0 |  |  | Instant hitscan beam — deletes a line. |

### Evolutions (A + B = C)

| Name | ID | Requires | Damage | Cooldown (s) | Projectiles | Pierce | Area | Description |
|---|---|---|---|---|---|---|---|---|
| Storm Caller | storm | wand + crossbow | 15 | 0.45 | 1 | 0 | 0 | Wand + Crossbow. Lightning chains between enemies. |
| Void Nova | nova | orb + hammer | 40 | 1.8 | 1 | 0 | 0 | Orb + Hammer. Expanding ring of destruction. |
| Inferno | inferno | flame + scythe | 60 | 1.6 | 1 | 0 | 0 | Sprayer + Scythe. Reaps a circle and leaves burning ground. |
| Pulsar | pulsar | beam + shuriken | 12 | 0.9 | 1 | 2 | 0 | Shuriken + Lance. Light-chakram dragging a burning laser trail. |

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
| Swift Boots | boots | speed_mul | 0.12 | 4 |  |  | +12% move speed |
| Vigor | vigor | max_hp_add | 20 | 6 |  |  | +20 max HP and heal 20 |
| Soul Magnet | magnet | pickup_mul | 0.6 | 4 |  |  | +60% pickup range |
| Piercing Shots | pierce | pierce_add | 1 | 3 |  |  | +1 pierce |
| Life Leech | leech | heal | 25 | 3 |  |  | Heal 25 HP |
| Regeneration | regen | regen_add | 1 | 4 |  |  | +1 HP per second |
| Might | might | damage_mul | 0.25 | 4 |  |  | +25% damage |
| Quickdraw | quickdraw | fire_rate | 0.13 | 3 |  |  | +13% fire rate |
| Twin Cast | twin_shot | proj_add | 1 | 2 |  |  | +1 projectile |
| Windrunner | windrunner | speed_mul | 0.08 | 6 |  |  | +8% move speed |
| Dark Feast | feast | heal | 40 | 2 |  |  | Heal 40 HP |
| Iron Constitution | constitution | max_hp_add | 35 | 4 |  |  | +35 max HP and heal 35 |
| Bulwark | bulwark | max_hp_add | 50 | 2 |  |  | +50 max HP and heal 50 |
| Vitality | vitality | regen_add | 2 | 2 |  |  | +2 HP per second |
| Treasure Sense | treasure | pickup_mul | 1 | 2 |  |  | +100% pickup range |
| Harvester | harvester | pickup_mul | 0.8 | 2 |  |  | +80% pickup range |
| Garrote Bolts | garrote | pierce_add | 2 | 1 |  |  | +2 pierce |
| Battle Echoes | echoes | fire_rate | 0.19 | 2 |  |  | +19% fire rate |
| Barrage | barrage | proj_add | 2 | 1 |  |  | +2 projectiles |
| Rejuvenation | rejuvenation | heal | 60 | 1 |  |  | Heal 60 HP |
| Fervor | fervor | damage_mul | 0.35 | 2 |  |  | +35% damage |
| Gale Steps | gale | speed_mul | 0.15 | 2 |  |  | +15% move speed |
| Heartwood | heartwood | max_hp_add | 70 | 1 |  |  | +70 max HP and heal 70 |
| Surge | surge | fire_rate | 0.25 | 1 |  |  | +25% fire rate |
| Stony Pledge | pledge | defense_add | 10 | 4 |  |  | +10 defense |
| Iron Conviction | conviction | defense_add | 25 | 2 |  |  | +25 defense |
| Minor Ward | ward_small | shield_add | 15 | 3 |  |  | +15 regenerating shield |
| Runic Ward | ward_great | shield_add | 30 | 2 |  |  | +30 regenerating shield |
| Gilded Fangs | leech_gold | lifesteal_add | 8 | 3 |  |  | +8% lifesteal |
| Soulfeed | leech_soul | lifesteal_add | 12 | 2 |  |  | +12% lifesteal |
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

### Unique items (one-time, rule-changing)

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Description |
|---|---|---|---|---|---|---|---|
| Spreadshot | u_spreadshot | fan | 1 | 1 |  |  | Double the volley spread and fire rate (all weapons) |
| Ignited Carapace | u_thorns | thorns | 3 | 1 |  |  | Getting hit detonates a burst dealing 3x incoming damage |
| Gambler's Eye | u_extra_choice | extra_choice | 1 | 1 |  |  | +1 card in every future level-up choice |
| Second Chance | u_reroll | reroll_add | 1 | 1 |  |  | +1 free reroll per level-up (total two) |
| Adrenaline | u_adrenaline | adrenaline | 1 | 1 |  |  | Below 30% HP: +60% speed and a 1s invulnerability every 20s |
| Singularity | u_black_hole | black_hole | 1 | 1 |  |  | Every 12s, violently yanks nearby enemies toward you |
| Storm Bolt | u_chain | chain | 1 | 1 |  |  | Every 3rd projectile hit chains lightning to 3 nearby enemies |
| Blood Price | u_blood_price | blood_price | 1 | 1 |  |  | Every 20 kills detonates a burst around you |
| Cold Blood | u_ice_blood | ice_blood | 1 | 1 |  |  | Enemies that hit you are slowed for 2s |
| Vampiric Heart | u_vampiric_heart | lifesteal_heal | 2 | 1 |  |  | Lifesteal heals 2 HP per proc instead of 1 |
| Seeking Missiles | uw_wand_seeking | w_unique_homing | 1 | 1 | wand |  | Arcane Wand: bolts home onto the nearest enemy |
| Blade Vortex | uw_dagger_vortex | w_unique_vortex | 1 | 1 | dagger |  | Throwing Dagger: blades spin 2x faster in a 25% wider orbit |
| Fragmenting Bolt | uw_crossbow_fragment | w_unique_area | 1.2 | 1 | crossbow |  | Heavy Crossbow: bolts explode on impact for area damage |
| Hearthfire | uw_flame_hearthfire | w_unique_hearthfire | 1 | 1 | flame |  | Ember Sprayer: cone is 50% wider and 40% longer |
| Cataclysm | uw_hammer_cataclysm | w_unique_cataclysm | 1 | 1 | hammer |  | Runic Hammer: explosions 60% larger with heavier knockback |
| Return Tempest | uw_shuriken_tempest | w_unique_area | 2.5 | 1 | shuriken |  | Storm Shuriken: returning blades detonate a 2.5-area burst |
| Echo Detonation | uw_orb_echo | w_unique_area | 1.5 | 1 | orb |  | Void Orb: every bounce splashes half damage around the hit |
| Reaper's Harvest | uw_scythe_harvest | w_unique_harvest | 3 | 1 | scythe |  | Soul Scythe: sweeps restore 3 HP per kill |
| Prism Lance | uw_beam_prism | w_unique_prism | 3 | 1 | beam |  | Solar Lance: fires 3 parallel beams at once |
| Thunderlord | uw_storm_thunderlord | w_unique_thunderlord | 4 | 1 | storm |  | Storm Caller: +4 chain jumps and no damage decay |
| Supernova | uw_nova_supernova | w_unique_supernova | 1 | 1 | nova |  | Void Nova: ring expands faster, wider, and hits harder |
| Everflame | uw_inferno_everflame | w_unique_everflame | 1 | 1 | inferno |  | Inferno: wider reap, burning ground lasts longer and burns harder |
| Arc Saw | uw_pulsar_arcsaw | w_unique_arcsaw | 1 | 1 | pulsar |  | Pulsar: the laser trail is 80% wider and deals 35% more damage |

### Milestones (every power-of-two level from 4 on)

| Name | ID | Effect | Value | Max stacks | Weapon | Level | Description |
|---|---|---|---|---|---|---|---|
| Aegis | m4_aegis | shield_add | 45 | 1 |  | 4 | MILESTONE: +45 regenerating shield |
| Blood Pact | m4_pact | lifesteal_add | 12 | 1 |  | 4 | MILESTONE: +12% lifesteal |
| Tempest | m4_tempest | proj_add | 2 | 1 |  | 4 | MILESTONE: +2 projectiles to every weapon |
| Stone Mantle | m8_mantle | defense_add | 60 | 1 |  | 8 | MILESTONE: +60 defense |
| Frenzy | m8_frenzy | fire_rate | 0.5 | 1 |  | 8 | MILESTONE: +50% fire rate to every weapon |
| Reaper's Grasp | m8_grasp | pierce_add | 3 | 1 |  | 8 | MILESTONE: +3 pierce to every weapon |
| Crimson Crown | m16_crown | lifesteal_add | 16 | 1 |  | 16 | MILESTONE: +16% lifesteal |
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

**Totals:** 86 upgrades (45 normal, 23 unique, 18 milestones).


---

### Upgrade kinds

| Kind       | When it is offered                                        |
|------------|-----------------------------------------------------------|
| normal     | Any level-up, subject to `max_stacks`                     |
| unique     | ~45% chance per level-up, one-time, violet card           |
| milestone  | Only on power-of-two levels (4, 8, 16, ...); separate 3-card pick of 2  |
| weapon     | Only while the named weapon is owned; buffs that slot     |
