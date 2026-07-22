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

char glyph_of(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return '.';
        case Terrain::kDirt: return '=';
        case Terrain::kWater: return '~';
        case Terrain::kStone: return '#';
        case Terrain::kSand: return ':';
        case Terrain::kTree: return 'T';
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

void report_terrain(const std::vector<Terrain>& t) {
    int hist[6] = {};
    for (Terrain x : t) ++hist[static_cast<int>(x)];
    const auto total = static_cast<double>(t.size());
    static const char* names[] = {"grass", "dirt", "water", "stone", "sand", "tree"};
    std::printf("terrain mix (home map, %d tiles):\n", kMapTiles * kMapTiles);
    for (int i = 0; i < 6; ++i) {
        std::printf("  %-6s %6d  %5.1f%%%s\n", names[i], hist[i],
                    100.0 * hist[i] / total,
                    (i == 2 || i == 5) ? "   <-- IMPASSABLE" : "");
    }
    const double blocked = 100.0 * (hist[2] + hist[5]) / total;
    std::printf("  impassable total: %.1f%%\n\n", blocked);
}

// 4x4 tiles per character. A cell shows the most "notable" terrain in it (impassable wins) so the
// map reads as obstacles rather than as an averaged blur.
void print_map(const std::vector<Terrain>& t) {
    constexpr int kStep = 4;
    std::printf("home map (1 char = %dx%d tiles; ~ water  T tree  # stone  : sand  = farm  . grass)\n",
                kStep, kStep);
    for (int by = 0; by < kMapTiles; by += kStep) {
        std::printf("  ");
        for (int bx = 0; bx < kMapTiles; bx += kStep) {
            int count[6] = {};
            for (int y = by; y < by + kStep; ++y) {
                for (int x = bx; x < bx + kStep; ++x) {
                    ++count[static_cast<int>(t[static_cast<std::size_t>(y) * kMapTiles + x])];
                }
            }
            const int cells = kStep * kStep;
            Terrain pick = Terrain::kGrass;
            if (count[static_cast<int>(Terrain::kWater)] * 2 >= cells) {
                pick = Terrain::kWater;
            } else if (count[static_cast<int>(Terrain::kTree)] * 2 >= cells) {
                pick = Terrain::kTree;
            } else if (count[static_cast<int>(Terrain::kStone)] * 2 >= cells) {
                pick = Terrain::kStone;
            } else if (count[static_cast<int>(Terrain::kDirt)] * 2 >= cells) {
                pick = Terrain::kDirt;
            } else if (count[static_cast<int>(Terrain::kSand)] * 2 >= cells) {
                pick = Terrain::kSand;
            }
            std::putchar(glyph_of(pick));
        }
        std::putchar('\n');
    }
    std::putchar('\n');
}

// Flood fill from the core over walkable tiles: what fraction of the map can actually reach the
// farm? A wave that spawns in an unreachable pocket never arrives, and the run silently looks calm.
void report_reachability(const std::vector<Terrain>& t) {
    std::vector<char> seen(t.size(), 0);
    std::vector<int> stack;
    const int start = kHomeTy * kMapTiles + kHomeTx;
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
    std::printf("reachability from the core:\n");
    std::printf("  walkable tiles connected to the core: %d / %d  (%.1f%%)\n", reached, walkable,
                100.0 * reached / (walkable > 0 ? walkable : 1));
    std::printf("  walkable RIM tiles that can reach it: %d / %d  (%.1f%%)  <-- waves spawn here\n\n",
                rim_ok, rim, 100.0 * rim_ok / (rim > 0 ? rim : 1));
}

}  // namespace

int main() {
    World world;
    world.build(4);
    world.start();

    const auto home = static_cast<std::uint16_t>(MapId::kHomeValley);

    // One tick is enough to make every chunk publish its terrain.
    world.step(kTickMs);
    world.sync_world();

    const std::vector<Terrain> terrain = read_terrain(world, home);
    report_terrain(terrain);
    print_map(terrain);
    report_reachability(terrain);

    // --- mob trace ---------------------------------------------------------------------------
    const int to_night = static_cast<int>(kDayMs / kTickMs) + 5;
    for (int i = 0; i < to_night; ++i) {
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
            const auto n = cv ? static_cast<std::uint32_t>(cv->mobs.size()) : 0u;
            total += n;
            if (n > 0 && !picked) {
                rim = ChunkCoord{home, static_cast<std::uint16_t>(cx),
                                 static_cast<std::uint16_t>(cy)};
                picked = true;
            }
        }
    }
    std::printf("mobs spawned: %u   tracing chunk (%u,%u)\n", total, rim.cx, rim.cy);

    ChunkViewPtr v = world.bus().load(rim);
    if (v && !v->mobs.empty()) {
        const Mob m0 = v->mobs[0];
        std::printf("traced mob %u start (%.2f,%.2f)  target core (%d,%d)\n", m0.id,
                    static_cast<double>(m0.x), static_cast<double>(m0.y), kHomeTx, kHomeTy);
        float last_x = m0.x;
        float last_y = m0.y;
        float travelled = 0.0f;
        for (int step = 1; step <= 12; ++step) {
            for (int i = 0; i < 25; ++i) {
                world.step(kTickMs);
                world.sync_world();
            }
            // The mob may have migrated; find it anywhere on the map.
            bool found = false;
            for (int i = 0; i < kChunksPerMap && !found; ++i) {
                ChunkViewPtr cv = world.bus().load_index(home * kChunksPerMap + i);
                if (!cv) continue;
                for (const Mob& m : cv->mobs) {
                    if (m.id != m0.id) continue;
                    const float d = std::sqrt((m.x - last_x) * (m.x - last_x) +
                                              (m.y - last_y) * (m.y - last_y));
                    travelled += d;
                    last_x = m.x;
                    last_y = m.y;
                    const float to_core = std::sqrt((m.x - kHomeTx) * (m.x - kHomeTx) +
                                                    (m.y - kHomeTy) * (m.y - kHomeTy));
                    std::printf("  +%3d ticks: pos=(%6.2f,%6.2f)  moved %5.2f  dist to core %6.2f\n",
                                step * 25, static_cast<double>(m.x), static_cast<double>(m.y),
                                static_cast<double>(d), static_cast<double>(to_core));
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::printf("  +%3d ticks: mob died\n", step * 25);
                break;
            }
        }
        std::printf("total distance travelled: %.1f tiles\n", static_cast<double>(travelled));
    } else {
        std::printf("no mobs found to trace\n");
    }

    world.stop();
    return 0;
}
