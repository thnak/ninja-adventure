// World map exporter — renders the whole overworld to a PNG so its structure can be *looked at*.
//
// This is a debugging tool first and a preview second, and it earns its place: world generation is
// the one system whose failures are invisible from inside the game. A village walled in by a
// stronghold, a ring boundary that reads as a dartboard, an ocean cutting the map in half — none of
// those show up in a screenshot of one 40x22-tile viewport, and all of them are obvious in one look
// at the whole map.
//
// It deliberately draws FLAT COLOURS rather than tiles. A debug map is for reading structure, and
// 1024 tiles of pixel art at 1px each would be mush.
//
// It also needs no actor engine: terrain is a pure function of (seed, x, y), so this links nothing
// but `tiles.hpp` and raylib's image API. That is the same property that lets any node in a cluster
// compute terrain without asking anyone — here it just makes the tool trivial.
//
// `--village N` crops to one settlement instead. That was added the moment villages grew a
// rampart: a wall is 3 tiles thick and 50 tiles across, so at 1px per tile it is a smudge, and from
// inside the game the camera cannot see two opposite sides of it at once. A hole in a wall is
// exactly the kind of fault that is invisible everywhere except from directly above.
//
// Run:  build/mmo_worldmap [--seed N] [--scale N] [--out FILE] [--rings] [--village N]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "raylib.h"

#include "world/tiles.hpp"
#include "world/worldgen.hpp"

using namespace mmo;

namespace {

struct Rgb {
    unsigned char r, g, b;
};

// Chosen for legibility at 1px, not for beauty: neighbouring terrains must be tellable apart when
// each is a single pixel.
[[nodiscard]] Rgb colour_of(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return {104, 158, 74};
        case Terrain::kDirt: return {148, 106, 66};
        case Terrain::kWater: return {58, 100, 168};
        case Terrain::kStone: return {126, 126, 134};
        case Terrain::kSand: return {214, 190, 130};
        case Terrain::kTree: return {48, 104, 56};
        case Terrain::kSnow: return {228, 234, 242};
        case Terrain::kMarsh: return {86, 96, 62};
        case Terrain::kAsh: return {74, 66, 76};
        // Generated features are deliberately LOUD against the land. This map is read for
        // structure, and the first question asked of it is always "did the roads connect?".
        case Terrain::kPath: return {206, 158, 108};
        case Terrain::kBuilding: return {40, 30, 28};
        case Terrain::kCount: break;
    }
    return {255, 0, 255};
}

constexpr const char* kRingNames[kRingCount] = {"Meadow", "Forest", "Wetland", "Snow", "Wasteland"};

void put(std::vector<unsigned char>& px, int w, int x, int y, Rgb c, unsigned char a = 255) {
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    px[i] = c.r;
    px[i + 1] = c.g;
    px[i + 2] = c.b;
    px[i + 3] = a;
}

// Each stroke has to be clipped on BOTH axes, not just the one it runs along. That was latent for
// as long as this tool only ever drew the whole map — every marker was in range by construction —
// and it became an out-of-bounds write the moment `--village` made the image a window: the other
// forty-nine villages are still in the list, and their markers land a long way outside it.
void cross(std::vector<unsigned char>& px, int w, int h, int x, int y, int arm, Rgb c) {
    if (x < -arm || x >= w + arm || y < -arm || y >= h + arm) return;
    for (int d = -arm; d <= arm; ++d) {
        if (x + d >= 0 && x + d < w && y >= 0 && y < h) put(px, w, x + d, y, c);
        if (y + d >= 0 && y + d < h && x >= 0 && x < w) put(px, w, x, y + d, c);
    }
}

