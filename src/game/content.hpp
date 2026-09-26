#pragma once

#include "core/render/color.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// Data-driven balance: everything here is loaded from assets/data/*.toml.

enum class AttackType : std::uint8_t {
  Projectile,   // standard aimed projectile(s) with spread
  Orbit,        // blades orbit player, deal contact damage
  Cone,         // instant AoE cone in aim direction
  Bomb,         // slow arcing projectile with gravity, explodes on impact
  Boomerang,    // projectile goes out to max range, returns to player
  Bounce,       // projectile bounces between enemies
  Beam,         // instant hitscan line
  Sweep,        // melee arc around player
  Zone,         // creates persistent damage zone on ground
  Chain,        // lightning jumps between enemies, optionally forking (evolution)
  Wave,         // crescent that LEAVES the player and travels outward (evolution)
  Nova,         // expanding ring from player (evolution)
  Inferno,      // target-centered circle reap + burning ground (evolution)
  Pulsar,       // boomerang that drags a damage trail (evolution)
  Halo,         // beams orbiting the player (evolution)
  Vortex,       // rotating suction zones that drag enemies inward (evolution)
  Prism,        // N separate beams, each locked onto its own target (evolution)
  Lure,         // planted beacon that hauls enemies into its kill zone
};

struct WeaponDef {
  std::string id;
  std::string name;
  std::string desc;
  AttackType attackType = AttackType::Projectile;
  float damage = 5.0F;
  float cooldown = 0.5F;
  int projectiles = 1;        // count for projectile/orbit/boomerang
  float projSpeed = 12.0F;    // speed for projectile/boomerang/bomb
  float projLife = 1.4F;      // lifetime for projectile/boomerang/bomb
  int pierce = 0;             // projectile/bounce
  float spread = 0.16F;       // radians between projectiles (projectile)
  core::render::Color projColor{1.0F, 0.95F, 0.55F, 1.0F};
  bool starter = false;
  std::vector<std::string> prereqs;
  bool homing = false;    // projectile homes toward nearest enemy

  // Cone
  float coneAngle = 0.8F;     // radians
  float coneRange = 2.5F;     // world units
  float coneTickRate = 0.1F;  // damage ticks per second
  // A cone that BITES instead of washing. While non-zero the cone stops being a
  // flamethrower: it latches onto the nearest body in the arc, chews only that
  // one, and its damage climbs by this fraction of the base per consecutive
  // tick until the target dies or leaves the arc. Zero = hit everything equally
  // (the Ember Sprayer's rule, and the reason the Jackhammer Drill needs its own).
  float coneBite = 0.0F;
  // Ceiling on that ramp, as a multiple of the base damage. A bite that climbs
  // forever is a weapon with no answer except walking away.
  float coneBiteMax = 4.0F;
  // Hearthfire: a cone that leaves burning ground behind it. `coneEmberAt` is how
  // far the tip has to travel before it drops another pool (0 = never), and the
  // pool's own size and life follow. A cone that only damages what is inside it
  // this instant is a cone you have to keep pointing at things; one that scorches
  // where it has been is a trail you lay down and then walk away from.
  float coneEmberAt = 0.0F;
  float coneEmberRadius = 1.0F;
  float coneEmberDuration = 2.5F;

  // Orbit
  float orbitRadius = 1.2F;   // distance from player
  float orbitSpeed = 2.0F;    // radians per second
  int orbitCount = 2;         // number of blades
  // A gap in the ring. Without it the blades are a solid wall and the whirl
  // inside is a uniform, weak, everywhere-equal grind. With it the ring has one
  // safe angle, and that angle is the ONLY place the interior is cut -- so the
  // hub stops being a bonus you cannot aim and becomes a spot you stand in and
  // let the horde walk into. This is what makes a faster ring worth having:
  // faster means the gap comes around more often, which only counts if you have
  // somewhere to be when it does.
  bool orbitWindow = false;

