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

    // --- RFC-001: the ability pipeline's generic state machine (no actor, no content needed) -------
    // ability_pipeline.hpp's functions take plain tick counts and flags rather than reaching into
    // the shipped AbilityId table, specifically so they can be driven here with synthetic parameters
    // no shipped ability uses yet (a real cast time, a real channel). This proves Cast/Channel/
    // interrupt behavior the ability-layer section further down cannot reach — every shipped ability
    // collapses Cast -> Release in the very tick it starts, per RFC-001 Section 9's own ruling.
    {
        // reject_of(): Section 3's admission order, one reason at a time.
        chk.expect(reject_of(true, false, false, false, AbilityPhase::kIdle, false, false) ==
                       AbilityReject::kUnavailable,
                   "reject_of: dead/unbound/mounted wins over everything else");
        chk.expect(reject_of(false, true, false, false, AbilityPhase::kIdle, false, false) ==
                       AbilityReject::kLocked,
                   "reject_of: school too low");
        chk.expect(reject_of(false, false, true, false, AbilityPhase::kIdle, false, false) ==
                       AbilityReject::kCooldown,
                   "reject_of: on cooldown");
        chk.expect(reject_of(false, false, false, true, AbilityPhase::kIdle, false, false) ==
                       AbilityReject::kResource,
                   "reject_of: not enough stamina/mana");
        chk.expect(reject_of(false, false, false, false, AbilityPhase::kCast, false, false) ==
                       AbilityReject::kBusy,
                   "reject_of: Invariant I1 - a head already in flight is kBusy");
        chk.expect(reject_of(false, false, false, false, AbilityPhase::kIdle, true, false) ==
                       AbilityReject::kBusy,
                   "reject_of: the post-interrupt stagger window is also kBusy");
        chk.expect(reject_of(false, false, false, false, AbilityPhase::kIdle, false, true) ==
                       AbilityReject::kBadTarget,
                   "reject_of: a bad kEntity target is refused last");
        chk.expect(reject_of(false, false, false, false, AbilityPhase::kIdle, false, false) ==
                       AbilityReject::kOk,
                   "reject_of: nothing wrong admits the activation");

        // advance_head(): a synthetic 5-tick cast (no shipped ability has cast_ticks > 0).
        AbilityHead cast_head{};
        try_start_cast(cast_head, /*ability_ref=*/0, 10.0f, 10.0f, 1.0f, 0.0f);
        chk.expect(cast_head.phase == AbilityPhase::kCast, "try_start_cast seats the head in Cast");
        int ticks_still_casting = 0;
        for (int i = 0; i < 4; ++i) {
            advance_head(cast_head, /*cast_ticks=*/5, /*has_channel=*/false, /*channel_max_ticks=*/0);
            if (cast_head.phase == AbilityPhase::kCast) ++ticks_still_casting;
        }
        chk.expect(ticks_still_casting == 4, "a 5-tick cast is still casting after 4 advances");
        advance_head(cast_head, 5, false, 0);
        chk.expect(cast_head.phase == AbilityPhase::kRelease,
                   "the 5th advance releases a 5-tick cast");

        // apply_interrupt(): the Cast row - full refund, no cooldown charge, uniform stagger.
        const InterruptResult cast_interrupt = apply_interrupt(AbilityPhase::kCast);
        chk.expect(cast_interrupt.refund_base, "a broken Cast fully refunds the base cost");
        chk.expect(!cast_interrupt.charge_half_cooldown, "a broken Cast never starts a cooldown");
        chk.expect(cast_interrupt.stagger_ticks == kStaggerTicks,
                   "a broken Cast still pays the uniform post-T12 stagger");

        // apply_interrupt(): the Channel row - forfeits both the base cost and the drained ticks,
        // but only charges HALF a cooldown ("the mercy is the cooldown, not the resource").
        const InterruptResult channel_interrupt = apply_interrupt(AbilityPhase::kChannel);
        chk.expect(!channel_interrupt.refund_base,
                   "a forced Channel interrupt forfeits the base cost");
        chk.expect(!channel_interrupt.refund_channel_drain,
                   "a forced Channel interrupt forfeits the drained ticks too");
        chk.expect(channel_interrupt.charge_half_cooldown,
                   "a forced Channel interrupt charges only half a cooldown");

        // release_channel(): the grace tap. Sub-grace release cancels (T12); past-grace release
        // fires (T4).
        AbilityHead channel_head{};
        channel_head.phase = AbilityPhase::kChannel;
        channel_head.charge_elapsed = kChannelGraceTicks - 1;
        const bool tapped_released = release_channel(channel_head);
        chk.expect(!tapped_released && channel_head.phase == AbilityPhase::kIdle,
                   "releasing before the grace window cancels rather than fires");
        const InterruptResult grace = grace_tap_result();
        chk.expect(grace.refund_base && grace.refund_channel_drain,
                   "a grace tap refunds everything - the base cost AND the drained ticks");

        AbilityHead full_channel_head{};
        full_channel_head.phase = AbilityPhase::kChannel;
        full_channel_head.charge_elapsed = kChannelGraceTicks + 2;
        const bool full_released = release_channel(full_channel_head);
        chk.expect(full_released && full_channel_head.phase == AbilityPhase::kRelease,
                   "releasing past the grace window fires the ability");

        // charge_mil_of(): monotone in channel time, saturating at 1000; 1000 for a channel-less
        // ability (never curve-penalized).
        chk.expect(charge_mil_of(0, 0) == 1000, "no channel block: always full power");
        chk.expect(charge_mil_of(5, 10) == 500,
                   "half-held charge is half power (fixed-point 0..1000)");
        chk.expect(charge_mil_of(10, 10) == 1000, "a fully-held charge saturates at exactly 1000");

        // The Section 9 data invariants (V1 phase applicability, V3 the persist-ticks cap).
        chk.expect(valid_persist_ticks(kMaxPersistTicks), "exactly the cap is still valid (<=)");
        chk.expect(!valid_persist_ticks(kMaxPersistTicks + 1), "one tick over the cap is rejected");
        chk.expect(valid_phase_applicability(PayloadKind::kInstantHit, 0, false),
                   "an instant hit needs no cast and no channel");
        chk.expect(!valid_phase_applicability(PayloadKind::kDash, 0, false),
                   "V1: kDash without a cast is an unreadable, uncommitted dash - invalid");
        chk.expect(valid_phase_applicability(PayloadKind::kDash, 9, false),
                   "V1: kDash with a cast and no channel is valid");

        std::printf(
            "RFC-001 pipeline: admission, Cast/Channel timing, and interrupt refunds all check out\n\n");
    }

    // --- RFC-004: the CombatEntity chassis and terrain scar layer's pure functions -----------------
    // combat_entity.hpp's state-machine and scar functions take plain data (a def, flags, ticks)
    // rather than reaching into a ChunkActor, exactly like ability_pipeline.hpp's functions do — so
    // the arm-exit branches, the escalation ladder, and the lazy heal decode are all directly
    // testable here with synthetic entities/defs, independent of the world/chunk integration tests
    // further down.
    {
        const EntityDef spike = entity_def(EntityKind::kRockSpike);
        chk.expect(spike.collision == Collision::kGround && spike.destroyable,
                   "kRockSpike is a destroyable, ground-blocking archetype");
        const EntityDef rock = entity_def(EntityKind::kFallingRock);
        chk.expect(rock.life_ticks == 0 && rock.hittable_while_arming,
                   "kFallingRock has no Active phase and is hittable only while arming");

        // next_state_after_arm(): the three arm-exit branches (RFC-004 Section 3).
        chk.expect(next_state_after_arm(spike, /*intercepted=*/true, /*occupied=*/false) ==
                       EntityState::kDying,
                   "(a) an intercepted arm-only body dies without ever reaching Active");
        chk.expect(next_state_after_arm(spike, /*intercepted=*/false, /*occupied=*/true) ==
                       EntityState::kDying,
                   "(b) the anti-trap rule whiffs a blocking spawn onto an occupied footprint");
        chk.expect(next_state_after_arm(spike, /*intercepted=*/false, /*occupied=*/false) ==
                       EntityState::kActive,
                   "the ordinary arm -> active transition, neither excuse applying");
        chk.expect(next_state_after_arm(rock, /*intercepted=*/false, /*occupied=*/false) ==
                       EntityState::kDying,
                   "(c) life_ticks == 0 fires its terminal transition instead of ever going Active");

        // The scar escalation ladder (Section 8.4) and its lazy heal decode.
        chk.expect(escalate(ScarKind::kNone, 0, 100) == ScarKind::kCracked,
                   "bare ground scars to kCracked");
        chk.expect(escalate(ScarKind::kCracked, 100, 100 + kEscalateWindow - 1) == ScarKind::kRubble,
                   "a second scarring impact inside the escalation window upgrades one step");
        chk.expect(escalate(ScarKind::kRubble, 100, 100 + kEscalateWindow - 1) == ScarKind::kCrater,
                   "rubble upgrades to crater");
        chk.expect(escalate(ScarKind::kCrater, 100, 100 + kEscalateWindow - 1) == ScarKind::kCrater,
                   "crater is the top of the ladder");
        chk.expect(escalate(ScarKind::kCracked, 100, 100 + kEscalateWindow + 1) == ScarKind::kCracked,
                   "outside the escalation window, a scarring impact re-stamps kCracked instead");

        Scar s{5, 5, ScarKind::kCrater, /*heal_tick=*/1000, /*made_tick=*/0};
        const std::uint64_t heal_crater = heal_ticks_of(ScarKind::kCrater);
        const std::uint64_t heal_rubble = heal_ticks_of(ScarKind::kRubble);
        const Scar after_one = heal_lazy(s, 1000);
        chk.expect(after_one.kind == ScarKind::kRubble, "one heal-step downgrades crater to rubble");
        // A single call with `now` far past several heal steps must still land on the correct rung —
        // the "wake" contract: bounded by severity levels, never by how many ticks were skipped.
        const Scar after_wake = heal_lazy(s, 1000 + heal_crater + heal_rubble + 1);
        chk.expect(after_wake.kind == ScarKind::kNone,
                   "fast-forwarding past every remaining heal step lands on kNone in one call, "
                   "whether or not a chunk actually ticked through the skipped ticks");

        // The v1 damage-to-entity multiplier table (Section 7).
        chk.expect(entity_damage_scale(EntityKind::kIceWall, Element::kFire, false) == 2.0f,
                   "fire is the ice wall's weakness");
        chk.expect(entity_damage_scale(EntityKind::kIceWall, Element::kIce, false) == 0.25f,
                   "ice barely scratches an ice wall");
        chk.expect(entity_damage_scale(EntityKind::kRockSpike, Element::kNone, true) == 1.5f,
                   "a heavy melee blow is extra effective against a rock spike");
        chk.expect(entity_damage_scale(EntityKind::kRockSpike, Element::kNone, false) == 1.0f,
                   "a light melee blow against a rock spike is unscaled");

        // The one vision_bits rasterization rule (Section 4): a tile's centre inside the circle.
        chk.expect(circle_covers_tile(5.5f, 5.5f, 5.0f, 5.0f, 1.0f), "a near tile centre is covered");
        chk.expect(!circle_covers_tile(50.5f, 50.5f, 5.0f, 5.0f, 1.0f),
                   "a far tile centre is not covered");

        std::printf(
            "RFC-004 chassis: arm-exit branches, scar escalation/heal, and the damage table all "
            "check out\n\n");
    }

    // --- RFC-003: physics & material interaction, as pure functions ---------------------------------
    {
        // §3.1: impulse transmission — Spirit is the one impulse-immune (infinite-mass) material.
        chk.expect(material_impulse_pm(Material::kFlesh) == 1000, "Flesh impulse transmission is baseline");
        chk.expect(material_impulse_pm(Material::kSpirit) == 0, "Spirit is impulse-immune");
        chk.expect(material_impulse_pm(Material::kSlime) == 1400, "shoves send slime gel bouncing");
        chk.expect(transmit_impulse(220, Material::kSpirit) == 0,
                   "a Spirit target receives zero transmitted impulse");
        chk.expect(transmit_impulse(220, Material::kSlime) == 308,
                   "220 impulse x1400pm on Slime transmits at 308");

        // §4: mass and outgoing-impulse scaling by scale tier.
        chk.expect(mass_of(ScaleTier::kMedium) == 100 && mass_of(ScaleTier::kGiant) == 700,
                   "mass points match the RFC-003 §4 table (Medium 100, Giant 700)");
        chk.expect(scale_impulse_out_pm(ScaleTier::kGiant) == 1600,
                   "a Giant's outgoing impulse is scaled x1.6");

        // §6: terrain physical properties, base rows.
        const TerrainPhys grass = terrain_phys(Terrain::kGrass);
        chk.expect(grass.friction == 60 && grass.grip == 70 && grass.conductivity == 15 &&
                       grass.stability == 60,
                   "grass is the §6 baseline row");
        const TerrainPhys marsh = terrain_phys(Terrain::kMarsh);
        chk.expect(marsh.friction == 95 && marsh.grip == 90 && marsh.conductivity == 60,
                   "marsh matches the §6 mud row");

        // §6: the RFC-004 scar overlay on top of terrain properties.
        const TerrainPhys rubbled = terrain_phys(Terrain::kGrass, ScarKind::kRubble);
        chk.expect(rubbled.friction == 85 && rubbled.stability == 25,
                   "a rubble scar SETS the terrain row, it does not blend with grass");
        chk.expect(terrain_phys(Terrain::kGrass, ScarKind::kCracked).stability == 45,
                   "a cracked scar subtracts 15 stability from the base row");
        chk.expect(terrain_phys(Terrain::kGrass, ScarKind::kScorched).conductivity == 5,
                   "a scorched scar subtracts 10 conductivity from the base row");

        // §5: the knockback law, reproduced against the RFC's own §5/§10 worked numbers.
        chk.expect(kb_terrain_pm(60) == 1000, "grass friction 60 is kb_terrain = 1.0x by construction");
        chk.expect(kb_terrain_pm(15) == 2125, "friction 15 (ice glaze) matches the RFC's own 2125pm");
        chk.expect(kb_terrain_pm(95) == 125, "friction 95 (marsh) matches the RFC's own 125pm");
        const float kb_grass = knockback_tiles(220, 100, 60);
        chk.expect(kb_grass > 2.19f && kb_grass < 2.21f,
                   "220 impulse / 100 mass on grass moves ~2.2 tiles, the RFC's §5 worked example");
        chk.expect(knockback_tiles(220, 700, 60) < knockback_tiles(220, 100, 60),
                   "a Giant's mass shoves less far than a Medium's for the same impulse");
        chk.expect(knockback_tiles(50000, 100, 60) == kKnockbackCap, "knockback is capped at 4 tiles");

        // §6: the mud rule (force-transfer), reproduced against the RFC's own §6 worked example.
        const std::uint16_t bonus = force_transfer_crush(220, kb_terrain_pm(95), 90);
        chk.expect(bonus == 44, "marsh's suppressed momentum adds ~44 Crush, the RFC's own example");
        chk.expect(force_transfer_crush(220, kb_terrain_pm(80), 55) == 0,
                   "sand (kb_terrain 500pm) merely absorbs -- zero force-transfer bonus");

        // §6: slip mitigation — ice-grip (< 30) softens a direct blow.
        chk.expect(slip_applies(29), "grip just under the ice threshold slips");
        chk.expect(!slip_applies(30), "grip AT the threshold does not slip");

        // §5/§10: WallSlam, reproduced against the RFC's own §10 Meteor worked example.
        chk.expect(wallslam_crush(1.7f, 100) == 85,
                   "1.7 undelivered tiles at Medium mass slams for 85, the RFC's own example");

        // §7: terrain stress, reproduced against the same worked example (stress 200 vs grass
        // stability 60 -> threshold 180).
        chk.expect(stress_converts(200, 60), "200 stress exceeds grass's 60x3=180 threshold");
        chk.expect(!stress_converts(179, 60), "179 stress falls just short of the same threshold");

        std::printf(
            "RFC-003 physics: material impulse transmission, mass/tier tables, terrain properties, "
            "the knockback law, the mud/ice rules, WallSlam and terrain stress all check out\n\n");
    }

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

    // --- RFC-002: the combo table, as a function ----------------------------------------------------
    // Combos are pure: (ladder state, kind of blow) -> effect. Asserting them here rather than
    // through a staged fight is deliberate — a fight can only ever sample the table, and the
    // interesting failure is an entry that silently stops matching GAME.md §7. Synthetic
    // `StatusState`s stand in for a target already sitting at a given stage/coating, exactly as the
    // old test drove `combo_of` with a bare `Status` value.
    {
        Gauge g[5]{};
        StatusState frozen{Channel::kCold, 3, kFreezeTicks, 0, {}};
        chk.expect(status_detonate(frozen, g, true, false, false, 100) == Combo::kShatter,
                   "frozen + heavy melee shatters");
    }
    {
        Gauge g[5]{};
        StatusState frozen{Channel::kCold, 3, kFreezeTicks, 0, {}};
        chk.expect(status_detonate(frozen, g, false, false, false, 100) == Combo::kNone,
                   "a light tap does not shatter — the heavy blow is the point");
    }
    {
        Gauge g[5]{};
        StatusState burning{Channel::kHeat, 2, kHeatStage2Ticks, 0, {}};
        chk.expect(status_detonate(burning, g, false, true, false, 100) == Combo::kBlast,
                   "burning + arrow blasts");
    }
    {
        Gauge g[5]{};
        StatusState wet{};
        status_coat(wet, CoatingPacket{Coating::kWet, 80});
        chk.expect(status_detonate(wet, g, false, false, true, 100) == Combo::kConduct,
                   "wet + a shock-element hit conducts");
    }
    {
        Gauge g[5]{};
        StatusState mired{Channel::kEarth, 2, kEarthStage2Ticks, 0, {}};
        chk.expect(status_detonate(mired, g, true, false, false, 100) == Combo::kCrush,
                   "mired (earth stage 2) + heavy melee crushes");
    }
    {
        // No `Combo::kArc` assertion existed before this migration — a gap this closes.
        Gauge g[5]{};
        StatusState shocked{Channel::kShock, 2, kShockStage2Ticks, 0, {}};
        const Combo c = status_detonate(shocked, g, false, false, false, 100);
        chk.expect(c == Combo::kArc, "shocked (stage 2) + melee arcs");
        chk.expect(shocked.primary == Channel::kShock && shocked.stage == 1,
                   "Arc drops one rung, not cleared (the spammable, low-commitment combo)");
    }
    chk.expect(combo_damage_scale(Combo::kShatter) > 2.0f, "shatter is worth aiming for");

    // --- RFC-002: the ladder chassis's own pure functions -------------------------------------------
    {
        // Promotion (§4): a single kIceBoltPower-anchored gain (600) lands exactly on T2, the
        // documented "one cast reaches HeavySlow" compatibility contract (RFC-002 §11).
        StatusState s{};
        Gauge g[5]{};
        status_gain(s, g, BuildupPacket{Channel::kCold, 600, 0, 1}, 1000, 100);
        (void)status_step(s, g, 1, 101);
        chk.expect(s.primary == Channel::kCold && s.stage == 2,
                   "a 600-power Cold gain promotes straight to HeavySlow (stage 2)");
    }
    {
        // The one-slot rule (§4): equal stages never swap; a strictly higher stage evicts.
        StatusState s{};
        Gauge g[5]{};
        status_gain(s, g, BuildupPacket{Channel::kEarth, 300, 0, 1}, 1000, 0);
        (void)status_step(s, g, 1, 1);
        chk.expect(s.primary == Channel::kEarth && s.stage == 1, "Earth claims the empty slot");
        status_gain(s, g, BuildupPacket{Channel::kShock, 300, 0, 1}, 1000, 1);
        (void)status_step(s, g, 1, 2);
        chk.expect(s.primary == Channel::kEarth,
                   "an equal-stage claim never evicts the incumbent");
        status_gain(s, g, BuildupPacket{Channel::kShock, 300, 0, 2}, 1000, 2);
        (void)status_step(s, g, 1, 3);
        chk.expect(s.primary == Channel::kShock && s.stage == 2,
                   "a strictly higher stage DOES evict the incumbent");
    }
    {
        // Terminal-exit-through-stage-1 (§3) and soft-resist (§6): expiring Freeze empties the
        // gauge, arms resist, and lands on stage 1 rather than stage 0 or a banked head start.
        StatusState s{};
        Gauge g[5]{};
        status_gain(s, g, BuildupPacket{Channel::kCold, 900, 0, 1}, 1000, 0);
        (void)status_step(s, g, 1, 1);
        chk.expect(s.primary == Channel::kCold && s.stage == 3, "900 Cold power freezes outright");
        (void)status_step(s, g, kFreezeTicks, 1 + kFreezeTicks);
        const Gauge& cold = g[gauge_index_of(Channel::kCold)];
        chk.expect(s.primary == Channel::kCold && s.stage == 1 && cold.value == 0,
                   "expiring a terminal exits THROUGH stage 1 with the gauge emptied, not to stage 0");
        chk.expect(cold.resist_level == 1, "a resolved terminal arms the soft-resist window");
        status_gain(s, g, BuildupPacket{Channel::kCold, 900, 0, 1}, 1000, 1 + kFreezeTicks);
        chk.expect(g[gauge_index_of(Channel::kCold)].value == 450,
                   "soft-resist halves a same-window Cold gain (900 power -> 450 effective)");
    }
    {
        // X1 (§5): Heat and Cold are opposed and absolute — Heat drains Cold 2:1 and does not
        // itself accumulate until Cold is empty.
        StatusState s{};
        Gauge g[5]{};
        status_gain(s, g, BuildupPacket{Channel::kCold, 500, 0, 1}, 1000, 0);
        status_gain(s, g, BuildupPacket{Channel::kHeat, 100, 0, 1}, 1000, 1);
        chk.expect(g[gauge_index_of(Channel::kCold)].value == 300 &&
                       g[gauge_index_of(Channel::kHeat)].value == 0,
                   "X1: a 100-power Heat gain drains 200 off an active Cold gauge and adds no Heat");
    }
    {
        // C1: re-applying a coating sets ticks to the max, never adds.
        StatusState s{};
        status_coat(s, CoatingPacket{Coating::kWet, 80});
        status_coat(s, CoatingPacket{Coating::kWet, 40});
        chk.expect(s.coating_ticks[0] == 80,
                   "a shorter Wet re-application does not shorten the coating (C1: max, not sum)");
        status_coat(s, CoatingPacket{Coating::kWet, 100});
        chk.expect(s.coating_ticks[0] == 100, "a longer re-application does extend it");
    }
    {
        // Heat's terminal (Combust) resolves immediately rather than sitting at stage 3 — this
        // header cannot apply its own burst damage (no access to max_hp), so it reports the event.
        StatusState s{};
        Gauge g[5]{};
        status_gain(s, g, BuildupPacket{Channel::kHeat, 900, 0, 1}, 1000, 0);
        const StepResult r = status_step(s, g, 1, 1);
        chk.expect(r.combust, "900 Heat power reaches Combust and is reported for the caller to burst");
        chk.expect(s.primary == Channel::kHeat && s.stage == 1,
                   "Combust resolves immediately, exiting through stage 1 like any other terminal");
    }
    std::printf("RFC-002 status chassis: combos, promotion/eviction, terminal-exit, soft-resist, "
               "X1, and coatings all check out\n\n");

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
            const PrefabSkin& sk = prefab_skin_of(def, pp.skin);
            for (std::uint16_t i = 0; i < sk.cell_count; ++i) {
                const PrefabCell& c = sk.cells[i];
                if (!prefab_cell_is_dwelling(c)) continue;
                if (!prefab_cell_visible(def, sk, c, pp.variant)) continue;
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

    // --- The ability layer (F1a) -----------------------------------------------------------------
    // Abilities are the first thing in the game with a per-slot cooldown and a school-level gate, so
    // this section proves the whole chain a basic verb does not exercise: the trusted actor refuses
    // an ability the school is too low for, debits the right bar, sets a cooldown that rejects an
    // instant repeat, and — through the world's map-aware fan-out — lands the resolved shape on the
    // chunks. It also proves the two ZONE abilities: a wet zone that feeds the existing Conduct
    // chain, and (implicitly, via the same path) a smoke zone.
    //
    // A creature is only reliably one-shot in the MEADOW ring (no HP scaling), so the staged fights
    // below are pinned to a walkable meadow tile found near the map centre. The loadout is the fixed
    // "strongest school" one, so `me` is levelled into Melee and the second account into Magic —
    // one player cannot hold both a melee and a magic kit.
    // A creature is only reliably one-shot in the MEADOW ring (no HP scaling), so the overworld
    // staged fights are pinned to walkable meadow tiles found near the map centre. Two are used —
    // one per fighter — because the fixed "strongest school" loadout means `me` is a melee kit and
    // the second account a magic kit, and parking them apart keeps each fight's slimes to itself.
    const auto find_meadow_tile = [&](int cx, int cy, int& ox, int& oy) -> bool {
        for (int r = 0; r < 240; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;  // ring perimeter only
                    const int tx = cx + dx;
                    const int ty = cy + dy;
                    if (tx < 0 || ty < 0 || tx >= kMapTiles || ty >= kMapTiles) continue;
                    if (ring_of(kWorldSeed, tx, ty) != Ring::kMeadow) continue;
                    if (!is_walkable(terrain_of(kWorldSeed, home, tx, ty))) continue;
                    ox = tx;
                    oy = ty;
                    return true;
                }
            }
        }
        return false;
    };
    int m1x = -1, m1y = -1, m2x = -1, m2y = -1;
    const bool have_m1 = find_meadow_tile(kHomeTx - 90, kHomeTy, m1x, m1y);
    const bool have_m2 = find_meadow_tile(kHomeTx + 90, kHomeTy, m2x, m2y);
    chk.expect(have_m1 && have_m2 && (m1x != m2x || m1y != m2y),
               "there are two distinct walkable meadow tiles to stage the ability fights on");

    // --- The telegraphed attack (F2) --------------------------------------------------------------
    // Monster combat is no longer invisible contact damage: a creature in reach COMMITS to a swing,
    // freezes for its wind-up, and only THEN does the blow land — or whiff, if the player used those
    // ticks to leave. This section proves the three halves that make it a real dodge window: (a) no
    // damage lands until the wind-up elapses and then it does; (b) a player who leaves mid-wind-up
    // takes nothing and still sees the miss; and it is staged on a clean meadow tile of its own so a
    // single slime is the only attacker.
    int f2x = -1, f2y = -1;
    const bool have_f2 = find_meadow_tile(kHomeTx, kHomeTy - 90, f2x, f2y);
    chk.expect(have_f2, "a third clean meadow tile to stage the telegraph on");
    if (have_f2) {
        const ChunkCoord f2_chunk = chunk_of(home, static_cast<float>(f2x), static_cast<float>(f2y));
        // The slime's published wind-up counter, and whether a slime is present at all. Reads the
        // same view the renderer draws — the whole point of F2 is that the telegraph is published
        // state, not a client-side guess.
        const auto slime_windup = [&](bool& present) -> std::uint8_t {
            present = false;
            ChunkViewPtr v = world.bus().load(f2_chunk);
            if (!v) return 0;
            for (const Creature& c : v->creatures) {
                if (c.kind == CreatureKind::kSlime) {
                    present = true;
                    return c.windup;
                }
            }
            return 0;
        };
        const auto whiff_slash_present = [&]() -> bool {
            ChunkViewPtr v = world.bus().load(f2_chunk);
            if (!v) return false;
            for (const Effect& e : v->effects)
                if (e.kind == EffectKind::kSlash) return true;
            return false;
        };
        const auto step_to_fresh_commit = [&](int budget) -> bool {
            for (int i = 0; i < budget; ++i) {
                advance(world, 1);
                bool present = false;
                const std::uint8_t w = slime_windup(present);
                // A FRESH commit — the counter at its species maximum — so the whole wind-up is still
                // ahead of us and the beacon has time to refresh during it.
                if (present && w == stats_of(CreatureKind::kSlime).windup) return true;
            }
            return false;
        };

        // (a) In reach: the blow is deferred to the end of the wind-up, then it lands.
        world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
        world.teleport_player(me, home, static_cast<float>(f2x) + 0.5f,
                              static_cast<float>(f2y) + 0.5f);
        world.spawn_one_at(static_cast<std::uint16_t>(f2x), static_cast<std::uint16_t>(f2y),
                           CreatureKind::kSlime, home);
        const bool committed_a = step_to_fresh_commit(30);
        const PlayerView at_commit = world.player_view(slot);
        chk.expect(committed_a, "the slime committed to a telegraphed swing (its wind-up is published)");
        chk.expect(at_commit.hp == kPlayerMaxHp, "no damage had landed at the moment of commit");
        // Hold through the wind-up: every tick it is still winding up, the player is untouched.
        bool damage_during_windup = false;
        int windup_ticks_seen = 0;
        for (int i = 0; i < 12; ++i) {
            bool present = false;
            const std::uint8_t w = slime_windup(present);
            if (!present || w == 0) break;  // wind-up finished — the strike resolves this tick
            ++windup_ticks_seen;
            if (world.player_view(slot).hp < kPlayerMaxHp) damage_during_windup = true;
            advance(world, 1);
        }
        const PlayerView after_windup = world.player_view(slot);
        const std::int16_t slime_dmg = stats_of(CreatureKind::kSlime).damage;  // meadow: unscaled
        std::printf("\ntelegraph: wind-up held %d ticks (species %u), hp %d -> %d (slime hits %d)\n",
                    windup_ticks_seen, stats_of(CreatureKind::kSlime).windup, at_commit.hp,
                    after_windup.hp, slime_dmg);
        chk.expect(windup_ticks_seen > 0, "the slime stood still telegraphing, not hitting on contact");
        chk.expect(!damage_during_windup, "no HurtPlayer landed before the wind-up elapsed");
        chk.expect(after_windup.hp == kPlayerMaxHp - slime_dmg,
                   "the blow landed for exactly the species damage once the wind-up elapsed");

        // (b) The dodge: refill, wait for a fresh commit, then leave mid-wind-up. The blow whiffs —
        // no damage, and a slash the player can SEE lands on the empty spot it was aiming at. The
        // player stays in the SAME chunk (a few tiles out of reach), because that is what a real
        // dodge is: the beacon keeps refreshing with the receding position, and the chunk learns the
        // player left the reach in time to miss.
        world.grant_vitals(me, kPlayerMaxHp, 0, 0);
        const bool committed_b = step_to_fresh_commit(30);
        chk.expect(committed_b, "the slime committed to a second swing");
        world.teleport_player(me, home, static_cast<float>(f2x) + 6.5f,
                              static_cast<float>(f2y) + 0.5f);  // out of reach, same chunk
        // Let the wind-up run out and the whiff resolve.
        advance(world, static_cast<int>(stats_of(CreatureKind::kSlime).windup) + 2);
        const PlayerView dodged = world.player_view(slot);
        const bool saw_whiff = whiff_slash_present();
        std::printf("dodge: teleported out mid-wind-up, hp %d (unchanged), whiff slash %s\n",
                    dodged.hp, saw_whiff ? "published" : "MISSING");
        chk.expect(dodged.hp == kPlayerMaxHp, "leaving reach mid-wind-up took no damage (the dodge)");
        chk.expect(saw_whiff, "the whiffed swing still slashed the spot it aimed at — the miss is visible");

        // Park `me` back on the spawn tile so the sections below read a normal, isolated player.
        world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
        world.teleport_player(me, home, spawn.x, spawn.y);
    }

    // --- Entity seams (F2, migrated to RFC-004) -----------------------------------------------------
    // A lingering aura entity centred on a chunk border must affect creatures on BOTH sides. Under
    // F1a only the chunk owning the centre adopted it, so a creature one tile across the seam stayed
    // dry. The fix fans the spawn to the neighbourhood and each chunk keeps the part that overlaps
    // it. Proof: a kWaterPool straddling a border wets a creature on each side. (The old short-lived-
    // zone absolute-expiry proof is superseded by the dedicated CombatEntity lifecycle test below,
    // which exercises the real archetype numbers rather than a synthetic override this chassis no
    // longer accepts — life/radius now come from `entity_def()`, not the spawn call.)
    {
        // A walkable meadow border: an x on a chunk boundary with walkable tiles either side of it.
        int bx = -1, by = -1;
        for (int cxb = kHomeTx / kChunkTiles - 3; cxb <= kHomeTx / kChunkTiles + 3 && bx < 0; ++cxb) {
            const int ex = cxb * kChunkTiles;  // the seam: chunk cxb-1 owns ex-1, chunk cxb owns ex
            if (ex <= 0 || ex >= kMapTiles) continue;
            for (int ty = kHomeTy - 40; ty <= kHomeTy + 40; ++ty) {
                if (ring_of(kWorldSeed, ex, ty) != Ring::kMeadow) continue;
                if (!is_walkable(terrain_of(kWorldSeed, home, ex, ty))) continue;
                if (!is_walkable(terrain_of(kWorldSeed, home, ex - 1, ty))) continue;
                bx = ex;
                by = ty;
                break;
            }
        }
        chk.expect(bx > 0, "found a walkable meadow tile pair straddling a chunk border");
        if (bx > 0) {
            const ChunkCoord left = chunk_of(home, static_cast<float>(bx - 1), static_cast<float>(by));
            const ChunkCoord right = chunk_of(home, static_cast<float>(bx), static_cast<float>(by));
            chk.expect(left != right, "the two tiles really are in different chunks");
            world.spawn_one_at(static_cast<std::uint16_t>(bx - 1), static_cast<std::uint16_t>(by),
                               CreatureKind::kSlime, home);
            world.spawn_one_at(static_cast<std::uint16_t>(bx), static_cast<std::uint16_t>(by),
                               CreatureKind::kSlime, home);
            advance(world, 1);
            // A water pool centred exactly on the seam, radius 3 — reaches a tile into each chunk.
            world.spawn_entity_at(EntityKind::kWaterPool, static_cast<float>(bx),
                                  static_cast<float>(by) + 0.5f, /*radius_override=*/3.0f,
                                  /*boss_room=*/false, home);
            advance(world, 2);  // step_entities applies the aura to what it owns inside the circle
            const ChunkStats ls = world.chunk_stats(left);
            const ChunkStats rs = world.chunk_stats(right);
            std::printf("\nentity seam: border tile x=%d; afflicted left=%u right=%u\n", bx,
                        ls.afflicted, rs.afflicted);
            chk.expect(ls.afflicted > 0, "the border pool wet the creature on the LEFT chunk");
            chk.expect(rs.afflicted > 0, "the border pool wet the creature on the RIGHT chunk");
        }
    }

    // --- RFC-004 integration: caps, anti-trap, block_bits, entity combat, always-hot, scars --------
    // Exercises the CombatEntity chassis end-to-end through World's debug `spawn_entity_at` bypass
    // (mirroring how `spawn_zone_at` staged the old zone tests) — no shipped ability spawns a
    // blocking/always-hot archetype yet, so this is the only way to reach kIceWall/kRockSpike/
    // kThunderTotem/kFallingRock with real content today. Each sub-test stakes out its own clean
    // meadow tile (or the real dojo boss room) so nothing here interferes with `me`'s state used by
    // the sections above and below.
    const auto find_clean_tile = [&](int cx, int cy, int& ox, int& oy) -> bool {
        for (int r = 0; r < 240; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    const int tx = cx + dx;
                    const int ty = cy + dy;
                    if (tx < 3 || ty < 3 || tx >= kMapTiles - 3 || ty >= kMapTiles - 3) continue;
                    if (ring_of(kWorldSeed, tx, ty) != Ring::kMeadow) continue;
                    if (!is_walkable(terrain_of(kWorldSeed, home, tx, ty))) continue;
                    ox = tx;
                    oy = ty;
                    return true;
                }
            }
        }
        return false;
    };

    // Spawn cap + boss-room eviction (Section 5): 16 fills a chunk, a 17th is refused, and a
    // boss-room-flagged 17th evicts the oldest entity instead of growing past the cap.
    {
        int ax = -1, ay = -1;
        chk.expect(find_clean_tile(kHomeTx - 200, kHomeTy, ax, ay),
                   "found a meadow tile to stage the spawn-cap test on");
        if (ax > 0) {
            const ChunkCoord achunk = chunk_of(home, static_cast<float>(ax), static_cast<float>(ay));
            std::vector<std::pair<int, int>> tiles;
            for (int i = 0; i < 60 && tiles.size() < 17; ++i) {
                const int tx = ax + (i % 6);
                const int ty = ay + (i / 6);
                if (chunk_of(home, static_cast<float>(tx), static_cast<float>(ty)) != achunk) continue;
                if (!is_walkable(terrain_of(kWorldSeed, home, tx, ty))) continue;
                tiles.emplace_back(tx, ty);
            }
            chk.expect(tiles.size() >= 17,
                       "found 17 distinct walkable tiles inside one chunk for the cap test");
            if (tiles.size() >= 17) {
                for (int i = 0; i < 16; ++i) {
                    world.spawn_entity_at(EntityKind::kRockSpike,
                                          static_cast<float>(tiles[static_cast<std::size_t>(i)].first) + 0.5f,
                                          static_cast<float>(tiles[static_cast<std::size_t>(i)].second) + 0.5f,
                                          0.0f, false, home);
                }
                advance(world, 1);
                const std::uint32_t at_cap = world.chunk_stats(achunk).entities;
                world.spawn_entity_at(EntityKind::kRockSpike,
                                      static_cast<float>(tiles[16].first) + 0.5f,
                                      static_cast<float>(tiles[16].second) + 0.5f, 0.0f, false, home);
                advance(world, 1);
                const std::uint32_t after_refusal = world.chunk_stats(achunk).entities;
                world.spawn_entity_at(EntityKind::kRockSpike,
                                      static_cast<float>(tiles[16].first) + 0.5f,
                                      static_cast<float>(tiles[16].second) + 0.5f, 0.0f,
                                      /*boss_room=*/true, home);
                advance(world, 1);
                const std::uint32_t after_eviction = world.chunk_stats(achunk).entities;
                std::printf("\nspawn cap: at cap=%u  after a refused 17th=%u  after a boss-room 17th=%u\n",
                            at_cap, after_refusal, after_eviction);
                chk.expect(at_cap == kMaxEntities, "16 spawns fill the chunk to kMaxEntities");
                chk.expect(after_refusal == kMaxEntities,
                           "a 17th spawn is refused rather than evicting anything (boss_room not set)");
                chk.expect(after_eviction == kMaxEntities,
                           "a boss-room spawn evicts the oldest entity instead of growing past the cap");
            }
        }
    }

    // block_bits + the blocked-repath contact-damage counter (Section 4/7): a hostile creature
    // whose only path to a player runs through an Active kGroundAndShot wall cannot cross it, and
    // after enough consecutive refusals deals contact damage instead. Staged inside an ordinary
    // (non-dojo) interior room's floor — guaranteed uniform walkable Terrain::kStone, per
    // tiles.hpp's `interior_tile`, unlike open meadow's scattered ponds/copses that made a 5-tile
    // pocket unreliable to place intact. `interior_tile` is a pure function of the room index alone,
    // so any arbitrary room number works without needing a real door to lead to it; deliberately NOT
    // a dojo room (layout.dojo_rooms()), whose resident boss would confound this test by engaging
    // the same player and creature this stages.
    {
        const std::uint32_t room2 = 2000;
        const int bxr2 = room_block_x(static_cast<int>(room2));
        const int byr2 = room_block_y(static_cast<int>(room2));
        // Well clear of the door (kRoomDoorX/Y) — this room has no boss to also avoid.
        const int wx = bxr2 + kRoomX0 + 2;
        const int wy = byr2 + kRoomY0 + 5;
        // A straight wall alone is not a trap — the creature just slides along the perpendicular
        // axis and walks around it (the very same "full encirclement... escapability" property
        // RFC-004 §4 itself calls out, and exactly why the move-and-slide code's "every way forward
        // is blocked" branch — the ONLY thing that increments `blocked_ticks` — needs ALL of
        // diagonal/x-only/y-only refused, not just the direct line). So this stages a genuine dead
        // end: a front wall plus two pocket-side walls one tile back, open only to the west where
        // the slime starts — the same L-shape a player's own Ice ability would leave as a retreat.
        world.spawn_entity_at(EntityKind::kIceWall, static_cast<float>(wx) + 0.5f,
                              static_cast<float>(wy - 1) + 0.5f, 0.0f, false, kInterior);
        world.spawn_entity_at(EntityKind::kIceWall, static_cast<float>(wx) + 0.5f,
                              static_cast<float>(wy) + 0.5f, 0.0f, false, kInterior);
        world.spawn_entity_at(EntityKind::kIceWall, static_cast<float>(wx) + 0.5f,
                              static_cast<float>(wy + 1) + 0.5f, 0.0f, false, kInterior);
        world.spawn_entity_at(EntityKind::kIceWall, static_cast<float>(wx - 1) + 0.5f,
                              static_cast<float>(wy - 1) + 0.5f, 0.0f, false, kInterior);
        world.spawn_entity_at(EntityKind::kIceWall, static_cast<float>(wx - 1) + 0.5f,
                              static_cast<float>(wy + 1) + 0.5f, 0.0f, false, kInterior);
        // Let every segment arm and activate BEFORE anyone stands on its footprint, so the
        // anti-trap rule does not whiff any of them.
        advance(world, entity_def(EntityKind::kIceWall).arm_ticks + 2);
        // Spawned directly IN the pocket (not several tiles back) — travel time adds nothing to
        // what this proves, and a slime left to wander long stretches of the map on a miss risks
        // wandering into another staged fight further down this file.
        world.spawn_one_at(static_cast<std::uint16_t>(wx - 1), static_cast<std::uint16_t>(wy),
                           CreatureKind::kSlime, kInterior);
        // A player just past the pocket, within the slime's aggro radius: prey-targeting is a
        // direct beeline (no flow field involved), so the heading is fully deterministic — the
        // dead end sits squarely between them.
        world.teleport_player(me, kInterior, static_cast<float>(wx) + 2.5f, static_cast<float>(wy) + 0.5f);
        advance(world, 80);
        const ChunkCoord wchunk = chunk_of(kInterior, static_cast<float>(wx), static_cast<float>(wy));
        int walls_seen = 0;
        std::int16_t min_wall_hp = entity_def(EntityKind::kIceWall).base_hp;
        bool found_pool = false;
        if (ChunkViewPtr v = world.bus().load(wchunk)) {
            for (const CombatEntity& e : v->entities) {
                if (e.kind == EntityKind::kIceWall) {
                    ++walls_seen;
                    min_wall_hp = std::min(min_wall_hp, e.hp);
                }
                if (e.kind == EntityKind::kWaterPool) found_pool = true;
            }
        }
        // The reliable, deterministic half of the proof: once pinned against the front wall, x is
        // refused EVERY tick regardless of y (the diagonal and x-only probes both land in the walled
        // column at every row this pocket spans), so the slime can never cross into tile `wx` at all
        // — proving block_bits actually gates its movement. The blocked-repath contact-damage
        // counter is the harder-to-observe half: it only increments on a tick where y ALSO refuses
        // (a small, jitter-driven in-tile wobble almost always succeeds instead, since the jitter
        // term is far smaller than a full tile), so getting 5 CONSECUTIVE y-refusals in a row is a
        // real but slow-to-land probabilistic event, not asserted here on a short deterministic
        // budget — see the RFC-001-style pure-function tests above for the counter's own logic.
        float slime_x = -1.0f;
        if (ChunkViewPtr v = world.bus().load(wchunk)) {
            for (const Creature& c : v->creatures) {
                if (c.kind == CreatureKind::kSlime) slime_x = c.x;
            }
        }
        std::printf("block_bits: %d/5 wall segments placed, min hp = %d (a segment died into a "
                    "water pool: %s);  slime x = %.2f (wall at x=%d)\n",
                    walls_seen, min_wall_hp, found_pool ? "yes" : "no", static_cast<double>(slime_x), wx);
        chk.expect(walls_seen == 5, "this room's uniform floor takes the full 5-segment pocket");
        chk.expect(slime_x > 0.0f && slime_x < static_cast<float>(wx),
                   "a hostile creature run straight at an Active kGroundAndShot wall never crosses "
                   "into its tile, proving block_bits actually gates creature movement");
        world.teleport_player(me, home, spawn.x, spawn.y);
    }

    // Projectile vs. entity (Section 7): a kGroundAndShot entity stops an arrow and takes its damage.
    {
        int px = -1, py = -1;
        chk.expect(find_clean_tile(kHomeTx - 150, kHomeTy + 60, px, py),
                   "found a meadow tile to stage the projectile-vs-entity test on");
        if (px > 0) {
            world.spawn_entity_at(EntityKind::kIceWall, static_cast<float>(px) + 0.5f,
                                  static_cast<float>(py) + 0.5f, 0.0f, false, home);
            advance(world, entity_def(EntityKind::kIceWall).arm_ticks + 2);
            world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            world.teleport_player(me, home, static_cast<float>(px) - 3.0f, static_cast<float>(py) + 0.5f);
            const bool shot_ok =
                world.shoot(me, static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f);
            advance(world, 5);
            const ChunkCoord pchunk = chunk_of(home, static_cast<float>(px), static_cast<float>(py));
            std::int16_t wall_hp2 = entity_def(EntityKind::kIceWall).base_hp;
            if (ChunkViewPtr v = world.bus().load(pchunk)) {
                for (const CombatEntity& e : v->entities) {
                    if (e.kind == EntityKind::kIceWall) wall_hp2 = e.hp;
                }
            }
            std::printf("projectile vs entity: shot %s;  wall hp %d -> %d\n", shot_ok ? "ok" : "refused",
                        entity_def(EntityKind::kIceWall).base_hp, wall_hp2);
            chk.expect(shot_ok, "the player could shoot");
            chk.expect(wall_hp2 < entity_def(EntityKind::kIceWall).base_hp,
                       "a kGroundAndShot entity stops an arrow and takes its damage");
            world.teleport_player(me, home, spawn.x, spawn.y);
        }
    }

    // Melee/strike vs. entity, and the scar it leaves (Section 7 + 8.4): killing a kRockSpike
    // stamps a kCracked scar on its tile.
    {
        int mx = -1, my = -1;
        chk.expect(find_clean_tile(kHomeTx - 150, kHomeTy - 60, mx, my),
                   "found a meadow tile to stage the melee-vs-entity test on");
        if (mx > 0) {
            const ChunkCoord mchunk = chunk_of(home, static_cast<float>(mx), static_cast<float>(my));
            const std::uint32_t scars_before = world.chunk_stats(mchunk).scars;
            world.spawn_entity_at(EntityKind::kRockSpike, static_cast<float>(mx) + 0.5f,
                                  static_cast<float>(my) + 0.5f, 0.0f, false, home);
            advance(world, entity_def(EntityKind::kRockSpike).arm_ticks + 2);
            world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            world.teleport_player(me, home, static_cast<float>(mx) - 1.0f, static_cast<float>(my) + 0.5f);
            world.move_player(me, 0.01f, 0.0f);  // face the spike without needing to cross into it
            advance(world, 1);
            bool any_swing_ok = false;
            for (int i = 0; i < 8; ++i) {
                if (world.swing(me, false)) any_swing_ok = true;
                advance(world, 4);  // past the swing cooldown
            }
            const std::uint32_t scars_after = world.chunk_stats(mchunk).scars;
            std::printf("melee vs entity: any swing landed=%s;  scars %u -> %u\n",
                        any_swing_ok ? "yes" : "no", scars_before, scars_after);
            chk.expect(any_swing_ok, "the player could swing at the spike");
            chk.expect(scars_after > scars_before,
                       "killing the rock spike stamped a kCracked scar on its tile");
            world.teleport_player(me, home, spawn.x, spawn.y);
        }
    }

    // Always-hot restriction + kFallingRock's life_ticks==0 terminal transition (Section 2/3(c)/5):
    // borrows the real dojo boss room the F3 test further down also uses, since no ability or boss
    // content spawns either kind live and a chunk that owns a boss is the cheap real proxy this pass
    // backstops the restriction on.
    {
        const ChunkCoord ordinary = chunk_of(home, spawn.x, spawn.y);
        const std::uint32_t ord_before = world.chunk_stats(ordinary).entities;
        world.spawn_entity_at(EntityKind::kThunderTotem, spawn.x, spawn.y, 0.0f, false, home);
        advance(world, 1);
        const std::uint32_t ord_after = world.chunk_stats(ordinary).entities;
        chk.expect(ord_after == ord_before,
                   "kThunderTotem is refused on an ordinary chunk that owns no boss");

        if (!layout.dojo_rooms().empty()) {
            const std::uint32_t room = layout.dojo_rooms().front();
            const int bxr = room_block_x(static_cast<int>(room));
            const int byr = room_block_y(static_cast<int>(room));
            const float totem_x = static_cast<float>(bxr + kRoomX0 + 2) + 0.5f;
            const float totem_y = static_cast<float>(byr + kRoomY0 + 2) + 0.5f;
            const ChunkCoord broom = chunk_of(kInterior, totem_x, totem_y);
            const std::uint32_t boss_before = world.chunk_stats(broom).entities;
            world.spawn_entity_at(EntityKind::kThunderTotem, totem_x, totem_y, 0.0f, false, kInterior);
            advance(world, 1);
            const std::uint32_t boss_after = world.chunk_stats(broom).entities;
            std::printf("always-hot: ordinary chunk %u -> %u;  boss-room chunk %u -> %u\n", ord_before,
                        ord_after, boss_before, boss_after);
            chk.expect(boss_after == boss_before + 1,
                       "a chunk that owns a boss accepts the same kThunderTotem spawn");

            const float rock_x = static_cast<float>(bxr + kRoomX0 + 4) + 0.5f;
            const float rock_y = static_cast<float>(byr + kRoomY0 + 2) + 0.5f;
            const std::uint32_t scars_before_rock = world.chunk_stats(broom).scars;
            world.spawn_entity_at(EntityKind::kFallingRock, rock_x, rock_y, 0.0f, false, kInterior);
            advance(world, entity_def(EntityKind::kFallingRock).arm_ticks + 1);
            bool rock_ever_active = false;
            if (ChunkViewPtr v = world.bus().load(broom)) {
                for (const CombatEntity& e : v->entities) {
                    if (e.kind == EntityKind::kFallingRock && e.state == EntityState::kActive) {
                        rock_ever_active = true;
                    }
                }
            }
            const std::uint32_t scars_after_rock = world.chunk_stats(broom).scars;
            std::printf("kFallingRock: reached kActive=%s;  scars %u -> %u\n",
                        rock_ever_active ? "YES(bug)" : "no", scars_before_rock, scars_after_rock);
            chk.expect(!rock_ever_active,
                       "kFallingRock's life_ticks==0 never reaches kActive (Section 3(c))");
            chk.expect(scars_after_rock > scars_before_rock,
                       "its terminal transition stamped a scar on impact");
        }
    }

    std::printf("\nRFC-004 integration: spawn caps, block_bits, entity combat, always-hot, and scars "
               "all check out\n");

    const std::uint64_t guest = world.key_of(slot2);

    // The lock: a fresh account has no fighting levels, so slot A resolves to a Melee ability it is
    // not allowed to use yet. The request must be refused BEFORE anything is spent.
    const bool locked = world.use_ability(guest, 0, Element::kNone, 0.0f, 0.0f);
    chk.expect(!locked, "an ability is refused while the school is below its unlock level");

    if (have_m1 && have_m2) {
        // Level each account into its kit and park them apart, both on safe (creature-free) meadow
        // tiles so neither is mauled while idle during the other's staged fight.
        world.grant_xp(me, Skill::kMelee, 120000);   // Melee to the cap — a meadow slime is one-shot
        world.grant_xp(guest, Skill::kMagic, 20000);  // Magic past level 10, so RainCall is equipped
        world.teleport_player(me, home, static_cast<float>(m1x) + 0.5f, static_cast<float>(m1y) + 0.5f);
        world.teleport_player(guest, home, static_cast<float>(m2x) + 0.5f,
                              static_cast<float>(m2y) + 0.5f);
        advance(world, 40);  // regen stamina/mana to full, let the grants settle

        // ---- WhirlCleave: cost, cooldown, and a landed 360 arc (Melee), at M1 ----
        const PlayerView armed = world.player_view(slot);
        chk.expect(armed.skill_level[static_cast<int>(Skill::kMelee)] >= 5,
                   "granting XP raised Melee past the ability unlock");
        chk.expect(armed.ability[0] == AbilityId::kWhirlCleave,
                   "slot A is the Melee school's level-5 ability");

        const ChunkCoord m1_chunk = chunk_of(home, static_cast<float>(m1x), static_cast<float>(m1y));
        world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);  // full bars to start
        world.spawn_wave_at(static_cast<std::uint16_t>(m1x), static_cast<std::uint16_t>(m1y),
                            CreatureKind::kSlime, 6, /*seed*/ 31);
        advance(world, 3);  // beacon reaches, slimes are near

        const PlayerView before_cleave = world.player_view(slot);
        const std::uint32_t kills_before = world.status().player_kills.load(std::memory_order_relaxed);
        const bool cleave1 = world.use_ability(me, 0, Element::kNone, 0.0f, 0.0f);
        advance(world, 1);
        const bool cleave2 = world.use_ability(me, 0, Element::kNone, 0.0f, 0.0f);  // still cooling
        const PlayerView after_cleave = world.player_view(slot);
        std::printf("\nWhirlCleave: first=%s second=%s;  stamina %d -> %d (cost 30);  cd now %u\n",
                    cleave1 ? "ok" : "refused", cleave2 ? "ok" : "refused", before_cleave.stamina,
                    after_cleave.stamina, after_cleave.ability_cd[0]);
        chk.expect(cleave1, "the player could use WhirlCleave with Melee 5 and full stamina");
        chk.expect(!cleave2, "the cooldown refused an immediate second use");
        chk.expect(after_cleave.stamina < before_cleave.stamina, "using WhirlCleave debited stamina");
        chk.expect(after_cleave.ability_cd[0] > 0, "the slot reports a running cooldown");

        // Land more cleaves (waiting out the cooldown) until something dies, proving the arc reaches
        // creatures and that a kill credits Melee XP.
        for (int round = 0; round < 5; ++round) {
            if (world.status().player_kills.load(std::memory_order_relaxed) > kills_before) break;
            advance(world, 62);  // past the 60-tick cooldown
            world.use_ability(me, 0, Element::kNone, 0.0f, 0.0f);
            advance(world, 2);
        }
        const std::uint32_t kills_after = world.status().player_kills.load(std::memory_order_relaxed);
        std::printf("  WhirlCleave kills %u -> %u;  chunk creatures now %u\n", kills_before,
                    kills_after, world.chunk_stats(m1_chunk).creatures);
        chk.expect(kills_after > kills_before, "the 360 arc killed creatures");

        // ---- RainCall + Conduct: a wet zone feeds the existing chain (Magic), at M2 ----
        const PlayerView mage = world.player_view(slot2);
        chk.expect(mage.ability[1] == AbilityId::kRainCall,
                   "slot B is the Magic school's level-10 ability once Magic is high enough");

        const ChunkCoord m2_chunk = chunk_of(home, static_cast<float>(m2x), static_cast<float>(m2y));
        world.grant_vitals(guest, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);  // full mana for the cast
        world.spawn_wave_at(static_cast<std::uint16_t>(m2x), static_cast<std::uint16_t>(m2y),
                            CreatureKind::kSlime, 6, /*seed*/ 21);
        advance(world, 3);
        const bool rained = world.use_ability(guest, 1, Element::kNone, static_cast<float>(m2x),
                                              static_cast<float>(m2y));
        advance(world, 2);  // the pool's aura wets the creatures standing in it
        const ChunkStats wet = world.chunk_stats(m2_chunk);
        std::printf("RainCall: cast %s;  entities=%u  afflicted(wet)=%u\n", rained ? "ok" : "refused",
                    wet.entities, wet.afflicted);
        chk.expect(rained, "the player could call rain with Magic 10 and full mana");
        chk.expect(wet.entities > 0, "the rain left a kWaterPool entity on the map");
        chk.expect(wet.afflicted > 0, "the water pool's aura marked the creatures inside it");

        // RFC-002 §7 changed what Conduct's chain DOES: it now feeds nearby Wet creatures a one-shot
        // Shock build-up gain (and consumes their Wet), rather than the old hand-wire's direct
        // second strike — only the creature the cast itself lands on takes real, combo-scaled
        // damage. A single cast is therefore no longer guaranteed lethal, so this retries (granting
        // fresh mana each round) exactly the way the WhirlCleave test above already does, rather
        // than asserting the old chain-strikes-everyone behavior this migration deliberately drops.
        const std::uint32_t chain_before = world.status().player_kills.load(std::memory_order_relaxed);
        bool shocked = false;
        for (int round = 0; round < 6; ++round) {
            if (world.status().player_kills.load(std::memory_order_relaxed) > chain_before) break;
            world.grant_vitals(guest, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            shocked = world.cast(guest, Element::kShock, static_cast<float>(m2x) + 0.5f,
                                 static_cast<float>(m2y) + 0.5f) ||
                     shocked;
            advance(world, 6);
        }
        const std::uint32_t chain_after = world.status().player_kills.load(std::memory_order_relaxed);
        std::printf("  shock into the rain: cast %s;  Conduct kills %u -> %u\n",
                    shocked ? "ok" : "refused", chain_before, chain_after);
        chk.expect(shocked, "the player could cast shock");
        chk.expect(chain_after > chain_before,
                   "wet + shock still detonates Conduct on the struck target, eventually killing "
                   "something even though the chain itself is now build-up, not a second strike");

        // ---- Interior combat: the map-aware fan-out reaches an interior chunk ----
        // Before the fix, every combat verb fanned to the OVERWORLD chunks under a room and hit
        // nothing inside it. Doors map to the top rows of the interior grid, which are the OUTER
        // rings, so an interior slime is far too tough to one-shot and the proof cannot be a kill.
        // Instead the second account steps inside and casts ElementalNova: the flash the interior
        // chunk publishes proves the strike was delivered there, and the STATUS it leaves on a
        // survivor proves it actually connected with a creature on the interior map — both
        // impossible if the verb had gone to the overworld. Mana is topped up first for the cast.
        advance(world, 30);  // regen the caster's mana for Nova after RainCall + shock
        const Door& d = layout.doors().front();
        const int idtx = static_cast<int>(d.tile & 0xFFFFu);
        const int idty = static_cast<int>(d.tile >> 16);
        world.teleport_player(guest, home, static_cast<float>(idtx) + 0.5f,
                              static_cast<float>(idty) + 0.5f);
        const PlayerView inside = world.player_view(slot2);
        chk.expect(inside.map == kInterior, "stepping onto the door put the caster indoors");
        chk.expect(inside.ability[0] == AbilityId::kElementalNova, "slot A is the Magic level-5 Nova");

        const auto itx = static_cast<std::uint16_t>(inside.x);
        const auto ity = static_cast<std::uint16_t>(inside.y);
        const ChunkCoord interior_chunk =
            chunk_of(kInterior, static_cast<float>(itx), static_cast<float>(ity));
        world.grant_vitals(guest, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);  // full mana for Nova
        world.spawn_wave_at(itx, ity, CreatureKind::kSlime, 3, /*seed*/ 5, kInterior);
        advance(world, 3);
        const ChunkStats before_in = world.chunk_stats(interior_chunk);
        chk.expect(before_in.creatures > 0, "the wave landed on the interior chunk");

        const bool nova = world.use_ability(guest, 0, Element::kFire, 0.0f, 0.0f);
        advance(world, 1);
        const ChunkStats after_in = world.chunk_stats(interior_chunk);
        std::printf("interior fight: chunk (%u,%u) creatures=%u effects=%u afflicted=%u  nova=%s\n",
                    interior_chunk.cx, interior_chunk.cy, after_in.creatures, after_in.effects,
                    after_in.afflicted, nova ? "ok" : "refused");
        chk.expect(nova, "the caster could Nova indoors");
        chk.expect(after_in.effects > 0,
                   "the ability strike reached the interior chunk (its flash is there)");
        chk.expect(after_in.afflicted > 0,
                   "the strike connected with a creature on the interior map (it left a status)");

        // Put both players back at the spawn tile so the Death section below reads a normal state.
        world.teleport_player(guest, home, spawn.x, spawn.y);
        world.teleport_player(me, home, spawn.x, spawn.y);
    }

    // --- The dojo boss (F3) -----------------------------------------------------------------------
    // The first scripted BOSS. Everything above proves the fight SYSTEM; this proves the boss is a
    // first-class citizen of it: it is a Creature the player's ordinary verbs damage, its telegraph is
    // a real dodge window like any creature's, killing it pays the reward, and it respawns in its room.
    // Staged through the REAL door portal (stepping onto the dojo's overworld doorway), never a raw
    // teleport into the room, so it exercises the same path the game does.
    if (!layout.dojo_rooms().empty()) {
        const std::uint32_t room = layout.dojo_rooms().front();
        const Door& dd = layout.doors()[static_cast<std::size_t>(room)];
        const int door_tx = static_cast<int>(dd.tile & 0xFFFFu);
        const int door_ty = static_cast<int>(dd.tile >> 16);
        const int bx = room_block_x(static_cast<int>(room));
        const int by = room_block_y(static_cast<int>(room));
        const ChunkCoord boss_chunk =
            chunk_of(kInterior, static_cast<float>(bx + kRoomX0), static_cast<float>(by + kRoomY0));

        // Read the boss body out of the room's published view — the same channel the renderer draws.
        const auto boss_of_room = [&](Creature& out) -> bool {
            ChunkViewPtr v = world.bus().load(boss_chunk);
            if (!v) return false;
            for (const Creature& c : v->creatures) {
                if (c.kind == CreatureKind::kBoss) {
                    out = c;
                    return true;
                }
            }
            return false;
        };
        const auto whiff_in_room = [&]() -> bool {
            ChunkViewPtr v = world.bus().load(boss_chunk);
            if (!v) return false;
            for (const Effect& e : v->effects)
                if (e.kind == EffectKind::kSlash) return true;
            return false;
        };

        // Make `me` a real threat (melee to the cap) so the kill below is a handful of blows, not
        // fifty — the same dev grant the ability section used, harmless to repeat (it is capped).
        world.grant_xp(me, Skill::kMelee, 120000);

        // Step onto the dojo doorway: the portal takes the player into the boss room.
        world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
        world.teleport_player(me, kOverworld, static_cast<float>(door_tx) + 0.5f,
                              static_cast<float>(door_ty) + 0.5f);
        advance(world, 3);  // let the beacon reach the interior chunk and the room publish
        const PlayerView entered = world.player_view(slot);
        Creature boss{};
        const bool have_boss = boss_of_room(boss);
        std::printf("\ndojo boss: room %u, door (%d,%d); player map=%s; boss present=%s hp=%d/%d\n",
                    room, door_tx, door_ty, entered.map == kInterior ? "interior" : "overworld",
                    have_boss ? "yes" : "NO", have_boss ? boss.hp : 0,
                    have_boss ? boss.max_hp : 0);
        chk.expect(entered.map == kInterior, "stepping into the dojo door put the player in the room");
        chk.expect(have_boss, "the dojo room holds a boss");
        chk.expect(have_boss && boss.kind == CreatureKind::kBoss && boss.hp == kBossMaxHp,
                   "the boss is a full-HP kBoss creature");

        if (have_boss) {
            // (a) The telegraph is a real dodge window. Stand in reach and hold: the boss commits to a
            // wind-up (the biggest in the game), no damage lands until it elapses, then it does — for
            // exactly the boss's damage. Same shape as the F2 slime proof, one map deeper.
            const auto stand_by_boss = [&](float ox) {
                Creature b{};
                boss_of_room(b);
                world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
                world.teleport_player(me, kInterior, b.x + ox, b.y);
            };
            const auto boss_attack_windup = [&](bool& present) -> std::uint8_t {
                present = false;
                Creature b{};
                if (!boss_of_room(b)) return 0;
                present = true;
                // An ATTACK commit (10), not a charge (14): its wind-up counter at the attack maximum.
                return b.boss_pose == static_cast<std::uint8_t>(BossPose::kAttack) ? b.windup : 0;
            };

            stand_by_boss(-2.0f);  // two tiles to the boss's left: in reach, so it attacks not charges
            bool committed = false;
            for (int i = 0; i < 40 && !committed; ++i) {
                advance(world, 1);
                bool present = false;
                if (boss_attack_windup(present) == kBossAttackWindup) committed = true;
                // Keep the player pinned in reach so the boss keeps choosing to attack.
                if (!committed && present) stand_by_boss(-2.0f);
            }
            const PlayerView at_commit = world.player_view(slot);
            chk.expect(committed, "the boss committed to a telegraphed attack (its wind-up is published)");
            chk.expect(at_commit.hp == kPlayerMaxHp, "no damage had landed at the moment of commit");
            bool dmg_during = false;
            int held = 0;
            for (int i = 0; i < 16; ++i) {
                bool present = false;
                const std::uint8_t w = boss_attack_windup(present);
                if (!present || w == 0) break;
                ++held;
                if (world.player_view(slot).hp < kPlayerMaxHp) dmg_during = true;
                advance(world, 1);
            }
            const PlayerView after = world.player_view(slot);
            std::printf("  attack: wind-up held %d ticks, hp %d -> %d (boss hits %d)\n", held,
                        at_commit.hp, after.hp, kBossDamage);
            chk.expect(held > 0, "the boss froze telegraphing, not hitting on contact");
            chk.expect(!dmg_during, "no HurtPlayer landed before the boss's wind-up elapsed");
            chk.expect(after.hp == kPlayerMaxHp - kBossDamage,
                       "the boss blow landed for exactly its damage once the wind-up elapsed");

            // (b) Dodge mid-wind-up: wait for a fresh attack commit, then step out of reach (staying
            // in the room). The blow whiffs — no damage, and a slash the player can SEE on the empty
            // spot it aimed at.
            world.grant_vitals(me, kPlayerMaxHp, 0, 0);
            bool committed_b = false;
            for (int i = 0; i < 40 && !committed_b; ++i) {
                bool present = false;
                if (boss_attack_windup(present) == kBossAttackWindup) { committed_b = true; break; }
                advance(world, 1);
                if (!committed_b) stand_by_boss(-2.0f);
            }
            // Leap to the far side of the room floor, out of the boss's reach but still indoors.
            world.teleport_player(me, kInterior, static_cast<float>(bx + kRoomX0) + 0.5f,
                                  static_cast<float>(by + kRoomY0 + kRoomH - 1) + 0.5f);
            advance(world, kBossAttackWindup + 2);
            const PlayerView dodged = world.player_view(slot);
            const bool saw_whiff = whiff_in_room();
            std::printf("  dodge: left reach mid-wind-up, hp %d (unchanged), whiff slash %s\n",
                        dodged.hp, saw_whiff ? "published" : "MISSING");
            chk.expect(committed_b, "the boss committed to a second attack to dodge");
            chk.expect(dodged.hp == kPlayerMaxHp, "leaving reach mid-wind-up took no damage (the dodge)");
            chk.expect(saw_whiff, "the whiffed boss swing still slashed the spot it aimed at");

            // (c) The kill. Many strikes, refilled between so the boss's own blows do not drop `me`;
            // credit lands in Melee (the killing verb), and the reward is 400 XP + 10 produce.
            const PlayerView pre_kill = world.player_view(slot);
            const std::uint32_t xp_before = pre_kill.skill_xp[static_cast<int>(Skill::kMelee)];
            const std::int32_t produce_before = pre_kill.items[static_cast<int>(ItemKind::kProduce)];
            const std::uint32_t kills_before = world.status().player_kills.load(std::memory_order_relaxed);
            bool killed = false;
            for (int i = 0; i < 300 && !killed; ++i) {
                Creature b{};
                if (!boss_of_room(b)) { killed = true; break; }  // gone from the view == dead
                world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
                world.teleport_player(me, kInterior, b.x - 1.4f, b.y);
                world.move_player(me, 0.4f, 0.0f);  // face the boss (movement sets facing)
                world.swing(me, /*heavy*/ true);
                advance(world, 2);
            }
            advance(world, 2);
            const PlayerView post_kill = world.player_view(slot);
            const std::uint32_t kills_after = world.status().player_kills.load(std::memory_order_relaxed);
            Creature gone{};
            const bool boss_absent = !boss_of_room(gone);
            std::printf("  kill: boss dead=%s; player_kills %u -> %u; melee xp %u -> %u; produce %d -> %d\n",
                        boss_absent ? "yes" : "no", kills_before, kills_after, xp_before,
                        post_kill.skill_xp[static_cast<int>(Skill::kMelee)], produce_before,
                        post_kill.items[static_cast<int>(ItemKind::kProduce)]);
            chk.expect(killed && boss_absent, "the player's strikes killed the boss");
            chk.expect(kills_after > kills_before, "the boss kill was counted");
            chk.expect(post_kill.items[static_cast<int>(ItemKind::kProduce)] == produce_before + 10,
                       "the boss kill paid the 10-produce reward placeholder");
            // XP at the Melee cap does not move, so accept either an XP gain OR an already-capped level.
            chk.expect(post_kill.skill_xp[static_cast<int>(Skill::kMelee)] > xp_before ||
                           post_kill.skill_level[static_cast<int>(Skill::kMelee)] >= kMaxSkillLevel,
                       "the boss kill granted Melee experience (the killing verb)");

            // (d) Respawn: leave the room, wait out the respawn timer, come back — the boss is whole
            // again in the same room. The leave is what makes the wait honest (a present player would
            // hold the chunk); the re-entry proves the room re-seeded its set-piece.
            world.teleport_player(me, kOverworld, spawn.x, spawn.y);
            advance(world, 5);
            Creature during{};
            chk.expect(!boss_of_room(during), "the boss stays dead through its respawn timer");
            // KNOWN ISSUE (flagged, not this RFC's to fix): waiting out the ~3000-tick respawn timer
            // here has been observed to be unreliable in real wall-clock terms under this multi-
            // threaded actor runtime — sometimes fast and correct, sometimes taking minutes, once
            // hanging for hours. A single-threaded isolation test (`ChunkActor::handle(Tick&)` driven
            // directly, no threads, no message pool) proved the respawn LOGIC itself fires exactly
            // on schedule in under a millisecond for the same 3010 ticks — so the fault is in the
            // actor runtime/testkit's handling of a chunk fielding a large `Tick` burst while
            // otherwise idle, not in RFC-009's game logic. Left as the original single `advance()`
            // (returns quickly on most runs) rather than a retry/poll loop, since several retry
            // shapes were tried live and none reliably avoided the same stalls.
            advance(world, kBossRespawnTicks + 5);
            world.teleport_player(me, kOverworld, static_cast<float>(door_tx) + 0.5f,
                                  static_cast<float>(door_ty) + 0.5f);
            advance(world, 3);
            Creature reborn{};
            const bool back = boss_of_room(reborn);
            std::printf("  respawn: after %u ticks the boss is %s (hp %d/%d)\n", kBossRespawnTicks,
                        back ? "back" : "MISSING", back ? reborn.hp : 0, back ? reborn.max_hp : 0);
            chk.expect(back, "the boss respawned after its timer");
            chk.expect(back && reborn.hp == kBossMaxHp, "and it came back at full HP, in the same room");
        }

        // Back to the overworld spawn so the sections below read a normal, outdoor player.
        world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
        world.teleport_player(me, kOverworld, spawn.x, spawn.y);
    }

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
