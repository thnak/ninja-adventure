// A procedural dungeon/room generator — an editor-side authoring HELPER (see map_editor_main.cpp's
// "Generate" panel), not a live runtime system. It fills an ordinary `AuthoredMap` (authored_map.hpp)
// the same way a person painting in the editor would, so its output is immediately editable by hand
// and saved through the exact same path — there is no separate "generated map" representation.
//
// Layered pipeline, chosen for output quality over any single technique, reusing machinery this
// codebase already has proven deterministic rather than inventing new noise:
//   1. BSP room+corridor skeleton for structure (portals are ROUTED TO as fixed anchors, not
//      dropped wherever the tree happens to end up).
//   2. Cellular automata for a fraction of rooms, tagged "mine" — organic caverns next to the
//      rectangular BSP rooms.
//   3. tiles.hpp's own fbm noise textures land-room floors (grass/stone), scoped to the room's
//      rect instead of the whole overworld.
//   4. A small table-driven scatter pass for big-stone obstacles and corner decor (interpreting
//      "blooms in the corners" as flower/plant clusters — flagged for confirmation in the plan).
// Every step is seeded off `GenConfig::seed` alone, so the same config always produces the same
// map (verified by sim_main.cpp's determinism test).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "render/atlas_slots.hpp"
#include "world/authored_map.hpp"
#include "world/tiles.hpp"

namespace mmo {

struct GenConfig {
    std::uint16_t width = 48;
    std::uint16_t height = 48;
    std::uint64_t seed = 0x5EED'0BEEF'CAFEull;
    int min_room_size = 6;              // BSP leaf floor, in tiles
    float mine_room_fraction = 0.25f;   // fraction of rooms turned into cellular-automata caverns
    int portal_count = 1;               // fixed anchor tiles the BSP tree routes to
    float decor_density = 0.06f;        // per-floor-tile chance of a big-stone scatter roll
};

namespace detail_dungeon {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] int cx() const noexcept { return x + w / 2; }
    [[nodiscard]] int cy() const noexcept { return y + h / 2; }
};

struct Room {
    Rect floor;
    bool mine = false;
};

// Recursively halve `r` until neither axis can support two `min_size` children, alternating the
// split axis toward whichever side is longer so leaves stay roughly square instead of degenerating
// into slivers. The cut point is randomized within the legal range, not fixed at the midpoint, so
// rooms vary in size rather than tiling uniformly.
inline void bsp_split(const Rect& r, int min_size, Rng& rng, std::vector<Rect>& leaves) {
    const bool can_split_w = r.w >= min_size * 2 + 1;
    const bool can_split_h = r.h >= min_size * 2 + 1;
    if (!can_split_w && !can_split_h) {
        leaves.push_back(r);
        return;
    }
    const bool split_h = can_split_h && (!can_split_w || r.h >= r.w);
    if (split_h) {
        const int span = r.h - min_size * 2;
        const int cut = min_size + static_cast<int>(rng.below(static_cast<std::uint32_t>(span + 1)));
        bsp_split(Rect{r.x, r.y, r.w, cut}, min_size, rng, leaves);
        bsp_split(Rect{r.x, r.y + cut, r.w, r.h - cut}, min_size, rng, leaves);
    } else {
        const int span = r.w - min_size * 2;
        const int cut = min_size + static_cast<int>(rng.below(static_cast<std::uint32_t>(span + 1)));
        bsp_split(Rect{r.x, r.y, cut, r.h}, min_size, rng, leaves);
        bsp_split(Rect{r.x + cut, r.y, r.w - cut, r.h}, min_size, rng, leaves);
    }
}

// A room inset from its leaf's bounds, leaving at least one tile of wall on every side so
// neighbouring leaves don't read as one undivided space before corridors punch through.
[[nodiscard]] inline Rect carve_room_rect(const Rect& leaf, int min_room_size) {
    const int pad_w = std::max(1, leaf.w / 6);
    const int pad_h = std::max(1, leaf.h / 6);
    int rw = std::clamp(leaf.w - 2 * pad_w, std::min(leaf.w, min_room_size), leaf.w);
    int rh = std::clamp(leaf.h - 2 * pad_h, std::min(leaf.h, min_room_size), leaf.h);
    return Rect{leaf.x + (leaf.w - rw) / 2, leaf.y + (leaf.h - rh) / 2, rw, rh};
}

}  // namespace detail_dungeon

