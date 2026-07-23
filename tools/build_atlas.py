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
import argparse
import io
import json
import math
import pathlib
import re
import sys
import zipfile
from pathlib import Path

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
    "NIntWall": (SRC / "ninja/Backgrounds/Tilesets/Interior/TilesetInterior.png", 16),
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
    #
    # VARIANT 0 IS THE PLAIN FILL, variants 1 and 2 are textured. That order is load-bearing: the
    # renderer draws variant 0 almost everywhere and only reaches for a textured one inside a
    # clustered patch (see `decor_gate` in raylib_bridge.cpp). Stamping a motif on every tile was
    # measured against the pack's own maps and is roughly four times their detail density — uniform
    # detail everywhere reads as wallpaper, which is why our `density` metric came out nearly double
    # the demos' while the screenshots looked emptier.
    #
    # Every plain pick sits next to its textured partner on the same sheet and is within a colour
    # distance of 9 of it (queried from assets/tile_index.json by role `fill_plain`) — a plain tile
    # from elsewhere in the pack would be a different shade of the same terrain, which repaints the
    # hard seam this whole change exists to remove.
    ("TerrainGrass", "NFloor",  0, 12),
    ("TerrainGrass1", "NFloor",  1, 12),
    ("TerrainGrass2", "NFloor",  3, 12),
    ("TerrainDirt", "NFloor", 11, 19),
    ("TerrainDirt1", "NFloor", 12, 19),
    ("TerrainDirt2", "NFloor", 14, 19),
    # Water's plain fill is the still body inside the pack's own shore sets, and its two textured
    # variants are the rippled tile that used to be all three. A whole-tile motif on every tile of a
    # lake is the single largest source of grid-aligned edges in any shot with water in it.
    ("TerrainWater", "NWater",  1,  7),
    ("TerrainWater1", "NWater", 11,  2),
    ("TerrainWater2", "NWater", 11,  4),
    # Stone and ash have no plain fill of their own shade anywhere in the pack — the nearest are 21
    # and 17 away on other sheets, which is over the ~10 that starts to read as a second terrain. So
    # these two keep a textured tile in slot 0 and simply stay busy. Written down rather than
    # silently fudged, because it is the one place the rule above does not hold.
    ("TerrainStone", "NIntFloor",  5, 13),
    ("TerrainStone1", "NIntFloor",  8, 13),
    ("TerrainStone2", "NIntFloor", 10, 15),
    ("TerrainSand", "NFloor",  0,  5),
    ("TerrainSand1", "NFloor",  1,  5),
    ("TerrainSand2", "NFloor",  3,  5),
    ("TerrainSnow", "NFloor",  0, 19),
    ("TerrainSnow1", "NFloor",  1, 19),
    ("TerrainSnow2", "NFloor",  3, 19),
    ("TerrainMarsh", "NFloor", 11, 12),
    ("TerrainMarsh1", "NFloor", 12, 12),
    ("TerrainMarsh2", "NFloor", 14, 12),
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
    ("TerrainPath", "NFloor", 12, 8),
    ("TerrainPath1", "NFloor", 19, 9),
    ("TerrainPath2", "NFloor", 16, 8),
    # A building's footprint is drawn as PATH and then covered by the building's own sprite, so
    # kBuilding needs a base tile that reads as trodden ground under an overhanging roof.
    ("TerrainBuilding", "NFloor", 12, 8),
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

# --- The interior room -------------------------------------------------------
# `TilesetInteriorWall.tres` is `tile_mode = 2` — an ATLAS, not an autotile. The author places its
# pieces by hand, so unlike the four floor sets there is no mask table to read out of the zip. What
# there is, is his own `World/Maps/Interior.tscn`, and rebuilding it (tools/_study/godot/) shows
# exactly which cell of the block he uses for each edge and corner. Those eight coordinates are
# below; `(1,9)` as the bottom-left was the only one that had to be confirmed by eye.
#
# The room is composed as ONE sprite rather than laid tile by tile, because it is always the same
# rectangle: there is nothing for an autotiler to decide. Its interior is left TRANSPARENT so the
# floor underneath is drawn by the ordinary terrain path, variants, mirroring and all — a 10x7 field
# of one repeated tile baked into a sprite is a visible lattice.
ROOM_W, ROOM_H = 12, 9   # must match kRoomW/kRoomH + 2 in world/tiles.hpp
ROOM_DOOR_X = 5          # the gap in the bottom wall, in sprite columns

ROOM_NINE = {
    "tl": (1, 0), "t": (5, 0), "tr": (6, 0),
    "l":  (1, 3),               "r":  (6, 3),
    "bl": (1, 9), "b": (5, 9), "br": (6, 9),
}


