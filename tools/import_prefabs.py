#!/usr/bin/env python3
"""Cut hand-composed parcels out of the pack author's own Godot map and emit them as prefab data.

Why this tool exists:

  * **Authored content, not generated.** worldgen.hpp can scatter tiles procedurally, but the CC0
    "Ninja Adventure" pack ships something better: `World/Maps/Village.tscn`, a village the pack's
    own author composed by hand -- houses sitting right on their palisade, a market yard with a cart
    and a radish garden, a pine camp with a tent and a campfire. This tool lifts rectangular
    PARCELS out of that map and writes them as data our worldgen can later stamp into the world,
    so the game's set-pieces are the author's composition rather than our procedural guess at one.

  * **Reads everything straight from the download.** Scene, tilesets AND textures all come out of
    `assets/_src/ninja/GodotProject.zip` (textures live inside it under `GodotProject/...`), with a
    fall back to `assets/_src/ninja/Backgrounds/Tilesets/<name>` for any texture not in the zip. No
    pre-extracted directory is required -- `assets/_src/` is gitignored, a download not a checked-in
    asset, so this adds no new precondition beyond the zip the rest of the toolchain already reads.

  * **Rebuilds the map the way the author's engine does.** The parse/decode logic is ported from
    tools/_study/godot/render_village.py: ext_resource/sub_resource TileSets, `tile_data`
    PoolIntArray triplets (position x/y as signed 16-bit, id in bits 0..28, flip_h bit 29, flip_v
    bit 30, transpose bit 31, autotile coord in the third int), layers sorted by z_index, and the
    one measured subtlety about `cell_tile_origin` recorded verbatim below.

This is the role ARCHITECTURE.md reserves for Python: build-time tooling, never the runtime.

    python3 tools/import_prefabs.py                       # full map + every PARCEL + contact print
    python3 tools/import_prefabs.py --map-only            # just assets/_gen/prefabs/_map.png
    python3 tools/import_prefabs.py --rect 40 33 19 9 --name foo   # one ad-hoc parcel

MEASURED, from tools/_study/godot/README.md -- preserved verbatim because the whole render depends
on it:

  `Village.tscn` sets `cell_tile_origin = 2` (BOTTOM_LEFT) on the `House` layer, and anchoring those
  tiles by their bottoms is wrong. Measured, not assumed: a palisade log on this map is three stacked
  pieces -- `id39` at y=8 is 32px tall, `id40` at y=10 is 16, `id41` at y=11 is 32 -- and rows 8..9,
  10, 11..12 only tile contiguously if each piece is drawn at its cell's top-left. Anchoring bottoms
  left row 9 empty, so every log came out cut through the middle, the wall between them sat two tiles
  too high, and so did the gate and every house (`id7` is a 64x48 atlas, three tiles tall -- hence
  exactly two tiles of error). All `tex_offset` values in that tileset are zero, so nothing else
  compensates.

  `cell_tile_origin = 1` (CENTRE) on the `Element` layer **is** honoured -- props there sit centred
  on their cell.
"""
import argparse
import io
import json
import pathlib
import re
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "_src"
GODOT_ZIP = SRC / "ninja/GodotProject.zip"                 # same source build_atlas.py reads
ZIP_PREFIX = "GodotProject/"
SCENE = ZIP_PREFIX + "World/Maps/Village.tscn"
TRES_DIR = ZIP_PREFIX + "World/Backgrounds/Tileset/"
PACK_TILESETS = SRC / "ninja/Backgrounds/Tilesets"          # texture fallback, see module docstring
OUT = ROOT / "assets" / "_gen" / "prefabs"

TILE = 16
PAD = 3      # border of empty tiles around the map, matches render_village.py so px maths agree
SCALE = 4    # parcel previews are upscaled this much, nearest-neighbour

