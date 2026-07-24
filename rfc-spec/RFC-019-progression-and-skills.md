# RFC-019: Progression & Skill System

> Status: **Accepted (revised after review)**
> Batch: RFC-019 (this document), [RFC-020](RFC-020-mission-quest-system.md) (Mission & Quest System),
> [RFC-021](RFC-021-world-map-wayfinding.md) (World Structure, Map & Wayfinding)
> Design source: [GAME.md](../GAME.md) §7 (Nhân vật và kỹ năng), §8 (Chế tạo/tài nguyên/trang bị),
> §4 (ring adaptation), §12 (UI)
> Touches accepted combat canon: [RFC-001](RFC-001-ability-system.md) §3 (admission),
> [RFC-008](RFC-008-data-driven-skill-definition.md) §7.6 (`player.unlock`), [RFC-009](RFC-009-damage-resistance-buildup.md)
> (damage formula — cited, not re-specified)
>
> **Numbering note.** RFC-001..010 are accepted canon for the combat system and are cited by number,
> never re-specified. RFC-011..018 are proposed-only (title + rationale in `README.md`); this RFC
> cites them as "(proposed)" and does not absorb their scope. RFC-020 and RFC-021 are siblings drafted
> in the same batch as this document and are cross-referenced the same way.

---

## Summary

This RFC is the player-growth contract: the rules that turn "you shot a slime" into "your Ranged bar
moved." It specifies:

- **XP attribution** — which of the four branches (Melee / Ranged / Magic / Trades) an action's XP
  lands in, extending the shipped kill-method rule (`chunk_actor.hpp::strike`, `GrantXp`) to the
  Trades actions (mining, crafting, cooking) that have no XP hook yet.
- **What a skill point buys** — a continuous per-level passive (already shipped as `skill_scale`:
  +6%/level on combat branches) plus discrete **ability unlock tiers** that plug directly into
  RFC-001 §3's `school_level < def.unlock_level → kLocked` admission gate and RFC-008 §7.6's
  `player.unlock.level` field.
- **The XP curve and the 34-point cap** — records and operationalizes GAME.md §7's already-decided
  `kSkillPointCap = 34` / `kMaxSkillLevel = 20` (shipped: `tiles.hpp`), works out what builds the cap
  actually permits, and specifies a **respec** policy (not shipped) so specialization is a choice, not
  a life sentence.
- **Interlocking progression axes** — how the skill-point axis relates to material tiers (mine depth),
  ring-adaptation technologies, and Essence-gated capstones, without absorbing any of those systems'
  own tables.
