# Authoring world content — the author kit

This game's set-pieces are **authored, not generated**: `tools/import_prefabs.py` lifts hand-composed
parcels out of the CC0 "Ninja Adventure" pack's own `Village.tscn` and `tools/build_atlas.py` packs
them into `assets/atlas.png` and generates `src/world/prefabs.hpp`. The **author kit** lets *you* add
your own set-pieces the same way: draw a scene in Godot 3 on the pack's tilesets, paint a few
semantic layers, name a couple of nodes, and import it as one prefab — with spawns, a boss anchor,
protected clusters, named zones, animated overlays and a provenance note all coming through.

Nothing you author is a runtime asset the game loads. The importer turns your `.tscn` into build-time
data; the generators bake it into the atlas and a header. So the whole pipeline stays true to
`ARCHITECTURE.md`: Python is build tooling, C++ is the runtime.

---

## 0. One-time: get the Godot project

```bash
# assets/_src/ is gitignored — it is a download, not a checked-in asset.
unzip assets/_src/ninja/GodotProject.zip -d /tmp/ninja
```

Open `/tmp/ninja/GodotProject/project.godot` in **Godot 3.x** (this pack is a Godot 3 project — its
tilesets use the Godot 3 autotile bitmask). You do not have to author *inside* the pack project, but
it is the easiest way to paint on the pack's tilesets with the right cell size (16×16).

---

## 1. Author a scene on the pack tilesets

Make a new scene. Add `TileMap` nodes and paint them with the pack's tilesets
(`res://World/Backgrounds/Tileset/*.tres`). A few rules the importer relies on:

* **Cell size is 16×16.** The whole pipeline is 16px-native (the renderer scales ×2 on screen).
* **Layer name = draw role.** The importer maps each drawable layer to one of four roles:

  | role | index | blocks? | y-sorts with actors? | canonical names |
  |---|---|---|---|---|
  | floor overlay | 0 | no | no | `Floor` |
  | floor detail  | 1 | no | no | `FloorDetail`, `Snow` |
  | structure     | 2 | **yes** | yes | `House` |
  | prop          | 3 | no | yes | `Element` |

  The six pack names above hit the table exactly. Any other name is classified by keyword — a name
  containing `wall`/`house`/`build`/`struct`/`block` becomes a blocking structure, `element`/`prop`/
  `tree`/`decor` a y-sorted prop, `detail`/`snow`/`overlay` a floor detail, and anything else a plain
  floor. When in doubt, use the canonical names.

* **The whole scene becomes one prefab.** There is no rectangle to select — the prefab's footprint is
  the bounding box of everything you painted (drawable *and* marker layers).

---

## 2. Paint `Mark:` semantic layers

A `TileMap` whose name starts with **`Mark:`** is *semantic, never drawn*. Any tile you paint on it
(use any tile — the art is ignored) marks that cell. Marker tiles are never rendered and never packed
into the atlas. The vocabulary:

| layer name | meaning | emitted as |
|---|---|---|
| `Mark:Spawn` | each painted cell is a spawn anchor | `PrefabDef::spawns` (list of `{dx,dy}`) |
| `Mark:Boss` | each painted cell is a boss anchor (the dojo-flag pattern) | `PrefabDef::bosses` |
| `Mark:KeepGroup` | the optional cluster under each painted cell **never drops** (overrides the 75% keep roll) | folds into `PrefabDef::keep_groups`, honoured by `prefab_group_kept` |
| `Mark:Zone` or `Mark:Zone:<label>` | the painted cells' bounding box is a named zone rect | `PrefabDef::zones` (list of `{name,x,y,w,h}`) |

An **unknown** `Mark:Something` is warned about, stored raw in the JSON under `unknown_marks`, and
skipped in codegen — so a typo never silently changes gameplay, and a future kind can be read back
from the JSON without re-importing.

`Mark:KeepGroup` works on the *optional clusters* the packer forms from connected structure/prop
cells: paint a marker on one cell of a cluster and that whole cluster is pinned. Group 0 (the floor
and the single big-house centrepiece) is always kept anyway, so a marker there is a harmless no-op.

---

## 3. Name `Anim:` / `Spin:` nodes → motiles

A `Sprite` (or `AnimatedSprite`) node whose name starts with **`Anim:`** or **`Spin:`** becomes a
**motile**: an animated overlay the renderer draws **closed-form from the world clock**, with no
stored animation state — so every client sees the same frame at the same instant and a screenshot at
a given world time is reproducible (the exact discipline the ability-zone particles use).

* `Anim:<name>` — a **frame cycle**. `frame = world_ms / period_ms % frames`.
* `Spin:<name>` — a **continuous rotation** of frame 0. `angle = 360° · (world_ms mod period) / period`.

How a motile node is read:

| node property | becomes |
|---|---|
| node **name** after the prefix | the motile's label |
| `@<ms>` suffix on the name (e.g. `Spin:fan@1500`) | the period in ms (defaults: anim 700, spin 2000) |
| `texture` (an `ExtResource`) | the frame **spritesheet** — a horizontal strip |
| `hframes` | the **frame count** (default 1); frame size = texture size ÷ (hframes, vframes) |
| `position` (a `Vector2`) | the sprite **centre**, converted to the frame-0 top-left offset in the prefab |

