# RFC-002: Status & Effect Framework

> **Status: Accepted (revised after review)**
> Part of the Unified Combat System RFC set (see `RFC_Unified_Combat_System.md`, sections 9 and 15).
> Companion RFCs are referenced by number; where a topic belongs to another RFC this document
> deliberately does not specify it.

---

## Summary

Statuses become first-class simulation objects driven by **build-up ladders** instead of on/off
flags. Every combatant (creature, player, boss, and destructible CombatEntity per RFC-004) carries
one `StatusState` beside RFC-009's `DefenderSheet`: the sheet holds the five build-up gauges
(**Cold, Heat, Shock, Earth, Stagger** — RFC-009 §4.5's gauges, one shared enum, one storage), and
`StatusState` holds at most **one active ladder status** (the *primary*) plus a small set of binary
**coatings** (Wet). Gauges fill from spells, projectiles, auras, terrain, and physics; crossing
thresholds promotes the target up a ladder (Slow → HeavySlow → Freeze); expiry walks it back down
one rung at a time. There are **no immunities anywhere in the data model** — big or exotic targets
accumulate more slowly and escape faster (multipliers owned by RFC-009), but every ladder tops out
on every target.

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
   (Slow vs Freeze), environmental pressure (bog mire), auras, or per-material affinity — all of
   which RFC-004/005/010 need to author content against.

3. **Statuses are the shared vocabulary of the whole RFC set.** Abilities (RFC-001) emit build-up;
   physics-adjacent damage (RFC-009 §4.5) feeds the Stagger gauge; CombatEntities (RFC-004) emit
   auras; bosses (RFC-005) are authored against channels; RL agents (RFC-007) observe them; damage
   math (RFC-009) scales them; the battlefield (RFC-010) turns them into hazards. If this
   vocabulary is not fixed precisely, ten RFCs invent ten dialects — which is why this document's
   channel enum, gauge range, thresholds, and anti-chain mechanism are **RFC-009 §4.5's, adopted
   verbatim**, not a parallel scheme.

Tone guardrail (GAME.md §0) applies here too: a status framework can easily become a countdown that
chases the player. This RFC makes the opposite a hard rule — see **E-rules** below: ambient sources
can never push a player past stage 1, and damage-over-time can never be the thing that kills a
player.

---

## Guide-level Explanation

### For a player

Under an enemy's health bar you never see numbers — you see its *state*. Hit a snow-ring golem with
Ice bolts: after a few hits it is tinged blue and trudging (**Slow**); keep committing and it is
deep blue and crawling (**HeavySlow**); push through and frost crackles over the sprite —
**Freeze**, dead stopped, for about two and a half seconds. That is your window: a heavy melee blow
now **Shatters** for massive damage. Miss the window and the golem thaws in readable steps — the
Freeze resolves into a lingering Slow, then fades — nothing snaps instantly, so you can always read
where a fight is.

Freeze it, and freezing it again costs real commitment — the gauge empties and refills at half
rate for the next fifteen seconds or so, and at quarter rate if you somehow land a second Freeze
inside that window. Never slower than that. Not impossible. Never impossible. The game has no
"IMMUNE" floaters; it has "not yet."

Coatings are the second layer, and they are *conditions, not damage*. Rain makes everyone **Wet**;
Wet things take Shock faster and freeze faster, and a Thunder hit on a Wet crowd **Conducts** to
every wet enemy nearby. Coatings alone never hurt you, which is why standing in the rain on your
farm costs you nothing (GAME.md §0 survives contact with this system).

Earth is the control channel — Rock magic, deep mud, and rubble fill it: **Encumbered**, then
**Mired** (the P2 deep-mud slow), then **Rooted** — held in place but still able to fight; Mired is
the setup for **Crush**. And a hard rule protects the chill: **damage-over-time never lands the
killing blow on a player**. Burning and shock ticks stop at 1 HP. Only a real hit can finish you.
Nothing kills you behind your back.

### For a designer

You never script "apply Freeze." You script *build-up*: `"statuses": [{"channel": "cold",
"amount": 600}]` on a skill's impact (RFC-008), or an aura `{"channel": "heat", "radius": 1500,
"gain": 30, "period": 5, "team_mask": "enemies"}` on a fire totem (RFC-004/008). Whether that
freezes the target depends on the target — its size, its material, what else has been hitting it,
how recently it escaped a freeze. Content authors choose *pressure*; this framework, plus RFC-009's
curves, decides *outcome*. That is what makes a new boss "primarily data configuration" (umbrella,
Core Design): the boss's fire breath and the player's fire book and a burning ground tile all speak
the same small packet.

Materials come for free: you tag an entity `Metal` and it now shocks fast (Thunder 1500‰) and burns
reluctantly, with no per-entity logic — one row of RFC-009 §4.3's single matrix, reused for
build-up via each ladder's source channel.

---

## Reference-level Design

### 1. Vocabulary and data shapes

All types live in a new pure header `src/world/status.hpp` (same pattern as `boss.hpp`: plain data
+ pure functions, no engine includes), shared verbatim by `ChunkActor` (creatures, CombatEntities),
`PlayerActor` (players), and the RL environment. Ticks are simulation ticks at the nominal 10 Hz.

```cpp
// The five ladder channels ARE RFC-009 §4.5's five gauges — one enum, one meaning. Cold/Heat/
// Shock/Earth map to the Ice/Fire/Thunder/Rock books; Stagger has no book on purpose: it is
// derived from the physical damage channels (RFC-009 §4.5's derived-Stagger rule) plus authored
// riders. Channels are effect plumbing; elements are one *source* of them (hard constraint:
// exactly 4 elements in v1). Poison is deliberately absent — see Open Question 2.
enum class Channel : std::uint8_t {
    kNone = 0, kCold = 1, kHeat = 2, kShock = 3, kEarth = 4, kStagger = 5, kCount = 6,
};

enum class Coating : std::uint8_t { kWet = 0, kCount = 1 };  // bits in a mask; view bits reserved (OQ 5)

