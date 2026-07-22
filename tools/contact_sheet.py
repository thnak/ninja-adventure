#!/usr/bin/env python3
"""Render a whole tileset as a labelled contact sheet so tiles can be identified by eye.

Unlike tools/atlas_preview.py (which magnifies a hand-picked rectangle), this renders an ENTIRE
sheet in row bands sized to stay legible, with `col,row` under every cell and a checkerboard behind
transparency. Feed the output straight into a vision-capable review pass.

    tools/contact_sheet.py --all --outdir /tmp/contact
    tools/contact_sheet.py SHEET.png OUT_PREFIX [--scale 5] [--rows-per-image 8]
"""
import argparse
import pathlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
TILESETS = ROOT / "assets" / "_src" / "ninja" / "Backgrounds" / "Tilesets"
TILE = 16


def _font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if pathlib.Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def render(sheet_path, out_prefix, scale=5, rows_per_image=8, label=True):
    sheet = Image.open(sheet_path).convert("RGBA")
    cols, rows = sheet.width // TILE, sheet.height // TILE
    px = TILE * scale
    lab_h = 14 if label else 0
    cell_w, cell_h = px + 4, px + 4 + lab_h
    font = _font(11)
    outs = []
    for band in range(0, rows, rows_per_image):
        br = min(rows_per_image, rows - band)
        img = Image.new("RGBA", (cols * cell_w + 8, br * cell_h + 8), (24, 24, 28, 255))
        d = ImageDraw.Draw(img)
        for r in range(br):
            for c in range(cols):
                ox, oy = 4 + c * cell_w, 4 + r * cell_h
                for by in range(0, px, 8):
                    for bx in range(0, px, 8):
                        s = 60 if ((bx // 8 + by // 8) % 2 == 0) else 100
                        d.rectangle([ox + bx, oy + by, ox + bx + 7, oy + by + 7],
                                    fill=(s, s, s, 255))
                sy = (band + r) * TILE
                t = sheet.crop((c * TILE, sy, c * TILE + TILE, sy + TILE))
                img.alpha_composite(t.resize((px, px), Image.NEAREST), (ox, oy))
                d.rectangle([ox - 1, oy - 1, ox + px, oy + px], outline=(200, 40, 40, 130))
                if label:
                    d.text((ox, oy + px + 1), f"{c},{band + r}", fill=(255, 226, 120), font=font)
        out = f"{out_prefix}_r{band}-{band + br - 1}.png"
        img.convert("RGB").save(out)
        outs.append(out)
        print(out, f"{cols}x{br}")
    return outs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", nargs="?")
    ap.add_argument("out_prefix", nargs="?")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--outdir", default="/tmp/contact")
    ap.add_argument("--scale", type=int, default=5)
    ap.add_argument("--rows-per-image", type=int, default=8)
    a = ap.parse_args()
    if a.all:
        od = pathlib.Path(a.outdir)
        od.mkdir(parents=True, exist_ok=True)
        for p in sorted(TILESETS.rglob("*.png")):
            render(p, str(od / p.stem), a.scale, a.rows_per_image)
    else:
        render(a.sheet, a.out_prefix, a.scale, a.rows_per_image)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