  // Bomb
  float bombArcHeight = 2.0F;     // vertical arc height
  float bombExplodeRadius = 1.5F; // explosion radius
  float bombKnockback = 3.0F;     // knockback force
  float bombFuse = 0.0F;          // 0 = explode on impact, >0 = delay
  // Deliberate over-shoot, in world units. A bomb normally lands where it was
  // aimed; one with `bombAhead` finds the enemy nearest its aim line and lands
  // that far PAST them, so it sails over the front rank and cooks the back of
  // the horde instead. This is the whole difference between the Siege Mortar and
  // the Runic Hammer, which are otherwise the same arcing shell with other
  // numbers: a hammer blows up what you were aiming at, a mortar ignores what is
  // in front of it.
  float bombAhead = 0.0F;
  // The other half of that pair: land the shell ON the body nearest the aim line.
  // The two rules are opposites rather than degrees -- the hammer answers the
  // thing in front of you, the mortar answers the horde behind it -- and a bomb
  // with neither keeps its plain ballistic range, which is the same arc it has
  // always had.
  bool bombOnTarget = false;
  // Re-aiming bolts. A projectile with `reaimRange` bends onto the next body
  // every time it lands on one, so its pierce buys steering rather than reach.
  // The budget is the pierce: each bend costs a pierce point, so a bolt can never
  // touch more bodies than its own statline admits to.
  float reaimRange = 0.0F;  // how far it can see for the next body, 0 = straight
  float reaimTurn = 0.0F;   // max course change per step, in radians

  // Boomerang
  float boomerangRange = 4.0F;    // max distance before return
  float boomerangReturnSpeed = 1.5F; // return speed multiplier

  // Bounce
  int bounceCount = 3;          // max bounces
  float bounceRange = 2.5F;     // search radius for next target
  float bounceDamageMul = 0.7F; // damage multiplier per bounce
  bool bounceInfinite = false;  // never expires: one eternal orb that hunts

  // Beam
  float beamRange = 8.0F;       // max range
  float beamWidth = 0.3F;       // line thickness
  float beamDuration = 0.15F;   // visual persist time

  // Chill: an on-hit status, not a stat. `chillMul` is the speed the enemy is
  // pinned to while it lasts (< 1), and `chillTime` how long. Zero mul means the
  // weapon does not chill at all, which is the default for everything except the
  // ice, so this is opt-in per weapon rather than a global rule.
  float chillMul = 0.0F;
  float chillTime = 0.0F;
  // A corona the projectile CARRIES while it flies, as opposed to a status it
  // leaves behind on a body it went through. Chill only ever reaches the targets
  // a shot hit, so it is a hit effect and the weapon has to keep hitting to keep
  // slowing. An aura drags a chilling, grinding bubble along with the shard, so
  // everything the volley PASSES walks in slowly whether it was aimed at it or
  // not -- which is the whole difference between "ice that hits" and "a corridor
  // of ice". Zero radius on every weapon except the one built for it.
  float auraRadius = 0.0F;
  float auraDps = 0.0F;
  float auraTick = 0.10F;
  float auraChillMul = 0.0F;
  float auraChillTime = 0.0F;

  // Halo (evolution)
  float haloKnockback = 0.0F;   // outward shove applied along the beam (per sec)
  // Dead zone at the player's feet. A spoke only reaches this far out, so a halo
  // with a hole in the middle stops being a bigger halo: it is a rotating WALL
  // with safe ground inside it. That is the entire difference between the Seraph
  // Array's wings and the Radiant Halo's blades, which reach the centre.
  float haloInner = 0.0F;

  // Sweep
  float sweepAngle = 3.14F;     // radians (PI = 180°, 2PI = 360°)
  float sweepRadius = 2.0F;     // reach
  float sweepKnockback = 2.0F;
  float sweepLead = 0.0F;       // arc center distance in front of the player
  // A sweep that DRAGS its catch in instead of shoving it away. The Soul Scythe is
  // a reap: a circle around its prey that clears space, so it keeps pushing. A
  // whip with barbs is the opposite of that, and without this the barbs were
  // only ever in the name. Opt-in so the two melee weapons stay opposites.
  bool sweepHook = false;

