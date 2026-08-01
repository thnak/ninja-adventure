// RFC-018 — Loot, Essence & Reward Tables.
//
// Replaces the two placeholder drop paths `chunk_actor.hpp::credit_kill()` already names as its
// own future work (a boss paying flat `400 XP + 10 produce`, an ordinary monster paying XP only)
// with the real thing: an equipment/material-tier data shape (§2-§4), socket gems as a direct
// consumer of RFC-002's closed six-channel status ladder (§5), Essence as a challenge-realm-only
// stackable (§6), and deterministic, ring-scaled loot tables (§7-§9) callable from both an ordinary
// kill and a boss kill's per-contributor roll (§10).
//
// Pure data plus pure functions, mirroring `status.hpp`/`combat_math.hpp`'s own split — no
// `ChunkActor` dependency, so this header is safe for `protocol.hpp` (wire messages) and
// `player_actor.hpp` (equipped-gear state) to both include directly.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, documented once here:
//   - Loot tables are authored as constexpr C++ data (below), not RFC-008 `loot.*` JSON documents.
//     RFC-008's own offline pack loader (`combat_pack.hpp`/`tools/build_combat_pack.py`) has no
//     runtime consumer for ANY of its domains yet — nothing in the live sim calls
//     `CombatPack::load()` — so a JSON `loot` domain here would be unconsumed content, exactly the
//     kind of half-finished pipeline stage this project avoids. This matches how the six shipped
//     abilities and every `CreatureStats` row are still authored as constexpr tables today, not
//     JSON either. The JSON schema RFC-018 §7 specifies remains the eventual authoring surface once
//     RFC-008's pack is actually wired into the live sim, for some domain, which it is not yet.
//   - Socket-gem riders (§5) are wired through the basic melee swing only (`MeleeSwing`, both light
//     and heavy) — not ranged shots or spell casts. This is GAME.md §8's own worked example ("a Fire
//     gem makes a MELEE weapon auto-apply Burning") done once, correctly, rather than the identical
//     rider plumbing re-added to `LaunchArrow`/`step_projectiles` and `CastSpell` for attack kinds
//     the RFC's own guide-level text never uses as its example.
//   - Weapon durability decrements once per successful swing ATTEMPT (`Ask<PlanAttack>` resolving
//     `ok=true`), not per confirmed landed hit — landed-hit confirmation happens on a different,
//     untrusted-OK actor (the chunk), and piping a wear-decrement message back to the trusted
//     `PlayerActor` for every individual creature struck would be a new cross-actor round trip this
//     RFC does not need for a cosmetic wear stat. Armor durability, by contrast, decrements exactly
//     as specified — once per `HurtPlayer` — because that already runs on `PlayerActor` itself.
//   - The boss's rare equipment row (§10.2) is genuinely realm-gated, matching §12's own recap table
//     ("Finished equipment... yes, rare" only under the Challenge-realm boss column) and GAME.md
//     §1's "no exceptions" framing for Essence. This codebase's one shipped boss (the dojo Giant Red
//     Samurai) lives in a village's PERSISTENT interior room, reached through an ordinary interior
//     door, never a `PortalKind::kRealmGate` — so `essence_pm`/the equipment row are wired for real
//     and will fire correctly the moment a future content pass routes an encounter through an actual
//     realm gate into an instanced `RealmType::kChallenge` map, but they do not fire for today's
//     dojo boss. Proven wired by a direct unit test in `sim_main.cpp` rather than by live dojo play.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "world/status.hpp"
#include "world/tiles.hpp"