# Parcels to cut, in MAP tile coordinates {name: (x, y, w, h)}. These were seeded from the reference
# render (tools/_study/godot/village_rebuilt.png, PAD=3, map min x=19 y=8) via
# map = px/16 - PAD + min, then refined against the _map.png this tool draws so each box captures a
# composed set-piece without slicing a house in half (trees and fences cut at an edge are honest).
PARCELS = {
    "street_houses": (40, 34, 19, 9),   # central fenced block: house row, red-roof house, well/crates
    "market_yard":   (42, 23, 16, 6),   # upper house pair with the cart, pots and radish garden
    "camp_clearing": (22, 42, 16, 10),  # pine-forest camp: tent, campfire, flowers, grass patches
    # --- nine more, harvested from the same map. Rects refined against _map.png so no house, tent
    # or tower sprite is sliced at an edge (trees, fences and water cut at edges are honest). ---
    "fort_gate":           (19, 8, 17, 12),  # top wall+gate, brick road, log towers, the two interior houses
    "fort_courtyard":      (19, 20, 20, 16),  # lower court walled by log towers north and south: barrels, wall door, grass
    "waterfall_bridge":    (35, 8, 6, 15),    # river column + falls + log crossing (starts right of the fort house)
    "north_treeline_well": (40, 18, 17, 6),   # tree row + stump wells + stake-fence run above the market houses
    "forest_cottage":      (60, 22, 22, 10),  # green forest, orange-roof cottage, sunflowers, stumps
    "snow_pond":           (60, 32, 22, 12),  # frozen pond, snow pines, sled, shore barrels
    "lake_islands":        (60, 45, 22, 12),  # lake shore + small islands with boat/props
    "stairs_plaza":        (42, 43, 17, 9),   # the grand staircase approach below the village block
    "south_orchard":       (42, 51, 18, 6),   # the tree alley on green at the south edge
}

# --- Godot .tscn/.tres value parsers, ported from tools/_study/godot/render_village.py ------------
V2 = lambda s, d=(TILE, TILE): (lambda n: (int(float(n[0])), int(float(n[1]))) if len(n) >= 2 else d)(
    re.findall(r'-?[\d.]+', re.sub(r'Vector2|Rect2', '', s or '')))
RECT = lambda s: [int(float(v)) for v in re.findall(r'-?[\d.]+', re.sub(r'Rect2', '', s or '0,0,0,0'))][:4]


class Pack:
    """Reader for GodotProject.zip: members, and textures resolved to (image, sheet-id)."""

    def __init__(self):
        self.zf = zipfile.ZipFile(GODOT_ZIP)
        self.names = set(self.zf.namelist())
        self._img = {}   # sheet-id -> Image

    def text(self, member):
        return self.zf.read(member).decode("utf-8")

    def texture(self, res_path):
        """A `res://World/...` texture path -> (RGBA image, sheet-id string).

        Prefer the zip member; fall back to the pack's loose tilesets by basename for any texture the
        zip lacks. The sheet-id string is what lands in the prefab JSON, so a later atlas-packing step
        can find the same pixels: the zip member path, or `Backgrounds/Tilesets/<name>` when loose.
        """
        member = ZIP_PREFIX + res_path
        if member in self.names:
            if member not in self._img:
                self._img[member] = Image.open(io.BytesIO(self.zf.read(member))).convert("RGBA")
            return self._img[member], member
        loose = PACK_TILESETS / Path(res_path).name
        if loose.exists():
            sid = "Backgrounds/Tilesets/" + loose.name
            if sid not in self._img:
                self._img[sid] = Image.open(loose).convert("RGBA")
            return self._img[sid], sid
        raise FileNotFoundError(f"{res_path} is in neither {GODOT_ZIP.name} nor {PACK_TILESETS}")


def parse_tileset(txt, ext, pack):
    """Body of a TileSet resource -> id -> dict(img, sheet, rx, ry, tw, th).

    `ext` maps ExtResource id -> res-path; for a `.tres` those are its own ext_resources, for the
    scene's House sub_resource they are the scene's. tile_mode 0 (single) uses the whole region as
    the tile; modes 1 (autotile) and 2 (atlas) subdivide it by autotile/tile_size.
    """
    out = {}
    for i in sorted({int(m) for m in re.findall(r'^(\d+)/name = ', txt, re.M)}):
        g = lambda k, d="": (re.search(rf'^{i}/{k} = (.+)$', txt, re.M) or [None, d])[1]
        m = re.search(r'ExtResource\( ?(\d+) ?\)', g("texture", ""))
        if not m or m.group(1) not in ext:
            continue
        img, sheet = pack.texture(ext[m.group(1)])
        rx, ry, rw, rh = RECT(g("region"))
        tw, th = V2(g("autotile/tile_size"))
        if g("tile_mode", "0") == "0":
            tw, th = rw or TILE, rh or TILE     # single tile: the whole region is one tile
        out[i] = dict(img=img, sheet=sheet, rx=rx, ry=ry, tw=tw, th=th)
    return out