def compose_room(sheet):
    room = Image.new("RGBA", (ROOM_W * TILE, ROOM_H * TILE), (0, 0, 0, 0))
    for y in range(ROOM_H):
        for x in range(ROOM_W):
            if y == 0:
                key = "tl" if x == 0 else ("tr" if x == ROOM_W - 1 else "t")
            elif y == ROOM_H - 1:
                key = "bl" if x == 0 else ("br" if x == ROOM_W - 1 else "b")
            elif x == 0:
                key = "l"
            elif x == ROOM_W - 1:
                key = "r"
            else:
                continue  # the floor shows through
            if y == ROOM_H - 1 and x == ROOM_DOOR_X:
                continue  # the doorway
            c, r = ROOM_NINE[key]
            room.paste(sheet.crop((c * TILE, r * TILE, (c + 1) * TILE, (r + 1) * TILE)),
                       (x * TILE, y * TILE))
    return room


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
    # Both trees are FOUR tiles wide, not two. The 2x3 rectangles that used to be here took the
    # middle two columns: they kept the centre lobe and the trunk and threw away both side lobes,
    # cutting through 58% and 60% of non-outline pixels on each border. That is why the forest read
    # as a palisade of narrow columns rather than as a canopy. `tools/check_sprite_rects.py --rect
    # NTree 4 2 4 3` reports 0% on all four borders; the old rectangle reports 58%.
    ("TreeBroad", "NTree", 4, 2, 4, 3),
    ("TreePine",  "NTree", 0, 2, 4, 3),
    # The one entry here that is COMPOSED rather than cropped. It has to be: an interior room is a
    # nine-slice, and this pack ships the nine slices without shipping a room. `compose_room` below
    # assembles one. It sits before the structures because `StructureKind` order starts at
    # `HouseOrange` and a static_assert in the renderer counts from there.
    ("Room", None, 0, 0, ROOM_W, ROOM_H),
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
    # --- the rampart. See world/village.hpp for how these four fit together. ---
    # The pack HAS a palisade, in TilesetHouse, and it is the one thing this manifest was missing:
    # a 3x5 log post, a 3x3 plank wall to run between two of them, the same wall with an arch cut
    # through it, and a 1x2 stake fence. The Godot project's own Village.tscn uses exactly these
    # four (House layer, tile ids 39/40/41 stacked = the log, 42 = wall, 43 = arch, 44/45/46 =
    # stakes), which is how the rectangles below were found rather than guessed.
    #
    # `check_sprite_rects.py --rect NHouse 19 3 3 5` reports 1/1/0/0 — the log is a whole sprite.
    # The other three deliberately score ~90% on left and right, and that is CORRECT here: a wall
    # is drawn to butt its own neighbour, so opaque pixels running off both sides is what a tiling
    # run looks like. The tool cannot tell that from a severed sprite; the grid render can.
    ("LogPost",     "NHouse", 19,  3, 3, 5),
    ("Rampart",     "NHouse", 22,  3, 3, 3),
    ("Gate",        "NHouse", 22,  6, 3, 3),
    # Three stakes, not one. They are near-identical by design — the difference is a pixel of
    # highlight — and cycling them along a run is what stops a fence reading as one sprite stamped
    # forty times.
    ("StakeA",      "NHouse", 22, 12, 1, 2),
    ("StakeB",      "NHouse", 23, 12, 1, 2),
    ("StakeC",      "NHouse", 24, 12, 1, 2),
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
    # --- combat ---------------------------------------------------------------------------------
    # An arrow is one 16x16 sprite rather than a tile in the terrain atlas, because it is the one
    # thing in the game that has to be drawn ROTATED — and `fx()` is already the draw path that
    # takes a rotation. Frame count 1: it does not animate, it points.
    ("Arrow",    "Items/Projectile/Arrow.png", 16, 16, 1),
    # The four schools and the blow that detonates them. These are the pack's own elemental strips;
    # their frame widths are NOT 16 and are not multiples of it, which is why FX_MANIFEST exists as
    # a separate concept from the tile grid at all. Each width below was measured by scanning the
    # sheet for fully transparent columns, not guessed from the file size.
    ("Slash",    "FX/Attack/Cut/SpriteSheet.png",           32, 32,  4),
    ("Fire",     "FX/Elemental/Flam/SpriteSheet.png",       25, 30,  8),
    ("Ice",      "FX/Elemental/Ice/SpriteSheet.png",        32, 32, 10),
    ("Earth",    "FX/Elemental/Rock/SpriteSheet.png",       30, 30, 14),
    ("Shock",    "FX/Elemental/Thunder/SpriteSheet.png",    20, 28,  8),
    ("Blast",    "FX/Elemental/Explosion/SpriteSheet.png",  40, 40,  9),
    # The Character screen's portrait. Ninja Adventure ships a 38x38 `Faceset.png` beside every
    # actor — off the 16px grid, like every other entry here, which is exactly what this list is
    # for. One frame: it is a portrait, not an animation.
    ("FacePlayer", "Actor/Character/NinjaGreen/Faceset.png", 38, 38, 1),
]

COLS = 8  # atlas width in cells; keeps the texture small and squarish

# --- Terrain edge sets ---------------------------------------------------------
# ONE OVERLAY SET PER TERRAIN, NOT ONE SET PER PAIR. `Terrain` has 11 values, so a set for every
# pair would be 55 x 47 = 2585 tiles. Where two terrains meet, the renderer fills the tile with the
# LOWER-priority one and lays the higher-priority terrain's own edge tile over it, so this costs
# 11 x 47 = 517 tiles and covers every pair including ones that never occur.
#
# THE MASK LAYOUT IS GODOT 3'S, BECAUSE THE ART IS GODOT 3'S. The pack ships a Godot project
# (assets/_src/ninja/GodotProject.zip) that is the very project its example GIFs were recorded from,
# and its tilesets declare `tile_mode = 1, autotile/bitmask_mode = 1` — BITMASK_3X3_MINIMAL, the
# 47-tile blob. Each set is an 11x5 block of which 47 cells carry a mask; the layout is not a grid
# anyone should read by eye, which is exactly why the coordinates are taken from the author's own
# `.tres` tables rather than eyeballed off a contact sheet.
#
# The bits are Godot's `TileSet::BIND_*`. A corner bit is set only when BOTH adjacent sides are
# also set — that "minimal" rule is what collapses 256 neighbourhoods to 47 tiles, and the renderer
# has to apply the same rule or it will ask for masks that were never drawn.
EDGE_TL, EDGE_T, EDGE_TR = 1, 2, 4
EDGE_L, EDGE_C, EDGE_R = 8, 16, 32
EDGE_BL, EDGE_B, EDGE_BR = 64, 128, 256

# The pack's Godot project, read straight out of the zip. `assets/_src/` is gitignored in its
# entirety — the pack is a download, not a checked-in asset — so this adds no new precondition
# beyond the PNGs the rest of this file already reads from the same directory.
GODOT_ZIP = SRC / "ninja/GodotProject.zip"
GODOT_TRES = "GodotProject/World/Backgrounds/Tileset/"

# Which 47-mask set each terrain's edge overlay comes from, in `Terrain` enum order. Index N here IS
# `static_cast<int>(Terrain)` N, so the renderer indexes without a lookup table.
#
#   ("pack", tres, tile_id, fallback_fill)  take the author's own art, alpha-cut (see edge_cut)
#   ("gen",  sheet, col, row)               generate the contour from that terrain's plain fill
#
# Only four terrains get pack art, and that is the honest result of looking rather than the result
# of settling. The pack draws boundaries for ITS world — water against grass, water against sand,
# dirt against grass, snow over anything — and those are four of the five boundaries that dominate
# an outdoor shot. It draws no sand-against-grass set at all: the author butts the two together with
# a hard staircase edge and scatters props over the join (visible in the rebuilt Village map). So
# the generated contour is not a placeholder waiting to be replaced; for seven terrains it is the
# only thing there is.
EDGE_MANIFEST = [
    ("Grass",    ("gen", "NFloor", 0, 12)),
    ("Dirt",     ("pack", "TilesetFloor.tres", 2, ("NFloor", 11, 19))),
    ("Water",    ("pack", "TilesetFloor.tres", 18, ("NWater", 11, 2))),
    ("Stone",    ("gen", "NIntFloor", 5, 13)),
    ("Sand",     ("gen", "NFloor", 0, 5)),
    # kTree draws the ground of its own ring and never uses its own set; it is here to keep the
    # index equal to the enum value.
    ("Tree",     ("gen", "NFloor", 0, 12)),
    ("Snow",     ("pack", "TilesetFloor.tres", 17, ("NFloor", 0, 19))),
    # NOT TilesetWater#27, which is the obvious candidate and is wrong. That set's interior is
    # poison water at (188, 132, 181); our Marsh is the wetland GROUND, a dark swamp green at
    # (116, 163, 52). Reaching for it because the name matched would have ringed every marsh in
    # purple. The pack has no marsh-ground edge set, so this one is generated.
    ("Marsh",    ("gen", "NFloor", 11, 12)),
    ("Ash",      ("gen", "NIntFloor", 16, 13)),
    ("Path",     ("gen", "NFloor", 12, 8)),
    ("Building", ("gen", "NFloor", 12, 8)),
]

