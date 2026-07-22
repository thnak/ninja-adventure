#!/usr/bin/env python3
"""Pack the tiles this game actually uses out of three Kenney CC0 packs into one atlas.

Why an offline packer rather than loading the source sheets directly:

  * **One texture bind.** The three packs have different grids (17px stride vs 16px) and only ~20
    of their ~2000 tiles are used. Packing produces a single small texture the renderer binds once.
  * **No magic numbers in C++.** This script emits `src/render/atlas_slots.hpp`, so the manifest
    below is the single source of truth for "which sprite is a slime". Re-arting the game is an
    edit here plus a re-run — no renderer change.
  * **Edge extrusion.** Each tile is padded by 1px of its own border colour. The camera zooms
    continuously, so a source rect can map to fractional texture coordinates; without the extruded
    border, sampling at a tile edge can pick up the neighbouring tile and show seams.

This is the role ARCHITECTURE.md reserves for Python: build-time tooling, never the runtime.

    tools/build_atlas.py            # writes assets/atlas.png + src/render/atlas_slots.hpp
"""
import pathlib
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "_src"

# (path, stride) — Kenney's roguelike sheets have a 1px gutter, the "tiny" tilemaps do not.
SHEETS = {
    "RL": (SRC / "roguelike/Spritesheet/roguelikeSheet_transparent.png", 17),
    "TT": (SRC / "tinytown/Tilemap/tilemap_packed.png", 16),
    "TD": (SRC / "tinydungeon/Tilemap/tilemap_packed.png", 16),
}

TILE = 16
PAD = 1  # extruded border, see module docstring

# Tiny Dungeon draws every creature on top of an opaque dark silhouette — the dungeon floor showing
# through. On grass that reads as a mud blob stuck to the sprite, so it is keyed out at pack time.
# (Verified by compositing both variants over the grass tile before choosing.)
KEY_COLOR = {"TD": (63, 38, 49, 255)}

# slot name -> (sheet, col, row).  Order defines the generated enum order.
MANIFEST = [
    # --- terrain (opaque, drawn as the base layer) ---
    ("TerrainGrass",     "RL",  5,  1),
    ("TerrainDirt",      "RL",  6,  1),
    ("TerrainWater",     "RL",  1,  0),
    ("TerrainStone",     "RL",  7,  1),
    ("TerrainSand",      "RL",  8,  1),
    # --- overlays (transparent, drawn over grass) ---
    # The roguelike pack's tall tree is TWO tiles: canopy dome on top of canopy-base-plus-trunk.
    # Row 9 holds a complete one-tile tree, but the two-tile version is what reads as a forest.
    ("TreeTop",          "RL", 13, 10),
    ("Tree",             "RL", 13, 11),
    # A second species, picked per tile by hash. One repeated tree sprite made a forest read as a
    # stamped grid; two silhouettes plus mirroring breaks the pattern without more art.
    ("TreeTopPine",      "RL", 16, 10),
    ("TreePine",         "RL", 16, 11),
    # --- crops: stage 0-1 use the seedling, 2 the growing plant, 3 the ripe per-kind sprite ---
    ("CropSeedling",     "RL", 44, 23),
    ("CropGrowing",      "RL", 44, 24),
    ("CropWheatRipe",    "RL", 42, 23),
    ("CropCarrotRipe",   "RL", 43, 23),
    ("CropPumpkinRipe",  "RL", 41, 23),
    # --- buildings ---
    ("BuildCore",        "TT", 10,  7),
    # Tiny Town's castle wall is a 3x3 nine-slice. An earlier version used its TOP-LEFT CORNER for
    # every wall tile, so a horizontal run repeated a left edge over and over. A one-tile-thick,
    # free-form player wall cannot use a nine-slice at all, so instead:
    #   WallRun      top-middle: crenellations along the length, no side edges. Drawn unrotated for
    #                a horizontal run and rotated 90 degrees for a vertical one.
    #   Wall         plain brick, no directional edges - correct for an isolated tile or a junction.
    ("BuildWall",        "TT",  6, 10),
    ("BuildWallRun",     "TT",  1,  8),
    ("BuildTurret",      "TT", 10,  9),
    ("BuildPlot",        "TT",  7,  3),
    # Fence is a directional set too, handled the same way as the wall: a vertical bar rotated for
    # horizontal runs, with the cross piece for isolated posts and junctions.
    ("BuildFence",       "TT", 11,  4),
    ("BuildFencePost",   "TT",  9,  3),
    # Where a wave comes out of. One tile, self-contained, unmistakably "things arrive here".
    ("SpawnCamp",        "RL", 50,  9),
    # --- creatures ---
    ("MobSlime",         "TD",  0,  9),
    ("MobSpider",        "TD",  2, 10),
    ("MobGhost",         "TD",  1, 10),
    ("Player",           "TD",  0,  8),
]

