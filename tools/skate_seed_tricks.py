#!/usr/bin/env python3
"""Seed skate/skate_tricks.json for the skateboard mod (SKATE_NOTES.md).

Writes one row per playable character per aerial with the per-aerial default trick
(nair shove-it, fair kickflip, bair heelflip, uair impossible, dair stomp), marked
"source": "default", plus the spec's hand-set reference: Falco forward air, a one-rotation
varial whose direction the game learns from Falco's own spin ("direction": "auto").

Default rows are placeholders. In game, the bone-sampling generator replaces each one the first
time that aerial is used (with the mod on) and F8 saves the result back into this file. Rows
marked "hand" are never overwritten, so hand edits survive.

    python tools/skate_seed_tricks.py [--out skate/skate_tricks.json] [--force]
"""
import argparse
import json
import os
import sys

# FighterKind order (decomp melee/ft/forward.h). Popo and Nana are both listed: each is a fighter.
CHARACTERS = [
    "Mario", "Fox", "Captain Falcon", "Donkey Kong", "Kirby", "Bowser", "Link", "Sheik", "Ness",
    "Peach", "Popo", "Nana", "Pikachu", "Samus", "Yoshi", "Jigglypuff", "Mewtwo", "Luigi", "Marth",
    "Zelda", "Young Link", "Dr. Mario", "Falco", "Pichu", "Mr. Game & Watch", "Ganondorf", "Roy",
]
DEFAULTS = {
    "nair": ("shoveit", 0.5),
    "fair": ("kickflip", 1),
    "bair": ("heelflip", 1),
    "uair": ("impossible", 1),
    "dair": ("stomp", 1),
}


def rows():
    out = []
    for name in CHARACTERS:
        for aerial, (trick, turns) in DEFAULTS.items():
            row = {"character": name, "aerial": aerial, "trick": trick, "direction": 1,
                   "rotations": turns, "source": "default"}
            if name == "Falco" and aerial == "fair":
                row.update(trick="varial", rotations=1, direction="auto", source="hand")
            out.append(row)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join("skate", "skate_tricks.json"))
    ap.add_argument("--force", action="store_true", help="overwrite a table that already has learned rows")
    args = ap.parse_args()
    if os.path.exists(args.out) and not args.force:
        with open(args.out, encoding="utf-8") as f:
            existing = json.load(f)
        if any(r.get("source") == "auto" for r in existing.get("rows", [])):
            print("%s already has learned rows; pass --force to replace them" % args.out, file=sys.stderr)
            return 1
    doc = {
        "_comment": "Board trick per character per aerial. source: hand rows are never overwritten; auto rows come "
                    "from sampling the character's bones in game; default rows are placeholders. F5 reloads, F8 saves.",
        "version": 1,
        "rows": rows(),
    }
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print("wrote %d rows to %s" % (len(doc["rows"]), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
