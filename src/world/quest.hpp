// RFC-020 — Mission & Quest System.
//
// A quest is an invitation the world already extends, written down: an `Offered -> Accepted ->
// Complete` state machine (with `Abandoned` free and reachable from either active state, and
// `Expired` reserved for emergent instances this pass does not build — see below), fed by
// `GameplayFact`s the untrusted chunk actor where an action happened hands to the trusted
// `PlayerActor` that owns the matching `QuestInstance` — the same one-way `tell` shape
// `loot.hpp::grant()` already uses for `GrantItems`/`GrantEquipment`.
//
// Pure data plus pure functions, mirroring `loot.hpp`/`status.hpp`'s own split — no `ChunkActor`
// dependency, so this header is safe for `protocol.hpp` (wire messages) and `player_actor.hpp`
// (quest-log state) to both include directly.
//
// SCOPE, AND WHY IT STOPS WHERE IT DOES — the RFC's own text, not a shortcut:
//   - Only the `build` and `clear` objective kinds are wired to a real, live `GameplayFact` source
//     this round. `fetch`/`trade`/`craft` need a `giver` NPC or crafting verb to emit `kDeliver`/
//     `kCraft` from — neither exists anywhere in this codebase (confirmed by survey: no
//     `VillageActor`, no NPC dialogue system, no crafting verb), and RFC-020's own Open Question 2
//     already names `kCraft`/`kDeliver` emitters as forward-contract vocabulary for an unspecced
//     future system. `escort` is explicitly "blocked on a future friendly-NPC AI RFC" by the RFC's
//     own §1 taxonomy. `defend`/`assault`/`rumor` (the emergent/community kinds) need a bindable
//     "this village is being raided" / "this fort is assault-able" / "this dungeon gate exists"
//     runtime object — today's raids spawn AT a `Stronghold`'s own tile, not targeting a `Village`,
//     and no fort/dungeon-gate object exists at all (RFC-020's own Open Questions 1, 3, 8 name
//     exactly these gaps as unowned by any RFC yet). Building fake binding plumbing to make them
//     "work" would be inventing content the RFC does not specify, not implementing it.
//   - `QuestState::kOffered` is not a stored state — Q1's guard (unlock check) is evaluated live by
//     `quest_offerable()` every time the Journal or Accept path asks, matching how `UseWaypoint`'s
//     own "USABLE" predicate is computed on demand rather than cached (RFC-021 precedent, cited in
//     `player_actor.hpp`). A `QuestInstance` exists only once `Accepted`.
//   - `Expired` (Q6) does not exist as a reachable state this round — it requires the emergent
//     sources this pass defers. Every authored v1 quest is `auto_complete: true` (rewards pay the
//     instant the last objective closes) because the `giver`-driven `kTalk` turn-in path needs the
//     NPC dialogue system this pass also does not build (RFC-020's own Open Question 3: no RFC
//     assigns a village its `giver` yet).
//   - `party_scope` is always `kSolo` in v1 authored content — `kParty`/`kCommunity` recipient
//     resolution (§7) is real, general-purpose logic problem RFC-020 hands to whichever system owns
//     party membership (explicitly not this RFC's own concern either), so it is not exercised by
//     content this pass ships.
//   - Reward `item_table` rows are authored as a guaranteed, fixed (kind, count) grant, not an
//     RFC-018 `LootTable::roll()` — RFC-018's roll is seeded from a creature kill's own
//     (creature_id, tick, contributor) triple (`loot.hpp::roll_seed_of`), which a quest completion
//     has no natural analog for; a deterministic reward is the correct simplification for content
//     this small, not a missing feature.
//   - `village_standing` rewards have no `VillageActor`-equivalent to pay into (RFC-020's own
//     Non-goals list this by name — "no RFC in the accepted or proposed set currently owns its
//     implementation"). Banked into a per-player counter here as the forward-compatible stand-in a
//     future `VillageActor` will read, the same shape `Gauge` stood in for RFC-009's unbuilt
//     `DefenderSheet` and `EquippedItem` stood in for unbuilt persistence.
//   - Quests are authored as a constexpr C++ table (`kQuestDefs`), not RFC-008 `quest.*`/`giver.*`
//     JSON documents — `loot.hpp`'s own precedent: RFC-008's offline pack loader has no runtime
//     consumer for any domain yet, so a JSON `quest` domain here would be unconsumed content.
#pragma once

#include <algorithm>
#include <cstdint>

#include "world/tiles.hpp"

