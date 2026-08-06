// ChunkActor — one actor owns one 32x32-tile chunk and is the SINGLE WRITER of everything in it.
//
// This is where the engine choice pays for itself. A chunk is a natural actor: it has state
// (terrain, creatures, projectiles, crops, buildings), that state is only ever mutated by messages,
// and the messages that mutate it are inherently serialised per chunk. `Sequential` therefore costs
// nothing and buys the entire absence of locking in this file — there is not one mutex or atomic in
// the simulation.
//
// TRUST TIER B. Placement is unconstrained (`Placement<HashById>`): a chunk may be hosted on a
// player's machine. That is safe *by selection*, not by hope — everything a chunk decides is either
// replayable from (chunk key, tick) via the deterministic RNG, or low-value (a slime's position).
// The things a player would want to forge — inventory, affordability, how hard their own sword hits
// — live in PlayerActor, which carries `Require<Trusted>` and is unreachable from here except
// through an `ask`. Note in particular that `MeleeSwing` arrives with its damage already decided.
//
// MIGRATION is the load-bearing demo. A creature that walks off the east edge of a chunk is removed
// from this actor and `tell`-ed to the neighbour as a `CreatureEnter`. Today that is an in-process
// enqueue; once chunks are placed across nodes it becomes a serialized frame over TCP, and **not one
// line of this file changes** — the router resolves a remote ActorRef exactly like a local one. The
// handoff needs no ack, no two-phase commit and no lock, because the sender removes before it sends
// and per-(sender,receiver) FIFO means the creature cannot be observed twice or out of order.
// Projectiles use the identical hand-off, deliberately: one mechanism, not two.
#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/placement_policies.hpp"

#include "world/battlefield.hpp"
#include "world/boss.hpp"
#include "world/boss_kit.hpp"
#include "world/combat_entity.hpp"
#include "world/flow_field.hpp"
#include "world/npc.hpp"
#include "world/persistence.hpp"
#include "world/physics.hpp"
#include "world/player_actor.hpp"
#include "world/protocol.hpp"
#include "world/rl_action.hpp"
#include "world/rl_obs.hpp"
#include "world/telegraph.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"

namespace mmo {

// The scripted BOSS's per-room state (F3). This is the "parallel slot" half of the boss: the
// damageable BODY is a Creature (kBoss) living in `creatures_` so every player verb hits it
// unchanged, and everything a Creature has no room for — its spawn/leash point, the room rectangle it
// is clamped to, the charge cooldown and committed-dash state, the respawn timer that must outlive
// the body — lives here, keyed to the body by id. One BossState per dojo room a chunk owns.
struct BossState {
    std::uint32_t room = 0;       // interior room index this boss guards
    std::uint16_t spawn_tx = 0;   // where it spawns and leashes back to (room top-centre)
    std::uint16_t spawn_ty = 0;
    float cx0 = 0.0f, cy0 = 0.0f;  // the room floor rectangle its FEET are clamped inside, in tiles
    float cx1 = 0.0f, cy1 = 0.0f;
    std::uint32_t body = 0;       // creature id of the body in creatures_, 0 while dead
    bool alive = false;
    std::uint16_t respawn_timer = 0;  // counts down while dead; at 0 the boss respawns
    std::uint16_t no_target = 0;      // ticks with no player in the room — the leash counter
    std::uint8_t charge_cd = 0;       // ticks until it may charge again
    std::uint8_t charging = 0;        // ticks of the committed dash left, 0 = not dashing
    bool winding_charge = false;      // the current wind-up will begin a charge, not land a blow
    bool dash_hit = false;            // the current dash has already connected (one hit per dash)
    float charge_dx = 0.0f, charge_dy = 0.0f;  // committed dash direction (unit)
    std::uint32_t telegraph_id = 0;   // RFC-006: the live Telegraph this boss committed, 0 = none
};

// RFC-002/009: the uniform Power anchor RFC-009 §4.5 calibrates for Ice (`kIceBoltPower = 600`)
// applied to all four elements alike — P2's own status-duration table was already symmetric across
// schools, so this preserves that symmetry rather than inventing per-element splits.
inline constexpr std::uint16_t kSpellPower = 600;

// RFC-003 §2.1/§10: authored impulse for the two hit shapes that already carry an explicit
// "this is a heavy/authored blow" flag today (physics.hpp's header note: no multi-channel payload
// exists yet to author this per skill in data, so it is a compile-time constant here, mirroring
// `kSpellPower`'s own posture). 220 is the RFC's own §10 worked-example value.
//
// Namespace scope (not a ChunkActor member) so RFC-017's `--sweep` mode (sim_main.cpp) can read the
// exact same production constant rather than re-declaring the number — a duplicated literal here
// would be exactly the kind of second source of truth RFC-017 §1 forbids the sweep from creating.
inline constexpr std::uint16_t kHeavyMeleeImpulse = 220;
inline constexpr std::uint16_t kCrushBlowImpulse = 260;

// Which build-up channel a cast element feeds. Namespace scope alongside the constants above, for
// the same RFC-017 reason: `--sweep` needs the exact production mapping, not a re-typed switch.
[[nodiscard]] inline constexpr Channel channel_of(Element e) noexcept {
    switch (e) {
        case Element::kFire: return Channel::kHeat;
        case Element::kIce: return Channel::kCold;
        case Element::kEarth: return Channel::kEarth;
        case Element::kShock: return Channel::kShock;
        case Element::kNone:
        case Element::kCount: break;
    }
    return Channel::kNone;
}

// RFC-014 §3.2: the shared, world-lifetime resources (one router, one bus, one status per `World`)
// every `ChunkActor` needs, whether eagerly registered (`World::build_chunks()`, which still assigns
// `router`/`bus`/`status` explicitly afterward — redundant with the default member initializers
// below, not incorrect) or lazily broker-constructed (`declare_lazy` + `PrimeInstanceChunk`, which
// has no per-instance way to pass them — `protocol.hpp`'s own POD-only rule for messages rules out
// carrying raw pointers on `PrimeInstanceChunk` itself). Set exactly once, cold, before the engine
// starts — the identical "process-wide pointer, written once, read everywhere thereafter" shape
// `tiles.hpp`'s own `detail::g_overlay` already uses and justifies at length, applied to three
// pointers instead of one array. The full 004-Resources `Cached<T>`/`ResourceScope` system
// (`QuarkCpp/include/quark/core/resource.hpp`) is the "real" engine-sanctioned mechanism for this,
// but converting `ChunkActor`'s three raw-pointer fields into typed resource members is a broader
// refactor of already-shipped, heavily-exercised code this pass does not take on.
namespace detail {
inline quark::LocalRouter* g_shared_router = nullptr;
inline SnapshotBus* g_shared_bus = nullptr;
inline WorldStatus* g_shared_status = nullptr;
}  // namespace detail

// RFC-014 §3.5: a type-level policy. Adding it here has ZERO effect on the persistent band —
// `World::build_chunks()` still calls `quark::register_actor<ChunkActor>()` (spawn.hpp), which never
// forwards `idle_timeout_ms_of<A>()` to `register_activation`'s `idle_ticks` parameter (verified
// against the real engine source, not assumed) — so every eagerly-registered chunk stays hardcoded
// to `idle_ticks=0`, immune to eviction, exactly as today. Only a `declare_lazy`'d, broker-
// constructed instance (the instanced band, `InstanceManager::allocate_new`) reads this policy at
// all, because only `Engine::handle_wake()` resolves `idle_timeout_ms_of<A>()` from the type's
// compiled metadata. One `ChunkActor` type, two registration entry points, one asymmetric outcome —
// not a per-band split of the type itself.
struct ChunkActor : quark::Actor<ChunkActor, quark::Sequential, quark::Priority<1>,
                                 quark::DrainBudget<64>, quark::Placement<quark::HashById>,
                                 quark::IdleTimeout<kInstanceChunkIdleTimeoutMs>> {
    using protocol =
        Protocol<Tick, CreatureEnter, CreatureContribEnter, ProjectileEnter, SpawnWave, PlayerBeacon,
                 MeleeSwing, CastSpell, LaunchArrow, AbilityStrike, SpawnEntity, PlantCrop,
                 PlaceBuilding, UpgradeBuilding, TillGround, HarvestAt, PrimeInstanceChunk,
                 Ask<GetChunkStats, ChunkStats>>;

    // --- Wired once at bring-up, before the engine starts -----------------------------------------
    ChunkCoord coord{};
    quark::LocalRouter* router = detail::g_shared_router;
    SnapshotBus* bus = detail::g_shared_bus;
    WorldStatus* status = detail::g_shared_status;
    const FlowField* flow = nullptr;  // read-only, never written after bring-up (see flow_field.hpp)
    // Fallback target when a creature is somewhere the flow field does not cover (an unreachable
    // pocket, an island): the settlement nearest to this chunk, resolved once at bring-up.
    float home_x = 0.0f;
    float home_y = 0.0f;
    // RFC-018 §6.6: kRest for every persistent-band chunk (the default); set once, at
    // handle(PrimeInstanceChunk), for an instanced chunk allocated behind a kRealmGate portal.
    RealmType realm_type_ = RealmType::kRest;

    // How often a creature may strike, in ticks. One second is slow enough to read on screen and to
    // step out of, which is the whole difficulty budget at this scale — the numbers that scale with
    // the ring are HP and damage, never cadence.
    static constexpr std::uint8_t kStrikeCooldown = 10;

    // ================================ handlers ====================================================

    // SIMULATION LOD. At 1024 chunk actors, most hold nothing a viewer could see at any moment.
    // Ticking them is not expensive (an empty `Sequential` handler over a few empty vectors is a few
    // hundred nanoseconds), but PUBLISHING them is: every publish allocates a ChunkView and copies a
    // 1 KB terrain array, and doing that 1024 times per tick is ~10 MB/s of pure garbage for frames
    // nobody is looking at.
    //
    // The rule used to be "an idle chunk publishes rarely", where idle meant empty. Wildlife broke
    // that — with animals seeded across the whole map almost no chunk is empty any more, and the LOD
    // would have quietly stopped saving anything. So the test is now the RIGHT one and always was:
    // publish at full rate only when a player is near enough to see it. `players_` is exactly that
    // predicate, for free, because the beacon lease already tracks it.
    //
    // This is also the P6 interest set, arriving early and from the other direction: the same roster
    // that says "publish this chunk" will say "stream this chunk to that client".
    static constexpr std::uint64_t kIdlePublish = 32;

    void handle(const Tick& t) noexcept {
        tick_ = t.tick;
        world_ms_ = t.world_ms;
        expire_beacons();

        const bool empty = creatures_.empty() && crops_.empty() && buildings_.empty() &&
                           shots_.empty() && effects_.empty() && entities_.empty() && scars_.empty() &&
                           patches_.empty() && fields_.empty() && telegraphs_.empty();
        // A chunk that owns a boss never takes the idle fast path: even while its boss is DEAD (its
        // body gone from creatures_) the respawn timer has to tick down, and that is step_bosses'.
        if (empty && bosses_.empty()) {
            if (tick_ % kIdlePublish == 0) publish();
            return;
        }

        Rng rng(chunk_key(coord) * 0x9E37'79B9'7F4A'7C15ull + t.tick);
        grow_crops();
        step_status();
        step_entities();  // before creatures: an entity's aura/aggro-suppress changes what they do
        step_scars();
        step_fields();       // RFC-010 §4.4: before creatures/projectiles — a field changes what they do THIS tick
        step_creatures(rng);
        step_projectiles();
        step_effects();
        step_patches();      // RFC-010 §4.4: decay chains + fire spread, after effects, before reap
        step_telegraphs();   // RFC-006 §1.4: age out FIZZLE records; windup-tied ones tick in step_creatures
        reap_dead();
        step_bosses();  // after reap: a boss killed this tick is already gone, so its slot respawns

        // Simulated always, published only when someone could be looking.
        if (!players_.empty() || tick_ % kIdlePublish == 0) publish();
    }

    // A creature arrived from a neighbouring chunk. Adopt it verbatim — the sender already owns the
    // decision that this chunk is the new owner.
    //
    // The republish matters. Views are published once per tick, so without it a migrating creature
    // is in NEITHER published view for up to a full tick: the sender already dropped it, and this
    // chunk will not publish until its next tick. At 10 Hz that is a 100 ms hole — visible as a
    // blink when a creature crosses a boundary, and it made snapshot-based counts read low
    // (measured: 70 alive by `ask`, 28 visible in views). Republishing on arrival closes the hole.
    void handle(const CreatureEnter& e) noexcept {
        creatures_.push_back(e.creature);
        if (!players_.empty()) publish();
    }

    // RFC-019 §5.8: the companion to `CreatureEnter` — see `CreatureContribEnter`'s own comment.
    // Per-(sender,receiver) FIFO guarantees this arrives immediately after the `CreatureEnter` it
    // was sent alongside, so the creature is always already present in `creatures_` by the time
    // this lands.
    void handle(const CreatureContribEnter& e) noexcept {
        std::array<Contribution, kMaxContributors> entries{};
        for (std::size_t i = 0; i < kMaxContributors; ++i) entries[i] = e.entries[i];
        ledgers_[e.creature_id] = entries;
    }

    void handle(const ProjectileEnter& e) noexcept { shots_.push_back(e.shot); }

    // RFC-014 §3.2's "Option 2": the per-instance imperative setup a `declare_lazy`'d, broker-
    // constructed `ChunkActor` needs but its type-level `wire()` hook has no natural way to receive
    // (which coordinate this is, which map's seed to use). Mirrors `World::build_chunks()`'s own
    // field-assignment exactly — `coord`/`router`/`bus`/`status`/terrain — with two deliberate
    // omissions for an instanced chunk: no flow-field pointer (no per-instance flow field is built;
    // creatures in an instance do not path toward a persistent-band village) and no wildlife seeding
    // (an instance starts empty; population is RFC-023's job, not this RFC's). `router`/`bus`/
    // `status` are not part of the message — they are process-wide, wired once by `InstanceManager`
    // onto itself and passed through the SAME pointers every chunk already shares, never re-sent per
    // chunk.
    void handle(const PrimeInstanceChunk& p) noexcept {
        coord = p.coord;
        flow = nullptr;
        home_x = 0.0f;
        home_y = 0.0f;
        realm_type_ = p.descriptor.origin_realm_type;  // RFC-018 §6.6: this chunk's own realm gate
        generate_terrain(p.seed);
        publish();
    }

    // RFC-018 §6/§6.6: Essence, socket gems, and a boss's rare equipment row all read this — a
    // persistent-band chunk (the overworld, an interior room, `World::build_chunks()`'s eager path,
    // which never sends `PrimeInstanceChunk` and so never touches `realm_type_`) stays at its
    // `RealmType::kRest` default, so none of those three ever fire outside an actual instanced
    // `kRealmGate` destination — no exceptions, matching GAME.md §1's own framing.
    [[nodiscard]] bool realm_allows_essence() const noexcept { return realm_type_ == RealmType::kChallenge; }

    // Soft state with a lease. An upsert, never a delete — see PlayerBeacon in protocol.hpp for why
    // the absence of a "player left" message is the point rather than an omission.
    void handle(const PlayerBeacon& b) noexcept {
        for (PlayerBeacon& p : players_) {
            if (p.player != b.player) continue;
            p = b;
            return;
        }
        players_.push_back(b);
    }

    void handle(const SpawnWave& w) noexcept {
        Rng rng(chunk_key(coord) ^ (static_cast<std::uint64_t>(w.seed) << 17));
        const auto kind = static_cast<CreatureKind>(w.kind % kCreatureKinds);
        for (std::uint16_t i = 0; i < w.count; ++i) {
            // Spawn anywhere walkable near the stronghold that this chunk owns.
            //
            // RETRY rather than skip: a single attempt makes wave size depend on how much of this
            // particular chunk happens to be lake, so a watery rim quietly produced a fraction of
            // the intended wave. Difficulty should come from the director, not from the terrain.
            const int diameter = 2 * w.radius + 1;
            int tx = 0;
            int ty = 0;
            bool placed = false;
            for (int attempt = 0; attempt < 12 && !placed; ++attempt) {
                tx = w.tx - w.radius + static_cast<int>(rng.below(static_cast<std::uint32_t>(diameter)));
                ty = w.ty - w.radius + static_cast<int>(rng.below(static_cast<std::uint32_t>(diameter)));
                placed = owns(static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty)) &&
                         in_map(static_cast<float>(tx), static_cast<float>(ty)) &&
                         is_walkable(terrain_at(tx, ty));
            }
            if (!placed) continue;  // a chunk that really is all water contributes nothing
            creatures_.push_back(make_creature(kind, tx, ty, /*wanders*/ false));
        }
    }

    // --- the player's three verbs -----------------------------------------------------------------
    // All three are sent to the 3x3 chunks around the player and filtered here by ownership, so a
    // swing at a chunk border still connects. Each chunk only ever touches creatures it owns, so
    // nothing can be hit twice.

    void handle(const MeleeSwing& s) noexcept {
        const float fx = facing_dx(s.facing);
        const float fy = facing_dy(s.facing);
        // The arc is drawn whether or not it connects — a swing that misses is information, and a
        // swing that only appears when it hits teaches the player nothing about their reach.
        if (owns_point(s.x, s.y)) {
            add_effect(s.x + fx * s.reach * 0.55f, s.y + fy * s.reach * 0.55f, EffectKind::kSlash);
        }
        for (std::size_t i = 0; i < creatures_.size(); ++i) {
            Creature& c = creatures_[i];
            const float dx = c.x - s.x;
            const float dy = c.y - s.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > s.reach * s.reach) continue;
            // A 180-degree arc in front of the player. Anything behind you is not hit, which is
            // what makes facing worth caring about and turning worth doing.
            if (d2 > 0.01f && (dx * fx + dy * fy) < 0.0f) continue;
            // RFC-009 §4.8's normative strike-path order: detonate against the stage the target
            // enters this hit WITH, then strike, then fold in this hit's OWN build-up (including
            // the derived Stagger contribution below) — promotion from that gain is deferred to
            // the next status_step() tick, never judged against the very hit that produced it.
            const Combo combo = status_detonate(c.status, c.gauges, s.heavy, /*by_projectile*/ false,
                                                /*by_shock_element*/ false, tick_);
            apply_combo_side_effects(combo, c, s.player);
            // RFC-003 §2.1: only a heavy swing carries authored impulse — a light tap never shoves.
            if (s.heavy) {
                strike(c, s.damage, combo, s.player, Skill::kMelee, fx, fy, kHeavyMeleeImpulse);
            } else {
                strike(c, s.damage, combo, s.player, Skill::kMelee);
            }
            if (c.hp > 0) {
                status_gain(c.status, c.gauges,
                           BuildupPacket{Channel::kStagger, derived_stagger_power(s.damage, s.heavy, false),
                                         0, s.player},
                           mult_pm_of(c.material, c.tier, Channel::kStagger), tick_);
                // RFC-018 §5: a socketed weapon's gem rider(s), already resolved trusted-side
                // (PlayerActor) and echoed on this message — applied to every creature the swing
                // actually connects with, exactly like the derived-Stagger rider just above.
                for (const GemRider& g : s.gems) {
                    if (g.coating) {
                        if (g.amount > 0) status_coat(c.status, CoatingPacket{Coating::kWet, static_cast<std::uint8_t>(g.amount)});
                    } else if (g.channel != Channel::kNone) {
                        status_gain(c.status, c.gauges, BuildupPacket{g.channel, g.amount, 0, s.player},
                                   mult_pm_of(c.material, c.tier, g.channel), tick_);
                    }
                }
            }
        }
        // RFC-004 §7: the same arc also hits Active, destroyable entities in reach — friendly fire
        // is always on for entities (a wall does not dodge, and you must be able to break your own).
        strike_entities_in_shape(s.x, s.y, s.reach, fx, fy, /*front_only*/ true, s.damage,
                                 Element::kNone, s.heavy);
    }

