// RFC-023: village NPCs — the civilian half. `Npc` is deliberately NOT a parallel entity type: it is
// a `Creature` (GAME.md §5's "one struct for everything alive that isn't the player") carrying
// `Faction::kVillager`, a role, a home anchor and a small hand-authored state machine (RFC-023 §3/§7).
//
// DIVERGENCE FROM THE RFC TEXT, forced by an engine constraint the RFC's own draft did not account
// for: `sizeof(Creature)` is hard-capped at exactly 192 bytes with ZERO headroom
// (`quark::detail::MessagePool::kMaxPayload`, see tiles.hpp's own note on this) because a `Creature`
// crosses chunk boundaries by value inside a fixed-size `CreatureEnter` message. RFC-023 §3's
// `faction_override`/`role`/`skin_id`/`home_struct`/`wander_radius`/`state` cannot be five new struct
// members — there is no room. This file instead packs everything an Npc needs into `Creature::target`
// ("the player key it is angry at" — a field a non-combat civilian never uses, since it never fights):
//
//   bit  63      : the Npc marker (1 = this Creature is an Npc, 0 = ordinary monster/wildlife/boss)
//   bits 40..47  : NpcState
//   bits 32..39  : NpcRole
//   bits  0..31  : home_struct (index into WorldLayout::structures())
//
// `skin_id` and `wander_radius` are not stored at all: per RFC-023 §9 both are pure functions (of
// role, and of (seed, home_struct) respectively) that must never be re-rolled — computing them on
// demand from `role`/`home_struct` IS the "fixed once, never re-rolled" contract, and removing the
// storage removes a place for the two copies to drift apart. Every write site that could otherwise
// touch `target` (provoke/rally_pack/credit_kill) is guarded against `creature_is_npc` in
// chunk_actor.hpp so this packed state is never stepped on by the ordinary combat/aggro machinery.
#pragma once

#include <cstdint>

#include "world/tiles.hpp"