def decode(body):
    """`tile_data` PoolIntArray -> list of cells. See the triplet layout in the module docstring."""
    td = re.search(r'tile_data = (?:Pool|Packed)Int(?:32)?Array\( (.*?) \)', body, re.S)
    if not td:
        return []
    n = [int(v) for v in td.group(1).split(", ")]
    cells = []
    for i in range(0, len(n), 3):
        p, v, ac = n[i:i + 3]
        x = p & 0xFFFF
        x -= 0x10000 if x >= 0x8000 else 0
        y = (p >> 16) & 0xFFFF
        y -= 0x10000 if y >= 0x8000 else 0
        cells.append(dict(x=x, y=y, id=v & 0x1FFFFFFF,
                          fh=bool(v & (1 << 29)), fv=bool(v & (1 << 30)), tr=bool(v & (1 << 31)),
                          ax=ac & 0xFFFF, ay=(ac >> 16) & 0xFFFF))
    return cells


def resolve_tilesets(scene, pack):
    """A .tscn's TileSet resources: (sext, ts_ext, ts_sub).

    `sext` maps every ExtResource id to its res-path; `ts_ext` is the id->parsed-tileset map for the
    `.tres` TileSets the scene references, and `ts_sub` the id->parsed-tileset map for any inline
    SubResource TileSet (the House layer's, in Village.tscn). Textures resolve out of the pack the
    same way whether the scene is Village.tscn or a user's own -- a user authors on the pack tilesets,
    so their ExtResource paths point at the very `res://World/Backgrounds/...` files this reads.
    """
    sext = {i: p for p, i in re.findall(r'\[ext_resource path="res://(.+?)".*?id=(\d+)\]', scene)}
    ts_ext = {}   # scene ExtResource id -> parsed tileset (the .tres files)
    for i, p in sext.items():
        if not p.endswith(".tres"):
            continue
        t = pack.text(ZIP_PREFIX + p)
        if 'type="TileSet"' not in t.splitlines()[0]:
            continue
        e = {j: q for q, j in re.findall(r'\[ext_resource path="res://(.+?)".*?id=(\d+)\]', t)}
        ts_ext[i] = parse_tileset(t, e, pack)
    ts_sub = {}   # SubResource id -> parsed tileset (an inline TileSet)
    for m in re.finditer(r'\[sub_resource type="TileSet" id=(\d+)\]\n(.*?)(?=\n\[|\Z)', scene, re.S):
        ts_sub[m.group(1)] = parse_tileset(m.group(2), sext, pack)
    return sext, ts_ext, ts_sub


def load_layers(pack):
    """Parse Village.tscn into draw-ordered layers plus the map's tile-coordinate bounds.

    Returns (layers, X0, Y0, X1, Y1) where each layer is
    (name, tileset, cells, origin, z_index, ysort), sorted by z_index (stable, so scene order breaks
    ties: Floor, Snow, Relief, FloorDetail, House, Element).
    """
    scene = pack.text(SCENE)
    _sext, ts_ext, ts_sub = resolve_tilesets(scene, pack)

    layers = []
    X0 = Y0 = 10 ** 9
    X1 = Y1 = -10 ** 9
    for m in re.finditer(r'\[node name="([^"]+)" type="TileMap".*?\]\n(.*?)(?=\n\[node|\Z)', scene, re.S):
        name, body = m.group(1), m.group(2)
        me = re.search(r'tile_set = ExtResource\( (\d+) \)', body)
        ms = re.search(r'tile_set = SubResource\( (\d+) \)', body)
        ts = ts_ext.get(me.group(1)) if me else ts_sub.get(ms.group(1)) if ms else None
        cells = decode(body)
        if not ts or not cells:
            continue
        o = re.search(r'cell_tile_origin = (\d+)', body)
        origin = int(o.group(1)) if o else 0
        z = int((re.search(r'z_index = (-?\d+)', body) or [0, 0])[1])
        ysort = "cell_y_sort = true" in body
        layers.append((name, ts, cells, origin, z, ysort))
        X0 = min(X0, min(c["x"] for c in cells))
        X1 = max(X1, max(c["x"] for c in cells))
        Y0 = min(Y0, min(c["y"] for c in cells))
        Y1 = max(Y1, max(c["y"] for c in cells))
        print(f"  {name:<12} z={z:>2} {'ysort' if ysort else 'flat '} origin={origin} cells={len(cells)}")

    layers.sort(key=lambda L: L[4])   # stable: scene order breaks z_index ties
    return layers, X0, Y0, X1, Y1


def source_rect(ts, c):
    """The (sheet, sx, sy, w, h) source-pixel rect a cell reads from, or None if out of bounds."""
    t = ts.get(c["id"])
    if not t:
        return None
    tw, th = t["tw"], t["th"]
    sx, sy = t["rx"] + c["ax"] * tw, t["ry"] + c["ay"] * th
    if sx + tw > t["img"].width or sy + th > t["img"].height:
        return None
    return t, sx, sy, tw, th


