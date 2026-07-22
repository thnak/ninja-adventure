# `assets/tile_index.json` — searchable tile catalogue

Every 16×16 tile of every Ninja Adventure tileset, plus a coarse index of the actor sprite sheets,
so a tile can be chosen by **querying** rather than by opening a PNG in an image editor and
guessing. 5225 tiles across 23 sheets.

Regenerate with:

```bash
taskset -c 0-3 python3 tools/tile_metrics.py      # pixel measurements -> tools/_tile_metrics.json
taskset -c 0-3 python3 tools/build_tile_index.py  # + labels + actors -> assets/tile_index.json
```

`build_tile_index.py` runs `tile_metrics.py` for you if the metrics file is missing, and it exits
non-zero if any tile named in `suggestions` no longer has the role it is supposed to have — so the
recommendations below cannot silently rot.

Related tools:

| Tool | What it does |
|---|---|
| `tools/tile_metrics.py` | measures every tile: opacity, borders, colour, texture statistics |
| `tools/tile_labels.py` | the hand-written region → meaning table (one entry per area of each sheet) |
| `tools/build_tile_index.py` | merges the two, indexes actors, writes the JSON |
| `tools/contact_sheet.py` | renders whole sheets with `col,row` under every tile — how the labels were written |
| `tools/atlas_preview.py` | magnifies a chosen rectangle when a contact sheet is not big enough |

---

## Grid

All tilesets are a **16×16 grid with no gutter** (stride 16). `col`/`row` are tile indices, so the
pixel rect of a tile is `(col*16, row*16, col*16+16, row*16+16)`.

One sheet is not an exact multiple of 16: **`TilesetFloor.png` is 352×417**, one pixel taller than
26 whole rows. That stray row is dropped; only rows 0–25 are indexed. Any such sheet carries a
`partial` object in its `sheets[]` entry. No sheet has a partial column.

---

## Top-level schema

```jsonc
{
  "schema_version": 1,
  "tile_size": 16,
  "stride": 16,
  "roles":  { "<role>": "what that role means" },
  "sheets": [ { sheet, path, width, height, cols, rows, tiles, roles:{...}, partial? } ],
  "totals": { "tiles": 5225, "sheets": 23, "by_role": {...} },
  "suggestions": { "<terrain need>": { wanted_role, pick, runner_up } },
  "actors": [ { folder, category, depicts, colour_variants, sheets:[...] } ],
  "tiles":  [ { ...one entry per tile, see below... } ]
}
```

## Per-tile fields

```json
{
  "sheet": "TilesetFloor.png",
  "col": 1,
  "row": 12,
  "name": "grass_lime_textured",
  "description": "bright lime-green grass ground fill, with visible lime-green pixel detail",
  "role": "fill_textured",
  "opaque": true,
  "self_contained": false,
  "mean_rgb": [172, 184, 55],
  "stddev": 5.99,
  "alpha_fraction": 1.0
}
```

| Field | Meaning |
|---|---|
| `sheet`, `col`, `row` | where the tile lives. Join to `sheets[]` on `sheet` to get the file path. |
| `name` | short slug. Not unique — variants of the same thing share a slug on purpose (`grass_lime_textured` covers four tiles). Use `sheet`+`col`+`row` as the key. |
| `description` | one sentence about what the tile depicts. |
| `role` | see the role table below. |
| `opaque` | every pixel has alpha 255. An opaque tile can be a base-layer tile; a non-opaque one must be drawn over something. |
| **`self_contained`** | **all four 1px borders are fully transparent.** See below — this is the field that matters most. |
| `mean_rgb` | mean colour over non-transparent pixels. |
| `stddev` | per-channel stddev over non-transparent pixels, averaged. **0.0 means a flat solid fill with no texture at all.** |
| `alpha_fraction` | fraction of the 256 pixels that are not fully transparent. `0.0` ⇒ an empty grid cell (`name: "empty"`, `role: "unknown"`). 1250 of the 5225 cells are empty padding. |

### `self_contained` — read this one

`self_contained: true` means the sprite fits inside this single tile: you can draw it on its own
and it will look like a complete thing.

