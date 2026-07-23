# Ninja Adventure — world rendering spec

> **Status: R0–R4 are DONE** (2026-07-23). §7 has the per-phase state. §9 covers R4 and corrects
> the set count in §5.1, which was measured by a scan rather than read from the pack's own
> project file. The old wording is left in place rather than deleted.
>
> **§0 has been corrected.** The headline measurement this document was written around turned out to
> be measuring the demo GIFs' screen-recording scale, not our rendering. The defects it pointed at
> are real and are fixed; the number is not evidence for them. The original claim is kept below,
> struck through, because a spec that quietly deletes its own wrong reasoning teaches nobody.

Architecture: [ARCHITECTURE.md](ARCHITECTURE.md) · Design: [GAME.md](GAME.md) · Roadmap: [ROADMAP.md](ROADMAP.md)

---

## 0. The problem, and one wrong measurement of it

The screenshots in `docs/shot_*.png` looked **rigid** next to the four `Example N.gif` files the
asset pack's own author recorded. The art was not the problem — it is the same art. The assembly
was, and the five defects in §2 are all real; you can see every one of them in the before/after
screenshots.

### 0.1 What the number said

> ~~One metric separates the two populations perfectly. Call it `grid_lock`: of all the pixels where
> colour changes, what fraction sit exactly on a 32px grid line?~~
>
> | | pack demos | our shots |
> |---|---|---|
> | ~~`grid_lock`~~ | ~~0.059 – 0.062~~ | ~~0.120 – 0.138~~ |
>
> ~~All four demo frames land inside `[0.059, 0.062]`. All seven of our world screenshots land inside
> `[0.120, 0.138]`. **The ranges do not overlap**, and the factor is exactly 2.1. That is the
> signature of flat fill tiles butted edge to edge with no transition set between them.~~

### 0.2 What it was actually measuring

The separation is real and perfectly reproducible. It is also **an artefact of how the demos were
recorded**, and it cannot be moved by any amount of tile work.

`tools/study_examples.py` now reports `capture_scale` next to every other number. The four demo
GIFs come out at **4x**: they are nearest-neighbour upscales, and Example 1 contains 317 runs of
exactly four identical columns against 2 runs of any other length. A colour change in those images
can therefore only occur on one column in four.

`grid_lock` counts a pixel as on-grid when `x % 32 == 0` or `(x + 1) % 32 == 0` — that is `x = 0`
and `x = 31`. Neither is in the demos' phase class. **The demos are arithmetically prevented from
scoring on the columns the metric samples**, whatever they draw.

Two independent checks, neither of which needs judgement:

| check | result |
|---|---|
| Chance level is `1 - (1 - 2/g)²`: 0.234 at g=16, 0.121 at g=32, 0.062 at g=64 | our shots sit **on** chance at every g (0.257 / 0.131 / 0.063) |
| Same sweep on the demos | roughly **half** of chance at every g, with **no peak at any g** — so no tile pitch is being detected |
| Histogram of edge pixels by `x % 32` | demos spike 2.0–2.7x at 1, 5, 9 … 29 and drop to 0.5x elsewhere; ours is flat |

If a demo has no peak at *any* grid size, there is no tile seam being found — there is only a phase
the sampler cannot see. Rendering at integer zoom and capturing at 4x would move our number to 0.06
without changing a single tile.

### 0.3 The metric that did track the defect

`density` — the fraction of pixels that are edge pixels — was originally dismissed as
non-discriminating because it came out *higher* for us. That was the finding, read backwards: we had
nearly twice the pack's detail density and looked emptier, because it was one grass tuft stamped on
every single grass tile (D4).

| | pack demos | ours before | ours after |
|---|---|---|---|
| `density` | 0.084 – 0.104 | 0.150 | **0.096** |

That one moved, for the reason it was supposed to move. The rest of this document's defects are
argued from the screenshots, which is what they should have been argued from in the first place.

---

## 1. Sources and tools

Two sources of truth, both already sitting in `assets/_src/ninja/`:

| Source | What it tells us |
|---|---|
| `Example 1..4.gif` | what the art looks like in motion |
| `Godot Project V4.zip` | **the tile placement rules and scene structure, written by the author** |
| `GodotProject.zip` (Godot 3) | the older build — every tile hand-placed, no autotiling |

