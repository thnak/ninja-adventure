#!/usr/bin/env python3
"""Crop candidate multi-tile STRUCTURE rectangles and lay them out for a look.

The whole reason this file exists: four separate rendering bugs in this project traced back to
picking tiles by number without seeing them (a tower roof used as a tree, a nine-slice corner used
as a wall, a map-editor swatch used as ground). A structure is worse than a tile, because a wrong
rectangle is only wrong at its EDGES — you get a house with half a neighbour's roof glued on, which
a per-tile contact sheet will never show you.

So: this crops each candidate exactly as `BIG_MANIFEST` would, at 6x, with its name and footprint
under it. Anything that survives this review goes into tools/build_atlas.py verbatim.

    tools/verify_structures.py OUT.png
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
TS = ROOT / "assets" / "_src" / "ninja" / "Backgrounds" / "Tilesets"
TILE = 16

# (name, sheet file, col, row, tiles wide, tiles tall)
CANDIDATES = [
    ("HouseOrange", "TilesetHouse.png", 0, 0, 4, 3),
    ("HouseCream", "TilesetHouse.png", 4, 0, 4, 3),
    ("HouseAmber", "TilesetHouse.png", 8, 0, 4, 3),
    ("HouseRed", "TilesetHouse.png", 12, 0, 4, 3),
    ("HouseBlue", "TilesetHouse.png", 16, 0, 3, 3),
    ("HouseTan", "TilesetHouse.png", 23, 0, 3, 3),
    ("HouseWood", "TilesetHouse.png", 26, 0, 3, 3),
    ("HouseSnowA", "TilesetHouse.png", 0, 11, 3, 3),
    ("HouseSnowB", "TilesetHouse.png", 3, 11, 3, 3),
    ("HouseSnowC", "TilesetHouse.png", 6, 11, 3, 3),
    ("HouseStone", "TilesetHouse.png", 3, 19, 3, 3),
    ("HouseStoneB", "TilesetHouse.png", 5, 19, 3, 3),
    ("Dojo", "TilesetHouse.png", 4, 4, 3, 1),
    ("WallStoneA", "TilesetHouse.png", 8, 8, 2, 2),
    ("WallStoneB", "TilesetHouse.png", 10, 8, 2, 2),
    ("WallStoneC", "TilesetHouse.png", 12, 8, 3, 2),
    ("FenceWoodA", "TilesetHouse.png", 8, 4, 2, 2),
    ("FenceWoodB", "TilesetHouse.png", 10, 4, 2, 2),
    ("FenceWoodC", "TilesetHouse.png", 12, 4, 3, 2),
    ("TentA", "tileset_camp.png", 4, 0, 3, 3),
    ("TentB", "tileset_camp.png", 7, 0, 3, 3),
    ("TentC", "tileset_camp.png", 10, 0, 3, 3),
    ("CaveMouth", "tileset_camp.png", 11, 3, 3, 2),
    ("RuinA", "TilesetVillageAbandoned.png", 11, 0, 3, 3),
    ("RuinB", "TilesetVillageAbandoned.png", 11, 3, 3, 3),
    ("RuinC", "TilesetVillageAbandoned.png", 14, 0, 3, 3),
]


def _font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if pathlib.Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def main() -> int:
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "structures.png")
    scale = 6
    pad = 10
    label_h = 18
    # A grass-green backdrop, not a checkerboard: these are drawn ON grass in the game, and a
    # transparent margin that reads as "part of the sprite" over a checkerboard reads as empty here.
    grass = (104, 158, 74, 255)

    sheets = {}
    cells = []
    for name, f, col, row, w, h in CANDIDATES:
        if f not in sheets:
            p = TS / f
            if not p.exists():
                print(f"missing {p}", file=sys.stderr)
                return 1
            sheets[f] = Image.open(p).convert("RGBA")
        sh = sheets[f]
        box = (col * TILE, row * TILE, (col + w) * TILE, (row + h) * TILE)
        if box[2] > sh.width or box[3] > sh.height:
            print(f"{name}: {box} outside {f} ({sh.width}x{sh.height})", file=sys.stderr)
            return 1
        img = sh.crop(box).resize((w * TILE * scale, h * TILE * scale), Image.NEAREST)
        cells.append((f"{name} {w}x{h}", f"{f[:14]} ({col},{row})", img))

    per_row = 6
    cw = max(c[2].width for c in cells) + pad * 2
    ch = max(c[2].height for c in cells) + pad * 2 + label_h * 2
    rows = (len(cells) + per_row - 1) // per_row
    canvas = Image.new("RGBA", (cw * per_row, ch * rows), (26, 28, 32, 255))
    d = ImageDraw.Draw(canvas)
    fnt = _font(13)

    for i, (title, src, img) in enumerate(cells):
        ox = (i % per_row) * cw
        oy = (i // per_row) * ch
        d.rectangle([ox + pad, oy + pad, ox + pad + img.width, oy + pad + img.height], fill=grass)
        canvas.alpha_composite(img, (ox + pad, oy + pad))
        # Tile grid over the sprite, so a footprint that is one tile too wide is countable.
        for gx in range(1, img.width // (TILE * scale)):
            x = ox + pad + gx * TILE * scale
            d.line([x, oy + pad, x, oy + pad + img.height], fill=(255, 60, 60, 110))
        for gy in range(1, img.height // (TILE * scale)):
            y = oy + pad + gy * TILE * scale
            d.line([ox + pad, y, ox + pad + img.width, y], fill=(255, 60, 60, 110))
        ty = oy + pad + img.height + 2
        d.text((ox + pad, ty), title, font=fnt, fill=(255, 236, 180, 255))
        d.text((ox + pad, ty + label_h - 2), src, font=fnt, fill=(150, 160, 170, 255))

    canvas.save(out)
    print(f"{out}  {canvas.width}x{canvas.height}  ({len(cells)} candidates)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