    void handle(const CastSpell& s) noexcept {
        const Channel ch = channel_of(s.element);
        if (owns_point(s.x, s.y)) {
            add_effect(s.x, s.y, effect_of(s.element));
            // RFC-010 §4.2's trigger table: Fire/Ice impacts write a tile patch at the spell's own
            // target tile (once per cast, not once per creature it happens to hit); Rock (kEarth)
            // impacts stamp/escalate a scar directly instead — the table's own "no patch" ruling.
            const auto tx = static_cast<int>(s.x);
            const auto ty = static_cast<int>(s.y);
            if (s.element == Element::kFire || s.element == Element::kIce) {
                apply_surface_impact(tx, ty, s.element);
            } else if (s.element == Element::kEarth) {
                stamp_scar(tx, ty, ScarKind::kCracked);
            }
        }
        for (Creature& c : creatures_) {
            const float dx = c.x - s.x;
            const float dy = c.y - s.y;
            if (dx * dx + dy * dy > s.radius * s.radius) continue;
            // RFC-003 §8: standing on a tile with effective Conductivity >= 50 (open water, deep
            // marsh) counts as Wet for Conduct's coating test — a one-tick synthetic coat is enough
            // to satisfy `status_detonate`'s check below, and it decays away on its own if unused.
            if (terrain_phys(terrain_at(static_cast<int>(c.x), static_cast<int>(c.y))).conductivity >=
                50) {
                status_coat(c.status, CoatingPacket{Coating::kWet, 1});
            }
            // A spell detonates whatever ladder currently holds the primary slot (e.g. Fire vs an
            // active Freeze breaks it early via X1, folded inside status_gain below) before it
            // feeds its own channel's build-up — RFC-002 never lets a spell read a banked, non-
            // primary gauge, which is what keeps chaining a decision.
            const Combo combo =
                status_detonate(c.status, c.gauges, false, false, s.element == Element::kShock, tick_);
            apply_combo_side_effects(combo, c, s.player);
            strike(c, s.damage, combo, s.player, Skill::kMagic);
            if (c.hp > 0 && ch != Channel::kNone) {
                status_gain(c.status, c.gauges, BuildupPacket{ch, kSpellPower, 0, s.player},
                           mult_pm_of(c.material, c.tier, ch), tick_);
                c.dot_owner = s.player;
            }
            if (combo == Combo::kConduct) chain_shock(c, s.player);
        }
    }

    void handle(const LaunchArrow& a) noexcept {
        if (!owns_point(a.x, a.y)) return;  // exactly one chunk creates the arrow
        Projectile p{};
        p.id = ++next_id_ | (static_cast<std::uint32_t>(chunk_key(coord)) << 12);
        p.x = a.x;
        p.y = a.y;
        p.vx = a.vx;
        p.vy = a.vy;
        p.damage = a.damage;
        p.life = kArrowLife;
        p.owner = a.player;
        shots_.push_back(p);
    }

    // --- the ability verbs ------------------------------------------------------------------------
    // A resolved striking ability. Same tier-B contract as MeleeSwing/CastSpell: the damage and the
    // shape arrive decided, and this chunk only touches creatures it owns, so a border cannot double
    // a hit. Three abilities share this: WhirlCleave (a ring around the caster), CrushBlow (the one
    // nearest creature ahead, stunned), ElementalNova (a ring that also leaves an element's status).
    void handle(const AbilityStrike& s) noexcept {
        const float fx = facing_dx(s.facing);
        const float fy = facing_dy(s.facing);

        if (s.shape == AbilityShape::kFront) {
            // CrushBlow: pick the single nearest creature this chunk owns that is within reach and in
            // front of the caster, and land the whole blow on it. Only the owning chunk strikes, so
            // a creature straddling a border is hit exactly once.
            std::size_t best = creatures_.size();
            float best_d2 = s.radius * s.radius;
            for (std::size_t i = 0; i < creatures_.size(); ++i) {
                const Creature& c = creatures_[i];
                if (c.hp <= 0) continue;
                const float dx = c.x - s.x;
                const float dy = c.y - s.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 > best_d2) continue;
                if (d2 > 0.01f && (dx * fx + dy * fy) < 0.0f) continue;  // behind the caster
                best_d2 = d2;
                best = i;
            }
            if (best < creatures_.size()) {
                Creature& c = creatures_[best];
                add_effect(c.x, c.y, s.fx);
                // RFC-003 §2.1: CrushBlow is authored as a heavy, weighted blow — it carries impulse.
                strike(c, s.damage, Combo::kNone, s.player, s.skill, fx, fy, kCrushBlowImpulse);
                if (c.hp > 0) {
                    // RFC-009 §4.5: CrushBlow's old flat `stun_ticks=20` migrates to a large
                    // authored Stagger rider, on top of the derived contribution any heavy blow
                    // already carries — together they reach Knockdown in one or two hits, same as
                    // the old stun did.
                    const std::uint32_t total =
                        static_cast<std::uint32_t>(derived_stagger_power(s.damage, /*heavy*/ true, false)) +
                        s.stagger_power;
                    status_gain(c.status, c.gauges,
                               BuildupPacket{Channel::kStagger,
                                             static_cast<std::uint16_t>(std::min<std::uint32_t>(1000, total)),
                                             0, s.player},
                               mult_pm_of(c.material, c.tier, Channel::kStagger), tick_);
                }
            }
            // RFC-004 §7: entities join the same hit loop as creatures — no single-nearest
            // precision is needed here, since entity damage carries no combo/stun bookkeeping.
            strike_entities_in_shape(s.x, s.y, s.radius, fx, fy, /*front_only*/ true, s.damage,
                                     s.element, /*heavy*/ false);
            return;
        }

