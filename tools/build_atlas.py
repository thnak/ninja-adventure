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
    # Ninja Adventure tilesets. No gutter, so stride 16. Fill tiles were located by scanning every
    # sheet for low-variance (flat) tiles and matching mean colour against the terrain palette —
    # eyeballing 500 tiles per sheet is not a method.
    "NFloor": (SRC / "ninja/Backgrounds/Tilesets/TilesetFloor.png", 16),
    "NWater": (SRC / "ninja/Backgrounds/Tilesets/TilesetWater.png", 16),
    "NDesert": (SRC / "ninja/Backgrounds/Tilesets/TilesetDesert.png", 16),
    "NRelief": (SRC / "ninja/Backgrounds/Tilesets/TilesetRelief.png", 16),
    "NLogic": (SRC / "ninja/Backgrounds/Tilesets/TilesetLogic.png", 16),
    "NTree": (SRC / "ninja/Backgrounds/Tilesets/TilesetNature.png", 16),
}

TILE = 16
PAD = 1  # extruded border, see module docstring

# Tiny Dungeon draws every creature on top of an opaque dark silhouette — the dungeon floor showing
# through. On grass that reads as a mud blob stuck to the sprite, so it is keyed out at pack time.
# (Verified by compositing both variants over the grass tile before choosing.)
KEY_COLOR = {"TD": (63, 38, 49, 255)}

# slot name -> (sheet, col, row).  Order defines the generated enum order.
MANIFEST = [
    # --- terrain (opaque, drawn as the base layer) — all Ninja Adventure ---
    ("TerrainGrass",     "NFloor",   0, 12),
    ("TerrainDirt",      "NFloor",  11, 19),
    ("TerrainWater",     "NWater",   1,  1),
    ("TerrainStone",     "NFloor",  20, 14),
    ("TerrainSand",      "NDesert", 15,  8),
    ("TerrainSnow",      "NFloor",   0, 19),
    ("TerrainMarsh",     "NRelief",  8,  1),
    ("TerrainAsh",       "NLogic",   6,  0),
    # --- overlays (transparent, drawn over the base layer) ---
    # Ninja Adventure's trees are two tiles as well: canopy on top of trunk.
    # Trees are NOT here — they are 2x3 multi-tile sprites, see BIG_MANIFEST.
    # --- crops: stage 0-1 use the seedling, 2 the growing plant, 3 the ripe per-kind sprite ---
    ("CropSeedling",     "RL", 44, 23),
    ("CropGrowing",      "RL", 44, 24),
    ("CropWheatRipe",    "RL", 42, 23),
    ("CropCarrotRipe",   "RL", 43, 23),
    ("CropPumpkinRipe",  "RL", 41, 23),
    # --- buildings ---
    ("BuildHearth",      "RL", 54,  6),   # campfire — the player's home fire
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
    # NOTE: creatures and the player are NOT here — they are animation sheets, see ANIM_MANIFEST.
]

# --- Multi-tile sprites ------------------------------------------------------
# Ninja Adventure's trees are 2 tiles WIDE and 3 TALL. Slicing a single 16x16 out of one is exactly
# the "cut with the wrong margin" artefact it looks like: you get the left half of a canopy sitting
# on grass. These are packed as one contiguous region and drawn as a single quad, anchored so the
# trunk sits on its own tile and the canopy overhangs the tiles above.
BIG_MANIFEST = [
    # (name, sheet, col, row, tiles wide, tiles tall)
    ("TreeBroad", "NTree", 5, 2, 2, 3),
    ("TreePine",  "NTree", 1, 2, 2, 3),
]

COLS = 8  # atlas width in cells; keeps the texture small and squarish

# --- Animation sheets --------------------------------------------------------
# Ninja Adventure ships each actor as its own file rather than as tiles in a shared sheet, and its
# walk sheets are a 4x4 grid: COLUMN = facing (down, up, left, right), ROW = animation frame.
# (Verified by rendering the grid with labels, not assumed.) These are packed as blocks below the
# single-tile grid, and the generated header exposes `anim_frame(slot, dir, frame)`.
NINJA = SRC / "ninja/Actor"