namespace mmo {

// --- §2: slot taxonomy — deliberately two, not a full paperdoll ----------------------------------
enum class EquipSlot : std::uint8_t { kWeapon = 0, kArmor = 1, kCount = 2 };

// --- §3: material tiers ---------------------------------------------------------------------------
enum class MaterialTier : std::uint8_t { kCopper = 0, kIron = 1, kSteel = 2, kMythril = 3, kCount = 4 };

[[nodiscard]] inline constexpr std::uint16_t tier_damage_pm(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kCopper: return 1000;
        case MaterialTier::kIron: return 1150;
        case MaterialTier::kSteel: return 1350;
        case MaterialTier::kMythril: return 1600;
        case MaterialTier::kCount: break;
    }
    return 1000;
}
// Per-mille — a small, additional DR source composed the same multiplicative way
// `combat_math.hpp::resolve_damage` already chains `Creature::dr[2]`'s two sources.
[[nodiscard]] inline constexpr std::uint16_t tier_dr_bonus(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kCopper: return 0;
        case MaterialTier::kIron: return 4;
        case MaterialTier::kSteel: return 9;
        case MaterialTier::kMythril: return 16;
        case MaterialTier::kCount: break;
    }
    return 0;
}
[[nodiscard]] inline constexpr std::uint8_t tier_toughness_bonus(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kCopper: return 0;
        case MaterialTier::kIron: return 0;
        case MaterialTier::kSteel: return 1;
        case MaterialTier::kMythril: return 2;
        case MaterialTier::kCount: break;
    }
    return 0;
}
[[nodiscard]] inline constexpr std::int16_t tier_max_durability(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kCopper: return 200;
        case MaterialTier::kIron: return 350;
        case MaterialTier::kSteel: return 550;
        case MaterialTier::kMythril: return 900;
        case MaterialTier::kCount: break;
    }
    return 200;
}
[[nodiscard]] inline constexpr std::uint8_t tier_socket_slots(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kCopper: return 0;
        case MaterialTier::kIron: return 1;
        case MaterialTier::kSteel: return 1;
        case MaterialTier::kMythril: return 2;
        case MaterialTier::kCount: break;
    }
    return 0;
}
// A broken Copper item never drops below its own baseline (§4.1).
[[nodiscard]] inline constexpr MaterialTier tier_below(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kIron: return MaterialTier::kCopper;
        case MaterialTier::kSteel: return MaterialTier::kIron;
        case MaterialTier::kMythril: return MaterialTier::kSteel;
        case MaterialTier::kCopper:
        case MaterialTier::kCount: break;
    }
    return MaterialTier::kCopper;
}
[[nodiscard]] inline constexpr ItemKind ore_kind_of(MaterialTier t) noexcept {
    switch (t) {
        case MaterialTier::kCopper: return ItemKind::kOreCopper;
        case MaterialTier::kIron: return ItemKind::kOreIron;
        case MaterialTier::kSteel: return ItemKind::kOreSteel;
        case MaterialTier::kMythril: return ItemKind::kOreMythril;
        case MaterialTier::kCount: break;
    }
    return ItemKind::kOreCopper;
}

// --- §4: equipped-item data shape — the entire answer to "what does loot drop into" --------------
struct SocketGem {
    ItemKind kind = ItemKind::kWood;  // one of the 24 kGem<Channel><Grade> ordinals, or kWood = empty
};

struct EquippedItem {
    std::uint16_t item_id = 0;  // 0 = nothing equipped (bare hand / bare skin baseline)
    MaterialTier tier = MaterialTier::kCopper;
    std::int16_t durability = 0;
    std::int16_t max_durability = 0;  // set at craft/drop time (§3's table); snapshot, not recomputed
    SocketGem sockets[2]{};           // fixed at the Mythril ceiling; lower tiers leave trailing slots empty
};

// --- §4.1: durability wears, never breaks -----------------------------------------------------
// `max_durability <= 0` (the bare-hand/bare-skin default) is never interpolated — Copper's own
// numbers are already the neutral baseline, so an empty slot is correctly inert without a special
// case in the callers below.
[[nodiscard]] inline constexpr std::uint16_t effective_tier_damage_pm(const EquippedItem& it) noexcept {
    if (it.item_id == 0 || it.max_durability <= 0) return tier_damage_pm(it.tier);
    const float frac = std::clamp(static_cast<float>(it.durability) / static_cast<float>(it.max_durability),
                                   0.0f, 1.0f);
    const float lo = static_cast<float>(tier_damage_pm(tier_below(it.tier)));
    const float hi = static_cast<float>(tier_damage_pm(it.tier));
    return static_cast<std::uint16_t>(lo + (hi - lo) * frac);
}
[[nodiscard]] inline constexpr std::uint16_t effective_tier_dr_bonus(const EquippedItem& it) noexcept {
    if (it.item_id == 0 || it.max_durability <= 0) return tier_dr_bonus(it.tier);
    const float frac = std::clamp(static_cast<float>(it.durability) / static_cast<float>(it.max_durability),
                                   0.0f, 1.0f);
    const float lo = static_cast<float>(tier_dr_bonus(tier_below(it.tier)));
    const float hi = static_cast<float>(tier_dr_bonus(it.tier));
    return static_cast<std::uint16_t>(lo + (hi - lo) * frac);
}
[[nodiscard]] inline constexpr std::uint8_t effective_tier_toughness_bonus(const EquippedItem& it) noexcept {
    if (it.item_id == 0 || it.max_durability <= 0) return tier_toughness_bonus(it.tier);
    const float frac = std::clamp(static_cast<float>(it.durability) / static_cast<float>(it.max_durability),
                                   0.0f, 1.0f);
    const float lo = static_cast<float>(tier_toughness_bonus(tier_below(it.tier)));
    const float hi = static_cast<float>(tier_toughness_bonus(it.tier));
    return static_cast<std::uint8_t>(lo + (hi - lo) * frac);
}

