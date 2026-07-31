// RFC-015 — Client Replication & Interest-Set Protocol.
//
// Confirmed before writing a line of this file: no client-facing wire/network protocol exists
// anywhere in `src/` today. `client_main.cpp` reads `SnapshotBus`/`PlayerBus` directly, in-process,
// on the same machine as the simulation — there is no socket, no serialization step, nothing this
// file replaces. What this file builds is the real, testable SHAPE that protocol would have: packed
// wire projections for the record families that don't have one yet (`Creature`/`Projectile`/`Effect`,
// plus a remote-player view), the delta-encoding engine that decides what a subscribed client would
// receive on a given tick, and the interest-set/banding/budget policy RFC-015 specifies — all as
// pure, engine-agnostic C++ a future P6 transport layer would sit underneath. The transport itself
// (sockets, auth, reconnection) is explicitly this RFC's own Non-goal, P6's job.
//
// SCOPE NARROWED FURTHER THAN THE RFC'S OWN TEXT ALREADY NARROWS IT. RFC-015 was written against an
// EARLIER `ChunkView` shape (`Creature`/`Projectile`/`Effect`/`Zone`/`Crop`/`Building` only) and
// explicitly excludes `Zone`/`Crop`/`Building` as a farm/economy replication problem with no owning
// authoring RFC. Since that RFC's text was finalized, RFC-004/006/010's own implementation work (done
// earlier in this project's history) added FIVE more fields to `ChunkView` this RFC's text never
// discusses at all: `entities` (CombatEntity), `scars` (Scar), `patches` (TilePatch), `fields`
// (FieldState), `telegraphs` (Telegraph). This file extends RFC-015's own reasoning — "a wire
// projection with no owning design RFC yet would mean inventing both at once" — to those five fields
// as well: no RFC in this set specs a packed wire shape for them (RFC-004/006/010 never built one,
// confirmed by source survey: neither `PublishedEntity` nor `PublishedScar`, the packed-projection
// precedent RFC-015's own text cites, exists anywhere in `src/`), so this file does not invent one
// either. Only `Creature`, `Projectile`, `Effect`, and a remote-player projection of `PlayerView` —
// RFC-015 §2's actual stated scope — get wire shapes here.
//
// ALSO DEFERRED, NAMED EXPLICITLY: the Map-marker channel (§6). RFC-015 §6 satisfies RFC-021 §5.4's
// marker-budget contract — but RFC-021's own discovery/fog/marker data model was never built (a
// direct source search for `MapMarker`/`discovery`/`fog` across `src/` returns nothing). §6 has no
// concrete input to carry; building the channel ahead of the data it would transport would be the
// same "inventing both at once" problem this file already declines for Zone/Crop/Building. Flagged
// here, not designed. Likewise deferred: §8's latency table and §9's reliable/best-effort message
// CLASSIFICATION are policy statements about a transport that does not exist yet — this file's
// `ChunkDelta` comments note which fields are which class (matching §9), but nothing here enforces
// delivery guarantees a real socket would need to provide.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "world/map_system.hpp"
#include "world/snapshot.hpp"
#include "world/status.hpp"
#include "world/tiles.hpp"

