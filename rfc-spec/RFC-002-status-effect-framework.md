# RFC-002: Status & Effect Framework

> **Status: Draft**
> Part of the Unified Combat System RFC set (see `RFC_Unified_Combat_System.md`, sections 9 and 15).
> Companion RFCs are referenced by number; where a topic belongs to another RFC this document
> deliberately does not specify it.

---

## Summary

Statuses become first-class simulation objects driven by **build-up ladders** instead of on/off
flags. Every combatant (creature, player, boss, and destructible CombatEntity per RFC-004) carries
one `StatusState`: five build-up meters (Frost, Heat, Shock, Poison, Stun), at most **one active
ladder status** (the *primary*), and a small set of binary **coatings** (Wet, Muddy). Meters fill
from spells, projectiles, auras, terrain, and physics; crossing thresholds promotes the target up a
ladder (Chilled → Frostbound → Frozen); expiry walks it back down one rung at a time. There are
**no immunities anywhere in the data model** — big or exotic targets accumulate more slowly and
escape faster (multipliers owned by RFC-009), but every ladder tops out on every target.

The framework preserves the shipped P2 invariant that made combos a decision — one primary status
per target, physical blows detonate it — while giving the umbrella spec what it asks for:
accumulation (§9), material interplay (§8, gameplay side), scale fairness (§10), and status visuals
that need zero new art (§15). All state is plain data, steppable at reduced tick rates, cheap to
replicate, and fixed-width for RL observation (RFC-007).

---

## Motivation

Three forces converge on this design:

1. **The umbrella spec bans absolute immunities** (§9). "The boss is immune to freeze" is the
   classic way big fights delete half a player's kit. The ladder replaces immunity with *cost*: a
   Giant can be frozen, but you must earn it, and it will not happen twice in a row for free.

2. **The shipped P2 system is good but saturated.** Today a status is a single enum + tick counter
   on `Creature` (`src/world/tiles.hpp`): applying a spell instantly sets the status, a second
   spell overwrites the first. That was the right minimal shape for four spells and five combos,
   but it cannot express accumulation ("this boss takes ten hits to freeze"), partial states
   (Chilled vs Frozen), environmental pressure (bog rot), auras, or per-material affinity — all of
   which RFC-004/005/010 need to author content against.

3. **Statuses are the shared vocabulary of the whole RFC set.** Abilities (RFC-001) emit build-up;
   physics (RFC-003) feeds the Stun channel; CombatEntities (RFC-004) emit auras; bosses (RFC-005)
   are authored against channels; RL agents (RFC-007) observe them; damage math (RFC-009) scales
   them; the battlefield (RFC-010) turns them into hazards. If this vocabulary is not fixed
   precisely, ten RFCs invent ten dialects.

Tone guardrail (GAME.md §0) applies here too: a status framework can easily become a countdown that
chases the player. This RFC makes the opposite a hard rule — see **E-rules** below: ambient sources
can never push a player past stage 1, and damage-over-time can never be the thing that kills a
player.

---

## Guide-level Explanation

### For a player

Under an enemy's health bar you never see numbers — you see its *state*. Hit a snow-ring golem with
Ice bolts: after two hits it is tinged blue and trudging (**Chilled**); two more and it is deep
blue and crawling (**Frostbound**); one more and frost crackles over the sprite — **Frozen**, dead
stopped, for about two seconds. That is your window: a heavy melee blow now **Shatters** for
massive damage. Miss the window and the golem thaws one step at a time — Frozen back to
Frostbound, then Chilled, then free — nothing snaps instantly, so you can always read where a fight
is.

Freeze it, and freezing it again is briefly harder — the meter refills at half rate for a few
seconds. Not impossible. Never impossible. The game has no "IMMUNE" floaters; it has "not yet."

Coatings are the second layer, and they are *conditions, not damage*. Rain makes everyone **Wet**;
Wet things take Shock faster and freeze faster, and a Thunder hit on a Wet crowd **Conducts** to
every wet enemy nearby. Rock magic and swamp bog make things **Muddy** — slow, and set up for
**Crush**. Coatings alone never hurt you, which is why standing in the rain on your farm costs you
nothing (GAME.md §0 survives contact with this system).

Poison is the attrition channel — no element book, no burst combo. Spider bites and bog water fill
it; at the top of its ladder you are not dying, you are *weakened*: healing works worse, your blows
hit softer. And a hard rule protects the chill: **damage-over-time never lands the killing blow on
a player**. Burning, poison, shock ticks stop at 1 HP. Only a real hit can finish you. Nothing
kills you behind your back.

