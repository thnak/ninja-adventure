// A placed prefab, and the pure rules that turn one parcel into many.
//
// `prefabs.hpp` is the ART: twelve hand-composed parcels cut from the pack author's own map, packed
// by tools/build_atlas.py. It says what a camp LOOKS like. This file says what happens when you drop
// one on the world — where it blocks, which optional clusters survive, and how two copies of the
// same parcel end up looking different so the forest is not tiled with one photograph.
//
// EVERYTHING HERE IS A PURE FUNCTION of (def, variant) and nothing else, for the same reason the
// rest of worldgen is: the simulation stamps a camp's blocking into the overlay and the renderer
// draws its floor and props, and the two run in different processes on different machines. If they
// disagreed on which cells a variant keeps, the picture would not match the collision. So both call
// the identical helpers below, and `variant` — a hash of (seed, anchor) computed once at placement —
// is the whole of the shared state. Integer arithmetic only, like `block_noise` next door.
#pragma once

#include <cstdint>

#include "world/prefabs.hpp"
#include "world/tiles.hpp"  // Rng

namespace mmo {

// A prefab dropped on the overworld. `tx,ty` is the parcel's TOP-LEFT tile — the same corner the
// PrefabCell dx/dy count from — so a cell of the parcel lands at (tx+dx, ty+dy). `variant` is a hash
// of (seed, anchor): it drives the mirror, the kept clusters and the edge feather, and it is what
// makes the instance at one anchor differ from the instance at the next.
struct PlacedPrefab {
    std::int32_t tx;
    std::int32_t ty;
    PrefabId id;
    std::uint32_t variant;
    // Which palette this instance is drawn in (see PrefabSkin in prefabs.hpp): 0 = the base green
    // parcel, higher = the pack author's deep-forest or snow twin. Chosen at placement from the
    // variant (prefab_pick_skin), so it is part of the shared state the renderer and the sim both
    // read, and the picture cannot pick a different skin than the collision assumed. A village
    // parcel and every unskinned type are 0.
    std::uint8_t skin;
};

// Prefab cells measure themselves in 16px tiles (see prefabs.hpp) — a floor cell is 16x16, a house
// up to 64x48. This is that 16, named so the tile arithmetic below reads. It is NOT the renderer's
// kTilePx (the on-screen size): a prefab is authored at the atlas' native resolution and scaled up
// when drawn, exactly like every other sprite.
inline constexpr int kPrefabTilePx = 16;

// Cell size in whole tiles, rounding up: a 48px cell is 3 tiles, a 40px one is 3 as well. Used to
// find which structure cell owns a blocked tile.
[[nodiscard]] inline constexpr int prefab_cell_tw(const PrefabCell& c) noexcept {
    return (c.pw + kPrefabTilePx - 1) / kPrefabTilePx;
}
[[nodiscard]] inline constexpr int prefab_cell_th(const PrefabCell& c) noexcept {
    return (c.ph + kPrefabTilePx - 1) / kPrefabTilePx;
}

// --- Doors, for parcels a village lays as furniture -------------------------------------------
// A prefab house is one of the pack's own TilesetHouse sprites (tagged `has_door` at pack time), and
// those are the very sprites world/village.hpp measured a doorway for: the dark run in the bottom
// row sits in COLUMN 1 of every one of them, which is what `kDoorDx` records. So a prefab dwelling
// is enterable at that same column, and needs no second measurement. These helpers give a village
// builder the door tile of a dwelling cell in PARCEL-LOCAL coordinates; the builder leaves that tile
// walkable under the sprite and worldgen emits a Door pointing at it, exactly as it does for a
// Structure house. They assume the UNMIRRORED footprint, which is all a village ever stamps (see the
// note where the builder clears the mirror bit).
inline constexpr int kPrefabDoorDx = 1;

[[nodiscard]] inline constexpr bool prefab_cell_is_dwelling(const PrefabCell& c) noexcept {
    return c.has_door;
}
[[nodiscard]] inline constexpr int prefab_door_dx(const PrefabCell& c) noexcept {
    return static_cast<int>(c.dx) + kPrefabDoorDx;
}
[[nodiscard]] inline constexpr int prefab_door_dy(const PrefabCell& c) noexcept {
    return static_cast<int>(c.dy) + prefab_cell_th(c) - 1;
}

// The skin array this instance draws from. A skin index past what the parcel actually has (never
// authored by the table, but cheap to guard) falls back to the base, so a caller can hand this any
// stored skin without a bounds check of its own.
[[nodiscard]] inline constexpr const PrefabSkin& prefab_skin_of(const PrefabDef& def,
                                                                std::uint8_t skin) noexcept {
    return def.skins[skin < def.skin_count ? skin : 0];
}

// Pick a skin for a placement from its variant, among the skins its placement row allows. `skin_mask`
// is a bit per skin index (bit 0 = base, always set by construction); the choice is a hash of the
// variant's TOP byte. That byte is spent HERE and nowhere else: the mirror reads bit 0, the
// kept-cluster rule reads bits 3+, and the feather hashes the whole word with (dx,dy) — all
// downstream of, and none moved by, this read. And because a placement's anchor is drawn from a
// different RNG stream BEFORE the variant exists, choosing a skin cannot move a placement at all:
// the anchors and the variants are byte-for-byte what they were before skins existed, and only the
// drawn palette changes. Pure and deterministic, so every node picks the same skin for the same
// parcel.
[[nodiscard]] inline constexpr std::uint8_t prefab_pick_skin(std::uint32_t variant,
                                                             std::uint8_t skin_mask) noexcept {
    int n = 0;
    for (int k = 0; k < 8; ++k) n += (skin_mask >> k) & 1u;
    if (n <= 1) {  // a single allowed skin (usually just the base): no choice to make
        for (int k = 0; k < 8; ++k) {
            if ((skin_mask >> k) & 1u) return static_cast<std::uint8_t>(k);
        }
        return 0;
    }
    int pick = static_cast<int>((variant >> 24) % static_cast<std::uint32_t>(n));
    for (int k = 0; k < 8; ++k) {
        if ((skin_mask >> k) & 1u) {
            if (pick == 0) return static_cast<std::uint8_t>(k);
            --pick;
        }
    }
    return 0;
}

// Is this instance drawn flipped left-to-right? Only when the parcel is safe to mirror (no readable
// glyphs baked into its art — `mirrorable`), and then on the low bit of the variant, so about half
// of the mirrorable ones are. Bit 0 is spent here and nowhere else.
[[nodiscard]] inline constexpr bool prefab_mirrored(const PrefabDef& def,
                                                    std::uint32_t variant) noexcept {
    return def.mirrorable && (variant & 1u) != 0u;
}

// Does this instance keep optional cluster `group`? Group 0 is the parcel's floor and centrepiece
// and is ALWAYS stamped. Groups 1..group_count are independent clusters of props/houses a caller may
// drop, so one parcel yields many arrangements. Kept ~75% of the time: two variant bits per group,
// dropped only when both are zero. (kCampClearing has no optional groups, so this is a no-op for it
// and lives here for the parcels P2 turns on next.)
[[nodiscard]] inline constexpr bool prefab_group_kept(const PrefabDef& def, std::uint32_t variant,
                                                      int group) noexcept {
    if (group <= 0 || group > static_cast<int>(def.group_count)) return true;
    return ((variant >> (2 + group)) & 3u) != 0u;
}

// Force EVERY optional cluster of an instance to survive, by setting the low group bit
// prefab_group_kept reads (one bit is enough — a non-zero pair keeps the cluster). Some parcels
// only read right whole: a fort whose ring of walls has a gap dropped is not a fort, a cottage
// whose one house was the dropped cluster is an empty lawn. Those set allow_group_drop=false in the
// placement table, and placement runs the variant through here BEFORE storing it in PlacedPrefab.
// Because the MASKED variant is what both the sim's blocking and the renderer's picture then read,
// neither side has to know the policy — the variant stays the whole of the shared state, exactly as
// this file's header insists.
[[nodiscard]] inline constexpr std::uint32_t prefab_force_groups(const PrefabDef& def,
                                                                 std::uint32_t variant) noexcept {
    for (int group = 1; group <= static_cast<int>(def.group_count); ++group) {
        variant |= (1u << (2 + group));
    }
    return variant;
}

// Is this cell drawn at all for this instance? Two independent reasons a cell is dropped:
//
//   * its cluster was dropped (see prefab_group_kept), or
//   * it is a FLOOR cell (layer 0/1) within one tile of the parcel border, and the feather hash says
//     so. Roughly 40% of border floor tiles vanish, which turns the parcel's hard rectangular
//     outline into a ragged, organic edge that reads as a clearing rather than as a stamp. Layer 2/3
//     cells — the tent, the crates, the props — never feather: a floating half-tent is worse than a
//     square edge, and the props are already scattered inside the parcel where no border touches them.
//
// EXCEPT under a prop. A border floor cell that a kept layer-2/3 cell stands on is exempt from the
// feather. Green-on-green hid why this matters; the snow pond showed it: its east-side barrels are
// border props, and feathering the snow out from under them left them floating on the ring's bare
// stone. The pedestal search is O(cell_count) per border cell, paid only by the few dozen border
// cells of the handful of parcels on screen.
//
// The feather is hashed from (variant, dx, dy): deterministic, so the sim's blocking and the
// renderer's picture agree, and per-instance, so two camps feather differently.
//
// `sk` is the instance's SKIN cells — the array `c` was iterated from. The pedestal search below
// walks it too (not def.cells) so it stays consistent: a snow skin drops the floor cell under where
// a sunflower used to be AND drops the sunflower itself, so nothing pedestals it and the floor
// feathers as it should. The layer-2/3 props a skin DOES keep sit at the same anchors as the base,
// so a camp's tent still pedestals its ground in every skin.
[[nodiscard]] inline bool prefab_cell_visible(const PrefabDef& def, const PrefabSkin& sk,
                                              const PrefabCell& c, std::uint32_t variant) noexcept {
    if (!prefab_group_kept(def, variant, c.group)) return false;
    if (c.layer >= 2) return true;  // structures and props never feather
    const bool border = c.dx == 0 || c.dy == 0 || c.dx + 1 >= def.w || c.dy + 1 >= def.h;
    if (!border) return true;
    for (std::uint16_t i = 0; i < sk.cell_count; ++i) {
        const PrefabCell& s = sk.cells[i];
        if (s.layer < 2 || !prefab_group_kept(def, variant, s.group)) continue;
        // The prop's tile footprint, one tile wider on each side when it draws centred — a centred
        // sprite overhangs its anchor cell by up to half its width, and a pedestal that is merely
        // approximate errs on keeping ground, never on dropping it.
        const int tw = prefab_cell_tw(s);
        const int th = prefab_cell_th(s);
        const int pad = s.centred ? 1 : 0;
        if (c.dx >= s.dx - pad && c.dx < s.dx + tw + pad &&
            c.dy >= s.dy - pad && c.dy < s.dy + th + pad) {
            return true;
        }
    }
    Rng r(static_cast<std::uint64_t>(variant) ^
          (static_cast<std::uint64_t>(c.dx) * 0x9E37'79B9'7F4A'7C15ull) ^
          (static_cast<std::uint64_t>(c.dy) * 0xC2B2'AE3D'27D4'EB4Full));
    return r.below(100) >= 40;  // keep ~60% of the border floor
}

// Does this instance BLOCK the tile at parcel offset (x,y)? The authored `block_rows` bitmask is the
// truth for the unmirrored parcel; a mirrored instance reads the mirrored column. A set bit is only
// a real block if the structure that owns it survived its cluster's drop — so once a bit is set we
// find the layer-2 cell covering that tile and consult its group.
//
// COST: the cell lookup is O(cell_count) in the worst case, but it runs only for tiles that are
// ALREADY marked blocked in block_rows — a handful per parcel (kCampClearing has nine) — so a stamp
// pays it a few dozen times, not once per footprint tile. Kept a pure function rather than a
// precomputed table because the sim calls it exactly once per camp at generation time.
//
// `sk` is the instance's SKIN cells, walked here for the owning-cell lookup so this reads the same
// array the renderer does. It CANNOT drift with the skin: a skin only ever drops floor/nature cells
// (layers 0/1/3) or swaps a rect, never a layer-2 STRUCTURE (those are unmapped TilesetHouse
// sprites, identical in every skin — enforced in tools/build_atlas.py), and block_rows is the
// authored base. So whichever skin is passed, the same layer-2 cell covers the tile and the same
// answer comes back — the collision is skin-invariant by construction, exactly as the header insists.
[[nodiscard]] inline bool prefab_blocks(const PrefabDef& def, const PrefabSkin& sk,
                                        std::uint32_t variant, int x, int y) noexcept {
    if (x < 0 || y < 0 || x >= def.w || y >= def.h) return false;
    // The column to read in the AUTHORED (unmirrored) footprint.
    const int sx = prefab_mirrored(def, variant) ? (def.w - 1 - x) : x;
    if (((def.block_rows[y] >> sx) & 1u) == 0u) return false;
    // The bit is set. If its owning cluster was dropped, the tile is open ground now.
    for (std::uint16_t i = 0; i < sk.cell_count; ++i) {
        const PrefabCell& c = sk.cells[i];
        if (c.layer != 2) continue;  // only structures block
        if (sx >= c.dx && sx < c.dx + prefab_cell_tw(c) && y >= c.dy &&
            y < c.dy + prefab_cell_th(c)) {
            return prefab_group_kept(def, variant, c.group);
        }
    }
    return true;  // a blocked bit with no matching cell: honour it rather than silently open it
}

}  // namespace mmo
