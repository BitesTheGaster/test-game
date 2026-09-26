#pragma once

#include "core/render/color.hpp"

#include <cstdint>

namespace game {

// All components are POD: they live in dense EnTT pools and never allocate.

struct Transform {
  float x = 0.0F;
  float y = 0.0F;
  float px = 0.0F; // previous tick position (for render interpolation)
  float py = 0.0F;
};

struct Velocity {
  float x = 0.0F;
  float y = 0.0F;
};

struct Radius {
  float r = 0.1F;
};

struct Sprite {
  core::render::Color color{};
  bool circle = true;
};

struct Health {
  float hp = 1.0F;
  float max = 1.0F;
};

struct PlayerTag {};

struct Enemy {
  float speed = 2.0F;
  float touch = 5.0F;
  float slowT = 0.0F; // >0 => slowed for this many seconds
  // The speed the enemy is pinned to while slowT runs. 0.45 is the Ice Blood
  // contact slow; ice weapons chill deeper. Kept per-enemy rather than as a
  // constant so a stronger source can overwrite a weaker one instead of the
  // stronger one having to know what the weaker one did.
  float slowMul = 0.45F;
  int def = -1;       // content enemy index for the bestiary (-1 = unknown)
  // Decaying knockback velocity, added on top of the AI seek each tick so
  // explosions, sweeps and the Repulsion Field unique actually shove enemies.
  float kbX = 0.0F;
  float kbY = 0.0F;
};

// Bit flags for elite/champion enemy traits (non-POD-free: plain POD).
// Elites roll exactly ONE trait; champions and stronger roll several.
enum TraitFlag : std::uint32_t {
  TraitNone = 0,
  TraitFast = 1u << 0,
  TraitArmored = 1u << 1,
  TraitRegenerating = 1u << 2,
  TraitExplosive = 1u << 3,
  TraitVenomous = 1u << 4,
  TraitVampiric = 1u << 5,
  TraitShielded = 1u << 6,
  TraitHeavy = 1u << 7,     // hits much harder
  TraitArcher = 1u << 8,    // shoots projectiles at the player
  TraitAura = 1u << 9,      // burns the player inside an aura
  TraitResistant = 1u << 10 // strong lifesteal + knockback resistance
};

struct EnemyTraits {
  std::uint32_t flags = TraitNone;
  std::uint8_t tier = 0; // 0 normal, 1 elite, 2 champion, 3 overlord
  float regen = 0.0F;    // HP per second
  float shield = 0.0F;   // absorbs damage point-for-point
  // Defense uses the same flat+percent curve as the player (mitigateDamage)
  // and grows with run time, so late enemies shrug off a slice of every hit.
  float defense = 0.0F;
  float lifestealRes = 0.0F; // 0..1: scales the player's lifesteal proc chance
  float knockbackRes = 0.0F; // 0..1: scales incoming knockback
  float shootCooldown = 0.0F; // >0 => ranged attacker (TraitArcher)
  float shootTimer = 0.0F;
  float auraRadius = 0.0F;    // >0 => damage aura (TraitAura)
  float auraDps = 0.0F;
};

// A hostile projectile spat out by a TraitArcher enemy.
struct EnemyShot {
  float damage = 1.0F;
  float life = 3.0F;
  core::render::Color color{1.0F, 0.4F, 0.2F, 1.0F};
};

struct Projectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 1.5F;
  float area = 0.0F;      // explosion radius on impact
  float strength = 0.0F;  // knockback force on hit
  bool homing = false;    // homes toward nearest enemy each tick
  int bounces = 0;        // wall bounces remaining after impact
  int bounceCount = 0;    // how many bounces have happened
  // Chill applied to anything this projectile hits: the speed the victim is
  // pinned to (< 1) and for how long, in seconds. Zero time means no chill, so
  // a weapon opts in by carrying the values rather than by every hit path asking
  // whether the weapon happens to be made of ice.
  float chillMul = 0.0F;
  float chillTime = 0.0F;

