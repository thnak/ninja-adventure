#!/usr/bin/env python3
"""Import AI-generated building art into atlas-ready sprites.

An image model returns a *painting* of pixel art, not pixel art. Measured on the sheet in
`tools/ai-generated/`: 171,001 unique colours, no alpha channel, and 92.6% of horizontal colour
runs are 1-2px wide at 1536x1024 — there is no pixel grid to recover, only a smooth image that
reads as pixel art at full size. None of that can enter `assets/atlas.png` as-is.

So this tool does the conformance pass, and the order of operations is the whole trick:

  1. CUT AT FULL RESOLUTION.  The alpha mask is thresholded against the background *before*
     downsampling, then the mask itself is downsampled and re-thresholded at 50%. Cutting after
     downsampling instead bakes background colour into the sprite's edge pixels.
  2. AREA-AVERAGE, DON'T LANCZOS.  Lanczos rings, and ringing on a 4x reduction shows up as light
     halos along every roof edge that no palette snap can remove. BOX is the honest average.
  3. SNAP, DON'T DITHER.  Error diffusion at 64x48 reads as dirt. Nearest-palette banding is what
     hand-drawn pixel art looks like anyway.
  4. REDRAW THE OUTLINE LAST.  The pack's houses all carry a 1px 141b1b silhouette (~300px each);
     downsampling always softens it, so it is re-stamped from the final alpha mask.

Buildings are found by connected components rather than by assuming a grid — the source sheet has
text labels down the left margin and unequal cell spacing, and a guessed 4x4 grid cropped two of
three test buildings straight through the middle.

    tools/import_ai.py SHEET.png --skip-rows 1        # row 1 of that sheet is the reference art
    tools/import_ai.py SHEET.png --report             # just list what was detected
"""
import argparse
import collections
import pathlib
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_vfx import checkerboard, snap  # noqa: E402

TILE = 16
OUTLINE = snap((0x14, 0x1B, 0x1B))
DETECT_SCALE = 4      # components are found on a /4 mask; 1.5M px of flood fill in Python is slow
FG_THRESH = 60        # sum |dR|+|dG|+|dB| above the background to count as foreground
MIN_AREA = 400        # in /4 space — drops the "WOOD" / "STONE" / "VARIATIONS" text labels
ROW_TOL = 60          # two components within this many source rows belong to the same shelf


def background_of(img):
    """The sheet's flat backdrop, taken as the modal colour."""
    return collections.Counter(img.get_flattened_data()).most_common(1)[0][0]


def foreground_mask(img, bg, thresh=FG_THRESH):
    px = img.load()
    w, h = img.size
    return [[sum(abs(a - b) for a, b in zip(px[x, y], bg)) > thresh for x in range(w)]
            for y in range(h)]


def components(mask, w, h):
    """Iterative 4-connected flood fill.

    Returns (boxes, labels). The label grid matters as much as the boxes: a bounding box is a
    rectangle, and cropping one on this sheet also scoops up whatever else happens to fall inside
    it — the "VARIATIONS" caption sits at the same rows as the building beside it and welded itself
    onto that sprite's corner. Membership is by component id, not by rectangle.
    """
    labels = [[0] * w for _ in range(h)]
    boxes = []
    nid = 0
    for sy in range(h):
        for sx in range(w):
            if not mask[sy][sx] or labels[sy][sx]:
                continue
            nid += 1
            stack, area = [(sx, sy)], 0
            labels[sy][sx] = nid
            x0 = x1 = sx
            y0 = y1 = sy
            while stack:
                x, y = stack.pop()
                area += 1
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)
                for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if 0 <= nx < w and 0 <= ny < h and mask[ny][nx] and not labels[ny][nx]:
                        labels[ny][nx] = nid
                        stack.append((nx, ny))
            if area >= MIN_AREA:
                boxes.append((x0, y0, x1, y1, area, {nid}))
    return boxes, labels