### For a designer

You never script "apply Frozen." You script *build-up*: `{channel: frost, amount: 60}` on a skill
(RFC-008), or an aura `{channel: heat, radius: 1.5, gain: 6, period: 5}` on a fire totem
(RFC-004). Whether that freezes the target depends on the target — its size, its material, what
else has been hitting it, how recently it escaped a freeze. Content authors choose *pressure*;
this framework, plus RFC-009's curves, decides *outcome*. That is what makes a new boss "primarily
data configuration" (umbrella, Core Design): the boss's fire breath and the player's fire book and
a burning ground tile all speak the same three-field packet.

Materials come for free: you tag an entity `Metal` and it now shocks fast, poisons almost never,
and burns reluctantly, with no per-entity logic — one multiplier row (table in Reference-level
Design; final numbers RFC-009).

---

## Reference-level Design

### 1. Vocabulary and data shapes

All types live in a new pure header `src/world/status.hpp` (same pattern as `boss.hpp`: plain data
+ pure functions, no engine includes), shared verbatim by `ChunkActor` (creatures, CombatEntities),
`PlayerActor` (players), and the RL environment. Ticks are simulation ticks at the nominal 10 Hz.

```cpp
// The five ladder channels. Frost/Heat/Shock map to the Ice/Fire/Thunder books; Rock magic feeds
// the Muddy COATING and the Stun channel (via RFC-003 crush/impulse) — Rock's identity is control,
// not a fifth DoT. Poison and Stun have no element book on purpose: channels are effect plumbing,
// elements are one *source* of them (hard constraint: exactly 4 elements in v1).
enum class Channel : std::uint8_t {
    kNone = 0, kFrost = 1, kHeat = 2, kShock = 3, kPoison = 4, kStun = 5, kCount = 6,
};

enum class Coating : std::uint8_t { kWet = 0, kMuddy = 1, kCount = 2 };  // bits in a mask

struct StatusState {                              // 14 bytes; one per combatant
    std::uint8_t  meter[5]      = {};             // build-up per channel (kFrost..kStun), 0..255
    Channel       primary       = Channel::kNone; // the ONE active ladder status
    std::uint8_t  stage         = 0;              // 1..3 while primary != kNone, else 0
    std::uint8_t  stage_ticks   = 0;              // ticks left at this stage (counts down)
    std::uint8_t  coatings      = 0;              // bitmask of Coating
    std::uint8_t  coating_ticks[2] = {};          // per-coating countdown (Wet, Muddy)
    std::uint8_t  soft_resist[5]   = {};          // post-proc lockout ticks left, per channel (×2 stored, see §6)
};
```

The gain path is one packet shape, produced by every source in the game:

```cpp
enum class GainFlag : std::uint8_t {
    kNone = 0,
    kAmbient  = 1 << 0,  // weather/terrain trickle — capped at stage 1 (rule E1)
    kOpposed  = 1 << 1,  // set internally when a gain is being spent draining the opposite meter
};

struct BuildupPacket {
    Channel       channel;
    std::uint16_t amount;      // pre-multiplier build-up points (RFC-009 owns the multipliers)
    std::uint8_t  flags;       // GainFlag bits
    std::uint64_t source;      // player key / entity id for DoT kill credit; 0 = environment
};

struct CoatingPacket { Coating coating; std::uint8_t ticks; };
```

And the pure API both actors call (two independent implementations must converge on these
signatures and the rules in §§2–8):

```cpp
// Fold one gain into the state: opposed-channel drain first (§5), then material/scale/resist
// multipliers (supplied by caller via RFC-009), then meter += result, clamped per rule E1.
void status_gain(StatusState&, const BuildupPacket&, float rfc009_multiplier);

void status_coat(StatusState&, const CoatingPacket&);   // ticks = max(current, new) — rule C1

// Advance dticks of simulated time (dticks > 1 under LOD, §9). Returns DoT owed, stage
// transitions that occurred (for FX/audio events), and whether primary expired a rung.
StepResult status_step(StatusState&, std::uint8_t dticks);

// A physical blow arrives: consult the combo table (§7), consume state, return the combo.
Combo status_detonate(StatusState&, bool heavy, bool by_projectile, Element by_element);
```

`Creature` replaces its current three status fields (`status`, `status_ticks`, `stun_ticks`) with
one `StatusState` plus one `std::uint64_t dot_owner` (the `source` of the most recent gain on the
primary channel — whoever cooked the target gets the XP if a DoT tick kills a *creature*; players
cannot be DoT-killed at all, rule E2). Memory cost: ~19 bytes × ~700 live combatants ≈ 13 KB —
negligible.

