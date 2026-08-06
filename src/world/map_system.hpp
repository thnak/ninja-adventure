// RFC-022 — Map System.
//
// Parts (1) and (3) RECORD existing shipped behavior (the `MapId` partition narrows
// `ARCHITECTURE.md §4`'s own sketch; the structure/portal boundary restates what `village.hpp`
// and `tiles.hpp::Door` already do). Parts (2), (4), (5) are green-field: `Portal`, `MapSession`,
// the join-vs-create decision, and the village-fit formula are new here.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - `MapId` is a type ALIAS for the `std::uint16_t` every map field already is (`PlayerView::map`,
//     `ChunkCoord::map`, `Teleport::map`, ...) — introducing it does not migrate any existing
//     signature, matching this RFC's own §1.1 framing ("this partition widens what MapId may
//     CONTAIN; it does not... widen how the engine INDEXES by MapId today").
//   - The dense `chunk_index()`/`SnapshotBus`/`effect_tick` sparse-addressing redesign §1.1 names as
//     a named blocker is explicitly NOT built here — it is RFC-014's prerequisite, not this RFC's.
//     Nothing in this file allocates an instanced MapId; `resolve_portal()` below stops at the same
//     line §2.3 itself stops at ("here is the decision that precedes the call").
//   - `allocate_new` is not implemented. `resolve_portal()` returns `ResolveOutcome::kNeedsAllocation`
//     instead of a fresh `MapId` when no live session satisfies the join rule — RFC-014's
//     `InstanceManager` is the thing that turns that outcome into a real map. `Door`/`Teleport`
//     (`tiles.hpp`, `protocol.hpp`) are untouched, exactly as §2.1 says they may stay.
//   - `MapSession` carries no "full"/"closing" flag — the RFC's own `resolve()` pseudocode filters
//     `kGroupInstance` candidates on `not (full or closing)`, but capacity and teardown state are
//     RFC-014's `TEARING_DOWN` machinery (named in `IMPLEMENTATION_MAP.md`'s RFC-016 §7 cross-ref),
//     not specified by this RFC's own `MapSession` shape (§2.3). `resolve_portal()` below matches
//     any session on `(origin_portal, owner_group)` alone; a caller that has RFC-014's richer session
//     state filters before calling in, or RFC-014 extends the struct when it lands.
//   - `Weather`/`weather_of(Ring)` are defined here as the world-layer, simulation-facing value this
//     RFC's §5.4 point 4 describes ("combat-status interaction... driven by Weather, not by which
//     map it's read on") — no such symbol existed in `world/` before. `render/raylib_bridge.cpp` had
//     its own, separate, translation-unit-local `Weather`/`weather_of` (same four values, same
//     mapping) for its particle-layer selection; once this header's `mmo::Weather` became reachable
//     from that file (transitively, via `world.hpp`), the two names collided under MSVC's anonymous-
//     namespace lookup rules (a real build error, not a hypothetical) — the render copy was renamed
//     `FxWeather`/`fx_weather_of` to resolve it, a mechanical, same-file-only rename with no behavior
//     change. Unifying the two properly (one canonical `Weather` driving both simulation and FX) is a
//     future rendering change this RFC's own Non-goals excludes ("Rendering... out of this RFC").
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "world/account.hpp"
#include "world/tiles.hpp"
#include "world/village.hpp"

