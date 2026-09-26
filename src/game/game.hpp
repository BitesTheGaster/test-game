#pragma once

#include "core/render/batcher.hpp"
#include "core/sim/fixed_timestep.hpp"
#include "core/sim/spatial_hash.hpp"
#include "game/components.hpp"
#include "game/content.hpp"
#include "game/profile.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// The game's title, and the single line the main menu puts under it.
inline constexpr const char* kGameName = "TEST GAME";
inline constexpr const char* kGameSubtitle = "";

// Word-wraps `str` into lines of at most `maxChars` characters (word-based,
// single words longer than the limit overflow their own line). Shared between
// the level-up card renderer and unit tests.
std::vector<std::string> wrapWords(std::string_view str, std::size_t maxChars);

// Word-wraps `str` for a proportional box instead of a character count: the
// same greedy algorithm, but the limit is `maxWidth` screen pixels at `scale`
// and every CONTINUATION line is indented by `indent` pixels and gets `indent`
// fewer pixels to work with.
//
// The hanging indent is the point. Without one, a wrapped description is a
// rectangle of text with no visual left edge for a continuation line, so the
// eye has to re-read the row above to find where the sentence restarted; with
// one, "where does this line begin" is answered by the indent itself.
//
// Pure layout, no Batcher: the arithmetic that decides what fits is the part
// worth testing, and a test cannot build a GL context.
//
// `contScale` is the size the CONTINUATION lines will be drawn at; pass 0 when
// they are the same size as the first. It is a separate argument because the
// card sets its wrapped lines larger, and a wrap measured at the first line's
// size would overflow the card by exactly the size of the bump.
[[nodiscard]] std::vector<std::string> wrapToWidth(std::string_view str,
                                                    float maxWidth, float scale,
                                                    float indent,
                                                    float contScale = 0.0F);

// How the level-up card sets its body text, derived from the card width.
//
// One size, one pitch, one left edge. The card used to set its wrapped lines
// larger than the first and indent them inboard of it, which read as a mistake
// rather than as a paragraph: the first line looked correct and the rest looked
// like a second, cruder block that had been shoved sideways. So the fields are
// still separate -- the wrap has to be able to measure continuation lines on
// their own terms -- but the shipped values are uniform, and a card is one
// column of body copy.
//
// Derived from the card width only for the TEXT BOX; the scale is a constant,
// because the card being narrower already shortens the measure without any help.
// See the comment on kCardDescScale for why the width-derivation was removed.
struct CardTextLayout {
  float width = 0.0F;   // usable text box width, padding already removed
  float scale = 1.6F;   // first line
  float contScale = 1.6F; // every line after the first
  float lineH = 21.0F;  // first line's vertical advance
  float contLineH = 21.0F; // every later line's advance
  float indent = 0.0F;  // left inset for continuation lines
};

[[nodiscard]] CardTextLayout cardTextLayout(float cardW);

// Vertical space a body of `lines` lines takes. 1 line is a single body line; n
// lines is the first line plus (n-1) more at the continuation pitch.
[[nodiscard]] float cardTextHeight(int lines, const CardTextLayout& lay);

// How many lines of `lay` fit in `avail` pixels, which is what the renderer
// clamps the wrap to. Exact inverse of cardTextHeight, not a division: the
// first line's advance and the continuation pitch are separate fields.
[[nodiscard]] int cardTextLinesThatFit(float avail, const CardTextLayout& lay);

// The level-up card's TITLE, as up to two lines.
//
// A single line cannot hold every shipped name: "Total Internal Reflection" is
// 25 characters, which is 345px at 2.3 -- a 260px cell in a 4-card row and a
// 196px cell in a 5-card one. Drawing it at a fixed 2.3 sliced it under the next
// card's panel with no ellipsis; shrinking it to fit drove the scale below the
// body's own, which stops the title reading as a title. So: keep 2.3 when the
// name fits on one line, and otherwise wrap to two lines at 2.0, which still
// holds 16 characters in the narrowest card the row can produce.
//
// Exposed as pure arithmetic so the "does every shipped name fit" rule is
// testable without a GL context, and so the renderer and the test cannot
// disagree about which names are too long.
struct CardNameLayout {
  float scale = 2.3F;   // 2.3 for a one-liner, 2.0 for a wrapped title
  float lineH = 24.0F;  // vertical advance of the title block
  // The title as it will actually be drawn, at most two lines. Carrying the
  // lines rather than only their count means the renderer and the test read the
  // same wrap instead of each calling wrapToWidth with its own arguments.
  std::vector<std::string> lines{std::string{}};
  // True when the wrap could not hold the whole name and the last line was cut
  // short with an ellipsis. No shipped name does this at any reachable card
  // width; a test says so, and the card tints the tail when it happens so the
  // loss is visible rather than silent.
  bool elided = false;
  // How far the description has to start below the card's top edge, which grows
  // by one line for a wrapped title.
  [[nodiscard]] float blockH() const { return lineH * static_cast<float>(lines.size()); }
  [[nodiscard]] bool wrapped() const { return lines.size() > 1; }
};
[[nodiscard]] CardNameLayout cardNameLayout(std::string_view name, float maxWidth);

// Normalized input gathered by main() from SDL each frame.
struct FrameInput {
  float moveX = 0.0F; // -1..1
  float moveY = 0.0F; // -1..1, y-up
  bool choose1 = false;
  bool choose2 = false;
  bool choose3 = false;
  bool choose4 = false;
  bool choose5 = false; // picks a 5th card (from +1 choice items)
  bool restart = false;
  bool togglePause = false;
  bool testModeToggle = false; // T: open/close the weapon test mode
  bool testShop = false;       // E: open/close the sandbox item picker
  bool testInvuln = false;     // I: toggle sandbox immortality
  bool testKill = false;       // X: kill the player on demand
  bool testTime = false;       // F: cycle the sandbox difficulty timer
  // Note: `restart` (R) is repurposed INSIDE the sandbox as "max every item".
  bool heal = false;           // H: guaranteed 50% max-HP heal (cooldown-gated)
  bool bestiary = false;       // B: toggle the bestiary while paused
  // --- Active abilities (always available, cooldown-gated) --------------------
  // Three buttons, no menu, no cards required: the build decides how they are
  // tuned, the player decides when to spend them. See the "Active abilities"
  // section in docs/mechanics.md.
  bool abilityBlink = false; // J: Phase Dash — teleport along your movement
  bool abilityBurst = false; // K: Overload — knock everything around you away
  bool abilitySlow = false;  // L: Stasis — the world slows down, you do not
  // --- Main menu --------------------------------------------------------------
  bool menuUp = false;    // Up / W
  bool menuDown = false;  // Down / S
  bool menuLeft = false;  // Left / A
  bool menuRight = false; // Right / D
  bool menuConfirm = false; // Enter / Space: activate the highlighted row
  // F1: open/close the in-game manual. Legal from the main menu, from a run and
  // from the pause screen, because a manual you can only read at the start is a
  // manual nobody reads.
  bool manualToggle = false;
  // Q: abandon the run and go back to the main menu. Only honoured on the pause
  // screen, and only on the SECOND press inside the arming window — quitting a
  // live run is the one irreversible thing the player can do by accident, so it
  // gets the same two-step treatment as wiping progress.
  bool quitRun = false;
};

// Attack-speed formula: the final delay between shots is
//
//     delay = baseCooldown / (1 + totalFireRateBonus)
//
// where the bonus is the additive sum of every fire-rate source (global
// upgrades + per-weapon cards). Being additive in the denominator means
// stacking can never drive the delay to zero — it approaches 0 asymptotically
// (+100% halves the delay, +300% quarters it, ...).
inline float attackCooldown(float baseCooldown, float fireRateBonus) {
  return baseCooldown / std::max(0.05F, 1.0F + fireRateBonus);
}

// Mutable per-run player modifiers; upgraded through UpgradeDef::effect.
struct PlayerStats {
  float damageMul = 1.0F;
  // Additive fire-rate bonus. The final per-shot delay is
  // `base / (1 + fireRateBonus)` (see attackCooldown) — additive so stacking
  // can never reach infinite attack speed.
  float fireRateBonus = 0.0F;
  float speedMul = 1.0F;
  float pickupMul = 1.0F;
  float speed = 5.2F;
  float maxHp = 100.0F;
  float regen = 0.0F;
  int projAdd = 0;
  int pierceAdd = 0;

  // Defense: one curve yields both a flat and a percent reduction.
  float defense = 0.0F;
  // Lifesteal: chance-based, but it only rolls on a KILL (not on every hit).
  // L% = L% chance per slain enemy to heal `lifestealHeal` HP; at L >= 100 the
  // first point is guaranteed. Tying it to kills (instead of per-damage) is
  // what keeps a many-hit weapon (e.g. the Halo) from out-healing the fight:
  // the number of procs is bounded by kills, not by hit count.
  float lifesteal = 0.0F;
  int lifestealHeal = 1; // HP per proc (Vampiric Heart unique raises to 2)
  // Armor pierce: subtracted from an enemy's defense before mitigation, so
  // this is the "пробитие брони" stat — it is what lets a build cut through
  // the flat+percent defense curve that late/elite enemies grow into.
  float armorPierce = 0.0F;
  // Shield: regenerating damage buffer (see rules in game.hpp docs).
  float shieldMax = 0.0F;
  // Shield refill SPEED and how long the "out of combat" wait before it starts.
  // Both used to be bare constants, which made a shield build a one-card trick:
  // the pool grew but the rate and the downtime did not, so a burst build
  // simply re-took the same chunk of HP five seconds later. Cards can now scale
  // the rate and shorten the delay (kShieldRegenRate / kShieldRegenDelay are the
  // 1.0x / 0.0s baselines).
  float shieldRegenMul = 1.0F;
  float shieldRegenDelay = 0.0F;
  // XP gain multiplier (Scholar-style items).
  float xpMul = 1.0F;

  // Unique-item effects (each is a distinct mechanic):
  float spreadMul = 1.0F; // widens weapon volleys (fan item)
  // Inaccuracy from the Spreadshot unique: each projectile is offset by up to
  // this many radians (0.5236 = +/- 30 degrees).
  float aimJitter = 0.0F;
  int extraChoice = 0;    // +N level-up cards
  int rerollCharges = 0;  // +N extra rerolls (the base budget is already 1)
  // Extra weapon slots. The arsenal starts at kBaseWeapons (4) and the single
  // "Arsenal Core" card adds one per stack (3 stacks), so a fully stacked build
  // holds exactly kMaxWeapons (7).
  int weaponSlots = 0;
  float thornsDmg = 0.0F; // AoE burst around player on hit
  int adrenaline = 0;     // speed burst when HP is low
  int blackHole = 0;      // periodic enemy pull
  int chain = 0;          // every 3rd projectile hit chains lightning
  int bloodPrice = 0;     // every 20 kills, burst around player
  int iceBlood = 0;       // enemies that hit you get slowed
  int lastStand = 0;      // 1s of iframes when hit below 20% HP (cooldown)
  // Repulsion Field unique: enemies that strike the player are shoved away.
  float knockbackRetaliate = 0.0F;
  // Impact cards: multiplies every knockback the player deals (default 1).
  float knockbackMul = 1.0F;
  // --- Momentum ----------------------------------------------------------------
  // The kill chain is the game's dynamic layer: it is NOT a flat stat you buy
  // and forget, it is a meter that fills while you keep killing and collapses
  // the moment you stop or get hit. The values below are what one card can add
  // to it; see Game::updateMomentum for how the meter turns into multipliers.
  float momentumDamage = 1.5F;  // % damage per stack (default 1.5 => +30% at cap)
  float momentumRate = 0.5F;    // % fire rate per stack
  float momentumSpeed = 0.0F;   // % move speed per stack (needs a card to matter)
  int momentumMax = 20;         // chain length that counts
  float momentumWindow = 3.0F;  // seconds of not killing before the chain drops
  float momentumGain = 1.0F;    // stacks per kill
  // --- On-hit marks ------------------------------------------------------------
  // Four ways to make a body take longer to reach you or shorter to live, and the
  // milestone card that buys one of them closes the other three for the rest of
  // the run. That is the point: they are four answers to the same question, not
  // four numbers to collect. Every one of them is applied inside
  // applyEnemyDamage, which is the single funnel every point of player damage
  // goes through -- so a mark works from every weapon, every projectile and every
  // area tick without any of them knowing it exists.
  float markChillTime = 0.0F;  // Frostbind: seconds of chill per hit
  float markBurnDps = 0.0F;    // Emberbrand: burn damage per second
  float markVuln = 0.0F;       // Hex: +damage taken added per hit
  float markVulnMax = 0.0F;    // and the ceiling on it
  float markDefStrip = 0.0F;   // Armour Split: defence removed per hit
  // One more weapon improved per chest. The base is one for an elite, three for a
  // champion and five for an overlord, so this is the only way to make an elite's
  // box worth as much as a champion's -- and it stacks, because the whole point
  // of the card is that the count is the reward.
  int chestBonus = 0;
  // --- Active abilities (J / K / L) -------------------------------------------
  // Three buttons that exist in every run from the first second — no unlock, no
  // card, no level. They are what the player reaches for when a build is done
  // growing and the fight has to be solved with positioning instead of numbers,
  // and they are the answer to "the run got boring": something to spend.
  // Cooldowns live in the Game (see abilityCooldownRemaining); these fields only
  // hold what the cards change.
  float abilityCdMul = 1.0F;   // multiplies all three cooldowns (cards lower it)
  float blinkDist = 3.2F;      // Phase Dash: teleport distance
  float blinkIframes = 0.35F;  // seconds of invulnerability the dash grants
  float burstRadius = 3.0F;    // Overload: blast radius
  float burstDamage = 45.0F;   // Overload: damage before damageMul
  float burstKnockback = 8.0F; // Overload: shove strength
  float stasisDuration = 2.5F; // Stasis: window length
  float stasisSlow = 0.35F;    // Stasis: enemy time multiplier while it is up
  int abilityEcho = 0;         // 1 = every ability also fires a 40% Overload
};