namespace mmo {

// ==================== §2: wire projections =========================================================
// Every struct below is genuinely packed: sizeof == the sum of its fields, no implicit alignment
// padding, matching RFC-015 §2's own discipline. Positions stay plain `float` — RFC-015 §2.1 rules
// against fixed-point quantization for replication specifically (that rule governs cross-node
// DETERMINISM comparisons, RFC-010 §5; a wire packet compares nothing, it only draws).

#pragma pack(push, 1)

struct PublishedCreature {       // 24 bytes
    std::uint32_t id = 0;        // continuity across deltas (renderer/audio dedup key)
    float x = 0.0f, y = 0.0f;    // map-global tile position
    std::int16_t hp = 0, max_hp = 0;
    std::uint8_t kind = 0;       // CreatureKind
    std::uint8_t facing = 0;     // Facing
    // Packed StatusState (RFC-002's ladder system superseded the flat `Status` enum RFC-015's own
    // text assumed when it named this field — that enum no longer exists in `src/`): bits[2:0] =
    // Channel (primary, 0..6), bits[4:3] = stage (0..3), bit 5 = the Wet coating bit. Bits 6-7 unused.
    std::uint8_t status = 0;
    std::uint8_t windup = 0;      // >0 while telegraphing — must reach the client every tick it's
                                    // nonzero, never coalesced away by band throttling (§4)
    std::uint8_t boss_pose = 0;   // BossPose, 0 (kIdle) for all non-boss creatures
    std::uint8_t disposition = 0; // Disposition — the player-visible "is this thing calm" state
    std::uint16_t _pad = 0;
};
static_assert(sizeof(PublishedCreature) == 24);

inline constexpr std::uint8_t kPublishedStatusWetBit = 1u << 5;

[[nodiscard]] inline constexpr std::uint8_t pack_status(const StatusState& s) noexcept {
    std::uint8_t v = static_cast<std::uint8_t>(s.primary) & 0x07u;
    v = static_cast<std::uint8_t>(v | ((s.stage & 0x03u) << 3));
    if ((s.coatings & (1u << static_cast<unsigned>(Coating::kWet))) != 0) v |= kPublishedStatusWetBit;
    return v;
}

struct PublishedProjectile {     // 24 bytes
    std::uint32_t id = 0;
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;  // tiles/sec — lets the client extrapolate motion between updates
    std::uint8_t element = 0;    // Element — selects the trail FX
    std::uint8_t _pad[3] = {};
};
static_assert(sizeof(PublishedProjectile) == 24);

struct PublishedEffect {         // 12 bytes — same size as tiles.hpp::Effect; nothing dropped or added
    float x = 0.0f, y = 0.0f;
    std::uint8_t kind = 0;       // EffectKind
    std::uint8_t age = 0;        // renderer maps age -> strip frame, exactly as it does in-process
    std::uint16_t _pad = 0;
};
static_assert(sizeof(PublishedEffect) == 12);

// The windup-commit geometry (§2.2): the ONLY wire home for "where the target stood when the windup
// started." Sent once, the tick PublishedCreature.windup transitions 0 -> nonzero; never re-sent for
// that windup's remaining lifetime (RFC-006 T2 already freezes the geometry server-side).
struct WindupCommit {            // 12 bytes
    std::uint32_t creature_id = 0;
    float x = 0.0f, y = 0.0f;
};
static_assert(sizeof(WindupCommit) == 12);

inline constexpr std::uint8_t kPlayerRemoteMounted = 1u << 0;
inline constexpr std::uint8_t kPlayerRemoteDead = 1u << 1;  // dead_ticks > 0

struct PublishedPlayerRemote {   // 28 bytes — what OTHER clients receive about a player
    std::uint64_t id = 0;
    float x = 0.0f, y = 0.0f;
    std::int16_t hp = 0, max_hp = 0;
    std::uint8_t facing = 0;
    std::uint8_t flags = 0;      // kPlayerRemoteMounted | kPlayerRemoteDead
    std::uint8_t _pad[2] = {};
    std::uint32_t last_swing_tick = 0;  // truncated from PlayerView's uint64_t — wraps after ~13.6
                                          // years of continuous 10 Hz ticking, not a practical concern
};
static_assert(sizeof(PublishedPlayerRemote) == 28);

// The owning client's own subscription is not a distinct struct — it is "send the existing
// PlayerView, narrowed to one recipient" (RFC-015 §2.5), the cheapest possible answer for the one
// case where full fidelity (inventory, skill XP, ability cooldowns) is required. No new type needed.

#pragma pack(pop)

[[nodiscard]] inline PublishedCreature publish_of(const Creature& c) noexcept {
    PublishedCreature p{};
    p.id = c.id;
    p.x = c.x;
    p.y = c.y;
    p.hp = c.hp;
    p.max_hp = c.max_hp;
    p.kind = static_cast<std::uint8_t>(c.kind);
    p.facing = static_cast<std::uint8_t>(c.facing);
    p.status = pack_status(c.status);
    p.windup = c.windup;
    p.boss_pose = c.boss_pose;
    p.disposition = static_cast<std::uint8_t>(c.disposition);
    return p;
}

[[nodiscard]] inline PublishedProjectile publish_of(const Projectile& pr) noexcept {
    PublishedProjectile p{};
    p.id = pr.id;
    p.x = pr.x;
    p.y = pr.y;
    p.vx = pr.vx;
    p.vy = pr.vy;
    p.element = static_cast<std::uint8_t>(pr.element);
    return p;
}

[[nodiscard]] inline PublishedEffect publish_of(const Effect& e) noexcept {
    PublishedEffect p{};
    p.x = e.x;
    p.y = e.y;
    p.kind = static_cast<std::uint8_t>(e.kind);
    p.age = e.age;
    return p;
}

[[nodiscard]] inline PublishedPlayerRemote publish_of(const PlayerView& v) noexcept {
    PublishedPlayerRemote p{};
    p.id = v.id;
    p.x = v.x;
    p.y = v.y;
    p.hp = v.hp;
    p.max_hp = v.max_hp;
    p.facing = static_cast<std::uint8_t>(v.facing);
    p.flags = static_cast<std::uint8_t>((v.mounted ? kPlayerRemoteMounted : 0) |
                                        (v.dead_ticks > 0 ? kPlayerRemoteDead : 0));
    p.last_swing_tick = static_cast<std::uint32_t>(v.last_swing_tick);
    return p;
}

[[nodiscard]] inline bool published_creature_equal(const PublishedCreature& a,
                                                    const PublishedCreature& b) noexcept {
    return std::memcmp(&a, &b, sizeof(PublishedCreature)) == 0;
}
[[nodiscard]] inline bool published_projectile_equal(const PublishedProjectile& a,
                                                      const PublishedProjectile& b) noexcept {
    return std::memcmp(&a, &b, sizeof(PublishedProjectile)) == 0;
}

// ==================== §1: the interest set ==========================================================
// Reuses `map_director.hpp::fan_beacons()`'s own loop bounds EXACTLY (kSpan=2, clamped to
// `[0, kMapChunks)`), not a second, independently-tuned radius — RFC-015's own central ruling. This
// inherits `fan_beacons()`'s known gap by construction, deliberately: the clamp is hardcoded to the
// overworld's 32-chunk edge and is not chunk_edge-aware for a smaller instanced map, a pre-existing
// gap RFC-014 Open Question 7 owns fixing, not this RFC (§1's own text: "this RFC does not fix
// fan_beacons()'s clamp... it only confirms the replication side inherits the same... gap").
//
// A SECOND, RELATED GAP DISCOVERED WHILE TESTING THIS FILE, NOT FIXED HERE FOR THE SAME REASON:
// `SnapshotBus::load()`'s instanced-band branch (`snapshot.hpp`) resolves a candidate chunk via
// `instance_local_index()` (`map_system.hpp`), which combines `cx`/`cy` into one linear index
// (`cy*chunk_edge + cx`) WITHOUT first checking either coordinate against `chunk_edge` individually.
// For a small instance, an interest-set candidate this file generates that lies outside the real
// footprint (e.g. `chunk_edge=2`, candidate `(cx=2, cy=0)`) does not cleanly miss — it ALIASES a
// different real chunk's slot (`(cx=0, cy=1)` also resolves to local index 2) and `load()` returns
// that chunk's genuine, non-null view under the wrong coordinate, rather than the null a true
// out-of-footprint miss should produce. This is RFC-014's own addressing scheme, not something this
// file introduces or can safely fix by changing only its own call site — flagged here as a sibling to
// the `fan_beacons()` clamp gap immediately above, for whichever pass closes RFC-014 Open Question 7.

inline constexpr int kInterestSpan = 2;  // chunks either side — RFC-015 §1, mirrors fan_beacons()

[[nodiscard]] inline std::vector<ChunkCoord> client_interest_set(MapId map, ChunkCoord home) {
    std::vector<ChunkCoord> out;
    out.reserve(static_cast<std::size_t>(2 * kInterestSpan + 1) * (2 * kInterestSpan + 1));
    for (int dy = -kInterestSpan; dy <= kInterestSpan; ++dy) {
        for (int dx = -kInterestSpan; dx <= kInterestSpan; ++dx) {
            const int cx = static_cast<int>(home.cx) + dx;
            const int cy = static_cast<int>(home.cy) + dy;
            if (cx < 0 || cy < 0 || cx >= kMapChunks || cy >= kMapChunks) continue;
            out.push_back(
                ChunkCoord{map, static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy)});
        }
    }
    return out;
}

