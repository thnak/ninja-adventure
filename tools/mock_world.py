#!/usr/bin/env python3
"""Render one patch of world twice -- the way we do it now, and the way the pack demos do it.

This is an argument settler, not a renderer. It draws no game state and is not imported by
anything; it exists so the difference between the two approaches can be looked at instead of
described. Compare its output against `assets/_src/ninja/Example 3.gif` and `Example 4.gif`.

The four differences it demonstrates, in the order they matter:

1. TRANSITION TILES.  The whole finding. `tools/study_examples.py` shows our screenshots pin
   12.8% of colour boundaries to the 32px tile grid against the demos' 6.1% -- no overlap between
   the populations -- because we butt one flat fill tile against another. The pack ships complete
   corner-transition sets we never packed; `tools/autotile_fit.py` recovers them.

   The subtlety that makes it work: terrain is decided **per corner vertex**, on a (W+1)x(H+1)
   grid, and each tile reads the four corners around it. Deciding terrain per TILE and then trying
   to infer corners from neighbours is the obvious way and it is wrong -- it produces masks that
   no tileset has art for (a lone diagonal touching), which then have to be fudged back to a flat
   fill, which puts the hard edges right back.

2. SUB-TILE PROP PLACEMENT.  Trees in our screenshots sit on an exact 32px lattice in rows and
   columns. The pack's sit at free pixel offsets and their canopies overlap, so a wood reads as a
   mass rather than an orchard. Cheap to fix: jitter, and draw back-to-front by Y.

3. CLUSTERED, SPARSE SCATTER.  We stamp the same grass tuft on every grass tile, which is why our
   `density` metric is nearly double the pack's while looking emptier -- uniform detail everywhere
   is wallpaper. The pack leaves most ground plain and clumps its detail.

4. ORGANIC REGION SHAPE.  Region boundaries come from a noise field, not from rectangles.

Usage:
    python3 tools/mock_world.py                     # writes tools/_study/mock_world.png
    python3 tools/mock_world.py --seed 7 --tiles 40 24 --scale 2
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

ROOT = pathlib.Path(__file__).resolve().parent.parent
TILESETS = ROOT / "assets" / "_src" / "ninja" / "Backgrounds" / "Tilesets"
AUTOTILE = ROOT / "tools" / "_autotile.json"

TILE = 16
TL, TR, BL, BR = 1, 2, 4, 8

# The grass-on-dirt set from autotile_fit.VERIFIED -- a pinned, previewed rectangle rather than an
# auto-detected candidate. Regenerate the table with:
#     python3 tools/autotile_fit.py --verified --emit tools/_autotile.json
# `a` is grass (bits set), `b` is dirt.
GRASS_DIRT = "grass_dirt"

# Tree sprites in TilesetNature: (col, row), 4 wide and 3 tall.
#
# NOT the rectangles BIG_MANIFEST uses. It has TreeBroad at (5,2) 2x3 and TreePine at (1,2) 2x3,
# and both are the middle two columns of a FOUR-column tree -- the canopy has three lobes and the
# crop keeps only the centre one plus the trunk. `tools/check_sprite_rects.py` scores those
# rectangles at 58% and 60% of their left and right borders cut through non-outline pixels; the
# full-width ones score 0% on every border. This is a large part of why the forest in
# `docs/shot_forest.png` reads as a palisade of identical narrow columns.
TREE_W, TREE_H = 4, 3
TREES = [(4, 2), (0, 2)]

# Ground scatter. `TilesetFloorDetail.png` is the sheet for this and the first draft of this tool
# did not use it -- it scattered tiles from TilesetNature rows 0-2, which are not decor at all but
# the upper CANOPY SLICES of the 2x3 trees, so the mock meadow was strewn with green fragments and
# pink cherry-blossom corners. Row 2 of FloorDetail is tufts, weeds and small bushes; row 0 is
# twigs, pebbles and bones for bare ground.
DECOR_GRASS = [(c, 2) for c in range(8)]
DECOR_DIRT = [(0, 0), (1, 0), (3, 0), (5, 0), (6, 0), (7, 0)]
DECOR_SHEET = "TilesetFloorDetail.png"


# --- deterministic noise -----------------------------------------------------
# Local rather than imported from gen_vfx: this tool has no other reason to load the palette, and
# a mock renderer that silently changes when the palette loader changes is a bad debugging tool.


def hash01(*args: int) -> float:
    h = 2166136261
    for a in args:
        h ^= (a + 0x9E3779B9) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= h >> 13
    return (h & 0xFFFFFF) / 0xFFFFFF


def smooth(t: float) -> float:
    return t * t * (3 - 2 * t)


def value_noise(x: float, y: float, seed: int) -> float:
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = smooth(x - x0), smooth(y - y0)
    a = hash01(x0, y0, seed)
    b = hash01(x0 + 1, y0, seed)
    c = hash01(x0, y0 + 1, seed)
    d = hash01(x0 + 1, y0 + 1, seed)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(x: float, y: float, seed: int, octaves: int = 3) -> float:
    v, amp, freq, norm = 0.0, 1.0, 1.0, 0.0
    for o in range(octaves):
        v += amp * value_noise(x * freq, y * freq, seed + o * 977)
        norm += amp
        amp *= 0.5
        freq *= 2.0
    return v / norm


# --- tile source -------------------------------------------------------------


class Sheets:
    def __init__(self) -> None:
        self._cache: dict[str, Image.Image] = {}

    def sheet(self, name: str) -> Image.Image:
        if name not in self._cache:
            self._cache[name] = Image.open(TILESETS / name).convert("RGBA")
        return self._cache[name]

    def tile(self, name: str, c: int, r: int, w: int = 1, h: int = 1) -> Image.Image:
        return self.sheet(name).crop(
            (c * TILE, r * TILE, (c + w) * TILE, (r + h) * TILE)
        )


def load_set(pair_id: str) -> tuple[str, dict]:
    if not AUTOTILE.exists():
        raise SystemExit(
            f"{AUTOTILE} missing -- run:  python3 tools/autotile_fit.py --emit tools/_autotile.json"
        )
    for sheet in json.loads(AUTOTILE.read_text()):
        for p in sheet["pairs"]:
            if p["id"] == pair_id:
                return sheet["sheet"], p
    raise SystemExit(f"pair {pair_id!r} not in {AUTOTILE}")


# --- the two renderers -------------------------------------------------------


def terrain_at(cx: int, cy: int, seed: int) -> int:
    """1 = grass, 0 = dirt, sampled at a CORNER vertex.

    The 0.52 threshold is off centre on purpose: at exactly 0.5 the two terrains interleave in
    equal measure and the result is speckle, not regions.
    """
    return 1 if fbm(cx / 9.0, cy / 9.0, seed) > 0.52 else 0


def render_correct(
    sh: Sheets, tw: int, th: int, seed: int, scatter: bool = True, props: bool = True
) -> Image.Image:
    sheet_name, aset = load_set(GRASS_DIRT)
    # `coherent`, not `tiles`: one seam-free tile per mask. See autotile_fit.coherent_pick -- a
    # random choice among a mask's variants reintroduces exactly the grid-aligned steps this whole
    # exercise is about removing, and it measurably did.
    tiles = {int(k): [v] for k, v in aset["coherent"].items()}
    grass_fill = aset["a_fills"] or [[1, 12]]
    dirt_fill = aset["b_fills"] or [[12, 19]]

    img = Image.new("RGBA", (tw * TILE, th * TILE), (0, 0, 0, 255))

    for ty in range(th):
        for tx in range(tw):
            # Four corner vertices around this tile.
            mask = 0
            for bit, (dx, dy) in zip((TL, TR, BL, BR), ((0, 0), (1, 0), (0, 1), (1, 1))):
                if terrain_at(tx + dx, ty + dy, seed):
                    mask |= bit
            if mask == 15:
                pick = grass_fill[int(hash01(tx, ty, seed) * len(grass_fill)) % len(grass_fill)]
            elif mask == 0:
                pick = dirt_fill[int(hash01(tx, ty, seed + 1) * len(dirt_fill)) % len(dirt_fill)]
            else:
                opts = tiles.get(mask)
                if not opts:
                    # Every mask 1..14 is present in this set, so this is unreachable -- but a
                    # renderer that crashes on a gap is useless for exploring OTHER sets, and one
                    # that silently draws a fill would hide exactly the hard edge we are hunting.
                    opts = [grass_fill[0]] if bin(mask).count("1") > 2 else [dirt_fill[0]]
                pick = opts[int(hash01(tx, ty, seed + 2) * len(opts)) % len(opts)]
            img.alpha_composite(sh.tile(sheet_name, pick[0], pick[1]), (tx * TILE, ty * TILE))

    if scatter:
        # Clumped, not uniform: a second noise field gates where detail is allowed at all, then a
        # per-tile roll decides. Most ground stays plain, which is what makes the clumps read.
        for ty in range(th):
            for tx in range(tw):
                corners = [terrain_at(tx + dx, ty + dy, seed) for dx in (0, 1) for dy in (0, 1)]
                if fbm(tx / 4.0, ty / 4.0, seed + 313) < 0.58:
                    continue
                if hash01(tx, ty, seed + 71) > 0.45:
                    continue
                # Decor has to match the ground under it, so only scatter on a PURE tile. On a
                # transition tile there is no single right answer and a twig half on grass reads
                # as a mistake.
                if all(corners):
                    pool = DECOR_GRASS
                elif not any(corners):
                    pool = DECOR_DIRT
                else:
                    continue
                c, r = pool[int(hash01(tx, ty, seed + 5) * len(pool)) % len(pool)]
                ox = int(hash01(tx, ty, seed + 6) * 10) - 5
                oy = int(hash01(tx, ty, seed + 7) * 10) - 5
                img.alpha_composite(sh.tile(DECOR_SHEET, c, r), (tx * TILE + ox, ty * TILE + oy))

    if props:
        # Free pixel offsets and back-to-front by Y, so canopies overlap into a mass.
        placed = []
        for ty in range(-2, th):
            for tx in range(-1, tw):
                if fbm(tx / 6.0, ty / 6.0, seed + 909) < 0.60:
                    continue
                if hash01(tx, ty, seed + 11) > 0.55:
                    continue
                px = tx * TILE + int(hash01(tx, ty, seed + 12) * 13) - 6
                py = ty * TILE + int(hash01(tx, ty, seed + 13) * 13) - 6
                # A tree stands on ground, so test the corner under its TRUNK, not its top-left.
                foot_x, foot_y = (px + TREE_W * TILE // 2) // TILE, (py + TREE_H * TILE) // TILE
                if not terrain_at(foot_x, foot_y, seed):
                    continue
                kind = TREES[int(hash01(tx, ty, seed + 14) * len(TREES)) % len(TREES)]
                placed.append((py, px, kind))
        for py, px, (c, r) in sorted(placed):
            img.alpha_composite(
                sh.tile("TilesetNature.png", c, r, TREE_W, TREE_H), (px, py)
            )

    return img


def render_naive(sh: Sheets, tw: int, th: int, seed: int) -> Image.Image:
    """What we do today: one flat fill per tile, props snapped to the lattice."""
    sheet_name, aset = load_set(GRASS_DIRT)
    grass_fill = aset["a_fills"] or [[1, 12]]
    dirt_fill = aset["b_fills"] or [[12, 19]]

    img = Image.new("RGBA", (tw * TILE, th * TILE), (0, 0, 0, 255))
    for ty in range(th):
        for tx in range(tw):
            # Terrain per TILE, sampled at its centre -- no corners, so no transitions possible.
            g = fbm((tx + 0.5) / 9.0, (ty + 0.5) / 9.0, seed) > 0.52
            pool = grass_fill if g else dirt_fill
            pick = pool[int(hash01(tx, ty, seed) * len(pool)) % len(pool)]
            img.alpha_composite(sh.tile(sheet_name, pick[0], pick[1]), (tx * TILE, ty * TILE))
            # A decoration on EVERY grass tile, at a fixed offset. This is the wallpaper effect.
            if g:
                img.alpha_composite(sh.tile(DECOR_SHEET, 0, 2), (tx * TILE, ty * TILE))

    for ty in range(0, th, 3):
        for tx in range(0, tw, 2):
            if fbm(tx / 6.0, ty / 6.0, seed + 909) < 0.60:
                continue
            if not (fbm((tx + 0.5) / 9.0, (ty + 2.5) / 9.0, seed) > 0.52):
                continue
            # The severed 2x3 rectangle, drawn as-is: this panel is what we ship today.
            img.alpha_composite(sh.tile("TilesetNature.png", 5, 2, 2, 3), (tx * TILE, ty * TILE))
    return img


# --- output ------------------------------------------------------------------


def terrain_seam_lock(img: Image.Image, aset: dict, grid: int) -> float:
    """Of the pixels where grass meets dirt, what fraction sit on a tile boundary?

    This is the measurement the whole exercise is actually about, stated without a denominator
    that can be gamed. It ignores every other edge in the scene -- tree outlines, tufts, pebbles --
    and looks only at the grass/dirt frontier, so adding or removing decoration cannot move it.

    1.00 means every terrain boundary in the image is a tile edge, which is what flat fills butted
    together produce and what the pack's transition sets exist to avoid.
    """
    ac, bc = tuple(aset["a_colour"]), tuple(aset["b_colour"])

    def side(p) -> int:
        da = abs(p[0] - ac[0]) + abs(p[1] - ac[1]) + abs(p[2] - ac[2])
        db = abs(p[0] - bc[0]) + abs(p[1] - bc[1]) + abs(p[2] - bc[2])
        # Only commit where the pixel is clearly one terrain or the other. Tree canopy and decor
        # sit far from both centres and must not be counted as a frontier.
        if min(da, db) > 170:
            return 0
        return 1 if da < db else 2

    px = img.load()
    w, h = img.size
    on = total = 0
    for y in range(h):
        for x in range(1, w):
            l, c = side(px[x - 1, y]), side(px[x, y])
            if l and c and l != c:
                total += 1
                if x % grid == 0:
                    on += 1
    for y in range(1, h):
        for x in range(w):
            u, c = side(px[x, y - 1]), side(px[x, y])
            if u and c and u != c:
                total += 1
                if y % grid == 0:
                    on += 1
    return on / total if total else 0.0


def label(img: Image.Image, text: str) -> Image.Image:
    from PIL import ImageDraw

    out = Image.new("RGBA", (img.width, img.height + 20), (18, 18, 22, 255))
    out.alpha_composite(img, (0, 20))
    ImageDraw.Draw(out).text((6, 5), text, fill=(235, 235, 235, 255))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--tiles", type=int, nargs=2, default=(40, 25), metavar=("W", "H"))
    ap.add_argument("--scale", type=int, default=2, help="the demos are recorded at 2x")
    ap.add_argument("--out", default="tools/_study/mock_world.png")
    ap.add_argument(
        "--measure", action="store_true", help="score both panels with study_examples"
    )
    args = ap.parse_args()

    sh = Sheets()
    tw, th = args.tiles

    a = render_naive(sh, tw, th, args.seed)
    b = render_correct(sh, tw, th, args.seed)

    if args.scale != 1:
        size = (a.width * args.scale, a.height * args.scale)
        a = a.resize(size, Image.NEAREST)
        b = b.resize(size, Image.NEAREST)

    if args.measure:
        import study_examples as se

        _, aset = load_set(GRASS_DIRT)
        print(f"{'panel':10} {'grid_lock':>10} {'seam':>10}")
        for nm, img in (("NOW", a), ("PACK", b)):
            rgb = img.convert("RGB")
            e, w, h = se.edge_map(rgb)
            print(
                f"{nm:10} {se.grid_lock(e, w, h, args.scale * TILE):>10.3f} "
                f"{terrain_seam_lock(rgb, aset, args.scale * TILE):>10.3f}"
            )
        print(
            "\ngrid_lock is a RATIO over all edges and is confounded here: the two panels differ\n"
            "in total edge count by design, and the NOW panel's tuft-on-every-tile floods the\n"
            "denominator with off-grid edges that flatter it. It separates the pack demos from\n"
            "our screenshots because those have comparable detail; it cannot arbitrate this A/B.\n"
            "`seam` is the unconfounded one: of the pixels where grass actually meets dirt, the\n"
            "fraction lying on a tile boundary. 1.00 means every terrain boundary is a tile edge."
        )

    a = label(a, "NOW  - flat fills butted together, decor on every tile, trees on the lattice")
    b = label(b, "PACK - corner transition tiles, clumped decor, trees at free offsets, Y-sorted")

    out = Image.new("RGBA", (a.width, a.height + b.height + 8), (18, 18, 22, 255))
    out.alpha_composite(a, (0, 0))
    out.alpha_composite(b, (0, a.height + 8))

    dest = ROOT / args.out
    dest.parent.mkdir(parents=True, exist_ok=True)
    out.convert("RGB").save(dest)
    print(f"wrote {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
