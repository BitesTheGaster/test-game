#!/usr/bin/env python3
"""Generate docs/content.md from assets/data/*.toml.

Run from the repo root (or anywhere):

    python3 tools/gendocs.py

The tables are fully generated — edit the TOML, not the markdown.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "assets" / "data"
OUT = ROOT / "docs" / "content.md"


def load(name: str) -> list[dict]:
    with open(DATA / name, "rb") as f:
        doc = tomllib.load(f)
    # each file uses a single top-level array key: weapon / enemy / upgrade
    for key in ("weapon", "enemy", "upgrade"):
        if key in doc:
            return doc[key]
    raise SystemExit(f"{name}: no content array found")


def md_escape(s: str) -> str:
    return s.replace("|", "\\|")


def table(headers: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join(["---"] * len(headers)) + "|"]
    out += ["| " + " | ".join(md_escape(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def fmt(x: float) -> str:
    if isinstance(x, int) or float(x).is_integer():
        return str(int(x))
    return f"{x:g}"


# The tier ladder (elite / champion / overlord) is not data: it is a set of
# constants and a switch in C++, and it is the single most balance-sensitive
# thing in the game. Copying it into this script would be a second source of
# truth that drifts silently, which is exactly how the previous version of this
# document ended up describing a 450x HP spread and a champion every seven
# seconds -- numbers that were true once and were left behind.
#
# So the numbers are READ OUT of the source, and a parser that cannot find one
# raises instead of emitting a blank. A stale parse fails loudly; a hand-typed
# table fails quietly, and the quiet one is the one that ships.
def cpp_consts(text: str) -> dict[str, float]:
    """Every `constexpr float|int NAME = 12.5F;` in the text, by name."""
    out: dict[str, float] = {}
    for m in re.finditer(
        r"constexpr\s+(?:float|int)\s+(k\w+)\s*=\s*(-?[0-9.]+)[fF]?\s*;",
        text,
    ):
        out[m.group(1)] = float(m.group(2))
    return out


def need(consts: dict[str, float], *names: str) -> list[float]:
    missing = [n for n in names if n not in consts]
    if missing:
        raise SystemExit(
            "tools/gendocs.py: cannot read " + ", ".join(missing) + " from the "
            "source. The tier ladder moved; update cpp_consts/need so this "
            "document fails instead of printing a blank cell."
        )
    return [consts[n] for n in names]


def tier_ladder_md() -> str:
    """The elite ladder, read out of game.cpp and game.hpp."""
    cpp = (ROOT / "src" / "game" / "game.cpp").read_text(encoding="utf-8")
    hpp = (ROOT / "src" / "game" / "game.hpp").read_text(encoding="utf-8")
    k = cpp_consts(cpp)
    g = cpp_consts(hpp)

    elite_min, elite_max, champ_min, champ_max, lord_min, lord_max = need(
        k, "kEliteHpMin", "kEliteHpMax", "kChampionHpMin", "kChampionHpMax",
        "kOverlordHpMin", "kOverlordHpMax")
    champ_slope, champ_cap = need(k, "kChampionChanceSlope", "kChampionChanceCap")
    lord_slope, lord_cap = need(k, "kOverlordChanceSlope", "kOverlordChanceCap")
    champ_gate, lord_gate = need(g, "kChampionPressure", "kOverlordPressure")
    elite_time, champ_time, lord_time = need(
        g, "kEliteMinTime", "kChampionMinTime", "kOverlordMinTime")
    # The elite cap is a bare literal in tierSpawnCap's switch, because it is the
    # end of a curve rather than a knob of its own. Read it the same way, and fail
    # the same way, rather than typing the number here and forgetting it.
    elite_cap = re.search(r"case\s+1\s*:\s*return\s+([0-9.]+)F\s*;", cpp)
    if elite_cap is None:
        raise SystemExit(
            "tools/gendocs.py: could not read the elite spawn cap out of "
            "tierSpawnCap in game.cpp. Update tier_ladder_md."
        )
    live_cap, grace = need(g, "kLiveTierCap", "kTierGrace")

    # Touch / speed / XP come from the tierBuffs switch, one row per tier.
    rows = re.findall(
        r"case\s+([123])\s*:\s*return\s*\{[^,]+,[^,]+,\s*([0-9.]+)F,\s*"
        r"([0-9.]+)F,\s*([0-9.]+)F\s*\};",
        cpp,
    )
    buffs = {int(t): (float(touch), float(spd), float(xp))
             for t, touch, spd, xp in rows}
    if set(buffs) != {1, 2, 3}:
        raise SystemExit(
            "tools/gendocs.py: could not read all three tierBuffs rows out of "
            f"game.cpp (got {sorted(buffs)}). Update tier_ladder_md."
        )

    def g(x: float) -> str:
        """Compact float for prose: 0.025 stays 0.025, 3.0 reads as 3."""
        return f"{x:g}"

    def mm(t: float) -> str:
        return f"{int(t // 60)}:{int(t % 60):02d}"

    names = {1: "Elite", 2: "Champion", 3: "Overlord"}
    hp = {1: (elite_min, elite_max), 2: (champ_min, champ_max),
          3: (lord_min, lord_max)}
    out = [
        "An elite and above is a trash mob with a body count problem. Its stats are",
        "a multiplier on whatever trash archetype it rolled, so a champion is always",
        "the same fight wearing a different body. HP is a range, rolled per spawn.",
        "",
        "| Tier | HP x trash | Touch | Speed | XP | Opens at | Gate | Rare at best |",
        "|------|-----------|-------|-------|----|----------|------|--------------|",
    ]
    for tier in (1, 2, 3):
        lo, hi = hp[tier]
        touch, spd, xp = buffs[tier]
        if tier == 1:
            opens = mm(elite_time)
            gate = "the elite rate rises with the clock"
            rare = f"{g(float(elite_cap.group(1)) * 100)}% of spawns"
        elif tier == 2:
            opens = mm(champ_time)
            gate = f"tier-1 pressure {g(champ_gate)}"
            rare = f"{g(champ_cap * 100)}% of spawns"
        else:
            opens = mm(lord_time)
            gate = f"tier-2 pressure {g(lord_gate)}"
            rare = f"{g(lord_cap * 100)}% of spawns"
        out.append(
            f"| {names[tier]} | {g(lo)}-{g(hi)} | x{g(touch)} | x{g(spd)} | x{g(xp)} "
            f"| {opens} | {gate} | {rare} |"
        )
    out += [
        "",
        f"At most **{int(live_cap)}** of a tier are alive at once, and a promotion",
        f"is followed by {g(grace)}s of grace, so a fresh {names[2].lower()} never lands",
        "on top of the one that just died.",
        "",
        "**The frequency is the balance here, not the HP.** A champion is meant to",
        "be an event. Its chance of any given spawn is",
        f"`min({g(champ_cap * 100)}%, (pressure - {g(champ_gate)}) * {g(champ_slope)})`,",
        "so a player who has only just earned one gets a trickle, and a build far",
        "past the gate still tops out at one in forty. An overlord is the run's boss:",
        f"`min({g(lord_cap * 100)}%, (pressure - {g(lord_gate)}) * {g(lord_slope)})`,",
        "a couple in a ten-minute run, each one a fight the player had to make room",
        "for.",
        "",
        "A floor matters as much as a ceiling: capping a boss at",
        f"{g(champ_cap * 100)}% only means something if the garbage below it is more",
        "common than that, which is what the elite rate is for.",
        "",
        "The XP multiplier is deliberately NOT scaled down with the HP. An elite is a",
        "reward before it is a threat, and if killing one is worse value than killing",
        "three pieces of trash then the player is right to ignore it -- which is",
        "exactly how a promotion stops being a promotion.",
    ]
    return "\n".join(out)


# Keys every weapon has, and which therefore have their own column. Everything
# else a weapon declares is the rule that makes it that weapon, and goes in the
# Rule column instead of a column per parameter.
COMMON_KEYS = {
    "id", "name", "desc", "attack_type", "requires", "starter",
    "damage", "cooldown", "projectiles", "pierce", "spread",
    "proj_speed", "proj_life", "proj_color",
}

# Flags worth naming rather than printing as 1.
FLAG_KEYS = {
    "homing": "homing",
    "bounce_infinite": "eternal",
    "bomb_on_target": "lands on target",
    "zone_from_above": "falls from above",
    "inferno_binds_to_lure": "binds to the bell",
}


def rule_params(w: dict) -> str:
    """Everything that makes this weapon this weapon, as `key value` pairs.

    Enumerated from the data rather than from a hand-written list per attack
    type. The previous version of this table had a fixed column set -- `area`,
    `bounces`, `strength` -- and three of those keys no longer exist in
    weapons.toml, so every weapon in the document showed an empty Traits cell and
    the parameters that actually define a Nova or a Lure were not printed at all.
    A generated column set cannot rot the way a written one does: a new parameter
    appears in the document the day it is added, and nothing is silently blank.
    """
    bits = []
    for key in sorted(w):
        if key in COMMON_KEYS:
            continue
        val = w[key]
        if key in FLAG_KEYS:
            if val:
                bits.append(FLAG_KEYS[key])
            continue
        if isinstance(val, bool):
            if val:
                bits.append(FLAG_KEYS.get(key, key))
            continue
        if isinstance(val, (int, float)) and val == 0:
            continue
        bits.append(f"{key} {fmt(val)}")
    return ", ".join(bits)


def difficulty_md() -> str:
    """The three difficulty ramps, read out of Game.

    `docs/mechanics.md` used to spell these out in a hand-written block, and
    every number in it was stale: a 7-minute knee that is at 8, a /95 HP leg that
    is /145, a 420s knee that is 480, a cap of 22 that is 20. A prose document
    restating a formula has no way to notice the formula moved, which is why this
    section is generated and the prose now points at it.
    """
    cpp = (ROOT / "src" / "game" / "game.cpp").read_text(encoding="utf-8")

    def body(sig: str, text: str) -> str:
        """A whole function, matched by brace balance rather than by regex.

        "Everything up to the first closing brace" is not a function body. It
        stops at the end of the first `if` block, which is how a reader can find
        a signature and still miss every number it was sent to look for -- and a
        reader that misses is worse than one that raises, because it raises.
        """
        # Matched as `sig(` and not as a bare prefix. "spawnWave" is a prefix of
        # "spawnWaveCrescent", so a prefix search finds the wrong function, reads
        # a body that does not contain the numbers it was sent for, and reports
        # them missing -- which at least fails loudly, but only by accident.
        at = text.find(sig + "(")
        if at < 0:
            raise SystemExit(
                f"tools/gendocs.py: could not find {sig} to read the difficulty "
                "ramp out of. Update difficulty_md."
            )
        start = text.index("{", at)
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[at:i + 1]
        raise SystemExit(f"tools/gendocs.py: unbalanced braces after {sig}.")

    scales = body("void Game::currentScales", cpp)
    spawn = body("void Game::spawnWave", cpp)

    def grab(pattern: str, text: str, what: str) -> float:
        """One number out of the source, named, or a loud failure.

        A bare `re.search(...).group(1)` turns a deleted line into an
        AttributeError three frames deep, which is a worse report than "I could
        not find the HP cap in currentScales" and gets the same fix.
        """
        m = re.search(pattern, text)
        if m is None:
            raise SystemExit(
                f"tools/gendocs.py: could not read {what} out of the source. The "
                "difficulty ramp moved; update difficulty_md so this document "
                "fails here instead of printing a wrong number."
            )
        return float(m.group(1))

    knee = grab(r"kHpKnee = ([0-9.]+)F", scales, "the HP knee")
    hp_leg = grab(r"hp = 1\.0F \+ simTime_ / ([0-9.]+)F", scales, "the shallow HP leg")
    sp_leg = grab(r"speed = 1\.0F \+ simTime_ / ([0-9.]+)F", scales,
                  "the shallow speed leg")
    hp_after = grab(r"\(simTime_ - kHpKnee\) / ([0-9.]+)F", scales,
                    "the steepened HP leg")
    sp_after = grab(
        r"speed = 1\.0F \+ kHpKnee / [0-9.]+F \+ \(simTime_ - kHpKnee\) / ([0-9.]+)F",
        scales, "the steepened speed leg")
    hp_cap = grab(r"hp = std::min\(hp, ([0-9.]+)F\)", scales, "the HP cap")
    sp_cap = grab(r"speed = std::min\(speed, ([0-9.]+)F\)", scales, "the speed cap")
    touch_leg = grab(r"touch = 1\.0F \+ simTime_ / ([0-9.]+)F", scales,
                     "the contact-damage leg")

    def mm(t: float) -> str:
        return f"{int(t // 60)}:{int(t % 60):02d}"

    def g(x: float) -> str:
        return f"{x:g}"

    # The three legs, read as the if/else chain that writes them. Capturing the
    # formula without the condition that selects it is how a table ends up
    # saying the second leg starts at 0:30 when it starts at 2:30 -- the offset
    # inside the formula is not the bound above it, and they are different
    # numbers that happen to look alike.
    legs: list[tuple[float | None, str]] = []
    cond: float | None = None
    for line in spawn.splitlines():
        m = re.match(r"\s*(?:\}\s*else\s+)?if \(simTime_ < ([0-9.]+)F\)", line)
        if m:
            cond = float(m.group(1))
            continue
        if re.match(r"\s*\}\s*else\s*\{", line):
            cond = None
            continue
        m = re.match(r"\s*spawnTimer_ = (.+);", line)
        if m:
            legs.append((cond, m.group(1).strip()))
    if len(legs) != 3 or legs[0][0] is None or legs[1][0] is None or legs[2][0] is not None:
        raise SystemExit(
            f"tools/gendocs.py: expected three spawn-interval legs with bounds "
            f"on the first two, got {legs}. Update difficulty_md."
        )

    def leg(label: str, expr: str) -> str:
        """One leg of the interval curve, as the line the source writes."""
        flat = re.fullmatch(r"([0-9.]+)F - simTime_ \* ([0-9.]+)F", expr)
        if flat:
            return (f"{label} : interval = {g(float(flat.group(1)))}"
                    f" - t*{g(float(flat.group(2)))}")
        ramped = re.fullmatch(
            r"std::max\(([0-9.]+)F, ([0-9.]+)F - \(simTime_ - ([0-9.]+)F\) \* ([0-9.]+)F\)",
            expr)
        if ramped:
            floor, at, from_, slope = (float(ramped.group(i)) for i in range(1, 5))
            return (f"{label} : interval = max({g(floor)}, {g(at)} - "
                    f"(t-{mm(from_)})*{g(slope)})")
        raise SystemExit(
            f"tools/gendocs.py: do not recognise the spawn-interval leg {expr!r}. "
            "Update difficulty_md."
        )

    # A leg is a RANGE. The condition above it says where the leg stops; the
    # offset inside the formula says where it starts, and those are different
    # numbers -- 2:30 and 0:30 here -- that look alike enough to be swapped
    # without noticing. So the label is built from the bounds in order.
    b0, b1 = float(legs[0][0]), float(legs[1][0])
    # Padded to one width so the three lines line up under each other in a code
    # block. A generated table with ragged edges reads as a hand-written one.
    raw = [f"t < {mm(b0)}", f"{mm(b0)}-{mm(b1)}", f"t >= {mm(b1)}"]
    labels = [r.ljust(max(len(x) for x in raw)) for r in raw]

    out = [
        "Three things rise over a run: how often a pack arrives, how big the",
        "enemies are, and how hard they hit. The first is a three-leg curve, the",
        "other two are a shallow leg that steepens at a knee.",
        "",
        "### Pack interval",
        "",
        "```text",
        leg(labels[0], legs[0][1]),
        leg(labels[1], legs[1][1]),
        leg(labels[2], legs[2][1]),
    ]
    floor = grab(r"std::max\(([0-9.]+)F", legs[2][1], "the spawn-interval floor")
    out += [
        "```",
        "",
        f"The floor is {g(floor)}s -- about {1 / floor:.0f} packs a second, and a",
        "pack is several bodies by the time the run gets there. At 0.12s the game",
        "threw around thirty enemies a second at the player, which no build can",
        "answer and which just ends the run early.",
        "",
        "The third leg used to start at 1:30 and bottom out at 0.20s, so the run",
        "went from one pack a second to five inside the two minutes where the heavy",
        "enemies were also arriving. Moving the leg later AND raising the floor is",
        "the same fix from both ends: the screen fills later, and the things in it",
        "can be killed when it does.",
        "",
        "### Enemy stats",
        "",
        "| Stat | Shallow leg | After the knee | Cap |",
        "|------|-------------|----------------|-----|",
        f"| HP | `1 + t/{g(hp_leg)}` | `+ (t-{g(knee)})/{g(hp_after)}` | x{g(hp_cap)} |",
        f"| Speed | `1 + t/{g(sp_leg)}` | `+ (t-{g(knee)})/{g(sp_after)}` | x{g(sp_cap)} |",
        f"| Contact | `1 + t/{g(touch_leg)}` | -- | uncapped |",
        "",
        f"The knee is at {mm(knee)}. Every number here is lower than it was and the",
        "knee is a minute later, for the same reason the XP curve is: **the HP ramp",
        "is the one piece of difficulty the player has no answer to.** A build three",
        "picks behind cannot outshoot a doubled enemy, so the ramp has to stop",
        "outrunning the build.",
        "",
        "The shape is intact -- a rising ramp that steepens -- it just stops being",
        "the whole difficulty. The heavies are now spread across the run (see",
        "`enemies.toml`), so the early leg does not have to carry all of it: at 2:30",
        "an ordinary enemy is barely doubled and the threat is whatever has actually",
        "walked in.",
    ]
    return "\n".join(out)


def chests_md() -> str:
    """The chest reward, read out of Game::spawnChest.

    Same reasoning as the tier ladder: these are three literals in a function, and
    they are the whole reason an elite corpse is worth walking over. Hand-typing
    them into a table is how the document ends up promising three cards for a
    champion while the game hands out one.
    """
    cpp = (ROOT / "src" / "game" / "game.cpp").read_text(encoding="utf-8")
    body = re.search(r"entt::entity Game::spawnChest\(.*?\n\}", cpp, re.S)
    if body is None:
        raise SystemExit("tools/gendocs.py: could not find Game::spawnChest in game.cpp")
    text = body.group(0)
    # The condition travels with the number. Matching `base = N;` on its own
    # looks like it works -- it reads `int base = 1` as the elite, then reads
    # `if (tier >= 3) base = 5` as another unconditioned default and quietly
    # overwrites the elite with five cards. Which is exactly what it did, the
    # first time, in a document nobody would have read twice.
    grants: dict[int, int] = {}
    for m in re.finditer(
        r"int base = (\d+);|if \(tier (?:==|>=) (\d+)\) base = (\d+);", text
    ):
        if m.group(2) is None:
            grants[1] = int(m.group(1))
        else:
            grants[int(m.group(2))] = int(m.group(3))
    if set(grants) != {1, 2, 3}:
        raise SystemExit(
            "tools/gendocs.py: expected a card count for tiers 1, 2 and 3 out of "
            f"spawnChest, got {sorted(grants)}. Update chests_md."
        )
    rows = [
        ["Elite", str(grants[1]), "One card. The small change."],
        ["Champion", str(grants[2]), "A whole hand."],
        ["Overlord", str(grants[3]), "Most of the arsenal in one pickup."],
    ]
    return (
        "A tiered enemy drops a chest where it died. Walk over it: a chest is a\n"
        "pickup, never a button. What is inside is cards that improve the weapons\n"
        "you already own and the build around them -- never a new weapon, and never\n"
        "a level-up, because a corpse must not be able to ask you a question you\n"
        "have to answer. The box leans on whichever weapon it has touched least, so\n"
        "a hand never lands four times on the same gun.\n"
        "\n"
        "| Dropped by | Cards | What it is |\n"
        "|------------|-------|------------|\n"
        + "\n".join(f"| {a} | {b} | {c} |" for a, b, c in rows)
        + "\n\n`Deep Cache` adds one more card to every box in the game. The count is\n"
        "deliberately NOT clamped to the size of the arsenal: clamping here would\n"
        "quietly downgrade a champion's gift to an elite's for every early run, and\n"
        "the early run is exactly when a champion is hardest to kill and the box is\n"
        "most worth having.\n"
    )


def weapons_md() -> str:
    ws = load("weapons.toml")
    base = [w for w in ws if not w.get("requires")]
    evo = [w for w in ws if len(w.get("requires", [])) == 2]
    sup = [w for w in ws if len(w.get("requires", [])) >= 3]

    heads = ["Name", "ID", "Attack", "Damage", "Cooldown (s)", "Projectiles",
             "Pierce", "Starter", "Rule", "Description"]

    def _w(w):
        return [w["name"], f"`{w['id']}`", w["attack_type"], fmt(w["damage"]),
                fmt(w["cooldown"]), fmt(w["projectiles"]), fmt(w["pierce"]),
                "yes" if w.get("starter") else "", rule_params(w),
                md_escape(w["desc"])]

    s = "### Base weapons\n\n"
    s += table(heads, [_w(w) for w in base])

    evo_heads = ["Name", "ID", "Attack", "Requires", "Damage", "Cooldown (s)",
                 "Projectiles", "Pierce", "Rule", "Description"]

    def _e(w):
        return [w["name"], f"`{w['id']}`", w["attack_type"],
                " + ".join(f"`{r}`" for r in w["requires"]), fmt(w["damage"]),
                fmt(w["cooldown"]), fmt(w["projectiles"]), fmt(w["pierce"]),
                rule_params(w), md_escape(w["desc"])]

    s += "\n\n### Evolutions (A + B = C)\n\n"
    s += table(evo_heads, [_e(w) for w in evo])
    s += "\n\n### Super evolutions (A + B + C)\n\n"
    s += table(evo_heads, [_e(w) for w in sup])
    return s


def enemies_md() -> str:
    es = load("enemies.toml")
    def _at(t: float) -> str:
        t = float(t)
        return f"{int(t // 60)}:{int(t % 60):02d}" if t >= 60 else f"{t:g}s"

    rows = [[e["name"], f"`{e['id']}`", fmt(e["hp"]), fmt(e["speed"]),
             fmt(e.get("speed_ramp", 0.0)), "yes" if e.get("fast") else "",
             fmt(e["touch"]), fmt(e["radius"]), fmt(e["xp"]), _at(e["unlock_at"]),
             fmt(e["weight"]), e["shape"]] for e in es]
    return table(["Name", "ID", "HP", "Speed", "Speed ramp", "Fast", "Touch dmg",
                  "Radius", "XP", "Unlocks at", "Weight", "Shape"], rows)


def upgrades_md() -> str:
    us = load("upgrades.toml")
    kinds = [("normal", "### Normal pool"),
             ("unique", "### Unique items (one-time, rule-changing)"),
             ("milestone", "### Milestones (every power-of-two level from 4 on)")]
    parts = []
    for kind, heading in kinds:
        rows = []
        for u in us:
            if u.get("kind", "normal") != kind:
                continue
            rows.append([u["name"], f"`{u['id']}`", u["effect"], fmt(u["value"]),
                         fmt(u.get("max_stacks", 1)), u.get("weapon", ""),
                         str(u.get("level", "")), u.get("group", ""),
                         md_escape(u["desc"])])
        if not rows:
            continue
        parts.append(heading + "\n\n" +
                     table(["Name", "ID", "Effect", "Value", "Max stacks",
                            "Weapon", "Level", "Group", "Description"], rows))
    s = "\n\n".join(parts)

    # The groups, spelled out. A milestone group is the design: every member is
    # on the same screen and taking one closes the rest for the run, so the group
    # column above is unreadable without knowing which cards share one.
    groups: dict[str, list[str]] = {}
    for u in us:
        if u.get("kind") == "milestone" and u.get("group"):
            groups.setdefault(u["group"], []).append(u["name"])
    if groups:
        s += "\n\n### Milestone groups (mutually exclusive for the run)\n\n"
        s += table(["Group", "Members", "N"],
                   [[k, ", ".join(sorted(v)), str(len(v))]
                    for k, v in sorted(groups.items())])

    # summary counts
    counts = {}
    for u in us:
        counts[u.get("kind", "normal")] = counts.get(u.get("kind", "normal"), 0) + 1
    summary = (f"\n\n**Totals:** {len(us)} upgrades "
               f"({counts.get('normal', 0)} normal, "
               f"{counts.get('unique', 0)} unique, "
               f"{counts.get('milestone', 0)} milestones).\n")
    return s + summary


def manual_md() -> str:
    """The in-game manual, rendered as markdown.

    `manual.toml` is the single source of truth: what the game shows on F1 is
    what this prints, so the in-game docs and the repo docs cannot drift apart
    without the generator showing it.
    """
    with open(DATA / "manual.toml", "rb") as f:
        pages = tomllib.load(f)["page"]
    listed = "\n".join(f"- `{p['id']}` — {p['title']} "
                       f"({len(p['lines'])} lines)" for p in pages)
    parts = []
    for p in pages:
        body = "\n".join("    " + line if line else "" for line in p["lines"])
        parts.append(f"### {p['title']}\n\n```\n{body}\n```")
    return (f"Shown in the game with **F1** (main menu, live run, or the pause "
            f"screen). **{len(pages)} pages:**\n\n{listed}\n\n"
            + "\n\n".join(parts))


def main() -> None:
    ws, es, us = load("weapons.toml"), load("enemies.toml"), load("upgrades.toml")
    with open(DATA / "manual.toml", "rb") as f:
        pages = tomllib.load(f)["page"]
    doc = f"""<!-- GENERATED FILE — do not edit by hand.
     Source: assets/data/*.toml, generator: tools/gendocs.py -->

# Content Reference

All numbers below are read directly from the TOML files in `assets/data/`.
Regenerate with:

```sh
python3 tools/gendocs.py
```

**Totals:** {len(ws)} weapons ({len([w for w in ws if not w.get('requires')])} base +
{len([w for w in ws if len(w.get('requires', [])) == 2])} evolutions +
{len([w for w in ws if len(w.get('requires', [])) >= 3])} super evolutions),
{len(es)} enemies, {len(pages)} in-game manual pages.

{weapons_md()}

---

## Enemies

{enemies_md()}

---

## Difficulty ramp

{difficulty_md()}

---

## The elite ladder

{tier_ladder_md()}

---

## Chests

{chests_md()}

---

## Upgrades

{upgrades_md()}

---

### Upgrade kinds

| Kind       | When it is offered                                                     |
|------------|-----------------------------------------------------------------------|
| normal     | Any level-up, subject to `max_stacks`                                  |
| unique     | One-time, violet card. Either global, or scoped to one weapon          |
| milestone  | Only on power-of-two levels (4, 8, 16, ...); replaces the whole choice |

There is no separate weapon kind. A weapon card is a card with a `Weapon` set: it
is only drawn while that weapon is equipped, and it edits that slot alone. Every
weapon in the game has at least two of them, so no weapon is a dead slot -- see
`No weapon in the game is a dead slot` in `tests/test_game.cpp`, which fails the
build if a weapon is added with fewer.

---

## In-game manual

{manual_md()}
"""
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(doc, encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} "
          f"({len(ws)} weapons, {len(es)} enemies, {len(us)} upgrades, "
          f"{len(pages)} manual pages)")


if __name__ == "__main__":
    sys.exit(main())