- **Multiplayer fairness** — a damage-contribution ledger so a 2–4 player dungeon party (GAME.md §11)
  all grow from a kill, not just whoever landed the last hit (today's shipped behavior).
- **Anti-grind guardrails** — and an explicit argument for why none of the above ever creates a clock
  behind the player's back.

Out of scope, by design: loot/reward tables (RFC-018, proposed), death penalties to progression
(RFC-013, proposed), the save-file encoding of any field this RFC defines (RFC-016, proposed),
mine/dungeon instance lifecycle (RFC-014, proposed), and quest reward tables (RFC-020, this batch).

---

## Motivation

Three forces:

1. **GAME.md §7 already made the hard calls** ("no hard classes," "learn by doing," a 34-point cap)
   and P2 already shipped the skeleton (`Skill` enum, `xp_for_level`, `kSkillPointCap`, per-kill
   `GrantXp`). What is missing is the *contract* around that skeleton: precisely which actions feed
   which branch, what a level is actually worth beyond a damage percentage, and what happens at the
   cap. Without this RFC, every future system that touches progression (quests, Trades stations,
   dungeon rewards) has to reinvent those answers independently and will diverge.

2. **RFC-001 already has a hook with nothing behind it.** `reject_of`'s `school_level < def.unlock_level
   → kLocked` and RFC-008's `player.unlock.level` field exist in accepted canon, but no document says
   what `unlock_level` values a designer should write or how a player's `school_level` (this RFC's
   `level_[Skill]`) gets there. This RFC is that document.

3. **Multiplayer is coming and the current credit model does not survive it.** `strike()` grants XP to
   exactly one `player` — whoever's message dealt the killing blow. GAME.md §11 designs dungeons for
   groups of 2–4; under the shipped rule, three players who each land solid hits on a shared target
   walk away with XP for one of them. That is not a bug in single-player (P2's target), but it is a
   fairness defect the moment P6 multiplayer ships, and it is cheaper to define correctly now than to
   patch after players notice.

And one constraint binds everything: **the tone guardrail (GAME.md §0).** A skill bar is a number a
player can watch grow at their own pace. Nothing in this system may decay, expire, or demand periodic
attention to stay lit.

---

## Guide-level Explanation

### For a player

You don't pick a class. You pick up a bow, and your Ranged bar creeps up. You cast Fire, and Magic
creeps up. You harvest a field, and Trades creeps up. There is no menu where you commit to "Ranger" —
the four bars on your Character screen (`C`) are just a record of what you've actually been doing.

Each bar goes from level 0 to level 20. Early levels come fast — your first level in anything costs
40 XP, about two slime kills. Later levels are a project — level 20 alone costs more than the first
nineteen combined. That's on purpose: the curve rewards trying a new branch (cheap early wins) while
making "maxed out at everything" take long enough that it isn't the expected outcome of casual play.

You can't max everything anyway. All four bars share one pool: **34 points, total, across all of
them.** Two branches at 20 would need 40 points — more than exist. You get to be very good at one
thing, competent at a second, and merely capable at the rest. That's the whole point: an MMO where
everyone can solo everything doesn't need other players.

Leveling a branch does two things:

- A small, continuous bonus every level (currently +6% damage per level for Melee/Ranged/Magic;
  Trades gets an analogous efficiency bonus — §5.4).
- At specific levels, a **new ability becomes equippable.** You still only ever carry two — the rig
  has two ability poses (RFC-001) — so unlocking an ability means it joins your *options*, not your
  *loadout*. Today, canon picks your two automatically (the strongest fighting school's pair); this
  RFC's Character screen assumes a future free, instant, player-chosen swap replaces that auto-pick,
  with no cooldown on changing your mind about your build — but deciding *how* slots get chosen, not
  just how the picker looks, is RFC-011's (proposed) call, not settled by this RFC.

If you decide you specialized wrong, walk to your Hearth and respec: pick the branch to clear and the
branch that should receive the refund. You lose some of the XP you put in — not all — and most of the
rest lands immediately in the branch you chose, in the same visit, no separate currency to manage.
There's no timer, no NPC to travel to find, no once-a-season limit.

Fighting alongside friends doesn't shortchange anyone: if you both land hits on the same boar before
it drops, you **both** get full kill XP into whatever skill you personally used. Nobody has to race
for the last hit.

### For a designer

You author abilities exactly as RFC-008 §7.6 already specifies — a `player.unlock` block with a
`school` and a `level`. This RFC tells you what levels are meaningful to write there: four tiers per
fighting branch (starter / core / advanced / capstone — Trades has no ability tiers, §5.3), anchored
at levels 2 / 6 / 12 / 18 (tunable — §5.4).
Capstone-tier abilities additionally require the player to have spent Essence to clear the branch's
last few levels (§5.7) — you don't author that per-ability; it's a property of the level, not the
skill.

You author Trades XP grants the same way farming already does it (`GrantXp{Skill::kCraft, 6}` on
harvest, shipped) — every new Trades action (a mining swing, a crafted item, a cooked dish) calls the
same message with a number from §5.2's table.

You never author decay, caps, or scheduling. The schema this RFC's data rides on on has no field for
any of them, by the same discipline RFC-008 already enforces for combat data.

---

## Reference-level Design

### 5.1 Terminology and the shipped baseline

| Term here | Code / other-RFC name |
|---|---|
| Branch | `Skill` enum (`tiles.hpp`): `kMelee=0, kRanged=1, kMagic=2, kCraft=3`. Display name for `kCraft` is **Trades**. |
| Branch level | `PlayerActor::level_[Skill]`, `u8`, range 0..`kMaxSkillLevel` (20, shipped) |
| Branch XP (banked, uncommitted) | `PlayerActor::xp_[Skill]`, `u32` — XP earned since the branch's last level-up, always `< xp_for_level(level_[s])` except during §5.7's Essence-gate wait (levels 18→20); grants that cannot convert at all (branch already maxed, or the global cap reached while the branch is below max) are dropped, not banked — see below |
| Total invested levels | `Σ level_[s]` for s in 0..3 (helper `total_levels()`, private today) — bounded by `kSkillPointCap = 34` (shipped) |
| RFC-001's `school_level` | This RFC's branch level, read at ability admission |
| RFC-008's `player.unlock.{school, level}` | The unlock-tier table this RFC populates (§5.4) |

Shipped and **not renegotiated** by this RFC — these constants and formulas only (all from
`src/world/tiles.hpp` / `player_actor.hpp`, P2):

```cpp
enum class Skill : std::uint8_t { kMelee = 0, kRanged = 1, kMagic = 2, kCraft = 3, kCount = 4 };
inline constexpr std::uint8_t  kMaxSkillLevel = 20;
inline constexpr std::uint16_t kSkillPointCap = 34;          // total levels, all four branches

// XP to go from `level` to `level + 1`.
xp_for_level(level) = 40 * (level + 1)^2

// Per-level passive multiplier (Melee/Ranged/Magic damage today).
skill_scale(level) = 1.0 + 0.06 * level
```

**One targeted change this RFC does make to shipped behavior.** The shipped `GrantXp` handler
(`player_actor.hpp`) runs `xp_[s] += g.amount` unconditionally on every grant, then only *converts*
that XP to levels inside a loop gated by `level_[s] < kMaxSkillLevel && total_levels() <
kSkillPointCap`. Once a branch is fully maxed, or the global 34-point cap is reached while that
branch sits below max, the loop can never run again for it — but the unconditional add keeps
banking every further grant forever, with no ceiling. Left alone, that unbounded overflow is exactly
what §5.6's respec formula would otherwise be able to launder into freely-spendable points on another
branch (farm a maxed branch — normally a no-op — to inflate a refund). This RFC closes it at the
source: once a branch can no longer convert (fully maxed, or blocked by the global cap), further
`GrantXp` grants to it are **dropped, not banked** — the Essence gate (§5.7) remains the *only*
sanctioned case where banked XP is allowed to sit at or above `xp_for_level(level_[s])` while it
waits. §5.7's Essence-gate precondition is likewise a precondition added to this same shipped
level-up loop, not a change to RFC-001's admission chain (§5.4) — noted here so "not renegotiated"
above is read as scoped to the constants/formulas, not to every line of the handler that reads them.

Cumulative XP to *reach* a level (derived, not a new formula — `Σ_{i=0}^{level-1} xp_for_level(i)`,
closed form `40 · level · (level+1) · (2·level+1) / 6`):

| Reach level | Cumulative XP | Notes |
|---|---|---|
| 1 | 40 | ~2 slime kills (12 XP each, ring 0) |
| 2 | 200 | Tier I unlock threshold (§5.4) |
| 6 | 3,640 | Tier II unlock threshold (§5.4) |
| 10 | 15,400 | |
| 12 | 26,000 | Tier III unlock threshold (§5.4) |
| 18 | 84,360 | Tier IV unlock threshold, Essence-gated (§5.4, §5.7) |
| 20 (max) | 114,800 | |

This table is presented for guide-level intuition; the formula is the contract, not the printed
numbers, and the formula is shipped code this RFC does not alter.

### 5.2 XP attribution

**Combat kill-method attribution (shipped, recorded here as canon).** `chunk_actor.hpp::strike(...,
Skill skill)` already credits XP to whichever branch dealt the *killing* damage instance, not
whichever branch is "equipped" or whatever first engaged the target — shoot it, it's Ranged; freeze
it then detonate the freeze with a melee hit (`Combo::kConduct` etc.), the detonating hit's branch
wins. This is the literal implementation of GAME.md §7's "XP theo cách con vật chết" (XP follows how
the creature died). Nothing in this RFC changes it; §5.8 only extends *who* receives it.

XP granted per kill (shipped, `stats_of(kind).xp × (1 + ring)`, ring ∈ {0..4} so the multiplier ranges
×1..×5 — GAME.md §4's ring difficulty curve made visible in the reward, not just the fight):

| Creature (ring 0 values) | Kind | Base XP |
|---|---|---|
| Chicken (Tame) | Wild, timid | 3 |
| Hare | Wild, timid | 6 |
| Slime | Monster | 12 |
| Spider | Monster | 21 |
| Boar | Wild, neutral | 27 |
| Wolf | Wild, neutral | 30 |
| Ghost | Monster | 36 |
| Bear | Wild, neutral | 78 |
| Skull | Monster | 66 |
| Boss | Monster | 400 (flat, not ring-scaled — shipped special case) |

Item drops for these kills are RFC-018's (proposed) table entirely; this row only concerns the XP
column.

**Trades attribution.** Farming is shipped (`HarvestAt` grants `GrantXp{kCraft, 6}` per ripe crop
tile). Mining, crafting, and cooking have no XP hook yet — P4 (economy) hasn't shipped the actions
themselves. This RFC specifies the attribution rule those systems must call into once they exist,
at the same call shape as harvest (`GrantXp{Skill::kCraft, amount}`):

| Trades action | When it fires | Base XP **(tunable)** | Scales with |
|---|---|---|---|
| Harvest a ripe crop | shipped | 6 | — (shipped value, unchanged) |
| Mine an ore node | on node depletion, not per swing | 8 / 14 / 22 / 35 | material tier (Copper→Mythril) |
| Craft an item at a station | on successful craft | 10 | recipe's material tier (same ladder) |
| Cook a dish | on successful cook | 5 | flat — cooking doesn't have a material ladder |
| Build a ring-adaptation structure (§5.7) | on first successful placement of that structure kind, once per player | 25 | flat, one-time per structure *kind* (not per instance — building six wells doesn't grant Trades XP six times, closing an obvious farm-XP-by-spam loop) |

All four unshipped rows are new numbers this RFC proposes; none exist in code today. The **shape**
of the call (one `GrantXp` message, fired synchronously by the trusted actor that resolves the
action, no accrual) is not optional — it is what keeps Trades XP LOD-tolerant and clock-free the same
way combat XP already is (§7).

**Attribution is never split across branches for one action.** An action feeds exactly one branch —
this mirrors the shipped `strike()` signature (one `Skill` parameter per call) and keeps the mental
model "you level what you did," not "you level a weighted blend of everything that happened."

### 5.3 What a skill point buys

For the three fighting branches (Melee, Ranged, Magic), two things, always both, never one without
the other:

1. **A continuous passive**, applied every level, no threshold:

   | Branch | Passive **(tunable except Melee/Ranged/Magic's 6%, which is shipped)** |
   |---|---|
   | Melee | +6% damage/level (`skill_scale`, shipped) |
   | Ranged | +6% damage/level (shipped) |
   | Magic | +6% damage/level (shipped) |
   | Trades | +3% Trades-action yield/level (crop yield, ore yield, craft output count where recipes produce >1) **and** −2% ring-adaptation structure material cost/level (§5.7), stacking additively, capped at level 20 → +60% yield / −40% cost |

   The exact damage-scaling *formula composition* (how this multiplier combines with weapon material
   tier, resistance, and build-up math) is RFC-009's; this RFC only supplies the per-level input.

   **Integer resolution (Trades only).** Trades' yield/cost percentages apply to integer quantities
   (crop counts, ore counts, craft-output counts, structure material costs), and this project has
   already been burned once by exactly this class of bug — ARCHITECTURE.md §2c documents a float
   comparison that desynced 266 tiles of road between Windows and Linux builds, and explicitly tracks
   per-kill XP as a value that must match byte-for-byte across platforms. This RFC pins Trades' math
   as integer fixed-point, never a `float`/`double` multiply: `effective = base * (1000 + 30*level) /
   1000` for yield and `effective = base * (1000 - 20*level) / 1000` for structure cost (30 and 20 are
   3%/2% expressed in per-mille), both integer division, floored, computed identically on every node.
   At level 20 this resolves to `base * 1600 / 1000` (+60% yield) and `base * 600 / 1000` (−40% cost),
   matching the guide-level percentages above at the cap while staying integer arithmetic throughout.

2. **Discrete ability-unlock tiers** — the branch level crossing a threshold makes an ability
   *eligible to equip*, nothing more. See §5.4.

**Trades is passive-only.** Trades levels buy the yield/cost passive above and nothing else — there
are no Trades ability-unlock tiers, matching shipped canon: `abilities.hpp` states plainly "Craft has
none — you do not fight with a hoe," and `equipped_ability` only ever selects among the three fighting
schools ("Craft cannot win because it has no abilities"). §5.4's tier table applies to Melee/Ranged/
Magic only.

### 5.4 Ability unlock tiers

Four tiers per fighting branch (Melee / Ranged / Magic — Trades has no ability tiers, §5.3), anchored
at levels **2 / 6 / 12 / 18 (tunable)**. Levels 2 and 6 are not arbitrary anchors: they are the exact
values already shipped and already playtest-tuned. `src/world/abilities.hpp` gates every Tier I
ability (WhirlCleave, FanVolley, ElementalNova) at school level 2 and every Tier II ability
(CrushBlow, SmokeBomb, RainCall) at level 6 — a pair the file's own comment says was "retuned from
5/10 the day a playtest met the 123-boar wall." RFC-008 §7.6's own worked examples independently
confirm both values: `skill.spike` (pose `ability1`, the starter slot) unlocks at level 2, and
`skill.meteor` (pose `ability2`, the core slot) unlocks at level 6. Tier I and Tier II here are names
for thresholds the shipped ability table and RFC-008 already demonstrated, not new ones this RFC
invents in conflict with them.

| Tier | Branch level required | Flavor | Essence required? |
|---|---|---|---|
| I — Starter | 2 | cheap cost, short cooldown, the first thing worth equipping over the basic attack | no |
| II — Core | 6 | the branch's signature verb (e.g., Meteor for Magic) | no |
| III — Advanced | 12 | meaningfully stronger, meaningfully more expensive | no |
| IV — Capstone | 18 | build-defining; at most one or two per branch | **yes — §5.7** |

This table populates the `level` half of RFC-008 §7.6's `player.unlock` field for every ability this
RFC's branches gate; it says nothing about *how many* abilities exist per tier (that's per-ability
content, outside this RFC — a branch could ship one Tier I ability or four).

**Where this plugs into accepted canon (not modified, only consumed):**

- RFC-001 §3 `reject_of`: `if school_level < def.unlock_level → kLocked` reads this RFC's
  `level_[Skill]` directly. No new admission clause is introduced — Tier IV's Essence requirement is
  enforced entirely inside *this* RFC's own leveling state machine (§5.7), never as a new gate bolted
  onto RFC-001's admission chain. This is a deliberate scope boundary: RFC-001 is accepted canon and
  this RFC does not amend it.
- RFC-008 §7.6 `player.unlock.{school, level}`: this RFC is the source of truth for what `level`
  values a designer writes there.
- The **two-slot kit** (RFC-001) is untouched by unlocking: unlocking an ability only adds it to the
  *set* a player may become eligible to equip. **Shipped canon today auto-picks the loadout for the
  player** (`equipped_ability`, `abilities.hpp`: the two abilities of the player's currently-strongest
  fighting school, ties broken toward the lower enum) — there is no manual pick yet. This RFC's
  Character-screen list (§5.10) assumes a future free, instant, player-chosen swap will replace that
  auto-pick; RFC-001's own non-goals defer the "loadout picking UI beyond the fixed `equipped_ability`
  rule" as future work, which reads as deferring the *rendering* of a pick, not necessarily deciding
  that manual, free, cross-branch, instant picking is the mechanic that will exist. This RFC does not
  get to settle that mechanic by writing around it — manual vs. auto, free vs. costed, cross-branch or
  not, is RFC-011's (proposed, Combat HUD, Input & Cooldown UI) call. Until RFC-011 lands, the shipped
  auto-pick remains canon, and "a player with all four tiers of Melee unlocked still carries exactly
  two abilities total, possibly zero from Melee, at their own choice" describes the assumed future
  state, not today's behavior. This RFC only supplies the read-only "what's unlocked" list the
  Character screen (§5.10) and RFC-011's eventual picker both consume from the same `PlayerView`.

### 5.5 Specialization math — what the 34-point cap actually permits

With `kMaxSkillLevel = 20` and `kSkillPointCap = 34`, the cap binds before the per-branch ceiling
does for any build touching more than one branch:

| Build shape | Example | Levels used | Fits in 34? |
|---|---|---|---|
| Pure specialist | Magic 20, nothing else | 20 | yes, 14 points unspent or spread thin |
| Specialist + secondary | Magic 20, Trades 14 | 34 | yes, exactly the cap — this is the "one main, one strong second" build |
| Two mains | Magic 20, Melee 20 | 40 | **impossible** — 6 over cap |
| Balanced generalist | 9 / 9 / 8 / 8 across all four | 34 | yes — every branch reaches past Tier II (level 6) but none reaches Tier III (12) |
| Combat triad | Melee 12, Ranged 12, Magic 10 | 34 | yes — two branches (Melee, Ranged) clear Tier III (12); the third (Magic, 10) reaches only Tier II (6) and falls short of Tier III; Trades stays at 0 (no farming/mining passives, no ring-adaptation discount) |
| Trades-heavy | Trades 20, Melee 14 | 34 | yes — a builder/economy character who can still fight competently |

**The load-bearing fact for reviewers to check:** no build reaches Tier IV (level 18, capstone) in
*two* branches simultaneously — 18 + 18 = 36 > 34. At most one branch per character can ever hold a
capstone ability. Nor does any 34-point build clear Tier III (level 12) in all three combat branches
at once — that would need ≥36 points (12×3), 2 over the cap, so the Combat-triad row above is as far
as three-branch investment can reach. Both are the mechanical expression of GAME.md §7's "forcing
specialization without locking class," and both are properties of the cap arithmetic, not rules this
RFC has to separately enforce.

### 5.6 Respec

Not shipped; new to this RFC. Design goals: reversible mistakes (chill), but not a costless toggle
(a build should still mean something), and — hard constraint — **no timer, no NPC travel requirement,
no limited uses per period** (any of those would be a clock).

- **Where:** at the player's own Hearth (`BuildKind::kHearth`, shipped P0 respawn structure). No new
  building, no dependency on a village-tier service (village tiers are GAME.md §6 / roadmap P3
  territory with no RFC number chartered in this batch — deliberately not depended on here).
- **What it does:** in one atomic action, the player picks **one branch to reset** (`from`) and **one
  branch to receive the refund** (`to` — may be a different branch, or, degenerately, `from` itself).
  There is no intermediate spendable balance: the refund is granted in the same action, not banked as
  a free-floating pool a player carries around and spends later. This keeps leveling exactly what
  §5.10 already says it is — "a passive readout, not a purchasable currency" — respec redistributes
  investment between two branches, it does not mint a general-purpose XP currency.
- **Cost:** the reset branch's committed value is `xp_to_reach(level)` (§5.1's cumulative formula,
  using **only the level actually reached** — any banked, uncommitted `xp_[from]` is discarded
  outright, not refunded; this is what keeps §5.1's overflow-closure meaningful, since a laundering
  path that farmed a maxed branch for banked overflow would gain nothing from respeccing it). **75%
  (tunable)** of that committed value is granted immediately as `GrantXp{to, amount}`, through the
  ordinary level-up path (§5.1) — so it is capped by `to`'s own `kMaxSkillLevel` and the global
  `kSkillPointCap` exactly like any other XP grant, and any portion that doesn't fit is simply forfeit,
  not banked for later. **25% is unconditionally forfeit.** There is no separate currency cost and no
  dependency on RFC-018's (proposed) economy.
- **Frequency:** unlimited, no cooldown. The 25% forfeiture is the entire cost mechanism — repeated
  respeccing is self-limiting because each pass loses a quarter of what's converted, not because the
  game makes you wait.
- **What is preserved:** unlocked-ability *history* is not retroactively revoked mid-fight — if a
  respec drops a branch below an unlock tier while an ability from that tier is currently equipped,
  the equipped slot is cleared (not silently kept overpowered), consistent with RFC-001 admission
  simply re-evaluating `school_level` on the next cast attempt.

### 5.7 Parallel progression axes and their interlocks

Four axes exist in the shipped/decided design; this RFC owns only the first and specifies how the
other three interlock with it, without owning their content:

| Axis | Owner | What it gates |
|---|---|---|
| **Skill points** (branches, levels) | **this RFC** | passives, ability-unlock eligibility |
| **Material tier** (Copper→Iron→Steel→Mythril, GAME.md §8) | mine-depth access is RFC-014 (proposed, instance lifecycle); the crafting/equipment table is RFC-018 (proposed) | weapon/armor base stats |
| **Ring-adaptation tech** (wells, stilt foundations, heaters, spiked boots, Essence cleansing — GAME.md §4) | unlock/blueprint mechanism is a building/village system with no RFC number chartered yet | settling further rings |
| **Essence** (challenge-realm currency, GAME.md §1/§3) | acquisition and general spend catalog is RFC-018 (proposed) | Tier IV capstone abilities (this RFC, below) and ring-adaptation tech (out of this RFC's scope) |

Interlock rules this RFC specifies:

1. **Skill points and material tier are orthogonal, by design.** A Melee-20 character swinging a
   Copper sword and a Melee-1 character swinging Mythril are both valid, coexisting states — one is
   very skilled with a weak tool, the other has a strong tool and little skill behind it. This RFC
   does not gate branch leveling on gear tier, and does not let gear tier substitute for branch level
   in any unlock check. The two multipliers simply both feed RFC-009's damage formula as independent
   inputs.

2. **Trades level discounts ring-adaptation cost, but does not gate it.** §5.3's −2%/level structure
   cost passive applies whenever a ring-adaptation structure is built, regardless of who unlocked
   access to build it. This RFC does not define the unlock/blueprint gate itself (table above) — only
   this one discount lever, so a future building-system spec has a documented number to consume
   rather than inventing its own Trades interlock independently.

3. **Essence gates the top three levels of every branch (Tier IV territory), not just capstone
   abilities.** Concretely: reaching level 18 costs XP as normal (§5.1's `xp_for_level`), but the
   level-up **does not commit** until the player has also spent **1 unit of Essence per level from 18
   to 20 (3 total, tunable)** on that branch. Banked XP past the level-18 threshold does not overflow
   or get lost — it simply waits, exactly like a chunk that dropped to 1 Hz still has its state
   waiting when it wakes (§7). A player who never sets foot in a challenge realm caps out at level 17
   in every branch forever, by choice, and loses nothing by staying there: 17×4 branches (68) is far
   above the 34-point total cap anyway, so this gate is never the reason a chill player feels short —
   it only bites the specialist reaching for the very top of one branch.
   - This is enforced entirely as a precondition inside this RFC's own `level_[]`/`xp_[]` state
     machine (a new field, `essence_paid_[Skill]`, 0..3 per branch) — RFC-001's admission chain is
     untouched (§5.4).
   - The exact Essence *amount* (1/level, 3 total) and the exact acquisition/spend UI for Essence are
     placeholders for RFC-018 (proposed) to size against its actual reward tables; this RFC commits
     only to the *shape* of the gate (a one-time toggle per level-band, not a per-cast cost, not a
     time-based unlock).

### 5.8 Multiplayer co-op fairness — shared kills, group XP

Not shipped (today: `strike()`'s `player` parameter credits exactly one player, the one whose damage
message reduced HP to ≤0). This RFC specifies the extension needed before P6 multiplayer, so the
system it ships with is correct from the start rather than patched after players notice solo credit
in a 4-player dungeon.

**Design stance: abundance, not a split pool.** XP is not a resource contested between players in a
cooperative, PvP-off game (GAME.md §11) — dividing a kill's XP among contributors would turn "bring a
friend" into a net loss for both of you, exactly the kind of scarcity pressure the chill tone rejects.
Instead: **every contributing player receives the full kill-XP amount**, each credited to their own
branch per §5.2's kill-method rule.

**Contribution ledger (new, per-creature, chunk-owned — same trust boundary as the creature itself):**

```cpp
struct Contribution {
    std::uint64_t player;
    Skill         skill;           // the branch of this player's most recent hit on this creature
    std::uint32_t last_hit_tick;   // absolute world tick (I4-style — never a decrementing counter)
};

// Fixed-size, LRU-overwritten. 4 matches GAME.md §11's stated dungeon group size (2-4) with headroom
// for a passer-by; larger parties beyond 4 distinct simultaneous contributors are not a design target.
inline constexpr std::size_t kMaxContributors = 4;  // (tunable)
inline constexpr std::uint32_t kAssistWindowTicks = 100;  // 10 s at 10 Hz (tunable)
```

**Memory cost and migration.** `Contribution` is roughly 16 bytes padded (`u64` + `Skill` + `u32`); at
`kMaxContributors = 4` that is a bounded ~64 bytes added to every attackable creature — the ~620
seeded wildlife plus all monsters — paid once at spawn, not a growing allocation. The ledger is stored
as a field on the `Creature` itself, not chunk-local side state, and **travels with the creature**
across the chunk-migration hand-off that already exists for creatures mid-fight (shipped P2 — arrows
"migrate y hệt sinh vật," ROADMAP.md, and the same mechanism applies to the creature body itself): a
creature that crosses a chunk boundary mid-fight keeps its contributors and assist window intact on
the receiving chunk. Dropping the ledger on hand-off would silently lose an assisting player's credit
the moment a fight crosses a boundary — exactly the fairness defect this section exists to prevent.

Rules:

- Every damage instance from a player (not just the killing one) updates or inserts that player's
  entry: `skill := (the skill that dealt this hit)`, `last_hit_tick := now`. A second hit from an
  already-listed player just refreshes recency and branch, it does not add a second entry.
- At the moment a creature's HP reaches ≤0, walk the ledger: any entry with `now − last_hit_tick ≤
  kAssistWindowTicks` is a **qualifying contributor** and receives one `GrantXp{entry.skill,
  kill_xp}` — the same `kill_xp` value the shipped single-killer path already computes (§5.2's
  table), unmodified per recipient, not divided by contributor count.
- **This replaces the single-recipient grant on the boss path too, not only the general one.**
  `strike()`'s boss branch (`c.kind == CreatureKind::kBoss`) is a separate early return today that
  grants the flat 400 XP (§5.2's table) to a single `player` before the general path even runs; the
  ledger walk above replaces that single-recipient grant exactly the same way it replaces the general
  path's — a boss kill shares its flat 400 with every qualifying contributor, no separate rule needed.
  Bosses are precisely the 2–4-player content this section exists to protect, so this applies there
  without exception. Boss **item** grants (the 10 produce) stay single-recipient, per the rule below.
- The **qualifying bar is "landed at least one hit inside the window," not a damage-share
  threshold.** A minimum-damage-share cutoff would mean a lower-level player partying with a
  high-level friend sometimes gets nothing for showing up and fighting — competitive, not
  cooperative. "You did something" is the only bar, matching "you level what you use" (§5.2):
  someone who never swings never enters the ledger in the first place.
- **Item/loot distribution** (who gets what drop) is entirely RFC-018's (proposed) table; this rule
  governs the XP column only. Boss and wildlife item grants keep their shipped single-recipient
  behavior until RFC-018 specifies otherwise.
- Non-lethal contributions (a creature that flees or resets aggro before dying) never grant XP —
  matching the shipped rule that only a kill message triggers `GrantXp`. There is no partial credit
  for damage dealt to something that survives.

### 5.9 Anti-grind guardrails

The primary guardrail is what this RFC deliberately does **not** add — see Tone Guardrail Compliance
below for the itemized argument. The two guardrails it *does* add are targeted at exploit surface,
not at play pacing:

- **One-time-per-kind Trades XP for repeatable placements** (§5.2's ring-adaptation-structure row):
  without this, building six identical wells would be a free 150 XP loop; capping it to "first well,
  first heater, first stilt foundation, etc." per player closes the loop without touching how often a
  player may farm, mine, or fight, which stay fully unbounded.
- **The contribution ledger's assist window (§5.8) is a fairness bound, not a nerf**: it exists so
  that "everyone who fought gets credit" doesn't degrade into "anyone who ever tagged this creature,
  including twenty minutes ago in a different fight, gets credit" — a correctness property of a
  per-creature ledger, not a grind limiter.

Explicitly rejected as guardrails, because each is a form of the clock GAME.md §0 forbids:

- **No XP decay** — `xp_[]`/`level_[]` only ever increase (respec is player-initiated, not automatic
  decay).
- **No daily or session XP cap.**
- **No rested/well-rested bonus.** A bonus that accrues while logged out and burns down while playing
  is a login-streak mechanic wearing a different hat — it pressures a player to log in on a schedule
  to "not waste" the bonus, which is exactly the daily-reward pattern GAME.md §0 rules out by name.
- **No diminishing per-kill XP for repeat-killing the same species.** Unnecessary in practice — most
  monster kinds don't respawn on a schedule a player controls (wildlife is seeded once and does not
  respawn at all, per GAME.md §5/P2; monster forts regenerate via the RL "generation" curve, which is
  itself a difficulty-that-waits system, not a farmable clock) — and adding one would be solving a
  problem that doesn't exist at the cost of a rule that looks like a nerf.

### 5.10 Character screen (`C`) — what's visible

GAME.md §12 assigns "chỉ số, kỹ năng, trang bị" (stats, skills, equipment) to this screen; this RFC
specifies the skills portion. Already on the wire (`PlayerView`, `snapshot.hpp`, shipped):

```cpp
std::uint8_t  skill_level[kSkillCount];  // per branch, 0..20
std::uint32_t skill_xp[kSkillCount];     // banked XP toward next level
std::uint32_t skill_next[kSkillCount];   // xp_for_level(skill_level[i]) — the denominator
```

No new wire field is required for the total-cap display: `Σ skill_level[i]` is four additions the
client already has the inputs for. This RFC's only addition to `PlayerView` is the Essence-gate state
from §5.7, needed because it isn't derivable from XP alone:

```cpp
std::uint8_t essence_paid[kSkillCount];  // 0..3 — new field this RFC adds to PlayerView
```

Layout (content spec; exact pixels/widgets are implementation, not this RFC's concern):

- Four branch rows (Melee / Ranged / Magic / Trades), each: level number, a progress bar
  `skill_xp[i] / skill_next[i]`, and — once level ≥ 18 — a small Essence pip row (`essence_paid[i]/3`)
  showing the Tier IV gate's progress.
- A single total counter, prominent: `Σ skill_level[i] / 34`. When it reads 34/34, every other
  branch's "spend a point" affordance (however leveling is surfaced elsewhere — a passive readout,
  not a purchasable currency in this design) is understood to be blocked until a respec frees points;
  this RFC does not mandate a specific disabled-state visual, only the underlying fact the UI must
  reflect (respec is possible any time, so "blocked" never means "stuck").
- Per-branch, a read-only list of unlock tiers (§5.4) with a lock icon for tiers not yet reached — the
  *interactive* equip surface for what goes in the two ability slots is RFC-011's (proposed), this
  screen only shows eligibility.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001** (accepted) | Consumes this RFC's `level_[Skill]` at admission (`school_level < def.unlock_level`, §3 unmodified). This RFC does not add, remove, or reorder any admission clause; Essence gating (§5.7) is enforced entirely inside this RFC's own leveling state, never inside RFC-001's `reject_of`. The two-ability-slot kit is untouched — unlocking never changes slot count. |
| **RFC-008** (accepted) | This RFC is the source of the `level` values that populate `player.unlock.{school, level}` (§7.6) for every ability gated by branch progress. Serialization, validation, and the build pipeline for that field remain wholly RFC-008's; this RFC supplies design numbers, not schema. |
| **RFC-009** (accepted) | Owns the damage formula's order of operations; this RFC's per-level passive (`skill_scale`, +6%/level) and the material-tier multiplier (RFC-009/003's domain) are independent inputs into that formula (§5.7 rule 1). This RFC does not specify how they combine. |
| **RFC-013** (proposed, Vitals/Death/Recovery) | Owns whether/what a player loses on death. This RFC explicitly does not touch XP or skill levels on death — see Non-goals. |
| **RFC-014** (proposed, Instance & Realm Lifecycle) | Owns mine-depth instance mechanics that gate material-tier *access*; this RFC only notes the axis exists (§5.7) and does not specify mine depth, tier unlock order, or instance behavior. |
| **RFC-016** (proposed, Persistence & Save-File Format) | Owns the on-disk/DB encoding of `level_[]`, `xp_[]`, and this RFC's new `essence_paid_[]` field. This RFC specifies the fields; RFC-016 specifies how they survive a restart. |
| **RFC-018** (proposed, Loot, Essence & Reward Tables) | Owns Essence acquisition, the full Essence spend catalog, and all item/loot distribution (including for multiplayer kills, §5.8). This RFC only commits to the *shape* and *size* of the one Essence gate it needs (§5.7) and the *existence* of a per-kill XP grant it expects RFC-018's drop resolution to run alongside, not inside. |
| **RFC-020** (this batch, Mission & Quest System) | Quest rewards may include skill XP or direct level grants; this RFC supplies the curve/cap they must respect (a quest cannot hand out a grant that pushes total levels past 34 — RFC-020 must clamp or refuse, not this RFC's job to enforce from outside). Quest-gated ability unlocks, if any, still resolve through this RFC's tier table (§5.4), not a separate quest-owned gate. |
| **RFC-021** (this batch, World Structure, Map & Wayfinding) | Ring-adaptation technology's *unlock mechanism* and mount/travel systems are that RFC's or a future building-system RFC's territory; this RFC's only touchpoint is the Trades-level cost discount (§5.7 rule 2), which is a number, not a system. |

---

## Multiplayer & Simulation-LOD Considerations

- **No per-tick accrual exists anywhere in this system.** Every XP change is a single discrete
  `GrantXp` message fired synchronously by the trusted actor resolving the triggering action (a kill,
  a harvest, a craft). There is no "XP per second while doing X" mechanic to reconcile against a
  chunk's tick rate, so **this system is LOD-tolerant by construction**: a creature fought inside a
  chunk running at 1 Hz simply takes longer in wall-clock time to kill (more real seconds between the
  same 10 ticks of combat resolution) — the XP granted at death is identical to what it would be at
  10 Hz. Nothing needs a closed-form "elapsed time" reconstruction the way RFC-002's build-up decay or
  RFC-004's absolute entity expiry do, because there is no decaying or expiring quantity here.
- **The contribution ledger (§5.8) lives in chunk state**, the same trust/LOD tier as the `Creature`
  it's attached to (untrusted-OK, ticks at chunk rate). A chunk demoted to 1 Hz mid-fight still
  accumulates ledger entries correctly on whatever ticks it does run — `last_hit_tick` is stored as an
  absolute world tick (matching RFC-001 I4's discipline: never a decrementing counter), so
  `kAssistWindowTicks` comparisons are correct regardless of how sparsely the chunk has been ticking.
- **`GrantXp` fan-out for a 4-player kill is bounded** (`kMaxContributors = 4`): at most 4 messages
  leave the chunk actor per qualifying kill, each addressed to a different `PlayerActor`. This is
  well inside the existing per-chunk message budgets RFC-001/010 already operate under.
- **`PlayerActor` is the sole writer of `level_[]`/`xp_[]`/`essence_paid_[]`**, matching the existing
  trust split (`PlayerActor`: trusted, ticks with the player; chunk: untrusted-OK). A compromised
  chunk-hosting node can send a forged `GrantXp`, but it cannot itself decide what the player's
  progression becomes — `PlayerActor` remains the sole authority that applies it, exactly the
  boundary `HarvestAt`'s existing comment already states for item grants.
- **Respec (§5.6) is a player-initiated, single-tick action** at the Hearth — no state to reconcile
  across LOD levels, no interaction with chunk sleep.

---

## Tone Guardrail Compliance

Argued point by point against GAME.md §0 ("nothing counts down behind the player's back"):

1. **No decay.** `xp_[]` and `level_[]` are monotonically non-decreasing except through the player's
   own explicit respec action (§5.6). There is no code path, scheduled or triggered by elapsed time,
   that reduces either.
2. **No daily/session caps, no login streaks.** §5.9 states this as an explicit rejection, not an
   omission — these were considered as anti-grind tools and rejected specifically because they are the
   daily-reward pattern GAME.md §0 names as the thing to avoid.
3. **No rested-bonus mechanic.** Considered and rejected in §5.9 with the specific argument that a
   bonus which accrues offline and depletes online is a login-streak mechanic by another name.
4. **Essence gating (§5.7) waits, it does not count down.** A player who reaches level 18 in a branch
   without ever visiting a challenge realm keeps their banked XP indefinitely — there is no "you have
   N days to pay the Essence cost or lose the level" clause, could not be, because nothing in this
   RFC's data shape has a wall-clock or world-tick deadline field (mirroring RFC-008 §7.6's own
   structural argument: "the schema has no wall-clock field... there is no document type through which
   combat data can put a countdown behind the player's back" — the same discipline applied to
   progression data). This is difficulty/depth waiting to be found, the exact pattern GAME.md §0
   already establishes for dungeon "generations."
5. **Respec has no cooldown, no travel gate beyond the player's own Hearth (which they already
   revisit constantly to respawn), and no limited-uses-per-period.** Its only cost is the 25%
   forfeiture, paid once, at the moment of the player's own choice.
6. **The XP curve itself never punishes a slow or infrequent player.** `xp_for_level` depends only on
   the *level*, never on elapsed real time, session count, or time since last login. A player who logs
   in once a month gains exactly the XP their in-session actions earn — indistinguishable from a daily
   player who did the same actions.
7. **Multiplayer fairness (§5.8) is additive, not competitive.** Choosing abundance over a split pool
   was explicit: a design where partying could *cost* a player XP relative to soloing would create
   pressure to solo, undermining GAME.md §11's cooperative framing without a single clock involved —
   worth stating because "creates pressure" is a tone violation even when no timer is present.

No section of this RFC introduces a field, message, or rule that reads real-world or world-clock time
and reduces a player-visible number as a result.

---

## Open Questions

1. **Q1 — Trades XP numbers (§5.2).** The mining/crafting/cooking values (8/14/22/35, 10, 5) are
   this RFC's first proposal with no shipped precedent to anchor against (unlike combat XP, which
   mirrors real `stats_of` values). They should be revisited once P4's actual mining/crafting time
   costs are known — a craft that takes 30 seconds of station time probably shouldn't grant the same
   flat 10 XP as a craft that takes 3 seconds.
2. **Q2 — Respec forfeiture rate (§5.6).** 75%/25% split is a starting proposal. If playtesting shows
   players respec constantly (build experimentation as a preferred playstyle, not a fix for a
   mistake), a steeper forfeiture might be warranted; if it shows players never respec despite
   regretting a build, it may be too steep. No data exists yet either way.
3. **Q3 — Essence gate amount (§5.7).** "1 unit per level, 3 total" is sized against nothing, because
   RFC-018 (which owns Essence's acquisition rate) doesn't exist yet as a finalized spec. This number
   needs revisiting once RFC-018 lands and Essence's actual drop rate from challenge realms is known —
   if Essence is scarce, 3 units gates capstones behind a lot of dungeon-clearing; if abundant, the
   gate is nearly free and arguably pointless.
4. **Q4 — Does group XP (§5.8) need an upper bound on total simultaneous kill events per second** to
   prevent a coordinated multi-account or macro setup from generating XP faster than intended? Single-
   player P2 has no such concern; this becomes live only once P6 multiplayer ships. Deferred pending
   RFC-015 (proposed, replication) and whatever anti-automation posture the project ultimately takes —
   noted here so it isn't lost.
5. **Q5 — Tier IV ability count per branch.** §5.4 says "at most one or two per branch" as flavor
   text, not a validated rule. Should this RFC pin an exact number (mirroring how RFC-008 V44 pins
   boss kit size), or is it fine left to content authoring discretion the way Tier I–III counts are?
6. **Q6 — Cross-branch passive stacking at the cap.** §5.5's table shows several valid 34-point
   builds, but doesn't address whether any two passives from different branches interact in a way
   that could be exploited (e.g., is there ever a reason Melee and Magic passives compound instead of
   summing?). No such interaction is specified today, but a future ability whose damage type is
   ambiguous between branches (a fire-infused sword, say) could raise the question — flagged for
   whoever authors that content, not resolved here.

---

## Non-goals

- **Loot and reward tables** (item drops, Essence drop rates, socket gem sources, equipment stat
  tables) — RFC-018 (proposed). This RFC only specifies the XP column of a kill/harvest/craft, never
  the item column.
- **Death penalties, vitals economy, respawn rules** — RFC-013 (proposed). This RFC's respec (§5.6) is
  a player-initiated action with an explicit cost, not a consequence of dying, and is not to be
  confused with any death-penalty mechanic RFC-013 might specify.
- **Instance/realm lifecycle** (mine depth tiers, dungeon allocation, group binding) — RFC-014
  (proposed). This RFC references "mine depth" only as the axis material tier rides on (§5.7) and
  specifies nothing about how an instance is allocated, entered, or torn down.
- **Save/persistence format** — RFC-016 (proposed). This RFC names the fields that need to survive a
  restart (`level_[]`, `xp_[]`, the new `essence_paid_[]`) but not their on-disk shape, versioning, or
  migration.
- **Replication protocol details** (exact wire encoding, interest-set bandwidth budget for
  `PlayerView`'s new field) — RFC-015 (proposed).
- **Quest reward tables and quest-specific unlock gating** — RFC-020 (this batch). This RFC supplies
  the curve/cap constraints quest rewards must respect but authors no quest content.
- **Ring-adaptation technology's unlock/blueprint mechanism, building costs beyond the one Trades
  discount lever, and mount/wayfinding systems** — no RFC in this batch fully charters the building
  system; RFC-021 owns world structure and wayfinding but this RFC does not assume or specify which
  document eventually owns blueprint unlocking.
- **Combat HUD / equip interaction surface** — RFC-011 (proposed). This RFC specifies unlock
  *eligibility* data only, never the picker UI, keybinding, or drag/drop interaction.
- **Ability content itself** (how many abilities per branch, their individual costs/cooldowns/effects)
  — that is per-ability authoring against RFC-001's pipeline and RFC-008's schema; this RFC only
  supplies the unlock-tier levels those authored documents reference.
- **PvP balance implications of skill levels** — PvP is off by default (GAME.md §11); not considered.

---

## Review Record

**Votes:** Reviewer-Opus — revise. Reviewer-Sonnet — revise. Both converged on the same core defects
after independent verification against shipped code (`player_actor.hpp`, `abilities.hpp`,
`chunk_actor.hpp`), RFC-008, RFC-001, and ARCHITECTURE.md.

**Applied (upheld by both reviewers):**
- §5.1/§5.6: closed the unbounded `xp_[s]` overflow past a maxed/capped branch; grants that can't convert now drop instead of banking.
- §5.6: replaced the "freely-spendable XP pool" with an atomic from/to respec (no new persisted field, no contradiction with §5.10's "not a purchasable currency").
- §5.4: Tier I anchor moved from level 1 to level 2, matching shipped `abilities.hpp` and both RFC-008 worked examples.
- §5.3/§5.4: scoped "always both, never one without the other" and the tier table to Melee/Ranged/Magic; Trades stated passive-only, matching `abilities.hpp`.
- §5.5: fixed the arithmetically-false "Combat triad clears Tier III" claim; added the three-branch-Tier-III impossibility note.
- §5.3: pinned Trades' yield/cost passives to integer fixed-point math, citing the project's prior float-determinism incident.
- §5.4/Guide: reworded free player-chosen ability swapping as an assumption pending RFC-011, not settled canon.

**Applied (single-reviewer, proof verified sound):**
- §5.1/§5.7 (Opus): clarified that §5.7's Essence gate adds a precondition to the shipped level-up loop, and scoped "not renegotiated" to the constants/formulas only.
- §5.8 (Opus): the co-op ledger walk now explicitly replaces the boss's single-recipient 400 XP grant, not only the general kill path.
- §5.8 (Opus): added the ledger's per-creature memory cost and stated it migrates with the creature across chunk hand-off.

**Unresolved:** none — every mustFix from both reviewers was applied; no item was rejected as unsound.