// --- §5: socket gems — the mechanical link to RFC-002's status ladder ----------------------------
// A gem rider: what a socketed gem applies on a landed hit, resolved from `kind` alone. `coating`
// selects `CoatingPacket` (Wet) instead of `BuildupPacket` (the five ladder channels) — the one
// coating in RFC-002/008's closed six-document status set.
//
// Grade (Minor/Lesser/Greater/Major, RFC-018 §5) is deferred — see `tiles.hpp::ItemKind`'s own
// header note on why (the 24-ordinal grade-baked scheme does not fit this engine's fixed 192-byte
// message-pool ceiling). Every gem here applies one flat, Lesser-equivalent rider.
struct GemRider {
    Channel channel = Channel::kNone;
    std::uint16_t amount = 0;  // Power (build-up channels) or ticks (Wet coating)
    bool coating = false;
};

[[nodiscard]] inline constexpr GemRider gem_rider_of(ItemKind kind) noexcept {
    switch (kind) {
        case ItemKind::kGemCold: return GemRider{Channel::kCold, 300, false};
        case ItemKind::kGemHeat: return GemRider{Channel::kHeat, 300, false};
        case ItemKind::kGemShock: return GemRider{Channel::kShock, 300, false};
        case ItemKind::kGemEarth: return GemRider{Channel::kEarth, 300, false};
        case ItemKind::kGemStagger: return GemRider{Channel::kStagger, 300, false};
        // Wet is a COATING, not a build-up channel — `amount` here is coating TICKS, not [0,1000]
        // Power, so it gets its own small scale (`CoatingPacket::ticks` is a `uint8_t`).
        case ItemKind::kGemWet: return GemRider{Channel::kNone, 80, true};
        default: break;  // kWood (empty socket) and every non-gem ordinal: no rider
    }
    return GemRider{};
}

// §5's "one free buildup slot" rule collapses to "always free" for the basic swing, which authors
// no buildup rider of its own — so every socketed gem fires on every ordinary hit.
[[nodiscard]] inline constexpr std::array<GemRider, 2> resolve_weapon_gems(const EquippedItem& weapon) noexcept {
    return {gem_rider_of(weapon.sockets[0].kind), gem_rider_of(weapon.sockets[1].kind)};
}

// --- §9: ring scaling -------------------------------------------------------------------------------
struct RingLootScale {
    std::uint16_t chance_mult_pm;  // multiplies an entry's chance_pm
    std::int32_t qty_bonus;        // added to an ore row's qty_max only
    MaterialTier ore_tier;         // which tier this ring's monster/boss kills pay out
};
[[nodiscard]] inline constexpr RingLootScale ring_loot_scale(Ring r) noexcept {
    switch (r) {
        case Ring::kMeadow: return RingLootScale{1000, 0, MaterialTier::kCopper};
        case Ring::kForest: return RingLootScale{1200, 0, MaterialTier::kCopper};
        case Ring::kWetland: return RingLootScale{1400, 1, MaterialTier::kIron};
        case Ring::kSnow: return RingLootScale{1600, 1, MaterialTier::kSteel};
        case Ring::kWasteland: return RingLootScale{1800, 2, MaterialTier::kMythril};
        case Ring::kCount: break;
    }
    return RingLootScale{1000, 0, MaterialTier::kCopper};
}

