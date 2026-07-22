#!/usr/bin/env python3
"""Render a labelled, magnified crop of a spritesheet so tile indices can be read off by eye.

Kenney sheets come in two layouts and this handles both:
  * spaced  (roguelike packs)   16px tiles, 1px gutter  -> --stride 17
  * packed  (tiny_* tilemaps)   16px tiles, no gutter   -> --stride 16

Each cell is labelled `col,row` and, when --cols-per-row is given, also the linear tile index that
Kenney's own `tile_NNNN.png` filenames use.

    tools/atlas_preview.py SHEET.png OUT.png --rect C0 R0 C1 R1 [--stride 16] [--scale 4]
"""
import argparse

from PIL import Image, ImageDraw


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet")
    ap.add_argument("out")
    ap.add_argument("--rect", nargs=4, type=int, required=True, metavar=("C0", "R0", "C1", "R1"))
    ap.add_argument("--tile", type=int, default=16)
    ap.add_argument("--stride", type=int, default=17)
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--cols-per-row", type=int, default=0,
                    help="sheet width in tiles; enables linear index labels")
    a = ap.parse_args()

    c0, r0, c1, r1 = a.rect
    sheet = Image.open(a.sheet).convert("RGBA")
    cols, rows = c1 - c0 + 1, r1 - r0 + 1

    px = a.tile * a.scale
    cell = px + 20
    out = Image.new("RGBA", (cols * cell, rows * cell), (30, 30, 34, 255))
    draw = ImageDraw.Draw(out)

    for r in range(rows):
        for c in range(cols):
            sx, sy = (c0 + c) * a.stride, (r0 + r) * a.stride
            if sx + a.tile > sheet.width or sy + a.tile > sheet.height:
                continue
            ox, oy = c * cell, r * cell
            # Checkerboard so transparent pixels are unmistakable.
            for by in range(0, px, 8):
                for bx in range(0, px, 8):
                    shade = 70 if ((bx // 8 + by // 8) % 2 == 0) else 95
                    draw.rectangle([ox + bx, oy + by, ox + bx + 7, oy + by + 7],
                                   fill=(shade, shade, shade, 255))
            tile = sheet.crop((sx, sy, sx + a.tile, sy + a.tile)).resize((px, px), Image.NEAREST)
            out.alpha_composite(tile, (ox, oy))
            draw.rectangle([ox, oy, ox + px - 1, oy + px - 1], outline=(255, 0, 0, 110))

            label = f"{c0 + c},{r0 + r}"
            if a.cols_per_row:
                label += f" #{(r0 + r) * a.cols_per_row + (c0 + c)}"
            draw.text((ox + 2, oy + px + 4), label, fill=(255, 230, 120))

    out.save(a.out)
    print(f"{a.out}  cols {c0}..{c1} rows {r0}..{r1}  ({cols}x{rows})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
