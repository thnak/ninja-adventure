#!/usr/bin/env python3
"""Recover the transition (autotile) sets the Ninja Adventure tilesets already ship.

Why this exists
---------------
`assets/tile_index.json` counts **1113 tiles with role `transition_edge`** and `MANIFEST` in
`tools/build_atlas.py` uses **none of them**. Every terrain in the game is drawn as one flat fill
tile butted against the next, so every grass/dirt boundary is a staircase of axis-aligned 32px
steps. `tools/study_examples.py` measures the consequence: our screenshots pin 12.7% of their
colour boundaries to the tile grid against the pack demos' 6.1%.

The fix is not to draw new art. The art exists -- e.g. TilesetFloor cols 12..19, rows 6..11 is a
complete dirt-on-grass set. What is missing is the **table** saying which of those tiles to place
for a given arrangement of neighbours. This tool derives that table from the pixels instead of
someone reading a tilesheet by eye and typing coordinates, because typing coordinates by eye is
how `BIG_MANIFEST` acquired a market stall it thought was a fire pit.

How it works
------------
A transition tile is defined by what terrain sits in each of its four CORNERS. So:

  1. Sample each tile's four corners (the outer 6x6 block of each, mode colour -- mode not mean,
     because a mean of grass-plus-a-yellow-tuft is a colour that appears nowhere in the tile and
     clusters into its own bogus terrain).
  2. Cluster all corner colours across the sheet into terrain families by a distance threshold.
     Fully transparent corners get the reserved family `void`, which is what makes OVERLAY sets
     work: a set drawn as terrain-A-fading-to-nothing is meant to be blended over a B fill, and
     is strictly better than an opaque A-to-B set because one overlay serves every B.
  3. A tile whose corners use exactly the families {A, B} is a transition tile for the pair
     (A, B). Its 4-bit mask is which corners are A: bit0 TL, bit1 TR, bit2 BL, bit3 BR.
  4. Group by pair, report every pair that covers enough of the 16 masks to be usable.

Mask 0 is "no A anywhere" (a pure B fill) and mask 15 is "A everywhere" (a pure A fill); the 14
in between are the actual edges and corners. A renderer needs all 16 to never show a seam.

Usage
-----
    python3 tools/autotile_fit.py --scan                    # every set in every tileset
    python3 tools/autotile_fit.py --scan --sheet TilesetFloor.png
    python3 tools/autotile_fit.py --emit tools/_autotile.json
    python3 tools/autotile_fit.py --preview PAIR_ID --out DIR   # render the set as a sanity check

Output is data, not code. Nothing in `src/` reads it. Wiring the result into `build_atlas.py` is
a separate, deliberate step.

Known limitation -- always `--preview` a set before trusting it
--------------------------------------------------------------
Several sheets pad their unused area with opaque **white** rather than transparency. White is
therefore an ambiguous family: it is both genuine snow/cloud art AND page padding. So a reported
pair whose B colour is `#ffffff` may be a real snow transition or may be nothing but the edge of
the drawn region against the margin, and the mask count cannot tell them apart. `TilesetFloor:0-2`
and `TilesetFloor:0-4` both report "complete" and both need looking at.

Pairs against `VOID` do not have this problem, and pairs between two saturated terrains
(`TilesetFloor:2-3`, grass to dirt) do not either. That one was previewed and is correct.
"""

from __future__ import annotations

import argparse
import json
import pathlib
from collections import Counter, defaultdict

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
TILESETS = ROOT / "assets" / "_src" / "ninja" / "Backgrounds" / "Tilesets"

TILE = 16
CORNER = 6  # size of the corner sample; 6 of 16 keeps the sample clear of the tile centre

# Two corner colours within this Manhattan distance are the same terrain. Tuned against the pack:
# the grass ramp spans about 70 between its darkest and lightest, and grass-to-dirt is about 200,
# so anything in 90..150 separates them. 110 sits in the middle of that window.
FAMILY_DIST = 110

# A corner this transparent is `void` rather than a colour.
ALPHA_CUT = 128

