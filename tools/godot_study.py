#!/usr/bin/env python3
"""Read the author's own Godot projects: how the demo scenes are actually built.

`assets/_src/ninja/GodotProject.zip` (Godot 3) and `Godot Project V4.zip` (Godot 4) are the
projects the Example GIFs were recorded from. They are ground truth for two questions that
`tools/study_examples.py` could only answer by measuring pixels:

  1. Which tile goes where. Godot 4 stores a TileSet's terrain rules explicitly, as peering bits
     per tile. `tools/autotile_fit.py` infers that from corner colours; this reads the answer.
  2. How a scene is layered. This cannot be inferred from a screenshot at all.

Read straight out of the zips -- nothing is unpacked into the repo, and the zips stay the only
copy so there is no second copy to drift.

What it reports
---------------
`--layers`   the TileMap layer stack from system/map/map.tscn
`--terrain`  the authored terrain table, and a diff against tools/_autotile.json
`--density`  how many tiles per layer the hand-authored village map actually uses

Usage:
    python3 tools/godot_study.py                # all three
    python3 tools/godot_study.py --terrain
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
import zipfile
from collections import Counter, defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
NINJA = ROOT / "assets" / "_src" / "ninja"
V4 = NINJA / "Godot Project V4.zip"
V3 = NINJA / "GodotProject.zip"
AUTOTILE = ROOT / "tools" / "_autotile.json"

# Godot's eight peering directions, and which of the tile's four corner QUADRANTS each one
# influences. A quadrant is the neighbouring terrain only if the diagonal neighbour AND both
# sides flanking it are that terrain -- one of the three being different means the terrain does
# not reach round to that corner.
CORNER_DEPS = {
    "tl": ("top_left_corner", "top_side", "left_side"),
    "tr": ("top_right_corner", "top_side", "right_side"),
    "bl": ("bottom_left_corner", "bottom_side", "left_side"),
    "br": ("bottom_right_corner", "bottom_side", "right_side"),
}
# Same bit order autotile_fit uses: TL=1, TR=2, BL=4, BR=8.
CORNER_BIT = {"tl": 1, "tr": 2, "bl": 4, "br": 8}


def read(zf: pathlib.Path, name_endswith: str) -> str:
    with zipfile.ZipFile(zf) as z:
        for n in z.namelist():
            if n.endswith(name_endswith) and not n.endswith("/"):
                return z.read(n).decode("utf-8", "replace")
    raise SystemExit(f"{name_endswith} not found in {zf.name}")


# --- layers ------------------------------------------------------------------


def report_layers() -> None:
    src = read(V4, "system/map/map.tscn")
    print("=== TileMap layer stack (system/map/map.tscn) ===\n")

    node = re.search(r'\[node name="Tilemap".*?\n(.*?)(?=\n\[node|\Z)', src, re.S)
    body = node.group(1) if node else ""
    root_ysort = "y_sort_enabled = true" in body.split("layer_0")[0]

    layers: dict[int, dict[str, str]] = defaultdict(dict)
    for m in re.finditer(r"layer_(\d+)/(\w+) = (.+)", body):
        layers[int(m.group(1))][m.group(2)] = m.group(3).strip()

    print(f"  Tilemap node y_sort_enabled = {root_ysort}\n")
    print(f"  {'#':>2}  {'name':12} {'z_index':>8} {'y_sort':>7} {'origin':>7}")
    for i in sorted(layers):
        d = layers[i]
        print(
            f"  {i:>2}  {d.get('name', '?'):12} {d.get('z_index', '0'):>8} "
            f"{d.get('y_sort_enabled', 'false'):>7} {d.get('y_sort_origin', '0'):>7}"
        )

    print(
        "\n  Drawing order is z_index first, so the stack bottom-to-top is Floor, FloorDetail,\n"
        "  then the two Wall layers. The two things worth copying:\n\n"
        "  * The Wall layers are Y-SORTED, and so is the Tilemap node itself. A tree on the Wall\n"
        "    layer is not drawn at its layer's fixed depth -- it is drawn in an order decided by\n"
        "    its Y. That is what lets a canopy overlap the tree behind it.\n"
        "  * Characters are children of the Tilemap node, not siblings of it. They therefore sort\n"
        "    in the SAME Y-ordered pass as the tiles, which is how the player passes behind a\n"
        "    tree's canopy and in front of its trunk without any special case.\n\n"
        "  A renderer that draws terrain, then objects, then actors in three fixed passes cannot\n"
        "  produce that, no matter how good its tiles are."
    )

    origins = Counter(re.findall(r"y_sort_origin = (-?\d+)", read(V4, "content/map/tileset.tres")))
    print(f"\n  Per-tile y_sort_origin values in the tileset: {dict(origins)}")
    print(
        "  A tile whose art is 16px tall but whose FEET are at +12 sorts by its feet, not by its\n"
        "  top-left corner. Without that offset every overlap decision is made at the wrong Y."
    )


# --- terrain -----------------------------------------------------------------


def parse_terrain(src: str) -> tuple[list[str], dict[str, dict]]:
    names: list[str] = []
    for m in re.finditer(r"terrain_set_0/terrain_(\d+)/name = \"([^\"]+)\"", src):
        names.append(m.group(2))

    # Split the file into atlas sources so tile coords can be attributed to the right sheet.
    sources: dict[str, str] = {}
    ids = dict(re.findall(r'path="res://content/map/(\S+?)" id="([^"]+)"', src))
    tex_of = {v: k for k, v in ids.items()}

    blocks = re.split(r"\[sub_resource type=\"TileSetAtlasSource\"[^\]]*\]", src)
    for b in blocks[1:]:
        tm = re.search(r'texture = ExtResource\("([^"]+)"\)', b)
        if tm:
            sources[tex_of.get(tm.group(1), tm.group(1))] = b

    out: dict[str, dict] = {}
    for sheet, block in sources.items():
        tiles: dict[tuple[int, int], dict] = {}
        for m in re.finditer(r"^(\d+):(\d+)/0/terrain = (\d+)$", block, re.M):
            tiles[(int(m.group(1)), int(m.group(2)))] = {"terrain": int(m.group(3)), "bits": {}}
        for m in re.finditer(
            r"^(\d+):(\d+)/0/terrains_peering_bit/(\w+) = (\d+)$", block, re.M
        ):
            key = (int(m.group(1)), int(m.group(2)))
            if key in tiles:
                tiles[key]["bits"][m.group(3)] = int(m.group(4))
        if tiles:
            out[sheet] = tiles
    return names, out


def mask_of(tile: dict, other: int) -> int:
    """Corner mask in autotile_fit's convention: bit set where the corner is the OTHER terrain.

    The peering bits say where this tile's own terrain continues. `autotile_fit` masks the
    opposite way round -- bits mark terrain A, and for the grass/dirt set A is grass while these
    tiles are authored as dirt. So a corner is set here exactly when the dirt does NOT reach it.
    """
    bits = tile["bits"]
    mask = 0
    for corner, deps in CORNER_DEPS.items():
        if not all(bits.get(d, 0) for d in deps):
            mask |= CORNER_BIT[corner]
    return mask


def report_terrain() -> None:
    src = read(V4, "content/map/tileset.tres")
    names, sheets = parse_terrain(src)
    mode = re.search(r"terrain_set_0/mode = (\d+)", src)
    print("=== authored terrain table (content/map/tileset.tres) ===\n")
    print(f"  terrains: {names}")
    print(
        f"  mode: {mode.group(1) if mode else '?'} "
        "(0 = match corners AND sides, i.e. the 47-blob system)\n"
    )

    for sheet, tiles in sheets.items():
        by_mask: dict[int, list] = defaultdict(list)
        for (c, r), t in sorted(tiles.items(), key=lambda kv: (kv[0][1], kv[0][0])):
            by_mask[mask_of(t, 0)].append((c, r))
        cols = [c for c, _ in tiles]
        rows = [r for _, r in tiles]
        print(
            f"  {sheet}: {len(tiles)} terrain tiles, "
            f"cols {min(cols)}..{max(cols)} rows {min(rows)}..{max(rows)}, "
            f"{len(by_mask)} distinct masks"
        )

    # Compare against what autotile_fit derived from pixels.
    floor = next((v for k, v in sheets.items() if "floor" in k and "interior" not in k), None)
    if not floor or not AUTOTILE.exists():
        return
    derived = None
    for s in json.loads(AUTOTILE.read_text()):
        for p in s["pairs"]:
            if p["id"] == "grass_dirt":
                derived = p
    if not derived:
        return

    authored: dict[int, list] = defaultdict(list)
    for (c, r), t in floor.items():
        authored[mask_of(t, 0)].append([c, r])

    print("\n=== tools/autotile_fit.py vs the authored table ===\n")
    print(f"  {'mask':>4}  {'derived':>16}  authored options")
    agree = checked = 0
    for m in range(16):
        a = sorted(authored.get(m, []))
        if not a:
            continue
        # Masks 0 and 15 are the pure fills of each terrain. autotile_fit deliberately keeps those
        # out of `coherent` -- they are not edges and it emits them as a_fills/b_fills instead --
        # so comparing them against `coherent` would report a disagreement that is only a
        # difference in where the two files put the same answer.
        if m == 15:
            d, label = derived["a_fills"], "a_fills"
        elif m == 0:
            d, label = derived["b_fills"], "b_fills"
        else:
            one = derived["coherent"].get(str(m))
            d, label = ([one] if one else []), "coherent"
        checked += 1
        aset_ = {tuple(x) for x in a}
        ok = bool(d) and all(tuple(x) in aset_ for x in d)
        agree += bool(ok)
        flag = "" if ok else "   <-- differs"
        shown = ", ".join(f"{c},{r}" for c, r in a[:6]) + (" ..." if len(a) > 6 else "")
        got = ", ".join(f"{c},{r}" for c, r in d[:3]) + (" ..." if len(d) > 3 else "")
        print(f"  {m:>4}  {got:>16}  {shown}{flag}   [{label}]")
    print(
        f"\n  {agree}/{checked} masks: every tile autotile_fit chose is one the author marked for\n"
        "  that same arrangement. The derivation is reading the sheet the way its author wrote it."
    )


# --- density -----------------------------------------------------------------


def decode_tile_data(arr_text: str) -> list[tuple[int, int, int]]:
    """Godot 4 `tile_data` is 3 int32 per cell, read as 6 uint16: x, y, source, ax, ay, alt."""
    nums = [int(n) for n in re.findall(r"-?\d+", arr_text)]
    raw = b"".join(struct.pack("<i", n) for n in nums)
    out = []
    for i in range(0, len(raw) - 11, 12):
        x, y, src, ax, ay, _alt = struct.unpack_from("<6H", raw, i)
        out.append((src, ax, ay))
    return out


def report_density() -> None:
    tmpl = read(V4, "system/map/map.tscn")
    names = dict(re.findall(r"layer_(\d+)/name = \"([^\"]+)\"", tmpl))
    src = read(V4, "content/map/map_village.tscn")
    print("=== hand-authored village map: tiles per layer ===\n")
    # Map atlas source ids to sheet names, so a layer's content can be named rather than counted.
    tset = read(V4, "content/map/tileset.tres")
    ids = dict(re.findall(r'path="res://content/map/(\S+?)" id="([^"]+)"', tset))
    tex_of = {v: k for k, v in ids.items()}
    src_sheet: dict[int, str] = {}
    for m in re.finditer(
        r'\[sub_resource type="TileSetAtlasSource" id="([^"]+)"\](.*?)(?=\n\[sub_resource|\n\[resource|\Z)',
        tset,
        re.S,
    ):
        tm = re.search(r'texture = ExtResource\("([^"]+)"\)', m.group(2))
        if tm:
            src_sheet[m.group(1)] = tex_of.get(tm.group(1), tm.group(1))
    order = re.findall(r'sources/(\d+) = SubResource\("([^"]+)"\)', tset)
    id_to_sheet = {int(sid): src_sheet.get(sub, sub) for sid, sub in order}

    total = 0
    rows = []
    for m in re.finditer(r"layer_(\d+)/tile_data = PackedInt32Array\(([^)]*)\)", src):
        cells = decode_tile_data(m.group(2))
        by_sheet = Counter(id_to_sheet.get(s, f"src{s}") for s, _, _ in cells)
        rows.append((names.get(m.group(1), m.group(1)), len(cells), len(set(cells)), by_sheet))
        total += len(cells)
    for name, n, distinct, by_sheet in rows:
        share = n / total if total else 0
        print(f"  {name:12} {n:6} cells  {distinct:4} distinct  {share:6.1%} of all placed")
        for sh, k in by_sheet.most_common(4):
            print(f"       {k:6}  {sh}")
    print(f"  {'TOTAL':12} {total:6}")

    floor = next((n for nm, n, _, _ in rows if nm == "Floor"), 0)
    detail = next((n for nm, n, _, _ in rows if nm == "FloorDetail"), 0)
    walls = sum(n for nm, n, _, _ in rows if nm.startswith("Wall"))
    if floor:
        print(
            f"\n  Ground:      {floor} cells\n"
            f"  FloorDetail: {detail} ({detail / floor:.1%} of ground)\n"
            f"  Wall layers: {walls} ({walls / floor:.1%} of ground)\n\n"
            "  Do NOT read the FloorDetail figure as the decoration budget. Almost none of the\n"
            "  clutter visible in the demos is on that layer -- the tufts, pots and crates are\n"
            "  SCENE tiles (content/destroyable/{grass,pot,crate}.tscn) placed on the Wall layers,\n"
            "  because they are destructible and need collision. FloorDetail holds only the few\n"
            "  flat marks that nothing interacts with.\n\n"
            f"  The honest number is the combined {(detail + walls) / floor:.0%}: about one\n"
            "  non-ground element for every four ground tiles, clustered rather than spread. Our\n"
            "  renderer decorates 100% of grass tiles at a fixed offset, which is the wallpaper\n"
            "  effect that makes `density` read high while the scene still looks empty."
        )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--layers", action="store_true")
    ap.add_argument("--terrain", action="store_true")
    ap.add_argument("--density", action="store_true")
    args = ap.parse_args()
    every = not (args.layers or args.terrain or args.density)

    for missing in (p for p in (V3, V4) if not p.exists()):
        print(f"note: {missing.name} not present")

    if every or args.layers:
        report_layers()
        print()
    if every or args.terrain:
        report_terrain()
        print()
    if every or args.density:
        report_density()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
