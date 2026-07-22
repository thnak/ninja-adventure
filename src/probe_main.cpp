// Diagnostic probe — answers two questions the headless runner cannot:
//
//   1. WHAT DOES THE TERRAIN LOOK LIKE?  A histogram plus an ASCII map of the home map, read from
//      the chunks' own published snapshots (not recomputed here, so it reports what the game
//      actually generated). Terrain shape decides whether a wave can reach the farm at all.
//   2. ARE MOBS ACTUALLY MOVING?  Traces one mob tick by tick with a full per-chunk barrier after
//      every step, removing the sampling ambiguity of the headless runner.
//
// Not part of the game. Run: taskset -c 0-3 build/mmo_probe
#include <cstdio>

#include "world/world.hpp"

using namespace mmo;

namespace {

// Wildlife is everywhere now, so "a chunk with creatures in it" is no longer the same thing as "a
// chunk a raid landed in". The trace wants a monster: an animal wandering around its home tile
// would produce a beautiful graph of it going nowhere.
[[nodiscard]] bool is_monster(const Creature& c) {
    return stats_of(c.kind).faction == Faction::kMonster;
}

[[nodiscard]] std::uint32_t monsters_in(const ChunkView& v) {
    std::uint32_t n = 0;
    for (const Creature& c : v.creatures) {
        if (is_monster(c)) ++n;
    }
    return n;
}

[[nodiscard]] const Creature* first_monster(const ChunkView& v) {
    for (const Creature& c : v.creatures) {
        if (is_monster(c)) return &c;
    }
    return nullptr;
}

char glyph_of(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return '.';
        case Terrain::kDirt: return '=';
        case Terrain::kWater: return '~';
        case Terrain::kStone: return '#';
        case Terrain::kSand: return ':';
        case Terrain::kTree: return 'T';
        case Terrain::kSnow: return '*';
        case Terrain::kMarsh: return ',';
        case Terrain::kAsh: return '%';
        case Terrain::kPath: return '-';
        case Terrain::kBuilding: return 'H';
        case Terrain::kCount: break;
    }
    return '?';
}

// Reads terrain out of the published chunk views for one map, into a flat kMapTiles^2 buffer.
std::vector<Terrain> read_terrain(World& world, std::uint16_t map) {
    std::vector<Terrain> out(static_cast<std::size_t>(kMapTiles) * kMapTiles, Terrain::kGrass);
    for (int cy = 0; cy < kMapChunks; ++cy) {
        for (int cx = 0; cx < kMapChunks; ++cx) {
            ChunkViewPtr v = world.bus().load(
                ChunkCoord{map, static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy)});
            if (!v) continue;
            for (int ly = 0; ly < kChunkTiles; ++ly) {
                for (int lx = 0; lx < kChunkTiles; ++lx) {
                    const int gx = cx * kChunkTiles + lx;
                    const int gy = cy * kChunkTiles + ly;
                    out[static_cast<std::size_t>(gy) * kMapTiles + gx] =
                        static_cast<Terrain>(v->terrain[ly * kChunkTiles + lx]);
                }
            }
        }
    }
    return out;
}

// SIZED BY `Terrain::kCount`, not by a literal. It used to be `int hist[6]` indexed by the terrain
// enumerator, which was correct when there were six terrains and became a stack-smashing
// out-of-bounds write the moment P1 added snow, marsh, ash, path and building. It did not crash for
// a while, which is exactly what makes this class of bug worth a comment rather than a silent fix:
// the array is now derived from the enum, so the next terrain added cannot reintroduce it.
constexpr int kTerrainCount = static_cast<int>(Terrain::kCount);
static const char* kTerrainNames[kTerrainCount] = {"grass", "dirt",  "water", "stone",   "sand",
                                                   "tree",  "snow",  "marsh", "ash",     "path",
                                                   "buildg"};

void report_terrain(const std::vector<Terrain>& t) {
    int hist[kTerrainCount] = {};
    for (Terrain x : t) ++hist[static_cast<int>(x)];
    const auto total = static_cast<double>(t.size());
    std::printf("terrain mix (overworld, %d tiles):\n", kMapTiles * kMapTiles);
    int blocked_tiles = 0;
    for (int i = 0; i < kTerrainCount; ++i) {
        const bool blocked = !is_walkable(static_cast<Terrain>(i));
        if (blocked) blocked_tiles += hist[i];
        std::printf("  %-6s %7d  %5.1f%%%s\n", kTerrainNames[i], hist[i], 100.0 * hist[i] / total,
                    blocked ? "   <-- IMPASSABLE" : "");
    }
    std::printf("  impassable total: %.1f%%\n\n", 100.0 * blocked_tiles / total);
}