# Sets whose rectangle has been pinned and whose --preview was looked at. Auto-detection finds
# CANDIDATES; this table records the ones that were checked, the same discipline BIG_MANIFEST uses
# for building rectangles and for the same reason -- a set that is one column too wide is only
# wrong at its edge, and its edge is exactly where it gets used.
#
# `--scan` on the whole sheet reports grass-on-dirt as one 20-column set covering all 14 edge
# masks, and that report is a merge of three side-by-side sets sharing a grass and a brown family.
#
# The rectangle below is no longer a guess. `tools/godot_study.py` reads the author's own Godot 4
# TileSet out of `Godot Project V4.zip`, where terrain membership is stored explicitly per tile,
# and it gives cols 0..10 rows 7..12 -- 56 tiles, terrains named "grass" and "dirt", terrain mode
# 0 (match corners and sides). Comparing what this file derives from pixels against that table
# agrees on 16 of 16 masks. An earlier pass here guessed cols 0..9 rows 7..10, which is the edge
# core but drops the extra fills and variants in col 10 and rows 11..12.
VERIFIED = {
    "grass_dirt": ("TilesetFloor.png", (0, 7, 10, 12)),
}

# Corner bit order. TL=1, TR=2, BL=4, BR=8 -- so a mask reads as a little-endian row-major grid,
# and `mask == 15` is "terrain A fills the tile".
TL, TR, BL, BR = 1, 2, 4, 8