  // A corona the projectile CARRIES, as opposed to a status it LEAVES on hit.
  //
  // Chill is applied to the one body the shard went through, so it is a hit
  // effect and the weapon has to keep hitting to keep slowing. An aura is the
  // other thing entirely: while the shard is in flight it drags a bubble of
  // cold around itself that chills and grinds everything inside it, whether it
  // ever touched them or not. That is what turns a line of shards into a
  // corridor that walks in slowly, and it is why this is an evolution rather
  // than a bigger Frost Shards.
  float auraRadius = 0.0F;  // 0 = no aura; every other weapon leaves this zero
  float auraDps = 0.0F;     // damage per second inside the bubble
  float auraTick = 0.10F;   // seconds between aura damage ticks
  float auraTimer = 0.0F;
  // The aura's own chill. Weaker than a direct hit, because it cannot stop
  // anything from being behind the shard -- it just makes everything it passes
  // walk into the shard more slowly.
  float auraChillMul = 0.0F;
  float auraChillTime = 0.0F;

  // --- Re-aiming ------------------------------------------------------------
  // A bolt that, on every hit, BENDS onto the next body instead of carrying on
  // straight. This is the Storm Caller's whole mechanic and it is the only thing
  // either of its parents cannot do: the Crossbow punches through in a straight
  // line and spends its pierce going further in the same direction, and the Wand
  // hits reliably but only the body it was aimed at. Here the pierce is spent on
  // STEERING, so the bolt keeps the punch-through and gets the wand's promise
  // that it does not simply sail past the pack.
  //
  // `reaimRange` is how far it can see for the next body and `reaimTurn` how hard
  // it can bend in one step (a limited turn, not a snap, so a fast target can
  // still slip behind something and the crossbow's weight survives).
  //
  // The bend has its own budget, `reaimLeft`, rather than being paid out of
  // `pierce`. Sharing the pierce reads better and is wrong: paying a point per
  // bend means a pierce-4 bolt only ever touches THREE bodies, so the number on
  // the card would mean something different here than it does on every other
  // weapon in the game. The budget is instead SEEDED from the pierce -- one bend
  // per body the bolt is allowed to touch -- which leaves `pierce` meaning
  // exactly what it means everywhere else: this many bodies, and no more.
  float reaimRange = 0.0F;  // 0 = no re-aim; every other weapon leaves this zero
  float reaimTurn = 0.0F;   // max radians of course change per step
  int reaimLeft = 0;        // bends still owed

  // Where the current bend is taking it, and whether one is running.
  //
  // The bend is a COURSE and not a kick. It was originally a single change of
  // heading applied on the frame of the hit, and a bolt moving at 24 units a
  // second had already flown most of the way to the next body by the time the
  // change was over -- so it would pass the target it had just been told to
  // take and straighten up somewhere past it, which is not a bend at all. It
  // holds the new heading and keeps steering toward it at `reaimTurn` every step
  // until it arrives, so `reaim_turn` is now an honest number: how sharply this
  // thing can corner.
  float reaimX = 0.0F;
  float reaimY = 0.0F;
  bool hasReaim = false;

  // The bodies this bolt has already paid, most recent last.
  //
  // A bolt that flies STRAIGHT never needs this: it leaves a body behind it and
  // is already outside that body's reach. A bolt that BENDS is still inside the
  // body it just left on the next step, so without a record of what it has hit it
  // pays the same body twice and the bend reads as a stutter rather than a
  // pass-through. Keeping the last few also stops the bend from doubling back
  // into the one it is trying to leave, and stops it from banking off the body
  // before that.
  //
  // It is only consulted when `reaimRange` is non-zero, so the bouncing weapons
  // are untouched: the Void Orb is SUPPOSED to ricochet between the same two
  // bodies forever, and a general "no double hits" rule would have quietly
  // deleted that weapon's entire identity.
  static constexpr int kReaimMemo = 6;
  int reaimMemo[kReaimMemo] = {-1, -1, -1, -1, -1, -1};
  int reaimMemoCount = 0;

