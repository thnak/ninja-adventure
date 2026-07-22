#!/usr/bin/env python3
"""Compute per-tile numeric metrics for every 16x16 tile of every Ninja Adventure tileset.

This is the *measurement* half of the tile catalogue (`assets/tile_index.json`). It never guesses
what a tile depicts — it only reports facts a computer can be trusted with:

  opaque           every pixel alpha == 255
  self_contained   all four 1px borders fully transparent  -> the sprite fits inside one tile
  alpha_fraction   fraction of pixels with alpha > 0
  mean_rgb         mean colour over non-transparent pixels
  stddev           per-channel stddev over non-transparent pixels, averaged. ~0 == flat solid fill

The naming/description/role half is done by eye from the contact sheets (tools/contact_sheet.py)
and lives in tools/tile_labels.py.

    tools/tile_metrics.py [--out metrics.json]
"""
import argparse
import json
import pathlib

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
TILESETS = ROOT / "assets" / "_src" / "ninja" / "Backgrounds" / "Tilesets"
TILE = 16


def _mean_rgb(px):
    """Mean RGB of the non-transparent pixels of a slice; transparent slices report None."""
    px = px.reshape(-1, 4)
    nz = px[:, 3] > 0
    if not nz.any():
        return None
    return [int(round(v)) for v in px[nz][:, :3].astype(np.float32).mean(axis=0)]


def _largest_blob(mask):
    """Size in pixels of the largest 4-connected True region. Plain flood fill; 16x16 is tiny."""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    best = 0
    for sy in range(h):
        for sx in range(w):
            if not mask[sy, sx] or seen[sy, sx]:
                continue
            stack, size = [(sy, sx)], 0
            seen[sy, sx] = True
            while stack:
                y, x = stack.pop()
                size += 1
                for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            best = max(best, size)
    return best


def sheet_paths():
    return sorted(TILESETS.rglob("*.png"), key=lambda p: str(p.relative_to(TILESETS)))


def measure_sheet(path):
    """Return (meta, [tile-metric dicts]) for one sheet, indexing only whole 16x16 rows/cols."""
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    a = np.asarray(im, dtype=np.uint8)
    cols, rows = w // TILE, h // TILE
    meta = {
        "sheet": path.name,
        "path": str(path.relative_to(ROOT)),
        "width": w,
        "height": h,
        "cols": cols,
        "rows": rows,
        "tiles": cols * rows,
    }
    # Sheets whose pixel height is not a multiple of 16 have a partial bottom row; it is dropped.
    if h % TILE or w % TILE:
        meta["partial"] = {
            "note": "dimension not a multiple of 16; only whole rows/cols indexed",
            "leftover_px_x": w % TILE,
            "leftover_px_y": h % TILE,
        }

    out = []
    for r in range(rows):
        for c in range(cols):
            t = a[r * TILE:(r + 1) * TILE, c * TILE:(c + 1) * TILE]
            al = t[:, :, 3]
            rgb = t[:, :, :3].astype(np.float32)
            nz = al > 0
            n = int(nz.sum())
            afrac = n / float(TILE * TILE)
            if n:
                px = rgb[nz]
                mean = px.mean(axis=0)
                std = float(px.std(axis=0).mean())
                mean_rgb = [int(round(v)) for v in mean]
            else:
                mean_rgb, std = [0, 0, 0], 0.0
            borders_clear = bool(
                al[0, :].max() == 0 and al[-1, :].max() == 0
                and al[:, 0].max() == 0 and al[:, -1].max() == 0
            )
            # Texture vs. edge: a tileable textured fill is the dominant colour plus a few tiny
            # specks; a transition/edge tile is the dominant colour plus one big contiguous shape.
            # So measure how much is non-dominant AND how big the largest non-dominant blob is.
            flat = t.reshape(-1, 4)
            uniq, counts = np.unique(flat, axis=0, return_counts=True)
            dom = uniq[counts.argmax()]
            minor = ~np.all(t == dom, axis=2)
            minor_frac = float(minor.mean())
            max_blob = _largest_blob(minor)
            if minor.any():
                ys, xs = np.nonzero(minor)
                cy, cx = float(ys.mean()), float(xs.mean())
            else:
                cy = cx = (TILE - 1) / 2.0
            out.append({
                "sheet": path.name,
                "col": c,
                "row": r,
                "opaque": bool(al.min() == 255),
                "self_contained": borders_clear,
                "mean_rgb": mean_rgb,
                "stddev": round(std, 2),
                "alpha_fraction": round(afrac, 4),
                # helper signals, not emitted in the final index
                "_edge_alpha": [int(al[0, :].max()), int(al[-1, :].max()),
                                int(al[:, 0].max()), int(al[:, -1].max())],
                "_ncolors": int(len(uniq)),
                "_minor_frac": round(minor_frac, 4),
                "_max_blob": int(max_blob),
                "_minor_cy": round(cy, 2),
                "_minor_cx": round(cx, 2),
                "_dom_rgb": [int(dom[0]), int(dom[1]), int(dom[2])],
                # Mean colour of each 1px border and of the inner 8x8, so the index builder can
                # say WHICH side of a tile shows a different terrain - a measured statement about
                # edge direction rather than a guess from where the minority pixels average out.
                "_sides": [_mean_rgb(t[0, :]), _mean_rgb(t[-1, :]),
                           _mean_rgb(t[:, 0]), _mean_rgb(t[:, -1])],
                "_center": _mean_rgb(t[4:12, 4:12].reshape(-1, 4)),
            })
    return meta, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "tools" / "_tile_metrics.json"))
    a = ap.parse_args()
    sheets, tiles = [], []
    for p in sheet_paths():
        m, ts = measure_sheet(p)
        sheets.append(m)
        tiles.extend(ts)
        print(f"{m['sheet']:32s} {m['width']}x{m['height']} {m['cols']}x{m['rows']} "
              f"= {m['tiles']} tiles{'  PARTIAL' if 'partial' in m else ''}")
    print(f"TOTAL {len(tiles)} tiles across {len(sheets)} sheets")
    pathlib.Path(a.out).write_text(json.dumps({"sheets": sheets, "tiles": tiles}))
    print("wrote", a.out)


if __name__ == "__main__":
    raise SystemExit(main())