# How far a GENERATED boundary is allowed to wander from the smooth contour, as a fraction of a
# tile. Zero gives clean arcs and straight half-tile lines — better than a tile-edge staircase but
# still visibly ruled. This is what turns it into a coastline.
TRANS_WOBBLE = 0.17

# Spread of the kernel that turns a 9-bit mask into a contour, in tiles. 0.55 was picked so that the
# isolated mask (centre only) closes into an island inside its own tile instead of bleeding to the
# border, and so that a straight edge sits on the tile boundary rather than inside it.
EDGE_SIGMA = 0.55


def godot_autotile(tres: str, tile_id: int) -> dict:
    """The author's own bitmask table for one autotile, read out of GodotProject.zip.

    Returns the source PNG's basename, the region origin in pixels, `flags` mapping each mask to
    the cells that carry it, and `prio` the weights among equal-mask cells.
    """
    import zipfile

    with zipfile.ZipFile(GODOT_ZIP) as zf:
        txt = zf.read(GODOT_TRES + tres).decode("utf-8")
    ext = {i: p for p, i in re.findall(r'\[ext_resource path="res://(.+?)".*?id=(\d+)\]', txt)}

    def field(key: str, default: str = "") -> str:
        m = re.search(rf"^{tile_id}/{key} = (.+)$", txt, re.M)
        return m.group(1) if m else default

    tex = re.search(r"\d+", field("texture", "0")).group(0)
    # Rect2's own "2" is a digit and would be read as a coordinate; strip the constructor first.
    rx, ry = [int(float(v)) for v in
              re.findall(r"-?[\d.]+", field("region", "0,0,0,0").replace("Rect2", ""))][:2]
    flags: dict[int, list[tuple[int, int]]] = {}
    for ax, ay, m in re.findall(r"Vector2\( (\d+), (\d+) \), (\d+)", field("autotile/bitmask_flags")):
        flags.setdefault(int(m), []).append((int(ax), int(ay)))
    prio = {(int(a), int(b)): int(c) for a, b, c in
            re.findall(r"Vector3\( (\d+), (\d+), (\d+) \)", field("autotile/priority_map"))}
    return {"png": Path(ext[tex]).name, "rx": rx, "ry": ry, "flags": flags, "prio": prio}


def _wrap_noise(seed: int, period: int) -> list[list[float]]:
    """A TILE-tall value-noise field that wraps at `period`, so it repeats seamlessly.

    Seamlessness is the whole point and it is not decorative. Adjacent tiles share two corners, so
    the interpolated field already agrees along their shared edge; if the wobble did not agree
    there too, every tile border would show a step — which is the artefact being removed. Wrapping
    at a divisor of TILE makes the field identical on both sides of every edge.
    """
    n = TILE // period
    lat = {}

    def at(i: int, j: int) -> float:
        key = (i % n, j % n)  # the wrap
        if key not in lat:
            h = (key[0] * 0x9E37_79B9 ^ key[1] * 0x85EB_CA6B ^ seed) & 0xFFFF_FFFF
            h ^= h >> 15
            h = (h * 0xC2B2_AE35) & 0xFFFF_FFFF
            h ^= h >> 13
            lat[key] = (h & 0xFFFF) / 65535.0
        return lat[key]

    out = []
    for y in range(TILE):
        row = []
        for x in range(TILE):
            fx, fy = x / period, y / period
            ix, iy = int(fx), int(fy)
            tx, ty = fx - ix, fy - iy
            sx = tx * tx * (3.0 - 2.0 * tx)
            sy = ty * ty * (3.0 - 2.0 * ty)
            a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * sx
            b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * sx
            row.append(a + (b - a) * sy)
        out.append(row)
    return out


def minimal_mask(bits: dict[tuple[int, int], bool]) -> int:
    """Godot's BITMASK_3X3_MINIMAL rule, in one place so the packer and the renderer agree.

    `bits[(dx, dy)]` is whether the neighbour at that offset counts as this terrain. A corner bit is
    set only when BOTH adjacent sides are also set; that is what collapses 256 neighbourhoods onto
    47 drawn tiles, and it is the rule the pack's own art was drawn against.
    """
    t, b = bits[(0, -1)], bits[(0, 1)]
    left, right = bits[(-1, 0)], bits[(1, 0)]
    m = EDGE_C
    if t:
        m |= EDGE_T
    if b:
        m |= EDGE_B
    if left:
        m |= EDGE_L
    if right:
        m |= EDGE_R
    if t and left and bits[(-1, -1)]:
        m |= EDGE_TL
    if t and right and bits[(1, -1)]:
        m |= EDGE_TR
    if b and left and bits[(-1, 1)]:
        m |= EDGE_BL
    if b and right and bits[(1, 1)]:
        m |= EDGE_BR
    return m


# The 47 masks the minimal rule can actually produce, in a fixed order. DERIVED by enumerating all
# 256 neighbourhoods rather than transcribed, because a hand-written list of 47 nine-bit constants
# is a transcription error waiting to happen — and the count is checked against the pack's own
# tables at build time, so if this and the art ever disagreed the build would say so.
def _edge_masks() -> list[int]:
    seen = set()
    for n in range(256):
        bits = {}
        i = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if (dx, dy) == (0, 0):
                    bits[(dx, dy)] = True
                else:
                    bits[(dx, dy)] = bool(n & (1 << i))
                    i += 1
        seen.add(minimal_mask(bits))
    return sorted(seen)


EDGE_MASKS = _edge_masks()


def edge_generated(fill: Image.Image, mask: int, jitter: tuple) -> Image.Image:
    """`fill`, cut to the region a nine-bit `mask` says this terrain wins.

    The field is a Gaussian-weighted vote of the nine cells, thresholded at half the total weight.
    That gives the two properties the shape has to have: a fully-surrounded mask is 1 everywhere so
    the plain fill stays consistent, and a straight edge lands on the tile boundary so two tiles
    either side of it meet without a step.

    THE WOBBLE DISPLACES THE SAMPLE POINT, NOT THE FIELD VALUE, and the first version did the
    latter. Adding noise to the field looks equivalent and is not: how far a given amount of field
    moves the contour depends on the local gradient, and the gradient across a straight edge is the
    steepest anywhere in the tile. So exactly where the wobble was needed most it did the least, and
    a long straight run of one mask — the edge of a road, the side of a village square — came out as
    a ruled line. Displacing the coordinate moves the contour by a fixed number of pixels wherever
    it falls.

    Threshold, never blend: a soft alpha ramp would put half-transparent pixels along every
    coastline, and this is pixel art at a 2x zoom where that reads as blur, not as a gradient.
    """
    bits = {
        (-1, -1): mask & EDGE_TL, (0, -1): mask & EDGE_T, (1, -1): mask & EDGE_TR,
        (-1, 0): mask & EDGE_L, (0, 0): mask & EDGE_C, (1, 0): mask & EDGE_R,
        (-1, 1): mask & EDGE_BL, (0, 1): mask & EDGE_B, (1, 1): mask & EDGE_BR,
    }
    jx, jy = jitter
    two_sigma_sq = 2.0 * EDGE_SIGMA * EDGE_SIGMA
    out = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
    src, dst = fill.load(), out.load()
    for y in range(TILE):
        for x in range(TILE):
            u = (x + 0.5) / TILE - 0.5 + (jx[y][x] - 0.5) * 2.0 * TRANS_WOBBLE
            v = (y + 0.5) / TILE - 0.5 + (jy[y][x] - 0.5) * 2.0 * TRANS_WOBBLE
            on = total = 0.0
            for (dx, dy), b in bits.items():
                w = math.exp(-((u - dx) ** 2 + (v - dy) ** 2) / two_sigma_sq)
                total += w
                if b:
                    on += w
            if on / total > 0.5:
                dst[x, y] = src[x, y]
    return out


