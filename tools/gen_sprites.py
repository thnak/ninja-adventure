#!/usr/bin/env python3
"""Generate world sprites from math: plants, ground deformation, whole buildings.

Companion to `tools/gen_vfx.py`, which covers effects. This file covers things that sit in the
world, and the three families here were picked because each is a *rule* rather than a drawing:

  plants      An L-system. A plant is a grammar plus a turn angle, and growth stages are just
              iteration counts — which is the whole crop pipeline this game needs (GAME.md is a
              farming game, so every crop wants 4-5 stages and hand-drawing them does not scale).
              The same grammar with a shallower angle and no fruit gives marsh reeds.

  ground      Terrain that CHANGES. A crater is a radial height profile; wear from grass to dirt is
              an ordered dither between two fills. Dithering matters: pixel art fades one surface
              into another with a threshold matrix, never with alpha, and a Bayer 4x4 is exactly
              the 5-step ramp a tile needs to age gradually.

  buildings   `src/world/tiles.hpp:66` says the game "needs no single-tile wall art: there is no
              such thing as half a house" — kBuilding is a whole multi-tile sprite. That rules out
              wall autotiles and rules IN whole structures, drawn to the five style rules read off
              the pack's own TilesetHouse.png (see the BUILDINGS section below).

Same three house rules as gen_vfx.py, asserted the same way: binary alpha, every colour snapped to
`Palette.png`, shading quantised to a short ramp. Build-time tooling only, per ARCHITECTURE.md.

    tools/gen_sprites.py                   # -> assets/_gen/sprites
    tools/gen_sprites.py --out /tmp/x --scale 6
"""
import argparse
import math
import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
import sys  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_vfx import PALETTE, RAMPS, checkerboard, hash01, ramp, snap, value_noise  # noqa: E402

RAMPS = dict(RAMPS)
RAMPS.update({
    "stem":   ramp("345a52", "56864c", "74a334"),
    "leaf":   ramp("56864c", "74a334", "a8a129", "adbc3a"),
    "fruit":  ramp("d14b34", "e46d3a", "ff9554"),
    "grain":  ramp("d78b4a", "f1c471", "ffe18d"),
    "soil":   ramp("543c52", "695953", "816855", "90775e", "b3957f"),
    "grass":  ramp("345a52", "56864c", "74a334", "8d977f"),
    "ash":    ramp("141b1b", "3b3643", "4e484a", "8e7c73"),
    "roof_r": ramp("965340", "d14b34", "e46d3a", "ef914f"),
    "roof_b": ramp("4a5270", "2d697b", "548789", "79b8ce"),
    "roof_g": ramp("345a52", "5f7160", "56864c", "8d977f"),
    "wall_w": ramp("8e7c73", "b3957f", "fce2ca", "ffffff"),
    "wall_d": ramp("543c52", "695953", "816855", "90775e"),
    "wood":   ramp("543c52", "695953", "965340", "a3754e"),
})

# The pixel-art way to fade one surface into another: an ordered threshold matrix, not alpha.
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def dithered(x, y, amount):
    """True where `amount` (0..1) beats the Bayer threshold — gives 17 stable coverage steps."""
    return amount * 16 > BAYER4[y % 4][x % 4]