def blit(dst, ts, c, origin, px, py):
    """Draw one cell onto `dst` at pixel (px, py) with its flips and centre-origin offset."""
    r = source_rect(ts, c)
    if not r:
        return
    t, sx, sy, tw, th = r
    src = t["img"].crop((sx, sy, sx + tw, sy + th))
    if c["tr"]:
        src = src.transpose(Image.TRANSPOSE)
    if c["fh"]:
        src = src.transpose(Image.FLIP_LEFT_RIGHT)
    if c["fv"]:
        src = src.transpose(Image.FLIP_TOP_BOTTOM)
    # See the docstring: origin 2 (BOTTOM_LEFT) does NOT offset -- draw at the cell's top-left. Only
    # origin 1 (CENTRE) offsets a tile bigger than its cell, by (8 - w/2, 8 - h/2).
    if origin == 1:
        px += 8 - tw // 2
        py += 8 - th // 2
    dst.alpha_composite(src, (px, py))


def composite(layers, X0, Y0, X1, Y1):
    """Rebuild the whole map into one RGBA image (opaque black background), PAD tiles of margin."""
    w = (X1 - X0 + 1 + PAD * 2) * TILE
    h = (Y1 - Y0 + 1 + PAD * 2) * TILE
    out = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    for name, ts, cells, origin, z, ysort in layers:
        order = sorted(cells, key=lambda c: c["y"]) if ysort else cells
        for c in order:
            px = (c["x"] - X0 + PAD) * TILE
            py = (c["y"] - Y0 + PAD) * TILE
            blit(out, ts, c, origin, px, py)
    return out


def map_px(x, y, X0, Y0):
    """Map tile coord -> pixel in the composite."""
    return (x - X0 + PAD) * TILE, (y - Y0 + PAD) * TILE


# Distinct colours for the parcel boxes drawn on _map.png.
BOX_COLORS = [(255, 80, 80), (80, 200, 120), (90, 150, 255), (240, 200, 60),
              (200, 100, 240), (80, 220, 220)]


def draw_map(comp, parcels, X0, Y0, X1, Y1, path):
    """The full composite with a 4-tile grid, map-coordinate labels, and each parcel boxed."""
    img = comp.copy()
    d = ImageDraw.Draw(img, "RGBA")
    x_lo, x_hi = X0 - PAD, X1 + PAD + 1
    y_lo, y_hi = Y0 - PAD, Y1 + PAD + 1
    # Light grid every 4 tiles, labels every 8, both in MAP coordinates (the raw tile_data x/y).
    for x in range(x_lo, x_hi + 1):
        if x % 4:
            continue
        px = (x - X0 + PAD) * TILE
        d.line([(px, 0), (px, img.height)], fill=(255, 255, 255, 40), width=1)
        if x % 8 == 0:
            d.text((px + 1, 1), str(x), fill=(255, 255, 255, 200))
    for y in range(y_lo, y_hi + 1):
        if y % 4:
            continue
        py = (y - Y0 + PAD) * TILE
        d.line([(0, py), (img.width, py)], fill=(255, 255, 255, 40), width=1)
        if y % 8 == 0:
            d.text((1, py + 1), str(y), fill=(255, 255, 255, 200))
    for i, (name, (rx, ry, rw, rh)) in enumerate(parcels.items()):
        col = BOX_COLORS[i % len(BOX_COLORS)]
        x0, y0 = map_px(rx, ry, X0, Y0)
        x1, y1 = map_px(rx + rw, ry + rh, X0, Y0)
        d.rectangle([x0, y0, x1 - 1, y1 - 1], outline=col + (255,), width=2)
        d.text((x0 + 2, y0 + 2), name, fill=col + (255,))
    img.save(path)
    print(f"  -> {path}  {img.size}")


