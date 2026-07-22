#!/usr/bin/env python3
"""Measure what the Ninja Adventure demo GIFs do that our screenshots do not.

The pack ships four animated demos (`assets/_src/ninja/Example N.gif`) showing the art in a
running game. They are the only ground truth we have for how the author intends the tiles to be
ASSEMBLED, as opposed to what each tile looks like on its own -- and assembly is the thing our
world generator is getting wrong.

"Rigid" is a real, measurable property, not a matter of taste. This tool measures four things on
both the demos and `docs/shot_*.png`, so the gap can be argued about with numbers:

  grid_lock    Of all the pixels where the image changes colour, what fraction sit exactly on a
               32-pixel grid line? The idea was that art drawn with transition tiles has its
               boundaries wandering THROUGH tiles while flat fills pin every boundary to the grid.
               IT DOES NOT MEASURE THAT -- it measures the capture scale. Read the note below
               before quoting it, and read `capture_scale` before trusting any number here.

  block_reuse  Distinct 32x32 blocks divided by total 32x32 blocks. Both a demo and a screenshot
               reuse blocks -- that is what a tileset is for -- but a screenshot that reuses far
               MORE is stamping one fill tile over a whole region instead of varying it.

  hue_runs     Mean horizontal run length of a quantised hue channel. Long runs mean large flat
               areas of a single terrain; short runs mean texture and interleaving.

  density      Fraction of pixels that are edge pixels. How much is going on per unit area.

  lattice      Autocorrelation of the edge map at a 32-pixel shift, minus the mean over shifts
               1..48. This was added to catch props snapped to the tile grid, and it DOES NOT
               WORK for that -- the pack scores higher than we do (0.24 vs 0.04), because its
               dense interlocking forests are genuinely periodic at 32px while our sparse scenes
               have too few edges anywhere to correlate. It is reported because it is a real
               measurement of periodicity, but it is not evidence of rigidity in either
               direction. Read `grid_lock` for that. Left in rather than deleted so nobody
               re-derives it and re-draws the same wrong conclusion.

**`grid_lock` DOES separate the two populations perfectly, and it is measuring the wrong thing.**
Every demo frame lands in 0.059..0.062 and every world screenshot in 0.104..0.162, with no
overlap. That looked like the signature of missing transition tiles. It is not. It is the
signature of a 4x screen recording.

The four demo GIFs are nearest-neighbour upscales by exactly 4 -- `capture_scale()` measures 317
runs of 4 identical columns in Example 1 and only 2 runs of any other length. A colour change can
therefore only occur on one column in four. `grid_lock` counts a pixel as on-grid when x % 32 == 0
or (x + 1) % 32 == 0, i.e. at x = 0 and x = 31 -- and neither 0 nor 31 is in the demos' phase
class. The demos are ARITHMETICALLY PREVENTED from scoring on those columns, whatever they draw.

Two checks confirm it and neither depends on judgement:

  * Chance level for `grid_lock` at grid g is 1 - (1 - 2/g)^2: 0.121 at 32, 0.234 at 16, 0.062 at
    64. Our screenshots sit ON chance at every g (0.131 / 0.257 / 0.063). The demos sit at roughly
    half of chance at every g, with no peak at any particular g -- so there is no tile pitch being
    detected, just a phase the sampler cannot see.
  * A histogram of edge pixels by x % 32 shows the demos spiking 2.0-2.7x at 1, 5, 9, ... 29 and
    dropping to 0.5x everywhere else. Ours is flat.

No amount of transition-tile work can move our screenshots to 0.06. Rendering at integer zoom and
capturing at 4x would, without changing a single tile.

`density` is the metric that DID track the real defect, and it moved: the pack averages 0.096, we
averaged 0.150 while looking emptier -- one grass tuft stamped on every grass tile -- and after
the fix we sit at 0.096. The other three do not discriminate:
  - `block_reuse` came out 0.581 pack vs 0.593 ours -- we are marginally MORE varied.
  - `hue_runs` came out 18.2 pack vs 13.6 ours -- nominally in our favour.
  - `lattice` is described above and does not work.
They are kept so that the next person to look does not re-derive them and mistake noise for a
finding. `grid_lock` is kept for the same reason, now with the trap written down next to it.

Usage:
    python3 tools/study_examples.py                    # measure + write contact sheets
    python3 tools/study_examples.py --no-sheets        # numbers only
    python3 tools/study_examples.py --out DIR

Nothing here is imported by the game. This is a measuring instrument.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from collections import Counter

from PIL import Image, ImageSequence

ROOT = pathlib.Path(__file__).resolve().parent.parent
NINJA = ROOT / "assets" / "_src" / "ninja"
DOCS = ROOT / "docs"

# The demos are recorded at 2x (a 16px tile occupies 32 screen px) and so are our screenshots,
# so both are measured against the same 32px lattice. If either ever changes zoom this constant
# is the thing that has to move -- see `--grid`.
GRID = 32

# A colour step this big or bigger counts as a boundary. Low enough to catch a grass/dirt seam,
# high enough to ignore the pack's own within-tile dithering and the GIF's palette noise.
EDGE_THRESH = 40


# --- measurement -------------------------------------------------------------


def edge_map(im: Image.Image) -> tuple[list[list[bool]], int, int]:
    """Mark every pixel whose left or top neighbour differs by more than EDGE_THRESH.

    Manhattan distance in RGB, not Euclidean: it is cheaper and, for the flat posterised palette
    this pack uses, it separates "different terrain" from "next shade of the same ramp" just as
    well.
    """
    px = im.convert("RGB").load()
    w, h = im.size
    e = [[False] * w for _ in range(h)]
    for y in range(1, h):
        for x in range(1, w):
            c = px[x, y]
            l = px[x - 1, y]
            u = px[x, y - 1]
            dl = abs(c[0] - l[0]) + abs(c[1] - l[1]) + abs(c[2] - l[2])
            du = abs(c[0] - u[0]) + abs(c[1] - u[1]) + abs(c[2] - u[2])
            e[y][x] = dl >= EDGE_THRESH or du >= EDGE_THRESH
    return e, w, h


def grid_lock(e: list[list[bool]], w: int, h: int, grid: int) -> float:
    """Fraction of edge pixels sitting on a grid line.

    A pixel counts as on-grid if its x or y is a multiple of `grid` or one less -- a seam between
    two tiles paints the last column of one and the first of the next, so both sides belong to
    the seam.
    """
    on = total = 0
    for y in range(h):
        y_on = y % grid == 0 or (y + 1) % grid == 0
        row = e[y]
        for x in range(w):
            if not row[x]:
                continue
            total += 1
            if y_on or x % grid == 0 or (x + 1) % grid == 0:
                on += 1
    return on / total if total else 0.0


def block_reuse(im: Image.Image, grid: int) -> tuple[float, int, int]:
    """Distinct grid-sized blocks over total blocks. Lower means more repetition."""
    px = im.convert("RGB").load()
    w, h = im.size
    seen: set[bytes] = set()
    n = 0
    for by in range(0, h - grid + 1, grid):
        for bx in range(0, w - grid + 1, grid):
            buf = bytearray()
            for y in range(by, by + grid):
                for x in range(bx, bx + grid):
                    buf += bytes(px[x, y])
            seen.add(bytes(buf))
            n += 1
    return (len(seen) / n if n else 0.0), len(seen), n


def lattice_score(e: list[list[bool]], w: int, h: int, grid: int) -> float:
    """How much more the edge map agrees with itself at a `grid` shift than at a typical shift.

    Props snapped to the tile lattice make the edge map periodic at exactly `grid`. Props placed
    at arbitrary offsets do not. Subtracting the mean over 1..48 removes the baseline agreement
    that any image has with a shifted copy of itself.
    """

    def agree(dx: int) -> float:
        hit = tot = 0
        for y in range(0, h, 2):  # every other row; the statistic is stable and this is 2x faster
            row = e[y]
            for x in range(0, w - dx):
                if row[x]:
                    tot += 1
                    if row[x + dx]:
                        hit += 1
        return hit / tot if tot else 0.0

    at_grid = agree(grid)
    others = [agree(d) for d in range(1, 49) if d % grid != 0]
    return at_grid - (sum(others) / len(others))


def hue_runs(im: Image.Image) -> float:
    """Mean horizontal run length of a coarsely quantised colour.

    Quantising to 5 bits per channel collapses the pack's shading ramps so a run measures "same
    terrain", not "same pixel".
    """
    px = im.convert("RGB").load()
    w, h = im.size
    runs = 0
    total = 0
    for y in range(0, h, 2):
        prev = None
        for x in range(w):
            c = px[x, y]
            q = (c[0] >> 5, c[1] >> 5, c[2] >> 5)
            if q != prev:
                runs += 1
                prev = q
            total += 1
    return total / runs if runs else 0.0


def measure(im: Image.Image, grid: int) -> dict:
    im = im.convert("RGB")
    e, w, h = edge_map(im)
    reuse, distinct, blocks = block_reuse(im, grid)
    n_edge = sum(sum(1 for v in row if v) for row in e)
    return {
        "size": [w, h],
        "grid_lock": round(grid_lock(e, w, h, grid), 4),
        "block_reuse": round(reuse, 4),
        "distinct_blocks": distinct,
        "blocks": blocks,
        "density": round(n_edge / (w * h), 4),
        "lattice": round(lattice_score(e, w, h, grid), 4),
        "hue_runs": round(hue_runs(im), 2),
        # Reported alongside every other number because it is the one that decides whether
        # `grid_lock` means anything for this image. See the module docstring.
        "capture_scale": capture_scale(im),
        # The unconfounded one. See `shoreline_lock`; nan when the frame has no water.
        "shoreline_lock": round(shoreline_lock(im, grid)[0], 4),
    }


# --- frame extraction --------------------------------------------------------


def demo_frames(path: pathlib.Path, count: int) -> list[Image.Image]:
    """Pull `count` frames spread evenly across a demo GIF.

    Evenly spread, not the first N: the demos pan across the map, so the first N frames all show
    the same corner and would measure one scene rather than the whole demo.
    """
    im = Image.open(path)
    frames = [f.convert("RGB") for f in ImageSequence.Iterator(im)]
    if len(frames) <= count:
        return frames
    step = len(frames) / count
    return [frames[int(i * step)] for i in range(count)]


def crop_hud(im: Image.Image) -> Image.Image:
    """Drop the top and bottom strips.

    Both the demos and our screenshots draw hearts, a health bar and a hotbar over the world.
    Those are UI, they are identical between frames, and they are drawn on an exact pixel grid --
    so leaving them in inflates `grid_lock` for BOTH sides and hides the difference we care about.
    """
    w, h = im.size
    return im.crop((0, int(h * 0.12), w, int(h * 0.88)))


# --- reporting ---------------------------------------------------------------


def shoreline_lock(im: Image.Image, grid: int = GRID) -> tuple[float, int]:
    """Is the water's edge a staircase pinned to the tile grid, or a coastline that wanders?

    Returns (peak share of one phase, number of boundary crossings). Chance is 1/grid = 0.031, and
    1.0 means every step of the shoreline lies on the same column of the tile lattice.

    TWO THINGS THIS DOES THAT `grid_lock` DOES NOT, and they are why it works where that fails:

      * **It fits the phase instead of assuming it.** `grid_lock` counts a pixel as on-grid when
        x % 32 == 0. The camera is centred on a player standing at a FRACTIONAL tile position, so
        the tile lattice lands at an arbitrary screen offset and that test checks a lattice which
        is not there. Taking the peak over all 32 phases asks the question that was meant.
      * **It looks only at a terrain boundary**, found by colour, so decoration cannot move it.
        `grid_lock` is a ratio over every edge in the image, which means removing grass tufts
        changes it without a single boundary moving.

    Water is the classifier because it is the one terrain in this pack no other can be confused
    with. A frame with too little water returns (nan, n) rather than a number nobody should quote.
    """
    # No HUD crop here: every caller passes an image `crop_hud` has already trimmed. Cropping twice
    # was the first version, and it cut the meadow's pond down to under the 200-crossing floor, so
    # the scene with the clearest before/after in the whole set reported `nan`.
    px = im.convert("RGB").load()
    w, h = im.size

    def wet(p: tuple[int, int, int]) -> bool:
        return p[2] > 195 and p[1] > 165 and p[0] < 200

    hist = [0] * grid
    total = 0
    for y in range(1, h):
        for x in range(1, w):
            if wet(px[x, y]) != wet(px[x - 1, y]):
                hist[x % grid] += 1
                total += 1
    if total < 120:
        return float("nan"), total
    return max(hist) / total, total


def capture_scale(im: Image.Image) -> int:
    """The integer factor this image was upscaled by, read off its own pixels.

    A nearest-neighbour upscale by N turns every source pixel into an NxN block, so the image is
    made of runs of exactly N identical columns. Measuring the modal run length recovers N.

    THIS IS THE MOST IMPORTANT FUNCTION IN THE FILE, and it exists because it invalidates the
    conclusion the file was originally written to support. See the module docstring.
    """
    px = im.convert("RGB").load()
    w, h = im.size
    runs: dict[int, int] = {}
    cur = 1
    for x in range(1, w):
        if all(px[x, y] == px[x - 1, y] for y in range(0, h, 7)):
            cur += 1
        else:
            runs[cur] = runs.get(cur, 0) + 1
            cur = 1
    runs[cur] = runs.get(cur, 0) + 1
    return max(runs, key=lambda k: runs[k]) if runs else 1


def contact_sheet(frames: list[Image.Image], out: pathlib.Path, cols: int = 2) -> None:
    if not frames:
        return
    fw, fh = frames[0].size
    scale = 640 / fw
    tw, th = int(fw * scale), int(fh * scale)
    rows = (len(frames) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tw, rows * th), (20, 20, 24))
    for i, f in enumerate(frames):
        sheet.paste(f.resize((tw, th), Image.NEAREST), ((i % cols) * tw, (i // cols) * th))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tools/_study")
    ap.add_argument("--grid", type=int, default=GRID)
    ap.add_argument("--frames", type=int, default=4, help="frames sampled per demo GIF")
    ap.add_argument("--no-sheets", action="store_true")
    args = ap.parse_args()

    out = ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)

    report: dict[str, dict] = {}

    print(f"{'source':28} {'scale':>6} {'shore':>7} {'grid_lock':>10} {'reuse':>8} "
          f"{'density':>8} {'runs':>7} {'lattice':>8}")
    print("-" * 92)

    def emit(label: str, m: dict) -> None:
        print(
            f"{label:28} {m['capture_scale']:>5}x {m['shoreline_lock']:>7.3f} "
            f"{m['grid_lock']:>10.3f} {m['block_reuse']:>8.3f} {m['density']:>8.3f} "
            f"{m['hue_runs']:>7.2f} {m['lattice']:>8.3f}"
        )

    for n in (1, 2, 3, 4):
        gif = NINJA / f"Example {n}.gif"
        if not gif.exists():
            print(f"  (missing {gif.name})", file=sys.stderr)
            continue
        frames = demo_frames(gif, args.frames)
        if not args.no_sheets:
            contact_sheet(frames, out / f"example{n}_frames.png")
            frames[0].save(out / f"example{n}_f0.png")
        per = [measure(crop_hud(f), args.grid) for f in frames]
        avg = {
            k: round(sum(p[k] for p in per) / len(per), 4)
            for k in ("grid_lock", "block_reuse", "density", "lattice", "hue_runs",
                      "shoreline_lock")
        }
        avg["capture_scale"] = per[0]["capture_scale"]
        report[f"example{n}"] = {"frames": len(per), **avg}
        emit(f"Example {n}.gif", avg)

    print()
    for shot in sorted(DOCS.glob("shot_*.png")):
        m = measure(crop_hud(Image.open(shot)), args.grid)
        report[shot.stem] = m
        emit(shot.name, m)

    # shot_menu and shot_journal are UI screens, not world. Averaging them in drags every
    # statistic towards "large flat panels of one colour" and says nothing about terrain.
    UI_ONLY = {"shot_menu", "shot_journal"}
    ex = [v for k, v in report.items() if k.startswith("example")]
    sh = [v for k, v in report.items() if k.startswith("shot_") and k not in UI_ONLY]
    if ex and sh:
        print()
        print("verdict  (world scenes only; menu and journal excluded)")
        print("-" * 76)
        for key, direction in (
            ("grid_lock", "LOWER IS BETTER -- boundaries pinned to the tile grid"),
            ("block_reuse", "not discriminating"),
            ("density", "not discriminating -- see docstring"),
            ("hue_runs", "not discriminating"),
            ("lattice", "not discriminating"),
        ):
            a = sum(v[key] for v in ex) / len(ex)
            b = sum(v[key] for v in sh) / len(sh)
            lo_e, hi_e = min(v[key] for v in ex), max(v[key] for v in ex)
            lo_s, hi_s = min(v[key] for v in sh), max(v[key] for v in sh)
            # Separation only means something if the two RANGES do not overlap. Comparing means
            # alone would call a 0.01 gap between two wide, interleaved spreads a finding.
            sep = "SEPARATED" if hi_e < lo_s or hi_s < lo_e else "overlaps"
            print(
                f"  {key:12} pack {a:7.3f} [{lo_e:.3f}-{hi_e:.3f}]   "
                f"ours {b:7.3f} [{lo_s:.3f}-{hi_s:.3f}]   {sep:10} ({direction})"
            )

    (out / "study.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nwrote {out / 'study.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