class Canvas:
    """A tiny paletted framebuffer. Every put() snaps to a ramp entry, so drift is impossible."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = {}

    def put(self, x, y, colour):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[(x, y)] = colour

    def band(self, x, y, cols, heat):
        self.put(x, y, cols[max(0, min(len(cols) - 1, int(heat * len(cols))))])

    def rect(self, x0, y0, x1, y1, colour):
        for y in range(int(y0), int(y1) + 1):
            for x in range(int(x0), int(x1) + 1):
                self.put(x, y, colour)

    def image(self):
        img = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        for (x, y), c in self.px.items():
            img.putpixel((x, y), c + (255,))
        return img


# ---------------------------------------------------------------------------------------------
# plants — L-system turtle
# ---------------------------------------------------------------------------------------------
def expand(axiom, rules, n):
    s = axiom
    for _ in range(n):
        s = "".join(rules.get(c, c) for c in s)
    return s


def walk(spec, iters, seed):
    """Run the turtle in unit-length space and return its segments and branch tips.

    Length is deliberately NOT decided here. The string for `F -> F[+F][-F]F` doubles its trunk
    every generation, so a fixed step makes generation 4 eight times taller than generation 1 and
    it grows straight off the top of a 16px tile. Measure first, scale after.
    """
    stack, segs, tips = [], [], []
    x, y, ang, depth, idx = 0.0, 0.0, -90.0, 0, 0
    for ch in expand(spec["axiom"], spec["rules"], iters):
        if ch == "F":
            idx += 1
            a = math.radians(ang + (hash01(seed, idx) - 0.5) * spec["jitter"])
            nx, ny = x + math.cos(a), y + math.sin(a)
            segs.append((x, y, nx, ny, depth))
            x, y = nx, ny
            tips.append((x, y, depth))
        elif ch == "+":
            ang -= spec["angle"]
        elif ch == "-":
            ang += spec["angle"]
        elif ch == "[":
            stack.append((x, y, ang, depth))
            depth += 1
        elif ch == "]":
            x, y, ang, depth = stack.pop()
    return segs, tips


def draw_plant(c, spec, iters, size_frac, seed):
    """Fit the walked skeleton to the tile, then plot stems, then foliage and fruit over them."""
    segs, tips = walk(spec, iters, seed)
    if not segs:
        return
    xs = [p for s in segs for p in (s[0], s[2])]
    ys = [p for s in segs for p in (s[1], s[3])]
    pad = spec["leaf_r"] + 1
    span_x, span_y = max(1e-6, max(xs) - min(xs)), max(1e-6, -min(ys))
    sc = min((c.w - 2 * pad) / span_x, (c.h - 1 - pad) / span_y) * size_frac
    mid = (max(xs) + min(xs)) / 2

    def to_px(px, py):
        return c.w / 2 + (px - mid) * sc, (c.h - 1) + py * sc

    for x0, y0, x1, y1, depth in segs:
        ax, ay = to_px(x0, y0)
        bx, by = to_px(x1, y1)
        steps = max(2, int(math.hypot(bx - ax, by - ay) * 2))
        for i in range(steps + 1):
            px, py = ax + (bx - ax) * i / steps, ay + (by - ay) * i / steps
            c.band(px, py, RAMPS["stem"], 0.30 + 0.55 * (depth / 3))
            if depth == 0 and sc > 3.0:  # thicken the trunk only, and only when there is room
                c.band(px + 1, py, RAMPS["stem"], 0.10)

    leaf = RAMPS[spec["leaf"]]
    rad = spec["leaf_r"] * (0.55 + 0.45 * size_frac)
    for i, (tx, ty, d) in enumerate(tips):
        if d < spec["leaf_depth"]:
            continue
        px, py = to_px(tx, ty)
        for dy in range(-3, 4):
            for dx in range(-3, 4):
                if math.hypot(dx, dy * 1.2) <= rad:
                    c.band(px + dx, py + dy, leaf, 0.40 + 0.55 * hash01(seed, i, dx * 7 + dy))
    if spec.get("fruit") and size_frac > 0.65:
        fr = RAMPS[spec["fruit"]]
        for i, (tx, ty, d) in enumerate(tips):
            if d >= spec["leaf_depth"] and hash01(seed, i, 99) > 0.5:
                px, py = to_px(tx, ty)
                c.band(px, py, fr, 0.9)
                c.band(px, py - 1, fr, 0.45)


PLANTS = [
    # (name, grammar, tile w, tile h, growth stages)
    ("crop_wheat", dict(axiom="F", rules={"F": "F[+F][-F]F"}, angle=13, jitter=9,
                        leaf="grain", leaf_depth=1, leaf_r=1.4, fruit="grain"), 16, 16, 5),
    ("crop_berry", dict(axiom="F", rules={"F": "FF[+F][-F]"}, angle=36, jitter=16,
                        leaf="leaf", leaf_depth=1, leaf_r=2.2, fruit="fruit"), 16, 16, 5),
    ("sapling",    dict(axiom="F", rules={"F": "F[+F]F[-F][F]"}, angle=27, jitter=14,
                        leaf="leaf", leaf_depth=1, leaf_r=2.6, fruit=None), 16, 16, 5),
    # macrophytes: shallow angle, tall thin stalks, no fruit — kMarsh and kWater dressing
    ("reed_marsh", dict(axiom="F", rules={"F": "F[+F]F[-F]"}, angle=11, jitter=7,
                        leaf="leaf", leaf_depth=1, leaf_r=1.2, fruit=None), 16, 24, 5),
    ("kelp_water", dict(axiom="F", rules={"F": "F[+F]F"}, angle=20, jitter=24,
                        leaf="stem", leaf_depth=1, leaf_r=1.6, fruit=None), 16, 24, 5),
]


def plant_strip(spec, w, h, stages, seed):
    sheet = Image.new("RGBA", (w * stages, h), (0, 0, 0, 0))
    for k in range(stages):
        c = Canvas(w, h)
        frac = (k + 1) / stages
        # a stage adds both a generation of the grammar and overall size: more branches AND taller
        draw_plant(c, spec, min(4, 1 + k), 0.30 + 0.70 * frac, seed)
        sheet.paste(c.image(), (k * w, 0))
    return sheet


# ---------------------------------------------------------------------------------------------
# ground — deformation and topography change
# ---------------------------------------------------------------------------------------------
def fill_noise(c, cols, x0, y0, x1, y1, freq, seed, lo=0.15, hi=0.95):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            n = value_noise(x * freq + seed * 13, y * freq + seed * 7)
            c.band(x, y, cols, lo + (hi - lo) * n)


def crater_tile(size, depth):
    """Radial height profile: raised rim, dark floor, shadow on the sun-facing side of the lip."""
    c = Canvas(size, size)
    fill_noise(c, RAMPS["soil"], 0, 0, size - 1, size - 1, 0.55, 3, 0.55, 0.95)
    if depth <= 0:
        return c.image()
    cx = cy = (size - 1) / 2
    rad = 0.30 + 0.62 * depth
    for y in range(size):
        for x in range(size):
            r = math.hypot(x - cx, y - cy) / (size / 2)
            r *= 0.88 + 0.24 * value_noise(x * 0.5, y * 0.5)  # broken, not a compass circle
            if r > rad:
                continue
            if r > rad * 0.78:  # rim: catches the light
                c.band(x, y, RAMPS["soil"], 0.85)
            else:  # floor: darker the deeper you go, and shadowed at the top edge
                shade = 0.42 - 0.30 * depth * (1 - r / rad) + (0.14 if y > cy else -0.10)
                c.band(x, y, RAMPS["soil"], max(0.02, shade))
    return c.image()


def wear_tile(size, amount):
    """Grass worn to dirt by footfall. The whole transition is one Bayer threshold."""
    c = Canvas(size, size)
    fill_noise(c, RAMPS["grass"], 0, 0, size - 1, size - 1, 0.45, 5, 0.35, 0.95)
    for y in range(size):
        for x in range(size):
            local = amount * (0.65 + 0.7 * value_noise(x * 0.35 + 21, y * 0.35 + 9))
            if dithered(x, y, local):
                c.band(x, y, RAMPS["soil"], 0.45 + 0.4 * value_noise(x * 0.6, y * 0.6))
    return c.image()


def scorch_tile(size, amount):
    """Meteor/fire aftermath. Ash core, dithered edge so it tiles against unburnt ground."""
    c = Canvas(size, size)
    fill_noise(c, RAMPS["soil"], 0, 0, size - 1, size - 1, 0.55, 3, 0.55, 0.95)
    cx = cy = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            r = math.hypot(x - cx, y - cy) / (size / 2)
            r *= 0.8 + 0.4 * value_noise(x * 0.6 + 31, y * 0.6 + 17)
            cov = max(0.0, min(1.0, (amount * 1.25 - r) * 2.2))
            if cov > 0 and dithered(x, y, cov):
                c.band(x, y, RAMPS["ash"], 0.15 + 0.6 * value_noise(x * 0.8, y * 0.8) * (1 - cov))
    return c.image()


def till_tile(size):
    """Ploughed furrows: a sine ridge pattern, light crest and dark trough."""
    c = Canvas(size, size)
    for y in range(size):
        for x in range(size):
            f = math.sin((y + 0.5) * math.pi * 2 / 5.0 + math.sin(x * 0.4) * 0.35)
            c.band(x, y, RAMPS["soil"], 0.30 + 0.45 * (f * 0.5 + 0.5)
                   + 0.15 * value_noise(x * 0.9, y * 0.9))
    return c.image()


def ground_strips(size):
    out = []
    out.append(("crater", [crater_tile(size, d) for d in (0, 0.25, 0.5, 0.75, 1.0)]))
    out.append(("path_wear", [wear_tile(size, a) for a in (0, 0.25, 0.5, 0.75, 1.0)]))
    out.append(("scorch", [scorch_tile(size, a) for a in (0, 0.3, 0.55, 0.8, 1.0)]))
    out.append(("tilled", [till_tile(size)]))
    return out


# ---------------------------------------------------------------------------------------------
# buildings — whole structures, since kBuilding is a whole-structure footprint
#
# The rules below were read off `Backgrounds/Tilesets/TilesetHouse.png`, not invented. The pack
# does NOT draw a gabled front elevation; it draws a top-down oblique, and five things carry the
# style. Break any of them and the result reads as a European cottage instead:
#
#   1. ROUNDED SLAB.   The roof is a rounded rectangle seen from above filling the top ~58%, with
#                      the wall as a short band under it. There is no triangular gable anywhere.
#   2. HARD OUTLINE.   A 1px 141b1b silhouette outline around the whole building. Every house in
#                      the sheet has ~300px of it; it is the single strongest style marker.
#   3. CORNER TUFTS.   Thatch roofs flick up into small light horns at the two top corners.
#   4. BLACK DOORWAY.  The door is an unlit opening — flat 141b1b with a rounded top. The pack
#                      never draws a wooden door leaf with a handle.
#   5. WIDE LATTICE.   Windows are wide, short, mullioned panels framed in the roof accent colour,
#                      not small punched squares.
# ---------------------------------------------------------------------------------------------
OUTLINE = snap((0x14, 0x1B, 0x1B))

ROOFS = {
    "thatch_orange": ramp("965340", "e66a3a", "ff8b62", "ffad5d"),
    "thatch_cream":  ramp("bd7959", "d2b37d", "eecf9b", "fce2ca"),
    "tile_red":      ramp("965340", "e0394c", "e66a3a", "ff8b62"),
    "tile_blue":     ramp("4a5270", "2d697b", "548789", "79b8ce"),
    "tile_green":    ramp("345a52", "5f7160", "56864c", "8d977f"),
}
WALLS = {
    "plaster": ramp("bd7959", "c8966b", "d2b37d", "eecf9b"),
    "timber":  ramp("695953", "816855", "90775e", "b3957f"),
}


def rrect_inset(row, height, r_top, r_bot):
    """Horizontal inset of the slab silhouette at `row`.

    A circular arc is the obvious guess and it is wrong. Measuring the pack's own house gives left
    insets of 9,8,7,7,6,6,5,4 down the first eight rows — a long shallow CHAMFER that takes ~13
    rows to reach the wall, not a quarter-circle that closes in 4. A circle here reads as a bubble.
    """
    top_run = max(4, int(r_top * 1.45))
    if row < top_run:
        return int(round(r_top * (1.0 - row / top_run)))
    bot_run = max(3, int(r_bot * 1.45))
    if row >= height - bot_run:
        return int(round(r_bot * (1.0 - (height - 1 - row) / bot_run)))
    return 0


def building(tw, th, roof_name, wall_name, style, seed, tile=16):
    """A house as parameters, following the five rules above."""
    w, h = tw * tile, th * tile
    c = Canvas(w, h)
    roof, wall = ROOFS[roof_name], WALLS[wall_name]
    roof_h = int(h * 0.58)
    r_top, r_bot = max(3, w // 8), max(3, w // 12)

    rows = [(y, rrect_inset(y, h, r_top, r_bot)) for y in range(h)]

    # --- roof slab -----------------------------------------------------------------------
    # Colours are put explicitly rather than through band(): the 1px dark rim inside the outline
    # is the shading step that reads as "slab seen from above", and heat quantisation loses it.
    dark, body, mid, light = roof[0], roof[1], roof[2], roof[3]
    for y, inset in rows[:roof_h]:
        x0, x1 = inset, w - 1 - inset
        for x in range(x0, x1 + 1):
            edge = min(x - x0, x1 - x, y)
            if edge == 0:
                c.put(x, y, OUTLINE)          # rule 2: hard silhouette
                continue
            if edge == 1:
                c.put(x, y, dark)             # rim, one pixel in
                continue
            col = body
            if style == "tile":               # kawara: a grid of tile courses
                course = (y - 2) // 3
                col = mid if (y - 2) % 3 == 0 else body
                if (x + course * 2) % 4 == 0:
                    col = dark
            else:                             # combed straw: vertical texture down both edges
                depth_in = min(x - x0, x1 - x)
                if depth_in < 6 and x % 2 == 0:
                    col = dark
                elif y < 4:
                    col = mid                 # ridge highlight along the top of the slab
                elif 5 < y < roof_h - 5 and value_noise(x * 0.8 + seed * 9, y * 0.8) > 0.94:
                    col = light               # a few straw glints, kept away from the edges
            c.put(x, y, col)

    # rule 3: corner tufts — straw horns that stand ABOVE the slab. They have to sit outside the
    # rounded silhouette, not on it, or they vanish into the outline and the roof reads rectangular.
    if style == "thatch":
        for side in (0, 1):
            for dy in range(1, max(4, r_top)):
                e = rrect_inset(dy, h, r_top, r_bot)
                for d in (2, 3):  # stay INSIDE the chamfer: outline is at e, rim at e+1
                    x = (e + d) if side == 0 else (w - 1 - e - d)
                    c.put(x, dy, light if d == 2 else mid)

    # --- eave: dark band under the roof plus the beam ends poking through --------------------
    for x in range(rows[roof_h][1], w - rows[roof_h][1]):
        c.put(x, roof_h - 1, OUTLINE)
        c.band(x, roof_h, roof, 0.28)
    for x in range(rows[roof_h][1] + 3, w - rows[roof_h][1] - 2, 5):
        c.band(x, roof_h + 1, roof, 0.20)  # beam tick

    # --- wall band -----------------------------------------------------------------------
    for y, inset in rows[roof_h + 1:]:
        x0, x1 = inset, w - 1 - inset
        for x in range(x0, x1 + 1):
            edge = min(x - x0, x1 - x, h - 1 - y)
            if edge == 0:
                c.put(x, y, OUTLINE)
                continue
            # sparse offset masonry dashes, not a full grid — the pack keeps the wall quiet
            col = wall[2]
            if (y - roof_h) % 4 == 0 and ((x + ((y - roof_h) // 4) * 3) // 3) % 3 != 0:
                col = wall[1]
            c.put(x, y, col)
    # corner posts, in the roof rim colour so the two halves read as one building
    for x in (rows[h - 3][1] + 1, w - 2 - rows[h - 3][1]):
        for y in range(roof_h + 1, h - 1):
            c.put(x, y, dark)

    # --- rule 4: the doorway is a hole, not a door ------------------------------------------
    dw = max(5, w // 7 | 1)
    dh = int((h - roof_h) * 0.68)
    dx0, dy0 = w // 2 - dw // 2, h - dh - 1
    for y in range(dy0, h - 1):
        for x in range(dx0, dx0 + dw):
            if y == dy0 and (x == dx0 or x == dx0 + dw - 1):
                continue  # rounded top
            c.put(x, y, OUTLINE)

    # --- rule 5: wide mullioned lattice panels ----------------------------------------------
    ww, wh = max(6, w // 6), max(4, (h - roof_h) // 3)
    wy = roof_h + 3
    for sx in (dx0 - ww - 3, dx0 + dw + 3):
        if sx < 3 or sx + ww > w - 3 or wy + wh > h - 3:
            continue
        c.rect(sx - 1, wy - 1, sx + ww, wy + wh, dark)      # frame, in the roof rim colour
        for y in range(wy, wy + wh):
            for x in range(sx, sx + ww):
                c.put(x, y, wall[3] if x % 3 else wall[1])  # vertical mullions
    return c.image()


BUILDINGS = [
    # (name, tiles w, tiles h, roof, wall, roof style, seed)
    ("house_thatch",  4, 3, "thatch_orange", "plaster", "thatch", 1),
    ("house_cream",   4, 3, "thatch_cream",  "plaster", "thatch", 2),
    ("temple_red",    4, 3, "tile_red",      "timber",  "tile",   3),
    ("shop_blue",     3, 3, "tile_blue",     "plaster", "tile",   4),
    ("hut_small",     2, 2, "thatch_orange", "timber",  "thatch", 5),
]


# ---------------------------------------------------------------------------------------------
def check(img, name):
    px = list(img.get_flattened_data())
    assert all(c[3] in (0, 255) for c in px), f"{name}: alpha must stay binary"
    assert all(c[:3] in PALETTE for c in px if c[3]), f"{name}: colours must stay on-palette"
    return len({c for c in px if c[3]})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "assets" / "_gen" / "sprites"))
    ap.add_argument("--scale", type=int, default=4)
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    rows = []
    for i, (name, spec, w, h, stages) in enumerate(PLANTS):
        sheet = plant_strip(spec, w, h, stages, i + 1)
        sheet.save(out / f"{name}.png")
        print(f"{name:14s} {sheet.width:3d}x{sheet.height:2d}  {stages} stages  "
              f"{check(sheet, name)} colours")
        rows.append((name, sheet))

    for name, tiles in ground_strips(16):
        sheet = Image.new("RGBA", (16 * len(tiles), 16), (0, 0, 0, 0))
        for i, t in enumerate(tiles):
            sheet.paste(t, (i * 16, 0))
        sheet.save(out / f"{name}.png")
        print(f"{name:14s} {sheet.width:3d}x16   {len(tiles)} steps   {check(sheet, name)} colours")
        rows.append((name, sheet))

    for name, tw, th, rr, wr, style, seed in BUILDINGS:
        img = building(tw, th, rr, wr, style, seed)
        img.save(out / f"{name}.png")
        print(f"{name:14s} {img.width:3d}x{img.height:2d}  {tw}x{th} tiles  "
              f"{check(img, name)} colours")
        rows.append((name, img))

    s, pad, label_w = args.scale, 6, 130
    width = label_w + max(im.width for _, im in rows) * s + pad * 2
    height = sum(im.height * s + pad for _, im in rows) + pad
    preview = checkerboard(width, height)
    d = ImageDraw.Draw(preview)
    y = pad
    for name, im in rows:
        big = im.resize((im.width * s, im.height * s), Image.NEAREST)
        preview.paste(big, (label_w, y), big)
        d.text((6, y + big.height // 2 - 4), name, fill=(210, 214, 222, 255))
        y += big.height + pad
    preview.save(out / "preview.png")
    print(f"\n-> {out}/preview.png")


if __name__ == "__main__":
    main()