[[nodiscard]] inline constexpr int chebyshev_distance(ChunkCoord a, ChunkCoord home) noexcept {
    const int dx = static_cast<int>(a.cx) - static_cast<int>(home.cx);
    const int dy = static_cast<int>(a.cy) - static_cast<int>(home.cy);
    return std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
}

// ==================== §4/§5: tunables ===============================================================

inline constexpr std::uint64_t kOuterBandSendPeriodTicks = 3;  // deliberately == kBeaconPeriod
inline constexpr int kWireCreatureCapPerChunk = 40;             // (tunable) — Open Question 1
inline constexpr int kWireProjectileCapPerChunk = 20;            // (tunable) — Open Question 1
inline constexpr std::size_t kPerTickDeltaBudget = 2560;        // (tunable) 2.5 KB — Open Question 6

// ==================== §3: baseline and delta shapes =================================================

struct ChunkBaseline {
    ChunkCoord coord{};
    std::uint64_t tick = 0;
    std::array<std::uint8_t, static_cast<std::size_t>(kChunkTiles) * kChunkTiles> terrain{};
    std::vector<PublishedCreature> creatures;
    std::vector<PublishedProjectile> projectiles;
    std::vector<PublishedEffect> effects;
    // Any creature already mid-windup at the moment this baseline is built needs its commit geometry
    // too — otherwise a client subscribing mid-telegraph would see `windup > 0` with no target spot.
    // Not part of RFC-015's own §3.1 pseudocode (which only lists baseline's steady-state fields) but
    // a necessary consequence of it: a baseline is "everything, once," and an in-flight windup's
    // frozen spot is part of that everything.
    std::vector<WindupCommit> windup_commits;
};