// 4x4 tiles per character. A cell shows the most "notable" terrain in it (impassable wins) so the
// map reads as obstacles rather than as an averaged blur.
void print_map(const std::vector<Terrain>& t) {
    constexpr int kStep = 4;
    std::printf("overworld (1 char = %dx%d tiles; ~water Ttree #stone :sand *snow ,marsh %%ash "
                "-road Hhouse =farm .grass)\n",
                kStep, kStep);
    for (int by = 0; by < kMapTiles; by += kStep) {
        std::printf("  ");
        for (int bx = 0; bx < kMapTiles; bx += kStep) {
            int count[kTerrainCount] = {};
            for (int y = by; y < by + kStep; ++y) {
                for (int x = bx; x < bx + kStep; ++x) {
                    ++count[static_cast<int>(t[static_cast<std::size_t>(y) * kMapTiles + x])];
                }
            }
            // An OBSTACLE wins the cell if it fills half of it, otherwise the most common terrain
            // does. The earlier version tested five named terrains in a fixed order, so the five
            // added since (snow, marsh, ash, path, building) were invisible on the map — the whole
            // northern half of the world printed as grass.
            const int cells = kStep * kStep;
            Terrain pick = Terrain::kGrass;
            int best = -1;
            for (int i = 0; i < kTerrainCount; ++i) {
                const auto ter = static_cast<Terrain>(i);
                if (!is_walkable(ter) && count[i] * 2 >= cells) {
                    pick = ter;
                    best = cells * 2;  // an obstacle outranks anything walkable
                    continue;
                }
                if (count[i] > best) {
                    best = count[i];
                    pick = ter;
                }
            }
            std::putchar(glyph_of(pick));
        }
        std::putchar('\n');
    }
    std::putchar('\n');
}

// Flood fill from a VILLAGE over walkable tiles: what fraction of the map can actually reach one?
// A raid that spawns in an unreachable pocket never arrives, and the run silently looks calm.
//
// The start used to be the map centre, which was where the core stood. There is no core any more,
// and the centre tile is quite likely to be inside a house now — a flood fill starting on solid
// ground reports 0% reachable and looks like a catastrophic worldgen bug rather than a stale
// constant.
void report_reachability(const std::vector<Terrain>& t, int start_x, int start_y) {
    std::vector<char> seen(t.size(), 0);
    std::vector<int> stack;
    const int start = start_y * kMapTiles + start_x;
    if (!is_walkable(t[static_cast<std::size_t>(start)])) {
        std::printf("reachability: start tile (%d,%d) is not walkable — skipped\n\n", start_x,
                    start_y);
        return;
    }
    stack.push_back(start);
    seen[static_cast<std::size_t>(start)] = 1;
    int reached = 0;
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        ++reached;
        const int x = p % kMapTiles;
        const int y = p / kMapTiles;
        const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
        for (const auto& n : nb) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= kMapTiles || n[1] >= kMapTiles) continue;
            const int q = n[1] * kMapTiles + n[0];
            if (seen[static_cast<std::size_t>(q)]) continue;
            if (!is_walkable(t[static_cast<std::size_t>(q)])) continue;
            seen[static_cast<std::size_t>(q)] = 1;
            stack.push_back(q);
        }
    }
    int walkable = 0;
    for (Terrain x : t) walkable += is_walkable(x) ? 1 : 0;

    // How much of the rim (where waves spawn) can reach the core?
    int rim = 0;
    int rim_ok = 0;
    for (int i = 0; i < kMapTiles; ++i) {
        const int edges[4] = {i, (kMapTiles - 1) * kMapTiles + i, i * kMapTiles,
                              i * kMapTiles + kMapTiles - 1};
        for (int e : edges) {
            if (!is_walkable(t[static_cast<std::size_t>(e)])) continue;
            ++rim;
            rim_ok += seen[static_cast<std::size_t>(e)] ? 1 : 0;
        }
    }
    std::printf("reachability from village (%d,%d):\n", start_x, start_y);
    std::printf("  walkable tiles connected to it:       %d / %d  (%.1f%%)\n", reached, walkable,
                100.0 * reached / (walkable > 0 ? walkable : 1));
    std::printf("  walkable RIM tiles that can reach it: %d / %d  (%.1f%%)  <-- raids come from out here\n\n",
                rim_ok, rim, 100.0 * rim_ok / (rim > 0 ? rim : 1));
}

}  // namespace