def merge_stacked(boxes, max_gap=4):
    """Rejoin a building that flood fill split into roof and wall.

    Several of these houses have an eave shadow dark enough to fall under FG_THRESH, which severs
    the roof from the wall and yields two components where there is one building. Anything sharing
    most of its horizontal span with a near neighbour directly above or below is the same house.

    `max_gap` is tight on purpose. A severed roof/wall pair is touching (measured gap 0), while two
    real buildings stacked in adjacent rows of this sheet are only ~13 mask-px apart — so a gap
    generous enough to feel safe silently welds every column into one tall blob.
    """
    boxes = list(boxes)
    changed = True
    while changed:
        changed = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                ax0, ay0, ax1, ay1, aa, aid = boxes[i]
                bx0, by0, bx1, by1, ba, bid = boxes[j]
                overlap = min(ax1, bx1) - max(ax0, bx0)
                if overlap < 0.5 * min(ax1 - ax0, bx1 - bx0):
                    continue
                gap = max(ay0, by0) - min(ay1, by1)
                if gap > max_gap:
                    continue
                boxes[i] = (min(ax0, bx0), min(ay0, by0), max(ax1, bx1), max(ay1, by1),
                            aa + ba, aid | bid)
                boxes.pop(j)
                changed = True
                break
            if changed:
                break
    return boxes


def shelve(boxes):
    """Group boxes into rows top-to-bottom, then order each row left-to-right."""
    rows = []
    for b in sorted(boxes, key=lambda b: b[1]):
        for row in rows:
            if abs(b[1] - row[0][1]) <= ROW_TOL // DETECT_SCALE:
                row.append(b)
                break
        else:
            rows.append([b])
    return [sorted(r, key=lambda b: b[0]) for r in rows]


def tile_size(bw, bh, tiles_tall):
    """Pick an atlas-aligned size that preserves the source aspect ratio."""
    h = tiles_tall * TILE
    w = max(2, min(6, round(bw / bh * tiles_tall))) * TILE
    return w, h


def belongs(labels, ids, x, y):
    """Is this full-res pixel part of the wanted component?

    Tested with a 3x3 dilation in label space because the label grid is DETECT_SCALE times coarser
    than the image; without it the sprite's outermost pixels fall in cells the coarse mask rounded
    to background and the silhouette gets shaved.
    """
    lx, ly = x // DETECT_SCALE, y // DETECT_SCALE
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if 0 <= ly + dy < len(labels) and 0 <= lx + dx < len(labels[0]):
                if labels[ly + dy][lx + dx] in ids:
                    return True
    return False


def fill_enclosed(a, close_bottom=True):
    """Re-solidify sprite pixels that the background threshold punched out.

    The sheet's backdrop is a dark (36,38,43), and any pixel in the art within FG_THRESH of it —
    roof-tile shadow lines, window recesses, the doorway — reads as background. Measured on the
    first import that was 6-26% of each sprite: transparent holes scattered through the roof, each
    one then getting its own 1px outline stamped around it by rule 4.

    A pixel is interior if the silhouette encloses it on all four sides. `close_bottom` treats the
    doorway, which runs off the bottom edge and so has nothing below it, as enclosed anyway — the
    pack fills its doorways solid rather than letting terrain show through.
    """
    w, h = a.size
    ap = a.load()
    rows = [[x for x in range(w) if ap[x, y]] for y in range(h)]
    cols = [[y for y in range(h) if ap[x, y]] for x in range(w)]
    lo_r = [(r[0], r[-1]) if r else None for r in rows]
    lo_c = [(c[0], c[-1]) if c else None for c in cols]
    for y in range(h):
        if lo_r[y] is None:
            continue
        for x in range(lo_r[y][0], lo_r[y][1] + 1):
            if ap[x, y] or lo_c[x] is None:
                continue
            top, bot = lo_c[x]
            if y > top and (y < bot or close_bottom):
                ap[x, y] = 255
    return a