### 2. The ladders

Thresholds are per-channel positions on the 0..255 meter. Crossing threshold *n* while eligible
(§4) promotes to stage *n*. All numbers below are **(tunable)**; stage *durations* are design
values owned here, stage *gain rates* are owned by RFC-009.

**Thresholds (tunable):** `T1 = 100`, `T2 = 170`, `T3 = 230` for Frost/Heat/Shock/Poison.
Stun uses two rungs only: `T1 = 100` (Staggered), `T2 = 200` (Stunned).

| Channel | Stage 1 | Stage 2 | Stage 3 |
|---|---|---|---|
| **Frost** | **Chilled** — speed ×0.8, 80 ticks | **Frostbound** — speed ×0.5, 50 ticks | **Frozen** — speed 0, no actions, no wind-ups; Shatter window. 20 ticks (players: 10) |
| **Heat** | **Singed** — 1 dmg / 10 ticks, 60 ticks | **Burning** — 1 dmg / 5 ticks, speed ×1.15 (panic), 50 ticks | **Ablaze** — 2 dmg / 5 ticks, emits Heat aura r=1.0, gain 4 / 5 ticks; 30 ticks |
| **Shock** | **Static** — wind-ups and attack cooldowns +25% slower, 60 ticks | **Shocked** — 1 dmg / 5 ticks, speed ×0.7, 30 ticks | **Overloaded** — as Stunned for 8 ticks, then arcs: one-shot Shock gain +`T1` to enemies within 1.5 tiles, then drops to Shocked |
| **Poison** | **Tainted** — healing received ×0.5, 100 ticks | **Poisoned** — 1 dmg / 8 ticks, stamina regen ×0.5, 80 ticks | **Blighted** — 2 dmg / 8 ticks, damage dealt ×0.75, healing ×0.5, 60 ticks |
| **Stun** | **Staggered** — current wind-up/cast cancelled, 4-tick hold | — | **Stunned** (at T2) — no actions, no movement, 20 ticks (players: 10) |

Notes the tables cannot carry:

- Stage effects **replace**, not stack, within a channel (Frostbound is ×0.5, not ×0.8×0.5).
- Cross-source speed multipliers **do** stack multiplicatively with coatings (Frostbound + Muddy =
  0.5 × 0.45) but the product is floored at 0.25 except Frozen/Stunned which are exactly 0
  (tunable) — three sources of slow must not be an unintended permafreeze.