struct UpgradeEffectResult {
  bool valid = false; // false => unknown effect id
  float heal = 0.0F;
  float shield = 0.0F; // instant shield granted on pick
};

// Applies one upgrade effect to stats. Shared between Game and unit tests.
UpgradeEffectResult applyUpgrade(PlayerStats& stats, std::string_view effect, float value);

// Effects whose `value` is a COUNT, so the applier feeds it through
// `static_cast<int>`. A fractional value in the data is silently truncated, and
// anything below 1.0 truncates to nothing: a card that says "+0.5 projectiles"
// is a card that prints a promise and does nothing.
//
// Public so the content test can ask the applier which ids are counts instead of
// keeping its own list -- which is exactly how a card ships a number the game
// quietly throws away.
[[nodiscard]] bool effectIsWholeNumberOnly(std::string_view effect);

// Defense formula: flat part (1 point per 5 defense) plus a percent part that
// approaches 50% as defense grows. Never makes damage negative.
float mitigateDamage(float raw, float defense);

// How far above its first target a chain bolt is drawn coming down from. A
// lightning strike that starts at the victim's own centre is just a dot; the
// vertical drop is what makes it read as a strike.
//
// The SIGN of this number is a load-bearing fact about the world -- world +y is
// UP, so "above" is "+" -- and the render got it wrong once already: the charge
// gathered in the sky and the bolt rose out of the floor to meet it. It is
// namespace-scope rather than a class constant so that chainChargeY() below, the
// one place the direction is written down, can be a free function a test calls.
constexpr float kChainSkyDrop = 4.2F;

// Where the sky end of a chain bolt's drop is, and how far down it has got.
//
// The two ends of one lightning effect, in one function, because they used to be
// two independent sign decisions in the renderer and they disagreed: the
// telegraph gathered at `y + kChainSkyDrop` while the drop ran from
// `y - kChainSkyDrop`. The effect therefore pointed both ways at once, and
// "the chain lightning animation is still broken" kept meaning exactly that --
// not a rough shape, not a bad easing, but a bolt that fell upward.
//
// `born` is the strike's 0..1 progress: at 0 the drop has not started, at 1 it has
// reached the target. `reachY` therefore moves DOWN from `skyY` as `born` rises,
// which is the property a test can assert and a comment cannot.
struct ChainDropSpan {
  float skyY = 0.0F;   // where the bolt comes from
  float reachY = 0.0F; // how far down it has got
};
[[nodiscard]] ChainDropSpan chainDropSpan(float targetY, float born);

// Where the strike's charge gathers. Must be the same end the drop falls from,
// or the effect is aimed one way and delivered another.
[[nodiscard]] constexpr float chainChargeY(float targetY) {
  return targetY + kChainSkyDrop;
}

// XP needed to advance from `level` to `level + 1`.
float xpForLevel(int level);

// AoE damage falloff: the more enemies a single blast catches, the less each
// one takes. 1 target = 100%, 2 = 90%, 3 = 81%, ... (0.9^(n-1)). Pierce reduces
// the falloff: at 20+ pierce there is no falloff at all. Clamped so a huge
// crowd still deals a floor of damage instead of underflowing.
float aoeFalloff(int enemiesHit, int pierce = 0);

// Enemy scaling curves, all pure so they can be unit-tested directly.
// Defense uses the same flat+percent curve as the player (mitigateDamage) and
// grows after a short grace period; higher tiers are tougher.
float enemyDefense(float simTime, int tier);
// Lifesteal resistance: 0.5 => the player's heal chance is halved.
float enemyLifestealResistance(float simTime, int tier, bool resistant);
// Knockback resistance: scales incoming push, grows with time.
float enemyKnockbackResistance(float simTime, int tier, bool resistant);
// How many traits a given tier rolls: elite = exactly 1, champions several,
// overlords even more. Grows a little with run time.
int traitsForTier(int tier, float simTime);

enum class RunState {
  Playing,
  LevelUp,
  Paused,
  GameOver,
};

// One level-up card: either an upgrade from content_ or a new weapon.
struct Choice {
  enum class Kind : std::uint8_t {
    Upgrade,
    Weapon,
    // Fallback card shown when every real upgrade is maxed or inapplicable.
    // Without it a level-up screen can be left with no usable card and the run
    // is stuck forever.
    Skip,
  };
  Kind kind = Kind::Upgrade;
  int index = -1; // upgrade index or weapon index
};

class Game {
public:
  // `content` is held BY REFERENCE and must outlive the Game. loadContent
  // returns a Content by value, so `Game g{loadContent(dir), seed};` compiles
  // and then reads freed memory on the first frame -- keep the Content in a
  // named local.
  explicit Game(const Content& content, std::uint32_t seed = 1337);

  void reset();

  // Frame-level entry: input handling, fixed-step sim, camera.
  void advance(float frameDt, const FrameInput& input);

  void render(core::render::Batcher& batcher, float alpha);

  [[nodiscard]] RunState state() const { return state_; }
  [[nodiscard]] float simTime() const { return simTime_; }
  [[nodiscard]] int level() const { return level_; }
  [[nodiscard]] int kills() const { return kills_; }
  [[nodiscard]] float playerHp() const;
  [[nodiscard]] float playerMaxHp() const;
  [[nodiscard]] float shield() const { return shield_; }
  [[nodiscard]] float shieldMax() const { return stats_.shieldMax; }
  [[nodiscard]] float xp() const { return xp_; }
  [[nodiscard]] float xpNext() const { return xpNext_; }
  [[nodiscard]] std::size_t enemyCount() const;
  [[nodiscard]] const PlayerStats& stats() const { return stats_; }
  [[nodiscard]] PlayerStats& stats() { return stats_; }
  [[nodiscard]] const std::vector<Choice>& upgradeChoices() const { return choices_; }
  [[nodiscard]] int upgradeStacks(std::size_t upgradeIndex) const;
  [[nodiscard]] int rerollsUsed() const { return rerollsUsed_; }
  [[nodiscard]] bool milestoneOffer() const { return milestoneOffer_; }
  // True while the weapon test mode is open (T). Exposed for tests.
  [[nodiscard]] bool testMode() const { return testMode_; }
  // Sandbox switches.
  [[nodiscard]] bool testInvulnerable() const { return testMode_ && testInvuln_; }
  [[nodiscard]] int testTimeScale() const { return testMode_ ? testTimeScale_ : 1; }
  [[nodiscard]] bool testShopOpen() const { return testMode_ && testShopOpen_; }
  [[nodiscard]] int testShopCursor() const { return testShopCursor_; }
  // True while the opening 3-weapon pick is being offered.
  [[nodiscard]] bool choosingStarter() const { return choosingStarter_; }
  // True while the bestiary overlay is open (paused).
  [[nodiscard]] bool bestiaryOpen() const { return bestiaryOpen_; }

  // --- Profile (skins / outline unlocks) -------------------------------------
  // The profile is OWNED BY THE CALLER (main() loads it from disk and keeps it
  // alive for the process lifetime). Game only mutates it in place — it never
  // touches the filesystem — so tests can hand in a plain local object and
  // assert on the results.
  // Attaching a profile repaints the player immediately: the constructor's
  // reset() already ran with no profile, so a saved skin would otherwise not
  // show up until the player changed it in the menu.
  void setProfile(Profile* profile) {
    profile_ = profile;
    syncedUnlocks_ = 0;
    applyProfileToPlayer();
  }
  [[nodiscard]] Profile* profile() const { return profile_; }
  // Test helper: the player's rendered body colour, so tests can assert that
  // the profile's skin actually reaches the Sprite.
  [[nodiscard]] core::render::Color testPlayerColor() const;
  // Test helper: the resolved outline colour (alpha 0 when no outline is worn).
  [[nodiscard]] core::render::Color testOutlineColor() const { return outlineColor_; }
  // Pushes newly earned tier unlocks (from kills) into the profile. Called
  // automatically on every kill; safe to call again.
  void syncProfileUnlocks();
  // True when the profile changed since the last consumeProfileDirty(), so
  // main() knows it should write it back to disk. Returns and clears the flag.
  bool consumeProfileDirty();

  // --- Main menu --------------------------------------------------------------
  // The menu is a Game-level overlay so it can reuse the same renderer. Tests
  // never open it, so `advance()` behaves exactly as before by default.
  void openMenu() { menuOpen_ = true; }
  void closeMenu() { menuOpen_ = false; }
  [[nodiscard]] bool menuOpen() const { return menuOpen_; }
  // Highlighted row, exposed for tests and for the renderer.
  [[nodiscard]] int menuSelection() const { return menuSelection_; }
  // Set when the player picks QUIT in the menu; main() watches this to break
  // its loop. Cleared by consumeQuitRequest().
  [[nodiscard]] bool quitRequested() const { return quitRequested_; }
  bool consumeQuitRequest() {
    const bool q = quitRequested_;
    quitRequested_ = false;
    return q;
  }

  // --- In-game manual (F1) ---------------------------------------------------
  // The same documentation docs/ ships, trimmed to what the bitmap font can
  // draw and split into pages. It is a Game-level overlay so it can be opened
  // from the main menu, from a live run and from the pause screen.
  void openManual();
  void closeManual();
  [[nodiscard]] bool manualOpen() const { return manualOpen_; }
  [[nodiscard]] std::size_t manualPageIndex() const { return manualPage_; }
  [[nodiscard]] std::size_t manualPageCount() const { return content_.manual.size(); }
  void nextManualPage();
  void prevManualPage();
  // Page id shown right now, or "" when the build ships no manual.
  [[nodiscard]] std::string manualPageId() const;

  // Manual page layout. Public so the "does every shipped page fit one screen"
  // test reads the same numbers the renderer draws with, instead of duplicating
  // them and silently going stale the next time the body is resized.
  static constexpr float kManualBodyX = 250.0F;   // body left edge, clear of the rail
  static constexpr float kManualBodyY = 74.0F;    // first body line
  static constexpr float kManualBodyScale = 1.6F; // readable body size
  static constexpr float kManualLineH = 15.0F;    // body line pitch
  static constexpr float kManualBottomPad = 56.0F; // kept for the hint bar
  static constexpr float kManualIndent = 18.0F;    // bullet / sub-line indent
  // The smallest window the HUD is laid out for; the fit test measures against
  // it because every other pixel literal in the renderer assumes it.
  //
  // kMinScreenHeight is a floor, not a target: the window is resizable and can be
  // made shorter than this, which is why the level-up row clamps its own top
  // against the real height rather than assuming a percentage. What it needs is
  // the height at which the clamp STOPS being the thing doing the work, so a
  // test can assert the row fits without it.
  static constexpr float kRefScreenHeight = 720.0F;
  static constexpr float kRefScreenWidth = 1280.0F;