`self_contained: false` on a sprite tile means **it is a slice of a larger object**. Trees in this
pack are 2 or 4 tiles wide and 2–3 tall; taking a single tile out of one gives you half a canopy
sitting on grass. When `role` is `object_part`, the `description` states the object's full
footprint and which slot this tile is, e.g.

> `tree_broad_canopy_far_left` — "canopy / far left slice of a 4x3-tile tree broad — large
> broadleaf tree, 4 tiles wide and 3 tall"

To place that tree you need all 12 tiles at the same relative offsets.

Note that `self_contained: false` is also true of ground fills (they are opaque edge to edge) and
of single-tile plants drawn deliberately edge-to-edge so they tile. Combine it with `role`:
`object_part` + `self_contained: false` is the dangerous combination.

### Roles

| Role | Count | Meaning |
|---|---:|---|
| `fill_plain` | 115 | **one solid colour, zero pixel detail.** Renders as a flat background wash, not as art. This is what makes ground read as a background colour. Avoid for terrain. |
| `fill_textured` | 435 | **full-tile tileable ground fill that HAS visible detail** — specks, tufts, wind ripples, individual stones. Use these for ground. |
| `transition_edge` | 1113 | edge/corner piece where one terrain meets another. Only correct next to the matching fill. The name suffix records a **measurement**: which of the four 1px borders differ in colour from the tile's middle (`_n`, `_sw`, `_nswe`…), plus `_ring` (all four differ — an isolated island tile) and `_c` (none differ — a body tile). |
| `object_part` | 2007 | one slice of a multi-tile sprite. Never usable alone. |
| `object_whole` | 75 | a sprite that fits entirely inside its tile. |
| `decor` | 230 | small detail to scatter over a fill (pebbles, tufts, flowers, cracks). |
| `unknown` | 1250 | empty grid cells (all of them; nothing is unclassified). |

`fill_plain` vs `fill_textured` is decided numerically, not by eye: a fill is `fill_plain` iff the
tile contains exactly one distinct RGBA value. It is `fill_textured` if at most 22 % of the tile is
non-dominant colour, no non-dominant blob exceeds 18 px, and `stddev ≤ 35` — i.e. scattered small
detail rather than one big shape. Anything else in a fill region is an edge.

---

## Actor sheets

Indexed per folder, not per frame:

```json
{
  "folder": "assets/_src/ninja/Actor/Character/Boy",
  "category": "Character",
  "depicts": "Boy - 16x16 humanoid character (player or NPC)",
  "colour_variants": ["base"],
  "sheets": [
    { "file": "SpriteSheet.png", "width": 64, "height": 112,
      "frame": [16, 16], "cols": 4, "rows": 7,
      "cols_mean": "facing down / up / left / right",
      "rows_mean": "walk f0, walk f1, walk f2, walk f3, attack, jump, (dead|item|special1|special2 by column)" }
  ]
}
```

The layouts are verified, not assumed:

* **`Character/*/SpriteSheet.png` (64×112)** — 4 columns × 7 rows of 16×16. Columns are
  down / up / left / right (the left and right columns are exact horizontal mirrors). Rows are
  walk frames 0–3 (frames 0 and 2 are identical and equal the idle pose), then attack, then jump,
  then a mixed row where the four columns are dead / item / special1 / special2. This was checked
  by comparing every row against the matching `SeparateAnim/*.png` pixel for pixel.
* **`Monster/*/<Name>.png` (64×64)** — 4 direction columns × 4 walk-frame rows of 16×16.
* **`Animal/*/SpriteSheet*.png`** — always **2 frames side by side**, so the frame size is
  `width/2 × height`. These are *not* 16×16: hamsters are 15×15, lions 16×23, side views up to
  30×16. A `*Side.png` in the same folder is the side view of the same animal.
* **`Faceset*.png` (38×38)** — dialogue portrait, not a map sprite.
* **`Boss/*`** — named animation strips (`Idle`, `Walk`, `Attack`, `Hit`, `Charge`, `Jump`); the
  frame is square at the strip's height, so `Idle.png` at 250×50 is 5 frames of 50×50.