def godot_sheet(png: str) -> Image.Image:
    """The PNG an autotile points at, from the pack directory if it is there and the zip if not.

    `TilesetSnow.png` is only in the zip. It is a complete 47-mask snow overlay — the one set in the
    whole pack the author drew with a transparent outside — and it ships in the Godot project
    without ever appearing in `Backgrounds/Tilesets/`. Reading both places is how it gets used.
    """
    import io
    import zipfile

    local = SRC / "ninja/Backgrounds/Tilesets" / png
    if local.exists():
        return Image.open(local).convert("RGBA")
    interior = SRC / "ninja/Backgrounds/Tilesets/Interior" / png
    if interior.exists():
        return Image.open(interior).convert("RGBA")
    with zipfile.ZipFile(GODOT_ZIP) as zf:
        for name in zf.namelist():
            if name.endswith("/" + png):
                return Image.open(io.BytesIO(zf.read(name))).convert("RGBA")
    raise FileNotFoundError(f"{png} is in neither the pack nor {GODOT_ZIP.name}")


def edge_from_pack(auto: dict, sheet: Image.Image) -> dict[int, Image.Image]:
    """The pack's own 47 tiles for one autotile, alpha-cut so they overlay any base.

    The pack's sets are OPAQUE PAIRS: `Water id18` carries grass at its rim and water in its middle,
    baked together. That is unusable as an overlay — laid over sand it would ring the coast in
    grass. So the outside terrain is knocked out, leaving the inside terrain plus its bank and foam,
    which is exactly what should be drawn over whatever the neighbour happens to be.

    Which colours are "outside" is READ OFF THE ART, not configured. For every drawn tile, a border
    row or column whose corresponding side bit is clear lies wholly outside the blob, so its colours
    are outside colours. Accumulating those over all 47 tiles gives the palette to remove. Pixel art
    has flat palettes and the inside and outside of all four sets used here share no colour at all
    (checked: zero overlap), so an exact-colour knockout takes the outside and nothing else.

    Cells declared for a mask but drawn empty are dropped. That is not defensive coding: the pack's
    `Floor id8` declares cell (1,4) as fully-surrounded and (3,3) as isolated, and both are entirely
    transparent — Godot renders holes there too.
    """
    def cell(cx: int, cy: int) -> Image.Image:
        x, y = auto["rx"] + cx * TILE, auto["ry"] + cy * TILE
        return sheet.crop((x, y, x + TILE, y + TILE))

    outside: set[tuple[int, int, int]] = set()
    for mask, cells in auto["flags"].items():
        for cx, cy in cells:
            px = cell(cx, cy).convert("RGBA").load()
            spans = []
            if not mask & EDGE_T:
                spans += [(x, 0) for x in range(TILE)]
            if not mask & EDGE_B:
                spans += [(x, TILE - 1) for x in range(TILE)]
            if not mask & EDGE_L:
                spans += [(0, y) for y in range(TILE)]
            if not mask & EDGE_R:
                spans += [(TILE - 1, y) for y in range(TILE)]
            for x, y in spans:
                if px[x, y][3] > 127:
                    outside.add(px[x, y][:3])

    # The fully-surrounded tile is pure inside; never knock any of its colours out, even if some
    # border sampling happened to catch one.
    full = auto["flags"].get(EDGE_TL | EDGE_T | EDGE_TR | EDGE_L | EDGE_C | EDGE_R
                             | EDGE_BL | EDGE_B | EDGE_BR, [])
    for cx, cy in full:
        for p in cell(cx, cy).convert("RGBA").getdata():
            if p[3] > 127:
                outside.discard(p[:3])

    out: dict[int, Image.Image] = {}
    for mask, cells in auto["flags"].items():
        best = None
        for cx, cy in cells:
            img = cell(cx, cy).convert("RGBA")
            if sum(1 for p in img.getdata() if p[3] > 127) < TILE * TILE // 4:
                continue  # declared but not drawn
            weight = auto["prio"].get((cx, cy), 1)
            if best is None or weight > best[0]:
                best = (weight, img)
        if best is None:
            continue
        img = best[1]
        px = img.load()
        for y in range(TILE):
            for x in range(TILE):
                if px[x, y][3] > 127 and px[x, y][:3] in outside:
                    px[x, y] = (0, 0, 0, 0)
        out[mask] = img
    return out

# --- Animation sheets --------------------------------------------------------
# Ninja Adventure ships each actor as its own file rather than as tiles in a shared sheet, and its
# walk sheets are a 4x4 grid: COLUMN = facing (down, up, left, right), ROW = animation frame.
# (Verified by rendering the grid with labels, not assumed.) These are packed as blocks below the
# single-tile grid, and the generated header exposes `anim_frame(slot, dir, frame)`.
NINJA = SRC / "ninja/Actor"