// Reliable, ordered (RFC-015 §9): removed_*, windup_commits, terrain patch, and the baseline itself.
// Best-effort, latest-wins (§9): added_*/updated_* creature and projectile records, effects.
struct ChunkDelta {
    ChunkCoord coord{};
    std::uint64_t tick = 0;
    std::vector<PublishedCreature> added_creatures;
    std::vector<PublishedCreature> updated_creatures;  // whole-record, id-keyed (§3.2 — no
                                                        // field-level bitmask; Open Question 3)
    std::vector<std::uint32_t> removed_creatures;
    std::vector<PublishedProjectile> added_projectiles;
    std::vector<PublishedProjectile> updated_projectiles;
    std::vector<std::uint32_t> removed_projectiles;
    std::vector<PublishedEffect> effects;  // no persistent id — every effect active this tick IS
                                            // the delta, by construction (§3.2)
    std::vector<WindupCommit> windup_commits;
    bool has_terrain_patch = false;
    std::uint16_t patch_tx = 0, patch_ty = 0;
    std::uint8_t patch_tile = 0;

    // Chebyshev distance from the subscribing client's home chunk — carried for §5's budget culling
    // (farthest-first), never itself part of the wire payload.
    int distance = 0;

    [[nodiscard]] std::size_t byte_estimate() const noexcept {
        return added_creatures.size() * sizeof(PublishedCreature) +
               updated_creatures.size() * sizeof(PublishedCreature) +
               removed_creatures.size() * sizeof(std::uint32_t) +
               added_projectiles.size() * sizeof(PublishedProjectile) +
               updated_projectiles.size() * sizeof(PublishedProjectile) +
               removed_projectiles.size() * sizeof(std::uint32_t) +
               effects.size() * sizeof(PublishedEffect) +
               windup_commits.size() * sizeof(WindupCommit) + (has_terrain_patch ? 5u : 0u);
    }

    [[nodiscard]] bool empty() const noexcept {
        return added_creatures.empty() && updated_creatures.empty() && removed_creatures.empty() &&
               added_projectiles.empty() && updated_projectiles.empty() &&
               removed_projectiles.empty() && effects.empty() && windup_commits.empty() &&
               !has_terrain_patch;
    }
};