COLS = 8  # atlas width in cells; keeps the texture small and squarish


def extrude(cell: Image.Image, tile: Image.Image) -> None:
    """Paste `tile` at (PAD,PAD) and smear its border pixels outward into the padding."""
    cell.paste(tile, (PAD, PAD))
    w = h = TILE
    for i in range(w):
        top, bottom = tile.getpixel((i, 0)), tile.getpixel((i, h - 1))
        for p in range(PAD):
            cell.putpixel((PAD + i, p), top)
            cell.putpixel((PAD + i, PAD + h + p), bottom)
    for j in range(h):
        left, right = tile.getpixel((0, j)), tile.getpixel((w - 1, j))
        for p in range(PAD):
            cell.putpixel((p, PAD + j), left)
            cell.putpixel((PAD + w + p, PAD + j), right)
    # Corners take the nearest diagonal pixel.
    for (cx, cy), (sx, sy) in (
        ((0, 0), (0, 0)),
        ((PAD + w, 0), (w - 1, 0)),
        ((0, PAD + h), (0, h - 1)),
        ((PAD + w, PAD + h), (w - 1, h - 1)),
    ):
        px = tile.getpixel((sx, sy))
        for dy in range(PAD):
            for dx in range(PAD):
                cell.putpixel((cx + dx, cy + dy), px)


def main() -> int:
    sheets = {}
    for key, (path, stride) in SHEETS.items():
        if not path.exists():
            print(f"missing source sheet: {path}\nrun tools/fetch_assets.sh first", file=sys.stderr)
            return 1
        sheets[key] = (Image.open(path).convert("RGBA"), stride)

    cell_px = TILE + 2 * PAD
    rows = (len(MANIFEST) + COLS - 1) // COLS
    atlas = Image.new("RGBA", (COLS * cell_px, rows * cell_px), (0, 0, 0, 0))

    entries = []
    for i, (name, key_sheet, col, row) in enumerate(MANIFEST):
        sheet, stride = sheets[key_sheet]
        sx, sy = col * stride, row * stride
        if sx + TILE > sheet.width or sy + TILE > sheet.height:
            print(f"{name}: ({col},{row}) is outside {key_sheet}", file=sys.stderr)
            return 1
        tile = sheet.crop((sx, sy, sx + TILE, sy + TILE))
        if key := KEY_COLOR.get(key_sheet):
            px = tile.load()
            for y in range(TILE):
                for x in range(TILE):
                    if px[x, y] == key:
                        px[x, y] = (0, 0, 0, 0)

        cx, cy = (i % COLS) * cell_px, (i // COLS) * cell_px
        cell = Image.new("RGBA", (cell_px, cell_px), (0, 0, 0, 0))
        extrude(cell, tile)
        atlas.paste(cell, (cx, cy))
        entries.append((name, cx + PAD, cy + PAD))

    out_png = ROOT / "assets" / "atlas.png"
    atlas.save(out_png)

    header = ROOT / "src" / "render" / "atlas_slots.hpp"
    lines = [
        "// GENERATED by tools/build_atlas.py — do not edit by hand.",
        "//",
        "// Slot rects into assets/atlas.png, packed from three Kenney CC0 packs (see",
        "// assets/CREDITS.md). Change the art by editing the MANIFEST in the packer and re-running",
        "// it; nothing in the renderer hard-codes a sprite position.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mmo {",
        "",
        f"inline constexpr int kAtlasTile = {TILE};",
        "",
        "struct AtlasRect {",
        "    std::int16_t x;",
        "    std::int16_t y;",
        "};",
        "",
        "enum class Slot : std::uint8_t {",
    ]
    lines += [f"    k{name}," for name, _, _ in entries]
    lines += [
        "    kCount,",
        "};",
        "",
        "inline constexpr AtlasRect kAtlasRects[static_cast<int>(Slot::kCount)] = {",
    ]
    lines += [f"    {{{x}, {y}}},  // k{name}" for name, x, y in entries]
    lines += [
        "};",
        "",
        "[[nodiscard]] inline constexpr AtlasRect rect_of(Slot s) noexcept {",
        "    return kAtlasRects[static_cast<int>(s)];",
        "}",
        "",
        "}  // namespace mmo",
        "",
    ]
    header.write_text("\n".join(lines))

    print(f"{out_png}  {atlas.width}x{atlas.height}  ({len(entries)} slots)")
    print(f"{header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