int main() {
    World world;
    world.build(4);
    world.start();

    const auto home = kOverworld;

    // One tick is enough to make every chunk publish its terrain.
    world.step(kTickMs);
    world.sync_world();

    const std::vector<Terrain> terrain = read_terrain(world, home);
    report_terrain(terrain);
    print_map(terrain);
    // Start from a real village square rather than from the middle of the map.
    const Village* seed_village =
        world.layout().villages().empty() ? nullptr : &world.layout().villages().front();
    report_reachability(terrain, seed_village ? seed_village->tx : kHomeTx,
                        seed_village ? seed_village->ty : kHomeTy);

    // --- mob trace ---------------------------------------------------------------------------
    const int to_night = static_cast<int>(kDayMs / kTickMs) + 5;
    for (int i = 0; i < to_night; ++i) {
        world.step(kTickMs);
        world.sync_world();
    }
    // A raid spawns AT nightfall, and a chunk with nobody near it republishes only every
    // `ChunkActor::kIdlePublish` ticks (the interest set — see chunk_actor.hpp). So a tool that
    // reads published views has to wait one full LOD period after an event before it can see it,
    // or it reports an empty world. This cost 40 ticks and one confusing "raid creatures: 0".
    for (int i = 0; i < 40; ++i) {
        world.step(kTickMs);
        world.sync_world();
    }
    std::printf("after nightfall: wave=%u\n", world.status().wave.load());

    ChunkCoord rim{home, 0, 0};
    bool picked = false;
    std::uint32_t total = 0;
    for (int cy = 0; cy < kMapChunks; ++cy) {
        for (int cx = 0; cx < kMapChunks; ++cx) {
            ChunkViewPtr cv = world.bus().load(
                ChunkCoord{home, static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy)});
            const auto n = cv ? monsters_in(*cv) : 0u;
            total += n;
            if (n > 0 && !picked) {
                rim = ChunkCoord{home, static_cast<std::uint16_t>(cx),
                                 static_cast<std::uint16_t>(cy)};
                picked = true;
            }
        }
    }
    std::printf("raid creatures: %u   tracing chunk (%u,%u)\n", total, rim.cx, rim.cy);

    ChunkViewPtr v = world.bus().load(rim);
    const Creature* pick = v ? first_monster(*v) : nullptr;
    if (pick != nullptr) {
        const Creature m0 = *pick;
        // A monster walks to the nearest VILLAGE — there is no core any more, and the flow field
        // targets settlements. Tracing against the map centre used to be right and now just
        // measures the wrong thing.
        const Village* dest = world.layout().nearest_village(static_cast<int>(m0.x),
                                                             static_cast<int>(m0.y));
        const float goal_x = dest ? static_cast<float>(dest->tx) : static_cast<float>(kHomeTx);
        const float goal_y = dest ? static_cast<float>(dest->ty) : static_cast<float>(kHomeTy);
        std::printf("traced creature %u start (%.2f,%.2f)  target village (%.0f,%.0f)\n", m0.id,
                    static_cast<double>(m0.x), static_cast<double>(m0.y),
                    static_cast<double>(goal_x), static_cast<double>(goal_y));
        float last_x = m0.x;
        float last_y = m0.y;
        float travelled = 0.0f;
        // Sample on the LOD boundary, not on a round number. A chunk with no player near it
        // republishes every `kIdlePublish` (32) ticks, so sampling every 25 gave a trace where
        // every third row read "moved 0.00" — which looks exactly like a creature stuck on terrain
        // and is nothing of the sort.
        constexpr int kSample = 32;
        for (int step = 1; step <= 12; ++step) {
            for (int i = 0; i < kSample; ++i) {
                world.step(kTickMs);
                world.sync_world();
            }
            // The mob may have migrated; find it anywhere on the map.
            bool found = false;
            for (int i = 0; i < kChunksPerMap && !found; ++i) {
                ChunkViewPtr cv = world.bus().load_index(home * kChunksPerMap + i);
                if (!cv) continue;
                for (const Creature& m : cv->creatures) {
                    if (m.id != m0.id) continue;
                    const float d = std::sqrt((m.x - last_x) * (m.x - last_x) +
                                              (m.y - last_y) * (m.y - last_y));
                    travelled += d;
                    last_x = m.x;
                    last_y = m.y;
                    const float to_goal = std::sqrt((m.x - goal_x) * (m.x - goal_x) +
                                                    (m.y - goal_y) * (m.y - goal_y));
                    std::printf("  +%3d ticks: pos=(%6.2f,%6.2f)  moved %5.2f  dist to village %6.2f\n",
                                step * kSample, static_cast<double>(m.x), static_cast<double>(m.y),
                                static_cast<double>(d), static_cast<double>(to_goal));
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::printf("  +%3d ticks: it died\n", step * kSample);
                break;
            }
        }
        std::printf("total distance travelled: %.1f tiles\n", static_cast<double>(travelled));
    } else {
        std::printf("no raid creatures found to trace\n");
    }

    world.stop();
    return 0;
}
