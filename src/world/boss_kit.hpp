// RFC-005 — Boss Ability Authoring.
//
// The one shipped boss (`boss.hpp` + `step_bosses`/`step_boss_ai` in `chunk_actor.hpp`) is
// generation 0 of this system (the RFC's own framing): this header generalizes its numbers into
// the RFC's DATA SHAPE — named ability slots with their own wind-up/active/recover timing and a
// declared hit shape, a difficulty-tier multiplier table, and the wind-up "readability floor"
// formula that makes a shipped boss's numbers a checkable claim instead of a comment. Pure data +
// pure functions, no `ChunkActor` dependency — mirrors `abilities.hpp`/`physics.hpp`'s own shape.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - There is exactly ONE real kit here (`samurai_red_kit()`), reproducing `boss.hpp`'s existing
//     shipped numbers verbatim (it reads FROM those named constants rather than re-typing them, so
//     the two cannot drift). RFC-005's own text frames the shipped boss's numbers as "reproduced by
//     the example kit" — this is that kit, not a second, independent one. A second sheet (Tengu,
//     the RFC's two-phase example) has no Trans-pose renderer/asset support in this engine at all,
//     so `phases[]`/TRANSITION is not modeled here — building an unexercised second-phase state
//     machine with nothing to drive it is exactly the "half-finished implementation" this project's
//     own conventions forbid. Superseded, not reinterpreted, the day a second kit lands.
//   - No JSON kit file, loader, or `tools/validate_boss_kit.py` exists — RFC-008 (the data-driven
//     definition format/schema/codegen) is Accepted but unbuilt, and the pose-manifest measurement
//     pass (`tools/build_atlas.py --pose-manifest`) is asset-pipeline tooling this session never
//     touches. Per RFC-005 §R2's own text, "whether the compiled form is codegen or runtime-loaded
//     is RFC-008's decision; the invariant this RFC requires is one table, read identically by sim
//     and renderer on every node" — a constexpr C++ table (the `abilities.hpp` precedent) satisfies
//     that invariant today; a real file format does not exist to load instead.
//   - `boss_policy`/`BossObs`/`BossAction` (`boss.hpp`) — the generation-0 "script" — are left
//     completely untouched. RFC-005 §R5's declarative script vocabulary (`winding_up`, `use <slot>`,
//     ...) exists to bind onto RFC-007 §3's fixed 15-action Cast-slot table, which is not built;
//     reformulating the working hand-written policy into a vocabulary with nothing to execute it
//     would be pure churn. The RL seam (`boss_policy(BossObs) -> BossAction`) stays exactly as it
//     is today, per this RFC's own R5 promise that generalizing the kit changes nothing about it.
//   - The `active` phase (windup -> ACTIVE window -> RECOVER, generalizing today's instant resolve)
//     is declared as a real field on every ability slot but is NOT wired into the FSM as a genuine
//     multi-tick sub-state this pass — `step_boss_ai`/`boss_resolve` keep resolving a blow the tick
//     wind-up reaches zero, exactly as today, so player-facing timing (and the existing dodge-mid-
//     windup integration test) stays byte-identical. RFC-005's own text names this "one deliberate
//     generalization" and only requires the FIELD to exist for a future kit to use a real window;
//     this pass exercises it at `active = 1`/`active = kBossChargeDashTicks` (the dash's existing
//     multi-tick run IS already a real active window — nothing new needed there).
//   - `body_half_width_of(ScaleTier)` is a genuinely NEW table — the survey confirmed no body/
//     collision-radius concept exists anywhere in RFC-003's `physics.hpp`/`combat_math.hpp` (every
//     hit-test in this engine is point-vs-circle). RFC-005 §R4's worked check cites "RFC-003's body
//     table" for the Giant charge's escape distance (≈1.25) — that number is reproduced exactly
//     here, but the table itself is this RFC's own minimal addition, not a lookup into RFC-003.
//   - The real hit-tests this engine has for the boss (`boss_resolve`'s attack branch, `boss_dash`'s
//     contact test) are both plain point-in-circle checks — no angle/facing-restricted arc exists
//     anywhere in the codebase (confirmed by survey; `AbilityShape::kFront` is a 180° half-plane,
//     not a narrow cone). The Samurai kit's cleave therefore declares `shape_arc_deg = 360.0f`,
//     honestly describing the unrestricted circle it already is, rather than claiming a narrower
//     cone the engine cannot enforce. `escape_distance_arc` only depends on radius (per §R2's own
//     table), so this has no effect on the readability-floor numbers either way.
//   - Only `arc` and `dash` — the two shapes the one real kit actually uses — have their escape-
//     distance formulas wired to a live kit. `ring`/`line`/`projectile` are real, tested pure
//     functions (§R2's table, verbatim); `zone`/`summon` correctly return 0 (the table's own "n/a").
#pragma once