// Per-(client, chunk) tracking state — "what does this client already have for this chunk." RFC-015
// §3.4 requires any chunk-keyed structure this RFC introduces to adopt RFC-014 §4's sparse two-tier
// addressing, never a `kChunkCount`-sized dense array. This table is `unordered_map`-keyed by
// `chunk_key()` (already sparse — sized to whatever's actually subscribed, at most the 25-chunk
// interest set) rather than replicating `SnapshotBus`'s persistent-array-plus-instance-block split:
// that split exists to serve MANY concurrent `ChunkActor` writers across the whole world; a single
// client's tracking table has exactly one writer (this session's own `advance()`) and a working set
// two orders of magnitude smaller than the world, so a plain sparse map already satisfies the
// underlying requirement — never a dense array sized to the world — without the concurrency
// machinery `SnapshotBus` needs for a reason this table does not share.
struct ChunkTrackState {
    ChunkCoord coord{};
    std::array<std::uint8_t, static_cast<std::size_t>(kChunkTiles) * kChunkTiles> terrain{};
    std::unordered_map<std::uint32_t, PublishedCreature> creatures;
    std::unordered_map<std::uint32_t, PublishedProjectile> projectiles;
    std::uint64_t last_sent_tick = 0;  // last tick this chunk was forwarded at full fidelity —
                                        // drives the outer band's kOuterBandSendPeriodTicks cadence
};

// One client's replication session: computes, each tick, exactly what that client would receive.
// Owns no socket and sends nothing anywhere — `advance()` returns the frame a transport layer (P6,
// not built here) would actually serialize and put on the wire.
class ReplicationSession {
public:
    struct Frame {
        std::vector<ChunkBaseline> baselines;
        std::vector<ChunkDelta> deltas;
        std::size_t bytes_sent = 0;
        std::size_t chunks_culled = 0;  // outer-band/non-windup content dropped this tick by §5
    };

    [[nodiscard]] std::size_t tracked_chunk_count() const noexcept { return tracked_.size(); }