namespace mmo {

// --- §4: the closed GameplayFact vocabulary -------------------------------------------------------
enum class FactKind : std::uint8_t {
    kKill = 0,
    // value 1 (kGather) is reserved, unassigned to any objective kind this pass — RFC-020's own text
    // leaves it reserved too.
    kCraft = 2,
    kBuild = 3,
    kDeliver = 4,
    kTalk = 5,
    kEnterRegion = 6,
    kDefendTick = 7,
    kAssaultTick = 8,
};

// --- §1: objective kinds and party-credit scope -----------------------------------------------------
enum class ObjectiveKind : std::uint8_t {
    kFetch = 0,
    kCraft = 1,
    kBuild = 2,
    kEscort = 3,
    kClear = 4,
    kDefend = 5,
    kAssault = 6,
    kTrade = 7,
    kRumor = 8,
};

enum class PartyScope : std::uint8_t { kSolo = 0, kParty = 1, kCommunity = 2 };

enum class RewardKind : std::uint8_t { kXp = 0, kVillageStanding = 1, kItemTable = 2 };

inline constexpr int kMaxObjectives = 4;   // QV06
inline constexpr int kMaxRewards = 3;      // within QV11's 1..6
inline constexpr int kMaxActiveQuests = 20;  // QI4 (personal + party combined), RFC-020's own tunable

// A single `{kind, target, count_required}` objective (§ Guide, §2). `target` is the matching
// `FactKind`'s own subject vocabulary: a `BuildKind` ordinal for `kBuild`, a `CreatureKind` ordinal
// for `kClear`.
struct Objective {
    ObjectiveKind kind = ObjectiveKind::kBuild;
    std::uint16_t target = 0;
    std::uint16_t count_required = 1;
};

// One reward hook (§7). `branch`/`by_cause` apply only to `kXp`; `item`/`item_count` only to
// `kItemTable`; `amount` is the xp amount for `kXp` or the standing amount for `kVillageStanding`.
struct RewardHook {
    RewardKind kind = RewardKind::kXp;
    Skill branch = Skill::kMelee;
    bool by_cause = false;
    std::uint32_t amount = 0;
    ItemKind item = ItemKind::kWood;
    std::int32_t item_count = 0;
};

// --- Quest identity and the authored table ----------------------------------------------------------
//
// Two authored quests this pass: one `build` (a real `PlaceBuilding` fact source), one `clear` (the
// existing RFC-019 §5.8 kill-credit ledger). `kClearSlimes` targets `CreatureKind::kSlime`, which is
// `Faction::kMonster` per `tiles.hpp`'s own faction table — satisfying §5's "never a Wild-faction
// harvest target" restriction (QV07), verified by inspection since no pack builder exists to check it
// mechanically yet.
enum class QuestId : std::uint8_t { kFarmPlot = 0, kClearSlimes = 1, kCount = 2 };
inline constexpr int kQuestCount = static_cast<int>(QuestId::kCount);

struct QuestDef {
    QuestId id = QuestId::kFarmPlot;
    PartyScope scope = PartyScope::kSolo;
    std::uint8_t unlock_village_tier_min = 0;
    bool repeatable = false;
    bool auto_complete = true;  // always true this pass — see header note on the giver/kTalk gap
    Objective objectives[kMaxObjectives]{};
    std::uint8_t objective_count = 0;
    RewardHook rewards[kMaxRewards]{};
    std::uint8_t reward_count = 0;
};

inline constexpr QuestDef kQuestDefs[kQuestCount] = {
    // quest.farm_plot — establish a farm plot. One-shot: a second plot does not re-offer this quest,
    // it is just more farmland (matches GAME.md's own framing of `kPlot` as base expansion).
    QuestDef{
        QuestId::kFarmPlot, PartyScope::kSolo, /*unlock*/ 0, /*repeatable*/ false,
        /*auto_complete*/ true,
        {Objective{ObjectiveKind::kBuild, static_cast<std::uint16_t>(BuildKind::kPlot), 1}},
        /*objective_count*/ 1,
        {RewardHook{RewardKind::kXp, Skill::kCraft, false, 25, ItemKind::kWood, 0},
         RewardHook{RewardKind::kItemTable, Skill::kMelee, false, 0, ItemKind::kSeed, 5}},
        /*reward_count*/ 2,
    },
    // quest.clear_the_glade — thin the slime population. Repeatable (QV10: authored + solo only,
    // satisfied) — there is always another glade of slimes, and re-offering costs nothing to author
    // since there is no cooldown field to misuse (Tone Guardrail §3).
    QuestDef{
        QuestId::kClearSlimes, PartyScope::kSolo, /*unlock*/ 0, /*repeatable*/ true,
        /*auto_complete*/ true,
        {Objective{ObjectiveKind::kClear, static_cast<std::uint16_t>(CreatureKind::kSlime), 3}},
        /*objective_count*/ 1,
        {RewardHook{RewardKind::kXp, Skill::kMelee, true, 35, ItemKind::kWood, 0},
         RewardHook{RewardKind::kVillageStanding, Skill::kMelee, false, 10, ItemKind::kWood, 0}},
        /*reward_count*/ 2,
    },
};

[[nodiscard]] inline const QuestDef& quest_def_of(QuestId id) noexcept {
    return kQuestDefs[static_cast<int>(id)];
}

// --- The gameplay-fact payload — also the wire message protocol.hpp fans chunk actors -> PlayerActor
// with directly (no separate wrapper struct; it is already POD-shaped, matching every other message
// in protocol.hpp) ------------------------------------------------------------------------------------
//
// `cause_branch` is `Skill` for `kKill` (RFC-019's own cause-of-death -> branch mapping, reused by
// reference — see `chunk_actor.hpp::credit_kill`'s per-contributor `Contribution::skill`, which IS
// this field). Other fact kinds this pass emits (`kBuild`) leave it at its default.
struct GameplayFact {
    FactKind kind = FactKind::kKill;
    std::uint64_t actor = 0;
    std::uint16_t subject_id = 0;
    std::uint16_t count = 1;
    std::uint8_t cause_branch = 0;
    std::uint32_t tick = 0;
};

[[nodiscard]] inline bool fact_matches(const Objective& obj, const GameplayFact& fact) noexcept {
    switch (obj.kind) {
        case ObjectiveKind::kBuild:
            return fact.kind == FactKind::kBuild && fact.subject_id == obj.target;
        case ObjectiveKind::kClear:
            return fact.kind == FactKind::kKill && fact.subject_id == obj.target;
        default:
            // Not wired to a live fact source this pass — see the header note.
            return false;
    }
}

// --- QuestInstance (Accepted state only — Offered/Expired are not stored, see header note) ----------
struct QuestInstance {
    QuestId id = QuestId::kCount;  // kCount == empty slot
    std::uint16_t count_current[kMaxObjectives]{};
    std::uint8_t dominant_branch = 0;  // last qualifying kKill fact's branch — feeds `by_cause` (§7)
    std::uint32_t accepted_tick = 0;   // QI1: display only, read by zero transition guard
};

[[nodiscard]] inline bool quest_instance_complete(const QuestInstance& qi) noexcept {
    if (qi.id == QuestId::kCount) return false;
    const QuestDef& def = quest_def_of(qi.id);
    for (int i = 0; i < def.objective_count; ++i) {
        if (qi.count_current[i] < def.objectives[i].count_required) return false;
    }
    return true;
}

// Q3: applies one fact to one instance's objectives, clamped to count_required. Returns true if any
// objective's progress changed (so the caller knows to `publish()`/check completion).
[[nodiscard]] inline bool quest_apply_fact(QuestInstance& qi, const GameplayFact& fact) noexcept {
    if (qi.id == QuestId::kCount) return false;
    const QuestDef& def = quest_def_of(qi.id);
    bool changed = false;
    for (int i = 0; i < def.objective_count; ++i) {
        const Objective& obj = def.objectives[i];
        if (!fact_matches(obj, fact)) continue;
        const std::uint16_t before = qi.count_current[i];
        const std::uint32_t after = std::min<std::uint32_t>(
            obj.count_required, static_cast<std::uint32_t>(before) + fact.count);
        qi.count_current[i] = static_cast<std::uint16_t>(after);
        if (qi.count_current[i] != before) changed = true;
    }
    if (changed && fact.kind == FactKind::kKill) qi.dominant_branch = fact.cause_branch;
    return changed;
}

// Q1's unlock guard, evaluated live (see header note) — `already_active`/`already_completed` are the
// caller's own QI3/repeatable checks, kept out of this pure function so it stays a one-line predicate
// over caller-supplied facts rather than reaching into `PlayerActor` state itself.
[[nodiscard]] inline bool quest_offerable(QuestId id, std::uint8_t max_visited_village_tier,
                                          bool already_active, bool already_completed) noexcept {
    if (already_active) return false;
    const QuestDef& def = quest_def_of(id);
    if (already_completed && !def.repeatable) return false;
    if (max_visited_village_tier < def.unlock_village_tier_min) return false;
    return true;
}

}  // namespace mmo
