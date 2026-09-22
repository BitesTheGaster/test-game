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
      def.damage = requireFloat(*t, "damage", where);
      def.cooldown = requireFloat(*t, "cooldown", where);
      def.projectiles = static_cast<int>((*t)["projectiles"].value_or(1));
      def.projSpeed = (*t)["proj_speed"].value_or(12.0F);
      def.projLife = (*t)["proj_life"].value_or(1.4F);
      def.pierce = static_cast<int>((*t)["pierce"].value_or(0));
      def.spread = (*t)["spread"].value_or(0.16F);
      def.starter = (*t)["starter"].value_or(false);
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