def cut_parcel(name, rect, comp, layers, X0, Y0):
    """Crop the preview and build the JSON for one parcel; return (preview_img, tile_count)."""
    rx, ry, rw, rh = rect
    # Preview: an honest pixel crop of the full composite -- sprites that overhang the rect show cut.
    x0, y0 = map_px(rx, ry, X0, Y0)
    crop = comp.crop((x0, y0, x0 + rw * TILE, y0 + rh * TILE))
    preview = crop.resize((rw * TILE * SCALE, rh * TILE * SCALE), Image.NEAREST)
    preview.save(OUT / f"{name}.png")

    data_layers = []
    tiles = {}     # (sheet, sx, sy, w, h) -> index, deduped source rects for later atlas packing
    counts = {}
    for lname, ts, cells, origin, z, ysort in layers:
        order = sorted(cells, key=lambda c: c["y"]) if ysort else cells
        lcells = []
        for c in order:
            if not (rx <= c["x"] < rx + rw and ry <= c["y"] < ry + rh):
                continue   # anchor must lie inside the rect
            r = source_rect(ts, c)
            if not r:
                continue
            t, sx, sy, tw, th = r
            key = (t["sheet"], sx, sy, tw, th)
            tiles.setdefault(key, len(tiles))
            lcells.append({
                "x": c["x"] - rx, "y": c["y"] - ry,
                "sheet": t["sheet"], "sx": sx, "sy": sy, "w": tw, "h": th,
                "flip_h": c["fh"], "flip_v": c["fv"], "transpose": c["tr"], "origin": origin,
            })
        if lcells:
            data_layers.append({"name": lname, "cells": lcells})
            counts[lname] = len(lcells)
    doc = {
        "name": name,
        "size": [rw, rh],
        "layers": data_layers,
        "tiles": [{"sheet": s, "sx": sx, "sy": sy, "w": w, "h": h}
                  for (s, sx, sy, w, h), _ in sorted(tiles.items(), key=lambda kv: kv[1])],
    }
    with open(OUT / f"{name}.json", "w") as f:
        json.dump(doc, f, indent=2)
    total = sum(counts.values())
    per = " ".join(f"{k}={v}" for k, v in counts.items())
    print(f"  {name:<14} rect={rect} cells={total:<4} tiles={len(tiles):<4} [{per}]")
    return preview, len(tiles)


# --- The author kit: import a standalone user-authored scene as ONE prefab ----------------------
# Everything below is the `--scene` path. It reuses the exact decode + tileset machinery the parcel
# path uses (so a user scene built on the pack tilesets reads identically), and adds the three
# author-facing conventions documented in docs/AUTHORING.md:
#
#   * `Mark:<Kind>` TileMap layers are SEMANTIC, never drawn: any tile painted on them marks that
#     cell. Spawn/Boss/KeepGroup/Zone are understood; an unknown kind is warned about and stored raw.
#   * `Anim:<name>` / `Spin:<name>` Sprite nodes are MOTILES: a frame-cycling overlay or a continuous
#     rotation the renderer animates closed-form from the world clock (chimney smoke, a windmill fan).
#   * a `meta.md` sidecar carries provenance/prose that flows into prefabs.hpp as comments.

# A Mark layer's name is `Mark:<Kind>` or `Mark:<Kind>:<label>` (the label names a Zone). These are
# the kinds codegen understands; anything else is stored raw and skipped in codegen with a warning.
MARK_KINDS = {"Spawn", "Boss", "KeepGroup", "Zone"}

# Default animation period per motile kind, in ms, when the node name carries no `@<ms>` override.
MOTILE_PERIOD_MS = {"anim": 700, "spin": 2000}

_NODE_RE = re.compile(r'\[node name="([^"]+)" type="([^"]+)".*?\]\n(.*?)(?=\n\[node|\Z)', re.S)


def open_res_texture(res_path, pack):
    """A `res://...`-relative texture path (the `res://` already stripped) -> an RGBA PIL image.

    Tries the zip member, then a loose file under assets/_src/ninja/<path> (this is how the FX sheets
    an `Anim:` node points at are found -- they live outside the tilesets dir), then the tilesets dir
    by basename. Only opened here to MEASURE the sheet; build_atlas.py re-resolves the same string.
    """
    member = ZIP_PREFIX + res_path
    if member in pack.names:
        return Image.open(io.BytesIO(pack.zf.read(member))).convert("RGBA")
    loose = SRC / "ninja" / res_path
    if loose.exists():
        return Image.open(loose).convert("RGBA")
    byname = PACK_TILESETS / Path(res_path).name
    if byname.exists():
        return Image.open(byname).convert("RGBA")
    raise FileNotFoundError(f"motile texture {res_path} not found in the pack")


