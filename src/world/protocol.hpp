// Every message in the game, in one place.
//
// These are the types that will cross the wire once chunks are placed on other machines, so they are
// deliberately POD-shaped: fixed-width fields, no pointers, no owning containers. Adding
// `QUARK_SERIALIZE(T, ...)` to each is all that is needed to make a message remotable (spec 016) —
// which is why the migration path from single-node to cluster touches no handler logic.
#pragma once

#include <cstdint>

#include "world/tiles.hpp"

namespace mmo {

// --- Chunk messages ------------------------------------------------------------------------------

// The simulation heartbeat. MapDirector fans one of these to every chunk per tick.
struct Tick {
    std::uint64_t tick = 0;
    std::int64_t world_ms = 0;
    bool night = false;
};

// A mob crossed a chunk boundary. The sending chunk has already removed it; the receiving chunk
// adopts it verbatim. This single message is the entire hand-off protocol — there is no
// transfer/ack/commit dance, because per-(sender,receiver) FIFO plus at-most-one-owner makes the
// naive version correct.
struct MobEnter {
    Mob mob{};
};

// The director asks the chunk that owns a spawn camp to release `count` mobs around it. The camp
// tile travels with the message rather than being recomputed here: the chunk would get the same
// answer (camp positions are a pure function), but sending it keeps the chunk from having to know
// which of its tiles is a camp.
struct SpawnWave {
    std::uint16_t count = 0;
    std::uint32_t seed = 0;
    std::uint8_t kind = 0;
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    std::uint16_t radius = 3;
};

struct PlantCrop {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    CropKind kind = CropKind::kWheat;
    std::int64_t now_ms = 0;
    std::uint64_t player = 0;
};

struct PlaceBuilding {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    BuildKind kind = BuildKind::kWall;
    std::uint64_t player = 0;
};

// Upgrade the building on this tile one level. Cost is debited from the trusted inventory BEFORE
// this is sent, exactly like PlaceBuilding.
struct UpgradeBuilding {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    std::uint64_t player = 0;
};

// Turn a tile into farmland. This is the base-expansion verb: the starting apron is part of the
// terrain function, but everything beyond it is a chunk-owned overlay.
struct TillGround {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    std::uint64_t player = 0;
};

struct HarvestAt {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    std::uint64_t player = 0;
};

// Ask: a compact per-chunk tally. Used by tests and the headless runner as a FIFO barrier — an
// answer to this proves every Tick posted before it has already been drained.
struct ChunkStats {
    std::uint32_t mobs = 0;
    std::uint32_t crops = 0;
    std::uint32_t ripe = 0;
    std::uint32_t buildings = 0;
    std::uint32_t tilled = 0;
    std::uint32_t building_levels = 0;  // sum of levels — proves an upgrade landed
    std::uint64_t tick = 0;
};
struct GetChunkStats {};

// --- Player messages -----------------------------------------------------------------------------

struct MoveIntent {
    float dx = 0.0f;  // desired displacement in tiles, already scaled by dt
    float dy = 0.0f;
};

struct GrantItems {
    ItemKind kind = ItemKind::kWood;
    std::int32_t count = 0;
};

// Spending is an ASK, not a tell: the caller needs to know whether the player could afford it
// before the world commits to placing a building. The player actor is the single writer of the
// inventory, so the check-and-debit is atomic by virtue of being one sequential handler.
struct SpendItems {
    ItemKind kind = ItemKind::kWood;
    std::int32_t count = 0;
};

struct HurtPlayer {
    std::int16_t amount = 0;
};

struct GetPlayer {};

// --- Director messages ---------------------------------------------------------------------------

struct DirectorTick {
    std::int64_t dt_ms = 0;
};

// A chunk reports mobs killed / core damage back to the director, which owns the world tally.
struct ReportKills {
    std::uint32_t killed = 0;
};

struct ReportCoreDamage {
    std::int32_t core_hp = 0;
};

struct ReportMigration {
    std::uint32_t count = 0;
};

struct GetWorldTick {};

}  // namespace mmo
