#!/usr/bin/env python3
"""Generate docs/content.md from assets/data/*.toml.

Run from the repo root (or anywhere):

    python3 tools/gendocs.py

The tables are fully generated — edit the TOML, not the markdown.
"""

from __future__ import annotations

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


def weapons_md() -> str:
    ws = load("weapons.toml")
    base = [w for w in ws if not w.get("requires")]
    evo = [w for w in ws if w.get("requires")]

    def _w(w):
        extra = []
        if w.get("area", 0.0) > 0.0: extra.append("splash")
        if w.get("homing"): extra.append("homing")
        if w.get("bounces", 0) > 0: extra.append("bounce" + str(w["bounces"]))
        if w.get("strength", 0.0) > 0.0: extra.append("knock")
        return " ".join(extra)
    rows = [[w["name"], w["id"], fmt(w["damage"]), fmt(w["cooldown"]),
             fmt(w["projectiles"]), fmt(w["proj_speed"]), fmt(w["pierce"]),
             fmt(w.get("spread", 0.16)),
             "yes" if w.get("starter") else "", _w(w), md_escape(w["desc"])]
            for w in base]
    s = "### Base weapons\n\n"
    s += table(["Name", "ID", "Damage", "Cooldown (s)", "Projectiles",
                "Proj. speed", "Pierce", "Spread (rad)", "Starter",
                "Traits", "Description"], rows)

    rows = [[w["name"], w["id"], " + ".join(w["requires"]), fmt(w["damage"]),
             fmt(w["cooldown"]), fmt(w["projectiles"]), fmt(w["pierce"]),
             md_escape(w["desc"])] for w in evo]
    s += "\n\n### Evolutions (A + B = C)\n\n"
    s += table(["Name", "ID", "Requires", "Damage", "Cooldown (s)",
                "Projectiles", "Pierce", "Area", "Description"], rows)
    return s


def enemies_md() -> str:
    es = load("enemies.toml")
    rows = [[e["name"], e["id"], fmt(e["hp"]), fmt(e["speed"]), fmt(e["touch"]),
             fmt(e["radius"]), fmt(e["xp"]), fmt(e["unlock_at"]),
             fmt(e["weight"]), e["shape"]] for e in es]
    return table(["Name", "ID", "HP", "Speed", "Touch dmg", "Radius", "XP",
                  "Unlocks at (s)", "Weight", "Shape"], rows)


def upgrades_md() -> str:
    us = load("upgrades.toml")
    kinds = [("normal", "### Normal pool"),
             ("unique", "### Unique items (one-time, rule-changing)"),
             ("milestone", "### Milestones (every 5th level)")]
    parts = []
    for kind, heading in kinds:
        rows = []
        for u in us:
            if u.get("kind", "normal") != kind:
                continue
            rows.append([u["name"], u["id"], u["effect"], fmt(u["value"]),
                         fmt(u.get("max_stacks", 1)), u.get("weapon", ""),
                         str(u.get("level", "")), md_escape(u["desc"])])
        if not rows:
            continue
        parts.append(heading + "\n\n" +
                     table(["Name", "ID", "Effect", "Value", "Max stacks",
                            "Weapon", "Level", "Description"], rows))
    s = "\n\n".join(parts)

    # summary counts
    counts = {}
    for u in us:
        counts[u.get("kind", "normal")] = counts.get(u.get("kind", "normal"), 0) + 1
    summary = (f"\n\n**Totals:** {len(us)} upgrades "
               f"({counts.get('normal', 0)} normal, "
               f"{counts.get('unique', 0)} unique, "
               f"{counts.get('milestone', 0)} milestones).\n")
    return s + summary


def main() -> None:
    ws, es, us = load("weapons.toml"), load("enemies.toml"), load("upgrades.toml")
    doc = f"""<!-- GENERATED FILE — do not edit by hand.
     Source: assets/data/*.toml, generator: tools/gendocs.py -->

# Content Reference

All numbers below are read directly from the TOML files in `assets/data/`.
Regenerate with:

```sh
python3 tools/gendocs.py
```

**Totals:** {len(ws)} weapons ({len([w for w in ws if not w.get('requires')])} base +
{len([w for w in ws if w.get('requires')])} evolutions), {len(es)} enemies.

{weapons_md()}

---

## Enemies

{enemies_md()}

---

## Upgrades

{upgrades_md()}

---

### Upgrade kinds

| Kind       | When it is offered                                        |
|------------|-----------------------------------------------------------|
| normal     | Any level-up, subject to `max_stacks`                     |
| unique     | ~45% chance per level-up, one-time, violet card           |
| milestone  | Only on levels divisible by 5; separate 3-card pick of 2  |
| weapon     | Only while the named weapon is owned; buffs that slot     |
"""
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(doc, encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} "
          f"({len(ws)} weapons, {len(es)} enemies, {len(us)} upgrades)")


if __name__ == "__main__":
    sys.exit(main())