struct StatusState {                              // 6 bytes; one per combatant, beside RFC-009's DefenderSheet
    Channel       primary       = Channel::kNone; // the ONE active ladder status
    std::uint8_t  stage         = 0;              // 1..3 while primary != kNone, else 0
    std::uint8_t  stage_ticks   = 0;              // ticks left at this rung (counts down)
    std::uint8_t  coatings      = 0;              // bitmask of Coating
    std::uint8_t  coating_ticks[2] = {};          // Wet + one reserved slot (OQ 5)
};
```

The five gauges themselves are **not stored here**: they live in RFC-009 §4.8's
`Gauge gauges[5]` (`{uint16 value ∈ [0,1000]; uint32 last_gain_tick; uint32 resist_until;
uint8 resist_level}`, ~55 bytes) inside the `DefenderSheet`. RFC-009 owns their storage, gain
arithmetic, closed-form decay, and soft-resist state; this RFC reads `value_at(now)` and defines
the soft-resist semantics (§6), writing only at the transition points
defined in §§3–7. Wherever this document says *gauge* or *meter*, it means that field. Earlier
drafts duplicated the meters in `StatusState` with a private 0..255 range; that duplication was
the bug and is deleted.

The gain path is one packet shape, produced by every source in the game:

```cpp
enum class GainFlag : std::uint8_t {
    kNone = 0,
    kAmbient  = 1 << 0,  // weather/terrain trickle — capped at stage 1 (rule E1)
    kOpposed  = 1 << 1,  // set internally when a gain is being spent draining the opposite gauge
};

// The runtime form of RFC-009 DamagePacket's {channel, power} rider and of RFC-008's
// first-class {"channel", "amount"} status entry (§7.4 there; see Interactions).
struct BuildupPacket {
    Channel       channel;
    std::uint16_t amount;      // Power on the [0,1000] gauge scale, pre-multiplier (RFC-009 owns the multipliers)
    std::uint8_t  flags;       // GainFlag bits
    std::uint64_t source;      // player key / entity id for DoT kill credit; 0 = environment
};

struct CoatingPacket { Coating coating; std::uint8_t ticks; };
```

And the pure API both actors call (two independent implementations must converge on these
signatures and the rules in §§2–8). Everything is integer arithmetic on per-mille multipliers with
floor division, per RFC-009 §1 — **no floats anywhere in the fold**:

```cpp
// Fold one gain into the state: opposed-channel drain first (§5), then the RFC-009 §4.5 gain
// formula (material × tier × soft-resist × coating, per-mille, floor), then value = min(1000, value + gain),
// clamped per rule E1. mult_pm is the caller-supplied per-mille product from RFC-009.
void status_gain(StatusState&, Gauge (&gauges)[5], const BuildupPacket&, std::uint16_t mult_pm);

void status_coat(StatusState&, const CoatingPacket&);   // ticks = max(current, new) — rule C1

// Advance dticks of simulated time (dticks > 1 under LOD, §9). Returns DoT owed, stage
// transitions that occurred (for FX/audio events), and whether primary expired a rung.
StepResult status_step(StatusState&, Gauge (&gauges)[5], std::uint8_t dticks);

