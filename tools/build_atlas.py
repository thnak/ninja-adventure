#!/usr/bin/env python3
"""Pack the sprites this game actually uses out of the CC0 source packs into one atlas.

Why an offline packer rather than loading the source sheets directly:

  * **One texture bind.** The source sheets have different grids (17px stride vs 16px) and only a
    few dozen of their ~5000 tiles are used. Packing produces one small texture, bound once.
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

# (path, stride) — Kenney's roguelike sheet has a 1px gutter; Ninja Adventure's tilesets do not.
#
# Tiny Town and Tiny Dungeon used to be here (walls, towers, plots, creatures). They are gone: the
# first world is entirely Ninja Adventure now, and a sheet listed here is loaded eagerly and fails
# the build if missing, so an unused entry is a liability rather than a convenience.
SHEETS = {
    "RL": (SRC / "roguelike/Spritesheet/roguelikeSheet_transparent.png", 17),
    # Ninja Adventure tilesets. No gutter, so stride 16. Fill tiles were located by scanning every
    # sheet for low-variance (flat) tiles and matching mean colour against the terrain palette —
    # eyeballing 500 tiles per sheet is not a method.
    "NFloor": (SRC / "ninja/Backgrounds/Tilesets/TilesetFloor.png", 16),
    "NWater": (SRC / "ninja/Backgrounds/Tilesets/TilesetWater.png", 16),
    "NDesert": (SRC / "ninja/Backgrounds/Tilesets/TilesetDesert.png", 16),
    "NRelief": (SRC / "ninja/Backgrounds/Tilesets/TilesetRelief.png", 16),
    "NIntFloor": (SRC / "ninja/Backgrounds/Tilesets/Interior/TilesetInteriorFloor.png", 16),
    "NTree": (SRC / "ninja/Backgrounds/Tilesets/TilesetNature.png", 16),
    # Whole buildings. Every rectangle taken out of these was looked at first, magnified, with a
    # tile grid over it — see tools/verify_structures.py and the note above BIG_MANIFEST.
    "NHouse": (SRC / "ninja/Backgrounds/Tilesets/TilesetHouse.png", 16),
    "NCamp": (SRC / "ninja/Backgrounds/Tilesets/tileset_camp.png", 16),
    "NVillage": (SRC / "ninja/Backgrounds/Tilesets/TilesetVillageAbandoned.png", 16),
    "NElem": (SRC / "ninja/Backgrounds/Tilesets/TilesetElement.png", 16),
    "NField": (SRC / "ninja/Backgrounds/Tilesets/TilesetField.png", 16),
}

TILE = 16
PAD = 1  # extruded border, see module docstring

# Per-sheet colour to punch out to transparency. Empty now that Tiny Dungeon is gone (its creatures
# were drawn on an opaque dungeon-floor silhouette that read as a mud blob on grass), kept because
# the mechanism is one line and the next pack may need it.
KEY_COLOR: dict[str, tuple[int, int, int, int]] = {}

# slot name -> (sheet, col, row).  Order defines the generated enum order.
MANIFEST = [
    # --- terrain (opaque, drawn as the base layer) ---
    # Every pick here is `fill_textured` from assets/tile_index.json. The previous set was chosen by
    # scanning for the LOWEST-variance tiles, which selects flat solid colours by construction — the
    # ground rendered as a plain background colour rather than as art. Two were worse than plain:
    # ash was a swatch from the map-EDITOR marker sheet, and marsh was a vertical cliff face.
    # See assets/TILE_INDEX.md for how to query for replacements.
    ("TerrainGrass", "NFloor",  1, 12),
    ("TerrainGrass1", "NFloor",  2, 11),
    ("TerrainGrass2", "NFloor",  3, 12),
    ("TerrainDirt", "NFloor", 12, 19),
    ("TerrainDirt1", "NFloor", 14, 19),
    ("TerrainDirt2", "NFloor", 13, 19),
    ("TerrainWater", "NWater", 11,  2),
    ("TerrainWater1", "NWater", 11,  2),
    ("TerrainWater2", "NWater", 11,  2),
    ("TerrainStone", "NIntFloor",  5, 13),
    ("TerrainStone1", "NIntFloor",  8, 13),
    ("TerrainStone2", "NIntFloor", 10, 15),
    ("TerrainSand", "NFloor",  1,  5),
    ("TerrainSand1", "NFloor",  3,  5),
    ("TerrainSand2", "NFloor",  2,  5),
    ("TerrainSnow", "NFloor",  1, 19),
    ("TerrainSnow1", "NFloor",  3, 19),
    ("TerrainSnow2", "NFloor",  2, 19),
    ("TerrainMarsh", "NFloor", 12, 12),
    ("TerrainMarsh1", "NFloor", 14, 12),
    ("TerrainMarsh2", "NFloor", 13, 12),
    ("TerrainAsh", "NIntFloor", 16, 13),
    ("TerrainAsh1", "NIntFloor", 19, 13),
    ("TerrainAsh2", "NIntFloor", 16, 16),
    # Road and village square. These are the CENTRE cells of the brown-dirt autotile set in
    # TilesetFloor (cols 14-20, rows 7-11) — the interior of a path, with no grass edge on any side.
    # The set's other cells all carry grass on one or more edges and would show a hard seam wherever
    # two path tiles met.
    #
    # The RED set two columns to the left (cols 3-9) is the same shape and was the first pick, but
    # roads run through every ring: a red clay road is fine on grass and looks like a wound across a
    # snowfield. Neutral brown is the only one of the two that works everywhere the road goes.
    ("TerrainPath", "NFloor", 19, 8),
    ("TerrainPath1", "NFloor", 19, 9),
    ("TerrainPath2", "NFloor", 16, 8),
    # A building's footprint is drawn as PATH and then covered by the building's own sprite, so
    # kBuilding needs a base tile that reads as trodden ground under an overhanging roof.
    ("TerrainBuilding", "NFloor", 19, 8),
    ("TerrainBuilding1", "NFloor", 19, 9),
    ("TerrainBuilding2", "NFloor", 16, 8),
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
    # Only two, and that is the point. Walls, towers and fences used to be here as single Kenney
    # tiles; they are gone because this pack has no single-tile version of any of them, and because
    # the game now places whole structures instead (see BIG_MANIFEST and GAME.md §6b).
    # A fire pit: ring of stones, logs inside. One tile, self-contained.
    # (TilesetElement (0,4) was tried first on the strength of its thumbnail and is a MARKET STALL
    # with an awning — which is exactly the failure mode tools/verify_structures.py exists to
    # catch, and which slipped through because a 2x1 crop was not put through it.)
    ("BuildHearth",      "NCamp",  4,  4),
    ("BuildPlot",        "NField", 3,  1),   # ochre tilled field, the interior cell of the set
    # NOTE: creatures and the player are NOT here — they are animation sheets, see ANIM_MANIFEST.
]

# --- Multi-tile sprites ------------------------------------------------------
# Ninja Adventure's trees are 2 tiles WIDE and 3 TALL. Slicing a single 16x16 out of one is exactly
# the "cut with the wrong margin" artefact it looks like: you get the left half of a canopy sitting
# on grass. These are packed as one contiguous region and drawn as a single quad, anchored so the
# trunk sits on its own tile and the canopy overhangs the tiles above.
# The house entries below are the reason this list exists at all. This pack has no single-tile wall
# and no single-tile tower — TilesetHouse's 759 tiles are every one of them a SLICE — so a building
# can only be drawn whole. Every rectangle here was reviewed at 6x with a tile grid over it
# (tools/verify_structures.py) before it was written down, because a footprint that is one column
# too wide is only wrong at its edge: you get a house with a strip of its neighbour's roof attached,
# which no per-tile contact sheet will ever show you.
#
# ORDER MATTERS from `HouseOrange` onward: it must match `StructureKind` in world/worldgen.hpp
# one for one, so the renderer maps a structure to a sprite by adding an offset instead of by
# maintaining a second switch that can drift out of step. There is a static_assert on the count.
BIG_MANIFEST = [
    # (name, sheet, col, row, tiles wide, tiles tall)
    ("TreeBroad", "NTree", 5, 2, 2, 3),
    ("TreePine",  "NTree", 1, 2, 2, 3),
    # --- StructureKind order starts here ---
    ("HouseOrange", "NHouse",  0,  0, 4, 3),
    ("HouseCream",  "NHouse",  4,  0, 4, 3),
    ("HouseAmber",  "NHouse",  8,  0, 4, 3),
    ("HouseRed",    "NHouse", 12,  0, 4, 3),
    ("HouseBlue",   "NHouse", 16,  0, 3, 3),
    ("HouseTan",    "NHouse", 23,  0, 3, 3),
    ("HouseWood",   "NHouse", 26,  0, 3, 3),
    ("HutSnowA",    "NHouse",  0, 11, 3, 3),
    ("HutSnowB",    "NHouse",  3, 11, 3, 3),
    ("HutSnowC",    "NHouse",  6, 11, 3, 3),
    ("RuinA",       "NVillage", 11, 0, 3, 3),
    ("RuinB",       "NVillage", 11, 3, 3, 3),
    ("TentA",       "NCamp",  4,  0, 3, 3),
    ("TentB",       "NCamp",  7,  0, 3, 3),
    ("TentC",       "NCamp", 10,  0, 3, 3),
]

# --- Particle strips ---------------------------------------------------------
# Ambience: drifting leaves, rain, snow. These are NOT on the 16px grid — a leaf is 12x7 and a
# raindrop 8x8 — so they cannot go through the tile packer, which is why they get their own pass.
#
# Frame counts were read off the images, not guessed: each file is one horizontal strip, so
# `frames = width / frame_w` and the split is verified by the assert in the packing loop below.
FX_MANIFEST = [
    # (name, path relative to the ninja pack, frame width, frame height, frames)
    ("Leaf",     "FX/Particle/Leaf.png",     12, 7, 6),
    ("LeafPink", "FX/Particle/LeafPink.png", 12, 7, 6),
    ("Rain",     "FX/Particle/Rain.png",      8, 8, 3),
    ("Snow",     "FX/Particle/Snow.png",      8, 8, 7),
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

    # --- particle strips, stacked below the structures ---------------------------------------
    # No extrusion here on purpose: a particle is drawn at a fixed size in SCREEN space, never
    # sampled at a fractional texture coordinate, so it has no seam to bleed. Padding between
    # frames is enough.
    fxs = []
    for name, rel, fw, fh, frames in FX_MANIFEST:
        path = SRC / "ninja" / rel
        if not path.exists():
            print(f"missing particle sheet: {path}", file=sys.stderr)
            return 1
        sheet = Image.open(path).convert("RGBA")
        if sheet.width != fw * frames or sheet.height < fh:
            print(f"{name}: {path.name} is {sheet.width}x{sheet.height}, expected "
                  f"{fw * frames}x{fh} ({frames} frames of {fw}x{fh})", file=sys.stderr)
            return 1
        strip_w = frames * (fw + 2 * PAD)
        new_h = anim_y + fh + 2 * PAD
        if new_h > atlas.height or strip_w > atlas.width:
            grown = Image.new("RGBA", (max(strip_w, atlas.width), max(new_h, atlas.height)),
                              (0, 0, 0, 0))
            grown.paste(atlas, (0, 0))
            atlas = grown
        for f in range(frames):
            frame = sheet.crop((f * fw, 0, f * fw + fw, fh))
            atlas.paste(frame, (f * (fw + 2 * PAD) + PAD, anim_y + PAD))
        fxs.append((name, PAD, anim_y + PAD, fw, fh, frames))
        anim_y += fh + 2 * PAD

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
        "// Each terrain has this many textured variants, packed consecutively as",
        "// kTerrainX, kTerrainX1, kTerrainX2 — a single repeated motif tiles visibly.",
        "inline constexpr int kTerrainVariants = 3;",
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
        "// --- Particle strips -----------------------------------------------------------------",
        "// Ambience sprites, off the tile grid: a leaf is 12x7, a raindrop 8x8. Frames run left to",
        "// right in one strip.",
        "struct AtlasFx {",
        "    std::int16_t x;",
        "    std::int16_t y;",
        "    std::uint8_t w;",
        "    std::uint8_t h;",
        "    std::uint8_t frames;",
        "};",
        "",
        "enum class Fx : std::uint8_t {",
    ]
    lines += [f"    k{name}," for name, _, _, _, _, _ in fxs]
    lines += [
        "    kCount,",
        "};",
        "",
        "inline constexpr AtlasFx kAtlasFx[static_cast<int>(Fx::kCount)] = {",
    ]
    lines += [f"    {{{x}, {y}, {w}, {h}, {n}}},  // k{name}" for name, x, y, w, h, n in fxs]
    lines += [
        "};",
        "",
        "[[nodiscard]] inline constexpr const AtlasFx& fx_of(Fx f) noexcept {",
        "    return kAtlasFx[static_cast<int>(f)];",
        "}",
        "",
        "// `frame` is wrapped, so a caller may pass a free-running counter.",
        "[[nodiscard]] inline constexpr AtlasRect fx_frame(Fx f, int frame) noexcept {",
        "    const AtlasFx& s = fx_of(f);",
        "    const int i = (frame % s.frames + s.frames) % s.frames;",
        "    return AtlasRect{static_cast<std::int16_t>(s.x + i * (s.w + 2)), s.y};",
        "}",
        "",
        "}  // namespace mmo",
        "",
    ]
    header.write_text("\n".join(lines))

    print(f"{out_png}  {atlas.width}x{atlas.height}  "
          f"({len(entries)} tiles, {len(anims)} anims, {len(bigs)} big, {len(fxs)} fx)")
    print(f"{header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