| Tool | Job |
|---|---|
| `tools/study_examples.py` | measures `grid_lock` on the demos and on our screenshots |
| `tools/autotile_fit.py` | derives the transition-tile table **from the tileset's pixels** |
| `tools/godot_study.py` | reads the rules and scene structure straight out of the two Godot zips |
| `tools/check_sprite_rects.py` | audits `BIG_MANIFEST` for rectangles that cut through a sprite |
| `tools/mock_world.py` | renders a patch of world both ways, to look at and to measure |

None of these is imported by `src/`. They are measuring instruments, run at build time or by hand.

**One important confidence check.** `autotile_fit.py` infers the table from pixel colours;
`godot_study.py` reads the table the author already wrote down. Compared against each other they
**agree on 16 of 16 masks**. The tool reads the tileset the way its author wrote it. That is the
basis for trusting it on the terrain sets the author did *not* declare (§5.3).

---

## 2. Five defects

### D1 — No transition tiles in the atlas at all

`assets/tile_index.json` counts **1113 tiles with role `transition_edge`**. `MANIFEST` in
`tools/build_atlas.py` uses **zero** of them. Every terrain is drawn as one flat fill tile butted
against the next. This is the primary cause of §0.

The art is not missing. What is missing is the **lookup table**: given an arrangement of neighbours,
which tile to place.

### D2 — Fixed draw order, no Y-sort

This is the **structural** defect, and the one no amount of art can fix.

In the Godot project the `TileMap` has `y_sort_enabled`, and **characters are children of the
`TileMap` node**, not siblings of it. Actors and tiles are therefore sorted in the **same Y-ordered
pass**. The player walks behind a tree's canopy and in front of its trunk with no special case
anywhere.

On top of that, 77 tiles in the tileset carry `y_sort_origin = 12`. A sprite whose art is 16px tall
but whose **feet are at +12** must sort by its feet, not by its top-left corner. Without that offset
every overlap decision is made at the wrong Y.

> A renderer that draws terrain → objects → actors in three fixed passes **cannot** produce this,
> however good its tiles are.

### D3 — Both tree sprites are cut in half

`BIG_MANIFEST` declares `TreeBroad = NTree(5,2) 2x3` and `TreePine = NTree(1,2) 2x3`.

`TilesetNature` draws both of those trees **four tiles wide**: a canopy of three lobes plus a root
band spanning the full width. Taking the middle two columns keeps the centre lobe and the trunk and
**discards both side lobes**.

`check_sprite_rects.py` measures what fraction of each crop border cuts through non-outline pixels:

| | left | right | verdict |
|---|---|---|---|
| `TreeBroad (5,2) 2x3` | 58% | 58% | severed |
| `TreePine (1,2) 2x3` | 60% | 60% | severed |
| `(4,2) 4x3` — corrected | 0% | 0% | whole |
| `(0,2) 4x3` — corrected | 0% | 0% | whole |

This is why the forest in `docs/shot_forest.png` reads as a **palisade of narrow columns** rather
than as a canopy.

> The tool validates itself. Its first version flagged 9 of 17 rectangles, because sprites packed
> touching on a sheet also have opaque borders. Switching the test to the **1px `141b1b` outline**
> the pack always draws dropped all 13 building rectangles — the ones already vetted with
> `verify_structures.py` — to 0%, while both trees stayed flagged. It also flags `RuinA`/`RuinB`,
> and correctly: that region is not discrete sprites but a continuous mosaic of rubble, so any 3×3
> window cuts through it.

### D4 — Decoration covers 100% of the ground

We stamp a grass tuft on **every** grass tile, at the same offset. It reads as wallpaper.

The author's hand-built village map:

| layer | cells | vs ground |
|---|---|---|
| Floor | 2889 | — |
| FloorDetail | 10 | 0.3% |
| Wall + Wall2 | 689 | 23.8% |

