# RFC-009: Damage, Resistance & Effect Build-up

> Status: **Draft**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §6 (Physical Rules), §9 (Effect Accumulation), §10 (Scale)
> Companions: RFC-001 (abilities), RFC-002 (status framework), RFC-003 (physics/materials), RFC-004 (combat entities), RFC-005 (boss authoring), RFC-006 (FX), RFC-007 (RL spaces), RFC-008 (skill data), RFC-010 (battlefield simulation)

Every number proposed in this document is a **starting value marked (tunable)** unless it is
explicitly called an invariant. Invariants are the rules two independent implementations — and the
tuning process itself — must never break.

---

## 1. Summary

This RFC specifies the arithmetic core of the unified combat system:

1. **The damage formula** — a fixed-order integer pipeline from an 8-channel `DamagePacket`,
   through per-material channel resistances, an outer multiplier slot (combos, environment), then
   percentage damage reduction (DR), then flat toughness, then a chip floor.
2. **Effect build-up mathematics** — five per-creature gauges (Cold, Heat, Shock, Earth, Stagger),
   each an integer in `[0, 1000]` with **Power** (per-hit gain), **Build-up** (the accumulated
   value) and **Decay** (closed-form linear drain), with stage thresholds producing ladders such as
   Cold → Slow → HeavySlow → Freeze.
3. **Scale-tier scaling** — six tiers (Tiny…Titan) that multiply build-up gain and terminal
   duration so a Titan boss *resists* crowd control without ever being *immune* to it.
4. **DR vs flat reduction** — what each is for, where each may come from, and the caps that keep
   both from going degenerate.
5. **The tunable-constant tables** that RFC-008 skill files and RFC-005 boss definitions plug into.

Everything is integer arithmetic on per-mille (`‰`, 1000 = ×1.0) multipliers with floor division,
in a defined order — so the simulation is bit-identical across nodes and toolchains, replicable
cheaply, and usable verbatim as the RL training environment.

---

## 2. Motivation

The P2 combat that ships today (`src/world/tiles.hpp`, `src/world/chunk_actor.hpp`) is a single
scalar: `strike()` subtracts `damage × combo_scale` from `hp`, and a status is a one-slot,
duration-based flag applied instantly on any elemental hit. That was right for P2 — it proved the
combo loop — but it cannot carry the umbrella spec:

- **No channels.** The umbrella (§6) says an attack carries Pierce, Crush, Heat, Cold, Electric,
  Explosion — today there is one number, so a stone golem cannot shrug arrows while fearing
  hammers, and Spirit cannot be "immune to Physical Impact" (§8).