  // The level-up card row, as pure arithmetic, so the "it all fits on screen"
  // rule is testable without a GL context. Mirrors the block in render() exactly;
  // the renderer calls it, so the two cannot drift.
  struct LevelUpRow {
    float cardW = 0.0F;   // one card's width
    float cardH = 0.0F;   // the row's shared height
    float top = 0.0F;     // the row's top edge
    float bodyTop = 0.0F; // where the description starts inside a card
    float hintY = 0.0F;   // where the reroll hint sits
    int bodyLines = 0;    // description lines the card will actually show
    int nameLines = 1;    // title lines the tallest card needs
  };
  [[nodiscard]] static LevelUpRow levelUpRowLayout(float px, float py, std::size_t n,
                                                   std::size_t tallestDescLines,
                                                   std::size_t tallestNameLines);

  // --- Progress reset (main menu) --------------------------------------------
  // Wipes the attached profile back to first-launch defaults: default skin, no
  // outline, no earned unlocks, and repaints the player. Marks the profile
  // dirty so main() persists the wipe.
  //
  // Two-step on purpose. `beginProgressReset()` arms it and returns true; the
  // actual wipe only happens on `confirmProgressReset()`, which must be a
  // SEPARATE call. A destructive action that fires on the same Enter that
  // selected it is one stray keypress away from wiping hours of unlocks.
  void beginProgressReset();
  [[nodiscard]] bool progressResetArmed() const { return resetArmed_; }
  // Wipes and disarms. Returns false when nothing was armed.
  bool confirmProgressReset();
  void cancelProgressReset() { resetArmed_ = false; }

  // --- Defense clock ----------------------------------------------------------
  // A shield is a pool AND a clock. These are the 1.0x rate and the 0.0s
  // baseline; the shield_add cards grow the pool, shield_regen and shield_delay
  // change these two, and the character sheet prints the result.
  static constexpr float kShieldRegenRate = 10.0F;  // HP/s once out of combat
  static constexpr float kShieldRegenDelay = 4.0F;   // seconds without damage
  // The delay is floored so no build can make the pool permanent.
  static constexpr float kShieldRegenDelayFloor = 0.5F;

  // --- Read-only build readouts ----------------------------------------------
  // The numbers a card is supposed to move, exposed so tests (and the
  // character sheet) can read the same value the simulation does rather than
  // re-deriving it from the card's effect id.
  // % fire rate the chain grants per live stack (see momentumFireRate() for the
  // value the simulation actually applies, which is stacks * this).
  [[nodiscard]] float momentumRatePerStack() const { return stats_.momentumRate; }
  [[nodiscard]] float blinkDistance() const { return stats_.blinkDist; }
  [[nodiscard]] float burstRadius() const { return stats_.burstRadius; }
  [[nodiscard]] float burstDamage() const { return stats_.burstDamage; }
  [[nodiscard]] float stasisDurationMax() const { return stats_.stasisDuration; }
  [[nodiscard]] float stasisSlowFactor() const { return stats_.stasisSlow; }
  // Shield refill HP/s and the seconds it waits after a hit before starting.
  // The delay is floored at kShieldRegenDelayFloor, so no build can make the
  // pool permanent. Exposed (not inlined into updateShield) so the character
  // sheet and the tests read the same number the simulation does.
  [[nodiscard]] float shieldRegenRate() const {
    return kShieldRegenRate * std::max(0.1F, stats_.shieldRegenMul);
  }
  [[nodiscard]] float shieldRegenDelay() const;
  // Base arsenal size + any "+1 weapon slot" cards. Weapons cannot be added
  // past this, and the level-up offer stops appearing once it is full. Clamped
  // to kMaxWeapons so a hand-edited content file cannot overflow the array.
  [[nodiscard]] int weaponCap() const {
    return std::min(kMaxWeapons, kBaseWeapons + std::max(0, stats_.weaponSlots));
  }
  // The arsenal starts at 4 weapons and the ONE slot card ("Arsenal Core", 3
  // stacks) takes a full build to 7. It used to be two cards -- a 3-stack and a
  // one-shot -- for a cap of 8, which meant two level-up screens describing the
  // same decision and one of them miscounting the total. Public so a test can
  // assert the shipped cap against the storage array rather than hard-coding it.
  static constexpr int kBaseWeapons = 4;
  static constexpr int kMaxWeapons = 7;
  // Hard cap on "+1 weapon slot" stacks, whatever the content says. Content ships
  // exactly this many today; it can never ship more.
  static constexpr int kMaxSlotCards = kMaxWeapons - kBaseWeapons;

  // How many cards a single milestone screen may hold. A milestone used to be a
  // flat two no matter what the content had, which is why a four-way question had
  // to be split across two screens and half of it was never asked. Now the
  // screen is as wide as the group is, capped here so a runaway group cannot
  // paint a wall of cards over the game.
  static constexpr std::size_t kMaxMilestoneSlots = 4;

  // How many cards the chest reveal panel can name at once. An overlord's box is
  // the largest thing on the floor (five), so this is the hard ceiling on the
  // panel rather than a chosen number.
  static constexpr std::size_t kChestRevealMax = 5;

  // --- Fast-enemy pacing ----------------------------------------------------
  //
  // Three numbers, one intent: a quick enemy should be something the run grows
  // into, not something it starts with.
  //
  // kFastEnemyMinTime holds every `fast`-flagged archetype out of the spawn
  // pool until this point even if its own unlock_at has passed. The opening
  // four minutes are a warm-up you can learn the game in; the first genuinely
  // quick enemy is then an escalation you can watch arrive.
  static constexpr float kFastEnemyMinTime = 240.0F; // 4:00
  // How long a per-type speed ramp (EnemyDef::speedRampMax) takes to saturate.
  // A fast type is authored at its opening speed and reaches
  // speed * (1 + speedRampMax) here, so the ramp exactly undoes the nerf by
  // the time the run has taught the player to handle it.
  static constexpr float kSpeedRampFullTime = 600.0F; // 10:00
  // How long the elite "Fast" trait takes to reach its full bonus. A flat 1.7x
  // from second one makes a 4-minute horde undodgeable, so the trait ramps:
  // brisk early, genuinely fast late.
  static constexpr float kFastTraitFullTime = 300.0F; // 5:00
  static constexpr float kFastTraitMaxMul = 1.7F;

  // --- Chain / bounce cascade bounds -----------------------------------------
  //
  // Both of these spawn extra entities from inside an update system, so both
  // need a hard stop: a fork that can fork, or a fragment that can split, is
  // exponential growth in entity count and will hang the game.
  static constexpr int kMaxBounceSplitDepth = 3;
  // Hard ceiling on a fragment's life. The cascade decays by depth and by
  // bounce budget already; this is the third bound, and without it the ROOT
  // orb's long life would keep the whole tree airborne and the cascade would
  // read as slow multiplication rather than a burst that dies down.
  static constexpr float kMaxSplitLife = 2.5F;
  // How long a chain bolt hangs after its last jump, so a traversal that is over
  // in a fraction of a second is at least visible before it disappears.
  //
  // 0.25 was too short to read: the whole bolt was gone inside a third of a
  // second, before the player's eye had finished travelling to the enemy it had
  // just hit. 0.45s is long enough to follow a five-hop arc without the effect
  // feeling like it is sticking around.
  static constexpr float kChainLinger = 0.45F;
  // How long the bolt spends converging on its first target before anything
  // damages. This is the whole "smooth" of the strike: a transparent ring on the
  // target shrinks through this window and the bolt only lands when it closes.
  static constexpr float kChainTelegraph = 0.22F;
  // How long the sky drop takes to draw itself down, once the ring has closed.
  //
  // 0.09 was shorter than the gap to the first hop (0.05s), so the drop was
  // replaced by an arc at 55% of its own length: the player saw a stub of
  // lightning that stopped in mid-air. It now outlasts the hop delay with room
  // to spare, and the hit is paid on the frame the drop REACHES the target
  // rather than the frame it starts.
  static constexpr float kChainStrike = 0.14F;
  // How long the landed drop sits there, fully drawn, before the bolt arcs on.
  // Without it the impact and the first arc are the same frame, and the strike
  // reads as an instantaneous swap from "vertical line" to "bent line".
  static constexpr float kChainLandHold = 0.07F;
  // Gap between the arcs of a multi-arc wave shot, in seconds.
  static constexpr float kWaveBurstGap = 0.08F;
  // How fast a ricochet aims itself, in radians per second. A target inside
  // bounceRange is a real ricochet and snaps the heading (this is the full turn);
  // anything further away only nudges at kBounceReturnTurn, which keeps the arc
  // reading as a puck searching for the next body rather than as a homing
  // missile. A value of zero on the far case is the bug this replaced: the orb
  // kept whatever heading it had, left the arena, and the player watched an empty
  // screen for the rest of a multi-second life.
  static constexpr float kBounceTurn = 6.2831853F;
  static constexpr float kBounceReturnTurn = 2.5F;