> **Do not read 0.3% as the decoration budget** — I nearly reported it that way. Almost none of the
> clutter visible in the demos is on `FloorDetail`: the tufts, pots and crates are **scene tiles**
> (`content/destroyable/{grass,pot,crate}.tscn`) placed on the Wall layers, because they are
> destructible and need collision. 48 of those Wall cells are scene-collection entries.
> `FloorDetail` holds only the few flat marks nothing interacts with.
>
> The honest figure is the combined **~24%**: roughly one non-ground element per four ground tiles,
> and **clustered**, not spread evenly.

### D5 — Region boundaries are rectangles

Terrain is sampled at tile centres and thresholded, so region boundaries come out as axis-aligned
staircases. It must be sampled at **corner vertices** instead (§3.1).

---

## 3. Spec — data model

### 3.1 Terrain is decided per CORNER VERTEX, not per tile

This is the crux, and the easiest thing to get wrong.

```
Wrong:  terrain[tx][ty]                              → then guess corners from neighbours
Right:  terrain_corner[tx][ty]  on a (W+1)×(H+1) grid → each tile reads the 4 corners around it
```

Sampling per tile and inferring corners produces masks **no tileset has art for** (two diagonal
corners touching, for instance). At that point you have to fake it with a flat fill — and the hard
edges are back.

The 4-bit mask, in the convention `autotile_fit.py` already uses:

```
bit 0 (1)  = top-left corner    is terrain A
bit 1 (2)  = top-right corner
bit 2 (4)  = bottom-left corner
bit 3 (8)  = bottom-right corner

mask 0  → all B (a plain B fill)
mask 15 → all A (a plain A fill)
mask 1..14 → the 14 real edge and corner tiles
```

### 3.2 The lookup table

`tools/_autotile.json`, generated by:

```bash
python3 tools/autotile_fit.py --verified --emit tools/_autotile.json
```

The verified set: `grass_dirt` = `TilesetFloor.png` cells `(0,7)`–`(10,12)`, 56 tiles, terrains named
`grass` and `dirt`, mode 0 (match corners *and* sides — the 47-blob system).

**Never pick randomly among a mask's variants.** Two variants are interchangeable only if their
**border pixels match**, and here they do not: `TilesetFloor` draws several "grass above, dirt below"
variants whose boundary crosses at different heights, so butting two of them together produces a
step exactly on a tile edge — the very artefact we are removing. The first version of
`mock_world.py` did this and **measured worse** than the flat-fill renderer it was meant to beat
(0.127 against 0.109).

Use the `coherent` field: exactly **one** tile per mask, chosen so all 14 tile together seamlessly.

### 3.3 Fill tiles must agree with each other too

Fills must be the same shade, not merely the same terrain family. `#d3865f` and `#a3754e` are both
dirt and are 82 apart — well over the 40 that reads as an edge. Interleaving them repaints the hard
seam, just arrived at from a different direction.

`autotile_fit.py` already filters twice: uniform borders, then cluster by mean colour and keep the
largest group. Use `a_fills` / `b_fills` as returned; do not gather extras.

---

## 4. Spec — draw order

### 4.1 Four layers

Exactly as in the author's `system/map/map.tscn`:

| # | layer | z_index | Y-sort | sort origin |
|---|---|---|---|---|
| 3 | Floor | −2 | no | 0 |
| 2 | FloorDetail | −1 | yes | 0 |
| 1 | Wall2 | 0 | yes | −5 |
| 0 | Wall | 0 | yes | −5 |

Drawing goes by `z_index` first, so bottom-to-top is Floor → FloorDetail → the two Wall layers.

### 4.2 Sort key

Within a Y-sorted pass the key is:

```
sort_key = y_world + sprite.sort_origin
```

`sort_origin` is a property **of each sprite**, not a global constant. A 4×3 tree has its feet at the
bottom of its lowest tile; so does a 4×3 house; a terrain tile is 0. The author uses `+12` on a 16px
grid for 77 tiles — about ¾ of a tile height.

### 4.3 Actors go in the SAME pass as tiles

This is the sufficient condition, and the real structural change. Actors and Wall-layer sprites must
go into **one list**, sorted once, drawn in one pass. Not "draw the tiles, then draw the actors".

