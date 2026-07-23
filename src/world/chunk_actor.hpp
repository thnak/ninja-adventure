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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/placement_policies.hpp"

#include "world/flow_field.hpp"
#include "world/player_actor.hpp"
#include "world/protocol.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"

namespace mmo {

struct ChunkActor : quark::Actor<ChunkActor, quark::Sequential, quark::Priority<1>,
                                 quark::DrainBudget<64>, quark::Placement<quark::HashById>> {
    using protocol =
        Protocol<Tick, CreatureEnter, ProjectileEnter, SpawnWave, PlayerBeacon, MeleeSwing,
                 CastSpell, LaunchArrow, AbilityStrike, SpawnZone, PlantCrop, PlaceBuilding,
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
                           shots_.empty() && effects_.empty() && zones_.empty();
        if (empty) {
            if (tick_ % kIdlePublish == 0) publish();
            return;
        }

        Rng rng(chunk_key(coord) * 0x9E37'79B9'7F4A'7C15ull + t.tick);
        grow_crops();
        step_status();
        step_zones();  // before creatures: a wet/smoke zone changes what they do this tick
        step_creatures(rng);
        step_projectiles();
        step_effects();
        reap_dead();

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
            const Combo combo = combo_of(c.status, s.heavy, /*by_projectile*/ false, Element::kNone);
            apply_combo_side_effects(combo, c, s.player);
            strike(c, s.damage, combo, s.player, Skill::kMelee);
        }
    }

    void handle(const CastSpell& s) noexcept {
        const Status st = status_of(s.element);
        if (owns_point(s.x, s.y)) add_effect(s.x, s.y, effect_of(s.element));
        for (Creature& c : creatures_) {
            const float dx = c.x - s.x;
            const float dy = c.y - s.y;
            if (dx * dx + dy * dy > s.radius * s.radius) continue;
            // Ice landing on something already wet freezes it solid rather than merely chilling it:
            // the status the spell leaves depends on the status it found. This is the one place two
            // schools interact without a physical blow, and it is what makes rain matter (P7).
            const Combo combo = combo_of(c.status, false, false, s.element);
            apply_combo_side_effects(combo, c, s.player);
            strike(c, s.damage, combo, s.player, Skill::kMagic);
            if (c.hp > 0) {
                c.status = st;
                c.status_ticks = status_ticks_of(st);
            }
            if (combo == Combo::kConduct) chain_shock(c, s.damage, s.player);
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
                strike(c, s.damage, Combo::kNone, s.player, s.skill);
                if (c.hp > 0 && s.stun_ticks > 0) {
                    c.stun_ticks = static_cast<std::uint8_t>(s.stun_ticks);
                }
            }
            return;
        }

