#!/usr/bin/env python3
"""Synthesise new scenes in the author's style by non-parametric neighbourhood sampling.

The second generator, and on this corpus the one that works. `tools/style_wfc.py` is the first;
read its docstring for the shared setup (the symbol grid, the corpus, the metrics). This file
exists because WFC has a specific, measured failure on a corpus this small, and the fix is a
different algorithm rather than another knob.

WHY WFC FAILS HERE, stated as the measurement rather than an opinion
--------------------------------------------------------------------
The alphabet is a whole vertical tile STACK, and there are 395 of them across only 2453 painted
cells. At n=3 that yields 2268 patterns from 4224 windows: 2086 of them occur exactly ONCE, and
the median pattern has ONE legal neighbour per direction. A pattern that occurs once has almost no
combinatorial freedom -- placing it forces a chain of unique continuations that usually ends in a
contradiction. So the assignments that survive to be complete solutions are, by survivorship, the
repetitive ones: every WFC run on the forest region collapsed into the one self-tiling brick
texture in its corner, scoring 3.17 nats of symbol-KL against a region whose own score is 0.

This is not a tuning failure. WFC demands EXACT pattern matches, and exact matching is exactly what
a corpus of mostly-unique patterns cannot supply. Symbol-KL against the author's own region, 28x28
output, 6 seeds each, lower is better:

    region     this file        style_wfc.py
    village    0.353 +- 0.027   1.556 +- 0.080
    forest     1.086 +- 0.542   2.922 +- 0.277
    fort       0.583 +- 0.243   2.150 +- 0.188

WHAT THIS DOES INSTEAD (Efros-Leung non-parametric sampling)
-----------------------------------------------------------
Fill the output one cell at a time in raster order. For each cell, take the neighbourhood already
decided around it, find every position in the author's map whose neighbourhood BEST MATCHES it,
and copy the symbol from the centre of one of those, chosen at random among the near-ties.

The single difference that matters: *best* match, not *exact* match. There is always a best match,
so this algorithm cannot contradict and never needs a restart -- and when it is asked for something
the author never drew, it degrades to his closest situation instead of failing. Frequency comes out
right for free, because a situation he drew often has many matching positions and is sampled often.

The cost is the honest one: it has no global consistency guarantee. WFC promises every NxN window
is one the author drew; this promises only that every cell is a plausible continuation of its own
neighbourhood. On this corpus that trade buys a working generator, which is the better deal --
`--compare` runs both and prints the two KL scores side by side.

Usage:
    python tools/style_corpus.py                                  # build the corpus first
    python tools/style_synth.py --region 0 33 34 17 --size 32 32  # a new forest
    python tools/style_synth.py --region 20 24 22 18 --seed 3     # a new village street
    python tools/style_synth.py --compare --region 0 33 34 17     # this vs WFC, scored

Writes only into the gitignored tools/_style/. Nothing here is imported by the game.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import import_prefabs as ip  # noqa: E402
import style_corpus as sc  # noqa: E402
import style_wfc as sw  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent

UNKNOWN = -1  # an output cell not yet decided


def neighbourhood_weights(k: int) -> np.ndarray:
    """Gaussian falloff over a (2k+1)^2 neighbourhood, flattened.

    Near neighbours should count for more than far ones: what a cell is depends much more on what
    it touches than on what is five tiles away. Sigma is k/2, the usual choice, and the centre is
    excluded by the caller (it is the value being predicted, not evidence for it).
    """
    d = 2 * k + 1
    ys, xs = np.mgrid[-k:k + 1, -k:k + 1]
    g = np.exp(-(ys ** 2 + xs ** 2) / (2 * (k / 2.0) ** 2))
    return g.reshape(d * d)


def synthesise(corpus: np.ndarray, oh: int, ow: int, k: int, rng, tol: float = 0.1,
               seed_patch: bool = True, solid: bool = False, passes: int = 2) -> np.ndarray:
    """Grow an (oh, ow) symbol grid from `corpus` by neighbourhood matching."""
    d = 2 * k + 1
    H, W = corpus.shape
    if H < d or W < d:
        raise SystemExit(f"corpus {W}x{H} is smaller than the {d}x{d} neighbourhood; lower --k")

    # Every position in the corpus whose full neighbourhood exists, as (M, d*d) plus its centre.
    wins = np.lib.stride_tricks.sliding_window_view(corpus, (d, d)).reshape(-1, d * d)
    centres = corpus[k:H - k, k:W - k].reshape(-1)
    if solid:
        keep = centres != sc.VOID
        wins, centres = wins[keep], centres[keep]
        if not len(centres):
            raise SystemExit("--solid left no candidates: this region is entirely VOID")
    wts = neighbourhood_weights(k)
    wts[d * d // 2] = 0.0  # the centre is the answer, not evidence

    out = np.full((oh, ow), UNKNOWN, dtype=np.int32)

    if seed_patch:
        # Bootstrap with one real dxd patch, so the first cells have genuine context rather than
        # matching against an all-unknown neighbourhood (which would pick a corpus position at
        # random and is a needlessly arbitrary way to start).
        m = int(rng.integers(len(wins)))
        out[:min(d, oh), :min(d, ow)] = wins[m].reshape(d, d)[:min(d, oh), :min(d, ow)]

    pad = np.full((oh + 2 * k, ow + 2 * k), UNKNOWN, dtype=np.int32)
    pad[k:k + oh, k:k + ow] = out

    for y in range(oh):
        for x in range(ow):
            if pad[y + k, x + k] != UNKNOWN:
                continue
            nb = pad[y:y + d, x:x + d].reshape(d * d)
            known = (nb != UNKNOWN) & (wts > 0)
            if not known.any():
                pick = int(rng.integers(len(centres)))
                pad[y + k, x + k] = centres[pick]
                continue
            # Weighted count of mismatches against every corpus neighbourhood at once.
            dist = ((wins[:, known] != nb[known]) * wts[known]).sum(axis=1)
            best = dist.min()
            # Sample among the near-ties rather than taking the argmin. Taking the best every time
            # makes the output deterministic given its start and so reproduces one region of the
            # author's map verbatim; the tolerance is what turns recall into style.
            cand = np.nonzero(dist <= best + tol * wts[known].sum() + 1e-9)[0]
            pad[y + k, x + k] = centres[int(rng.choice(cand))]

    # REFINEMENT, and the artefact it removes. The raster pass decides each cell from the cells
    # ABOVE AND LEFT of it only -- the rest are still unknown -- so its evidence is half a
    # neighbourhood and always the same half. Small errors therefore accumulate down and to the
    # right, and the first forest this produced had a clean diagonal seam with the whole top-right
    # corner flooded to bare sand. Re-deciding every cell in random order against its now-COMPLETE
    # neighbourhood removes the directional bias, because no cell is judged by only one side any
    # more. Two passes is enough; more just slowly erodes variety back towards the corpus.
    for _ in range(passes):
        order = rng.permutation(oh * ow)
        for idx in order:
            y, x = divmod(int(idx), ow)
            keep_val = pad[y + k, x + k]
            pad[y + k, x + k] = UNKNOWN
            nb = pad[y:y + d, x:x + d].reshape(d * d)
            known = (nb != UNKNOWN) & (wts > 0)
            if not known.any():
                pad[y + k, x + k] = keep_val
                continue
            dist = ((wins[:, known] != nb[known]) * wts[known]).sum(axis=1)
            best = dist.min()
            cand = np.nonzero(dist <= best + tol * wts[known].sum() + 1e-9)[0]
            pad[y + k, x + k] = centres[int(rng.choice(cand))]

    return pad[k:k + oh, k:k + ow]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--k", type=int, default=2, help="neighbourhood radius (window is 2k+1)")
    ap.add_argument("--size", type=int, nargs=2, default=(32, 32), metavar=("W", "H"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--tol", type=float, default=0.1,
                    help="accept matches within this fraction of the best (higher = more varied)")
    ap.add_argument("--region", type=int, nargs=4, metavar=("X", "Y", "W", "H"),
                    help="train on this sub-rectangle only; see style_wfc.py's note on why")
    ap.add_argument("--solid", action="store_true", help="never place VOID, so the scene is filled")
    ap.add_argument("--passes", type=int, default=2,
                    help="random-order refinement passes after the raster pass (see the note there)")
    ap.add_argument("--compare", action="store_true", help="also run WFC and score both")
    ap.add_argument("--out", default="tools/_style")
    args = ap.parse_args()

    out = ROOT / args.out
    if not (out / "corpus.npz").exists():
        raise SystemExit(f"{out / 'corpus.npz'} missing -- run:  python tools/style_corpus.py")

    grid = np.load(out / "corpus.npz")["grid"]
    blob = json.loads((out / "corpus.json").read_text())
    palette, meta = blob["palette"], blob["meta"]

    if args.region:
        rx, ry, rw, rh = args.region
        grid = grid[ry:ry + rh, rx:rx + rw]
        print(f"region ({rx},{ry}) {rw}x{rh} -> {grid.shape[1]}x{grid.shape[0]}")

    pack = ip.Pack()
    lmeta = sc.layer_meta(pack)
    ow, oh = args.size
    tag = (f"k{args.k}_{ow}x{oh}_tol{args.tol:g}_p{args.passes}"
           f"{'_solid' if args.solid else ''}"
           f"{'_r' + '-'.join(map(str, args.region)) if args.region else ''}")

    for i in range(args.count):
        seed = args.seed + i
        rng = np.random.default_rng(seed)
        t0 = time.time()
        gen = synthesise(grid, oh, ow, args.k, rng, args.tol, True, args.solid, args.passes)
        kl = sw.symbol_kl(gen, grid, len(palette))
        nov, fresh, tot = sw.novelty(gen, grid, 2 * args.k + 1)
        print(f"\nseed {seed}: {time.time() - t0:.1f}s")
        print(f"  symbol-KL vs author {kl:.3f} nats (0 = same mix)")
        print(f"  novelty @{2 * args.k + 1}x{2 * args.k + 1} {nov:.1%} "
              f"({fresh}/{tot} windows the author never drew)")

        dest = out / f"synth_{tag}_seed{seed}.png"
        sc.render(gen, palette, meta, lmeta).convert("RGB").save(dest)
        np.savez_compressed(dest.with_suffix(".npz"), grid=gen)
        print(f"  wrote {dest}")

        if args.compare:
            train = np.pad(grid, 2, constant_values=sc.VOID)
            pats, counts, _ = sw.patterns_from(train, 3, None)
            compat = sw.build_compat(pats)
            allow0 = ~(pats == sc.VOID).any(axis=(1, 2))
            g2, att = sw.generate(pats, counts ** 0.5, compat, oh, ow, 3, seed, 6, allow0, 1.0)
            if g2 is None:
                print(f"  WFC: no solution in 6 restarts")
            else:
                print(f"  WFC (n=3, solid, temper 0.5): KL {sw.symbol_kl(g2, grid, len(palette)):.3f} "
                      f"nats in {att} attempt(s)")
                d2 = out / f"wfccmp_{tag}_seed{seed}.png"
                sc.render(g2, palette, meta, lmeta).convert("RGB").save(d2)
                print(f"  wrote {d2}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