# A 4x4 sheet is 4 FACINGS across x 4 frames down (down / up / left / right, matching `Facing`).
# A sheet with ONE row has no facings at all — its columns are frames — and the renderer detects
# that from `rows == 1` rather than from a flag here, so a two-frame chicken can be handed a
# four-way facing and simply bob instead of snapping between two sprites (see `Impl::anim`).
#
# EVERY ENTRY BELOW WAS LOOKED AT before it was written down (a 6x contact sheet of the candidate
# sheets, side by side, with a tile grid over them). That is the same discipline verify_structures.py
# enforces for multi-tile crops, and it is what ruled out three plausible-by-name choices: Hyena is
# 28x13 and does not fit the grid at all, Monster/Beast is a red demon rather than anything that
# reads as wildlife, and Animal/Racoon is a two-frame side view where Monster/Racoon is a full
# four-direction sheet of the same creature.
ANIM_MANIFEST = [
    # (name, path relative to NINJA, cols, rows)
    ("Player",     "Character/NinjaGreen/SeparateAnim/Walk.png", 4, 4),
    ("MobSlime",   "Monster/Slime/Slime.png",                    4, 4),
    ("MobSpider",  "Monster/SpiderRed/SpriteSheet.png",          4, 4),
    ("MobSpirit",  "Monster/Spirit/SpriteSheet.png",             4, 4),
    ("MobSkull",   "Monster/Skull/SpriteSheet.png",              4, 4),
    ("Boar",       "Animal/WildBoar/SpriteSheet.png",            2, 1),
    ("Wolf",       "Animal/DogBlack/SpriteSheet.png",            2, 1),
    ("Bear",       "Monster/Bear/SpriteSheet.png",               4, 4),
    ("Racoon",     "Monster/Racoon/SpriteSheet.png",             4, 4),
    ("Chicken",    "Animal/Chicken/SpriteSheetWhite.png",        2, 1),
    ("Cow",        "Animal/Cow/SpriteSheetWhite.png",            2, 1),
    ("Horse",      "Animal/Horse/SpriteSheetBrownSide.png",      2, 1),
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


# --- Prefab parcels ----------------------------------------------------------
# Phase 0's tools/import_prefabs.py cut hand-composed set-pieces out of the pack author's own
# Village.tscn into assets/_gen/prefabs/*.json (schema: size, layers[Floor/FloorDetail/House/
# Element], each cell an x/y parcel-tile anchor plus an sx/sy/w/h SOURCE-PIXEL rect into `sheet`).
# This pass packs every distinct crop those cells reference into the atlas and emits
# src/world/prefabs.hpp so the engine can stamp a parcel with zero flip flags and one texture bind.
#
# FLIPS ARE BAKED AT PACK TIME. A cell may set flip_h/flip_v/transpose; rather than carry those to
# the renderer, the crop is transformed here and packed as a plain sprite, so a flipped variant is
# simply a second atlas entry. The transform order matches import_prefabs.blit (transpose, then
# flip_h, then flip_v) so a packed sprite is pixel-identical to that tool's preview.
#
# 16x16 crops go through the same grid+extrusion path as MANIFEST tiles: they tile the ground and
# need the 1px extruded border so fractional-zoom sampling never bleeds a neighbour. Larger crops
# (houses, tents, trees) pack like BIG/FX entries with no extrusion -- they are drawn at a fixed
# size and have no seam to smear.
PREFAB_DIR = ROOT / "assets" / "_gen" / "prefabs"

# PrefabId is DERIVED, not hand-listed: every <stem>.json in PREFAB_DIR becomes an enum member, sorted
# alphabetically by stem so the C++ order is a pure function of what import_prefabs.py cut. Adding a
# parcel is one importer edit plus a re-run -- no second list to keep in step here.


def _prefab_camel(stem: str) -> str:
    """`camp_clearing` -> `CampClearing`; the enum member is `k` + this."""
    return "".join(part.capitalize() for part in stem.split("_"))


# Godot layer name -> prefabs.hpp layer index. 0=floor overlay, 1=floor detail, 2=structure
# (blocks, y-sorts with actors), 3=prop (does not block). The pack's `Snow` layer (snow patches laid
# over the ground in the frozen-pond parcel) is a floor-level overlay drawn between Floor and
# FloorDetail, so it maps to 1: like every floor/detail cell it is always stamped (group 0), never an
# optional cluster.
PREFAB_LAYER = {"Floor": 0, "Snow": 1, "FloorDetail": 1, "House": 2, "Element": 3}

# Whether a parcel may be stamped mirrored (flip_h) for variety. Default True; a parcel is False only
# when it bakes in readable glyphs, because a mirrored letter reads instantly as wrong. Only
# street_houses qualifies -- its DOJO building has the word "DOJO" painted on the sign. The other
# parcels were checked against their _gen/prefabs/*.png previews: the fort's wall pieces are plain
# planks and log caps (no glyphs), the cottage/market/camp signage is pictorial, so all stay True.
MIRRORABLE = {
    "street_houses": False,
}


def _prefab_groups(cells: list) -> int:
    """Assign each cell its `group` and return the parcel's optional-group count.

    group 0 is always stamped. Floor/FloorDetail cells (layer 0/1) are always group 0. House/Element
    cells (layer 2/3) are split into connected components -- two connect when the Chebyshev distance
    between their anchor tiles is <= 2 -- and each component becomes an optional group numbered 1..k
    by its topmost-then-leftmost member. The one exception keeps a parcel's centerpiece from ever
    vanishing: if EXACTLY ONE component holds a House-layer sprite >= 48px wide, that component stays
    group 0. With zero or several such big-house components there is no single centerpiece, so every
    component is optional.
    """
    for c in cells:
        c["group"] = 0
    opt = [i for i, c in enumerate(cells) if c["layer"] in (2, 3)]

    parent = {i: i for i in opt}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for ai, a in enumerate(opt):
        for b in opt[ai + 1:]:
            if (abs(cells[a]["dx"] - cells[b]["dx"]) <= 2
                    and abs(cells[a]["dy"] - cells[b]["dy"]) <= 2):
                parent[find(b)] = find(a)

    comps: dict[int, list] = {}
    for i in opt:
        comps.setdefault(find(i), []).append(i)
    comp_list = list(comps.values())

    def is_big(members):
        return any(cells[i]["layer"] == 2 and cells[i]["pw"] >= 48 for i in members)

    big = [m for m in comp_list if is_big(m)]
    forced0 = big[0] if len(big) == 1 else None

    def key(members):
        return min((cells[i]["dy"], cells[i]["dx"]) for i in members)

    optional = sorted((m for m in comp_list if m is not forced0), key=key)
    for gnum, members in enumerate(optional, start=1):
        for i in members:
            cells[i]["group"] = gnum
    return len(optional)

_PREFAB_SHEETS: dict[str, Image.Image] = {}


def prefab_sheet(sheet_id: str) -> Image.Image:
    """The texture a prefab cell's `sheet` string names.

    import_prefabs.py writes either a GodotProject.zip member path (`GodotProject/World/...`) or a
    pack-relative fallback (`Backgrounds/Tilesets/<name>`); read the zip member if present, else the
    loose file under assets/_src/ninja/. Cached, since a handful of sheets back hundreds of cells.
    """
    if sheet_id in _PREFAB_SHEETS:
        return _PREFAB_SHEETS[sheet_id]
    with zipfile.ZipFile(GODOT_ZIP) as zf:
        if sheet_id in set(zf.namelist()):
            img = Image.open(io.BytesIO(zf.read(sheet_id))).convert("RGBA")
        else:
            loose = SRC / "ninja" / sheet_id
            if not loose.exists():
                raise FileNotFoundError(f"prefab sheet {sheet_id} is in neither {GODOT_ZIP.name} "
                                        f"nor {loose}")
            img = Image.open(loose).convert("RGBA")
    _PREFAB_SHEETS[sheet_id] = img
    return img


def prefab_crop(key: tuple) -> Image.Image:
    """One crop key -> its transformed RGBA sprite. Transform order matches import_prefabs.blit."""
    sheet, sx, sy, w, h, fh, fv, tr = key
    img = prefab_sheet(sheet).crop((sx, sy, sx + w, sy + h))
    if tr:
        img = img.transpose(Image.TRANSPOSE)
    if fh:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
    if fv:
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    return img


def _cell_key(c: dict) -> tuple:
    return (c["sheet"], c["sx"], c["sy"], c["w"], c["h"],
            c["flip_h"], c["flip_v"], c["transpose"])


def pack_prefabs(atlas: Image.Image, anim_y: int, cell_px: int):
    """Pack every distinct prefab crop into `atlas`; return (atlas, anim_y, prefabs).

    `prefabs` is a list of dicts (stem, cpp, name, w, h, cells, blocks) in PREFAB_ORDER, or None if
    there is nothing to pack -- a missing _gen/prefabs/ makes the whole pass a no-op so clean
    checkouts still build.
    """
    if not PREFAB_DIR.exists():
        print("  no assets/_gen/prefabs/ -- skipping prefab pass "
              "(run tools/import_prefabs.py to populate it)")
        return atlas, anim_y, None
    docs = []
    for path in sorted(PREFAB_DIR.glob("*.json")):
        stem = path.stem
        docs.append((stem, _prefab_camel(stem), json.loads(path.read_text())))
    if not docs:
        print("  assets/_gen/prefabs/ holds no prefab JSONs -- skipping prefab pass")
        return atlas, anim_y, None

    # Collect distinct crops in first-seen order, split into the grid (16x16, extruded) and big
    # (everything larger, unextruded) pools. Deterministic order = reproducible atlas.
    grid_order: list[tuple] = []
    big_order: list[tuple] = []
    seen: set[tuple] = set()
    for _stem, _cpp, doc in docs:
        for layer in doc["layers"]:
            for c in layer["cells"]:
                key = _cell_key(c)
                if key in seen:
                    continue
                seen.add(key)
                (grid_order if c["w"] == TILE and c["h"] == TILE else big_order).append(key)

    pos: dict[tuple, tuple[int, int]] = {}

    # Grid pool: COLS columns of extruded cells, stacked below whatever came before.
    grid_rows = (len(grid_order) + COLS - 1) // COLS
    need_w, need_h = COLS * cell_px, anim_y + grid_rows * cell_px
    if need_w > atlas.width or need_h > atlas.height:
        grown = Image.new("RGBA", (max(need_w, atlas.width), max(need_h, atlas.height)),
                          (0, 0, 0, 0))
        grown.paste(atlas, (0, 0))
        atlas = grown
    for i, key in enumerate(grid_order):
        tile = prefab_crop(key)
        cx, cy = (i % COLS) * cell_px, anim_y + (i // COLS) * cell_px
        cell = Image.new("RGBA", (cell_px, cell_px), (0, 0, 0, 0))
        extrude(cell, tile)
        atlas.paste(cell, (cx, cy))
        pos[key] = (cx + PAD, cy + PAD)
    anim_y += grid_rows * cell_px

    # Big pool: each crop on its own row with a 1px transparent border, like BIG_MANIFEST.
    for key in big_order:
        _s, _sx, _sy, w, h, _fh, _fv, _tr = key
        region = prefab_crop(key)
        block = Image.new("RGBA", (w + 2 * PAD, h + 2 * PAD), (0, 0, 0, 0))
        block.paste(region, (PAD, PAD))
        new_h = anim_y + h + 2 * PAD
        if new_h > atlas.height or w + 2 * PAD > atlas.width:
            grown = Image.new("RGBA", (max(w + 2 * PAD, atlas.width), max(new_h, atlas.height)),
                              (0, 0, 0, 0))
            grown.paste(atlas, (0, 0))
            atlas = grown
        atlas.paste(block, (0, anim_y))
        pos[key] = (PAD, anim_y + PAD)
        anim_y += h + 2 * PAD

    # Resolve each prefab's cells against the packed positions and compute its block mask.
    prefabs = []
    for stem, cpp, doc in docs:
        w, h = doc["size"]
        if w > 32:  # block_rows is one u32 per row; a wider parcel would silently lose columns
            print(f"  prefab {stem}: width {w} > 32, does not fit block_rows u32", file=sys.stderr)
            return atlas, anim_y, None
        blocks = [0] * h
        cells = []
        for layer in doc["layers"]:
            li = PREFAB_LAYER[layer["name"]]
            for c in layer["cells"]:
                ax, ay = pos[_cell_key(c)]
                cells.append({
                    "dx": c["x"], "dy": c["y"], "layer": li,
                    "ax": ax, "ay": ay, "pw": c["w"], "ph": c["h"],
                    "centred": c["origin"] == 1,
                })
                if li == 2:  # a House sprite blocks its full footprint in tiles, clipped to rect
                    tw = (c["w"] + TILE - 1) // TILE
                    th = (c["h"] + TILE - 1) // TILE
                    for ry in range(c["y"], min(c["y"] + th, h)):
                        for rx in range(c["x"], min(c["x"] + tw, w)):
                            blocks[ry] |= 1 << rx
        group_count = _prefab_groups(cells)
        cells.sort(key=lambda d: (d["layer"], d["dy"], d["dx"]))
        prefabs.append({"stem": stem, "cpp": cpp, "name": doc.get("name", stem),
                        "w": w, "h": h, "cells": cells, "blocks": blocks,
                        "group_count": group_count, "mirrorable": MIRRORABLE.get(stem, True)})
        print(f"  prefab {stem:<20} {w}x{h}  cells={len(cells)}  groups={group_count}  "
              f"mirror={'yes' if MIRRORABLE.get(stem, True) else 'NO'}")
    print(f"  packed {len(grid_order)} grid + {len(big_order)} big prefab crops")
    return atlas, anim_y, prefabs


def write_prefabs_header(prefabs) -> None:
    """Emit src/world/prefabs.hpp from the packed prefab data.

    PrefabId, the cell tables and the kPrefabs table are all DERIVED from whatever parcels
    import_prefabs.py cut -- the enum members are the JSON stems, CamelCased and sorted.
    """
    ids = ", ".join(f"k{p['cpp']}" for p in prefabs)
    lines = [
        "// GENERATED by tools/build_atlas.py -- do not edit by hand.",
        "//",
        "// Hand-composed parcels lifted from the pack author's own Village.tscn (see",
        "// tools/import_prefabs.py) with every referenced crop packed into assets/atlas.png. Flips",
        "// are baked at pack time, so a cell carries only its atlas rect -- no flip flags.",
        "//",
        "// PrefabId is generated from the discovered parcel files, alphabetically by stem, so the",
        "// enum is a pure function of _gen/prefabs/*.json.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mmo {",
        "",
        f"enum class PrefabId : std::uint8_t {{ {ids}, kCount }};",
        "",
        "// One drawn sprite inside a prefab. Layers: 0=floor overlay, 1=floor detail, 2=structure",
        "// (blocks, y-sorts with actors), 3=prop (does not block, y-sorts with actors).",
        "//",
        "// `group` selects which cells a stamp draws. Group 0 is always drawn: it is the whole floor",
        "// (layers 0/1) plus, when a parcel has a single big-house centerpiece, that centerpiece.",
        "// Groups 1..group_count are optional clusters (connected House/Element props) a caller may",
        "// keep or drop independently, so one parcel yields many arrangements.",
        "struct PrefabCell {",
        "    std::uint8_t dx, dy;        // anchor tile inside the prefab, 0..w-1 / 0..h-1",
        "    std::uint8_t layer;         // 0..3 as above",
        "    std::uint8_t group;         // 0 = always stamped; 1..group_count = optional cluster",
        "    std::int16_t ax, ay;        // pixel rect in atlas.png",
        "    std::uint8_t pw, ph;        // pixel size (16x16 for floor tiles; up to 64x48 for houses)",
        "    bool centred;               // true: draw centred on the anchor cell (Godot origin=1)",
        "};",
        "",
        "struct PrefabDef {",
        "    const char* name;",
        "    std::uint8_t w, h;                  // footprint in tiles",
        "    const PrefabCell* cells;            // sorted: layer asc, then dy, then dx",
        "    std::uint16_t cell_count;",
        "    const std::uint32_t* block_rows;    // h entries; bit x set = tile (x,row) blocked (w <= 32)",
        "    std::uint8_t group_count;           // number of optional cell groups (1..group_count)",
        "    bool mirrorable;                    // safe to stamp flipped (no readable glyphs baked in)",
        "};",
        "",
    ]
    for p in prefabs:
        lines.append(f"inline constexpr PrefabCell kPrefabCells_{p['cpp']}[] = {{")
        for c in p["cells"]:
            lines.append(f"    {{{c['dx']}, {c['dy']}, {c['layer']}, {c['group']}, "
                         f"{c['ax']}, {c['ay']}, {c['pw']}, {c['ph']}, "
                         f"{str(c['centred']).lower()}}},")
        lines.append("};")
        blk = ", ".join(f"0x{b:08x}u" for b in p["blocks"])
        lines.append(f"inline constexpr std::uint32_t kPrefabBlocks_{p['cpp']}[] = {{{blk}}};")
        lines.append("")
    lines.append("inline constexpr PrefabDef kPrefabs[static_cast<int>(PrefabId::kCount)] = {")
    for p in prefabs:
        lines.append(f"    {{\"{p['name']}\", {p['w']}, {p['h']}, kPrefabCells_{p['cpp']}, "
                     f"{len(p['cells'])}, kPrefabBlocks_{p['cpp']}, "
                     f"{p['group_count']}, {str(p['mirrorable']).lower()}}},")
    lines += [
        "};",
        "",
        "}  // namespace mmo",
        "",
    ]
    (ROOT / "src" / "world" / "prefabs.hpp").write_text("\n".join(lines))
    print(f"{ROOT / 'src' / 'world' / 'prefabs.hpp'}")


def prefab_proof(prefabs) -> None:
    """Re-render each prefab PURELY from the packed data, sampling assets/atlas.png at 4x.

    Compares against import_prefabs.py's own <name>.png: identical apart from parcel-edge cuts.
    """
    atlas = Image.open(ROOT / "assets" / "atlas.png").convert("RGBA")
    scale = 4
    for p in prefabs:
        img = Image.new("RGBA", (p["w"] * TILE, p["h"] * TILE), (0, 0, 0, 0))
        for c in p["cells"]:  # already sorted layer, dy, dx -> correct draw order
            sprite = atlas.crop((c["ax"], c["ay"], c["ax"] + c["pw"], c["ay"] + c["ph"]))
            px, py = c["dx"] * TILE, c["dy"] * TILE
            if c["centred"]:
                px += 8 - c["pw"] // 2
                py += 8 - c["ph"] // 2
            img.alpha_composite(sprite, (px, py))
        out = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
        dest = PREFAB_DIR / f"{p['stem']}_atlas.png"
        out.save(dest)
        print(f"  proof {p['stem']:<14} -> {dest.name}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack the game's sprites into one atlas.")
    ap.add_argument("--prefab-proof", action="store_true",
                    help="also re-render each prefab from the generated data for visual checking")
    args = ap.parse_args()

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
        if key_sheet is None:
            region = compose_room(sheets["NIntWall"][0])
        else:
            sheet, stride = sheets[key_sheet]
            region = sheet.crop((col * stride, row * stride,
                                 (col + wt) * stride, (row + ht) * stride))
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

    # --- terrain transition sets, generated ---------------------------------------------------
    # Two octaves of wrapping noise: the coarse one gives the boundary a shape, the fine one gives
    # it a ragged pixel edge. One octave alone was either too smooth to read as a coastline or too
    # busy to read as a boundary at all.
    coarse = _wrap_noise(0x51A3, 8)
    fine = _wrap_noise(0x2C77, 4)
    jx = [[0.62 * coarse[y][x] + 0.38 * fine[y][x] for x in range(TILE)] for y in range(TILE)]
    coarse_y = _wrap_noise(0x7B19, 8)
    fine_y = _wrap_noise(0x3D41, 4)
    jy = [[0.62 * coarse_y[y][x] + 0.38 * fine_y[y][x] for x in range(TILE)] for y in range(TILE)]
    jitter = (jx, jy)

    trans = []
    edge_source = {}
    for name, spec in EDGE_MANIFEST:
        if spec[0] == "pack":
            _, tres, tile_id, fallback = spec
            auto = godot_autotile(tres, tile_id)
            if len(auto["flags"]) != len(EDGE_MASKS):
                print(f"Edge{name}: {tres}#{tile_id} declares {len(auto['flags'])} masks, "
                      f"expected {len(EDGE_MASKS)}", file=sys.stderr)
                return 1
            tiles = edge_from_pack(auto, godot_sheet(auto["png"]))
            edge_source[name] = f"{auto['png']}#{tile_id}"
            key_sheet, col, row = fallback
        else:
            _, key_sheet, col, row = spec
            tiles = {}
            edge_source[name] = "generated"

        sheet, stride = sheets[key_sheet]
        sx, sy = col * stride, row * stride
        if sx + TILE > sheet.width or sy + TILE > sheet.height:
            print(f"Edge{name}: ({col},{row}) is outside {key_sheet}", file=sys.stderr)
            return 1
        fill = sheet.crop((sx, sy, sx + TILE, sy + TILE))

        strip_w = len(EDGE_MASKS) * cell_px
        new_h = anim_y + cell_px
        if new_h > atlas.height or strip_w > atlas.width:
            grown = Image.new("RGBA", (max(strip_w, atlas.width), max(new_h, atlas.height)),
                              (0, 0, 0, 0))
            grown.paste(atlas, (0, 0))
            atlas = grown
        for i, mask in enumerate(EDGE_MASKS):
            cell = Image.new("RGBA", (cell_px, cell_px), (0, 0, 0, 0))
            # A mask the pack declared but drew empty falls back to the generated contour, so every
            # row is complete however patchy its source was.
            art = tiles.get(mask) or edge_generated(fill, mask, jitter)
            # No extrusion. Extrusion smears a tile's border colour outward so that sampling at a
            # fractional texture coordinate cannot pick up the neighbour — but these tiles are
            # deliberately transparent right up to their edge, and smearing that transparency
            # outward is a no-op while smearing the OPAQUE side outward would thicken the coastline
            # by a pixel wherever it touches a tile border. The 1px gap between cells is enough.
            cell.paste(art, (PAD, PAD))
            atlas.paste(cell, (i * cell_px, anim_y))
        trans.append((name, PAD, anim_y + PAD))
        anim_y += cell_px

    # Is each terrain's variant 0 actually PLAIN? MEASURED off the pixels, not asserted. Comparing
    # the variant-0 and variant-1 coordinates was the first attempt and it answers a different
    # question — they always differ, including for the two terrains that have no plain fill at all.
    by_name = {n: (s, c, r) for n, s, c, r in MANIFEST}
    has_plain = {}
    for name, _ in EDGE_MANIFEST:
        entry = by_name.get(f"Terrain{name}")
        if entry is None:
            # kTree has no fill of its own — it draws its ring's ground, so it inherits grass.
            has_plain[name] = has_plain.get("Grass", True)
            continue
        sheet, stride = sheets[entry[0]]
        crop = sheet.crop((entry[1] * stride, entry[2] * stride,
                           entry[1] * stride + TILE, entry[2] * stride + TILE))
        px = list(crop.convert("RGB").getdata())
        n = len(px)
        mean = [sum(p[i] for p in px) / n for i in range(3)]
        sd = (sum(sum((p[i] - mean[i]) ** 2 for i in range(3)) for p in px) / n) ** 0.5
        # The measured spread is three-way, not two-way, which is why the threshold is 15 and not
        # the 6 first tried: the five real plain fills score 0.0, the road's earth grain scores 8.9,
        # and the two masonry fills score 22-23. The road tile comes from the centre of an autotile
        # set and therefore repeats seamlessly, so it belongs with the plain group. Printed below so
        # the boundary can be checked rather than trusted.
        has_plain[name] = sd < 15.0
        print(f"  fill {name:9s} variant0 stddev {sd:5.1f}  -> "
              f"{'plain' if has_plain[name] else 'WHOLE-TILE MOTIF, will be mirrored'}")

    # --- prefab parcels, stacked below the transition sets -----------------------------------
    atlas, anim_y, prefabs = pack_prefabs(atlas, anim_y, cell_px)

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
        "// --- Terrain edge sets ---------------------------------------------------------------",
        "// One EDGE set per terrain (see EDGE_MANIFEST in the packer). Where two terrains meet, the",
        "// tile is filled with the lower-priority one and the higher-priority terrain's edge tile is",
        "// laid over it — so this is 11 sets rather than the 55 a per-pair scheme would need, and it",
        "// covers pairs the pack draws no art for.",
        "//",
        "// The mask is Godot 3's BITMASK_3X3_MINIMAL, because four of these sets ARE the pack's own",
        "// autotiles and that is what they were drawn against. A corner bit counts only when both",
        "// adjacent sides are set; `edge_mask` below is the only place that rule is written, and it",
        "// mirrors `minimal_mask` in the packer.",
        f"inline constexpr int kTransMasks = {len(EDGE_MASKS)};",
        f"inline constexpr int kTransTerrains = {len(EDGE_MANIFEST)};",
        "",
        "inline constexpr int kEdgeTL = 1, kEdgeT = 2, kEdgeTR = 4;",
        "inline constexpr int kEdgeL = 8, kEdgeC = 16, kEdgeR = 32;",
        "inline constexpr int kEdgeBL = 64, kEdgeB = 128, kEdgeBR = 256;",
        "",
        "// Every mask the minimal rule can produce is drawn, so the plain fill is the only case a",
        "// caller has to branch on.",
        f"inline constexpr int kEdgeFull = {EDGE_MASKS[-1]};",
        "",
        "inline constexpr AtlasRect kAtlasTrans[kTransTerrains] = {",
    ]
    lines += [f"    {{{x}, {y}}},  // {name} — {edge_source[name]}" for name, x, y in trans]
    lines += [
        "};",
        "",
        "// Whether this terrain's variant 0 is a genuinely PLAIN fill, derived from the manifest by",
        "// comparing it against variant 1 rather than restated by hand. Two terrains are false:",
        "// Ninja Adventure ships no flat ash and no bare rock, so stone and ash use a whole-tile",
        "// masonry motif for all three variants. The renderer needs to know, because an unmirrored",
        "// whole-tile motif tiles into a perfect brick lattice — see `ground` in raylib_bridge.cpp.",
        "inline constexpr bool kTerrainHasPlain[kTransTerrains] = {",
    ]
    lines += [f"    {str(has_plain[name]).lower()},  // {name}" for name, _, _ in trans]
    lines += [
        "};",
        "",
        "// Column of each mask in a row. Only 47 of the 512 nine-bit values are reachable through",
        "// the minimal rule; the rest are -1 and a caller that lands on one has built its mask by",
        "// some other rule than `edge_mask`.",
        "inline constexpr std::int8_t kEdgeSlot[512] = {",
    ]
    slot_of_mask = {m: i for i, m in enumerate(EDGE_MASKS)}
    for base in range(0, 512, 16):
        lines.append("    " + " ".join(
            f"{slot_of_mask.get(base + k, -1)}," for k in range(16)))
    lines += [
        "};",
        "",
        "// Godot's BITMASK_3X3_MINIMAL. `n` answers \"does the neighbour at (dx, dy) count as this",
        "// terrain\"; a corner counts only when both of its adjacent sides do.",
        "template <typename Neighbour>",
        "[[nodiscard]] inline constexpr int edge_mask(Neighbour n) noexcept {",
        "    const bool t = n(0, -1), b = n(0, 1), l = n(-1, 0), r = n(1, 0);",
        "    int m = kEdgeC;",
        "    if (t) m |= kEdgeT;",
        "    if (b) m |= kEdgeB;",
        "    if (l) m |= kEdgeL;",
        "    if (r) m |= kEdgeR;",
        "    if (t && l && n(-1, -1)) m |= kEdgeTL;",
        "    if (t && r && n(1, -1)) m |= kEdgeTR;",
        "    if (b && l && n(-1, 1)) m |= kEdgeBL;",
        "    if (b && r && n(1, 1)) m |= kEdgeBR;",
        "    return m;",
        "}",
        "",
        "// `mask` must be one `edge_mask` can return; kEdgeFull is the plain fill and has a tile too.",
        "[[nodiscard]] inline constexpr AtlasRect trans_rect(int terrain, int mask) noexcept {",
        "    const AtlasRect& row = kAtlasTrans[terrain];",
        "    const int slot = kEdgeSlot[mask & 0x1FF];",
        "    return AtlasRect{static_cast<std::int16_t>(row.x + slot * (kAtlasTile + 2)), row.y};",
        "}",
        "",
        "}  // namespace mmo",
        "",
    ]
    header.write_text("\n".join(lines))

    print(f"{out_png}  {atlas.width}x{atlas.height}  "
          f"({len(entries)} tiles, {len(anims)} anims, {len(bigs)} big, {len(fxs)} fx)")
    print(f"{header}")

    if prefabs:
        write_prefabs_header(prefabs)
        if args.prefab_proof:
            prefab_proof(prefabs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
