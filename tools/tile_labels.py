#!/usr/bin/env python3
"""Human-authored region labels for the Ninja Adventure tilesets.

Every rule here was written after LOOKING at a labelled contact sheet of the sheet in question
(tools/contact_sheet.py --all, then tools/atlas_preview.py to magnify anything ambiguous). Numbers
alone cannot tell a grass tuft from a tree canopy; numbers alone also cannot tell you that the
trees in this pack are 2 or 4 tiles wide, which is exactly the mistake this catalogue exists to
stop. So: geometry and colour come from tools/tile_metrics.py, meaning comes from here.

A rule covers an inclusive (col0..col1, row0..row1) rectangle of one sheet and carries:

    kind      how to name and role the tiles in it
              'fill'   tileable ground fill        -> fill_plain / fill_textured (numeric split)
              'edge'   autotile / transition set   -> transition_edge, with a compass suffix
                                                      taken from where the minority colour sits;
                                                      cells that turn out to be flat or lightly
                                                      speckled are re-classified as fills
              'object' multi-tile sprite grid      -> object_part (or object_whole if 1x1),
                                                      named by its slot inside the ow x oh footprint
              'decor'  small self-contained props  -> decor
              'prop'   mixed prop area             -> role decided by geometry
    terrain   slug used to name fill/edge tiles (e.g. 'grass', 'sand', 'water')
    name      slug used to name object/decor/prop tiles
    desc      one short sentence describing what the region depicts

Later rules win, so a broad sheet-wide rule can be listed first and carved up afterwards.
"""

# --------------------------------------------------------------------------------------------
# TilesetFloor.png (22x26) — the main outdoor ground sheet.
#
# Layout, read off the contact sheet: two 10-column terrain blocks (cols 0-9 and cols 11-20) with
# an empty spacer column between and after them. Vertically, three 7-row groups plus a 5-row
# water/lava group at the bottom. Inside one 7-row group:
#     rows +0..+4  a blob autotile: the terrain as an island with a rimmed edge
#     row  +5      col+0 is the FLAT fill, col+1..col+4 are the TEXTURED fills  <- what we want
#     row  +6      the terrain blended with grass/scrub
# Row +4 col+0 and col+1 are always a twig/crack and a pebble decoration instead of edge pieces.
# --------------------------------------------------------------------------------------------
FLOOR_GROUPS = [
    # (row0, block-A terrain for the autotile, A fill terrain, B autotile terrain, B fill terrain)
    (0,  "sand",       "sand",        "sand_rose",   "sand_rose"),
    (7,  "dirt_red",   "grass_lime",  "dirt_brown",  "grass_green"),
    (14, "snow",       "snow",        "earth_taupe", "earth_taupe"),
]
FLOOR_TERRAIN_DESC = {
    "sand":        "warm orange desert sand",
    "sand_rose":   "pale rose-pink sand",
    "dirt_red":    "red-brown packed dirt bordered by grass",
    "dirt_brown":  "dark brown packed dirt bordered by grass",
    "grass_lime":  "bright lime-green grass",
    "grass_green":  "deep green grass",
    "snow":        "clean white snow",
    "earth_taupe": "dry taupe earth",
    "water_ice":   "pale blue meltwater ringed by snow",
    "lava":        "orange lava ringed by pale rock",
}


def _floor_rules():
    out = []
    for row0, a_edge, a_fill, b_edge, b_fill in FLOOR_GROUPS:
        for col0, edge_t, fill_t in ((0, a_edge, a_fill), (11, b_edge, b_fill)):
            out += [
                dict(cols=(col0, col0 + 10), rows=(row0, row0 + 4), kind="edge", terrain=edge_t,
                     desc=f"edge of a {FLOOR_TERRAIN_DESC[edge_t]} area"),
                dict(cols=(col0, col0), rows=(row0 + 4, row0 + 4), kind="decor",
                     name=f"{edge_t}_twig", desc="small fallen twig / surface crack detail"),
                dict(cols=(col0 + 1, col0 + 1), rows=(row0 + 4, row0 + 4), kind="decor",
                     name=f"{edge_t}_pebble", desc="single pale pebble lying on the ground"),
                # cols +2/+3 of the decoration row carry the group's own ground fill, not an edge
                dict(cols=(col0 + 2, col0 + 3), rows=(row0 + 4, row0 + 4), kind="fill",
                     terrain=fill_t,
                     desc=f"{FLOOR_TERRAIN_DESC[fill_t]} ground fill"),
                dict(cols=(col0, col0 + 4), rows=(row0 + 5, row0 + 5), kind="fill", terrain=fill_t,
                     desc=f"{FLOOR_TERRAIN_DESC[fill_t]} ground fill"),
                dict(cols=(col0, col0 + 5), rows=(row0 + 6, row0 + 6), kind="edge",
                     terrain=f"{fill_t}_scrub",
                     desc=f"{FLOOR_TERRAIN_DESC[fill_t]} blended with a patch of leafy scrub"),
            ]
    for col0, t in ((0, "water_ice"), (11, "lava")):
        out.append(dict(cols=(col0, col0 + 10), rows=(21, 25), kind="edge", terrain=t,
                        desc=f"shoreline of {FLOOR_TERRAIN_DESC[t]}"))
    return out


