#!/usr/bin/env python3
"""Turn the pack author's hand-authored maps into a symbol grid a statistical model can learn from.

This is step 1 of "learn the author's map-design style and synthesise new scenes in it". Steps 2
and 3 (learn + generate) live in `tools/style_wfc.py`; this file is the corpus and the renderer,
and it is imported by that one rather than duplicated.

WHAT A SYMBOL IS, and why it is the whole design decision
---------------------------------------------------------
The author does not paint one tile per cell. He paints a STACK: a floor tile, then maybe a snow
tile over it, then maybe a bush from the Element layer on top. Village.tscn has five such layers.

The naive move is to learn each layer independently and overlay the results. That destroys the
style outright -- the thing that makes his maps read as composed is precisely that a bush appears
*on grass, near a fence*, and a layer-independent model puts bushes on water. So the alphabet here
is the **whole vertical stack, as one indivisible symbol**. 2453 occupied cells collapse to 394
distinct stacks, which is a rich but learnable alphabet, and every co-occurrence rule the author
followed between layers is baked into the symbol instead of having to be re-learned as a
constraint.

`0` is reserved for VOID -- a cell the author left empty. Void is a real symbol and is learned like
any other, because "where the map stops" is part of the composition: the map's ragged tree-lined
border is not a bounding box, it is drawn.

WHAT IS DELIBERATELY THROWN AWAY
--------------------------------
Nothing about the tiles themselves. `(id, ax, ay, flip_h, flip_v, transpose)` is kept per layer, so
a symbol round-trips to the exact pixels the author drew -- `--verify` re-renders the grid and
diffs it against `import_prefabs.composite()` to prove it. If that diff is ever non-zero the
learned model is learning something that is not the author's map.

The decode/tileset/blit machinery is imported wholesale from `tools/import_prefabs.py`, including
its one measured subtlety about `cell_tile_origin` on the House layer. That tool is the verified
reader for this scene and re-deriving it here would only create a second thing to keep in sync.

Usage:
    python tools/style_corpus.py                  # extract, verify, write tools/_style/corpus.npz
    python tools/style_corpus.py --verify         # + round-trip pixel diff against import_prefabs
    python tools/style_corpus.py --render         # + corpus.png redrawn from the symbol grid alone

Nothing here is imported by the game. This is build-time analysis tooling (ARCHITECTURE.md's rule
for Python), and it writes only into the gitignored tools/_style/.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import import_prefabs as ip  # noqa: E402  (needs the sys.path line above)

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "tools" / "_style"

TILE = ip.TILE
VOID = 0  # symbol id for "the author drew nothing here" -- see the module docstring


def extract(pack: ip.Pack) -> tuple[np.ndarray, list, dict]:
    """Village.tscn -> (grid, palette, meta).

    `grid` is (H, W) int32 of symbol ids, `palette[i]` is the stack symbol `i` denotes: a list of
    `[layer_name, id, ax, ay, fh, fv, tr]`, ordered bottom-to-top by the layer's draw order.
    `palette[0]` is the empty list, i.e. VOID.
    """
    layers, X0, Y0, X1, Y1 = ip.load_layers(pack)
    W, H = X1 - X0 + 1, Y1 - Y0 + 1

    # Bottom-to-top draw order is already what load_layers returns (sorted by z_index, stable).
    order = [name for name, _ts, _c, _o, _z, _y in layers]

    stacks: dict[tuple[int, int], dict] = {}
    for name, _ts, cells, _origin, _z, _ysort in layers:
        for c in cells:
            key = (c["x"] - X0, c["y"] - Y0)
            stacks.setdefault(key, {})[name] = (
                c["id"], c["ax"], c["ay"], int(c["fh"]), int(c["fv"]), int(c["tr"])
            )

    palette: list = [[]]          # index 0 == VOID
    index: dict[tuple, int] = {(): VOID}
    grid = np.zeros((H, W), dtype=np.int32)
    for (x, y), by_layer in stacks.items():
        # Canonicalise on DRAW ORDER, not dict insertion order, so the same visual stack always
        # hashes to the same symbol regardless of which layer happened to be parsed first.
        sym = tuple(
            (nm, *by_layer[nm]) for nm in order if nm in by_layer
        )
        if sym not in index:
            index[sym] = len(palette)
            palette.append([list(t) for t in sym])
        grid[y, x] = index[sym]

    meta = {
        "source": ip.SCENE,
        "layer_order": order,
        "map_origin": [X0, Y0],
        "size": [W, H],
        "symbols": len(palette),
        "occupied": int((grid != VOID).sum()),
    }
    return grid, palette, meta


def layer_meta(pack: ip.Pack) -> dict:
    """Layer name -> (tileset, origin, ysort), everything `render` needs to draw a symbol."""
    layers, _X0, _Y0, _X1, _Y1 = ip.load_layers(pack)
    return {name: (ts, origin, ysort) for name, ts, _cells, origin, _z, ysort in layers}


def render(grid: np.ndarray, palette: list, meta: dict, lmeta: dict, pad: int = 0) -> Image.Image:
    """Draw a symbol grid back to pixels using the author's own tilesets.

    This is the function that makes a GENERATED grid viewable, which is the entire point of the
    exercise -- and because it is the same code path that redraws the CORPUS grid, `--verify` can
    prove it faithful before any generated map is trusted.

    Y-sorted layers are drawn in a separate pass ordered by row, exactly as the author's engine
    does it, so a house's roof overlaps the row behind it instead of being sliced by it.
    """
    H, W = grid.shape
    img = Image.new("RGBA", ((W + pad * 2) * TILE, (H + pad * 2) * TILE), (0, 0, 0, 255))

    for name in meta["layer_order"]:
        ts, origin, ysort = lmeta[name]
        todo = []
        for y in range(H):
            for x in range(W):
                s = int(grid[y, x])
                if s == VOID:
                    continue
                for entry in palette[s]:
                    if entry[0] != name:
                        continue
                    _nm, tid, ax, ay, fh, fv, tr = entry
                    todo.append((y, x, dict(id=tid, ax=ax, ay=ay,
                                            fh=bool(fh), fv=bool(fv), tr=bool(tr))))
        if ysort:
            todo.sort(key=lambda t: t[0])
        for y, x, cell in todo:
            ip.blit(img, ts, cell, origin, (x + pad) * TILE, (y + pad) * TILE)
    return img


def verify(pack: ip.Pack, grid: np.ndarray, palette: list, meta: dict, lmeta: dict) -> int:
    """Round-trip check: does the symbol grid redraw to the same pixels as the original scene?

    Compares against `import_prefabs.composite()`, the renderer that produced the prefabs already
    shipping in the game, so a pass here means the corpus is the author's map and not a lossy
    paraphrase of it.
    """
    layers, X0, Y0, X1, Y1 = ip.load_layers(pack)
    want = ip.composite(layers, X0, Y0, X1, Y1)
    # composite() pads by ip.PAD on every side; match it so the two images are comparable.
    got = render(grid, palette, meta, lmeta, pad=ip.PAD)

    if want.size != got.size:
        print(f"  FAIL size {want.size} != {got.size}")
        return 1
    a = np.asarray(want.convert("RGBA"), dtype=np.int16)
    b = np.asarray(got.convert("RGBA"), dtype=np.int16)
    diff = np.abs(a - b).sum(axis=2)
    bad = int((diff > 0).sum())
    total = diff.size
    if bad:
        ys, xs = np.nonzero(diff)
        print(f"  FAIL {bad}/{total} pixels differ ({bad / total:.4%}); "
              f"first at px ({xs[0]}, {ys[0]})")
        Image.fromarray((diff > 0).astype(np.uint8) * 255).save(OUT / "verify_diff.png")
        print(f"  wrote {OUT / 'verify_diff.png'}")
        return 1
    print(f"  OK  round-trip is pixel-exact over {total} pixels")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true", help="pixel-diff the round trip")
    ap.add_argument("--render", action="store_true", help="write corpus.png from the grid alone")
    ap.add_argument("--out", default="tools/_style")
    args = ap.parse_args()

    out = ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)
    globals()["OUT"] = out

    print(f"reading {ip.SCENE}")
    pack = ip.Pack()
    grid, palette, meta = extract(pack)
    lmeta = layer_meta(pack)

    H, W = grid.shape
    occ = meta["occupied"]
    print(f"\ngrid {W}x{H} = {W * H} cells, {occ} occupied ({occ / (W * H):.1%}), "
          f"{len(palette)} distinct stack symbols (incl. VOID)")

    counts = np.bincount(grid.ravel(), minlength=len(palette))
    print("\nmost common symbols:")
    for s in np.argsort(-counts)[:8]:
        s = int(s)
        desc = "VOID" if s == VOID else " + ".join(f"{e[0]}#{e[1]}" for e in palette[s])
        print(f"  {counts[s]:5}  sym{s:<4} {desc}")

    np.savez_compressed(out / "corpus.npz", grid=grid)
    (out / "corpus.json").write_text(
        json.dumps({"meta": meta, "palette": palette}, indent=1) + "\n"
    )
    print(f"\nwrote {out / 'corpus.npz'} and {out / 'corpus.json'}")

    rc = 0
    if args.verify:
        print("\nround-trip verification:")
        rc = verify(pack, grid, palette, meta, lmeta)

    if args.render:
        render(grid, palette, meta, lmeta).convert("RGB").save(out / "corpus.png")
        print(f"wrote {out / 'corpus.png'}")

    return rc


if __name__ == "__main__":
    raise SystemExit(main())