def convert(img, mask, labels, box, ids, tiles_tall, ncolours):
    """Crop, cut, downsample and palette-lock one building. See the four rules in the docstring."""
    x0, y0, x1, y1 = box
    crop = img.crop((x0, y0, x1 + 1, y1 + 1))
    # rule 1: build the alpha mask at full resolution, then shrink the mask itself
    a = Image.new("L", crop.size, 0)
    ap = a.load()
    for y in range(crop.height):
        for x in range(crop.width):
            if mask[y0 + y][x0 + x] and belongs(labels, ids, x0 + x, y0 + y):
                ap[x, y] = 255

    a = fill_enclosed(a)                          # close the holes BEFORE the mask is downsampled

    w, h = tile_size(crop.width, crop.height, tiles_tall)
    small = crop.resize((w, h), Image.BOX)        # rule 2: area average, no ringing
    amask = a.resize((w, h), Image.BOX)
    # Collapse to a handful of source colours BEFORE snapping. Snapping a noisy 4x reduction
    # one pixel at a time scatters neighbouring pixels onto different palette entries and the
    # roof reads as static; median cut makes whole regions agree first. The pack's own houses
    # carry 5-12 colours, so this is also what "on style" means numerically.
    small = small.quantize(colors=ncolours, method=Image.MEDIANCUT).convert("RGB")

    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sp, mp = small.load(), amask.load()
    solid = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if mp[x, y] < 128:                     # binary alpha, thresholded at 50%
                continue
            solid[y][x] = True
            out.putpixel((x, y), snap(sp[x, y]) + (255,))  # rule 3: snap, never dither

    # rule 4: re-stamp the 1px silhouette outline from the final mask
    for y in range(h):
        for x in range(w):
            if not solid[y][x]:
                continue
            if any(not (0 <= x + dx < w and 0 <= y + dy < h and solid[y + dy][x + dx])
                   for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))):
                out.putpixel((x, y), OUTLINE + (255,))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet")
    ap.add_argument("--out", default=str(ROOT / "assets" / "_gen" / "buildings"))
    ap.add_argument("--skip-rows", type=int, default=0, help="leading rows to ignore (reference art)")
    ap.add_argument("--tiles-tall", type=int, default=3)
    ap.add_argument("--prefix", default="ai_house")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--colours", type=int, default=12,
                    help="median-cut target before the palette snap; the pack uses 5-12")
    ap.add_argument("--report", action="store_true", help="list detections and stop")
    args = ap.parse_args()

    img = Image.open(args.sheet).convert("RGB")
    bg = background_of(img)
    small = img.resize((img.width // DETECT_SCALE, img.height // DETECT_SCALE), Image.BOX)
    boxes, labels = components(foreground_mask(small, bg), small.width, small.height)
    rows = shelve(merge_stacked(boxes))

    print(f"background {bg}   detected {sum(len(r) for r in rows)} components "
          f"in {len(rows)} rows")
    for i, row in enumerate(rows):
        tag = "SKIP (reference)" if i < args.skip_rows else ""
        print(f"  row {i}: {len(row)} components  {tag}")
    if args.report:
        return

    full_mask = foreground_mask(img, bg)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    results = []
    for r, row in enumerate(rows):
        if r < args.skip_rows:
            continue
        for i, (x0, y0, x1, y1, _, ids) in enumerate(row):
            box = (x0 * DETECT_SCALE, y0 * DETECT_SCALE,
                   (x1 + 1) * DETECT_SCALE - 1, (y1 + 1) * DETECT_SCALE - 1)
            sprite = convert(img, full_mask, labels, box, ids, args.tiles_tall, args.colours)
            name = f"{args.prefix}_{r - args.skip_rows + 1}{chr(ord('a') + i)}"
            sprite.save(out / f"{name}.png")
            px = list(sprite.get_flattened_data())
            assert all(c[3] in (0, 255) for c in px), f"{name}: alpha must stay binary"
            ncol = len({c for c in px if c[3]})
            print(f"{name:16s} {sprite.width:2d}x{sprite.height:2d} "
                  f"({sprite.width // TILE}x{sprite.height // TILE} tiles)  {ncol} colours")
            results.append((name, sprite))

    s, pad = args.scale, 6
    cols = 4
    cw = max(im.width for _, im in results) * s + pad
    ch = max(im.height for _, im in results) * s + pad + 10
    sheet = checkerboard(cw * cols + pad, ch * ((len(results) + cols - 1) // cols) + pad)
    for i, (name, im) in enumerate(results):
        big = im.resize((im.width * s, im.height * s), Image.NEAREST)
        sheet.paste(big, (pad + (i % cols) * cw, pad + (i // cols) * ch), big)
    sheet.save(out / "preview.png")
    print(f"\n{len(results)} buildings -> {out}/preview.png")


if __name__ == "__main__":
    main()
