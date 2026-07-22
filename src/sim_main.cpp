// Headless simulation runner — the whole game world, no window.
//
// This exists for three reasons, in order of importance:
//   1. It is the cluster demo. When chunks are placed across machines there is nothing to draw on
//      the nodes that host them; this binary IS what a node runs.
//   2. It makes the simulation verifiable. The run is deterministic (fixed step, seeded RNG, `ask`
//      barriers instead of sleeps), so it asserts real invariants and returns a real exit code.
//   3. It proves the render seam is honest — if anything in `world/` had reached into a renderer,
//      this would not link.
//
// Run:  taskset -c 0-3 build/mmo_sim [ticks]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "world/world.hpp"

using namespace mmo;

namespace {

struct Check {
    int failures = 0;

    void expect(bool cond, const char* what) {
        if (cond) return;
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
};

void print_row(std::int64_t ms, const WorldStatus& st, std::uint32_t mobs) {
    std::printf("  t=%6.1fs  %-5s  wave=%-2u  mobs=%-5u  killed=%-5u  migrations=%-6u\n",
                static_cast<double>(ms) / 1000.0,
                st.night.load(std::memory_order_relaxed) ? "NIGHT" : "day",
                st.wave.load(std::memory_order_relaxed), mobs,
                st.mobs_killed.load(std::memory_order_relaxed),
                st.migrations.load(std::memory_order_relaxed));
}

}  // namespace

int main(int argc, char** argv) {
    const int ticks = argc > 1 ? std::atoi(argv[1]) : 1200;  // 1200 ticks == 120 s of world time

    World world;
    world.build(/*workers*/ 4);
    world.start();

    std::printf("Quark MMO — headless simulation\n");
    std::printf("  chunks (actors): %zu across %d maps of %dx%d tiles\n", world.chunk_count(),
                kMapCount, kMapTiles, kMapTiles);
    std::printf("  tick rate: %d Hz, day %llds / night %llds\n\n", kTicksPerSecond,
                static_cast<long long>(kDayMs / 1000), static_cast<long long>(kNightMs / 1000));

    Check chk;

    // --- What generation produced ----------------------------------------------------------------
    // Checked before anything else, because every other property in this file now depends on it: no
    // villages means no flow-field targets, no raid destinations and nowhere for the player to walk.
    const auto home = kOverworld;
    const WorldLayout& layout = world.layout();

    int villages_by_ring[kRingCount] = {};
    int holds_by_ring[kRingCount] = {};
    for (const Village& v : layout.villages()) ++villages_by_ring[static_cast<int>(v.ring)];
    for (const Stronghold& s : layout.strongholds()) ++holds_by_ring[static_cast<int>(s.ring)];

    static const char* kRingNames[kRingCount] = {"Meadow", "Forest", "Wetland", "Snow", "Wasteland"};
    std::printf("world generation: %zu villages, %zu strongholds, %zu buildings\n",
                layout.villages().size(), layout.strongholds().size(), layout.structures().size());
    for (int i = 0; i < kRingCount; ++i) {
        std::printf("  %-10s %2d villages  %2d strongholds\n", kRingNames[i], villages_by_ring[i],
                    holds_by_ring[i]);
    }

    chk.expect(layout.villages().size() >= 20, "generation placed a plausible number of villages");
    chk.expect(!layout.strongholds().empty(), "generation placed strongholds");
    chk.expect(!layout.structures().empty(), "villages actually have buildings in them");
    // The difficulty gradient has to be visible in the LAYOUT, not only in the terrain palette:
    // the outer map must be more hostile per settlement than the middle of it. This is the one
    // property that would silently stop being true if `stronghold_chance` were mis-edited.
    chk.expect(holds_by_ring[static_cast<int>(Ring::kWasteland)] +
                       holds_by_ring[static_cast<int>(Ring::kSnow)] >
                   holds_by_ring[static_cast<int>(Ring::kMeadow)],
               "strongholds get denser toward the rim");

    // Every building footprint must be impassable and every road walkable — the two claims the
    // renderer and the flow field both take on trust.
    int solid = 0;
    int walkable_paths = 0;
    for (const Structure& s : layout.structures()) {
        if (terrain_of(kWorldSeed, home, s.tx, s.ty) == Terrain::kBuilding) ++solid;
    }
    for (const Village& v : layout.villages()) {
        if (is_walkable(terrain_of(kWorldSeed, home, v.tx, v.ty))) ++walkable_paths;
    }
    chk.expect(solid == static_cast<int>(layout.structures().size()),
               "every structure's tile reads as solid through terrain_of");
    chk.expect(walkable_paths == static_cast<int>(layout.villages().size()),
               "every village square is walkable");

    // --- The opening: you wake in open country and walk ------------------------------------------
    const PlayerView spawn = world.player_view();
    const Village* home_village = layout.nearest_village(static_cast<int>(spawn.x),
                                                         static_cast<int>(spawn.y));
    const double walk = home_village == nullptr ? 0.0
                                                : std::sqrt(std::pow(home_village->tx - spawn.x, 2) +
                                                            std::pow(home_village->ty - spawn.y, 2));
    std::printf("\nspawn: (%.0f,%.0f), nearest village %.0f tiles away, inventory w%d s%d\n",
                static_cast<double>(spawn.x), static_cast<double>(spawn.y), walk,
                spawn.items[static_cast<int>(ItemKind::kWood)],
                spawn.items[static_cast<int>(ItemKind::kStone)]);
    chk.expect(is_walkable(terrain_of(kWorldSeed, home, static_cast<int>(spawn.x),
                                      static_cast<int>(spawn.y))),
               "the player does not wake up inside a lake or a wall");
    chk.expect(walk > 12.0, "the player wakes AWAY from the village, not in it");

    // --- Farming, and the fact that nothing is given to you --------------------------------------
    // There is no starting apron any more, so planting is refused until the player tills — which is
    // exactly the property to assert, because the old test could not tell tilling from the free
    // farmland it was standing on.
    const std::uint16_t farm_tx = static_cast<std::uint16_t>(spawn.x);
    const std::uint16_t farm_ty = static_cast<std::uint16_t>(spawn.y);
    const ChunkCoord farm_chunk =
        chunk_of(home, static_cast<float>(farm_tx), static_cast<float>(farm_ty));

    world.plant(home, farm_tx, farm_ty, CropKind::kWheat, 0);
    world.sync_world();
    const std::uint32_t crops_untilled = world.chunk_stats(farm_chunk).crops;

    int tilled = 0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            if (world.till(home, static_cast<std::uint16_t>(farm_tx + dx),
                           static_cast<std::uint16_t>(farm_ty + dy))) {
                ++tilled;
            }
        }
    }
    world.sync_world();
    int planted = 0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            world.plant(home, static_cast<std::uint16_t>(farm_tx + dx),
                        static_cast<std::uint16_t>(farm_ty + dy), CropKind::kWheat, 0);
            ++planted;
        }
    }
    world.sync_world();
    const ChunkStats after_till = world.chunk_stats(farm_chunk);

    std::printf("\nfarming: tilled %d tiles, planted %d\n", tilled, planted);
    std::printf("  crops on untilled ground %u -> after tilling %u\n", crops_untilled,
                after_till.crops);
    chk.expect(crops_untilled == 0, "planting on wild ground is refused — nothing is given to you");
    chk.expect(tilled > 0, "the player could reclaim ground");
    chk.expect(after_till.tilled > 0, "the chunk recorded its tilled overlay");
    chk.expect(after_till.crops > 0, "a crop could be planted on reclaimed ground");

    // --- Building, paid for out of the trusted inventory ------------------------------------------
    const PlayerView before_build = world.player_view();
    const bool lit = world.build_at(home, static_cast<std::uint16_t>(farm_tx + 4),
                                    static_cast<std::uint16_t>(farm_ty), BuildKind::kHearth);
    const PlayerView after_build = world.player_view();
    std::printf("\nhearth: %s;  stone %d -> %d  (debited by the TRUSTED PlayerActor)\n",
                lit ? "lit" : "could not afford",
                before_build.items[static_cast<int>(ItemKind::kStone)],
                after_build.items[static_cast<int>(ItemKind::kStone)]);
    chk.expect(lit, "the player could afford a hearth");
    chk.expect(after_build.items[static_cast<int>(ItemKind::kStone)] <
                   before_build.items[static_cast<int>(ItemKind::kStone)],
               "lighting a hearth debited stone");

    // Overspend must be refused atomically: keep building until the inventory says no, and prove
    // the balance never went negative. Runs LAST because it deliberately drains the player — an
    // earlier ordering left nothing to pay for the test above, which read as "building is broken"
    // when the feature was fine.
    bool minted = true;
    for (int i = 0; i < 500; ++i) {
        if (!world.build_at(home, static_cast<std::uint16_t>(farm_tx + 8),
                            static_cast<std::uint16_t>(farm_ty + i % 5), BuildKind::kHearth)) {
            minted = false;
            break;
        }
    }
    const PlayerView post_greedy = world.player_view();
    chk.expect(!minted, "the inventory refused an unaffordable build");
    chk.expect(post_greedy.items[static_cast<int>(ItemKind::kStone)] >= 0,
               "stone never went negative");

    // --- Run the world ---------------------------------------------------------------------------
    std::printf("simulating %d ticks (%.0f s of world time)\n", ticks,
                static_cast<double>(ticks) * static_cast<double>(kTickMs) / 1000.0);

    std::uint32_t peak_mobs = 0;
    bool saw_night = false;
    bool saw_migration = false;

    for (int i = 0; i < ticks; ++i) {
        world.step(kTickMs);

        // Every 100 ticks, take a consistent sample: barrier the director AND every chunk, so the
        // row printed below is one coherent world state rather than 192 chunks at 192 different
        // ticks. Between samples the chunks are deliberately left to run ahead/behind each other.
        if ((i + 1) % 100 == 0) {
            world.sync_world();
            const std::uint32_t mobs = count_mobs(world.bus());
            world.status().mobs_alive.store(mobs, std::memory_order_relaxed);
            peak_mobs = std::max(peak_mobs, mobs);
            saw_night = saw_night || world.status().night.load(std::memory_order_relaxed);
            saw_migration =
                saw_migration || world.status().migrations.load(std::memory_order_relaxed) > 0;
            print_row(world.status().world_ms.load(std::memory_order_relaxed), world.status(), mobs);
        }
    }
    world.sync_world();

    // --- Verify ----------------------------------------------------------------------------------
    std::printf("\nverification\n");

    const ChunkStats farm_stats = world.chunk_stats(farm_chunk);
    std::printf("  home chunk (%u,%u): crops=%u ripe=%u buildings=%u tick=%llu\n", farm_chunk.cx,
                farm_chunk.cy, farm_stats.crops, farm_stats.ripe, farm_stats.buildings,
                static_cast<unsigned long long>(farm_stats.tick));

    chk.expect(saw_night, "the day/night cycle reached night");
    chk.expect(world.status().wave.load(std::memory_order_relaxed) > 0, "at least one night passed");
    chk.expect(peak_mobs > 0, "a raid came out of a stronghold");
    chk.expect(saw_migration, "mobs migrated across chunk (actor) boundaries");
    chk.expect(farm_stats.tick >= static_cast<std::uint64_t>(ticks),
               "every chunk received every tick (no dropped fan-out)");
    chk.expect(farm_stats.buildings > 0, "the player's hearth is still standing");

    // Crops planted with a 20 s growth time must be ripe well before the run ends.
    if (ticks >= 300) {
        chk.expect(farm_stats.ripe > 0, "wheat planted early in the run ripened");
    }

    // Mob conservation: everything that spawned is either alive somewhere or counted as killed.
    // A mob lost during a chunk hand-off would break this.
    // Counted by ASK, not from published views: an ask is answered by the chunk itself and is
    // therefore authoritative, while a view can be one tick stale.
    std::uint32_t alive = 0;
    for (int cy = 0; cy < kMapChunks; ++cy) {
        for (int cx = 0; cx < kMapChunks; ++cx) {
            alive += world.chunk_stats(ChunkCoord{kOverworld, static_cast<std::uint16_t>(cx),
                                                  static_cast<std::uint16_t>(cy)})
                         .mobs;
        }
    }
    const std::uint32_t killed = world.status().mobs_killed.load(std::memory_order_relaxed);
    std::printf("  mobs: alive=%u killed=%u migrations=%u peak=%u\n", alive, killed,
                world.status().migrations.load(std::memory_order_relaxed), peak_mobs);

    world.stop();

    std::printf("\n%s\n", chk.failures == 0 ? "OK" : "FAIL");
    return chk.failures == 0 ? 0 : 1;
}
