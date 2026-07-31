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
#include <vector>

#include "world/abilities.hpp"
#include "world/ability_pipeline.hpp"
#include "world/combat_entity.hpp"
#include "world/map_system.hpp"
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

// RFC-019 §5.8's contribution ledger, carried across the SAME hand-off as `CreatureEnter` — as a
// second, small companion message rather than a field on `Creature` itself, because `Creature` is
// already sized exactly to QuarkCpp's `MessagePool::kMaxPayload` (192 bytes) with zero headroom
// (see tiles.hpp's `Creature` comment). Per-(sender,receiver) FIFO means this always arrives right
// after the `CreatureEnter` it follows, addressed by `creature_id` rather than embedded in it.
struct CreatureContribEnter {
    std::uint32_t creature_id = 0;
    Contribution entries[kMaxContributors]{};
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

// An ability's HIT, resolved by the chunk exactly like MeleeSwing/CastSpell: the damage arrives
// already computed by the trusted PlayerActor, and the SHAPE (a ring around the caster, or the one
// creature ahead of them) is spelled out so tier B only has to apply it, never decide it. Covers the
// three striking abilities — WhirlCleave (kRing), CrushBlow (kFront), ElementalNova (kRing + a
// status). Fanned to the 3x3 neighbourhood by the player's map, the same as a swing.
struct AbilityStrike {
    float x = 0.0f;
    float y = 0.0f;
    Facing facing = Facing::kDown;
    AbilityShape shape = AbilityShape::kRing;
    float radius = 0.0f;
    std::int16_t damage = 0;
    // CrushBlow's authored Stagger Power rider (RFC-002/009): on top of the derived contribution
    // any heavy blow carries, this pushes CrushBlow toward Knockdown in one or two hits — the same
    // ballpark the old flat `stun_ticks=20` gave, now paid for through the ladder like every other
    // status instead of being a bespoke flag.
    std::uint16_t stagger_power = 0;
    Element element = Element::kNone;   // Nova feeds this element's channel; kNone for melee
    Skill skill = Skill::kMelee;        // which skill the kill credits — "you level what you use"
    EffectKind fx = EffectKind::kSlash; // the flash the chunk publishes where it lands
    std::uint64_t player = 0;
};

// Spawn a CombatEntity (RFC-004). The chunk that owns the centre adopts it (see `CombatEntity`),
// steps it through arm -> active -> dying, and applies whatever collision/aura/vision the archetype
// carries. Replaces the old `SpawnZone`: `kSmokeCloud`/`kWaterPool` are what SmokeBomb/RainCall now
// spawn, with the same tier/fan-out rules SpawnZone had (see `entity_intersects_chunk`-style clipping
// in chunk_actor.hpp). `x/y` are the exact float spawn point, matching `Zone`'s own shape — the chunk
// tile-snaps them to a tile centre itself for a `Collision != kNone` kind (RFC-004 §4); non-blocking
// kinds (auras, smoke) keep the float point as-is, exactly as a zone did. `radius_override` is an
// aura/footprint radius override (0 = the archetype's own default); `boss_room` is RFC-005's
// eviction-exemption flag — a real field, inert until boss authoring exists (no ability sets it
// today).
struct SpawnEntity {
    EntityKind kind = EntityKind::kIceWall;
    float x = 0.0f;
    float y = 0.0f;
    Faction team = Faction::kPlayer;
    std::uint64_t owner = 0;
    float radius_override = 0.0f;
    bool boss_room = false;
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
    std::uint32_t entities = 0;   // CombatEntity instances (RFC-004) this chunk owns
    std::uint32_t scars = 0;      // terrain scars this chunk owns
    std::uint32_t effects = 0;    // live flashes — proves a verb/ability landed on THIS chunk
    std::uint32_t watchers = 0;   // players whose beacon this chunk currently holds
    std::uint32_t crops = 0;
    std::uint32_t ripe = 0;
    std::uint32_t buildings = 0;
    std::uint32_t tilled = 0;
    std::uint32_t building_levels = 0;  // sum of levels — proves an upgrade landed
    std::uint32_t patches = 0;    // RFC-010: live tile patches (burning/mud/ice) this chunk owns
    std::uint32_t burning = 0;    // ... of those, how many are kBurning specifically
    std::uint32_t fields = 0;     // RFC-010: live field states (earthquake) this chunk owns
    std::uint32_t telegraphs = 0;  // RFC-006: live telegraph records this chunk owns
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

// RFC-019 §5.6: an atomic respec at the player's own Hearth. `from`'s committed value (`xp_to_reach`
// of its current level) is reset to 0; 75% of that value lands in `to` immediately, through the
// ordinary GrantXp level-up path, so it is capped by `to`'s own ceiling and the global 34-point cap
// exactly like any other grant. Any banked, uncommitted XP in `from` is discarded outright, not
// refunded — see PlayerActor::handle(const RespecSkill&).
struct RespecSkill {
    Skill from = Skill::kMelee;
    Skill to = Skill::kMelee;
};

// RFC-019 §5.7: one unit of Essence spent against a branch's Tier IV gate (levels 18->20, 3 total).
// Acquisition/spend UI is RFC-018's (proposed) territory; this message is the shape RFC-018's
// eventual reward tables would call into — a one-time toggle per level-band, never a per-cast cost.
struct GrantEssence {
    Skill skill = Skill::kMelee;
    std::uint8_t amount = 1;
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
    // Which map the swing happens on. The trusted actor is the only thing that knows for certain —
    // the client's view can be a step stale across a doorway — so the world fans the verb to THIS
    // map's chunks rather than assuming the overworld. Interiors get combat because of this field.
    std::uint16_t map = 0;
};

// "May I use the ability in this slot, and how does it land?" — the ability layer's counterpart of
// PlanAttack, and check-and-debit for the same reason: stamina/mana and the per-slot cooldown are
// state a compromised chunk host must not be able to answer for itself. The client names only the
// SLOT (0 = A, 1 = B); which ability that is, whether the school is high enough, whether it is off
// cooldown and affordable — all of that the trusted actor decides. `element` is the caster's current
// school, read only by ElementalNova (exactly as PlanAttack's element is read only by a cast).
struct UseAbility {
    std::uint8_t slot = 0;
    Element element = Element::kNone;
    float aim_x = 0.0f;  // where the cursor is, in map tiles — used only to aim FanVolley
    float aim_y = 0.0f;
};

struct AbilityPlan {
    bool ok = false;
    AbilityReject reason = AbilityReject::kUnavailable;
    AbilityId ability = AbilityId::kCount;  // which one the slot resolved to
    std::int16_t damage = 0;                // pre-scaled melee/spell damage for a striking ability
    Element element = Element::kNone;       // the element Nova will imprint
    Facing facing = Facing::kDown;
    float x = 0.0f;  // where the actor believes the player is — the client is not asked
    float y = 0.0f;
    std::uint16_t map = 0;  // and on which map — the fan-out target, as for AttackPlan
    float aim_x = 0.0f;     // echoed back so the world can aim FanVolley without re-reading the client
    float aim_y = 0.0f;
    // RFC-001 Section 4: the head's phase AFTER this call. kIdle means Cast collapsed straight to
    // Release in the same call (true for every shipped ability today, all cast_ticks = 0) and `x/y/
    // facing/damage/element/aim_x/aim_y` above are a resolved payload ready to dispatch. Any other
    // phase (kCast/kChannel) means the ability is still winding up — the world must NOT dispatch a
    // chunk message yet. No shipped content reaches that branch yet, but the field makes it a
    // defined "not yet" instead of a silent wrong dispatch.
    AbilityPhase phase = AbilityPhase::kIdle;
};

struct GetPlayer {};

// Toggle a mount. Travel across a 1024x1024 map is a design problem, not a convenience: the
// diagonal is nearly four minutes on foot. Riding costs nothing but forbids attacking, which is the
// trade that keeps it from simply being "the walk speed, but correct".
struct SetMounted {
    bool mounted = false;
};

// Put the player somewhere, ignoring everything in the way.
//
// This is NOT a movement message and must not be reachable from input. `MoveIntent` is checked
// against terrain a step at a time, which is exactly what stops a player walking through a
// palisade — and exactly what stops a debug flag or a respawn from crossing the map in one hop,
// since a single move of two hundred tiles is tested against the tile it lands on and nothing in
// between. Two different verbs because they are two different things.
struct Teleport {
    std::uint16_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
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

// --- RFC-014: instance lifecycle messages ---------------------------------------------------------

// Sent once per chunk_key() in a freshly-allocated instance's chunk_edge x chunk_edge grid, by
// InstanceManager::allocate_new() (RFC-014 §3.2-§3.3), doing the same imperative field-assignment
// World::build_chunks() already does for the persistent band — Option 2 of RFC-014 §3.2's two
// sketched wiring paths (declare_lazy's own wire() hook stays untouched; this message is the
// per-instance setup instead, reusing the exact same setup code path, not a second one).
struct PrimeInstanceChunk {
    ChunkCoord coord{};
    MapDescriptor descriptor{};
    std::uint64_t seed = 0;
};

// InstanceManager tells MapDirector to add/remove a live instance's chunk coordinates from its
// per-tick fan-out list (RFC-014 §3.4) — a MESSAGE, never a direct mutating call into
// `MapDirector::chunks` (a plain, non-atomic `std::vector` `MapDirector`'s own handler concurrently
// iterates; RFC-014's own Open Question 4, resolved during review as "must be a message").
struct FanOutAdd {
    std::vector<ChunkCoord> coords;
};
struct FanOutRemove {
    std::vector<ChunkCoord> coords;
};

// A player's connection dropped with no kReturnPortal use (RFC-014 §6) — reverts the session slot
// to unbound WITHOUT touching position, unlike `BindAccount`'s unconditional respawn-and-refill.
// No client-facing network/disconnect layer exists yet (RFC-015's territory); this message is the
// data-level mechanism a future disconnect detector would send.
struct Unbind {};

// The reconnect half of RFC-014 §6: re-arms an already-positioned slot for the SAME account without
// touching position or vitals, unlike `BindAccount`'s unconditional fresh-spawn reset. Sent by
// `World::login()`'s resume branch when a reconnecting account's last known map points at a still-
// open `InstanceSession`.
struct Rebind {
    std::uint32_t account = 0;
};

// RFC-024 §3.5: the one new wire message that RFC specifies — a reliable, ordered broadcast the
// leader sends BEFORE tearing down on a clean quit, so a connected client can skip
// `kClientLeaderTimeoutTicks`'s wait and show "The host closed the world" instead of waiting out a
// timeout for a reason it could have been told immediately. Same "data-level mechanism a future
// detector would send" shape as `Unbind` above — no client-facing network layer exists yet
// (P6/RFC-015's territory) to actually carry this across a socket.
struct WorldClosing {};

// RFC-013 §6.2: caches a MapSession's return coordinates on the dying player's own actor, mirroring
// SetRespawn exactly. Sent once by whichever call already lands a Teleport onto a freshly-joined-or-
// created instanced MapSession (World::use_portal()), so death-time ejection (handle(HurtPlayer)) is
// a single-actor operation — no cross-actor query into InstanceManager on the damage-resolution path.
struct SetInstanceReturn {
    std::uint16_t map = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};

// RFC-016 §4/§7: BindAccount's counterpart for a returning account — restores a persisted
// progression row instead of handing out the fixed starter pack. Sent once, at login, in place of
// BindAccount whenever World::login() finds a saved PlayerProgression for the account (world.hpp).
// `items`/`level`/`xp`/`essence_paid` carry exactly kItemKinds/kSkillCount entries in that fixed
// enum order (tiles.hpp) — plain C arrays, like every other message here, since messages in this
// codebase are not (yet) QUARK_SERIALIZE-described; only world/persistence.hpp's own durable
// structs are.
struct RestoreProgression {
    std::uint32_t account = 0;
    std::uint16_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    std::int16_t mana = 0;
    std::int16_t stamina = 0;
    std::uint32_t deaths = 0;
    std::uint16_t respawn_tx = 0;
    std::uint16_t respawn_ty = 0;
    std::uint16_t return_map = 0;
    std::uint16_t return_x = 0;
    std::uint16_t return_y = 0;
    std::int32_t items[kItemKinds] = {};
    std::uint8_t level[kSkillCount] = {};
    std::uint32_t xp[kSkillCount] = {};
    // RFC-019 §5.7: how much of each branch's Tier IV Essence gate (levels 18->20, 3 total) has
    // already been paid — must survive a restart or a returning specialist re-faces a gate they
    // already cleared.
    std::uint8_t essence_paid[kSkillCount] = {};
};

}  // namespace mmo