// A physical blow arrives: consult the combo table (§7), consume state, return the combo.
Combo status_detonate(StatusState&, Gauge (&gauges)[5], bool heavy, bool by_projectile, Element by_element);
```

`Creature` replaces its current three status fields (`status`, `status_ticks`, `stun_ticks`) with
one `StatusState` plus one `std::uint64_t dot_owner` (the `source` of the most recent gain on the
primary channel — whoever cooked the target gets the XP if a DoT tick kills a *creature*; players
cannot be DoT-killed at all, rule E2). Memory cost: 6 + 8 = **14 bytes** on top of RFC-009's
~62-byte `DefenderSheet`, × ~700 live combatants ≈ 10 KB for this RFC's share — negligible.

### 2. The ladders

Thresholds are positions on the shared `[0, 1000]` gauge. Crossing threshold *n* while eligible
(§4) promotes to stage *n*. **Thresholds are RFC-009 §4.5's single triple, identical for all
ladders: `T1 = 300`, `T2 = 600`, `T3 = 900` (tunable as a triple, there).** Stage names and
headline effects below are RFC-009 §4.5's proposed contract, adopted verbatim; this RFC owns their
*implementation* and the per-rung walk-down durations (non-terminal rungs), which are **(tunable)**
design values owned here. Terminal durations are RFC-009's named constants.

| Channel | Stage 1 @300 | Stage 2 @600 | Terminal @900 |
|---|---|---|---|
| **Cold** | **Slow** — speed ×0.85, 80 ticks | **HeavySlow** — speed ×0.50, 50 ticks | **Freeze** — full stop, no actions, no wind-ups; Shatter window. `kFreezeTicks = 25` (players: 12) |
| **Heat** | **Singed** — DoT 1 / 5 ticks, 60 ticks | **Burning** — DoT 3 / 5 ticks, speed ×1.15 (panic), 50 ticks | **Combust** — burst `min(15% max_hp, 60)` as Fire, then resolves immediately (terminal exit, §3) |
| **Shock** | **Static** — speed ×0.90, 60 ticks | **Shocked** — DoT 2 / 5 ticks, speed ×0.70, 30 ticks | **Paralyze** — no actions, no movement, `kParalyzeTicks = 12` (players: 6) |
| **Earth** | **Encumbered** — speed ×0.80, 60 ticks | **Mired** — speed ×0.45 (P2's Muddy, unchanged), 50 ticks | **Root** — cannot move, may still act, `kRootTicks = 20` (players: 10) |
| **Stagger** | **Unsteady** — knockback taken ×1.25 (gauge-derived; no slot, §4) | **Staggered** — current wind-up/cast cancelled, 5-tick flinch (event; no slot, §4) | **Knockdown** — incapacitated, `kKnockdownTicks = 15` (players: 8) |

Notes the tables cannot carry:

- Stage effects **replace**, not stack, within a channel (HeavySlow is ×0.5, not ×0.85 × 0.5).
- Player terminal durations are **half the creature value (tunable)** — a tone rule owned here,
  layered on RFC-009's base constants (see Open Question 3).
- Because of the one-slot rule (§4), at most one ladder's speed modifier applies at a time. The
  product of a primary-stage slow with any future speed-bearing coating is floored at 0.25 except
  Freeze/Paralyze/Knockdown, which are exactly 0 (tunable) — stacked slows must not be an
  unintended permafreeze.
- Freeze, Paralyze and Knockdown cancel an in-flight wind-up (`Creature::windup` → 0, no whiff
  strike) — freezing a monster mid-telegraph *is* the counterplay the umbrella promises (§1,
  "every phase can be counter-played").
- Earth's terminal deliberately leaves the target able to act: Root is control, not a second
  Knockdown — collapsing them would make two channels one channel with two skins. No DoT rides
  Earth; the attrition-channel question is parked (Open Question 2).

### 3. Gauge dynamics: fill, decay, walk-down

- **Fill.** `status_gain` folds `amount` through RFC-009 §4.5's gain formula
  (`floor(power × material × tier × soft-resist × coating / 1000⁴)` in per-mille steps — material
  via the ladder's source channel, RFC-009's reuse rule; soft-resist per §6). `value = min(1000,
  value + gain)`, further clamped per rule E1.
- **Decay.** Closed-form and owned by RFC-009 §4.5: after a grace of `kDecayDelay = 15` ticks the
  gauge drains at per-ladder `kDecayRate` (Cold 6/tick … Stagger 14/tick; a full Cold gauge
  self-empties in ~17 s). Decay is *computed, never ticked* — the LOD invariant RFC-010 relies on.
  This RFC owns none of those numbers.
- **Promotion** happens only at end-of-tick evaluation (§4), never mid-strike, so within one tick
  ordering of packets cannot matter.
- **Walk-down (non-terminal rungs).** When `stage_ticks` hits 0 on a stage-1 or stage-2 primary,
  the primary drops **one rung**: `stage -= 1`, `stage_ticks = duration(stage)`,
  `gauges[primary].value = T(stage) − 1` (a write, which also resets `last_gain_tick`, so it does
  not instantly re-promote). At stage 0 the primary clears. Statuses fade in steps players can
  read — nothing vanishes in one frame.
- **Terminal exit (precedence rule).** Leaving a terminal (Freeze, Combust, Paralyze, Root,
  Knockdown) by *any* exit — expiry, detonation (§7), or interaction break (§5 X1/X2) — resolves
  per this rule and **takes precedence over the generic walk-down** (the two must never both fire
  for one event): `gauges[primary].value := 0`, the soft-resist window arms (§6), and the target
  exits *through* stage 1 (`stage := 1`, `stage_ticks := duration(1)`), fading from there — the
  readable step-down is preserved as a *stage*, not as banked gauge, so escaping a terminal never
  leaves a head start toward the next one. Eviction of a terminal cannot occur (§4); if a
  future ladder shape ever allows it, eviction of a terminal is a terminal exit and takes this
  same branch.

### 4. Primary selection — the one-slot rule, made deterministic

Exactly one ladder status is active at a time. This preserves the shipped P2 property that made
combos *decisions* (you cannot stack four setups on one target and pick a finisher at leisure), it
keeps replication at ~2 bytes per bystander (§10), and it keeps the RL observation small (RFC-007).
Coatings and Stagger stages 1–2 are exempt: coatings coexist freely; **Unsteady** is a
gauge-derived passive (it holds while `gauges[kStagger].value_at(now) ≥ T1`, like a coating), and
**Staggered** is a 5-tick interrupt that fires once per upward T2 crossing and ends — neither
claims the slot. **Knockdown (Stagger stage 3) does claim the slot** — it is a real state.

End-of-tick promotion algorithm (normative — both implementations must match):

```
would_stage(ch) = number of thresholds of ch that gauges[ch].value_at(now) has crossed
best = channel with the highest would_stage;
       ties broken by most-recent-gain tick, then by lowest channel index

if best == kStagger and would_stage(best) <= 2:
    # exemption (prose above): Unsteady is gauge-derived, Staggered fires as an
    # interrupt event on the upward T2 crossing; neither claims the slot
    fire_stagger_side_effects(); do not promote
else if primary == kNone and would_stage(best) >= 1:
    promote(best, would_stage(best))
else if best == primary and would_stage(best) > stage:
    stage = would_stage(best); stage_ticks = duration(stage)          # escalate in place
else if best != primary and would_stage(best) > stage:
    # eviction: the stronger claim wins the slot
    gauges[primary] stays as-is and resumes normal decay
    promote(best, would_stage(best))
