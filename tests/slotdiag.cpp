// Diagnostic: play a real run headless and report what the slot cap does to the
// card stream. Not a test -- a measurement. Run:
//   ./build/debug/tests/game_tests "[slotdiag]" -s
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game/content.hpp"
#include "game/game.hpp"

TEST_CASE("slotdiag", "[slotdiag]") {
  const auto content = game::loadContent(GAME_ASSETS_DIR "/data");
  game::Game g{content, 7};
  g.testDisableWaves();

  std::map<std::string, int> seen;
  std::vector<std::string> log;
  int starved = 0;    // level-ups where the only card was "continue"
  int depthTakes = 0; // re-takes of a slotted card already held
  int newAxisTakes = 0;
  int weaponTakes = 0;
  int uniqueTakes = 0;
  int offeredWhileMaxed = 0;
  std::vector<int> emptyLevels;
  std::vector<std::string> emptyWhy;

  // A faithful autoplayer: take whatever is on the screen, preferring a new
  // weapon while the arsenal has room (that is what a player does -- a weapon is
  // always better than a stat card early), then a new axis, then a deepening.
  // The first version of this diagnostic owned ONE weapon and reported 47 empty
  // level-up screens, which said more about the diagnostic than about the game:
  // 92 of the 160 cards are per-weapon and a player with one weapon can never
  // see them.
  const auto pickFrom = [&](const std::vector<std::string>& ids) -> int {
    int weapon = -1;
    int newAxis = -1;
    int depth = -1;
    int other = -1;
    for (std::size_t k = 0; k < ids.size(); ++k) {
      const auto& id = ids[k];
      if (id == "<skip>") continue;
      const auto* w = content.weapon(id.c_str());
      if (w != nullptr) {
        if (weapon < 0) weapon = static_cast<int>(k);
        continue;
      }
      const auto* card = content.upgrade(id.c_str());
      if (card == nullptr) continue;
      if (!card->slot) {
        if (other < 0) other = static_cast<int>(k);
        continue;
      }
      if (g.testUpgradeStacks(g.testUpgradeContentIndex(id.c_str())) == 0) {
        if (newAxis < 0) newAxis = static_cast<int>(k);
      } else if (depth < 0) {
        depth = static_cast<int>(k);
      }
    }
    // Weapons only while there is room: past the cap a weapon offer is a card
    // that does nothing, so preferring it would make this diagnostic lie.
    if (weapon >= 0 && g.testWeaponCount() < g.testWeaponCap()) return weapon;
    if (newAxis >= 0) return newAxis;
    if (other >= 0) return other;
    return depth;
  };

  for (int level = 1; level <= 140; ++level) {
    g.testSetLevel(level);
    const int cap = g.testItemSlotCap();
    const int used = g.testUsedItemSlots();
    const auto ids = g.testChoiceIds();
    std::set<std::string> distinct;
    for (const auto& id : ids) {
      ++seen[id];
      distinct.insert(id);
      // A card that is already maxed must never reach the screen. If this
      // counter is non-zero the offer is ignoring max_stacks, which is a real
      // bug and not a diagnostic artefact.
      const auto* card = content.upgrade(id.c_str());
      if (card == nullptr) continue;
      const int st = g.testUpgradeStacks(g.testUpgradeContentIndex(id.c_str()));
      if (st >= card->maxStacks) {
        ++offeredWhileMaxed;
        std::printf("!! OFFERED WHILE MAXED: %s (%d/%d) at level %d\n", id.c_str(), st,
                    card->maxStacks, level);
      }
    }
    if (distinct.size() <= 1 || (distinct.size() == 1 && distinct.count("<skip>"))) {
      ++starved;
      emptyLevels.push_back(level);
      // WHY it was empty. This is the whole question the number answers, and
      // "38" on its own is not an answer: an empty screen because a coin flip
      // went the wrong way is a bug, and an empty screen because a 140-level run
      // has genuinely consumed every card in the game is arithmetic. The
      // difference is whether uniques were still on the table.
      //
      // The two counts use the SAME filters as buildChoices, including the ones
      // that first version of this diagnostic skipped and got a wrong answer
      // from: 46 stat cards looked available at level 102 when 7 of them were
      // deepening room behind a full bar and the rest belonged to weapons this
      // run never picked. Counting "cards in the content file" and calling it
      // "cards on the pool" is how a diagnostic talks you into fixing nothing.
      int uniquesLeft = 0;
      int normalsLeft = 0;
      for (std::size_t i = 0; i < content.upgrades.size(); ++i) {
        const auto& u = content.upgrades[i];
        if (g.testUpgradeStacks(static_cast<int>(i)) >= u.maxStacks) continue;
        if (!u.weapon.empty() && !g.testOwnsWeapon(u.weapon)) continue;
        if (u.kind == "unique") {
          ++uniquesLeft;
        } else if (u.kind == "normal") {
          if (u.slot && g.testUpgradeStacks(static_cast<int>(i)) == 0 &&
              used >= cap) {
            continue; // would open a new slot and the bar is full
          }
          ++normalsLeft;
        }
      }
      emptyWhy.push_back("L" + std::to_string(level) + " uniques=" +
                          std::to_string(uniquesLeft) + " normals=" +
                          std::to_string(normalsLeft));
    }
    if (level % 8 == 0 || level <= 4) {
      log.push_back("L" + std::to_string(level) + " cap=" + std::to_string(cap) +
                    " used=" + std::to_string(used) + " weapons=" +
                    std::to_string(g.testWeaponCount()) + " cards=" +
                    std::to_string(distinct.size()) + " [" + [&] {
                      std::string s;
                      for (const auto& id : ids) { if (!s.empty()) s += " "; s += id; }
                      return s;
                    }() + "]");
    }
    const int pick = pickFrom(ids);
    if (pick < 0) continue;
    const std::string& id = ids[static_cast<std::size_t>(pick)];
    if (content.weapon(id.c_str()) != nullptr) {
      g.testAddWeapon(g.testWeaponContentIndex(id.c_str()));
      ++weaponTakes;
      continue;
    }
    const auto* card = content.upgrade(id.c_str());
    if (card == nullptr) continue;
    const int idx = g.testUpgradeContentIndex(card->id.c_str());
    if (!g.testGrantUpgrade(idx)) continue;
    if (card->slot) {
      if (g.testUpgradeStacks(idx) == 1) ++newAxisTakes; else ++depthTakes;
    } else if (card->kind == "unique") {
      ++uniqueTakes;
    }
  }

  std::printf("\n=== SLOT LADDER (every 8th level) ===\n");
  for (const auto& l : log) std::printf("%s\n", l.c_str());
  std::printf("\n=== SUMMARY over 140 levels ===\n");
  std::printf("slots used at the end  : %d / %d\n", g.testUsedItemSlots(),
              g.testItemSlotCap());
  std::printf("weapons at the end     : %d\n", g.testWeaponCount());
  std::printf("distinct cards offered : %zu\n", seen.size());
  std::printf("EMPTY level-ups        : %d / 140\n", starved);
  std::printf("takes: weapon=%d newAxis=%d depth=%d unique=%d\n", weaponTakes,
              newAxisTakes, depthTakes, uniqueTakes);
  std::printf("offered while maxed    : %d\n", offeredWhileMaxed);
  std::printf("empty at levels        : ");
  for (int l : emptyLevels) std::printf("%d ", l);
  std::printf("\n");
  // The first few are the interesting ones; past that the answer is the same on
  // every line and the list is just noise.
  std::printf("why (first 6)          : ");
  for (std::size_t i = 0; i < emptyWhy.size() && i < 6; ++i) {
    std::printf("%s  ", emptyWhy[i].c_str());
  }
  std::printf("\n");
  std::printf("cumulative XP to L%d   : %.0f\n", emptyLevels.empty() ? 0 : emptyLevels.front(),
              [&] {
                float total = 0.0F;
                for (int l = 1; l <= (emptyLevels.empty() ? 0 : emptyLevels.front()); ++l) {
                  const float n = static_cast<float>(l - 1);
                  total += 10.0F + 6.5F * n + 1.55F * n * (n + 1.0F);
                }
                return static_cast<double>(total);
              }());
  std::printf("most repeated card     : ");
  int worst = 0;
  std::string worstId;
  for (const auto& [id, n] : seen) {
    if (n > worst) { worst = n; worstId = id; }
  }
  std::printf("%s x%d\n", worstId.c_str(), worst);
  REQUIRE(g.testUsedItemSlots() <= g.testItemSlotCap());
}
