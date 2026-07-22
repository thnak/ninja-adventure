// Every message in the game, in one place.
//
// These are the types that will cross the wire once chunks are placed on other machines, so they are
// deliberately POD-shaped: fixed-width fields, no pointers, no owning containers. Adding
// `QUARK_SERIALIZE(T, ...)` to each is all that is needed to make a message remotable (spec 016) —
// which is why the migration path from single-node to cluster touches no handler logic.
//
// EVERY PLAYER VERB CARRIES A `player` KEY. Not because there are two players yet, but because
// adding the field later means touching every handler that reads it — ROADMAP principle 2. The
// same reasoning is why `PlayerActor` is keyed and `World`'s verbs take the key explicitly instead
// of assuming "the" player.
#pragma once

#include <cstdint>

#include "world/tiles.hpp"

namespace mmo {

// --- Chunk messages ------------------------------------------------------------------------------

// The simulation heartbeat. MapDirector fans one of these to every chunk AND to every player actor
// per tick — one clock for the whole world, so a creature and the player it is chasing never step
// out of phase with each other.
struct Tick {
    std::uint64_t tick = 0;
    std::int64_t world_ms = 0;
    bool night = false;
};

// A creature crossed a chunk boundary. The sending chunk has already removed it; the receiving chunk
// adopts it verbatim. This single message is the entire hand-off protocol — there is no
// transfer/ack/commit dance, because per-(sender,receiver) FIFO plus at-most-one-owner makes the
// naive version correct.
struct CreatureEnter {
    Creature creature{};
};

// An arrow crossed a chunk boundary. Identical hand-off, deliberately: a projectile is just a thing
// that moves and is owned by whichever chunk it is over, so it reuses the mechanism rather than
// inventing a second one.
struct ProjectileEnter {
    Projectile shot{};
};

// The director asks the chunk that owns a stronghold to release `count` creatures around it. The
// tile travels with the message rather than being recomputed here: the chunk would get the same
// answer, but sending it keeps the chunk from having to know which of its tiles is a stronghold.
struct SpawnWave {
    std::uint16_t count = 0;
    std::uint32_t seed = 0;
    std::uint8_t kind = 0;  // CreatureKind
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
    BuildKind kind = BuildKind::kHearth;
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

// --- Combat --------------------------------------------------------------------------------------

// Where a player is, told to the 3x3 chunks around them.
//
// WHY A BEACON AND NOT AN `ask`: a creature deciding whom to chase needs the player's position
// every tick. Asking the (trusted, possibly remote) PlayerActor for it would put a synchronous
// cross-actor — eventually cross-machine — read on the movement hot path of every creature in the
// world. Pushing it the other way costs nine tells every few ticks per player, total.
//
// It is deliberately SOFT STATE with a lease: an entry the chunk has not heard about for
// `kBeaconLease` ticks is dropped. Nothing has to send a "player left" message, a lost beacon
// self-heals, and a chunk whose owner node died and was re-placed simply learns the roster again on
// the next beat. This is ARP, and it is the right shape for the same reason ARP is.
struct PlayerBeacon {
    std::uint64_t player = 0;
    std::uint16_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    std::uint64_t tick = 0;
};

inline constexpr std::uint64_t kBeaconPeriod = 3;  // ticks between beacons
inline constexpr std::uint64_t kBeaconLease = 12;  // ticks a chunk keeps one without hearing again

// A swing, resolved by the chunk the player is standing in. Damage arrives ALREADY COMPUTED by the
// trusted PlayerActor (see `PlanAttack`) — the chunk is tier B and must not be the thing that
// decides how hard the player hits.
struct MeleeSwing {
    float x = 0.0f;
    float y = 0.0f;
    Facing facing = Facing::kDown;
    float reach = kMeleeReach;
    std::int16_t damage = 0;
    bool heavy = false;
    std::uint64_t player = 0;
};

// A spell landing on a point. Sets a status; the damage is secondary — the point of a school is the
// status it leaves behind for a physical blow to detonate.
struct CastSpell {
    float x = 0.0f;
    float y = 0.0f;
    Element element = Element::kFire;
    float radius = kSpellRadius;
    std::int16_t damage = 0;
    std::uint64_t player = 0;
};

// Create an arrow. The chunk owns it from here (see `Projectile`).
struct LaunchArrow {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    std::int16_t damage = 0;
    std::uint64_t player = 0;
};

// Ask: a compact per-chunk tally. Used by tests and the headless runner as a FIFO barrier — an
// answer to this proves every Tick posted before it has already been drained.
struct ChunkStats {
    std::uint32_t creatures = 0;
    std::uint32_t hostile = 0;    // of those, how many are currently willing to fight the player
    std::uint32_t afflicted = 0;  // ... and how many are carrying an elemental status
    std::uint32_t projectiles = 0;
    std::uint32_t watchers = 0;   // players whose beacon this chunk currently holds
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
    std::uint32_t source = 0;  // the creature id, for the record; 0 for environmental damage
};

// A combo, or a kill, giving something back. Kept as one message rather than three because all
// three are "the world hands the trusted actor a positive number" and the actor clamps them the
// same way.
struct GrantVitals {
    std::int16_t hp = 0;
    std::int16_t mana = 0;
    std::int16_t stamina = 0;
};

// Experience, awarded by whichever chunk resolved the kill. Which skill it lands in is decided by
// HOW the creature died, not by what killed it — that is the whole of "you level what you use".
struct GrantXp {
    Skill skill = Skill::kMelee;
    std::uint32_t amount = 0;
};

// Lighting a hearth moves where you wake up.
struct SetRespawn {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
};

// Bind an account to this session slot. Sent once, at login, before the actor does anything else.
// Until it arrives the actor is inert: it does not beacon, does not regenerate and is not drawn.
struct BindAccount {
    std::uint32_t account = 0;
    std::uint16_t spawn_tx = 0;
    std::uint16_t spawn_ty = 0;
    std::int32_t wood = 0;
    std::int32_t stone = 0;
    std::int32_t seed = 0;
};

// What kind of blow the player is trying to land. The answer comes back from the TRUSTED actor,
// because "may I swing, and how hard" is exactly the pair a compromised chunk host must not get to
// answer for itself — the same check-and-debit shape as SpendItems, applied to stamina and mana.
enum class AttackKind : std::uint8_t { kLight = 0, kHeavy = 1, kShoot = 2, kCast = 3 };

struct PlanAttack {
    AttackKind kind = AttackKind::kLight;
    Element element = Element::kNone;  // only read for kCast
};

struct AttackPlan {
    bool ok = false;
    std::int16_t damage = 0;
    float reach = 0.0f;
    Element element = Element::kNone;
    Facing facing = Facing::kDown;
    float x = 0.0f;  // where the actor believes the player is — the client is not asked
    float y = 0.0f;
};

struct GetPlayer {};

// Toggle a mount. Travel across a 1024x1024 map is a design problem, not a convenience: the
// diagonal is nearly four minutes on foot. Riding costs nothing but forbids attacking, which is the
// trade that keeps it from simply being "the walk speed, but correct".
struct SetMounted {
    bool mounted = false;
};

// --- Director messages ---------------------------------------------------------------------------

struct DirectorTick {
    std::int64_t dt_ms = 0;
};

// A chunk reports creatures killed back to the director, which owns the world tally.
struct ReportKills {
    std::uint32_t killed = 0;
};

struct ReportMigration {
    std::uint32_t count = 0;
};

struct GetWorldTick {};

}  // namespace mmo
