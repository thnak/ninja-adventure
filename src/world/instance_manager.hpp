// RFC-014 — Instance & Realm Lifecycle. `allocate_new()`'s internals: RFC-022 §2.4 named this as
// entirely out of its own scope ("allocate_new's internals... — RFC-014").
//
// DELIBERATE DIVERGENCE FROM THE RFC TEXT, the significant one, documented once here rather than
// scattered:
//
// `InstanceManager` is NOT wired as a real `quark::Actor<..., Require<Trusted>>` this pass. The RFC
// specifies it that way (§2) so allocation decisions are leader-observed facts once cross-node
// placement exists (P6) — the identical reasoning `MapDirector`/`PlayerActor` already follow. This
// class is instead a plain object `World` owns and drives synchronously, for two concrete reasons:
//
//   1. Turning it into a real actor would require `allocate_new()` — which must `tell()` a
//      `PrimeInstanceChunk` to every one of an instance's `chunk_edge²` coordinates and then BLOCK
//      until each has answered (the priming-completion barrier, RFC-014 Open Question 8) — to `ask`
//      other actors FROM WITHIN its own message handler. Whether `quark::block_on` is safe to call
//      re-entrantly from inside a `Sequential` actor's own handler (as opposed to from `World`'s
//      already-external, non-actor context, where every other multi-step verb in this codebase
//      already does exactly this) is a genuine, unverified question this pass does not have the
//      engine-source confidence to answer either way — and RFC-014 itself does not commit to an
//      answer (§3.6, Open Question 8: "does not commit to it as final").
//   2. `World` already IS this codebase's "leader," in the only topology that exists to test against
//      today — its own header comment states the plan plainly: "SINGLE PROCESS TODAY, N PROCESSES
//      LATER." Every other cross-cutting decision `World` makes (`login()`'s slot assignment,
//      `swing()`'s ask-then-tell ordering) is likewise a plain synchronous method, not a message
//      handler, and upgrading any of them to a real distributed actor is future, P6-scoped work, not
//      a redesign forced by this file.
//
// What IS built for real: the genuine QuarkCpp `declare_lazy<A>()`/`IdleTimeout<Ms>` primitive
// (verified present and working against the actual engine source before committing to it — see
// `chunk_actor.hpp`'s own header note), the sparse two-tier `SnapshotBus` (`snapshot.hpp`), the
// session state machine and membership bookkeeping (`map_system.hpp`), and the priming barrier
// (below, via the ALREADY-EXISTING `Ask<GetChunkStats,...>` every `ChunkActor` answers — reused
// rather than inventing a new `PrimeAck` ask type RFC-014 §3.6 sketches but does not commit to).
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

#include "world/chunk_actor.hpp"
#include "world/map_director.hpp"
#include "world/map_system.hpp"
#include "world/protocol.hpp"

namespace mmo {

class InstanceManager {
public:
    // Wired once at bring-up by `World::build()`, mirroring `MapDirector`'s own wiring pattern.
    // `director_ref` can only be set AFTER `MapDirector` itself has registered (it is looked up by
    // key, `router->get<MapDirector>(kDirectorKey)`), so `World::build()` wires it last.
    quark::LocalRouter* router = nullptr;
    quark::ActorRef<MapDirector> director_ref{};
    SnapshotBus* bus = nullptr;
    std::uint64_t world_seed = 0;

    // §3.1: monotonic, never reused within one process's uptime (65,520-wide band; see the RFC's
    // own arithmetic on why wraparound is not designed for at this project's scale).
    [[nodiscard]] MapId next_map_id() noexcept {
        const MapId id = next_map_id_;
        next_map_id_ = (next_map_id_ == 65535) ? kPersistentBandEnd : static_cast<MapId>(next_map_id_ + 1);
        return id;
    }

    [[nodiscard]] std::size_t open_session_count() const noexcept { return sessions_.size(); }