        // A ring around the caster (WhirlCleave, Nova). The flash goes at the caster whether or not
        // it connects — a whiff is information, exactly as a plain swing's arc is.
        if (owns_point(s.x, s.y)) {
            add_effect(s.x, s.y, s.fx);
            // RFC-010 §4.2: same per-cast (not per-creature) surface/scar impact CastSpell applies —
            // Nova is the only ring ability that ever carries a real element (WhirlCleave's is kNone).
            const auto tx = static_cast<int>(s.x);
            const auto ty = static_cast<int>(s.y);
            if (s.element == Element::kFire || s.element == Element::kIce) {
                apply_surface_impact(tx, ty, s.element);
            } else if (s.element == Element::kEarth) {
                stamp_scar(tx, ty, ScarKind::kCracked);
            }
        }
        const Channel ch = channel_of(s.element);
        for (Creature& c : creatures_) {
            if (c.hp <= 0) continue;
            const float dx = c.x - s.x;
            const float dy = c.y - s.y;
            if (dx * dx + dy * dy > s.radius * s.radius) continue;
            if (s.element != Element::kNone) {
                // RFC-003 §8: same standing-in-water extension as CastSpell.
                if (terrain_phys(terrain_at(static_cast<int>(c.x), static_cast<int>(c.y))).conductivity >=
                    50) {
                    status_coat(c.status, CoatingPacket{Coating::kWet, 1});
                }
                // Nova is a big CastSpell: it detonates a wet target for Conduct and feeds its own
                // channel's build-up on the survivors — the same interaction the cast path has.
                const Combo combo = status_detonate(c.status, c.gauges, false, false,
                                                    s.element == Element::kShock, tick_);
                apply_combo_side_effects(combo, c, s.player);
                strike(c, s.damage, combo, s.player, s.skill);
                if (c.hp > 0 && ch != Channel::kNone) {
                    status_gain(c.status, c.gauges, BuildupPacket{ch, kSpellPower, 0, s.player},
                               mult_pm_of(c.material, c.tier, ch), tick_);
                    c.dot_owner = s.player;
                }
                if (combo == Combo::kConduct) chain_shock(c, s.player);
            } else {
                // WhirlCleave: a light, non-heavy blow — still derives a small Stagger contribution
                // like any physical hit (RFC-009 §4.5), just no combo detection (matches CrushBlow's
                // own kFront branch, which never attempted one either).
                strike(c, s.damage, Combo::kNone, s.player, s.skill);
                if (c.hp > 0) {
                    status_gain(c.status, c.gauges,
                               BuildupPacket{Channel::kStagger, derived_stagger_power(s.damage, false, false),
                                             0, s.player},
                               mult_pm_of(c.material, c.tier, Channel::kStagger), tick_);
                }
            }
        }
        strike_entities_in_shape(s.x, s.y, s.radius, 0.0f, 0.0f, /*front_only*/ false, s.damage,
                                 s.element, /*heavy*/ false);
    }

    // Spawn a CombatEntity (RFC-004). Blocking kinds are a single tile-snapped footprint, so only the
    // chunk that OWNS that exact tile may accept one (RFC-004 §4 — no fan-out for a wall segment).
    // Non-blocking kinds (auras, smoke) keep the F2 seam fix a Zone had: fanned to the 3x3
    // neighbourhood, each chunk keeping its own copy clipped to what its area overlaps.
    void handle(const SpawnEntity& e) noexcept {
        const EntityDef def = entity_def(e.kind);
        const bool blocking = def.collision != Collision::kNone;
        float sx = e.x;
        float sy = e.y;
        if (blocking) {
            const auto tx = static_cast<std::uint16_t>(e.x);
            const auto ty = static_cast<std::uint16_t>(e.y);
            if (!owns(tx, ty)) return;
            // Spawn-validity (§4): walkable terrain, and no existing blocker already on the tile.
            if (!is_walkable(terrain_at(tx, ty)) ||
                block_bits_.test(static_cast<std::size_t>(local_tile_index(tx, ty)))) {
                return;
            }
            sx = static_cast<float>(tx) + 0.5f;
            sy = static_cast<float>(ty) + 0.5f;
        } else {
            const float r = e.radius_override > 0.0f ? e.radius_override : def.default_radius;
            if (!entity_intersects_chunk(e.x, e.y, r)) return;
        }
        // Always-hot restriction (§2/§5): kThunderTotem/kFallingRock may only be authored into
        // boss-room/dojo content. No RFC-005 authoring concept exists yet, so this backstops on the
        // cheapest real proxy available — a chunk that owns a boss — exactly like the RFC's own
        // "verb-side check is the backstop" framing. No ability or boss content spawns either kind
        // today; this path is exercised only by synthetic tests.
        if ((e.kind == EntityKind::kThunderTotem || e.kind == EntityKind::kFallingRock) &&
            bosses_.empty()) {
            return;
        }
        if (entities_.size() >= kMaxEntities) {
            if (!e.boss_room) return;  // refusal, not eviction, is the default
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < entities_.size(); ++i) {
                if (entities_[i].state_tick < entities_[oldest].state_tick) oldest = i;
            }
            entities_.erase(entities_.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        entities_.push_back(make_entity(e.kind, sx, sy, e.team, e.owner, e.radius_override));
        // The throw is a one-shot puff; the lingering FX is entity_def's own arm_fx. Only the chunk
        // that owns the CENTRE throws it, or a border spawn would puff up to nine times.
        if (blocking == false && owns_point(e.x, e.y) && e.kind == EntityKind::kSmokeCloud) {
            add_effect(e.x, e.y, EffectKind::kSmoke);
        }
    }

    // Does a circle (a non-blocking entity's footprint) overlap this chunk's tile rectangle?
    // Standard circle-vs-AABB, mirrors the F2 seam fix a Zone used to need — a chunk simply ignores a
    // spawn that does not reach into it.
    [[nodiscard]] bool entity_intersects_chunk(float ex, float ey, float r) const noexcept {
        const float x0 = static_cast<float>(coord.cx * kChunkTiles);
        const float y0 = static_cast<float>(coord.cy * kChunkTiles);
        const float x1 = x0 + static_cast<float>(kChunkTiles);
        const float y1 = y0 + static_cast<float>(kChunkTiles);
        const float nx = std::clamp(ex, x0, x1);
        const float ny = std::clamp(ey, y0, y1);
        const float dx = ex - nx;
        const float dy = ey - ny;
        return dx * dx + dy * dy <= r * r;
    }

    // --- the farming verbs ------------------------------------------------------------------------

    void handle(const PlantCrop& p) noexcept {
        if (!owns(p.tx, p.ty)) return;
        if (terrain_at(p.tx, p.ty) != Terrain::kDirt) return;  // tier-B validates the EFFECT
        if (occupied(p.tx, p.ty)) return;
        Crop c{};
        c.tx = p.tx;
        c.ty = p.ty;
        c.kind = p.kind;
        c.stage = 0;
        c.planted_ms = p.now_ms;
        c.ripe_ms = p.now_ms + grow_ms_of(p.kind);
        crops_.push_back(c);
    }

    void handle(const PlaceBuilding& b) noexcept {
        if (!owns(b.tx, b.ty)) return;
        if (!is_walkable(terrain_at(b.tx, b.ty))) return;
        if (occupied(b.tx, b.ty)) return;
        Building bd{};
        bd.tx = b.tx;
        bd.ty = b.ty;
        bd.kind = b.kind;
        bd.level = 1;
        bd.hp = max_hp_of(b.kind, 1);
        buildings_.push_back(bd);
        // A hearth is where you wake up. Told rather than asked because nothing depends on the
        // answer — and because this chunk is tier B, so "where does the player respawn" is a claim
        // the trusted actor is free to sanity-check rather than a fact it must accept.
        if (b.kind == BuildKind::kHearth && router != nullptr && b.player != 0) {
            router->get<PlayerActor>(b.player).tell(SetRespawn{b.tx, b.ty});
        }
        // RFC-020 §4: the one live `kBuild` fact source this pass wires — the Build screen's own
        // placement verb, matching the table's own citation of it.
        if (router != nullptr && b.player != 0) {
            router->get<PlayerActor>(b.player).tell(
                GameplayFact{FactKind::kBuild, b.player, static_cast<std::uint16_t>(b.kind), 1, 0,
                            static_cast<std::uint32_t>(tick_)});
        }
    }

    // Upgrade in place: level up, and heal by exactly the HP the new level adds, so upgrading a
    // damaged building does not silently repair it.
    void handle(const UpgradeBuilding& u) noexcept {
        if (!owns(u.tx, u.ty)) return;
        for (Building& b : buildings_) {
            if (b.tx != u.tx || b.ty != u.ty) continue;
            if (b.level >= kMaxLevel) return;
            const std::int16_t before = max_hp_of(b.kind, b.level);
            ++b.level;
            const std::int16_t after = max_hp_of(b.kind, b.level);
            b.hp = static_cast<std::int16_t>(b.hp + (after - before));
            return;
        }
    }

    // Base expansion. Farmland is no longer given to anyone — there is no starting apron any more
    // (GAME.md §6b) — so every tile of soil in the world was tilled by a player, and that is this
    // chunk's own overlay, written straight into the terrain cache.
    //
    // Safe to keep out of `terrain_of` because tilling never changes WALKABILITY — dirt and grass
    // are both passable — so a neighbouring chunk (or another node) computing pure terrain for
    // movement still gets the right answer. Only planting cares, and planting is always handled by
    // the tile's owner.
    void handle(const TillGround& t) noexcept {
        if (!owns(t.tx, t.ty)) return;
        const Terrain cur = terrain_at(t.tx, t.ty);
        if (cur == Terrain::kDirt || !is_walkable(cur)) return;  // water/tree must be cleared first
        if (occupied(t.tx, t.ty)) return;
        terrain_[static_cast<std::size_t>(local_tile_index(t.tx, t.ty))] = Terrain::kDirt;
        ++tilled_;
    }

    void handle(const HarvestAt& h) noexcept {
        if (!owns(h.tx, h.ty)) return;
        for (std::size_t i = 0; i < crops_.size(); ++i) {
            Crop& c = crops_[i];
            if (c.tx != h.tx || c.ty != h.ty) continue;
            if (c.stage < kCropStages - 1) return;  // not ripe — nothing happens
            // Credit goes to the TRUSTED actor. A compromised node hosting this chunk can send this
            // message, but it cannot decide what the inventory becomes: PlayerActor owns that, and
            // a rate/plausibility check belongs there rather than here.
            grant(h.player, GrantItems{ItemKind::kProduce, 1 + static_cast<std::int32_t>(c.kind)});
            grant(h.player, GrantItems{ItemKind::kSeed, 1});
            if (router != nullptr && h.player != 0) {
                router->get<PlayerActor>(h.player).tell(GrantXp{Skill::kCraft, 6});
            }
            crops_.erase(crops_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }

    // An answer to this proves every message posted before it has been drained (mailbox FIFO), so
    // the headless runner uses it as a barrier instead of sleeping.
    void handle(const Ask<GetChunkStats, ChunkStats>& m) noexcept {
        ChunkStats s{};
        s.creatures = static_cast<std::uint32_t>(creatures_.size());
        s.projectiles = static_cast<std::uint32_t>(shots_.size());
        s.entities = static_cast<std::uint32_t>(entities_.size());
        s.scars = static_cast<std::uint32_t>(scars_.size());
        s.effects = static_cast<std::uint32_t>(effects_.size());
        s.watchers = static_cast<std::uint32_t>(players_.size());
        s.crops = static_cast<std::uint32_t>(crops_.size());
        s.buildings = static_cast<std::uint32_t>(buildings_.size());
        s.tilled = tilled_;
        s.patches = static_cast<std::uint32_t>(patches_.size());
        s.fields = static_cast<std::uint32_t>(fields_.size());
        s.telegraphs = static_cast<std::uint32_t>(telegraphs_.size());
        for (const TilePatch& p : patches_) {
            if (p.s == Surface::kBurning) ++s.burning;
        }
        for (const Creature& c : creatures_) {
            if (c.disposition == Disposition::kHostile || c.anger_ticks > 0) ++s.hostile;
            // "Afflicted" is any status at all — a ladder primary OR a coexisting coating (Wet no
            // longer has a primary slot of its own under RFC-002, so this must check both).
            if (c.status.primary != Channel::kNone || c.status.coatings != 0) ++s.afflicted;
        }
        for (const Building& b : buildings_) s.building_levels += b.level;
        s.tick = tick_;
        for (const Crop& c : crops_)
            if (c.stage >= kCropStages - 1) ++s.ripe;
        m.respond(s);
    }

    // ================================ bring-up ====================================================

    // Fill this chunk's terrain cache from the world's terrain function. The cache exists only to
    // avoid recomputing the hash for the tiles this chunk draws and walks most often — it is never
    // the source of truth, so it can never disagree with what a neighbour computes.
    void generate_terrain(std::uint64_t world_seed) noexcept {
        world_seed_ = world_seed;
        for (int ly = 0; ly < kChunkTiles; ++ly) {
            for (int lx = 0; lx < kChunkTiles; ++lx) {
                terrain_[static_cast<std::size_t>(ly * kChunkTiles + lx)] =
                    terrain_of(world_seed, coord.map, coord.cx * kChunkTiles + lx,
                               coord.cy * kChunkTiles + ly);
            }
        }
    }

    // Animals, placed once from the chunk key. They are not spawned by the director and never
    // respawn: wildlife is scenery that happens to fight back, and a respawning deer would make the
    // world feel like a spreadsheet refilling itself. Density is per-ring and deliberately low —
    // one or two per chunk is already ~1500 animals in the world.
    void seed_wildlife(std::uint64_t world_seed) noexcept {
        Rng rng(world_seed ^ (chunk_key(coord) * 0xA24B'AED4'963E'E407ull));
        const int mid_x = coord.cx * kChunkTiles + kChunkTiles / 2;
        const int mid_y = coord.cy * kChunkTiles + kChunkTiles / 2;
        const Ring ring = ring_of(world_seed, mid_x, mid_y);
        // Life thins out as the land gets worse: the meadow is busy, the wasteland nearly bare.
        const std::uint32_t chance = ring == Ring::kMeadow      ? 70
                                     : ring == Ring::kForest    ? 60
                                     : ring == Ring::kWetland   ? 35
                                     : ring == Ring::kSnow      ? 22
                                                                : 10;
        const int herd = 1 + static_cast<int>(rng.below(3));
        if (rng.below(100) >= chance) return;
        // Animals arrive as a group around one spot, not scattered evenly: a wolf pack you can walk
        // around is a decision, four lone wolves spread over a chunk is just noise.
        const int hx = coord.cx * kChunkTiles + static_cast<int>(rng.below(kChunkTiles));
        const int hy = coord.cy * kChunkTiles + static_cast<int>(rng.below(kChunkTiles));
        const CreatureKind kind = wildlife_kind_of(ring, rng.next() >> 8);
        for (int i = 0; i < herd; ++i) {
            const int tx = std::clamp(hx + static_cast<int>(rng.below(5)) - 2, 0, kMapTiles - 1);
            const int ty = std::clamp(hy + static_cast<int>(rng.below(5)) - 2, 0, kMapTiles - 1);
            if (!owns(static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty))) continue;
            if (!is_walkable(terrain_at(tx, ty))) continue;
            creatures_.push_back(make_creature(kind, tx, ty, /*wanders*/ true));
        }
    }

    // Publish once at bring-up, before the engine starts. Required by the LOD rule above: an
    // unwatched chunk republishes only every 32nd tick, so without a first publish the renderer
    // would have nothing to draw for that chunk's terrain until tick 32 — a visible three-second
    // hole in the world on the first frames.
    void publish_now() noexcept { publish(); }

    void add_building(std::uint16_t tx, std::uint16_t ty, BuildKind k) noexcept {
        Building b{};
        b.tx = tx;
        b.ty = ty;
        b.kind = k;
        b.hp = max_hp_of(k);
        buildings_.push_back(b);
    }

    // Wire a dojo boss (F3) into this chunk, once at bring-up. `room` is an interior room index whose
    // door a village's DOJO leads into (WorldLayout::dojo_rooms); the chunk that owns that room's
    // block is the one this is called on. The boss stands at the room's top-centre and is clamped to
    // the room floor forever; the player enters at the bottom-centre door, so the fight opens with the
    // whole room between them — long enough for the boss's opening charge to read. The body is spawned
    // here so it is drawn from the first frame; a later death respawns it via step_bosses.
    void add_boss(std::uint32_t room) noexcept {
        BossState b{};
        b.room = room;
        const int bx = room_block_x(static_cast<int>(room));
        const int by = room_block_y(static_cast<int>(room));
        // Floor rectangle (see tiles.hpp kRoom* constants), inset one tile from the side walls so the
        // giant does not clip into a corner. The feet clamp uses tile centres (+0.5).
        const int fx0 = bx + kRoomX0 + 1;
        const int fx1 = bx + kRoomX0 + kRoomW - 2;
        const int fy0 = by + kRoomY0;
        const int fy1 = by + kRoomY0 + kRoomH - 1;
        b.cx0 = static_cast<float>(fx0) + 0.5f;
        b.cx1 = static_cast<float>(fx1) + 0.5f;
        b.cy0 = static_cast<float>(fy0) + 0.5f;
        b.cy1 = static_cast<float>(fy1) + 0.5f;
        b.spawn_tx = static_cast<std::uint16_t>(bx + kRoomX0 + kRoomW / 2);
        b.spawn_ty = static_cast<std::uint16_t>(by + kRoomY0 + 1);
        bosses_.push_back(b);
        spawn_boss(bosses_.back());
    }

    // RFC-023: wire a civilian NPC into this chunk, once at bring-up (World::build_npcs, mirroring
    // add_boss above). `home_struct` is the index of the house Structure this NPC is anchored to;
    // `door_tx/door_ty` is where it stands (a stationary role stands there forever, a wandering one
    // strays up to `npc_wander_radius(role)` tiles from it and always returns). Spawned already
    // Idle, at full HP, disposition kNeutral per RFC-023 §3's normative construction rule (never left
    // at Creature's own kHostile default), damage 0 (§6: no civilian role is ever combat-capable).
    void add_npc(NpcRole role, std::uint32_t home_struct, int door_tx, int door_ty) noexcept {
        Creature c{};
        c.id = ++next_id_ | (static_cast<std::uint32_t>(chunk_key(coord)) << 12);
        c.x = static_cast<float>(door_tx) + 0.5f;
        c.y = static_cast<float>(door_ty) + 0.5f;
        c.home_tx = static_cast<std::uint16_t>(door_tx);
        c.home_ty = static_cast<std::uint16_t>(door_ty);
        // A placeholder combat-stat/defender row, not a meaningful species — see npc.hpp's header
        // note on why CreatureKind gets no new NPC-capable entries. kChicken is the weakest existing
        // wildlife row, which is why it was picked over any other kind for this purpose.
        c.kind = CreatureKind::kChicken;
        c.max_hp = kNpcMaxHp;
        c.hp = c.max_hp;
        c.damage = 0;
        c.disposition = Disposition::kNeutral;
        const DefenderProfile def = defender_of(CreatureKind::kChicken);
        c.material = def.material;
        c.tier = def.tier;
        c.toughness = tier_toughness(def.tier);
        c.facing = Facing::kDown;
        npc_init(c, role, home_struct);
        creatures_.push_back(c);
    }

    // RFC-016 §6.4: folds a recovered ChunkOverlaySnapshot into this chunk — called once at
    // bring-up by World::recover_overlay(), AFTER generate_terrain() has already run and BEFORE
    // the world starts ticking, so a recovered tilled tile always overwrites the seed-derived
    // terrain underneath it, never the reverse (§6.4's own ordering rule).
    void apply_recovered_overlay(const ChunkOverlaySnapshot& snap) noexcept {
        crops_ = snap.crops;
        buildings_ = snap.buildings;
        for (const TilledTile& t : snap.tilled) {
            if (!owns(t.tx, t.ty)) continue;
            terrain_[static_cast<std::size_t>(local_tile_index(t.tx, t.ty))] = Terrain::kDirt;
            ++tilled_;  // handle(TillGround)'s own counter — GetChunkStats reads it, not terrain_[]
        }
    }

    // Test/tools accessor, mirroring GetChunkStats's own `tilled` field — for asserting
    // apply_recovered_overlay() without needing a full engine+router+mailbox round trip.
    [[nodiscard]] std::uint32_t tilled_count() const noexcept { return tilled_; }

    [[nodiscard]] bool owns(std::uint16_t tx, std::uint16_t ty) const noexcept {
        return tx / kChunkTiles == coord.cx && ty / kChunkTiles == coord.cy;
    }

    [[nodiscard]] bool owns_point(float fx, float fy) const noexcept {
        return in_map(fx, fy) && chunk_of(coord.map, fx, fy) == coord;
    }

    // Owned tiles come from the cache; anything else is computed. The `owns` test is what makes
    // this correct at a chunk border — an earlier version indexed the cache unconditionally, and
    // because `local_tile_index` wraps modulo, a lookup one tile past the edge silently returned a
    // tile from the *opposite* side of this chunk. Creatures stepping across a boundary read that
    // unrelated tile, and any whose mirrored tile happened to be water froze on the border forever.
    [[nodiscard]] Terrain terrain_at(int tx, int ty) const noexcept {
        if (tx / kChunkTiles == coord.cx && ty / kChunkTiles == coord.cy) {
            return terrain_[static_cast<std::size_t>(local_tile_index(tx, ty))];
        }
        return terrain_of(world_seed_, coord.map, tx, ty);
    }

private:
    // --- creation ---------------------------------------------------------------------------------
    // Stats are baked in at birth, already scaled for the ring the creature was born in. A slime
    // that wanders inward from the wasteland stays a wasteland slime — which is both the honest
    // reading of "the outer rings are harder" and the only version that cannot be gamed by luring
    // something across a boundary.
    [[nodiscard]] Creature make_creature(CreatureKind kind, int tx, int ty, bool wanders) noexcept {
        const CreatureStats st = stats_of(kind);
        const Ring ring = ring_of(world_seed_, tx, ty);
        Creature c{};
        c.id = ++next_id_ | (static_cast<std::uint32_t>(chunk_key(coord)) << 12);
        c.x = static_cast<float>(tx) + 0.5f;
        c.y = static_cast<float>(ty) + 0.5f;
        c.max_hp = static_cast<std::int16_t>(static_cast<float>(st.max_hp) * ring_hp_scale(ring));
        c.hp = c.max_hp;
        c.damage = static_cast<std::int16_t>(static_cast<float>(st.damage) * ring_damage_scale(ring));
        c.kind = kind;
        c.disposition = st.disposition;
        const DefenderProfile def = defender_of(kind);
        c.material = def.material;
        c.tier = def.tier;
        c.toughness = tier_toughness(def.tier);
        if (wanders) {
            c.home_tx = static_cast<std::uint16_t>(tx);
            c.home_ty = static_cast<std::uint16_t>(ty);
        }
        return c;
    }

    // --- beacons ----------------------------------------------------------------------------------
    void expire_beacons() noexcept {
        for (std::size_t i = players_.size(); i-- > 0;) {
            if (tick_ >= players_[i].tick + kBeaconLease) {
                players_.erase(players_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    // Nearest living player within `range`, or null.
    [[nodiscard]] const PlayerBeacon* nearest_player(float x, float y, float range) const noexcept {
        const PlayerBeacon* best = nullptr;
        float best_d2 = range * range;
        for (const PlayerBeacon& p : players_) {
            if (p.hp <= 0) continue;
            const float dx = p.x - x;
            const float dy = p.y - y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > best_d2) continue;
            best_d2 = d2;
            best = &p;
        }
        return best;
    }

    // --- crop growth -----------------------------------------------------------------------------
    // Stage is derived from elapsed wall-clock time rather than accumulated per-tick counters, so a
    // chunk that was idle (deactivated, or migrated between nodes) catches up correctly the moment
    // it next ticks. There is no "lost growth" failure mode to debug.
    void grow_crops() noexcept {
        for (Crop& c : crops_) {
            const std::int64_t span = c.ripe_ms - c.planted_ms;
            if (span <= 0) {
                c.stage = kCropStages - 1;
                continue;
            }
            const std::int64_t done = std::clamp(world_ms_ - c.planted_ms, std::int64_t{0}, span);
            c.stage = static_cast<std::uint8_t>((done * (kCropStages - 1)) / span);
        }
    }

    // --- elemental status (RFC-002/009) -------------------------------------------------------------
    // Every status_gain call below computes its `mult_pm` from the creature's own material/tier via
    // `mult_pm_of` (combat_math.hpp) — RFC-009's real gain formula, no longer an identity stand-in.
    // `kSpellPower`/`kHeavyMeleeImpulse`/`kCrushBlowImpulse`/`channel_of` moved to namespace scope
    // above the class (RFC-017): still this file's, just reachable from outside a ChunkActor too.

    // RFC-009 §4.5's derived-Stagger rule, simplified to a single-channel proxy: this engine has one
    // flat `damage` scalar per hit, not RFC-003's unbuilt 7-channel taxonomy (Crush/Explosion/
    // Damage/Pierce), so a heavy blow is treated as pure Crush-weight (1000‰), a light melee blow as
    // Damage-weight (200‰), and an arrow as Pierce-weight (100‰) — spells contribute none, matching
    // the RFC's own table.
    [[nodiscard]] static constexpr std::uint16_t derived_stagger_power(std::int16_t damage, bool heavy,
                                                                       bool by_projectile) noexcept {
        if (damage <= 0) return 0;
        const std::uint32_t weight_pm = heavy ? 1000u : (by_projectile ? 100u : 200u);
        return static_cast<std::uint16_t>((static_cast<std::uint32_t>(damage) * weight_pm) / 1000u);
    }

    void step_status() noexcept {
        for (Creature& c : creatures_) {
            if (c.hp <= 0) continue;
            const StepResult r = status_step(c.status, c.gauges, /*dticks*/ 1, tick_,
                                             tier_terminal_dur_pm(c.tier));
            std::int16_t dmg = r.dot_damage;
            if (r.combust) {
                // Heat's terminal (Combust) bursts for min(15% max_hp, 60) as it resolves — this
                // header cannot compute that (no access to max_hp), so status_step only reports the
                // event and this caller applies it.
                dmg = static_cast<std::int16_t>(dmg + std::min<std::int32_t>(60, (c.max_hp * 15) / 100));
            }
            if (dmg <= 0) continue;
            c.hp = static_cast<std::int16_t>(c.hp - dmg);
            // RFC-019 §5.8: an actively-ticking burn keeps its owner's assist-window entry rolling,
            // the same as a live flurry of hits would — a burn that has been dealing damage every
            // tick for longer than kAssistWindowTicks is still "someone currently fighting this,"
            // not a stale tag from a fight that ended minutes ago.
            record_contribution(c, c.dot_owner, Skill::kMagic);
            if (c.hp > 0) continue;
            // A DoT tick took the last point: credit the kill through the same path a direct strike
            // uses, crediting whoever last fed this creature's primary channel (RFC-002 §1's
            // `dot_owner`) rather than leaving it silently uncredited.
            credit_kill(c, c.dot_owner, Skill::kMagic);
        }
    }

    // --- combat entities (RFC-004) -----------------------------------------------------------------

    [[nodiscard]] CombatEntity make_entity(EntityKind kind, float x, float y, Faction team,
                                           std::uint64_t owner, float radius_override) noexcept {
        const EntityDef def = entity_def(kind);
        CombatEntity ce{};
        // Same chunk-local-monotonic-OR-chunk-key scheme as creatures/projectiles (make_creature,
        // handle(LaunchArrow)).
        ce.id = ++next_id_ | (static_cast<std::uint32_t>(chunk_key(coord)) << 12);
        ce.kind = kind;
        ce.state = EntityState::kArming;
        ce.team = team;
        ce.hp = def.base_hp;
        ce.x = x;
        ce.y = y;
        ce.radius = radius_override > 0.0f ? radius_override : def.default_radius;
        ce.owner = owner;
        ce.state_tick = tick_;
        return ce;
    }

    // Any creature or player standing on the footprint tile? The anti-trap rule (§4) tests this
    // exactly once, at the kArming -> kActive transition of a blocking entity.
    [[nodiscard]] bool footprint_occupied(float x, float y) const noexcept {
        const int tx = static_cast<int>(x);
        const int ty = static_cast<int>(y);
        for (const Creature& c : creatures_) {
            if (c.hp <= 0) continue;
            if (static_cast<int>(c.x) == tx && static_cast<int>(c.y) == ty) return true;
        }
        for (const PlayerBeacon& p : players_) {
            if (p.hp <= 0) continue;
            if (static_cast<int>(p.x) == tx && static_cast<int>(p.y) == ty) return true;
        }
        return false;
    }

    // RFC-002 §8's aura apply: feed `def.aura_channel`'s build-up or refresh `def.aura_coating` on
    // every creature in radius that `aura_affects` selects. There is no "bare ground or an existing
    // match only" gate any more (the old Status stand-in's rule) — the one-slot promotion algorithm
    // (status.hpp) is itself the replacement: an aura's gain simply accumulates in its own gauge,
    // and it only becomes primary by out-accumulating whatever currently holds the slot.
    void apply_aura(const CombatEntity& e, const EntityDef& def) noexcept {
        const float r2 = e.radius * e.radius;
        for (Creature& c : creatures_) {
            if (c.hp <= 0) continue;
            if (def.aura_affects == AuraAffects::kEnemiesOfTeam &&
                stance_between(e.team, stats_of(c.kind).faction) != Stance::kHostile) {
                continue;
            }
            const float dx = c.x - e.x;
            const float dy = c.y - e.y;
            if (dx * dx + dy * dy > r2) continue;
            if (def.aura_kind == AuraKind::kChannel) {
                status_gain(c.status, c.gauges,
                           BuildupPacket{def.aura_channel, def.aura_gain, 0, e.owner},
                           mult_pm_of(c.material, c.tier, def.aura_channel), tick_);
                if (e.owner != 0) c.dot_owner = e.owner;
            } else if (def.aura_kind == AuraKind::kCoating) {
                status_coat(c.status, CoatingPacket{def.aura_coating, def.aura_coating_ticks});
            }
        }
    }

    // Marks an entity kDying and — unless this was an interception/anti-trap whiff, which stamp/spawn
    // nothing (§3) — applies its death products. Products only STAGE here; they are pushed to
    // `entities_` once this whole step_entities() pass is done (see the `pending` vector below),
    // because pushing mid-loop could reallocate the vector out from under other live references.
    struct PendingSpawn {
        EntityKind kind;
        float x, y;
        Faction team;
        std::uint64_t owner;
    };

    void kill_entity(CombatEntity& e, const EntityDef& def, bool apply_products,
                    std::vector<PendingSpawn>& pending) noexcept {
        e.state = EntityState::kDying;
        e.state_tick = tick_;
        if (!apply_products) return;
        if (def.death_scar != ScarKind::kNone) {
            stamp_scar(static_cast<int>(e.x), static_cast<int>(e.y), def.death_scar);
        }
        if (def.death_spawn != EntityKind::kCount) {
            pending.push_back(PendingSpawn{def.death_spawn, e.x, e.y, e.team, e.owner});
        }
    }

    // Rebuilds the two derived occupancy bitmaps from scratch. Cheap (kMaxEntities=16) and called
    // only on an entity state change, never every tick — never published, always recomputed by
    // whichever consumer needs it (RFC-004 §4's "derived, not stored" discipline).
    void rebuild_occupancy_bits() noexcept {
        block_bits_.reset();
        vision_bits_.reset();
        const int cx0 = coord.cx * kChunkTiles;
        const int cy0 = coord.cy * kChunkTiles;
        for (const CombatEntity& e : entities_) {
            if (e.state != EntityState::kActive) continue;
            const EntityDef def = entity_def(e.kind);
            if (def.collision != Collision::kNone) {
                const auto tx = static_cast<int>(e.x);
                const auto ty = static_cast<int>(e.y);
                if (owns(static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty))) {
                    block_bits_.set(static_cast<std::size_t>(local_tile_index(tx, ty)));
                }
            }
            if (def.blocks_vision) {
                for (int ly = 0; ly < kChunkTiles; ++ly) {
                    for (int lx = 0; lx < kChunkTiles; ++lx) {
                        const float tcx = static_cast<float>(cx0 + lx) + 0.5f;
                        const float tcy = static_cast<float>(cy0 + ly) + 0.5f;
                        if (circle_covers_tile(tcx, tcy, e.x, e.y, e.radius)) {
                            vision_bits_.set(static_cast<std::size_t>(ly * kChunkTiles + lx));
                        }
                    }
                }
            }
        }
    }

    // Step every entity through arm -> active -> dying -> removed. Called before creatures, the same
    // position step_zones held: an active kSmokeCloud/aura changes what a creature does this tick.
    void step_entities() noexcept {
        std::vector<PendingSpawn> pending;

        for (std::size_t i = entities_.size(); i-- > 0;) {
            CombatEntity& e = entities_[i];
            const EntityDef def = entity_def(e.kind);

            if (e.state == EntityState::kArming) {
                // (a) Intercepted: checked every tick, not only at arm elapse — a projectile can
                // kill a hittable-while-arming kFallingRock before its telegraph finishes.
                const bool intercepted = def.hittable_while_arming && e.hp <= 0;
                const bool arm_done = tick_ >= e.state_tick + def.arm_ticks;
                if (!intercepted && !arm_done) continue;
                // (b) The anti-trap whiff: only meaningful for a blocking kind, only at arm elapse.
                const bool occupied = !intercepted && arm_done && def.collision != Collision::kNone &&
                                      footprint_occupied(e.x, e.y);
                if (next_state_after_arm(def, intercepted, occupied) == EntityState::kDying) {
                    kill_entity(e, def, /*apply_products=*/!intercepted && !occupied, pending);
                    rebuild_occupancy_bits();
                    continue;
                }
                e.state = EntityState::kActive;
                e.state_tick = tick_;
                e.expire_tick = tick_ + def.life_ticks;
                e.next_aura_tick = tick_;
                rebuild_occupancy_bits();
                // Falls through to run this tick's Active-phase logic immediately below — matches
                // the old zone's "wets what it owns inside the circle" same-tick behaviour for a
                // zero-arm-tick kind like kWaterPool.
            }

            if (e.state == EntityState::kActive) {
                if (e.kind == EntityKind::kSmokeCloud) {
                    // kSmokeCloud's aggro-suppress is a kind-keyed special case, not table-driven —
                    // "drop target and cannot re-acquire" is not a Status. Reproduces
                    // ZoneKind::kSmokeSuppress's exact behaviour; the "cannot re-acquire" half is
                    // still enforced in step_creatures via in_suppress_cloud().
                    const float r2 = e.radius * e.radius;
                    for (Creature& c : creatures_) {
                        if (c.hp <= 0) continue;
                        const float dx = c.x - e.x;
                        const float dy = c.y - e.y;
                        if (dx * dx + dy * dy > r2) continue;
                        c.target = 0;
                        c.anger_ticks = 0;
                    }
                } else if (def.aura_period > 0 && tick_ >= e.next_aura_tick) {
                    apply_aura(e, def);
                    e.next_aura_tick = tick_ + def.aura_period;
                }
                const bool dead = def.destroyable && e.hp <= 0;
                if (dead || tick_ >= e.expire_tick) {
                    kill_entity(e, def, /*apply_products=*/true, pending);
                    rebuild_occupancy_bits();
                }
                continue;
            }

            // kDying: inert, waiting out its death FX.
            if (tick_ >= e.state_tick + effect_life_of(def.death_fx)) {
                entities_.erase(entities_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }

        // Apply staged death-spawn products now that the loop above is done mutating `entities_` by
        // index — pushing mid-loop could reallocate the vector out from under the `e` references above.
        for (const PendingSpawn& p : pending) {
            if (entities_.size() >= kMaxEntities) break;  // refusal, not eviction, applies here too
            entities_.push_back(make_entity(p.kind, p.x, p.y, p.team, p.owner, 0.0f));
        }
    }

    // Is this point under an Active smoke cloud? Cheap and usually a no-op — most chunks hold no
    // entity, and the caller guards on `entities_.empty()` before asking.
    [[nodiscard]] bool in_suppress_cloud(float px, float py) const noexcept {
        for (const CombatEntity& e : entities_) {
            if (e.kind != EntityKind::kSmokeCloud || e.state != EntityState::kActive) continue;
            const float dx = px - e.x;
            const float dy = py - e.y;
            if (dx * dx + dy * dy <= e.radius * e.radius) return true;
        }
        return false;
    }

    // Damage to an entity is deliberately dumber than to a creature (RFC-004 §7): no statuses, no
    // combos, no build-up — just the v1 element multiplier stand-in.
    void strike_entity(CombatEntity& e, std::int16_t damage, Element element, bool heavy) noexcept {
        if (damage <= 0 || e.hp <= 0) return;
        const float scale = entity_damage_scale(e.kind, element, heavy);
        const auto dealt = static_cast<std::int16_t>(static_cast<float>(damage) * scale);
        e.hp = static_cast<std::int16_t>(e.hp - dealt);
    }

    // Shared by MeleeSwing's arc and AbilityStrike's kFront/kRing shapes — "no third loop is added"
    // (§7): entities join the two hit loops that already exist rather than getting one of their own.
    void strike_entities_in_shape(float cx, float cy, float radius, float fx, float fy,
                                  bool front_only, std::int16_t damage, Element element,
                                  bool heavy) noexcept {
        for (CombatEntity& e : entities_) {
            if (e.state != EntityState::kActive || !entity_def(e.kind).destroyable) continue;
            const float dx = e.x - cx;
            const float dy = e.y - cy;
            const float d2 = dx * dx + dy * dy;
            if (d2 > radius * radius) continue;
            if (front_only && d2 > 0.01f && (dx * fx + dy * fy) < 0.0f) continue;
            strike_entity(e, damage, element, heavy);
        }
    }

    // --- the terrain scar layer (RFC-004 §8) --------------------------------------------------------

    [[nodiscard]] ScarKind scar_kind_at(int tx, int ty) const noexcept {
        if (tx / kChunkTiles != coord.cx || ty / kChunkTiles != coord.cy) return ScarKind::kNone;
        const auto ltx = static_cast<std::uint8_t>(tx % kChunkTiles);
        const auto lty = static_cast<std::uint8_t>(ty % kChunkTiles);
        for (const Scar& s : scars_) {
            if (s.tx == ltx && s.ty == lty) return s.kind;
        }
        return ScarKind::kNone;
    }

    // Stamp (or escalate) a scar at a map-global tile this chunk owns. §8.4's ladder: a scarring
    // impact within `kEscalateWindow` of the existing mark's `made_tick` upgrades it one step;
    // outside the window, or with no existing mark, it (re-)stamps `kCracked`.
    void stamp_scar(int tx, int ty, ScarKind kind) noexcept {
        if (kind == ScarKind::kNone) return;
        const auto ltx = static_cast<std::uint8_t>(tx % kChunkTiles);
        const auto lty = static_cast<std::uint8_t>(ty % kChunkTiles);
        for (Scar& s : scars_) {
            if (s.tx != ltx || s.ty != lty) continue;
            s.kind = escalate(s.kind, s.made_tick, tick_);
            s.made_tick = tick_;
            s.heal_tick = tick_ + heal_ticks_of(s.kind);
            return;
        }
        const Scar fresh{ltx, lty, ScarKind::kCracked, tick_ + heal_ticks_of(ScarKind::kCracked), tick_};
        if (scars_.size() >= kMaxScars) {
            // Full: the oldest mark (smallest made_tick) is overwritten (§8's own rule).
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < scars_.size(); ++i) {
                if (scars_[i].made_tick < scars_[oldest].made_tick) oldest = i;
            }
            scars_[oldest] = fresh;
            return;
        }
        scars_.push_back(fresh);
    }

    // Lazily fast-forward every scar's heal ladder and drop the ones that have fully healed. Bounded
    // by severity levels per scar (see `heal_lazy`), never by elapsed ticks — the "wake" contract.
    void step_scars() noexcept {
        for (std::size_t i = scars_.size(); i-- > 0;) {
            scars_[i] = heal_lazy(scars_[i], tick_);
            if (scars_[i].kind == ScarKind::kNone) {
                scars_.erase(scars_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    // --- battlefield: tile patches (RFC-010 §4.2) ---------------------------------------------------

    // Heat/tick a live kBurning patch feeds an occupant — a chosen tunable (RFC-010's own text leaves
    // the rate to RFC-009), picked to match kFirePatch's existing aura_gain=350 per 10-tick pulse
    // (combat_entity.hpp) so standing in ambient fire and standing on a combat-ignited patch burn at
    // comparable rates.
    static constexpr std::uint16_t kBurnPatchPower = 35;

    [[nodiscard]] bool surface_at(int tx, int ty, Surface& out) const noexcept {
        for (const TilePatch& p : patches_) {
            if (p.tx == tx && p.ty == ty) {
                out = p.s;
                return true;
            }
        }
        return false;
    }

    // Insert/replace at a map-global tile this chunk owns. §4.2: one patch per tile — a new patch on
    // a patched tile REPLACES it; when the array is full, the record with the smallest `end_ms` is
    // overwritten, or the incoming patch is dropped if IT would be the soonest to expire.
    void write_patch(int tx, int ty, Surface s, std::int64_t end_ms) noexcept {
        for (TilePatch& p : patches_) {
            if (p.tx == tx && p.ty == ty) {
                p.s = s;
                p.end_ms = end_ms;
                return;
            }
        }
        const TilePatch fresh{static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty), s, 0, end_ms};
        if (patches_.size() >= kMaxPatches) {
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < patches_.size(); ++i) {
                if (patch_expires_before(patches_[i], patches_[oldest])) oldest = i;
            }
            if (patch_expires_before(patches_[oldest], fresh)) patches_[oldest] = fresh;
            return;
        }
        patches_.push_back(fresh);
    }

    void remove_patch(int tx, int ty) noexcept {
        for (std::size_t i = 0; i < patches_.size(); ++i) {
            if (patches_[i].tx == tx && patches_[i].ty == ty) {
                patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    // Fire/Ice spell impact at the spell's own target tile (§4.2's trigger table). Rock (kEarth)
    // impacts stamp a scar directly at their own call site instead of going through this table — the
    // Rock row is "no patch" by the RFC's own text; Thunder never produces a patch either.
    void apply_surface_impact(int tx, int ty, Element element) noexcept {
        Surface existing{};
        const bool has = surface_at(tx, ty, existing);
        if (element == Element::kFire) {
            if (!has && !flammable_of(terrain_at(tx, ty))) return;
            const SurfaceOutcome o = fire_impact(has, existing);
            if (o.event == SurfaceEvent::kRemoved) {
                remove_patch(tx, ty);
                return;
            }
            write_patch(tx, ty, o.result, world_ms_ + dur_ms_of(o.result));
        } else if (element == Element::kIce) {
            const SurfaceOutcome o = ice_impact(has, existing);
            if (o.event == SurfaceEvent::kRemoved) {
                remove_patch(tx, ty);
                return;
            }
            write_patch(tx, ty, o.result, world_ms_ + dur_ms_of(o.result));
        }
    }

    void step_patches() noexcept {
        // Decay chain: burnout stamps an RFC-004 kScorched scar (which heals on ITS OWN clock); the
        // other two surfaces simply revert to baseline. `end_ms` is an absolute deadline, never a
        // countdown (Invariant L-2) — correct under any future tick-rate LOD without rework.
        for (std::size_t i = patches_.size(); i-- > 0;) {
            if (world_ms_ < patches_[i].end_ms) continue;
            if (patches_[i].s == Surface::kBurning) {
                stamp_scar(patches_[i].tx, patches_[i].ty, ScarKind::kScorched);
            }
            patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(i));
        }

        // Occupant Burn build-up: every live kBurning patch feeds Heat to whatever stands on its tile.
        for (const TilePatch& p : patches_) {
            if (p.s != Surface::kBurning) continue;
            for (Creature& c : creatures_) {
                if (c.hp <= 0) continue;
                if (static_cast<int>(c.x) != p.tx || static_cast<int>(c.y) != p.ty) continue;
                status_gain(c.status, c.gauges, BuildupPacket{Channel::kHeat, kBurnPatchPower, 0, 0},
                           mult_pm_of(c.material, c.tier, Channel::kHeat), tick_);
            }
        }

        // Fire spread (§4.2), array-layout-independent per §5's D3 requirement: candidates are
        // gathered from ALL kBurning patches, merged onto their canonical (tx,ty) keeping the
        // strongest inherited window, sorted lexicographic, then rolled in THAT order on a stream
        // salted separately from step_creatures'/step_bosses' own per-tick Rng instances (§5: "each
        // D3 consumer constructs its own Rng instance").
        std::size_t burning_count = 0;
        for (const TilePatch& p : patches_) {
            if (p.s == Surface::kBurning) ++burning_count;
        }
        if (tick_ % kSpreadPeriod == 0 && burning_count > 0 && burning_count < kMaxBurning) {
            struct Candidate {
                std::uint16_t tx, ty;
                std::int64_t inherit_end_ms;
            };
            std::vector<Candidate> cand;
            static constexpr int kDx[4] = {1, -1, 0, 0};
            static constexpr int kDy[4] = {0, 0, 1, -1};
            for (const TilePatch& p : patches_) {
                if (p.s != Surface::kBurning) continue;
                for (int d = 0; d < 4; ++d) {
                    const int nx = static_cast<int>(p.tx) + kDx[d];
                    const int ny = static_cast<int>(p.ty) + kDy[d];
                    if (nx < 0 || ny < 0 || nx >= kMapTiles || ny >= kMapTiles) continue;
                    if (!owns(static_cast<std::uint16_t>(nx), static_cast<std::uint16_t>(ny))) continue;
                    Surface unused{};
                    if (surface_at(nx, ny, unused)) continue;  // already patched
                    // Also the closest thing to a fireproof-claim/village-wall check this engine has:
                    // no ownership or wall-ring lookup exists anywhere in the codebase yet (confirmed
                    // by survey), so §4.6 rule 3's guarantee is only PARTIALLY honored — a village's
                    // rampart tiles are `Terrain::kBuilding`, hence never flammable, but an unwalled
                    // claim interior on grass is not fireproofed this pass. Named divergence, not an
                    // oversight: building the claim system is out of this RFC's scope entirely.
                    if (!flammable_of(terrain_at(nx, ny))) continue;
                    cand.push_back({static_cast<std::uint16_t>(nx), static_cast<std::uint16_t>(ny), p.end_ms});
                }
            }
            std::sort(cand.begin(), cand.end(), [](const Candidate& a, const Candidate& b) {
                if (a.tx != b.tx) return a.tx < b.tx;
                return a.ty < b.ty;
            });
            std::vector<Candidate> merged;
            for (const Candidate& c : cand) {
                if (!merged.empty() && merged.back().tx == c.tx && merged.back().ty == c.ty) {
                    merged.back().inherit_end_ms = std::max(merged.back().inherit_end_ms, c.inherit_end_ms);
                } else {
                    merged.push_back(c);
                }
            }
            Rng spread_rng((chunk_key(coord) ^ 0x5EED'F17Eull) * 0x9E37'79B9'7F4A'7C15ull + tick_);
            for (const Candidate& c : merged) {
                if (burning_count >= kMaxBurning) break;
                if (spread_rng.below(1000) >= kSpreadChancePm) continue;
                if (c.inherit_end_ms <= world_ms_) continue;  // the igniter is itself about to expire
                write_patch(c.tx, c.ty, Surface::kBurning, c.inherit_end_ms);
                ++burning_count;
            }
        }
    }

    // --- battlefield: field states (RFC-010 §4.3) ---------------------------------------------------
    // No in-game path creates a FieldState this pass — see battlefield.hpp's header note (RFC-005
    // boss authoring and a StrongholdActor raid event, this RFC's only two creation authorities,
    // neither built yet). step_fields/field_intensity_at are wired for real so strike()/
    // step_projectiles already do the right thing the day a creator lands.

    void step_fields() noexcept {
        for (std::size_t i = fields_.size(); i-- > 0;) {
            if (world_ms_ >= fields_[i].end_ms) {
                fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    // §4.3 stacking: an actor inside several fields takes the single highest intensity.
    [[nodiscard]] std::uint8_t field_intensity_at(float x, float y) const noexcept {
        std::uint8_t best = 0;
        for (const FieldState& f : fields_) {
            const float dx = x - static_cast<float>(f.cx);
            const float dy = y - static_cast<float>(f.cy);
            if (dx * dx + dy * dy > static_cast<float>(f.radius) * static_cast<float>(f.radius)) continue;
            best = highest_intensity(best, f.intensity);
        }
        return best;
    }

    // --- battlefield: telegraphs (RFC-006 §1.4/§2) --------------------------------------------------
    // Only FIZZLE aging happens here: a windup-tied telegraph's `left` is decremented in lockstep with
    // `c.windup` (step_boss_ai), never independently, so the two can never drift apart.

    void step_telegraphs() noexcept {
        for (std::size_t i = telegraphs_.size(); i-- > 0;) {
            if ((telegraphs_[i].flags & kTelegraphFizzling) == 0) continue;
            if (telegraphs_[i].left == 0 || --telegraphs_[i].left == 0) {
                telegraphs_.erase(telegraphs_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    [[nodiscard]] Telegraph* find_telegraph(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        for (Telegraph& t : telegraphs_) {
            if (t.id == id) return &t;
        }
        return nullptr;
    }

    // §2's cap/eviction rule. Returns the new record's id, or 0 if the commit is refused (a
    // well-defined Hold no-op — the caller must not spend the ability's cooldown).
    std::uint32_t push_telegraph(Telegraph t) noexcept {
        t.id = next_telegraph_id_ + 1;
        if (telegraphs_.size() >= kMaxTelegraphs) {
            std::array<Telegraph, kMaxTelegraphs> live{};
            std::copy_n(telegraphs_.begin(), kMaxTelegraphs, live.begin());
            const int evict = telegraph_eviction_index(live, kMaxTelegraphs, t.tier);
            if (evict < 0) return 0;  // refused: the newest commit is not strictly higher tier
            telegraphs_[static_cast<std::size_t>(evict)] = t;
            ++next_telegraph_id_;
            return t.id;
        }
        telegraphs_.push_back(t);
        ++next_telegraph_id_;
        return t.id;
    }

    // --- creatures ---------------------------------------------------------------------------------
    void step_creatures(Rng& rng) noexcept {
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        std::uint32_t migrated = 0;

        for (std::size_t i = creatures_.size(); i-- > 0;) {
            Creature& c = creatures_[i];
            // The boss is a Creature so the player's verbs hit it, but its BRAIN is not the generic
            // creature AI: it never uses the flow field, never wanders, never migrates (it is clamped
            // to its room), and it decides through boss_policy. All of that is step_bosses' job — the
            // generic loop simply leaves it alone. (step_status still ticks its burn/shock/freeze, and
            // the player-verb handlers still strike it, exactly as for any creature.)
            if (c.kind == CreatureKind::kBoss) continue;
            // RFC-023: a civilian NPC's brain is step_npc's four-state FSM, not the monster/wildlife
            // AI below — no aggro, no flow field, no maul/attack. It still migrates across chunk
            // borders through the ordinary hand-off further down... except it never actually strays
            // that far (wander_radius tops out at 10 tiles, well inside one 32-tile chunk), so in
            // practice this `continue` is the only place its tick ends.
            if (creature_is_npc(c)) {
                step_npc(c, rng);
                continue;
            }
            const CreatureStats st = stats_of(c.kind);

            if (c.attack_cd > 0) --c.attack_cd;
            if (c.anger_ticks > 0 && --c.anger_ticks == 0) c.target = 0;
            // Stagger's terminal (Knockdown) is the ladder's flat stun — status_step (step_status,
            // called earlier this tick) already owns its countdown; this is only the gate.
            if (c.status.primary == Channel::kStagger && c.status.stage == 3) {
                c.windup = 0;  // a stun interrupts a committed swing — the telegraph is cancelled (F2)
                continue;      // a stunned creature does not move, turn or strike
            }

            // RFC-003 §5: a Displaced creature is mid-slide — it does not steer, turn or strike this
            // tick either, exactly like a committed wind-up or a Knockdown.
            if (c.kb_ticks_left > 0) {
                step_knockback(c);
                continue;
            }

            // A creature that has committed to a swing is frozen mid-telegraph (F2): it does not
            // steer, flee or re-acquire — it counts its wind-up down and the blow lands the tick the
            // counter reaches zero. This freeze IS the dodge window, so everything below (targeting,
            // movement, mauling) is skipped for a committed creature.
            if (c.windup > 0) {
                if (--c.windup == 0) resolve_windup(c);
                continue;
            }

            // Inside a smoke cloud a creature is blinded: it drops what it was chasing and cannot pick
            // up prey again while it stands there. It still wanders and still flees — the suppression
            // is of AGGRESSION, not of movement.
            const bool suppressed = !entities_.empty() && in_suppress_cloud(c.x, c.y);
            if (suppressed) {
                c.target = 0;
                c.anger_ticks = 0;
            }

            // A neutral animal that has not been touched still resents being crowded. Its personal
            // space is deliberately much smaller than a monster's aggro radius: you can walk past a
            // boar, you just cannot walk over one.
            const bool angry = c.anger_ticks > 0;
            if (!angry && !suppressed && c.disposition == Disposition::kNeutral) {
                if (const PlayerBeacon* p = nearest_player(c.x, c.y, st.aggro * 0.45f)) {
                    provoke(c, p->player, /*by_attack*/ false);
                }
            }

            const bool will_fight =
                !suppressed && (c.disposition == Disposition::kHostile || c.anger_ticks > 0);
            const PlayerBeacon* prey =
                will_fight ? nearest_player(c.x, c.y, st.aggro * (angry ? 1.8f : 1.0f)) : nullptr;
            const PlayerBeacon* threat =
                (c.disposition == Disposition::kTimid) ? nearest_player(c.x, c.y, st.aggro) : nullptr;

            float dx = 0.0f;
            float dy = 0.0f;
            if (prey != nullptr) {
                dx = prey->x - c.x;
                dy = prey->y - c.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist <= st.reach && c.attack_cd == 0 && c.damage > 0) {
                    commit_windup(c, *prey);
                    continue;  // it planted its feet — the wind-up begins, the blow lands a beat later
                }
            } else if (threat != nullptr) {
                dx = c.x - threat->x;  // straight away, and fast
                dy = c.y - threat->y;
            } else if (st.territory > 0.0f) {
                wander(c, st, rng, dx, dy);
            } else {
                // Hostile creatures with nobody to chase head for the nearest settlement. This is
                // the only consumer of the flow field, exactly as GAME.md §5 requires — wildlife
                // never touches it, which is what makes wildlife cheap enough to have a lot of.
                int fx = 0;
                int fy = 0;
                if (flow != nullptr && flow->ready() &&
                    flow->descend(static_cast<int>(c.x), static_cast<int>(c.y), fx, fy)) {
                    dx = static_cast<float>(fx);
                    dy = static_cast<float>(fy);
                } else {
                    dx = home_x - c.x;
                    dy = home_y - c.y;
                }
            }

            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.001f) {
                dx /= len;
                dy /= len;
            }
            // Small deterministic jitter so a wave does not collapse into a single-file line along
            // the field's steepest path.
            dx += (rng.unit() - 0.5f) * 0.35f;
            dy += (rng.unit() - 0.5f) * 0.35f;

            // RFC-004 §8.5: speed = base x status x scar, floored at kMinScarSpeed so stacked
            // partial slows never become a de-facto root — but Frozen (an explicit, intentional
            // root) is exempt from that floor, not composed with it.
            const float status_mult = speed_scale_of(c.status);
            float speed_mult = status_mult;
            if (status_mult > 0.001f) {
                const float scar_mult =
                    scar_speed_scale(scar_kind_at(static_cast<int>(c.x), static_cast<int>(c.y)));
                speed_mult = std::max(status_mult * scar_mult, kMinScarSpeed);
            }
            const float speed = st.speed * speed_mult;
            if (speed <= 0.001f) continue;  // frozen solid

            // Try the full step, then slide along each axis. Without the slide a creature that
            // meets water head-on stops forever, because its steering vector keeps pointing into
            // the obstacle; sliding lets it round the edge in a tick or two.
            const float step_x = dx * speed * dt;
            const float step_y = dy * speed * dt;
            float nx = c.x;
            float ny = c.y;
            if (passable(c.x + step_x, c.y + step_y)) {
                nx = c.x + step_x;
                ny = c.y + step_y;
            } else if (passable(c.x + step_x, c.y)) {
                nx = c.x + step_x;
            } else if (passable(c.x, c.y + step_y)) {
                ny = c.y + step_y;
            } else {
                // Every way forward is blocked. If a BUILDING is what is blocking it, break that
                // building — this is the entire reason a perimeter works. If a CombatEntity is
                // blocking instead, RFC-004 §7's blocked-repath counter takes over: only after
                // kBlockedRepathTicks consecutive refusals does it deal contact damage, mirroring
                // `attack_blocking_building`'s own shape rather than the wind-up/strike machinery (a
                // deliberate simplification of §7's "same machinery" language). Wildlife re-rolls its
                // wander instead of ever attacking a blocker.
                if (CombatEntity* blocker = blocking_entity(c.x, c.y, step_x, step_y)) {
                    if (st.faction == Faction::kWild) {
                        c.blocked_ticks = 0;
                        c.wander_cd = 0;
                    } else if (++c.blocked_ticks >= kBlockedRepathTicks &&
                              entity_def(blocker->kind).destroyable &&
                              stance_between(st.faction, blocker->team) == Stance::kHostile) {
                        strike_entity(*blocker, c.damage, Element::kNone, false);
                        c.blocked_ticks = 0;
                    }
                } else {
                    attack_blocking_building(c, step_x, step_y);
                }
                continue;  // terrain-boxed instead; the jittered heading will differ next tick
            }
            c.blocked_ticks = 0;

            // Facing is derived from the step actually taken, not the step intended — a creature
            // sliding along a wall should face where it is going.
            c.facing = facing_of(nx - c.x, ny - c.y);
            c.x = nx;
            c.y = ny;

            // A raid crossing a forest kills the deer in it (GAME.md §5). Checked after moving and
            // only for monsters, so it costs one short loop over a chunk's own creature list and
            // nothing at all in a chunk with no monsters in it.
            if (st.faction == Faction::kMonster && c.attack_cd == 0) maul_wildlife(c, st);

            // ---- the hand-off ----------------------------------------------------------------
            const ChunkCoord owner = chunk_of(coord.map, c.x, c.y);
            if (owner == coord) continue;
            if (router != nullptr) {
                router->get<ChunkActor>(chunk_key(owner)).tell(CreatureEnter{c});
                // RFC-019 §5.8: the ledger travels too, as a second small message right behind
                // CreatureEnter (per-(sender,receiver) FIFO — arrives in order, right after).
                const auto lit = ledgers_.find(c.id);
                if (lit != ledgers_.end()) {
                    CreatureContribEnter ce{};
                    ce.creature_id = c.id;
                    for (std::size_t k = 0; k < kMaxContributors; ++k) ce.entries[k] = lit->second[k];
                    router->get<ChunkActor>(chunk_key(owner)).tell(ce);
                }
            }
            ledgers_.erase(c.id);
            creatures_.erase(creatures_.begin() + static_cast<std::ptrdiff_t>(i));
            ++migrated;
        }

        if (migrated != 0 && status != nullptr) {
            status->migrations.fetch_add(migrated, std::memory_order_relaxed);
        }
    }

    // --- RFC-023: civilian NPCs -----------------------------------------------------------------
    // Walk one step toward (tx,ty), sliding along an axis when blocked head-on (the same fallback
    // step_creatures' own movement uses). Returns true once close enough to call it arrived. Slower
    // than wildlife's pace on purpose — a village should read as unhurried, not skittish.
    [[nodiscard]] bool step_npc_toward(Creature& c, float tx, float ty) noexcept {
        const float dx = tx - c.x;
        const float dy = ty - c.y;
        const float dist2 = dx * dx + dy * dy;
        if (dist2 < 0.04f) {  // ~0.2 tile: close enough, and avoids ever dividing by a near-zero len
            c.x = tx;
            c.y = ty;
            return true;
        }
        const float len = std::sqrt(dist2);
        const float ux = dx / len;
        const float uy = dy / len;
        constexpr float kNpcSpeed = 1.6f;  // tiles/second
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        const float step_x = ux * kNpcSpeed * dt;
        const float step_y = uy * kNpcSpeed * dt;
        float nx = c.x;
        float ny = c.y;
        if (passable(c.x + step_x, c.y + step_y)) {
            nx = c.x + step_x;
            ny = c.y + step_y;
        } else if (passable(c.x + step_x, c.y)) {
            nx = c.x + step_x;
        } else if (passable(c.x, c.y + step_y)) {
            ny = c.y + step_y;
        }
        c.facing = facing_of(nx - c.x, ny - c.y);
        c.x = nx;
        c.y = ny;
        return false;
    }

    // Picks a fresh waypoint offset from home, reusing `wander_dx/wander_dy` (Creature's existing
    // wildlife-wander fields — a relative tile offset either way, exactly what a civilian needs too)
    // and clamped so it can never fall outside this chunk. NPCs do not migrate (see step_creatures'
    // note at its call site) — clamping the pick is what makes that true rather than merely usual,
    // since a house near a chunk border could otherwise place a waypoint one tile over the line.
    void npc_pick_waypoint(Creature& c, NpcRole role, Rng& rng) noexcept {
        const int r = static_cast<int>(npc_wander_radius(role));
        if (r <= 0) {
            c.wander_dx = 0;
            c.wander_dy = 0;
            return;
        }
        const int cx0 = coord.cx * kChunkTiles;
        const int cy0 = coord.cy * kChunkTiles;
        const int lo_x = cx0 - static_cast<int>(c.home_tx);
        const int hi_x = cx0 + kChunkTiles - 1 - static_cast<int>(c.home_tx);
        const int lo_y = cy0 - static_cast<int>(c.home_ty);
        const int hi_y = cy0 + kChunkTiles - 1 - static_cast<int>(c.home_ty);
        const int rx = std::clamp(r, 0, std::min(-lo_x, hi_x));
        const int ry = std::clamp(r, 0, std::min(-lo_y, hi_y));
        c.wander_dx = static_cast<std::int8_t>(
            rx <= 0 ? 0 : static_cast<int>(rng.below(static_cast<std::uint32_t>(2 * rx + 1))) - rx);
        c.wander_dy = static_cast<std::int8_t>(
            ry <= 0 ? 0 : static_cast<int>(rng.below(static_cast<std::uint32_t>(2 * ry + 1))) - ry);
    }

    // The civilian FSM (RFC-023 §7). Threat detection is a same-chunk proximity scan, not a beacon
    // subscription: a raid monster must already be in this NPC's own chunk (having migrated in via
    // the ordinary creature hand-off) before it can threaten anyone in it, so there is nothing to
    // replicate cross-chunk that step_creatures' existing migration does not already provide for
    // free. See npc.hpp's kNpcThreatRadius note on why this substitutes for the RFC's primary
    // (unshipped) raid-warning trigger rather than merely approximating it.
    void step_npc(Creature& c, Rng& rng) noexcept {
        const NpcRole role = npc_role_of(c);
        bool threatened = false;
        for (const Creature& other : creatures_) {
            if (other.id == c.id || other.hp <= 0) continue;
            if (stats_of(other.kind).faction != Faction::kMonster) continue;
            const float dx = other.x - c.x;
            const float dy = other.y - c.y;
            if (dx * dx + dy * dy <= kNpcThreatRadius * kNpcThreatRadius) {
                threatened = true;
                break;
            }
        }

        NpcState state = npc_state_of(c);
        if (threatened && state != NpcState::kSheltering) {
            npc_set_state(c, NpcState::kSheltering);
            state = NpcState::kSheltering;
        }

        if (state == NpcState::kSheltering) {
            // wander_cd doubles as "ticks since a threat was last actually seen" here — reusing the
            // same GAME.md §5 cooldown (kAngerTicks, ~20s at 10 Hz) Wild fauna already uses to drop a
            // grudge, per RFC-023 §7's own citation of that constant for this exact purpose.
            c.wander_cd = threatened ? kAngerTicks : (c.wander_cd > 0 ? c.wander_cd - 1 : 0);
            (void)step_npc_toward(c, static_cast<float>(c.home_tx) + 0.5f,
                                  static_cast<float>(c.home_ty) + 0.5f);
            if (!threatened && c.wander_cd == 0) npc_set_state(c, NpcState::kIdle);
            return;
        }

        if (npc_wander_radius(role) <= 0.0f) return;  // stationary role: stands at its door, always

        switch (state) {
            case NpcState::kIdle:
                npc_pick_waypoint(c, role, rng);
                npc_set_state(c, NpcState::kMoveToWaypoint);
                break;
            case NpcState::kMoveToWaypoint: {
                const float tx = static_cast<float>(c.home_tx) + 0.5f + static_cast<float>(c.wander_dx);
                const float ty = static_cast<float>(c.home_ty) + 0.5f + static_cast<float>(c.wander_dy);
                if (step_npc_toward(c, tx, ty)) {
                    if (npc_has_work_pose(role)) {
                        c.wander_cd = npc_dwell_ticks(role, rng);
                        npc_set_state(c, NpcState::kWorkAction);
                    } else {
                        npc_set_state(c, NpcState::kIdle);
                    }
                }
                break;
            }
            case NpcState::kWorkAction:
                if (c.wander_cd > 0) {
                    --c.wander_cd;
                } else {
                    npc_set_state(c, NpcState::kIdle);
                }
                break;
            case NpcState::kSheltering:
                break;  // handled above; unreachable here
        }
    }

    // Wildlife steering: pick a heading, hold it for a while, and turn for home when it strays too
    // far. Holding the heading is what stops an animal from vibrating in place — a fresh random
    // direction every tick averages to standing still.
    void wander(Creature& c, const CreatureStats& st, Rng& rng, float& dx, float& dy) noexcept {
        const float hx = static_cast<float>(c.home_tx) + 0.5f;
        const float hy = static_cast<float>(c.home_ty) + 0.5f;
        const float ax = hx - c.x;
        const float ay = hy - c.y;
        if (ax * ax + ay * ay > st.territory * st.territory) {
            dx = ax;
            dy = ay;
            c.wander_cd = 0;
            return;
        }
        if (c.wander_cd == 0) {
            c.wander_cd = static_cast<std::uint8_t>(15 + rng.below(35));
            c.wander_dx = static_cast<std::int8_t>(static_cast<int>(rng.below(3)) - 1);
            c.wander_dy = static_cast<std::int8_t>(static_cast<int>(rng.below(3)) - 1);
        } else {
            --c.wander_cd;
        }
        dx = static_cast<float>(c.wander_dx);
        dy = static_cast<float>(c.wander_dy);
    }

    // Commit to a swing (F2). Freeze mid-telegraph, remember WHO it swung at and WHERE they stood,
    // and throw the puff the renderer reads as "about to hit". No damage lands here — `resolve_windup`
    // lands it `windup` ticks later, and the player has those ticks to step out of reach. This is a
    // straight buff to player agency: the per-species damage is unchanged, only the timing is.
    void commit_windup(Creature& c, const PlayerBeacon& prey) noexcept {
        c.facing = facing_of(prey.x - c.x, prey.y - c.y);
        c.windup = stats_of(c.kind).windup;
        c.windup_target = prey.player;
        c.windup_x = prey.x;
        c.windup_y = prey.y;
        add_effect(c.x, c.y, EffectKind::kSmoke);  // the tell: a puff at its feet as it plants them
    }

    // The committed blow resolves (F2). Look the target up again by key — a swing is aimed at the
    // player it was committed against, not at whoever is now closest. Still within a hair over reach
    // (reach*1.15 — a grace so a real dodge earns the miss but pixel-perfect edging does not) and it
    // connects: HurtPlayer plus a slash ON the player. Slipped out of the window and it whiffs: a
    // slash on the empty spot it aimed at, because a miss the player SEES is the reward for dodging.
    // Either way the recover cooldown starts, so a dodged swing still costs the creature its beat.
    void resolve_windup(Creature& c) noexcept {
        c.attack_cd = kStrikeCooldown;
        const std::uint64_t target = c.windup_target;
        c.windup_target = 0;
        const float grace = stats_of(c.kind).reach * 1.15f;
        if (const PlayerBeacon* p = beacon_of(target)) {
            const float dx = p->x - c.x;
            const float dy = p->y - c.y;
            if (dx * dx + dy * dy <= grace * grace) {
                if (router != nullptr) {
                    router->get<PlayerActor>(target).tell(HurtPlayer{c.damage, c.id});
                }
                add_effect(p->x, p->y, EffectKind::kSlash);  // the blow, on the player
                return;
            }
        }
        add_effect(c.windup_x, c.windup_y, EffectKind::kSlash);  // a whiff, where the player was
    }

    // The beacon for one SPECIFIC player, or null. `nearest_player` settles for whoever is closest;
    // resolving a committed swing must re-check the exact target it was aimed at, even if another
    // player has since stepped nearer.
    [[nodiscard]] const PlayerBeacon* beacon_of(std::uint64_t player) const noexcept {
        if (player == 0) return nullptr;
        for (const PlayerBeacon& p : players_) {
            if (p.player == player && p.hp > 0) return &p;
        }
        return nullptr;
    }

    // A monster hits whatever wildlife it is standing next to. Deliberately not a targeting system:
    // monsters do not hunt animals, they just kill what is in the way, which is what makes an
    // untended stronghold read as a blight on the map rather than as a predator simulation.
    void maul_wildlife(Creature& attacker, const CreatureStats& st) noexcept {
        for (Creature& other : creatures_) {
            if (other.id == attacker.id || other.hp <= 0) continue;
            // RFC-023 §7: a Sheltering civilian is untargetable — the same guarantee strike() gives
            // the player's own verbs, extended here since maul_wildlife hits HP directly and never
            // goes through strike() at all.
            if (creature_is_npc(other) && npc_state_of(other) == NpcState::kSheltering) continue;
            // faction_of(), not stats_of(other.kind).faction directly: an NPC's faction is kVillager
            // regardless of the placeholder CreatureKind it carries (npc.hpp) — this is the one line
            // that makes "a raid crossing a forest kills the deer in it" (GAME.md §5) also true of a
            // village's own people, exactly as RFC-023 §7 says it must be.
            if (stance_between(st.faction, faction_of(other)) != Stance::kHostile) continue;
            const float dx = other.x - attacker.x;
            const float dy = other.y - attacker.y;
            if (dx * dx + dy * dy > st.reach * st.reach) continue;
            other.hp = static_cast<std::int16_t>(other.hp - attacker.damage);
            attacker.attack_cd = kStrikeCooldown;
            return;
        }
    }

    // --- the boss (F3) ----------------------------------------------------------------------------
    // The boss body is a Creature (kBoss) in `creatures_`, so the player's verbs, combos, status and
    // stun all reach it through the ordinary handlers. What runs here is only the parts that are NOT
    // a normal creature: the scripted brain, the committed charge dash, the room clamp, the leash and
    // the respawn. All of it drives the SAME wind-up/strike machinery a creature uses (Creature::
    // windup for the telegraph, HurtPlayer for the blow), so a boss reads exactly like a big creature.
    // RFC-005: every dojo boss in this pass is the one real kit (see boss_kit.hpp's header note on
    // why a second kit/phase is not modeled yet). `step_boss_ai`/`spawn_boss`/`boss_commit`/
    // `boss_resolve`/`boss_dash` read their numbers from here instead of `boss.hpp`'s scattered
    // constants directly — this is the literal sense in which the shipped machinery is "generalized
    // into data": the FSM below is kit-driven, even though only one kit exists to drive it.
    static constexpr BossKitDef kSamuraiKit = samurai_red_kit();

    void step_bosses() noexcept {
        if (bosses_.empty()) return;
        Rng rng(chunk_key(coord) * 0xB055'0F17'11EEull + tick_);
        for (BossState& b : bosses_) {
            Creature* body = find_creature(b.body);
            if (b.alive && (body == nullptr || body->hp <= 0)) {
                // Killed (reaped this tick, or a DoT took the last point): begin the respawn wait.
                b.alive = false;
                b.body = 0;
                b.respawn_timer = kSamuraiKit.respawn_ticks;
                continue;
            }
            if (!b.alive) {
                if (b.respawn_timer > 0) {
                    --b.respawn_timer;
                    continue;
                }
                spawn_boss(b);
                continue;
            }
            step_boss_ai(*body, b, rng);
        }
    }

    // Create (or recreate) a boss body at its spawn tile. Stats are the FLAT design numbers, NOT run
    // through make_creature's ring scaling — a dojo boss is 700 HP wherever on the interior map its
    // room happens to fall, which is the whole point of a scripted set-piece over an ambient monster.
    void spawn_boss(BossState& b) noexcept {
        Creature c{};
        c.id = ++next_id_ | (static_cast<std::uint32_t>(chunk_key(coord)) << 12);
        c.x = static_cast<float>(b.spawn_tx) + 0.5f;
        c.y = static_cast<float>(b.spawn_ty) + 0.5f;
        c.max_hp = kSamuraiKit.base_hp;
        c.hp = kSamuraiKit.base_hp;
        c.damage = kSamuraiKit.abilities[0].damage;
        c.kind = CreatureKind::kBoss;
        c.disposition = Disposition::kHostile;
        const DefenderProfile def = defender_of(CreatureKind::kBoss);
        c.material = def.material;
        c.tier = def.tier;
        c.toughness = tier_toughness(def.tier);
        c.facing = Facing::kDown;
        c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
        creatures_.push_back(c);
        b.body = c.id;
        b.alive = true;
        b.respawn_timer = 0;
        b.no_target = 0;
        b.charge_cd = 0;
        b.charging = 0;
        b.winding_charge = false;
        b.dash_hit = false;
        b.telegraph_id = 0;  // a fresh body owns no leftover telegraph (e.g. one orphaned by a mid-
                              // windup death — reap_dead never resolves/erases it, this does)
    }

    [[nodiscard]] Creature* find_creature(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        for (Creature& c : creatures_) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    void step_boss_ai(Creature& c, BossState& b, Rng& rng) noexcept {
        (void)rng;
        if (b.charge_cd > 0) --b.charge_cd;
        if (c.attack_cd > 0) --c.attack_cd;

        // Stun cancels a committed wind-up or dash, exactly as it does for any creature (F2). RFC-006
        // T4: the telegraph does not simply vanish — it FIZZLEs (a 2-tick grey collapse, no impact
        // FX), so the player can tell "I interrupted it" from "it fired and missed".
        if (c.status.primary == Channel::kStagger && c.status.stage == 3) {
            if (c.windup > 0 || b.charging > 0) {
                if (Telegraph* t = find_telegraph(b.telegraph_id)) {
                    t->flags = static_cast<std::uint8_t>(t->flags | kTelegraphFizzling);
                    t->left = kFizzleTicks;
                }
            }
            c.windup = 0;
            c.windup_target = 0;
            b.charging = 0;
            b.winding_charge = false;
            b.telegraph_id = 0;
            c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
            return;
        }

        // A committed wind-up: freeze and telegraph (Creature::windup drives F2's red pulse + smoke,
        // and now the real RFC-006 Telegraph record ticks in lockstep), and resolve the tick it
        // reaches zero — an attack lands, a charge begins its dash.
        if (c.windup > 0) {
            c.boss_pose = static_cast<std::uint8_t>(b.winding_charge ? BossPose::kCharge
                                                                     : BossPose::kAttack);
            --c.windup;
            if (Telegraph* t = find_telegraph(b.telegraph_id)) t->left = c.windup;
            if (c.windup == 0) boss_resolve(c, b);
            return;
        }

        // A committed dash: fly toward the point the target stood at commit, damaging on contact,
        // wall-clamped to the room.
        if (b.charging > 0) {
            boss_dash(c, b);
            return;
        }

        // Idle: build the observation and let the policy decide. THIS is the RL seam — the chunk
        // gathers the obs, calls boss_policy, and executes the action through the same machinery
        // below; F4 swaps the policy body and none of this moves.
        const PlayerBeacon* target = boss_target(b);
        if (target == nullptr) {
            // Nobody in the room. Drift back to the post, and after the leash window reset to full —
            // fleeing through the door "resets the boss", so a player cannot chip it down across trips.
            c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
            if (++b.no_target >= kSamuraiKit.leash_ticks) {
                boss_reset(c, b);
            } else {
                const float sx = static_cast<float>(b.spawn_tx) + 0.5f;
                const float sy = static_cast<float>(b.spawn_ty) + 0.5f;
                if (std::abs(sx - c.x) > 0.2f || std::abs(sy - c.y) > 0.2f) {
                    boss_move(c, b, sx, sy, kSamuraiKit.approach_speed);
                    c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                }
            }
            return;
        }
        b.no_target = 0;

        BossObs o{};
        o.dx = static_cast<std::int16_t>(std::lround(target->x - c.x));
        o.dy = static_cast<std::int16_t>(std::lround(target->y - c.y));
        o.hp_frac = static_cast<std::uint16_t>(c.max_hp > 0 ? (c.hp * 1000) / c.max_hp : 0);
        o.attack_cd = c.attack_cd;
        o.charge_cd = b.charge_cd;
        o.winding_up = c.windup > 0;
        const BossAction a = boss_policy(o);

        // RFC-007: the RL observation is assembled every decision this pass makes, at the same
        // cadence a real trainer would consume it — the result is currently unread (see
        // rl_obs.hpp's header note: no trainer is vendored into this repo to read it), proving the
        // encoder is live and crash-safe against real chunk state rather than merely compiling.
        (void)build_boss_obs(c, b, target);

        // Execute through the RFC-007 §3 action space, not the old 5-case switch — this IS the RL
        // seam: `boss_policy` still emits the legacy 5-action `BossAction` (RFC-005's own decision to
        // leave the gen-0 script untouched), translated here into the 15-id space a future network
        // would emit directly. `execute_rl_action` is total — every one of the 15 ids is safe to pass
        // it, even the 11 gen-0 never produces.
        execute_rl_action(to_rl_action(a.kind), c, b, target);
    }

    // §3's total-function executor: every id in `RlActionId` is safe here, in every FSM state this
    // is ever called from (idle, not committed — `step_boss_ai` only reaches this branch when
    // `c.windup == 0 && b.charging == 0`, so the "promise rule" of §3.1 is already satisfied by the
    // caller; this function does not need to re-check it). Dead/cooling Cast slots and Lead's
    // missing velocity data (see rl_obs.hpp's header note) degrade to safe, documented fallbacks —
    // never a crash, never silent wrong behavior.
    void execute_rl_action(RlActionId a, Creature& c, BossState& b, const PlayerBeacon* target) noexcept {
        switch (a) {
            case RlActionId::kHold:
                if (target != nullptr) c.facing = facing_of(target->x - c.x, target->y - c.y);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
                return;
            case RlActionId::kStepN:
                boss_step(c, b, 0.0f, -1.0f, kSamuraiKit.approach_speed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                return;
            case RlActionId::kStepE:
                boss_step(c, b, 1.0f, 0.0f, kSamuraiKit.approach_speed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                return;
            case RlActionId::kStepS:
                boss_step(c, b, 0.0f, 1.0f, kSamuraiKit.approach_speed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                return;
            case RlActionId::kStepW:
                boss_step(c, b, -1.0f, 0.0f, kSamuraiKit.approach_speed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                return;
            case RlActionId::kApproach:
                if (target == nullptr) { c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle); return; }
                boss_move(c, b, target->x, target->y, kSamuraiKit.approach_speed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                return;
            case RlActionId::kRetreat: {
                if (target == nullptr) { c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle); return; }
                float dx = c.x - target->x;
                float dy = c.y - target->y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.01f) { dx /= len; dy /= len; } else { dx = facing_dx(c.facing); dy = facing_dy(c.facing); }
                boss_step(c, b, dx, dy, kSamuraiKit.approach_speed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                return;
            }
            case RlActionId::kCastSlot0Direct:
            case RlActionId::kCastSlot1Direct:
            case RlActionId::kCastSlot2Direct:
            case RlActionId::kCastSlot3Direct:
            case RlActionId::kCastSlot0Lead:
            case RlActionId::kCastSlot1Lead:
            case RlActionId::kCastSlot2Lead:
            case RlActionId::kCastSlot3Lead: {
                // Direct and Lead currently commit to the same point (see rl_obs.hpp's header note:
                // no target-velocity state exists to extrapolate Lead's aim with) — both total-
                // function-safe, Lead just isn't a distinct behavior yet.
                const int slot = (a >= RlActionId::kCastSlot0Lead)
                                     ? static_cast<int>(a) - static_cast<int>(RlActionId::kCastSlot0Lead)
                                     : static_cast<int>(a) - static_cast<int>(RlActionId::kCastSlot0Direct);
                if (target == nullptr || slot >= kSamuraiKit.ability_count) {
                    c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
                    return;  // dead slot: §3.2's mandatory Hold coercion
                }
                const std::uint16_t cd = (slot == 0) ? c.attack_cd : b.charge_cd;
                if (cd > 0) {
                    c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
                    return;  // cooling slot: §3.2's mandatory Hold coercion
                }
                c.facing = (target->x < c.x) ? Facing::kLeft : Facing::kRight;
                boss_commit(c, b, *target, /*charge*/ slot == 1);
                return;
            }
            case RlActionId::kCount: break;
        }
    }

    // One tick of movement along a fixed unit heading (as opposed to `boss_move`'s seek-a-point
    // shape) — the RFC-007 Step N/E/S/W actions.
    void boss_step(Creature& c, BossState& b, float ux, float uy, float speed) noexcept {
        const float sp = speed * speed_scale_of(c.status);
        if (sp <= 0.001f) return;
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        float nx = c.x + ux * sp * dt;
        float ny = c.y + uy * sp * dt;
        (void)clamp_to_room(b, nx, ny);
        c.facing = facing_of(nx - c.x, ny - c.y);
        c.x = nx;
        c.y = ny;
    }

    // RFC-007 §2: assemble the 120-float observation for `c`/`b` against CURRENT chunk state only
    // (§2.8's statelessness rule — no history is stored anywhere for this). `target`/`target2` are
    // the nearest and second-nearest living players in the boss's own room. See rl_obs.hpp's header
    // note for exactly which blocks this engine can genuinely source today (Self, Ray, Ground, and
    // Entity are real; several Target/Secondary-target floats are honestly left at 0 for lack of a
    // cross-trust-tier data source, never fabricated).
    [[nodiscard]] RlObs build_boss_obs(const Creature& c, const BossState& b,
                                       const PlayerBeacon* target) const noexcept {
        RlObs obs{};
        float* v = obs.v.data();

        // --- Block S: self, idx 0-22 -----------------------------------------------------------
        v[0] = obs_frac(static_cast<float>(c.hp), static_cast<float>(c.max_hp));
        if (c.status.primary != Channel::kNone) {
            v[1 + gauge_index_of(c.status.primary)] = 1.0f;
            v[6] = static_cast<float>(c.status.stage) / 3.0f;
            v[7] = std::min(1.0f, static_cast<float>(c.status.stage_ticks) / 80.0f);
        }
        if ((c.status.coatings & (1u << static_cast<std::uint8_t>(Coating::kWet))) != 0) v[8] = 1.0f;
        // idx 9 reserved (RFC-002 OQ5 — only Wet is a real coating in v1).

        // Own pipeline phase (idx 10-13: Idle/Windup/Active/Recover), SYNTHESIZED — no single stored
        // phase enum exists on Creature/BossState (see header note). Active has no ticks-remaining
        // source of its own (RFC-005's `active` field is declared, not a runtime sub-state), so it is
        // detected only as "mid-dash"; a resolving attack is instantaneous and never observed mid-Active.
        if (c.windup > 0) {
            v[11] = 1.0f;  // Windup
            v[14] = 1.0f - static_cast<float>(c.windup) /
                               static_cast<float>(b.winding_charge ? kSamuraiKit.abilities[1].windup
                                                                    : kSamuraiKit.abilities[0].windup);
        } else if (b.charging > 0) {
            v[12] = 1.0f;  // Active (the committed dash)
            v[14] = 1.0f - static_cast<float>(b.charging) /
                               static_cast<float>(kSamuraiKit.abilities[1].active);
        } else if (c.attack_cd > 0 || b.charge_cd > 0) {
            v[13] = 1.0f;  // Recover
        } else {
            v[10] = 1.0f;  // Idle
        }

        // Ability cooldown fractions, slots 0-3 (idx 15-18) — 0 when ready OR slot absent, per §2's
        // own convention (a padded slot reads identically to a ready one, both harmless to a policy).
        if (kSamuraiKit.ability_count > 0) {
            v[15] = obs_frac(static_cast<float>(c.attack_cd),
                             static_cast<float>(kSamuraiKit.abilities[0].cooldown));
        }
        if (kSamuraiKit.ability_count > 1) {
            v[16] = obs_frac(static_cast<float>(b.charge_cd),
                             static_cast<float>(kSamuraiKit.abilities[1].cooldown));
        }

        switch (c.facing) {
            case Facing::kDown: v[19] = 1.0f; break;
            case Facing::kUp: v[20] = 1.0f; break;
            case Facing::kLeft: v[21] = 1.0f; break;
            case Facing::kRight: v[22] = 1.0f; break;
        }

        // --- Block T: primary target, idx 23-42 ------------------------------------------------
        if (target != nullptr) {
            v[23] = 1.0f;  // a live beacon this tick is maximally fresh; no staleness model here
            v[24] = obs_offset(target->x - c.x, kObsRange);
            v[25] = obs_offset(target->y - c.y, kObsRange);
            // idx 26-27 (velocity), 29-37 (status), 38-42 (pipeline phase/progress): left at 0 — see
            // rl_obs.hpp's header note. A chunk cannot see a player's own status/pipeline (both live
            // on the trusted PlayerActor) or compute velocity without stored history this pass adds
            // none of.
            v[28] = obs_frac(static_cast<float>(target->hp), static_cast<float>(kPlayerMaxHp));
        }

        // --- Block T2: secondary target, idx 43-48 ----------------------------------------------
        const PlayerBeacon* target2 = nullptr;
        float best_d2 = 1e18f;
        for (const PlayerBeacon& p : players_) {
            if (p.hp <= 0 || !in_room(b, p)) continue;
            if (target != nullptr && p.player == target->player) continue;
            const float dx = p.x - c.x;
            const float dy = p.y - c.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; target2 = &p; }
        }
        if (target2 != nullptr) {
            v[43] = 1.0f;
            v[44] = obs_offset(target2->x - c.x, kObsRange);
            v[45] = obs_offset(target2->y - c.y, kObsRange);
            v[46] = obs_frac(static_cast<float>(target2->hp), static_cast<float>(kPlayerMaxHp));
            // idx 47 (winding-up flag): left at 0, same cross-trust-tier gap as Block T.
        }

        // --- Block R: 8 compass rays, idx 49-64 -------------------------------------------------
        // A coarse whole-tile walk (see rl_obs.hpp's header note: no DDA/Bresenham helper exists in
        // this engine to reuse).
        static constexpr float kRayDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        static constexpr float kRayDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
        for (int ray = 0; ray < 8; ++ray) {
            float blocked_c = 0.0f;
            float hazard_c = 0.0f;
            bool blocked_found = false;
            bool hazard_found = false;
            for (int t = 1; t <= static_cast<int>(kObsRange) && !(blocked_found && hazard_found); ++t) {
                const auto tx = static_cast<int>(c.x + kRayDx[ray] * static_cast<float>(t));
                const auto ty = static_cast<int>(c.y + kRayDy[ray] * static_cast<float>(t));
                if (!blocked_found && !is_walkable(terrain_at(tx, ty))) {
                    blocked_c = obs_closeness(kObsRange, static_cast<float>(t));
                    blocked_found = true;
                }
                if (!hazard_found) {
                    Surface s{};
                    const bool burning = surface_at(tx, ty, s) && s == Surface::kBurning;
                    bool entity_hazard = false;
                    for (const CombatEntity& e : entities_) {
                        if (entity_def(e.kind).obs_class != ObsClass::kHazardZone) continue;
                        const float edx = static_cast<float>(tx) + 0.5f - e.x;
                        const float edy = static_cast<float>(ty) + 0.5f - e.y;
                        if (edx * edx + edy * edy <= e.radius * e.radius) { entity_hazard = true; break; }
                    }
                    if (burning || entity_hazard) {
                        hazard_c = obs_closeness(kObsRange, static_cast<float>(t));
                        hazard_found = true;
                    }
                }
            }
            v[kObsBlockR + 2 * ray] = blocked_c;
            v[kObsBlockR + 2 * ray + 1] = hazard_c;
        }

        // --- Block G: ground & confinement, idx 65-70 -------------------------------------------
        const auto sx = static_cast<int>(c.x);
        const auto sy = static_cast<int>(c.y);
        const TerrainPhys tphys = terrain_phys(terrain_at(sx, sy), scar_kind_at(sx, sy));
        Surface self_surf{};
        const bool has_self_surf = surface_at(sx, sy, self_surf);
        if (tphys.conductivity >= 50 || (has_self_surf && self_surf == Surface::kIced)) {
            v[65 + 2] = 1.0f;  // Conductive
        } else if (tphys.friction >= 85 || (has_self_surf && self_surf == Surface::kMudded) ||
                  scar_kind_at(sx, sy) == ScarKind::kRubble || scar_kind_at(sx, sy) == ScarKind::kCrater) {
            v[65 + 1] = 1.0f;  // Slow
        } else if (scar_kind_at(sx, sy) == ScarKind::kCracked) {
            v[65 + 3] = 1.0f;  // Unstable
        } else {
            v[65 + 0] = 1.0f;  // Normal
        }
        bool in_hazard = false;
        for (const CombatEntity& e : entities_) {
            if (entity_def(e.kind).obs_class != ObsClass::kHazardZone) continue;
            const float edx = c.x - e.x;
            const float edy = c.y - e.y;
            if (edx * edx + edy * edy <= e.radius * e.radius) { in_hazard = true; break; }
        }
        v[69] = in_hazard ? 1.0f : 0.0f;
        int wall_d = 5;
        for (int r = 0; r <= 4 && wall_d > 4; ++r) {
            for (int dy = -r; dy <= r && wall_d > 4; ++dy) {
                for (int dx = -r; dx <= r && wall_d > 4; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    if (!is_walkable(terrain_at(sx + dx, sy + dy))) wall_d = r;
                }
            }
        }
        v[70] = obs_closeness(4.0f, static_cast<float>(wall_d));

        // --- Block E: 3 nearest observable CombatEntity slots, idx 71-106 ----------------------
        std::array<const CombatEntity*, 3> slots{nullptr, nullptr, nullptr};
        std::array<float, 3> slot_d2{1e18f, 1e18f, 1e18f};
        for (const CombatEntity& e : entities_) {
            const EntityDef def = entity_def(e.kind);
            if (!def.observable) continue;
            const float dx = e.x - c.x;
            const float dy = e.y - c.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > kObsRange * kObsRange) continue;
            for (int i = 0; i < 3; ++i) {
                if (d2 < slot_d2[static_cast<std::size_t>(i)]) {
                    for (int j = 2; j > i; --j) {
                        slot_d2[static_cast<std::size_t>(j)] = slot_d2[static_cast<std::size_t>(j - 1)];
                        slots[static_cast<std::size_t>(j)] = slots[static_cast<std::size_t>(j - 1)];
                    }
                    slot_d2[static_cast<std::size_t>(i)] = d2;
                    slots[static_cast<std::size_t>(i)] = &e;
                    break;
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            const CombatEntity* e = slots[static_cast<std::size_t>(k)];
            if (e == nullptr) continue;
            const std::size_t base = kObsBlockE + 12 * static_cast<std::size_t>(k);
            v[base + 0] = 1.0f;
            v[base + 1] = obs_offset(e->x - c.x, kObsRange);
            v[base + 2] = obs_offset(e->y - c.y, kObsRange);
            const EntityDef def = entity_def(e->kind);
            v[base + 3 + static_cast<std::size_t>(def.obs_class)] = 1.0f;
            // Element one-hot (+7..+10): derived from the entity's aura channel where it has an
            // aura-emitting kind — EntityDef carries no standalone `element` field (RFC-004 never
            // added one), so a barrier-class entity with no aura (e.g. kIceWall) reads elementless
            // here despite its name. A real, named gap, not a bug in this encoder.
            if (def.aura_kind == AuraKind::kChannel) {
                switch (def.aura_channel) {
                    case Channel::kHeat: v[base + 7] = 1.0f; break;
                    case Channel::kCold: v[base + 8] = 1.0f; break;
                    case Channel::kEarth: v[base + 9] = 1.0f; break;
                    case Channel::kShock: v[base + 10] = 1.0f; break;
                    default: break;
                }
            }
            v[base + 11] = obs_frac(static_cast<float>(e->expire_tick > tick_ ? e->expire_tick - tick_ : 0),
                                    100.0f);
        }

        // --- Reserved, idx 107-119: always 0 in v1 ----------------------------------------------
        return obs;
    }

    // Commit to a telegraphed action. Freeze for the wind-up (the biggest telegraph in the game),
    // remember WHO and WHERE, and throw the smoke puff F2 reads as "incoming". No damage here — it
    // lands (or the dash begins) when the counter reaches zero.
    void boss_commit(Creature& c, BossState& b, const PlayerBeacon& prey, bool charge) noexcept {
        const BossAbilityKit& ability = kSamuraiKit.abilities[charge ? 1 : 0];
        c.windup = ability.windup;
        c.windup_target = prey.player;
        c.windup_x = prey.x;
        c.windup_y = prey.y;
        b.winding_charge = charge;
        c.boss_pose = static_cast<std::uint8_t>(charge ? BossPose::kCharge : BossPose::kAttack);
        add_effect(c.x, c.y, EffectKind::kSmoke);

        // RFC-006 §2/T2: the telegraph's geometry is frozen at commit, from the same committed aim
        // point (`prey.x/y`) `boss_resolve`'s grace check and `boss_dash`'s heading already use — one
        // replicated record, no second source of truth. Cone (cleave) points at the committed target;
        // Line (charge) runs from the boss to the committed point.
        Telegraph t{};
        t.element = Element::kNone;  // both abilities are plain physical blows (§1.2 "Physical")
        t.tier = ability.telegraph_tier;
        t.total = static_cast<std::uint8_t>(ability.windup);
        t.left = static_cast<std::uint8_t>(ability.windup);
        t.x = c.x;
        t.y = c.y;
        float dx = prey.x - c.x;
        float dy = prey.y - c.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.01f) { dx /= len; dy /= len; } else { dx = facing_dx(c.facing); dy = facing_dy(c.facing); }
        if (charge) {
            t.shape = TelegraphShape::kLine;
            t.ex = prey.x;
            t.ey = prey.y;
            t.radius = std::max(1.0f, ability.shape_radius * 0.5f);  // R1: line width floor, 1.0 tile
        } else {
            t.shape = TelegraphShape::kCone;
            t.ex = dx;
            t.ey = dy;
            t.radius = ability.shape_radius;
            t.arc_deg_half = static_cast<std::uint8_t>(ability.shape_arc_deg / 2.0f);
        }
        b.telegraph_id = push_telegraph(t);
    }

    // A committed wind-up reaches zero. A charge begins its dash toward the aimed-at spot; an attack
    // lands on the committed player if they are still within a hair over reach, and whiffs a visible
    // slash on the empty spot if they left — the same grace and the same "a miss you can see" rule as
    // resolve_windup, so a dodge works against the boss exactly as it does against a slime.
    void boss_resolve(Creature& c, BossState& b) noexcept {
        // RFC-006: the promise is fulfilled the instant windup hits zero — the decal dies here either
        // way (an attack hands off to the impact Effect below; a charge's dash is no longer a
        // promise, it IS the resolved action, per §1.4's lifecycle table).
        if (b.telegraph_id != 0) {
            for (std::size_t i = 0; i < telegraphs_.size(); ++i) {
                if (telegraphs_[i].id == b.telegraph_id) {
                    telegraphs_.erase(telegraphs_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            b.telegraph_id = 0;
        }
        if (b.winding_charge) {
            b.winding_charge = false;
            float dx = c.windup_x - c.x;
            float dy = c.windup_y - c.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 0.01f) {
                dx = facing_dx(c.facing);
                dy = facing_dy(c.facing);
                len = 1.0f;
            }
            b.charge_dx = dx / len;
            b.charge_dy = dy / len;
            b.charging = kSamuraiKit.abilities[1].active;
            b.dash_hit = false;
            b.charge_cd = kSamuraiKit.abilities[1].cooldown;
            c.boss_pose = static_cast<std::uint8_t>(BossPose::kCharge);
            return;
        }
        c.attack_cd = kSamuraiKit.abilities[0].recover;
        const std::uint64_t tgt = c.windup_target;
        c.windup_target = 0;
        const float grace = kSamuraiKit.abilities[0].shape_radius * 1.15f;
        if (const PlayerBeacon* p = beacon_of(tgt)) {
            const float dx = p->x - c.x;
            const float dy = p->y - c.y;
            if (dx * dx + dy * dy <= grace * grace) {
                if (router != nullptr) {
                    router->get<PlayerActor>(tgt).tell(HurtPlayer{c.damage, c.id});
                }
                add_effect(p->x, p->y, EffectKind::kSlash);  // the blow, on the player
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
                return;
            }
        }
        add_effect(c.windup_x, c.windup_y, EffectKind::kSlash);  // a whiff, where the player was
        c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
    }

    // One tick of the committed dash: fly along the committed heading, clamp to the room floor, and
    // deal the boss's blow once to a player caught in the way. Ends when the dash runs out or it hits
    // a wall — a charge into masonry is spent, which is the counterplay (make it commit into a corner).
    void boss_dash(Creature& c, BossState& b) noexcept {
        c.boss_pose = static_cast<std::uint8_t>(BossPose::kCharge);
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        float nx = c.x + b.charge_dx * kSamuraiKit.abilities[1].shape_speed * dt;
        float ny = c.y + b.charge_dy * kSamuraiKit.abilities[1].shape_speed * dt;
        const bool clamped = clamp_to_room(b, nx, ny);
        c.x = nx;
        c.y = ny;
        if (!b.dash_hit) {
            if (const PlayerBeacon* p =
                    nearest_player_in_room(b, c.x, c.y, kSamuraiKit.abilities[1].shape_radius)) {
                if (router != nullptr) {
                    router->get<PlayerActor>(p->player).tell(HurtPlayer{c.damage, c.id});
                }
                add_effect(p->x, p->y, EffectKind::kSlash);
                b.dash_hit = true;
            }
        }
        if (--b.charging == 0 || clamped) {
            b.charging = 0;
            c.attack_cd = kSamuraiKit.abilities[1].recover;  // a beat of recovery, like any strike
            c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
        }
    }

    // Step toward a point at `speed`, honouring an elemental status (frozen stops it dead) and the
    // room clamp. The boss never leaves its floor rectangle — the player can, through the door.
    void boss_move(Creature& c, BossState& b, float tx, float ty, float speed) noexcept {
        float dx = tx - c.x;
        float dy = ty - c.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f) return;
        const float sp = speed * speed_scale_of(c.status);
        if (sp <= 0.001f) return;  // frozen solid
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        float nx = c.x + (dx / len) * sp * dt;
        float ny = c.y + (dy / len) * sp * dt;
        (void)clamp_to_room(b, nx, ny);
        c.facing = facing_of(nx - c.x, ny - c.y);
        c.x = nx;
        c.y = ny;
    }

    // Clamp a would-be position to the room floor rectangle. Returns whether it had to — the dash
    // reads that as "hit a wall".
    [[nodiscard]] static bool clamp_to_room(const BossState& b, float& x, float& y) noexcept {
        bool hit = false;
        if (x < b.cx0) { x = b.cx0; hit = true; }
        if (x > b.cx1) { x = b.cx1; hit = true; }
        if (y < b.cy0) { y = b.cy0; hit = true; }
        if (y > b.cy1) { y = b.cy1; hit = true; }
        return hit;
    }

    // Is a player beacon in THIS boss's room? Interior beacons carry the interior map, and room
    // membership is pure arithmetic, so this is the whole "the player is in the boss room" test.
    [[nodiscard]] static bool in_room(const BossState& b, const PlayerBeacon& p) noexcept {
        return static_cast<std::uint32_t>(
                   room_index_at(static_cast<int>(p.x), static_cast<int>(p.y))) == b.room;
    }

    // The boss's target: the nearest living player standing in its room. Nobody outside the room can
    // be a target, which is what makes leaving through the door a genuine escape.
    [[nodiscard]] const PlayerBeacon* boss_target(const BossState& b) const noexcept {
        const PlayerBeacon* best = nullptr;
        float best_d2 = 1e18f;
        for (const PlayerBeacon& p : players_) {
            if (p.hp <= 0 || !in_room(b, p)) continue;
            const float dx = p.x - (b.cx0 + b.cx1) * 0.5f;
            const float dy = p.y - (b.cy0 + b.cy1) * 0.5f;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = &p;
            }
        }
        return best;
    }

    // Nearest in-room player within `range` of (x,y) — the dash's contact test.
    [[nodiscard]] const PlayerBeacon* nearest_player_in_room(const BossState& b, float x, float y,
                                                             float range) const noexcept {
        const PlayerBeacon* best = nullptr;
        float best_d2 = range * range;
        for (const PlayerBeacon& p : players_) {
            if (p.hp <= 0 || !in_room(b, p)) continue;
            const float dx = p.x - x;
            const float dy = p.y - y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > best_d2) continue;
            best_d2 = d2;
            best = &p;
        }
        return best;
    }

    // The leash: a boss with no target for kBossLeashTicks snaps back to full HP at its spawn point.
    // Documented as a leash — it stops a player kiting the boss down the corridor of interior rooms.
    void boss_reset(Creature& c, BossState& b) noexcept {
        c.hp = c.max_hp;
        c.x = static_cast<float>(b.spawn_tx) + 0.5f;
        c.y = static_cast<float>(b.spawn_ty) + 0.5f;
        c.windup = 0;
        c.windup_target = 0;
        // RFC-002 §9: an instance/leash reset clears status wholesale, the same as sleep teardown.
        c.status = StatusState{};
        for (Gauge& g : c.gauges) g = Gauge{};
        c.dot_owner = 0;
        ledgers_.erase(c.id);  // RFC-019 §5.8: an instance/leash reset clears the ledger too
        c.attack_cd = 0;
        c.facing = Facing::kDown;
        c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
        b.charging = 0;
        b.winding_charge = false;
        b.charge_cd = 0;
        b.no_target = 0;
        b.telegraph_id = 0;
    }

    // --- projectiles ------------------------------------------------------------------------------
    void step_projectiles() noexcept {
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        std::uint32_t migrated = 0;
        for (std::size_t i = shots_.size(); i-- > 0;) {
            Projectile& p = shots_[i];
            if (p.life == 0) {
                shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            --p.life;
            // RFC-010 §4.3: a shot inside an active field drifts perpendicular to its own heading, a
            // fixed per-tick offset (tiles/tick), pure in (shot id, launch tick, tick, intensity). No
            // `launch_tick` field is stored on `Projectile` (byte-budget discipline, matching the
            // Creature knockback state's own minimal-footprint precedent) — it is derived from the
            // life counter already decremented above, which is equivalent for a shot that never
            // migrates chunks mid-flight (arrows are short-lived, kArrowLife=12 ticks).
            const std::uint8_t field_int = field_intensity_at(p.x, p.y);
            if (field_int > 0) {
                const float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy);
                if (speed > 0.001f) {
                    const auto launch_tick =
                        static_cast<std::uint32_t>(tick_) - (kArrowLife - static_cast<std::uint32_t>(p.life));
                    const float d = drift_perp(static_cast<std::uint32_t>(tick_), p.id, launch_tick, field_int);
                    p.x += (-p.vy / speed) * d;
                    p.y += (p.vx / speed) * d;
                }
            }
            p.x += p.vx * dt;
            p.y += p.vy * dt;

            if (!in_map(p.x, p.y) || arrow_blocked(p.x, p.y)) {
                shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }

            bool hit = false;
            for (Creature& c : creatures_) {
                if (c.hp <= 0) continue;
                const float dx = c.x - p.x;
                const float dy = c.y - p.y;
                if (dx * dx + dy * dy > 0.42f) continue;  // ~0.65 tiles
                const Combo combo = status_detonate(c.status, c.gauges, false, /*by_projectile*/ true,
                                                    /*by_shock_element*/ false, tick_);
                apply_combo_side_effects(combo, c, p.owner);
                strike(c, p.damage, combo, p.owner, Skill::kRanged);
                if (c.hp > 0) {
                    status_gain(c.status, c.gauges,
                               BuildupPacket{Channel::kStagger,
                                             derived_stagger_power(p.damage, false, true), 0, p.owner},
                               mult_pm_of(c.material, c.tier, Channel::kStagger), tick_);
                }
                if (combo == Combo::kBlast) splash(c.x, c.y, 2.0f, p.damage / 2, p.owner);
                hit = true;
                break;
            }
            if (hit) {
                shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }

            // RFC-004 §7: a kGroundAndShot Active entity stops the arrow (you can shoot OVER a
            // spike, but not THROUGH an ice wall); a kArming kFallingRock is hittable specifically
            // by projectiles — the only way to intercept it before it lands.
            bool hit_entity = false;
            for (CombatEntity& e : entities_) {
                const EntityDef edef = entity_def(e.kind);
                if (e.state == EntityState::kActive && edef.collision == Collision::kGroundAndShot &&
                    static_cast<int>(e.x) == static_cast<int>(p.x) &&
                    static_cast<int>(e.y) == static_cast<int>(p.y)) {
                    strike_entity(e, p.damage, p.element, false);
                    hit_entity = true;
                    break;
                }
                if (e.state == EntityState::kArming && edef.hittable_while_arming) {
                    const float dx = e.x - p.x;
                    const float dy = e.y - p.y;
                    if (dx * dx + dy * dy <= kFallingRockHitRadius * kFallingRockHitRadius) {
                        strike_entity(e, p.damage, p.element, false);
                        hit_entity = true;
                        break;
                    }
                }
            }
            if (hit_entity) {
                shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }

            const ChunkCoord owner = chunk_of(coord.map, p.x, p.y);
            if (owner == coord) continue;
            if (router != nullptr) {
                router->get<ChunkActor>(chunk_key(owner)).tell(ProjectileEnter{p});
            }
            shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(i));
            ++migrated;
        }
        if (migrated != 0 && status != nullptr) {
            status->migrations.fetch_add(migrated, std::memory_order_relaxed);
        }
    }

    // An arrow flies over water and over a crop; it stops at a tree trunk or a wall.
    [[nodiscard]] bool arrow_blocked(float fx, float fy) const noexcept {
        const Terrain t = terrain_at(static_cast<int>(fx), static_cast<int>(fy));
        return t == Terrain::kTree || t == Terrain::kBuilding;
    }

    // --- damage resolution ------------------------------------------------------------------------

    // RFC-019 §5.8: update-or-insert `player`'s ledger entry for this creature. A repeat hit from an
    // already-listed player just refreshes recency/branch — it does not add a second entry. A new
    // contributor evicts whichever slot has the OLDEST `last_hit_tick`, which is always an empty
    // slot (`player == 0`, `last_hit_tick == 0`) while one exists, since a real entry can never be
    // older than "never touched." Keyed by creature id in `ledgers_` rather than a field on
    // `Creature` — see `Creature`'s own comment (tiles.hpp) and `CreatureContribEnter` (protocol.hpp)
    // for why.
    void record_contribution(const Creature& c, std::uint64_t player, Skill skill) noexcept {
        if (player == 0) return;
        std::array<Contribution, kMaxContributors>& ledger = ledgers_[c.id];
        int slot = -1;
        for (int i = 0; i < static_cast<int>(kMaxContributors); ++i) {
            if (ledger[i].player == player) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < static_cast<int>(kMaxContributors); ++i) {
                if (ledger[i].last_hit_tick < ledger[slot].last_hit_tick) slot = i;
            }
        }
        ledger[static_cast<std::size_t>(slot)] = Contribution{player, skill, static_cast<std::uint32_t>(tick_)};
    }

    // RFC-018 §7-§10: resolves `loot_table_of(c.kind)` against a per-(creature,tick,recipient)
    // deterministic seed and applies the result through the same trusted hand-off every other
    // reward already uses (`grant()`). Called once per recipient — a single call for an ordinary
    // monster kill's sole `player`, once PER QUALIFYING CONTRIBUTOR for a boss kill (§10.2) — so
    // the seed's `recipient` term is what makes concurrent rolls on the same kill genuinely
    // independent (§8's own determinism claim).
    void grant_loot(const Creature& c, std::uint64_t recipient, Ring ring, bool realm_challenge) noexcept {
        if (recipient == 0) return;
        const LootTable& table = loot_table_of(c.kind);
        const std::uint64_t seed = roll_seed_of(world_seed_, c.id, tick_, recipient);
        const RewardBundle b = roll_loot(table, seed, ring, realm_challenge);
        for (const auto& row : b.items) grant(recipient, GrantItems{row.kind, row.count});
        if (b.essence > 0) grant(recipient, GrantItems{ItemKind::kEssence, b.essence});
        if (b.equipment) grant(recipient, GrantEquipment{table.equipment_slot, *b.equipment});
    }

    // The BOSS pays 400 XP into whichever skill struck (or, for a DoT kill, `Skill::kMagic` — see
    // step_status) the killing blow ("you level what you use"); an ordinary Monster kill pays XP
    // scaled by the killing creature's own ring — killing a wasteland slime is genuinely harder
    // than killing a meadow one, because it IS a harder slime (`make_creature`). step_bosses
    // notices the body is gone next tick and starts the respawn timer. Shared by `strike()` and
    // `step_status()`'s DoT path so a kill is credited identically regardless of what finished the
    // creature off.
    //
    // RFC-019 §5.8: kill XP is not single-recipient. Every ledger entry still inside the assist
    // window is a qualifying contributor and gets the FULL kill-XP amount into their own branch —
    // not a divided share (GAME.md §11: abundance, not a split pool).
    //
    // RFC-018 §10.1/§10.2: item/Essence/equipment loot follows a DIFFERENT distribution rule than
    // XP, by the RFC's own design — an ordinary Monster kill's loot is single-recipient (`player`,
    // the caller's own killing-blow/DoT-owner argument), while a BOSS kill's loot rolls
    // INDEPENDENTLY for every qualifying ledger contributor, same as its XP (§10.2's "abundance"
    // argument extended to the item column). `Faction::kWild` is unchanged — wildlife is food, not
    // this RFC's table.
    void credit_kill(const Creature& c, std::uint64_t player, Skill skill) noexcept {
        if (router == nullptr) return;
        // RFC-023 §Non-goals: "NPC injury/death mechanics" are explicitly not defined by this RFC.
        // A struck-down civilian grants no XP and no loot — `stats_of(kChicken)` would otherwise
        // silently pay out wildlife's "food" reward for killing a person, which is wrong on its face.
        if (creature_is_npc(c)) return;
        const CreatureStats st = stats_of(c.kind);
        const Ring ring_enum = ring_of(world_seed_, static_cast<int>(c.x), static_cast<int>(c.y));
        const auto ring = static_cast<std::uint32_t>(ring_enum);
        const bool realm_challenge = realm_allows_essence();
        bool any_contributor = false;
        const auto it = ledgers_.find(c.id);
        if (it != ledgers_.end()) {
            for (const Contribution& e : it->second) {
                if (e.player == 0) continue;
                if (static_cast<std::uint32_t>(tick_) - e.last_hit_tick > kAssistWindowTicks) continue;
                any_contributor = true;
                if (c.kind == CreatureKind::kBoss) {
                    router->get<PlayerActor>(e.player).tell(GrantXp{e.skill, 400});
                    grant_loot(c, e.player, ring_enum, realm_challenge);
                } else {
                    router->get<PlayerActor>(e.player).tell(
                        GrantXp{e.skill, static_cast<std::uint32_t>(st.xp) * (1u + ring)});
                }
                // RFC-020 §4: `kKill` facts feed `clear` quest objectives, co-subscribing to this
                // SAME per-contributor ledger walk rather than re-deriving kill attribution — every
                // qualifying contributor gets a fact, not only whoever landed the last hit (§4's own
                // resolution of the party-scope `clear` question).
                router->get<PlayerActor>(e.player).tell(
                    GameplayFact{FactKind::kKill, e.player, static_cast<std::uint16_t>(c.kind), 1,
                                static_cast<std::uint8_t>(e.skill), static_cast<std::uint32_t>(tick_)});
            }
        }
        // A ledger miss (the caller's own hit fell outside every contributor's assist window, or
        // this creature was killed by something that never went through record_contribution) still
        // credits the caller directly — the shipped single-player guarantee this replaces must never
        // regress into a silent zero just because the multiplayer ledger came up empty.
        if (!any_contributor && player != 0) {
            router->get<PlayerActor>(player).tell(
                GrantXp{skill, (c.kind == CreatureKind::kBoss)
                                   ? 400u
                                   : static_cast<std::uint32_t>(st.xp) * (1u + ring)});
            if (c.kind == CreatureKind::kBoss) grant_loot(c, player, ring_enum, realm_challenge);
            router->get<PlayerActor>(player).tell(
                GameplayFact{FactKind::kKill, player, static_cast<std::uint16_t>(c.kind), 1,
                            static_cast<std::uint8_t>(skill), static_cast<std::uint32_t>(tick_)});
        }
        if (player != 0) {
            if (st.faction == Faction::kMonster && c.kind != CreatureKind::kBoss) {
                grant_loot(c, player, ring_enum, realm_challenge);
            } else if (st.faction == Faction::kWild) {
                // Wildlife is food, unchanged by this RFC.
                grant(player, GrantItems{ItemKind::kProduce, 1});
            }
        }
        if ((any_contributor || player != 0) && status != nullptr) {
            status->player_kills.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // The one place a creature loses HP to a player, so the one place a direct-hit kill can be
    // credited. `combo` no longer touches status here — RFC-002's `status_detonate` already
    // consumed/updated the ladder BEFORE this is called (see the strike-path order in every
    // handler above), so this function is purely damage + credit, matching RFC-009 §4.4/§4.8's
    // split between combo detection and the damage step.
    //
    // RFC-003 wiring: `impulse` is an authored physics scalar (0 = none — every existing call site
    // keeps working unchanged), pushing the target along the unit direction (`impulse_dir_x/y`,
    // typically the attack's own facing). Two RFC-003 terrain rules apply to every strike
    // unconditionally, impulse or not: slip mitigation (§6 — a low-grip/ice tile softens the blow)
    // and, only when an impulse was authored, the mud rule's force-transfer bonus (§6 — momentum the
    // terrain suppresses hurts instead).
    void strike(Creature& c, std::int16_t damage, Combo combo, std::uint64_t player, Skill skill,
                float impulse_dir_x = 0.0f, float impulse_dir_y = 0.0f,
                std::uint16_t impulse = 0) noexcept {
        if (c.hp <= 0 || damage <= 0) return;
        // RFC-023 §7/Multiplayer: "a new Creature-level check in strike() that rejects damage while
        // state == kSheltering" — the normative targetability-suppression hook the RFC's own review
        // flagged as a hard prerequisite for the Sheltering guarantee to mean anything. A Sheltering
        // civilian is not a valid target for the player's own verbs either, not just a raid monster's.
        if (creature_is_npc(c) && npc_state_of(c) == NpcState::kSheltering) return;
        const TerrainPhys tphys = terrain_phys(terrain_at(static_cast<int>(c.x), static_cast<int>(c.y)),
                                               scar_kind_at(static_cast<int>(c.x), static_cast<int>(c.y)));
        // RFC-010 §4.2: a live kMudded/kIced patch overrides the baseline terrain's knockback/damage
        // coefficients wherever it sits, on top of (not instead of) RFC-004's scar overlay above.
        Surface surf{};
        const bool has_surf = surface_at(static_cast<int>(c.x), static_cast<int>(c.y), surf);
        const SurfaceCoeff sc = surface_coeff(has_surf, surf);

        std::int32_t adj_damage = damage;
        if (slip_applies(tphys.grip)) adj_damage = (adj_damage * kSlipMitigationPm) / 1000;
        if (has_surf && surf == Surface::kIced) adj_damage = (adj_damage * sc.direct_damage_pm) / 1000;

        std::uint16_t effective_impulse = 0;
        if (impulse > 0) {
            effective_impulse = transmit_impulse(impulse, c.material);
            std::int32_t force_bonus =
                force_transfer_crush(effective_impulse, kb_terrain_pm(tphys.friction), tphys.grip);
            if (has_surf && surf == Surface::kMudded) force_bonus = (force_bonus * sc.force_transfer_pm) / 1000;
            adj_damage += force_bonus;
        }

        // RFC-010 §4.3: a struck target inside an active field takes the deterministic per-mille
        // accuracy penalty — no separate M_outer battlefield slot exists in combat_math.hpp, so this
        // folds in directly, same posture as the mud/ice coefficients just above.
        const std::uint8_t field_int = field_intensity_at(c.x, c.y);
        if (field_int > 0) adj_damage = (adj_damage * field_accuracy_pm(field_int)) / 1000;

        // RFC-009 §4.4's five-step formula: M_outer (the combo scale, Shatter ignoring DR), DR
        // stacking, flat toughness, chip floor. `dr`/`toughness` are real per-creature fields
        // (`combat_math.hpp`'s `DefenderMitigation`); DR sources (gear/stance/cover) don't exist yet
        // so `dr` is always `{0,0}` this pass — the mechanism is real, the content is not.
        const auto dealt = resolve_damage(
            static_cast<std::int16_t>(std::clamp<std::int32_t>(adj_damage, 0, 32000)), combo,
            DefenderMitigation{{c.dr[0], c.dr[1]}, c.toughness});
        c.hp = static_cast<std::int16_t>(c.hp - dealt);
        provoke(c, player, /*by_attack*/ true);
        record_contribution(c, player, skill);  // RFC-019 §5.8: every hit, not just the killing one

        if (c.hp > 0 && effective_impulse > 0) {
            float kb = knockback_tiles(effective_impulse, mass_of(c.tier), tphys.friction);
            if (has_surf) kb = (kb * static_cast<float>(sc.knockback_pm)) / 1000.0f;
            if (kb >= kFlinchTiles) {
                // §5: "new knockback replaces remaining" — this simply overwrites all three fields.
                c.kb_dx = impulse_dir_x * kb;
                c.kb_dy = impulse_dir_y * kb;
                c.kb_ticks_left = kKbTicks;
            }
        }

        if (c.hp > 0) return;
        credit_kill(c, player, skill);
    }

    // RFC-003 §5: advance one tick of a Displaced creature's slide. Moves `(kb_dx,kb_dy)/kb_ticks_left`
    // along the remaining vector and re-checks walkability. A blocked next tile converts the
    // undelivered momentum into Crush harm (WallSlam) against the creature and, if the obstacle is a
    // destructible CombatEntity, against it too; against plain impassable terrain it instead runs the
    // §7 stress test, which may scar the tile.
    //
    // Called from a `continue`d branch of `step_creatures`'s loop, so a slide does not (this tick)
    // reach the normal end-of-loop cross-chunk hand-off — a creature displaced within
    // `kKnockbackCap` tiles of an owning chunk's edge briefly holds a position outside its owner
    // until its NEXT ordinary movement tick catches the hand-off. §5's own text sanctions exactly
    // this class of soft cross-chunk debt ("a lost message drops the remainder — acceptable soft
    // state") for the slide-across-a-seam case; this is the same tolerance, one tick later.
    void step_knockback(Creature& c) noexcept {
        const float step_x = c.kb_dx / static_cast<float>(c.kb_ticks_left);
        const float step_y = c.kb_dy / static_cast<float>(c.kb_ticks_left);
        const float nx = c.x + step_x;
        const float ny = c.y + step_y;
        if (passable(nx, ny)) {
            c.x = nx;
            c.y = ny;
            c.kb_dx -= step_x;
            c.kb_dy -= step_y;
            --c.kb_ticks_left;
            if (c.kb_ticks_left == 0) {
                c.kb_dx = 0.0f;
                c.kb_dy = 0.0f;
            }
            return;
        }

        const float remaining = std::sqrt(c.kb_dx * c.kb_dx + c.kb_dy * c.kb_dy);
        const std::int16_t slam = wallslam_crush(remaining, mass_of(c.tier));
        c.hp = static_cast<std::int16_t>(c.hp - slam);
        if (c.hp > 0) {
            if (CombatEntity* e = blocking_entity(c.x, c.y, step_x, step_y)) {
                strike_entity(*e, slam, Element::kNone, /*heavy*/ true);
            } else if (in_map(nx, ny)) {
                const auto tx = static_cast<int>(nx);
                const auto ty = static_cast<int>(ny);
                if (owns(static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty)) &&
                    stress_converts(static_cast<std::uint16_t>(slam),
                                    terrain_phys(terrain_at(tx, ty)).stability)) {
                    stamp_scar(tx, ty, ScarKind::kCracked);
                }
            }
        }
        // A slam that finishes the creature carries no killer attribution (physics.hpp header note);
        // `reap_dead()` collects it next cycle same as any other zero-hp creature.
        c.kb_dx = 0.0f;
        c.kb_dy = 0.0f;
        c.kb_ticks_left = 0;
    }

    // Anger, and the memory of it. Getting hit always provokes; being crowded provokes only a
    // neutral animal. A timid one never fights back — it just runs harder.
    void provoke(Creature& c, std::uint64_t player, bool by_attack) noexcept {
        if (player == 0) return;
        // RFC-023: an NPC's `target` field is repurposed to pack role/state/home_struct (npc.hpp) —
        // it is never a real "angry at this player" key. A civilian is always kNeutral and never
        // fights back regardless, so there is no behavior lost by never provoking one, only a real
        // bug avoided (writing here would corrupt that packed state).
        if (creature_is_npc(c)) return;
        if (c.disposition == Disposition::kTimid) {
            if (by_attack) c.anger_ticks = kAngerTicks;  // makes it flee for a good while
            return;
        }
        if (!by_attack && c.disposition != Disposition::kNeutral) return;
        c.target = player;
        c.anger_ticks = static_cast<std::uint16_t>(
            kAngerTicks + kAngerPerGrudge * static_cast<std::uint16_t>(c.grudge));
        if (by_attack && c.grudge < kMaxGrudge) ++c.grudge;
        if (by_attack) rally_pack(c, player);
    }

    // Hit one wolf and you have hit the pack. Only same-kind neighbours, only within a short
    // radius, and only on a real blow — walking near one animal must not turn its whole species
    // against you.
    void rally_pack(const Creature& hurt, std::uint64_t player) noexcept {
        constexpr float kPackRadius = 7.0f;
        for (Creature& other : creatures_) {
            if (other.id == hurt.id || other.kind != hurt.kind || other.hp <= 0) continue;
            if (creature_is_npc(other)) continue;  // see provoke()'s note — never write its `target`
            if (other.disposition != Disposition::kNeutral) continue;
            const float dx = other.x - hurt.x;
            const float dy = other.y - hurt.y;
            if (dx * dx + dy * dy > kPackRadius * kPackRadius) continue;
            other.target = player;
            other.anger_ticks = kAngerTicks;
        }
    }

    void apply_combo_side_effects(Combo combo, Creature& c, std::uint64_t player) noexcept {
        // Every combo flashes, and the flash is the same one for all of them. A player has to be
        // able to see that something extra happened without reading a number — that is the whole
        // job of the signature mechanic's feedback, and it is worth exactly one shared sprite.
        if (combo != Combo::kNone) add_effect(c.x, c.y, EffectKind::kBlast);
        switch (combo) {
            // RFC-002 §7: Crush no longer applies a flat stun — it applies Stagger BUILD-UP, which
            // is what makes it fair across scale (RFC-009 §4.6). `c` is still alive here (combo
            // detection runs before the strike that may kill it).
            case Combo::kCrush:
                status_gain(c.status, c.gauges, BuildupPacket{Channel::kStagger, 800, 0, player},
                           mult_pm_of(c.material, c.tier, Channel::kStagger), tick_);
                break;
            // Arcing off a shocked target feeds mana back to whoever struck it, which is what makes
            // Shock the school that sustains a mixed build rather than one that only spends.
            case Combo::kArc: grant_vitals(player, GrantVitals{0, 10, 0}); break;
            case Combo::kShatter:
            case Combo::kBlast:
            case Combo::kConduct:
            case Combo::kNone: break;
        }
    }

    // Conduct (RFC-002 §7): the struck target's own hit already ran the full detonate/strike/gain
    // sequence above — this is the CHAIN, a one-shot Shock gain to every OTHER Wet creature nearby,
    // whose Wet is then consumed. Not a second direct-damage strike (the old P2 hand-wire's shape);
    // being wet is the conductor, so a rainstorm (P7) turns this from a combo into a strategy.
    void chain_shock(const Creature& from, std::uint64_t player) noexcept {
        constexpr float kChainRadius = 4.0f;
        constexpr auto kWetBit = static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(Coating::kWet));
        for (Creature& c : creatures_) {
            if (c.id == from.id || c.hp <= 0 || (c.status.coatings & kWetBit) == 0) continue;
            const float dx = c.x - from.x;
            const float dy = c.y - from.y;
            if (dx * dx + dy * dy > kChainRadius * kChainRadius) continue;
            status_gain(c.status, c.gauges, BuildupPacket{Channel::kShock, kThreshold1, 0, player},
                       mult_pm_of(c.material, c.tier, Channel::kShock), tick_);
            c.dot_owner = player;
            c.status.coatings = static_cast<std::uint8_t>(c.status.coatings & ~kWetBit);
            c.status.coating_ticks[static_cast<std::uint8_t>(Coating::kWet)] = 0;
        }
    }

    void splash(float x, float y, float radius, std::int16_t damage, std::uint64_t player) noexcept {
        for (Creature& c : creatures_) {
            if (c.hp <= 0) continue;
            const float dx = c.x - x;
            const float dy = c.y - y;
            if (dx * dx + dy * dy > radius * radius) continue;
            strike(c, damage, Combo::kNone, player, Skill::kRanged);
        }
    }

    void grant(std::uint64_t player, const GrantItems& g) noexcept {
        if (router == nullptr || player == 0) return;
        router->get<PlayerActor>(player).tell(g);
    }

    void grant(std::uint64_t player, const GrantEquipment& g) noexcept {
        if (router == nullptr || player == 0) return;
        router->get<PlayerActor>(player).tell(g);
    }

    void grant_vitals(std::uint64_t player, const GrantVitals& g) noexcept {
        if (router == nullptr || player == 0) return;
        router->get<PlayerActor>(player).tell(g);
    }

    // Effects age out. Capped, because a big enough fight would otherwise put an unbounded vector
    // into every published snapshot — and a snapshot is copied, not referenced.
    void add_effect(float x, float y, EffectKind k) noexcept {
        constexpr std::size_t kMaxEffects = 24;
        if (effects_.size() >= kMaxEffects) return;
        effects_.push_back(Effect{x, y, k, 0});
    }

    void step_effects() noexcept {
        for (std::size_t i = effects_.size(); i-- > 0;) {
            if (++effects_[i].age < effect_life_of(effects_[i].kind)) continue;
            effects_.erase(effects_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    void reap_dead() noexcept {
        std::uint32_t killed = 0;
        for (std::size_t i = creatures_.size(); i-- > 0;) {
            if (creatures_[i].hp > 0) continue;
            // RFC-019 §5.8: the ledger's cleanup point — every death reaches here regardless of
            // whether it went through credit_kill (a wallslam kill, physics.hpp's own header note,
            // carries no killer attribution and is never credited, but its ledger entry still needs
            // to go, matching `dot_owner`'s own reset-on-death shape).
            ledgers_.erase(creatures_[i].id);
            creatures_.erase(creatures_.begin() + static_cast<std::ptrdiff_t>(i));
            ++killed;
        }
        if (killed != 0 && status != nullptr) {
            status->creatures_killed.fetch_add(killed, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool passable(float fx, float fy) const noexcept {
        if (!in_map(fx, fy)) return false;
        const int tx = static_cast<int>(fx);
        const int ty = static_cast<int>(fy);
        return is_walkable(terrain_at(tx, ty)) && !building_blocks(tx, ty) && !entity_blocks(tx, ty);
    }

    // Hit whichever solid building sits on the tile the creature wanted to move onto. Checks the
    // diagonal target first, then each axis, matching the order the movement code tried them.
    void attack_blocking_building(Creature& c, float step_x, float step_y) noexcept {
        if (c.attack_cd > 0 || c.damage <= 0) return;
        const float probes[3][2] = {{c.x + step_x, c.y + step_y}, {c.x + step_x, c.y},
                                    {c.x, c.y + step_y}};
        for (const auto& pr : probes) {
            if (!in_map(pr[0], pr[1])) continue;
            const auto tx = static_cast<std::uint16_t>(pr[0]);
            const auto ty = static_cast<std::uint16_t>(pr[1]);
            for (std::size_t i = 0; i < buildings_.size(); ++i) {
                Building& b = buildings_[i];
                if (b.tx != tx || b.ty != ty || !blocks_movement(b.kind)) continue;
                b.hp = static_cast<std::int16_t>(b.hp - c.damage);
                c.attack_cd = kStrikeCooldown / 2;
                if (b.hp <= 0) buildings_.erase(buildings_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    // RFC-004 §7: whichever Active, collision-bearing entity sits on the tile a creature's move was
    // refused onto. Same probe order as `attack_blocking_building`. Returns a pointer (not a strike)
    // because the caller decides whether the creature is even willing to attack it (hostility, the
    // blocked-repath counter) — unlike a building, which every monster contests on first contact.
    [[nodiscard]] CombatEntity* blocking_entity(float cx, float cy, float step_x, float step_y) noexcept {
        const float probes[3][2] = {{cx + step_x, cy + step_y}, {cx + step_x, cy}, {cx, cy + step_y}};
        for (const auto& pr : probes) {
            if (!in_map(pr[0], pr[1])) continue;
            const auto tx = static_cast<int>(pr[0]);
            const auto ty = static_cast<int>(pr[1]);
            for (CombatEntity& e : entities_) {
                if (e.state != EntityState::kActive) continue;
                if (entity_def(e.kind).collision == Collision::kNone) continue;
                if (static_cast<int>(e.x) == tx && static_cast<int>(e.y) == ty) return &e;
            }
        }
        return nullptr;
    }

    // Is a solid building standing on this tile? Only this chunk's buildings are visible, so a wall
    // sitting exactly on a chunk border does not block creatures arriving from the far side. Bases
    // are built well inside a chunk in practice; the general fix is a neighbour-summary message,
    // which is P3.
    [[nodiscard]] bool building_blocks(int tx, int ty) const noexcept {
        for (const Building& b : buildings_) {
            if (b.tx == tx && b.ty == ty) return blocks_movement(b.kind);
        }
        return false;
    }

    // Same same-chunk-only visibility as `building_blocks` — the seam gap documented there (P3's
    // neighbour-summary message is the general fix) applies here too.
    [[nodiscard]] bool entity_blocks(int tx, int ty) const noexcept {
        if (tx / kChunkTiles != coord.cx || ty / kChunkTiles != coord.cy) return false;
        return block_bits_.test(static_cast<std::size_t>(local_tile_index(tx, ty)));
    }

    [[nodiscard]] bool occupied(std::uint16_t tx, std::uint16_t ty) const noexcept {
        for (const Building& b : buildings_)
            if (b.tx == tx && b.ty == ty) return true;
        for (const Crop& c : crops_)
            if (c.tx == tx && c.ty == ty) return true;
        return false;
    }

    [[nodiscard]] static constexpr float facing_dx(Facing f) noexcept {
        return f == Facing::kLeft ? -1.0f : (f == Facing::kRight ? 1.0f : 0.0f);
    }
    [[nodiscard]] static constexpr float facing_dy(Facing f) noexcept {
        return f == Facing::kUp ? -1.0f : (f == Facing::kDown ? 1.0f : 0.0f);
    }

    // --- render publication ----------------------------------------------------------------------
    void publish() noexcept {
        if (bus == nullptr) return;
        auto v = std::make_shared<ChunkView>();
        v->coord = coord;
        v->tick = tick_;
        v->world_ms = world_ms_;
        for (std::size_t i = 0; i < terrain_.size(); ++i) {
            v->terrain[i] = static_cast<std::uint8_t>(terrain_[i]);
        }
        v->creatures = creatures_;
        v->shots = shots_;
        v->effects = effects_;
        v->entities = entities_;
        v->scars = scars_;
        v->crops = crops_;
        v->buildings = buildings_;
        v->patches = patches_;
        v->fields = fields_;
        v->telegraphs = telegraphs_;
        bus->publish(coord, std::move(v));
    }

    std::array<Terrain, kChunkTiles * kChunkTiles> terrain_{};
    std::vector<Creature> creatures_;
    std::vector<Projectile> shots_;
    std::vector<Effect> effects_;
    std::vector<CombatEntity> entities_;
    std::vector<Scar> scars_;
    std::vector<TilePatch> patches_;  // RFC-010 §4.2, capped at kMaxPatches
    std::vector<FieldState> fields_;  // RFC-010 §4.3, capped at kMaxFields
    std::vector<Telegraph> telegraphs_;  // RFC-006 §2, capped at kMaxTelegraphs
    std::uint32_t next_telegraph_id_ = 0;
    // Derived from `entities_`, rebuilt on any entity state change (rebuild_occupancy_bits) — never
    // published, never stored beyond this chunk's own tick (RFC-004 §4's "derived, not stored").
    std::bitset<kChunkTiles * kChunkTiles> block_bits_;
    std::bitset<kChunkTiles * kChunkTiles> vision_bits_;
    std::vector<Crop> crops_;
    std::vector<Building> buildings_;
    std::vector<PlayerBeacon> players_;  // soft state: who is near enough to matter
    std::vector<BossState> bosses_;      // the dojo bosses this chunk owns, one per dojo room (F3)
    // RFC-019 §5.8: the multiplayer kill-credit ledger, keyed by Creature::id — not a field on
    // Creature itself; see Creature's own comment (tiles.hpp) for why. Entries are removed the
    // moment their creature is (reap_dead, the migration hand-off, boss_reset), so this never
    // outlives the creature it is keyed by.
    std::unordered_map<std::uint32_t, std::array<Contribution, kMaxContributors>> ledgers_;
    std::uint32_t tilled_ = 0;
    std::uint64_t world_seed_ = 0;
    std::uint64_t tick_ = 0;
    std::int64_t world_ms_ = 0;
    std::uint32_t next_id_ = 0;
};

}  // namespace mmo