  // True if this bolt has already paid `entity`, which only ever means something
  // for a re-aiming bolt.
  [[nodiscard]] bool reaimedAlready(int entity) const {
    if (reaimRange <= 0.0F) return false;
    for (int i = 0; i < reaimMemoCount && i < kReaimMemo; ++i) {
      if (reaimMemo[i] == entity) return true;
    }
    return false;
  }
  void rememberReaimed(int entity) {
    if (reaimRange <= 0.0F) return;
    if (reaimMemoCount < kReaimMemo) {
      reaimMemo[reaimMemoCount++] = entity;
      return;
    }
    // Full: drop the oldest, so the memo is the RECENT past. Four is more than
    // any weapon's pierce is long, so this only matters if a card hands out a
    // very deep pierce -- and even then it degrades into "the oldest body is
    // hittable again", which is a far better failure than a bolt that stops.
    for (int i = 0; i + 1 < kReaimMemo; ++i) reaimMemo[i] = reaimMemo[i + 1];
    reaimMemo[kReaimMemo - 1] = entity;
  }
};

// Orbiting blades (dagger, etc.)
struct OrbitBlade {
  float damage = 1.0F;
  float radius = 1.2F;     // orbit distance from player
  float speed = 2.0F;      // radians per second
  float angle = 0.0F;      // current angle
  int pierce = 0;
  int weaponIndex = -1;    // owning weapon slot (-1 = orphaned)
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Arcing bomb projectile (hammer)
struct BombProjectile {
  float damage = 1.0F;
  int pierce = 0;          // reduces crowd damage falloff
  float explodeRadius = 1.5F;
  float knockback = 3.0F;
  float life = 2.0F;
  float initialLife = 2.0F;
  float arcHeight = 2.0F;
  float startX = 0.0F;
  float startY = 0.0F;
  float targetX = 0.0F;
  float targetY = 0.0F;
  float fuse = 0.0F;       // 0 = explode on impact
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Boomerang projectile (shuriken)
struct BoomerangProjectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 3.0F;
  float maxRange = 4.0F;
  float returnSpeed = 1.5F;
  float startX = 0.0F;
  float startY = 0.0F;
  float targetX = 0.0F;
  float targetY = 0.0F;
  bool returning = false;
  int bounceCount = 0;
  float blastRadius = 0.0F;  // >0: explode on return (shuriken unique)
  // "Pulsar" evolution: while flying, the blade drags a damage trail behind it
  // (width > 0 enables it).
  //
  // The trail covers the blade's whole recent PATH, not the one sliver it swept in
  // the current frame. That distinction is the entire weapon: at 60 frames a
  // second and twenty units a second the per-frame segment is a third of a unit
  // long, which is indistinguishable from the blade's own contact hit -- so the
  // "burning laser trail" was doing nothing a shuriken was not already doing, and
  // the weapon was, exactly as it looked, a prettier shuriken. With a remembered
  // path the trail is a swath several units wide that the whole line burns at
  // once, and the chakram becomes a thing that carves corridors.
  static constexpr int kTrailSamples = 10;
  float trailPathX[kTrailSamples] = {};
  float trailPathY[kTrailSamples] = {};
  int trailCount = 0;        // how many entries of the buffer are live
  int trailHead = 0;         // next slot to write
  float trailWidth = 0.0F;   // >0: burning laser trail (pulsar)
  float trailDamage = 0.0F;  // damage per trail tick
  float trailTick = 0.15F;   // seconds between trail damage ticks
  float trailTimer = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Bouncing projectile (orb)
struct BounceProjectile {
  float damage = 1.0F;
  int pierce = 0;
  float life = 2.5F;
  int maxBounces = 3;
  float bounceRange = 2.5F;
  float damageMul = 0.7F;
  int bounceCount = 0;
  float lastHitX = 0.0F;
  float lastHitY = 0.0F;
  float splashRadius = 0.0F; // >0: splash damage on each bounce (orb unique)
  bool infinite = false;     // never expires: the eternal Void Orb (bounce_infinite)
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
  // Child orbs still owed. A piece throws off `splits` of them ONCE, on its
  // first impact -- not on every bounce. Re-splitting per bounce multiplies
  // each generation by its whole bounce budget instead of by two, and the
  // cascade stops being a burst that dies down and becomes a multiplication
  // that outruns the game. The result is a fixed 1+2+4+8 tree per shot.
  int splits = 0;
  int depth = 0;
  bool splitUsed = false;
};

// Instant beam (beam)
struct BeamEffect {
  float damage = 1.0F;
  int pierce = 0;          // reduces crowd damage falloff
  float range = 8.0F;
  float width = 0.3F;
  float duration = 0.15F;
  float timer = 0.0F;
  float startX = 0.0F;
  float startY = 0.0F;
  float endX = 0.0F;
  float endY = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Melee sweep (scythe)
struct SweepEffect {
  float damage = 1.0F;
  float radius = 2.0F;
  float angle = 3.14F;      // radians
  float knockback = 2.0F;
  float duration = 0.2F;
  float timer = 0.0F;
  float startAngle = 0.0F;
  float endAngle = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Damage zone on ground (ember/zone)
struct ZoneEffect {
  float dps = 15.0F;
  int pierce = 0;          // reduces crowd damage falloff
  float radius = 1.2F;
  float duration = 4.0F;
  float timer = 0.0F;
  float tickTimer = 0.0F;
  float tickRate = 0.1F;
  // Which weapon slot laid this pool. The inferno weapon places its burning
  // ground at the Grave Bell, so "the oldest pool this weapon owns" has to be
  // answerable without also matching the pools every other weapon dropped.
  int weaponIndex = -1;
  // True when the fire ARRIVED from above rather than burning where it was
  // created. The two inferno weapons are the same pool entity and were drawn
  // identically, which is most of why they read as one weapon: the Ember Sprayer's
  // pools are left on the ground by a cone, while Ashfall's shells come down on
  // the Grave Bell. So Ashfall's pool is drawn as a shaft with an impact ring at
  // the top, and the ground pool keeps its low disc.
  bool fromAbove = false;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Chain lightning (storm evolution)
struct ChainLightning {
  float damage = 1.0F;
  int maxJumps = 4;
  float jumpRange = 2.5F;
  float damageMul = 0.6F;
  int jumpsDone = 0;
  float timer = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
  // The enemy the bolt just left. Kept so the next hop can prefer a target it
  // has not been to: without it a two-enemy pocket makes the bolt oscillate
  // A-B-A-B forever, which reads as a strobe, not as lightning.
  std::uint32_t lastTarget = 0;
  // Plain shards thrown off on the very first hit (Blizzard Rail). Unlike a
  // fork these are NOT bolts: they fly flat and do not arc, which is the read
  // that tells you the slug came apart rather than branched. One-shot: the
  // field is cleared once it has been paid out, so later hops do not re-shatter
  // and the cascade cannot run away.
  int shatter = 0;
  float shatterSpeed = 0.0F;
  float shatterSpread = 0.55F;
  // Remaining life once the jumps are done. The bolt used to be destroyed the
  // instant it ran out of targets, which at 0.05s per hop meant the whole
  // effect was over in about a fifth of a second -- too fast to read at all.
  // The bolt now hangs and fades, so a horde game can actually see its lightning.
  float linger = 0.0F;

