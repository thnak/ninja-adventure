// RFC-009 — Damage, Resistance & Effect Build-up.
//
// This is the arithmetic core RFC-002's ladder chassis (status.hpp) borrowed a placeholder for:
// the material/scale-tier product that becomes `status_gain`'s `mult_pm`, and the five-step
// integer damage formula (`resolve_damage`) that replaces `strike()`'s bare
// `damage * combo_damage_scale(combo)` float multiply. Pure data + pure functions, no `ChunkActor`/
// `Creature` dependency — mirrors `status.hpp`/`combat_entity.hpp`'s own shape.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - RFC-009's own text hands the 8-channel `AttackPayload`/7-channel `DamagePacket` taxonomy,
//     the material x damage-channel matrix, impulse/knockback, and terrain physics to RFC-003 —
//     which is Accepted but, like RFC-009 was until now, not yet implemented in code. `resolve_damage`
//     below takes one flat `int16_t base` (this engine's existing single-scalar hit shape) rather
//     than inventing RFC-003's packet now; only RFC-009's OWN formula (M_outer, DR, toughness, chip
//     floor) is built for real. There is deliberately no material-based DAMAGE mitigation anywhere
//     in this file (Spirit's 150‰ physical floor, Stone shrugging arrows, etc.) — that matrix is
//     RFC-003 §3.1's alone, not built here.
//   - `Material`/`ScaleTier` per-creature-kind assignment (`defender_of`, tiles.hpp) borrows RFC-003
//     §3/§4's own stated defaults (bears/bosses Large-or-Giant, ghosts Spirit, slimes Slime
//     material, critters Tiny) since RFC-009 cannot exercise its tier/material tables against real
//     content otherwise — RFC-003's actual scope (impulse, terrain physics, the interaction rule
//     table) stays fully deferred.
//   - DR (`dr[2]`) is threaded through the formula for real but is always `{0,0}` this pass: its
//     sources (gear quality, boss guard stances, terrain cover) are RFC-004/RFC-005 content that
//     does not exist yet. Toughness IS live: it is tier-only (no gear bonus source exists either),
//     assigned per `defender_of`, and genuinely changes chip-damage outcomes against Large+ tier
//     creatures today.
//   - `combo_m_outer_pm` restates `status.hpp`'s float `combo_damage_scale` as literal per-mille
//     integers rather than converting it at each call site — RFC-009 §1's "no floats anywhere in
//     the fold" applies to the damage pipeline even though the float form still exists (kept for
//     the HUD-facing `describe`/test use `combo_damage_scale` already had before this RFC).
//   - Player-side and CombatEntity-side `DefenderMitigation` are out of scope, matching RFC-002's
//     own precedent: players carry no Material/ScaleTier/DR/toughness this round either, and
//     CombatEntities keep RFC-004's simpler `entity_damage_scale` table rather than migrating to
//     this shape.
#pragma once

#include <algorithm>
#include <cstdint>

#include "world/status.hpp"

namespace mmo {

enum class Material : std::uint8_t {
    kFlesh = 0, kStone = 1, kSpirit = 2, kMetal = 3, kWood = 4, kPlant = 5, kWater = 6, kSlime = 7,
    kCount = 8,
};

enum class ScaleTier : std::uint8_t {
    kTiny = 0, kSmall = 1, kMedium = 2, kLarge = 3, kGiant = 4, kTitan = 5, kCount = 6,
};

// RFC-009 §4.3: mitigates BUILD-UP Power per RFC-002 channel — a different matrix from RFC-003's
// (deferred) damage-facing one. Every cell >= 100‰ (Invariant I4): no zero cell, no immunity.
[[nodiscard]] inline constexpr std::uint16_t status_affinity(Material m, Channel ch) noexcept {
    static constexpr std::uint16_t kTable[8][5] = {
        //           Cold  Heat  Shock Earth Stagger
        /* Flesh  */ {1000, 1000, 1000, 1000, 1000},
        /* Stone  */ { 500,  500,  750,  400,  600},
        /* Spirit */ { 750,  750,  750,  250,  250},
        /* Metal  */ { 750,  750, 1500,  800,  800},
        /* Wood   */ {1000, 1500,  750,  750, 1000},
        /* Plant  */ {1250, 1750,  750,  750, 1000},
        /* Water  */ {1500,  500, 1750, 1250,  500},
        /* Slime  */ {1250, 1000, 1000, 1250,  250},
    };
    if (ch == Channel::kNone || ch == Channel::kCount) return 1000;
    return kTable[static_cast<std::uint8_t>(m)][gauge_index_of(ch)];
}

// RFC-009 §4.6 / RFC-003 §4 (quoted there, owned here): build-up intake by tier. Invariant I5:
// every tier's gain is >= 250‰ once affinity/coating multipliers fold in — big things resist,
// nothing is immune.
[[nodiscard]] inline constexpr std::uint16_t tier_gain_pm(ScaleTier t) noexcept {
    switch (t) {
        case ScaleTier::kTiny: return 1600;
        case ScaleTier::kSmall: return 1300;
        case ScaleTier::kMedium: return 1000;
        case ScaleTier::kLarge: return 600;
        case ScaleTier::kGiant: return 350;
        case ScaleTier::kTitan: return 200;
        case ScaleTier::kCount: break;
    }
    return 1000;
}

// RFC-002 §6's `kTierTerminalDur`, owned there, quoted here where the tier table already lives:
// only Large/Giant/Titan shorten a terminal's lock; smaller tiers keep the baseline duration.
[[nodiscard]] inline constexpr std::uint16_t tier_terminal_dur_pm(ScaleTier t) noexcept {
    switch (t) {
        case ScaleTier::kLarge: return 800;
        case ScaleTier::kGiant: return 600;
        case ScaleTier::kTitan: return 450;
        case ScaleTier::kTiny:
        case ScaleTier::kSmall:
        case ScaleTier::kMedium:
        case ScaleTier::kCount: break;
    }
    return 1000;
}

// RFC-009 §4.6's flat toughness column (this RFC's own; gear bonuses are RFC-004, not built).
[[nodiscard]] inline constexpr std::uint8_t tier_toughness(ScaleTier t) noexcept {
    switch (t) {
        case ScaleTier::kTiny:
        case ScaleTier::kSmall: return 0;
        case ScaleTier::kMedium: return 1;
        case ScaleTier::kLarge: return 2;
        case ScaleTier::kGiant: return 4;
        case ScaleTier::kTitan: return 6;
        case ScaleTier::kCount: break;
    }
    return 0;
}

// The `mult_pm` status_gain() takes at face value (status.hpp's own header note): material x tier,
// one floor. Two sequential floors (this, then status_gain's own product) can diverge by at most a
// rounding unit from a single four-factor floor — an accepted consequence of `mult_pm` being a
// single pre-combined parameter, per status.hpp's own interface (RFC-002's own phrasing: "the
// caller-supplied per-mille product from RFC-009").
[[nodiscard]] inline constexpr std::uint16_t mult_pm_of(Material m, ScaleTier t, Channel ch) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(status_affinity(m, ch)) * tier_gain_pm(t)) / 1000);
}

