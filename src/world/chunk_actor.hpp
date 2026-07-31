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
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/placement_policies.hpp"

#include "world/boss.hpp"
#include "world/combat_entity.hpp"
#include "world/flow_field.hpp"
#include "world/physics.hpp"
#include "world/player_actor.hpp"
#include "world/protocol.hpp"
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
};

struct ChunkActor : quark::Actor<ChunkActor, quark::Sequential, quark::Priority<1>,
                                 quark::DrainBudget<64>, quark::Placement<quark::HashById>> {
    using protocol =
        Protocol<Tick, CreatureEnter, ProjectileEnter, SpawnWave, PlayerBeacon, MeleeSwing,
                 CastSpell, LaunchArrow, AbilityStrike, SpawnEntity, PlantCrop, PlaceBuilding,
                 UpgradeBuilding, TillGround, HarvestAt, Ask<GetChunkStats, ChunkStats>>;

    // --- Wired once at bring-up, before the engine starts -----------------------------------------
    ChunkCoord coord{};
    quark::LocalRouter* router = nullptr;
    SnapshotBus* bus = nullptr;
    WorldStatus* status = nullptr;
    const FlowField* flow = nullptr;  // read-only, never written after bring-up (see flow_field.hpp)
    // Fallback target when a creature is somewhere the flow field does not cover (an unreachable
    // pocket, an island): the settlement nearest to this chunk, resolved once at bring-up.
    float home_x = 0.0f;
    float home_y = 0.0f;

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
                           shots_.empty() && effects_.empty() && entities_.empty() && scars_.empty();
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
        step_creatures(rng);
        step_projectiles();
        step_effects();
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

    void handle(const ProjectileEnter& e) noexcept { shots_.push_back(e.shot); }

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
            }
        }
        // RFC-004 §7: the same arc also hits Active, destroyable entities in reach — friendly fire
        // is always on for entities (a wall does not dodge, and you must be able to break your own).
        strike_entities_in_shape(s.x, s.y, s.reach, fx, fy, /*front_only*/ true, s.damage,
                                 Element::kNone, s.heavy);
    }

    void handle(const CastSpell& s) noexcept {
        const Channel ch = channel_of(s.element);
        if (owns_point(s.x, s.y)) add_effect(s.x, s.y, effect_of(s.element));
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
        if (owns_point(s.x, s.y)) add_effect(s.x, s.y, s.fx);
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
    // The uniform Power anchor RFC-009 §4.5 calibrates for Ice (`kIceBoltPower = 600`) applied to
    // all four elements alike — P2's own status-duration table was already symmetric across
    // schools, so this preserves that symmetry rather than inventing per-element splits.
    static constexpr std::uint16_t kSpellPower = 600;

    // RFC-003 §2.1/§10: authored impulse for the two hit shapes that already carry an explicit
    // "this is a heavy/authored blow" flag today (physics.hpp's header note: no multi-channel
    // payload exists yet to author this per skill in data, so it is a compile-time constant here,
    // mirroring `kSpellPower`'s own posture). 220 is the RFC's own §10 worked-example value.
    static constexpr std::uint16_t kHeavyMeleeImpulse = 220;
    static constexpr std::uint16_t kCrushBlowImpulse = 260;

    [[nodiscard]] static constexpr Channel channel_of(Element e) noexcept {
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
            }
            creatures_.erase(creatures_.begin() + static_cast<std::ptrdiff_t>(i));
            ++migrated;
        }

        if (migrated != 0 && status != nullptr) {
            status->migrations.fetch_add(migrated, std::memory_order_relaxed);
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
            const CreatureStats os = stats_of(other.kind);
            if (stance_between(st.faction, os.faction) != Stance::kHostile) continue;
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
    void step_bosses() noexcept {
        if (bosses_.empty()) return;
        Rng rng(chunk_key(coord) * 0xB055'0F17'11EEull + tick_);
        for (BossState& b : bosses_) {
            Creature* body = find_creature(b.body);
            if (b.alive && (body == nullptr || body->hp <= 0)) {
                // Killed (reaped this tick, or a DoT took the last point): begin the respawn wait.
                b.alive = false;
                b.body = 0;
                b.respawn_timer = kBossRespawnTicks;
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
        c.max_hp = kBossMaxHp;
        c.hp = kBossMaxHp;
        c.damage = kBossDamage;
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

        // Stun cancels a committed wind-up or dash, exactly as it does for any creature (F2).
        if (c.status.primary == Channel::kStagger && c.status.stage == 3) {
            c.windup = 0;
            c.windup_target = 0;
            b.charging = 0;
            b.winding_charge = false;
            c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
            return;
        }

        // A committed wind-up: freeze and telegraph (Creature::windup drives F2's red pulse + smoke),
        // and resolve the tick it reaches zero — an attack lands, a charge begins its dash.
        if (c.windup > 0) {
            c.boss_pose = static_cast<std::uint8_t>(b.winding_charge ? BossPose::kCharge
                                                                     : BossPose::kAttack);
            if (--c.windup == 0) boss_resolve(c, b);
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
            if (++b.no_target >= kBossLeashTicks) {
                boss_reset(c, b);
            } else {
                const float sx = static_cast<float>(b.spawn_tx) + 0.5f;
                const float sy = static_cast<float>(b.spawn_ty) + 0.5f;
                if (std::abs(sx - c.x) > 0.2f || std::abs(sy - c.y) > 0.2f) {
                    boss_move(c, b, sx, sy, kBossApproachSpeed);
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

        switch (a.kind) {
            case BossActionKind::kHold:
                c.facing = facing_of(target->x - c.x, target->y - c.y);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
                break;
            case BossActionKind::kApproach:
                boss_move(c, b, target->x, target->y, kBossApproachSpeed);
                c.boss_pose = static_cast<std::uint8_t>(BossPose::kWalk);
                break;
            case BossActionKind::kAttackLeft:
                c.facing = Facing::kLeft;
                boss_commit(c, b, *target, /*charge*/ false);
                break;
            case BossActionKind::kAttackRight:
                c.facing = Facing::kRight;
                boss_commit(c, b, *target, /*charge*/ false);
                break;
            case BossActionKind::kCharge:
                c.facing = (target->x < c.x) ? Facing::kLeft : Facing::kRight;
                boss_commit(c, b, *target, /*charge*/ true);
                break;
        }
    }

    // Commit to a telegraphed action. Freeze for the wind-up (the biggest telegraph in the game),
    // remember WHO and WHERE, and throw the smoke puff F2 reads as "incoming". No damage here — it
    // lands (or the dash begins) when the counter reaches zero.
    void boss_commit(Creature& c, BossState& b, const PlayerBeacon& prey, bool charge) noexcept {
        c.windup = charge ? kBossChargeWindup : kBossAttackWindup;
        c.windup_target = prey.player;
        c.windup_x = prey.x;
        c.windup_y = prey.y;
        b.winding_charge = charge;
        c.boss_pose = static_cast<std::uint8_t>(charge ? BossPose::kCharge : BossPose::kAttack);
        add_effect(c.x, c.y, EffectKind::kSmoke);
    }

    // A committed wind-up reaches zero. A charge begins its dash toward the aimed-at spot; an attack
    // lands on the committed player if they are still within a hair over reach, and whiffs a visible
    // slash on the empty spot if they left — the same grace and the same "a miss you can see" rule as
    // resolve_windup, so a dodge works against the boss exactly as it does against a slime.
    void boss_resolve(Creature& c, BossState& b) noexcept {
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
            b.charging = kBossChargeDashTicks;
            b.dash_hit = false;
            b.charge_cd = kBossChargeCd;
            c.boss_pose = static_cast<std::uint8_t>(BossPose::kCharge);
            return;
        }
        c.attack_cd = kBossAttackCd;
        const std::uint64_t tgt = c.windup_target;
        c.windup_target = 0;
        const float grace = kBossReach * 1.15f;
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
        float nx = c.x + b.charge_dx * kBossChargeSpeed * dt;
        float ny = c.y + b.charge_dy * kBossChargeSpeed * dt;
        const bool clamped = clamp_to_room(b, nx, ny);
        c.x = nx;
        c.y = ny;
        if (!b.dash_hit) {
            if (const PlayerBeacon* p = nearest_player_in_room(b, c.x, c.y, kBossReach)) {
                if (router != nullptr) {
                    router->get<PlayerActor>(p->player).tell(HurtPlayer{c.damage, c.id});
                }
                add_effect(p->x, p->y, EffectKind::kSlash);
                b.dash_hit = true;
            }
        }
        if (--b.charging == 0 || clamped) {
            b.charging = 0;
            c.attack_cd = kBossAttackCd;  // a beat of recovery, like any strike
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
        c.attack_cd = 0;
        c.facing = Facing::kDown;
        c.boss_pose = static_cast<std::uint8_t>(BossPose::kIdle);
        b.charging = 0;
        b.winding_charge = false;
        b.charge_cd = 0;
        b.no_target = 0;
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

    // The BOSS drops the design reward, flat and not ring-scaled: 400 XP into whichever skill struck
    // (or, for a DoT kill, `Skill::kMagic` — see step_status) the killing blow ("you level what you
    // use") plus 10 produce. This is a PLACEHOLDER — P4 owns real loot tables, and inventing a boss
    // loot table now would be inventing it twice — but it credits the right skill and pays out
    // through the same GrantXp/GrantItems the rest of the game uses. step_bosses notices the body is
    // gone next tick and starts the respawn timer. Shared by `strike()` and `step_status()`'s DoT
    // path so a kill is credited identically regardless of what finished the creature off.
    void credit_kill(const Creature& c, std::uint64_t player, Skill skill) noexcept {
        if (router == nullptr || player == 0) return;
        if (c.kind == CreatureKind::kBoss) {
            router->get<PlayerActor>(player).tell(GrantXp{skill, 400});
            grant(player, GrantItems{ItemKind::kProduce, 10});
            if (status != nullptr) status->player_kills.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const CreatureStats st = stats_of(c.kind);
        // XP follows the ring, not just the species: killing a wasteland slime is genuinely harder
        // than killing a meadow one, because it IS a harder slime (see `make_creature`).
        const auto ring = static_cast<std::uint32_t>(
            ring_of(world_seed_, static_cast<int>(c.x), static_cast<int>(c.y)));
        router->get<PlayerActor>(player).tell(
            GrantXp{skill, static_cast<std::uint32_t>(st.xp) * (1u + ring)});
        // Wildlife is food. Monsters drop nothing yet — loot tables are P4, and inventing a
        // placeholder one now would be inventing it twice.
        if (st.faction == Faction::kWild) {
            grant(player, GrantItems{ItemKind::kProduce, 1});
        }
        if (status != nullptr) status->player_kills.fetch_add(1, std::memory_order_relaxed);
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
        const TerrainPhys tphys = terrain_phys(terrain_at(static_cast<int>(c.x), static_cast<int>(c.y)),
                                               scar_kind_at(static_cast<int>(c.x), static_cast<int>(c.y)));
        std::int32_t adj_damage = damage;
        if (slip_applies(tphys.grip)) adj_damage = (adj_damage * kSlipMitigationPm) / 1000;

        std::uint16_t effective_impulse = 0;
        if (impulse > 0) {
            effective_impulse = transmit_impulse(impulse, c.material);
            adj_damage += force_transfer_crush(effective_impulse, kb_terrain_pm(tphys.friction), tphys.grip);
        }

        // RFC-009 §4.4's five-step formula: M_outer (the combo scale, Shatter ignoring DR), DR
        // stacking, flat toughness, chip floor. `dr`/`toughness` are real per-creature fields
        // (`combat_math.hpp`'s `DefenderMitigation`); DR sources (gear/stance/cover) don't exist yet
        // so `dr` is always `{0,0}` this pass — the mechanism is real, the content is not.
        const auto dealt = resolve_damage(
            static_cast<std::int16_t>(std::clamp<std::int32_t>(adj_damage, 0, 32000)), combo,
            DefenderMitigation{{c.dr[0], c.dr[1]}, c.toughness});
        c.hp = static_cast<std::int16_t>(c.hp - dealt);
        provoke(c, player, /*by_attack*/ true);

        if (c.hp > 0 && effective_impulse > 0) {
            const float kb = knockback_tiles(effective_impulse, mass_of(c.tier), tphys.friction);
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
        bus->publish(coord, std::move(v));
    }

    std::array<Terrain, kChunkTiles * kChunkTiles> terrain_{};
    std::vector<Creature> creatures_;
    std::vector<Projectile> shots_;
    std::vector<Effect> effects_;
    std::vector<CombatEntity> entities_;
    std::vector<Scar> scars_;
    // Derived from `entities_`, rebuilt on any entity state change (rebuild_occupancy_bits) — never
    // published, never stored beyond this chunk's own tick (RFC-004 §4's "derived, not stored").
    std::bitset<kChunkTiles * kChunkTiles> block_bits_;
    std::bitset<kChunkTiles * kChunkTiles> vision_bits_;
    std::vector<Crop> crops_;
    std::vector<Building> buildings_;
    std::vector<PlayerBeacon> players_;  // soft state: who is near enough to matter
    std::vector<BossState> bosses_;      // the dojo bosses this chunk owns, one per dojo room (F3)
    std::uint32_t tilled_ = 0;
    std::uint64_t world_seed_ = 0;
    std::uint64_t tick_ = 0;
    std::int64_t world_ms_ = 0;
    std::uint32_t next_id_ = 0;
};

}  // namespace mmo