  // --- The strike, in three beats -------------------------------------------
  //
  // The bolt used to go straight from "spawned" to "damaged", so it read as a
  // pop rather than a strike: the enemy was already flashing before the player
  // could tell what was about to happen. Now it converges first.
  //
  //   telegraph > 0  the bolt is aiming. Nothing damages yet. A ring on the
  //                  target shrinks over this window; it is the only warning.
  //   strike         the sky drop is drawing itself down, 0..1 over
  //                  kChainStrike. Scales the bolt's brightness and thickness.
  //
  // Both are set at spawn and only ever count down, so the render can derive the
  // whole animation from them without keeping its own clock.
  float telegraph = 0.0F;
  float strike = 0.0F;

  // The victim this bolt was AIMED at, still owed a hit.
  //
  // The first strike used to be applied by fireWeapons, before the bolt was even
  // spawned: it damaged the target at the same instant it created the bolt that
  // is supposed to be the thing doing the damage. That is precisely the "pop"
  // this animation exists to remove -- the health bar moved first and the
  // lightning arrived afterwards to take the credit. The hit now belongs to the
  // bolt and is paid when the ring closes, which is also what makes a lone enemy
  // take damage: the strike is not the bolt arcing onward, it is the bolt
  // landing, and it is paid whether or not there is anybody left to arc to.
  //
  // 0 means "nothing owed". Stored as a raw id like lastTarget, and validated
  // before use: an id can be recycled between the spawn and the landing.
  std::uint32_t pendingFirst = 0;
};

// A crescent of force that LEAVES the player and travels outward (Sunder).
//
// The difference from NovaRing is the whole reason this type exists: a nova is
// pinned to the player and only grows, so it always hits the same crowd in the
// same order. A wave leaves, and whatever it shoves keeps going.
struct WaveEffect {
  // How many victims one wave can remember. A wave's band is as LONG as it is
  // wide, so a target standing in it would otherwise be re-hit on every frame
  // for as long as the wave takes to pass it -- which is a stream of damage
  // wearing the costume of a single slash. Remembering the hits is what makes
  // "hits each enemy once" true rather than aspirational.
  static constexpr int kMaxHits = 24;
  std::uint32_t hits[kMaxHits] = {};
  int hitCount = 0;