  // Zone
  float zoneRadius = 1.2F;
  float zoneDuration = 4.0F;
  float zoneDps = 15.0F;
  int zoneMaxPools = 3;
  // The pool is laid by something that fell on it (a shell) rather than by
  // something that burned where it stood (a cone), and is drawn as a shaft.
  bool zoneFromAbove = false;

  // Chain (evolution)
  float chainJumpRange = 2.5F;
  int chainMaxJumps = 4;
  float chainDamageMul = 0.6F;
  // Extra bolts branching off at each hop: 0 = one linear arc, >0 = a fork.
  // Flat shards thrown off on the first hit (0 = the bolt stays whole).
  int chainShatter = 0;
  float chainShatterSpeed = 15.0F;
  float chainShatterSpread = 0.55F;

  // Bounce: child orbs thrown off on each impact (0 = never splits).
  int bounceSplits = 0;

  // Wave (Sunder): a crescent that travels away from the player.
  float waveSpeed = 6.0F;
  float waveRange = 7.0F;
  float waveWidth = 2.2F;
  float waveKnockback = 4.0F;
  float waveDamageMul = 0.8F;
  int waveCount = 1;        // arcs per shot, each rotated by waveArcStep
  float waveArcStep = 0.0F;  // radians between those arcs
  // > 0 makes the arcs HERD what they catch across the fan instead of shoving it
  // down their own heading, so each arc hands its catch to the next. Scales the
  // arc's knockback. 0 (the Sundering Core) is the plain shove.
  float waveHookPull = 0.0F;
  // How far the crescent's horns open, in radians. Both wave weapons are the same
  // entity, and this is the field that makes them read as two different things: a
  // wide, slow, deep arc is a wall you shove a whole rank down with (the
  // Sundering Core), a narrow fast one is a lash that rakes (the Tidal Lash).
  float waveSpread = 0.9F;
  // How far the crescent's horns open, in radians. This is a rule, not a number:
  // a wide arc with a slow, heavy body is a wall you shove a whole rank down with
  // (the Sundering Core), and a narrow one thrown three times is a lash that
  // rakes (the Tidal Lash). Same wave, two readable weapons.

  // Nova (evolution)
  float novaMaxRadius = 4.0F;
  float novaExpandSpeed = 3.0F;
  float novaDamagePerTick = 25.0F;
  float novaTickRate = 0.15F;
  // A ring that comes IN instead of going out. It is cast at full radius, rushes
  // the player, drags everything it passes toward the centre, and detonates when
  // it arrives -- so the Void Nova is the exact inverse of the Shock Core, which
  // is a wall expanding away from you and shoving the crowd into open ground.
  // Without this the two are the same weapon with a bigger number on it.
  bool novaContract = false;
  float novaPull = 0.0F;        // inward drag while contracting (units / sec)
  float novaBurstDamage = 0.0F; // one-off hit at the centre on arrival
  // A second ring cast this many seconds after the first (0 = one ring only). The
  // point of a double is timing, not size: the echo lands after the first blast
  // has thinned the pack, so a weapon that used to be a single pulse now is one.
  float novaEcho = 0.0F;

  // Inferno: land the barrage on a planted Lure bell instead of the nearest
  // enemy, which is what pairs a mortar with a bell.
  bool infernoBindsToLure = false;

  // Vortex (evolution)
  float vortexRadius = 1.3F;      // damage core radius
  float vortexReach = 2.6F;       // enemies inside this start getting pulled in
  float vortexPull = 5.0F;        // pull strength (world units / sec)
  float vortexOrbit = 2.6F;       // distance of a zone from the player
  float vortexOrbitSpeed = 1.7F;  // radians / second
  float vortexTickRate = 0.1F;    // damage ticks per second
  // Seconds a well has to swallow before it COLLAPSES: a burst far larger than
  // its steady grind, after which the well implodes and reopens somewhere else on
  // the orbit. Zero means it never collapses, which is the Void Gyre's whole
  // character -- a permanent, patient drag you can build a position around. A
  // weapon with a collapse is a different thing: it hoards, gathers, and pays out
  // in one violent moment, so its damage is lumpy and its threat is a clock.
  float vortexCollapseAt = 0.0F;
  float vortexBurstDamage = 0.0F;  // the collapse hit (0 = use a multiple of damage)
  float vortexBurstRadius = 0.0F; // 0 = use the core radius