// --- The damage formula (RFC-009 §4.4) --------------------------------------------------------------

inline constexpr std::int16_t kChipDamage = 1;       // Invariant I3: any non-DoT hit deals >= this
inline constexpr std::uint16_t kMOuterMin = 250, kMOuterMax = 4000;
inline constexpr std::uint16_t kDrCapPm = 750;        // Invariant I1: total DR <= 750‰

// The five combos' damage scale as literal per-mille integers (status.hpp's `combo_damage_scale`
// restated — see header note) — RFC-002 §7's own numbers, Shatter additionally ignoring DR.
[[nodiscard]] inline constexpr std::uint16_t combo_m_outer_pm(Combo c) noexcept {
    switch (c) {
        case Combo::kShatter: return 2500;
        case Combo::kBlast: return 1600;
        case Combo::kConduct: return 1400;
        case Combo::kCrush: return 1300;
        case Combo::kArc: return 1100;
        case Combo::kNone: break;
    }
    return 1000;
}

// DR sources (gear, guard stance, terrain cover — all RFC-004/RFC-005, not built) and flat
// toughness (tier, this pass's only live source). `dr[2]` stacks multiplicatively in the
// complement (two 300‰ sources give 510‰, not 600‰), capped at `kDrCapPm`.
struct DefenderMitigation {
    std::uint16_t dr[2] = {0, 0};
    std::uint8_t toughness = 0;
};

// Steps 1-5, normative order. `base` stands in for the (unbuilt RFC-003) packet's channel sum —
// this engine still carries one flat damage scalar per hit (see header note). DoT ticks skip this
// function entirely per §4.4 ("DoT ticks... apply base directly, no chip floor") — callers apply
// DoT damage straight to hp, never through here.
[[nodiscard]] inline constexpr std::int16_t resolve_damage(std::int16_t base, Combo combo,
                                                           const DefenderMitigation& def) noexcept {
    if (base <= 0) return 0;
    const std::uint16_t m_outer =
        std::clamp<std::uint16_t>(combo_m_outer_pm(combo), kMOuterMin, kMOuterMax);
    const auto scaled = static_cast<std::int32_t>((static_cast<std::int32_t>(base) * m_outer) / 1000);
    std::int32_t after_dr = scaled;
    if (combo != Combo::kShatter) {  // Shatter ignores armour (kIgnoreDr, RFC-002 §7)
        std::uint32_t complement = 1000;
        for (const std::uint16_t d : def.dr) {
            complement = (complement * (1000 - std::min<std::uint16_t>(d, 1000))) / 1000;
        }
        const auto dr_total = static_cast<std::uint16_t>(std::min<std::uint32_t>(kDrCapPm, 1000 - complement));
        after_dr = (scaled * (1000 - dr_total)) / 1000;
    }
    const std::int32_t after_t = std::max<std::int32_t>(0, after_dr - def.toughness);
    return static_cast<std::int16_t>(std::max<std::int32_t>(after_t, kChipDamage));
}

}  // namespace mmo