    // §1/§4/§5/§7 composed into one call. `bus` is read-only; nothing here mutates simulation state.
    [[nodiscard]] Frame advance(const SnapshotBus& bus, MapId new_map, ChunkCoord new_home,
                                 std::uint64_t tick) {
        Frame frame;
        // §7: a map change (a Teleport, RFC-014's instance crossing) is a full unsubscribe/
        // resubscribe, never a partial diff — there is no meaningful relationship between an
        // overworld window and a freshly-primed instance's window, and an instanced MapId is never
        // reused within a session (RFC-014 §3.1), so even a matching (cx,cy) means nothing carries
        // over.
        const bool map_changed = !has_subscribed_ || new_map != map_;
        if (map_changed) tracked_.clear();
        map_ = new_map;
        home_ = new_home;
        has_subscribed_ = true;

        const std::vector<ChunkCoord> interest = client_interest_set(new_map, new_home);

        if (!map_changed) {
            // Ordinary walking: drop tracking for chunks that left the window (§1's "small" shift —
            // at most one new row/column per chunk boundary crossed).
            std::unordered_map<std::uint64_t, bool> keep;
            keep.reserve(interest.size());
            for (const ChunkCoord& c : interest) keep[chunk_key(c)] = true;
            for (auto it = tracked_.begin(); it != tracked_.end();) {
                if (keep.find(it->first) == keep.end()) {
                    it = tracked_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const ChunkCoord& c : interest) {
            const std::uint64_t key = chunk_key(c);
            ChunkViewPtr view = bus.load(c);
            if (view == nullptr) continue;  // not primed/ticked yet — nothing to subscribe to

            auto it = tracked_.find(key);
            if (it == tracked_.end()) {
                ChunkTrackState st;
                st.coord = c;
                ChunkBaseline b = build_baseline(*view, st);
                st.last_sent_tick = tick;
                tracked_.emplace(key, std::move(st));
                frame.baselines.push_back(std::move(b));
                continue;
            }

            ChunkTrackState& st = it->second;
            const int dist = chebyshev_distance(c, home_);
            const bool cadence_due = dist <= 1 || (tick - st.last_sent_tick >= kOuterBandSendPeriodTicks);
            ChunkDelta d = compute_delta(*view, st, cadence_due);
            d.distance = dist;
            if (!d.empty()) {
                if (cadence_due) st.last_sent_tick = tick;
                frame.deltas.push_back(std::move(d));
            }
        }

        apply_budget(frame);
        return frame;
    }

private:
    // §3.1: full baseline for a chunk newly entering the interest set — terrain plus every current
    // record, capped exactly as a delta would be (§5's worst-case arithmetic assumes the cap applies
    // uniformly, not just steady-state).
    [[nodiscard]] static ChunkBaseline build_baseline(const ChunkView& view, ChunkTrackState& st) {
        ChunkBaseline b;
        b.coord = view.coord;
        b.tick = view.tick;
        for (std::size_t i = 0; i < st.terrain.size(); ++i) {
            b.terrain[i] = view.terrain[i];
            st.terrain[i] = view.terrain[i];
        }
        std::size_t n = 0;
        for (const Creature& c : view.creatures) {
            if (n >= static_cast<std::size_t>(kWireCreatureCapPerChunk)) break;
            ++n;
            const PublishedCreature pc = publish_of(c);
            b.creatures.push_back(pc);
            st.creatures.emplace(c.id, pc);
            if (pc.windup > 0) b.windup_commits.push_back(WindupCommit{c.id, c.windup_x, c.windup_y});
        }
        n = 0;
        for (const Projectile& pr : view.shots) {
            if (n >= static_cast<std::size_t>(kWireProjectileCapPerChunk)) break;
            ++n;
            const PublishedProjectile pp = publish_of(pr);
            b.projectiles.push_back(pp);
            st.projectiles.emplace(pr.id, pp);
        }
        for (const Effect& e : view.effects) b.effects.push_back(publish_of(e));
        return b;
    }

    // §3.2/§3.3/§4: id-keyed diff against tracked state. `cadence_due` gates everything EXCEPT the
    // §4 hard exception (a nonzero windup — including the commit and the record that carries it —
    // always forwards) and §3.3/§9's reliable class (removed entries, terrain patch). A record that
    // changed but was throttled this tick is deliberately NOT committed into `st` — it stays stale
    // there on purpose, so the diff persists and is re-offered the next cadence-due (or inner-band,
    // every-tick) pass rather than silently forgotten.
    [[nodiscard]] static ChunkDelta compute_delta(const ChunkView& view, ChunkTrackState& st,
                                                   bool cadence_due) {
        ChunkDelta d;
        d.coord = view.coord;
        d.tick = view.tick;

        std::unordered_map<std::uint32_t, bool> seen;
        seen.reserve(view.creatures.size());
        std::size_t n = 0;
        for (const Creature& c : view.creatures) {
            if (n >= static_cast<std::size_t>(kWireCreatureCapPerChunk)) break;
            ++n;
            seen[c.id] = true;
            const PublishedCreature pc = publish_of(c);
            auto prev = st.creatures.find(c.id);
            const bool is_new = (prev == st.creatures.end());
            const bool windup_started =
                pc.windup > 0 && (is_new || prev->second.windup == 0);
            if (windup_started) d.windup_commits.push_back(WindupCommit{c.id, c.windup_x, c.windup_y});

            const bool changed = is_new || !published_creature_equal(prev->second, pc);
            const bool must_send = pc.windup > 0;  // §4: never subject to outer-band throttling
            if (changed && (cadence_due || must_send)) {
                if (is_new) {
                    d.added_creatures.push_back(pc);
                } else {
                    d.updated_creatures.push_back(pc);
                }
                st.creatures[c.id] = pc;
            }
        }
        for (auto it = st.creatures.begin(); it != st.creatures.end();) {
            if (seen.find(it->first) == seen.end()) {
                d.removed_creatures.push_back(it->first);  // §3.3: always reliable, never throttled
                it = st.creatures.erase(it);
            } else {
                ++it;
            }
        }

        std::unordered_map<std::uint32_t, bool> pseen;
        pseen.reserve(view.shots.size());
        n = 0;
        for (const Projectile& pr : view.shots) {
            if (n >= static_cast<std::size_t>(kWireProjectileCapPerChunk)) break;
            ++n;
            pseen[pr.id] = true;
            const PublishedProjectile pp = publish_of(pr);
            auto prev = st.projectiles.find(pr.id);
            const bool is_new = (prev == st.projectiles.end());
            const bool changed = is_new || !published_projectile_equal(prev->second, pp);
            if (changed && cadence_due) {
                if (is_new) {
                    d.added_projectiles.push_back(pp);
                } else {
                    d.updated_projectiles.push_back(pp);
                }
                st.projectiles[pr.id] = pp;
            }
        }
        for (auto it = st.projectiles.begin(); it != st.projectiles.end();) {
            if (pseen.find(it->first) == pseen.end()) {
                d.removed_projectiles.push_back(it->first);
                it = st.projectiles.erase(it);
            } else {
                ++it;
            }
        }

        // Effects carry no id — best-effort, dropped entirely on a throttled tick rather than queued.
        if (cadence_due) {
            for (const Effect& e : view.effects) d.effects.push_back(publish_of(e));
        }

        // §2.6: terrain is sent once (baseline) and again only as a targeted single-tile delta — rare
        // and reliable, so unconditional on band. One patch per tick, matching "rare, single-tile."
        for (std::size_t i = 0; i < st.terrain.size(); ++i) {
            if (view.terrain[i] == st.terrain[i]) continue;
            d.has_terrain_patch = true;
            d.patch_tx = static_cast<std::uint16_t>(i % static_cast<std::size_t>(kChunkTiles));
            d.patch_ty = static_cast<std::uint16_t>(i / static_cast<std::size_t>(kChunkTiles));
            d.patch_tile = view.terrain[i];
            st.terrain[i] = view.terrain[i];
            break;
        }

        return d;
    }

    // §5: if the frame's steady-state total exceeds the per-client budget, cull farthest-first —
    // first the outer band's added/updated PROJECTILE content, then non-windup CREATURE content —
    // and never `removed_*`, `windup_commits`, or a terrain patch (§9's reliable class, and §4's
    // hard exception). A simplification versus §5's literal text: this does not additionally weight
    // "outer band" as its own culling pass separate from distance — `distance` (Chebyshev, farthest
    // first) already captures it, since every outer-band chunk has a strictly greater distance than
    // every inner-band chunk. Flagged alongside Open Questions 1/6 as unmeasured against real traffic.
    static void apply_budget(Frame& frame) {
        std::size_t total = 0;
        for (const ChunkDelta& d : frame.deltas) total += d.byte_estimate();
        if (total <= kPerTickDeltaBudget) {
            frame.bytes_sent = total;
            return;
        }

        std::vector<std::size_t> order(frame.deltas.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&frame](std::size_t a, std::size_t b) {
            return frame.deltas[a].distance > frame.deltas[b].distance;
        });

        for (std::size_t i : order) {
            if (total <= kPerTickDeltaBudget) break;
            ChunkDelta& d = frame.deltas[i];
            const std::size_t freed = d.added_projectiles.size() * sizeof(PublishedProjectile) +
                                      d.updated_projectiles.size() * sizeof(PublishedProjectile);
            if (freed > 0) {
                total -= freed;
                ++frame.chunks_culled;
                d.added_projectiles.clear();
                d.updated_projectiles.clear();
            }
        }
        for (std::size_t i : order) {
            if (total <= kPerTickDeltaBudget) break;
            ChunkDelta& d = frame.deltas[i];
            bool culled_any = false;
            auto cull_non_windup = [&](std::vector<PublishedCreature>& v) {
                std::vector<PublishedCreature> kept;
                kept.reserve(v.size());
                for (PublishedCreature& c : v) {
                    if (total > kPerTickDeltaBudget && c.windup == 0) {
                        total -= sizeof(PublishedCreature);
                        culled_any = true;
                        continue;
                    }
                    kept.push_back(c);
                }
                v = std::move(kept);
            };
            cull_non_windup(d.added_creatures);
            cull_non_windup(d.updated_creatures);
            if (culled_any) ++frame.chunks_culled;
        }
        frame.bytes_sent = total;
    }

    bool has_subscribed_ = false;
    MapId map_ = 0;
    ChunkCoord home_{};
    std::unordered_map<std::uint64_t, ChunkTrackState> tracked_;
};

}  // namespace mmo