    // §2, §3: turns a `kNeedsAllocation` outcome from `resolve_portal()` (RFC-022 §2.3) into a real,
    // primed `MapSession`. `descriptor.id` is overwritten with the freshly drawn `MapId` — the
    // caller-supplied descriptor only needs to carry `chunk_edge`/`biome`/`weather_mode`/etc; WHICH
    // map it becomes is this call's job, not the caller's (mirrors RFC-022's own `fixed_to_map`
    // pattern: the id is resolved here, never chosen upstream). Returns `nullptr` if
    // `kMaxConcurrentInstances` (a bug guard, not a resource limit — see `map_system.hpp`) is
    // already at capacity.
    [[nodiscard]] MapSession* allocate_new(const PortalDef& portal, GroupId owner_group,
                                            MapDescriptor descriptor, AccountId requester) {
        if (sessions_.size() >= kMaxConcurrentInstances) return nullptr;
        if (router == nullptr || bus == nullptr) return nullptr;

        const MapId id = next_map_id();
        descriptor.id = id;
        descriptor.category = MapCategory::kInstanced;
        // RFC-018 §13: the one small field this RFC named as a real, unassumed gap — closed here,
        // at the one place a fresh instance's descriptor is actually built, from the same `portal`
        // this call already receives. No new lookup, no new field on `PortalDef`.
        descriptor.origin_kind = portal.kind;
        descriptor.origin_realm_type = portal.realm_type;

        InstanceSession is;
        is.session.map_id = id;
        is.session.origin_portal = portal.id;
        is.session.scope = portal.scope;
        is.session.owner_group = owner_group;
        is.session.return_map = portal.from_map;
        is.session.return_x = portal.from_x;
        is.session.return_y = portal.from_y;
        is.state = SessionState::kAllocating;
        is.chunk_edge = descriptor.chunk_edge;
        is.members.push_back(requester);
        is.present.push_back(requester);

        bus->register_instance_block(id, descriptor.chunk_edge);

        // §3.3: prime the whole instance eagerly, at session creation — a player should never watch
        // a room materialize as they walk toward it. tell() every coordinate (forces lazy
        // activation), then ask<ChunkStats> each as the completion barrier (Open Question 8): an
        // answer proves that coordinate's ChunkActor is constructed, wired, and draining its mailbox
        // before this call hands back a Teleport-able session.
        std::vector<ChunkCoord> coords;
        coords.reserve(static_cast<std::size_t>(descriptor.chunk_edge) * descriptor.chunk_edge);
        for (int cy = 0; cy < descriptor.chunk_edge; ++cy) {
            for (int cx = 0; cx < descriptor.chunk_edge; ++cx) {
                coords.push_back(ChunkCoord{id, static_cast<std::uint16_t>(cx),
                                            static_cast<std::uint16_t>(cy)});
            }
        }
        for (const ChunkCoord& c : coords) {
            PrimeInstanceChunk p{};
            p.coord = c;
            p.descriptor = descriptor;
            p.seed = world_seed;
            router->get<ChunkActor>(chunk_key(c)).tell(p);
        }
        for (const ChunkCoord& c : coords) {
            (void)quark::block_on(router->get<ChunkActor>(chunk_key(c)).ask<ChunkStats>(GetChunkStats{}));
        }

        // §3.4: the new map's coordinates join MapDirector's per-tick fan-out ONLY once priming has
        // actually finished — a coordinate ticking before its ChunkActor exists would just dead-letter,
        // but sequencing it after the barrier above is the correct, not merely harmless, order.
        director_ref.tell(FanOutAdd{coords});

        is.state = SessionState::kActive;
        auto [it, inserted] = sessions_.emplace(id, std::move(is));
        (void)inserted;
        return &it->second.session;
    }