// --- §7/§10: loot tables -----------------------------------------------------------------------
struct LootEntry {
    ItemKind item = ItemKind::kWood;  // ignored when ore_slot is true
    std::uint16_t chance_pm = 0;
    std::int32_t qty_min = 0;
    std::int32_t qty_max = 0;
    bool ore_slot = false;     // true: kind resolved from the kill's ring instead of `item`
    bool realm_gated = false;  // true: only rolled inside a challenge-realm map (§6.6)
};

inline constexpr std::size_t kMaxLootEntries = 2;

struct LootTable {
    std::array<LootEntry, kMaxLootEntries> entries{};
    std::uint8_t entry_count = 0;
    std::uint16_t essence_pm = 0;  // 0 = this table grants no Essence
    std::int32_t essence_qty_min = 1;
    std::int32_t essence_qty_max = 1;
    bool has_equipment = false;  // boss tables only (§10.2)
    std::uint16_t equipment_chance_pm = 0;
    EquipSlot equipment_slot = EquipSlot::kWeapon;
    MaterialTier equipment_tier = MaterialTier::kCopper;
    std::uint16_t equipment_item_id = 0;
};

// §10.2's occupied-slot rule, as a pure function so it is testable without an actor: strictly
// higher tier auto-equips, anything else does not (the caller converts a refused drop to a
// guaranteed ore stack — see `PlayerActor::handle(const GrantEquipment&)`).
[[nodiscard]] inline constexpr bool equipment_upgrades(const EquippedItem& current,
                                                        const EquippedItem& incoming) noexcept {
    return incoming.tier > current.tier;
}

struct RewardBundle {
    struct ItemRow {
        ItemKind kind;
        std::int32_t count;
    };
    std::vector<ItemRow> items;
    std::int32_t essence = 0;
    std::optional<EquippedItem> equipment;
};

// §8: deterministic roll seed, a pure function of already-known local values — reuses this
// project's one splitmix64 mixer (`Rng`, tiles.hpp) rather than inventing a second one.
[[nodiscard]] inline std::uint64_t roll_seed_of(std::uint64_t world_seed, std::uint32_t creature_spawn_id,
                                                 std::uint64_t kill_world_tick,
                                                 std::uint64_t contributor_account_id) noexcept {
    Rng mix(world_seed ^ static_cast<std::uint64_t>(creature_spawn_id) ^ (kill_world_tick << 32) ^
            (contributor_account_id << 20));
    return mix.next();
}

// §7-§9: every entry independently rolled, ring-scaled, and — for Essence/gems/equipment — gated
// on `realm_challenge`, the resolving chunk's own already-primed state (§6.6/§13), never a
// cross-actor query. Essence and the equipment row have NO EXCEPTIONS per GAME.md §1/§12: they
// simply do not fire outside a challenge-realm map, full stop.
[[nodiscard]] inline RewardBundle roll_loot(const LootTable& table, std::uint64_t seed, Ring ring,
                                             bool realm_challenge) noexcept {
    Rng rng(seed);
    RewardBundle b{};
    const RingLootScale rs = ring_loot_scale(ring);
    for (std::uint8_t i = 0; i < table.entry_count; ++i) {
        const LootEntry& e = table.entries[i];
        if (e.realm_gated && !realm_challenge) continue;
        const auto chance = static_cast<std::uint32_t>(
            (static_cast<std::uint32_t>(e.chance_pm) * rs.chance_mult_pm) / 1000);
        if (rng.below(1000) >= std::min<std::uint32_t>(chance, 1000)) continue;
        const std::int32_t qty_max = e.qty_max + (e.ore_slot ? rs.qty_bonus : 0);
        const std::int32_t span = std::max<std::int32_t>(1, qty_max - e.qty_min + 1);
        const std::int32_t qty =
            e.qty_min + static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(span)));
        const ItemKind kind = e.ore_slot ? ore_kind_of(rs.ore_tier) : e.item;
        b.items.push_back({kind, qty});
    }
    if (table.essence_pm > 0 && realm_challenge && rng.below(1000) < table.essence_pm) {
        const std::int32_t span = std::max<std::int32_t>(1, table.essence_qty_max - table.essence_qty_min + 1);
        b.essence = table.essence_qty_min + static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(span)));
    }
    if (table.has_equipment && realm_challenge && rng.below(1000) < table.equipment_chance_pm) {
        EquippedItem item{};
        item.item_id = table.equipment_item_id;
        item.tier = table.equipment_tier;
        item.max_durability = tier_max_durability(table.equipment_tier);
        item.durability = item.max_durability;
        b.equipment = item;
    }
    return b;
}