# equal stages never swap: the first claimant keeps the slot
```

`promote(ch, s)` sets `primary = ch`, `stage = s`, `stage_ticks = duration(ch, s)` and leaves the
gauge where it is (still climbing toward the next rung under sustained pressure). A channel whose
gauge sits above T1 while another primary holds the slot simply *waits*, decaying normally — banked
pressure, visible on the gauge, usable a moment later. This is the entire replacement for P2's
"second spell overwrites the first": overwriting now requires *out-accumulating*.

**A terminal can never be evicted:** eviction requires `would_stage(best) > stage`, no
`would_stage` exceeds 3, and every ladder is 3-rung — so stage 3 is unbeatable while it holds. The
chain-CC seam earlier drafts had here (an asymmetric 2-rung ladder whose banked gauge could
re-claim after eviction with no cooldown) is closed by construction; §3's terminal-exit branch
covers the hypothetical anyway.

### 5. Cross-channel and coating interactions

Interactions are rules, not per-skill code (umbrella §7). The normative set for v1:

| # | Rule | Effect |
|---|---|---|
| X1 | **Heat gain vs Cold** (and symmetric Cold vs Heat) | While `gauges[cold] > 0` or Cold is primary, incoming Heat build-up is spent draining Cold at **2 : 1** (2 Cold removed per 1 Heat offered, flagged `kOpposed`); no Heat accumulates until Cold is empty and Cold is not primary. The drain is deliberately absolute — Heat can never out-accumulate an active Cold gauge; thaw-first is the intended shape of the opposed pair, not an oversight. A single packet offering ≥ **80 (tunable)** Heat to a **Freeze** target breaks the terminal early (terminal exit, §3 — the one sanctioned break of RFC-009's terminal lock; thaw is counterplay for allies-of-the-frozen too). |
| X2 | **Wet × Heat** | Heat gains ×0.5 (500‰) while Wet. Promoting to Burning (Heat stage ≥ 2) removes Wet (steam FX event in `StepResult`). Applying Wet while Heat primary at stage ≥ 2 drops Heat to Singed and sets `gauges[heat].value = T1 − 1` (douse). |
| X3 | **Wet × Shock** | Shock gains ×1.5 (1500‰) while Wet. (The Conduct combo additionally chains — §7.) |
| X4 | **Wet × Cold** | Cold gains ×1.25 (1250‰) while Wet — wet things freeze faster; this is the §9-seasons hook: rain, then winter, is a Cold build (GAME.md §9). |
| X5 | **Mired physics** | While Earth stage ≥ 2: knockback received ×0.5 and force-transfer damage +25% — stated here for completeness, formula owned by RFC-003 (umbrella §6 "Mud"). Sets up Crush (§7). |
| X6 | **Coating refresh (C1)** | Re-applying a coating sets `coating_ticks = max(current, new)`. Never additive. Default: Wet 80 ticks (tunable). Standing in rain re-applies every pulse, i.e. effectively continuous. |

The multiplier *values* in X1–X4 are gameplay-defining and therefore normative here (tunable, as
per-mille integers), even though RFC-009 folds them into its single gain formula; RFC-009 must
treat them as inputs, not re-derive them.

### 6. Immunity-free by construction: soft-resist, never immunity

There is **no immunity flag, no immunity tag, and no zero multiplier anywhere in this framework**,
and RFC-009 enforces the same by invariant: material cells floor at ×0.1 (I4), tier intake at
150‰ (RFC-003 §4's floor). What prevents chain-freezing a boss to death is the **two-level
soft-resist window**, owned here as the one anti-chain mechanism and consumed by RFC-009 §4.5's
gain formula as a normative input:

- When a terminal resolves (§3), the channel's gauge empties to 0,
  `resist_level := min(2, resist_level + 1)`, and `resist_until := now + kResistWindow = 150`
  (15 s, tunable) — both stored in RFC-009 §4.8's `Gauge`.
- While `now < resist_until`, gains on that channel are multiplied by
  `kSoftResist[resist_level] = {1000, 500, 250}‰` — half rate after one terminal, quarter rate
  after a second inside the window, **never deeper** (two levels is the ceiling; a third terminal
  re-arms the window at level 2). **Invariant I5b: the soft-resist factor never goes below 250‰**
  — defined here, referenced by RFC-009. The read rule is explicit: at gain time, if
  `now ≥ resist_until` then `resist_level := 0` *before* the multiplier is looked up — the window
  expires in place, no separate tick needed, and `resist_level` is a clamped `uint8` (nothing
  increments unboundedly).
- Why this and not a flat refractory (the shape an earlier revision adopted — ×0.6 gains for 150
  ticks over a post-trigger gauge floor of 450): with that floor, a second Freeze cost
  `(900 − 450) / 0.6 = 750` effective Power against 900 for the first — the "anti-chain" mechanism
  made chains *cheaper*. With the gauge emptied and soft-resist armed, a second Freeze costs ×2
  the Power of the first and a third ×4 — chain-CC decays geometrically while never reaching
  immunity (script-verified; see RECONCILIATION.md ruling 3).
- Soft-resist expires in real ticks and is fully symmetric between players and monsters: monsters
  cannot chain-stun the player either, which is the tone guardrail applied to CC (a player who
  wanders into a challenge realm can be beaten, but never held down indefinitely).

Scale tiers (umbrella §10: Tiny…Titan) enter as build-up gain multipliers (RFC-003 §4's intake
numbers, quoted in RFC-009 §4.6) **and** as terminal duration multipliers owned here:
`kTierTerminalDur` (Large 800‰, Giant 600‰, Titan 450‰, invariant I5c ≥ 300‰) scales every
terminal in §2's table. Big things are slower to bottle and quicker to escape, and never immune.

### 7. Detonation — the combo table restated on ladders

`status_detonate` runs when a physical blow or an elemental hit resolves (before damage math; the
damage-scale column feeds RFC-009 §4.4's `M_outer` slot as per-mille). It preserves all five
shipped P2 combos with ladder-aware requirements. **The Requires column tests the target's
*current* `primary` + `stage`, or a coating bit — never a banked non-primary gauge** (the one-slot
rule is what makes a combo a decision):

| Combo | Requires | Result (damage scale → RFC-009 `M_outer`) | Consumes |
|---|---|---|---|
| **Shatter** | Cold **terminal (Freeze)** + heavy melee | ×2.5 (2500‰), sets `kIgnoreDr` | Terminal resolved (§3): gauge → 0, soft-resist armed (§6) |
| **Blast** | Heat **stage ≥ 2** + projectile | ×1.6, splash 2 tiles | Primary cleared, `gauges[heat] = 0` (non-terminal consume — no soft-resist; soft-resist arms on terminal resolution only, §6) |
| **Conduct** | **Wet coating** + Thunder-element hit | ×1.4; one-shot Shock gain +`T1` (300) to every Wet enemy within 3 tiles, **excluding the struck target** (it already received the hit's own build-up — no double-dip); fan-out across chunk seams is RFC-010's job | Wet consumed on the struck target **and** on each chained target |
| **Crush** | Earth **stage ≥ 2 (Mired)** + heavy melee | ×1.3; **Stagger build-up +800** on the target | Primary cleared, `gauges[earth] = 0` |
| **Arc** | Shock **stage ≥ 1** + melee | ×1.1; striker refunds 10 mana | Shock drops **one rung** (`gauges[shock] = T(new stage) − 1`; not cleared — Arc is the spammable poke by design) |

Two changes from P2, both deliberate: Crush no longer applies a flat 2-second stun — it applies
Stagger *build-up*, which is exactly how umbrella §10 makes Crush fair across scale (800 Power is
an instant Knockdown on a small target after its tier gain, under half the ladder on a Giant). And
Arc consumes one rung instead of the whole status, making it the low-commitment combo.
Non-detonating elemental hits (e.g. a Fire spell on a Freeze target) do not read this table at
all — they interact through §5.

### 8. Auras, zones, and environmental sources

The P2 `Zone` (`kWet`, `kSmokeSuppress`) generalises to **aura emission**, one spec shape used by
zones on the ground, CombatEntities (totems, smoke clouds — RFC-004), and weather. The shape is
RFC-008 §7.5's `aura` block, restated as the runtime struct, with targeting via RFC-004's existing
`AuraAffects` enum:

```cpp
struct AuraApply { Channel channel; std::uint16_t gain; };  // RFC-008's {channel|coating, gain}:
                                                            // gain is absolute [0,1000] Power per
                                                            // pulse (coating auras carry ticks)