        // A ring around the caster (WhirlCleave, Nova). The flash goes at the caster whether or not
        // it connects — a whiff is information, exactly as a plain swing's arc is.
        if (owns_point(s.x, s.y)) add_effect(s.x, s.y, s.fx);
        const Status st = status_of(s.element);
        for (Creature& c : creatures_) {
            if (c.hp <= 0) continue;
            const float dx = c.x - s.x;
            const float dy = c.y - s.y;
            if (dx * dx + dy * dy > s.radius * s.radius) continue;
            if (s.element != Element::kNone) {
                // Nova is a big CastSpell: it detonates a wet target for Conduct and leaves its own
                // element's status on the survivors — the same interaction the cast path has.
                const Combo combo = combo_of(c.status, false, false, s.element);
                apply_combo_side_effects(combo, c, s.player);
                strike(c, s.damage, combo, s.player, s.skill);
                if (c.hp > 0) {
                    c.status = st;
                    c.status_ticks = status_ticks_of(st);
                }
                if (combo == Combo::kConduct) chain_shock(c, s.damage, s.player);
            } else {
                strike(c, s.damage, Combo::kNone, s.player, s.skill);
            }
        }
    }

    // Adopt a lingering zone. Exactly one chunk — the one that owns the centre — keeps it, so the
    // renderer draws it once and its per-tick effect is applied once. A radius that reaches into a
    // neighbour therefore under-covers at the seam; that fan-out is F2's, and this is the deliberately
    // minimal F1a shape (see `Zone`).
    void handle(const SpawnZone& z) noexcept {
        if (!owns_point(z.x, z.y)) return;
        if (zones_.size() >= kMaxZones) return;
        zones_.push_back(Zone{z.kind, z.x, z.y, z.radius, z.ticks});
        // The throw is a one-shot puff; the lingering haze is the zone loop the renderer runs.
        if (z.kind == ZoneKind::kSmokeSuppress) add_effect(z.x, z.y, EffectKind::kSmoke);
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
        s.zones = static_cast<std::uint32_t>(zones_.size());
        s.effects = static_cast<std::uint32_t>(effects_.size());
        s.watchers = static_cast<std::uint32_t>(players_.size());
        s.crops = static_cast<std::uint32_t>(crops_.size());
        s.buildings = static_cast<std::uint32_t>(buildings_.size());
        s.tilled = tilled_;
        for (const Creature& c : creatures_) {
            if (c.disposition == Disposition::kHostile || c.anger_ticks > 0) ++s.hostile;
            if (c.status != Status::kNone) ++s.afflicted;
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

    // --- elemental status -------------------------------------------------------------------------
    void step_status() noexcept {
        for (Creature& c : creatures_) {
            if (c.status_ticks == 0) {
                c.status = Status::kNone;
                continue;
            }
            --c.status_ticks;
            // Damage over time, applied every fifth tick rather than every tick. Twice a second is
            // legible; ten times a second is a number blurring, and it makes burn strictly better
            // than every other school at any duration.
            if ((c.status == Status::kBurning || c.status == Status::kShocked) && tick_ % 5 == 0) {
                c.hp = static_cast<std::int16_t>(c.hp - (c.status == Status::kBurning ? 3 : 2));
            }
            if (c.status_ticks == 0) c.status = Status::kNone;
        }
    }

    // --- zones ------------------------------------------------------------------------------------
    // Step every lingering zone down one tick and apply what it does to the creatures this chunk
    // owns inside it. kWet re-marks each tick, so a creature that just wandered in is a conductor and
    // one that stays does not dry out mid-storm — but it never overwrites a status a player set on
    // purpose (frozen/burning), only bare ground or an existing wetting. kSmokeSuppress strips target
    // and anger here; the "cannot re-acquire" half is enforced in step_creatures, which is where
    // targeting happens.
    void step_zones() noexcept {
        for (std::size_t i = zones_.size(); i-- > 0;) {
            Zone& z = zones_[i];
            if (z.ticks_left == 0) {
                zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            --z.ticks_left;
            const float r2 = z.radius * z.radius;
            for (Creature& c : creatures_) {
                if (c.hp <= 0) continue;
                const float dx = c.x - z.x;
                const float dy = c.y - z.y;
                if (dx * dx + dy * dy > r2) continue;
                if (z.kind == ZoneKind::kWet) {
                    if (c.status == Status::kNone || c.status == Status::kWet) {
                        c.status = Status::kWet;
                        c.status_ticks = status_ticks_of(Status::kWet);
                    }
                } else {  // kSmokeSuppress
                    c.target = 0;
                    c.anger_ticks = 0;
                }
            }
        }
    }

    // Is this point under a smoke zone? Cheap and usually a no-op — most chunks hold no zone, and the
    // caller guards on `zones_.empty()` before asking.
    [[nodiscard]] bool in_suppress_zone(float px, float py) const noexcept {
        for (const Zone& z : zones_) {
            if (z.kind != ZoneKind::kSmokeSuppress) continue;
            const float dx = px - z.x;
            const float dy = py - z.y;
            if (dx * dx + dy * dy <= z.radius * z.radius) return true;
        }
        return false;
    }

    // --- creatures ---------------------------------------------------------------------------------
    void step_creatures(Rng& rng) noexcept {
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        std::uint32_t migrated = 0;

        for (std::size_t i = creatures_.size(); i-- > 0;) {
            Creature& c = creatures_[i];
            const CreatureStats st = stats_of(c.kind);

            if (c.attack_cd > 0) --c.attack_cd;
            if (c.anger_ticks > 0 && --c.anger_ticks == 0) c.target = 0;
            if (c.stun_ticks > 0) {
                --c.stun_ticks;
                continue;  // a stunned creature does not move, turn or strike
            }

            // Inside a smoke zone a creature is blinded: it drops what it was chasing and cannot pick
            // up prey again while it stands there. It still wanders and still flees — the suppression
            // is of AGGRESSION, not of movement.
            const bool suppressed = !zones_.empty() && in_suppress_zone(c.x, c.y);
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
                    c.facing = facing_of(dx, dy);
                    strike_player(c, prey->player);
                    continue;  // it planted its feet to swing
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

            const float speed = st.speed * status_speed_scale(c.status);
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
                // building — this is the entire reason a perimeter works.
                attack_blocking_building(c, step_x, step_y);
                continue;  // terrain-boxed instead; the jittered heading will differ next tick
            }

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

    void strike_player(Creature& c, std::uint64_t player) noexcept {
        c.attack_cd = kStrikeCooldown;
        if (router == nullptr || player == 0) return;
        router->get<PlayerActor>(player).tell(HurtPlayer{c.damage, c.id});
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
                const Combo combo = combo_of(c.status, false, /*by_projectile*/ true, Element::kNone);
                apply_combo_side_effects(combo, c, p.owner);
                strike(c, p.damage, combo, p.owner, Skill::kRanged);
                if (combo == Combo::kBlast) splash(c.x, c.y, 2.0f, p.damage / 2, p.owner);
                hit = true;
                break;
            }
            if (hit) {
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

    // The one place a creature loses HP to a player, so the one place a kill can be credited.
    void strike(Creature& c, std::int16_t damage, Combo combo, std::uint64_t player,
                Skill skill) noexcept {
        if (c.hp <= 0 || damage <= 0) return;
        const auto dealt =
            static_cast<std::int16_t>(static_cast<float>(damage) * combo_damage_scale(combo));
        c.hp = static_cast<std::int16_t>(c.hp - dealt);
        if (combo != Combo::kNone) {
            c.status = Status::kNone;  // detonating a status consumes it
            c.status_ticks = 0;
        }
        provoke(c, player, /*by_attack*/ true);
        if (c.hp > 0 || router == nullptr || player == 0) return;
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
            case Combo::kCrush: c.stun_ticks = 20; break;
            // Arcing off a shocked target feeds mana back to whoever struck it, which is what makes
            // Shock the school that sustains a mixed build rather than one that only spends.
            case Combo::kArc: grant_vitals(player, GrantVitals{0, 10, 0}); break;
            case Combo::kShatter:
            case Combo::kBlast:
            case Combo::kConduct:
            case Combo::kNone: break;
        }
    }

    // Conduct: the shock jumps to every WET creature near the one it landed on. Being wet is the
    // conductor, so a rainstorm (P7) will turn this from a combo into a strategy.
    void chain_shock(const Creature& from, std::int16_t damage, std::uint64_t player) noexcept {
        constexpr float kChainRadius = 4.0f;
        for (Creature& c : creatures_) {
            if (c.id == from.id || c.hp <= 0 || c.status != Status::kWet) continue;
            const float dx = c.x - from.x;
            const float dy = c.y - from.y;
            if (dx * dx + dy * dy > kChainRadius * kChainRadius) continue;
            strike(c, damage, Combo::kConduct, player, Skill::kMagic);
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
        return is_walkable(terrain_at(tx, ty)) && !building_blocks(tx, ty);
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
        v->zones = zones_;
        v->crops = crops_;
        v->buildings = buildings_;
        bus->publish(coord, std::move(v));
    }

    std::array<Terrain, kChunkTiles * kChunkTiles> terrain_{};
    std::vector<Creature> creatures_;
    std::vector<Projectile> shots_;
    std::vector<Effect> effects_;
    std::vector<Zone> zones_;
    std::vector<Crop> crops_;
    std::vector<Building> buildings_;
    std::vector<PlayerBeacon> players_;  // soft state: who is near enough to matter
    std::uint32_t tilled_ = 0;
    std::uint64_t world_seed_ = 0;
    std::uint64_t tick_ = 0;
    std::int64_t world_ms_ = 0;
    std::uint32_t next_id_ = 0;
};

}  // namespace mmo
