#include "game/content.hpp"

#include "core/render/batcher.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <stdexcept>

namespace game {
namespace {

core::render::Color parseColor(const toml::node& node, const std::string& where) {
  const auto* arr = node.as_array();
  if (arr == nullptr || arr->size() != 4) {
    throw std::runtime_error(where + ": color must be an array of 4 floats");
  }
  core::render::Color c{};
  std::size_t i = 0;
  for (const auto& v : *arr) {
    const auto f = v.value<float>();
    if (!f) {
      throw std::runtime_error(where + ": color entries must be floats");
    }
    switch (i) {
      case 0: c.r = *f; break;
      case 1: c.g = *f; break;
      case 2: c.b = *f; break;
      case 3: c.a = *f; break;
      default: break;
    }
    ++i;
  }
  return c;
}

std::string requireString(const toml::table& t, const char* key, const std::string& where) {
  const auto v = t[key].value<std::string>();
  if (!v) {
    throw std::runtime_error(where + ": missing string field \"" + key + "\"");
  }
  return *v;
}

// A string that will be DRAWN, so it also has to be spellable in the in-game
// font: ASCII 32..96 with lowercase folded onto uppercase. Anything else -- a
// typographic dash, a curly quote, an emoji -- does not fail, it silently draws
// as '?', which is worse than a crash because the content looks fine in the
// .toml and wrong on screen.
//
// This check used to run only over the manual pages, which left every weapon and
// upgrade name and description unchecked. Four shipped descriptions carried an
// em-dash and every player met four question marks on the level-up screen.
std::string requireDrawableString(const toml::table& t, const char* key,
                                  const std::string& where) {
  std::string s = requireString(t, key, where);
  if (!core::render::fontSupports(s)) {
    throw std::runtime_error(where + ": field \"" + std::string(key) +
                             "\" contains a character the in-game font cannot draw "
                             "(allowed: ASCII 32..96): " + s);
  }
  return s;
}

float requireFloat(const toml::table& t, const char* key, const std::string& where) {
  const auto v = t[key].value<float>();
  if (!v) {
    throw std::runtime_error(where + ": missing float field \"" + key + "\"");
  }
  return *v;
}

AttackType parseAttackType(const toml::table& t, const std::string& where) {
  const auto v = t["attack_type"].value<std::string>();
  if (!v) return AttackType::Projectile;
  std::string s = *v;
  if (s == "projectile") return AttackType::Projectile;
  if (s == "orbit") return AttackType::Orbit;
  if (s == "cone") return AttackType::Cone;
  if (s == "bomb") return AttackType::Bomb;
  if (s == "boomerang") return AttackType::Boomerang;
  if (s == "bounce") return AttackType::Bounce;
  if (s == "beam") return AttackType::Beam;
  if (s == "sweep") return AttackType::Sweep;
  if (s == "zone") return AttackType::Zone;
  if (s == "chain") return AttackType::Chain;
  if (s == "wave") return AttackType::Wave;
  if (s == "nova") return AttackType::Nova;
  if (s == "inferno") return AttackType::Inferno;
  if (s == "pulsar") return AttackType::Pulsar;
  if (s == "halo") return AttackType::Halo;
  if (s == "vortex") return AttackType::Vortex;
  if (s == "prism") return AttackType::Prism;
  if (s == "lure") return AttackType::Lure;
  throw std::runtime_error(where + ": unknown attack_type \"" + s + "\"");
}

// Every key in a data table must be one the loader READS. A key it does not read is
// an instruction that looks authoritative and does nothing -- the worst failure mode
// a data file has, and the only one a designer cannot see. `threshold = 0.25` on a
// card whose effect hardcodes the threshold produces a card that behaves correctly
// and a file that lies: the number is right and editing it changes nothing.
//
// This asks the one question no test of the OUTPUT can answer, which is "did you
// read everything you were handed", and it asks it at load, where a broken data file
// is a failed load rather than a card that quietly under-delivers. Two cards in the
// milestone tree carried exactly such a key for two rounds -- a threshold that never
// tightened a band and an area that never widened a blast -- and neither was caught
// by a behavioural test, because the behaviour was right and only the file was wrong.
//
// Keys are named explicitly rather than derived, because the derived version of this
// check is the one that passes: ask the table what it contains and you have learned
// nothing.
void rejectUnknownKeys(const toml::table& t, std::initializer_list<std::string_view> known,
                       const std::string& where, const std::string& entryId) {
  for (const auto& [key, node] : t) {
    (void)node;
    bool ok = false;
    for (const auto& k : known) {
      if (key == k) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      throw std::runtime_error(where + ": entry '" + entryId + "' has key '" +
                               std::string(key) + "', which the loader does not read");
    }
  }
}

toml::table parseTable(const std::filesystem::path& file) {
  try {
    return toml::parse_file(file.string());
  } catch (const toml::parse_error& err) {
    throw std::runtime_error(file.string() + ": " + std::string(err.description()));
  }
}

} // namespace

const WeaponDef* Content::weapon(std::string_view id) const {
  for (const auto& w : weapons) {
    if (w.id == id) {
      return &w;
    }
  }
  return nullptr;
}

const EnemyDef* Content::enemy(std::string_view id) const {
  for (const auto& e : enemies) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

const UpgradeDef* Content::upgrade(std::string_view id) const {
  for (const auto& u : upgrades) {
    if (u.id == id) {
      return &u;
    }
  }
  return nullptr;
}

const ManualPage* Content::manualPage(std::string_view id) const {
  for (const auto& p : manual) {
    if (p.id == id) {
      return &p;
    }
  }
  return nullptr;
}

Content loadContent(const std::filesystem::path& dir) {
  Content content;

  // --- weapons.toml ---------------------------------------------------------
  {
    const auto file = dir / "weapons.toml";
    const toml::table tbl = parseTable(file);
    const auto* arr = tbl["weapon"].as_array();
    if (arr == nullptr) {
      throw std::runtime_error(file.string() + ": missing [[weapon]] array");
    }
    for (const auto& node : *arr) {
      const auto* t = node.as_table();
      if (t == nullptr) {
        throw std::runtime_error(file.string() + ": [[weapon]] entry is not a table");
      }
      const std::string where = file.string();
      WeaponDef def;
      def.id = requireString(*t, "id", where);
      rejectUnknownKeys(*t,
                        {"id", "name", "desc", "attack_type", "damage", "cooldown",
                        "projectiles", "proj_speed", "proj_life", "pierce", "spread",
                        "proj_color", "starter", "orbit_radius", "orbit_speed",
                        "orbit_count", "homing", "cone_angle", "cone_range",
                        "cone_tick_rate", "bomb_arc_height", "bomb_explode_radius",
                        "bomb_knockback", "bomb_fuse", "bomb_on_target",
                        "boomerang_range", "boomerang_return_speed", "bounce_count",
                        "bounce_range", "bounce_damage_mul", "bounce_infinite",
                        "sweep_angle", "sweep_radius", "sweep_knockback", "beam_range",
                        "beam_width", "beam_duration", "chill_mul", "chill_time",
                        "bomb_ahead", "lure_radius", "lure_reach", "lure_pull",
                        "lure_dps", "lure_duration", "lure_tick_rate",
                        "lure_max_beacons", "cone_bite", "cone_bite_max",
                        "nova_max_radius", "nova_expand_speed", "nova_damage_per_tick",
                        "nova_tick_rate", "sweep_lead", "chain_jump_range",
                        "chain_max_jumps", "chain_damage_mul", "requires",
                        "reaim_range", "reaim_turn", "nova_contract", "nova_pull",
                        "nova_burst_damage", "zone_radius", "zone_dps",
                        "zone_duration", "halo_knockback", "halo_inner",
                        "chain_shatter", "inferno_binds_to_lure", "zone_from_above",
                        "bounce_splits", "wave_speed", "wave_range", "wave_width",
                        "wave_knockback", "wave_damage_mul", "wave_count",
                        "wave_arc_step", "wave_spread", "wave_hook_pull",
                        "vortex_radius", "vortex_reach", "vortex_pull", "vortex_orbit",
                        "vortex_orbit_speed", "vortex_tick_rate", "vortex_crowd",
                        "prism_range", "prism_width", "prism_max_targets",
                        "prism_ricochet", "vortex_collapse_at", "vortex_burst_damage",
                        "vortex_burst_radius", "aura_radius", "aura_dps", "aura_tick",
                        "aura_chill_mul", "aura_chill_time"},
                        where, def.id);
      def.name = requireDrawableString(*t, "name", where);
      def.desc = (*t)["desc"].value<std::string>().value_or("Auto-fires at the nearest enemy.");
      if (!core::render::fontSupports(def.desc)) {
        throw std::runtime_error(where + ": weapon \"" + def.id +
                                 "\" desc contains a character the in-game font "
                                 "cannot draw (allowed: ASCII 32..96): " + def.desc);
      }
      def.attackType = parseAttackType(*t, where);
      def.damage = requireFloat(*t, "damage", where);
      def.cooldown = requireFloat(*t, "cooldown", where);
      def.projectiles = static_cast<int>((*t)["projectiles"].value_or(1));
      def.projSpeed = (*t)["proj_speed"].value_or(12.0F);
      def.projLife = (*t)["proj_life"].value_or(1.4F);
      def.pierce = static_cast<int>((*t)["pierce"].value_or(0));
      def.spread = (*t)["spread"].value_or(0.16F);
      def.starter = (*t)["starter"].value_or(false);
      def.homing = (*t)["homing"].value_or(false);
      const auto* colorNode = t->get("proj_color");
      if (colorNode != nullptr) {
        def.projColor = parseColor(*colorNode, where);
      }
      if (const auto reqArr = (*t)["requires"].as_array(); reqArr != nullptr) {
        for (const auto& node : *reqArr) {
          const auto s = node.value<std::string>();
          if (!s) {
            throw std::runtime_error(where + ": requires entries must be strings");
          }
          def.prereqs.push_back(*s);
        }
      }

      // Cone
      def.coneAngle = (*t)["cone_angle"].value_or(0.8F);
      def.coneRange = (*t)["cone_range"].value_or(2.5F);
      def.coneTickRate = (*t)["cone_tick_rate"].value_or(0.1F);
      def.coneBite = (*t)["cone_bite"].value_or(0.0F);
      def.coneBiteMax = (*t)["cone_bite_max"].value_or(4.0F);
      def.coneEmberAt = (*t)["cone_ember_at"].value_or(0.0F);
      def.coneEmberRadius = (*t)["cone_ember_radius"].value_or(1.0F);
      def.coneEmberDuration = (*t)["cone_ember_duration"].value_or(2.5F);

      // Orbit
      def.orbitRadius = (*t)["orbit_radius"].value_or(1.2F);
      def.orbitSpeed = (*t)["orbit_speed"].value_or(2.0F);
      def.orbitCount = static_cast<int>((*t)["orbit_count"].value_or(2));
      def.orbitWindow = (*t)["orbit_window"].value_or(false);

      // Bomb
      def.bombArcHeight = (*t)["bomb_arc_height"].value_or(2.0F);
      def.bombExplodeRadius = (*t)["bomb_explode_radius"].value_or(1.5F);
      def.bombKnockback = (*t)["bomb_knockback"].value_or(3.0F);
      def.bombFuse = (*t)["bomb_fuse"].value_or(0.0F);
      def.bombAhead = (*t)["bomb_ahead"].value_or(0.0F);
      def.bombOnTarget = (*t)["bomb_on_target"].value_or(false);
      def.reaimRange = (*t)["reaim_range"].value_or(0.0F);
      def.reaimTurn = (*t)["reaim_turn"].value_or(0.0F);

      // Boomerang
      def.boomerangRange = (*t)["boomerang_range"].value_or(4.0F);
      def.boomerangReturnSpeed = (*t)["boomerang_return_speed"].value_or(1.5F);

      // Bounce
      def.bounceCount = static_cast<int>((*t)["bounce_count"].value_or(3));
      def.bounceRange = (*t)["bounce_range"].value_or(2.5F);
      def.bounceDamageMul = (*t)["bounce_damage_mul"].value_or(0.7F);
      def.bounceInfinite = (*t)["bounce_infinite"].value_or(false);
      def.bounceSplits = static_cast<int>((*t)["bounce_splits"].value_or(0));

      // Beam
      def.beamRange = (*t)["beam_range"].value_or(8.0F);
      def.beamWidth = (*t)["beam_width"].value_or(0.3F);
      def.beamDuration = (*t)["beam_duration"].value_or(0.15F);

      // Chill
      def.chillMul = (*t)["chill_mul"].value_or(0.0F);
      def.chillTime = (*t)["chill_time"].value_or(0.0F);
      def.auraRadius = (*t)["aura_radius"].value_or(0.0F);
      def.auraDps = (*t)["aura_dps"].value_or(0.0F);
      def.auraTick = (*t)["aura_tick"].value_or(0.10F);
      def.auraChillMul = (*t)["aura_chill_mul"].value_or(0.0F);
      def.auraChillTime = (*t)["aura_chill_time"].value_or(0.0F);

      // Halo
      def.haloKnockback = (*t)["halo_knockback"].value_or(0.0F);
      def.haloInner = (*t)["halo_inner"].value_or(0.0F);

      // Sweep
      def.sweepAngle = (*t)["sweep_angle"].value_or(3.14F);
      def.sweepRadius = (*t)["sweep_radius"].value_or(2.0F);
      def.sweepKnockback = (*t)["sweep_knockback"].value_or(2.0F);
      def.sweepLead = (*t)["sweep_lead"].value_or(0.0F);
      def.sweepHook = (*t)["sweep_hook"].value_or(false);

      // Zone
      def.zoneRadius = (*t)["zone_radius"].value_or(1.2F);
      def.zoneDuration = (*t)["zone_duration"].value_or(4.0F);
      def.zoneDps = (*t)["zone_dps"].value_or(15.0F);
      def.zoneFromAbove = (*t)["zone_from_above"].value_or(false);
      def.zoneMaxPools = static_cast<int>((*t)["zone_max_pools"].value_or(3));

      // Chain
      def.chainJumpRange = (*t)["chain_jump_range"].value_or(2.5F);
      def.chainMaxJumps = static_cast<int>((*t)["chain_max_jumps"].value_or(4));
      def.chainDamageMul = (*t)["chain_damage_mul"].value_or(0.6F);
      def.chainShatter = static_cast<int>((*t)["chain_shatter"].value_or(0));
      def.chainShatterSpeed = (*t)["chain_shatter_speed"].value_or(15.0F);
      def.chainShatterSpread = (*t)["chain_shatter_spread"].value_or(0.55F);
      def.infernoBindsToLure = (*t)["inferno_binds_to_lure"].value_or(false);

      // Wave
      def.waveSpeed = (*t)["wave_speed"].value_or(6.0F);
      def.waveRange = (*t)["wave_range"].value_or(7.0F);
      def.waveWidth = (*t)["wave_width"].value_or(2.2F);
      def.waveKnockback = (*t)["wave_knockback"].value_or(4.0F);
      def.waveDamageMul = (*t)["wave_damage_mul"].value_or(0.8F);
      def.waveCount = static_cast<int>((*t)["wave_count"].value_or(1));
      def.waveArcStep = (*t)["wave_arc_step"].value_or(0.0F);
      def.waveHookPull = (*t)["wave_hook_pull"].value_or(0.0F);
      def.waveSpread = (*t)["wave_spread"].value_or(0.9F);

      // Nova
      def.novaMaxRadius = (*t)["nova_max_radius"].value_or(4.0F);
      def.novaExpandSpeed = (*t)["nova_expand_speed"].value_or(3.0F);
      def.novaDamagePerTick = (*t)["nova_damage_per_tick"].value_or(25.0F);
      def.novaTickRate = (*t)["nova_tick_rate"].value_or(0.15F);
      def.novaContract = (*t)["nova_contract"].value_or(false);
      def.novaPull = (*t)["nova_pull"].value_or(0.0F);
      def.novaBurstDamage = (*t)["nova_burst_damage"].value_or(0.0F);
      def.novaEcho = (*t)["nova_echo"].value_or(0.0F);

      // Vortex
      def.vortexRadius = (*t)["vortex_radius"].value_or(1.3F);
      def.vortexReach = (*t)["vortex_reach"].value_or(2.6F);
      def.vortexPull = (*t)["vortex_pull"].value_or(4.0F);
      def.vortexOrbit = (*t)["vortex_orbit"].value_or(2.6F);
      def.vortexOrbitSpeed = (*t)["vortex_orbit_speed"].value_or(1.8F);
      def.vortexTickRate = (*t)["vortex_tick_rate"].value_or(0.1F);
      def.vortexCollapseAt = (*t)["vortex_collapse_at"].value_or(0.0F);
      def.vortexCrowd = (*t)["vortex_crowd"].value_or(0.0F);
      def.vortexBurstDamage = (*t)["vortex_burst_damage"].value_or(0.0F);
      def.vortexBurstRadius = (*t)["vortex_burst_radius"].value_or(0.0F);

      // Prism
      def.prismRange = (*t)["prism_range"].value_or(9.0F);
      def.prismWidth = (*t)["prism_width"].value_or(0.45F);
      def.prismMaxTargets = static_cast<int>((*t)["prism_max_targets"].value_or(6));
      def.prismRicochet = (*t)["prism_ricochet"].value_or(4.0F);

      // Lure
      def.lureRadius = (*t)["lure_radius"].value_or(1.4F);
      def.lureReach = (*t)["lure_reach"].value_or(3.6F);
      def.lurePull = (*t)["lure_pull"].value_or(5.0F);
      def.lureDps = (*t)["lure_dps"].value_or(14.0F);
      def.lureDuration = (*t)["lure_duration"].value_or(5.0F);
      def.lureTickRate = (*t)["lure_tick_rate"].value_or(0.15F);
      def.lureMaxBeacons = static_cast<int>((*t)["lure_max_beacons"].value_or(2));

      content.weapons.push_back(std::move(def));
    }
  }

  // --- enemies.toml ---------------------------------------------------------
  {
    const auto file = dir / "enemies.toml";
    const toml::table tbl = parseTable(file);
    const auto* arr = tbl["enemy"].as_array();
    if (arr == nullptr) {
      throw std::runtime_error(file.string() + ": missing [[enemy]] array");
    }
    for (const auto& node : *arr) {
      const auto* t = node.as_table();
      if (t == nullptr) {
        throw std::runtime_error(file.string() + ": [[enemy]] entry is not a table");
      }
      const std::string where = file.string();
      EnemyDef def;
      def.id = requireString(*t, "id", where);
      rejectUnknownKeys(*t,
                        {"id", "name", "hp", "speed", "speed_ramp", "touch", "radius",
                        "xp", "unlock_at", "weight", "color", "shape", "fast"},
                        where, def.id);
      def.name = requireDrawableString(*t, "name", where);
      def.hp = requireFloat(*t, "hp", where);
      def.speed = requireFloat(*t, "speed", where);
      def.touch = requireFloat(*t, "touch", where);
      def.radius = requireFloat(*t, "radius", where);
      def.xp = (*t)["xp"].value_or(1.0F);
      def.unlockAt = (*t)["unlock_at"].value_or(0.0F);
      def.weight = (*t)["weight"].value_or(1.0F);
      def.speedRampMax = (*t)["speed_ramp"].value_or(0.0F);
      def.fast = (*t)["fast"].value_or(false);
      // A ramp that would push a type past twice its authored speed would make
      // the late game unreadable, and a `fast` type with no ramp is a
      // contradiction (slow, then suddenly quick with nothing in between). Both
      // are almost certainly typos in the data rather than intent, so the
      // loader refuses them instead of letting the roster drift.
      if (def.speedRampMax < 0.0F) {
        throw std::runtime_error(where + ": enemy \"" + def.id +
                                 "\" has a negative speed_ramp");
      }
      if (def.speedRampMax > kSpeedRampCeiling) {
        throw std::runtime_error(where + ": enemy \"" + def.id +
                                 "\" has speed_ramp above the ceiling");
      }
      if (def.fast && def.speedRampMax <= 0.0F) {
        throw std::runtime_error(where + ": enemy \"" + def.id +
                                 "\" is flagged fast but has no speed_ramp");
      }
      const auto* colorNode = t->get("color");
      if (colorNode == nullptr) {
        throw std::runtime_error(where + ": missing field \"color\"");
      }
      def.color = parseColor(*colorNode, where);
      const auto shape = (*t)["shape"].value<std::string>();
      def.circle = !shape || *shape == "circle";
      content.enemies.push_back(std::move(def));
    }
  }

  // --- upgrades.toml --------------------------------------------------------
  {
    const auto file = dir / "upgrades.toml";
    const toml::table tbl = parseTable(file);
    const auto* arr = tbl["upgrade"].as_array();
    if (arr == nullptr) {
      throw std::runtime_error(file.string() + ": missing [[upgrade]] array");
    }
    for (const auto& node : *arr) {
      const auto* t = node.as_table();
      if (t == nullptr) {
        throw std::runtime_error(file.string() + ": [[upgrade]] entry is not a table");
      }
      const std::string where = file.string();
      UpgradeDef def;
      def.id = requireString(*t, "id", where);
      def.name = requireDrawableString(*t, "name", where);
      def.desc = requireDrawableString(*t, "desc", where);
      def.effect = requireString(*t, "effect", where);
      def.value = requireFloat(*t, "value", where);
      def.maxStacks = static_cast<int>((*t)["max_stacks"].value_or(5));
      def.kind = (*t)["kind"].value<std::string>().value_or("normal");
      def.weapon = (*t)["weapon"].value<std::string>().value_or("");
      def.level = static_cast<int>((*t)["level"].value_or(0));
      def.group = (*t)["group"].value_or(std::string());
      // Only a GLOBAL card can occupy an item slot: kind "normal", and no weapon
      // of its own. A card for one weapon is `kind = "normal"` too, and keying the
      // rule off `kind` alone charged all 92 of them -- which is the exact
      // mistake this comment exists to prevent, because a per-weapon card is the
      // one kind of upgrade that provably cannot stack its way to global reach.
      //
      // A unique is a once-per-run treasure and a milestone already closes two
      // other axes for the rest of the run, so neither competes for the eight
      // slots: charging them would fill the pool with cards nobody could take, and
      // charging a milestone would bill the player twice for the same exclusivity.
      // Everything else does, unless the file says otherwise; see the `slot`
      // field's comment in content.hpp for what the limit is actually for.
      def.slot = (*t)["slot"].value_or(def.kind == "normal" && def.weapon.empty());
      def.after = (*t)["after"].value_or("");

      rejectUnknownKeys(*t,
                        {"id", "name", "desc", "effect", "value", "max_stacks",
                         "kind", "weapon", "level", "group", "slot", "after"},
                        where, def.id);
      content.upgrades.push_back(std::move(def));
    }
  }

  // --- manual.toml (optional) -------------------------------------------------
  // The in-game manual. Missing file => no manual (the screen degrades to a
  // "not available in this build" note), because a stripped distribution should
  // still run. Present but malformed, or containing text the bitmap font cannot
  // draw, IS an error: a screen full of question marks is worse than a failed
  // load, and only a test can catch the difference.
  {
    const auto file = dir / "manual.toml";
    if (std::filesystem::exists(file)) {
      const toml::table tbl = parseTable(file);
      const auto* arr = tbl["page"].as_array();
      if (arr == nullptr) {
        throw std::runtime_error(file.string() + ": missing [[page]] array");
      }
      for (const auto& node : *arr) {
        const auto* t = node.as_table();
        if (t == nullptr) {
          throw std::runtime_error(file.string() + ": [[page]] entry is not a table");
        }
        ManualPage page;
        page.id = requireString(*t, "id", file.string());
        rejectUnknownKeys(*t, {"id", "title", "lines"}, file.string(), page.id);
        page.title = requireString(*t, "title", file.string());
        const auto* lines = t->get("lines");
        const auto* lineArr = lines != nullptr ? lines->as_array() : nullptr;
        if (lineArr == nullptr) {
          throw std::runtime_error(file.string() + ": page \"" + page.id +
                                   "\" has no \"lines\" array");
        }
        std::size_t lineNo = 0;
        for (const auto& ln : *lineArr) {
          const auto s = ln.value<std::string>();
          if (!s) {
            throw std::runtime_error(file.string() + ": page \"" + page.id +
                                     "\" line entries must be strings");
          }
          ++lineNo;
          // The font is ASCII 32..96 with lowercase folded onto uppercase. A
          // typographic dash or a curly quote would silently draw as '?', so it
          // is rejected here where the file and line number are known.
          if (!core::render::fontSupports(*s)) {
            throw std::runtime_error(file.string() + ": page \"" + page.id + "\" line " +
                                     std::to_string(lineNo) +
                                     " contains a character the in-game font cannot "
                                     "draw (allowed: ASCII 32..96)");
          }
          page.lines.push_back(*s);
        }
        if (page.lines.empty()) {
          throw std::runtime_error(file.string() + ": page \"" + page.id + "\" is empty");
        }
        content.manual.push_back(std::move(page));
      }
      // Two pages with the same id would make the page-jump list ambiguous.
      for (std::size_t i = 0; i < content.manual.size(); ++i) {
        for (std::size_t k = i + 1; k < content.manual.size(); ++k) {
          if (content.manual[i].id == content.manual[k].id) {
            throw std::runtime_error(file.string() + ": duplicate manual page id \"" +
                                     content.manual[i].id + "\"");
          }
        }
      }
    }
  }

  if (content.weapons.empty() || content.enemies.empty() || content.upgrades.empty()) {
    throw std::runtime_error(dir.string() + ": content tables must not be empty");
  }
  return content;
}

} // namespace game