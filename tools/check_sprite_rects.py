#!/usr/bin/env python3
"""Find multi-tile sprite rectangles that cut through the middle of the artwork.

`BIG_MANIFEST` in `tools/build_atlas.py` names a sprite by a rectangle of tiles. Get the rectangle
wrong and the failure is quiet: the sprite still draws, still lines up on the grid, still looks
like a tree -- it is just a tree with its outer branches sliced off. Nothing in the build
complains, and a per-tile contact sheet will never show it, because the error is not in any tile.

The test is a silhouette one and needs no knowledge of what the sprite depicts. A rectangle that
contains a whole sprite has transparency at its border, because that is what makes it a sprite and
not a tile. A rectangle that severs one has opaque pixels running off the edge. So: count opaque
pixels along each border of the crop, as a fraction of that border's length.

"Opaque" alone is not enough, and the first version of this tool flagged nine of seventeen
rectangles because of it. Sprites are packed touching each other on a tilesheet, so a correct
rectangle butted against its neighbour also has an opaque border -- the neighbour's edge, not its
own. Every sprite in this pack is drawn with a 1px `141b1b` outline (the same rule the building
generator and the AI import pass enforce), so the two cases are distinguishable:

    border pixel is the outline colour  -> this is the sprite's own edge, the rectangle is right
    border pixel is an interior colour  -> the rectangle cut through the middle of the artwork

The reported score is the fraction of the border that is opaque AND not outline. Buildings, which
are solid blocks bounded by their outline, drop to near zero. The trees do not.

Reading the numbers:

  * A high LEFT and RIGHT score together is the signature of a crop that is too narrow -- the
    sprite continues past both sides. This is what found the trees.
  * A high BOTTOM score alone is usually fine and often correct. Ground-contact sprites (a tree's
    roots, a building's foundation) are drawn running to the bottom edge so they seat against the
    terrain instead of hovering.
  * A high TOP score means the sprite continues upward, which is almost always wrong.

Confirmed finding, left here as the worked example:

    TreeBroad  NTree (5,2) 2x3  -> left 79%, right 79%   SEVERED
    TreePine   NTree (1,2) 2x3  -> left 69%, right 69%   SEVERED

  Both are the middle two columns of a **four**-tile-wide tree. TilesetNature draws the broad tree
  across cols 4..7 and the pine across cols 0..3, each with a canopy of three lobes and a root
  band spanning the full width. Taking the middle two columns keeps the central lobe and the trunk
  and discards the left and right lobes, which is why a wood built from them reads as a palisade
  of identical narrow columns rather than as a canopy. The corrected rectangles score 19% and 8%,
  and what remains is the root band, which is supposed to reach the edge.

Usage:
    python3 tools/check_sprite_rects.py                 # audit BIG_MANIFEST
    python3 tools/check_sprite_rects.py --rect NTree 4 2 4 3
"""

from __future__ import annotations

import argparse
import pathlib
import re

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
PACKER = ROOT / "tools" / "build_atlas.py"

# Anything at or above this fraction of a border being cut is called out.
SEVER = 0.35

# The pack's silhouette outline. A border pixel this colour is the sprite's own edge.
OUTLINE = (0x14, 0x1B, 0x1B)
# Tolerance, because a few sprites outline in a near-black rather than exactly this one.
OUTLINE_TOL = 48


def is_outline(p) -> bool:
    return (
        abs(p[0] - OUTLINE[0]) + abs(p[1] - OUTLINE[1]) + abs(p[2] - OUTLINE[2]) <= OUTLINE_TOL
    )


def sheets_and_manifest() -> tuple[dict[str, pathlib.Path], list[tuple]]:
    """Read SHEETS and BIG_MANIFEST out of the packer by text.

    Importing build_atlas would be tidier and is deliberately avoided: importing it runs its
    module-level sheet loading, so this audit would fail for reasons that have nothing to do with
    the rectangles it is auditing. Parsing the two literals keeps the check independent of whether
    the packer currently builds.
    """
    src = PACKER.read_text()

    sheets: dict[str, pathlib.Path] = {}
    block = re.search(r"SHEETS = \{(.*?)\n\}", src, re.S)
    if block:
        for key, path in re.findall(r'"(\w+)":\s*\(SRC\s*/\s*"([^"]+)"', block.group(1)):
            sheets[key] = ROOT / "assets" / "_src" / path

    entries: list[tuple] = []
    block = re.search(r"BIG_MANIFEST = \[(.*?)\n\]", src, re.S)
    if block:
        for m in re.finditer(
            r'\(\s*"(\w+)",\s*"(\w+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\)', block.group(1)
        ):
            n, s, c, r, w, h = m.groups()
            entries.append((n, s, int(c), int(r), int(w), int(h)))
    return sheets, entries


def borders(im: Image.Image, c: int, r: int, w: int, h: int, tile: int = 16) -> dict[str, float]:
    t = im.crop((c * tile, r * tile, (c + w) * tile, (r + h) * tile))
    px = t.load()
    W, H = t.size
    def cut(p) -> bool:
        return p[3] > 0 and not is_outline(p)

    return {
        "left": sum(1 for y in range(H) if cut(px[0, y])) / H,
        "right": sum(1 for y in range(H) if cut(px[W - 1, y])) / H,
        "top": sum(1 for x in range(W) if cut(px[x, 0])) / W,
        "bottom": sum(1 for x in range(W) if cut(px[x, H - 1])) / W,
    }


def verdict(b: dict[str, float]) -> str:
    if b["left"] >= SEVER and b["right"] >= SEVER:
        return "SEVERED - too narrow, sprite continues both sides"
    if b["top"] >= SEVER:
        return "SEVERED - sprite continues above"
    if b["left"] >= SEVER or b["right"] >= SEVER:
        return "suspect - one side runs off"
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rect", nargs=5, metavar=("SHEET", "C", "R", "W", "H"))
    args = ap.parse_args()

    sheets, entries = sheets_and_manifest()
    if not sheets:
        print(f"could not parse SHEETS out of {PACKER}")
        return 2

    if args.rect:
        key, c, r, w, h = args.rect
        entries = [("(--rect)", key, int(c), int(r), int(w), int(h))]

    cache: dict[str, Image.Image] = {}
    print(f"{'sprite':16} {'sheet':10} {'rect':14} {'L':>5} {'R':>5} {'T':>5} {'B':>5}  verdict")
    print("-" * 96)
    bad = 0
    for name, key, c, r, w, h in entries:
        path = sheets.get(key)
        if path is None or not path.exists():
            print(f"{name:16} {key:10} -- sheet not found")
            continue
        if key not in cache:
            cache[key] = Image.open(path).convert("RGBA")
        b = borders(cache[key], c, r, w, h)
        v = verdict(b)
        bad += bool(v)
        print(
            f"{name:16} {key:10} {f'{c},{r} {w}x{h}':14} "
            f"{b['left']:5.0%} {b['right']:5.0%} {b['top']:5.0%} {b['bottom']:5.0%}  {v}"
        )

    print(f"\n{bad} of {len(entries)} rectangles look severed.")
    # Deliberately exits 0 even on findings: a border score is evidence, not proof, and a sprite
    # that legitimately fills its rectangle would make this a build-breaking false alarm.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
