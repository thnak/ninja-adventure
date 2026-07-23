#!/usr/bin/env python3
"""Score how *composed* a screenshot looks, by the share of each tile a single colour owns.

Why this tool exists:

  The prefab rewrite (RENDER_SPEC / the R-series and the parcel pipeline) made a promise that has to
  be checkable, not asserted: the world should stop reading as a procedural noise-scatter and start
  reading like the pack author's own hand-composed map. "Reads composed" is a feeling; this tool is
  the number under the feeling, and it is deliberately the SAME number for the author's demo and for
  our game, so the two can be put in one column and compared.

  The measure is per-cell dominant-colour SHARE. Cut the playfield into 16px cells (one tile of the
  native art) and, in each cell, find the single most common colour and the fraction of the cell it
  covers. A cell that is a flat fill of grass or stone is almost one colour -> high share. A cell
  carrying an edge, a prop or a sprite is many colours -> low share. Three buckets, chosen once and
  never tuned per image:

      flat      share  > 0.85     a solid fill, nothing on it
      textured  0.55 < share <= 0.85   a fill with grain, or one contour through it
      busy      share <= 0.55     a prop, a sprite, a junction of edges -- authored detail

  A world of nothing but flat cells is the noise-scatter we are trying to leave behind; a world that
  matches the author sits with roughly half its cells busy.

The baseline every run prints for comparison is measured live from the pack author's own demo:

  `assets/_src/ninja/Example 1.gif` frame 0 -- one of the four `Example N.gif` clips recorded from
  `GodotProject.zip`, the same project our prefabs are cut from. The GIF is a 4x nearest-neighbour
  upscale of a 320x176 window (the project's own `window/size`), so it is downscaled by 4 back to
  native resolution FIRST -- nearest, to recover exact source colours rather than blend them -- and
  then measured at 16px cells with its top HUD row (hearts + resource bar) excluded. Those settings
  reproduce the recorded reference of flat 19% / textured 33% / busy 48%; if you change the method,
  that row is the regression test.

Game screenshots are rendered at 32px per tile (nearest, so a cell's colour histogram is unchanged
by the 2x upscale and the share is directly comparable), so they are measured at `--cell 32` with no
downscale. Our HUD -- the health/mana bars and the hotbar -- sits along the BOTTOM, so pass
`--hud-rows N` to drop that many 32px rows off the bottom; the only thing left up top is a one-cell
day label, too small to bother excluding.

    python3 tools/screen_metric.py docs/shot_meadow.png --hud-rows 2
    python3 tools/screen_metric.py shot_a.png shot_b.png --cell 32 --hud-rows 2

PIL + the standard library only; no numpy, because the cells are tiny and Counter is plenty.
"""
import argparse
import pathlib

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The pack author's own demo, and the settings that recover its native resolution and reference row.
AUTHOR_GIF = ROOT / "assets" / "_src" / "ninja" / "Example 1.gif"
AUTHOR_DOWNSCALE = 4   # the GIF is a 4x nearest upscale of the project's 320x176 window
AUTHOR_CELL = 16       # one tile of the native art
AUTHOR_TOP_ROWS = 1    # top HUD row: hearts on the left, resource bar on the right
AUTHOR_REFERENCE = (19, 33, 48)  # recorded flat / textured / busy, the regression target

# Bucket thresholds on a cell's dominant-colour share. Fixed; never tuned per image.
FLAT_ABOVE = 0.85
BUSY_AT_OR_BELOW = 0.55


def cell_shares(img, cell, top_rows=0, bottom_rows=0):
    """Yield the dominant-colour share of every whole `cell`x`cell` block of the playfield.

    `top_rows` / `bottom_rows` drop that many cell-rows off the top / bottom before measuring, which
    is how the HUD is kept out of the count. A partial row or column at the right/bottom edge that is
    not a whole cell wide is ignored -- a fractional cell has no honest dominant share.
    """
    rgb = img.convert("RGB")
    w, h = rgb.size
    y0 = top_rows * cell
    y1 = h - bottom_rows * cell
    for cy in range(y0, y1 - cell + 1, cell):
        for cx in range(0, w - cell + 1, cell):
            block = rgb.crop((cx, cy, cx + cell, cy + cell))
            # getcolors returns the whole (count, colour) histogram in one C-level pass; maxcolors is
            # the pixel count so it never overflows to None. The dominant colour is the largest count.
            histogram = block.getcolors(maxcolors=cell * cell)
            top_count = max(count for count, _colour in histogram)
            yield top_count / (cell * cell)


def bucket(shares):
    """Turn a stream of shares into (n, flat%, textured%, busy%) rounded to whole percent."""
    flat = textured = busy = 0
    for s in shares:
        if s > FLAT_ABOVE:
            flat += 1
        elif s <= BUSY_AT_OR_BELOW:
            busy += 1
        else:
            textured += 1
    n = flat + textured + busy
    if n == 0:
        return 0, 0, 0, 0
    return n, round(100 * flat / n), round(100 * textured / n), round(100 * busy / n)


def measure_image(path, cell, hud_rows):
    img = Image.open(path)
    return bucket(cell_shares(img, cell, bottom_rows=hud_rows))


def measure_author_baseline():
    """The reference row, measured live from the author's GIF; falls back to the recorded numbers."""
    if not AUTHOR_GIF.exists():
        f, t, b = AUTHOR_REFERENCE
        return None, f, t, b, True
    gif = Image.open(AUTHOR_GIF)
    gif.seek(0)
    native = gif.convert("RGB")
    w, h = native.size
    native = native.resize((round(w / AUTHOR_DOWNSCALE), round(h / AUTHOR_DOWNSCALE)), Image.NEAREST)
    return (*bucket(cell_shares(native, AUTHOR_CELL, top_rows=AUTHOR_TOP_ROWS)), False)


def _print_row(name, n, flat, textured, busy):
    cells = f"{n:>5}" if n else "    -"
    print(f"{name:<34}{cells}  {flat:>5}%  {textured:>8}%  {busy:>5}%")


def main():
    ap = argparse.ArgumentParser(
        description="Per-tile dominant-colour share of a screenshot, bucketed flat / textured / busy.")
    ap.add_argument("images", nargs="+", type=pathlib.Path, help="screenshots to score")
    ap.add_argument("--cell", type=int, default=32,
                    help="cell size in px; 32 for the game's 32px/tile screenshots (default)")
    ap.add_argument("--hud-rows", type=int, default=0,
                    help="cell-rows of HUD to drop off the BOTTOM (health bars + hotbar)")
    args = ap.parse_args()

    print(f"{'image':<34}{'cells':>5}  {'flat':>5}  {'textured':>8}  {'busy':>5}")
    print("-" * 66)
    for path in args.images:
        n, flat, textured, busy = measure_image(path, args.cell, args.hud_rows)
        _print_row(path.name, n, flat, textured, busy)

    n, flat, textured, busy, fallback = measure_author_baseline()
    label = "AUTHOR BASELINE (Example 1.gif)"
    if fallback:
        label += " [recorded]"
    print("-" * 66)
    _print_row(label, n, flat, textured, busy)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