def parse_marks(scene, X0, Y0):
    """`Mark:` TileMap layers -> (spawn, boss, keep, zones, unknown), all in PREFAB-LOCAL tiles.

    spawn/boss/keep are lists of [dx, dy]; zones is a list of {name, x, y, w, h} (the bounding box of
    each `Mark:Zone[:label]` layer's painted cells); unknown maps an unrecognised kind to its raw
    [dx, dy] cells. Marker tiles are never rendered or packed -- only their positions matter.
    """
    spawn, boss, keep, zones, unknown = [], [], [], [], {}
    for m in _NODE_RE.finditer(scene):
        name, ntype, body = m.group(1), m.group(2), m.group(3)
        if ntype != "TileMap" or not name.startswith("Mark:"):
            continue
        kind, _, label = name[len("Mark:"):].partition(":")
        cells = [[c["x"] - X0, c["y"] - Y0] for c in decode(body)]
        if not cells:
            continue
        if kind == "Spawn":
            spawn.extend(cells)
        elif kind == "Boss":
            boss.extend(cells)
        elif kind == "KeepGroup":
            keep.extend(cells)
        elif kind == "Zone":
            xs = [c[0] for c in cells]
            ys = [c[1] for c in cells]
            zones.append({"name": label or "zone", "x": min(xs), "y": min(ys),
                          "w": max(xs) - min(xs) + 1, "h": max(ys) - min(ys) + 1})
        else:
            print(f"  WARNING: unknown marker layer 'Mark:{kind}' -- stored raw, skipped in codegen")
            unknown.setdefault(kind, []).extend(cells)
    return spawn, boss, keep, zones, unknown


def parse_motiles(scene, sext, pack, X0, Y0):
    """`Anim:`/`Spin:` Sprite nodes -> a list of motile dicts in PREFAB-LOCAL pixels.

    A motile node's texture is a horizontal spritesheet strip; `hframes` is the frame count. The node
    NAME carries the kind (Anim = frame cycle, Spin = continuous rotation) and, after an optional
    `@<ms>`, the animation period. `position` is the sprite's CENTRE in scene pixels; it is converted
    to the top-left pixel offset of frame 0 within the prefab. The frames themselves are packed into
    the atlas by build_atlas.py, which re-resolves the `sheet` string written here.
    """
    out = []
    for m in _NODE_RE.finditer(scene):
        name, ntype, body = m.group(1), m.group(2), m.group(3)
        if ntype not in ("Sprite", "AnimatedSprite"):
            continue
        if name.startswith("Anim:"):
            kind, label = "anim", name[len("Anim:"):]
        elif name.startswith("Spin:"):
            kind, label = "spin", name[len("Spin:"):]
        else:
            continue
        period = MOTILE_PERIOD_MS[kind]
        if "@" in label:
            label, _, per = label.partition("@")
            period = int(re.search(r"\d+", per).group(0))
        tex = re.search(r'texture = ExtResource\( ?(\d+) ?\)', body)
        if not tex or tex.group(1) not in sext:
            print(f"  WARNING: motile '{name}' has no resolvable texture -- skipped")
            continue
        res_path = sext[tex.group(1)]
        img = open_res_texture(res_path, pack)
        hframes = int((re.search(r'hframes = (\d+)', body) or [0, 1])[1])
        vframes = int((re.search(r'vframes = (\d+)', body) or [0, 1])[1])
        hframes = max(1, hframes)
        pw = img.width // hframes
        ph = img.height // max(1, vframes)
        px, py = V2((re.search(r'position = (Vector2\([^)]*\))', body) or [0, "Vector2( 0, 0 )"])[1],
                    (0, 0))
        out.append({
            "name": label or kind, "kind": kind, "period_ms": period,
            "sheet": res_path, "sx0": 0, "sy0": 0, "stride": pw,
            "pw": pw, "ph": ph, "frames": hframes,
            # Frame 0's top-left, prefab-local pixels: sprite centre minus the parcel origin and half
            # the frame. Godot draws a Sprite centred on its position.
            "dx": int(round(px - X0 * TILE - pw / 2.0)),
            "dy": int(round(py - Y0 * TILE - ph / 2.0)),
        })
        print(f"  motile {kind:<4} {label!r:<14} {pw}x{ph}x{hframes} @{period}ms  "
              f"at ({out[-1]['dx']},{out[-1]['dy']})px  from {res_path}")
    return out


def parse_meta(scene_path):
    """A scene's `meta.md` sidecar -> {"front": {k: v}, "prose": str} or None.

    Looked for at `<scene>.meta.md` first, then `meta.md` beside the scene. Leading `key: value` lines
    are the front matter; everything after the first blank line (or the first non-`key: value` line) is
    free prose. Both flow into prefabs.hpp as comments above the generated PrefabDef.
    """
    p = Path(scene_path)
    for cand in (p.with_suffix(p.suffix + ".meta.md"), p.parent / "meta.md"):
        if cand.exists():
            break
    else:
        return None
    lines = cand.read_text().splitlines()
    front, i = {}, 0
    while i < len(lines):
        m = re.match(r'^([A-Za-z][\w -]*):\s*(.*)$', lines[i])
        if not m or (lines[i].strip() == ""):
            break
        front[m.group(1).strip()] = m.group(2).strip()
        i += 1
    prose = "\n".join(lines[i:]).strip()
    print(f"  meta.md: {cand.name}  ({len(front)} front-matter keys, "
          f"{len(prose.splitlines())} prose lines)")
    return {"front": front, "prose": prose}