`build_atlas.py` packs the frames into the atlas (a MOTILE pass, appended last so no existing slot
moves) and emits a `PrefabMotile{dx,dy, ax,ay, pw,ph, frames, period_ms, kind}` table. The renderer's
`Impl::motile` draws each one over the sorted pass — motiles ride *above* their prefab (smoke off a
roof, a fan on a tower), so they are never something an actor walks in front of.

### The shipped example: chimney smoke

You do **not** edit the pack author's `Village.tscn` to get chimney smoke — editing his map is not the
point of the kit. Instead the smoke is attached at codegen: `build_atlas.py`'s `chimney_motiles`
walks every prefab, finds each dwelling cell (a `TilesetHouse` sprite ≥ 48px in the house band — the
same rule that tags a door), and attaches the pack's own 6-frame 32×32 smoke sheet at the roof ridge.
Every village house therefore puffs smoke in-game, deterministically, at no runtime cost.

---

## 4. Write a `meta.md` sidecar

Drop a `meta.md` next to your scene — either `myscene.tscn.meta.md` or `meta.md` in the same folder.
Leading `key: value` lines are **front matter**; everything after is **free prose**:

```
Author: Jane Q. Mapmaker
Ring: Forest
Provenance: composed for the F5 windmill milestone

A lone hill windmill with a turning fan. The Mark:Spawn cell is the miller's post; the
Mark:Zone:yard rect is the flour yard the caravan event uses.
```

Both flow into `prefabs.hpp` as comments **above** that prefab's `PrefabDef`, so the next reader sees
where the parcel came from and what its markers mean, right next to the data.

---

## 5. Import, pack, build, verify

```bash
# 5a. Import the scene as ONE prefab. --name defaults to snake_case of the filename.
python3 tools/import_prefabs.py --scene path/to/windmill.tscn --name windmill
#      (optionally crop with --rect X Y W H, in the scene's own tile coords)

# 5b. Re-pack the atlas and regenerate the headers.
python3 tools/build_atlas.py
#      add --prefab-proof to also re-render each prefab from the packed data for eyeballing:
python3 tools/build_atlas.py --prefab-proof   # writes assets/_gen/prefabs/<name>_atlas.png

# 5c. Rebuild.
cmake --build build -j$(nproc)

# 5d. See it. Point the camera at a village (chimney smoke) or a placed prefab:
xvfb-run -a ./build/mmo_client --shot 3 shot.png --village 10   # then: mv shot.png <somewhere>
```

> **Screenshot note.** raylib's `TakeScreenshot` writes to the current working directory regardless
> of the path you pass, so run the client from the repo root and `mv` the file afterwards.

The default `python3 tools/import_prefabs.py` (no `--scene`) still cuts the pack's own parcels and
must produce byte-identical JSONs — the author kit only ever *adds* content, never rewrites the
hand-cut parcels.

Your new prefab exists in `prefabs.hpp` as `PrefabId::k<Name>` the moment you import it, but it is not
placed in the world until worldgen references it — add it to a placement row in
`src/world/worldgen.hpp` (or a village builder in `src/world/village.hpp`) to actually see it in-game.

---

## Worked example — a windmill with a turning fan (`Spin:fan`)

No windmill sprite ships with the pack; the *recipe* is the deliverable. In Godot:

1. **Ground.** A `Floor` TileMap: a small hill of grass/dirt tiles.
2. **Tower.** A `House` TileMap (blocks + y-sorts): the mill body. Name it `House` so the player
   collides with it and walks behind it.
3. **The fan.** A `Sprite` named **`Spin:fan@1500`**:
   * `texture` = your fan spritesheet (for a pure rotation, a single frame is enough — `hframes = 1`).
   * `position` = centred on the tower's hub.
   * `@1500` makes it complete one revolution every 1.5 s of world time.
4. **A spawn.** A `Mark:Spawn` TileMap with one cell at the mill's door — the miller's post.
5. **A zone (optional).** A `Mark:Zone:yard` TileMap painted over the flour yard, for a future event.
6. **Provenance.** A `windmill.tscn.meta.md` with a line or two on what it is.

Import and pack:

```bash
python3 tools/import_prefabs.py --scene windmill.tscn --name windmill
python3 tools/build_atlas.py
```

You now have `PrefabId::kWindmill` with a `Spin` motile, a spawn anchor, a named zone, and your
meta.md prose sitting above its `PrefabDef`. The fan turns closed-form from the world clock: every
client sees it at the same angle at the same instant, and it costs the simulation nothing.

---

## Reference: the regression fixture

`tools/testdata/test_plot.tscn` (+ `test_plot.tscn.meta.md`) is a tiny scene kept in the tree that
exercises the whole kit: a drawable `Ground` layer on `TilesetFloor`, a `Mark:Spawn` cell, and an
`Anim:smoke@700` motile. Importing it must yield a JSON with `spawn_cells` and `motiles`, and codegen
must emit `kPrefabSpawns_TestPlot` and `kPrefabMotiles_TestPlot`. If it stops doing so, the kit has
regressed.

```bash
python3 tools/import_prefabs.py --scene tools/testdata/test_plot.tscn --name test_plot
```
