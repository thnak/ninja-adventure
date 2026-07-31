// RFC-003 — Physics & Material Interaction.
//
// The physical layer: per-material impulse transmission (§3.1), mass and outgoing-impulse scaling
// by scale tier (§4 — the physics half; RFC-009 §4.6 owns the build-up-facing tier tables in
// `combat_math.hpp`), the four terrain physical properties (§6), the knockback law and its
// terrain-suppression companions — force-transfer ("the mud rule") and slip mitigation ("the ice
// rule") — and the WallSlam/terrain-stress formulas (§5/§7). Pure data + pure functions, mirroring
// `combat_math.hpp`/`status.hpp`'s own shape: no `ChunkActor` dependency, so every formula here is
// directly unit-testable.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - No `AttackPayload` struct. RFC-003 §2's eight-channel payload is produced by RFC-001's ability
//     pipeline and authored via RFC-008's skill data — neither exists yet (same gap RFC-009 already
//     documented for its own `DamagePacket`). This engine still carries one flat `int16_t damage`
//     scalar per hit; `chunk_actor.hpp` authors a single `impulse` scalar as a compile-time constant
//     at the few call sites that already distinguish a "heavy"/authored blow (heavy melee, CrushBlow)
//     rather than inventing a payload struct nothing produces yet. Wiring a real multi-channel
//     payload in later is a call-site change, not a signature change here.
//   - Knockback distance is `float` tiles, not the RFC's fixed-point 1/256 `int32_t`. §1's fixed-point
//     requirement exists for cross-node determinism; `Creature::x/y` are already `float` end-to-end
//     (`tiles.hpp`) and knockback resolves entirely within the owning chunk's own tick (§9: "results,
//     not rule evaluations, are what leave the node") — so this matches every other physics-adjacent
//     quantity already in this codebase rather than introducing the RFC's literal type.
//   - `TerrainPhys`'s scar overlay (the `terrain_phys(Terrain, ScarKind)` overload) implements only
//     the RFC-004 scar half of §6's merged "kRubbled patch / kRubble-kCrater scar" and
//     "kCracked"/"kScorched" rows — the RFC-010 tile-patch half (kMudded/kIced/kRubbled-as-patch) is
//     unbuilt (RFC-010 has not landed). Applying a patch overlay later is a new overload, not a
//     signature change.
//   - The full §8 interaction rule table (8 ordered rows) is NOT built here. Six of the eight rows
//     test state that does not exist yet: ignition/kBurning-patch and kIced-patch rows (1,2,3,4,6,7,8)
//     all key off RFC-010 tile patches, and row 5 (metal arcs) needs a raw Electric channel magnitude
//     this engine has no source for until RFC-001/008 land. Only the two extensions this RFC's own
//     text says it contributes to RFC-002's combo system are wired, at their one call site in
//     `chunk_actor.hpp`: standing on a tile with effective Conductivity >= 50 counts as Wet for
//     Conduct's coating test, and this file's `stress_converts`/WallSlam-into-terrain path is the one
//     real (non-rule-table) instance of "frozen ground shatters"'s sibling mechanism, terrain stress.
//   - WallSlam damage carries no killer attribution. `Creature` has no budget left for a "who
//     knocked me into this wall" field under the pooled-message cap (`combat_math.hpp`/`status.hpp`
//     already document this ceiling being tight); a kill finished by a slam goes uncredited, matching
//     how every other environmental kill in this engine already works (there was no environmental
//     kill path before this RFC to set a precedent against).
//   - Player-target and boss-target knockback are both out of scope this pass, matching RFC-002/009's
//     own "player-target deferred" precedent: `PlayerActor` carries no physics state, and
//     `step_bosses` never calls `strike()` against a player. Only the four existing player-initiated
//     hit verbs (melee, cast, ability, arrow) against ordinary creatures are wired.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "world/combat_entity.hpp"  // ScarKind (pulls in tiles.hpp for Terrain, status.hpp)
#include "world/combat_math.hpp"    // Material, ScaleTier

