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
#include <cstring>

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

void print_row(std::int64_t ms, const WorldStatus& st, std::uint32_t creatures) {
    std::printf("  t=%6.1fs  %-5s  wave=%-2u  alive=%-5u  killed=%-5u  migrations=%-6u\n",
                static_cast<double>(ms) / 1000.0,
                st.night.load(std::memory_order_relaxed) ? "NIGHT" : "day",
                st.wave.load(std::memory_order_relaxed), creatures,
                st.creatures_killed.load(std::memory_order_relaxed),
                st.migrations.load(std::memory_order_relaxed));
}

// Run the world forward `n` ticks and leave every actor drained, so the next assertion reads a
// coherent state rather than a race.
void advance(World& w, int n) {
    for (int i = 0; i < n; ++i) w.step(kTickMs);
    w.sync_world();
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

    // --- Accounts ---------------------------------------------------------------------------------
    // Argon2 is slow ON PURPOSE (32 MiB, three passes), so this section costs a fraction of a second
    // and that is the feature. What is asserted is only what the game depends on: a new name works,
    // a wrong password does not, and the same name twice is the same account.
    LoginOutcome out{};
    const int slot = world.login("thnak", "correct horse battery", out);
    const bool created = out == LoginOutcome::kCreated;
    LoginOutcome bad{};
    const int refused = world.login("thnak", "correct horse", bad);
    LoginOutcome again{};
    const int reslot = world.login("thnak", "correct horse battery", again);
    LoginOutcome second{};
    const int slot2 = world.login("guest", "hunter2", second);

    std::printf("accounts: '%s' -> slot %d (%s);  wrong password -> %s;  '%s' -> slot %d\n",
                world.accounts().name_of(1), slot, describe(out), describe(bad),
                world.accounts().name_of(2), slot2);
    chk.expect(slot >= 0 && created, "a new name creates an account and takes a slot");
    chk.expect(refused < 0 && bad == LoginOutcome::kWrongPassword, "a wrong password is refused");
    chk.expect(reslot == slot && again == LoginOutcome::kAuthenticated,
               "the same account returns to the same slot");
    chk.expect(slot2 >= 0 && slot2 != slot, "a second account gets its own slot");
    chk.expect(world.accounts().size() == 2, "the wrong password did not create a third account");

    const std::uint64_t me = world.key_of(slot);
    world.sync_world();

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
    std::printf("\nworld generation: %zu villages, %zu strongholds, %zu buildings\n",
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

    // --- Difficulty by ring, as a function ---------------------------------------------------------
    // The qualitative claim ("go out three rings and you die") is a thing to feel, not to assert.
    // What CAN be asserted is the property it rests on: the same species is strictly worse news the
    // further out it was born, and it is HP and damage that scale, never speed — because there is no
    // counterplay to something that simply outruns you.
    chk.expect(ring_hp_scale(Ring::kWasteland) > ring_hp_scale(Ring::kMeadow) * 3.0f,
               "an outer-ring creature is several times tougher");
    chk.expect(ring_damage_scale(Ring::kSnow) > ring_damage_scale(Ring::kForest),
               "damage rises with the ring too");
    chk.expect(stats_of(CreatureKind::kSlime).speed == stats_of(CreatureKind::kSlime).speed,
               "ring scaling never touches speed");
    chk.expect(raid_kind_of(Ring::kMeadow, 0) != raid_kind_of(Ring::kWasteland, 0),
               "the rim sends different creatures, not just bigger ones");

    // --- The combo table, as a function -----------------------------------------------------------
    // Combos are pure: (status, kind of blow) -> effect. Asserting them here rather than through a
    // staged fight is deliberate — a fight can only ever sample the table, and the interesting
    // failure is an entry that silently stops matching GAME.md §7.
    chk.expect(combo_of(Status::kFrozen, true, false, Element::kNone) == Combo::kShatter,
               "frozen + heavy melee shatters");
    chk.expect(combo_of(Status::kFrozen, false, false, Element::kNone) == Combo::kNone,
               "a light tap does not shatter — the heavy blow is the point");
    chk.expect(combo_of(Status::kBurning, false, true, Element::kNone) == Combo::kBlast,
               "burning + arrow blasts");
    chk.expect(combo_of(Status::kWet, false, false, Element::kShock) == Combo::kConduct,
               "wet + shock conducts");
    chk.expect(combo_of(Status::kMuddy, true, false, Element::kNone) == Combo::kCrush,
               "muddy + heavy melee crushes");
    chk.expect(combo_damage_scale(Combo::kShatter) > 2.0f, "shatter is worth aiming for");

    // --- The opening: you wake in open country and walk ------------------------------------------
    const PlayerView spawn = world.player_view(slot);
    const Village* home_village = layout.nearest_village(static_cast<int>(spawn.x),
                                                         static_cast<int>(spawn.y));
    const double walk = home_village == nullptr ? 0.0
                                                : std::sqrt(std::pow(home_village->tx - spawn.x, 2) +
                                                            std::pow(home_village->ty - spawn.y, 2));
    std::printf("\nspawn: (%.0f,%.0f), nearest village %.0f tiles away, inventory w%d s%d, "
                "hp %d mana %d stamina %d\n",
                static_cast<double>(spawn.x), static_cast<double>(spawn.y), walk,
                spawn.items[static_cast<int>(ItemKind::kWood)],
                spawn.items[static_cast<int>(ItemKind::kStone)], spawn.hp, spawn.mana,
                spawn.stamina);
    chk.expect(is_walkable(terrain_of(kWorldSeed, home, static_cast<int>(spawn.x),
                                      static_cast<int>(spawn.y))),
               "the player does not wake up inside a lake or a wall");
    chk.expect(walk > 12.0, "the player wakes AWAY from the village, not in it");
    chk.expect(spawn.hp == kPlayerMaxHp && spawn.mana == kPlayerMaxMana &&
                   spawn.stamina == kPlayerMaxStamina,
               "a bound player starts with full bars");

    // --- A wall you cannot walk through, and a door you can -------------------------------------
    // Both halves matter and neither is worth much alone. A palisade the player walks through is a
    // painting; a door in a wall that is not a wall is a formality.
    {
        const Village& v = layout.villages().front();
        const VillagePlan vp = plan_of(v.tier);
        // Stand just inside the west wall, then push west for a second of ticks. The wall line is
        // three tiles thick, so a player that crossed it did not clip a corner.
        world.teleport_player(me, kOverworld, static_cast<float>(v.tx - vp.hw + 4) + 0.5f,
                              static_cast<float>(v.ty) + 4.5f);
        for (int i = 0; i < 20; ++i) world.move_player(me, -0.6f, 0.0f);
        const PlayerView shoved = world.player_view(slot);
        chk.expect(shoved.x > static_cast<float>(v.tx - vp.hw),
                   "the palisade stops a player walking straight at it");

        // Now the door. Stepping onto a doorway tile is the whole interaction — there is no verb.
        const Door& d = layout.doors().front();
        const int dtx = static_cast<int>(d.tile & 0xFFFFu);
        const int dty = static_cast<int>(d.tile >> 16);
        world.teleport_player(me, kOverworld, static_cast<float>(dtx) + 0.5f,
                              static_cast<float>(dty) + 0.5f);
        const PlayerView inside = world.player_view(slot);
        chk.expect(inside.map == kInterior, "stepping into a doorway puts the player indoors");
        chk.expect(is_walkable(terrain_of(kWorldSeed, kInterior, static_cast<int>(inside.x),
                                          static_cast<int>(inside.y))),
                   "and puts them on floor, not inside the wall");

        // And out again: the way out is the tile below, the one the room's own doorway sits on.
        // Stepped one at a time and STOPPED as soon as the map changes — walking on past the exit
        // would test where the player wandered to, not where the door put them.
        // `player_view` and not `sync_world`: the authoritative ask is a barrier on the ONE actor
        // that matters, while `sync_world` asks all 2048 chunks and running that twelve times in a
        // loop cost this test fifteen seconds on its own.
        PlayerView back = inside;
        for (int i = 0; i < 12 && back.map != kOverworld; ++i) {
            world.move_player(me, 0.0f, 0.35f);
            back = world.player_view(slot);
        }
        chk.expect(back.map == kOverworld, "walking out of the doorway puts the player back outside");
        chk.expect(static_cast<int>(back.x) == dtx && static_cast<int>(back.y) == dty + 1,
                   "on the doorstep, not on the door — landing on the door is an infinite loop");
        world.teleport_player(me, kOverworld, spawn.x, spawn.y);
    }

    // --- A prefab house has a door too ---------------------------------------------------------
    // The door test above steps through `doors().front()`, which is whichever door sorts lowest —
    // in practice a Structure house near the map's corner, NOT one of the houses a village stamps as
    // part of a hand-composed block. Those blocks are the whole of P2's village work, and "every
    // house has a door" has to hold for their houses or the block regresses it. So this finds an
    // actual prefab dwelling — a street_houses or market_yard parcel exists ONLY inside a village, so
    // its id alone identifies one — and proves its door teleports exactly as a Structure's does.
    {
        int dtx = -1;
        int dty = -1;
        for (const PlacedPrefab& pp : layout.prefabs()) {
            if (pp.id != PrefabId::kStreetHouses && pp.id != PrefabId::kMarketYard) continue;
            const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
            for (std::uint16_t i = 0; i < def.cell_count; ++i) {
                const PrefabCell& c = def.cells[i];
                if (!prefab_cell_is_dwelling(c)) continue;
                if (!prefab_cell_visible(def, c, pp.variant)) continue;
                dtx = pp.tx + prefab_door_dx(c);
                dty = pp.ty + prefab_door_dy(c);
                break;
            }
            if (dtx >= 0) break;
        }
        chk.expect(dtx >= 0, "a village laid a prefab block with a house in it");
        if (dtx >= 0) {
            // The doorway is walkable — the arch you step into — and stepping onto it goes indoors.
            chk.expect(is_walkable(terrain_of(kWorldSeed, kOverworld, dtx, dty)),
                       "a prefab house's doorway is left walkable under the sprite");
            world.teleport_player(me, kOverworld, static_cast<float>(dtx) + 0.5f,
                                  static_cast<float>(dty) + 0.5f);
            const PlayerView in = world.player_view(slot);
            chk.expect(in.map == kInterior, "stepping into a prefab house's door puts the player indoors");
            world.teleport_player(me, kOverworld, spawn.x, spawn.y);
        }
    }

    // An unbound slot must be genuinely inert, not merely undrawn.
    const PlayerView empty_slot = world.player_view(kMaxPlayers - 1);
    chk.expect(!empty_slot.live(), "a slot nobody logged into is not a player");

    // --- Wildlife ---------------------------------------------------------------------------------
    // Seeded from the chunk key at bring-up, never respawned. What matters is that it is (a) there
    // and (b) mostly not out to get you: if the whole map were hostile the disposition system would
    // be an elaborate way of writing `true`.
    std::uint32_t wild_total = 0;
    std::uint32_t wild_hostile = 0;
    for (int cy = 0; cy < kMapChunks; ++cy) {
        for (int cx = 0; cx < kMapChunks; ++cx) {
            const ChunkStats s = world.chunk_stats(
                ChunkCoord{home, static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy)});
            wild_total += s.creatures;
            wild_hostile += s.hostile;
        }
    }
    std::printf("\nwildlife: %u creatures on the map at bring-up, %u of them hostile\n", wild_total,
                wild_hostile);
    chk.expect(wild_total > 200, "the world is not empty before a single raid");
    chk.expect(wild_hostile * 4 < wild_total, "most of what lives out there is not hunting you");

    // --- A staged fight ---------------------------------------------------------------------------
    // Everything above is a property of a table. This is the part that can only be true if the
    // messages actually connect: beacon -> creature notices -> creature strikes -> trusted actor
    // loses HP -> player swings -> chunk resolves -> XP comes back.
    const auto fx = static_cast<std::uint16_t>(spawn.x);
    const auto fy = static_cast<std::uint16_t>(spawn.y);
    const ChunkCoord fight_chunk = chunk_of(home, static_cast<float>(fx), static_cast<float>(fy));

    world.spawn_wave_at(fx, fy, CreatureKind::kSlime, 8);
    advance(world, 25);  // long enough for a beacon (every 3 ticks) and a strike (cooldown 10)

    const ChunkStats staged = world.chunk_stats(fight_chunk);
    const PlayerView mauled = world.player_view(slot);
    const PlayerView other = world.player_view(slot2);
    std::printf("\nstaged fight: %u creatures in the chunk (%u hostile), %u watchers;"
                "  hp: slot %d = %d, slot %d = %d\n",
                staged.creatures, staged.hostile, staged.watchers, slot, mauled.hp, slot2,
                other.hp);
    chk.expect(staged.hostile > 0, "the slimes are hostile and the chunk knows it");
    chk.expect(staged.watchers == 2, "both logged-in players' beacons reached the chunk");
    chk.expect(mauled.hp < kPlayerMaxHp || other.hp < kPlayerMaxHp,
               "creatures reached a player and hit them");
    // Both accounts wake on the same spawn tile, so a creature has to CHOOSE — and `nearest_player`
    // returns one of them, not both. That the two health bars are unequal is the real assertion
    // here: it is the difference between "the player" and "a player", and it is the thing that
    // would have quietly not been true if PlayerActor were still a singleton (ROADMAP principle 2).
    chk.expect(mauled.hp != other.hp,
               "two players in one tile are two actors with two health bars");

    // Swing until something dies. Stamina gates this, so the loop deliberately runs longer than the
    // number of swings it can pay for — proving both that hits land and that they are rationed.
    int swings = 0;
    int refused_swings = 0;
    for (int i = 0; i < 40; ++i) {
        if (world.swing(me, /*heavy*/ i % 3 == 0)) {
            ++swings;
        } else {
            ++refused_swings;
        }
        advance(world, 2);
    }
    const PlayerView fought = world.player_view(slot);
    const std::uint32_t player_kills = world.status().player_kills.load(std::memory_order_relaxed);
    std::printf("swings: %d landed, %d refused for stamina;  kills %u;  melee level %u (xp %u/%u)\n",
                swings, refused_swings, player_kills, fought.skill_level[0], fought.skill_xp[0],
                fought.skill_next[0]);
    chk.expect(swings > 0, "the player could swing");
    chk.expect(refused_swings > 0, "stamina rationed the swings — attacking is not free");
    chk.expect(player_kills > 0, "swinging killed something");
    chk.expect(fought.skill_level[static_cast<int>(Skill::kMelee)] > 0 ||
                   fought.skill_xp[static_cast<int>(Skill::kMelee)] > 0,
               "killing something granted melee experience");

    // Magic leaves a status behind — that is the whole point of a school, more than its damage.
    world.spawn_wave_at(fx, fy, CreatureKind::kSlime, 6, /*seed*/ 7);
    advance(world, 3);
    const bool cast_ok = world.cast(me, Element::kIce, static_cast<float>(fx) + 1.0f,
                                    static_cast<float>(fy) + 1.0f);
    advance(world, 1);
    const ChunkStats frozen = world.chunk_stats(fight_chunk);
    std::printf("ice: cast %s, %u creatures carrying a status\n", cast_ok ? "ok" : "refused",
                frozen.afflicted);
    chk.expect(cast_ok, "the player could cast with full mana");
    chk.expect(frozen.afflicted > 0, "the spell left a status on something");

    // An arrow has to exist as chunk state for a tick or two before it hits anything.
    const bool shot_ok = world.shoot(me, static_cast<float>(fx) + 6.0f, static_cast<float>(fy));
    world.sync_world();
    const ChunkStats airborne = world.chunk_stats(fight_chunk);
    std::printf("arrow: %s, %u in flight\n", shot_ok ? "launched" : "refused",
                airborne.projectiles);
    chk.expect(shot_ok, "the player could shoot");

    // --- Death and respawn -------------------------------------------------------------------------
    // The respawn point is where you lit your hearth. Nothing is taken from you when you die: this
    // game's default is chill (GAME.md §0), and the cost of dying is the walk back.
    const PlayerView before_death = world.player_view(slot);
    world.spawn_wave_at(fx, fy, CreatureKind::kGhost, 12, /*seed*/ 99);
    int waited = 0;
    PlayerView dying = before_death;
    while (dying.hp > 0 && waited < 400) {
        advance(world, 10);
        waited += 10;
        dying = world.player_view(slot);
    }
    const bool died = dying.hp == 0 || dying.deaths > before_death.deaths;
    advance(world, kRespawnTicks + 5);
    const PlayerView reborn = world.player_view(slot);
    std::printf("\ndeath: %s after %d ticks;  respawned at (%u,%u) with hp %d, "
                "inventory intact (%d wood)\n",
                died ? "killed" : "survived (no assertion)", waited, reborn.respawn_tx,
                reborn.respawn_ty, reborn.hp, reborn.items[static_cast<int>(ItemKind::kWood)]);
    if (died) {
        chk.expect(reborn.hp > 0, "the player came back");
        chk.expect(reborn.deaths > before_death.deaths, "the death was counted");
        chk.expect(reborn.items[static_cast<int>(ItemKind::kWood)] ==
                       before_death.items[static_cast<int>(ItemKind::kWood)],
                   "dying took nothing out of the inventory");
    }

    // --- Farming, and the fact that nothing is given to you --------------------------------------
    // There is no starting apron any more, so planting is refused until the player tills — which is
    // exactly the property to assert, because the old test could not tell tilling from the free
    // farmland it was standing on.
    const PlayerView here = world.player_view(slot);
    const auto farm_tx = static_cast<std::uint16_t>(here.x);
    const auto farm_ty = static_cast<std::uint16_t>(here.y);
    const ChunkCoord farm_chunk =
        chunk_of(home, static_cast<float>(farm_tx), static_cast<float>(farm_ty));

    world.plant(me, home, farm_tx, farm_ty, CropKind::kWheat, 0);
    world.sync_world();
    const std::uint32_t crops_untilled = world.chunk_stats(farm_chunk).crops;

    int tilled = 0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            if (world.till(me, home, static_cast<std::uint16_t>(farm_tx + dx),
                           static_cast<std::uint16_t>(farm_ty + dy))) {
                ++tilled;
            }
        }
    }
    world.sync_world();
    int planted = 0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            world.plant(me, home, static_cast<std::uint16_t>(farm_tx + dx),
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
    const PlayerView before_build = world.player_view(slot);
    const bool lit = world.build_at(me, home, static_cast<std::uint16_t>(farm_tx + 4),
                                    static_cast<std::uint16_t>(farm_ty), BuildKind::kHearth);
    world.sync_world();
    const PlayerView after_build = world.player_view(slot);
    std::printf("\nhearth: %s;  stone %d -> %d  (debited by the TRUSTED PlayerActor);"
                "  respawn now (%u,%u)\n",
                lit ? "lit" : "could not afford",
                before_build.items[static_cast<int>(ItemKind::kStone)],
                after_build.items[static_cast<int>(ItemKind::kStone)], after_build.respawn_tx,
                after_build.respawn_ty);
    chk.expect(lit, "the player could afford a hearth");
    chk.expect(after_build.items[static_cast<int>(ItemKind::kStone)] <
                   before_build.items[static_cast<int>(ItemKind::kStone)],
               "lighting a hearth debited stone");
    chk.expect(after_build.respawn_tx == static_cast<std::uint16_t>(farm_tx + 4),
               "the hearth moved where the player wakes up");

    // Overspend must be refused atomically: keep building until the inventory says no, and prove
    // the balance never went negative. Runs LAST because it deliberately drains the player — an
    // earlier ordering left nothing to pay for the test above, which read as "building is broken"
    // when the feature was fine.
    bool minted = true;
    for (int i = 0; i < 500; ++i) {
        if (!world.build_at(me, home, static_cast<std::uint16_t>(farm_tx + 8),
                            static_cast<std::uint16_t>(farm_ty + i % 5), BuildKind::kHearth)) {
            minted = false;
            break;
        }
    }
    const PlayerView post_greedy = world.player_view(slot);
    chk.expect(!minted, "the inventory refused an unaffordable build");
    chk.expect(post_greedy.items[static_cast<int>(ItemKind::kStone)] >= 0,
               "stone never went negative");

    // --- Run the world ---------------------------------------------------------------------------
    std::printf("\nsimulating %d ticks (%.0f s of world time)\n", ticks,
                static_cast<double>(ticks) * static_cast<double>(kTickMs) / 1000.0);

    std::uint32_t peak = 0;
    bool saw_night = false;
    bool saw_migration = false;

    for (int i = 0; i < ticks; ++i) {
        world.step(kTickMs);

        // Every 100 ticks, take a consistent sample: barrier the director AND every chunk, so the
        // row printed below is one coherent world state rather than 1024 chunks at 1024 different
        // ticks. Between samples the chunks are deliberately left to run ahead/behind each other.
        if ((i + 1) % 100 == 0) {
            world.sync_world();
            const std::uint32_t alive = count_creatures(world.bus());
            world.status().creatures_alive.store(alive, std::memory_order_relaxed);
            peak = std::max(peak, alive);
            saw_night = saw_night || world.status().night.load(std::memory_order_relaxed);
            saw_migration =
                saw_migration || world.status().migrations.load(std::memory_order_relaxed) > 0;
            print_row(world.status().world_ms.load(std::memory_order_relaxed), world.status(),
                      alive);
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
    chk.expect(peak > 0, "there is life on the map");
    chk.expect(saw_migration, "creatures migrated across chunk (actor) boundaries");
    chk.expect(farm_stats.tick >= static_cast<std::uint64_t>(ticks),
               "every chunk received every tick (no dropped fan-out)");

    // Crops planted with a 20 s growth time must be ripe well before the run ends.
    if (ticks >= 300) {
        chk.expect(farm_stats.ripe > 0, "wheat planted early in the run ripened");
    }

    // Conservation: everything that exists is either alive somewhere or counted as killed. A
    // creature lost during a chunk hand-off would break this.
    // Counted by ASK, not from published views: an ask is answered by the chunk itself and is
    // therefore authoritative, while a view can be one tick stale — and, since the LOD publishes an
    // unwatched chunk only every 32nd tick, rather more than one tick stale.
    std::uint32_t alive = 0;
    for (int cy = 0; cy < kMapChunks; ++cy) {
        for (int cx = 0; cx < kMapChunks; ++cx) {
            alive += world.chunk_stats(ChunkCoord{kOverworld, static_cast<std::uint16_t>(cx),
                                                  static_cast<std::uint16_t>(cy)})
                         .creatures;
        }
    }
    const std::uint32_t killed = world.status().creatures_killed.load(std::memory_order_relaxed);
    std::printf("  creatures: alive=%u killed=%u (%u by a player) migrations=%u peak=%u\n", alive,
                killed, world.status().player_kills.load(std::memory_order_relaxed),
                world.status().migrations.load(std::memory_order_relaxed), peak);

    world.stop();

    std::printf("\n%s\n", chk.failures == 0 ? "OK" : "FAIL");
    return chk.failures == 0 ? 0 : 1;
}