If only one item in this whole document gets done, do this one. `grid_lock` measures hard edges;
what it cannot measure is the sense of depth, and depth comes from here.

---

## 5. Spec — atlas

### 5.1 The change of plan: the edge sets are GENERATED, not found

> **R4 corrected the central claim of this section — see §9. The pack ships 13 complete sets, not
> three, and the author's own Godot project names every one of them with coordinates. The scan below
> undercounted because it looked for the wrong mask layout. What survives is the conclusion: there
> is still no sand-on-grass set, so seven of the eleven terrains are still generated.**

The spec called for a `TRANSITION_MANIFEST` naming sets recovered from the pack by
`autotile_fit.py`. **That is not what was built, and the reason is a measurement.**

`autotile_fit.py --scan` over all ten tilesets finds 31 candidate sets and exactly **three** that
cover all 16 corner masks. Not one of them is sand-on-grass — and sand-on-grass is the boundary that
dominates every outdoor screenshot in this game, because the meadow's own thresholds put a band of
beach around every pond. The closest candidates cover 6 masks of 16:

| candidate | A / B | masks |
|---|---|---|
| `TilesetVillageAbandoned:0-1#1` | `#d2b37d` / `#adbc3a` | 6 of 16 |
| `TilesetNature:3-7#3` | `#4a7f4b` / `#c69469` | 6 of 16 |

The pack's transition art is excellent and it is drawn for the pack's **own** world — grass, dirt
paths and water. Ours is eleven noise-thresholded terrains. A scheme that handles only the pairs the
author happened to draw would have left the single most visible boundary hard-edged.

So `TRANS_MANIFEST` names **one fill per terrain**, and `transition_tile()` cuts that fill to an
irregular contour per mask. What is generated is only the **shape** of the boundary; the pixels are
still the pack's own art, so re-arting a terrain changes its transitions automatically and no
coordinates are typed by eye.

The contour is a bilinear interpolation of the four corner bits, thresholded at 0.5, plus two
octaves of noise that **wrap at 16px**. The wrap is what makes it work: adjacent tiles share two
corners, so the interpolated field already agrees along their shared edge, and a wrapping wobble
agrees there too. Without it every tile border would show a step — which is the artefact being
removed.

It also means masks **6 and 9** exist. §3.1 warns that per-tile sampling produces corner
arrangements no tileset draws; generating the set removes the warning's teeth, and is what allows
the overlay rule in §7.1 that the spec's own model forbids.

> The pack's four water sets — on grass, on sand, on ice, on marsh — **were** recovered and do share
> one geometry. They are better than what is generated: they carry a white foam ring and a brown
> bank. Swapping them in was R4, and it is **done** — see §9.

### 5.2 Budget

The atlas is currently `448×1834` with 82 slots; tiles are 16px + 2px padding = 18px, so 24 columns
per row.

Built: 11 terrains × 14 masks = **154 tiles**, one strip per terrain, and the atlas went from
448×1834 to 448×2032 — 198px of height. Not a concern, as predicted.

**The one place the pack ran out.** `kTerrainHasPlain` in the generated header records whether each
terrain's variant 0 is genuinely flat, and it is **measured off the pixels**, not asserted. The
spread is three-way and decides the mirroring rule:

| | stddev of variant 0 |
|---|---|
| grass, dirt, water, sand, snow, marsh | **0.0** — real plain fills |
| path / building | 8.9 — an earth grain, but from the centre of an autotile set, so it repeats seamlessly |
| **stone, ash** | **22–23** — whole-tile masonry |

Ninja Adventure ships no flat ash and no bare rock. Every dark ground in it is an *interior*
masonry floor, and `TilesetRelief`'s "cliff stone" is a vertical cliff **face** seen side-on, not
ground. So stone and ash keep a whole-tile motif in all three variants, and are the only two
terrains still mirrored: unmirrored they tile into a flawless brick lattice and the wasteland reads
as a cathedral floor. Mirrored, the seams are the lesser evil — broken rubble is at least the right
kind of wrong for scorched ground.

> The real fix is art this CC0 pack does not contain. Comparing the wasteland screenshot against any
> other biome shows the cost, and it is the one place where the ground still looks made rather than
> grown.