def corner_samples(tile: Image.Image) -> list[tuple | None]:
    """Mode colour of each corner, or None where the corner is transparent."""
    px = tile.load()
    boxes = [(0, 0), (TILE - CORNER, 0), (0, TILE - CORNER), (TILE - CORNER, TILE - CORNER)]
    out: list[tuple | None] = []
    for ox, oy in boxes:
        hist: Counter = Counter()
        clear = 0
        for y in range(oy, oy + CORNER):
            for x in range(ox, ox + CORNER):
                r, g, b, a = px[x, y]
                if a < ALPHA_CUT:
                    clear += 1
                else:
                    hist[(r, g, b)] += 1
        # Majority-transparent means void. A corner that is merely FRINGED with transparency is
        # still art, and taking the mode of what is left is the right answer for it.
        out.append(None if clear > CORNER * CORNER // 2 else hist.most_common(1)[0][0])
    return out


def edge_bits(tile: Image.Image, centres: list[tuple], hi: int) -> dict[str, tuple]:
    """Classify each border pixel of a tile as terrain A (1) or not (0).

    Two tiles may only sit next to each other if the pixels along their shared border agree. This
    is the constraint that makes a tileset a tileset, and it is invisible in a per-tile view.
    """
    px = tile.load()
    n = TILE

    def cls(x: int, y: int) -> int:
        r, g, b, a = px[x, y]
        return 0 if a < ALPHA_CUT else int(family_of((r, g, b), centres) == hi)

    return {
        "top": tuple(cls(x, 0) for x in range(n)),
        "bottom": tuple(cls(x, n - 1) for x in range(n)),
        "left": tuple(cls(0, y) for y in range(n)),
        "right": tuple(cls(n - 1, y) for y in range(n)),
    }


def coherent_pick(
    by_mask: dict[int, list], sigs: dict[tuple[int, int], dict[str, tuple]]
) -> dict[int, list[int]]:
    """Choose ONE tile per mask such that the chosen tiles tile seamlessly with each other.

    Picking at random among a mask's variants is the obvious thing and it is wrong -- it was the
    first version of this tool and the mock render it produced scored WORSE on `grid_lock` than the
    flat-fill renderer it was supposed to beat (0.127 against 0.109). Variants of a mask are only
    interchangeable if they share border pixels, and these do not: TilesetFloor draws several
    wiggle variants of "grass above, dirt below" whose left and right borders cross at different
    heights, so butting two of them together produces a step at the shared edge -- a step that
    lands exactly on a tile boundary, which is the artefact we set out to remove. Random variants
    manufacture the disease.

    In a corner-based set the appearance of a border is fixed by the two corners it runs between:
    the left border by (TL, BL), the top border by (TL, TR), and so on. So take the most common
    border pattern for each corner pair, and then pick, per mask, the tile that best matches those.
    Most common rather than first-found, because the majority pattern is the one the rest of the
    set was drawn to meet.
    """
    HORIZ = {"top": (TL, TR), "bottom": (BL, BR)}
    VERT = {"left": (TL, BL), "right": (TR, BR)}

    votes: dict[tuple, Counter] = defaultdict(Counter)
    for mask, tiles in by_mask.items():
        for c, r in tiles:
            s = sigs.get((c, r))
            if not s:
                continue
            for side, (b1, b2) in {**HORIZ, **VERT}.items():
                axis = "h" if side in HORIZ else "v"
                key = (axis, bool(mask & b1), bool(mask & b2))
                votes[key][s[side]] += 1

    best: dict[tuple, tuple] = {k: v.most_common(1)[0][0] for k, v in votes.items()}

    out: dict[int, list[int]] = {}
    for mask, tiles in by_mask.items():
        scored = []
        for c, r in tiles:
            s = sigs.get((c, r))
            if not s:
                continue
            hits = 0
            for side, (b1, b2) in {**HORIZ, **VERT}.items():
                axis = "h" if side in HORIZ else "v"
                if s[side] == best.get((axis, bool(mask & b1), bool(mask & b2))):
                    hits += 1
            # Tie-break on position so the result is stable across runs.
            scored.append((-hits, r, c))
        if scored:
            scored.sort()
            out[mask] = [scored[0][2], scored[0][1]]
    return out


def build_families(colours: list[tuple]) -> list[tuple]:
    """Greedy single-pass clustering of corner colours into terrain families.

    Greedy rather than k-means because k is unknown, the clusters are far apart relative to their
    spread (that is what a posterised palette gives you), and greedy is deterministic -- k-means
    with a random seed would reshuffle family ids on every run and make the emitted JSON churn.
    Most frequent colour first, so the family centre is a colour the artist actually used a lot,
    not an outlier that happened to be visited first.
    """
    centres: list[tuple] = []
    for c, _ in Counter(colours).most_common():
        if all(
            abs(c[0] - k[0]) + abs(c[1] - k[1]) + abs(c[2] - k[2]) >= FAMILY_DIST for k in centres
        ):
            centres.append(c)
    return centres


def family_of(c: tuple | None, centres: list[tuple]) -> int:
    """Index into `centres`, or -1 for void."""
    if c is None:
        return -1
    best, bd = -1, 1 << 30
    for i, k in enumerate(centres):
        d = abs(c[0] - k[0]) + abs(c[1] - k[1]) + abs(c[2] - k[2])
        if d < bd:
            best, bd = i, d
    return best


def analyse(path: pathlib.Path, rect: tuple[int, int, int, int] | None = None) -> dict:
    """Derive every (A,B) transition set present in one tilesheet.

    `rect` (c0, r0, c1, r1, inclusive) restricts which tiles may form a set, without restricting
    which tiles define the colour families -- families stay whole-sheet so that pinning a region
    does not shift the family indices in the emitted id.

    You need `rect` more often than you would hope. Automatic grouping cannot always tell two sets
    apart: TilesetFloor rows 7..11 hold THREE grass-on-brown sets laid side by side with no gap
    between them, all sharing one grass and one brown family, so neither colour nor adjacency
    separates them and the tool reports one 20-column set with 18 variants for mask 3. Picking at
    random among those variants cuts from one set to another mid-boundary. Pass the rectangle.
    """
    im = Image.open(path).convert("RGBA")
    cols, rows = im.size[0] // TILE, im.size[1] // TILE

    grid: dict[tuple[int, int], list] = {}
    every: list[tuple] = []
    for r in range(rows):
        for c in range(cols):
            s = corner_samples(im.crop((c * TILE, r * TILE, c * TILE + TILE, r * TILE + TILE)))
            grid[(c, r)] = s
            every += [x for x in s if x is not None]
    if not every:
        return {"sheet": path.name, "families": [], "pairs": []}

    centres = build_families(every)

    # tile -> the family of each of its four corners
    fam: dict[tuple[int, int], list[int]] = {
        k: [family_of(x, centres) for x in v] for k, v in grid.items()
    }

    # Bucket tiles by the unordered pair of families they contain.
    pairs: dict[tuple[int, int], dict[int, list]] = defaultdict(lambda: defaultdict(list))
    fills: dict[int, list] = defaultdict(list)
    def inside(c: int, r: int) -> bool:
        return rect is None or (rect[0] <= c <= rect[2] and rect[1] <= r <= rect[3])

    for (c, r), f in fam.items():
        uniq = sorted(set(f))
        if len(uniq) == 1:
            # Fills are collected sheet-wide even under --rect: a set's pure fill is usually drawn
            # just OUTSIDE the transition block, so restricting fills to the rect finds none.
            if uniq[0] != -1:
                fills[uniq[0]].append([c, r])
            continue
        if not inside(c, r):
            continue
        if len(uniq) != 2:
            continue  # three or more terrains in one tile is a decorated object, not a transition
        lo, hi = uniq
        # `hi` is terrain A -- the one whose corners set bits. Choosing the HIGHER family index
        # puts void (-1) in the B role, so an overlay set masks as "how much A is on this tile",
        # which is the orientation a renderer wants.
        mask = 0
        for bit, corner_fam in zip((TL, TR, BL, BR), f):
            if corner_fam == hi:
                mask |= bit
        pairs[(lo, hi)][mask].append([c, r])

    def cluster(tiles: list[tuple[int, int, int]], gap: int = 2) -> list[list[tuple[int, int, int]]]:
        """Split (col, row, mask) tiles into spatially connected groups.

        A colour PAIR is not a SET. One sheet draws the same grass-on-dirt transition several
        times -- different grass shade, different dirt texture, laid out in separate blocks -- and
        bucketing purely by colour family merges all of them. That merge is not cosmetic: it made
        mask 3 report 18 "variants" that are actually one variant from each of several unrelated
        sets, so picking among them at random cuts between sets mid-boundary. Grouping by
        adjacency recovers the blocks the artist actually drew.

        `gap` of 2 rather than 1 because a set can have a hole in it -- a mask the artist did not
        draw, or one this classifier read as three families and skipped -- and a one-tile hole
        must not sever the block in two.
        """
        rest = list(tiles)
        groups = []
        while rest:
            seed_t = rest.pop()
            group = [seed_t]
            grew = True
            while grew:
                grew = False
                for t in list(rest):
                    if any(
                        abs(t[0] - g[0]) <= gap and abs(t[1] - g[1]) <= gap for g in group
                    ):
                        group.append(t)
                        rest.remove(t)
                        grew = True
            groups.append(group)
        return groups

    def local_fills(family: int, block: list[list[int]], margin: int = 2) -> list[list[int]]:
        """Pure fills of `family` that sit NEAR this set's transition tiles.

        Filtering by colour family alone is not enough and the first version of this tool got it
        wrong: a sheet holds several sets that share a family, so "any tile whose corners are all
        grass" collects greens from every set on the page, and the mock renderer drew a meadow
        speckled with grey cobble and tan brick. A fill only belongs to a set if the artist drew
        it next to that set.
        """
        if not block:
            return []
        c0 = min(c for c, _ in block) - margin
        c1 = max(c for c, _ in block) + margin
        r0 = min(r for _, r in block) - margin
        r1 = max(r for _, r in block) + margin
        near = [t for t in fills.get(family, []) if c0 <= t[0] <= c1 and r0 <= t[1] <= r1]

        # A fill must also present the SAME FACE on all four borders, and this is not implied by
        # its four corners matching. The corner test passes for a tile that is dirt at every corner
        # but has grass running along an edge between them -- cols 8..10 of TilesetFloor are full
        # of those. Used as plain ground they put a grass edge on a tile boundary every time they
        # meet a real fill, which is the exact artefact this whole tool exists to remove: it cost
        # 0.020 of grid_lock in the mock render, more than every other refinement combined.
        def uniform(t: list[int]) -> bool:
            tile = im.crop((t[0] * TILE, t[1] * TILE, t[0] * TILE + TILE, t[1] * TILE + TILE))
            e = edge_bits(tile, centres, family)
            return all(all(v) for v in e.values())

        strict = [t for t in near if uniform(t)] or near

        # Fills must also agree with EACH OTHER, not merely with the family centre. FAMILY_DIST is
        # 110 because that is what separates grass from dirt, which leaves plenty of room for two
        # visibly different browns inside one family: #d3865f and #a3754e are both dirt and are 82
        # apart, well over the 40 that reads as an edge. Interleaving them at random paints a hard
        # step on a tile boundary -- the same artefact, arrived at from a different direction, and
        # it undid the border fix entirely until this was added.
        #
        # Keep the largest mutually-agreeing group, so the choice is the shade the set mostly uses.
        def mean_of(t: list[int]) -> tuple:
            tile = im.crop((t[0] * TILE, t[1] * TILE, t[0] * TILE + TILE, t[1] * TILE + TILE))
            px = tile.load()
            n, acc = 0, [0, 0, 0]
            for y in range(TILE):
                for x in range(TILE):
                    p = px[x, y]
                    if p[3] >= ALPHA_CUT:
                        n += 1
                        for i in range(3):
                            acc[i] += p[i]
            return tuple(v // max(1, n) for v in acc)

        means = {tuple(t): mean_of(t) for t in strict}
        groups: list[list[list[int]]] = []
        for t in strict:
            m = means[tuple(t)]
            for g in groups:
                gm = means[tuple(g[0])]
                if abs(m[0] - gm[0]) + abs(m[1] - gm[1]) + abs(m[2] - gm[2]) <= 40:
                    g.append(t)
                    break
            else:
                groups.append([t])
        return max(groups, key=len)[:8] if groups else []

    out_pairs = []
    for (lo, hi), masks in pairs.items():
        flat = [(c, r, m) for m, v in masks.items() for c, r in v]
        for gi, group in enumerate(sorted(cluster(flat), key=lambda g: (min(t[1] for t in g), min(t[0] for t in g)))):
            by_mask: dict[int, list] = defaultdict(list)
            for c, r, m in group:
                by_mask[m].append([c, r])
            covered = len(by_mask)
            if covered < 6:
                continue  # too few distinct arrangements to be a set; probably incidental art
            block = [[c, r] for c, r, _ in group]
            out_pairs.append(
                {
                    "id": f"{path.stem}:{lo}-{hi}#{gi}",
                    "a_family": hi,
                    "b_family": lo,
                    "a_colour": None if hi < 0 else list(centres[hi]),
                    "b_colour": None if lo < 0 else list(centres[lo]),
                    "b_is_void": lo == -1,
                    "block": [
                        min(c for c, _ in block),
                        min(r for _, r in block),
                        max(c for c, _ in block),
                        max(r for _, r in block),
                    ],
                    "masks_covered": covered,
                    "complete": covered >= 14,
                    "tiles": {str(m): v for m, v in sorted(by_mask.items())},
                    # The one to actually render with. `tiles` keeps every candidate for
                    # inspection; `coherent` is the seam-free choice, see coherent_pick.
                    "coherent": {
                        str(m): v
                        for m, v in sorted(
                            coherent_pick(
                                by_mask,
                                {
                                    (c, r): edge_bits(
                                        im.crop(
                                            (c * TILE, r * TILE, c * TILE + TILE, r * TILE + TILE)
                                        ),
                                        centres,
                                        hi,
                                    )
                                    for c, r, _ in group
                                },
                            ).items()
                        )
                    },
                    "a_fills": local_fills(hi, block),
                    "b_fills": local_fills(lo, block),
                }
            )
    out_pairs.sort(key=lambda p: -p["masks_covered"])
    return {
        "sheet": path.name,
        "cols": cols,
        "rows": rows,
        "families": [list(c) for c in centres],
        "pairs": out_pairs,
    }


def preview(sheet: pathlib.Path, pair: dict, out: pathlib.Path, scale: int = 6) -> None:
    """Lay the 16 masks out in mask order so a wrong table is visible at a glance.

    Read it as a 4x4 grid of mask values 0..15. If the table is right, tiles in the same column
    share a left/right edge and tiles in the same row share a top/bottom edge, so the whole sheet
    reads as a continuous shape. If a tile is in the wrong cell it breaks that continuity and you
    can see it without counting bits.
    """
    im = Image.open(sheet).convert("RGBA")
    cell = TILE * scale
    canvas = Image.new("RGBA", (cell * 4, cell * 4), (24, 24, 28, 255))
    for m in range(16):
        got = pair["tiles"].get(str(m))
        if not got:
            continue
        c, r = got[0]
        t = im.crop((c * TILE, r * TILE, c * TILE + TILE, r * TILE + TILE))
        t = t.resize((cell, cell), Image.NEAREST)
        canvas.alpha_composite(t, ((m % 4) * cell, (m // 4) * cell))
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet", help="one sheet name; default is every tileset")
    ap.add_argument("--scan", action="store_true", help="print what was found")
    ap.add_argument("--emit", help="write the full table as JSON")
    ap.add_argument("--preview", help="pair id to render")
    ap.add_argument("--out", default="tools/_study")
    ap.add_argument("--min-masks", type=int, default=6)
    ap.add_argument(
        "--rect",
        type=int,
        nargs=4,
        metavar=("C0", "R0", "C1", "R1"),
        help="restrict to one block of tiles, inclusive; needs --sheet",
    )
    ap.add_argument(
        "--verified", action="store_true", help="emit only the pinned sets in VERIFIED"
    )
    args = ap.parse_args()

    if args.rect and not args.sheet:
        ap.error("--rect needs --sheet")

    # `--preview <name>` where <name> is a VERIFIED key has to resolve against the pinned rect --
    # scanning the raw sheet gives ids like "TilesetFloor:2-3#0" and would never match. Previewing
    # a verified set is the common case (it is how you check one before trusting it), so make the
    # short name work without also having to remember --verified.
    if args.preview in VERIFIED:
        args.verified = True

    report = []
    if args.verified:
        for name, (sheet, rect) in VERIFIED.items():
            r = analyse(TILESETS / sheet, rect)
            # A pinned rect must yield exactly one set. More than one means the rectangle spans a
            # boundary and is wrong, and failing loudly here is the whole value of pinning it.
            if len(r["pairs"]) != 1:
                raise SystemExit(
                    f"VERIFIED[{name}] rect {rect} yields {len(r['pairs'])} sets, expected 1"
                )
            r["pairs"][0]["id"] = name
            report.append(r)
    else:
        sheets = (
            [TILESETS / args.sheet]
            if args.sheet
            else sorted(p for p in TILESETS.glob("*.png") if p.is_file())
        )
        for s in sheets:
            r = analyse(s, tuple(args.rect) if args.rect else None)
            r["pairs"] = [p for p in r["pairs"] if p["masks_covered"] >= args.min_masks]
            if r["pairs"]:
                report.append(r)

    if args.scan or not (args.emit or args.preview):
        print(f"{'pair id':34} {'masks':>6} {'cplt':>5}  {'A colour':16} {'B colour':16}")
        print("-" * 86)
        for r in report:
            for p in r["pairs"]:
                a = "void" if p["a_colour"] is None else "#%02x%02x%02x" % tuple(p["a_colour"])
                b = "VOID (overlay)" if p["b_is_void"] else "#%02x%02x%02x" % tuple(p["b_colour"])
                print(
                    f"{p['id']:34} {p['masks_covered']:>6} "
                    f"{'yes' if p['complete'] else '':>5}  {a:16} {b:16}"
                )
        total = sum(len(r["pairs"]) for r in report)
        full = sum(1 for r in report for p in r["pairs"] if p["complete"])
        print(f"\n{total} candidate sets across {len(report)} sheets; {full} cover all 16 masks")

    if args.emit:
        dest = ROOT / args.emit
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(json.dumps(report, indent=1) + "\n")
        print(f"wrote {dest}")

    if args.preview:
        for r in report:
            for p in r["pairs"]:
                if p["id"] == args.preview:
                    name = p["id"].replace(":", "_")
                    dest = ROOT / args.out / f"autotile_{name}.png"
                    preview(TILESETS / r["sheet"], p, dest)
                    print(f"wrote {dest}")
                    return 0
        print(f"no pair {args.preview!r}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
