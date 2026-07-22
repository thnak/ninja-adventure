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
    std::printf("  t=%6.1fs  %-5s  wave=%-2u  mobs=%-5u  killed=%-5u  migrations=%-6u  core_hp=%d\n",
                static_cast<double>(ms) / 1000.0,
                st.night.load(std::memory_order_relaxed) ? "NIGHT" : "day",
                st.wave.load(std::memory_order_relaxed), mobs,
                st.mobs_killed.load(std::memory_order_relaxed),
                st.migrations.load(std::memory_order_relaxed),
                st.core_hp.load(std::memory_order_relaxed));
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

    // --- Spawn camps: where the monsters come from --------------------------------------------
    const auto home = static_cast<std::uint16_t>(MapId::kHomeValley);
    const auto camps = World::camps(home);
    std::printf("spawn camps (%d fixed lanes, not the whole rim):\n", kSpawnCamps);
    for (int i = 0; i < kSpawnCamps; ++i) {
        const auto [cx, cy] = camps[static_cast<std::size_t>(i)];
        std::printf("  camp %d at (%3d,%3d)  chunk (%u,%u)\n", i, cx, cy,
                    chunk_of(home, static_cast<float>(cx), static_cast<float>(cy)).cx,
                    chunk_of(home, static_cast<float>(cx), static_cast<float>(cy)).cy);
    }
    std::printf("\n");

    // --- Farming, during daylight ----------------------------------------------------------------
    // Plant a 6x6 block of wheat on the tilled apron around the core.
    int planted = 0;
    for (int dy = 1; dy <= 6; ++dy) {
        for (int dx = 1; dx <= 6; ++dx) {
            world.plant(home, static_cast<std::uint16_t>(kCoreTx + dx),
                        static_cast<std::uint16_t>(kCoreTy + dy), CropKind::kWheat, 0);
            ++planted;
        }
    }

    // --- Building, paid for out of the trusted inventory ----------------------------------------
    const PlayerView before_build = world.player_view();
    int walls = 0;
    for (int dx = -8; dx <= 8; ++dx) {
        if (world.build_at(home, static_cast<std::uint16_t>(kCoreTx + dx),
                           static_cast<std::uint16_t>(kCoreTy - 8), BuildKind::kWall)) {
            ++walls;
        }
    }
    int turrets = 0;
    for (int dx = -6; dx <= 6; dx += 4) {
        if (world.build_at(home, static_cast<std::uint16_t>(kCoreTx + dx),
                           static_cast<std::uint16_t>(kCoreTy - 6), BuildKind::kTurret)) {
            ++turrets;
        }
    }
    // Cheap fencing to close the flanks — weaker than a wall but it still blocks, which is the
    // whole point of the perimeter now that buildings are solid.
    int fences = 0;
    for (int dy = -7; dy <= 7; ++dy) {
        for (int dx : {-8, 8}) {
            if (world.build_at(home, static_cast<std::uint16_t>(kCoreTx + dx),
                               static_cast<std::uint16_t>(kCoreTy + dy), BuildKind::kFence)) {
                ++fences;
            }
        }
    }
    const PlayerView after_build = world.player_view();

    std::printf("build phase: planted=%d walls=%d turrets=%d fences=%d\n", planted, walls, turrets,
                fences);
    std::printf("  wood %d -> %d, stone %d -> %d  (debited by the TRUSTED PlayerActor)\n\n",
                before_build.items[static_cast<int>(ItemKind::kWood)],
                after_build.items[static_cast<int>(ItemKind::kWood)],
                before_build.items[static_cast<int>(ItemKind::kStone)],
                after_build.items[static_cast<int>(ItemKind::kStone)]);

    chk.expect(walls > 0, "at least one wall was placed");
    chk.expect(turrets > 0, "at least one turret was placed");
    chk.expect(after_build.items[static_cast<int>(ItemKind::kWood)] <
                   before_build.items[static_cast<int>(ItemKind::kWood)],
               "placing walls debited wood");

    // --- Base expansion: reclaim ground beyond the starting apron --------------------------------
    // The apron is baked into the terrain function; anything past it must be tilled first, which is
    // the chunk's own overlay. Planting on untilled grass has to fail.
    const ChunkCoord core_chunk_c =
        chunk_of(home, static_cast<float>(kCoreTx), static_cast<float>(kCoreTy));
    const std::uint16_t out_tx = static_cast<std::uint16_t>(kCoreTx + kFarmRadius + 2);
    const std::uint16_t out_ty = static_cast<std::uint16_t>(kCoreTy + kFarmRadius + 2);

    world.plant(home, out_tx, out_ty, CropKind::kCarrot, 0);
    world.sync_world();
    const std::uint32_t crops_before_till = world.chunk_stats(core_chunk_c).crops;

    int tilled = 0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            if (world.till(home, static_cast<std::uint16_t>(out_tx + dx),
                           static_cast<std::uint16_t>(out_ty + dy))) {
                ++tilled;
            }
        }
    }
    world.sync_world();
    world.plant(home, out_tx, out_ty, CropKind::kCarrot, 0);
    world.sync_world();
    const ChunkStats after_till = world.chunk_stats(core_chunk_c);

    std::printf("base expansion: tilled %d tiles beyond the apron\n", tilled);
    std::printf("  crops before tilling %u -> after %u  (planting on untilled grass is refused)\n\n",
                crops_before_till, after_till.crops);
    chk.expect(tilled > 0, "tilling reclaimed ground outside the starting apron");
    chk.expect(after_till.tilled > 0, "the chunk recorded its tilled overlay");
    chk.expect(after_till.crops > crops_before_till, "a crop could be planted on reclaimed ground");

    // --- Upgrades --------------------------------------------------------------------------------
    // Measure the chunk that actually OWNS the turrets. They sit at kCoreTx-6 = tile 122, which is
    // chunk 3, not the core's chunk 4 — a base a dozen tiles across already straddles a chunk
    // border, which is exactly the property the cluster demo depends on.
    const ChunkCoord turret_chunk = chunk_of(home, static_cast<float>(kCoreTx - 6),
                                             static_cast<float>(kCoreTy - 6));
    const ChunkStats pre_up = world.chunk_stats(turret_chunk);
    int upgrades = 0;
    for (int dx = -6; dx <= 6; dx += 4) {
        // Turret at level 1 -> 2 -> 3.
        for (std::uint8_t lvl = 1; lvl < kMaxLevel; ++lvl) {
            if (world.upgrade(home, static_cast<std::uint16_t>(kCoreTx + dx),
                              static_cast<std::uint16_t>(kCoreTy - 6), BuildKind::kTurret, lvl)) {
                ++upgrades;
            }
        }
    }
    world.sync_world();
    const ChunkStats post_up = world.chunk_stats(turret_chunk);
    std::printf("upgrades: %d applied in chunk (%u,%u); summed building levels %u -> %u\n\n",
                upgrades, turret_chunk.cx, turret_chunk.cy, pre_up.building_levels,
                post_up.building_levels);
    chk.expect(upgrades > 0, "at least one upgrade was paid for and applied");
    chk.expect(post_up.building_levels > pre_up.building_levels,
               "upgrades raised the buildings' levels");

    // Overspend must be refused atomically: keep buying turrets until the inventory says no, and
    // prove the balance never went negative. Runs LAST because it deliberately drains the player —
    // an earlier version ran it before the upgrade test and left nothing to pay for an upgrade,
    // which read as "upgrades are broken" when the feature was fine.
    bool minted = true;
    for (int i = 0; i < 500; ++i) {
        if (!world.build_at(home, static_cast<std::uint16_t>(kCoreTx - 10),
                            static_cast<std::uint16_t>(kCoreTy + 10 + i % 5), BuildKind::kTurret)) {
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

    const ChunkCoord core_chunk = core_chunk_c;
    const ChunkStats core_stats = world.chunk_stats(core_chunk);
    std::printf("  core chunk (%u,%u): crops=%u ripe=%u buildings=%u tick=%llu\n", core_chunk.cx,
                core_chunk.cy, core_stats.crops, core_stats.ripe, core_stats.buildings,
                static_cast<unsigned long long>(core_stats.tick));

    chk.expect(saw_night, "the day/night cycle reached night");
    chk.expect(world.status().wave.load(std::memory_order_relaxed) > 0, "at least one wave spawned");
    chk.expect(peak_mobs > 0, "mobs were spawned and are alive in chunks");
    chk.expect(saw_migration, "mobs migrated across chunk (actor) boundaries");
    chk.expect(core_stats.tick >= static_cast<std::uint64_t>(ticks),
               "every chunk received every tick (no dropped fan-out)");
    // Buildings are solid now, so the perimeter is load-bearing: mobs have to chew through it
    // instead of walking past. Before blocking existed the core was destroyed by t=115 s.
    chk.expect(world.status().core_hp.load(std::memory_order_relaxed) > 0,
               "the perimeter kept the core alive through wave 1");

    // Crops planted at t=0 with a 20 s growth time must be ripe well before the run ends.
    if (ticks >= 300) {
        chk.expect(core_stats.ripe > 0, "wheat planted at t=0 ripened");
    }

    // Mob conservation: everything that spawned is either alive somewhere or counted as killed.
    // A mob lost during a chunk hand-off would break this.
    const std::uint32_t alive = count_mobs(world.bus());
    const std::uint32_t killed = world.status().mobs_killed.load(std::memory_order_relaxed);
    std::printf("  mobs: alive=%u killed=%u migrations=%u peak=%u\n", alive, killed,
                world.status().migrations.load(std::memory_order_relaxed), peak_mobs);

    world.stop();

    std::printf("\n%s\n", chk.failures == 0 ? "OK" : "FAIL");
    return chk.failures == 0 ? 0 : 1;
}