  // Test/debug hooks.
  void grantXp(float amount);
  // Test helper: how many cards the level-up screen is currently offering. The
  // renderer divides by this, so "never zero" is a load-bearing rule rather than
  // a cosmetic one.
  [[nodiscard]] std::size_t testChoiceCount() const { return choices_.size(); }
  // Test helper: add weapon by index (bypasses normal level-up flow)
  void testAddWeapon(int defIndex) {
    starterChoicePending_ = false; // tests set up weapons directly
    addWeapon(defIndex);
  }
  // Test helper: how many weapons are armed. Several tests want an arsenal with
  // more than one weapon -- a chest's 50/50 weapon-or-item roll only has two
  // sides if the weapon side is not empty -- and each was otherwise re-deriving
  // that by snapshotting slot 0 and hoping.
  [[nodiscard]] int testWeaponCount() const { return weaponCount_; }
  // Test helper: a comparable copy of one weapon slot's whole CONFIGURABLE stat
  // block, so a test can assert that taking a card actually MOVED something
  // instead of silently doing nothing. Comparing two snapshots with == covers
  // every field at once, so a new WeaponSlot field needs no test edit. Pure
  // runtime state (timers, the live bounce handle, the nova radius) is left out
  // on purpose: it ticks on its own, so including it would make every
  // comparison fail for reasons that have nothing to do with the card.
  struct WeaponSnapshot {
    int def = 0;
    int attackType = 0;
    float cooldown = 0;
    float damage = 0;
    int projectiles = 0;
    float speed = 0;
    float life = 0;
    int pierce = 0;
    float spread = 0;
    float coneAngle = 0;
    float coneRange = 0;
    float coneTickRate = 0;
    float coneBite = 0;
    float coneBiteMax = 0;
    float coneEmberAt = 0;
    float coneEmberRadius = 0;
    float coneEmberDuration = 0;
    float orbitRadius = 0;
    float orbitSpeed = 0;
    int orbitCount = 0;
    bool orbitWindow = false;
    float waveSpread = 0;
    float bombArcHeight = 0;
    float bombExplodeRadius = 0;
    float bombKnockback = 0;
    float bombFuse = 0;
    float bombAhead = 0;
    bool bombOnTarget = false;
    float reaimRange = 0;
    float reaimTurn = 0;
    float boomerangRange = 0;
    float boomerangReturnSpeed = 0;
    int bounceCount = 0;
    float bounceRange = 0;
    float bounceDamageMul = 0;
    bool bounceInfinite = 0;
    float beamRange = 0;
    float beamWidth = 0;
    float beamDuration = 0;
    // Chill this weapon applies on hit, so the "did this card do anything" test
    // covers it like every other identity parameter.
    float chillMul = 0;
    float chillTime = 0;
    float auraRadius = 0;
    float auraDps = 0;
    float auraTick = 0;
    float auraChillMul = 0;
    float auraChillTime = 0;
    float haloKnockback = 0;
    float haloInner = 0;
    float sweepAngle = 0;
    float sweepRadius = 0;
    float sweepKnockback = 0;
    bool sweepHook = false;
    float zoneRadius = 0;
    float zoneDuration = 0;
    float zoneDps = 0;
    int zoneMaxPools = 0;
    bool zoneFromAbove = false;
    float chainJumpRange = 0;
    int chainMaxJumps = 0;
    float chainDamageMul = 0;
    int chainShatter = 0;
    int bounceSplits = 0;
    float waveSpeed = 0;
    float waveRange = 0;
    float waveWidth = 0;
    float waveKnockback = 0;
    float waveDamageMul = 0;
    int waveCount = 0;
    float waveArcStep = 0;
    float waveHookPull = 0;
    float novaMaxRadius = 0;
    float novaExpandSpeed = 0;
    float novaDamagePerTick = 0;
    float novaTickRate = 0;
    bool novaContract = false;
    float novaPull = 0;
    float novaBurstDamage = 0;
    float novaEcho = 0;
    float area = 0;
    float strength = 0;
    bool homing = 0;
    int bounces = 0;
    float cdBonus = 0;
    float sweepLead = 0;
    float uniqueHeal = 0;
    int beamSplit = 0;
    float vortexRadius = 0;
    float vortexReach = 0;
    float vortexPull = 0;
    float vortexOrbit = 0;
    float vortexOrbitSpeed = 0;
    float vortexTickRate = 0;
    float vortexCollapseAt = 0;
    float vortexCrowd = 0;
    float vortexBurstDamage = 0;
    float vortexBurstRadius = 0;
    float prismRange = 0;
    float prismWidth = 0;
    int prismMaxTargets = 0;
    float prismRicochet = 0;
    float lureRadius = 0;
    float lureReach = 0;
    float lurePull = 0;
    float lureDps = 0;
    float lureDuration = 0;
    float lureTickRate = 0;
    int lureMaxBeacons = 0;
    [[nodiscard]] bool operator==(const WeaponSnapshot& other) const;
    [[nodiscard]] bool operator!=(const WeaponSnapshot& other) const {
      return !(*this == other);
    }
  };
  [[nodiscard]] WeaponSnapshot testWeaponSnapshot(int slotIndex) const;
  // Test helper: drop owned weapons + their persistent entities (orbit blades).
  void testClearWeapons();
  // Test helper: place a stationary, high-HP enemy at a world position.
  void testSpawnEnemyAt(float x, float y);
  // Test helper: the same body, but carrying an elite tier, so a test can check
  // that a box drops from the right kind of corpse. Tier 0 leaves the component
  // absent, exactly as a normal spawn does.
  void testSpawnEliteAt(float x, float y, int tier);
  // Test helper: kill exactly the body the last test spawn created. "The first
  // enemy" is not good enough once a test has more than one on the floor, and
  // registry view order is not something a test should depend on.
  void testKillLastSpawned();
  // Test helper: one live number off an equipped slot, by name. A test that
  // asserts a weapon is "better" needs to say WHICH number moved, and adding a
  // whole snapshot comparison for one float buries the assertion in noise.
  [[nodiscard]] float testWeaponStat(int slot, std::string_view name) const;
  // Test helper: put a chest on the floor and report what it would spend.
  int testSpawnChest(float x, float y, int tier, int grants);
  // Test helper: how many chest bodies are on the floor right now.
  [[nodiscard]] int testChestCount() const;
  // Test helper: remove every live enemy outright. There is no way to reach an
  // empty arena through the damage path -- a body at zero HP is only reaped by
  // the hit that killed it -- and "what does a projectile do when there is
  // nothing left to aim at" is exactly the case worth testing.
  void testDespawnEnemies();
  // Test helper: place an elite/champion/overlord enemy (tier 1..3) so tests
  // can inspect the guaranteed stat boosts, resistances and trait flags.
  // `def` selects the content enemy type (default: the first one).
  void testSpawnTieredEnemyAt(float x, float y, int tier, std::uint32_t traits = 0,
                              int def = 0);
  // Test helper: tier of every live Enemy (0 normal, 1 elite, 2 champion, 3
  // overlord). Order unspecified.
  [[nodiscard]] std::vector<int> testEnemyTiers() const;
  // Test helper: number of set trait flags for every live Enemy. Order
  // unspecified.
  [[nodiscard]] std::vector<int> testEnemyTraitCounts() const;
  // Test hook: freeze wave spawning so tests control the enemy pool exactly.
  void testDisableWaves() { wavesEnabled_ = false; }
  void testEnableWaves() { wavesEnabled_ = true; }
  // Test helper: content index of the weapon with this id (-1 if unknown), so a
  // test can add a NAMED weapon instead of trusting a row order.
  [[nodiscard]] int testWeaponContentIndex(std::string_view id) const;
  // Test helper: which effect ids belong to the weapon-wide set. Public so the
  // effect-coverage test asks the applier instead of keeping its own copy of the
  // list -- which is exactly how a card ships that prints a promise and does
  // nothing.
  [[nodiscard]] static bool isWeaponWideEffect(std::string_view effect) {
    return effect == "w_all_damage" || effect == "w_all_rate" ||
           effect == "w_all_reach" || effect == "w_all_knockback";
  }
  // Test helper: the content index of an upgrade card by id, or -1. Every other
  // "find this card" in the tests is a hand-rolled loop over the vector, which
  // means every one of them re-decides what happens when two cards share an id.
  [[nodiscard]] int testUpgradeContentIndex(std::string_view id) const;
  // Test helper: spend a chest exactly as walking into it would, and report how
  // many cards it actually gave.
  int testOpenChestFor(int grants) { return openChest(grants); }
  [[nodiscard]] std::vector<int> testLegalWeaponCards(int slot) const {
    return legalWeaponCards(slot);
  }
  // Test helper: open the first chest on the floor and remove it, which is what
  // the pickup does.
  void testOpenFirstChest();
  // Test hook: jump the run clock, so time-gated systems (spawn tiers, scaling
  // curves) can be tested without simulating minutes of real time.
  void testSetSimTime(float seconds) { simTime_ = seconds; }
  // --- Weapon test sandbox (T) — public controls, used by input + tests ------
  void enterTestMode();
  void exitTestMode();
  void setTestWeapon(int defIndex);
  void toggleTestBoost() { toggleTestBoost_(); }
  void toggleTestInvuln() { testInvuln_ = !testInvuln_; }
  // Difficulty-timer presets: 1x, 4x, 10x, 20x, then back to 1x.
  void cycleTestTimeScale();
  void toggleTestShop();
  // Test hook: grant one stack of an upgrade straight into the sandbox.
  bool testGrantUpgrade(int index) { return grantTestUpgrade(index); }
  // Test hook: the sandbox "max everything" cheat.
  void testMaxAllItems();
  // Test hook: put the player at death's door through the normal damage path.
  void testKillPlayer();
  // Test helper: apply a per-weapon upgrade (same path as weapon Focus cards).
  void testAddWeaponUpgrade(int slot, std::string_view effect, float value) {
    applyWeaponEffect(slot, effect, value);
  }
  // Test helper: current HP of the first Enemy in the registry (-1 if none).
  [[nodiscard]] float testFirstEnemyHp() const;
  // Test helper: how many cards the most recently opened chest actually granted,
  // and which content card the last of them was. -1 / 0 before any box is opened.
  [[nodiscard]] int testLastChestGrants() const { return lastChestGrants_; }
  [[nodiscard]] int testLastChestCard() const { return lastChestCard_; }
  // What a box actually handed out, in order. The reveal panel is the only
  // place a champion's three cards are ever named, so a test can ask.
  [[nodiscard]] const std::array<int, kChestRevealMax>& testChestReveal() const {
    return chestReveal_;
  }
  [[nodiscard]] int testChestRevealCount() const { return chestRevealCount_; }
  // Test helper: elites-and-above currently alive. The spawner consults it so
  // the screen cannot fill with elites, and so a test can pin that cap.
  [[nodiscard]] int testLiveTierCount() const;
  // Test helper: move the player straight to a level and rebuild the offer, so a
  // test can look at a milestone screen without playing four minutes to reach it.
  // Level-up is a state machine in the real game; this jumps the counter only.
  void testSetLevel(int level) {
    level_ = level;
    milestoneOffer_ = false;
    buildChoices();
  }
  // Test helper: whether an upgrade card has been closed off by an earlier pick
  // from its mutually exclusive group.
  [[nodiscard]] bool testUpgradeBlocked(int index) const {
    return index < 0 || static_cast<std::size_t>(index) >= blocked_.size() ||
           blocked_[static_cast<std::size_t>(index)] != 0;
  }
  // Test helper: the speed multiplier each body is currently pinned to (its own
  // slowMul while a chill is running, 1.0 otherwise), and the chill's seconds
  // left, interleaved.
  [[nodiscard]] std::vector<float> testEnemySpeedMuls() const;
  // Test helper: damage-aura radius/dps of the first Enemy carrying an aura
  // (0 if none), so the per-tier aura sizes can be asserted.
  [[nodiscard]] float testFirstAuraRadius() const;
  [[nodiscard]] float testFirstAuraDps() const;
  // Test helper: true when the first Enemy can shoot (has the Archer trait).
  [[nodiscard]] bool testFirstCanShoot() const;
  // Test helper: overwrite the first enemy's current HP (death-boundary tests).
  void testSetFirstEnemyHp(float hp);
  // Test helper: force the first enemy's knockback resistance (and give it the
  // EnemyTraits component it does not have by default). Lets a test compare how
  // hard a shove or a suction field moves a soft enemy versus a boss.
  void testSetFirstEnemyKnockbackRes(float res);
  // --- Momentum introspection (HUD + tests) -----------------------------------
  [[nodiscard]] int killStreak() const { return streak_; }
  [[nodiscard]] float killStreakTimer() const { return streakTimer_; }
  [[nodiscard]] float momentumDamageMul() const { return momentumDamageMul_; }
  [[nodiscard]] float momentumFireRate() const { return momentumRate_; }
  [[nodiscard]] float momentumSpeedMul() const { return momentumSpeedMul_; }
  // Test hook: set the chain directly.
  void testSetStreak(int streak) {
    streak_ = std::max(0, streak);
    streakTimer_ = 0.0F;
  }
  // Test helper: route a raw damage packet through applyEnemyDamage() on the
  // first enemy (so defense mitigation and the lethal rule are exercised).
  void testDamageFirstEnemy(float dmg);
  // Test helper: wound the PLAYER by `amount` HP, bypassing defense (the point
  // is to set up a heal test, not to measure mitigation). Clamped so the run
  // never ends from a test's own bookkeeping.
  void testDamagePlayer(float amount);
  // Test helper: distance from the player to the first Enemy (-1 if none).
  [[nodiscard]] float testFirstEnemyDistToPlayer() const;
  // Test helper: distance from the first Enemy to the NEAREST live Vortex zone
  // (-1 if there is no enemy or no zone). This is what proves the suction zones
  // actually drag prey into their cores.
  [[nodiscard]] float testFirstEnemyDistToVortex() const;
  // Test helper: distance from the first enemy to the NEAREST planted Lure bell
  // (-1 if there is no enemy or no bell). This is what proves the Grave Bell
  // actually taunts prey into its core instead of just sitting there.
  [[nodiscard]] float testFirstEnemyDistToLure() const;
  // Test helpers: the player's world position (Blink moves it).
  [[nodiscard]] float testPlayerX() const;
  [[nodiscard]] float testPlayerY() const;
  // Test helper: the aim direction of every live beam, in radians. Used to prove
  // the Solar Lance's "Prism Lance" unique really spreads into front/left/right
  // instead of stacking three beams on the same line.
  [[nodiscard]] std::vector<float> testBeamAngles() const;
  // Test helper: current speed (velocity magnitude) of every live Enemy.
  [[nodiscard]] std::vector<float> testEnemySpeeds() const;
  // Test helper: current HP of every live Enemy (order unspecified — use for
  // sums / range checks, not positional assertions).
  [[nodiscard]] std::vector<float> testEnemyHps() const;
  // Test helper: contact radius of every live bounce projectile (the eternal
  // Void Orb grows its size with the projectile stat).
  [[nodiscard]] std::vector<float> testBounceRadii() const;
  // Test helper: number of live chain bolts, live travelling waves and live
  // bounce projectiles. These are the three things a chain/fork/split can spawn
  // from inside an update system, so they are the numbers that decide whether a
  // cascade is bounded.
  [[nodiscard]] std::size_t testChainCount() const;
  [[nodiscard]] std::size_t testWaveCount() const;
  [[nodiscard]] std::size_t testBounceCount() const;
  // Test helper: number of live straight Projectiles. The Blizzard Rail's shard
  // fan is made of these (deliberately not bolts), so this is how a test sees
  // the shatter.
  [[nodiscard]] std::size_t testProjectileCount() const;
  // Test helper: world position of every live arcing shell, flattened x,y pairs.
  // A shell's landing point is the whole Siege Mortar: it is aimed at the front
  // rank and detonates BEHIND it, so where the shell is going is the only way to
  // see the difference from a bomb that lands where you pointed.
  [[nodiscard]] std::vector<float> testBombPositions() const;
  // Test helper: current radius of every live nova ring. A ring that expands
  // starts near zero and grows; a contracting one starts at its maximum and
  // shrinks to nothing, so the sequence is what proves the two are opposites
  // rather than one bigger than the other.
  [[nodiscard]] std::vector<float> testNovaRadii() const;
  // Test helper: the inner dead-zone radius of every live halo spoke. Zero means
  // the spoke reaches the player (a personal guard); a positive value means there
  // is a ring of safe ground at their feet (a wall they keep things out of).
  [[nodiscard]] std::vector<float> testHaloBeamInners() const;
  // Test helper: the collapse charge of every live vortex well, in seconds, and
  // its current orbit angle, flattened charge,angle pairs. A well that collapses
  // wraps its charge and jumps its angle by half a turn; a well that does not
  // (the Void Gyre) keeps its charge pinned at zero and its angle advancing
  // smoothly, and that is the entire difference between the two weapons.
  [[nodiscard]] std::vector<float> testVortexCharges() const;
  // Test helper: the angle of the gap in a slot's blade ring, or -1 when the ring
  // has no gap (no "Blade Vortex" card, or a single blade, where a gap would
  // have nowhere to be). The gap is the only place the interior grind bites, so
  // a test needs the angle to place a target in it deliberately.
  [[nodiscard]] float testOrbitWindowAngle(int slot) const;
  // Test helper: world position of every live BoomerangProjectile, flattened x,y
  // pairs. The Pulsar's trail is a corridor along the blade's remembered path, so
  // "was this target hit by the blade or by the scar it left" is a question about
  // where the blade was, and this is the only way to ask it.
  [[nodiscard]] std::vector<float> testBoomerangPositions() const;
  // Test helper: current HP of the enemy whose centre is NEAREST to a point (-1 if
  // there is no enemy). testEnemyHps() returns an unordered list, which is fine for
  // sums and useless for "did the drill chew this one and not that one" -- and the
  // whole point of the drill is which body it chose.
  [[nodiscard]] float testEnemyHpNear(float x, float y) const;
  // Test helper: how many live chain bolts are still converging (telegraph > 0)
  // and how many have landed (telegraph == 0). The strike is in three beats, and
  // "a bolt that has not damaged anything yet" has to be observable, or the
  // wind-up is invisible to a test and therefore free to disappear.
  [[nodiscard]] std::vector<int> testChainTelegraphs() const;
  // Test helper: world position of every live travelling wave. A wave LEAVES the
  // player, so this is what proves it is not just a nova pinned to the player.
  [[nodiscard]] std::vector<float> testWavePositions() const;
  // Test helper: world position of every live Enemy, flattened x,y pairs. Used to
  // assert where knockback did and did not move a target.
  [[nodiscard]] std::vector<float> testEnemyPositions() const;
  // Test helper: world position of every live BounceProjectile, flattened x,y
  // pairs. A ricochet that has nothing to hit should curve back toward the player
  // rather than leave the arena, and this is the only way to see where it went.
  [[nodiscard]] std::vector<float> testBouncePositions() const;
  // Test helper: the speed multiplier and remaining time of every live Enemy's
  // chill, flattened mul,time pairs. A multiplier of 1.0 means not chilled.
  [[nodiscard]] std::vector<float> testEnemyChills() const;
  // Test helper: run the simulation forward by `seconds` of fixed steps without
  // rendering. Weapon fire and the effect update systems only run inside a
  // step, so a test that wants to watch a bolt travel has to drive them itself.
  void testAdvance(float seconds);
  // Test helper: move the player. Anything whose rule is about DISTANCE THE
  // PLAYER COVERS -- the Hearthfire trail lays its pools by how far the cone's
  // tip has travelled, so a stationary player lays exactly one and a walking one
  // lays a road -- is untestable without this.
  void testSetPlayerPosition(float x, float y);
  // Test helper: the equipped weapon ids, oldest first (e.g. for test-mode
  // snapshot/restore assertions). Empty when no weapons are equipped.
  [[nodiscard]] std::vector<std::string> armedWeaponIds() const;
  // Test helper: the names of every card in content that is scoped to this
  // weapon id (focus cards and weapon uniques alike), in content order. Lets a
  // test assert the "every weapon has cards" rule against the same roster the
  // game offers.
  [[nodiscard]] std::vector<std::string> collectWeaponCards(std::string_view weaponId) const;
  // Test helper: angular position (radians) of the first orbit blade, so a
  // test can measure how fast the dagger spins. -1 if no blades exist.
  [[nodiscard]] float testOrbitBladeAngle() const;
  // Test helper: circular angular gaps (radians) between a slot's orbit
  // blades, sorted. All gaps equal 2*pi/count when blades are evenly spaced.
  [[nodiscard]] std::vector<float> testOrbitBladeGaps(int slot) const;
  // Test helper: current invulnerability window (seconds) and what a base
  // window becomes after the defense scaling.
  [[nodiscard]] float testIframes() const { return iframes_; }
  [[nodiscard]] float testIframeDuration(float base) const { return iframeDuration(base); }
  // Test helper: bestiary kill count / tier bitmask for a content enemy index.
  [[nodiscard]] int testBestiaryKills(int def) const {
    if (def < 0 || static_cast<std::size_t>(def) >= bestiaryKills_.size()) return 0;
    return bestiaryKills_[static_cast<std::size_t>(def)];
  }
  [[nodiscard]] int testBestiaryTiers(int def) const {
    if (def < 0 || static_cast<std::size_t>(def) >= bestiaryTiers_.size()) return 0;
    return static_cast<int>(bestiaryTiers_[static_cast<std::size_t>(def)]);
  }
  // Test helper: deal direct damage to the player through the normal path.
  void testHurtPlayer(float amount) { hurtPlayer(amount); }
  // Test helper: set the player's current HP (for low-HP unique tests).
  void testSetPlayerHp(float hp);
  // Test helper: kill the first live Enemy through the normal kill path (so
  // the bestiary records it). No-op when the registry has no enemies.
  void testKillFirstEnemy();
  // Highest tier slain this run (0 none, 1 elite, 2 champion, 3 overlord), and
  // the enemy def index that achieved it. Drives the bestiary's "strongest
  // kill" line.
  [[nodiscard]] int strongestKilledTier() const { return strongestKilledTier_; }
  [[nodiscard]] int strongestKilledDef() const { return strongestKilledDef_; }
  // Bitmask (1<<tier) of every tier killed at least once this run. Used both by
  // the bestiary and to unlock the elite/champion/overlord player outlines.
  [[nodiscard]] std::uint8_t tierKillMask() const { return tierKillMask_; }
  // --- Adaptive tribunal director (test + HUD introspection) -----------------
  // True while a tier is allowed to spawn at all. Tier 0 always, tier 1 from
  // 90s on, tier 2 once elites are routine, tier 3 once champions are.
  [[nodiscard]] bool tierUnlocked(int tier) const;
  // Per-member spawn chance for a tier right now (0 while the tier is shut).
  [[nodiscard]] float tierSpawnChance(int tier) const;
  // Decaying "handling" score for a tier (elite/champion scores drive the
  // next tier's gate).
  [[nodiscard]] float tierPressure(int tier) const {
    return tier >= 0 && tier < 4 ? tierPressure_[tier] : 0.0F;
  }
  // Test hook: feed the director directly, so the champion/overlord gates can be
  // tested without simulating minutes of real kills.
  void testAddTierPressure(int tier, float amount) {
    if (tier > 0 && tier < 4) tierPressure_[tier] += amount;
  }
  void testSetTierOpen(int tier, bool open) {
    if (tier > 0 && tier < 4) tierOpen_[tier] = open;
  }
  // --- Enemy-type retirement -------------------------------------------------
  // When an overlord of a given enemy type spawns, that type is "retired": it
  // no longer spawns, which stops the same overlord from repeating forever.
  // The 3 most recently unlocked types are exempt so the spawn pool can never
  // dry up (there is always something new to fight).
  void retireEnemyType(int def);
  [[nodiscard]] bool typeRetired(int def) const;
  // Full spawn-eligibility rule used by pickDef (unlocked AND not retired,
  // unless it is one of the 3 most recent types).
  [[nodiscard]] bool typeCanSpawn(int def) const;
  // Run time before `def` becomes eligible at all: the later of its own
  // unlock_at and, for a `fast` archetype, kFastEnemyMinTime. Public so a test
  // can reason about the gate instead of duplicating the rule (and going stale
  // the next time the floor moves).
  [[nodiscard]] float typeLockedUntil(int def) const;
  // Per-type speed ramp at the current run clock: 1.0 for a type that does not
  // accelerate on its own, rising to 1 + speedRampMax at kSpeedRampFullTime.
  [[nodiscard]] float typeSpeedRamp(int def) const;
  // The elite "Fast" trait's speed multiplier at the current run clock: 1.0
  // (no bonus) at the start, rising to kFastTraitMaxMul by kFastTraitFullTime.
  [[nodiscard]] float fastTraitSpeedMul() const;
  // The exact speed a fresh spawn of `def` gets right now (authored speed, then
  // the per-type ramp, then the global difficulty ramp). One function so a test
  // reads the real product instead of re-deriving the factors.
  [[nodiscard]] float enemySpawnSpeed(int def) const;
  // Test helpers for the retirement rule.
  void testRetireType(int def) { retireEnemyType(def); }
  [[nodiscard]] bool testTypeRetired(int def) const { return typeRetired(def); }
  [[nodiscard]] bool testTypeCanSpawn(int def) const { return typeCanSpawn(def); }
  // Test helper: whether a def index is among the 3 most recently unlocked
  // types (the ones exempt from retirement).
  [[nodiscard]] bool testTypeIsRecent(int def) const;