* **`CharacterAnimated/NinjaGreen`** — the 32×32 hero rig; use the `Separate/` files, each a grid
  of 32×32 frames.

`colour_variants` lists the recolour suffixes present in the folder (`["base"]`, `["Black",
"White"]`, …).

---

## How to query it

```bash
# every textured ground fill, greenest first
jq -r '.tiles[] | select(.role=="fill_textured") | [.sheet,.col,.row,.name] | @tsv' \
   assets/tile_index.json

# the flat fills to stop using
jq -r '.tiles[] | select(.role=="fill_plain") | [.sheet,.col,.row,.name] | @tsv' \
   assets/tile_index.json

# anything that is safe to drop on the map as a single tile
jq -r '.tiles[] | select(.self_contained and .role!="unknown") | [.sheet,.col,.row,.name] | @tsv' \
   assets/tile_index.json

# is this specific tile safe to use alone?
jq '.tiles[] | select(.sheet=="TilesetNature.png" and .col==0 and .row==3)' assets/tile_index.json

# search descriptions
jq -r '.tiles[] | select(.description|test("cobble")) | [.sheet,.col,.row,.name] | @tsv' \
   assets/tile_index.json

# the recommended terrain picks
jq '.suggestions' assets/tile_index.json

# actors depicting a cat
jq -r '.actors[] | select(.depicts|test("Cat")) | .folder' assets/tile_index.json
```

Python:

```python
import json
idx = json.load(open("assets/tile_index.json"))
by_key = {(t["sheet"], t["col"], t["row"]): t for t in idx["tiles"]}
grass = [t for t in idx["tiles"] if t["role"] == "fill_textured" and "grass" in t["name"]]
```

---

## Suggested terrain picks

Read the live values out of `suggestions` — the table below is a snapshot. Every `pick` and
`runner_up` is asserted at build time to still have `wanted_role`.

| Need | Pick | Runner-up |
|---|---|---|
| `grass` | `TilesetFloor.png 1,12` | `TilesetFloor.png 12,12` |
| `dirt` | `TilesetFloor.png 12,19` | `TilesetFloor.png 14,19` |
| `sand` | `TilesetFloor.png 1,5` | `tileset_camp.png 16,5` |
| `snow` | `TilesetFloor.png 1,19` | `TilesetFloor.png 3,19` |
| `marsh` | `TilesetFloor.png 12,12` | `TilesetFloor.png 12,19` |
| `ash` | `TilesetInteriorFloor.png 16,13` | `TilesetInteriorFloor.png 14,15` |
| `stone` | `TilesetInteriorFloor.png 5,13` | `TilesetInteriorFloor.png 5,1` |
| `water` | `TilesetWater.png 11,2` | `TilesetWater.png 24,2` |
| `water_shore` | `TilesetWater.png 1,6` (whole autotile: cols 0–9, rows 6–10) | `TilesetWater.png 1,0` (cols 0–9, rows 0–4) |

`water_shore` is the one need whose `wanted_role` is `transition_edge` rather than
`fill_textured`: a shoreline is a 10×5 autotile, and you want the whole block, not one tile.

### Sheets worth knowing about

* **`TilesetFloor.png`** is the main outdoor ground sheet: two 10-column terrain blocks
  (cols 0–9 and 11–20) × three 7-row groups, plus a water/lava group at rows 21–25. In each
  7-row group, row +5 column +0 is the flat fill and columns +1…+4 are the textured fills.
* **`TilesetLogic.png` is not art.** Row 0 is eight flat editor colour swatches and rows 1–9 are
  marker glyphs. Nothing on this sheet belongs on a map.
* **`TilesetNature.png` has no ground fills at all** — it is trees, rocks and plants, almost all
  multi-tile. Check `self_contained` before using anything from it.
* **`TilesetDesert.png`** is a desert village kit; its only ground tiles are the flat sand block
  around cols 14–16 rows 8–9.
* **`TilesetRelief.png`** is cliffs — cols 0–3 are the flat plateau top, cols 4–11 the vertical
  cliff *face*. A cliff face is not a ground tile.