RULES = {
    "TilesetFloor.png": _floor_rules(),

    # ------------------------------------------------------------------------------------
    # TilesetWater.png (28x17) — water bodies and their shorelines, plus a wooden pier.
    # Block A cols 0-9 / block B cols 13-23, five rows per shoreline set. Col 11 (and col 24
    # for block B) holds the open-water fill variants; cols 24-27 hold floating props.
    # ------------------------------------------------------------------------------------
    "TilesetWater.png": [
        dict(cols=(0, 10), rows=(0, 4), kind="edge", terrain="water_shore_sand",
             desc="water meeting a sandy shore, with a white foam rim"),
        dict(cols=(11, 11), rows=(0, 4), kind="fill", terrain="water",
             desc="open water surface"),
        dict(cols=(11, 11), rows=(1, 1), kind="decor", name="water_rocks",
             desc="two small rocks breaking the water surface"),
        dict(cols=(11, 11), rows=(3, 3), kind="decor", name="water_lilypad",
             desc="single green lily pad floating on open water"),
        dict(cols=(11, 11), rows=(4, 4), kind="decor", name="water_fish",
             desc="two small fish swimming just below the surface"),
        dict(cols=(12, 12), rows=(0, 0), kind="decor", name="water_boulder",
             desc="brown boulder standing out of the water"),
        dict(cols=(13, 23), rows=(0, 4), kind="edge", terrain="water_shore_snow",
             desc="pale ice-water meeting a snow bank, with a white foam rim"),
        dict(cols=(24, 24), rows=(0, 4), kind="fill", terrain="water_ice",
             desc="pale ice-water surface"),
        dict(cols=(24, 24), rows=(0, 0), kind="decor", name="ice_water_rock",
             desc="sandy rock island in ice-water"),
        dict(cols=(24, 24), rows=(3, 3), kind="decor", name="ice_water_lilypad",
             desc="green lily pad floating on ice-water"),
        dict(cols=(25, 27), rows=(0, 1), kind="prop", name="water_flotsam",
             desc="floating wooden debris, a buoy and a small boat"),
        dict(cols=(0, 0), rows=(5, 5), kind="fill", terrain="sand",
             desc="warm sand ground fill matching the sandy shoreline set"),
        dict(cols=(13, 13), rows=(5, 5), kind="fill", terrain="snow",
             desc="white snow ground fill matching the snow shoreline set"),
        dict(cols=(0, 10), rows=(6, 10), kind="edge", terrain="water_shore_grass",
             desc="water meeting a grassy bank, with a white foam rim"),
        dict(cols=(3, 3), rows=(9, 9), kind="decor", name="grass_sand_patch",
             desc="small round sand patch set into grass"),
        dict(cols=(13, 23), rows=(6, 10), kind="edge", terrain="swamp_water",
             desc="purple swamp water meeting a muddy bank"),
        dict(cols=(16, 16), rows=(9, 9), kind="decor", name="mud_patch",
             desc="small round mud patch set into the bank"),
        dict(cols=(0, 0), rows=(11, 11), kind="fill", terrain="grass_lime",
             desc="lime-green grass ground fill matching the grassy shoreline set"),
        dict(cols=(13, 13), rows=(11, 11), kind="fill", terrain="earth_tan",
             desc="tan muddy earth ground fill matching the swamp shoreline set"),
        dict(cols=(0, 10), rows=(12, 15), kind="object", name="pier_wood", ow=11, oh=4,
             collab=["end_w", "w", "w2", "mid", "mid2", "mid3", "mid4", "mid5", "e2", "e", "end_e"],
             rowlab=["top", "deck", "deck2", "edge"],
             desc="wooden pier / bridge deck of vertical planks"),
        dict(cols=(0, 8), rows=(16, 16), kind="prop", name="pier_wood_broken",
             desc="broken pier planking, a support beam and a hatch"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetDesert.png (20x12) — a desert village kit, NOT a ground sheet. The only ground
    # tiles are the flat sand block around cols 14-16 rows 8-9.
    # ------------------------------------------------------------------------------------
    "TilesetDesert.png": [
        dict(cols=(0, 5), rows=(0, 7), kind="prop", name="desert_dome",
             desc="green-domed adobe building: roof, walls, arched doorways and windows"),
        dict(cols=(6, 9), rows=(0, 9), kind="prop", name="desert_wall",
             desc="whitewashed brick wall and building facade section"),
        dict(cols=(10, 13), rows=(0, 7), kind="prop", name="desert_building",
             desc="sandstone building facade with battlements and windows"),
        dict(cols=(14, 19), rows=(0, 7), kind="prop", name="desert_rampart",
             desc="crenellated sandstone rampart and gate section"),
        dict(cols=(10, 13), rows=(4, 11), kind="object", name="palm_tree", ow=4, oh=3,
             collab=["far_left", "left", "right", "far_right"],
             rowlab=["fronds", "fronds_low", "trunk"],
             desc="palm tree, drawn 4 tiles wide and 3 tall"),
        dict(cols=(14, 16), rows=(8, 9), kind="fill", terrain="sand_pale",
             desc="pale beige desert sand ground fill"),
        dict(cols=(17, 19), rows=(7, 10), kind="edge", terrain="oasis_water",
             desc="bright blue oasis pool with a pale sand rim"),
        dict(cols=(0, 9), rows=(8, 11), kind="prop", name="desert_interior",
             desc="desert interior props: rugs, cloth awnings, tables and beds"),
        dict(cols=(14, 19), rows=(10, 11), kind="prop", name="desert_debris",
             desc="desert ground props: a treasure map, bones and a skull"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetRelief.png (20x12) — cliffs. Cols 0-3 are the flat plateau TOP surface, cols 4-11
    # the vertical cliff FACE seen from the front, col 7 a fall of water/sand down the face.
    # Rows 0-4 are the grey-green stone set, rows 5-9 the sandstone set.
    # ------------------------------------------------------------------------------------
    "TilesetRelief.png": [
        dict(cols=(0, 3), rows=(0, 4), kind="edge", terrain="plateau_snow",
             desc="snow-capped plateau top surface and its rim"),
        dict(cols=(4, 11), rows=(0, 4), kind="fill", terrain="cliff_stone",
             desc="vertical grey-green stone cliff face"),
        dict(cols=(7, 7), rows=(0, 4), kind="fill", terrain="waterfall",
             desc="pale blue waterfall running down a cliff face"),
        dict(cols=(0, 3), rows=(5, 9), kind="edge", terrain="plateau_sand",
             desc="sandstone plateau top surface and its rim"),
        dict(cols=(4, 11), rows=(5, 7), kind="fill", terrain="cliff_sandstone",
             desc="vertical orange sandstone cliff face"),
        dict(cols=(7, 7), rows=(5, 7), kind="fill", terrain="sandfall",
             desc="pale sand cascade running down a sandstone cliff face"),
        dict(cols=(0, 6), rows=(8, 9), kind="edge", terrain="plateau_sand_top",
             desc="pale sandstone plateau surface with small rock outcrops"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetReliefDetail.png (12x12) — things that hang off a cliff: cave mouths, ladders,
    # loose rocks, mine props. Column 3 is a ladder, cols 4-5 loose rock piles.
    # ------------------------------------------------------------------------------------
    "TilesetReliefDetail.png": [
        dict(cols=(0, 11), rows=(0, 11), kind="prop", name="cliff_detail",
             desc="cliff-side detail prop"),
        dict(cols=(1, 2), rows=(0, 2), kind="object", name="cave_mouth_stone", ow=2, oh=3,
             collab=["left", "right"], rowlab=["top", "mid", "base"],
             desc="dark cave mouth set into a grey-green stone cliff"),
        dict(cols=(1, 2), rows=(3, 5), kind="object", name="cave_mouth_sandstone", ow=2, oh=3,
             collab=["left", "right"], rowlab=["top", "mid", "base"],
             desc="dark cave mouth set into a sandstone cliff"),
        dict(cols=(3, 3), rows=(0, 5), kind="prop", name="ladder",
             desc="wooden ladder rung section for climbing a cliff"),
        dict(cols=(4, 5), rows=(0, 5), kind="prop", name="loose_rocks",
             desc="loose boulders and rubble that sit against a cliff"),
        dict(cols=(0, 0), rows=(0, 5), kind="decor", name="small_rock",
             desc="single small rock or shrub sitting on the ground"),
        dict(cols=(0, 2), rows=(6, 6), kind="prop", name="plank_bridge",
             desc="short wooden plank walkway"),
        dict(cols=(0, 5), rows=(7, 9), kind="object", name="mine_entrance", ow=3, oh=3,
             collab=["left", "mid", "right"], rowlab=["lintel", "mouth", "base"],
             desc="timber-framed mine entrance cut into rock"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetNature.png (24x21) — trees, rocks and plants. THE sheet that caused the bad pick:
    # nothing here is a ground fill and almost nothing is one tile. Trees are 2 wide x 2 tall
    # in rows 0-1, 4 wide x 3 tall in rows 2-4 and 5-7, and 3 wide x 3 tall in rows 18-20.
    # Only row 10-11 and 12-17 singles are genuinely one-tile sprites.
    # ------------------------------------------------------------------------------------
    "TilesetNature.png": [
        dict(cols=(0, 23), rows=(0, 20), kind="prop", name="nature_prop",
             desc="natural scenery prop"),
        dict(cols=(0, 19), rows=(0, 1), kind="object", name="tree_small", ow=2, oh=2,
             collab=["left", "right"], rowlab=["canopy", "trunk"],
             desc="small broadleaf tree, 2 tiles wide and 2 tall"),
        dict(cols=(0, 23), rows=(2, 4), kind="object", name="tree_broad", ow=4, oh=3,
             collab=["far_left", "left", "right", "far_right"],
             rowlab=["canopy_top", "canopy", "trunk"],
             desc="large broadleaf tree, 4 tiles wide and 3 tall"),
        dict(cols=(0, 19), rows=(5, 7), kind="object", name="landmark_large", ow=4, oh=3,
             collab=["far_left", "left", "right", "far_right"],
             rowlab=["top", "mid", "base"],
             desc="large landmark: dead tree, snowy mountain peak or boulder outcrop, 4x3 tiles"),
        dict(cols=(0, 17), rows=(8, 9), kind="object", name="stump_log", ow=2, oh=2,
             collab=["left", "right"], rowlab=["top", "base"],
             desc="tree stump, fallen log, bush or boulder, 2 tiles wide and 2 tall"),
        dict(cols=(0, 13), rows=(10, 11), kind="decor", name="plant",
             desc="single-tile plant: grass tuft, fern, flower or bamboo shoot"),
        dict(cols=(0, 3), rows=(12, 13), kind="object", name="boulder_round", ow=2, oh=2,
             collab=["left", "right"], rowlab=["top", "base"],
             desc="large round boulder or snowball, 2 tiles wide and 2 tall"),
        dict(cols=(4, 13), rows=(12, 13), kind="decor", name="ground_object",
             desc="single-tile ground object: rock, snowdrift, sand mound or cut log"),
        dict(cols=(0, 3), rows=(14, 14), kind="decor", name="leaf",
             desc="single fallen leaf - autumn, frosted, blossom or green"),
        dict(cols=(0, 3), rows=(15, 16), kind="decor", name="crystal",
             desc="coloured crystal / gem shard, small chip and large shard"),
        dict(cols=(4, 6), rows=(14, 17), kind="decor", name="ore_bush",
             desc="single-tile bush or rock studded with ore nodules or berries"),
        dict(cols=(7, 10), rows=(14, 17), kind="object", name="berry_bush", ow=2, oh=1,
             collab=["left", "right"], rowlab=["whole"],
             desc="berry / ore bush drawn 2 tiles wide"),
        dict(cols=(12, 13), rows=(14, 15), kind="object", name="stump_cut", ow=1, oh=2,
             rowlab=["top", "base"],
             desc="freshly cut tree stump with regrowth, 1 tile wide and 2 tall"),
        dict(cols=(13, 14), rows=(16, 18), kind="object", name="tree_tall_trunk", ow=2, oh=3,
             collab=["left", "right"], rowlab=["upper", "mid", "base"],
             desc="tall bare tree trunk, 2 tiles wide and 3 tall"),
        dict(cols=(15, 22), rows=(10, 17), kind="object", name="boulder_massive", ow=8, oh=4,
             collab=["c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"],
             rowlab=["r0", "r1", "r2", "r3"],
             desc="massive multi-tile boulder formation, sandstone or grey stone"),
        dict(cols=(15, 18), rows=(18, 18), kind="decor", name="rock_pile",
             desc="small pile of sandy rocks"),
        dict(cols=(0, 11), rows=(18, 20), kind="object", name="tree_round", ow=3, oh=3,
             collab=["left", "mid", "right"], rowlab=["canopy_top", "canopy", "trunk"],
             desc="round-canopy tree in blossom / green / snow / autumn, 3 tiles wide and 3 tall"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetFloorB.png (11x7) — a snow/ice patch drawn OVER another terrain. Transparent
    # outside the patch, so these are overlays, not fills.
    # ------------------------------------------------------------------------------------
    "TilesetFloorB.png": [
        dict(cols=(0, 10), rows=(0, 4), kind="edge", terrain="snow_patch_overlay",
             desc="soft white snow patch overlay with a pale blue rim, drawn over other ground"),
        dict(cols=(0, 0), rows=(5, 5), kind="decor", name="ice_sheet",
             desc="small sheet of ice with diagonal highlight streaks"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetFloorDetail.png (16x5) — loose ground decoration, all on transparency.
    # ------------------------------------------------------------------------------------
    "TilesetFloorDetail.png": [
        dict(cols=(0, 15), rows=(0, 0), kind="decor", name="ground_debris",
             desc="ground debris: cracks, scattered pebbles, sticks, a pot, a skull and a bone"),
        dict(cols=(0, 0), rows=(1, 1), kind="decor", name="ore_vein",
             desc="blue-grey ore vein showing through the ground"),
        dict(cols=(0, 7), rows=(2, 2), kind="decor", name="grass_tuft",
             desc="green grass tuft, fern or flower clump to scatter over ground fill"),
        dict(cols=(0, 7), rows=(3, 3), kind="decor", name="snow_tuft",
             desc="frosted white grass tuft or snow clump to scatter over snow fill"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetField.png (5x15) — cultivated field / meadow patches in five colours. Cols 0-2 are
    # the rounded patch outline, cols 3-4 the interior fill pieces. Three rows per colour:
    # top edge, middle, bottom fringe.
    # ------------------------------------------------------------------------------------
    "TilesetField.png": [
        dict(cols=(0, 4), rows=(0, 2), kind="edge", terrain="field_orange",
             desc="orange cultivated field patch with a rounded outline and fringed bottom edge"),
        dict(cols=(0, 4), rows=(3, 5), kind="edge", terrain="field_lime",
             desc="lime-green meadow patch with a rounded outline and fringed bottom edge"),
        dict(cols=(0, 4), rows=(6, 8), kind="edge", terrain="field_green",
             desc="deep green meadow patch with a rounded outline and fringed bottom edge"),
        dict(cols=(0, 4), rows=(9, 11), kind="edge", terrain="field_rose",
             desc="rose-pink field patch with a rounded outline and fringed bottom edge"),
        dict(cols=(0, 4), rows=(12, 14), kind="edge", terrain="field_snow",
             desc="white snow field patch with a rounded outline and fringed bottom edge"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetHole.png (11x5) — pits/chasms. Cols 0-3 a square pit, cols 4-10 diagonal pit edges.
    # ------------------------------------------------------------------------------------
    "TilesetHole.png": [
        dict(cols=(0, 10), rows=(0, 4), kind="edge", terrain="pit",
             desc="dark chasm floor with a brown rock rim"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetLogic.png (8x10) — MAP-EDITOR MARKERS, not art. Row 0 is eight flat colour swatches
    # (this is where the flat grey 'ash' tile came from); rows 1-9 are key/eye/chest/?/X/A/B/C/D
    # glyphs in the same eight colours. Never put these on the map.
    # ------------------------------------------------------------------------------------
    "TilesetLogic.png": [
        dict(cols=(0, 7), rows=(0, 0), kind="fill", terrain="editor_swatch",
             desc="flat editor colour swatch - a debug marker, not terrain art"),
        dict(cols=(0, 7), rows=(1, 9), kind="decor", name="editor_glyph",
             desc="editor marker glyph (key, eye, chest, question mark, X, A, B, C, D)"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetDungeon.png (12x4) — dungeon dressing: portraits, chests, orbs, switches.
    # ------------------------------------------------------------------------------------
    "TilesetDungeon.png": [
        dict(cols=(0, 11), rows=(0, 3), kind="prop", name="dungeon_prop",
             desc="dungeon dressing: framed portrait, rug, cabinet, glowing orb or switch"),
    ],

    # ------------------------------------------------------------------------------------
    # Pipes.png (15x3) — industrial pipe runs in three colours, each 5 columns wide.
    # ------------------------------------------------------------------------------------
    "Pipes.png": [
        dict(cols=(0, 4), rows=(0, 2), kind="object", name="pipe_orange", ow=5, oh=3,
             collab=["end", "mid", "mid2", "mid3", "cap"], rowlab=["top", "body", "base"],
             desc="orange industrial pipe run"),
        dict(cols=(5, 9), rows=(0, 2), kind="object", name="pipe_grey", ow=5, oh=3,
             collab=["end", "mid", "mid2", "mid3", "cap"], rowlab=["top", "body", "base"],
             desc="grey industrial pipe run"),
        dict(cols=(10, 14), rows=(0, 2), kind="object", name="pipe_green", ow=5, oh=3,
             collab=["end", "mid", "mid2", "mid3", "cap"], rowlab=["top", "body", "base"],
             desc="green industrial pipe run"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetTowers.png (24x6) — landmark formations, every one 2 tiles wide, in three row bands.
    # ------------------------------------------------------------------------------------
    "TilesetTowers.png": [
        dict(cols=(0, 23), rows=(0, 1), kind="object", name="landmark_tower", ow=2, oh=2,
             collab=["left", "right"], rowlab=["top", "base"],
             desc="landmark formation: rock arch, cairn or shrine, 2 tiles wide and 2 tall"),
        dict(cols=(0, 23), rows=(2, 3), kind="object", name="landmark_tower_b", ow=2, oh=2,
             collab=["left", "right"], rowlab=["top", "base"],
             desc="landmark formation in stone, coral or foliage, 2 tiles wide and 2 tall"),
        dict(cols=(0, 23), rows=(4, 5), kind="object", name="landmark_tower_c", ow=2, oh=2,
             collab=["left", "right"], rowlab=["top", "base"],
             desc="landmark formation: lava rock, sand mound, gate or crystal, 2x2 tiles"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetElement.png (16x15) — furniture and village props on transparency. The yellow block
    # at cols 12-15 rows 4-6 is a tileable straw / thatch surface.
    # ------------------------------------------------------------------------------------
    "TilesetElement.png": [
        dict(cols=(0, 15), rows=(0, 14), kind="prop", name="village_prop",
             desc="village prop: barrels, pots, crates, signs, fences, stalls and furniture"),
        dict(cols=(12, 15), rows=(4, 6), kind="fill", terrain="straw",
             desc="tileable straw / thatch surface"),
        dict(cols=(0, 11), rows=(8, 13), kind="prop", name="furniture",
             desc="indoor furniture: chests of drawers, bookshelves, tables, banners and beds"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetHouse.png (33x23) — full building kit: roofs, walls, doors, windows, fences,
    # interiors, shop counters and shrine pieces. Far too varied for tile-level naming, so it is
    # banded by what the contact sheet shows in each region.
    # ------------------------------------------------------------------------------------
    "TilesetHouse.png": [
        dict(cols=(0, 32), rows=(0, 22), kind="prop", name="house_part",
             desc="building kit piece"),
        dict(cols=(0, 15), rows=(0, 2), kind="prop", name="house_roof",
             desc="house roof and upper wall: orange tile, plaster or red pagoda"),
        dict(cols=(0, 15), rows=(3, 4), kind="prop", name="house_front",
             desc="house frontage: doors, windows, shutters and a DOJO sign"),
        dict(cols=(0, 7), rows=(5, 7), kind="prop", name="torii_gate",
             desc="red torii gate uprights and crossbeams"),
        dict(cols=(8, 15), rows=(4, 7), kind="prop", name="fence",
             desc="wooden fence, gate and railing sections"),
        dict(cols=(16, 21), rows=(0, 9), kind="prop", name="house_interior_wall",
             desc="interior wall, sliding screen and ceiling beams"),
        dict(cols=(22, 28), rows=(0, 9), kind="prop", name="house_interior_floor",
             desc="interior wooden floor, tatami and stair pieces"),
        dict(cols=(29, 32), rows=(0, 9), kind="prop", name="house_furniture",
             desc="indoor furniture: cabinets, screens, tables and hearths"),
        dict(cols=(0, 15), rows=(8, 15), kind="prop", name="wall_stone",
             desc="stone and brick perimeter wall, arches and battlements"),
        dict(cols=(0, 8), rows=(11, 13), kind="prop", name="window_glass",
             desc="large paned window and shutter set"),
        dict(cols=(16, 24), rows=(10, 15), kind="prop", name="shop_counter",
             desc="shop counter, market stall, barrels and produce baskets"),
        dict(cols=(25, 32), rows=(10, 18), kind="prop", name="barn",
             desc="timber barn wall, gable and door"),
        dict(cols=(0, 15), rows=(16, 22), kind="prop", name="shrine_stone",
             desc="carved stone shrine, statue faces and steps"),
        dict(cols=(16, 24), rows=(16, 22), kind="prop", name="interior_wood",
             desc="wooden interior panelling, shelving and ladders"),
        dict(cols=(22, 28), rows=(19, 22), kind="decor", name="rug",
             desc="small patterned rug in one of six colours"),
        dict(cols=(29, 32), rows=(19, 22), kind="prop", name="stone_arch",
             desc="grey stone arch and pillar pieces"),
    ],

    # ------------------------------------------------------------------------------------
    # TilesetVillageAbandoned.png (20x12) — ruined, moss-covered version of the village kit.
    # ------------------------------------------------------------------------------------
    "TilesetVillageAbandoned.png": [
        dict(cols=(0, 19), rows=(0, 11), kind="prop", name="ruin_part",
             desc="abandoned village piece: collapsed roofs, mossy walls and overgrown rubble"),
        dict(cols=(0, 10), rows=(2, 5), kind="prop", name="ruin_wall",
             desc="crumbling moss-covered wall and doorway"),
        dict(cols=(11, 19), rows=(0, 11), kind="prop", name="ruin_house",
             desc="derelict timber house frame, broken windows and sagging roof"),
        dict(cols=(0, 10), rows=(6, 11), kind="object", name="overgrown_mound", ow=4, oh=3,
             collab=["far_left", "left", "right", "far_right"], rowlab=["top", "mid", "base"],
             desc="mound of rubble buried under moss and grass"),
    ],

    # ------------------------------------------------------------------------------------
    # tileset_bed.png (14x12) — beds and heavy furniture, 3 rows per bed colour.
    # ------------------------------------------------------------------------------------
    "tileset_bed.png": [
        dict(cols=(0, 13), rows=(0, 11), kind="prop", name="bedroom_furniture",
             desc="bedroom furniture piece"),
        dict(cols=(0, 12), rows=(0, 2), kind="object", name="bed_straw", ow=13, oh=3,
             collab=["l", "r", "d_l", "d_m", "d_r", "s_l", "s_r", "w_l", "w_r",
                     "b_l", "b_m", "b_r", "n"],
             rowlab=["pillow", "sheet", "foot"],
             desc="straw / linen bed, single and double widths"),
        dict(cols=(0, 12), rows=(3, 5), kind="object", name="bed_red", ow=13, oh=3,
             collab=["l", "r", "d_l", "d_m", "d_r", "s_l", "s_r", "w_l", "w_r",
                     "b_l", "b_m", "b_r", "n"],
             rowlab=["pillow", "sheet", "foot"],
             desc="red-sheeted bed, single and double widths"),
        dict(cols=(0, 13), rows=(6, 8), kind="prop", name="bench_cabinet",
             desc="padded bench, cabinet and wooden shelving"),
        dict(cols=(0, 6), rows=(9, 11), kind="prop", name="stone_slab",
             desc="green-grey cracked stone slab and plinth pieces"),
    ],

    # ------------------------------------------------------------------------------------
    # tileset_camp.png (23x9) — camp gear plus a big sand mesa. Cols 14-22 rows 1-8 are the
    # mesa: mostly FLAT sand with a handful of wind-ripple textured tiles at cols 16-17.
    # ------------------------------------------------------------------------------------
    "tileset_camp.png": [
        dict(cols=(0, 3), rows=(0, 8), kind="prop", name="camp_crate",
             desc="wooden crate, barrel and plank stack"),
        dict(cols=(4, 12), rows=(0, 2), kind="object", name="tent", ow=3, oh=3,
             collab=["left", "mid", "right"], rowlab=["peak", "body", "entrance"],
             desc="canvas camp tent, 3 tiles wide and 3 tall"),
        dict(cols=(4, 13), rows=(3, 8), kind="prop", name="camp_gear",
             desc="camp gear: cooking pots, bedrolls, firewood, rope and lanterns"),
        dict(cols=(10, 13), rows=(3, 6), kind="prop", name="camp_rock_ring",
             desc="ring of grey stones around a fire pit or spring"),
        dict(cols=(13, 22), rows=(0, 2), kind="edge", terrain="mesa_brown",
             desc="brown rock mesa top with layered strata"),
        dict(cols=(14, 22), rows=(2, 8), kind="edge", terrain="sand_dune",
             desc="broad golden sand mesa surface with wind-ripple detail"),
        dict(cols=(16, 18), rows=(5, 7), kind="fill", terrain="sand_dune_ripple",
             desc="golden dune sand carrying long wind-ripple strokes"),
        dict(cols=(17, 18), rows=(8, 8), kind="prop", name="sand_pool",
             desc="white pool set into the sand mesa"),
    ],

    # ------------------------------------------------------------------------------------
    # Interior/Elements.png (9x3) — a handful of interior blocks: a crate wall, a stone
    # sarcophagus / pedestal pair and a green slime block.
    # ------------------------------------------------------------------------------------
    "Elements.png": [
        dict(cols=(0, 1), rows=(0, 2), kind="object", name="crate_wall", ow=2, oh=3,
             collab=["left", "right"], rowlab=["top", "mid", "base"],
             desc="stacked orange crate wall"),
        dict(cols=(2, 8), rows=(0, 2), kind="prop", name="interior_block",
             desc="grey-green stone block, pedestal or slime block"),
    ],

    # ------------------------------------------------------------------------------------
    # Interior/TilesetInterior.png (16x20) — interior wall autotile drawn against a black void.
    # Two 8-column blocks, four 5-row colour sets (pale, orange, mauve, green).
    # ------------------------------------------------------------------------------------
    "TilesetInterior.png": [
        dict(cols=(0, 7), rows=(0, 9), kind="edge", terrain="wall_pale",
             desc="pale plaster interior wall run seen from above, against a dark void"),
        dict(cols=(8, 15), rows=(0, 9), kind="edge", terrain="wall_orange",
             desc="orange interior wall run seen from above, against a dark void"),
        dict(cols=(0, 7), rows=(10, 19), kind="edge", terrain="wall_mauve",
             desc="mauve brick interior wall run seen from above, against a dark void"),
        dict(cols=(8, 15), rows=(10, 19), kind="edge", terrain="wall_green",
             desc="green brick interior wall run seen from above, against a dark void"),
    ],

    # ------------------------------------------------------------------------------------
    # Interior/TilesetWallSimple.png (10x11) — the same walls in a simplified 5-column set,
    # with a round rug / floor medallion in the middle of each.
    # ------------------------------------------------------------------------------------
    "TilesetWallSimple.png": [
        dict(cols=(0, 4), rows=(0, 4), kind="edge", terrain="wall_simple_pale",
             desc="simplified pale interior wall ring with a round rug in the middle"),
        dict(cols=(5, 9), rows=(0, 4), kind="edge", terrain="wall_simple_orange",
             desc="simplified orange interior wall ring with a round rug in the middle"),
        dict(cols=(0, 4), rows=(6, 10), kind="edge", terrain="wall_simple_mauve",
             desc="simplified mauve brick interior wall ring with an arched alcove"),
        dict(cols=(5, 9), rows=(6, 10), kind="edge", terrain="wall_simple_green",
             desc="simplified green brick interior wall ring with an arched alcove"),
    ],

    # ------------------------------------------------------------------------------------
    # Interior/TilesetInteriorFloor.png (22x17) — tileable patterned floors. Two 10-column
    # blocks per row group; within a block cols 0-3 are a bordered frame, cols 4-9 the plain
    # repeating pattern. Every one of these is a genuinely textured fill.
    # ------------------------------------------------------------------------------------
    "TilesetInteriorFloor.png": [
        dict(cols=(0, 10), rows=(0, 5), kind="fill", terrain="floor_brick_pale",
             desc="pale brick interior floor with a bordered frame"),
        dict(cols=(11, 21), rows=(0, 5), kind="fill", terrain="floor_wood",
             desc="tan wooden plank interior floor with a bordered frame"),
        dict(cols=(0, 10), rows=(6, 11), kind="fill", terrain="floor_brick_orange",
             desc="orange brick interior floor with a bordered frame"),
        dict(cols=(11, 21), rows=(6, 11), kind="fill", terrain="floor_stone_green",
             desc="green-grey stone interior floor with a bordered frame"),
        dict(cols=(0, 10), rows=(12, 16), kind="fill", terrain="floor_cobble_sand",
             desc="sandy cobblestone floor, rounded stones in a warm mortar"),
        dict(cols=(11, 21), rows=(12, 16), kind="fill", terrain="floor_cobble_dark",
             desc="dark cobblestone floor, brown stones in near-black mortar"),
        dict(cols=(12, 14), rows=(5, 5), kind="decor", name="floor_decal",
             desc="floor decal: water splash, ice shards or a compass rose inlay"),
        dict(cols=(12, 14), rows=(11, 11), kind="decor", name="floor_decal_green",
             desc="floor decal on green stone: water splash, ice shards or a rose inlay"),
    ],
}