  // Prism (evolution)
  float prismRange = 9.0F;        // max distance a locked beam can reach
  float prismWidth = 0.45F;
  int prismMaxTargets = 5;        // hard cap on simultaneously locked beams
  float prismRicochet = 4.0F;     // how far a beam can jump to its next victim

  // Lure: a planted beacon. Enemies inside `lureReach` are dragged toward it and
  // anything inside `lureRadius` is cut up, so the beacon does the crowd control
  // while the rest of the arsenal does the damage.
  float lureRadius = 1.4F;        // damage core
  float lureReach = 3.6F;         // outer edge where the drag starts
  float lurePull = 5.0F;          // inward drag (world units / sec)
  float lureDps = 14.0F;          // damage per second inside the core
  float lureDuration = 5.0F;      // seconds a beacon stays planted
  float lureTickRate = 0.15F;     // damage ticks per second
  int lureMaxBeacons = 2;         // beacons alive at once
};

// A per-type speed ramp may not push a type past twice its authored speed.
// The global difficulty ramp already multiplies on top of this, so a large
// per-type ramp compounds into something no amount of positioning can answer.
inline constexpr float kSpeedRampCeiling = 1.0F;

struct EnemyDef {
  std::string id;
  std::string name;
  float hp = 10.0F;
  float speed = 2.0F;
  // Extra fraction of `speed` this type has gained by the time the ramp
  // saturates (see Game::kSpeedRampFullTime), and which it keeps for the rest
  // of the run. 0 = this type does not accelerate on its own; the global
  // difficulty ramp still applies to it like everything else.
  //
  // This exists so a fast archetype can be authored at a survivable opening
  // speed and still reach its intended top speed late: `speed` is what the
  // player meets in the first minute, `speed * (1 + speedRampMax)` is what the
  // same enemy is at 10:00.
  float speedRampMax = 0.0F;
  float touch = 5.0F;
  float radius = 0.3F;
  float xp = 1.0F;
  float unlockAt = 0.0F;
  float weight = 1.0F;
  // A "speedster" archetype. These are held out of the spawn pool until
  // Game::kFastEnemyMinTime even when their unlock_at has passed, so the
  // opening minutes are a readable warm-up and the first genuinely quick enemy
  // is an escalation you can see coming.
  bool fast = false;
  core::render::Color color{};
  bool circle = true;
};

struct UpgradeDef {
  std::string id;
  std::string name;
  std::string desc;
  std::string effect;
  float value = 0.0F;
  int maxStacks = 5;
  std::string kind = "normal";
  std::string weapon;
  int level = 0;
};

// One screen of the in-game manual (assets/data/manual.toml).
//
// `lines` are drawn verbatim, one per row. A leading '>' is a highlighted
// bullet, a leading '#' is a sub-heading, a leading '  ' is an indent
// continuation of the bullet above. Everything else is body text.
struct ManualPage {
  std::string id;
  std::string title;
  std::vector<std::string> lines;
};

struct Content {
  std::vector<WeaponDef> weapons;
  std::vector<EnemyDef> enemies;
  std::vector<UpgradeDef> upgrades;
  // Empty when manual.toml is missing: the manual screen then says so instead
  // of crashing, so a stripped build still runs.
  std::vector<ManualPage> manual;

  [[nodiscard]] const WeaponDef* weapon(std::string_view id) const;
  [[nodiscard]] const EnemyDef* enemy(std::string_view id) const;
  // nullptr when the id is unknown.
  [[nodiscard]] const ManualPage* manualPage(std::string_view id) const;
};

Content loadContent(const std::filesystem::path& dir);

} // namespace game