  float damage = 1.0F;
  float speed = 6.0F;
  float width = 2.2F;      // half-thickness of the crescent
  // How far the crescent's horns open, in radians. This is the drawn shape, and
  // it is a rule rather than a number: a wide, slow, deep crescent reads as a
  // WALL you shove a rank down with (the Sundering Core), while a narrow, fast,
  // shallow one reads as a LASH that rakes (the Tidal Lash). They are the same
  // entity, and this is the field that tells them apart on screen.
  float spread = 0.9F;
  float angle = 0.0F;      // direction of travel, radians
  float knockback = 4.0F;

  // A hooking wave HERDS instead of shoving. Zero is the Sundering Core: what
  // the crescent touches is pushed along the crescent's own heading, which
  // clears space in front of the player.
  //
  // A hooked wave pushes it ACROSS the fan instead -- toward where the next
  // crescent is going -- so the Tidal Lash's three arcs work as a funnel that
  // hands each catch along rather than three independent pushes that each send
  // things down a different line. The weapon's description had been promising
  // exactly this ("dragging its catch into the next") for a full pass before the
  // entity had a field for it, which is the kind of lie that survives review
  // because nobody reads the number back out of the TOML.
  //
  // `hookSide` is which way the fan rotates, so the herd always goes the way
  // the next arc is actually thrown rather than whichever way is convenient.
  float hookPull = 0.0F;
  float hookSide = 1.0F;

  // How far off its own heading a hooked wave pushes. Mostly across the fan,
  // with a little still along it, so a hooked wave is a funnel and not a pure
  // sideswipe that throws the catch out of the fight entirely.
  static constexpr float kHookAngle = 1.15F; // radians, ~66 degrees

  float travelled = 0.0F;
  float range = 7.0F;      // despawns once it has covered this much
  float life = 0.5F;       // visual fade
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};

