// RFC-004 — the CombatEntity chassis and the terrain scar layer.
//
// CombatEntity is the one chassis for every spawned battlefield thing that is not a creature, a
// player, or a projectile: ice walls, rock spikes, smoke clouds, water pools, fire patches, thunder
// totems, and interceptable falling bodies. It is chunk state, exactly like `Creature`/`Projectile`/
// the now-deleted `Zone` — a record in a chunk-owned, capped array, published in the chunk view.
//
// The scar layer is a SEPARATE, strictly independent mechanism: a per-tile modifier (crater, rubble,
// cracked, scorched ground) that decays on its own and never touches `terrain_of`/walkability. It
// lives here because it shares this RFC's absolute-tick discipline, not because it is part of the
// entity chassis.
//
// WHY THIS IS A SEPARATE HEADER FROM tiles.hpp/chunk_actor.hpp, mirroring ability_pipeline.hpp: this
// is pure data plus pure functions, with no dependency on `ChunkActor`'s own state (creature/player
// lists, occupancy). Admission (cap checks, terrain validity, anti-trap occupancy) needs that state
// and stays in chunk_actor.hpp; what is reusable and directly testable without a chunk lives here.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - Aura fields were originally a `Status` + duration stand-in (RFC-002 had not landed); RFC-002
//     has since landed and RECONCILIATION.md ruling 8 says its `Channel`/`Coating` shape supersedes
//     that stand-in directly, which `EntityDef.aura_kind`/`aura_channel`/`aura_coating`/`aura_gain`/
//     `aura_coating_ticks` below now do — see status.hpp for the ladder machinery those fields feed.
//   - `radius_q` (a fixed-point tiles-times-16 byte) is a plain `float radius` on `CombatEntity`,
//     matching how `Zone::radius` already worked — no packed wire-projection convention exists
//     anywhere in this codebase yet (see chunk_actor.hpp's `ChunkView` copy), so inventing one here
//     for this one record would be scope creep into a discipline nothing else follows.
//   - Absolute-tick fields are `uint64_t`, matching `tick_`/`Zone::expires_at_tick`, not the RFC's
//     `uint32_t` — this engine's own tick counter is already 64-bit everywhere else.
//   - `EntityDef.periodic_cast` (kThunderTotem) is stored and time-advanced but never invoked: no
//     ability or boss content spawns a totem today (RFC-001's multi-tile Persist shapes and RFC-005
//     boss authoring don't exist), so there is nothing for it to call. Likewise `kFallingRock`'s
//     arm-elapse terminal transition stamps its scar but fires no RFC-001 Impact payload, for the
//     same reason — nothing spawns a falling rock yet. Both kinds' state-machine rows are complete
//     and exercised only by synthetic tests (a debug `spawn_entity_at`, mirroring `spawn_zone_at`).
#pragma once

#include <cstdint>

#include "world/status.hpp"
#include "world/tiles.hpp"

