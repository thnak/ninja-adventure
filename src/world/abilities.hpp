// The ABILITY table — six hand-written entries, shared verbatim by the simulation and the renderer.
//
// An ability is a school's signature move: it costs a vital, it has a cooldown the basic verbs do
// not, and it unlocks at a school level rather than being available from the first swing. There are
// two per school (Melee, Ranged, Magic), gated at school level 2 and level 6 (retuned from 5/10 the day a playtest met the 123-boar wall in front of the first one), and Craft has none — you do
// not fight with a hoe.
//
// WHY A CONSTEXPR TABLE AND NOT DATA READ AT RUNTIME. The trusted PlayerActor reads it to check-and-
// debit (may I, and what does it cost), the untrusted chunk reads the resolved shape to apply it,
// and the renderer reads it to grey out a slot the player cannot afford. All three must agree to the
// last integer, and the cheapest way to guarantee that is one header they all include — the same
// argument tiles.hpp makes for the combat constants. Costs are integers because vitals are; radii
// are floats because positions already are (see tiles.hpp coordinate note).
#pragma once

#include <cstdint>

#include "world/combat_entity.hpp"
#include "world/tiles.hpp"

namespace mmo {

enum class AbilityId : std::uint8_t {
    kWhirlCleave = 0,    // Melee, lvl 5  — a 360 arc
    kCrushBlow = 1,      // Melee, lvl 10 — a single heavy strike that stuns
    kFanVolley = 2,      // Ranged, lvl 5 — three arrows in a fan
    kSmokeBomb = 3,      // Ranged, lvl 10 — a zone that blinds
    kElementalNova = 4,  // Magic, lvl 5  — a ring of the current element
    kRainCall = 5,       // Magic, lvl 10 — a wet zone that feeds the Conduct chain
    kCount = 6,
};

inline constexpr int kAbilityCount = static_cast<int>(AbilityId::kCount);

// Two equipped slots. For F1a the loadout is FIXED (see `equipped_ability` below): slot A is the
// level-5 ability of your strongest fighting school, slot B the level-10 one. The array shape is
// ready for player-chosen loadouts; only the picker is deferred.
inline constexpr int kAbilitySlots = 2;

// Which vital an ability spends. Melee and ranged run on stamina (the bar that paces a fight); magic
// runs on mana (the bar that paces a day). Health is never an ability cost — it is only ever what
// the world takes from you (tiles.hpp).
enum class AbilityCost : std::uint8_t { kStamina = 0, kMana = 1 };

// How the world dispatches a resolved ability to the chunks.
//   kStrike — an arc/single/ring hit resolved by AbilityStrike (WhirlCleave, CrushBlow, Nova)
//   kVolley — several arrows, reusing LaunchArrow (FanVolley)
//   kZone   — a CombatEntity dropped with SpawnEntity (SmokeBomb, RainCall — RFC-004)
enum class AbilityKind : std::uint8_t { kStrike = 0, kVolley = 1, kZone = 2 };

// The hit shape of a kStrike ability, resolved by the chunk.
//   kRing  — everything within `radius`, all around the caster (WhirlCleave, Nova)
//   kFront — the single nearest creature within `radius` and ahead of the caster (CrushBlow)
enum class AbilityShape : std::uint8_t { kRing = 0, kFront = 1 };

// RFC-001 Section 7 — how an ability resolves an aim point/direction at admission time. kSelf and
// kDirection are the only values any shipped ability uses today (see the table below); kPoint and
// kEntity are real, exercised-by-tests code but have no shipped caller yet — see ability_pipeline.hpp's
// header comment and this file's per-ability notes for why.
enum class TargetingModel : std::uint8_t { kSelf = 0, kPoint = 1, kDirection = 2, kEntity = 3 };

struct AbilityDef {
    Skill school;
    std::uint8_t unlock_level;   // the school level at which this ability may be equipped
    AbilityCost cost_kind;
    std::int16_t cost;
    std::uint16_t cooldown;      // ticks before it may be used again
    AbilityKind kind;