- Frozen and Stunned cancel an in-flight wind-up (`Creature::windup` → 0, no whiff strike) —
  freezing a monster mid-telegraph *is* the counterplay the umbrella promises (§1, "every phase
  can be counter-played").
- Poison deliberately has **no** stage-3 hard-CC and **no** detonation combo: it is the attrition
  channel; giving every channel a burst finisher would collapse them into one channel with four
  skins. (Open Question 2 tracks whether this survives playtesting.)

### 3. Meter dynamics: fill, decay, walk-down

- **Fill.** `status_gain` adds `amount × rfc009_multiplier` (material × scale-tier × soft-resist;
  RFC-009 owns the formula and curve shapes). Meter clamps at 255.
- **Decay.** A channel meter untouched for a grace period of **10 ticks (tunable)** decays at
  **3 points / tick (tunable)** — a full meter self-empties in ~1.5 s of neglect plus grace. Decay
  pauses while the channel is the active primary (the stage timer is the clock then).
- **Promotion** happens only at end-of-tick evaluation (§4), never mid-strike, so within one tick
  ordering of packets cannot matter.
- **Walk-down.** When `stage_ticks` hits 0 the primary drops **one rung**: `stage -= 1`,
  `stage_ticks = duration(stage)`, `meter[primary] = T(stage) − 1` (so it does not instantly
  re-promote). At stage 0 the primary clears. Statuses fade in steps players can read — nothing
  vanishes in one frame.
- **Leaving a top rung** (Frozen, Ablaze, Overloaded, Blighted, Stunned) by *any* exit — expiry,
  detonation, or interaction break — empties that channel's meter to 0 and arms **soft-resist**
  (§6).

### 4. Primary selection — the one-slot rule, made deterministic

Exactly one ladder status is active at a time. This preserves the shipped P2 property that made
combos *decisions* (you cannot stack four setups on one target and pick a finisher at leisure), it
keeps replication at ~2 bytes per bystander (§10), and it keeps the RL observation small (RFC-007).
Coatings and Stun-stage-1 Stagger are exempt: coatings coexist freely; a Stagger is a 4-tick
interrupt that does not claim the slot. **Stunned (Stun stage 2) does claim the slot** — it is a
real state, and it evicts (see below).

End-of-tick promotion algorithm (normative — both implementations must match):

```
would_stage(ch) = number of thresholds of ch that meter[ch] has crossed
best = channel with the highest would_stage;
       ties broken by most-recent-gain tick, then by lowest channel index

if primary == kNone and would_stage(best) >= 1:
    promote(best, would_stage(best))
else if best == primary and would_stage(best) > stage:
    stage = would_stage(best); stage_ticks = duration(stage)          # escalate in place
else if best != primary and would_stage(best) > stage:
    # eviction: the stronger claim wins the slot
    meter[primary] stays as-is and resumes normal decay
    promote(best, would_stage(best))
# equal stages never swap: the first claimant keeps the slot
```

`promote(ch, s)` sets `primary = ch`, `stage = s`, `stage_ticks = duration(ch, s)` and leaves the
meter where it is (still climbing toward the next rung under sustained pressure). A channel whose
meter sits above T1 while another primary holds the slot simply *waits*, decaying normally — banked
pressure, visible on the meter, usable a moment later. This is the entire replacement for P2's
"second spell overwrites the first": overwriting now requires *out-accumulating*.

### 5. Cross-channel and coating interactions

Interactions are rules, not per-skill code (umbrella §7). The normative set for v1:

| # | Rule | Effect |
|---|---|---|
| X1 | **Heat gain vs Frost** (and symmetric Frost vs Heat) | While `meter[frost] > 0` or Frost is primary, incoming Heat build-up is spent draining Frost at **2 : 1** (2 Frost removed per 1 Heat offered, flagged `kOpposed`); no Heat accumulates until Frost is empty and Frost is not primary. A single packet offering ≥ **20 (tunable)** Heat to a **Frozen** target breaks Frozen → Frostbound immediately (thaw is counterplay for allies-of-the-frozen too). |
| X2 | **Wet × Heat** | Heat gains ×0.5 while Wet. Promoting to Burning (Heat stage ≥ 2) removes Wet (steam FX event in `StepResult`). Applying Wet while Heat primary at stage ≥ 2 drops Heat to Singed and sets `meter[heat] = T1 − 1` (douse). |
| X3 | **Wet × Shock** | Shock gains ×1.5 while Wet. (The Conduct combo additionally chains — §7.) |
| X4 | **Wet × Frost** | Frost gains ×1.25 while Wet — wet things freeze faster; this is the §9-seasons hook: rain, then winter, is a Frost build (GAME.md §9). |
| X5 | **Muddy** | Speed ×0.45 (unchanged from P2). Knockback received ×0.5 and force-transfer damage +25% — stated here for completeness, formula owned by RFC-003 (umbrella §6 "Mud"). Sets up Crush (§7). |
| X6 | **Coating refresh (C1)** | Re-applying a coating sets `coating_ticks = max(current, new)`. Never additive. Defaults: Wet 80 ticks, Muddy 60 ticks (tunable). Standing in rain / bog re-applies every pulse, i.e. effectively continuous. |

The multiplier *values* in X2–X4 are gameplay-defining and therefore normative here (tunable), even
though RFC-009 folds them into its single gain formula; RFC-009 must treat them as inputs, not
re-derive them.

### 6. Immunity-free by construction: soft-resist

There is **no immunity flag, no immunity tag, and no zero multiplier anywhere in this framework**,
and RFC-009 inherits that constraint: material and scale multipliers have a hard floor of
**0.1 (tunable)**. What prevents chain-freezing a boss to death is *soft-resist*:

- When a channel leaves its top rung (§3), set `soft_resist[ch] = 150 ticks (tunable)`.
- While `soft_resist[ch] > 0`, gains on that channel are **×0.5**; if the top rung is reached
  *again* inside the window, the refreshed window is **×0.25** (encode: store the window in
  half-tick units and double duration on repeat — the two-level scheme is deliberate and stops
  there; it never deepens to ×0.125).
- Soft-resist decays in real ticks and is fully symmetric between players and monsters: monsters
  cannot chain-stun the player either, which is the tone guardrail applied to CC (a player who
  wanders into a challenge realm can be beaten, but never held down indefinitely).

Scale tiers (umbrella §10: Tiny…Titan) live in RFC-009 as gain divisors, with one rule owned here:
**top-rung durations shrink one step up the scale ladder** — Frozen/Stunned duration ×0.75 at
Large, ×0.5 at Giant/Titan (tunable). Big things are slower to bottle and quicker to escape, and
never immune.

### 7. Detonation — the combo table restated on ladders

`status_detonate` runs when a physical blow or an elemental hit resolves (before damage math,
RFC-009). It preserves all five shipped P2 combos with ladder-aware requirements:

| Combo | Requires | Result (damage scale → RFC-009) | Consumes |
|---|---|---|---|
| **Shatter** | Frost **stage 3** + heavy melee | ×2.5, ignores armour | Primary cleared, `meter[frost] = 0`, soft-resist armed |
| **Blast** | Heat **stage ≥ 2** + projectile | ×1.6, splash 2 tiles | Same, on Heat |
| **Conduct** | **Wet coating** + Shock-element hit | ×1.4; one-shot Shock gain +`T1` to every Wet enemy within 3 tiles (fan-out across chunk seams is RFC-010's job) | Wet consumed on the struck target **and** on each chained target |
| **Crush** | **Muddy coating** + heavy melee | ×1.3; **Stun build-up +180** on the target | Muddy consumed |
| **Arc** | Shock **stage ≥ 1** + melee | ×1.1; striker refunds 10 mana | Shock drops **one rung** (not cleared — Arc is the spammable poke by design) |

Two changes from P2, both deliberate: Crush no longer applies a flat 2-second stun — it applies
stun *build-up*, which is exactly how umbrella §10 makes Crush fair across scale (180 points is an
instant Stun on a small target, a third of the ladder on a Giant). And Arc consumes one rung
instead of the whole status, making it the low-commitment combo. Non-detonating elemental hits
(e.g. Fire spell on a Frozen target) do not read this table at all — they interact through §5.

### 8. Auras, zones, and environmental sources

The P2 `Zone` (`kWet`, `kSmokeSuppress`) generalises to **aura emission**, one spec shape used by
zones on the ground, CombatEntities (totems, smoke clouds — RFC-004), Ablaze victims (§2), and
weather:

```cpp
struct AuraSpec {
    Channel      channel  = Channel::kNone;  // exactly one of channel / coating set
    Coating      coating  = Coating::kWet;
    bool         is_coating = false;
    float        radius   = 1.5f;            // tiles; ≤ 3.0 so any aura fits a 10×7 boss room
    std::uint8_t gain     = 6;               // build-up points (or coating ticks) per pulse
    std::uint8_t period   = 5;               // ticks between pulses
    std::uint8_t team_mask = kEnemies;       // whom it touches (teams per RFC-004)
};
```

Pulses fire when `world_tick % period == emitter_id % period` — phase-scattered so a room of auras
does not spike one tick, deterministic per emitter, and trivially caught up under LOD (§9). The
owning chunk applies pulses to its own creatures only; cross-seam coverage is RFC-010.

**Environmental sources and the tone rules (normative):**

| Source | Emits | Where |
|---|---|---|
| Rain / storm (weather, MapDirector) | Wet coating, refresh pulse every 20 ticks | outdoors, whole map |
| Bog tiles (wetland ring) | Muddy coating + Poison gain 2 / 10 ticks, `kAmbient` | standing on bog |
| Blizzard (snow ring, winter) | Frost gain 1 / 10 ticks, `kAmbient`, suppressed within 4 tiles of a lit hearth/campfire | outdoors in ring 3+ |

- **E1 (ambient cap).** A gain flagged `kAmbient` can never raise a meter past `T2 − 1` on
  creatures, and never past `T1` on **players**. Weather makes you Wet and winter makes you
  Chilled; *no environment, ever, freezes/stuns/blights a player by itself.* Combat sources are
  uncapped. This is GAME.md §0 as a load-bearing arithmetic clamp, not a guideline.
- **E2 (DoT floor).** Damage-over-time from any status stops at 1 HP on players. It can kill
  creatures (crediting `dot_owner`). Nothing counts down a player to death behind their back;
  the killing blow is always a legible hit.
- **E3.** Ring 0–1 terrain defines no ambient emitters at all. The farm is status-free by
  construction, not by tuning.

### 9. Ticking, LOD, and sleep

All timers are tick counts; `status_step(state, dticks)` is written for `dticks ≥ 1`:

- **Active chunk (10 Hz):** `dticks = 1`.
- **Background chunk (1 Hz):** `dticks = 10`. DoT owed is computed arithmetically
  (`ticks_elapsed / dot_period` accumulated with a carried remainder), stage walk-down may descend
  multiple rungs in one call, aura pulses batch as `dticks / period` applications. No special
  cases in callers.
- **Sleeping chunk:** on sleep entry the chunk **clears `StatusState` on every creature it owns**.
  Justification: the longest possible chain of stages is < 300 ticks (30 s), far below the sleep
  threshold; a sleeping chunk by definition has no player near enough to observe or to have caused
  recent combat; and wake-up therefore needs zero catch-up math. This is a stated invariant, not
  an optimisation to revisit.
- **Instance teardown / boss leash reset** likewise clears state (already the P2 boss pattern).

Determinism boundary, stated honestly (ARCHITECTURE.md §2c): status state is **chunk state**, fed
by cross-actor messages whose arrival order is scheduler-dependent. It is single-writer-consistent
and replicable, but *not* seed-pure across runs; no tool may assert cross-run equality on meters.
Within one tick, packets are buffered and folded at end-of-tick in creature-index order, then the
promotion pass runs — so intra-tick arrival order cannot change outcomes.

### 10. Replication and visibility

Server-authoritative throughout: the chunk owns creature/entity status; `PlayerActor` (trusted)
owns player status; both run the same pure functions. Published views carry:

- **Per creature in `ChunkView`: 2 bytes.** Byte 1: `primary` (3 bits) + `stage` (2 bits) +
  `coatings` (2 bits, 1 spare). Byte 2: `meter[primary] >> 3` (5 bits, boss-bar granularity) +
  3 spare. Bystanders render tint/overlay from stage alone; a targeted boss shows its primary
  meter. Full 5-meter arrays are **not** replicated for creatures.
- **Own player, in the player view: full `StatusState`** (14 bytes) for the HUD — your own meters
  are always visible as pips beside the status icon (the 24 px icon set with Disabled twins covers
  the iconography; layout per RFC-006).

Every stage transition and combo emits a one-shot FX event through the existing published-`Effect`
channel (combat legibility argument in `tiles.hpp` applies unchanged); the *persistent* look of a
status (tint, frost shell, arcs) is derived by the renderer from the replicated stage, **not** from
one-shot effects — see Asset & Engine Constraints.

### 11. Migration from the shipped P2 model

| P2 (today, `tiles.hpp`) | This RFC |
|---|---|
| `Status::kWet`, `kMuddy` (timed statuses) | Coatings (bitmask), same speed numbers |
| `Status::kFrozen` | Frost stage 3 (duration 25 → 20 ticks (tunable)) |
| `Status::kBurning` | Heat stage 2 |
| `Status::kShocked` | Shock stage 2 |
| `Creature::stun_ticks` | Stun channel stage 2 |
| Apply-overwrites-apply | Promotion algorithm §4 (out-accumulate to evict) |
| `combo_of(status, heavy, proj, elem)` | `status_detonate` §7, same five combos |
| Status drawn as per-element tint | Unchanged, plus stage-scaled intensity (RFC-006) |
| Spells set a status instantly | Spells grant build-up sized (RFC-009) so that on a **Medium Flesh** target with empty meters, the P2 spell count to reach the equivalent stage is preserved (baseline: one cast ≈ T2 on baseline targets — one Ice bolt still visibly chills, two still freeze) |

The last row is the compatibility contract: on baseline targets the game *feels* like P2; ladders
only become visible against big, exotic, or recently-proc'd targets — exactly where immunities
would have appeared instead.

---

## Interactions with Other RFCs

- **RFC-001 (Abilities):** ability definitions carry `BuildupPacket`/`CoatingPacket` payloads on
  their Impact and Persist phases; the two equipped ability slots + basic attack all route through
  `status_gain`/`status_detonate`. Cast interruption by Stagger/Frozen/Stunned is specified here
  (§2 notes); which phases are interruptible is RFC-001's.
- **RFC-003 (Physics & Materials):** sole feeder of the Stun channel outside the Crush combo
  (impulse/crush → stun build-up conversion is RFC-003's formula); Muddy's knockback/force-transfer
  numbers live there. Material *identity* of an entity is RFC-003/004; the per-material status
  affinity **classes** are below, their final values in RFC-009's tables.
- **RFC-004 (Combat Entities):** CombatEntities carry `StatusState` like creatures (an ice wall
  can be Ablaze; a wooden totem can Burn down) and are the primary aura emitters (`AuraSpec`).
- **RFC-005 (Boss Authoring):** boss skills are authored against channel names and `AuraSpec`s
  only; no boss may reference a stage directly ("apply Frozen" is unrepresentable in the authoring
  schema by construction).
- **RFC-006 (Visual FX):** owns tint values, overlay choice, meter/pip widgets, and telegraph
  standards; consumes the stage transitions and steam/thaw/douse events `StepResult` emits.
- **RFC-007 (RL Observation):** consumes the fixed-width encoding in RL Considerations below;
  owns final tensor layout.
- **RFC-008 (Skill Definition):** the JSON field names for build-up/coating/aura payloads must
  match §1/§8 verbatim (`channel`, `amount`, `radius`, `gain`, `period`, `team_mask`).
- **RFC-009 (Damage & Build-up curves):** owns the gain formula
  `amount × material × scale_tier × soft_resist` (soft-resist semantics fixed here, §6), the
  multiplier floor 0.1, per-source amount tables, and the §11 compatibility calibration.
- **RFC-010 (Battlefield Simulation):** owns cross-chunk fan-out of Conduct chains and aura radii
  at seams, terrain ignition downstream of Ablaze (fire spread is a *terrain* event, not a status
  event), and battlefield-state modifiers (e.g. Earthquake shaking does not touch this framework).

Material affinity classes (normative shape; values RFC-009, all **(tunable)**, none may be < 0.1):

| Material | Frost | Heat | Shock | Poison | Stun | Innate |
|---|---|---|---|---|---|---|
| Flesh | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | — |
| Stone | 0.5 | 0.5 | 0.75 | 0.25 | 0.6 | — |
| Metal | 0.75 | 0.75 | 1.5 | 0.25 | 0.8 | — |
| Wood | 1.0 | 1.5 | 0.75 | 0.5 | 1.0 | — |
| Plant | 1.25 | 1.75 | 0.75 | 0.75 | 1.0 | — |
| Water | 1.5 | 0.5 | 1.75 | 1.0 | 0.5 | permanent Wet coating |
| Spirit | 0.75 | 0.75 | 0.75 | 0.25 | 0.25 | (Spirit's physical-damage story is RFC-003/009) |
| Slime | 1.25 | 1.0 | 1.0 | 0.5 | 0.25 | — |

---

## RL Considerations

- **Fixed width, quantised, room-local** — the `boss.hpp` contract extended. A combatant's status
  contributes: 5 meters (`/255` → floats), primary as 6-wide one-hot, `stage/3`, 2 coating bits,
  5 soft-resist flags (`> 0`) — **18 floats** for self, and a reduced 9 (primary one-hot + stage +
  coatings) for the opponent, both from replicated state only. RFC-007 owns the final layout and
  may prune; it must not need anything this framework does not replicate.
- **Frozen/Stunned ticks still produce transitions.** The environment steps and records experience
  while the agent is action-locked (actions coerced to `kHold`), so the policy pays for eating a
  freeze in observed reward rather than the frames being invisible to it. No action masking is
  introduced.
- **One policy per archetype survives** because channels and rules are species-independent: a
  policy trained on the Samurai reads the same 18 floats on GiantBamboo. Statuses add zero
  per-species logic — which is precisely what keeps the 10–15 policy budget intact.
- **Dojo training realism:** monster self-play dojos include scripted status pressure (a rotating
  "sparring caster" applying build-up per RFC-005) so the first RL boss does not meet Frost for the
  first time against a live player. Training scheduling itself is RFC-007/ROADMAP P8 territory.
- **Determinism:** meters are chunk state (§9); RL checkpoint evaluation must not assert bit-equal
  trajectories across runs, matching the existing ARCHITECTURE.md §2c boundary.

---

## Asset & Engine Constraints Honored

- **Zero new sprites required.** Stage visuals are tint + FX overlay, the P2-proven technique
  (per-element tint already ships; umbrella §15 lists the per-status looks). Walk-only monsters
  (66/66, 4×4 walk only) show Frozen as a halted walk frame + blue tint + frost overlay at their
  position; strikes and procs are FX overlays at/around the monster — never a monster pose.
- **The `kEffectLife` trap is designed around, not just patched.** The one-shot `Effect` channel
  now has per-kind lifetimes (`effect_life_of`, up to 14 frames for Earth), and this RFC keeps
  one-shot FX one-shot: *persistent* status visuals derive from the replicated `stage`, so no
  status look ever depends on stretching a 6-tick flash. Long ambient loops (arcs, frost shell)
  are renderer-side functions of `(stage, world_time)` — the same zero-state pattern as the
  particle layer. New FX families (Magic/*, spinning projectiles) remain unpacked; nothing here
  requires them (RFC-006 may adopt them later).
- **Exactly 4 elements.** Frost/Heat/Shock map to BookIce/BookFire/BookThunder; Rock feeds Muddy +
  Stun. Poison and Stun are non-elemental channels, not schools — no fifth book, no use of the
  spare school icons (Plant/Water/Light/Darkness/Wind/Death stay future-work only).
- **Player kit shape untouched:** basic attack + two equipped abilities; this framework adds
  no hotbar entries — statuses are what those three verbs *do*, not new verbs.
- **UI comes from stock:** 24 px skill icons with Disabled twins serve as status icons + lockout
  states; meters are drawn pips (raygui primitives), no new atlas work.
- **Boss shortlist compatibility:** aura radius cap 3.0 tiles fits 10×7 interior rooms; nothing
  here requires an attack pose beyond what the ~11 posed boss sheets (Samurai first) provide;
  Dragons and idle-only bosses are not referenced.
- **Chill tone is enforced by arithmetic** (rules E1–E3): no ambient hard-CC, no DoT deaths for
  players, no status pressure in rings 0–1. Nothing in this framework counts down behind the
  player's back; every ladder a player faces is one they walked toward.
- **LOD/sleep-safe and replication-cheap:** `dticks` stepping, sleep-clears invariant, 2 bytes per
  bystander creature (§§9–10) — compatible with 1024 chunks, 1 Hz backgrounds, and a no-VPS leader.

---

## Open Questions

1. **DoT floor vs stakes (rule E2).** Stopping DoT at 1 HP on players is the strongest reading of
   the tone guardrail; does it make Poison/Burning toothless in challenge realms where players
   *opted in*? Alternative: floor at 1 HP only outside instances. Needs playtest.
2. **Poison has no detonation combo.** Deliberate asymmetry (§2), but if playtests show Poison
   feels like a lesser channel, a candidate is *Rupture* (Poison stage ≥ 2 + heavy melee →
   transfers remaining DoT as instant damage). Decide after RFC-009 numbers exist.
3. **Player break-out.** Players suffer halved top-rung durations; should mashing movement input
   additionally shave Frozen/Stunned ticks (action-game feel), or does that undercut monster
   Crush/Freeze plays? (RL is unaffected either way — agents face the unmodified durations.)
4. **Enemy meter visibility.** §10 replicates the primary meter at 5-bit granularity for a
   targeted boss bar; should ordinary creatures show meters too, or is stage-tint enough? UI
   clutter vs build-planning clarity — RFC-006 decides, but the replication budget here caps the
   option at the existing 2 bytes.
5. **Coating budget.** The view byte reserves 1 spare coating bit (a third coating, e.g. Oiled,
   fits; a fourth does not without widening the view). Is 3 the right ceiling for v1?
6. **Baseline calibration constant** (§11: "one cast ≈ T2 on baseline targets") — confirm against
   RFC-009's resistance curves once drafted; if RFC-009 needs one cast ≈ T1 on baseline for its
   scale math to work, the compatibility contract must be renegotiated, not silently broken.

---

## Non-goals

- **Numeric build-up gain curves, resistances, scale-tier divisors, damage formulas** — RFC-009.
  This RFC fixes semantics, thresholds, durations, and interaction *rules*; every multiplier here
  marked (tunable) is a proposed default RFC-009 may re-derive, except the X2–X4 coating
  multipliers and the 0.1 floor, which are normative inputs to it.
- **Positive buffs** (food buffs, enchant procs, village blessings). They are timed stat modifiers
  with none of the ladder machinery (no meters, no promotion, no detonation) and live with player
  progression/crafting (GAME.md §8), not in `StatusState`. Bridging them into this framework would
  bloat the replicated struct for every creature to serve a player-only feature.
- **Terrain state changes** (fire spreading to grass, water pools freezing, rubble) — RFC-010;
  this RFC ends at "an Ablaze entity emits a Heat aura."
- **Telegraph and FX visual standards, tint values, widget design** — RFC-006.
- **Smoke/blind and other perception effects** (`kSmokeSuppress` today): targeting suppression is
  an AI/perception concern, kept as a zone behaviour under RFC-004/010, deliberately *not* a sixth
  channel — it affects what an entity can see, not what its body is doing.
- **PvP status tuning.** PvP is off by default (GAME.md §11); soft-resist symmetry (§6) is the only
  PvP-relevant provision and it exists for monsters-vs-player fairness.