namespace mmo {

// Append-only (RFC-004 Section 1): values are a future RL observation contract, never reordered.
enum class EntityKind : std::uint8_t {
    kIceWall = 0,       // Ice    — blocking wall segment
    kRockSpike = 1,     // Rock   — blocking spike
    kSmokeCloud = 2,    // vision blocker; replaces ZoneKind::kSmokeSuppress
    kWaterPool = 3,     // wet aura; replaces ZoneKind::kWet
    kFirePatch = 4,     // burning-ground hazard
    kThunderTotem = 5,  // periodic caster (inert in this pass — see header note)
    kFallingRock = 6,   // interceptable arm-only body (inert in this pass — see header note)
    kCount = 7,
};

enum class EntityState : std::uint8_t {
    kArming = 0,  // telegraph running; not hittable unless `hittable_while_arming`
    kActive = 1,  // collision + aura + vision per def; hittable if destroyable
    kDying = 2,   // death FX playing; already inert
};

enum class Collision : std::uint8_t { kNone = 0, kGround = 1, kGroundAndShot = 2 };
enum class AuraAffects : std::uint8_t { kEnemiesOfTeam = 0, kEveryone = 1 };

// RFC-002 §8's `AuraSpec`, restated as a tagged union of the two things an entity's aura can apply:
// a ladder channel's build-up Power, or a coating's tick refresh. Never both — v1's archetypes each
// pick exactly one, matching RFC-008's own single-entry `{channel|coating, gain}` aura shape.
enum class AuraKind : std::uint8_t { kNone = 0, kChannel = 1, kCoating = 2 };

// The RFC-007 Block E class taxonomy (defined here as real per-kind data; RFC-007 itself is not
// implemented, so nothing reads this yet — same inert-but-correct posture as `TargetingModel::kEntity`
// in abilities.hpp today). `kFallingRock` is the only Projectile-class kind; the engine's own
// `Projectile` records remain a separate chunk-state type, never a CombatEntity kind.
enum class ObsClass : std::uint8_t { kBarrier = 0, kHazardZone = 1, kProjectile = 2, kCaster = 3 };

// --- The terrain scar layer (RFC-004 Section 8) ---------------------------------------------------
// A per-tile modifier with an absolute heal tick. Strictly separate from CombatEntity and from
// `terrain_of`/walkability (Invariant: scars never change walkability). Declared here, ahead of
// `EntityDef`, because `EntityDef.death_scar` needs the enum.
enum class ScarKind : std::uint8_t {
    kNone = 0,
    kCracked = 1,    // no movement effect; escalation precursor
    kRubble = 2,     // speed x0.6
    kCrater = 3,     // speed x0.45
    kScorched = 4,   // no movement effect; a fire-caused mark, off the crack ladder
    kCount = 5,
};

struct Scar {
    std::uint8_t tx = 0, ty = 0;   // chunk-local tile
    ScarKind kind = ScarKind::kNone;
    std::uint64_t heal_tick = 0;   // absolute; at t >= heal_tick the scar downgrades one step
    std::uint64_t made_tick = 0;   // absolute; escalation-window bookkeeping
};

inline constexpr std::size_t kMaxScars = 64;       // per chunk

[[nodiscard]] inline constexpr std::uint16_t heal_ticks_of(ScarKind k) noexcept {
    switch (k) {
        case ScarKind::kCracked: return 1500;   // 2.5 min
        case ScarKind::kRubble: return 3000;    // 5 min
        case ScarKind::kCrater: return 6000;    // 10 min
        case ScarKind::kScorched: return 1200;  // 2 min
        case ScarKind::kNone:
        case ScarKind::kCount: break;
    }
    return 0;
}

// Movement multiplier a scar imposes, composed by the caller with terrain/status multipliers and
// clamped at `kMinScarSpeed` (mirrors status.hpp's `speed_scale_of`).
[[nodiscard]] inline constexpr float scar_speed_scale(ScarKind k) noexcept {
    switch (k) {
        case ScarKind::kRubble: return 0.6f;
        case ScarKind::kCrater: return 0.45f;
        case ScarKind::kCracked:
        case ScarKind::kScorched:
        case ScarKind::kNone:
        case ScarKind::kCount: break;
    }
    return 1.0f;
}

// Chunk state, sibling of `Creature`/`Projectile`. Ownership is derived from position exactly as for
// creatures — no stored owner-chunk field to forget.
struct CombatEntity {
    std::uint32_t id = 0;               // chunk-local monotonic, same scheme as creature/projectile ids
    EntityKind kind = EntityKind::kIceWall;
    EntityState state = EntityState::kArming;
    Faction team = Faction::kPlayer;
    std::int16_t hp = 0;                 // meaningless while `!entity_def(kind).destroyable`
    float x = 0.0f;                      // map-global tile coords; blocking kinds are tile-centre snapped
    float y = 0.0f;
    float radius = 0.0f;                 // spawned aura/footprint radius: def default, or a spawn override
    std::uint64_t owner = 0;             // player key, or 0 for boss/world — kill/assist credit
    std::uint64_t state_tick = 0;        // absolute tick the current state began
    std::uint64_t expire_tick = 0;       // absolute tick of natural expiry
    std::uint64_t next_aura_tick = 0;    // absolute; 0 for kinds with no aura
    std::uint64_t next_cast_tick = 0;    // absolute; kThunderTotem only, 0 otherwise
};

// Everything invariant per kind — one constexpr table shared by sim, renderer and any future trusted
// checker, mirroring `ability_def()`'s own argument for why this is data and not per-record state.
struct EntityDef {
    Collision collision = Collision::kNone;
    bool blocks_vision = false;
    bool destroyable = true;
    bool hittable_while_arming = false;   // true only for kFallingRock (interception window)
    bool observable = true;               // RFC-007 Block E eligibility; inert until RFC-007 lands
    ObsClass obs_class = ObsClass::kBarrier;
    std::int16_t base_hp = 0;
    std::uint16_t arm_ticks = 0;           // telegraph duration before kActive
    std::uint16_t life_ticks = 0;          // kActive duration; 0 = no Active phase (kFallingRock only)
    // Aura (RFC-002 §8's AuraSpec, see header note). `aura_kind` selects which of the next two pairs
    // is live; the other is unused zero-value.
    AuraKind aura_kind = AuraKind::kNone;
    Channel aura_channel = Channel::kNone;       // kChannel only — Power fed to status_gain per pulse
    Coating aura_coating = Coating::kWet;        // kCoating only
    std::uint16_t aura_gain = 0;                 // kChannel only — Power per pulse, [0,1000] scale
    std::uint8_t aura_coating_ticks = 0;         // kCoating only — ticks granted per pulse
    AuraAffects aura_affects = AuraAffects::kEnemiesOfTeam;
    float default_radius = 0.0f;           // aura/footprint radius unless a spawn overrides it
    std::uint16_t aura_period = 0;         // ticks between applications to the same field
    // Products, applied once at kDying entry.
    ScarKind death_scar = ScarKind::kNone;
    EntityKind death_spawn = EntityKind::kCount;   // kCount = none (e.g. kIceWall -> kWaterPool)
    // kThunderTotem only; a raw AbilityId ordinal (0xFFFF = none), NOT the AbilityId type itself —
    // abilities.hpp needs EntityKind (for AbilityDef.spawn_entity_kind), so this header must not
    // include abilities.hpp back. Stored and time-advanced (`next_cast_tick`), never invoked.
    std::uint16_t periodic_cast = 0xFFFF;
    std::uint16_t cast_period = 0;                 // ticks
    // Visuals — indices into the existing packed FX strips; no bespoke sprite exists or is needed.
    EffectKind arm_fx = EffectKind::kSlash;
    EffectKind active_fx = EffectKind::kSlash;
    EffectKind death_fx = EffectKind::kSlash;
};

// The v1 archetype roster (RFC-004 Section 2). Numbers are the RFC's own tunables, reproduced
// verbatim except where this file's header note documents a stand-in.
[[nodiscard]] inline constexpr EntityDef entity_def(EntityKind k) noexcept {
    switch (k) {
        case EntityKind::kIceWall:
            return EntityDef{.collision = Collision::kGroundAndShot, .blocks_vision = false,
                              .destroyable = true, .hittable_while_arming = false, .observable = true,
                              .obs_class = ObsClass::kBarrier, .base_hp = 40, .arm_ticks = 8,
                              .life_ticks = 300, .aura_affects = AuraAffects::kEveryone,
                              .default_radius = 0.0f,
                              .aura_period = 0, .death_scar = ScarKind::kNone,
                              .death_spawn = EntityKind::kWaterPool,
                              .arm_fx = EffectKind::kIce, .active_fx = EffectKind::kIce,
                              .death_fx = EffectKind::kIce};
        case EntityKind::kRockSpike:
            return EntityDef{.collision = Collision::kGround, .destroyable = true, .observable = true,
                              .obs_class = ObsClass::kBarrier, .base_hp = 30, .arm_ticks = 8,
                              .life_ticks = 200, .death_scar = ScarKind::kCracked,
                              .death_spawn = EntityKind::kCount, .arm_fx = EffectKind::kEarth,
                              .active_fx = EffectKind::kEarth, .death_fx = EffectKind::kEarth};
        case EntityKind::kSmokeCloud:
            // Migrates ZoneKind::kSmokeSuppress onto this chassis, reproducing its exact current
            // numbers (radius 3.0, 50 ticks) and its aggro-suppress behaviour — the latter is a
            // kind-keyed special case in chunk_actor.hpp's step_entities(), not table-driven via
            // the aura fields below, because "drop target and cannot re-acquire" is not a Status.
            return EntityDef{.collision = Collision::kNone, .blocks_vision = true, .destroyable = false,
                              .observable = true, .obs_class = ObsClass::kHazardZone, .base_hp = 0,
                              .arm_ticks = 2, .life_ticks = 50, .default_radius = 3.0f,
                              .death_spawn = EntityKind::kCount, .arm_fx = EffectKind::kSmoke,
                              .active_fx = EffectKind::kSmoke, .death_fx = EffectKind::kSmoke};
        case EntityKind::kWaterPool:
            // Migrates ZoneKind::kWet onto this chassis, reproducing its exact current numbers
            // (radius 4.0, 100 ticks, Wet coating for 80 ticks per pulse — unchanged by RFC-002,
            // since Wet is a coating, not a ladder channel).
            return EntityDef{.collision = Collision::kNone, .destroyable = false, .observable = true,
                              .obs_class = ObsClass::kHazardZone, .base_hp = 0, .arm_ticks = 0,
                              .life_ticks = 100, .aura_kind = AuraKind::kCoating,
                              .aura_coating = Coating::kWet, .aura_coating_ticks = 80,
                              .aura_affects = AuraAffects::kEveryone, .default_radius = 4.0f,
                              .aura_period = 10, .death_spawn = EntityKind::kCount,
                              .arm_fx = EffectKind::kSlash, .active_fx = EffectKind::kSlash,
                              .death_fx = EffectKind::kSlash};
        case EntityKind::kFirePatch:
            // RFC-002: the old model set Status::kBurning directly each pulse; the ladder instead
            // feeds Heat build-up (350 Power/pulse, tunable — reaches Singed in one pulse, Burning
            // within two, preserving "stand in fire, start burning quickly").
            return EntityDef{.collision = Collision::kNone, .destroyable = false, .observable = true,
                              .obs_class = ObsClass::kHazardZone, .base_hp = 0, .arm_ticks = 5,
                              .life_ticks = 80, .aura_kind = AuraKind::kChannel,
                              .aura_channel = Channel::kHeat, .aura_gain = 350,
                              .aura_affects = AuraAffects::kEnemiesOfTeam, .default_radius = 1.2f,
                              .aura_period = 10, .death_scar = ScarKind::kScorched,
                              .death_spawn = EntityKind::kCount, .arm_fx = EffectKind::kFire,
                              .active_fx = EffectKind::kFire, .death_fx = EffectKind::kFire};
        case EntityKind::kThunderTotem:
            return EntityDef{.collision = Collision::kGround, .destroyable = true, .observable = true,
                              .obs_class = ObsClass::kCaster, .base_hp = 60, .arm_ticks = 10,
                              .life_ticks = 400, .death_scar = ScarKind::kCracked,
                              .death_spawn = EntityKind::kCount, .cast_period = 50,
                              .arm_fx = EffectKind::kShock,
                              .active_fx = EffectKind::kShock, .death_fx = EffectKind::kShock};
        case EntityKind::kFallingRock:
            return EntityDef{.collision = Collision::kNone, .destroyable = true,
                              .hittable_while_arming = true, .observable = true,
                              .obs_class = ObsClass::kProjectile, .base_hp = 30, .arm_ticks = 20,
                              .life_ticks = 0, .death_scar = ScarKind::kCracked,
                              .death_spawn = EntityKind::kCount, .arm_fx = EffectKind::kEarth,
                              .active_fx = EffectKind::kEarth, .death_fx = EffectKind::kBlast};
        case EntityKind::kCount: break;
    }
    return entity_def(EntityKind::kIceWall);
}

inline constexpr std::size_t kMaxEntities = 16;   // per chunk — a published view is copied, not referenced
inline constexpr std::uint16_t kEscalateWindow = 600;      // ticks — a second scarring impact upgrades
inline constexpr std::uint8_t kBlockedRepathTicks = 5;     // consecutive blocked ticks before contact damage
inline constexpr float kMinScarSpeed = 0.25f;              // floor on the composed speed multiplier
inline constexpr float kFallingRockHitRadius = 0.6f;       // projectile-vs-arming-kFallingRock test

// The arm-exit decision (Section 3): (a) intercepted, (b) the anti-trap whiff, (c) the
// `life_ticks == 0` terminal fire (kFallingRock only), or the ordinary arm -> active transition. A
// pure function so the whole branch is directly testable with synthetic entities/defs — the caller
// (which has the same three booleans) decides separately whether to stamp a scar / spawn a product,
// since (a) and (b) stamp/spawn nothing while (c) and the ordinary path may.
[[nodiscard]] inline constexpr EntityState next_state_after_arm(const EntityDef& def, bool intercepted,
                                                                 bool footprint_occupied) noexcept {
    if (intercepted) return EntityState::kDying;
    if (footprint_occupied) return EntityState::kDying;
    if (def.life_ticks == 0) return EntityState::kDying;
    return EntityState::kActive;
}

// Section 8.4's escalation ladder: a scarring impact within `kEscalateWindow` of the existing scar's
// `made_tick` upgrades it one step (capping at crater); outside the window it re-stamps `kCracked`.
[[nodiscard]] inline constexpr ScarKind escalate(ScarKind current, std::uint64_t made_tick,
                                                  std::uint64_t now) noexcept {
    if (current == ScarKind::kNone) return ScarKind::kCracked;
    if (now - made_tick > kEscalateWindow) return ScarKind::kCracked;
    switch (current) {
        case ScarKind::kCracked: return ScarKind::kRubble;
        case ScarKind::kRubble:
        case ScarKind::kCrater: return ScarKind::kCrater;
        case ScarKind::kScorched: return ScarKind::kCracked;  // a different ladder; re-stamp
        case ScarKind::kNone:
        case ScarKind::kCount: break;
    }
    return ScarKind::kCracked;
}

// Fast-forwards a scar by however many whole heal-steps `now` has passed — bounded by the number of
// severity levels (at most 3), never by elapsed ticks, so a chunk that slept needs no catch-up loop.
[[nodiscard]] inline constexpr Scar heal_lazy(Scar s, std::uint64_t now) noexcept {
    while (s.kind != ScarKind::kNone && now >= s.heal_tick) {
        const std::uint64_t step = heal_ticks_of(s.kind);
        switch (s.kind) {
            case ScarKind::kCrater: s.kind = ScarKind::kRubble; break;
            case ScarKind::kRubble: s.kind = ScarKind::kCracked; break;
            case ScarKind::kCracked:
            case ScarKind::kScorched: s.kind = ScarKind::kNone; break;
            case ScarKind::kNone:
            case ScarKind::kCount: break;
        }
        s.heal_tick += step;
    }
    return s;
}

// RFC-004 §7's v1 damage-to-entity multiplier table — deliberately dumber than creature damage (no
// statuses, no combos, no build-up). A stand-in RFC-003's material system supersedes cell-for-cell;
// kSmokeCloud/kWaterPool/kFirePatch are indestructible and never read through this (`destroyable`
// gates the caller before it matters).
[[nodiscard]] inline constexpr float entity_damage_scale(EntityKind kind, Element element,
                                                          bool heavy_melee) noexcept {
    switch (kind) {
        case EntityKind::kIceWall:
            if (element == Element::kFire) return 2.0f;
            if (element == Element::kIce) return 0.25f;
            return 1.0f;
        case EntityKind::kRockSpike:
            if (element == Element::kFire) return 0.75f;
            if (element == Element::kEarth) return 0.5f;
            if (element == Element::kNone) return heavy_melee ? 1.5f : 1.0f;
            return 1.0f;
        case EntityKind::kThunderTotem:
            return element == Element::kShock ? 0.5f : 1.0f;
        case EntityKind::kFallingRock:
            return element == Element::kEarth ? 0.5f : 1.0f;
        case EntityKind::kSmokeCloud:
        case EntityKind::kWaterPool:
        case EntityKind::kFirePatch:
        case EntityKind::kCount:
            break;
    }
    return 1.0f;
}

// The one rasterization rule for `vision_bits` (Section 4): a tile's centre lies inside the circle.
[[nodiscard]] inline constexpr bool circle_covers_tile(float tile_cx, float tile_cy, float cx, float cy,
                                                        float radius) noexcept {
    const float dx = tile_cx - cx;
    const float dy = tile_cy - cy;
    return dx * dx + dy * dy < radius * radius;
}

}  // namespace mmo