  // Debug introspection for tests: how many live entities each attack type has.
  struct DebugCounts {
    std::size_t projectiles = 0;
    std::size_t orbitBlades = 0;
    std::size_t halos = 0;
    std::size_t bombs = 0;
    std::size_t boomerangs = 0;
    std::size_t bounces = 0;
    std::size_t beams = 0;
    std::size_t sweeps = 0;
    std::size_t zones = 0;
    std::size_t chains = 0;
    std::size_t novas = 0;
    std::size_t vortices = 0;
    std::size_t lures = 0;
  };
  [[nodiscard]] DebugCounts debugCounts() const;
  [[nodiscard]] std::size_t debugEnemyCount() const;

  // --- Active abilities (J / K / L) -------------------------------------------
  // Every run has all three buttons, from the first second, with no unlock and
  // no card. They cost nothing but a cooldown, so the interesting decision is
  // never "do I have it" but "is now the moment" — which is exactly what a
  // finished build needs. Each answers a different problem a run keeps hitting:
  // J gets you out of a hug, K clears what your weapons cannot reach through,
  // L buys the three seconds a big enemy needs to die.
  enum class Ability : int { Blink = 0, Burst = 1, Stasis = 2 };
  static constexpr int kAbilityCount = 3;
  // Base cooldowns in seconds, scaled by PlayerStats::abilityCdMul.
  static constexpr float kBaseAbilityCd[kAbilityCount] = {5.0F, 14.0F, 30.0F};
  [[nodiscard]] float abilityCooldown(Ability a) const {
    return kBaseAbilityCd[static_cast<int>(a)] * stats_.abilityCdMul;
  }
  // Seconds left on an ability (0 = ready).
  [[nodiscard]] float abilityCooldownRemaining(Ability a) const {
    return abilityCd_[static_cast<int>(a)];
  }
  [[nodiscard]] bool abilityReady(Ability a) const {
    return abilityCd_[static_cast<int>(a)] <= 0.0F;
  }
  // Enemy time multiplier right now (1.0 unless Stasis is up).
  [[nodiscard]] float worldTimeScale() const { return worldTimeScale_; }
  [[nodiscard]] float stasisRemaining() const { return stasis_; }
  // Test hook: cast an ability through the normal input path.
  void testTriggerAbility(Ability a);

private:
  // Owned weapons (fixed slots, no allocation on the hot path).
  struct WeaponSlot {
    int def = -1;
    AttackType attackType = AttackType::Projectile;
    float cooldown = 0.5F;
    float timer = 0.0F;
    float damage = 5.0F;
    int projectiles = 1;
    float speed = 12.0F;
    float life = 1.4F;
    int pierce = 0;
    float spread = 0.16F;
    core::render::Color color{1.0F, 0.95F, 0.55F, 1.0F};