The threshold is 15, not the 6 first tried, because 6 caught the road as well — and mirroring a road
would put a seam across every tile of it.

### 5.3 Why overlays rather than per-pair sets

`Terrain` has 11 values. A transition set for **every pair** is 55 pairs × 14 = 770 tiles —
unacceptable.

The right model is a **terrain priority order**. Where two terrains meet, draw the lower-priority one
as the fill and overlay the **higher-priority terrain's own edge set** on top (a set drawn against
transparency). That needs one set per terrain, not per pair: 9 × 14 = 126.

`autotile_fit.py --scan` already found several such overlay sets (`b_is_void`) in `TilesetNature`,
`TilesetElement`, `tileset_camp` and `TilesetVillageAbandoned`.

**A warning, and the real limit of this document:** the pack declares exactly **one** terrain set.
The Godot 3 project uses `tile_mode` 0 and 2 — every tile hand-placed, no autotiling. The Godot 4
project defines only `grass`/`dirt`. For water, snow, sand and the rest, `autotile_fit.py` is the
only source, and every set **must be `--preview`ed before it goes into `VERIFIED`**.

One known trap: several sheets pad their unused area with **opaque white** rather than transparency,
so white is an ambiguous family — both genuine snow art and page margin. `TilesetFloor:0-2` and
`TilesetFloor:0-4` both report "all 16 masks" and both need a human to look.

---

## 6. Spec — object placement

| Aspect | Today | Spec |
|---|---|---|
| Density | 100% of grass tiles | **~24%** of ground tile count |
| Distribution | uniform | clustered — a second noise field gates where, then a per-tile roll |
| Position | snapped to the 32px lattice | free pixel offsets (±5px is enough) |
| Decor choice | one sprite | matched to the ground: grass uses `TilesetFloorDetail` row 2, bare ground row 0 |
| On transition tiles | — | **place nothing** — a twig half on grass reads as a mistake |

Trees: cluster them, offset them freely, and test the terrain **under their FEET**, not at their
top-left corner.

> The first version of `mock_world.py` scattered decor taken from `TilesetNature` rows 0–2. Those are
> not decor at all — they are the **upper canopy slices of the 2×3 trees** — so the meadow came out
> strewn with green fragments and pink cherry-blossom corners. The correct sheet is
> `TilesetFloorDetail.png`.

---

## 7. Implementation order

Ordered so each phase **stands alone and reverts alone**, and no phase needs a later one to be
correct. The current branch is never blocked.

| Phase | Work | State |
|---|---|---|
| R0 | Fix the two tree rectangles in `BIG_MANIFEST` to `(4,2) 4x3` and `(0,2) 4x3` | **done** |
| R1 | Drop decor density to ~24%, cluster it, free offsets | **done** — `textured_here` |
| R2 | Corner-vertex terrain + a transition set per terrain | **done** — see §5.1 for the change of plan |
| R3 | Y-sort: actors and sprites in one pass | **done** — `Sprite`, `draw_sprite`, the sorted pass |
| R4 | Swap generated edge sets for the pack's hand-drawn ones where they exist | **done** — §9 |

### What each phase actually cost, against what was predicted

| | predicted | actual |
|---|---|---|
| R0 | very low | 2 coordinates — but the sprite doubled in width, so the tree **stride** and anchor had to move with it |
| R2 | medium | the plan had to change (§5.1), and it exposed two defects the spec never listed (§7.1) |
| R3 | **high — do it last** | the smallest of the four. Gathering into a `Sprite` list and sorting once replaced three hand-ordered passes and *deleted* the special cases they needed |

R3 being the cheapest is worth recording, because "it touches the main draw path" is a statement
about blast radius, not about difficulty, and the two were conflated. The draw code it replaced was
already ordering things by Y — by hand, in three places, with a comment at each explaining which
overlap case that pass got wrong.

### 7.1 Two defects this document did not list, found by doing the work

Both were invisible until something else was fixed, which is why looking again after each phase
matters more than the plan did.