  // True once this entity has already been struck by this wave. The list is a
  // short linear scan: it is capped at kMaxHits and a wave is a local effect,
  // so a set would be more machinery than the problem deserves. When the list
  // is full the wave simply stops being selective, which bounds both the scan
  // and the entity count.
  [[nodiscard]] bool alreadyHit(std::uint32_t id) const {
    for (int i = 0; i < hitCount && i < kMaxHits; ++i) {
      if (hits[i] == id) return true;
    }
    return false;
  }
  void rememberHit(std::uint32_t id) {
    if (hitCount < kMaxHits) hits[hitCount++] = id;
  }
};

// Expanding nova ring (nova evolution)
struct NovaRing {
  float damagePerTick = 25.0F;
  int pierce = 0;          // reduces crowd damage falloff
  float maxRadius = 4.0F;
  float expandSpeed = 3.0F;
  float tickRate = 0.15F;
  float radius = 0.0F;
  float timer = 0.0F;
  float tickTimer = 0.0F;
  // A contracting ring runs the same code backwards. `expandSpeed` goes negative
  // and `pull` is added, which is how a single ring type serves both the Shock
  // Core (a wall going out) and the Void Nova (a knot coming in) without either
  // being a special case in the update.
  float pull = 0.0F;
  // Set once the ring has collapsed, so the centre detonation is paid exactly
  // one time rather than every tick the ring spends inside its own core radius.
  bool burstDone = false;
  float burstDamage = 0.0F;
  core::render::Color color{1.0F, 1.0F, 1.0F, 1.0F};
};

// Grave Bell (lure): a planted beacon that taunts the horde. The defining
// mechanic is the DRAG — anything caught inside `reach` is walked toward the
// bell, so a stationary beacon keeps funneling enemies into a small kill zone
// instead of the player having to hold a knife fight there. It is the only
// weapon that fights from a fixed spot, which is what makes it a good partner
// for fast melee and a bad one to stand next to.
struct Lure {
  float damage = 14.0F;    // damage per second inside the core
  int pierce = 0;          // reduces crowd damage falloff
  float radius = 1.4F;     // damage core radius
  float reach = 3.6F;      // outer edge where the drag starts
  float pull = 5.0F;       // inward drag, world units per second
  float life = 5.0F;       // seconds left before it dies
  float maxLife = 5.0F;    // original lifetime (render fade)
  float tickRate = 0.15F;  // damage ticks per second
  float tickTimer = 0.0F;
  int weaponIndex = -1;    // owning weapon slot (-1 = orphaned)
  core::render::Color color{0.6F, 0.4F, 1.0F, 1.0F};
};

struct Xp {
  float value = 1.0F;
};

// Persistent rotating beam (halo evolution): a spoke of light anchored to the
// player that sweeps around and damages everything along its length.
struct HaloBeam {
  float damage = 30.0F;    // damage per second along the beam
  int pierce = 0;          // reduces crowd damage falloff
  float length = 6.0F;     // beam reach from the player
  float width = 0.4F;      // beam thickness
  float angle = 0.0F;      // current rotation angle
  float spin = 2.0F;       // radians per second
  float knockback = 0.0F;  // outward shove applied along the beam (per second)
  // Where the spoke begins. 0 = at the player (a guard, the Radiant Halo). > 0 =
  // it starts further out, so there is a ring of safe ground at your feet and the
  // beam is a wall you keep OUT of rather than something that keeps you safe. It
  // is also what stops a wide, fast halo from being a strictly better halo: the
  // hole is the price of the reach.
  float inner = 0.0F;
  int weaponIndex = -1;    // owning weapon slot (-1 = orphaned)
  core::render::Color color{1.0F, 1.0F, 0.75F, 1.0F};
};

// Vortex evolution: a heavy suction zone anchored at a fixed distance from the
// player, circling them. It is NOT a damage pool like Nova/Zone — its whole
// identity is the inward drag: anything caught in `reach` is hauled toward the
// core, and anything that reaches the core itself is cut up. Several of these
// orbit at once, so a crowd gets corkscrewed into a kill box.
struct Vortex {
  float damage = 30.0F;    // damage per second once an enemy is inside the core
  int pierce = 0;          // reduces crowd damage falloff
  float radius = 1.3F;     // damage core radius
  float reach = 2.6F;      // outer edge where the pull starts
  float pull = 5.0F;       // inward drag, world units per second
  float orbitRadius = 2.6F;// distance from the player
  float spin = 1.7F;       // radians per second
  float angle = 0.0F;      // current position on the orbit
  float tickRate = 0.1F;   // damage ticks per second
  float tickTimer = 0.0F;
  // Collapse charge. A well with `collapseAt > 0` swallows for that many seconds
  // and then implodes: one large burst, and the well reopens at a new point on the
  // orbit and starts over. `collapseAt <= 0` (the Void Gyre) never collapses, so
  // its charge stays at 0 and this whole path is inert -- the two vortex weapons
  // share one type and differ only in whether it ends.
  float collapseAt = 0.0F;
  float charge = 0.0F;
  float burstDamage = 0.0F;
  float burstRadius = 0.0F;
  int weaponIndex = -1;    // owning weapon slot (-1 = orphaned)
  core::render::Color color{0.6F, 0.4F, 1.0F, 1.0F};
};

} // namespace game
