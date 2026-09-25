#pragma once

#include "core/render/color.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// ---------------------------------------------------------------------------
// Player profile: the small amount of data that persists BETWEEN runs.
//
// Stored as a tiny line-based text file next to the data dir (or the working
// directory), so it works on any machine without touching a system location:
//   v1
//   skin <index>
//   outline <index>
//   unlock <tier>
//
// Tiers are 1 = elite, 2 = champion, 3 = overlord. An outline becomes
// selectable once its tier has been unlocked (kill one of that tier ever).
// ---------------------------------------------------------------------------

// The fixed palette the player can pick from. Index 0 is the default.
struct SkinColor {
  const char* name;
  core::render::Color color;
};

const std::vector<SkinColor>& skinPalette();

// Outline styles unlocked by killing an elite / champion / overlord at least
// once. Index 0 is "none" and is always available.
struct OutlineStyle {
  const char* name;
  core::render::Color color;
  // Human-readable unlock requirement, shown while the style is locked.
  // (Named `requirement`, not `requires` — the latter is a C++20 keyword.)
  const char* requirement;
};

const std::vector<OutlineStyle>& outlinePalette();

// Bit i set => outline style i may be selected.
using UnlockMask = std::uint8_t;

constexpr UnlockMask kUnlockElite = 1u << 0;
constexpr UnlockMask kUnlockChampion = 1u << 1;
constexpr UnlockMask kUnlockOverlord = 1u << 2;

struct Profile {
  int skin = 0;            // index into skinPalette()
  int outline = 0;         // index into outlinePalette()
  UnlockMask unlocks = 0;  // which outlines have been earned

  // Grants the outline for `tier` (1 elite, 2 champion, 3 overlord). Returns
  // true when this actually unlocked something new.
  bool unlockTier(int tier);
  [[nodiscard]] bool canUseOutline(int index) const;
  // Clamps the selection to something currently legal (used after loading a
  // profile that references a locked outline).
  void sanitize();
};

// Reads a profile from `path`; a missing or malformed file yields defaults.
Profile loadProfile(const std::string& path);
// Best-effort write. Silently does nothing if the file cannot be opened.
void saveProfile(const std::string& path, const Profile& profile);

// Default profile file name (resolved next to the assets/data dir).
constexpr const char* kProfileFileName = "player.profile";

} // namespace game