- **No accumulation.** One ice bolt freezes a boss exactly as fast as it freezes a slime. The only
  fix available today would be a per-boss immunity flag — which the umbrella (§9: *"No absolute
  immunities"*) forbids, and which produces the worst kind of fight: the one where half your kit
  silently does nothing.
- **No principled mitigation.** `Shatter: ×2.5, ignores armour` is written in a comment, but there
  is no armour to ignore. Boss authoring (RFC-005) and gear sockets (GAME.md §8) both need a
  mitigation model with known caps *before* content is tuned against it, because tuning combat
  twice is the most expensive waste in the roadmap (ROADMAP, "P1 or P2 first").

This RFC exists so that RFC-001/005/008 can author *data* and RFC-002/003/004 can apply *rules*
against one shared, closed arithmetic — the "adding a boss is data configuration" promise of the
umbrella's Core Design section.

---

## 3. Guide-level Explanation

### For the player

You hit a giant samurai with an ice spell. He does not freeze — a blue **frost rim** creeps over
his sprite (a tint, per RFC-006 — the art has no frozen pose and needs none). Hit him again and he
visibly **slows**. Keep committing to ice and he slows to a crawl, then locks solid for a moment —
your Shatter window. Walk away instead, and the frost fades on its own. Nothing about him was
immune; he was just *big*, and big things take persistence.

A slime, meanwhile, freezes on the second bolt. Same spell, same math, different scale tier — the
player learns one rule ("big resists, nothing is immune") instead of sixty-six bespoke immunity
lists.

Chill guardrail (GAME.md §0): every gauge in this system **decays to zero on its own**, and no
build-up, DoT or debuff outlives disengagement by more than a few seconds. Combat difficulty here
is something you walk *into*; the moment you walk away, the math lets go of you. Nothing in this
RFC ticks, counts down, or accumulates behind a player's back outside of a fight they are standing
in.

### For the designer

You author a skill (RFC-008) by filling in a `DamagePacket`: *how much of which channels, how much
build-up Power on which ladder*. You never write `if (target == GiantFrog)`. The target's material
row, scale tier, DR and toughness do the rest. Making a boss "feel armoured" is two integers
(material = Metal, tier = Giant), not a script.

Two knobs you will actually turn while tuning:

- **Power** on a skill: how fast its ladder climbs — the "status-build weapon" knob.
- **Tier gain multiplier** on an archetype: how stubborn a boss is against *all* ladders at once.

### For the engineer

One function, pure, integer, order-fixed:

```
(DamagePacket, DefenderSheet, OuterMult) -> (hp_delta, gauge_deltas[5], stage_events[])
```

No floats, no RNG, no reads outside its arguments. It is called from the chunk's strike path
(where `MeleeSwing`/`CastSpell`/`AbilityStrike` resolve today), and identically from the RL
training environment (RFC-007) and from hazard auras (RFC-004). Gauge decay is closed-form over
elapsed ticks, so a chunk ticked at 1 Hz or woken from sleep (RFC-010) catches up in O(1) — the
same design that lets crops survive sleeping chunks (ARCHITECTURE.md §4).

---

## 4. Reference-level Design

All tick counts assume the **10 Hz** simulation tick. All multipliers are per-mille `std::uint16_t`
(1000 = ×1.0). All arithmetic is `int32`, all division is floor division; intermediate values are
clamped to `≥ 0` before each subsequent step. This section is normative.

### 4.1 Damage channels

Eight channels, fixed order (the order is part of the wire/data format, RFC-008):

| # | Channel | Family | Today's source (code) | Typical carrier |
|---|---|---|---|---|
| 0 | **Slash** | physical | melee light swing | swords, claws |
| 1 | **Pierce** | physical | arrows (`LaunchArrow`) | arrows, spikes, spears |
| 2 | **Crush** | physical | heavy melee (`heavy`) | hammers, boulders, CrushBlow |
| 3 | **Blast** | physical | Blast combo splash | explosions, meteor impact |
| 4 | **Fire** | elemental | `Element::kFire` | fire school |
| 5 | **Ice** | elemental | `Element::kIce` | ice school |
| 6 | **Rock** | elemental | `Element::kEarth` | rock school |
| 7 | **Thunder** | elemental | `Element::kShock` | thunder school |

Exactly four elemental channels — Fire / Ice / Rock / Thunder — matching the four skill books the
asset pack ships icons for (`BookFire/BookIce/BookRock/BookThunder`). Plant, Water, Light,
Darkness, Wind and Death exist **as icons only** and are out of scope for v1; a future RFC may add
channels 8+ without disturbing this format (the channel array is length-prefixed in RFC-008 data,
fixed `[8]` in the runtime struct). Note the deliberate naming split: **Rock/Thunder** are the
in-game element names; the existing enum values `kEarth`/`kShock` are their code spellings.

The umbrella's §6 payload list maps as: Damage → the channel amounts; Heat/Cold/Electric →
Fire/Ice/Thunder; Explosion → Blast; Pierce/Crush → themselves; **Impulse is not a damage
channel** — it is a separate field applied by the physics rules of RFC-003 (`Impulse / Mass =
Knockback`, umbrella §11) and merely *travels* in the same packet.

### 4.2 `DamagePacket` — the payload

```cpp
// One hit, fully resolved by the attacker's side (RFC-001 has already applied school damage,
// ability damage_scale, ring scaling and any charge multiplier — mirroring today's protocol.hpp
// rule that "damage arrives ALREADY COMPUTED by the trusted actor").
struct DamagePacket {
    std::int16_t  amount[8];      // per-channel damage, >= 0
    std::uint16_t impulse;        // physics only — consumed by RFC-003, never by 4.4
    // Build-up riders. A packet may push at most TWO ladders (matches the two-ability kit;
    // no skill in v1 needs more, and a fixed [2] keeps the struct POD and replication flat).
    struct { std::uint8_t ladder; std::uint16_t power; } buildup[2];   // ladder 0xFF = unused
    std::uint8_t  flags;          // bit0 kIgnoreDr, bit1 kIgnoreToughness, bit2 kIsDot
    std::uint8_t  source_kind;    // player | creature | hazard | terrain — for the kill record
};
```

Size: 26 bytes, POD, no pointers — cheap to replicate and to log (server-authoritative leader,
no VPS; combat state must stay this shape-simple).

Rules:

- **R1.** `amount[ch] ≥ 0` always. There is no healing channel; healing is not damage and does not
  pass through this pipeline (non-goal, §8).
- **R2.** Ranged/melee/spell verbs and both equipped abilities (the kit is basic attack + exactly
  TWO abilities — the art has exactly two ability poses per character) all emit this one struct.
  There is no second damage path.
- **R3.** Elemental channels usually carry their own ladder as `buildup[0]`
  (e.g. an ice bolt: `amount[Ice]=12`, `buildup[0]={Cold, 180}`). This is a convention for
  RFC-008 authoring defaults, not an engine rule — a pure-build-up skill (RainCall) may carry
  Power with zero damage, and a pure-damage skill may carry none.

### 4.3 Materials and the resistance matrix

Every creature, player and CombatEntity (RFC-004) has exactly one `Material` (umbrella §8):
Flesh, Stone, Spirit, Metal, Wood, Plant, Water, Slime. (Plant/Water here are *materials* — what a
thing is made of — not elements; the name collision with the future-work element icons is noted to
prevent exactly that confusion.)

`kMaterialMult[8][8]` — per-mille damage multiplier, material × channel. Starting values, **all
(tunable)**:

| ‰ | Slash | Pierce | Crush | Blast | Fire | Ice | Rock | Thunder |
|---|---|---|---|---|---|---|---|---|
| **Flesh**  | 1000 | 1000 | 1000 | 1000 | 1000 | 1000 | 1000 | 1000 |
| **Stone**  | 400 | 300 | 1200 | 1300 | 700 | 800 | 500 | 600 |
| **Spirit** | 150 | 150 | 150 | 300 | 1300 | 1300 | 1300 | 1300 |
| **Metal**  | 500 | 400 | 900 | 900 | 800 | 700 | 700 | 1500 |
| **Wood**   | 1200 | 800 | 900 | 1100 | 1600 | 700 | 800 | 500 |
| **Plant**  | 1300 | 700 | 800 | 1000 | 1800 | 900 | 700 | 600 |
| **Water**  | 800 | 600 | 700 | 900 | 500 | 1400 | 800 | 1800 |
| **Slime**  | 600 | 400 | 1300 | 1200 | 1200 | 900 | 1000 | 700 |

Flesh is the identity row on purpose: it is the row players and most of the 66 monsters sit on, so
the baseline tuning of P2 carries over unchanged.

**Invariant I4:** every cell is in `[100, 2000]` — floor ×0.1, cap ×2.0. There is **no 0‰ cell**.
The umbrella says Spirit is "immune to Physical Impact"; this RFC deliberately renders that as
150‰ physical damage plus **true impulse immunity** (Spirit's mass is treated as infinite by
RFC-003 — it cannot be shoved), because a literal 0× damage cell would make Spirit unkillable for
a melee-committed player, violating the deeper §9 rule that nothing is absolutely immune. The
"vulnerable to Arcane/Holy" clause is future work (those schools are icons only); in v1 Spirit's
counterweight is ×1.3 from **all four** elements. Flagged in Open Questions Q1.

**Reuse rule for build-up (important simplification):** the same matrix mitigates build-up Power.
A ladder's *source channel* is fixed (§4.5), and Power gain is multiplied by
`kMaterialMult[mat][source_channel]`. One table, two duties: a stone thing that shrugs ice damage
also frosts over more slowly, for free, with no second 64-cell table to keep consistent.

### 4.4 The damage formula — fixed order of operations

Inputs: packet `P`, defender sheet `p` (material `mat`, DR list, toughness `T`), outer multiplier
`M_outer` (see below).

```
Step 1  resisted  = Σ_ch  floor(P.amount[ch] * kMaterialMult[mat][ch] / 1000)
Step 2  scaled    = floor(resisted * M_outer / 1000)          // M_outer clamped to [250, 4000]
Step 3  dr_total  = 1000 - Π_i (1000 - dr_i)/1000  (per-mille), clamped to [0, 750]
        after_dr  = floor(scaled * (1000 - dr_total) / 1000)  // skipped if kIgnoreDr
Step 4  after_t   = max(0, after_dr - T)                      // skipped if kIgnoreToughness
Step 5  dealt     = (Σ_ch P.amount[ch] > 0) ? max(after_t, kChipDamage) : 0
```

- **`M_outer`** is a single per-mille scalar handed in by the caller, pre-multiplied from at most:
  the combo multiplier (RFC-002 detects the combo, e.g. Shatter 2500‰, and also sets `kIgnoreDr`
  for Shatter — this preserves today's "×2.5, ignores armour" exactly), the terrain/material
  interaction multiplier (RFC-003, e.g. mud's force-transfer bonus on Crush), and the battlefield
  state multiplier (RFC-010, e.g. earthquake accuracy). RFC-009 defines the *slot and clamp*;
  the contributing values belong to those RFCs. Clamp `[250, 4000]` (tunable) is invariant
  in *existence*: some clamp must exist so stacked multipliers cannot overflow int16 HP.
- **DR stacking (Step 3)** is multiplicative in the complement — two 300‰ sources give 510‰, not
  600‰ — so no combination of sources ramps linearly into the cap.
- **DoT ticks** (`kIsDot` set) skip Steps 2–4 entirely and apply
  `floor(amount * kMaterialMult / 1000)` only, no chip floor. Rationale: DoT numbers are 1–3 per
  tick; toughness ≥ 1 would silently zero every DoT in the game, and DR on DoT is double-dipping
  (the *application* hit already paid it).
- **Invariant I3 (chip floor):** any non-DoT hit whose packet carried damage deals at least
  `kChipDamage = 1` (tunable, but ≥ 1 is invariant). Combined with I4 and I5 this is the
  arithmetic form of "no absolute immunities": every fight is winnable by attrition, however
  badly matched the kit.
- **Rounding:** floor at every marked division, never round-to-nearest, never accumulate
  fractions. Two implementations that follow the five steps literally produce identical int32
  results; that is the convergence test.

Ring scaling (today's `ring_hp_scale` ×1→×5 / `ring_damage_scale` ×1→×3.2) stays where it is: baked
into stats at spawn (RFC-004), **not** a step of this pipeline. The pipeline must not know where
the fight happens.

### 4.5 Effect build-up: Power / Build-up / Decay

Five **gauges** per combatant, integer `value ∈ [0, 1000]`:

| # | Ladder | Source channel (for material mitigation) | Fed by |
|---|---|---|---|
| 0 | **Cold** | Ice | ice-school packets, winter water (RFC-002/003) |
| 1 | **Heat** | Fire | fire school, burning ground hazards (RFC-004) |
| 2 | **Shock** | Thunder | thunder school, Conduct chains (RFC-002) |
| 3 | **Earth** | Rock | rock school, mud/rubble hazards |
| 4 | **Stagger** | Crush | *derived* from physical damage — see below |

**Gain (Power → Build-up).** On hit, for each `buildup[i]` rider:

```
gain = floor( power
            * kMaterialMult[mat][source_channel(ladder)] / 1000
            * kTierGain[tier] / 1000
            * refractory(ladder) / 1000 )
value = min(1000, value + gain)
```

**Stagger is derived, not authored:** every non-DoT packet implicitly contributes
`floor((amount[Crush]*1000 + amount[Blast]*800 + amount[Slash]*200 + amount[Pierce]*100) / 1000)`
(all four weights tunable) as Stagger Power, in addition to any explicit `buildup` rider. This is
how "hitting things hard interrupts them" emerges from the rules instead of per-skill stun flags —
today's `CrushBlow stun_ticks=20` migrates to a large authored Stagger rider.

**Decay (closed-form — load-bearing for RFC-010).** Per gauge, store
`{ std::uint16_t value; std::uint32_t last_gain_tick; std::uint32_t refractory_until; }`.
The current value at tick `t` is *computed, never ticked*:

```
value_at(t) = max(0, value - kDecayRate[ladder] * max(0, t - last_gain_tick - kDecayDelay))
```

- `kDecayDelay = 15` ticks (1.5 s) (tunable): a grace window so alternating two abilities does not
  bleed the gauge between hits.
- `kDecayRate` per ladder, per tick (tunable): Cold 6, Heat 8, Shock 8, Earth 6, **Stagger 14**
  (stagger must be a burst achievement, not a slow grind). A full Cold gauge drains in ~17 s.
- Because decay is a pure function of elapsed ticks, a chunk at 1 Hz evaluates it 10× less often
  and gets the same answer; a slept chunk evaluates it once on wake. **No gauge state is ever lost
  or drifted by LOD** — the invariant RFC-010 needs from us (I7).
- A standing hazard (RFC-004) that applies `power_per_tick` to an occupant integrates the same
  way on catch-up: `gain = power_per_tick × elapsed`, applied through the same mitigation, then
  stage crossings are evaluated **once**, in ascending order, firing each crossed stage a single
  time (a catch-up may legally jump from stage 0 to terminal — it fires I, II, T once each).

**Stages and thresholds.** Fixed thresholds at **300 / 600 / 900** of 1000 (tunable as a triple,
identical for all ladders — per-ladder thresholds were considered and rejected: five ladders ×
three thresholds is fifteen knobs nobody can hold in their head, and the tier multiplier already
provides per-target pacing). Crossing a threshold upward emits a `StageEvent` consumed by RFC-002
(which owns what stages *do*) and RFC-006 (which owns how they *look* — tints and FX overlays,
never poses, because the 66 monster sheets are walk-only). Downward crossings by decay emit the
matching clear events.

Stage effect values below are the **proposed contract with RFC-002** — RFC-002 owns their
implementation; the numbers live here because they are tuning constants of the same family:

| Ladder | I @300 | II @600 | Terminal @900 |
|---|---|---|---|
| **Cold** | **Slow** — speed ×0.85 | **HeavySlow** — speed ×0.50 | **Freeze** — full stop, `kFreezeTicks = 25` (matches today's Frozen 25) |
| **Heat** | **Singed** — DoT 1 / 5 ticks | **Burning** — DoT 3 / 5 ticks, speed ×1.15 (it panics) | **Combust** — burst `min(15% max_hp, 60)` as Fire, then clears |
| **Shock** | **Static** — speed ×0.90 | **Shocked** — DoT 2 / 5 ticks, speed ×0.70 | **Paralyze** — stun `kParalyzeTicks = 12` |
| **Earth** | **Encumbered** — speed ×0.80 | **Mired** — speed ×0.45 (matches today's Muddy) | **Root** — cannot move, may still act, `kRootTicks = 20` |
| **Stagger** | **Unsteady** — knockback taken ×1.25 (RFC-003) | **Staggered** — interrupts current wind-up/channel (RFC-001/005), 5-tick flinch | **Knockdown** — `kKnockdownTicks = 15`, incapacitated |

All durations tunable; the *shape* (I = soft tell, II = today's status strength, T = brief hard
state) is the design. Deliberate continuity: stage II of each elemental ladder reproduces today's
P2 status numbers, so existing combos (Shatter/Blast/Conduct/Crush/Arc, `tiles.hpp`) re-target
cleanly — RFC-002 re-keys them from "has status" to "is at stage ≥ II" (Shatter requires the Freeze
terminal itself), preserving "chaining is a decision" (the window is now the terminal duration).

**Terminal resolution and refractory.** When a terminal fires:

1. The gauge **locks** at 1000 for the terminal's duration (no gain, no decay — you cannot extend
   a Freeze by hitting it with more ice; that would turn a window into a lock).
2. On expiry: `value := kPostTriggerFloor = 450` (tunable) and
   `refractory_until := t + kRefractoryTicks = 150` (15 s, tunable). While refractory,
   `refractory(ladder) = kRefractoryGain = 600‰` (tunable); otherwise 1000‰.
3. Refractory does **not** stack or grow across repeats — one flat, temporary ×0.6, one `uint32`
   of state. Elden-Ring-style growing thresholds were considered and rejected for v1: they add
   unbounded per-creature state and an invisible-to-the-player difficulty ramp; the flat
   refractory plus the 450-floor reset already makes the second Freeze meaningfully slower than
   the first without ever reaching immunity (**invariant I5b: `refractory ≥ 250‰`**). Q2 keeps the
   alternative open.

**Player asymmetry (chill guardrail):** the pipeline is symmetric — monsters build gauges on
players through the identical math — but players in rings 0–1 take Stagger Power at
`kHomeRingStaggerGain = 500‰` (tunable). A knockdown-chain in the meadow ring is the "difficulty
chasing the player" failure mode GAME.md §0 forbids; deep-ring and instance combat uses the full
values because the player opted in by walking there. This is one named constant, not a fork of the
formula. Additionally, all five gauges hard-clear (with their refractories) on respawn and on
leaving combat by `kOutOfCombatClear = 100` ticks (10 s, tunable) without giving or receiving a
packet — nothing follows the player home.

### 4.6 Scale tiers — big things resist, nothing is immune

Six tiers (umbrella §10). Tier is a property of the **archetype** (RFC-005/RFC-007: one policy per
archetype, 10–15 policies total), so an RL policy always trains against consistent gauge dynamics.

| Tier | `kTierGain` ‰ | `kTierTerminalDur` ‰ | Flat toughness `T` | Mass class (RFC-003) | Examples (from the verified sheet audit) |
|---|---|---|---|---|---|
| Tiny | 1400 | 1200 | 0 | 0.5 | critters, wisps |
| Small | 1150 | 1100 | 0 | 1 | most of the 66 walk-only monsters (slime, spider) |
| Medium | 1000 | 1000 | 1 | 1 | **players**, ghost, skull, wolves |
| Large | 700 | 800 | 2 | 3 | GiantFrog (2.5 t, smallest boss), Squids |
| Giant | 500 | 600 | 4 | 6 | GiantRedSamurai / GiantBlueSamurai (first RL boss), GiantBamboo, GiantRacoon (+Gold), Tengu (both phases), DemonCyclop |
| Titan | 350 | 450 | 6 | 12 | reserved — scripted set-pieces only in v1 |

All values (tunable). Invariants: **I5 `kTierGain ≥ 250‰`** and **I5c `kTierTerminalDur ≥ 300‰`** —
a Titan freezes for less time and needs ~3× the Power, but it freezes. Tier also scales terminal
durations: `duration = floor(kBaseDuration * kTierTerminalDur / 1000)`.

Dragons and GiantSlime/Flam/Spirit are absent from the RL rows by constraint (multi-part rigs /
idle+hit-only sheets — not RL agents); if used at all they are scripted and still get a tier, since
tier is pure math and needs no art.

**Worked example — hits to Freeze (ice bolt, `power = 180` (tunable), Flesh, no refractory, hits
faster than decay):**

| Target | Tier | Gain/hit | Hits to 900 | Second freeze (refractory ×0.6, from 450) |
|---|---|---|---|---|
| Slime (Slime mat: Ice 900‰) | Small | floor(180×900/1000×1150/1000)=186 | 5 | 8 |
| Ghost (Spirit mat: Ice 1300‰) | Medium | 234 | 4 | 7 |
| GiantFrog (Flesh) | Large | 126 | 8 | 12 |
| GiantRedSamurai (Flesh) | Giant | 90 | 10 | 17 |
| (Titan, Flesh) | Titan | 63 | 15 | 24 |

This table is the design goal made concrete: the same skill is a hard-CC tool against trash, a
commitment against the first RL boss, and never a dead button.

### 4.7 DR vs flat reduction — roles, sources, caps

Two mitigation primitives with deliberately different shapes:

| | **DR (percentage)** | **Toughness (flat)** |
|---|---|---|
| Scales with | hit size — big hits lose more absolute damage | hit count — each hit loses the same |
| Punishes | nothing specifically; even pressure | many-small-hits kits (FanVolley, DoT-free chip) |
| Reads as | "armoured" | "thick-skinned; ignores scratches" |
| Sources | armour quality (0–300‰, RFC-004 gear), boss guard stances (≤ 400‰, RFC-005 — e.g. the Samurai's Charge pose doubling as a guard), terrain cover (≤ 200‰, RFC-004) | scale tier (table §4.6), heavy armour pieces (+1..+2, RFC-004) |
| Stacking | multiplicative complement (§4.4 Step 3) | additive |
| Cap (invariant) | **I1: total ≤ 750‰** | **I2: total ≤ 8** |
| Position | before flat (Step 3 → 4) | last gate before chip |

Why percent-before-flat (the order matters and must not be re-derived differently): with flat
last, toughness has a stable, explainable meaning — *"after resists, this creature ignores N
damage per hit"* — a single number a player can learn, a HUD can show, and an RL observation can
carry (RFC-007). Flat-first would make toughness's effective value depend on the DR stack, i.e. a
hidden multiplication no one can reason about.

Anti-degeneracy tuning rule (not an engine invariant, a content invariant for RFC-005/008
authors): at every ring, the weakest intended weapon for that ring must retain ≥ 25% of its
pre-mitigation damage against the toughest intended target of that ring. If a tuning pass breaks
this, lower `T` or the DR source — never raise the weapon, which would power-creep every other
matchup.

### 4.8 State, replication, and the strike path

Per-combatant combat sheet added by this RFC:

```cpp
struct Gauge   { std::uint16_t value; std::uint32_t last_gain_tick; std::uint32_t refractory_until; };
struct DefenderSheet {
    Material      mat;        // 1 byte
    ScaleTier     tier;       // 1 byte
    std::uint8_t  toughness;  // resolved: tier + gear
    std::uint16_t dr[2];      // resolved sources, ‰ (gear, stance); terrain cover passed per-hit
    Gauge         gauges[5];  // 50 bytes
};
```

~56 bytes per combatant; gauges replicate delta-only and a zero gauge (value 0, no refractory) is
omitted — most of the world's creatures most of the time replicate **nothing** from this RFC,
which is what "combat state must be replicable and cheap" requires of us. The leader never needs
gauge state to validate anything (packets are validated at emission by the trusted actor, exactly
as today's check-and-debit in `abilities.hpp`); gauges live and die with the chunk that owns the
creature and are *not* persisted (same class of state as "monster positions: not saved" —
ARCHITECTURE.md §3).

Strike-path integration (normative sequence, replacing today's `strike()` body):

```
on packet arrival at owning chunk:
  1. refresh gauges to now (closed-form decay, emit downward stage events)
  2. RFC-002: detect combo from current stages  -> M_combo, flags
  3. RFC-003/004/010: collect M_env             -> M_outer = clamp(M_combo*M_env)
  4. §4.4 damage pipeline                       -> hp delta
  5. §4.5 gain per rider + derived Stagger      -> upward stage events
  6. emit events to RFC-002 (apply), RFC-006 (show), RFC-007 (observe)
```

Step 1 before Step 2 is load-bearing: a combo must be judged against the gauge as it *is*, not as
it was when the chunk last ticked — otherwise 1 Hz chunks judge Shatter against stale Freezes.

---

## 5. Interactions with Other RFCs

- **RFC-001 (Ability System):** computes attacker-side scaling and emits the final `DamagePacket`
  per the "already computed by the trusted actor" rule; consumes `Staggered` stage events as
  wind-up/channel interrupts. Cooldowns, costs and unlock gates stay entirely in RFC-001.
- **RFC-002 (Status & Effect Framework):** owns what stages *do* (the effect implementations
  behind the §4.5 table), combo detection, coatings (Wet remains a binary, duration-based coating
  and Conduct's conductor — it is **not** a fifth-element ladder), and cleansing. RFC-009 hands it
  gauge values and stage-crossing events; RFC-002 hands back `M_outer` contributions and flags.
- **RFC-003 (Physics & Material Interaction):** consumes `impulse` (Impulse/Mass = knockback,
  mass class from the tier table), owns terrain-material multipliers feeding `M_outer` (mud's
  Crush bonus, ice's damage reduction), and implements Spirit's impulse immunity.
- **RFC-004 (Terrain & Combat Entity):** every CombatEntity carries the same `DefenderSheet`
  (an Ice Pillar is Material Water, Tier Large, so a fire school burns it down faster —
  destructible counterplay priced by the same matrix); hazards emit packets/Power-per-tick through
  this pipeline; gear resolves `dr[]`/toughness.
- **RFC-005 (Boss Ability Authoring):** authors boss packets and picks (material, tier) per boss;
  guard-stance DR windows; uses the §4.6 hits-to-CC table as its tuning baseline for the Samurai.
- **RFC-006 (Visual FX & Telegraphs):** renders stage changes as tints/overlays/FX at the
  creature's position (walk-only sheets — never poses); must honour per-kind FX lifetimes (§7).
- **RFC-007 (RL Observation & Action Space):** consumes normalized gauges (§6).
- **RFC-008 (Data-driven Skill Definition):** serializes `DamagePacket`, the material matrix, and
  every (tunable) table in this RFC as data files; channel order in §4.1 is normative for it.
- **RFC-010 (Battlefield Simulation):** relies on invariant I7 (closed-form decay ⇒ LOD-safe and
  sleep-safe gauges) and the once-only catch-up crossing rule; owns which battlefield states feed
  `M_outer`.

## 6. RL Considerations

- **Observation:** per combatant, five gauges normalize to `value/1000 ∈ [0,1]` floats plus five
  terminal-active bits — 10 values for self, 10 for the current target; final layout is RFC-007's.
  Gauges are *dense, monotonic* signals: unlike a binary status, an agent gets gradient-like
  feedback ("ice is climbing") long before the discrete Freeze event, which is exactly the shape
  DQN reward propagation wants.
- **Reward shaping hooks:** stage events are clean discrete rewards (landing a terminal on the
  player: positive; own Stagger terminal: negative). The derived-Stagger rule means "get hit less"
  and "don't get interrupted" are the same lesson.
- **Determinism:** the pipeline is pure integer math — the training environment
  (`CombatEnvironment` replacing RLDrive's `core/env/`) runs bit-identical to the live game on
  every toolchain, the same class of guarantee the project already proved for worldgen
  (ARCHITECTURE.md §2c). What is *not* comparable across runs is message order; the pipeline
  itself never depends on it.
- **Per-archetype policies:** tier and material are archetype constants, so each of the 10–15
  policies trains against stationary gauge dynamics; no per-individual state exists in this RFC
  beyond the gauges themselves, which reset between episodes.
- **Cost:** evaluation is a few dozen int32 multiplies per hit; at RLDrive's measured ~790
  steps/s CPU training rate the damage math is noise, and the dojo's `Priority<2>` budget
  (visible in-world dojos, RL bosses in 10×7 interior rooms) is unaffected.

## 7. Asset & Engine Constraints Honored

- **Tone (GAME.md §0):** all state decays to zero; out-of-combat clear; home-ring Stagger
  asymmetry; nothing in this RFC runs a clock the player didn't start (§4.5).
- **66 monsters are 64×64 walk-only, zero attack frames:** no rule in this RFC requires a pose.
  Stage feedback is tint/overlay (frost rim, emissive burn, arcs — umbrella §15) rendered at the
  creature's position; RFC-006 owns the standard.
- **Boss sheets:** the tier table's Large/Giant rows name only the ~11 audited bosses with real
  Attack/Charge poses; Samurai (first RL boss) is the Giant-tier tuning baseline; Tengu's two
  phases share one tier (Trans sheet = same body scale). Dragons and GiantSlime/Flam/Spirit are
  excluded from RL rows.
- **Player kit:** basic attack + exactly two abilities; `buildup[2]` and the loadout-agnostic
  packet assume nothing wider (no hotbar).
- **Four elements exactly:** channels 4–7 are Fire/Ice/Rock/Thunder; the six spare school icons
  are future work only (§4.1).
- **121 skill icons with Disabled twins:** cooldown UI is RFC-001/006's; nothing here needs new
  icon art — gauges display as tints/bars, not icons.
- **FX lifetime gap:** the historical single `kEffectLife=6` truncated long strips (Rock's element
  FX is 14 frames; the fix — per-kind `effect_life_of` in `tiles.hpp` — already landed). This RFC
  adds new stage-change FX kinds; each MUST declare its own per-kind lifetime in that table
  (RFC-006), never inherit a global constant. No bespoke combo art exists; combo feedback stays
  text + existing FX composites.
- **Simulation LOD / sleep (1024×1024 world):** invariant I7 — decay and hazard gain are
  closed-form over elapsed ticks; a 1 Hz or slept chunk computes exact catch-up in O(1) (§4.5,
  §4.8).
- **Server-authoritative, first-node leader, no VPS:** packets validated at trusted emission;
  gauge state is chunk-local, delta-replicated, zero-suppressed, never persisted (§4.8).
- **RL substrate:** integer determinism and per-archetype constants fit the reused DQN core and
  the one-policy-per-archetype rule (§6); note RLDrive's hard-coded `kActionCount=15` remains
  RFC-007's hazard to handle, not ours.

## 8. Non-goals

- Status *behavior*, slots, coatings (Wet), cleansing, and combo detection — RFC-002.
- Knockback resolution, mass, friction, conductivity, terrain interaction rules — RFC-003.
- Hazard/entity lifecycles, gear and socket definitions — RFC-004.
- Boss phase scripts and per-boss numbers — RFC-005. FX/telegraph presentation — RFC-006.
- Skill file syntax and validation — RFC-008. LOD scheduling policy — RFC-010.
- Healing, vitals economy (health/mana/stamina regen), and death rules.
- PvP tuning (PvP is off by default; no constant here was balanced for it).
- Elements beyond the four; Arcane/Holy for Spirit — future work only.

## 9. Open Questions

1. **Q1 — Spirit vs the umbrella's "immune to Physical Impact".** This RFC renders it as 150‰
   physical damage + true impulse immunity, arguing a literal 0× breaks I4 and melee-only
   progression. Accept this reading, or additionally gate Spirit enemies to rings/instances where
   element access is guaranteed?
2. **Q2 — Refractory model.** Flat ×0.6 for 15 s (one uint32 of state) vs Elden-Ring-style growing
   thresholds (unbounded state, invisible ramp). v1 proposes flat; revisit if playtests show
   terminal-CC chaining on Giant bosses despite it.
3. **Q3 — Threshold uniformity.** One 300/600/900 triple for all ladders is a deliberate
   knob-count reduction; does Stagger want a two-stage ladder (600/900 only) so light hits never
   show a misleading stage-I tell?
4. **Q4 — Player-side stagger in deep rings.** Full-rate Stagger on players in rings 2+ is
   opt-in-difficulty by this RFC's reading of GAME.md §0 — confirm the tone owner agrees
   knockdowns on players are acceptable *anywhere*, or extend `kHomeRingStaggerGain` world-wide
   and reserve knockdown for instances.
5. **Q5 — Combust self-synergy.** The Heat terminal deals Fire damage, which is itself
   Heat-mitigated by material but could re-feed the Heat gauge if authored carelessly; proposal is
   that terminal-burst packets carry no build-up riders (rule for RFC-005/008 authors). Should
   that be promoted from authoring rule to engine invariant?
6. **Q6 — Chip floor vs swarms.** `kChipDamage=1` × FanVolley × large packs could trivialize
   toughness-heavy content; if so, the fix is capping volley shot counts (RFC-001), not raising
   the floor — flagging so the tuning conversation lands in the right RFC.