// A hollow rectangle in map-tile space, clipped to the image on both axes. Used to box a camp's
// footprint — a box rather than a cross because a camp is an AREA, and the one question the map
// answers about it is "does it overlap a road, a village or water?", which needs its extent shown.
void box(std::vector<unsigned char>& px, int w, int h, int x0, int y0, int bw, int bh, Rgb c) {
    for (int x = x0; x < x0 + bw; ++x) {
        if (x < 0 || x >= w) continue;
        if (y0 >= 0 && y0 < h) put(px, w, x, y0, c);
        if (y0 + bh - 1 >= 0 && y0 + bh - 1 < h) put(px, w, x, y0 + bh - 1, c);
    }
    for (int y = y0; y < y0 + bh; ++y) {
        if (y < 0 || y >= h) continue;
        if (x0 >= 0 && x0 < w) put(px, w, x0, y, c);
        if (x0 + bw - 1 >= 0 && x0 + bw - 1 < w) put(px, w, x0 + bw - 1, y, c);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 0x5EED'0BEEF'CAFEull;
    int scale = 1;  // output pixels per tile
    bool rings = false;
    int only_village = -1;
    std::string out = "worldmap.png";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (std::strcmp(argv[i], "--rings") == 0) {
            rings = true;
        } else if (std::strcmp(argv[i], "--village") == 0 && i + 1 < argc) {
            only_village = std::atoi(argv[++i]);
        } else {
            std::printf("usage: %s [--seed N] [--scale N] [--out FILE] [--rings] [--village N]\n",
                        argv[0]);
            return 2;
        }
    }
    if (scale < 1) scale = 1;

    // BEFORE the first terrain query. `world_layout` publishes the overlay that `terrain_of` reads,
    // and it caches on first call — so a tool that wants a non-default seed has to be the one to
    // make that call. Query a tile first and the map would show the land with nothing built on it,
    // which is a silent, plausible-looking wrong answer.
    const WorldLayout& layout = world_layout(seed);

    // The window. The whole map unless `--village` narrows it to one enclosure plus ten tiles of
    // country, which is enough to see whether the roads reached the gates.
    int ox = 0;
    int oy = 0;
    int tw = kMapTiles;
    int th = kMapTiles;
    if (only_village >= 0 && only_village < static_cast<int>(layout.villages().size())) {
        const Village& v = layout.villages()[static_cast<std::size_t>(only_village)];
        const VillagePlan vp = plan_of(v.tier);
        const int pad = 10;
        ox = std::max(0, v.tx - vp.hw - pad);
        oy = std::max(0, v.ty - vp.hh - pad);
        tw = std::min(kMapTiles - ox, 2 * (vp.hw + pad) + 1);
        th = std::min(kMapTiles - oy, 2 * (vp.hh + pad) + 1);
        if (scale == 1) scale = 6;
    }

    const int w = tw * scale;
    const int h = th * scale;
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4, 255);

    // --- terrain + a per-ring tally ------------------------------------------------------------
    long long ring_tiles[kRingCount] = {};
    long long terrain_tiles[static_cast<int>(Terrain::kCount)] = {};
    long long blocked = 0;

    for (int wy = 0; wy < th; ++wy) {
        for (int wx = 0; wx < tw; ++wx) {
            const int tx = ox + wx;
            const int ty = oy + wy;
            const Terrain t = terrain_of(seed, kOverworld, tx, ty);
            const Ring r = ring_of(seed, tx, ty);
            ++ring_tiles[static_cast<int>(r)];
            ++terrain_tiles[static_cast<int>(t)];
            if (!is_walkable(t)) ++blocked;

            Rgb c = colour_of(t);
            if (rings) {
                // Tint alternate rings very slightly so the bands are visible without hiding
                // the terrain underneath.
                if (static_cast<int>(r) % 2 == 1) {
                    c.r = static_cast<unsigned char>(c.r * 0.88f);
                    c.g = static_cast<unsigned char>(c.g * 0.88f);
                    c.b = static_cast<unsigned char>(c.b * 0.94f);
                }
            }
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    put(px, w, wx * scale + sx, wy * scale + sy, c);
                }
            }
        }
    }

    // --- markers -------------------------------------------------------------------------------
    // Villages white, strongholds red, the player's spawn a cyan cross. Three colours is enough to
    // answer every question this map exists for: are the villages spread out, do the strongholds
    // crowd them, and does the player wake up somewhere sensible.
    for (const Village& v : layout.villages()) {
        cross(px, w, h, (v.tx - ox) * scale, (v.ty - oy) * scale, (2 + v.tier) * scale,
              Rgb{255, 255, 255});
        // The four gates, in green, so "did the road find the hole?" is one glance rather than a
        // count of wall segments.
        const GateSet g = gates_of(v.tx, v.ty, v.tier);
        for (int i = 0; i < kGateCount; ++i) {
            cross(px, w, h, (g.x[i] - ox) * scale, (g.y[i] - oy) * scale, 2 * scale,
                  Rgb{90, 250, 120});
        }
    }
    for (const Stronghold& s : layout.strongholds()) {
        cross(px, w, h, (s.tx - ox) * scale, (s.ty - oy) * scale, 3 * scale, Rgb{240, 70, 70});
    }
    cross(px, w, h, (layout.spawn_tx() - ox) * scale, (layout.spawn_ty() - oy) * scale, 7 * scale,
          Rgb{90, 240, 240});
    // Placed prefabs in magenta, boxed to their footprint. Loud on purpose: the question asked of
    // them is whether each sits only in its own ring and clear of everything else.
    for (const PlacedPrefab& pp : layout.prefabs()) {
        const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
        box(px, w, h, (pp.tx - ox) * scale, (pp.ty - oy) * scale, def.w * scale, def.h * scale,
            Rgb{240, 60, 240});
    }

    Image img{};
    img.data = px.data();
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    if (!ExportImage(img, out.c_str())) {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }

    // --- report --------------------------------------------------------------------------------
    const double total = static_cast<double>(kMapTiles) * kMapTiles;
    std::printf("world map  seed=0x%llX  %dx%d tiles -> %s (%dx%d px)\n\n",
                static_cast<unsigned long long>(seed), kMapTiles, kMapTiles, out.c_str(), w, h);

    std::printf("rings\n");
    for (int i = 0; i < kRingCount; ++i) {
        std::printf("  %-10s %8lld  %5.1f%%\n", kRingNames[i], ring_tiles[i],
                    100.0 * static_cast<double>(ring_tiles[i]) / total);
    }
    static const char* tn[] = {"grass", "dirt", "water", "stone", "sand",  "tree",
                               "snow",  "marsh", "ash",  "path",  "buildg"};
    std::printf("\nterrain\n");
    for (int i = 0; i < static_cast<int>(Terrain::kCount); ++i) {
        std::printf("  %-6s %9lld  %5.1f%%%s\n", tn[i], terrain_tiles[i],
                    100.0 * static_cast<double>(terrain_tiles[i]) / total,
                    (i == static_cast<int>(Terrain::kWater) || i == static_cast<int>(Terrain::kTree))
                        ? "   <-- impassable"
                        : "");
    }
    std::printf("\nimpassable total: %.1f%%\n", 100.0 * static_cast<double>(blocked) / total);

    // --- what generation produced ----------------------------------------------------------------
    int by_ring[kRingCount] = {};
    int holds_by_ring[kRingCount] = {};
    for (const Village& v : layout.villages()) ++by_ring[static_cast<int>(v.ring)];
    for (const Stronghold& s : layout.strongholds()) ++holds_by_ring[static_cast<int>(s.ring)];

    // Split, because a single total stopped meaning anything once villages grew a wall: a rampart
    // is thirty-odd structures and a village has a dozen houses, so "4849 buildings" reads as ten
    // times the settlement this world actually has.
    std::size_t dwellings = 0;
    for (const Structure& s : layout.structures()) {
        if (is_dwelling(s.kind)) ++dwellings;
    }
    std::printf("\nsettlements: %zu villages, %zu strongholds, %zu buildings (%zu dwellings + %zu "
                "rampart pieces)\n",
                layout.villages().size(), layout.strongholds().size(), layout.structures().size(),
                dwellings, layout.structures().size() - dwellings);
    std::printf("  %-10s %8s %12s\n", "ring", "villages", "strongholds");
    for (int i = 0; i < kRingCount; ++i) {
        std::printf("  %-10s %8d %12d\n", kRingNames[i], by_ring[i], holds_by_ring[i]);
    }

    // Placed prefabs, tallied per TYPE and, within a type, per ring. Each landmark is placed in ONE
    // ring by construction (see kPoiTable), so a count outside a type's home ring is a bug in
    // `prefab_fits`, not a curiosity — which is why the tally is split per type AND per ring rather
    // than printed as a bare total, and why an off-ring line is flagged loudly.
    int poi_by_type[static_cast<int>(PrefabId::kCount)] = {};
    int poi_by_type_ring[static_cast<int>(PrefabId::kCount)][kRingCount] = {};
    int poi_by_type_skin[static_cast<int>(PrefabId::kCount)][8] = {};
    for (const PlacedPrefab& pp : layout.prefabs()) {
        const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
        const int rr = static_cast<int>(ring_of(seed, pp.tx + def.w / 2, pp.ty + def.h / 2));
        ++poi_by_type[static_cast<int>(pp.id)];
        ++poi_by_type_ring[static_cast<int>(pp.id)][rr];
        ++poi_by_type_skin[static_cast<int>(pp.id)][pp.skin];
    }
    // A type may now be a table row in MORE than one ring — a camp scatters green/autumn in the
    // forest AND, in its snow skin, in the snow ring — so its home rings are gathered from every
    // row bearing its id, not from one row. A count outside that set is still a `prefab_fits` bug.
    bool ring_ok[static_cast<int>(PrefabId::kCount)][kRingCount] = {};
    for (const PoiPlacement& row : kPoiTable) {
        ring_ok[static_cast<int>(row.id)][static_cast<int>(row.ring)] = true;
    }
    static const char* kSkinNames[] = {"base", "autumn", "snow"};
    std::printf("\nplaced prefabs: %zu total\n", layout.prefabs().size());
    // Split per type, then per ring AND per skin — the point of the skins is that a camp's total is
    // now two populations (a forest one wearing base/autumn, a snow-ring one wearing snow), and a
    // bare total would hide whether either landed in sane numbers. A type printed once, even if it
    // owns two rows.
    bool printed[static_cast<int>(PrefabId::kCount)] = {};
    for (const PoiPlacement& row : kPoiTable) {
        const int ti = static_cast<int>(row.id);
        if (printed[ti]) continue;
        printed[ti] = true;
        std::printf("  %-16s %4d\n", kPrefabs[ti].name, poi_by_type[ti]);
        for (int i = 0; i < kRingCount; ++i) {
            if (poi_by_type_ring[ti][i] == 0) continue;
            std::printf("      %-10s %4d%s\n", kRingNames[i], poi_by_type_ring[ti][i],
                        ring_ok[ti][i] ? "" : "   <-- OUT OF RING (bug)");
        }
        for (int s = 0; s < 8; ++s) {
            if (poi_by_type_skin[ti][s] == 0) continue;
            std::printf("      skin %-8s %4d\n", s < 3 ? kSkinNames[s] : "?", poi_by_type_skin[ti][s]);
        }
    }
    // The parcels a village lays as furniture, tallied apart from the scattered landmarks — they are
    // in `prefabs()` too (one list, one draw path), but they are placed by the village builder against
    // its plan, not by `kPoiTable`, so they belong under their own heading. A type is village-owned
    // when it is not a row of the placement table (`poi_gap` is zero for exactly those).
    std::printf("\nvillage parcels (laid by the builder, not scattered):\n");
    for (int ti = 0; ti < static_cast<int>(PrefabId::kCount); ++ti) {
        if (poi_by_type[ti] == 0 || poi_gap(static_cast<PrefabId>(ti)) != 0) continue;
        std::printf("  %-18s %4d   village-laid\n", kPrefabs[ti].name, poi_by_type[ti]);
    }

    // One `--shot ... --at TX TY` sample per type, aimed at the parcel's centre-x and a few tiles
    // SOUTH of its centre — so the player stands near the parcel's lower edge, which is also the
    // cross-chunk case (a tall parcel straddles the chunk border below the player) worth eyeballing.
    // One sample PER ROW, not per type: a camp's forest row and its snow row want separate --at
    // points (one to eyeball an autumn camp, one a snow camp), so the sample is matched to the row's
    // ring and reports the skin the instance drew.
    std::printf("\nsample --at per row (mmo_client --shot 2 <name>.png --at TX TY):\n");
    for (const PoiPlacement& row : kPoiTable) {
        for (const PlacedPrefab& pp : layout.prefabs()) {
            if (pp.id != row.id) continue;
            const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
            if (static_cast<int>(ring_of(seed, pp.tx + def.w / 2, pp.ty + def.h / 2)) !=
                static_cast<int>(row.ring)) {
                continue;
            }
            std::printf("  %-16s %-6s %-6s --at %d %d\n", def.name, kRingNames[static_cast<int>(row.ring)],
                        pp.skin < 3 ? kSkinNames[pp.skin] : "?", pp.tx + def.w / 2,
                        pp.ty + def.h / 2 + 5);
            break;
        }
    }
    if (const Village* home = layout.nearest_village(layout.spawn_tx(), layout.spawn_ty())) {
        const double d = std::sqrt(
            static_cast<double>((home->tx - layout.spawn_tx()) * (home->tx - layout.spawn_tx()) +
                                (home->ty - layout.spawn_ty()) * (home->ty - layout.spawn_ty())));
        std::printf("\nspawn (%d,%d) -> nearest village #%zu (%u,%u) tier %u, %.0f tiles away\n",
                    layout.spawn_tx(), layout.spawn_ty(),
                    static_cast<std::size_t>(home - layout.villages().data()), home->tx, home->ty,
                    home->tier, d);
    }
    // Village indices run in generation (scan) order, which is spatial — so #0 is a corner of the
    // map, not a typical village. Printing one index per ring gives `mmo_client --village N`
    // something meaningful to point at.
    std::printf("\none village per ring, for --village N:\n");
    for (int i = 0; i < kRingCount; ++i) {
        for (std::size_t j = 0; j < layout.villages().size(); ++j) {
            const Village& v = layout.villages()[j];
            if (static_cast<int>(v.ring) != i) continue;
            std::printf("  %-10s #%-3zu at (%4u,%4u) tier %u, %u buildings\n", kRingNames[i], j,
                        v.tx, v.ty, v.tier, v.count);
            break;
        }
    }
    return 0;
}