namespace mmo {

// --- §3.1: per-material impulse transmission, per-mille -------------------------------------------
// The one channel RFC-009 hands over in full ("Impulse is not a damage channel"). Spirit is the one
// 0‰ cell this RFC owns (impulse-immune, infinite mass) — RFC-009 Invariant I4 (no 0‰ cell) governs
// only the DAMAGE-facing matrix, which lives in `combat_math.hpp` and is untouched by this table.
[[nodiscard]] inline constexpr std::uint16_t material_impulse_pm(Material m) noexcept {
    switch (m) {
        case Material::kFlesh: return 1000;
        case Material::kStone: return 400;
        case Material::kSpirit: return 0;  // infinite mass -> zero displacement
        case Material::kMetal: return 800;
        case Material::kWood: return 900;
        case Material::kPlant: return 1000;
        case Material::kWater: return 600;
        case Material::kSlime: return 1400;
        case Material::kCount: break;
    }
    return 1000;
}

[[nodiscard]] inline constexpr std::uint16_t transmit_impulse(std::uint16_t impulse,
                                                               Material target_material) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(impulse) * material_impulse_pm(target_material)) / 1000u);
}

// --- §4: mass and outgoing-impulse scaling, by scale tier ------------------------------------------
// Normatively owned here (RFC-009 §4.6 owns the build-up-facing tier tables instead: gain, terminal
// duration, toughness — see `combat_math.hpp`). Mass may be overridden per archetype in RFC-008 data
// (not built); tier modifiers themselves never vary, which is what keeps "big" meaning one thing.
[[nodiscard]] inline constexpr std::uint16_t mass_of(ScaleTier t) noexcept {
    switch (t) {
        case ScaleTier::kTiny: return 25;
        case ScaleTier::kSmall: return 50;
        case ScaleTier::kMedium: return 100;
        case ScaleTier::kLarge: return 250;
        case ScaleTier::kGiant: return 700;
        case ScaleTier::kTitan: return 2000;
        case ScaleTier::kCount: break;
    }
    return 100;
}

// §2.1: producer-side scaling — a caster's impulse/crush/explosion channels are multiplied by their
// own `scale_impulse_out` at emission. Not wired at a call site this pass (every hit that authors an
// impulse today is player-sourced, and players have no ScaleTier of their own yet — see header note);
// the table is real so wiring it in for boss-authored hits later is a call-site change.
[[nodiscard]] inline constexpr std::uint16_t scale_impulse_out_pm(ScaleTier t) noexcept {
    switch (t) {
        case ScaleTier::kTiny: return 500;
        case ScaleTier::kSmall: return 750;
        case ScaleTier::kMedium: return 1000;
        case ScaleTier::kLarge: return 1250;
        case ScaleTier::kGiant: return 1600;
        case ScaleTier::kTitan: return 2000;
        case ScaleTier::kCount: break;
    }
    return 1000;
}

// --- §6: terrain physical properties -----------------------------------------------------------------

struct TerrainPhys {
    std::uint8_t friction = 60;
    std::uint8_t grip = 70;
    std::uint8_t conductivity = 15;
    std::uint8_t stability = 60;
};

[[nodiscard]] inline constexpr std::uint8_t clamp_prop(int v) noexcept {
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 100 ? 100 : v));
}

[[nodiscard]] inline constexpr TerrainPhys terrain_phys(Terrain t) noexcept {
    switch (t) {
        case Terrain::kGrass: return TerrainPhys{60, 70, 15, 60};
        case Terrain::kDirt: return TerrainPhys{70, 80, 25, 50};
        case Terrain::kWater: return TerrainPhys{0, 0, 100, 0};    // impassable; conducts only
        case Terrain::kStone: return TerrainPhys{55, 75, 5, 90};
        case Terrain::kSand: return TerrainPhys{80, 55, 10, 40};
        case Terrain::kTree: return TerrainPhys{0, 0, 10, 35};     // impassable
        case Terrain::kSnow: return TerrainPhys{75, 45, 10, 55};
        case Terrain::kMarsh: return TerrainPhys{95, 90, 60, 30};
        case Terrain::kAsh: return TerrainPhys{65, 65, 5, 45};
        case Terrain::kPath: return TerrainPhys{55, 80, 10, 70};
        case Terrain::kBuilding: return TerrainPhys{0, 0, 5, 100};  // never converts
        case Terrain::kCount: break;
    }
    return TerrainPhys{60, 70, 15, 60};
}