namespace mmo {

// --- §1.1: MapId partition --------------------------------------------------------------------------

using MapId = std::uint16_t;

inline constexpr MapId kPersistentBandEnd = 16;  // [0,16) persistent, [16,65536) instanced

[[nodiscard]] inline constexpr bool map_id_persistent(MapId id) noexcept {
    return id < kPersistentBandEnd;
}
[[nodiscard]] inline constexpr bool map_id_instanced(MapId id) noexcept {
    return id >= kPersistentBandEnd;
}

// --- §1.2: MapDescriptor and the one sizing rule ------------------------------------------------------

enum class MapCategory : std::uint8_t { kPersistent, kInstanced };

// Moved above `MapDescriptor` (RFC-022 §2's own portal-system section originally declared these
// further down) so RFC-018 §13's `origin_kind`/`origin_realm_type` fields below can name them —
// same values/meaning, not re-derived.
enum class PortalKind : std::uint8_t {
    kInteriorDoor,
    kRealmGate,
    kMineMouth,
    kMissionPortal,
    kReturnPortal,
};
enum class RealmType : std::uint8_t { kRest, kChallenge };

// §5.2. See header note: distinct from render/raylib_bridge.cpp's own TU-local Weather.
enum class Weather : std::uint8_t { kLeaves, kRain, kSnow, kNone };

[[nodiscard]] inline constexpr Weather weather_of(Ring r) noexcept {
    switch (r) {
        case Ring::kMeadow:
        case Ring::kForest: return Weather::kLeaves;
        case Ring::kWetland: return Weather::kRain;
        case Ring::kSnow: return Weather::kSnow;
        case Ring::kWasteland:
        case Ring::kCount: break;
    }
    return Weather::kNone;
}

enum class WeatherMode : std::uint8_t { kAmbient, kFixed, kInherit };

struct MapDescriptor {
    MapId id = 0;
    MapCategory category = MapCategory::kPersistent;
    std::uint8_t chunk_edge = 32;  // 1..32 — the map is chunk_edge x chunk_edge chunks
    Ring biome = Ring::kMeadow;    // a TAG (§5.1), not a computed gradient off the overworld
    WeatherMode weather_mode = WeatherMode::kAmbient;
    Weather weather_fixed = Weather::kNone;  // meaningful only when weather_mode == kFixed
    bool allow_free_build = true;
    // RFC-018 §13's requested addition: appended, never inserted, so the two aggregate-init
    // factories below (and every other positional `MapDescriptor{...}` call site) stay correct
    // with these left at their defaults. Meaningful only when category == kInstanced; a persistent
    // map (the overworld, an interior room) is never a realm gate's destination, so it is correctly
    // read as kInteriorDoor/kRest — never a challenge realm — everywhere else. Populated once, at
    // `InstanceManager::allocate_new()` time, from the triggering `PortalDef` (RFC-018 §13/§6.6:
    // the data an Essence/gem/boss-equipment realm gate needs, with no cross-actor lookup).
    PortalKind origin_kind = PortalKind::kInteriorDoor;
    RealmType origin_realm_type = RealmType::kRest;
};

[[nodiscard]] inline constexpr int edge_tiles(const MapDescriptor& d) noexcept {
    return static_cast<int>(d.chunk_edge) * kChunkTiles;
}
[[nodiscard]] inline constexpr int chunk_count(const MapDescriptor& d) noexcept {
    return static_cast<int>(d.chunk_edge) * static_cast<int>(d.chunk_edge);
}

// §1.3 table's first two (as-built) rows, described rather than invented — the overworld and
// interior grid are unchanged by this RFC, this just gives their existing shape a MapDescriptor.
[[nodiscard]] inline constexpr MapDescriptor overworld_descriptor() noexcept {
    return MapDescriptor{kOverworld,       MapCategory::kPersistent, static_cast<std::uint8_t>(kMapChunks),
                         Ring::kMeadow,    WeatherMode::kAmbient,    Weather::kNone,
                         /*allow_free_build*/ true};
}
[[nodiscard]] inline constexpr MapDescriptor interior_descriptor() noexcept {
    return MapDescriptor{kInterior,        MapCategory::kPersistent, static_cast<std::uint8_t>(kMapChunks),
                         Ring::kMeadow,    WeatherMode::kFixed,      Weather::kNone,
                         /*allow_free_build*/ false};
}
// The first hand-authored/generated persistent map (world/authored_map.hpp). `chunk_edge` is
// `kDojoAnnexChunkEdge` (2), not `kMapChunks` (32) like the two maps above — see that constant's
// own comment in tiles.hpp for why a small map must not claim the full chunk grid.
[[nodiscard]] inline constexpr MapDescriptor dojo_annex_descriptor() noexcept {
    return MapDescriptor{kDojoAnnex,       MapCategory::kPersistent,
                         static_cast<std::uint8_t>(kDojoAnnexChunkEdge), Ring::kMeadow,
                         WeatherMode::kFixed, Weather::kNone,
                         /*allow_free_build*/ false};
}

// world.hpp's build_chunks() calls this per map index so a small persistent map (kDojoAnnex) only
// gets the chunk actors it actually needs, instead of build_chunks() unconditionally spinning up
// kMapChunks x kMapChunks (1024) actors per map the way it always has for kOverworld/kInterior.
[[nodiscard]] inline constexpr int map_chunk_edge(int map) noexcept {
    if (map == static_cast<int>(kDojoAnnex)) return kDojoAnnexChunkEdge;
    return kMapChunks;
}

// --- §4: the village-always-fits invariant --------------------------------------------------------
// `full_width`/`full_height` use gates_of()'s ACTUAL `+3` approach-tile margin (village.hpp:182-189),
// not worldgen.hpp's current `+2` edge test — §4.1 names that one-tile gap explicitly (Open Q1) and
// says this RFC's formula, not the shipped edge test, is the one new tooling should build against.

[[nodiscard]] inline constexpr int village_full_width(int tier) noexcept {
    return 2 * (static_cast<int>(plan_of(tier).hw) + 3) + 1;
}
[[nodiscard]] inline constexpr int village_full_height(int tier) noexcept {
    return 2 * (static_cast<int>(plan_of(tier).hh) + 3) + 1;
}

// Strict inequality (§4.1): at least one valid centre position must have the whole footprint
// inside the map on all four sides.
[[nodiscard]] inline constexpr bool village_fits(int chunk_edge, int tier) noexcept {
    const int edge = chunk_edge * kChunkTiles;
    return edge > std::max(village_full_width(tier), village_full_height(tier));
}

// --- §2: the portal system --------------------------------------------------------------------------

using PortalId = std::uint32_t;
using GroupId = std::uint16_t;

enum class RealmFlavor : std::uint8_t {
    kNone,
    kDungeon,
    kTrial,
    kFishingLake,
    kHotSpring,
    kCloudIsle,
    kSpiritRealm,
};
enum class PortalBinding : std::uint8_t { kFixedTarget, kAllocateOnUse };
enum class SessionScope : std::uint8_t { kSharedPersistent, kGroupInstance, kSoloInstance };

// Named `PortalDef`, not the RFC's own `Portal` — `tiles.hpp:973` already ships an UNRELATED
// `Portal` (the resolved {map,tx,ty,valid} answer `portal_at()`'s door binary-search returns,
// consumed live by `player_actor.hpp`). Renaming that shipped struct to free the name was out of
// scope and unnecessary risk for a naming collision alone; this is the RFC's §2.2 data shape under
// a name that doesn't collide with it.
struct PortalDef {
    PortalId id = 0;
    MapId from_map = kOverworld;
    std::uint16_t from_x = 0, from_y = 0;
    PortalKind kind = PortalKind::kInteriorDoor;
    RealmType realm_type = RealmType::kRest;      // meaningful only when kind == kRealmGate
    RealmFlavor flavor = RealmFlavor::kNone;      // meaningful only when kind == kRealmGate
    PortalBinding binding = PortalBinding::kFixedTarget;
    SessionScope scope = SessionScope::kSharedPersistent;  // meaningful only when binding == kAllocateOnUse
    // kFixedTarget only:
    MapId fixed_to_map = kOverworld;
    std::uint16_t fixed_to_x = 0, fixed_to_y = 0;
};

// --- §2.3: session scope and the join-vs-create rule --------------------------------------------------

struct MapSession {
    MapId map_id = 0;
    PortalId origin_portal = 0;
    SessionScope scope = SessionScope::kSharedPersistent;
    GroupId owner_group = 0;  // 0 when scope == kSharedPersistent
    MapId return_map = kOverworld;
    std::uint16_t return_x = 0, return_y = 0;
};

enum class ResolveOutcome : std::uint8_t { kFound, kNeedsAllocation };

struct ResolveResult {
    ResolveOutcome outcome = ResolveOutcome::kNeedsAllocation;
    MapSession session{};  // valid only when outcome == kFound
};

// §2.3's resolve(), evaluated against an already-known set of live sessions (leader-owned state in
// the real game — see header note on `allocate_new`). A `kFixedTarget` portal never needs
// allocation: if no session is on record yet for its destination (first-ever use), one is
// synthesized here, since the destination and its return point are already fully known from the
// portal itself — there is nothing for RFC-014 to allocate in that case.
[[nodiscard]] inline ResolveResult resolve_portal(const PortalDef& portal,
                                                   const std::vector<MapSession>& live_sessions,
                                                   GroupId group) {
    if (portal.binding == PortalBinding::kFixedTarget) {
        for (const MapSession& s : live_sessions) {
            if (s.map_id == portal.fixed_to_map) return ResolveResult{ResolveOutcome::kFound, s};
        }
        MapSession s;
        s.map_id = portal.fixed_to_map;
        s.origin_portal = portal.id;
        s.scope = SessionScope::kSharedPersistent;
        s.owner_group = 0;
        s.return_map = portal.from_map;
        s.return_x = portal.from_x;
        s.return_y = portal.from_y;
        return ResolveResult{ResolveOutcome::kFound, s};
    }
    switch (portal.scope) {
        case SessionScope::kSharedPersistent:
            for (const MapSession& s : live_sessions) {
                if (s.origin_portal == portal.id) return ResolveResult{ResolveOutcome::kFound, s};
            }
            return ResolveResult{ResolveOutcome::kNeedsAllocation, {}};
        case SessionScope::kGroupInstance:
            for (const MapSession& s : live_sessions) {
                if (s.origin_portal == portal.id && s.owner_group == group) {
                    return ResolveResult{ResolveOutcome::kFound, s};
                }
            }
            return ResolveResult{ResolveOutcome::kNeedsAllocation, {}};
        case SessionScope::kSoloInstance:
            return ResolveResult{ResolveOutcome::kNeedsAllocation, {}};
    }
    return ResolveResult{ResolveOutcome::kNeedsAllocation, {}};
}

// ==================== RFC-014: Instance & Realm Lifecycle ==========================================
//
// This is `allocate_new()`'s own file split — RFC-022 §2.4 named `allocate_new`'s internals as
// entirely RFC-014's, and RFC-014 §1 opens by citing this file's `MapId`/`MapDescriptor`/`PortalDef`/
// `MapSession`/`resolve_portal()` as inherited, not reproduced. The pure data shapes below live here
// (alongside RFC-022's) so `InstanceManager` (`instance_manager.hpp`) stays free of struct
// definitions and reads as orchestration only, matching this session's `status.hpp`/`combat_math.hpp`
// split precedent (pure data+formulas in one header, actor wiring in another).
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, documented at point of use in `instance_manager.hpp`:
//   - `InstanceManager` is NOT wired as a real `quark::Actor<..., Require<Trusted>>` this pass — it
//     is a plain class `World` owns and calls synchronously, exactly the way `World` already drives
//     every other multi-step verb (`swing`, `cast`, `build_at`) through direct `block_on`/`tell`
//     calls rather than actor-to-actor messaging. See `instance_manager.hpp`'s header note for the
//     full reasoning (nested nested nested ask reentrancy is unverified; a single-process World
//     already IS this codebase's own "leader" today, per `world.hpp`'s own header comment).
//   - `declare_lazy<ChunkActor>()` + a `PrimeInstanceChunk` message (RFC-014 §3.2's own "Option 2")
//     ARE built for real — the genuine QuarkCpp primitive, verified present and working
//     (`engine.hpp`'s `declare_lazy`/`ActivationBroker`, `policies.hpp`'s `IdleTimeout<Ms>`) before
//     committing to it, not assumed from the RFC's prose alone.
//   - Priming-completion detection (RFC-014 Open Question 8) is resolved here as: `tell()` every
//     `PrimeInstanceChunk`, then `ask<ChunkStats>(GetChunkStats{})` (a message every `ChunkActor`
//     already answers) to each coordinate as the completion barrier — reusing existing protocol
//     rather than inventing a new `PrimeAck` ask type.
//   - Atlas/pack refcounting (§7) is built as the real, reusable `PackRefcount` table below, but is
//     NOT wired to any actual texture load/unload — no pack-id namespace exists yet (RFC-008 Q3,
//     unresolved) and rendering is out of this RFC's scope regardless. `pack_ref_add`/`pack_ref_release`
//     are pure, tested functions ready for a future RFC-008/render-layer caller.
//   - Disconnect/reconnect (§6) ship as the `Unbind` message (`protocol.hpp`) + a `World::login()`
//     resume branch (`world.hpp`) — real, but there is genuinely no client-facing network/disconnect
//     DETECTOR in this codebase (RFC-015's territory), so nothing calls `Unbind` automatically yet;
//     it is callable exactly the way `world.teleport_player()` already is (a real verb, driven by
//     tests/tools today, by a future network layer later).

enum class SessionState : std::uint8_t { kAllocating, kActive, kIdle, kTearingDown, kClosed };

// §3.5's two independent timers, and §5's default membership cap / §Multiplayer's safety valve —
// all four (tunable) per the RFC's own framing.
inline constexpr std::int64_t kInstanceIdleGraceMs = 300'000;     // 5 min — matches ARCHITECTURE.md §4
inline constexpr std::int64_t kInstanceChunkIdleTimeoutMs = 30'000;
inline constexpr std::size_t kInstanceMemberCap = 4;               // GAME.md §3: "nhóm 2-4"
inline constexpr std::size_t kMaxConcurrentInstances = 64;         // a bug guard, not a resource limit

// §5: the minimum membership bookkeeping a MapSession lifecycle needs, independent of how a "group"
// forms (RFC-022 Open Question 8, unowned). `kSoloInstance` sessions carry exactly one `members`
// entry forever; `kSharedPersistent` sessions use neither vector (§5's own carve-out).
struct InstanceSession {
    MapSession session{};
    SessionState state = SessionState::kAllocating;
    std::int64_t idle_since_ms = -1;  // -1 while present.size() > 0
    std::vector<AccountId> members;
    std::vector<AccountId> present;
    // The map's own chunk_edge (RFC-022 §1.2), kept so teardown (RFC-014 §3.4/§3.5) can recompute
    // the exact coordinate list `FanOutRemove` needs without a second lookup table.
    std::uint8_t chunk_edge = 0;
};

[[nodiscard]] inline bool instance_session_full(const InstanceSession& s) noexcept {
    return s.members.size() >= kInstanceMemberCap;
}

// §3.6: only TEARING_DOWN/CLOSED count as "closing" for resolve()'s join purposes — ACTIVE and IDLE
// both still accept a returning member of the same group, which is what makes the grace window
// meaningful (RFC-014 §3.6's own words).
[[nodiscard]] inline constexpr bool instance_session_closing(SessionState s) noexcept {
    return s == SessionState::kTearingDown || s == SessionState::kClosed;
}

// --- §4: sparse chunk-actor addressing --------------------------------------------------------------
// `persistent_index()`/`chunk_index()` (tiles.hpp) are UNCHANGED — the formula already works for any
// c.map in [0, kPersistentBandEnd) once the array it indexes is sized to that band, not to the live
// kMapCount. Only the instanced band needs a new scheme: one block per open session, sized to that
// map's own chunk_edge (RFC-022 §1.2), not to the global kMapChunks (32) — the second bug RFC-014's
// own Motivation §3 names (a naive "just widen the array" fix would still waste 32x the index space
// for every instance smaller than the overworld).
// `InstanceChunkBlock` itself (the slots array) lives in `snapshot.hpp` alongside `SnapshotBus` and
// `ChunkViewPtr` — defining it here would make this header depend on `snapshot.hpp` for
// `ChunkViewPtr` while `snapshot.hpp` depends on this header for `MapId`/`kPersistentBandEnd`, a
// cycle. Only the pure index arithmetic, which needs neither, lives here.
[[nodiscard]] inline constexpr int instance_local_index(ChunkCoord c,
                                                         std::uint8_t chunk_edge) noexcept {
    return static_cast<int>(c.cy) * static_cast<int>(chunk_edge) + static_cast<int>(c.cx);
}

// --- §7: per-realm atlas/pack refcounting, generalized from RFC-022 §5.4's single-instance framing -
// Keyed by pack id (RFC-008's namespace, not yet authored) rather than by MapId: two concurrent
// kGroupInstance runs of the same dungeon template share one pack load. A `std::uint32_t` stand-in
// for `PackId` — RFC-008 Q3 leaves the real id type/range unresolved; narrowing this to whatever
// RFC-008 lands is a type alias change, not a signature change, when that happens.
using PackId = std::uint32_t;

struct PackRefcount {
    std::unordered_map<PackId, std::uint32_t> counts;
};

// Returns true the first time this pack transitions 0 -> 1 (caller's cue to actually load it).
[[nodiscard]] inline bool pack_ref_add(PackRefcount& r, PackId pack) {
    std::uint32_t& n = r.counts[pack];
    ++n;
    return n == 1;
}

// Returns true the moment this pack transitions 1 -> 0 (caller's cue to actually free it). A
// release on a pack with no outstanding refs is a no-op (defensive against a double-release), not UB.
[[nodiscard]] inline bool pack_ref_release(PackRefcount& r, PackId pack) {
    auto it = r.counts.find(pack);
    if (it == r.counts.end() || it->second == 0) return false;
    --it->second;
    if (it->second == 0) {
        r.counts.erase(it);
        return true;
    }
    return false;
}

}  // namespace mmo