[[nodiscard]] inline AuthoredMap generate_dungeon(const GenConfig& cfg) {
    using namespace detail_dungeon;

    AuthoredMap out;
    out.width = cfg.width;
    out.height = cfg.height;
    out.terrain.assign(static_cast<std::size_t>(cfg.width) * cfg.height, Terrain::kBuilding);

    const auto set = [&](int x, int y, Terrain t) {
        if (x < 0 || y < 0 || x >= static_cast<int>(cfg.width) || y >= static_cast<int>(cfg.height)) {
            return;
        }
        out.terrain[static_cast<std::size_t>(y) * cfg.width + static_cast<std::size_t>(x)] = t;
    };
    const auto get = [&](int x, int y) -> Terrain {
        if (x < 0 || y < 0 || x >= static_cast<int>(cfg.width) || y >= static_cast<int>(cfg.height)) {
            return Terrain::kBuilding;
        }
        return out.terrain[static_cast<std::size_t>(y) * cfg.width + static_cast<std::size_t>(x)];
    };

    Rng rng(cfg.seed);

    // --- 1. BSP skeleton -------------------------------------------------------------------------
    std::vector<Rect> leaves;
    bsp_split(Rect{0, 0, cfg.width, cfg.height}, std::max(3, cfg.min_room_size), rng, leaves);
    std::vector<Room> rooms;
    rooms.reserve(leaves.size());
    for (const Rect& leaf : leaves) rooms.push_back({carve_room_rect(leaf, cfg.min_room_size), false});

    // --- 2. Tag a fraction of rooms as mine/cave zones --------------------------------------------
    for (Room& r : rooms) {
        if (rng.unit() < cfg.mine_room_fraction) r.mine = true;
    }

    // --- 3a. Land-room floors: tiles.hpp's own fbm, scoped to the room's rect ---------------------
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        const Room& r = rooms[i];
        if (r.mine) continue;
        const std::uint64_t salt = cfg.seed ^ (static_cast<std::uint64_t>(i) * 0x9E37'79B9'7F4A'7C15ull);
        for (int y = r.floor.y; y < r.floor.y + r.floor.h; ++y) {
            for (int x = r.floor.x; x < r.floor.x + r.floor.w; ++x) {
                const float rock = fbm(salt ^ 0x7777'8888ull, static_cast<float>(x),
                                       static_cast<float>(y), 10.0f);
                set(x, y, rock > 0.62f ? Terrain::kStone : Terrain::kGrass);
            }
        }
    }

    // --- 3b. Mine rooms: cellular automata caverns, scoped to the room's rect ---------------------
    // Standard cave-gen: random fill, then repeatedly wall a cell whose 8-neighbourhood has enough
    // wall neighbours already — a handful of passes turns noise into organic-looking caverns.
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        Room& r = rooms[i];
        if (!r.mine) continue;
        const std::uint64_t salt =
            cfg.seed ^ (static_cast<std::uint64_t>(i) * 0xD1B5'4A32'D192'ED03ull) ^ 0xCAFE'F00Dull;
        Rng cave_rng(salt);
        std::vector<bool> wall(static_cast<std::size_t>(r.floor.w) * r.floor.h, false);
        const auto idx = [&](int lx, int ly) { return static_cast<std::size_t>(ly) * r.floor.w + lx; };
        for (int ly = 0; ly < r.floor.h; ++ly) {
            for (int lx = 0; lx < r.floor.w; ++lx) {
                const bool edge = lx == 0 || ly == 0 || lx == r.floor.w - 1 || ly == r.floor.h - 1;
                wall[idx(lx, ly)] = edge || cave_rng.unit() < 0.42f;
            }
        }
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<bool> next = wall;
            for (int ly = 1; ly < r.floor.h - 1; ++ly) {
                for (int lx = 1; lx < r.floor.w - 1; ++lx) {
                    int walls = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            if (wall[idx(lx + dx, ly + dy)]) ++walls;
                        }
                    }
                    next[idx(lx, ly)] = walls >= 5;
                }
            }
            wall = std::move(next);
        }
        for (int ly = 0; ly < r.floor.h; ++ly) {
            for (int lx = 0; lx < r.floor.w; ++lx) {
                set(r.floor.x + lx, r.floor.y + ly,
                    wall[idx(lx, ly)] ? Terrain::kBuilding : Terrain::kStone);
            }
        }
    }

    // --- Corridors: connect rooms in leaf order (a simple connected chain — every room reachable,
    // not a minimum-spanning search, which is genre-standard for BSP dungeons and stays legible) --
    const auto carve_corridor = [&](int x0, int y0, int x1, int y1) {
        const bool bend_first_h = rng.unit() < 0.5f;
        if (bend_first_h) {
            for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x) set(x, y0, Terrain::kPath);
            for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y) set(x1, y, Terrain::kPath);
        } else {
            for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y) set(x0, y, Terrain::kPath);
            for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x) set(x, y1, Terrain::kPath);
        }
    };
    for (std::size_t i = 1; i < rooms.size(); ++i) {
        carve_corridor(rooms[i - 1].floor.cx(), rooms[i - 1].floor.cy(), rooms[i].floor.cx(),
                       rooms[i].floor.cy());
    }

    // --- Portals: fixed anchors the skeleton routes to, not dropped wherever the tree ends up -----
    // Spread along the map's border, each wired to its nearest room's centre by the same corridor
    // carver used between rooms — guaranteeing every portal is reachable.
    if (!rooms.empty()) {
        for (int p = 0; p < cfg.portal_count; ++p) {
            const float t = cfg.portal_count <= 1
                                ? 0.5f
                                : static_cast<float>(p) / static_cast<float>(cfg.portal_count - 1);
            const int px = std::clamp(static_cast<int>(t * static_cast<float>(cfg.width - 1)), 1,
                                      static_cast<int>(cfg.width) - 2);
            const int py = 1;  // the north border — an arbitrary but consistent edge
            std::size_t nearest = 0;
            int best_d = -1;
            for (std::size_t i = 0; i < rooms.size(); ++i) {
                const int dx = rooms[i].floor.cx() - px;
                const int dy = rooms[i].floor.cy() - py;
                const int d = dx * dx + dy * dy;
                if (best_d < 0 || d < best_d) {
                    best_d = d;
                    nearest = i;
                }
            }
            set(px, py, Terrain::kPath);
            carve_corridor(px, py, rooms[nearest].floor.cx(), rooms[nearest].floor.cy());
            // to_map/to_x/to_y are left at 0 — the caller (map_editor's Generate panel, or whoever
            // consumes this map) wires the actual destination once it knows it; this only reserves
            // the anchor tile and guarantees it's connected.
            out.portals.push_back(AuthoredPortal{static_cast<std::uint16_t>(px),
                                                 static_cast<std::uint16_t>(py), 0, 0, 0});
        }
    }

    // --- 4. Scatter: big-stone obstacles in mine rooms, corner "blooms" in land rooms -------------
    // Placeholder art — kTerrainStone2/kTerrainGrass2 stand in for dedicated big-stone/flower decor
    // tiles until Phase 3 (tools/build_atlas.py manifest expansion) adds them for real.
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        const Room& r = rooms[i];
        const std::uint64_t salt = cfg.seed ^ (static_cast<std::uint64_t>(i) * 0xA24B'AED4'963E'E407ull);
        Rng scatter_rng(salt);
        if (r.mine) {
            for (int y = r.floor.y; y < r.floor.y + r.floor.h; ++y) {
                for (int x = r.floor.x; x < r.floor.x + r.floor.w; ++x) {
                    if (get(x, y) != Terrain::kStone) continue;
                    if (scatter_rng.unit() < cfg.decor_density) {
                        out.decor.push_back(
                            AuthoredDecor{static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y),
                                         Slot::kTerrainStone2});
                    }
                }
            }
        } else {
            // The four inset corners of the room floor.
            const int x0 = r.floor.x + 1, x1 = r.floor.x + r.floor.w - 2;
            const int y0 = r.floor.y + 1, y1 = r.floor.y + r.floor.h - 2;
            const int cx[4] = {x0, x1, x0, x1};
            const int cy[4] = {y0, y0, y1, y1};
            for (int c = 0; c < 4; ++c) {
                if (get(cx[c], cy[c]) != Terrain::kGrass) continue;
                out.decor.push_back(AuthoredDecor{static_cast<std::uint16_t>(cx[c]),
                                                  static_cast<std::uint16_t>(cy[c]),
                                                  Slot::kTerrainGrass2});
            }
        }
    }

    return out;
}

}  // namespace mmo