// --- §10.1: the four Monster-faction loot tables ----------------------------------------------
// A modest ore row (always live) plus one flavor gem row (§6.6-gated). §10.1: "a modest table of
// raw material scaled by the killing creature's ring" — no stated per-kind differentiation beyond
// ring, so all four share the same shape; the gem channel is a flavor pick per kind, not a rule.
[[nodiscard]] inline constexpr LootTable monster_loot_table(ItemKind gem, std::uint16_t gem_chance_pm,
                                                              std::uint16_t essence_pm) noexcept {
    LootTable t{};
    t.entries[0] = LootEntry{ItemKind::kWood, 250, 1, 2, /*ore_slot*/ true, /*realm_gated*/ false};
    t.entries[1] = LootEntry{gem, gem_chance_pm, 1, 1, /*ore_slot*/ false, /*realm_gated*/ true};
    t.entry_count = 2;
    t.essence_pm = essence_pm;
    t.essence_qty_min = 1;
    t.essence_qty_max = 1;
    return t;
}

inline constexpr LootTable kLootSlime = monster_loot_table(ItemKind::kGemCold, 15, 150);
inline constexpr LootTable kLootSpider = monster_loot_table(ItemKind::kGemShock, 15, 150);
inline constexpr LootTable kLootGhost = monster_loot_table(ItemKind::kGemWet, 15, 150);
inline constexpr LootTable kLootSkull = monster_loot_table(ItemKind::kGemEarth, 15, 150);

// --- §10.2: the one shipped boss (dojo Giant Red Samurai) ---------------------------------------
// Named gear ids — a stand-in for the RFC-008 `gear.*` document domain (see header note): this
// project has exactly one authored equipment drop, so a closed enum is honest rather than
// under-building a lookup table with one row in it.
enum class GearId : std::uint16_t { kNone = 0, kCrimsonKatana = 1 };

[[nodiscard]] inline constexpr LootTable boss_loot_table() noexcept {
    LootTable t{};
    // A guaranteed material row — a boss kill is never a worse payout than an ordinary kill of the
    // same ring (§10.2).
    t.entries[0] = LootEntry{ItemKind::kWood, 1000, 3, 5, /*ore_slot*/ true, /*realm_gated*/ false};
    t.entry_count = 1;
    t.essence_pm = 350;
    t.essence_qty_min = 1;
    t.essence_qty_max = 2;
    t.has_equipment = true;
    t.equipment_chance_pm = 8;  // within the RFC's 5-10‰ tunable range
    t.equipment_slot = EquipSlot::kWeapon;
    t.equipment_tier = MaterialTier::kMythril;
    t.equipment_item_id = static_cast<std::uint16_t>(GearId::kCrimsonKatana);
    return t;
}
inline constexpr LootTable kLootBossSamurai = boss_loot_table();

[[nodiscard]] inline constexpr const LootTable& loot_table_of(CreatureKind k) noexcept {
    switch (k) {
        case CreatureKind::kSlime: return kLootSlime;
        case CreatureKind::kSpider: return kLootSpider;
        case CreatureKind::kGhost: return kLootGhost;
        case CreatureKind::kSkull: return kLootSkull;
        case CreatureKind::kBoss: return kLootBossSamurai;
        // kWild kinds (boar/wolf/bear/hare/chicken) never reach this — `credit_kill`'s existing
        // `Faction::kWild` branch (flat 1 produce) is unchanged by this RFC and never calls here.
        case CreatureKind::kBoar:
        case CreatureKind::kWolf:
        case CreatureKind::kBear:
        case CreatureKind::kHare:
        case CreatureKind::kChicken:
        case CreatureKind::kCount: break;
    }
    return kLootSlime;
}

}  // namespace mmo