struct AuraSpec {
    float        radius = 1.5f;   // tiles (= RFC-008 radius / 1000, millitiles); ≤ 3.0 so any aura fits a 10×7 boss room
    std::uint8_t period = 5;      // ticks between pulses (RFC-008 period; ≥ 1, validator V24)
    AuraAffects  affects = AuraAffects::kEnemiesOfTeam;   // RFC-004's enum ↔ RFC-008 team_mask
    // + the AuraApply entry (RFC-008's single channel|coating aura, §7.5 there)
};
```

RFC-004's v1 `EntityDef` carries this directly as a single-entry apply list —
`aura_channel`/`aura_coating` + `aura_gain` (RFC-004 §1) — the "future build-up" its aura note
once reserved is this RFC. Earlier drafts defined a divergent `{channel, gain, team_mask}` shape
here; deleted — three RFCs, one aura vocabulary.

Pulses fire when `world_tick % period == emitter_id % period` — phase-scattered so a room of auras
does not spike one tick, deterministic per emitter, and trivially caught up under LOD (§9). The
owning chunk applies pulses to its own creatures only; cross-seam coverage is RFC-010.

**Environmental sources and the tone rules (normative):**

| Source | Emits | Where |
|---|---|---|
| Rain / storm (weather, MapDirector) | Wet coating, refresh pulse every 20 ticks | outdoors, whole map |
| Bog tiles (wetland ring) | Wet coating + Earth gain 8 / 10 ticks, `kAmbient` | standing on bog |
| Blizzard (snow ring, winter) | Cold gain 4 / 10 ticks, `kAmbient`, suppressed within 4 tiles of a lit hearth/campfire | outdoors in ring 3+ |

- **E1 (ambient cap).** A gain flagged `kAmbient` can never raise a gauge past `T2 − 1` (599) on
  creatures, and never past `T1` (300) on **players**. Weather makes you Wet and winter makes you
  Slowed; *no environment, ever, freezes/roots/knocks down a player by itself.* Combat sources are
  uncapped. This is GAME.md §0 as a load-bearing arithmetic clamp, not a guideline.
- **E2 (DoT floor).** Damage-over-time from any status stops at 1 HP on players. It can kill
  creatures (crediting `dot_owner`). Nothing counts down a player to death behind their back;
  the killing blow is always a legible hit.
- **E3.** Ring 0–1 terrain defines no ambient emitters at all. The farm is status-free by
  construction, not by tuning.

### 9. Ticking, LOD, and sleep

All timers are tick counts; `status_step(state, gauges, dticks)` is written for `dticks ≥ 1`:

- **Active chunk (10 Hz):** `dticks = 1`.
- **Background chunk (1 Hz):** `dticks = 10`. Gauge decay costs nothing (closed-form, §3); DoT
  owed is computed arithmetically (`ticks_elapsed / dot_period` accumulated with a carried
  remainder), stage walk-down may descend multiple rungs in one call, aura pulses batch as
  `dticks / period` applications. No special cases in callers.
- **Sleeping chunk:** on sleep entry the chunk **clears `StatusState` and the five gauges on every
  creature it owns**. Justification: the longest possible chain of stages is < 300 ticks (30 s),
  far below the sleep threshold, and RFC-009's decay would have drained any gauge to zero long
  before wake anyway (a full gauge self-empties in ~17 s); a sleeping chunk by definition has no
  player near enough to observe or to have caused recent combat; and wake-up therefore needs zero
  catch-up math. This is a stated invariant, not an optimisation to revisit.
- **Instance teardown / boss leash reset** likewise clears state (already the P2 boss pattern).

Determinism boundary, stated honestly (ARCHITECTURE.md §2c): the fold itself is RFC-009's pure
integer per-mille arithmetic — bit-identical across nodes and toolchains, usable verbatim as the RL
training environment. What is *not* comparable across live runs is message arrival order: status
state is **chunk state**, fed by cross-actor messages whose ordering is scheduler-dependent, so
live meters are single-writer-consistent and replicable but not seed-pure across runs. Within one
tick, packets are buffered and folded at end-of-tick in creature-index order, then the promotion
pass runs — so intra-tick arrival order cannot change outcomes, and identical message sequences
(as in RL training) produce identical gauges to the bit.

### 10. Replication and visibility

Server-authoritative throughout: the chunk owns creature/entity status; `PlayerActor` (trusted)
owns player status; both run the same pure functions. Published views carry:

- **Per creature in `ChunkView`: 2 bytes.** Byte 1: `primary` (3 bits) + `stage` (2 bits) +
  coatings (2 bits, 1 used by Wet). Byte 2: the primary gauge quantised to 5 bits
  (`value × 31 / 1000`, boss-bar granularity) + 3 spare. Bystanders render tint/overlay from stage
  alone; a targeted boss shows its primary gauge. Full 5-gauge arrays are **not** replicated for
  creatures.
- **Own player, in the player view: full `StatusState` + the five gauge values + soft-resist
  bits (~17 bytes)** for the HUD — your own gauges are always visible as tinted bars/pips
  (widget design per RFC-006; see Asset & Engine Constraints for what iconography actually
  exists).

Every stage transition and combo emits a one-shot FX event through the existing published-`Effect`
channel (combat legibility argument in `tiles.hpp` applies unchanged); the *persistent* look of a
status (tint, frost shell, arcs) is derived by the renderer from the replicated stage, **not** from
one-shot effects — see Asset & Engine Constraints.

### 11. Migration from the shipped P2 model

| P2 (today, `tiles.hpp`) | This RFC |
|---|---|
| `Status::kWet` (timed status) | Wet coating (bitmask), same numbers |
| `Status::kMuddy` | Earth ladder stage 2 (**Mired** — same ×0.45 speed); Muddy stops being a coating |
| `Status::kFrozen` | Cold terminal (**Freeze**, `kFreezeTicks = 25` — unchanged from today's 25) |
| `Status::kBurning` | Heat stage 2 |
| `Status::kShocked` | Shock stage 2 |
| `Creature::stun_ticks` | Stagger terminal via a large authored Stagger rider (RFC-009 §4.5's CrushBlow migration) |
| Apply-overwrites-apply | Promotion algorithm §4 (out-accumulate to evict) |
| `combo_of(status, heavy, proj, elem)` | `status_detonate` §7, same five combos |
| Status drawn as per-element tint | Unchanged, plus stage-scaled intensity (RFC-006) |
| Spells set a status instantly | Spells grant Power (RFC-009 §4.5) sized so the P2 feel holds on a **Medium Flesh** target with empty gauges: `kIceBoltPower = 600` → T2 (HeavySlow) in one cast, Freeze in two — the two-cast-freeze contract, verified in RFC-009 §4.6 (Open Question 6, resolved) |

The last row is the compatibility contract: on baseline targets the game should *feel* like P2;
ladders only become visible against big, exotic, or recently-proc'd targets — exactly where
immunities would have appeared instead.

---

## Interactions with Other RFCs

- **RFC-001 (Abilities):** ability definitions carry build-up/coating payloads on their Impact and
  Persist phases; the two equipped ability slots + basic attack all route through
  `status_gain`/`status_detonate`. Cast interruption by Staggered/Freeze/Paralyze/Knockdown is
  specified here (§2 notes); which phases are interruptible is RFC-001's.
- **RFC-003 (Physics & Materials):** owns impulse/knockback (`Impulse / Mass`), which is *not* a
  gauge — Stagger fills only through RFC-009 §4.5's derived-Stagger weights on physical damage
  plus authored riders. Mired's knockback/force-transfer numbers (X5) live in RFC-003. Material
  *identity* of an entity is RFC-003/004; the single affinity matrix is RFC-009 §4.3.
- **RFC-004 (Combat Entities):** CombatEntities carry `StatusState` + `DefenderSheet` like
  creatures (an ice wall can Combust; a wooden totem can burn down) and are the primary aura
  emitters. §8's `AuraSpec` is the runtime form of its aura fields — RFC-004's `aura_channel`/
  `aura_coating` + `aura_gain` is that single-entry apply list — targeting stays its
  `AuraAffects` enum.
- **RFC-005 (Boss Authoring):** boss skills are authored against channel names and aura blocks
  only; no boss may reference a stage directly ("apply Freeze" is unrepresentable in the authoring
  schema by construction).
- **RFC-006 (Visual FX):** owns tint values, overlay choice, gauge/pip widgets, and telegraph
  standards; consumes the stage transitions and steam/thaw/douse events `StepResult` emits.
- **RFC-007 (RL Observation):** consumes the fixed-width encoding in RL Considerations below;
  owns final tensor layout.
- **RFC-008 (Skill Definition):** payloads are its first-class channel entries —
  `statuses: [{"channel", "amount"}]` for build-up and `{"coating", "ticks"}` for coatings on
  impact/persist, and entity `aura {channel|coating, radius, gain, period, team_mask}` blocks
  (§§7.4–7.5 there) — and this RFC conforms to those verbatim names. `amount`/`gain` are
  **absolute build-up Power on the shared [0,1000] gauge scale**: there is no per-document
  `buildup_max` and no load-time rescale (earlier drafts of both RFCs carried one; deleted). The
  `status.*` document set is closed at the five channels plus the Wet coating (V41 there); those
  documents carry per-channel tunables (decay, stage durations, tint), not identities.
- **RFC-009 (Damage & Build-up curves):** owns the gauges themselves (§4.8 storage), the gain
  formula, closed-form decay, thresholds (300/600/900), the tier-gain quoting of RFC-003 §4, and
  the single material matrix with its build-up reuse rule. This RFC owns what stages *do*, the
  one-slot rule, coatings, interactions (§5, normative inputs to the gain formula), the
  soft-resist window and I5b (§6, likewise a normative input), terminal-duration tier scaling
  (`kTierTerminalDur`, §6), combo detection (returning `M_outer` contributions and flags), and
  cleansing.
- **RFC-010 (Battlefield Simulation):** owns cross-chunk fan-out of Conduct chains and aura radii
  at seams, terrain ignition downstream of Heat terminals (fire spread is a *terrain* event, not a
  status event), and battlefield-state modifiers (e.g. Earthquake shaking does not touch this
  framework).

**Material affinity is one table, not two.** RFC-009 §4.3's `kMaterialMult` matrix mitigates
build-up Power through each ladder's fixed source channel (Cold ← Ice, Heat ← Fire, Shock ←
Thunder, Earth ← Rock, Stagger ← Crush) — the reuse rule is RFC-009's, its floor invariant I4
guarantees no zero cell. This RFC adds exactly one material innate: **Material Water carries a
permanent Wet coating** (its `coating_ticks` never decrement). An earlier draft carried a second
per-channel affinity table here; deleted — dual bookkeeping across RFCs was the bug.

---

## RL Considerations

- **Fixed width, quantised, room-local** — the `boss.hpp` contract extended. A combatant's status
  contributes: 5 gauges (`value / 1000` → floats), primary as a **5-wide one-hot** (`kNone` = all
  zeros), `stage / 3`, 1 Wet bit, 5 soft-resist-active bits (`resist_until > now`) —
  **17 floats** for self, and a reduced 7 (one-hot + stage + Wet) for the opponent, both from
  replicated state only. RFC-009 §6 sketches a leaner 10-value gauge encoding; both are inputs to
  RFC-007, which owns the final layout and may prune — it must not need anything this framework
  does not replicate.
- **Freeze/Knockdown ticks still produce transitions.** The environment steps and records
  experience while the agent is action-locked (actions coerced to `kHold`), so the policy pays for
  eating a freeze in observed reward rather than the frames being invisible to it.
  **Recommendation to RFC-007** (which owns the action space and must ratify): introduce no action
  masking for CC — coercion keeps the action space fixed at RLDrive's assumptions.
- **One policy per archetype survives** because channels and rules are species-independent: a
  policy trained on the Samurai reads the same 17 floats on GiantBamboo. Statuses add zero
  per-species logic — which is precisely what keeps the 10–15 policy budget intact.
- **Dojo training realism:** monster self-play dojos include scripted status pressure (a rotating
  "sparring caster" applying build-up per RFC-005) so the first RL boss does not meet Cold for the
  first time against a live player. Training scheduling itself is RFC-007/ROADMAP P8 territory.
- **Determinism:** the fold is RFC-009's bit-identical integer pipeline; the RL environment feeds
  it a fixed message sequence and gets bit-equal trajectories. Live chunks do not (§9) — checkpoint
  evaluation against live play must therefore compare outcomes, not trajectories, matching the
  existing ARCHITECTURE.md §2c boundary.

---

## Asset & Engine Constraints Honored

- **Zero new sprites required.** Stage visuals are tint + FX overlay, the P2-proven technique
  (per-element tint already ships; umbrella §15 lists the per-status looks). Walk-only monsters
  (66/66, 4×4 walk only) show Freeze as a halted walk frame + blue tint + frost overlay at their
  position; strikes and procs are FX overlays at/around the monster — never a monster pose.
- **The `kEffectLife` trap is designed around, not just patched.** The one-shot `Effect` channel
  now has per-kind lifetimes (`effect_life_of`, up to 14 frames for Earth), and this RFC keeps
  one-shot FX one-shot: *persistent* status visuals derive from the replicated `stage`, so no
  status look ever depends on stretching a 6-tick flash. Long ambient loops (arcs, frost shell)
  are renderer-side functions of `(stage, world_time)` — the same zero-state pattern as the
  particle layer. New FX families (Magic/*, spinning projectiles) remain unpacked; nothing here
  requires them (RFC-006 may adopt them later).
- **Exactly 4 elements.** Cold/Heat/Shock/Earth map to BookIce/BookFire/BookThunder/BookRock;
  Stagger is non-elemental by construction (derived from physical channels, RFC-009 §4.5) — no
  fifth book, no use of the spare school icons (Plant/Water/Light/Darkness/Wind/Death stay
  future-work only).
- **Player kit shape untouched:** basic attack + two equipped abilities; this framework adds
  no hotbar entries — statuses are what those three verbs *do*, not new verbs.
- **UI: tints, bars, and honest icon coverage.** Statuses render as tint + overlay FX and gauges
  as drawn bars/pips (raygui primitives) — the render model RFC-008 §7.4 (`tint` + `overlay_fx`)
  and RFC-009 §7 ("tints/bars, not icons") already commit to. Where an icon label helps (HUD,
  tooltips), the four elemental channels reuse the Book* icons with their Disabled twins; **no
  stock icon exists for Stagger or Wet** (verified against the 24 px set: only
  Items & Weapon / Job & Action / Meteo / Spell) — those two fall back to tinted pips, flagged to
  RFC-006. No new atlas work either way.
- **Boss shortlist compatibility:** aura radius cap 3.0 tiles fits 10×7 interior rooms; nothing
  here requires an attack pose beyond what the ~11 posed boss sheets (Samurai first) provide;
  Dragons and idle-only bosses are not referenced.
- **Chill tone is enforced by arithmetic** (rules E1–E3): no ambient hard-CC, no DoT deaths for
  players, no status pressure in rings 0–1. Nothing in this framework counts down behind the
  player's back; every ladder a player faces is one they walked toward.
- **LOD/sleep-safe and replication-cheap:** closed-form decay, `dticks` stepping, sleep-clears
  invariant, 2 bytes per bystander creature (§§9–10) — compatible with 1024 chunks, 1 Hz
  backgrounds, and a no-VPS leader.

---

## Open Questions

1. **DoT floor vs stakes (rule E2).** Stopping DoT at 1 HP on players is the strongest reading of
   the tone guardrail; does it make Burning/Shocked toothless in challenge realms where players
   *opted in*? Alternative: floor at 1 HP only outside instances. Needs playtest.
2. **Poison.** Earlier drafts carried a Poison attrition channel (healing-reduction, soft DoT,
   spider/bog fantasy); v1's five gauges are fixed by RFC-009 §4.5 and Poison is not among them.
   The attrition design is parked as a candidate future gauge (the runtime array is fixed `[5]`
   for v1 — adding one is a coordinated RFC-009 revision, not a patch here). Bog/spider pressure
   is expressed through Earth + ordinary DoT stages meanwhile.
3. **Player break-out.** Players get halved terminal durations (§2, owned here); should mashing
   movement input additionally shave Freeze/Knockdown ticks (action-game feel), or does that
   undercut monster Crush/Freeze plays? (RL is unaffected either way — agents face the unmodified
   durations.)
4. **Enemy gauge visibility.** §10 replicates the primary gauge at 5-bit granularity for a
   targeted boss bar; should ordinary creatures show gauges too, or is stage-tint enough? UI
   clutter vs build-planning clarity — RFC-006 decides, but the replication budget here caps the
   option at the existing 2 bytes.
5. **Coating budget.** v1 uses one coating (Wet); the view byte reserves 2 bits, so a second and
   third coating (e.g. Oiled) fit and a fourth does not without widening the view. Is that the
   right ceiling?
6. **Baseline calibration.** ~~§11's compatibility contract vs RFC-009 §4.6's worked table.~~
   **Resolved by RECONCILIATION.md ruling 4:** RFC-009's anchor `kIceBoltPower = 600` on the
   canonical [0,1000] scale lands T2 in one cast and Freeze in two on a Medium Flesh target —
   §11's two-cast-freeze contract holds. The "~4 casts to stage 2" figure referred to a
   superseded draft (Power 180 on the old 0..255 scale).
7. **Stagger ladder shape.** ~~Should Stagger drop to two stages so light hits never show a
   misleading stage-1 tell?~~ **Resolved by RECONCILIATION.md ruling 2:** one threshold triple
   (300/600/900) for all five ladders; Stagger keeps three rungs — the §4 slot exemption already
   makes Unsteady cheap, and a per-ladder threshold variant would fork the single-triple rule.

---

## Non-goals

- **Numeric build-up gain curves, resistances, scale-tier multipliers, damage formulas** — RFC-009.
  This RFC fixes stage semantics, walk-down durations, and interaction *rules*; every multiplier
  here marked (tunable) is a proposed default RFC-009 may re-derive, except the X1–X4 interaction
  multipliers, which are normative inputs to it (the no-zero floor is RFC-009's own invariant I4).
- **Stat-modifier buffs** (food buffs, village blessings, gear stat bonuses). They are timed stat
  modifiers with none of the ladder machinery (no gauges, no promotion, no detonation) and live
  with player progression/crafting (GAME.md §8), not in `StatusState`. **Socket and enchant procs
  are *not* excluded**: a Fire-socketed weapon that applies Heat build-up on hit (GAME.md §8) is
  an ordinary `BuildupPacket` source routed through `status_gain` like any other — only its
  stat-modifier side stays out of scope.
- **Terrain state changes** (fire spreading to grass, water pools freezing, rubble) — RFC-010;
  this RFC ends at "a Heat terminal fired here."
- **Telegraph and FX visual standards, tint values, widget design** — RFC-006.
- **Smoke/blind and other perception effects** (`kSmokeSuppress` today): targeting suppression is
  an AI/perception concern, kept as a zone behaviour under RFC-004/010, deliberately *not* a sixth
  channel — it affects what an entity can see, not what its body is doing.
- **PvP status tuning.** PvP is off by default (GAME.md §11); soft-resist symmetry (§6) is the
  only PvP-relevant provision and it exists for monsters-vs-player fairness.

---

## Review Record

Reviewer-Opus: **revise**. Reviewer-Sonnet: **revise**. All jointly-upheld fixes applied; single-reviewer items verified against the files and applied.

- Channel set re-based to RFC-009 §4.5's gauges (Cold/Heat/Shock/Earth/Stagger); Poison deferred (OQ 2); Muddy folded into Earth (Mired).
- Gauge range re-keyed to integer [0,1000], thresholds 300/600/900; storage unified on RFC-009 §4.8's `Gauge[5]`; `StatusState` slimmed to slot + coatings; all byte counts recomputed (6 / 14 / ~17 / 2).
- Escalating `soft_resist` deleted (with its uint8 overflow and missing read rule). *(The flat terminal refractory this bullet originally adopted was superseded during reconciliation — §6 owns the two-level soft-resist window and I5b; see the Reconciliation entry below, RECONCILIATION.md ruling 3.)*
- `status_gain` multiplier changed from float to integer per-mille `uint16`; §9/RL determinism language reconciled with RFC-009 §1's bit-identity mandate.
- RFC-008 field-name mandate replaced by conformance to its `{channel, amount}` / aura schema (Interactions); build-up `amount` is absolute on the shared [0,1000] scale — **no `buildup_max`, no load-time rescale** (RECONCILIATION.md ruling 5); `status.rooted` now resolves (Earth terminal).
- §8 `AuraSpec` re-shaped to RFC-008's aura block with RFC-004's `AuraAffects`; `team_mask` and the whole-Status emit model removed.
- §3 terminal-exit rule given explicit precedence over generic walk-down (gauge emptied to 0, through stage 1); eviction shown impossible for terminals (all ladders 3-rung) and covered by the terminal-exit branch regardless (§4) — closes the old Stun re-chain seam.
- §4 pseudocode now encodes the Stagger slot exemption (stages 1–2 branch) it previously only promised in prose.
- §7 Requires column pinned to current primary/stage or coating (never banked gauges); Conduct fan-out explicitly excludes the struck target; Crush rider re-keyed (+800 on the 1000 scale).
- Icon-coverage claim corrected after asset verification: Book* icons cover the four elemental channels only; Stagger/Wet fall back to tinted pips per RFC-009 §7's tints/bars model.
- RL width recounted (17 floats, 5-wide one-hot, kNone = zeros); Stagger stage indexing unambiguous (3-rung table); tier-duration rule re-keyed to RFC-009's `kTierTerminalDur`.
- Non-goals split: socket/enchant build-up procs are in-scope `BuildupPacket` sources; stat buffs remain out. Action masking downgraded to a recommendation for RFC-007 to ratify. X1 thaw-first intent stated explicitly (Sonnet's X1, as a clarification).
- Unresolved: none outstanding. (RFC-004's constexpr `Status aura_status` field has since been
  replaced with this RFC's mapped `AuraSpec` shape, per RECONCILIATION.md ruling 8. An earlier
  claim here that RFC-008 "still declares `buildup_max: 100`" was stale — RFC-008 §7.4 already
  serializes absolute `amount` on the shared scale with a first-class `channel` key; see
  RECONCILIATION.md ruling 5. Baseline-calibration gap with RFC-009 §4.6 was recorded as OQ 6,
  since resolved.)
- Reconciliation: anti-chain mechanism restated as the two-level soft-resist window owned in §6 (I5b defined here; flat refractory + 450 floor deleted as chain-accelerating); terminal exit now empties the gauge and exits through stage 1; RFC-008 conformance rewritten to the real `{channel, amount}` schema with absolute [0,1000] amounts (no `buildup_max`/rescale); `kTierTerminalDur` ownership pinned here; OQ 6 and OQ 7 closed — per RECONCILIATION.md rulings 2, 3, 4, 5.
- Reconciliation: §8's aura prose (`AuraSpec`/Interactions) re-keyed from RFC-004's now-retired `Status aura_status` field name to its mapped `aura_channel`/`aura_coating`/`aura_gain` fields, since RFC-004 has adopted this RFC's mapping directly; Unresolved bullet on that mapping closed — per RECONCILIATION.md ruling 8.
- Reconciliation: Applied bullet on the §3 terminal-exit rule corrected — it still read "exit floor 450" (the rejected flat-refractory number, per §6's own description of the mechanism this RFC deleted), contradicting both §3's actual code (`gauges[primary].value := 0`) and this Review Record's own later Reconciliation bullet on the same rule; restated as "gauge emptied to 0" — per RECONCILIATION.md ruling 16.
