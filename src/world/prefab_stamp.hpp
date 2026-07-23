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

// Is this cell drawn at all for this instance? Two independent reasons a cell is dropped:
//
//   * its cluster was dropped (see prefab_group_kept), or
//   * it is a FLOOR cell (layer 0/1) within one tile of the parcel border, and the feather hash says
//     so. Roughly 40% of border floor tiles vanish, which turns the parcel's hard rectangular
//     outline into a ragged, organic edge that reads as a clearing rather than as a stamp. Layer 2/3
//     cells — the tent, the crates, the props — never feather: a floating half-tent is worse than a
//     square edge, and the props are already scattered inside the parcel where no border touches them.
//
// The feather is hashed from (variant, dx, dy): deterministic, so the sim's blocking and the
// renderer's picture agree, and per-instance, so two camps feather differently.
[[nodiscard]] inline bool prefab_cell_visible(const PrefabDef& def, const PrefabCell& c,
                                              std::uint32_t variant) noexcept {
    if (!prefab_group_kept(def, variant, c.group)) return false;
    if (c.layer >= 2) return true;  // structures and props never feather
    const bool border = c.dx == 0 || c.dy == 0 || c.dx + 1 >= def.w || c.dy + 1 >= def.h;
    if (!border) return true;
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
[[nodiscard]] inline bool prefab_blocks(const PrefabDef& def, std::uint32_t variant, int x,
                                        int y) noexcept {
    if (x < 0 || y < 0 || x >= def.w || y >= def.h) return false;
    // The column to read in the AUTHORED (unmirrored) footprint.
    const int sx = prefab_mirrored(def, variant) ? (def.w - 1 - x) : x;
    if (((def.block_rows[y] >> sx) & 1u) == 0u) return false;
    // The bit is set. If its owning cluster was dropped, the tile is open ground now.
    for (std::uint16_t i = 0; i < def.cell_count; ++i) {
        const PrefabCell& c = def.cells[i];
        if (c.layer != 2) continue;  // only structures block
        if (sx >= c.dx && sx < c.dx + prefab_cell_tw(c) && y >= c.dy &&
            y < c.dy + prefab_cell_th(c)) {
            return prefab_group_kept(def, variant, c.group);
        }
    }
    return true;  // a blocked bit with no matching cell: honour it rather than silently open it
}

}  // namespace mmo
