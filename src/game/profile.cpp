#include "game/profile.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace game {

const std::vector<SkinColor>& skinPalette() {
  // The default (index 0) is the original player blue, so an existing save or
  // a fresh profile looks exactly like the pre-settings game.
  static const std::vector<SkinColor> palette = {
      {"Azure",     {0.55F, 0.85F, 1.00F, 1.0F}},
      {"Crimson",   {0.95F, 0.30F, 0.32F, 1.0F}},
      {"Verdant",   {0.40F, 0.90F, 0.45F, 1.0F}},
      {"Amber",     {1.00F, 0.72F, 0.20F, 1.0F}},
      {"Violet",    {0.72F, 0.45F, 1.00F, 1.0F}},
      {"Frost",     {0.75F, 0.95F, 1.00F, 1.0F}},
      {"Rose",      {1.00F, 0.55F, 0.70F, 1.0F}},
      {"Onyx",      {0.28F, 0.30F, 0.36F, 1.0F}},
  };
  return palette;
}

const std::vector<OutlineStyle>& outlinePalette() {
  // Index 0 is always available. 1..3 are the tier rewards: killing an elite /
  // champion / overlord at least once (ever) permanently unlocks that outline.
  // Killing an overlord is "very cool", so it gets the most dramatic one: a
  // violet halo ring plus a second orbiting spark ring.
  static const std::vector<OutlineStyle> palette = {
      {"None",      {0.00F, 0.00F, 0.00F, 0.0F},  nullptr},
      {"Gold",      {1.00F, 0.85F, 0.20F, 1.0F}, "Kill an ELITE"},
      {"Ember",     {1.00F, 0.45F, 0.10F, 1.0F}, "Kill a CHAMPION"},
      {"Overlord",  {0.85F, 0.35F, 1.00F, 1.0F}, "Kill an OVERLORD"},
  };
  return palette;
}

bool Profile::unlockTier(int tier) {
  // tier 1 = elite, 2 = champion, 3 = overlord. Tier 0 is the normal enemy and
  // earns nothing, so it (and anything out of range) is a no-op.
  if (tier < 1 || tier > 3) return false;
  const UnlockMask bit = static_cast<UnlockMask>(1u << (tier - 1));
  if ((unlocks & bit) != 0u) return false;
  unlocks = static_cast<UnlockMask>(unlocks | bit);
  return true;
}

bool Profile::canUseOutline(int index) const {
  if (index <= 0) return true; // "None" is always legal
  if (index >= static_cast<int>(outlinePalette().size())) return false;
  // Outline i (1..3) is gated by unlock bit (i - 1): 1=elite, 2=champion,
  // 3=overlord. The vectors line up by construction, but index the bit by
  // style rather than assuming.
  const UnlockMask bit = static_cast<UnlockMask>(1u << (index - 1));
  return (unlocks & bit) != 0u;
}

void Profile::sanitize() {
  const int skins = static_cast<int>(skinPalette().size());
  if (skin < 0 || skin >= skins) skin = 0;
  const int outlines = static_cast<int>(outlinePalette().size());
  if (outline < 0 || outline >= outlines) outline = 0;
  // A hand-edited file could claim an outline whose unlock bit is missing;
  // fall back to "None" rather than letting the player wear something they
  // have not earned.
  if (!canUseOutline(outline)) outline = 0;
  // Drop unlock bits that point outside the palette (forward-compatible: a
  // future version adding styles still reads an old file cleanly).
  const UnlockMask valid = static_cast<UnlockMask>((1u << (outlines - 1)) - 1u);
  unlocks = static_cast<UnlockMask>(unlocks & valid);
}

Profile loadProfile(const std::string& path) {
  Profile p;
  std::ifstream in(path);
  if (!in) return p; // missing file => defaults (first launch)
  std::string line;
  while (std::getline(in, line)) {
    // Trim a trailing CR so a CRLF file written on Windows still parses.
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string key;
    ls >> key;
    if (key.empty() || key[0] == '#') continue;
    if (key == "skin") {
      ls >> p.skin;
    } else if (key == "outline") {
      ls >> p.outline;
    } else if (key == "unlock") {
      int tier = 0;
      ls >> tier;
      p.unlockTier(tier);
    }
    // Unknown keys are ignored so an older build can read a newer file.
  }
  p.sanitize();
  return p;
}

void saveProfile(const std::string& path, const Profile& profile) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return; // best-effort: an unwritable location just loses progress
  out << "v1\n";
  out << "skin " << profile.skin << '\n';
  out << "outline " << profile.outline << '\n';
  // One "unlock" line per earned tier keeps the file human-readable and
  // order-independent.
  for (int tier = 1; tier <= 3; ++tier) {
    if (profile.canUseOutline(tier)) out << "unlock " << tier << '\n';
  }
}

} // namespace game
