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
//     map it's read on") — no such symbol existed in `world/` before. `render/raylib_bridge.cpp`
//     keeps its own, separate, translation-unit-local `Weather`/`weather_of` (same four values, same
//     mapping, anonymous namespace, never exported) for its particle-layer selection; unifying the
//     two is a future rendering change this RFC's own Non-goals excludes ("Rendering... out of this
//     RFC"), not a regression introduced here — the render copy simply predates this one.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

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

enum class PortalKind : std::uint8_t {
    kInteriorDoor,
    kRealmGate,
    kMineMouth,
    kMissionPortal,
    kReturnPortal,
};
enum class RealmType : std::uint8_t { kRest, kChallenge };
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

}  // namespace mmo