    // kStrike: the arc/ring reach and the melee/spell damage multiplier. kZone: the zone radius.
    // kVolley: `radius` is unused; see `shots`/`spread`.
    float radius;
    float damage_scale;          // x the school's base damage (kStrike only)
    AbilityShape shape;          // kStrike only

    // Per-ability trimmings, zero where an ability does not use them. Named rather than a single
    // opaque `extra` so the table reads as its own documentation.
    // CrushBlow's authored Stagger Power rider (RFC-002/009 — see protocol.hpp's AbilityStrike):
    // the old flat `stun_ticks=20` migrated to a build-up gain fed through status_gain instead.
    std::uint16_t stagger_power;
    bool applies_element;         // Nova — feeds the caster's current element's channel
    // kZone only (RFC-004): which CombatEntity archetype this spawns — kSmokeCloud/kWaterPool for
    // SmokeBomb/RainCall. Life and default radius come from `entity_def()`; only the SPAWN POINT is
    // this ability's own (kSelf, matching shipped behaviour — see TargetingModel's comment above).
    EntityKind spawn_entity_kind;
    std::uint8_t shots;          // kVolley only — arrows loosed
    std::uint8_t spread_deg;     // kVolley only — total fan angle in degrees
    EffectKind fx;               // the flash the chunk publishes when it lands