namespace mmo {

// RFC-023 §4. `kGuardMilitia/Spear/Bow/Soldier/Captain` are intentionally absent — see §Non-goals in
// the RFC-023 implementation commit: this codebase's RFC-007 covers only the Dojo Master boss's RL
// scaffolding (rl_obs.hpp/rl_action.hpp/boss_kit.hpp), not the 13-policy guard archetype roster §6's
// table assumes already exists. There is nothing to assign a guard role to yet. `kMerchantWander` is
// also absent: it is the one civilian role anchored to a road segment instead of a `home_struct`
// (§7's own note), and RFC-023's Open Questions 6/8 flag its `PathfieldActor` reuse as unverified —
// the four home-anchored roles below carry no such dependency.
enum class NpcRole : std::uint8_t {
    kMerchantShop = 0,
    kQuestGiver = 1,
    kFarmer = 2,
    kChild = 3,
    kWanderer = 4,
    kCount = 5,
};

// RFC-023 §7's four-state civilian FSM.
enum class NpcState : std::uint8_t {
    kIdle = 0,
    kMoveToWaypoint = 1,
    kWorkAction = 2,
    kSheltering = 3,
};

inline constexpr std::uint64_t kNpcMarkerBit = 1ull << 63;

// A civilian's HP is enough to make "was this NPC ever actually struck" a real, readable event (a
// health bar, per RFC-023 §7's own discussion of the vulnerability being "structurally real") without
// pretending they are combatants — they deal 0 damage regardless (§6).
inline constexpr std::int16_t kNpcMaxHp = 20;

// Tiles: how close a hostile Faction::kMonster creature must get before an NPC starts Sheltering. See
// npc_role_of's header note and chunk_actor.hpp's step_npc — this is the substitute trigger for
// RFC-023 §7's unshipped raid-warning event (RFC-021 §3.4: the real scout/announcement system is
// unassigned P3 scope). Deliberately larger than any monster's melee reach (tiles.hpp's largest
// `CreatureStats::reach` is 2.6, the boss's) so a civilian is already Sheltering, not merely fleeing,
// by the time a raider could land a blow.
inline constexpr float kNpcThreatRadius = 5.0f;

[[nodiscard]] inline constexpr bool creature_is_npc(const Creature& c) noexcept {
    return (c.target & kNpcMarkerBit) != 0;
}

[[nodiscard]] inline constexpr NpcRole npc_role_of(const Creature& c) noexcept {
    return static_cast<NpcRole>((c.target >> 32) & 0xFFull);
}

[[nodiscard]] inline constexpr NpcState npc_state_of(const Creature& c) noexcept {
    return static_cast<NpcState>((c.target >> 40) & 0xFFull);
}

[[nodiscard]] inline constexpr std::uint32_t npc_home_struct_of(const Creature& c) noexcept {
    return static_cast<std::uint32_t>(c.target & 0xFFFF'FFFFull);
}

inline constexpr void npc_set_state(Creature& c, NpcState s) noexcept {
    c.target = (c.target & ~(0xFFull << 40)) | (static_cast<std::uint64_t>(s) << 40);
}

// Stamps a fresh Creature as an Npc — called once, at roster bring-up (world.hpp's build_npcs()).
inline constexpr void npc_init(Creature& c, NpcRole role, std::uint32_t home_struct) noexcept {
    c.target = kNpcMarkerBit | (static_cast<std::uint64_t>(role) << 32) |
               (static_cast<std::uint64_t>(NpcState::kIdle) << 40) | home_struct;
}

// RFC-023 §3's normative faction mechanism, minus the new struct field: an Npc's faction is
// `kVillager` unconditionally; anything else reads its ordinary per-kind stat row exactly as before.
// stance_between(Player, kVillager) and stance_between(kMonster, kVillager) are already correct in
// tiles.hpp's shipped matrix — this is the missing piece that lets a Creature actually report the
// faction that matrix was written for.
[[nodiscard]] inline constexpr Faction faction_of(const Creature& c) noexcept {
    return creature_is_npc(c) ? Faction::kVillager : stats_of(c.kind).faction;
}

// RFC-023 §7's table, 0 tiles = stationary (the role never leaves its door tile).
[[nodiscard]] inline constexpr float npc_wander_radius(NpcRole role) noexcept {
    switch (role) {
        case NpcRole::kMerchantShop: return 0.0f;
        case NpcRole::kQuestGiver: return 0.0f;
        case NpcRole::kFarmer: return 6.0f;
        case NpcRole::kChild: return 4.0f;
        case NpcRole::kWanderer: return 10.0f;
        case NpcRole::kCount: break;
    }
    return 0.0f;
}

[[nodiscard]] inline constexpr bool npc_has_work_pose(NpcRole role) noexcept {
    return role == NpcRole::kFarmer || role == NpcRole::kChild;
}

// RFC-023 §7's dwell windows, in ticks (10 Hz, tiles.hpp's kTicksPerSecond) — farmer 4-8s, child
// 3-6s. Roles with no work pose never call this.
[[nodiscard]] inline std::uint16_t npc_dwell_ticks(NpcRole role, Rng& rng) noexcept {
    if (role == NpcRole::kChild) return static_cast<std::uint16_t>(30 + rng.below(31));  // 3.0-6.0s
    return static_cast<std::uint16_t>(40 + rng.below(41));                               // 4.0-8.0s (farmer/default)
}

// RFC-023 §1.2's tint table, reused here as the NPC placeholder skin (see §Divergences: the runtime
// atlas has no sprite slots for any of the 91 eligible character folders §2/§5 name, so every
// civilian draws the same NinjaGreen rig the player uses — Idle/Walk only, no combat frames needed —
// tinted per instance instead of re-skinned).
struct NpcTint {
    std::uint8_t r = 255, g = 255, b = 255;
};

inline constexpr NpcTint kNpcPalette[8] = {
    {255, 255, 255},  // 0 default (unused for NPCs — kept for index alignment with §1.2's table)
    {255, 140, 140},  // 1 Crimson
    {140, 170, 255},  // 2 Azure
    {255, 210, 120},  // 3 Amber
    {170, 170, 180},  // 4 Slate
    {200, 150, 255},  // 5 Violet
    {235, 235, 245},  // 6 Snow
    {255, 120, 90},   // 7 Ember
};

// RFC-023 §5/§9: a pure function of (seed, home_struct) — fixed once at roster bring-up, never
// re-rolled, never stored (recomputing costs one Rng draw and removes a field that could otherwise
// drift out of sync with this formula). Index 0 (the undyed default) is skipped for NPCs so a
// civilian is never mistaken for the player at a glance.
[[nodiscard]] inline NpcTint npc_tint_of(std::uint64_t seed, std::uint32_t home_struct) noexcept {
    Rng rng(seed ^ (static_cast<std::uint64_t>(home_struct) * 0x9E37'79B9'7F4A'7C15ull) ^ 0xC0DEull);
    return kNpcPalette[1 + rng.below(7)];
}

}  // namespace mmo