def cut_scene(name, comp, layers, X0, Y0, X1, Y1, extra):
    """Build the one prefab JSON that a whole authored scene becomes, plus a preview PNG.

    Mirrors cut_parcel's per-layer cell schema exactly, but over the scene's FULL bounds (no rect) and
    with the author-kit `extra` fields (spawn_cells, motiles, meta ...) folded in only when present, so
    a scene with no markers is schema-compatible with a plain parcel.
    """
    rx, ry = X0, Y0
    rw, rh = X1 - X0 + 1, Y1 - Y0 + 1
    x0, y0 = map_px(rx, ry, X0, Y0)
    crop = comp.crop((x0, y0, x0 + rw * TILE, y0 + rh * TILE))
    crop.resize((rw * TILE * SCALE, rh * TILE * SCALE), Image.NEAREST).save(OUT / f"{name}.png")

    data_layers, tiles, counts = [], {}, {}
    for lname, ts, cells, origin, z, ysort in layers:
        order = sorted(cells, key=lambda c: c["y"]) if ysort else cells
        lcells = []
        for c in order:
            r = source_rect(ts, c)
            if not r:
                continue
            t, sx, sy, tw, th = r
            tiles.setdefault((t["sheet"], sx, sy, tw, th), len(tiles))
            lcells.append({
                "x": c["x"] - rx, "y": c["y"] - ry,
                "sheet": t["sheet"], "sx": sx, "sy": sy, "w": tw, "h": th,
                "flip_h": c["fh"], "flip_v": c["fv"], "transpose": c["tr"], "origin": origin,
            })
        if lcells:
            data_layers.append({"name": lname, "cells": lcells})
            counts[lname] = len(lcells)
    doc = {
        "name": name,
        "size": [rw, rh],
        "layers": data_layers,
        "tiles": [{"sheet": s, "sx": sx, "sy": sy, "w": w, "h": h}
                  for (s, sx, sy, w, h), _ in sorted(tiles.items(), key=lambda kv: kv[1])],
    }
    doc.update(extra)   # spawn_cells / boss_cells / keep_group_cells / zones / unknown_marks / motiles / meta
    with open(OUT / f"{name}.json", "w") as f:
        json.dump(doc, f, indent=2)
    per = " ".join(f"{k}={v}" for k, v in counts.items())
    print(f"  {name:<14} scene {rw}x{rh} cells={sum(counts.values())} tiles={len(tiles)} [{per}]")


def import_scene(pack, scene_path, name, rect=None):
    """Import one standalone user-authored .tscn as a single prefab JSON (the `--scene` path)."""
    scene = Path(scene_path).read_text()
    sext, ts_ext, ts_sub = resolve_tilesets(scene, pack)

    layers = []
    X0 = Y0 = 10 ** 9
    X1 = Y1 = -10 ** 9
    print("Visual layers:")
    for m in re.finditer(r'\[node name="([^"]+)" type="TileMap".*?\]\n(.*?)(?=\n\[node|\Z)', scene, re.S):
        lname, body = m.group(1), m.group(2)
        if lname.startswith("Mark:"):
            continue  # semantic, handled by parse_marks -- never drawn or packed
        me = re.search(r'tile_set = ExtResource\( (\d+) \)', body)
        ms = re.search(r'tile_set = SubResource\( (\d+) \)', body)
        ts = ts_ext.get(me.group(1)) if me else ts_sub.get(ms.group(1)) if ms else None
        cells = decode(body)
        if not ts or not cells:
            continue
        o = re.search(r'cell_tile_origin = (\d+)', body)
        origin = int(o.group(1)) if o else 0
        z = int((re.search(r'z_index = (-?\d+)', body) or [0, 0])[1])
        ysort = "cell_y_sort = true" in body
        layers.append((lname, ts, cells, origin, z, ysort))
        X0, X1 = min(X0, min(c["x"] for c in cells)), max(X1, max(c["x"] for c in cells))
        Y0, Y1 = min(Y0, min(c["y"] for c in cells)), max(Y1, max(c["y"] for c in cells))
        print(f"  {lname:<14} z={z:>2} {'ysort' if ysort else 'flat '} origin={origin} cells={len(cells)}")
    if not layers:
        raise SystemExit(f"{scene_path}: no drawable TileMap layers found")
    layers.sort(key=lambda L: L[4])

    # Marker cells can sit outside the drawn footprint (a spawn point on the border); grow bounds to
    # include them so nothing is clipped, THEN convert everything to the final prefab-local origin.
    for m in _NODE_RE.finditer(scene):
        if m.group(2) == "TileMap" and m.group(1).startswith("Mark:"):
            for c in decode(m.group(3)):
                X0, X1, Y0, Y1 = min(X0, c["x"]), max(X1, c["x"]), min(Y0, c["y"]), max(Y1, c["y"])

    if rect:  # optional crop: clamp the whole-scene bounds to the requested rect
        cx, cy, cw, ch = rect
        X0, Y0, X1, Y1 = cx, cy, cx + cw - 1, cy + ch - 1

    comp = composite(layers, X0, Y0, X1, Y1)

    print("Markers:")
    spawn, boss, keep, zones, unknown = parse_marks(scene, X0, Y0)
    print("Motiles:")
    motiles = parse_motiles(scene, sext, pack, X0, Y0)
    meta = parse_meta(scene_path)

    extra = {}
    if spawn:
        extra["spawn_cells"] = spawn
    if boss:
        extra["boss_cells"] = boss
    if keep:
        extra["keep_group_cells"] = keep
    if zones:
        extra["zones"] = zones
    if unknown:
        extra["unknown_marks"] = unknown
    if motiles:
        extra["motiles"] = motiles
    if meta:
        extra["meta"] = meta

    print("Scene prefab:")
    cut_scene(name, comp, layers, X0, Y0, X1, Y1, extra)
    print(f"  spawn={len(spawn)} boss={len(boss)} keep={len(keep)} zones={len(zones)} "
          f"motiles={len(motiles)} unknown={sorted(unknown)}")