    // Cone
    float coneAngle = 0.8F;
    float coneRange = 2.5F;
    float coneTickRate = 0.1F;
    float coneTimer = 0.0F;        // for continuous cone damage
    // --- The drill's bite -----------------------------------------------------
    // The Ember Sprayer and the Jackhammer Drill were both "a cone", so they were
    // the same weapon with different numbers: the drill was narrower, longer and
    // ticked faster, which is not a different idea. `coneBite` is the difference.
    // While it is non-zero the cone latches onto the nearest body in the arc,
    // chews only that one, and its damage climbs by coneBite per consecutive tick
    // (up to coneBiteMax times the base). The Sprayer washes a crowd evenly; the
    // drill commits to one target and gets through its armour, which is a real
    // answer to an elite and a real mistake against trash.
    float coneBite = 0.0F;
    float coneBiteMax = 4.0F;
    // What the bit is currently buried in, and for how many consecutive ticks.
    // Zero means "not latched", which is also the state after the latch target
    // dies or walks out of the arc -- then it re-latches and the ramp restarts.
    entt::entity coneLatch = entt::null;
    int coneBiteTicks = 0;

    // Hearthfire: the cone leaves burning ground behind it. A cone that only ever
    // damages what is inside it this instant is a cone you must keep pointing at
    // things; one that scorches where it has been is a trail you lay down and then
    // walk away from. `coneEmberAt` is how far the tip must travel before it drops
    // another pool -- 0 disables the rule entirely, which is what every cone except
    // the upgraded Ember Sprayer wants.
    float coneEmberAt = 0.0F;
    float coneEmberRadius = 1.0F;
    float coneEmberDuration = 2.5F;
    // Distance already walked since the last pool. Kept as a walked distance
    // rather than a timer so a fast player does not leave a thinner trail.
    float coneEmberWalked = 0.0F;
    // Where the tip was last time, so the walk is measured against the cone and
    // not against the player's own movement (the player can turn in place and the
    // tip still sweeps a long way).
    float coneEmberLastX = 0.0F;
    float coneEmberLastY = 0.0F;
    // Live pools this weapon owns, so the cap can be enforced against its OWN
    // pools rather than against every burning patch on the map.
    int coneEmberLive = 0;

    // Orbit
    float orbitRadius = 1.2F;
    float orbitSpeed = 2.0F;
    int orbitCount = 2;
    float orbitAngle = 0.0F;       // current rotation angle
    // Blade Vortex: the ring has one gap in it, and the interior grind only bites
    // inside that gap. The gap therefore stops being a place to stand and becomes
    // the place to stand, because it is the one spot in the ring where the horde
    // that followed you in is actually being cut.
    bool orbitWindow = false;

    // Bomb
    float bombArcHeight = 2.0F;
    float bombExplodeRadius = 1.5F;
    float bombKnockback = 3.0F;
    float bombFuse = 0.0F;
    // Deliberate over-shoot: land this many units PAST the enemy nearest the aim
    // line. The Runic Hammer blows up what you were aiming at; the Siege Mortar
    // sails over the front rank and cooks the back of the horde.
    float bombAhead = 0.0F;
    // Land ON the body nearest the aim line instead of past it. The mirror of
    // `bombAhead`, and the Runic Hammer's whole identity.
    bool bombOnTarget = false;
    // Re-aim: bend onto the next body on every hit, spending a pierce point.
    float reaimRange = 0.0F;
    float reaimTurn = 0.0F;

    // Boomerang
    float boomerangRange = 4.0F;
    float boomerangReturnSpeed = 1.5F;

    // Bounce
    int bounceCount = 3;
    float bounceRange = 2.5F;
    float bounceDamageMul = 0.7F;
    bool bounceInfinite = false; // eternal orb: no life/bounce budget
    entt::entity liveBounce = entt::null; // the one eternal orb (single, grows)

    // Beam
    float beamRange = 8.0F;
    float beamWidth = 0.3F;
    float beamDuration = 0.15F;

    // Chill carried by this weapon's projectiles. Zero disables it. The Rime
    // Lanes unique deepens and lengthens the chill rather than swapping it for
    // bigger damage numbers, which is what the card used to do.
    float chillMul = 0.0F;
    float chillTime = 0.0F;
    // The corona this weapon's shots drag through the air: radius, damage per
    // second inside it, the tick that pays that damage out, and the (weaker)
    // chill it applies. Chill itself is an on-HIT status, so it only ever reaches
    // the bodies a shot went through; this is what reaches the ones it passed.
    float auraRadius = 0.0F;
    float auraDps = 0.0F;
    float auraTick = 0.10F;
    float auraChillMul = 0.0F;
    float auraChillTime = 0.0F;

    // Halo (evolution)
    float haloKnockback = 0.0F;
    // Dead zone at the player's feet, in world units. 0 = the spoke reaches the
    // centre (a personal guard, the Radiant Halo). > 0 = it does not, so the
    // weapon is a rotating wall with safe ground inside it rather than a bigger
    // guard, which is what the Seraph Array's wings are.
    float haloInner = 0.0F;

    // Sweep
    float sweepAngle = 3.14F;
    float sweepRadius = 2.0F;
    float sweepKnockback = 2.0F;

    // Zone
    float zoneRadius = 1.2F;
    float zoneDuration = 4.0F;
    float zoneDps = 15.0F;
    int zoneMaxPools = 3;
    /// The pool is laid by a shell that fell on it, so it is drawn as a shaft
  /// rather than as a low disc. See ZoneEffect::fromAbove.
  bool zoneFromAbove = false;

    // Chain (evolution)
    float chainJumpRange = 2.5F;
    int chainMaxJumps = 4;
    float chainDamageMul = 0.6F;
    // How many EXTRA bolts branch off at each hop. 0 = a single linear arc
    // (Tesla Coil). >0 = the bolt forks and the crowd gets lit up from several
    // directions at once (Storm Caller). This, not the damage number, is what
    // tells the two apart on screen.
    // Flat shards thrown off on the first hit (Blizzard Rail): the slug comes
    // apart instead of carrying on intact.
    int chainShatter = 0;
    float chainShatterSpeed = 15.0F;
    float chainShatterSpread = 0.55F;

    // Bounce: how many child orbs each bounce throws off. 0 = the puck stays a
    // single puck (Pinball). >0 = it comes apart on every impact, so a single
    // Chaos Orb fills the screen with ricocheting fragments (Chaos Orb).
    int bounceSplits = 0;

    // Wave (Sunder): a crescent that LEAVES the player and travels, unlike a
    // nova which stays centred on the player and only grows. Damages each
    // enemy once, and shoves it along the wave's direction of travel.
    float waveSpeed = 6.0F;
    float waveRange = 7.0F;
    float waveWidth = 2.2F;
    float waveKnockback = 4.0F;
    float waveDamageMul = 0.8F;
    // How many arcs a single shot throws, each rotated by waveArcStep. 1 = one
    // crescent (a plain wave). >1 = the tide comes around you (Tidewhip).
    int waveCount = 1;
    float waveArcStep = 0.0F;
    // How far the crescent's horns open, in radians. Both wave weapons are the
    // same entity; this is the field that makes the Sundering Core read as a
    // wide slow wall and the Tidal Lash as a narrow fast rake, and it is drawn
    // (see the wave block in render()) rather than used for the hit box, which
    // stays a symmetric band behind the leading edge.
    float waveSpread = 0.9F;
    // > 0 makes the arcs HERD their catch across the fan toward the next arc
    // instead of shoving it down their own heading. 0 is the plain shove, which
    // is the Sundering Core; the Tidal Lash is the one that hands its catch on.
    float waveHookPull = 0.0F;
    // Still-maturing bits of a multi-arc shot, so a 3-arc whip spreads over a
    // beat instead of landing as one flat wall.
    int waveBurstLeft = 0;
    float waveBurstTimer = 0.0F;
    float waveBurstAngle = 0.0F;

    // Nova (evolution)
    float novaMaxRadius = 4.0F;
    float novaExpandSpeed = 3.0F;
    float novaDamagePerTick = 25.0F;
    float novaTickRate = 0.15F;
    // The contracting ring. Void Nova and Shock Core were both "a nova", so one
    // was a strictly bigger version of the other. A contracting ring is cast at
    // full radius, rushes the player, drags what it passes toward the centre, and
    // detonates on arrival -- the inverse of the Shock Core, which is a wall
    // expanding outward and shoving the horde into open ground.
    bool novaContract = false;
    float novaPull = 0.0F;
    float novaBurstDamage = 0.0F;
    // Discharge: a SECOND ring cast this many seconds after the first. The card's
    // rule is that a pulse is now a double, not that the pulse is bigger -- the
    // second ring lands after the first has thinned the pack, which is the whole
    // reason to want it. 0 = one ring only.
    float novaEcho = 0.0F;
    int novaEchoesLeft = 0;
    float novaEchoTimer = 0.0F;
    float novaRadius = 0.0F;       // current radius
    float novaTimer = 0.0F;        // tick timer
    bool novaActive = false;       // whether nova is expanding

    // General projectile fields (for projectile/boomerang/bounce)
    float area = 0.0F;
    float strength = 0.0F;
    bool homing = false;
    int bounces = 0;

    // Per-weapon fire-rate bonus (additive; see attackCooldown()).
    float cdBonus = 0.0F;
    // Sweep: the arc is centered this far in front of the player (scythe).
    float sweepLead = 0.0F;
    // A hooked whip drags its catch in rather than shoving it out. Off by
    // default, and only the two whip cards turn it on, so the Soul Scythe keeps
    // being the one weapon whose knockback circle works exactly as its name says.
    bool sweepHook = false;
    // Weapon-unique modifiers.
    float uniqueHeal = 0.0F; // scythe "Reaper's Harvest": heal HP per kill
    int beamSplit = 0;       // beam "Prism Lance": beam count multiplier

    // Vortex (evolution): rotating suction zones around the player.
    float vortexRadius = 1.3F;   // damage core radius
    float vortexReach = 2.6F;    // how far out enemies start getting pulled in
    float vortexPull = 4.0F;     // pull strength (world units / sec)
    float vortexOrbit = 2.6F;    // distance of the zone from the player
    float vortexOrbitSpeed = 1.8F;
    float vortexTickRate = 0.1F;
    // How long a well has to swallow before it collapses. This is the difference
    // between the two vortex weapons. The Void Gyre never collapses: it is a
    // patient, permanent drag you can build a position around, and the damage is
    // a steady drip. A collapsing well hoards, gathers, and pays out in one
    // violent moment -- lumpier damage, but a threat you can read on a clock.
    float vortexCollapseAt = 0.0F;
    // See WeaponDef::vortexCrowd. Lives here (not only on the def) so the tick
    // can read the live value the way it reads pull, damage and reach.
    float vortexCrowd = 0.0F;
    float vortexBurstDamage = 0.0F;
    float vortexBurstRadius = 0.0F;

    // Inferno: when true, the reap and the burning ground land on a planted
  // Grave Bell instead of on the nearest enemy (Siege Mortar). This is the
  // whole point of pairing a mortar with a lure: gather, then cook.
  bool infernoBindsToLure = false;

  // Prism (evolution): one locked beam per projectile, up to a hard cap.
    float prismRange = 9.0F;
    float prismWidth = 0.45F;
    int prismMaxTargets = 6;
    float prismRicochet = 4.0F;  // range of a reflected beam's next jump

    // Lure: planted beacon that drags enemies into its kill core.
    float lureRadius = 1.4F;
    float lureReach = 3.6F;
    float lurePull = 5.0F;
    float lureDps = 14.0F;
    float lureDuration = 5.0F;
    float lureTickRate = 0.15F;
    int lureMaxBeacons = 2;
  };
  // The storage arrays below hold the fully-stacked weaponCap(), which the
  // public constants above bound.

  // Pending spawn telegraphs (enemies walk in after a short warning).
  struct PendingSpawn {
    float x = 0.0F;
    float y = 0.0F;
    float t = 0.0F;
    int def = 0;
    float hpMul = 1.0F;
    float touchMul = 1.0F;
    float speedMul = 1.0F;
    float xpMul = 1.0F;
    std::uint32_t traits = TraitNone;
    std::uint8_t tier = 0;
  };

