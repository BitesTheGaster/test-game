#include "game/content.hpp"

#include <toml++/toml.hpp>

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
  if (s == "nova") return AttackType::Nova;
  if (s == "inferno") return AttackType::Inferno;
  if (s == "pulsar") return AttackType::Pulsar;
  throw std::runtime_error(where + ": unknown attack_type \"" + s + "\"");
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
      def.name = requireString(*t, "name", where);
      def.desc = (*t)["desc"].value<std::string>().value_or("Auto-fires at the nearest enemy.");
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

      // Orbit
      def.orbitRadius = (*t)["orbit_radius"].value_or(1.2F);
      def.orbitSpeed = (*t)["orbit_speed"].value_or(2.0F);
      def.orbitCount = static_cast<int>((*t)["orbit_count"].value_or(2));

      // Bomb
      def.bombArcHeight = (*t)["bomb_arc_height"].value_or(2.0F);
      def.bombExplodeRadius = (*t)["bomb_explode_radius"].value_or(1.5F);
      def.bombKnockback = (*t)["bomb_knockback"].value_or(3.0F);
      def.bombFuse = (*t)["bomb_fuse"].value_or(0.0F);

      // Boomerang
      def.boomerangRange = (*t)["boomerang_range"].value_or(4.0F);
      def.boomerangReturnSpeed = (*t)["boomerang_return_speed"].value_or(1.5F);

      // Bounce
      def.bounceCount = static_cast<int>((*t)["bounce_count"].value_or(3));
      def.bounceRange = (*t)["bounce_range"].value_or(2.5F);
      def.bounceDamageMul = (*t)["bounce_damage_mul"].value_or(0.7F);
      def.bounceInfinite = (*t)["bounce_infinite"].value_or(false);

      // Beam
      def.beamRange = (*t)["beam_range"].value_or(8.0F);
      def.beamWidth = (*t)["beam_width"].value_or(0.3F);
      def.beamDuration = (*t)["beam_duration"].value_or(0.15F);

      // Sweep
      def.sweepAngle = (*t)["sweep_angle"].value_or(3.14F);
      def.sweepRadius = (*t)["sweep_radius"].value_or(2.0F);
      def.sweepKnockback = (*t)["sweep_knockback"].value_or(2.0F);
      def.sweepLead = (*t)["sweep_lead"].value_or(0.0F);

      // Zone
      def.zoneRadius = (*t)["zone_radius"].value_or(1.2F);
      def.zoneDuration = (*t)["zone_duration"].value_or(4.0F);
      def.zoneDps = (*t)["zone_dps"].value_or(15.0F);
      def.zoneMaxPools = static_cast<int>((*t)["zone_max_pools"].value_or(3));

      // Chain
      def.chainJumpRange = (*t)["chain_jump_range"].value_or(2.5F);
      def.chainMaxJumps = static_cast<int>((*t)["chain_max_jumps"].value_or(4));
      def.chainDamageMul = (*t)["chain_damage_mul"].value_or(0.6F);

      // Nova
      def.novaMaxRadius = (*t)["nova_max_radius"].value_or(4.0F);
      def.novaExpandSpeed = (*t)["nova_expand_speed"].value_or(3.0F);
      def.novaDamagePerTick = (*t)["nova_damage_per_tick"].value_or(25.0F);
      def.novaTickRate = (*t)["nova_tick_rate"].value_or(0.15F);

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
      def.name = requireString(*t, "name", where);
      def.hp = requireFloat(*t, "hp", where);
      def.speed = requireFloat(*t, "speed", where);
      def.touch = requireFloat(*t, "touch", where);
      def.radius = requireFloat(*t, "radius", where);
      def.xp = (*t)["xp"].value_or(1.0F);
      def.unlockAt = (*t)["unlock_at"].value_or(0.0F);
      def.weight = (*t)["weight"].value_or(1.0F);
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
      def.name = requireString(*t, "name", where);
      def.desc = requireString(*t, "desc", where);
      def.effect = requireString(*t, "effect", where);
      def.value = requireFloat(*t, "value", where);
      def.maxStacks = static_cast<int>((*t)["max_stacks"].value_or(5));
      def.kind = (*t)["kind"].value<std::string>().value_or("normal");
      def.weapon = (*t)["weapon"].value<std::string>().value_or("");
      def.level = static_cast<int>((*t)["level"].value_or(0));
      content.upgrades.push_back(std::move(def));
    }
  }

  if (content.weapons.empty() || content.enemies.empty() || content.upgrades.empty()) {
    throw std::runtime_error(dir.string() + ": content tables must not be empty");
  }
  return content;
}

} // namespace game