def contact_print(previews, path):
    """All parcel previews in a row, each under its name, on a dark background."""
    gap, cap = 12, 16
    w = sum(p.width for _, p in previews) + gap * (len(previews) + 1)
    h = max(p.height for _, p in previews) + cap + gap * 2
    sheet = Image.new("RGBA", (w, h), (30, 30, 34, 255))
    d = ImageDraw.Draw(sheet)
    x = gap
    for name, p in previews:
        d.text((x, gap), name, fill=(230, 230, 230, 255))
        sheet.alpha_composite(p, (x, gap + cap))
        x += p.width + gap
    sheet.save(path)
    print(f"  -> {path}  {sheet.size}")


def main():
    ap = argparse.ArgumentParser(description="Cut prefab parcels out of the pack's Village.tscn.")
    ap.add_argument("--map-only", action="store_true", help="only write _map.png, cut no parcels")
    ap.add_argument("--rect", nargs=4, type=int, metavar=("X", "Y", "W", "H"),
                    help="cut one ad-hoc parcel at these MAP tile coords instead of the dict")
    ap.add_argument("--scene", metavar="FILE.tscn",
                    help="import a standalone user-authored Godot 3 scene as ONE prefab (the author "
                         "kit): the whole scene becomes the prefab, Mark:/Anim:/Spin: layers and a "
                         "meta.md sidecar carry semantics -- see docs/AUTHORING.md")
    ap.add_argument("--name", default=None,
                    help="prefab name (default: 'parcel' for --rect, snake_case of the file for --scene)")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    pack = Pack()

    if args.scene:
        stem = Path(args.scene).stem
        name = args.name or re.sub(r'[^a-z0-9]+', '_', stem.lower()).strip('_')
        rect = tuple(args.rect) if args.rect else None
        print(f"Importing scene {args.scene} as prefab '{name}':")
        import_scene(pack, args.scene, name, rect)
        return

    print("Layers (draw order after z-index sort):")
    layers, X0, Y0, X1, Y1 = load_layers(pack)
    print(f"Map extent: x[{X0}..{X1}] y[{Y0}..{Y1}]  size {X1 - X0 + 1}x{Y1 - Y0 + 1} tiles"
          f"  (min x={X0}, min y={Y0})")

    comp = composite(layers, X0, Y0, X1, Y1)

    if args.rect:
        parcels = {(args.name or "parcel"): tuple(args.rect)}
    else:
        parcels = PARCELS

    print("Map render:")
    draw_map(comp, parcels, X0, Y0, X1, Y1, OUT / "_map.png")
    if args.map_only:
        return

    print("Parcels:")
    previews = []
    for name, rect in parcels.items():
        preview, _ = cut_parcel(name, rect, comp, layers, X0, Y0)
        previews.append((name, preview))

    if previews:
        print("Contact print:")
        contact_print(previews, OUT / "preview.png")


if __name__ == "__main__":
    main()