// The RFC-004 scar half of §6's merged patch/scar rows (header note: the RFC-010 patch half is not
// built). kRubble/kCrater SET the row (matching the RFC table's `=` convention); kCracked/kScorched
// apply a delta on top of the base terrain, clamped 0-100.
[[nodiscard]] inline constexpr TerrainPhys terrain_phys(Terrain t, ScarKind scar) noexcept {
    switch (scar) {
        case ScarKind::kRubble:
        case ScarKind::kCrater:
            return TerrainPhys{85, 60, 5, 25};
        case ScarKind::kCracked: {
            TerrainPhys p = terrain_phys(t);
            p.stability = clamp_prop(static_cast<int>(p.stability) - 15);
            return p;
        }
        case ScarKind::kScorched: {
            TerrainPhys p = terrain_phys(t);
            p.conductivity = clamp_prop(static_cast<int>(p.conductivity) - 10);
            return p;
        }
        case ScarKind::kNone:
        case ScarKind::kCount:
            break;
    }
    return terrain_phys(t);
}

// --- §5: impulse resolution — knockback and the slam --------------------------------------------------

inline constexpr float kKnockbackCap = 4.0f;    // tiles, tunable
inline constexpr float kFlinchTiles = 0.25f;    // below this, a hit is a flinch: no state, no move
inline constexpr std::uint8_t kKbTicks = 3;
inline constexpr std::uint8_t kStressUnit = 3;  // §7, points per stability unit

[[nodiscard]] inline constexpr std::uint16_t kb_terrain_pm(std::uint8_t friction) noexcept {
    return static_cast<std::uint16_t>((100u - friction) * 25u);
}

// `kb_raw = impulse_effective / mass`, scaled by the terrain factor, capped. `impulse_effective` is
// the impulse AFTER §3.1's material transmission (`transmit_impulse`), matching the RFC's own
// sequencing (impulse transmission happens once, before knockback is resolved).
[[nodiscard]] inline float knockback_tiles(std::uint16_t impulse_effective, std::uint16_t mass,
                                           std::uint8_t friction) noexcept {
    if (mass == 0) return 0.0f;
    const float kb_raw = static_cast<float>(impulse_effective) / static_cast<float>(mass);
    const float kb_terrain = static_cast<float>(kb_terrain_pm(friction)) / 1000.0f;
    return std::min(kb_raw * kb_terrain, kKnockbackCap);
}

// §6's "mud rule", generalised: momentum the terrain suppressed hurts instead. Only fires when
// `kb_terrain_pm` < 500‰ (heavy suppression); zero otherwise (e.g. sand, which merely absorbs).
[[nodiscard]] inline constexpr std::uint16_t force_transfer_crush(std::uint16_t impulse_effective,
                                                                   std::uint16_t kb_terrain_pm_v,
                                                                   std::uint8_t grip) noexcept {
    if (kb_terrain_pm_v >= 500) return 0;
    const std::int64_t num = static_cast<std::int64_t>(impulse_effective) *
                             (500 - static_cast<std::int64_t>(kb_terrain_pm_v)) *
                             static_cast<std::int64_t>(grip) * 6;
    return static_cast<std::uint16_t>(num / 1'000'000LL);
}

inline constexpr std::uint16_t kSlipGripThreshold = 30;
inline constexpr std::uint16_t kSlipMitigationPm = 850;

// §6's mirror rule: on a low-grip tile (ice), the target gives way instead of taking the blow
// square. Applies to the physical damage scalar (this engine's stand-in for Damage+Crush — see
// header note on the missing multi-channel payload).
[[nodiscard]] inline constexpr bool slip_applies(std::uint8_t grip) noexcept {
    return grip < kSlipGripThreshold;
}

// The undelivered momentum, converted back into harm (§5). `remaining_tiles` is the displacement
// that never landed (the slide's remaining magnitude at the moment it was blocked).
[[nodiscard]] inline std::int16_t wallslam_crush(float remaining_tiles, std::uint16_t mass) noexcept {
    const float v = remaining_tiles * static_cast<float>(mass) * 0.5f;
    return static_cast<std::int16_t>(std::clamp(v, 0.0f, 32000.0f));
}

// --- §7: terrain stress -------------------------------------------------------------------------------
// Transient by design (header note in the RFC itself): no persistent per-tile pool, just a
// threshold test at the moment stress is deposited (WallSlam-into-terrain, this engine's one real
// stress source — see header note on the deferred explosion/ground-Crush sources).
[[nodiscard]] inline constexpr bool stress_converts(std::uint16_t stress, std::uint8_t stability) noexcept {
    return stress >= static_cast<std::uint16_t>(stability) * kStressUnit;
}

}  // namespace mmo