  // Elite name labels are no longer drawn; elites/champions/overlords are
  // marked by a coloured outline instead (see render()).

  // Particles: fixed ring buffer of PODs.
  struct Particle {
    float x, y, vx, vy, life, maxLife, size;
    core::render::Color color;
  };

  void fixedUpdate();
  // One simulation step. fixedUpdate() may run this several times per frame
  // when the test sandbox's difficulty clock is fast-forwarded.
  void fixedStep();
  void spawnWave();
  // Spawns one travelling crescent (the Wave attack type, see components.hpp).
  void spawnWaveCrescent(const WeaponSlot& w, float angle, float damage, int pierce);
  // One nova ring. Every ring in the game is spawned here, which is what keeps
  // the expanding Shock Core and the contracting Void Nova from drifting apart.
  void spawnNovaRing(float x, float y, const WeaponSlot& w, int pierce);
  // Drops a chest for a dead elite-and-above. Returns the entity, or null if this
  // death rolled no box.
  entt::entity spawnChest(float x, float y, int tier);
  // Spends a chest on the player's build: `grants` cards, each one an independent
  // 50/50 roll between a weapon's own cards and the item cards. A weapon card
  // prefers a weapon that has no card yet, so a box opens new lines instead of
  // stacking a fourth copy of one. Returns how many cards were actually granted.
  int openChest(int grants);
  // Clears the chest reveal panel in one place, so the card list and the
  // "this box gave N cards" counter can never disagree about whether a panel is
  // up. The renderer gates on both.
  void clearChestReveal(int tier);
  // Every weapon-targeted card in the content that is legal for the weapon in
  // `weaponSlot` and not yet maxed.
  [[nodiscard]] std::vector<int> legalWeaponCards(int weaponSlot) const;
  // Every ITEM card a box may hand out: no weapon of its own, not a milestone,
  // not blocked by an answered milestone group, not maxed, and usable as things
  // stand. This is the half of a chest that improves the run rather than one
  // weapon -- health, shields, the on-hit marks, the weapon-wide items.
  [[nodiscard]] std::vector<int> legalItemCards() const;

  // Edits one weapon's own numbers for a weapon-wide item. Separate from
  // applyWeaponEffect because the weapon-wide set answers a different question --
  // what does this card mean on EVERY weapon, not on THIS one -- and one function
  // with two halves is how the second half stops being tested.
  static void applyWeaponWide(WeaponSlot& w, std::string_view effect, float value);
  // Spawns one chain bolt. `depth` > 0 means a fork thrown off a parent.
  void spawnChainBolt(float x, float y, const WeaponSlot& w,
                      std::uint32_t fromTarget);
  // Throws the flat shard fan a shattering bolt pays out on its first hit.
  void spawnChainShatter(const ChainLightning& cl, float x, float y, float heading,
                         float damage);
  // Throws the radial ice burst a frozen corpse pays out when it dies. Separate
  // from the bolt shatter because it is a ring rather than a directed fan, and
  // because it is the ice weapon's payoff rather than a chain weapon's trick.
  void spawnShatterBurst(float x, float y, float damage);
  void processPendingSpawns();
  void fireWeapons();
  void movePlayer();
  void updateEnemies();
  void updateProjectiles();
  void updateOrbitBlades();
  void updateHaloBeams();
  void updateVortices();
  void updateBombProjectiles();
  void updateBoomerangProjectiles();
  void updateBounceProjectiles();
  void updateBeamEffects();
  void updateSweepEffects();
  void updateZoneEffects();
  void updateLures();
  void updateChainLightning();
  void updateWaveEffects();
  void updateNovaRing();
  void updateEnemyShots();
  void updatePickups();
  void updateShield();
  void updateUniqueEffects();
  void buildSpatialHash();
  void enterLevelUp();
  // Opening pick: the run begins with no weapon, offering three starter
  // weapons (wand/dagger/crossbow) instead of a random one.
  void enterStarterPick();
  void buildStarterChoices();
  void buildChoices();
  void chooseUpgrade(int slot);
  // Applies one upgrade card to the run (stats, stack count, per-weapon cards,
  // HP/shield top-up). Shared by the level-up picker and the test item list.
  bool applyUpgradeAt(int upgradeIndex);
  void reroll();
  // Consumes the pending level-up (spends its XP, advances the level and either
  // queues the next one or resumes play). Shared by every kind of pick.
  void completeLevelUp();
  // True when the card can actually be applied right now. Weapon-specific
  // cards are unusable while their weapon is not equipped.
  [[nodiscard]] bool upgradeIsUsable(int upgradeIndex) const;
  void addWeapon(int defIndex);
  void syncOrbitBlades(int slot); // add orbit blades up to the current count
  void syncHaloBeams(int slot);   // add halo beams up to the current count
  void syncVortices(int slot);    // add vortex zones up to the current count
  // weaponCap() (public, above) is the limit addWeapon enforces: weapons cannot
  // be added past it, and the level-up offer stops appearing once it is full.
  void applyWeaponEffect(int slotIndex, std::string_view effect, float value);
  int findWeaponSlot(std::string_view weaponId) const;
  bool ownsWeapon(int defIndex) const;
  // Collects every new weapon the player could be offered (evolutions first,
  // then normals), shuffled. Used to offer several weapon cards at once.
  std::vector<int> collectWeaponGrants();
  void spawnEnemy(const PendingSpawn& p);
  // Recomputes recentTypes_: the 3 highest-unlock_at enemy types, which are
  // exempt from overlord retirement so the spawn pool never runs dry.
  void refreshRecentTypes();
  // `mark` is true for damage the player dealt and false for the burn ticking,
  // so the burn cannot keep re-applying the mark that lit it. Every other caller
  // leaves it alone.
  void applyEnemyDamage(entt::entity e, float dmg, bool mark = true);
  // The four on-hit marks. Split out so the damage path stays readable and so a
  // test can call them directly on a known body.
  void applyMarks(entt::entity e, float towardX, float towardY);
  void updateChestToast(float dt);
  // Chill/status helper. `mul` < 1 is the speed the enemy is pinned to; the
  // strongest chill in play wins and any chill refreshes the timer.
  void applyChill(entt::entity e, float mul, float time);
  void killEnemy(entt::entity e);
  void chainBolt(float x, float y, float dmg);
  // Lifesteal now rolls on a KILL (not per hit) so a many-hit weapon cannot
  // out-heal the fight; see the definition for the full rationale.
  void tryLifestealOnKill(entt::entity source);
  void applyKnockback(entt::entity e, float angle, float force);
  // Knockback resistance AT THIS MOMENT. Tier/trait resistance is stored when
  // the enemy spawns, but the time-based part keeps growing for enemies that
  // were already on the field when the run started scaling.
  [[nodiscard]] float displacementResistance(entt::entity e) const;
  // Continuous fields (Void Gyre drag, Black Hole) use the same resistance but
  // keep a small floor, so a maxed enemy is hard to pin rather than immune to
  // crowd control outright.
  [[nodiscard]] float continuousPullScale(entt::entity e) const;
  // Repulsion Field unique: shove an enemy that just damaged the player.
  void retaliateKnockback(entt::entity attacker);
  void explodeBomb(entt::entity bomb, const BombProjectile& bp, float x, float y);
  void hurtPlayer(float amount);
  void damagePlayerDirect(float amount); // no thorns trigger (DoT auras)
  void spawnParticles(float x, float y, core::render::Color c, int count, float speed);
  void renderPlayerStats(core::render::Batcher& b, float px, float py);
  // The level-up card's title, shrunk to the card rather than sliced by the next
  // card's panel.
  void drawCardName(core::render::Batcher& b, std::string_view name, float x, float y,
                    float maxWidth, const core::render::Color& accent,
                    const core::render::Color& fallback);
  // Main menu overlay: title, the five rows (START / SKIN / OUTLINE / MANUAL /
  // RESET PROGRESS / QUIT) and the live skin+outline preview.
  void renderMainMenu(core::render::Batcher& b, float px, float py);
  // Handles one frame of menu input (navigation + activation). Runs before any
  // gameplay input so the menu is fully modal.
  void updateMainMenu(const FrameInput& input);
  // In-game manual overlay: one page at a time, page list on the left, the
  // page's own lines on the right.
  void renderManual(core::render::Batcher& b, float px, float py);
  // Handles one frame of manual input (page flipping, closing). The manual is
  // modal too: it eats the frame so a page flip never also moves the player.
  void updateManual(const FrameInput& input);
  // Skin + outline applied to the player Sprite, and the ring drawn around it.
  void applyProfileToPlayer();
  // Bestiary overlay (paused, B): discovered enemy types, their stats,
  // appearance, kill counts and which elite+ variants have been slain.
  void renderBestiary(core::render::Batcher& b, float px, float py);
  // Current global enemy scaling (shared by spawning and the bestiary).
  void currentScales(float& hp, float& speed, float& touch) const;
  // Player invulnerability window, nudged up ~1% per 5 defense.
  [[nodiscard]] float iframeDuration(float base) const;
  // Weapon test mode (T): cycle every weapon incl. evolutions, apply a boosted
  // build to see stat couplings, toggle waves, close to restore the run.
  // The entry/exit bodies are private; the thin public wrappers live with the
  // other test hooks above.
  void enterTestModeImpl();
  void exitTestModeImpl();
  void toggleTestBoost_();
  void spawnTestFodder();
  // Test sandbox: the "pick any item" list ([E], arrows, Enter).
  void updateTestShop(const FrameInput& input);
  void renderTestShop(core::render::Batcher& b, float px, float py);
  // Test sandbox: grant one stack of an upgrade without going through the
  // level-up roll. Returns false when the item is already maxed.
  bool grantTestUpgrade(int upgradeIndex);

