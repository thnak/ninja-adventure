// ChunkActor — one actor owns one 32x32-tile chunk and is the SINGLE WRITER of everything in it.
//
// This is where the engine choice pays for itself. A chunk is a natural actor: it has state
// (terrain, mobs, crops, buildings), that state is only ever mutated by messages, and the messages
// that mutate it are inherently serialised per chunk. `Sequential` therefore costs nothing and buys
// the entire absence of locking in this file — there is not one mutex or atomic in the simulation.
//
// TRUST TIER B. Placement is unconstrained (`Placement<HashById>`): a chunk may be hosted on a
// player's machine. That is safe *by selection*, not by hope — everything a chunk decides is either
// replayable from (chunk key, tick) via the deterministic RNG, or low-value (a slime's position).
// The things a player would want to forge — inventory, affordability — live in PlayerActor, which
// carries `Require<Trusted>` and is unreachable from here except through an `ask`.
//
// MIGRATION is the load-bearing demo. A mob that walks off the east edge of a chunk is removed from
// this actor and `tell`-ed to the neighbour as a `MobEnter`. Today that is an in-process enqueue;
// once chunks are placed across nodes it becomes a serialized frame over TCP, and **not one line of
// this file changes** — the router resolves a remote ActorRef exactly like a local one. The handoff
// needs no ack, no two-phase commit and no lock, because the sender removes before it sends and
// per-(sender,receiver) FIFO means the mob cannot be observed twice or out of order.
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
    using protocol = Protocol<Tick, MobEnter, SpawnWave, PlantCrop, PlaceBuilding, UpgradeBuilding,
                              TillGround, HarvestAt, Ask<GetChunkStats, ChunkStats>>;

    // --- Wired once at bring-up, before the engine starts -----------------------------------------
    ChunkCoord coord{};
    quark::LocalRouter* router = nullptr;
    SnapshotBus* bus = nullptr;
    WorldStatus* status = nullptr;
    quark::ActorRef<PlayerActor> player{};
    const FlowField* flow = nullptr;  // read-only, never written after bring-up (see flow_field.hpp)
    // Fallback target when a mob is somewhere the flow field does not cover. Once world generation
    // lands this becomes "the nearest settlement"; today it is the map centre.
    float home_x = 0.0f;
    float home_y = 0.0f;

    // ================================ handlers ====================================================

    void handle(const Tick& t) noexcept {
        tick_ = t.tick;
        world_ms_ = t.world_ms;
        Rng rng(chunk_key(coord) * 0x9E37'79B9'7F4A'7C15ull + t.tick);

        grow_crops();
        fire_turrets(rng);
        step_mobs(rng);
        publish();
    }

    // A mob arrived from a neighbouring chunk. Adopt it verbatim — the sender already owns the
    // decision that this chunk is the new owner.
    //
    // The republish matters. Views are published once per tick, so without it a migrating mob is in
    // NEITHER published view for up to a full tick: the sender already dropped it, and this chunk
    // will not publish until its next tick. At 10 Hz that is a 100 ms hole — visible as a blink
    // when a mob crosses a boundary, and it made snapshot-based counts read low (measured: 70 mobs
    // alive by `ask`, 28 visible in views). Republishing on arrival closes the hole; it costs one
    // extra publish per migration, which is nothing.
    void handle(const MobEnter& e) noexcept {
        mobs_.push_back(e.mob);
        publish();
    }

    void handle(const SpawnWave& w) noexcept {
        Rng rng(chunk_key(coord) ^ (static_cast<std::uint64_t>(w.seed) << 17));
        const auto kind = static_cast<MobKind>(w.kind % 3);
        const auto st = stats_of(kind);
        for (std::uint16_t i = 0; i < w.count; ++i) {
            // Spawn anywhere walkable inside this chunk. Edge chunks are the ones the director
            // picks, so "spawn inside the chunk" already means "spawn at the map's rim".
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
            Mob m{};
            m.id = ++next_mob_id_ | (static_cast<std::uint32_t>(chunk_key(coord)) << 12);
            m.x = static_cast<float>(tx) + 0.5f;
            m.y = static_cast<float>(ty) + 0.5f;
            m.hp = st.max_hp;
            m.kind = kind;
            mobs_.push_back(m);
        }
    }

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
    }

    // Upgrade in place: level up, and heal by exactly the HP the new level adds, so upgrading a
    // damaged wall does not silently repair it.
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

    // Base expansion. The starting apron is baked into the terrain FUNCTION; everything the player
    // reclaims later is this chunk's own overlay, written straight into the terrain cache.
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
            player.tell(GrantItems{ItemKind::kProduce, 1 + static_cast<std::int32_t>(c.kind)});
            player.tell(GrantItems{ItemKind::kSeed, 1});
            crops_.erase(crops_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }

    // An answer to this proves every message posted before it has been drained (mailbox FIFO), so
    // the headless runner uses it as a barrier instead of sleeping.
    void handle(const Ask<GetChunkStats, ChunkStats>& m) noexcept {
        ChunkStats s{};
        s.mobs = static_cast<std::uint32_t>(mobs_.size());
        s.crops = static_cast<std::uint32_t>(crops_.size());
        s.buildings = static_cast<std::uint32_t>(buildings_.size());
        s.tilled = tilled_;
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

    // Owned tiles come from the cache; anything else is computed. The `owns` test is what makes
    // this correct at a chunk border — an earlier version indexed the cache unconditionally, and
    // because `local_tile_index` wraps modulo, a lookup one tile past the edge silently returned a
    // tile from the *opposite* side of this chunk. Mobs stepping across a boundary read that
    // unrelated tile, and any mob whose mirrored tile happened to be water froze on the border
    // permanently.
    [[nodiscard]] Terrain terrain_at(int tx, int ty) const noexcept {
        if (tx / kChunkTiles == coord.cx && ty / kChunkTiles == coord.cy) {
            return terrain_[static_cast<std::size_t>(local_tile_index(tx, ty))];
        }
        return terrain_of(world_seed_, coord.map, tx, ty);
    }

private:
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

    // --- turrets ---------------------------------------------------------------------------------
    // A turret only sees mobs inside its own chunk. That is a real limitation, and it is the honest
    // one to ship first: cross-chunk targeting means a chunk reading another chunk's state, which is
    // exactly the shared-mutable-state coupling the actor model is here to prevent. The fix, when
    // it's needed, is for chunks to publish a threat summary to their neighbours as a message.
    void fire_turrets(Rng& rng) noexcept {
        (void)rng;
        for (Building& b : buildings_) {
            if (b.kind != BuildKind::kTurret) continue;
            if (b.cooldown > 0) {
                --b.cooldown;
                continue;
            }
            const float bx = static_cast<float>(b.tx) + 0.5f;
            const float by = static_cast<float>(b.ty) + 0.5f;
            const float range = turret_range(b.level);
            Mob* best = nullptr;
            float best_d2 = range * range;
            for (Mob& m : mobs_) {
                const float dx = m.x - bx;
                const float dy = m.y - by;
                const float d2 = dx * dx + dy * dy;
                if (d2 <= best_d2) {
                    best_d2 = d2;
                    best = &m;
                }
            }
            if (best == nullptr) continue;
            best->hp = static_cast<std::int16_t>(best->hp - turret_damage(b.level));
            b.cooldown = turret_cooldown(b.level);
        }
        reap_dead_mobs();
    }

    void reap_dead_mobs() noexcept {
        std::uint32_t killed = 0;
        for (std::size_t i = mobs_.size(); i-- > 0;) {
            if (mobs_[i].hp > 0) continue;
            mobs_.erase(mobs_.begin() + static_cast<std::ptrdiff_t>(i));
            ++killed;
        }
        if (killed != 0 && status != nullptr) {
            status->mobs_killed.fetch_add(killed, std::memory_order_relaxed);
        }
    }

    // --- mobs ------------------------------------------------------------------------------------
    void step_mobs(Rng& rng) noexcept {
        const float dt = static_cast<float>(kTickMs) / 1000.0f;
        std::uint32_t migrated = 0;

        for (std::size_t i = mobs_.size(); i-- > 0;) {
            Mob& m = mobs_[i];
            const auto st = stats_of(m.kind);

            // Heading: follow the flow field downhill if this tile is on it, otherwise fall back
            // to steering straight at home (an unreachable pocket, or no field built).
            float dx = 0.0f;
            float dy = 0.0f;
            int fx = 0;
            int fy = 0;
            if (flow != nullptr && flow->ready() &&
                flow->descend(static_cast<int>(m.x), static_cast<int>(m.y), fx, fy)) {
                dx = static_cast<float>(fx);
                dy = static_cast<float>(fy);
            } else {
                dx = home_x - m.x;
                dy = home_y - m.y;
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

            if (m.attack_cd > 0) --m.attack_cd;

            // Try the full step, then slide along each axis. Without the slide a mob that meets
            // water head-on stops forever, because its steering vector keeps pointing into the
            // obstacle; sliding lets it round the edge in a tick or two.
            const float step_x = dx * st.speed * dt;
            const float step_y = dy * st.speed * dt;
            float nx = m.x;
            float ny = m.y;
            if (passable(m.x + step_x, m.y + step_y)) {
                nx = m.x + step_x;
                ny = m.y + step_y;
            } else if (passable(m.x + step_x, m.y)) {
                nx = m.x + step_x;
            } else if (passable(m.x, m.y + step_y)) {
                ny = m.y + step_y;
            } else {
                // Every way forward is blocked. If a BUILDING is what is blocking it, break that
                // building — this is the entire reason a perimeter works. Previously a mob attacked
                // whatever happened to be within 1.4 tiles whether or not it was in the way, and
                // walls did not block movement at all, so a wall was decoration a mob walked past.
                attack_blocking_building(m, st.damage, step_x, step_y);
                continue;  // terrain-boxed instead; the jittered heading will differ next tick
            }

            // Facing is derived from the step actually taken, not the step intended — a mob
            // sliding along a wall should face where it is going.
            m.facing = facing_of(nx - m.x, ny - m.y);
            m.x = nx;
            m.y = ny;

            // ---- the hand-off ----------------------------------------------------------------
            const ChunkCoord owner = chunk_of(coord.map, m.x, m.y);
            if (owner == coord) continue;
            if (router != nullptr) {
                router->get<ChunkActor>(chunk_key(owner)).tell(MobEnter{m});
            }
            mobs_.erase(mobs_.begin() + static_cast<std::ptrdiff_t>(i));
            ++migrated;
        }

        if (migrated != 0 && status != nullptr) {
            status->migrations.fetch_add(migrated, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool passable(float fx, float fy) const noexcept {
        if (!in_map(fx, fy)) return false;
        const int tx = static_cast<int>(fx);
        const int ty = static_cast<int>(fy);
        return is_walkable(terrain_at(tx, ty)) && !building_blocks(tx, ty);
    }

    // Hit whichever solid building sits on the tile the mob wanted to move onto. Checks the
    // diagonal target first, then each axis, matching the order the movement code tried them.
    void attack_blocking_building(Mob& m, std::int16_t damage, float step_x, float step_y) noexcept {
        if (m.attack_cd > 0) return;
        const float probes[3][2] = {
            {m.x + step_x, m.y + step_y}, {m.x + step_x, m.y}, {m.x, m.y + step_y}};
        for (const auto& pr : probes) {
            if (!in_map(pr[0], pr[1])) continue;
            const auto tx = static_cast<std::uint16_t>(pr[0]);
            const auto ty = static_cast<std::uint16_t>(pr[1]);
            for (std::size_t i = 0; i < buildings_.size(); ++i) {
                Building& b = buildings_[i];
                if (b.tx != tx || b.ty != ty || !blocks_movement(b.kind)) continue;
                b.hp = static_cast<std::int16_t>(b.hp - damage);
                m.attack_cd = 5;
                if (b.hp <= 0) buildings_.erase(buildings_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    // Is a solid building standing on this tile? Only this chunk's buildings are visible, so a wall
    // sitting exactly on a chunk border does not block mobs arriving from the far side. Bases are
    // built well inside a chunk in practice; the general fix is the same neighbour-summary message
    // the turret targeting limitation needs.
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
        v->mobs = mobs_;
        v->crops = crops_;
        v->buildings = buildings_;
        bus->publish(coord, std::move(v));
    }

    std::array<Terrain, kChunkTiles * kChunkTiles> terrain_{};
    std::vector<Mob> mobs_;
    std::vector<Crop> crops_;
    std::vector<Building> buildings_;
    std::uint32_t tilled_ = 0;
    std::uint64_t world_seed_ = 0;
    std::uint64_t tick_ = 0;
    std::int64_t world_ms_ = 0;
    std::uint32_t next_mob_id_ = 0;
};

}  // namespace mmo