    // RFC-001 fields. The six entries below all set cast_ticks = 0 and no channel block — Section 9
    // of the RFC rules that the scripted six "map onto this machine with cast_ticks = 0, no
    // channel" until RFC-008's data schema lands real authored content, so every activation still
    // resolves Cast -> Release in the same tick, byte-identical to today's behavior.
    std::uint16_t cast_ticks;     // wind-up before Release; 0 = instant (RFC-001 Section 1/9)
    bool move_cancels;            // does moving during Cast cancel it? (Section 5, default true)
    TargetingModel targeting_model;  // Section 7
    float max_range;              // Section 7 — point/entity clamp, or dash/travel reach
};

// The table. Numbers are the design's, approved and final; do not tune them here without the same.
[[nodiscard]] inline constexpr AbilityDef ability_def(AbilityId a) noexcept {
    switch (a) {
        case AbilityId::kWhirlCleave:
            return AbilityDef{Skill::kMelee, 2, AbilityCost::kStamina, 30, 60, AbilityKind::kStrike,
                              2.2f, 1.4f, AbilityShape::kRing,
                              0, false, EntityKind::kIceWall, 0, 0, EffectKind::kSlashHeavy,
                              0, true, TargetingModel::kSelf, 0.0f};
        case AbilityId::kCrushBlow:
            // kFront's nearest-ahead pick happens at Impact, not here — this stays kSelf targeting
            // per RFC-001 Section 7's own note ("existing melee kFront... is kSelf targeting with an
            // entity-picking Impact resolver, and stays that way").
            return AbilityDef{Skill::kMelee, 6, AbilityCost::kStamina, 35, 100, AbilityKind::kStrike,
                              2.0f, 1.8f, AbilityShape::kFront,
                              700, false, EntityKind::kIceWall, 0, 0, EffectKind::kSlashCombo,
                              0, true, TargetingModel::kSelf, 0.0f};
        case AbilityId::kFanVolley:
            return AbilityDef{Skill::kRanged, 2, AbilityCost::kStamina, 30, 80, AbilityKind::kVolley,
                              0.0f, 1.0f, AbilityShape::kRing,
                              0, false, EntityKind::kIceWall, 3, 30, EffectKind::kSlash,
                              0, true, TargetingModel::kDirection, 20.0f};
        case AbilityId::kSmokeBomb:
            // Self-centered, matching shipped behavior (world.hpp always sends the spawn point = the
            // caster's own position) — NOT re-targeted to kPoint. See this file's TargetingModel
            // comment and the RFC-001 implementation plan: retargeting away from the caster would be
            // an unrequested gameplay change, left for RFC-008 content authors to decide deliberately.
            // RFC-004: spawns EntityKind::kSmokeCloud — radius 3.0 (below) and life 50 ticks now live
            // in entity_def(), reproducing SmokeBomb's exact former ZoneKind::kSmokeSuppress numbers.
            return AbilityDef{Skill::kRanged, 6, AbilityCost::kStamina, 25, 150, AbilityKind::kZone,
                              3.0f, 0.0f, AbilityShape::kRing,
                              0, false, EntityKind::kSmokeCloud, 0, 0, EffectKind::kSmoke,
                              0, true, TargetingModel::kSelf, 0.0f};
        case AbilityId::kElementalNova:
            return AbilityDef{Skill::kMagic, 2, AbilityCost::kMana, 30, 90, AbilityKind::kStrike,
                              2.8f, 1.3f, AbilityShape::kRing,
                              0, true, EntityKind::kIceWall, 0, 0, EffectKind::kBlast,
                              0, true, TargetingModel::kSelf, 0.0f};
        case AbilityId::kRainCall:
            // RFC-004: spawns EntityKind::kWaterPool — radius 4.0 (below) and life 100 ticks now live
            // in entity_def(), reproducing RainCall's exact former ZoneKind::kWet numbers.
            return AbilityDef{Skill::kMagic, 6, AbilityCost::kMana, 35, 200, AbilityKind::kZone,
                              4.0f, 0.0f, AbilityShape::kRing,
                              0, false, EntityKind::kWaterPool, 0, 0, EffectKind::kSlash,
                              0, true, TargetingModel::kSelf, 0.0f};
        case AbilityId::kCount: break;
    }
    return ability_def(AbilityId::kWhirlCleave);
}

// The FIXED F1a loadout, as a pure function of the player's levels. Slot 0 (A) is the level-5
// ability of the strongest fighting school; slot 1 (B) is that school's level-10 ability. Ties go to
// the lower enum (Melee before Ranged before Magic), so the answer is deterministic. Returns
// AbilityId::kCount for a slot whose school has not yet reached even the first unlock — the HUD greys those,
// but they still report their intended ability so the greyed icon is the right one.
//
// It always returns the SAME school's pair so the two slots read as one kit. `levels` is indexed by
// `static_cast<int>(Skill)`.
[[nodiscard]] inline constexpr AbilityId equipped_ability(const std::uint8_t* levels,
                                                          int slot) noexcept {
    // Strongest of the three fighting schools; Craft cannot win because it has no abilities.
    Skill best = Skill::kMelee;
    std::uint8_t best_lv = levels[static_cast<int>(Skill::kMelee)];
    if (levels[static_cast<int>(Skill::kRanged)] > best_lv) {
        best = Skill::kRanged;
        best_lv = levels[static_cast<int>(Skill::kRanged)];
    }
    if (levels[static_cast<int>(Skill::kMagic)] > best_lv) {
        best = Skill::kMagic;
    }
    switch (best) {
        case Skill::kMelee: return slot == 0 ? AbilityId::kWhirlCleave : AbilityId::kCrushBlow;
        case Skill::kRanged: return slot == 0 ? AbilityId::kFanVolley : AbilityId::kSmokeBomb;
        case Skill::kMagic: return slot == 0 ? AbilityId::kElementalNova : AbilityId::kRainCall;
        default: break;
    }
    return AbilityId::kCount;
}

// Why an ability request was turned down, so the client can say WHICH bar is empty rather than just
// flashing red. Mirrors the shape of the reasons a PlanAttack silently folds into `ok == false`, but
// abilities have four distinct no's worth telling apart.
enum class AbilityReject : std::uint8_t {
    kOk = 0,
    kLocked = 1,    // the school is not high enough level to equip this ability
    kCooldown = 2,  // it was used too recently
    kResource = 3,  // not enough stamina/mana
    kUnavailable = 4,  // dead, unbound, or mounted
    kBusy = 5,      // RFC-001 Invariant I1: a head instance is already in Cast/Channel/Release, or
                    // the caster is still in its post-interrupt stagger window (Section 5)
    kBadTarget = 6, // RFC-001 Section 7: kEntity targeting only — no ability uses it yet
};

}  // namespace mmo