ANIM_MANIFEST = [
    # (name, path relative to NINJA, cols, rows)
    ("Player",     "Character/NinjaGreen/SeparateAnim/Walk.png", 4, 4),
    ("MobSlime",   "Monster/Slime/Slime.png",                    4, 4),
    ("MobSpider",  "Monster/SpiderRed/SpriteSheet.png",          4, 4),
    ("MobSpirit",  "Monster/Spirit/SpriteSheet.png",             4, 4),
    ("Chicken",    "Animal/Chicken/SpriteSheetWhite.png",        2, 1),
    ("Cow",        "Animal/Cow/SpriteSheetWhite.png",            2, 1),
]


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

    # --- animation blocks, stacked below the tile grid --------------------------------------
    anims = []
    anim_y = rows * cell_px
    max_w = COLS * cell_px
    for name, rel, acols, arows in ANIM_MANIFEST:
        path = NINJA / rel
        if not path.exists():
            print(f"missing animation sheet: {path}", file=sys.stderr)
            return 1
        sheet = Image.open(path).convert("RGBA")
        need_w, need_h = acols * TILE, arows * TILE
        if sheet.width < need_w or sheet.height < need_h:
            print(f"{name}: {path.name} is {sheet.width}x{sheet.height}, "
                  f"expected at least {need_w}x{need_h}", file=sys.stderr)
            return 1
        block_w, block_h = acols * cell_px, arows * cell_px
        # Grow the canvas to fit this block.
        new_w, new_h = max(max_w, block_w), anim_y + block_h
        if new_w > atlas.width or new_h > atlas.height:
            grown = Image.new("RGBA", (max(new_w, atlas.width), max(new_h, atlas.height)), (0, 0, 0, 0))
            grown.paste(atlas, (0, 0))
            atlas = grown
        max_w = max(max_w, block_w)
        for r in range(arows):
            for c in range(acols):
                frame = sheet.crop((c * TILE, r * TILE, c * TILE + TILE, r * TILE + TILE))
                cellimg = Image.new("RGBA", (cell_px, cell_px), (0, 0, 0, 0))
                extrude(cellimg, frame)
                atlas.paste(cellimg, (c * cell_px, anim_y + r * cell_px))
        anims.append((name, PAD, anim_y + PAD, acols, arows))
        anim_y += block_h

    # --- multi-tile sprites, stacked below the animations ------------------------------------
    bigs = []
    for name, key_sheet, col, row, wt, ht in BIG_MANIFEST:
        sheet, stride = sheets[key_sheet]
        region = sheet.crop((col * stride, row * stride, (col + wt) * stride, (row + ht) * stride))
        bw, bh = wt * TILE, ht * TILE
        block = Image.new("RGBA", (bw + 2 * PAD, bh + 2 * PAD), (0, 0, 0, 0))
        block.paste(region, (PAD, PAD))
        new_h = anim_y + bh + 2 * PAD
        if new_h > atlas.height or bw + 2 * PAD > atlas.width:
            grown = Image.new("RGBA", (max(bw + 2 * PAD, atlas.width), max(new_h, atlas.height)),
                              (0, 0, 0, 0))
            grown.paste(atlas, (0, 0))
            atlas = grown
        atlas.paste(block, (0, anim_y))
        bigs.append((name, PAD, anim_y + PAD, wt, ht))
        anim_y += bh + 2 * PAD

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
        "// --- Animation sheets ----------------------------------------------------------------",
        "// Ninja Adventure walk sheets are COLUMN = facing, ROW = frame. Verified by rendering the",
        "// grid with labels; do not reorder these without re-checking the art.",
        "enum : int { kDirDown = 0, kDirUp = 1, kDirLeft = 2, kDirRight = 3, kDirCount = 4 };",
        "",
        "struct AtlasAnim {",
        "    std::int16_t x;",
        "    std::int16_t y;",
        "    std::uint8_t cols;  // facings",
        "    std::uint8_t rows;  // frames",
        "};",
        "",
        "enum class Anim : std::uint8_t {",
    ]
    lines += [f"    k{name}," for name, _, _, _, _ in anims]
    lines += [
        "    kCount,",
        "};",
        "",
        "inline constexpr AtlasAnim kAtlasAnims[static_cast<int>(Anim::kCount)] = {",
    ]
    lines += [f"    {{{x}, {y}, {c}, {r}}},  // k{name}" for name, x, y, c, r in anims]
    lines += [
        "};",
        "",
        "[[nodiscard]] inline constexpr const AtlasAnim& anim_of(Anim a) noexcept {",
        "    return kAtlasAnims[static_cast<int>(a)];",
        "}",
        "",
        "// `dir` and `frame` are wrapped, so a caller may pass a free-running frame counter and a",
        "// facing that this particular sheet does not have (animals only face two ways).",
        "[[nodiscard]] inline constexpr AtlasRect anim_frame(Anim a, int dir, int frame) noexcept {",
        "    const AtlasAnim& s = anim_of(a);",
        "    const int c = (dir % s.cols + s.cols) % s.cols;",
        "    const int r = (frame % s.rows + s.rows) % s.rows;",
        "    return AtlasRect{static_cast<std::int16_t>(s.x + c * (kAtlasTile + 2)),",
        "                     static_cast<std::int16_t>(s.y + r * (kAtlasTile + 2))};",
        "}",
        "",
        "// --- Multi-tile sprites --------------------------------------------------------------",
        "// Drawn as ONE quad spanning `w` x `h` tiles, anchored so the bottom-centre sits on the",
        "// owning tile — a tree's trunk is on its tile and the canopy overhangs the ones above.",
        "struct AtlasBig {",
        "    std::int16_t x;",
        "    std::int16_t y;",
        "    std::uint8_t w;  // tiles wide",
        "    std::uint8_t h;  // tiles tall",
        "};",
        "",
        "enum class Big : std::uint8_t {",
    ]
    lines += [f"    k{name}," for name, _, _, _, _ in bigs]
    lines += [
        "    kCount,",
        "};",
        "",
        "inline constexpr AtlasBig kAtlasBigs[static_cast<int>(Big::kCount)] = {",
    ]
    lines += [f"    {{{x}, {y}, {w}, {h}}},  // k{name}" for name, x, y, w, h in bigs]
    lines += [
        "};",
        "",
        "[[nodiscard]] inline constexpr const AtlasBig& big_of(Big b) noexcept {",
        "    return kAtlasBigs[static_cast<int>(b)];",
        "}",
        "",
        "}  // namespace mmo",
        "",
    ]
    header.write_text("\n".join(lines))

    print(f"{out_png}  {atlas.width}x{atlas.height}  "
          f"({len(entries)} tiles, {len(anims)} anims, {len(bigs)} big)")
    print(f"{header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
