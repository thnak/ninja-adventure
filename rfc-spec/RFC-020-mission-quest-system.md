# RFC-020: Mission & Quest System

> Status: **Accepted (revised after review)**
> Batch: RFC-019 (Progression & Skill System), **RFC-020 (this document)**, RFC-021 (World
> Structure, Map & Wayfinding) — drafted concurrently against the accepted RFC-001..010 combat set
> and GAME.md / ROADMAP.md / ARCHITECTURE.md.
> Depends on (accepted): RFC-001 (ability pipeline — attribution/caster fields), RFC-004
> (`EntityDef.obs_class`/team — target identification), RFC-008 (data-document conventions, reused
> not redefined), RFC-009 (damage formula only — RFC-009 does not own kill/credit attribution; see
> below).
> Depends on (this batch, Draft): RFC-019 (skill XP branches, the cause-of-death → branch rule that
> quest completion bonuses route through, and §5.8's per-contributor kill-credit ledger that `kKill`
> facts are derived from), RFC-021 (Map screen markers for quests, forts, and raids; wayfinding to a
> quest's location; owns the raid-warning marker's discovery-gated visibility, §5.3).
> Depends on (proposed, not yet specced): RFC-011 (HUD raid-alert presentation), RFC-013
> (vitals/death — quests never define death penalties), RFC-014 (instance/realm lifecycle — dungeon
> entry), RFC-016 (persistence — quest log save format), RFC-018 (loot/reward value tables).

---

## Summary

A quest is **an invitation the world already extends, written down**. RFC-020 defines:

- **Taxonomy.** Two sources — **authored templates** (fetch/craft/build/escort/clear, hand-placed at
  village quest-givers) and **emergent opportunities** (raid defense, fort assault, dungeon rumors —
  instantiated at runtime from a world event, never authored per-instance). One state machine, one
  Journal, one reward-hook contract, both sources.
- **The state machine.** `Offered → Accepted → Complete`, with `Abandoned` reachable from either
  active state at zero cost, and `Expired` reachable only by emergent instances whose bound world
  event concluded without the player. **No transition is triggered by elapsed wall-clock or
  game-calendar time** — every transition is either a player action or a simulated event's own
  outcome (a raid resolving in combat, not a timer).
- **Sources.** Village quest-givers (unlocked at tier ≥2, per GAME.md §6), the scout's one-day-ahead
  raid warning (GAME.md §6), and world discoveries (an active fort, a dungeon gate) — all surfacing
  through **one screen, the Journal (`J`)**.
