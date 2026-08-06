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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "world/combat_ids.hpp"
#include "world/combat_pack.hpp"
#include "world/gate_sidecar.hpp"
#include "world/loot.hpp"
#include "world/map_system.hpp"
#include "world/replication.hpp"
#include "world/reward.hpp"
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

// --- RFC-017 §2: `--sweep`, the payload-vs-material effective-channel table ------------------------
//
// Every number below comes from a REAL production function (combat_math.hpp's `resolve_damage`/
// `mult_pm_of`, physics.hpp's `transmit_impulse`/`knockback_tiles`, status.hpp's own `status_gain`)
// — never a second, hand-copied formula. See those headers' own divergence notes for what each
// formula does and does not model yet (there is, for instance, deliberately no material-based
// DAMAGE mitigation today — only build-up and knockback vary by material — which is why every
// material row below shows the same `dmg` for a given payload; that is this build's real, current
// behavior, not a sweep bug).
//
// Payload catalog: RFC-008's `skill.*` documents (§7.6) do not exist on disk yet (confirmed by
// survey — no `data/` directory in this repo). Until they land, this reads the same compile-time
// constants the live game itself calls (`tiles.hpp`'s `kBaseMeleeDamage`/`kBaseRangedDamage`/
// `kBaseSpellDamage`, `chunk_actor.hpp`'s `kSpellPower`/`kHeavyMeleeImpulse`) rather than re-typing
// the numbers — swapping this array for a directory read is the only change RFC-008 landing
// requires here.
int run_sweep() {
    struct Payload {
        const char* id;
        std::int16_t base_damage;
        std::uint16_t impulse;
        Channel channel;         // Channel::kNone if this payload authors no build-up rider
        std::uint16_t buildup_power;
    };
    static constexpr Payload kPayloads[] = {
        {"skill.basic_melee_light", kBaseMeleeDamage, 0, Channel::kNone, 0},
        {"skill.basic_melee_heavy", static_cast<std::int16_t>(kBaseMeleeDamage * 2), kHeavyMeleeImpulse,
         Channel::kNone, 0},
        {"skill.basic_ranged_arrow", kBaseRangedDamage, 0, Channel::kNone, 0},
        {"skill.fire_bolt", kBaseSpellDamage, 0, Channel::kHeat, kSpellPower},
        {"skill.ice_bolt", kBaseSpellDamage, 0, Channel::kCold, kSpellPower},
        {"skill.earth_bolt", kBaseSpellDamage, 0, Channel::kEarth, kSpellPower},
        {"skill.shock_bolt", kBaseSpellDamage, 0, Channel::kShock, kSpellPower},
    };
    static constexpr Material kMaterials[] = {Material::kFlesh, Material::kStone, Material::kSpirit,
                                              Material::kMetal, Material::kWood,  Material::kPlant,
                                              Material::kWater, Material::kSlime};
    static constexpr const char* kMaterialNames[] = {"Flesh", "Stone", "Spirit", "Metal",
                                                      "Wood",  "Plant", "Water",  "Slime"};
    static constexpr const char* kChannelNames[] = {"none", "cold", "heat", "shock", "earth", "stagger"};

    // RFC-017 §2's reference dummy. Mass 100 matches `mass_of(ScaleTier::kMedium)` exactly (no
    // override needed); DR 0 / Toughness 0 are the RFC's own PINNED isolation values, not
    // `tier_toughness(kMedium)`'s real 1 — the point of a reference dummy is to isolate the payload
    // from a specific creature's derived stats. Friction is read from the real `terrain_phys` table
    // for Grass rather than re-typed as a literal 60.
    constexpr ScaleTier kDummyTier = ScaleTier::kMedium;
    const std::uint16_t dummy_mass = mass_of(kDummyTier);
    const TerrainPhys grass = terrain_phys(Terrain::kGrass);
    const DefenderMitigation dummy_def{};  // dr={0,0}, toughness=0

    std::printf("# mmo_sim sweep v1\n");
    std::printf("# reference dummy: tier=Medium mass=%u dr=0 toughness=0 friction=%u buildup=empty\n",
                dummy_mass, grass.friction);
    std::printf("%-26s %-7s %6s %16s %16s\n", "payload", "material", "dmg", "buildup",
                "knockback_tiles");

    for (const Payload& p : kPayloads) {
        for (std::size_t mi = 0; mi < 8; ++mi) {
            const Material m = kMaterials[mi];

            // Damage: RFC-009 §4.4 Steps 1-5, the live formula. Combo::kNone — the dummy carries no
            // primed status to detonate ("first-hit numbers", per the dummy's own definition).
            const std::int16_t dmg = resolve_damage(p.base_damage, Combo::kNone, dummy_def);

            // Build-up gain: RFC-009 §4.3 via the SAME `status_gain` the live game calls, against a
            // fresh (empty) gauge array — matching the dummy's "existing build-up: empty" row.
            char buildup_buf[24] = "-";
            if (p.channel != Channel::kNone) {
                StatusState s{};
                Gauge g[5]{};
                const std::uint16_t mult_pm = mult_pm_of(m, kDummyTier, p.channel);
                status_gain(s, g, BuildupPacket{p.channel, p.buildup_power, 0, 0}, mult_pm, 0);
                std::snprintf(buildup_buf, sizeof buildup_buf, "%s:%u",
                             kChannelNames[static_cast<int>(p.channel)],
                             g[gauge_index_of(p.channel)].value);
            }

            // Knockback: RFC-003 §5, transmit_impulse then knockback_tiles, both live functions.
            const std::uint16_t impulse_eff = transmit_impulse(p.impulse, m);
            const float kb =
                p.impulse == 0 ? 0.0f : knockback_tiles(impulse_eff, dummy_mass, grass.friction);

            std::printf("%-26s %-7s %6d %16s %16.2f\n", p.id, kMaterialNames[mi], dmg, buildup_buf, kb);
        }
    }

    // The acceptance check this mode owes RECONCILIATION.md Ruling 4 (RFC-017 §2): a Medium Flesh
    // dummy hit with ice_bolt must reach Freeze (stage 3) in exactly 2 casts — Ruling 2's worked
    // commitment curve for kIceBoltPower=600.
    StatusState s{};
    Gauge g[5]{};
    const std::uint16_t mult_pm = mult_pm_of(Material::kFlesh, kDummyTier, Channel::kCold);
    status_gain(s, g, BuildupPacket{Channel::kCold, kSpellPower, 0, 0}, mult_pm, 0);
    (void)status_step(s, g, 1, 1);
    const bool not_frozen_after_one = !(s.primary == Channel::kCold && s.stage == 3);
    status_gain(s, g, BuildupPacket{Channel::kCold, kSpellPower, 0, 0}, mult_pm, 1);
    (void)status_step(s, g, 1, 2);
    const bool frozen_after_two = s.primary == Channel::kCold && s.stage == 3;
    const bool ok = not_frozen_after_one && frozen_after_two;
    std::printf("\nacceptance (Ruling 2/4): Medium Flesh + ice_bolt reaches Freeze in exactly 2 casts: "
               "%s\n",
               ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// --- RFC-017 §4: `--gate-check` / `--gate-report` -----------------------------------------------
//
// Thin readers over `world/gate_sidecar.hpp` — no training loop, no episode running, no checkpoint
// state machine. Both apply RFC-007 §6.3's arithmetic exactly once, the same way every time.

std::string slurp_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    if (!ok) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int run_gate_check(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: mmo_sim --gate-check <policy_id> <meta.json path>\n");
        return 2;
    }
    const std::string want_policy = argv[2];
    const std::string path = argv[3];
    bool opened = false;
    const std::string doc = slurp_file(path, opened);
    if (!opened) {
        std::fprintf(stderr, "gate-check: cannot open '%s'\n", path.c_str());
        return 2;
    }
    const auto sidecar = parse_sidecar(doc);
    if (!sidecar) {
        std::fprintf(stderr,
                     "gate-check: '%s' is missing a required field (policy_id/generation/"
                     "vs_incumbent_winrate/vs_persona_winrate/episodes)\n",
                     path.c_str());
        return 2;
    }
    if (sidecar->policy_id != want_policy) {
        std::fprintf(stderr,
                     "gate-check: sidecar policy_id '%s' does not match requested '%s' — refusing to "
                     "evaluate the wrong checkpoint\n",
                     sidecar->policy_id.c_str(), want_policy.c_str());
        return 2;
    }
    const GateVerdict v = evaluate_gates(*sidecar);
    std::printf("policy_id:             %s\n", sidecar->policy_id.c_str());
    std::printf("generation:            %u\n", sidecar->generation);
    std::printf("vs_incumbent_winrate:  %.2f   (Gate A: >= %.2f, %s)\n", sidecar->vs_incumbent_winrate,
               kGateAWinrate, v.gate_a ? "PASS" : "FAIL");
    std::printf("vs_persona_winrate:    %.2f   (Gate B: <= %.2f, %s)\n", sidecar->vs_persona_winrate,
               kGateBWinrate, v.gate_b ? "PASS" : "FAIL");
    std::printf("episodes:              %u    (>= %u required, %s)\n", sidecar->episodes,
               kGateAEpisodes, sidecar->episodes >= kGateAEpisodes ? "PASS" : "FAIL");
    std::printf("generation_cap:        %u/%u  (%s)\n", sidecar->generation, kGenerationCap,
               v.cap_ok ? "PASS" : "FAIL");
    std::printf("verdict:               %s\n", v.publish() ? "PUBLISH" : "HOLD");
    return v.publish() ? 0 : 1;
}

int run_gate_report(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "gate-report: '%s' is not a directory\n", dir.c_str());
        return 2;
    }
    int total = 0;
    int not_publishable = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        static constexpr std::string_view kSuffix = ".meta.json";
        if (name.size() < kSuffix.size() ||
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        bool opened = false;
        const std::string doc = slurp_file(entry.path().string(), opened);
        ++total;
        const auto sidecar = opened ? parse_sidecar(doc) : std::nullopt;
        if (!sidecar) {
            std::printf("%-32s  UNREADABLE\n", name.c_str());
            ++not_publishable;
            continue;
        }
        const GateVerdict v = evaluate_gates(*sidecar);
        std::printf("%-32s gen=%-3u  A=%s  B=%s  cap=%s  verdict=%s\n", sidecar->policy_id.c_str(),
                   sidecar->generation, v.gate_a ? "PASS" : "FAIL", v.gate_b ? "PASS" : "FAIL",
                   v.cap_ok ? "PASS" : "FAIL", v.publish() ? "PUBLISH" : "HOLD");
        if (!v.publish()) ++not_publishable;
    }
    std::printf("\n%d policies checked, %d not publishable\n", total, not_publishable);
    return not_publishable == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // RFC-017 §1: three new modes, dispatched on argv[1] BEFORE it ever reaches std::atoi, so
    // `mmo_sim 600` (and `mmo_sim_smoke`, which depends on exactly that) is unaffected and
    // `mmo_sim --sweep` never gets misread as a tick count.
    if (argc > 1 && std::strcmp(argv[1], "--sweep") == 0) return run_sweep();
    if (argc > 1 && std::strcmp(argv[1], "--gate-check") == 0) return run_gate_check(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--gate-report") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: mmo_sim --gate-report <dir of *.meta.json>\n");
            return 2;
        }
        return run_gate_report(argv[2]);
    }
    const bool dump_mode = argc > 1 && std::strcmp(argv[1], "--determinism-dump") == 0;
    const int ticks = (argc > 1 && !dump_mode) ? std::atoi(argv[1]) : 1200;  // 1200 ticks == 120 s

    World world;
    world.build(/*workers*/ 4);
    world.start();

    std::printf("Quark MMO — headless simulation\n");
    std::printf("  chunks (actors): %zu across %d maps of %dx%d tiles\n", world.chunk_count(),
                kMapCount, kMapTiles, kMapTiles);
    std::printf("  tick rate: %d Hz, day %llds / night %llds\n\n", kTicksPerSecond,
                static_cast<long long>(kDayMs / 1000), static_cast<long long>(kNightMs / 1000));

    Check chk;

    // --- RFC-017 §4: the gate-sidecar reader, against RFC-007 §6.2's own worked example -------------
    {
        static constexpr const char* kWorked = R"({
  "policy_id": "boss.melee_bruiser",
  "obs_version": 1,
  "action_version": 1,
  "generation": 47,
  "parent_hash": "sha256:deadbeef",
  "weights_hash": "sha256:cafef00d",
  "eval": { "vs_incumbent_winrate": 0.58, "vs_persona_winrate": 0.63, "episodes": 200 }
})";
        const auto sidecar = parse_sidecar(kWorked);
        chk.expect(sidecar.has_value(), "the gate-sidecar reader parses RFC-007 §6.2's own example");
        if (sidecar) {
            chk.expect(sidecar->policy_id == "boss.melee_bruiser", "policy_id parses through nesting");
            chk.expect(sidecar->generation == 47, "generation parses");
            chk.expect(sidecar->episodes == 200, "the nested eval.episodes parses");
            const GateVerdict v = evaluate_gates(*sidecar);
            chk.expect(v.publish(), "RFC-007's own worked example passes both gates and the cap");
        }
        // A sidecar that fails Gate A (too few episodes) and Gate B (over the ceiling) must HOLD, not
        // PUBLISH — the mechanical reader has to fail loudly, not just parse cleanly.
        CheckpointSidecar bad;
        bad.policy_id = "boss.ranged_kiter";
        bad.generation = 12;
        bad.vs_incumbent_winrate = 0.52f;
        bad.vs_persona_winrate = 0.91f;
        bad.episodes = 150;
        const GateVerdict bv = evaluate_gates(bad);
        chk.expect(!bv.gate_a && !bv.gate_b && !bv.publish(),
                   "a checkpoint under-episoded AND over the persona ceiling holds on both gates");
    }
    std::printf("RFC-017 gate-sidecar reader: RFC-007's own worked example publishes; "
               "a double-failing sidecar holds\n\n");

    // --- RFC-022 §4: the village-always-fits invariant, against plan_of()'s real shipped values -----
    {
        // §4.2's fit table, reproduced tier-by-tier (hw/hh come from the live village.hpp plan_of()).
        struct FitRow {
            int tier;
            int full_w, full_h;
        };
        static constexpr FitRow kRows[] = {{1, 33, 29}, {2, 45, 39}, {3, 45, 49}, {4, 57, 49},
                                           {5, 57, 59}};
        for (const FitRow& r : kRows) {
            chk.expect(village_full_width(r.tier) == r.full_w,
                       "village_full_width matches RFC-022 §4.2's table for this tier");
            chk.expect(village_full_height(r.tier) == r.full_h,
                       "village_full_height matches RFC-022 §4.2's table for this tier");
        }
        chk.expect(!village_fits(1, 1), "a 1-chunk (32-tile) map fits no shipped village tier");
        for (int tier = 1; tier <= 5; ++tier) {
            chk.expect(village_fits(2, tier),
                       "every shipped village tier fits a 2x2-chunk (64-tile) map, per §4.2");
        }
    }

    // --- RFC-022 §2.3: the portal join-vs-create resolution rule -----------------------------------
    {
        std::vector<MapSession> live;

        // kFixedTarget with no prior record synthesizes the one session directly — nothing to
        // allocate, the destination is already known (§2.3).
        PortalDef door;
        door.id = 1;
        door.from_map = kOverworld;
        door.from_x = 10;
        door.from_y = 20;
        door.kind = PortalKind::kInteriorDoor;
        door.binding = PortalBinding::kFixedTarget;
        door.fixed_to_map = kInterior;
        door.fixed_to_x = 5;
        door.fixed_to_y = 6;
        const ResolveResult door_r = resolve_portal(door, live, 0);
        chk.expect(door_r.outcome == ResolveOutcome::kFound,
                   "a kFixedTarget portal always resolves (never needs allocation)");
        chk.expect(door_r.session.map_id == kInterior && door_r.session.return_map == kOverworld,
                   "the synthesized session targets fixed_to_map and returns to from_map");

        // kSharedPersistent: nothing live yet -> needs allocation; once recorded, any group finds it.
        PortalDef gate;
        gate.id = 2;
        gate.kind = PortalKind::kRealmGate;
        gate.binding = PortalBinding::kAllocateOnUse;
        gate.scope = SessionScope::kSharedPersistent;
        chk.expect(resolve_portal(gate, live, 7).outcome == ResolveOutcome::kNeedsAllocation,
                   "kSharedPersistent with no live session needs allocation");
        MapSession shared;
        shared.map_id = 20;
        shared.origin_portal = gate.id;
        shared.scope = SessionScope::kSharedPersistent;
        live.push_back(shared);
        chk.expect(resolve_portal(gate, live, 999).outcome == ResolveOutcome::kFound,
                   "kSharedPersistent joins the one running session regardless of group");

        // kGroupInstance: one group's session does not satisfy a different group's request.
        PortalDef dungeon;
        dungeon.id = 3;
        dungeon.kind = PortalKind::kRealmGate;
        dungeon.binding = PortalBinding::kAllocateOnUse;
        dungeon.scope = SessionScope::kGroupInstance;
        MapSession group_a;
        group_a.map_id = 21;
        group_a.origin_portal = dungeon.id;
        group_a.scope = SessionScope::kGroupInstance;
        group_a.owner_group = 5;
        live.push_back(group_a);
        chk.expect(resolve_portal(dungeon, live, 5).outcome == ResolveOutcome::kFound,
                   "kGroupInstance: the owning group rejoins its own open instance");
        chk.expect(resolve_portal(dungeon, live, 6).outcome == ResolveOutcome::kNeedsAllocation,
                   "kGroupInstance: a different group gets its own instance, not the first group's");

        // kSoloInstance always allocates fresh, even with other sessions on the same portal live.
        PortalDef solo;
        solo.id = 4;
        solo.binding = PortalBinding::kAllocateOnUse;
        solo.scope = SessionScope::kSoloInstance;
        chk.expect(resolve_portal(solo, live, 5).outcome == ResolveOutcome::kNeedsAllocation,
                   "kSoloInstance always needs a fresh allocation");
    }
    std::printf("RFC-022 map system: village-fit table matches plan_of(), portal join-vs-create "
               "resolves correctly for all four scopes\n\n");

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

    // --- RFC-010: battlefield simulation, as pure functions ------------------------------------------
    {
        // §4.2: the decay chain — every surface's duration in ms matches its authored tick count.
        chk.expect(dur_ms_of(Surface::kBurning) == 4000, "kBurning's 40-tick duration is 4s");
        chk.expect(dur_ms_of(Surface::kMudded) == 30000, "kMudded's 300-tick duration is 30s");
        chk.expect(dur_ms_of(Surface::kIced) == 30000, "kIced's 300-tick duration is 30s");

        // flammability, scoped to the two terrains the RFC's guide-level text names.
        chk.expect(flammable_of(Terrain::kGrass) && flammable_of(Terrain::kTree),
                   "grass and forest tiles are flammable");
        chk.expect(!flammable_of(Terrain::kStone) && !flammable_of(Terrain::kBuilding),
                   "stone and buildings are not — buildings are the engine's own fireproof-claim proxy");

        // §4.2's trigger table: Fire on baseline ignites; on an existing ice/mud patch it steams the
        // patch away instead; on an existing burn it refreshes.
        chk.expect(fire_impact(false, Surface::kBurning).event == SurfaceEvent::kSet &&
                       fire_impact(false, Surface::kBurning).result == Surface::kBurning,
                   "fire on baseline sets kBurning");
        chk.expect(fire_impact(true, Surface::kIced).event == SurfaceEvent::kRemoved,
                   "fire steams an iced patch away — no patch remains");
        chk.expect(fire_impact(true, Surface::kMudded).event == SurfaceEvent::kRemoved,
                   "fire steams a mudded patch away too");
        chk.expect(fire_impact(true, Surface::kBurning).event == SurfaceEvent::kRefreshed,
                   "fire on an already-burning tile just refreshes it");
        chk.expect(ice_impact(false, Surface::kBurning).result == Surface::kIced,
                   "ice on baseline sets kIced");
        chk.expect(ice_impact(true, Surface::kBurning).event == SurfaceEvent::kRemoved,
                   "ice extinguishes a burning patch — no patch remains");
        chk.expect(ice_impact(true, Surface::kMudded).result == Surface::kIced,
                   "ice freezes a mudded patch into kIced");

        // §4.2's coefficient rows: mud softens knockback but hardens the crush bonus; ice does the
        // opposite trade — a harder shove, a softer direct hit.
        const SurfaceCoeff mud = surface_coeff(true, Surface::kMudded);
        chk.expect(mud.knockback_pm == 500 && mud.force_transfer_pm == 1250,
                   "mud: knockback x0.5, force-transfer damage x1.25");
        const SurfaceCoeff ice = surface_coeff(true, Surface::kIced);
        chk.expect(ice.knockback_pm == 1500 && ice.direct_damage_pm == 800,
                   "ice: knockback x1.5, direct damage x0.8");
        chk.expect(surface_coeff(false, Surface::kMudded).knockback_pm == 1000,
                   "no live patch means no coefficient at all — the identity default");

        // §4.2's eviction tie-break: smallest end_ms first, then (tx, ty) lexicographic.
        chk.expect(patch_expires_before(TilePatch{1, 1, Surface::kBurning, 0, 100},
                                        TilePatch{2, 2, Surface::kBurning, 0, 200}),
                   "the record with the smaller end_ms expires first");
        chk.expect(patch_expires_before(TilePatch{1, 5, Surface::kBurning, 0, 100},
                                        TilePatch{2, 1, Surface::kBurning, 0, 100}),
                   "a tied end_ms breaks on tx, then ty, lexicographically");

        // §4.3: the deterministic accuracy table and stacking rule.
        chk.expect(field_accuracy_pm(1) == 900 && field_accuracy_pm(2) == 800 && field_accuracy_pm(3) == 700,
                   "the earthquake accuracy multiplier matches the §4.3 table");
        chk.expect(field_accuracy_pm(0) == 1000, "no field at all is the identity multiplier");
        chk.expect(highest_intensity(1, 3) == 3 && highest_intensity(2, 1) == 2,
                   "overlapping fields never stack — the actor takes the single highest intensity");

        // §4.3's drift formula: pure in (shot id, launch tick, tick, intensity); zero outside any field.
        chk.expect(field_drift_amplitude(0) == 0.0f, "intensity 0 (no field) drifts nothing");
        chk.expect(drift_perp(0, 7, 0, 0) == 0.0f, "zero intensity is zero amplitude regardless of phase");
        const float d1 = drift_perp(5, 42, 0, 2);
        const float d2 = drift_perp(5, 42, 0, 2);
        chk.expect(d1 == d2, "drift is a pure function — same inputs, same output, every call");
        chk.expect(std::fabs(d1) <= field_drift_amplitude(2) + 0.0001f,
                   "drift never exceeds its own intensity's amplitude");

        std::printf(
            "RFC-010 battlefield: the tile-patch decay chain, the fire/ice trigger table, mud/ice "
            "coefficients, eviction tie-break, and the earthquake accuracy/drift formulas all check "
            "out\n\n");
    }

    // --- RFC-005: boss ability authoring, as pure functions -------------------------------------------
    {
        // A new, minimal table (see boss_kit.hpp's header note) — Giant reproduces the RFC's own
        // worked-example number exactly.
        chk.expect(body_half_width_of(ScaleTier::kGiant) == 1.25f,
                   "a Giant's body half-width matches the RFC's own §R4 worked example");

        // §R2's escape-distance column.
        chk.expect(escape_distance_arc(2.6f) == 2.6f, "an arc's escape distance is just its radius");
        chk.expect(escape_distance_dash(body_half_width_of(ScaleTier::kGiant)) == 1.75f,
                   "a Giant's dash escape distance (0.5 + 1.25) matches the RFC's own worked example");
        chk.expect(escape_distance_ring(5.0f, 2.0f) == 3.0f, "a ring's escape distance is radius - hole");

        // §R4's tier table.
        chk.expect(tier_min_windup(0) == 5 && tier_min_windup(1) == 8 && tier_min_windup(2) == 12 &&
                       tier_min_windup(3) == 16,
                   "the danger-tier minimum windups match RFC-006 §1.3's table, delegated here");
        const BossTierMult adept = tier_mult(BossTier::kAdept);
        chk.expect(adept.hp_pm == 1400 && adept.damage_pm == 1250 && adept.cooldown_pm == 1000,
                   "Adept (tier 2, 'the intended fight') matches the RFC's own §R4 table");
        const BossTierMult elite = tier_mult(BossTier::kElite);
        chk.expect(elite.hp_pm == 2400 && elite.cooldown_pm == 700,
                   "Elite (tier 4) hits the RFC's own ceiling multipliers");

        // §R4's readability floor, reproduced against the RFC's own two worked checks for the
        // shipped Samurai.
        const int cleave_floor = windup_floor_ticks(1, escape_distance_arc(kBossReach));
        chk.expect(cleave_floor == 8,
                   "cleave's floor is governed by the tier-1 minimum (8), exactly as the RFC's own "
                   "worked check finds (geometric term is only 7)");
        chk.expect(kBossAttackWindup >= cleave_floor,
                   "the shipped cleave windup (10) clears its own floor with margin");
        const int charge_floor =
            windup_floor_ticks(2, escape_distance_dash(body_half_width_of(ScaleTier::kGiant)));
        chk.expect(charge_floor == 12,
                   "the charge dash's floor is governed by the tier-2 minimum (12), exactly as the "
                   "RFC's own worked check finds (geometric term is only 5)");
        chk.expect(kBossChargeWindup >= charge_floor,
                   "the shipped charge windup (14) clears its own floor");

        // The floor formula genuinely rejects an under-authored wind-up — proving the validator
        // logic this RFC specifies would actually catch a bad kit, not just rubber-stamp the good one.
        chk.expect(windup_floor_ticks(0, 20.0f) > kWindupFloorTicks,
                   "a wide-reaching ability at the lowest danger tier still needs real geometric "
                   "margin, not just the flat floor");
        chk.expect(3 < windup_floor_ticks(3, 20.0f),
                   "an authored windup of 3 ticks would fail a 20-tile-escape, tier-3 ability's floor");

        // The one real kit reproduces the shipped Samurai's numbers verbatim (boss_kit.hpp reads
        // FROM boss.hpp's own named constants rather than retyping them).
        const BossKitDef samurai = samurai_red_kit();
        chk.expect(samurai.ability_count == 2 && samurai.base_hp == kBossMaxHp,
                   "the Samurai kit carries exactly cleave + charge_dash, at the shipped HP");
        chk.expect(samurai.abilities[0].windup == kBossAttackWindup &&
                       samurai.abilities[1].windup == kBossChargeWindup,
                   "the kit's wind-ups are the shipped boss's own, not re-authored numbers");
        chk.expect(samurai.abilities[1].cooldown > samurai.abilities[1].active + samurai.abilities[1].recover,
                   "validator #9's FSM-sanity rule (cooldown > active + recover) holds for the real kit");

        std::printf(
            "RFC-005 boss authoring: the body-half-width/escape-distance tables, the tier multiplier "
            "table, and the readability-floor formula all check out against the shipped Samurai's own "
            "numbers\n\n");
    }

    // --- RFC-007: RL observation & action space, as pure functions ------------------------------------
    {
        chk.expect(kObsSize == 120 && kObsVersion == 1, "the obs contract is exactly the RFC's own size/version");
        chk.expect(kActionCount == 15, "the action space is exactly 15, matching RLDrive's sampler bound");

        // §2's closeness/offset/fraction encodings — "absent/far = 0" must hold at and beyond horizon.
        chk.expect(obs_closeness(8.0f, 0.0f) == 1.0f, "touching the agent is maximally close");
        chk.expect(obs_closeness(8.0f, 8.0f) == 0.0f, "exactly at the horizon reads as absent");
        chk.expect(obs_closeness(8.0f, 20.0f) == 0.0f, "beyond the horizon never goes negative");
        chk.expect(obs_offset(20.0f, 8.0f) == 1.0f && obs_offset(-20.0f, 8.0f) == -1.0f,
                   "egocentric offsets clamp to +/-1 well beyond kObsRange");
        chk.expect(obs_frac(150.0f, 100.0f) == 1.0f && obs_frac(-5.0f, 100.0f) == 0.0f,
                   "fractions clamp into [0,1] even from out-of-range inputs");

        // §3's generation-0 compatibility table, verbatim.
        chk.expect(to_rl_action(BossActionKind::kHold) == RlActionId::kHold, "kHold -> id 0");
        chk.expect(to_rl_action(BossActionKind::kApproach) == RlActionId::kApproach, "kApproach -> id 5");
        chk.expect(to_rl_action(BossActionKind::kAttackLeft) == RlActionId::kCastSlot0Direct &&
                       to_rl_action(BossActionKind::kAttackRight) == RlActionId::kCastSlot0Direct,
                   "both attack sides collapse onto Cast slot 0 Direct (id 7) -- facing is derived, "
                   "not chosen, per the RFC's own note");
        chk.expect(to_rl_action(BossActionKind::kCharge) == RlActionId::kCastSlot1Direct,
                   "kCharge -> Cast slot 1 Direct (id 8)");

        // §5's reward shaping — the structural rules matter more than the coefficients.
        chk.expect(reward_damage_dealt(50.0f, 700.0f, /*pipeline_delivered*/ true) > 0.0f,
                   "pipeline-delivered damage earns a positive reward");
        chk.expect(reward_damage_dealt(50.0f, 700.0f, /*pipeline_delivered*/ false) == 0.0f,
                   "R-honesty: contact_damage (not pipeline-delivered) earns exactly zero");
        chk.expect(reward_damage_taken(20.0f, 100.0f) < 0.0f, "taking damage is always penalized");
        chk.expect(reward_terminal(/*won*/ true, /*lost*/ false) == kRewardTerminalWin &&
                       reward_terminal(false, true) == kRewardTerminalLoss,
                   "terminal reward matches the RFC's own §5 table");
        chk.expect(reward_turtling(10, /*target_present*/ true, /*any_slot_ready*/ true) < 0.0f,
                   "holding past the threshold with a live target and a ready slot is turtling");
        chk.expect(reward_turtling(10, /*target_present*/ true, /*any_slot_ready*/ false) == 0.0f,
                   "the guard condition protects legitimate post-strike recovery from the turtle penalty");
        chk.expect(reward_turtling(3, true, true) == 0.0f,
                   "holding is free below the RFC's own kTurtleHoldThreshold");
        chk.expect(clip_reward(5.0f) == 1.0f && clip_reward(-5.0f) == -1.0f,
                   "the per-decision reward sum clips to [-1,+1]");

        std::printf(
            "RFC-007 RL contract: the obs vector's encoding conventions, the generation-0 action "
            "mapping, and the reward-shaping formulas all check out\n\n");
    }

    // --- RFC-006: visual FX & telegraph standards, as pure functions ----------------------------------
    {
        // §1.3's tier qualification, reproduced against the RFC's own worked check: the shipped
        // Samurai's attack (20 damage on a 100-HP player, d=0.2) is tier 1 (moderate), whose 8-tick
        // floor the authored windup (10) clears.
        const float d = expected_damage_fraction(static_cast<float>(kBossDamage), 1.0f,
                                                 static_cast<float>(kPlayerMaxHp));
        chk.expect(d > 0.19f && d < 0.21f, "the Samurai's attack computes to d=0.2, the RFC's own number");
        chk.expect(telegraph_tier_of(d, 0) == 1,
                   "d=0.2 with no hard control qualifies as tier 1 (moderate), matching the RFC's own "
                   "worked check");
        chk.expect(telegraph_tier_of(0.05f, 0) == 0, "a light jab (d<0.10, no control) is tier 0");
        chk.expect(telegraph_tier_of(0.30f, 0) == 2, "d=0.30 alone (no control) is tier 2 (heavy)");
        chk.expect(telegraph_tier_of(0.0f, 25) == 3, "25 ticks of hard control alone is tier 3, by the "
                                                     "cc clause regardless of damage");
        chk.expect(telegraph_tier_of(0.60f, 5) == 3,
                   "highest tier wins: a deadly-damage, mild-control hit is still tier 3");

        // §1.4's fill fraction and lifecycle state machine.
        chk.expect(telegraph_fill_frac(10, 10) == 0.0f && telegraph_fill_frac(10, 0) == 1.0f,
                   "fill runs 0 at commit to 1 the instant the wind-up elapses");
        chk.expect(telegraph_state_of(10, 9, false) == TelegraphState::kArm,
                   "the first tick of a 10-tick wind-up is ARM");
        chk.expect(telegraph_state_of(10, 5, false) == TelegraphState::kCharge,
                   "the middle of a 10-tick wind-up is CHARGE");
        chk.expect(telegraph_state_of(10, 3, false) == TelegraphState::kImminent,
                   "the last 3 ticks are IMMINENT");
        chk.expect(telegraph_state_of(5, 3, false) == TelegraphState::kImminent,
                   "a total<=5 wind-up skips CHARGE entirely -- ARM hands straight to IMMINENT");
        chk.expect(telegraph_state_of(10, 1, /*fizzling*/ true) == TelegraphState::kFizzle,
                   "an interrupted wind-up reads as FIZZLE regardless of its remaining ticks");

        // §2's cap/eviction rule: only a STRICTLY higher tier evicts, and only the lowest-tier (oldest
        // among ties) record — never silently dropping a promise that was already drawn.
        std::array<Telegraph, kMaxTelegraphs> live{};
        for (std::size_t i = 0; i < kMaxTelegraphs; ++i) {
            live[i].id = static_cast<std::uint32_t>(i + 1);
            live[i].tier = 1;
        }
        chk.expect(telegraph_eviction_index(live, kMaxTelegraphs, 1) == -1,
                   "an equal-tier commit against a full, uniform-tier chunk is refused, not swapped "
                   "in -- a well-defined Hold no-op");
        chk.expect(telegraph_eviction_index(live, kMaxTelegraphs, 0) == -1,
                   "a strictly-lower-tier commit is refused too");
        chk.expect(telegraph_eviction_index(live, kMaxTelegraphs, 2) == 0,
                   "among tied-lowest records, the OLDEST (smallest id) is the one that yields to a "
                   "strictly higher tier");
        live[3].tier = 0;  // the one weak spot among 8 otherwise-tier-1 records
        chk.expect(telegraph_eviction_index(live, kMaxTelegraphs, 1) == 3,
                   "a strictly-higher-tier commit evicts the single lowest-tier live record, not the "
                   "oldest overall");

        // §1.2's element palette: the motif column is mandatory, but hue alone must still be a real,
        // distinct value per element -- this is the data half of that accessibility rule.
        chk.expect(telegraph_hue_of(Element::kNone).r == 255 && telegraph_hue_of(Element::kNone).g == 45,
                   "physical telegraphs use the existing red wind-up pulse color");
        chk.expect(telegraph_hue_of(Element::kIce).b == 255, "Ice reads unmistakably blue");

        std::printf(
            "RFC-006 telegraphs: the danger-tier qualification formula, the fill/lifecycle state "
            "machine, the cap/eviction rule, and the element palette all check out\n\n");
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

    // --- RFC-014: instance lifecycle, live against the real engine ---------------------------------
    // No portal-step trigger detector exists (client/input-layer territory, not this RFC's), so this
    // drives `World::use_portal`/`disconnect_player`/`sweep_instances` directly — the same real,
    // callable verbs a future trigger would invoke, exercised the way `teleport_player` already is.
    {
        PortalDef portal{};
        portal.id = 501;
        portal.from_map = kOverworld;
        portal.from_x = static_cast<std::uint16_t>(spawn.x);
        portal.from_y = static_cast<std::uint16_t>(spawn.y);
        portal.kind = PortalKind::kRealmGate;
        portal.realm_type = RealmType::kChallenge;
        portal.flavor = RealmFlavor::kDungeon;
        portal.binding = PortalBinding::kAllocateOnUse;
        portal.scope = SessionScope::kGroupInstance;

        MapDescriptor desc{};
        desc.chunk_edge = 2;
        desc.biome = Ring::kForest;
        desc.weather_mode = WeatherMode::kFixed;
        desc.allow_free_build = false;

        const AccountId acct = world.account_of(slot);
        const bool ok1 = world.use_portal(me, portal, /*group*/ 1, acct, desc);
        chk.expect(ok1, "use_portal allocates a fresh instance on first use");
        const PlayerView in1 = world.player_view(slot);
        chk.expect(map_id_instanced(in1.map), "the player lands on an instanced MapId");
        const MapId target = in1.map;

        InstanceSession* s1 = world.instances().find_session(target);
        chk.expect(s1 != nullptr && s1->state == SessionState::kActive,
                   "a fresh instance is ACTIVE immediately — priming is a synchronous barrier");
        chk.expect(s1 != nullptr && s1->chunk_edge == 2,
                   "the session records the descriptor's own chunk_edge");
        chk.expect(s1 != nullptr && !s1->members.empty() && s1->members[0] == acct,
                   "the requester is recorded as a member");

        const ChunkStats cs = world.chunk_stats(ChunkCoord{target, 0, 0});
        chk.expect(cs.creatures == 0,
                   "a freshly primed instance chunk exists and answers — declare_lazy really "
                   "activated it, empty since no population is authored yet");

        // Rejoin: the SAME group reusing the SAME portal must land on the SAME session.
        const bool ok2 = world.use_portal(me, portal, 1, acct, desc);
        chk.expect(ok2, "rejoining the same group's open session succeeds");
        chk.expect(world.player_view(slot).map == target,
                   "the same group rejoins the SAME instance, never a fresh allocation");

        // A different group using the identical portal gets its OWN instance.
        const std::uint64_t other = world.key_of(slot2);
        const AccountId acct2 = world.account_of(slot2);
        const bool ok3 = world.use_portal(other, portal, 2, acct2, desc);
        chk.expect(ok3, "a second group can use the same portal");
        const MapId target2 = world.player_view(slot2).map;
        chk.expect(map_id_instanced(target2) && target2 != target,
                   "a different group gets a DIFFERENT instance, never the first group's");

        // Disconnect: position preserved exactly, the slot goes unbound.
        world.disconnect_player(me, target, acct);
        chk.expect(!world.player_view(slot).live(), "a disconnected slot is unbound");
        chk.expect(world.player_view(slot).map == target,
                   "disconnect preserves position — it does not move the player");

        // Reconnect: same account, same slot, resumes IN PLACE (not the overworld spawn).
        LoginOutcome resumed_out{};
        const int resumed_slot = world.login("thnak", "correct horse battery", resumed_out);
        chk.expect(resumed_slot == slot, "reconnecting the same account takes back the same slot");
        chk.expect(world.player_view(slot).live() && world.player_view(slot).map == target,
                   "reconnecting resumes inside the still-open instance, not a fresh spawn");

        // Teardown: empty `present`, sweep past the grace window, the session closes for good.
        world.leave_instance(target, acct);
        world.sweep_instances(1'000);
        InstanceSession* idle = world.instances().find_session(target);
        chk.expect(idle != nullptr && idle->state == SessionState::kIdle,
                   "an empty session goes IDLE first, not closed immediately — the grace window");
        world.sweep_instances(1'000 + kInstanceIdleGraceMs);
        chk.expect(world.instances().find_session(target) == nullptr,
                   "once the grace window elapses, the session is torn down and forgotten");
        chk.expect(world.bus().load(ChunkCoord{target, 0, 0}) == nullptr,
                   "the SnapshotBus block is released — a stale load for the closed map returns null");

        world.teleport_player(me, kOverworld, spawn.x, spawn.y);
        world.teleport_player(other, kOverworld, spawn.x, spawn.y);
    }
    std::printf("RFC-014 instance lifecycle: allocate/rejoin/group-isolation/disconnect/reconnect/"
               "teardown all check out\n\n");

    // --- RFC-013: vitals, death & recovery ----------------------------------------------------------
    // Persistent-band death (hearth respawn, nothing lost) is unchanged by this RFC and already has
    // its own dedicated regression further below (the "Death and respawn" section) — the fork this
    // RFC adds never triggers for map < kPersistentBandEnd. This block covers the two contracts §6
    // actually adds: instanced-band ejection to a session's real return point (with full item loss,
    // untouched XP/levels), and §6.2's guard falling back to the bound hearth when a player's
    // instance_return_* was never wired at all. Both run on FRESH, dedicated accounts rather than
    // `me`/`slot` — ejection permanently zeroes carried items, and the rest of this suite (farming,
    // hearth-building) depends on `me` still holding its starting wood/stone/seed.
    {
        PortalDef portal{};
        portal.id = 502;
        portal.from_map = kOverworld;
        portal.from_x = static_cast<std::uint16_t>(spawn.x);
        portal.from_y = static_cast<std::uint16_t>(spawn.y);
        portal.kind = PortalKind::kRealmGate;
        portal.realm_type = RealmType::kChallenge;
        portal.flavor = RealmFlavor::kDungeon;
        portal.binding = PortalBinding::kAllocateOnUse;
        portal.scope = SessionScope::kSoloInstance;

        MapDescriptor desc{};
        desc.chunk_edge = 1;
        desc.biome = Ring::kForest;
        desc.weather_mode = WeatherMode::kFixed;
        desc.allow_free_build = false;

        LoginOutcome eject_out{};
        const int eject_slot = world.login("ejecttest", "hunter2", eject_out);
        chk.expect(eject_slot >= 0, "a dedicated account logs in for the ejection test");
        if (eject_slot >= 0) {
            const std::uint64_t eject_key = world.key_of(eject_slot);
            const AccountId acct = world.account_of(eject_slot);
            world.teleport_player(eject_key, kOverworld, spawn.x, spawn.y);
            const PlayerView before = world.player_view(eject_slot);
            chk.expect(before.items[static_cast<int>(ItemKind::kWood)] > 0,
                       "the player carries something worth losing, going in");

            const bool entered = world.use_portal(eject_key, portal, /*group*/ 3, acct, desc);
            chk.expect(entered, "use_portal allocates a fresh solo instance for the ejection test");
            const MapId target = world.player_view(eject_slot).map;
            chk.expect(map_id_instanced(target), "landed on an instanced MapId");

            world.hurt_player(eject_key, kPlayerMaxHp);
            chk.expect(world.player_view(eject_slot).dead_ticks > 0,
                       "a lethal hit starts the same death countdown as an overworld death");

            advance(world, kRespawnTicks + 5);
            const PlayerView ejected = world.player_view(eject_slot);
            chk.expect(ejected.map == portal.from_map,
                       "ejection lands the player back on the map they entered through");
            chk.expect(std::fabs(ejected.x - (static_cast<float>(portal.from_x) + 0.5f)) < 0.01f &&
                           std::fabs(ejected.y - (static_cast<float>(portal.from_y) + 0.5f)) < 0.01f,
                       "ejection lands exactly at the session's cached return point");
            chk.expect(ejected.items[static_cast<int>(ItemKind::kWood)] == 0 &&
                           ejected.items[static_cast<int>(ItemKind::kStone)] == 0 &&
                           ejected.items[static_cast<int>(ItemKind::kSeed)] == 0,
                       "ejection clears every carried item, in full (§6.5)");
            chk.expect(ejected.hp == kPlayerMaxHp && ejected.mana == kPlayerMaxMana &&
                           ejected.stamina == kPlayerMaxStamina,
                       "vitals restore to maximum, identically to an overworld respawn");
            chk.expect(ejected.skill_level[static_cast<int>(Skill::kMelee)] ==
                               before.skill_level[static_cast<int>(Skill::kMelee)] &&
                           ejected.skill_xp[static_cast<int>(Skill::kMelee)] ==
                               before.skill_xp[static_cast<int>(Skill::kMelee)],
                       "XP and skill levels are never touched by ejection (§6.6)");
            chk.expect(ejected.deaths > before.deaths,
                       "ejection counts as a death, same counter as overworld");

            world.leave_instance(target, acct);
            world.sweep_instances(2'000);
            world.sweep_instances(2'000 + kInstanceIdleGraceMs);

            // Ejection lands this account back on the shared overworld spawn tile — the same tile
            // `me`/`guest` wake on. Disconnect it (goes inert, stops beaconing) so it does not become
            // a silent third watcher in the staged-fight section below, which counts exactly two.
            world.disconnect_player(eject_key, world.player_view(eject_slot).map, acct);
        }

        // §6.2's guard: a player placed on an instanced map WITHOUT ever going through use_portal (so
        // instance_return_map_/x_/y_ are still the all-zero default) must fall back to their own bound
        // hearth on ejection, not trust the zero triple as a real destination.
        LoginOutcome guard_out{};
        const int guard_slot = world.login("ejectguard", "hunter2", guard_out);
        chk.expect(guard_slot >= 0, "a third account logs in clean for the fallback guard test");
        if (guard_slot >= 0) {
            const std::uint64_t guard_key = world.key_of(guard_slot);
            const PlayerView guard_before = world.player_view(guard_slot);
            world.teleport_player(guard_key, kPersistentBandEnd, 2.5f, 2.5f);
            chk.expect(map_id_instanced(world.player_view(guard_slot).map),
                       "the guard test player sits on an instanced map with no SetInstanceReturn ever sent");
            world.hurt_player(guard_key, kPlayerMaxHp);
            advance(world, kRespawnTicks + 5);
            const PlayerView guard_reborn = world.player_view(guard_slot);
            chk.expect(guard_reborn.map == kOverworld,
                       "an unset return point falls back to the overworld hearth, not the instanced map");
            chk.expect(
                std::fabs(guard_reborn.x - (static_cast<float>(guard_before.respawn_tx) + 0.5f)) < 0.01f &&
                    std::fabs(guard_reborn.y - (static_cast<float>(guard_before.respawn_ty) + 0.5f)) <
                        0.01f,
                "the fallback lands exactly at the player's own bound hearth point (§7)");
            chk.expect(guard_reborn.items[static_cast<int>(ItemKind::kWood)] == 0,
                       "the fallback path still clears carried items — it is still an ejection");

            // Same reason as ejecttest above: the fallback also lands on the shared spawn tile.
            world.disconnect_player(guard_key, kOverworld, world.account_of(guard_slot));
        }
    }
    std::printf("RFC-013 vitals/death/recovery: instanced ejection + return-point guard fallback "
               "check out\n\n");

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

    // --- RFC-017 §3: `--determinism-dump` -----------------------------------------------------------
    // Formalizes ARCHITECTURE.md §2c's already-proven GCC/MSVC tile-for-tile check: pull every value
    // that check's own text names as invariant into one normalized, grep/diff-able block, reusing
    // every value already computed above (never recomputed by a second code path), plus one fresh
    // pass over `terrain_of` for the per-terrain tile tally (the same pure worldgen function every
    // other terrain check in this file already calls).
    //
    // DIVERGENCE: this block is emitted immediately after the narrative prints above, not instead of
    // them — suppressing ~15 existing `std::printf` call sites across this file for one CLI mode
    // was a broad, risky edit for no functional gain, since every dump line is uniquely prefixed
    // (`worldgen.`/`wildlife.`/`staged_fight.`) between the two marker lines below. `mmo_sim
    // --determinism-dump | sed -n '/^--- determinism dump/,/^--- end determinism dump/p'` (or an
    // equivalent grep on the prefixes) extracts exactly the "grep/diff-able block" §3 asks for.
    //
    // Chunk migrations are deliberately NEVER in this block (ARCHITECTURE.md §2c / RFC-017 §3's own
    // normative rule): that counter depends on cross-actor message arrival order and is proven to
    // vary run-to-run on the SAME binary and machine, so it can never be a cross-platform invariant.
    if (dump_mode) {
        std::uint64_t terrain_tally[static_cast<int>(Terrain::kCount)] = {};
        for (int ty = 0; ty < kMapTiles; ++ty) {
            for (int tx = 0; tx < kMapTiles; ++tx) {
                ++terrain_tally[static_cast<int>(terrain_of(kWorldSeed, home, tx, ty))];
            }
        }
        std::printf("\n--- determinism dump (RFC-017 §3) ---\n");
        std::printf("worldgen.villages=%zu\n", layout.villages().size());
        std::printf("worldgen.strongholds=%zu\n", layout.strongholds().size());
        std::printf("worldgen.buildings=%zu\n", layout.structures().size());
        std::printf("worldgen.spawn=%d,%d\n", static_cast<int>(spawn.x), static_cast<int>(spawn.y));
        for (int i = 0; i < static_cast<int>(Terrain::kCount); ++i) {
            std::printf("worldgen.terrain_tally.%d=%llu\n", i,
                        static_cast<unsigned long long>(terrain_tally[i]));
        }
        std::printf("wildlife.bring_up=%u\n", wild_total);
        std::printf("staged_fight.creatures=%u\n", staged.creatures);
        std::printf("staged_fight.hostile=%u\n", staged.hostile);
        std::printf("staged_fight.watchers=%u\n", staged.watchers);
        std::printf("staged_fight.hp_slot0=%d\n", mauled.hp);
        std::printf("staged_fight.hp_slot1=%d\n", other.hp);
        std::printf("staged_fight.hits_landed=%d\n", swings);
        std::printf("staged_fight.hits_refused=%d\n", refused_swings);
        std::printf("staged_fight.kills=%u\n", player_kills);
        std::printf("staged_fight.melee_xp=%u\n", fought.skill_xp[static_cast<int>(Skill::kMelee)]);
        std::printf("staged_fight.cast_ok=%d\n", cast_ok ? 1 : 0);
        std::printf("staged_fight.afflicted=%u\n", frozen.afflicted);
        std::printf("staged_fight.shot_ok=%d\n", shot_ok ? 1 : 0);
        std::printf("staged_fight.projectiles_airborne=%u\n", airborne.projectiles);
        std::printf("--- end determinism dump ---\n");
        return chk.failures == 0 ? 0 : 1;
    }

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

    // --- RFC-010 integration: a Fire cast ignites grass, burns out into an RFC-004 scar --------------
    {
        int gtx = -1;
        int gty = -1;
        for (int r = 0; r < 80 && gtx < 0; ++r) {
            const int tx = static_cast<int>(spawn.x) + r;
            const int ty = static_cast<int>(spawn.y);
            if (terrain_of(kWorldSeed, home, tx, ty) == Terrain::kGrass) {
                gtx = tx;
                gty = ty;
            }
        }
        chk.expect(gtx >= 0, "found a grass tile near spawn to set alight");
        if (gtx >= 0) {
            world.teleport_player(guest, home, static_cast<float>(gtx) + 0.5f,
                                  static_cast<float>(gty) + 0.5f);
            world.grant_vitals(guest, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            const ChunkCoord burn_chunk = chunk_of(home, static_cast<float>(gtx), static_cast<float>(gty));
            const bool burned = world.cast(guest, Element::kFire, static_cast<float>(gtx) + 0.5f,
                                           static_cast<float>(gty) + 0.5f);
            advance(world, 1);
            const ChunkStats lit = world.chunk_stats(burn_chunk);
            std::printf("battlefield: fire cast %s on grass (%d,%d);  patches=%u burning=%u\n",
                        burned ? "ok" : "refused", gtx, gty, lit.patches, lit.burning);
            chk.expect(burned, "the player could cast fire");
            chk.expect(lit.burning > 0, "the impact left a live kBurning tile patch");

            const std::uint32_t scars_before = lit.scars;
            advance(world, kBurningDurTicks + 2);
            const ChunkStats burnt_out = world.chunk_stats(burn_chunk);
            std::printf("  burnout: patches now=%u scars %u -> %u\n", burnt_out.patches, scars_before,
                        burnt_out.scars);
            chk.expect(burnt_out.burning == 0, "the patch burned itself out on its own bounded clock");
            chk.expect(burnt_out.scars > scars_before,
                       "burnout stamped an RFC-004 kScorched scar (Layer 0b), never a longer chain here");

            world.teleport_player(guest, home, spawn.x, spawn.y);
        }
        std::printf("RFC-010 integration: a live combat fire patch spreads/decays and hands off to "
                    "RFC-004's scar layer at burnout, all check out\n\n");
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

            // RFC-006 §2: the commit above also wrote a real, replicated Telegraph record, not just
            // the legacy `windup` counter — the same commit `boss_commit` (chunk_actor.hpp) now drives.
            Creature committed_boss{};
            boss_of_room(committed_boss);
            const ChunkCoord droom_chunk = chunk_of(kInterior, committed_boss.x, committed_boss.y);
            chk.expect(world.chunk_stats(droom_chunk).telegraphs >= 1,
                       "the committed attack published a real RFC-006 Telegraph record");
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
            chk.expect(world.chunk_stats(droom_chunk).telegraphs == 0,
                       "the telegraph record disappeared the instant the blow resolved -- hand-off "
                       "to the impact Effect, per §1.4's lifecycle table");

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
            // credit lands in Melee (the killing verb), and the reward is 400 XP + RFC-018 §10.2's
            // real boss loot table: a guaranteed ore row (regardless of realm) plus Essence/rare
            // equipment rows that stay dormant here — this dojo room is a PERSISTENT interior (an
            // ordinary door, never a kRealmGate portal), so `realm_type_` is `kRest` — matching
            // loot.hpp's own header note on why the RFC's "dojo bosses are challenge-realm content"
            // assumption does not hold for THIS codebase's one shipped boss.
            const PlayerView pre_kill = world.player_view(slot);
            const std::uint32_t xp_before = pre_kill.skill_xp[static_cast<int>(Skill::kMelee)];
            std::int32_t ore_before = 0;
            for (int k = static_cast<int>(ItemKind::kOreCopper); k <= static_cast<int>(ItemKind::kOreMythril); ++k) {
                ore_before += pre_kill.items[k];
            }
            const std::int32_t essence_before = pre_kill.items[static_cast<int>(ItemKind::kEssence)];
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
            std::int32_t ore_after = 0;
            for (int k = static_cast<int>(ItemKind::kOreCopper); k <= static_cast<int>(ItemKind::kOreMythril); ++k) {
                ore_after += post_kill.items[k];
            }
            const std::int32_t essence_after = post_kill.items[static_cast<int>(ItemKind::kEssence)];
            std::printf("  kill: boss dead=%s; player_kills %u -> %u; melee xp %u -> %u; ore %d -> %d; "
                       "essence %d -> %d\n",
                        boss_absent ? "yes" : "no", kills_before, kills_after, xp_before,
                        post_kill.skill_xp[static_cast<int>(Skill::kMelee)], ore_before, ore_after,
                        essence_before, essence_after);
            chk.expect(killed && boss_absent, "the player's strikes killed the boss");
            chk.expect(kills_after > kills_before, "the boss kill was counted");
            chk.expect(ore_after >= ore_before + 3,
                       "the boss's guaranteed ore row (qty 3-5, RFC-018 §10.2) paid out regardless "
                       "of realm — replacing the old flat 10-produce placeholder");
            chk.expect(essence_after == essence_before,
                       "Essence stayed dormant — this dojo room is a persistent interior, not a "
                       "challenge realm (§6's no-exceptions rule)");
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

    // --- RFC-015: client replication & interest-set protocol -----------------------------------------
    // No client-facing socket exists to test end to end (P6's territory, confirmed absent — see
    // replication.hpp's own header note). What IS real and testable: the interest-set/distance math
    // (pure), and ReplicationSession's baseline/delta engine driven against the LIVE SnapshotBus a
    // real ChunkActor publishes to — the exact same bus `client_main.cpp` reads today, just read
    // through the new session/delta machinery instead of directly.
    //
    // Placed here rather than earlier in the file (its first draft sat right after RFC-013/014,
    // before the wildlife/boss sections): the extra instance this block allocates and tears down was
    // observed to make the boss-respawn section's own already-documented actor-runtime flakiness
    // (see that section's "KNOWN ISSUE" comment, above) fail reliably instead of intermittently —
    // more live ChunkActors at the moment of a large single-burst `advance()` call made the
    // pre-existing timing sensitivity worse, not a new bug this block introduces. Running after the
    // boss section, once that timer has already resolved, avoids the interaction entirely.
    {
        // Pure §1 math: the interest set mirrors fan_beacons()'s own 5x5 window, clamped the same way.
        const ChunkCoord mid_home{kOverworld, 10, 10};
        const std::vector<ChunkCoord> mid_set = client_interest_set(kOverworld, mid_home);
        chk.expect(mid_set.size() == 25, "an interior home chunk gets the full 5x5=25 interest window");
        const ChunkCoord corner_home{kOverworld, 0, 0};
        const std::vector<ChunkCoord> corner_set = client_interest_set(kOverworld, corner_home);
        chk.expect(corner_set.size() == 9,
                   "a corner home chunk's window clamps to 3x3=9, same clamp fan_beacons() itself uses");
        chk.expect(chebyshev_distance(ChunkCoord{kOverworld, 12, 10}, mid_home) == 2 &&
                       chebyshev_distance(ChunkCoord{kOverworld, 11, 11}, mid_home) == 1 &&
                       chebyshev_distance(mid_home, mid_home) == 0,
                   "Chebyshev distance matches the inner/outer band split §4 keys on");

        // Pure §2 math: wire projections carry exactly what the RFC says and nothing else.
        Creature src{};
        src.id = 777;
        src.x = 12.5f;
        src.y = 8.25f;
        src.hp = 40;
        src.max_hp = 60;
        src.kind = CreatureKind::kWolf;
        src.facing = Facing::kLeft;
        src.windup = 5;
        src.disposition = Disposition::kHostile;
        src.status.primary = Channel::kHeat;
        src.status.stage = 2;
        src.status.coatings = static_cast<std::uint8_t>(1u << static_cast<unsigned>(Coating::kWet));
        const PublishedCreature pc = publish_of(src);
        chk.expect(pc.id == 777 && pc.x == 12.5f && pc.y == 8.25f && pc.hp == 40 && pc.max_hp == 60 &&
                       pc.kind == static_cast<std::uint8_t>(CreatureKind::kWolf) && pc.windup == 5,
                   "PublishedCreature carries id/position/vitals/kind/windup unchanged");
        chk.expect((pc.status & 0x07u) == static_cast<std::uint8_t>(Channel::kHeat) &&
                       ((pc.status >> 3) & 0x03u) == 2 && (pc.status & kPublishedStatusWetBit) != 0,
                   "the packed status byte round-trips channel, stage, and the Wet coating bit");

        // §7's instance-crossing full-swap and §3.1's baseline-on-subscribe, against the real engine.
        ReplicationSession session;
        world.spawn_wave_at(fx, fy, CreatureKind::kSlime, 4, /*seed*/ 4242);
        advance(world, ChunkActor::kIdlePublish + 5);  // past every chunk's own idle-publish floor once
        const ChunkCoord home1 = chunk_of(home, spawn.x, spawn.y);
        const ReplicationSession::Frame f1 = session.advance(world.bus(), home, home1, 1);
        chk.expect(!f1.baselines.empty(), "the first frame baselines every chunk that has already published");
        chk.expect(session.tracked_chunk_count() == f1.baselines.size(),
                   "every baselined chunk becomes a tracked chunk");
        const std::size_t tracked_after_first = session.tracked_chunk_count();

        const ReplicationSession::Frame f2 = session.advance(world.bus(), home, home1, 2);
        chk.expect(f2.baselines.empty(),
                   "the SAME home one tick later re-baselines nothing — every chunk is already tracked");
        chk.expect(session.tracked_chunk_count() == tracked_after_first,
                   "the tracked chunk set is stable when the player hasn't moved");

        PortalDef rep_portal{};
        rep_portal.id = 503;
        rep_portal.from_map = kOverworld;
        rep_portal.from_x = static_cast<std::uint16_t>(spawn.x);
        rep_portal.from_y = static_cast<std::uint16_t>(spawn.y);
        rep_portal.kind = PortalKind::kRealmGate;
        rep_portal.realm_type = RealmType::kChallenge;
        rep_portal.flavor = RealmFlavor::kDungeon;
        rep_portal.binding = PortalBinding::kAllocateOnUse;
        rep_portal.scope = SessionScope::kSoloInstance;
        MapDescriptor rep_desc{};
        // chunk_edge=3, not 2: at kInterestSpan=2 the window around home chunk (0,0) covers exactly
        // cx,cy in [0,2] — for edge=3 that is the WHOLE instance, so every candidate is genuinely
        // in-footprint. A smaller edge (e.g. 2) would leave candidates outside the real footprint,
        // and SnapshotBus::instance_local_index (map_system.hpp) does not bound-check cx/cy against
        // chunk_edge individually before combining them into one linear index — an out-of-footprint
        // candidate can alias a DIFFERENT real chunk's slot instead of cleanly returning null (e.g.
        // edge=2: (cx=2,cy=0) and (cx=0,cy=1) both resolve to local index 2). This is a real,
        // pre-existing gap in RFC-014's own addressing scheme, discovered while writing this test —
        // named here rather than fixed here, the same scope boundary RFC-015 §1 already draws around
        // fan_beacons()'s own instance-agnostic clamp (RFC-014 Open Question 7 owns both).
        rep_desc.chunk_edge = 3;
        rep_desc.biome = Ring::kForest;
        rep_desc.weather_mode = WeatherMode::kFixed;
        rep_desc.allow_free_build = false;

        LoginOutcome rep_out{};
        const int rep_slot = world.login("repltest", "hunter2", rep_out);
        chk.expect(rep_slot >= 0, "a dedicated account logs in for the replication instance-crossing test");
        if (rep_slot >= 0) {
            const std::uint64_t rep_key = world.key_of(rep_slot);
            const AccountId rep_acct = world.account_of(rep_slot);
            world.teleport_player(rep_key, kOverworld, spawn.x, spawn.y);
            const bool rep_entered = world.use_portal(rep_key, rep_portal, /*group*/ 4, rep_acct, rep_desc);
            chk.expect(rep_entered, "use_portal allocates a fresh instance for the replication test");
            const MapId rep_map = world.player_view(rep_slot).map;
            const ChunkCoord inst_home = chunk_of(rep_map, 1.5f, 1.5f);

            const ReplicationSession::Frame f3 = session.advance(world.bus(), rep_map, inst_home, 3);
            chk.expect(!f3.baselines.empty() && f3.deltas.empty(),
                       "crossing onto a new MapId re-baselines every chunk in the new window, no deltas");
            chk.expect(session.tracked_chunk_count() == 9,
                       "a chunk_edge=3 instance's whole 3x3 footprint matches the interest window "
                       "exactly and every one of the 9 chunks is tracked after crossing");

            world.leave_instance(rep_map, rep_acct);
            world.sweep_instances(3'000);
            world.sweep_instances(3'000 + kInstanceIdleGraceMs);
            world.disconnect_player(rep_key, kOverworld, rep_acct);
        }
        world.teleport_player(me, kOverworld, spawn.x, spawn.y);
    }
    std::printf("RFC-015 client replication: interest set, wire projections, baseline/delta "
               "tracking, and instance-crossing resubscribe all check out\n\n");

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

    // --- RFC-016: persistence & save-file format ----------------------------------------------------
    // First use of World::open_save() anywhere in this file — every login() before this point took
    // the (unaffected) fresh-BindAccount path throughout, exactly as before this RFC existed.
    const std::string p16_scratch_root =
        (std::filesystem::temp_directory_path() / "mmo_rfc016_test").string();
    int p16_slot = -1;
    std::uint16_t p16_hearth_tx = 0;
    std::uint16_t p16_hearth_ty = 0;
    PlayerView p16_checkpoint{};
    {
        std::error_code ec;
        std::filesystem::remove_all(p16_scratch_root, ec);  // a stale run from a prior crash

        // §2.1 manifest round trip.
        {
            std::filesystem::create_directories(p16_scratch_root, ec);
            const std::string path = p16_scratch_root + "/manifest.dat";
            WorldManifest m{};
            m.world_seed = 20260722;
            m.created_at_unix = 1784800000;
            std::snprintf(m.world_name, sizeof m.world_name, "%s", "Thung Lung Suong");
            chk.expect(save_world_manifest(path.c_str(), m), "a world manifest can be written");
            WorldManifest loaded{};
            chk.expect(load_world_manifest(path.c_str(), loaded) &&
                           loaded.world_seed == m.world_seed &&
                           loaded.created_at_unix == m.created_at_unix &&
                           std::strcmp(loaded.world_name, m.world_name) == 0,
                       "the manifest round-trips its seed, name, and creation time");
        }

        // §5's already-respawned ruling — persistent-band, instanced-band-with-a-return-point, and
        // instanced-band-with-no-return-point (§7's hearth fallback).
        {
            PlayerProgression dead{};
            dead.hp = 0;
            dead.map = kOverworld;
            dead.respawn_tx = 40;
            dead.respawn_ty = 50;
            apply_recovery_defaults(dead);
            chk.expect(dead.hp == kPlayerMaxHp && dead.mana == kPlayerMaxMana &&
                           dead.stamina == kPlayerMaxStamina,
                       "a persistent-band checkpoint with hp<=0 loads back at full vitals");
            chk.expect(dead.x == static_cast<float>(dead.respawn_tx) + 0.5f &&
                           dead.y == static_cast<float>(dead.respawn_ty) + 0.5f,
                       "...at the hearth, not wherever it died");

            PlayerProgression dead_instanced{};
            dead_instanced.hp = 0;
            dead_instanced.map = kPersistentBandEnd;  // an instanced MapId
            dead_instanced.return_map = kOverworld;
            dead_instanced.return_xy = 12u | (34u << 16);  // packed x=12, y=34 (return_x/return_y)
            apply_recovery_defaults(dead_instanced);
            chk.expect(dead_instanced.map == kOverworld && dead_instanced.x == 12.5f &&
                           dead_instanced.y == 34.5f,
                       "an instanced-band checkpoint with a return point ejects there on recovery");

            PlayerProgression dead_instanced_no_return{};
            dead_instanced_no_return.hp = 0;
            dead_instanced_no_return.map = kPersistentBandEnd;
            dead_instanced_no_return.respawn_tx = 7;
            dead_instanced_no_return.respawn_ty = 8;
            apply_recovery_defaults(dead_instanced_no_return);
            chk.expect(dead_instanced_no_return.map == kOverworld &&
                           dead_instanced_no_return.x == 7.5f && dead_instanced_no_return.y == 8.5f,
                       "...and falls back to the bound hearth when no return point was ever set (§7)");
        }

        // PlayerProgressionStore: a real FileStore round trip — write, close, then read back
        // through a FRESH store instance pointed at the same directory (simulating a restart).
        {
            const std::string root = p16_scratch_root + "/progression";
            PlayerProgression p;
            p.map = kOverworld;
            p.x = 111.5f;
            p.y = 222.5f;
            p.hp = 40;
            p.mana = 10;
            p.stamina = 20;
            p.deaths = 3;
            p.respawn_tx = 100;
            p.respawn_ty = 200;
            p.level.assign(kSkillCount, 0);
            p.xp.assign(kSkillCount, 0);
            p.level[static_cast<int>(Skill::kMelee)] = 7;
            p.xp[static_cast<int>(Skill::kMelee)] = 555;
            p.items.assign(kItemKinds, 0);
            p.items[static_cast<int>(ItemKind::kWood)] = 42;
            {
                PlayerProgressionStore writer;
                writer.open(root);
                writer.save(1001, p);
            }  // closed — the FileStore below opens the same .qwal file fresh, like a restart

            PlayerProgressionStore reader;
            reader.open(root);
            std::optional<PlayerProgression> back = reader.load(1001);
            chk.expect(back.has_value(),
                       "a saved player row is present for a fresh store instance at the same root");
            if (back.has_value()) {
                chk.expect(back->x == p.x && back->y == p.y && back->hp == p.hp &&
                               back->deaths == p.deaths,
                           "vitals/position/deaths round-trip through disk");
                chk.expect(back->level.size() == static_cast<std::size_t>(kSkillCount) &&
                               back->level[static_cast<int>(Skill::kMelee)] == 7 &&
                               back->xp[static_cast<int>(Skill::kMelee)] == 555,
                           "skill levels/xp round-trip through disk");
                chk.expect(back->items.size() == static_cast<std::size_t>(kItemKinds) &&
                               back->items[static_cast<int>(ItemKind::kWood)] == 42,
                           "items round-trip through disk");
            }
            chk.expect(!reader.load(9999).has_value(), "an account with no saved row loads as nullopt");
        }

        // WorldOverlayStore: record the five shipped event shapes into one chunk, then recover it
        // through a FRESH store instance — proves §6.4's fold, not just live memory.
        {
            const std::string root = p16_scratch_root + "/overlay";
            const ChunkCoord coord{kOverworld, 5, 5};
            {
                WorldOverlayStore writer;
                writer.open(root);
                chk.expect(!writer.recover(coord).has_value(),
                           "an untouched chunk has nothing to recover");
                writer.record(coord, ChunkMutationEvent{OverlayEventKind::kPlaceBuilding, 100, 100,
                                                        static_cast<std::uint8_t>(BuildKind::kPlot),
                                                        0, 1});
                writer.record(coord, ChunkMutationEvent{OverlayEventKind::kUpgradeBuilding, 100, 100,
                                                        0, 0, 1});
                writer.record(coord,
                              ChunkMutationEvent{OverlayEventKind::kTillGround, 101, 101, 0, 0, 1});
                writer.record(coord, ChunkMutationEvent{OverlayEventKind::kPlantCrop, 101, 101,
                                                        static_cast<std::uint8_t>(CropKind::kWheat),
                                                        1000, 1});
                writer.record(coord, ChunkMutationEvent{OverlayEventKind::kPlantCrop, 102, 102,
                                                        static_cast<std::uint8_t>(CropKind::kCarrot),
                                                        2000, 1});
                writer.record(coord,
                              ChunkMutationEvent{OverlayEventKind::kHarvestAt, 102, 102, 0, 0, 1});
            }  // closed — same "restart before reading it back" discipline as the store above

            WorldOverlayStore reader;
            reader.open(root);
            std::optional<ChunkOverlaySnapshot> snap = reader.recover(coord);
            chk.expect(snap.has_value(),
                       "a touched chunk's overlay recovers through a fresh store instance");
            if (snap.has_value()) {
                chk.expect(snap->buildings.size() == 1 && snap->buildings[0].level == 2,
                           "PlaceBuilding + UpgradeBuilding fold into one level-2 building");
                chk.expect(snap->buildings[0].hp == max_hp_of(BuildKind::kPlot, 2),
                           "the folded building's hp matches its level — never a damaged value (§6.6)");
                chk.expect(snap->tilled.size() == 1 && snap->tilled[0].tx == 101 &&
                               snap->tilled[0].ty == 101,
                           "TillGround folds into the tilled-tile list");
                chk.expect(snap->crops.size() == 1 && snap->crops[0].tx == 101,
                           "the harvested crop (102,102) is gone; the un-harvested one (101,101) "
                           "remains");
            }
        }

        // Live integration: World::open_save()/login()'s restore path, against the real engine.
        chk.expect(world.open_save(p16_scratch_root, "persisttest_world"),
                   "open_save creates a save directory for a live World");

        LoginOutcome p16_out{};
        p16_slot = world.login("persisttest", "hunter2", p16_out);
        chk.expect(p16_slot >= 0 && p16_out == LoginOutcome::kCreated,
                   "a brand-new account still gets the fresh starter pack (no saved row yet)");
        if (p16_slot >= 0) {
            const std::uint64_t p16_key = world.key_of(p16_slot);
            const AccountId p16_acct = world.account_of(p16_slot);

            // Well clear of every hearth/farm tile the sections above already placed near spawn.
            const float p16_home_x =
                std::clamp(spawn.x + 300.0f, 10.0f, static_cast<float>(kMapTiles - 10));
            const float p16_home_y =
                std::clamp(spawn.y - 300.0f, 10.0f, static_cast<float>(kMapTiles - 10));
            world.teleport_player(p16_key, kOverworld, p16_home_x, p16_home_y);
            advance(world, 2);

            world.grant_xp(p16_key, Skill::kMelee, 500);
            world.grant_vitals(p16_key, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            p16_hearth_tx = static_cast<std::uint16_t>(p16_home_x);
            p16_hearth_ty = static_cast<std::uint16_t>(p16_home_y);
            const bool p16_lit =
                world.build_at(p16_key, kOverworld, p16_hearth_tx, p16_hearth_ty, BuildKind::kHearth);
            chk.expect(p16_lit, "the persistence test account could afford its hearth");
            world.till(p16_key, kOverworld, static_cast<std::uint16_t>(p16_hearth_tx + 1),
                      p16_hearth_ty);
            world.plant(p16_key, kOverworld, static_cast<std::uint16_t>(p16_hearth_tx + 1),
                       p16_hearth_ty, CropKind::kWheat, 0);
            advance(world, 2);

            p16_checkpoint = world.player_view(p16_slot);
            world.checkpoint_progression();  // §6.3 point 1, forced rather than waited for
            world.compact_overlay();         // §6.3 point 2

            // Diverge the LIVE state past the checkpoint, without checkpointing again — proves a
            // later restore reads the DISK snapshot, not whatever memory happens to hold.
            for (int i = 0; i < 20; ++i) world.move_player(p16_key, 1.0f, 0.0f);
            advance(world, 4);
            const PlayerView p16_diverged = world.player_view(p16_slot);
            chk.expect(std::abs(p16_diverged.x - p16_checkpoint.x) > 0.5f,
                       "the live position moved well past the checkpoint");

            world.disconnect_player(p16_key, kOverworld, p16_acct);
            LoginOutcome p16_out2{};
            const int p16_slot2 = world.login("persisttest", "hunter2", p16_out2);
            chk.expect(p16_slot2 == p16_slot, "the same account reuses the same session slot");
            const PlayerView p16_restored = world.player_view(p16_slot2);
            chk.expect(std::abs(p16_restored.x - p16_checkpoint.x) < 0.01f &&
                           std::abs(p16_restored.y - p16_checkpoint.y) < 0.01f,
                       "login restores the CHECKPOINTED position, not the diverged live one");
            chk.expect(p16_restored.hp == p16_checkpoint.hp, "vitals restore from the checkpoint too");
            chk.expect(p16_restored.skill_xp[static_cast<int>(Skill::kMelee)] ==
                           p16_checkpoint.skill_xp[static_cast<int>(Skill::kMelee)],
                       "skill progress restores from the checkpoint");
            chk.expect(p16_restored.respawn_tx == p16_hearth_tx &&
                           p16_restored.respawn_ty == p16_hearth_ty,
                       "the hearth's respawn point restores from the checkpoint");

            world.disconnect_player(p16_key, kOverworld, p16_acct);
        }
        // Persists accounts.dat too — the second bring-up below needs the SAME AccountId for
        // "persisttest", not a freshly-minted one, to resolve its progression row.
        world.save_world_now();
    }
    std::printf("RFC-016 persistence: manifest, progression/overlay durability, and login-restore "
               "all check out\n\n");

    // RFC-024 §3.5: World::connected_player_count() — the host-side courtesy notice's input. A
    // fresh login/disconnect pair, isolated from every other slot this file may have bound, so the
    // assertion holds regardless of what ran earlier.
    {
        const int p24_before = world.connected_player_count();
        LoginOutcome p24_out{};
        const int p24_slot = world.login("rfc024courtesy", "hunter2", p24_out);
        chk.expect(p24_slot >= 0, "the courtesy-notice test account logs in");
        if (p24_slot >= 0) {
            chk.expect(world.connected_player_count() == p24_before + 1,
                       "connected_player_count() counts the newly bound slot");
            world.disconnect_player(world.key_of(p24_slot), kOverworld, world.account_of(p24_slot));
            chk.expect(world.connected_player_count() == p24_before + 1,
                       "disconnect_player() is data-level only (RFC-014) and never frees `bound_` — "
                       "connected_player_count() stays inflated by design, documented on the method");
        }
    }

    // RFC-024 §2.2/§3.2: ClientLivenessTracker — pure state machine, no transport, exercised
    // directly (mirrors ReplicationSession's own "exercised directly by tests" precedent, RFC-015).
    {
        ClientLivenessTracker t;
        chk.expect(t.state() == ConnectionState::kConnected,
                   "a freshly constructed tracker starts Connected");
        chk.expect(t.advance(10'000) == ConnectionState::kConnected,
                   "no traffic ever received yet never times out on its own");

        t.on_traffic(0);
        chk.expect(t.advance(kClientLeaderTimeoutTicks - 1) == ConnectionState::kConnected,
                   "one tick short of the timeout stays Connected");
        chk.expect(t.advance(kClientLeaderTimeoutTicks) == ConnectionState::kLeaderUnreachable,
                   "kClientLeaderTimeoutTicks with no traffic declares LeaderUnreachable (§3.2)");

        t.on_traffic(kClientLeaderTimeoutTicks + 5);
        chk.expect(t.state() == ConnectionState::kConnected,
                   "traffic resuming moves LeaderUnreachable back to Connected (§3.2)");

        t.on_world_closing();
        chk.expect(t.state() == ConnectionState::kHostClosed,
                   "WorldClosing moves straight to HostClosed, skipping the timeout wait");
        t.on_traffic(kClientLeaderTimeoutTicks + 1000);
        chk.expect(t.state() == ConnectionState::kHostClosed,
                   "HostClosed is terminal - no diagram edge leads back out of it (§3.2)");
    }

    // RFC-024 §2.1: NodeLivenessTracker — the peer-node-side counterpart, against Tick fan-out.
    {
        NodeLivenessTracker n;
        chk.expect(!n.leader_unreachable(1'000'000),
                   "a node that has never seen a Tick does not declare unreachable on its own");
        n.on_tick(100);
        chk.expect(!n.leader_unreachable(100 + kNodeLeaderTimeoutTicks - 1),
                   "one tick short of kNodeLeaderTimeoutTicks stays reachable");
        chk.expect(n.leader_unreachable(100 + kNodeLeaderTimeoutTicks),
                   "kNodeLeaderTimeoutTicks with no further Tick declares the leader unreachable");
    }

    // RFC-024 §3.5: the host-side courtesy line — a fact, never a gate.
    {
        chk.expect(host_closing_notice(1) ==
                       "1 other player is connected - closing now will disconnect them.",
                   "host_closing_notice() singularizes for exactly one other player");
        chk.expect(host_closing_notice(3) ==
                       "3 other players are connected - closing now will disconnect them.",
                   "host_closing_notice() pluralizes for more than one");
    }
    std::printf("RFC-024 leader failure & session recovery: connected_player_count() and the pure "
               "liveness trackers check out\n\n");

    // RFC-008: load the real generated combat pack (tools/build_combat_pack.py's output) and
    // verify it end-to-end - hash check, JSON parse, typed skill/entity lookups by u16 id. Not a
    // mock: this is the exact `assets/_gen/combat_pack.json` the packer wrote from
    // `data/combat/**.json`.
    {
        std::shared_ptr<const CombatPack> pack;
        for (const char* dir : {"assets/_gen", "../assets/_gen", "../../assets/_gen"}) {
            pack = CombatPack::load(dir);
            if (pack) break;
        }
        chk.expect(pack != nullptr, "CombatPack::load() finds and hash-verifies the generated pack");
        if (pack) {
            chk.expect(pack->struct_hash() == kCombatPackStructHash &&
                           pack->full_hash() == kCombatPackFullHash,
                       "the loaded pack's hashes match combat_ids.hpp's generated constants");
            chk.expect(pack->id_of("skill.meteor") == combat_ids::kSkillMeteor &&
                           pack->id_of("skill.spike") == combat_ids::kSkillSpike,
                       "id_of() agrees with the packer's own id assignment");

            const SkillDoc* meteor = pack->skill_by_id(combat_ids::kSkillMeteor);
            chk.expect(meteor != nullptr && meteor->name == "Meteor" && meteor->element == "rock",
                       "skill_by_id() returns Meteor's real authored fields");
            if (meteor != nullptr) {
                chk.expect(meteor->player_castable && meteor->cost_vital == "mana" &&
                               meteor->cost_amount == 35 && meteor->cooldown_ticks == 200,
                           "Meteor's player-cast fields round-trip through the pack");
                chk.expect(meteor->payload_damage == 120 && meteor->payload_crush == 180 &&
                               meteor->payload_impulse == 220 && meteor->payload_explosion == 200 &&
                               meteor->payload_heat == 40,
                           "Meteor's impact payload matches RFC-003 §10's canonical numbers");
            }

            const SkillDoc* spike = pack->skill_by_id(combat_ids::kSkillSpike);
            chk.expect(spike != nullptr && spike->name == "Spike" && spike->payload_pierce == 100 &&
                           spike->payload_crush == 60,
                       "skill_by_id() returns Spike's real authored fields");

            const EntityDoc* meteor_body = pack->entity_by_id(combat_ids::kEntityMeteorBody);
            chk.expect(meteor_body != nullptr && meteor_body->hp == 40 &&
                           meteor_body->collision == "none" && meteor_body->destroyable,
                       "entity_by_id() returns meteor_body's real authored fields");

            const EntityDoc* rock_spike = pack->entity_by_id(combat_ids::kEntityRockSpike);
            chk.expect(rock_spike != nullptr && rock_spike->hp == 30 &&
                           rock_spike->collision == "ground" && rock_spike->material == "stone",
                       "entity_by_id() returns rock_spike's real authored fields");

            chk.expect(pack->skill_by_id(0) == nullptr && pack->entity_by_id(0) == nullptr,
                       "id 0 is reserved (V10) and never resolves to a document");

            const JsonValue* wet = pack->find("status", "status.wet");
            chk.expect(wet != nullptr && wet->get_string("kind") == "coating" &&
                           wet->get_int("default_ticks") == 80,
                       "find() reaches the generic (untyped) status domain too");
        }
    }
    std::printf("RFC-008 data-driven skill definition: the generated combat pack loads, "
               "hash-verifies, and resolves real skill/entity documents by id\n\n");

    // RFC-019 §5.1/§5.6/§5.7: progression's XP overflow-closure, respec, and the Essence gate — all
    // on ONE new account. `kMaxPlayers == 8` is a fixed SESSION-SLOT count and `disconnect_player()`
    // is data-level only (RFC-024's own finding, above: it never frees `bound_`), so this file's
    // total distinct account count is a hard, permanent ceiling, not a per-test budget — by this
    // point in the file exactly one slot remains, so every RFC-019 scenario below shares it, and the
    // co-op ledger test after it reuses two ALREADY-logged-in accounts (`me`/`guest`) rather than
    // minting new ones. Skills are chosen per sub-test so the running total never exceeds the
    // 34-point cap except exactly where a sub-test means to hit it, and Melee is left untouched on
    // this account so it stays available, fresh, for the ledger test's reuse below.
    // `xp_to_reach`/`xp_for_level` are the shipped formulas themselves (tiles.hpp), not a parallel
    // test-only model of them, so these assertions check the real conversion path.
    {
        LoginOutcome p19_out{};
        const int p19_slot = world.login("rfc019progress", "hunter2", p19_out);
        chk.expect(p19_slot >= 0, "the progression test account logs in (the file's last free slot)");
        if (p19_slot >= 0) {
            const std::uint64_t p19_key = world.key_of(p19_slot);
            const int gate0 = kEssenceGateStartLevel - 1;  // 17: one short of the gate

            // --- §5.7: the Essence gate — Magic climbs cleanly from 0 to 20, 20 of 34 points -------
            world.grant_xp(p19_key, Skill::kMagic, xp_to_reach(static_cast<std::uint8_t>(gate0)));
            advance(world, 1);
            PlayerView v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] == gate0 &&
                           v.skill_xp[static_cast<int>(Skill::kMagic)] == 0,
                       "Magic reaches level 17 exactly, one short of the Essence gate");

            world.grant_xp(p19_key, Skill::kMagic, xp_for_level(static_cast<std::uint8_t>(gate0)));
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] == gate0 &&
                           v.skill_xp[static_cast<int>(Skill::kMagic)] ==
                               xp_for_level(static_cast<std::uint8_t>(gate0)) &&
                           v.essence_paid[static_cast<int>(Skill::kMagic)] == 0,
                       "enough XP for level 18 banks AT the threshold and waits — no Essence paid "
                       "yet, and nothing is lost while it does (§5.7)");

            world.grant_essence(p19_key, Skill::kMagic, 1);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] == kEssenceGateStartLevel &&
                           v.skill_xp[static_cast<int>(Skill::kMagic)] == 0 &&
                           v.essence_paid[static_cast<int>(Skill::kMagic)] == 1,
                       "one Essence unit unblocks the banked XP — level 18 commits immediately");

            world.grant_xp(p19_key, Skill::kMagic, xp_for_level(kEssenceGateStartLevel));
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] == kEssenceGateStartLevel &&
                           v.essence_paid[static_cast<int>(Skill::kMagic)] == 1,
                       "level 19 needs a SECOND Essence unit — one is not enough (1 unit per level)");

            world.grant_essence(p19_key, Skill::kMagic, 2);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] ==
                               kEssenceGateStartLevel + 1 &&
                           v.essence_paid[static_cast<int>(Skill::kMagic)] == kEssenceGateTotal,
                       "essence_paid caps at kEssenceGateTotal (3) and level 19 commits");

            world.grant_xp(p19_key, Skill::kMagic, xp_for_level(kEssenceGateStartLevel + 1));
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] == kMaxSkillLevel,
                       "the gate is already fully paid — level 20 commits on XP alone");

            // --- §5.1: a fully-maxed branch drops further grants instead of banking them forever ---
            world.grant_xp(p19_key, Skill::kMagic, 5000);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_xp[static_cast<int>(Skill::kMagic)] == 0,
                       "a grant to a fully-maxed branch is dropped, not banked (§5.1's overflow fix)");

            // --- §5.1: a grant blocked by the GLOBAL cap is dropped too, even below max level -------
            world.grant_xp(p19_key, Skill::kRanged, xp_to_reach(14));  // Magic 20 + Ranged 14 == 34
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kRanged)] == 14,
                       "Ranged reaches exactly level 14, putting total levels at the 34-point cap");
            world.grant_xp(p19_key, Skill::kCraft, 1000);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kCraft)] == 0 &&
                           v.skill_xp[static_cast<int>(Skill::kCraft)] == 0,
                       "a grant to an under-max branch is STILL dropped once the global cap is hit");

            // --- §5.6: respec, gated on standing at the player's own Hearth -------------------------
            // Teleported well clear of every other test's hearth/farm tiles near the shared spawn
            // point (same convention RFC-016's own persistence test already uses, above).
            const float hx = std::floor(std::clamp(spawn.x + 300.0f, 10.0f,
                                                   static_cast<float>(kMapTiles - 10))) +
                            0.5f;
            const float hy = std::floor(std::clamp(spawn.y + 300.0f, 10.0f,
                                                   static_cast<float>(kMapTiles - 10))) +
                            0.5f;
            world.teleport_player(p19_key, kOverworld, hx, hy);
            advance(world, 2);
            const bool hearth_paid =
                world.build_at(p19_key, kOverworld, static_cast<std::uint16_t>(hx),
                               static_cast<std::uint16_t>(hy), BuildKind::kHearth);
            advance(world, 2);
            v = world.player_view(p19_slot);
            chk.expect(hearth_paid && v.respawn_tx == static_cast<std::uint16_t>(hx) &&
                           v.respawn_ty == static_cast<std::uint16_t>(hy),
                       "the fresh starter pack affords a Hearth (20 of 25 stone) and it actually "
                       "lands, moving the respawn point");

            const std::uint32_t ranged_xp_before = v.skill_xp[static_cast<int>(Skill::kRanged)];
            world.teleport_player(p19_key, kOverworld, hx + 50.0f, hy);  // outside kRespecRadiusTiles
            advance(world, 1);
            world.respec_skill(p19_key, Skill::kRanged, Skill::kCraft);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kRanged)] == 14 &&
                           v.skill_xp[static_cast<int>(Skill::kRanged)] == ranged_xp_before,
                       "respec away from the Hearth is refused outright — nothing moved");

            world.teleport_player(p19_key, kOverworld, hx, hy);  // back at the Hearth
            advance(world, 1);
            const std::uint32_t ranged_committed = xp_to_reach(14);
            world.respec_skill(p19_key, Skill::kRanged, Skill::kCraft);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kRanged)] == 0 &&
                           v.skill_xp[static_cast<int>(Skill::kRanged)] == 0,
                       "respec at the Hearth resets the `from` branch to 0/0");
            auto total_xp_of = [&](Skill s) {
                const int i = static_cast<int>(s);
                return xp_to_reach(v.skill_level[i]) + v.skill_xp[i];
            };
            auto expected_refund_of = [](std::uint32_t committed) {
                return static_cast<std::uint32_t>(static_cast<std::uint64_t>(committed) *
                                                  kRespecRefundPm / 1000u);
            };
            const std::uint32_t craft_total_after = total_xp_of(Skill::kCraft);
            const std::uint32_t ranged_refund = expected_refund_of(ranged_committed);
            chk.expect(ranged_refund > 0 && craft_total_after == ranged_refund,
                       "Craft's new total XP equals exactly 75% of Ranged's committed value — the "
                       "other 25% is unconditionally forfeit, not banked anywhere (§5.6)");

            // Respec also clears the RESET branch's Essence-gate progress (Magic was fully paid to
            // kEssenceGateTotal above) — a fresh climb re-pays it. Ranged is the refund target this
            // time (freed back to 0 by the respec just above), and its OWN essence_paid_ is still 0,
            // so — a second real-world consequence of the same mechanic — the refunded XP converts
            // only as far as Ranged's own Essence gate allows, banking the rest, never losing it.
            const std::uint32_t magic_committed = xp_to_reach(kMaxSkillLevel);
            world.respec_skill(p19_key, Skill::kMagic, Skill::kRanged);
            advance(world, 1);
            v = world.player_view(p19_slot);
            chk.expect(v.skill_level[static_cast<int>(Skill::kMagic)] == 0 &&
                           v.essence_paid[static_cast<int>(Skill::kMagic)] == 0,
                       "respec resets essence_paid_ for the reset branch alongside its level");
            const std::uint32_t ranged_total_after2 = total_xp_of(Skill::kRanged);
            const std::uint32_t magic_refund = expected_refund_of(magic_committed);
            chk.expect(magic_refund > 0 && ranged_total_after2 == magic_refund,
                       "Ranged's new total XP equals exactly 75% of Magic's committed value too — "
                       "conserved in full even though Ranged's own (unpaid) Essence gate blocks it "
                       "from converting all the way, proving the wait banks rather than drops it");

            // --- §5.8: the multiplayer contribution ledger ------------------------------------------
            // A shared kill grants FULL kill XP to every recent contributor, not a divided share and
            // not just whoever landed the killing blow. Reuses `p19_key` (this account's own Melee
            // is untouched by every sub-test above) alongside `guest` — an ALREADY-logged-in account,
            // per the account-budget note above — rather than minting a third new one; `guest`'s own
            // Melee is likewise untouched (every earlier use of it casts spells or channels an
            // ability, never swings), so a kill's Melee XP is a clean, detectable signal for both.
            // A blind coordinate offset is not good enough here — the slime's HP is ring-scaled
            // (higher outside Meadow) and the tile itself might not even be walkable (a silent
            // spawn_one_at rejection, chunk_actor.hpp's SpawnWave handler). `find_meadow_tile` is
            // the same proven helper the WhirlCleave/RainCall fights above already stake their own
            // walkable, ring-0 (flat 30 hp Slime, no scaling) ground on.
            int lxi = -1, lyi = -1;
            const bool have_ledger_tile = find_meadow_tile(kHomeTx + 150, kHomeTy + 150, lxi, lyi);
            chk.expect(have_ledger_tile, "a walkable meadow tile exists for the ledger-test fight");
            const float lx = static_cast<float>(lxi) + 0.5f;
            const float ly = static_cast<float>(lyi) + 0.5f;
            // `guest` fights several magic battles earlier in this file and can still be mid-
            // respawn (`dead_ticks_ > 0`) from creature retaliation there — `grant_vitals` tops up
            // hp/mana/stamina but does not clear that countdown, and `PlanAttack` refuses every
            // swing while it is nonzero (player_actor.hpp). Wait it out first, the same way the
            // death test earlier in this file waits with `advance(world, kRespawnTicks + 5)`.
            if (world.player_view(slot2).dead_ticks > 0 || world.player_view(p19_slot).dead_ticks > 0) {
                advance(world, kRespawnTicks + 5);
            }
            world.teleport_player(p19_key, kOverworld, lx, ly);
            world.teleport_player(guest, kOverworld, lx, ly);
            world.grant_vitals(p19_key, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            world.grant_vitals(guest, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            advance(world, 2);
            world.spawn_one_at(static_cast<std::uint16_t>(lx), static_cast<std::uint16_t>(ly),
                               CreatureKind::kSlime, kOverworld);
            advance(world, 2);

            const std::uint32_t p19_melee_xp_before =
                world.player_view(p19_slot).skill_xp[static_cast<int>(Skill::kMelee)];
            const std::uint32_t guest_xp_before =
                world.player_view(slot2).skill_xp[static_cast<int>(Skill::kMelee)];

            const ChunkCoord ledger_chunk = chunk_of(kOverworld, lx, ly);

            // `p19_key` lands ONE light swing (14 damage against a 30-hp slime — it survives) and
            // never touches it again; `guest` alone finishes it off. `player_kills` is a world-wide
            // counter (every player-caused kill anywhere), so the real signal is this chunk's own
            // hostile count dropping — a global-counter delta could pass spuriously off an unrelated
            // kill happening elsewhere during these same ticks.
            const bool tag_hit = world.swing(p19_key, /*heavy*/ false);
            advance(world, 2);
            chk.expect(tag_hit, "the tagging swing lands");
            for (int i = 0; i < 4; ++i) {
                world.swing(guest, /*heavy*/ true);
                advance(world, 2);
            }

            chk.expect(world.chunk_stats(ledger_chunk).hostile == 0, "the slime died");

            const PlayerView p19_after = world.player_view(p19_slot);
            const PlayerView guest_after = world.player_view(slot2);
            chk.expect(p19_after.skill_xp[static_cast<int>(Skill::kMelee)] > p19_melee_xp_before ||
                           p19_after.skill_level[static_cast<int>(Skill::kMelee)] > 0,
                       "the player who only tagged it — and never landed the killing blow — still "
                       "gets full kill XP, from the shared contribution ledger (§5.8)");
            chk.expect(guest_after.skill_xp[static_cast<int>(Skill::kMelee)] > guest_xp_before ||
                           guest_after.skill_level[static_cast<int>(Skill::kMelee)] > 0,
                       "the player who landed the killing blow also gets full kill XP, not a "
                       "divided share between the two contributors");

            // --- RFC-011 §5: the manual loadout picker ----------------------------------------------
            // Still `p19_key`: the Magic->Ranged respec above reset Magic to 0, leaving Ranged
            // (level 17) the strongest fighting school — its auto-pick pair (FanVolley/SmokeBomb) is
            // real, live-eligible content to pick between, and Magic's now-0 level makes a clean
            // "ineligible" negative case for free.
            const PlayerView lo0 = world.player_view(p19_slot);
            chk.expect(lo0.ability[0] == AbilityId::kFanVolley &&
                           lo0.ability[1] == AbilityId::kSmokeBomb &&
                           lo0.loadout_raw[0] == kLoadoutAuto && lo0.loadout_raw[1] == kLoadoutAuto,
                       "before any manual pick, both slots still show the shipped auto-pick, and "
                       "the raw picker state confirms neither slot has ever been touched");

            // No duplicate (§5.1): each slot's CURRENT (auto-resolved) value is exactly what the
            // OTHER slot would collide with — the natural case, no artificial setup needed.
            world.set_loadout(p19_key, 0, AbilityId::kSmokeBomb);
            advance(world, 1);
            chk.expect(world.player_view(p19_slot).ability[0] == AbilityId::kFanVolley,
                       "a pick that would duplicate the other (still-auto) slot's ability is "
                       "refused — slot F is unchanged");

            // Cross-branch, free, instant: unlock WhirlCleave (Melee) so a genuine three-way choice
            // exists, then manually equip it — an ability from a THIRD school, not a re-pick of the
            // pair auto-selection already carries.
            world.grant_xp(p19_key, Skill::kMelee, xp_to_reach(2));
            advance(world, 1);
            world.set_loadout(p19_key, 0, AbilityId::kWhirlCleave);
            advance(world, 1);
            PlayerView lo1 = world.player_view(p19_slot);
            chk.expect(lo1.ability[0] == AbilityId::kWhirlCleave &&
                           lo1.loadout_raw[0] == static_cast<std::uint8_t>(AbilityId::kWhirlCleave),
                       "a cross-branch manual pick of an eligible ability lands in the slot and "
                       "the raw picker state records it as manual, not auto");
            chk.expect(lo1.ability[1] == AbilityId::kSmokeBomb,
                       "slot G is untouched by slot F's manual pick");

            // Eligibility is re-validated server-side regardless of what the client asked for —
            // Magic is 0 now, so Nova (unlock level 2) is not eligible no matter the slot.
            world.set_loadout(p19_key, 1, AbilityId::kElementalNova);
            advance(world, 1);
            chk.expect(world.player_view(p19_slot).ability[1] == AbilityId::kSmokeBomb,
                       "a pick of an ability the player's levels do not make eligible is refused");

            // Out-of-combat gate (§5.1): reuses RFC-013's own combat-cooldown flag, not a new timer.
            world.hurt_player(p19_key, 1);
            advance(world, 1);
            world.set_loadout(p19_key, 1, AbilityId::kFanVolley);
            advance(world, 1);
            chk.expect(world.player_view(p19_slot).ability[1] == AbilityId::kSmokeBomb,
                       "a loadout change is refused while still inside the combat cooldown window");
            advance(world, kCombatCooldownMs / kTickMs + 2);  // wait it out
            world.set_loadout(p19_key, 1, AbilityId::kFanVolley);
            advance(world, 1);
            chk.expect(world.player_view(p19_slot).ability[1] == AbilityId::kFanVolley &&
                           world.player_view(p19_slot).loadout_raw[1] ==
                               static_cast<std::uint8_t>(AbilityId::kFanVolley),
                       "once out of combat again, the same pick succeeds");

            // "Reset to Auto" (§5.1/§5.3): AbilityId::kCount is the wire sentinel: it always
            // succeeds and hands the slot back to the shipped auto-pick rule, permanently, until
            // the picker is opened again.
            world.set_loadout(p19_key, 0, AbilityId::kCount);
            advance(world, 1);
            const PlayerView lo2 = world.player_view(p19_slot);
            chk.expect(lo2.ability[0] == AbilityId::kWhirlCleave &&
                           lo2.loadout_raw[0] == kLoadoutAuto,
                       "Reset to Auto clears the manual flag; the slot happens to auto-resolve to "
                       "the same ability here only because Melee is now the strongest school");

            // Respec clears a manual pick it invalidates (RFC-019 §5.6/RFC-011 §5.4) — a MANUAL
            // slot does not silently fall back to auto; it goes genuinely empty (AbilityId::kCount)
            // until the player picks again. Slot G is still manually FanVolley (Ranged); respeccing
            // Ranged away must clear it, not leave it pointing at a now-ineligible ability.
            world.teleport_player(p19_key, kOverworld, hx, hy);  // back at the Hearth
            advance(world, 1);
            world.respec_skill(p19_key, Skill::kRanged, Skill::kCraft);
            advance(world, 1);
            const PlayerView lo3 = world.player_view(p19_slot);
            chk.expect(lo3.ability[1] == AbilityId::kCount && lo3.loadout_raw[1] == kLoadoutAuto,
                       "a respec that drops a branch below a manually-equipped ability's unlock "
                       "tier clears that slot to genuinely empty, not a silent auto-pick fallback");

            // Persistence round-trip (RFC-016 §4.1/RFC-011 §5.3), pure-function: the raw picker
            // byte — not the resolved ability — is what must survive a restart, so a never-touched
            // slot can never come back mistaken for a manual lock-in.
            PlayerView pv{};
            pv.loadout_raw[0] = static_cast<std::uint8_t>(AbilityId::kCrushBlow);
            pv.loadout_raw[1] = kLoadoutAuto;
            const PlayerProgression pp = progression_of(pv);
            const RestoreProgression rp = restore_message(1, pp);
            chk.expect(rp.loadout[0] == static_cast<std::uint8_t>(AbilityId::kCrushBlow) &&
                           rp.loadout[1] == kLoadoutAuto,
                       "the raw loadout byte round-trips through PlayerProgression unchanged, "
                       "manual and auto slots alike");
        }
    }
    std::printf("RFC-019 progression & co-op fairness: the Essence gate, XP overflow-closure, "
               "respec, and the shared-kill contribution ledger (§5.1/§5.6/§5.7/§5.8) check out\n\n");
    std::printf("RFC-011 combat HUD/input: the manual loadout picker's eligibility/no-duplicate/"
               "out-of-combat gates, Reset to Auto, respec-clears-a-manual-slot, and the raw "
               "loadout-byte persistence round-trip all check out\n\n");

    world.stop();

    // RFC-016 §6.4, direct: ChunkActor::apply_recovered_overlay() — the exact call
    // World::recover_overlay() makes per chunk at bring-up — wired against a real ChunkActor,
    // without paying for a second full engine+router bring-up. A second full World was tried here
    // first; it built and ran correctly most of the time but occasionally hung, the same
    // load-triggered actor-runtime timing flakiness RFC-015's own test block already ran into with
    // the boss-respawn timer (see that block's header note) — not a logic bug in this RFC's code,
    // but not worth the reliability cost for what this narrower test proves just as well. The
    // disk-durability half ("a fresh store instance reads back what a closed one wrote") is already
    // proven above by PlayerProgressionStore's and WorldOverlayStore's own round trips; this proves
    // the remaining piece — that a recovered ChunkOverlaySnapshot actually lands on a live chunk.
    if (p16_slot >= 0) {
        SnapshotBus local_bus;
        ChunkActor ch;
        ch.coord = ChunkCoord{kOverworld, 9, 9};
        ch.bus = &local_bus;
        ch.generate_terrain(kWorldSeed);

        ChunkOverlaySnapshot snap;
        Building b{};
        b.tx = 288;
        b.ty = 288;
        b.kind = BuildKind::kPlot;
        b.level = 2;
        b.hp = max_hp_of(BuildKind::kPlot, 2);
        snap.buildings.push_back(b);
        Crop c{};
        c.tx = 289;
        c.ty = 288;
        c.kind = CropKind::kCarrot;
        c.stage = 0;
        c.planted_ms = 5000;
        c.ripe_ms = 5000 + grow_ms_of(CropKind::kCarrot);
        snap.crops.push_back(c);
        snap.tilled.push_back(TilledTile{289, 288});
        snap.tilled.push_back(TilledTile{290, 288});

        chk.expect(ch.tilled_count() == 0, "a freshly generated chunk starts with no tilled tiles");
        ch.apply_recovered_overlay(snap);
        ch.publish_now();

        chk.expect(ch.tilled_count() == 2, "apply_recovered_overlay() updates the tilled counter too");
        ChunkViewPtr view = local_bus.load(ch.coord);
        chk.expect(view != nullptr, "apply_recovered_overlay publishes a real ChunkView");
        if (view) {
            chk.expect(view->buildings.size() == 1 && view->buildings[0].tx == 288 &&
                           view->buildings[0].level == 2,
                       "the recovered building lands on the ChunkActor");
            chk.expect(view->crops.size() == 1 && view->crops[0].tx == 289,
                       "the recovered crop lands on the ChunkActor");
            const auto idx0 = static_cast<std::size_t>(local_tile_index(289, 288));
            const auto idx1 = static_cast<std::size_t>(local_tile_index(290, 288));
            chk.expect(view->terrain[idx0] == static_cast<std::uint8_t>(Terrain::kDirt) &&
                           view->terrain[idx1] == static_cast<std::uint8_t>(Terrain::kDirt),
                       "both recovered tilled tiles overwrite the seed-derived terrain (§6.4's "
                       "post-generate_terrain ordering)");
        }
    }
    std::printf("RFC-016 persistence: apply_recovered_overlay() lands a recovered snapshot on a "
               "real ChunkActor\n\n");

    std::error_code p16_cleanup_ec;
    std::filesystem::remove_all(p16_scratch_root, p16_cleanup_ec);  // best-effort scratch cleanup

    // --- RFC-018: loot, Essence & reward tables — as pure functions, same convention RFC-002/003/
    // 005/006/007/010 already use for their own header-only logic (no actor/router needed to test
    // it, and by this point in the file every login slot is already spent — see the RFC-019 block's
    // own note on the fixed 8-slot ceiling). --------------------------------------------------------
    {
        using namespace mmo;

        // §3: the material-tier table — the numbers RFC-021 §3.6 handed off, verbatim.
        chk.expect(tier_damage_pm(MaterialTier::kCopper) == 1000 &&
                       tier_damage_pm(MaterialTier::kIron) == 1150 &&
                       tier_damage_pm(MaterialTier::kSteel) == 1350 &&
                       tier_damage_pm(MaterialTier::kMythril) == 1600,
                   "§3's tier_damage_pm ladder matches the RFC's table exactly");
        chk.expect(tier_below(MaterialTier::kMythril) == MaterialTier::kSteel &&
                       tier_below(MaterialTier::kCopper) == MaterialTier::kCopper,
                   "tier_below never drops a broken Copper item below its own baseline");
        chk.expect(ore_kind_of(MaterialTier::kIron) == ItemKind::kOreIron &&
                       ore_kind_of(MaterialTier::kMythril) == ItemKind::kOreMythril,
                   "ore_kind_of resolves the right ItemKind per tier");

        // §4.1: durability interpolation — full, empty, half, and the "no item" baseline.
        EquippedItem fresh_iron{};
        fresh_iron.item_id = 1;
        fresh_iron.tier = MaterialTier::kIron;
        fresh_iron.max_durability = tier_max_durability(MaterialTier::kIron);
        fresh_iron.durability = fresh_iron.max_durability;
        chk.expect(effective_tier_damage_pm(fresh_iron) == tier_damage_pm(MaterialTier::kIron),
                   "a fresh (full-durability) item plays at its own tier's numbers exactly");

        EquippedItem broken_iron = fresh_iron;
        broken_iron.durability = 0;
        chk.expect(effective_tier_damage_pm(broken_iron) == tier_damage_pm(MaterialTier::kCopper),
                   "a fully-worn item floors at the tier BELOW its own — never a brick, never below "
                   "Copper's own baseline (§4.1)");

        EquippedItem half_iron = fresh_iron;
        half_iron.durability = static_cast<std::int16_t>(fresh_iron.max_durability / 2);
        const std::uint16_t half_dmg = effective_tier_damage_pm(half_iron);
        chk.expect(half_dmg > tier_damage_pm(MaterialTier::kCopper) &&
                       half_dmg < tier_damage_pm(MaterialTier::kIron),
                   "a half-worn item interpolates strictly between the tier below and its own tier");

        EquippedItem bare{};  // item_id == 0: bare hand / bare skin
        chk.expect(effective_tier_damage_pm(bare) == 1000 && effective_tier_dr_bonus(bare) == 0 &&
                       effective_tier_toughness_bonus(bare) == 0,
                   "an empty slot (item_id == 0) is always the neutral ×1.0/+0/+0 baseline, "
                   "regardless of its default tier field");

        // §5: socket gems — the mechanical link to RFC-002's status ladder. Grade is deferred (see
        // tiles.hpp::ItemKind's own header note on the 192-byte message-pool ceiling); every gem
        // below is a single flat rider.
        const GemRider cold = gem_rider_of(ItemKind::kGemCold);
        chk.expect(cold.channel == Channel::kCold && cold.amount > 0 && !cold.coating,
                   "a Cold gem resolves to a real Channel::kCold build-up rider");
        const GemRider wet = gem_rider_of(ItemKind::kGemWet);
        chk.expect(wet.coating && wet.amount > 0 && wet.amount <= 255,
                   "a Wet gem resolves to a coating rider whose amount fits CoatingPacket::ticks's "
                   "uint8_t (would silently truncate/overflow otherwise)");
        const GemRider empty_socket = gem_rider_of(ItemKind::kWood);
        chk.expect(empty_socket.channel == Channel::kNone && !empty_socket.coating &&
                       empty_socket.amount == 0,
                   "kWood (the empty-socket sentinel) resolves to no rider at all");

        // §9: ring scaling, cited from RFC-021 §2.1/§3.6.
        chk.expect(ring_loot_scale(Ring::kMeadow).chance_mult_pm == 1000 &&
                       ring_loot_scale(Ring::kMeadow).ore_tier == MaterialTier::kCopper,
                   "Meadow (ring 0) scales loot at the neutral ×1.0 baseline, Copper ore");
        chk.expect(ring_loot_scale(Ring::kWasteland).chance_mult_pm == 1800 &&
                       ring_loot_scale(Ring::kWasteland).qty_bonus == 2 &&
                       ring_loot_scale(Ring::kWasteland).ore_tier == MaterialTier::kMythril,
                   "Wasteland (ring 4) scales loot to ×1.8/+2 qty/Mythril ore, matching §9's table");

        // §8: the deterministic roll seed — same inputs, same seed; a different recipient (the term
        // that makes a boss kill's per-contributor rolls genuinely independent, §10.2) changes it.
        const std::uint64_t seed_a = roll_seed_of(kWorldSeed, 4242, 100, 7);
        const std::uint64_t seed_a2 = roll_seed_of(kWorldSeed, 4242, 100, 7);
        const std::uint64_t seed_b = roll_seed_of(kWorldSeed, 4242, 100, 9);
        chk.expect(seed_a == seed_a2, "roll_seed_of is a pure function of its inputs — identical "
                                      "inputs reproduce the identical seed (§8's replay claim)");
        chk.expect(seed_a != seed_b, "a different contributor_account_id changes the seed, which is "
                                     "what makes two contributors' rolls on the same kill independent");

        // §7-§9: roll_loot determinism, and the realm gate's "no exceptions" rule (§6/§12) — Essence
        // and the boss equipment row must NEVER fire outside a challenge realm, across many seeds;
        // inside one, they eventually do (the guaranteed ore row always does, everywhere).
        const LootTable& slime = loot_table_of(CreatureKind::kSlime);
        const RewardBundle first = roll_loot(slime, 1234567, Ring::kWasteland, true);
        const RewardBundle second = roll_loot(slime, 1234567, Ring::kWasteland, true);
        chk.expect(first.items.size() == second.items.size() && first.essence == second.essence,
                   "roll_loot is deterministic — the same seed reproduces the same reward bundle");

        bool essence_seen_gated = false;
        bool essence_seen_open = false;
        bool equipment_seen_open = false;
        const LootTable& boss = loot_table_of(CreatureKind::kBoss);
        // 4000 trials at the equipment row's authored 8‰ chance leaves P(zero hits) = 0.992^4000 ≈
        // 4e-15 — 300 trials (≈9% false-failure rate) is what this test originally used and was
        // flaky enough to prove it.
        for (std::uint64_t s = 0; s < 4000; ++s) {
            const RewardBundle gated = roll_loot(slime, s * 0x9E3779B97F4A7C15ull, Ring::kWasteland,
                                                 /*realm_challenge*/ false);
            if (gated.essence > 0) essence_seen_gated = true;
            const RewardBundle open = roll_loot(slime, s * 0x9E3779B97F4A7C15ull, Ring::kWasteland,
                                                /*realm_challenge*/ true);
            if (open.essence > 0) essence_seen_open = true;
            const RewardBundle boss_open =
                roll_loot(boss, s * 0xBF58476D1CE4E5B9ull, Ring::kWasteland, /*realm_challenge*/ true);
            if (boss_open.equipment.has_value()) equipment_seen_open = true;
            const RewardBundle boss_gated = roll_loot(boss, s * 0xBF58476D1CE4E5B9ull, Ring::kWasteland,
                                                      /*realm_challenge*/ false);
            chk.expect(!boss_gated.equipment.has_value(),
                       "the boss equipment row never fires outside a challenge realm — no exceptions");
            chk.expect(!boss_gated.items.empty(),
                       "the boss's guaranteed ore row fires regardless of realm (§10.2)");
        }
        chk.expect(!essence_seen_gated,
                   "Essence never drops outside a challenge realm, across 4000 distinct seeds (§6 "
                   "rule 1 — no exceptions)");
        chk.expect(essence_seen_open,
                   "Essence DOES drop inside a challenge realm — the gate isn't just always-closed");
        chk.expect(equipment_seen_open,
                   "the boss's rare equipment row fires at least once across 4000 challenge-realm "
                   "rolls at its authored 8‰ chance");

        // §10.2: the occupied-slot rule, as the pure function PlayerActor::handle(GrantEquipment)
        // actually calls.
        EquippedItem copper_sword{};
        copper_sword.item_id = 1;
        copper_sword.tier = MaterialTier::kCopper;
        EquippedItem mythril_drop{};
        mythril_drop.tier = MaterialTier::kMythril;
        EquippedItem another_copper_drop{};
        another_copper_drop.tier = MaterialTier::kCopper;
        chk.expect(equipment_upgrades(copper_sword, mythril_drop),
                   "a strictly higher tier auto-equips (§10.2)");
        chk.expect(!equipment_upgrades(copper_sword, another_copper_drop),
                   "an equal tier does not replace the worn item — it converts to an ore refund");
    }
    std::printf("RFC-018 loot, Essence & reward tables: tier/durability math, socket-gem riders, "
               "ring scaling, deterministic per-contributor rolls, and the challenge-realm gate's "
               "no-exceptions rule (§3/§4.1/§5/§6/§8/§9/§10.2) all check out\n\n");

    // --- RFC-020: mission & quest system. Pure-function coverage first (quest.hpp needs no actor),
    // then a standalone `PlayerActor` exercising the real handlers (`AcceptQuest`/`GameplayFact`/
    // `AbandonQuest`) directly — the same "no router, no bus, call handle() straight" pattern the
    // RFC-016 block above already uses for a standalone `ChunkActor`, since every login slot is
    // already spent by this point in the file (see the RFC-018 block's own note). `bus == nullptr`
    // makes every `publish()` call inside these handlers a safe no-op. -------------------------------
    {
        using namespace mmo;

        // Pure functions: fact matching, progress clamping, completion, and the live unlock guard.
        {
            QuestInstance qi{};
            qi.id = QuestId::kFarmPlot;
            const GameplayFact wrong_kind{FactKind::kKill, 1,
                                          static_cast<std::uint16_t>(BuildKind::kPlot), 1, 0, 0};
            chk.expect(!quest_apply_fact(qi, wrong_kind),
                       "a fact of the wrong FactKind never matches a build objective");
            const GameplayFact wrong_target{FactKind::kBuild,
                                            1, static_cast<std::uint16_t>(BuildKind::kHearth), 1, 0, 0};
            chk.expect(!quest_apply_fact(qi, wrong_target),
                       "a fact whose subject_id does not match the objective's target is ignored");
            const GameplayFact right{FactKind::kBuild, 1, static_cast<std::uint16_t>(BuildKind::kPlot),
                                     1, 0, 0};
            chk.expect(quest_apply_fact(qi, right) && qi.count_current[0] == 1,
                       "a matching fact increments count_current");
            chk.expect(quest_instance_complete(qi), "count_current reaching count_required completes it");

            QuestInstance clear{};
            clear.id = QuestId::kClearSlimes;
            const GameplayFact kill{FactKind::kKill, 1, static_cast<std::uint16_t>(CreatureKind::kSlime),
                                    1, static_cast<std::uint8_t>(Skill::kRanged), 0};
            (void)quest_apply_fact(clear, kill);
            (void)quest_apply_fact(clear, kill);
            chk.expect(!quest_instance_complete(clear) && clear.count_current[0] == 2,
                       "a 3-count objective is not complete at 2, and progress is not clamped early");
            (void)quest_apply_fact(clear, kill);
            (void)quest_apply_fact(clear, kill);  // a 4th fact past count_required must not overshoot
            chk.expect(quest_instance_complete(clear) && clear.count_current[0] == 3,
                       "progress clamps at count_required rather than overshooting past it");
            chk.expect(clear.dominant_branch == static_cast<std::uint8_t>(Skill::kRanged),
                       "a qualifying kKill fact's cause_branch becomes the instance's dominant_branch, "
                       "feeding a by_cause reward");

            chk.expect(quest_offerable(QuestId::kFarmPlot, 0, false, false),
                       "a tier-0-unlock quest is offerable to a player who has visited no village");
            chk.expect(!quest_offerable(QuestId::kFarmPlot, 0, true, false),
                       "an already-active quest never re-offers (QI3)");
            chk.expect(!quest_offerable(QuestId::kFarmPlot, 0, false, true),
                       "a completed, non-repeatable quest never re-offers");
            chk.expect(quest_offerable(QuestId::kClearSlimes, 0, false, true),
                       "a completed, REPEATABLE quest re-offers immediately — no cooldown (QI1/QV10)");
        }

        // The real handlers, on a standalone actor.
        {
            PlayerActor pa;
            pa.handle(BindAccount{1, 100, 100, 0, 0, 0});
            chk.expect(pa.view().account == 1, "the standalone actor is bound and ready");

            pa.handle(AcceptQuest{QuestId::kFarmPlot});
            PlayerView v = pa.view();
            chk.expect(v.quest_active_id[0] == QuestId::kFarmPlot && v.quest_active_progress[0] == 0,
                       "Q1->Q2: AcceptQuest creates a QuestInstance visible in the next view");
            chk.expect(v.quest_offerable[static_cast<int>(QuestId::kFarmPlot)] == 0,
                       "an active quest drops off the offerable list (QI3)");

            pa.handle(AcceptQuest{QuestId::kFarmPlot});
            int active_farm_plots = 0;
            for (int i = 0; i < kMaxActiveQuests; ++i) {
                if (pa.view().quest_active_id[i] == QuestId::kFarmPlot) ++active_farm_plots;
            }
            chk.expect(active_farm_plots == 1,
                       "re-accepting an already-active quest is refused, not a second instance (QI3)");

            const std::int32_t seed_before = pa.view().items[static_cast<int>(ItemKind::kSeed)];
            const std::uint32_t craft_xp_before = pa.view().skill_xp[static_cast<int>(Skill::kCraft)];
            pa.handle(GameplayFact{FactKind::kBuild, 1, static_cast<std::uint16_t>(BuildKind::kPlot), 1,
                                   0, 5});
            v = pa.view();
            bool farm_plot_still_active = false;
            for (int i = 0; i < kMaxActiveQuests; ++i) {
                if (v.quest_active_id[i] == QuestId::kFarmPlot) farm_plot_still_active = true;
            }
            chk.expect(!farm_plot_still_active,
                       "Q4: the single kBuild fact completes the 1-count objective and the instance "
                       "is gone from the log — auto_complete pays instantly, no giver turn-in needed");
            chk.expect(v.items[static_cast<int>(ItemKind::kSeed)] == seed_before + 5,
                       "§7 kItemTable reward: 5 Seed granted on completion");
            chk.expect(v.skill_xp[static_cast<int>(Skill::kCraft)] > craft_xp_before,
                       "§7 kXp reward: fixed-branch (Craft) XP granted on completion");
            chk.expect(v.quest_completed[static_cast<int>(QuestId::kFarmPlot)] == 1,
                       "a non-repeatable completion sets the completed gate");

            pa.handle(AcceptQuest{QuestId::kFarmPlot});
            bool refused_after_completion = true;
            for (int i = 0; i < kMaxActiveQuests; ++i) {
                if (pa.view().quest_active_id[i] == QuestId::kFarmPlot) refused_after_completion = false;
            }
            chk.expect(refused_after_completion,
                       "a completed, non-repeatable quest refuses re-acceptance");

            // kClearSlimes: repeatable, by_cause XP, and village_standing's forward-compatible stand-in.
            pa.handle(AcceptQuest{QuestId::kClearSlimes});
            const std::uint32_t melee_xp_before = pa.view().skill_xp[static_cast<int>(Skill::kMelee)];
            for (int i = 0; i < 3; ++i) {
                pa.handle(GameplayFact{FactKind::kKill, 1,
                                       static_cast<std::uint16_t>(CreatureKind::kSlime), 1,
                                       static_cast<std::uint8_t>(Skill::kMelee),
                                       static_cast<std::uint32_t>(10 + i)});
            }
            v = pa.view();
            chk.expect(v.skill_xp[static_cast<int>(Skill::kMelee)] > melee_xp_before,
                       "§7 by_cause: the dominant_branch (Melee, from the qualifying kKill facts) "
                       "resolves the reward's branch, not a fixed one");
            chk.expect(v.quest_completed[static_cast<int>(QuestId::kClearSlimes)] == 1,
                       "kClearSlimes completes on the 3rd matching kill fact");

            // Repeatable: immediately re-offerable and re-acceptable.
            chk.expect(v.quest_offerable[static_cast<int>(QuestId::kClearSlimes)] == 1,
                       "a repeatable quest is offerable again the instant it completes — no cooldown");
            pa.handle(AcceptQuest{QuestId::kClearSlimes});
            bool clear_active_again = false;
            for (int i = 0; i < kMaxActiveQuests; ++i) {
                if (pa.view().quest_active_id[i] == QuestId::kClearSlimes) clear_active_again = true;
            }
            chk.expect(clear_active_again, "a repeatable quest can be accepted again after completion");

            // Q5: Abandon is free, silent, and immediate.
            pa.handle(AbandonQuest{QuestId::kClearSlimes});
            v = pa.view();
            bool clear_active_after_abandon = false;
            for (int i = 0; i < kMaxActiveQuests; ++i) {
                if (v.quest_active_id[i] == QuestId::kClearSlimes) clear_active_after_abandon = true;
            }
            chk.expect(!clear_active_after_abandon,
                       "Q5: AbandonQuest removes the instance with no penalty");
            chk.expect(v.quest_offerable[static_cast<int>(QuestId::kClearSlimes)] == 1,
                       "an abandoned repeatable quest is immediately offerable again — QI2's "
                       "'externally identical to a fresh offer' rule");
        }
    }
    std::printf("RFC-020 mission & quest system: the build-up state machine (Q1-Q5), fact matching "
               "and progress clamping, live kBuild/kKill GameplayFact wiring off the RFC-019 §5.8 "
               "kill-credit ledger and the Build-screen placement verb, QI1-QI4, and the xp/"
               "item_table/village_standing reward hooks all check out\n\n");

    // --- RFC-023: village NPCs — the civilian half (roster, FSM, Sheltering targetability) ---------
    {
        // Packing round-trip: npc.hpp's whole mechanism is bits stolen from Creature::target (the
        // 192-byte hard cap leaves no room for real new fields) — if this drifts, everything above it
        // silently reads garbage, so it is worth proving directly rather than only through the roster
        // check below.
        {
            Creature c{};
            chk.expect(!creature_is_npc(c), "a plain Creature{} is not an Npc by default");
            npc_init(c, NpcRole::kFarmer, 42);
            chk.expect(creature_is_npc(c), "npc_init sets the marker bit");
            chk.expect(npc_role_of(c) == NpcRole::kFarmer, "npc_init round-trips the role");
            chk.expect(npc_home_struct_of(c) == 42, "npc_init round-trips home_struct");
            chk.expect(npc_state_of(c) == NpcState::kIdle, "a freshly init'd Npc starts Idle");
            chk.expect(faction_of(c) == Faction::kVillager,
                       "an Npc's faction is kVillager regardless of its placeholder CreatureKind");
            npc_set_state(c, NpcState::kSheltering);
            chk.expect(npc_state_of(c) == NpcState::kSheltering, "npc_set_state updates state in place");
            chk.expect(npc_role_of(c) == NpcRole::kFarmer && npc_home_struct_of(c) == 42,
                       "changing state does not disturb role/home_struct packed alongside it");

            Creature wolf{};
            wolf.kind = CreatureKind::kWolf;
            chk.expect(faction_of(wolf) == Faction::kWild,
                       "a non-Npc Creature's faction still falls through to stats_of(kind)");
        }

        // §7's role table: stationary vs. wandering, work pose.
        {
            chk.expect(npc_wander_radius(NpcRole::kMerchantShop) == 0.0f &&
                          npc_wander_radius(NpcRole::kQuestGiver) == 0.0f,
                      "shop/giver are stationary — fixed at their door tile (§7)");
            chk.expect(npc_wander_radius(NpcRole::kFarmer) > 0.0f &&
                          npc_wander_radius(NpcRole::kChild) > 0.0f &&
                          npc_wander_radius(NpcRole::kWanderer) > 0.0f,
                      "farmer/child/wanderer roam a radius around home (§7)");
            chk.expect(npc_has_work_pose(NpcRole::kFarmer) && npc_has_work_pose(NpcRole::kChild),
                      "farmer/child dwell in a work pose at each waypoint (§7)");
            chk.expect(!npc_has_work_pose(NpcRole::kWanderer) && !npc_has_work_pose(NpcRole::kMerchantShop),
                      "wanderer/shop do not (§7)");
        }

        // §9: skin/tint is a pure function of (seed, home_struct), fixed once, never re-rolled — and
        // never the undyed index, so a civilian is never mistaken for the player at a glance.
        {
            const NpcTint a = npc_tint_of(kWorldSeed, 7);
            const NpcTint b = npc_tint_of(kWorldSeed, 7);
            chk.expect(a.r == b.r && a.g == b.g && a.b == b.b,
                      "npc_tint_of is a pure, deterministic function of (seed, home_struct) — §9");
            bool any_default = false;
            for (std::uint32_t hs = 0; hs < 64; ++hs) {
                const NpcTint t = npc_tint_of(kWorldSeed, hs);
                if (t.r == 255 && t.g == 255 && t.b == 255) any_default = true;
            }
            chk.expect(!any_default, "an NPC never draws the undyed default palette entry");
        }

        // §8.1: a live roster, off the shared World this whole file already built — confirms
        // build_npcs() actually ran and placed real Creatures, not just that the formula compiles.
        {
            const WorldLayout& layout = world.layout();
            const Village* v2 = nullptr;
            for (const Village& v : layout.villages()) {
                if (v.tier >= 2) {
                    v2 = &v;
                    break;
                }
            }
            chk.expect(v2 != nullptr, "the world has at least one tier>=2 village to check a roster against");
            if (v2 != nullptr) {
                bool found_shop = false;
                bool found_giver = false;
                bool found_any = false;
                for (std::uint32_t i = v2->first;
                     i < static_cast<std::uint32_t>(v2->first) + v2->count && i < layout.structures().size();
                     ++i) {
                    const Structure& s = layout.structures()[i];
                    if (!is_dwelling(s.kind)) continue;
                    const ChunkCoord cc = chunk_of(kOverworld, static_cast<float>(door_tx(s)),
                                                   static_cast<float>(door_ty(s)));
                    ChunkViewPtr view = world.bus().load(cc);
                    if (!view) continue;
                    for (const Creature& m : view->creatures) {
                        if (!creature_is_npc(m) || npc_home_struct_of(m) != i) continue;
                        found_any = true;
                        if (npc_role_of(m) == NpcRole::kMerchantShop) found_shop = true;
                        if (npc_role_of(m) == NpcRole::kQuestGiver) found_giver = true;
                        chk.expect(m.disposition == Disposition::kNeutral,
                                  "§3: an Npc is constructed kNeutral, never Creature's own kHostile "
                                  "default");
                        chk.expect(m.damage == 0, "§6: no civilian role ever deals damage");
                    }
                }
                chk.expect(found_any, "build_npcs() placed real NPCs into a tier>=2 village's chunks");
                chk.expect(found_shop, "a tier>=2 village gets its §8.1 kMerchantShop slot");
                chk.expect(found_giver, "a tier>=2 village gets its §8.1 kQuestGiver slot");
            }
        }

        // §7/§Multiplayer: proximity-triggered Sheltering (this pass's substitute for the unshipped
        // raid-warning event, npc.hpp's kNpcThreatRadius note) makes an NPC untargetable to a raid
        // monster's maul_wildlife contact — the actual "monster reaches the village and has nothing
        // but empty houses to walk into" gap this RFC was implemented to close. A standalone
        // ChunkActor, the same pattern RFC-016's persistence test above uses, driven only through its
        // public message handlers (CreatureEnter, Tick) — no router, no engine.
        {
            SnapshotBus local_bus;
            ChunkActor ch;
            ch.coord = ChunkCoord{kOverworld, 40, 40};
            ch.bus = &local_bus;
            ch.generate_terrain(kWorldSeed);

            const int hx = 40 * kChunkTiles + 10;
            const int hy = 40 * kChunkTiles + 10;
            ch.add_npc(NpcRole::kFarmer, /*home_struct*/ 1, hx, hy);

            // Spawned 3 tiles off — inside kNpcThreatRadius (5.0) but outside a slime's own reach
            // (1.0), so the very first tick can detect the threat before any contact is possible,
            // with no same-tick ordering race to account for.
            Creature raider{};
            raider.id = 555001;
            raider.kind = CreatureKind::kSlime;
            raider.hp = 30;
            raider.max_hp = 30;
            raider.damage = 4;
            raider.disposition = Disposition::kHostile;
            raider.x = static_cast<float>(hx) + 3.0f;
            raider.y = static_cast<float>(hy) + 0.5f;
            ch.handle(CreatureEnter{raider});

            for (std::uint64_t t = 1; t <= 8; ++t) {
                ch.handle(Tick{t, static_cast<std::int64_t>(t) * kTickMs, false});
            }
            // handle(Tick) only publishes every kIdlePublish (32) ticks with no player watching —
            // force one so the view below reflects what just happened rather than nothing at all.
            ch.publish_now();

            ChunkViewPtr view = local_bus.load(ch.coord);
            chk.expect(view != nullptr, "the standalone chunk publishes a view after ticking");
            if (view) {
                const Creature* npc = nullptr;
                for (const Creature& m : view->creatures) {
                    if (creature_is_npc(m)) npc = &m;
                }
                chk.expect(npc != nullptr, "the NPC is still alive — it was never reaped");
                if (npc != nullptr) {
                    chk.expect(npc_state_of(*npc) == NpcState::kSheltering,
                              "a hostile monster within kNpcThreatRadius trips Sheltering");
                    chk.expect(npc->hp == kNpcMaxHp,
                              "Sheltering makes the NPC untargetable — maul_wildlife's own "
                              "hostile-contact path (GAME.md §5) never reduced its hp");
                }
            }
        }
    }
    std::printf("RFC-023 character & NPC roster system: the civilian half — role/state packed into "
               "Creature::target with no struct growth (the 192-byte message-pool cap), the §7 "
               "four-state FSM, the §8.1 tier-gated roster formula off a village's actual built "
               "houses, and proximity-triggered Sheltering making a civilian untargetable to a raid "
               "monster's maul_wildlife contact all check out. Guard rosters (§8.2) and the "
               "road-graph-anchored kMerchantWander role are not built — see npc.hpp's header note: "
               "this codebase's RFC-007 covers only the Dojo Master boss, not the guard archetype "
               "roster §6 assumes exists\n\n");

    std::printf("\n%s\n", chk.failures == 0 ? "OK" : "FAIL");
    return chk.failures == 0 ? 0 : 1;
}