    [[nodiscard]] InstanceSession* find_session(MapId id) noexcept {
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const std::vector<MapSession>& live_sessions_for_resolve() {
        live_cache_.clear();
        for (const auto& [id, s] : sessions_) {
            if (!instance_session_closing(s.state)) live_cache_.push_back(s.session);
        }
        return live_cache_;
    }

    // §6 Join: called once `resolve_portal()` (RFC-022) has returned a `kFound` session for an
    // instanced (non-`kSharedPersistent`) `MapId`.
    void enter(MapId id, AccountId account) {
        InstanceSession* s = find_session(id);
        if (s == nullptr) return;
        if (std::find(s->members.begin(), s->members.end(), account) == s->members.end()) {
            s->members.push_back(account);
        }
        if (std::find(s->present.begin(), s->present.end(), account) == s->present.end()) {
            s->present.push_back(account);
        }
        s->idle_since_ms = -1;
        if (s->state == SessionState::kIdle) s->state = SessionState::kActive;
    }

    // §6 Deliberate exit / Disconnect: both remove `account` from `present` only — `members` is
    // unchanged either way (a player who stepped through the return portal, or whose connection
    // dropped, is still "of" this run; only a closed/torn-down session forgets them for good).
    void leave_present(MapId id, AccountId account) {
        InstanceSession* s = find_session(id);
        if (s == nullptr) return;
        auto it = std::find(s->present.begin(), s->present.end(), account);
        if (it != s->present.end()) s->present.erase(it);
    }

    // §3.5: called on a coarse cadence (once per DirectorTick's own day/night check, not once per
    // simulation tick — the caller's choice, this method does no rate-limiting of its own). Ages
    // ACTIVE sessions with an empty `present` into IDLE->TEARING_DOWN, and immediately finalizes a
    // TEARING_DOWN session to CLOSED (§3.6: this pass has no separate "wait for chunk actors to
    // finish idling out" step to observe, since it does not track per-chunk deactivation — the
    // SnapshotBus block is released at the same moment the fan-out is cut, matching the RFC's own
    // "this timer alone never frees any memory, it only stops new traffic" framing for the session
    // level; actual chunk-actor memory reclamation is the engine's own IdleTimeout wheel, invisible
    // to this class by design).
    void sweep_idle(std::int64_t world_ms) {
        for (auto& [id, s] : sessions_) {
            if (s.state != SessionState::kActive && s.state != SessionState::kIdle) continue;
            if (!s.present.empty()) {
                s.idle_since_ms = -1;
                if (s.state == SessionState::kIdle) s.state = SessionState::kActive;
                continue;
            }
            if (s.idle_since_ms < 0) {
                s.idle_since_ms = world_ms;
                s.state = SessionState::kIdle;
                continue;
            }
            if (world_ms - s.idle_since_ms >= kInstanceIdleGraceMs) {
                s.state = SessionState::kTearingDown;
            }
        }
        // A second pass: finalize every TEARING_DOWN session found above (or left over from a prior
        // sweep) to CLOSED, releasing its SnapshotBus block and cutting MapDirector's fan-out. Kept
        // as its own pass so a caller can observe the TEARING_DOWN state (e.g. for a save hook, RFC-
        // 016's territory) before this class forgets the session entirely.
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second.state != SessionState::kTearingDown) {
                ++it;
                continue;
            }
            const MapId id = it->first;
            const std::uint8_t edge = it->second.chunk_edge;
            std::vector<ChunkCoord> coords;
            coords.reserve(static_cast<std::size_t>(edge) * edge);
            for (int cy = 0; cy < edge; ++cy) {
                for (int cx = 0; cx < edge; ++cx) {
                    coords.push_back(
                        ChunkCoord{id, static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy)});
                }
            }
            director_ref.tell(FanOutRemove{coords});
            if (bus != nullptr) bus->release_instance_block(id);
            it->second.state = SessionState::kClosed;
            it = sessions_.erase(it);
        }
    }

private:
    std::unordered_map<MapId, InstanceSession> sessions_;
    std::vector<MapSession> live_cache_;
    MapId next_map_id_ = kPersistentBandEnd;
};

}  // namespace mmo