- **Credit and multiplayer semantics.** Solo quests belong to one player's trusted actor; party
  quests (2–4, matching GAME.md §11's dungeon group size) share credit unconditionally among current
  party members; community events (raid defense) track a contribution ledger open to any number of
  participants, paying out proportional to contribution with a floor so showing up always counts for
  something.
- **Reward hooks.** A quest completion pays into three systems by reference, never by value: XP
  (RFC-019's branches), village standing (an unowned-as-of-yet `VillageActor`-equivalent — GAME.md
  §6 canon), and item tables (RFC-018, proposed — this RFC carries only the reference id).
- **Data format.** `data/quests/**` — strict JSON, dotted ids, `ids.lock.json`, struct/full hashing,
  schema versioning: **RFC-008's general contract, reused by citation, not re-derived.** This RFC
  defines only the quest- and giver-specific document schemas and their own validation rules (`QV`
  namespace, disjoint from RFC-008's `V` namespace).
- **NPC presentation.** Quest-givers speak through the shipped `Faceset.png` portraits (one per
  character, GAME.md §13) in a plain offer/progress/complete dialogue box — no branching dialogue
  trees in v1.

---

## Motivation

Three forces, and one hard constraint that overrides all of them:

1. **GAME.md §6 already ships a village economy with no quest layer to narrate it.** Villages tier
   up when traded with, defended, connected, or freed from a nearby fort — but nothing today tells
   the player *which* village needs *what*, or that a raid is happening three villages over and they
   are welcome to help. A quest system does not invent new mechanics here; it packages mechanics
   GAME.md already decided into something the player can see and choose.

2. **Raids, forts, and dungeon "generation" numbers are already visible-but-mute.** ROADMAP P1
   shipped random raids (10%/fort/night today, tuning toward GAME.md's 30%/village/month), and P8
   will ship dungeons whose monsters visibly train ("Thế hệ 47"). Both are *content the world already
   generates*; without a quest layer surfacing them, a player has no way to discover "a raid is
   happening" or "this dungeon has been training for three weeks" short of stumbling into it.

3. **RFC-018's loot tables and RFC-019's skill branches both need a caller.** Neither RFC invents a
   reason to *hand out* a reward — something has to decide when a reward is earned and route it.
   Quests are that caller for a large slice of directed play (the rest is raw combat/farming XP,
   already RFC-019's).

**The overriding constraint (GAME.md §0):** *nothing counts down behind the player's back.* Every
prior quest-system instinct — daily boards, escort timers, "the raid ends in 04:59" — is exactly the
"thế giới đang thua" pressure GAME.md explicitly rejected for villages in §6. This RFC is written
under the same discipline RFC-001 and RFC-008 already proved out for combat: **a system with no
wall-clock field cannot violate the guardrail no matter how it is played**, so the schema and state
machine below are built to make a hidden timer *inexpressible*, not merely undesired.

---

## Guide-level Explanation

### For a player

You never see a quest log with a red exclamation mark demanding attention. You see the **Journal
(`J`)**, and inside it a **Quests** tab with three lists:

- **Active** — what you've accepted, with a progress count ("Wolfpelts: 2/5") and one always-visible
  **Abandon** button. Abandoning costs nothing, asks nothing, and says nothing — the quest just isn't
  there next time you open the Journal.
- **Nearby & known** — offers you can accept: a villager standing at a tier-≥2 settlement with
  something on their mind (talk to them, see the offer, take it or don't), and world-visible
  **community events** — right now, somewhere on the map, a village is being raided and its guards
  could use help, or a fort has been left alone long enough that its watch has gotten sloppy. These
  show up whether or not you ever talk to anyone — the scout who warned the village a day ago already
  told the world, not you specifically.
- **Completed** *(session log only — see Non-goals)* — a quiet record of what you finished.

Nothing here has a deadline. If you accept "repair the well" today and finish it next month, the
reward is identical. If a raid is announced and you're three biome rings away farming, the village
defends itself — RFC-020 adds nothing that changes that outcome; it only adds the *option* to go
help, and the option simply stops being offerable once that particular raid is over, exactly the way
you can't un-miss yesterday's rain.

**Trade requests** are the one quest type that reappears: a tier-≥2 village always has a standing
request for whatever it's short of. It isn't a daily reset — it's demand. Sell them a full wagon of
lumber and the request goes away until their stock runs low again, whether that takes an hour or a
season.

### For a designer

You author a quest as one small JSON file that answers four questions:

1. **Who offers it, and when does it become available?** — a `giver` (a named villager with a
   portrait) gated by `unlock.village_tier_min`, or `source: emergent` bound to a runtime village/
   fort/dungeon-gate instance instead of a fixed giver.
2. **What has to happen?** — 1 to 4 objectives (`fetch`/`craft`/`build`/`escort`/`clear`/`defend`/
   `assault`/`trade`/`rumor`), each a `{kind, target, count_required}` triple that increments from
   the same gameplay-fact stream that already feeds RFC-019's XP.
3. **Who shares credit?** — `solo`, `party` (2–4, unconditional sharing), or `community` (open,
   contribution-weighted).
4. **What does it pay?** — a small list of reward *hooks*, never reward *values*: an XP branch and
   amount (RFC-019), a village-standing nudge, and/or a reference into RFC-018's future loot tables.

You never write a countdown, a day-of-week, or a "resets at" field, because **the schema has none** —
the same structural guarantee RFC-008 gives combat data (§ Reference-level Design, QV09).

### For an engineer

Two runtime pieces, matching where the *authority* for the underlying fact already lives:

- **Personal/party `QuestInstance`s** live on the trusted actor(s) of their owner(s) — the same tier
  as `PlayerActor`'s cooldown table (RFC-001 §8's `ready_at_tick[slot]` pattern), because a quest's
  progress is exactly as trustworthy as the player's own vitals and inventory already have to be.
- **Community `QuestInstance`s** (raid defense, fort assault) live on the trusted actor that will
  eventually own the bound village/fort's authoritative state (ARCHITECTURE.md §3: village state is
  leader-side SQLite) — a contribution ledger keyed by player, capped, and reset when the bound event
  resolves.

Both kinds are **event-driven, never polled**: a `GameplayFact` message increments a matching
objective the instant it happens, and nothing about a `QuestInstance` needs attention on ticks where
nothing happened — the same "degrade by doing less, never queue catch-up" LOD discipline RFC-001 §4
and RFC-008 §11 already established for combat state.

---

## Reference-level Design

### 1. Taxonomy

Nine objective **kinds**, in two **sources**:

| Source | Kinds | Instantiation |
|---|---|---|
| `authored` | `fetch`, `craft`, `build`, `escort`, `trade` | one JSON document per quest, hand-placed at a `giver`; `escort` is **blocked on a future friendly-NPC AI RFC** (Open Question 1) — its data shape and completion fact are specced here, but it cannot ship until that RFC lands |
| `authored` | `clear` | one JSON document; targets a fixed creature/region tag (a specific glade, a specific fort's monster archetype) |
| `emergent` | `defend` | **template**, parameterized at raid-roll time (GAME.md §6) against the specific village rolled |
| `emergent` | `assault` | **template**, parameterized against a specific fort the player has discovered |
| `emergent` | `rumor` | **template**, parameterized against a specific dungeon gate the player has discovered (P8) |

An emergent document is a **template**: it is authored once (`quest.raid_defense`,
`quest.fort_watch_weak`, `quest.dungeon_rumor`) and *instantiated* many times at runtime with a
`bound_entity` filled in — the same archetype-vs-instance split RFC-004 §7.5 already uses for
`entity.*` documents vs. live `CombatEntity`s. Nothing about the template itself is per-village; the
binding is a runtime fact, not authored data.

### 2. Quest document schema (`quest.*`)

```
data/quests/
  pack.json                  # schema version, pack name — mirrors RFC-008 §2's manifest
  ids.lock.json              # append-only id ledger — RFC-008 §4's mechanism, own id space
  quests/<name>.quest.json   # domain "quest"
  givers/<name>.giver.json   # domain "giver"
```

Two domains only (`quest`, `giver`) — the quest surface is narrower than combat's six-domain set, so
it does not need one.

```json
{
  "schema": 1,
  "id": "quest.village_well_repair",
  "source": "authored",
  "giver": "giver.elder_maru",
  "unlock": {"village_tier_min": 2, "requires_quest": null},
  "repeatable": false,
  "auto_complete": false,
  "party_scope": "solo",
  "objectives": [
    {"kind": "build", "target": "structure.well", "count_required": 1}
  ],
  "dialogue": {
    "offer": "text.quest.village_well_repair.offer",
    "progress": "text.quest.village_well_repair.progress",
    "complete": "text.quest.village_well_repair.complete"
  },
  "rewards": [
    {"kind": "xp", "branch": "trades", "amount": 40},
    {"kind": "village_standing", "amount": 10},
    {"kind": "item_table", "ref": "loot.village_gratitude_small"}
  ]
}
```

An emergent template omits `giver`, sets `source: emergent`, and adds `binds_to`:

```json
{
  "schema": 1,
  "id": "quest.raid_defense",
  "source": "emergent",
  "binds_to": "village",
  "unlock": {"village_tier_min": 0},
  "repeatable": false,
  "auto_complete": true,
  "party_scope": "community",
  "objectives": [
    {"kind": "defend", "target": "$bound", "count_required": 1}
  ],
  "dialogue": {"offer": "text.quest.raid_defense.offer"},
  "rewards": [
    {"kind": "xp", "branch": "by_cause", "amount": 50},
    {"kind": "village_standing", "amount": 15},
    {"kind": "item_table", "ref": "loot.raid_defense_participation"}
  ]
}
```

`"target": "$bound"` is the sole placeholder token this schema defines: at instantiation the runtime
substitutes the actual bound entity's id. No other templating exists — this keeps the format a
strict-JSON leaf document, matching RFC-008's anti-scripting stance (§7.6 "no scripting, no arbitrary
graphs") rather than inventing a second templating language.

### 3. Giver document schema (`giver.*`)

```json
{
  "schema": 1,
  "id": "giver.elder_maru",
  "name": "Elder Maru",
  "faceset": "ninja/Actor/Character/Elder1/Faceset.png",
  "dialogue_display_ticks": 6
}
```

`faceset` resolves exactly like RFC-008 §7.3's icon map — a path check at pack build time (QV16), no
scaling or cropping logic (the pack already ships one portrait per character at a fixed size, GAME.md
§13). `dialogue_display_ticks` is the **only** `_ticks` field this schema permits anywhere (QV09): it
is a client-side text-reveal pacing value for one dialogue box, bounded `1..30` — a UI animation
duration, not anything that outlives the interaction.

### 4. Objective kinds and the `GameplayFact` vocabulary

Nine objective kinds map onto a closed `FactKind` enum. Progress is **event-driven**: a fact matching
an active objective's `(kind, target)` increments `objectives[i].count_current` by `fact.count`,
clamped to `count_required`.

```cpp
enum class FactKind : std::uint8_t {
    kKill = 0, kCraft = 2, kBuild = 3, kDeliver = 4,
    kTalk = 5, kEnterRegion = 6, kDefendTick = 7, kAssaultTick = 8,
    // value 1 (kGather) is reserved, not yet assigned to any objective kind — see Open Questions
};

struct GameplayFact {
    FactKind        kind;
    std::uint64_t   actor;          // player key that performed the action
    std::uint16_t   subject_id;     // creature obs_class / item id / structure kind / npc id / region id
    std::uint16_t   count;          // usually 1; batched gather can be >1
    std::uint8_t    cause_branch;   // kKill only: RFC-019's branch-by-cause (how the creature died).
                                     // kDefendTick derives it from the participant's co-incident kKill
                                     // facts during that tick; kAssaultTick derives it from the landed
                                     // hit's own branch. Neither silently defaults to branch 0 (Melee).
    std::uint32_t   tick;           // absolute world tick — informational, never gates a transition
};
```

| Objective kind | `FactKind` | Notes |
|---|---|---|
| `fetch` | `kDeliver` | player hands `count_required` of `target` (an item id) to the giver, or drops it at a marked point for emergent variants |
| `craft` | `kCraft` | emitted by whichever system implements crafting (not yet specced — see Open Questions) |
| `build` | `kBuild` | emitted by the Build (`B`) screen's placement verb (GAME.md §6b: whole-structure placement) |
| `escort` | `kEnterRegion` | the escorted NPC's arrival at the destination region, attributed to the escorting player — see Open Questions on escort-AI ownership |
| `clear` | `kKill` | see §5 for the disposition/faction restriction on valid kill targets |
| `defend` | `kDefendTick` | emitted once per tick a player is in combat inside the bound village's raid perimeter during an active raid, per participant |
| `assault` | `kAssaultTick` | emitted once per qualifying hit landed on the bound fort's defenders/structures |
| `trade` | `kDeliver` | identical mechanism to `fetch`; kept as a separate kind only because its `target`/`count_required` are recomputed from village stock (§8), not authored fixed values |
| `rumor` | `kEnterRegion` | satisfied by entering the bound dungeon gate's instance once (RFC-014 owns everything past that point) |

`kKill` facts are the **same facts RFC-019 consumes for combat XP** — RFC-020 does not re-derive
kill attribution; it co-subscribes to RFC-019 §5.8's existing per-contributor kill-credit ledger
(`kAssistWindowTicks`, "every contributing player receives the full kill-XP amount") and RFC-019's
cause-of-death → branch mapping (`cause_branch`), reusing both by reference. This resolves the
party-scope `clear` question directly: a `kKill` fact is emitted **per qualifying contributor**, per
RFC-019 §5.8's ledger — not only to whichever player landed the last hit. `kCraft`/
`kBuild`/`kDeliver` are vocabulary this RFC introduces as a **forward contract**: no accepted or
proposed RFC yet owns crafting/economy (ROADMAP P4), so this table is what that future system must
emit to make `fetch`/`craft`/`build`/`trade` objectives work. This is stated plainly as a dependency,
not assumed away (see Open Questions).

### 5. `clear` objective target restriction (GAME.md §5 faction/disposition)

A `clear` objective's `target` must resolve to a creature archetype whose **default disposition is
Hostile**, or explicitly whose faction is `Monster` (GAME.md §5's faction table). A quest can never
require killing a `Wild`-faction creature at Neutral or Tame disposition as an objective — those are
harvest/husbandry content (GAME.md §5 "Hiền → thịt, da, lông... nguồn vật nuôi"), and turning them
into a kill-count objective would frame chill wildlife content as combat content, which is not this
RFC's call to make. `QV07` enforces this at pack build time by cross-checking the target id against
the (not-yet-formalized-as-data, currently `tiles.hpp`-resident) faction/disposition table; until
that table is itself data-driven, the packer ships a hand-maintained allowlist mirroring it, refreshed
the same way RFC-008 §2's `capabilities/boss_poses.json` is refreshed from a measurement pass.

### 6. The state machine

```
                  ┌─── player declines / walks away ───┐
                  ▼                                     │
 (none) ──Q1──▶ Offered ──Q2──▶ Accepted ──Q3(loop)──▶ Accepted ──Q4──▶ Complete
                  │                │                                      │
                  │                Q5 (Abandon, any time, free) ──────────┤
                  │                Q6 (emergent only: bound event         │
                  │                    concluded without the player)      │
                  ▼                ▼                                     ▼
              (removed)        Abandoned                             (reward paid,
                                (or Expired,                           §7)
                                 emergent-only)
```

| # | From → To | Guard | Effect |
|---|---|---|---|
| Q1 | (none) → Offered | authored: player enters interaction range of an unlocked `giver` (village tier ≥ `unlock.village_tier_min`, `unlock.requires_quest` completed if set); emergent: a world event instantiates the template (raid roll, fort discovery, dungeon-gate discovery) | template bound to `bound_entity` (emergent only); offer surfaced to Journal "Nearby & known" and, for `giver`-sourced quests, to the dialogue box |
| Q2 | Offered → Accepted | player accepts (dialogue "Accept" or Journal "Accept"); community-scope quests auto-accept for any player who performs a qualifying action inside the event's radius (no separate accept step — showing up *is* accepting, matching the "invitation" framing) | solo/party: one `QuestInstance` created on the owner's (or each party member's) trusted actor, `objectives[*].count_current = 0`; community: no per-player instance — the single village/fort actor's existing `QuestInstance` gains a ledger entry for the accepting player (§ Multiplayer) |
| Q3 | Accepted → Accepted | a `GameplayFact` matches an objective's `(kind, target)` | `count_current = min(count_required, count_current + fact.count)` |
| Q4 | Accepted → Complete | all `objectives[i].count_current == count_required` | if `auto_complete`, pay rewards (§7) immediately; else the quest is marked ready and rewards pay on the next `kTalk` fact directed at the `giver` |
| Q5 | Offered/Accepted → Abandoned | explicit player action (Journal "Abandon", or declining the offer) | `QuestInstance` deleted; **no cooldown, no penalty, no log entry visible as a failure**; if `repeatable`, immediately re-offerable |
| Q6 | Accepted (emergent only) → Expired | the bound world event reaches its own natural conclusion with objectives not fully met: `defend` — the raid's own combat resolves (guards win or lose, per § Multiplayer); `assault` — the bound fort stops being a valid target because it is **closed** (GAME.md §6's "một cứ điểm gần đó bị đóng" tier-up trigger — the exact closing mechanism is not yet owned by any RFC, RFC-021 §3.4 explicitly leaves fort/raid mechanics unassigned; treat as *concretely trigger-able but mechanism-TBD*, tracked in Open Questions rather than assumed settled); `rumor` — the dungeon gate instance the rumor pointed at is torn down per RFC-014 | `QuestInstance` deleted; **externally identical to Q5** — removed from Journal with no failure marker, no stat, no message framed as loss |

Invariants:

- **QI1 — no time-triggered transition exists.** Every guard above names a player action or a
  simulated event's own state change. No guard reads `now - accepted_tick` or any calendar field.
  `accepted_tick` (absolute world tick) is stored for *display only* ("accepted 3 days ago" in the
  Journal, purely informational) and participates in zero transition guard.
- **QI2 — Abandon and Expire are behaviorally identical from the player's side.** Both delete the
  instance silently. The only difference is *who* triggered it, which is why they are named
  separately internally (telemetry/UX copy can distinguish "you chose to drop this" from "this
  moment passed") — neither carries a penalty, and neither blocks re-offering a `repeatable` quest.
- **QI3 — one active instance per (owner, quest id).** A player cannot hold two concurrent
  `Accepted` instances of the same `quest.*` id (mirrors RFC-001 I1's "one head per caster" —
  simple, and prevents double-counting the same fact stream against two copies of the same
  objective).
- **QI4 — active instance cap.** `kMaxActiveQuests = 20` **(tunable)** per player (personal + party
  combined); Q1 is refused (offer simply doesn't surface) past the cap. No cap on lifetime completed
  count. This bounds `QuestInstance` replication cost the same way RFC-001 §4 caps travel bodies and
  persist records per chunk.

### 7. Reward hooks

Reward *kinds* are closed (QV11); reward *values* for `item_table` are not this RFC's to define.

| Kind | Paid by | RFC-020's contribution |
|---|---|---|
| `xp` | RFC-019 (this batch) | resolves `branch` (fixed, or `by_cause` resolved from the dominant `cause_branch` among the quest's qualifying facts), computes `amount × ring_multiplier(ring)`, calls RFC-019's grant entry point — **subject to the 34-point cap rule below** |
| `village_standing` | the future `VillageActor`-equivalent (unowned — GAME.md §6 canon, no RFC yet) | emits a `VillageStandingEvent{village_id, amount}` fact; **RFC-020 does not compute tier thresholds or decide what standing does** — see Non-goals |
| `item_table` | RFC-018 (proposed) | carries only `ref: "loot.<name>"`; RFC-020 calls `RFC-018.roll(ref, recipient, share_pm)` once per recipient — the roll math is entirely RFC-018's |

**34-point skill cap (RFC-019's delegation, satisfied here).** RFC-019's Interactions table states
"a quest cannot hand out a grant that pushes total levels past 34 — RFC-020 must clamp or refuse."
RFC-020 clamps, it never refuses the quest itself: a completion whose `xp` reward would push
`Σ level_[Skill]` past 34 still **banks** the XP into `xp_[branch]` (same field RFC-019 §5.7 already
uses to hold banked-but-uncommitted XP behind the Essence gate), but the level-up that XP would
otherwise trigger **does not commit** until the player has freed a point elsewhere in the 34-point
budget — mirroring RFC-019 §5.7's "waits, it does not count down" Essence-gate pattern exactly, so no
new tone-guardrail surface is introduced. This is a runtime payout rule, not a build-time authoring
error — no quest document is invalid for naming an `xp` reward regardless of a future recipient's cap
headroom (see new `QV18`, §9).

**Ring multiplier** (world difficulty ring, GAME.md §4 — *not* village tier, a different axis):

| Ring | 0 (Meadow) | 1 (Forest) | 2 (Swamp/Desert) | 3 (Snow) | 4 (Ashlands) |
|---|---|---|---|---|---|
| `ring_multiplier` (tunable) | ×1.0 | ×1.3 | ×1.7 | ×2.2 | ×3.0 |

For an authored quest, `ring` is the ring of the giver's village. For an emergent quest, `ring` is
the ring of `bound_entity`'s location. This loosely mirrors — but is a distinct table from — the
combat HP/damage ring curve ROADMAP P2 already shipped (×1→×5); a quest's XP bonus is a smaller
quantity than raw combat scaling and gets its own tunable curve rather than reusing combat's numbers
verbatim.

**Recipient resolution and payout share:**

| `party_scope` | Recipients | `share_pm` (permille of full reward) |
|---|---|---|
| `solo` | the accepting player | `1000` always |
| `party` | every player who was in the accepting player's party for **at least one qualifying `GameplayFact`** while the quest was `Accepted`, regardless of distance or how much they individually contributed | `1000` for every recipient — sharing is unconditional *in amount* (a party member who spent the quest gathering materials elsewhere still gets the full share, matching RFC-019 §5.8's "full credit, not a split pool" philosophy), but not unconditional *in eligibility*: a player who joined the party after Q4-triggering facts already stopped arriving contributes nothing and is not a recipient, closing the zero-contribution party-hop that an unbounded distance/time reading would otherwise leave open. Bounded further by the 2–4 party-size cap regardless. |
| `community` | every player with `contribution[player] > 0` in the event's ledger | `share_pm = min(1000, 300 + 700 × min(1000, contribution[player] × 1000 / kFullCreditContribution) / 1000)` — a floor of 300‰ for any nonzero contribution, scaling to full credit at `kFullCreditContribution = 5` qualifying facts **(tunable)**. Worked example: `contribution=1` → inner `min(1000, 200) = 200` → `300 + 700×200/1000 = 440`; `contribution=5` → inner `min(1000, 1000) = 1000` → `300 + 700×1000/1000 = 1000`; `contribution=3` → inner `600` → `720`. |

Base `xp.amount` and `village_standing.amount` are multiplied by `share_pm / 1000` before payout;
`item_table.roll` receives `share_pm` as a weighting hint whose exact use is RFC-018's.

**Example completion bonus table** (all **(tunable)**, before ring multiplier and `share_pm`):

| Quest kind | Base XP bonus | Branch |
|---|---|---|
| `fetch` / `trade` | 15–20 | `trades` |
| `craft` | 25 | `trades` |
| `build` | 40 | `trades` |
| `escort` | 30 | `trades` — `by_cause` is not available to `escort` (its only fact is `kEnterRegion`, which carries no `cause_branch`; see QV12) |
| `clear` | 35 | `by_cause` |
| `defend` | 50 | `by_cause` |
| `assault` | 60 | `by_cause` |
| `rumor` | 10 | `trades` (finding it is scouting knowledge, not combat) |

### 8. Trade request pricing (illustrative formula, flagged for a future economy RFC)

A tier-≥2 village's `trade` quest is **recomputed from village stock state**, not authored per
village:

```
count_required = ceil(village_population / 5) × village_tier          // whole units
shortage_resource = argmin_r( stock[r] / shortage_threshold[village_tier][r] )
```

| `village_tier` | `shortage_threshold` per resource category (tunable) |
|---|---|
| 2 | 50 |
| 3 | 100 |
| 4 | 200 |

This is presented as a **worked formula this RFC needs to make `trade` concrete**, not as economy
ownership: village population, stock tracking, and resource categories belong to whichever system
eventually implements the village economy (ROADMAP P4, unspecced — see Non-goals). If that system
lands with different numbers, this formula is the part of RFC-020 that yields, not `trade`'s
existence as a quest kind.

To avoid Journal thrash when stock hovers near the threshold, a fulfilled `trade` request is not
recomputed until the player's **next interaction** with that giver (talk or Journal open) rather than
on a background timer — a debounce triggered by player attention, not by elapsed time (QI1
compliance; see Tone Guardrail Compliance).

### 9. Validation rules (`QV` namespace — disjoint from RFC-008's `V01`–`V45`)

| Rule | Check |
|---|---|
| QV01 | `id = <domain>.<file-stem>`, file under the matching directory (mirrors RFC-008 V05) |
| QV02 | `source ∈ {authored, emergent}` |
| QV03 | objective `kind ∈ {fetch, craft, build, escort, clear, defend, assault, trade, rumor}` |
| QV04 | `party_scope ∈ {solo, party, community}`; `community` only with `source: emergent` and non-null `binds_to` |
| QV05 | `authored` requires `giver`, forbids `binds_to`; `emergent` forbids `giver`, requires `binds_to ∈ {village, fort, dungeon_gate}` |
| QV06 | `1 ≤ objectives.length ≤ kMaxObjectives = 4` (tunable) |
| QV07 | `clear`/`defend`/`assault` targets restricted to Hostile-disposition/`Monster`-faction archetypes (§5) |
| QV08 | `objective.count_required ∈ 1..9999` (`u16`) |
| QV09 | no field in `quest.*`/`giver.*` represents a real-world or calendar duration; the sole permitted `_ticks` field is `giver.dialogue_display_ticks` (`1..30`) |
| QV10 | `repeatable: true` requires `source: authored` and `party_scope: solo` |
| QV11 | `1 ≤ rewards.length ≤ 6` (tunable); `reward.kind ∈ {xp, village_standing, item_table}` |
| QV12 | `reward.kind: xp` requires `branch ∈ {melee, ranged, magic, trades, by_cause}`; `by_cause` requires at least one `clear`/`defend`/`assault` objective present (never `escort` — its sole fact `kEnterRegion` carries no `cause_branch`, §4) |
| QV13 | `reward.kind: item_table` `ref` matches `loot.[a-z0-9_]+`; resolution against RFC-018's table is a non-fatal warning until RFC-018 lands |
| QV14 | `unlock.village_tier_min ∈ 0..4` |
| QV15 | `unlock.requires_quest`, if present, references an existing non-retired `quest.*` id; the requirement graph is acyclic (mirrors RFC-008 V38) |
| QV16 | `giver.faceset` PNG exists under the character portrait tree |
| QV17 | every `dialogue.*` value is a text key (`text.quest.<id>.<slot>`), never an inline literal string |
| QV18 | *(runtime, not build-time)* an `xp` reward payout that would push a recipient's `Σ level_[Skill]` past RFC-019's 34-point cap still banks the XP into `xp_[branch]`; the pending level-up does not commit until cap headroom exists (§7) — no `quest.*` document is invalid for this at pack-build time |

### 10. Build pipeline and cluster identity

`tools/build_quest_pack.py` produces `assets/_gen/quest_pack.json` + `quest_pack.hash.json`
(struct/full hash pair) exactly as RFC-008 §5–§6 specify for the combat pack — same canonicalization
rules (sorted keys, no floats, `notes` stripped), same id-space-partition pattern in
`data/quests/ids.lock.json` (its own `next`/`assigned`/`retired` ledger, independent of combat's).
The quest pack's `(major, minor, struct_hash, full_hash)` 4-tuple is exchanged **alongside** the
combat pack's tuple in the same cluster-join handshake RFC-008 §6 defines (extending the exchanged
set by one entry, not modifying RFC-008's mechanism) — a mismatched quest pack refuses the join,
identically to a mismatched combat pack.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001** (accepted) | `AbilityPayload.caster` is the attribution root `kKill` facts trace back to; RFC-020 does not touch the ability pipeline. |
| **RFC-004** (accepted) | `EntityDef.obs_class`/team fields identify valid `clear`/`defend`/`assault` targets (§5). |
| **RFC-008** (accepted) | Owns the general data-document contract (strict JSON, `ids.lock.json`, canonical form, two-hash versioning, cluster-join identity check) this RFC reuses by citation for `data/quests/**`; RFC-020 defines only the quest/giver-specific schemas and its own `QV` rule namespace. |
| **RFC-009** (accepted) | Owns the damage formula (DR, flat reduction, build-up gauge) only — RFC-009 contains no kill-credit/attribution mechanism; see RFC-019 below. |
| **RFC-019** (this batch, Draft) | Owns skill XP amounts, the cause-of-death → branch mapping, and (§5.8) the per-contributor kill-credit ledger that `kKill`/`kDefendTick`/`kAssaultTick` facts are derived from; quest `xp` rewards and `by_cause` resolution route through both. RFC-020 only adds a completion *bonus* on top of RFC-019's normal per-action XP, never replaces it. RFC-019 also delegates the 34-point skill-cap clamp on quest XP grants to RFC-020 (§7). |
| **RFC-021** (this batch, Draft) | Owns the Map (`M`) screen's rendering of quest/raid/fort markers, wayfinding to them, and — per §5.3 — the raid-warning marker's discovery-gated visibility rule itself (world-visible once the target village is discovered, independent of interest set); RFC-020 emits the marker *data* (kind, location, status) that RFC-021 presents and does not re-specify who can see it. |
| **RFC-011** (proposed) | Owns the in-combat HUD raid-alert presentation GAME.md §12 lists ("cảnh báo đợt tấn công"); RFC-020 only supplies the underlying raid-defense `QuestInstance`'s existence and location as the data that alert would read. |
| **RFC-013** (proposed) | Owns vitals/death/recovery; RFC-020 defines no death penalty for failing or dying mid-quest — dying during a `clear`/`defend`/`assault` objective simply means those facts stop arriving until the player is back up, with no separate quest-side consequence. |
| **RFC-014** (proposed) | Owns instance/realm lifecycle; `rumor` quests point at a dungeon gate and are satisfied by entering, but everything past the gate (instance allocation, group binding, timeout) is RFC-014's. |
| **RFC-015** (proposed) | Owns the replication/interest-set protocol that community-event *participation* (auto-accept, contribution accrual — not the offer's visibility, which is RFC-021's discovery-gated marker) will eventually ride on; RFC-020 only states the participation-gating *rule* (§ Multiplayer), not the wire protocol. |
| **RFC-016** (proposed) | Owns the save-file format; quest log persistence (which `QuestInstance`s survive a restart, completed-quest history) is explicitly deferred there. |
| **RFC-018** (proposed) | Owns loot/reward *value* tables; RFC-020's `item_table` reward carries only a reference id. |

---

## Multiplayer & Simulation-LOD Considerations

**Ownership split**, mirroring RFC-001 §4's head/tail split by trust tier:

```
 SOLO / PARTY QuestInstance                    COMMUNITY QuestInstance
 lives on: owner's/each party member's         lives on: the trusted actor that will own
   trusted actor (PlayerActor tier)               the bound village/fort's state (leader
                                                    tier, ARCHITECTURE.md §3)
 GameplayFact arrives from: the (untrusted)     GameplayFact arrives from: any chunk actor
   chunk actor where the action happened,         inside the event's bound region — a
   same one-message pattern as RFC-001 §4's        contribution ledger entry per player,
   AbilityPayload handoff                          capped at kMaxParticipants = 50 (tunable,
                                                     matching server population cap)
```

**Event-driven, zero per-tick cost.** No `QuestInstance` field decrements per tick (QI1) — a quest
sitting `Accepted` with no matching facts arriving costs literally nothing between facts, which is
the strongest possible LOD story: **a system with no timer has no LOD problem to solve.** This
contrasts with e.g. ability cooldowns (RFC-001 §8's `ready_at_tick`), which are absolute-tick
comparisons but at least *exist* as a field; a quest objective doesn't even need that, because Q1–Q6
never compare against `now`.

**Community-event *discovery/visibility* on the Map and in the Journal's "Nearby & known" list is
RFC-021's, not restated here.** RFC-021 §5.3 owns the raid marker as "the one marker type explicitly
meant to travel past your immediate area" — visible to anyone with the target village discovered,
independent of interest set or distance. RFC-020 does not re-specify or narrow that rule; §Guide's
"world-visible... the scout told the world, not you specifically" is the same claim in player-facing
language, not a separate one.

**Interest-set/presence gates only two things: auto-accept (Q2) and contribution accrual (§7), never
the offer's visibility.** A player who has discovered the village can see the raid-defense quest
offered in their Journal from anywhere; they cannot auto-accept it (Q2's "showing up *is* accepting")
or accrue `contribution[player]` (§7) until they are physically present and performing qualifying
actions inside the event's bound region — the same interest-set/beacon precedent (ARCHITECTURE.md
§2b's `PlayerBeacon` roster, wire mechanism RFC-015's) still applies, just to *participation*, not to
*seeing the offer*. This is the property GAME.md §6 designed raids around ("hiếm với quê bạn, thường
xuyên ở đâu đó trên bản đồ") — rare for your home, visible everywhere, actionable once you travel
there.

**No player attendance requirement, ever.** Per GAME.md §6, tier-≥2 villages defend themselves via
their own guard RL/behavior regardless of whether any `QuestInstance` was ever accepted — the
`defend` quest's *entire function* is optional narration and reward on top of an outcome the
simulation computes independently. A chunk hosting the raid at 1 Hz or asleep (no player nearby)
still resolves the fight through whatever owns village-guard behavior (GAME.md §10's RL/behavior-
table fallback); the quest layer simply has zero participants and pays zero rewards that tick,
exactly like an ability whose travel body gets dropped at chunk demotion (RFC-001 §4) — nothing
behind anyone's back, because there was no one there to have something happen behind.

**Determinism boundary** (ARCHITECTURE.md §2c's distinction, applied here): the raid *roll* (which
village, which month, per GAME.md §6's probability formula) is a pure function of seed and must
match bit-for-bit across nodes; the *order* in which multiple players' `GameplayFact`s arrive at a
community ledger during the same raid is message-order-dependent and does not need to match across
independent runs — only the final ledger contents at resolution matter, and resolution is gated on
the raid's own combat outcome, not on message arrival order.

**Party credit has no spatial radius by design.** Sharing regardless of distance (§7) avoids
introducing a new distance constant purely for quest bookkeeping; the existing party membership
relation (owned by whatever system manages parties — not this RFC) is sufficient, and generosity here
costs nothing simulation-wise since it only affects *who* a reward is copied to at Q4, not any
per-tick tracking.

---

## Tone Guardrail Compliance

Argued exhaustively, because this is the section an adversarial reviewer will attack first.

**1. No timed quests — structurally, not by convention.** QV09 makes it a *build error* to author a
field representing a calendar or wall-clock duration anywhere in `quest.*`/`giver.*`. The only
`_ticks` field permitted (`giver.dialogue_display_ticks`) is bounded to 30 ticks (3 seconds) and
governs a single dialogue box's text reveal, not the quest's lifetime — it cannot be repurposed into
a deadline because no transition guard (§6) ever reads it. This is the identical structural argument
RFC-008 §11.4 makes for combat data ("no document type can schedule anything... this is the tone
guardrail made structural"), applied to a second domain.

**2. No failure-by-clock.** Enumerate every terminal state: `Complete` (objectives met — success),
`Abandoned` (player chose to stop — free, silent, immediate, no cooldown), `Expired` (emergent only —
the bound world event concluded without the player). There is no fourth terminal state and no path
into any of the three that reads `now - accepted_tick` or any tick counter at all (QI1). `Expired`
is deliberately made **externally indistinguishable from Abandoned** (QI2) precisely so a player
cannot tell, and does not need to tell, "I failed" from "I chose not to." Nothing is logged as a
loss, no streak breaks, no stat decrements.

**3. No daily/weekly hooks.** The one quest kind that *reappears* (`trade`) is re-offered by
**recomputing from village stock state** (§8), triggered on the player's next interaction with the
giver — never on a background timer, never on a calendar boundary. A village whose stock never
recovers never re-offers; a village whose stock crashes five minutes after fulfillment can re-offer
five minutes later. Contrast with a daily quest board, which resets on a schedule *regardless of
world state* — that shape is structurally absent here because there is no scheduler to attach it to.

**4. Abandoning is free and silent.** Q5 has no guard beyond "player asked to." No resource is
forfeited (nothing was ever escrowed by accepting — objectives only *observe* facts, they never lock
inventory or vitals), no cooldown is set before the same quest can be re-offered (if `repeatable`),
and the Journal entry disappears rather than moving to a "failed" list. This mirrors RFC-001 §5's
full-refund-on-broken-Cast philosophy: being interrupted (or, here, choosing to stop) already cost
the moment; a second punishment on top would be the double punishment RFC-001 explicitly rejects.

**5. Emergent opportunities are weather, not deadlines — the argument GAME.md §6 already made,
extended.** A raid-defense quest's `Expire` (Q6) fires on the raid's own combat resolution, which is
bounded by the fight itself (guards vs. raiders), not by a duration anyone authored or the player can
see counting down. Joining at any point during the fight counts identically to joining at the start
(there is no "you're too late for full credit" rule beyond the contribution-share formula in §7,
which rewards *how much you did*, never *when you arrived*). Not joining costs the player literally
nothing, for every tier `quest.raid_defense` can surface (`unlock.village_tier_min: 0`, §2), though
what "nothing" covers differs by tier: for **tier ≥2** villages GAME.md §6 guarantees the RL-trained
guards fight the raid to the same outcome whether or not any player shows; for **tier 0/1** villages
(no or minimal military) an unaided raid can still cost buildings/goods and stall that village's
tier-up for a while — but GAME.md §6's "Mất gì khi thua" is unconditional across all tiers on the one
guarantee that actually matters here: **no tier ever drops** from an unaided raid, at any tier. The
only asymmetric case GAME.md carves out — a village tier dropping — requires the player
to have **deliberately provoked a fort** first (GAME.md §6's sole exception); no `assault` quest in
this RFC frames provoking a fort as an obligation or a timed challenge, only as an available, fully
optional, informed action (see Open Question 5 on whether the Journal should carry an explicit risk
line before offering it).

**6. Village tier-up is never gated behind quests.** §7's `village_standing` reward is explicitly
one *input signal* among the several GAME.md §6 already lists (trading regularly, defending,
connecting roads, closing a fort) — none of which require ever opening the Journal. A player who
trades and defends without accepting a single quest raises their village exactly as much as one who
follows the arc; the quest layer is discovery and narration over mechanics that already work without
it, never a gate in front of them.

---

## Open Questions

1. **Escort-AI ownership gap.** `escort` objectives assume a friendly NPC that follows the player and
   survives to a destination. No accepted or proposed RFC currently owns friendly-NPC pathing/AI.
   This RFC defines `escort`'s data shape and completion fact (`kEnterRegion`), but the system that
   makes an escorted NPC actually move and stay alive does not yet exist as a spec. Should a future
   RFC own "friendly NPC behavior" broadly (traders, escort targets, village population), or is this
   narrow enough to fold into whichever RFC ends up owning village population?
2. **`kCraft`/`kBuild`/`kDeliver` fact emitters don't exist yet.** These are forward-contract
   vocabulary for ROADMAP P4's (unspecced) crafting/economy system. If P4 lands with a different
   event shape, this RFC's `fetch`/`craft`/`build`/`trade` objective kinds need a follow-up
   reconciliation pass (in the spirit of RECONCILIATION.md) rather than assuming today's guess holds.
3. **Village quest-giver assignment.** §3 assumes every tier-≥2 village exposes exactly one stable
   `giver` binding, but no RFC (this batch or accepted) specifies how worldgen or a village-population
   system assigns *which* villager is the giver, or how that assignment survives village growth
   (tier 2→3→4 — does the giver change, or gain more dialogue?). Left open pending whichever RFC ends
   up owning village population/NPC rosters.
4. **`trade` pricing formula (§8) is illustrative, not authoritative.** It exists so `trade` has a
   concrete shape to validate against; a future village-economy RFC may supersede the numbers
   entirely, and this RFC explicitly yields to it (see §8's closing note).
5. **Should `assault` quests carry an explicit risk disclaimer?** GAME.md §6's sole tier-down
   exception requires the player to have "chủ động chọc" (deliberately provoked) a fort. Framing that
   action as a *quest* — even an optional, unlock-gated one — risks making it read as sanctioned/safe
   rather than the informed-risk action GAME.md intends. Leaning toward: yes, the offer dialogue for
   `quest.fort_watch_weak`-style templates should state the retaliation risk in-fiction, but the
   exact copy is a presentation decision outside this RFC's Reference-level Design.
6. **Is the `trade` re-offer debounce (§8, "wait for next interaction") itself defensible, or should
   it be removed entirely?** It is framed as attention-triggered rather than time-triggered and so
   passes QI1, but a stricter reading could argue *any* delay mechanism invites scope creep toward a
   timer later. Leaning toward keeping it as specified (§8) since it has no tick field to misuse, but
   flagging for reviewer scrutiny given how much of this RFC's argument rests on "no field to abuse."
7. **`kMaxActiveQuests = 20`, `kFullCreditContribution = 5`, `kMaxParticipants = 50`, and the reward
   tables in §7 are all first-draft tunables** with no playtest behind them yet — expect all of them
   to move once P3 quests ship against the real 1024×1024 world and 20–50-player population.
8. **`assault` quest expiry (Q6) names "the bound fort is closed" as its guard, but no RFC currently
   owns the fort-closing mechanism itself.** GAME.md §6 establishes that closing a nearby fort is a
   tier-up trigger, and RFC-021 §3.4 explicitly leaves raid/fort mechanics unassigned to any numbered
   RFC in this batch. §6's Q6 row states the guard is *concretely trigger-able* (closure is a real,
   detectable event, not vague), but the precise definition of "closed" (Essence spend, a combat
   threshold, a worldgen flag) is pending whichever RFC ends up owning fort lifecycle.

---

## Non-goals

- **Reward item values, drop tables, Essence economy** — RFC-018 (proposed). RFC-020 carries only
  `item_table.ref`.
- **XP formulas, skill branches, the 34-point cap, cause-of-death → branch mapping** — RFC-019 (this
  batch). RFC-020 only adds a completion bonus on top.
- **Vitals, death, and recovery consequences** — RFC-013 (proposed). No quest defines a death
  penalty; dying mid-objective simply pauses fact generation.
- **Instance/realm lifecycle** (dungeon entry, group binding, timeout, per-realm atlas loading) —
  RFC-014 (proposed). `rumor` quests point at a gate; everything past it is RFC-014's.
- **Replication wire protocol / interest-set byte budgets** — RFC-015 (proposed). RFC-020 states the
  participation-gating *rule* only (§ Multiplayer); marker *visibility* is RFC-021's (§5.3).
- **Quest log persistence, save format** — RFC-016 (proposed).
- **Balance tuning harness** — RFC-017 (proposed).
- **`VillageActor` tier-up state machine, thresholds, and defense mechanics** — GAME.md §6 canon; no
  RFC in the accepted or proposed set currently owns its implementation. RFC-020 only emits a
  `village_standing` signal into it.
- **Friendly-NPC pathing/AI, crafting/building/economy verb implementations** — not yet specced by
  any RFC (see Open Questions 1–2). RFC-020 defines only the `GameplayFact` shapes those future
  systems must emit.
- **Combat HUD raid-alert presentation** — RFC-011 (proposed).
- **Map screen rendering of quest/raid/fort markers, wayfinding** — RFC-021 (this batch). RFC-020
  emits marker data only.
- **PvP quest content** — PvP is off by default (GAME.md §11).
- **Localization of dialogue/quest text** — RFC-008 Open Question 2 tracks the general decision;
  RFC-020 only ensures dialogue fields are text keys, never inline literals (QV17).
- **Branching dialogue trees, NPC relationship/affinity systems** — v1 dialogue is offer/progress/
  complete, three lines, no player choices beyond accept/decline.
- **Guide and Encyclopedia tabs of the Journal (`J`) screen** — GAME.md §12 lists them alongside
  Quests as Journal content; only the Quests tab is this RFC's scope.
- **Repeatable "daily"-style content of any kind** — deliberately excluded; see Tone Guardrail
  Compliance §3.

---

## Review Record

**Opus: revise. Sonnet: revise.** Both reviewers converged on 7 shared major defects; applied all.

Applied (both reviewers):
- Fixed the broken `share_pm` community formula (missing ÷1000) and added a worked example (§7).
- Rewrote raid-visibility: RFC-021 §5.3 owns discovery-gated marker visibility; interest set only gates auto-accept + contribution accrual (§ Multiplayer, Interactions).
- Replaced RFC-009 kill-credit citations with RFC-019 §5.8; stated `kKill` is per-contributor (header, §4, Interactions).
- Added the 34-point cap clamp-not-refuse rule (banked XP, non-committing level-up) and new QV18 (§7, §9).
- Dropped `by_cause` from the `escort` reward row (now always `trades`); corrected QV12's kind enumeration (§7, §9).
- Added tier-0/1 unaided-loss statement (no tier drop, ever) alongside the tier-≥2 self-defense guarantee (Tone Guardrail point 5).
- Closed the party-credit zero-contribution loophole: recipients now require ≥1 qualifying fact while partied (§7).

Applied (single-reviewer, verified sound):
- `cause_branch` now defined for `kDefendTick`/`kAssaultTick` (derived, not defaulted) — Opus only.
- Marked `kGather` reserved/unused in `FactKind` — Opus only.
- `assault` Q6 expiry reworded to name fort closure concretely and moved the undefined mechanism to new Open Question 8 — Opus only.
- Corrected GAME.md §3 → §11 citation for dungeon party size — Opus only.
- Reworded Q2's Effect column to distinguish per-owner (solo/party) instances from the single community actor + ledger — Opus only.
- Flagged `escort` as blocked on a future friendly-NPC AI RFC in the §1 taxonomy — Opus only.

Unresolved: none — every mustFix from both reviewers' lists was applied or subsumed by an applied fix.
