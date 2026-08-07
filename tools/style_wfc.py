#!/usr/bin/env python3
"""Learn the author's map-design style from his own map, then synthesise new scenes in it.

Step 2 and 3 of the exercise `tools/style_corpus.py` starts. That tool proves a lossless symbol
grid can be recovered from `World/Maps/Village.tscn`; this one learns the statistics of that grid
and samples new grids from them, rendering the result with the author's own tiles.

**READ THIS BEFORE USING IT TO MAKE ANYTHING.** On this corpus WFC loses, measurably and by a wide
margin, to the non-parametric sampler in `tools/style_synth.py`. Symbol-KL against the author's own
region, 28x28 output, 6 seeds each, lower is better:

    region     synth            wfc
    village    0.353 +- 0.027   1.556 +- 0.080
    forest     1.086 +- 0.542   2.922 +- 0.277
    fort       0.583 +- 0.243   2.150 +- 0.188

The cause is in that file's docstring and it is structural, not a matter of tuning: 2086 of the
2268 n=3 patterns occur exactly once, so exact pattern matching has almost nothing to match. This
file is kept because it is the honest baseline, because its `--report` is the clearest statement of
how repetitive the author's map actually is, and so that nobody re-derives it expecting the
textbook result. Generate scenes with style_synth; reproduce the table with its `--compare`.

THE MODEL: overlapping WaveFunctionCollapse
-------------------------------------------
Every NxN window of the corpus is recorded as a PATTERN, with a weight equal to how often it
occurs. Two patterns may sit next to each other iff their overlap agrees. Generation then solves a
constraint problem: every NxN window of the output must be a window that appears in the input.

Why this and not a Markov chain over single cells: a first-order chain reproduces which tile
FOLLOWS which, which is enough for a texture and nowhere near enough for a composition. It cannot
represent "a house roof is three rows tall and its door is on the bottom row", because that fact
spans more than one adjacency. An NxN overlapping model represents any regularity that fits in an
NxN window, which is what actually carries this author's style -- roof/wall/door stacks, the
two-tile band of sand that always separates grass from water, fence posts spaced along a line.

WHAT IT IS AND IS NOT LEARNING. It learns LOCAL style: the vocabulary of the author's terrain
transitions, his decoration density, how his buildings are assembled row by row. It does not learn
GLOBAL layout -- there is no notion here of "a village has one main street" or "the dojo faces the
square", because those are facts about a 64x62 region and no NxN window can hold them. This is the
same boundary ROADMAP.md §R8 drew when it chose to stamp the author's hand-composed parcels rather
than to guess at them procedurally; what is new is that the LOCAL half no longer has to be a
hand-tuned noise-scatter, it can be sampled from the author's own statistics.

WHY NO SYMMETRY AUGMENTATION BY DEFAULT. The textbook WFC adds rotations and reflections of every
pattern to enlarge a small corpus. This art is directional: roofs have tops, the dojo has readable
`DOJO` lettering, and shorelines are lit from one side. `--mirror` is offered because a horizontal
flip is the one transform the pack's own prefab importer already treats as safe for some parcels,
but it is off by default and it will mirror any lettering it touches.

CONTRADICTIONS. A wave that paints itself into a corner is restarted with a fresh seed rather than
backtracked. Backtracking a wave this size costs more to write and to run than a restart, and the
restart count is reported so a corpus too sparse to solve shows up as a number instead of a hang.

Usage:
    python tools/style_corpus.py                       # build the corpus first
    python tools/style_wfc.py --report                 # what was learned, no generation
    python tools/style_wfc.py --size 48 48 --seed 7    # generate + render a new scene
    python tools/style_wfc.py --n 2 --size 64 64       # smaller window = looser, more novel

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

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (dx, dy) for the four cardinal neighbours, and the index of each one's opposite.
DIRS = [(1, 0), (-1, 0), (0, 1), (0, -1)]
OPP = [1, 0, 3, 2]


# --- learning ----------------------------------------------------------------


def mirror_palette(palette: list) -> tuple[list, np.ndarray]:
    """Extend `palette` with the horizontal mirror of every symbol; return (palette, map).

    Mirroring a stack means toggling each tile's flip_h bit. The mirrored stack often is not
    already in the palette, so the palette grows -- and that is correct: it is a symbol the author
    never drew but which is made of his tiles under a transform he himself uses elsewhere.
    """
    index = {tuple(tuple(e) for e in s): i for i, s in enumerate(palette)}
    out = list(palette)
    mapping = np.zeros(len(palette), dtype=np.int32)
    for i, stack in enumerate(palette):
        # entry layout is [layer, id, ax, ay, fh, fv, tr]; index 4 is flip_h.
        m = tuple(tuple(e[:4] + [1 - e[4]] + e[5:]) for e in stack)
        if m not in index:
            index[m] = len(out)
            out.append([list(e) for e in m])
        mapping[i] = index[m]
    return out, mapping


def patterns_from(grid: np.ndarray, n: int, mirror_map: np.ndarray | None = None):
    """Every distinct NxN window of `grid`, with occurrence counts.

    Non-periodic: the corpus is a map with edges, not a torus, and wrapping it would invent
    adjacencies between the fort in the north-west and the lake in the south-east.
    """
    H, W = grid.shape
    wins = np.lib.stride_tricks.sliding_window_view(grid, (n, n)).reshape(-1, n, n)
    if mirror_map is not None:
        flipped = mirror_map[wins][:, :, ::-1]
        wins = np.concatenate([wins, flipped], axis=0)
    keys = wins.reshape(len(wins), -1)
    uniq, inv, counts = np.unique(keys, axis=0, return_inverse=True, return_counts=True)
    return uniq.reshape(-1, n, n), counts.astype(np.float64), (H, W)


def overlap(pats: np.ndarray, dx: int, dy: int) -> np.ndarray:
    """The part of each pattern that a neighbour at (dx, dy) must agree with."""
    n = pats.shape[1]
    ys = slice(max(0, dy), n + min(0, dy))
    xs = slice(max(0, dx), n + min(0, dx))
    return pats[:, ys, xs]


def build_compat(pats: np.ndarray) -> list[np.ndarray]:
    """compat[d][q, p] = may pattern q sit at the DIRS[d]-neighbour of pattern p?

    NOTE THE INDEX ORDER -- `q` (the neighbour) is first, `p` (the anchor) second. It reads
    backwards, and it is that way because `propagate`'s hot path asks "pattern q just died; which
    p did it support?", which is a whole row in this orientation and a strided column in the
    natural one.

    Built by hashing the overlap regions and matching equal keys, which is O(P) per direction --
    the naive pairwise comparison is O(P^2) and at a few thousand patterns that is the difference
    between a second and a minute.
    """
    P = len(pats)
    out = []
    for dx, dy in DIRS:
        a = overlap(pats, dx, dy).reshape(P, -1)
        b = overlap(pats, -dx, -dy).reshape(P, -1)
        # Map each distinct overlap block to an integer, then compare integers.
        both = np.concatenate([a, b], axis=0)
        _, ids = np.unique(both, axis=0, return_inverse=True)
        ids = ids.ravel()
        ka, kb = ids[:P], ids[P:]
        # C-contiguous, and stored so that `compat[d][q]` is a whole ROW. `propagate` needs, for a
        # banned pattern q, every p that q supported -- a COLUMN of the natural orientation, whose
        # stride-P read costs several times a row read at this size.
        out.append(np.ascontiguousarray(kb[:, None] == ka[None, :]))
    return out


# --- generation --------------------------------------------------------------


class Wave:
    """An overlapping-WFC wave over a (h, w) field of pattern slots.

    Entropy is tracked INCREMENTALLY -- each cell keeps running sums of the weights still allowed
    there, updated by `ban`. Recomputing it from the (h, w, P) boolean each observation is the
    obvious way and it dominated the runtime completely: a 32x32 scene took 34s per attempt that
    way and well under a second this way, because the per-observation work drops from O(h*w*P) to
    O(h*w).
    """

    def __init__(self, h, w, weights, compat, rng, allow0=None, jitter=1.0):
        self.h, self.w = h, w
        self.P = len(weights)
        self.weights = weights
        self.jitter = jitter
        self.wlw = weights * np.log(weights)
        self.compat = compat
        self.rng = rng
        self.allowed = np.ones((h, w, self.P), dtype=bool)
        # support[y, x, p, d] = how many patterns still allowed at the d-neighbour support p there.
        # Sum over axis 0 because compat is stored neighbour-first -- see build_compat.
        supp = np.stack([c.sum(axis=0) for c in compat], axis=1).astype(np.int32)  # (P, 4)
        self.support = np.broadcast_to(supp, (h, w, self.P, 4)).copy()
        self.count = np.full((h, w), self.P, dtype=np.int32)
        self.sum_w = np.full((h, w), weights.sum(), dtype=np.float64)
        self.sum_wlw = np.full((h, w), self.wlw.sum(), dtype=np.float64)
        self.stack: list[tuple[int, int, int]] = []
        self.contradiction = False
        if allow0 is not None:
            for p in np.nonzero(~allow0)[0]:
                for y in range(h):
                    for x in range(w):
                        self.ban(y, x, int(p))

    def ban(self, y, x, p):
        if not self.allowed[y, x, p]:
            return
        self.allowed[y, x, p] = False
        self.support[y, x, p, :] = 0
        self.count[y, x] -= 1
        self.sum_w[y, x] -= self.weights[p]
        self.sum_wlw[y, x] -= self.wlw[p]
        if self.count[y, x] == 0:
            self.contradiction = True
        self.stack.append((y, x, p))

    def propagate(self):
        while self.stack and not self.contradiction:
            y, x, p = self.stack.pop()
            for d, (dx, dy) in enumerate(DIRS):
                # The cell whose d-neighbour is (y, x): losing p there costs it support.
                cy, cx = y - dy, x - dx
                if not (0 <= cy < self.h and 0 <= cx < self.w):
                    continue
                # p sat at the d-neighbour of (cy,cx); it supported exactly those q there that
                # compat says may have p as their d-neighbour -- one contiguous row.
                mask = self.compat[d][p]
                col = self.support[cy, cx, :, d]
                col -= mask
                dead = mask & self.allowed[cy, cx] & (col <= 0)
                for q in np.nonzero(dead)[0]:
                    self.ban(cy, cx, int(q))
                    if self.contradiction:
                        return

    def observe(self) -> bool:
        """Collapse the lowest-entropy undecided cell. False when everything is decided."""
        undecided = self.count > 1
        if not undecided.any():
            return False
        # Shannon entropy of the weights still allowed, from the running sums, plus a hair of noise
        # to break ties randomly.
        with np.errstate(divide="ignore", invalid="ignore"):
            ent = np.log(self.sum_w) - self.sum_wlw / self.sum_w
        ent = np.where(undecided, ent, np.inf)
        # The jitter is a real knob, not a tie-break epsilon. Pure min-entropy resolves the most
        # constrained cell every time, which grows one region outward from wherever it started and
        # over-represents whatever texture tiles with itself -- the author's fence field came out
        # at 28% of a scene where he drew it at 18%. Randomising the ORDER of observation spreads
        # the scene's seeds out and pulls that back towards his mix; the cost is more contradictions,
        # since min-entropy is what keeps a wave solvable. See `--sweep`.
        ent = ent + self.rng.random(ent.shape) * self.jitter
        y, x = np.unravel_index(np.argmin(ent), ent.shape)
        opts = np.nonzero(self.allowed[y, x])[0]
        pw = self.weights[opts]
        pick = int(self.rng.choice(opts, p=pw / pw.sum()))
        for q in opts:
            if q != pick:
                self.ban(int(y), int(x), int(q))
        self.propagate()
        return True

    def run(self) -> bool:
        self.propagate()
        while not self.contradiction:
            if not self.observe():
                return True
        return False

    def result(self, pats: np.ndarray, n: int) -> np.ndarray:
        """Wave of pattern slots -> the (h+n-1, w+n-1) symbol grid it denotes."""
        out = np.zeros((self.h + n - 1, self.w + n - 1), dtype=np.int32)
        for y in range(self.h):
            for x in range(self.w):
                p = int(np.argmax(self.allowed[y, x]))
                # Each slot contributes its top-left cell; the last row/column contribute their
                # whole pattern, which is what fills the n-1 cells the slots do not reach.
                ry = range(n) if y == self.h - 1 else [0]
                rx = range(n) if x == self.w - 1 else [0]
                for j in ry:
                    for i in rx:
                        out[y + j, x + i] = pats[p, j, i]
        return out


def generate(pats, weights, compat, oh, ow, n, seed, tries, allow0=None, jitter=1.0):
    """Solve a wave, restarting on contradiction. Returns (grid, attempts) or (None, tries)."""
    h, w = oh - n + 1, ow - n + 1
    for attempt in range(1, tries + 1):
        rng = np.random.default_rng(seed + attempt * 7919)
        wave = Wave(h, w, weights, compat, rng, allow0, jitter)
        if wave.run():
            return wave.result(pats, n), attempt
    return None, tries


# --- reporting ---------------------------------------------------------------


def novelty(gen: np.ndarray, corpus: np.ndarray, k: int) -> tuple[float, int, int]:
    """Fraction of the generated map's kxk windows that never occur in the corpus.

    The honest test that this is STYLE TRANSFER and not COPYING. At k = n it must be 0 by
    construction -- every n-window is a corpus window, that is what WFC enforces. Above n it should
    rise: the model is recombining the author's vocabulary into arrangements he never drew. A value
    still near 0 at k = n+2 means the corpus was too sparse to do anything but quote it back.
    """
    def wins(g):
        if g.shape[0] < k or g.shape[1] < k:
            return np.empty((0, k * k), dtype=np.int32)
        return np.lib.stride_tricks.sliding_window_view(g, (k, k)).reshape(-1, k * k)

    cw, gw = wins(corpus), wins(gen)
    if len(gw) == 0:
        return float("nan"), 0, 0
    seen = {w.tobytes() for w in cw}
    fresh = sum(1 for w in gw if w.tobytes() not in seen)
    return fresh / len(gw), fresh, len(gw)


def symbol_kl(gen: np.ndarray, corpus: np.ndarray, nsym: int) -> float:
    """KL(generated || corpus) over the single-symbol distribution, in nats.

    Near 0 means the new scene uses the author's tiles in the author's proportions -- as much grass
    per bush, as much shoreline per lake. It is a check on the model's FIDELITY, and it is the
    counterweight to `novelty`: a map can be novel by being wrong.
    """
    p = np.bincount(gen.ravel(), minlength=nsym).astype(np.float64)
    q = np.bincount(corpus.ravel(), minlength=nsym).astype(np.float64)
    p /= p.sum()
    q = (q + 1e-9) / (q + 1e-9).sum()
    nz = p > 0
    return float((p[nz] * np.log(p[nz] / q[nz])).sum())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=3, help="pattern window size")
    ap.add_argument("--size", type=int, nargs=2, default=(48, 48), metavar=("W", "H"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=1, help="how many scenes to generate")
    ap.add_argument("--tries", type=int, default=12, help="restarts allowed per scene")
    ap.add_argument("--mirror", action="store_true", help="augment with horizontal mirrors")
    ap.add_argument("--solid", action="store_true",
                    help="forbid VOID, so the scene comes out filled edge to edge")
    ap.add_argument("--jitter", type=float, default=1.0, metavar="J",
                    help="randomness in observation ORDER: 0 = strict min-entropy (blobby), "
                         "higher = more evenly mixed but more contradictions")
    ap.add_argument("--temper", type=float, default=0.5, metavar="A",
                    help="raise pattern weights to this power: 1 = the author's true frequencies, "
                         "0 = every pattern equally likely. Default 0.5 is measured, see the note "
                         "in main.")
    ap.add_argument("--no-pad", action="store_true",
                    help="do not ring the corpus in VOID (see the padding note in main)")
    ap.add_argument("--region", type=int, nargs=4, metavar=("X", "Y", "W", "H"),
                    help="train on this sub-rectangle of the corpus only (see the note in main)")
    ap.add_argument("--report", action="store_true", help="report what was learned, generate nothing")
    ap.add_argument("--sweep", action="store_true",
                    help="score a range of --temper values by symbol-KL and novelty, render none")
    ap.add_argument("--out", default="tools/_style")
    args = ap.parse_args()

    out = ROOT / args.out
    corpus_npz = out / "corpus.npz"
    corpus_json = out / "corpus.json"
    if not corpus_npz.exists():
        raise SystemExit(f"{corpus_npz} missing -- run:  python tools/style_corpus.py")

    grid = np.load(corpus_npz)["grid"]
    blob = json.loads(corpus_json.read_text())
    palette, meta = blob["palette"], blob["meta"]

    # REGION CROPPING, and why it matters more than any other knob here. The author's map is not
    # one style, it is five neighbouring ones -- a log fort, a brick village street, a pine forest,
    # a snowfield and a lake. An NxN model has no representation of "which of those am I in", so
    # trained on the whole map it samples freely between them and the result reads as a soup of
    # everything at once. Cropping to one coherent region is the honest way to ask for one of them:
    # it does not add a constraint the model cannot express, it removes the mixture the model was
    # never told about. Use tools/_style/corpus.png to pick coordinates.
    if args.region:
        rx, ry, rw, rh = args.region
        grid = grid[ry:ry + rh, rx:rx + rw]
        print(f"region ({rx},{ry}) {rw}x{rh} -> {grid.shape[1]}x{grid.shape[0]}")

    mirror_map = None
    if args.mirror:
        palette, mirror_map = mirror_palette(palette)
        print(f"mirror augmentation: palette {meta['symbols']} -> {len(palette)} symbols")

    t0 = time.time()
    # Ring the corpus in VOID before extracting patterns. Without it, 71 of the 2172 n=3 patterns
    # have NO legal neighbour in some direction -- they are the ones lying against the edge of the
    # author's map, where his content simply stops and no pattern was ever sampled to continue it.
    # A wave that places one is guaranteed to contradict, and every 24x24 solve failed because of
    # it. The ring is not a fudge: VOID is the symbol for "the author drew nothing here", and the
    # truth being taught is that his map really does end in nothing. It removes all 71 dead ends.
    train = grid if args.no_pad else np.pad(grid, args.n - 1, constant_values=sc.VOID)
    pats, counts, (H, W) = patterns_from(train, args.n, mirror_map)
    compat = build_compat(pats)
    P = len(pats)
    dens = sum(c.mean() for c in compat) / 4
    dead = sum(int((compat[d].sum(axis=0) == 0).sum()) for d in range(4))

    # `--solid` keeps only patterns with no VOID in them, i.e. the fully-drawn interior of his map.
    # Generation otherwise reproduces his 38% void faithfully and correctly, which is right for a
    # map with a coastline and wrong for "give me a filled scene".
    allow0 = None
    if args.solid:
        allow0 = ~(pats == sc.VOID).any(axis=(1, 2))
        print(f"--solid: {int(allow0.sum())}/{P} patterns contain no VOID")

    # WEIGHT TEMPERING, and the failure it exists to fix. Sampled at the author's true frequencies
    # (`--temper 1`), a `--solid` scene collapses into open water: the lake is his single largest
    # uniform region, so its fill pattern outweighs the ~1700 patterns that occur once each, and
    # min-entropy resolution spreads it over everything. Tempering raises every weight to the power
    # A, which compresses that gap without reordering it -- the lake stays his most common fill,
    # it just stops being 300x more likely than a house wall.
    #
    # THE DEFAULT IS 0.5 BECAUSE IT MEASURED BETTER, and the result is not the obvious one:
    # DISTORTING the pattern frequencies produces a SYMBOL distribution closer to the author's
    # (KL 0.54 against his own village region, versus 0.86 at his true frequencies, 6 seeds each).
    # Sampling patterns at their true rate is not the same as laying tiles at their true rate,
    # because a frequent pattern also propagates its neighbours into place and so spends itself
    # more than once. `--sweep` re-measures this on whatever region and n you are actually using.
    weights = counts ** args.temper if args.temper != 1.0 else counts

    print(f"corpus {W}x{H}, {len(palette)} symbols"
          f"{'' if args.no_pad else f' (VOID-padded by {args.n - 1})'}")
    print(f"patterns n={args.n}: {P} distinct from {(H - args.n + 1) * (W - args.n + 1)} windows"
          f"{' (x2 mirrored)' if args.mirror else ''}")
    print(f"adjacency density: {dens:.3%} of pattern pairs may touch  "
          f"(low = rigid style, high = permissive)")
    print(f"dead-end patterns (no neighbour in some direction): {dead}")
    print(f"learned in {time.time() - t0:.1f}s")

    if args.report:
        # The most frequent patterns are the author's habits, stated as data.
        top = np.argsort(-counts)[:6]
        print("\nmost frequent patterns (symbol ids, VOID=0):")
        for p in top:
            rows = " / ".join(" ".join(f"{v:>3}" for v in row) for row in pats[p])
            print(f"  x{int(counts[p]):<4} {rows}")
        ent = -(counts / counts.sum() * np.log(counts / counts.sum())).sum()
        print(f"\npattern entropy: {ent:.2f} nats "
              f"(max {np.log(P):.2f} if every pattern were equally likely)")
        return 0

    ow, oh = args.size

    if args.sweep:
        print(f"\nknob sweep, {ow}x{oh}, {args.count} seed(s) each")
        print(f"  {'jitter':>7} {'temper':>7} {'solved':>7} {'KL':>7} {'nov@' + str(args.n + 2):>8}")
        for jit in (0.0, 0.3, 1.0, 3.0):
            for a in (0.5, 1.0):
                wts = counts ** a if a != 1.0 else counts
                kls, novs, solved = [], [], 0
                for i in range(args.count):
                    g, _ = generate(pats, wts, compat, oh, ow, args.n, args.seed + i,
                                    args.tries, allow0, jit)
                    if g is None:
                        continue
                    solved += 1
                    kls.append(symbol_kl(g, grid, len(palette)))
                    novs.append(novelty(g, train, args.n + 2)[0])
                if not solved:
                    print(f"  {jit:>7} {a:>7.2f} {'0/' + str(args.count):>7}")
                    continue
                print(f"  {jit:>7} {a:>7.2f} {str(solved) + '/' + str(args.count):>7} "
                      f"{np.mean(kls):>7.3f} {np.mean(novs):>7.1%}")
        print("\nKL is fidelity (0 = the author's own mix of tiles); nov is how much of the result "
              "he never drew.\nWant KL low WITHOUT novelty collapsing to near zero -- that is "
              "style, not tracing -- and `solved` high enough to be usable.")
        return 0

    pack = ip.Pack()
    lmeta = sc.layer_meta(pack)
    ok = 0
    for i in range(args.count):
        seed = args.seed + i
        t1 = time.time()
        gen, attempts = generate(pats, weights, compat, oh, ow, args.n, seed, args.tries,
                                 allow0, args.jitter)
        if gen is None:
            print(f"\nseed {seed}: no solution in {args.tries} restarts")
            continue
        ok += 1
        # Scored against `train`, the grid the patterns actually came from. Scoring against the
        # unpadded corpus reports ~0.4% novelty at n, which looks like a broken WFC guarantee and
        # is really just the VOID ring being absent from the comparison set.
        nov_n, _, _ = novelty(gen, train, args.n)
        nov_hi, fresh, tot = novelty(gen, train, args.n + 2)
        kl = symbol_kl(gen, grid, len(palette))
        void = float((gen == sc.VOID).mean())
        print(f"\nseed {seed}: solved in {attempts} attempt(s), {time.time() - t1:.1f}s")
        print(f"  void {void:.1%}   symbol-KL vs author {kl:.3f} nats (0 = same mix)")
        print(f"  novelty @{args.n}x{args.n} {nov_n:.1%} (0 expected -- WFC's own guarantee)")
        print(f"  novelty @{args.n + 2}x{args.n + 2} {nov_hi:.1%} "
              f"({fresh}/{tot} windows the author never drew)")

        img = sc.render(gen, palette, meta, lmeta)
        tag = (f"n{args.n}_{ow}x{oh}_j{args.jitter:g}_t{args.temper:g}"
               f"{'_solid' if args.solid else ''}{'_mir' if args.mirror else ''}"
               f"{'_r' + '-'.join(map(str, args.region)) if args.region else ''}")
        dest = out / f"gen_{tag}_seed{seed}.png"
        img.convert("RGB").save(dest)
        np.savez_compressed(dest.with_suffix(".npz"), grid=gen)
        print(f"  wrote {dest}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