**The overlay layer.** §3.1 says terrain is decided per corner vertex, and roads and building
footprints have no value at a corner — they are placed per tile by world generation. So the first
implementation exempted them and drew them flat. With every coastline now organic, a village
rendered as **a flat brown rectangle with a staircase border**: the exact defect, promoted to the
most visible thing on screen. Fixed by giving a corner the highest-priority overlay among the four
tiles that meet at it, which grows a placed region by half a tile and gives a road a wandering edge.

**Mirroring.** Every fill tile used to be mirrored per tile, to break up a repeated motif at no
cost. But a motif that reaches a tile's edge does not survive being flipped — the two halves no
longer meet, so mirroring writes a discontinuity onto **every** tile boundary. On a lake, whose fill
was one large rippling motif, that was thousands of colour boundaries pinned exactly to the grid.
Mirroring is now off, except for the two terrains that have no plain fill at all (§5.2).

---

## 8. Acceptance criteria

```bash
python3 tools/check_sprite_rects.py     # R0: 0 "SEVERED" rectangles (RuinA/RuinB excepted)   PASS
python3 tools/study_examples.py         # R1: density on world shots inside the pack's band   PASS
python3 tools/mock_world.py --measure   # control: seam < 0.05                                PASS
```

| criterion | target | result |
|---|---|---|
| severed rectangles | 0 (bar the two ruins) | **0** — both trees now report 0% on all four borders |
| `density`, world scenes | 0.084 – 0.104 (the pack's band) | **0.096** average; meadow 0.094, forest 0.086 |
| `seam` on the `mock_world.py` A/B | < 0.05 | **0.006**, from 0.586 |
| `shoreline_lock` on the real renderer | near chance (0.031) | see below |

**`grid_lock < 0.09` has been struck from this list** and cannot be met — see §0.2. It was the one
criterion here that was not a criterion at all, and leaving it in would have had someone chasing an
arithmetic impossibility.

### 8.1 `shoreline_lock` — the measurement that should have been here from the start

`tools/study_examples.py::shoreline_lock` asks the question §0 meant to ask, on the actual renderer
rather than on a mock: **of the columns where water starts or stops, what fraction land on a single
phase of the tile lattice?**

| scene | before | after | chance |
|---|---|---|---|
| `shot_wetland` | **0.984** | 0.249 | 0.031 |
| `shot_meadow` | 0.640 | 0.182 | 0.031 |
| `shot_forest` | 0.623 | 0.162 | 0.031 |

Before, 98% of the wetland's coastline stepped on one column of the grid. It was a staircase, and
the number says so without anyone having to look.

It differs from `grid_lock` in exactly two ways, and both were needed:

- **It fits the phase rather than assuming it.** `grid_lock` tests `x % 32 == 0`. The camera is
  centred on a player standing at a *fractional* tile position, so the tile lattice lands at an
  arbitrary screen offset — that test checks a lattice which is not in the image. This is a second,
  independent reason `grid_lock` could never have measured our tile seams, on top of §0.2.
- **It looks only at a terrain boundary**, found by colour, so decoration cannot move it. Removing
  grass tufts changes `grid_lock` without a single boundary moving.

It is still not at chance, and that is expected rather than a shortfall: the contour is interpolated
between corners that *are* on the lattice, so a residual preference for tile edges is built into the
scheme. The staircase is gone; the lattice has not been abolished.

---

## 9. What does NOT change

Written down so nobody widens the scope:

- **No** change to `kTilePx = 32`, `kChunkTiles = 32`, `kMapTiles = 1024`.
- **No** change to `enum class Terrain`. The transition table is a renderer concern; terrain stays a
  `std::uint8_t` as it is.
- **No** change to `protocol.hpp` / `snapshot.hpp`. Not one byte is added to the wire — corner masks
  are derived from terrain that is already there, computed on the draw side.
- **No** Python at runtime. `_autotile.json` is baked into the atlas and `atlas_slots.hpp` at build
  time, which is exactly the role `ARCHITECTURE.md` reserves for Python.
- **No** change to the existing `MANIFEST`. Transition tiles go in a new section; the current fill
  slots stay as they are.

---

## 10. Risks

| Risk | Level | Mitigation |
|---|---|---|
| R3 (Y-sort) breaks the working draw path | high | do it last, after the current branch merges; keep before/after screenshots |
| A terrain set other than `grass_dirt` is derived wrongly | medium | `--preview` is mandatory before `VERIFIED`; see the opaque-white trap in §5.3 |
| Per-frame Y sorting costs CPU | medium | sort only sprites in view; the Floor layer does not participate |
| Atlas growth | low | 126 tiles = 108px of height, see §5.2 |
| `seam` improves but it still looks bad | low | `mock_world.py` writes an image to look at, not only a number |

---

## 11. Appendix — commands

```bash
# measure the demos and our screenshots (~60s; pure-Python pixel scan)
python3 tools/study_examples.py

# derive the transition table
python3 tools/autotile_fit.py --scan                        # every candidate set, every sheet
python3 tools/autotile_fit.py --preview grass_dirt          # LOOK at it before trusting it
python3 tools/autotile_fit.py --verified --emit tools/_autotile.json

# read the author's own rules and scene structure
python3 tools/godot_study.py --layers      # layer stack + Y-sort
python3 tools/godot_study.py --terrain     # terrain table + diff against the derived one
python3 tools/godot_study.py --density     # real decoration density

# audit multi-tile sprite rectangles
python3 tools/check_sprite_rects.py
python3 tools/check_sprite_rects.py --rect NTree 4 2 4 3

# render a trial patch and measure it
python3 tools/mock_world.py --measure
```

---

## 9. R4 — the pack's own 47-mask sets

The pack ships **its author's Godot project**, in `assets/_src/ninja/GodotProject.zip`, and it is
the project the four `Example N.gif` demos were recorded from. That file answers by hand almost
every question §5.1 answered by measurement, and it corrects one of them.

### 9.1 What the project file says

**The set count in §5.1 is wrong.** `autotile_fit.py --scan` reported three complete sets. There are
**thirteen**, and `World/Backgrounds/Tileset/*.tres` gives each one's texture, origin and mask table.
The scan undercounted because it was looking for a **16-mask corner layout**. The pack's sets are
`tile_mode = 1, autotile/bitmask_mode = 1` — Godot 3's **BITMASK_3X3_MINIMAL**, a 47-tile blob in an
11×5 block, where a corner bit counts only when both adjacent sides do. A 16-mask scan cannot find a
47-mask set no matter how many tilesets it sweeps.

The crop was never the problem: scanning all 256 pixel offsets, offset (0,0) ranks **1 of 256** for
every set, with a standard deviation of exactly 0.00 across the fully-surrounded tile.

**How the demos are assembled**, from `World/Maps/Village.tscn`:

| layer | z | sorting | tile origin |
|---|---|---|---|
| `Floor`, `Snow`, `Relief`, `FloorDetail` | −1 | none | top-left |
| `House` | 0 | `cell_y_sort` | **bottom-left** |
| `Element` | 0 | `cell_y_sort` | centre |
| `Destroyable`, `Actors` | 0 | `YSort` | — |

Four flat ground layers, then everything else Y-sorted in one space with the tile origin moved to
the cell's foot. That is what R3 already does.

**Decoration density is a weighted die, not a noise field.** Each set has three fully-surrounded
tiles and `priority_map` weights them 8 : 1 : 1. Counted on the author's own map: 82.6% / 7.6% /
9.9% against a predicted 80 / 10 / 10 — so **~17.5%** of interior ground carries a motif. `Element`
is 14.8% of floor cells, `FloorDetail` 0.6%.

**`project.godot` renders at 320×176 and displays at 1280×704** — exactly 4×, with
`stretch/mode = "viewport"` and `flags/filter = false`. That is independent confirmation of §0: the
`grid_lock` separation is the recording scale, derived there from pixel run lengths and stated here
by the author's own configuration.

### 9.2 What was taken, and what still is not there

Four terrains now use the pack's art; seven are still generated. §5.1's conclusion holds even though
its count did not — **there is no sand-on-grass set.** Rebuilding the author's Village map from the
scene file shows why: he butts sand against grass with a hard staircase and scatters props over the
join.

| terrain | source | what it brings |
|---|---|---|
| Water | `TilesetWater#18` | white foam ring, brown bank |
| Dirt | `TilesetFloor#2` | grass fringe overhanging the earth |
| Snow | `TilesetSnow#17` | the pack's only alpha-cut overlay set |
| Marsh, Sand, Grass, Stone, Ash, Path, Building | generated | contour only; the pixels are still the pack's fills |

`TilesetSnow.png` **is not in the pack** — it ships only inside the zip.

Two traps, both found by measuring rather than by looking:

* **The pack's sets are opaque PAIRS**, not overlays: `#18` has grass baked into its rim. Laid over
  sand it would ring the coast in grass. `edge_from_pack` knocks the outside terrain out by exact
  colour, reading which colours are outside off the art itself — a border row whose side bit is
  clear lies wholly outside the blob. Inside and outside share **zero** colours in all four sets
  used, so the knockout takes the outside and nothing else.
* **`TilesetWater#27` is the obvious pick for Marsh and is wrong.** Its interior is poison water at
  `(188, 132, 181)`; our Marsh is wetland *ground* at `(116, 163, 52)`. Taking it because the name
  matched would have ringed every marsh in purple. Marsh is generated.

The pack's own data has a defect worth knowing about: `TilesetFloor#8` declares cell (1,4) as
fully-surrounded and (3,3) as isolated, and **both are entirely transparent**. Godot renders holes
there too. The importer drops any declared cell that is not drawn.

### 9.3 Three renderer changes this forced

**Terrain is sampled per TILE again.** The corner lattice existed to make a boundary cross a tile
interior when a tile could only hold one flat colour. A blob set already carries the rounded corner
and the overhanging bank, and asking one for a corner mask it was never drawn for is exactly the
failure §3.1 warns about. `build_corners` became `build_terrain`; `edge_mask` in the generated
header is the single place the minimal rule is written.

**Water moved from the bottom of `terrain_priority` to near the top.** The old order read as "what
sits under what" and put water lowest, reasoning that a bank overhangs a shore. That is a fine
description and it is not how the art is drawn: the bank and foam are painted on the **water** tile.
With water lowest it was never the terrain doing the drawing, so its shoreline stayed a staircase
while the generated boundaries beside it wandered. Water also had to move above **Path**: a village
square stamped around a pond was cutting it into a rectangle.

**The generated contour displaces the sample point, not the field value.** These look equivalent and
are not. How far a given amount of field moves a contour depends on the local gradient, and the
gradient across a straight edge is the steepest in the tile — so exactly where wobble was needed
most it did least, and a long run of one mask (a road edge, the side of a village square) came out
ruled. Displacing the coordinate moves the contour a fixed number of pixels wherever it falls.

**A fourth change was made and then removed.** `shot_village` measured 0.265 under the corner
lattice and 0.462 straight after the rewrite, so the rim of every placed region was eroded with a
noise field to give the art something to be ragged about. Both halves of that number turned out to
be the two fixes above, and the erosion cost a great deal: a road is two tiles wide, so every tile
of it is a rim tile, and a field eroding both sides at once ate it into a dashed line of crumbs.
Guarding on thickness did not save it — the roads run diagonally, so their axis-aligned
cross-section is wider than the road is. It is gone rather than tuned.

### 9.4 Measured

| scene | R2 (corner) | R4 | crossings R2 → R4 |
|---|---|---|---|
| `shot_forest` | 0.162 | **0.143** | 1278 → 1143 |
| `shot_wetland` | 0.249 | **0.171** | 1068 → 1120 |
| `shot_meadow` | 0.182 | *nan* | 154 → 106 |
| `shot_village` | 0.265 | *nan* | 136 → 108 |

Chance is 0.031. The two scenes with over a thousand crossings both improved. The other two now fall
under `shoreline_lock`'s 120-crossing floor and it reports `nan` rather than a number nobody should
quote — **which is a consequence of the change, not a failure of it**: water now recedes behind its
own foam and bank, so a small pond has less open water than it used to. `shot_village`'s R2 figure
rested on 136 crossings, barely over the floor, and was never worth much.

`ctest` passes. The wasteland is unchanged and still reads as interior masonry — that is the art gap
recorded in `assets/CREDITS.md`, and no autotile set fixes it.