#include <cmath>
#include <cstdint>

#include "world/boss.hpp"
#include "world/combat_math.hpp"
#include "world/tiles.hpp"

namespace mmo {

// §R1's complete pose vocabulary. Only kIdle/kWalk/kAttack(L/R)/kCharge(L/R) are ever reached by
// real content today (`BossPose`, boss.hpp) — kShoot/kTrans/kHit/kDead await a second kit whose
// sheet actually has those rows.
enum class PoseCap : std::uint8_t {
    kIdle = 0,
    kWalk = 1,
    kAttack = 2,
    kAttackL = 3,
    kAttackR = 4,
    kCharge = 5,
    kChargeL = 6,
    kChargeR = 7,
    kShoot = 8,
    kTrans = 9,
    kHit = 10,
    kDead = 11,
    kCount = 12,
};

// §R2's shape vocabulary.
enum class AbilityShapeKind : std::uint8_t {
    kArc = 0,
    kRing = 1,
    kDash = 2,
    kLine = 3,
    kProjectile = 4,
    kZone = 5,
    kSummon = 6,
    kCount = 7,
};

// A new, minimal table (see header note) — half the body's footprint in tiles, used only by §R4's
// escape-distance formula for `dash`-shaped abilities. Giant = 1.25 reproduces the RFC's own worked
// example number exactly.
[[nodiscard]] inline constexpr float body_half_width_of(ScaleTier t) noexcept {
    switch (t) {
        case ScaleTier::kTiny: return 0.20f;
        case ScaleTier::kSmall: return 0.35f;
        case ScaleTier::kMedium: return 0.55f;
        case ScaleTier::kLarge: return 0.85f;
        case ScaleTier::kGiant: return 1.25f;
        case ScaleTier::kTitan: return 1.80f;
        case ScaleTier::kCount: break;
    }
    return 0.55f;
}

// §R2's escape-distance column, one function per shape kind that has a closed form. `zone`/`summon`
// are the table's own "n/a" — dodged by not standing in them, not by outrunning a wind-up.
[[nodiscard]] inline constexpr float escape_distance_arc(float radius) noexcept { return radius; }
[[nodiscard]] inline constexpr float escape_distance_ring(float radius, float hole) noexcept {
    return radius - hole;
}
[[nodiscard]] inline constexpr float escape_distance_dash(float body_half_width) noexcept {
    return 0.5f + body_half_width;
}
[[nodiscard]] inline constexpr float escape_distance_line(float width) noexcept {
    return width * 0.5f + 0.5f;
}
[[nodiscard]] inline constexpr float escape_distance_projectile(float radius) noexcept { return radius; }

// §R4's floor formula constants and the danger-tier minimum RFC-006 §1.3 delegates to this RFC's
// own validator.
inline constexpr int kWindupFloorTicks = 6;
inline constexpr int kDodgeGraceTicks = 2;
inline constexpr int kFloorTickRateHz = 10;

[[nodiscard]] inline constexpr int tier_min_windup(std::uint8_t danger_tier) noexcept {
    switch (danger_tier) {
        case 0: return 5;
        case 1: return 8;
        case 2: return 12;
        case 3: return 16;
        default: break;
    }
    return 16;
}

// §R4's floor, verbatim: the largest of the flat floor, the danger tier's own minimum, and the
// geometric term (how long a player at `kPlayerSpeed` needs to clear `escape_distance`, plus a
// reaction grace). `std::ceil` on an integer ratio needs float math even though everything else in
// this pipeline is tick-integer — matching `knockback_tiles`'s own float precedent (physics.hpp).
[[nodiscard]] inline int windup_floor_ticks(std::uint8_t danger_tier, float escape_distance) noexcept {
    const int geometric =
        static_cast<int>(std::ceil(static_cast<float>(kFloorTickRateHz) * escape_distance / kPlayerSpeed)) +
        kDodgeGraceTicks;
    int floor_ticks = kWindupFloorTicks;
    if (tier_min_windup(danger_tier) > floor_ticks) floor_ticks = tier_min_windup(danger_tier);
    if (geometric > floor_ticks) floor_ticks = geometric;
    return floor_ticks;
}

// §R4's tier table. `cooldown_pm < 1000` means FASTER (a higher tier attacks more often), matching
// the RFC's own "Cooldown x0.85/x0.7" phrasing for tiers 3/4.
enum class BossTier : std::uint8_t { kInitiate = 1, kAdept = 2, kMaster = 3, kElite = 4 };

struct BossTierMult {
    std::uint16_t hp_pm = 1000;
    std::uint16_t damage_pm = 1000;
    std::uint16_t cooldown_pm = 1000;
};

[[nodiscard]] inline constexpr BossTierMult tier_mult(BossTier t) noexcept {
    switch (t) {
        case BossTier::kInitiate: return BossTierMult{1000, 1000, 1150};
        case BossTier::kAdept: return BossTierMult{1400, 1250, 1000};
        case BossTier::kMaster: return BossTierMult{1900, 1500, 850};
        case BossTier::kElite: return BossTierMult{2400, 1750, 700};
    }
    return BossTierMult{};
}

// §R2's per-ability shape — the data a kit binds to one Cast slot. `id` is a stable string (never
// hashed/serialized this pass — no wire format exists yet); `shape_speed` is meaningful for `kDash`
// only, `shape_arc_deg`/`shape_hole` only for `kArc`/`kRing` respectively.
struct BossAbilityKit {
    const char* id = "";
    PoseCap pose = PoseCap::kAttack;
    bool directional = false;
    std::uint8_t telegraph_tier = 1;  // RFC-006 §1.3 danger tier, 0..3
    std::uint16_t windup = 0;
    std::uint16_t active = 0;   // see header note: declared, not yet a real multi-tick FSM sub-state
    std::uint16_t recover = 0;
    std::uint16_t cooldown = 0;
    AbilityShapeKind shape = AbilityShapeKind::kArc;
    float shape_radius = 0.0f;
    float shape_arc_deg = 0.0f;  // kArc only
    float shape_hole = 0.0f;     // kRing only
    float shape_speed = 0.0f;    // kDash only, tiles/second
    std::int16_t damage = 0;     // RFC-009's damage_key is unbuilt; this pass carries the flat number
};

inline constexpr std::size_t kMaxBossAbilities = 4;  // §R2: 2-4 slots, bound onto RFC-007's Cast 0-3

struct BossKitDef {
    const char* boss_id = "";
    Element element = Element::kNone;
    ScaleTier scale = ScaleTier::kGiant;
    Material material = Material::kFlesh;
    std::int16_t base_hp = 0;
    float approach_speed = 0.0f;
    std::uint16_t leash_ticks = 0;
    std::uint16_t respawn_ticks = 0;
    BossAbilityKit abilities[kMaxBossAbilities]{};
    std::uint8_t ability_count = 0;
};

// The one real kit — reproduces `boss.hpp`'s shipped Samurai verbatim (reads its named constants,
// never retypes them, so the two cannot drift). Ability 0 = cleave (attack), ability 1 = charge
// dash — the two the shipped `step_boss_ai` already dispatches.
[[nodiscard]] inline constexpr BossKitDef samurai_red_kit() noexcept {
    BossKitDef k{};
    k.boss_id = "samurai_red";
    k.element = Element::kFire;
    k.scale = ScaleTier::kGiant;
    k.material = Material::kFlesh;
    k.base_hp = kBossMaxHp;
    k.approach_speed = kBossApproachSpeed;
    k.leash_ticks = kBossLeashTicks;
    k.respawn_ticks = kBossRespawnTicks;
    k.ability_count = 2;

    BossAbilityKit cleave{};
    cleave.id = "cleave";
    cleave.pose = PoseCap::kAttack;
    cleave.directional = true;
    cleave.telegraph_tier = 1;
    cleave.windup = kBossAttackWindup;
    cleave.active = 1;
    cleave.recover = kBossAttackCd;
    cleave.cooldown = static_cast<std::uint16_t>(kBossAttackWindup + kBossAttackCd);
    cleave.shape = AbilityShapeKind::kArc;
    cleave.shape_radius = kBossReach;
    cleave.shape_arc_deg = 360.0f;  // see header note: the engine's own hit-test is unrestricted
    cleave.damage = kBossDamage;
    k.abilities[0] = cleave;

    BossAbilityKit charge_dash{};
    charge_dash.id = "charge_dash";
    charge_dash.pose = PoseCap::kCharge;
    charge_dash.directional = true;
    charge_dash.telegraph_tier = 2;
    charge_dash.windup = kBossChargeWindup;
    charge_dash.active = kBossChargeDashTicks;
    charge_dash.recover = kBossAttackCd;  // the shipped dash also holds attack_cd on landing
    charge_dash.cooldown = kBossChargeCd;
    charge_dash.shape = AbilityShapeKind::kDash;
    charge_dash.shape_radius = kBossReach;  // contact test radius, matching the shipped reuse
    charge_dash.shape_speed = kBossChargeSpeed;
    charge_dash.damage = kBossDamage;
    k.abilities[1] = charge_dash;

    return k;
}

}  // namespace mmo