  WeaponSlot weapons_[kMaxWeapons];
  int weaponCount_ = 0;
  // Weapon test mode state: snapshot of the real run while testing.
  bool testMode_ = false;
  bool testBoosted_ = false;
  // Stats snapshot taken the moment the "max build" boost was switched on, so
  // switching it back off only undoes the boost and keeps any item picks.
  PlayerStats testBoostBase_;
  int testWeaponIdx_ = 0;
  // --- Sandbox switches (all reset when the mode is left) --------------------
  bool testInvuln_ = false;   // immortality: incoming damage is ignored
  int testTimeScale_ = 1;     // difficulty-timer multiplier (1 / 4 / 10 / 20)
  bool testShopOpen_ = false; // the "pick any item" list
  int testShopCursor_ = 0;    // highlighted row in that list
  int testShopScroll_ = 0;    // first visible row (list is longer than the screen)
  // Full snapshot of the run. The sandbox is completely hermetic: anything that
  // happens inside it (XP, levels, kills, bestiary entries, outline unlocks,
  // item stacks, HP/shield) is rolled back when the mode is closed.
  int savedWeaponCount_ = 0;
  WeaponSlot savedWeapons_[kMaxWeapons];
  // Enemies already on the field when the sandbox opened. Everything spawned
  // after that is sandbox fodder and is removed on exit; the original horde is
  // kept so the run is not handed back a suspiciously empty arena. The full
  // component state is stored too: a sandbox weapon can kill, damage, displace
  // or knock the tier off a real enemy, and the run has to come back untouched.
  struct SavedEnemyState {
    entt::entity entity = entt::null;
    Transform transform{};
    Velocity velocity{};
    Radius radius{};
    Health health{};
    Enemy enemy{};
    EnemyTraits traits{};
    Sprite sprite{};
    Xp xp{};
  };
  std::vector<SavedEnemyState> savedEnemies_;
  // Every entity that existed when the sandbox opened. Anything else that
  // appears (XP orbs, drops, enemy shots, effect entities) is sandbox-only and
  // is deleted on exit instead of leaking into the real run.
  std::vector<entt::entity> savedEntityIds_;
  // Per-enemy spawn bookkeeping so a real enemy killed inside the sandbox can be
  // recreated with its original stats instead of simply vanishing.
  struct SavedSpawnState {
    int def = 0;
    int tier = 0;
    float hpMul = 1.0F;
    float touchMul = 1.0F;
    float speedMul = 1.0F;
    float xpMul = 1.0F;
    std::uint32_t traitFlags = TraitNone;
  };
  std::vector<SavedSpawnState> savedEnemySpawns_;
  float savedPlayerX_ = 0.0F;
  float savedPlayerY_ = 0.0F;
  PlayerStats savedStats_;
  std::vector<char> savedBlocked_;
  // The body the last test spawn created. Not saved with the sandbox: it points
  // at a test-only entity and is meaningless outside a test.
  entt::entity lastTestSpawn_ = entt::null;
  std::vector<int> savedStacks_;
  std::vector<int> savedBestiaryKills_;
  std::vector<std::uint8_t> savedBestiaryTiers_;
  float savedXp_ = 0.0F;
  float savedXpNext_ = 0.0F;
  int savedLevel_ = 1;
  int savedKills_ = 0;
  float savedHp_ = 0.0F;
  float savedHpMax_ = 0.0F;
  float savedShield_ = 0.0F;
  float savedShieldDelay_ = 0.0F;
  float savedIframes_ = 0.0F;
  float savedHealCd_ = 0.0F;
  float savedSimTime_ = 0.0F;
  bool savedWaves_ = true;
  bool savedChoosingStarter_ = false;
  int savedStrongestTier_ = 0;
  int savedStrongestDef_ = -1;
  std::uint8_t savedTierKillMask_ = 0;
  float savedTierPressure_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  bool savedTierOpen_[4] = {false, true, false, false};
  float savedTierGrace_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  std::string savedTierBanner_;
  float savedTierBannerT_ = 0.0F;
  int savedStreak_ = 0;
  float savedStreakTimer_ = 0.0F;
  float savedAbilityCd_[kAbilityCount] = {0.0F, 0.0F, 0.0F};
  float savedStasis_ = 0.0F;
  float savedWorldTimeScale_ = 1.0F;
  std::vector<PendingSpawn> pending_;
  std::vector<PendingSpawn> savedPending_;
  // --- Rest of the run state that also mutates inside the sandbox ----------
  float savedSpawnTimer_ = 0.0F;
  float savedHordeTimer_ = 0.0F;
  bool savedStarterChoicePending_ = false;
  float savedLastStandCd_ = 0.0F;
  std::vector<std::uint8_t> savedRetiredTypes_;
  std::vector<std::uint8_t> savedRecentTypes_;
  int savedRerollsUsed_ = 0;
  bool savedMilestoneOffer_ = false;
  std::vector<Choice> savedChoices_;
  float savedPoison_ = 0.0F;
  int savedChainCounter_ = 0;
  int savedBloodKills_ = 0;
  float savedBlackHoleTimer_ = 8.0F;
  float savedAdrenalineCd_ = 0.0F;
  bool savedAdrenalineActive_ = false;
  bool savedBestiaryOpen_ = false;
  UnlockMask savedSyncedUnlocks_ = 0;
  bool savedProfileDirty_ = false;
  float savedMoveX_ = 0.0F;
  float savedMoveY_ = 0.0F;
  float savedCamX_ = 0.0F;
  float savedCamY_ = 0.0F;
  float savedZoom_ = 48.0F;
  std::vector<Particle> savedParticles_;
  std::size_t savedParticleCursor_ = 0;
  std::mt19937 savedRng_{};

  const Content& content_;
  entt::registry registry_;
  core::sim::SpatialHash hash_{1.0F};
  core::sim::FixedTimestep timestep_{1.0 / 60.0, 2};
  std::mt19937 rng_;

  RunState state_ = RunState::Playing;
  float simTime_ = 0.0F;
  int level_ = 1;
  float xp_ = 0.0F;
  float xpNext_ = 6.0F;
  int kills_ = 0;
  float spawnTimer_ = 0.0F;
  float hordeTimer_ = 120.0F;  // first horde burst arrives at ~2 minutes
  bool wavesEnabled_ = true; // tests may freeze spawning for determinism
  float iframes_ = 0.0F;
  float healCd_ = 0.0F; // H heal cooldown remaining
  bool starterChoicePending_ = false; // show the opening 3-weapon pick
  bool choosingStarter_ = false;      // currently in that opening pick
  bool bestiaryOpen_ = false;         // bestiary overlay (paused)
  float lastStandCd_ = 0.0F;          // low-HP iframe unique cooldown
  std::vector<int> bestiaryKills_;    // kills per content enemy index
  std::vector<std::uint8_t> bestiaryTiers_; // bitmask of tiers killed
  // Enemy types retired by an overlord spawn (bit per content enemy index).
  // The 3 most recently unlocked types stay eligible regardless, so the pool
  // can never empty out.
  std::vector<std::uint8_t> retiredTypes_;
  // Bit per type: is this one of the 3 most recently unlocked? Recomputed as
  // more types unlock over the run.
  std::vector<std::uint8_t> recentTypes_;
  // Strongest enemy killed this run: the tier and the enemy def that reached it.
  int strongestKilledTier_ = 0;
  int strongestKilledDef_ = -1;
  // Bit per tier (1<<tier) of tiers killed at least once this run.
  std::uint8_t tierKillMask_ = 0;

  // --- Adaptive tribunal director --------------------------------------------
  // The heavy tiers are NOT on a timer: champions only start showing up once the
  // player is handling ELITES easily, and overlords once champions are routine.
  // `tierPressure_` is a decaying "how well is this tier being handled" score:
  // every kill of that tier adds weight, the score bleeds away over
  // kPressureWindow seconds, and the next tier is gated on the current one.
  static constexpr float kPressureWindow = 30.0F;
  // Elite kills (weighted 1.0) needed inside the window to call elites routine.
  static constexpr float kChampionPressure = 7.0F;
  // Champion kills needed inside the window to call champions routine.
  static constexpr float kOverlordPressure = 6.0F;
  // How many elite-and-above bodies may be alive at once. The ask was not "make
  // elites weaker" -- they are supposed to be the spike -- but "make them rarer,
  // so there are one or two on screen and meeting one is an event". A live cap
  // is the only version of that which is actually guaranteed: per-member spawn
  // chances are independent, so a pack of three can roll three elites and three
  // packs can roll nine, and no amount of tuning the percentage promises the
  // player a quiet screen. Rolling the tier happens only when there is room.
  static constexpr int kLiveTierCap = 2;
  // Even a great player waits this long: the first minute is for the build.
  static constexpr float kChampionMinTime = 90.0F;
  static constexpr float kOverlordMinTime = 240.0F;
  // Once earned, a tier stays open for a while so the director does not
  // flicker on and off between two kills.
  static constexpr float kTierGrace = 20.0F;
  float tierPressure_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  bool tierOpen_[4] = {false, true, false, false};
  float tierGrace_[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  // Transient HUD banner ("CHAMPION TRIBUNAL OPEN", "HORDE INCOMING"). One
  // channel is enough: the newest message replaces the old one.
  std::string tierBanner_;
  float tierBannerT_ = 0.0F;
  [[nodiscard]] int liveTierCount() const;
  void showBanner(std::string text, float seconds) {
    tierBanner_ = std::move(text);
    tierBannerT_ = seconds;
  }

  // Per-tick update of the director (decay + open/close with hysteresis).
  void updateTierDirector();

  // --- Momentum (the kill chain) ---------------------------------------------
  // Every kill adds to the chain; the chain dies if you stop killing for
  // `momentumWindow` seconds or if something hits you. While it is alive the
  // player is measurably stronger, which is what turns "walk away and let the
  // aura tick" into "stay in the middle of the horde".
  int streak_ = 0;
  float streakTimer_ = 0.0F;
  // Multipliers derived from the chain once per step; applied to every damage,
  // cooldown and movement calculation instead of being folded into PlayerStats
  // so the character sheet keeps showing the flat build.
  float momentumDamageMul_ = 1.0F;
  float momentumRate_ = 0.0F;
  float momentumSpeedMul_ = 1.0F;

  // The last chest the player opened: every card that came out of it, how many it
  // actually gave, and how long the panel stays up.
  //
  // It is a LIST, not a single card, because a box gives up to five of them and a
  // one-line toast naming the last one is how a champion's hand of three cards
  // arrives as a single ambiguous sentence. The player asked for this directly:
  // a simple readout of what fell out. Five is the overlord's cap, so the array
  // cannot be overrun -- and a sixth card is dropped rather than reallocating,
  // because a reveal that grows without bound is a reveal that stops being read.
  // kChestRevealMax itself is declared up with the other class constants, because
  // the test hooks below name it in a signature and a signature needs it first.
  std::array<int, kChestRevealMax> chestReveal_{};
  int chestRevealCount_ = 0;
  int chestRevealTier_ = 0;
  int lastChestCard_ = -1;
  int lastChestGrants_ = 0;
  float lastChestTimer_ = 0.0F;
  static constexpr float kChestToastTime = 3.4F;

  // Active ability state (see the Ability enum above).
  float abilityCd_[kAbilityCount] = {0.0F, 0.0F, 0.0F};
  // Stasis window remaining, and the enemy time multiplier it produces. The
  // player and every weapon keep running at full speed while this is up: only
  // the world slows down, which is what makes it readable at a glance.
  float stasis_ = 0.0F;
  float worldTimeScale_ = 1.0F;
  void updateMomentum();
  // Called when something actually lands a hit: the chain takes the hit with
  // you. Halved, minus two, and floored at zero.
  void breakMomentum();

  void updateAbilities(const FrameInput& input);
  void castBlink();
  void castBurst(float damageScale);
  void castStasis();

  // --- Profile / main menu state ---------------------------------------------
  // Not owned: main() owns the Profile and keeps it alive. Null in tests that
  // do not care, in which case skins stay at the default and nothing is
  // unlocked.
  Profile* profile_ = nullptr;
  bool profileDirty_ = false;
  // Unlock bits already pushed into the profile, so syncProfileUnlocks() only
  // reports genuinely new unlocks.
  UnlockMask syncedUnlocks_ = 0;
  bool menuOpen_ = false;
  // 0 START, 1 SKIN, 2 OUTLINE, 3 MANUAL, 4 RESET PROGRESS, 5 QUIT.
  // Named so the tests and the renderer cannot drift apart on the numbering.
  static constexpr int kMenuRows = 6;
  static constexpr int kMenuStart = 0;
  static constexpr int kMenuSkin = 1;
  static constexpr int kMenuOutline = 2;
  static constexpr int kMenuManual = 3;
  static constexpr int kMenuReset = 4;
  static constexpr int kMenuQuit = 5;
  int menuSelection_ = kMenuStart;
  bool quitRequested_ = false;
  // Armed by the first confirm on RESET PROGRESS; the wipe needs a second one.
  bool resetArmed_ = false;
  // "Q was pressed once on the pause screen" — the first press arms the abandon,
  // the second performs it. See the quitRun branch in advance().
  bool quitArmed_ = false;
  // In-game manual overlay.
  bool manualOpen_ = false;
  std::size_t manualPage_ = 0;
  // The selected outline is drawn as a ring around the player; this caches the
  // resolved color so render() does not touch the profile pointer.
  core::render::Color outlineColor_{0.0F, 0.0F, 0.0F, 0.0F};

  float moveX_ = 0.0F; // latched input for fixed steps
  float moveY_ = 0.0F;

  // Shield state (regen delay resets whenever damage is absorbed).
  float shield_ = 0.0F;
  float shieldDelay_ = 0.0F;

  // Unique-item state.
  int rerollsUsed_ = 0; // free rerolls consumed this level-up
  bool milestoneOffer_ = false;
  float poison_ = 0.0F; // venomous DoT timer on the player
  int chainCounter_ = 0;
  int bloodKills_ = 0;
  float blackHoleTimer_ = 8.0F;
  float adrenalineCd_ = 0.0F;
  bool adrenalineActive_ = false;

  PlayerStats stats_;
  std::vector<int> stacks_;    // per-upgrade stack counts
  // Per-upgrade "this one is closed off", set when a card from a mutually
  // exclusive group is taken. Separate from stacks_ on purpose: a blocked card
  // has zero stacks and is not available at max stacks -- it is a decision the
  // run already made and cannot take back.
  std::vector<char> blocked_;
  std::vector<Choice> choices_; // level-up cards

  entt::entity player_ = entt::null;

  // Camera (frame-level, not simulated).
  float camX_ = 0.0F;
  float camY_ = 0.0F;
  float zoom_ = 48.0F;

  // Scratch buffers, reused every tick (no hot-path allocation).
  std::vector<float> scratchX_, scratchY_;
  std::vector<std::uint32_t> scratchId_;
  std::vector<entt::entity> destroyQueue_;

  std::vector<Particle> particles_;
  static constexpr std::size_t kMaxParticles = 4096;
  std::size_t particleCursor_ = 0;
};

} // namespace game
