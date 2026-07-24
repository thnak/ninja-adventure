# RFC-009: Damage, Resistance & Effect Build-up

> Status: **Accepted (revised after review)**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §6 (Physical Rules), §9 (Effect Accumulation), §10 (Scale)
> Companions: RFC-001 (abilities), RFC-002 (status framework), RFC-003 (physics/materials), RFC-004 (combat entities), RFC-005 (boss authoring), RFC-006 (FX), RFC-007 (RL spaces), RFC-008 (skill data), RFC-010 (battlefield simulation)

Every number proposed in this document is a **starting value marked (tunable)** unless it is
explicitly called an invariant. Invariants are the rules two independent implementations — and the
tuning process itself — must never break.

Ownership note (post-review): this RFC specifies **formulas and constants only** where a companion
RFC has not already claimed the ground. The material×channel matrix belongs to RFC-003 §3.1, the
scale-tier intake/mass numbers to RFC-003 §4, and the entire status/meter *model* (state shape,
thresholds, ladders, one-primary rule, soft-resist semantics) to RFC-002. Where those appear below
they appear **by reference**, with the exact numbers quoted for convenience and marked as quotes.

---

## 1. Summary

This RFC specifies the arithmetic core of the unified combat system:

1. **The damage formula** — a fixed-order integer pipeline over the *effective* payload RFC-003
   hands us (channels already scaled by its §3.1 material matrix and §8 Pass-A rules), through an
   outer multiplier slot (combos, environment), then percentage damage reduction (DR), then flat
   toughness, then a chip floor.
2. **Effect build-up mathematics** — the **gain formula** behind RFC-002's five build-up gauges
   (Cold, Heat, Shock, Earth, Stagger; `Gauge[5]` on [0,1000], stored here in §4.8's
   `DefenderSheet` per RFC-002 §1): Power (per-hit gain), the per-material status-affinity values,
   the derived Stagger contribution, and the calibration constants. Gauge semantics, thresholds,
   ladders (Slow → HeavySlow → Freeze), promotion and walk-down are RFC-002's; this RFC computes
   what gets added and owns the storage and decay constants.
3. **Scale-tier scaling** — how RFC-003 §4's six tiers (Tiny…Titan) enter the gain formula so a
   Titan boss *resists* crowd control without ever being *immune* to it, plus the per-tier flat
   toughness this RFC owns.
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
visibly **slows** (Slow). Keep committing to ice and he slows to a crawl (HeavySlow), then
locks solid for a moment — **Freeze**, your Shatter window. Walk away instead, and the frost fades
on its own. Nothing about him was immune; he was just *big*, and big things take persistence.

A slime, meanwhile, freezes on the very first bolt. Same spell, same math, different scale tier —
the player learns one rule ("big resists, nothing is immune") instead of sixty-six bespoke immunity
lists.

Chill guardrail (GAME.md §0): every meter in this system **decays to zero on its own** (RFC-002
§3), and no build-up, DoT or debuff outlives disengagement by more than a few seconds. Combat
difficulty here is something you walk *into*; the moment you walk away, the math lets go of you.
Nothing in this RFC ticks, counts down, or accumulates behind a player's back outside of a fight
they are standing in.

### For the designer

You author a skill (RFC-008) by filling in a payload: *how much of which channels, how much
build-up Power on which RFC-002 channel*. You never write `if (target == GiantFrog)`. The target's
material row (RFC-003), scale tier, DR and toughness do the rest. Making a boss "feel armoured" is
two integers (material = Metal, tier = Giant), not a script.

Two knobs you will actually turn while tuning:

- **Power** on a skill: how fast its ladder climbs — the "status-build weapon" knob.
- **Tier gain multiplier** on an archetype (numbers in RFC-003 §4): how stubborn a boss is against
  *all* ladders at once.

### For the engineer

One function, pure, integer, order-fixed:

```
(EffectivePacket, DefenderSheet, M_outer) -> (hp_delta, meter_gains[5], events)
```

No floats, no RNG, no reads outside its arguments. It is called from the chunk's strike path
(where `MeleeSwing`/`CastSpell`/`AbilityStrike` resolve today), and identically from the RL
training environment (RFC-007) and from hazard auras (RFC-004). Meter decay, promotion and
walk-down live in RFC-002's `status_step`; every quantity this RFC feeds it is linear in elapsed
ticks, so a chunk ticked at 1 Hz or woken from sleep (RFC-010) catches up exactly in O(1) — the
same design that lets crops survive sleeping chunks (ARCHITECTURE.md §4).

---

## 4. Reference-level Design

All tick counts assume the **10 Hz** simulation tick. All multipliers are per-mille `std::uint16_t`
(1000 = ×1.0). All arithmetic is `int32` (`int64` intermediates where products require it), all
division is floor division; intermediate values are clamped to `≥ 0` before each subsequent step.
This section is normative.

### 4.1 Damage channels

The channel taxonomy and order are **owned by RFC-003 §2** (`AttackPayload`): Damage / Pierce /
Crush / Impulse / Heat / Cold / Electric / Explosion. This RFC's pipeline consumes the seven
harm-carrying channels; **Impulse never enters the damage path** — it is consumed by RFC-003's
knockback law (`Impulse / Mass`, umbrella §11) and merely travels in the same packet.

| # | Channel | Family | Today's source (code) | Typical carrier |
|---|---|---|---|---|
| 0 | **Damage** | physical | melee light swing | swords, claws — the untyped baseline |
| 1 | **Pierce** | physical | arrows (`LaunchArrow`) | arrows, spikes, spears |
| 2 | **Crush** | physical | heavy melee (`heavy`) | hammers, boulders, CrushBlow |
| 3 | **Explosion** | physical | Blast combo splash | explosions, meteor impact |
| 4 | **Heat** | elemental | `Element::kFire` | fire school (BookFire) |
| 5 | **Cold** | elemental | `Element::kIce` | ice school (BookIce) |
| 6 | **Electric** | elemental | `Element::kShock` | thunder school (BookThunder) |

Exactly four elemental *books* — Fire / Ice / Rock / Thunder — matching the four skill-book icons
the asset pack ships (`BookFire/BookIce/BookRock/BookThunder`). Fire/Ice/Thunder map onto the
Heat/Cold/Electric energy channels; **Rock (`Element::kEarth`, BookRock) is not an energy type** —
per RFC-003 §2 it emits Crush + Impulse + Explosion, so there is no Rock damage channel and no
fifth element. Plant, Water, Light, Darkness, Wind and Death exist **as icons only** and are out
of scope for v1; a future school claims a new channel or a new combination via RFC-003, without
disturbing this format.

### 4.2 The effective packet — what this pipeline receives

RFC-003 §8's `resolve()` applies its §3.1 material matrix and Pass-A interaction rules **before
RFC-009 sees the hit** (RFC-003's words: the matrix is applied "before RFC-009 sees it"). What
arrives here is the *effective* packet — channel amounts already material-scaled — plus the
build-up riders, which the matrix does not touch (build-up mitigation is §4.5's job, through a
different table):

```cpp
// One hit, post RFC-003 §3.1 / §8 Pass-A. Attacker-side scaling (school damage, ability
// damage_scale, ring scaling, charge multipliers) was applied at emission by RFC-001 —
// mirroring today's protocol.hpp rule that "damage arrives ALREADY COMPUTED by the trusted actor".
struct DamagePacket {
    std::int16_t  amount[7];      // effective per-channel damage, RFC-003 §2 order minus Impulse
    std::uint16_t impulse;        // physics only — consumed by RFC-003, never by §4.4
    // Build-up riders. `channel` is RFC-002's Channel enum (kCold..kStagger). A packet may push at
    // most TWO channels (matches the two-ability kit; no skill in v1 needs more, and a fixed [2]
    // keeps the struct POD and replication flat).
    struct { std::uint8_t channel; std::uint16_t power; } buildup[2];   // channel 0 = unused
    std::uint8_t  flags;          // bit0 kIgnoreDr, bit1 kIgnoreToughness, bit2 kIsDot
    std::uint8_t  source_kind;    // player | creature | hazard | terrain — for the kill record
};
```

Size: ~26 bytes, POD, no pointers — cheap to replicate and to log (server-authoritative leader,
no VPS; combat state must stay this shape-simple).

Rules:

- **R1.** `amount[ch] ≥ 0` always. There is no healing channel; healing is not damage and does not
  pass through this pipeline (non-goal, §8).
- **R2.** Ranged/melee/spell verbs and both equipped abilities (the kit is basic attack + exactly
  TWO abilities — the art has exactly two ability poses per character) all emit this one struct.
  There is no second damage path.
- **R3.** Elemental packets usually carry their own channel as `buildup[0]`
  (e.g. an ice bolt: `amount[Cold]=12`, `buildup[0]={kCold, 600}`). This is a convention for
  RFC-008 authoring defaults, not an engine rule — a pure-build-up skill (RainCall) may carry
  Power with zero damage, and a pure-damage skill may carry none.

### 4.3 Materials — damage matrix by reference, status affinity owned here

Every creature, player and CombatEntity (RFC-004) has exactly one `Material` (RFC-003 §3):
Flesh, Stone, Spirit, Metal, Wood, Plant, Water, Slime.

**Damage mitigation by material is RFC-003 §3.1's matrix, applied before this pipeline** — this
RFC defines no material×damage-channel table and applies no second material multiply (doing both
would double-mitigate every hit). RFC-003's settled readings bind here, including **Spirit at 0‰
on all four physical channels** (counterweighted by ×1.3 energy and 250‰ Explosion, owned there).
Per-creature deviation *from* the material default is this RFC's territory, expressed through DR
and toughness (§4.7), never through a second matrix.

What this RFC does own is the **status-affinity table** — the per-mille values behind RFC-002's
material affinity classes (RFC-002 "Interactions" table fixes the shape and class ratios; the
final numbers live here, as RFC-002 directs). It mitigates build-up Power, per RFC-002 channel:

| ‰ (tunable) | Cold | Heat | Shock | Earth | Stagger |
|---|---|---|---|---|---|
| **Flesh**  | 1000 | 1000 | 1000 | 1000 | 1000 |
| **Stone**  | 500 | 500 | 750 | 400 | 600 |
| **Metal**  | 750 | 750 | 1500 | 800 | 800 |
| **Wood**   | 1000 | 1500 | 750 | 750 | 1000 |
| **Plant**  | 1250 | 1750 | 750 | 750 | 1000 |
| **Water**  | 1500 | 500 | 1750 | 1250 | 500 |
| **Spirit** | 750 | 750 | 750 | 250 | 250 |
| **Slime**  | 1250 | 1000 | 1000 | 1250 | 250 |

Flesh is the identity row on purpose: it is the row players and most of the 66 monsters sit on, so
the baseline tuning of P2 carries over unchanged.

**Invariant I4:** every status-affinity cell is `≥ 100‰` (×0.1) — RFC-002 §6's multiplier floor,
restated as this table's hard floor. There is no 0‰ cell and no status immunity: a ghost is mired
slowly; it mires.

### 4.4 The damage formula — fixed order of operations

Inputs: effective packet `E` (§4.2), defender sheet `p` (DR list, toughness `T`), outer multiplier
`M_outer` (see below).

```
Step 1  base      = Σ_ch  E.amount[ch]                        // material already applied (RFC-003 §3.1)
Step 2  scaled    = floor(base * M_outer / 1000)              // M_outer clamped to [250, 4000]
Step 3  dr_total  = 1000 - Π_i (1000 - dr_i)/1000  (per-mille), clamped to [0, 750]
        after_dr  = floor(scaled * (1000 - dr_total) / 1000)  // skipped if kIgnoreDr
Step 4  after_t   = max(0, after_dr - T)                      // skipped if kIgnoreToughness
Step 5  dealt     = (base > 0) ? max(after_t, kChipDamage) : 0
```

- **`M_outer`** is a single per-mille scalar composed by the caller from at most three per-mille
  contributors: the combo multiplier `M_combo` (RFC-002 detects the combo, e.g. Shatter 2500‰, and
  also sets `kIgnoreDr` for Shatter — preserving today's "×2.5, ignores armour" exactly), the
  terrain/material interaction multiplier `M_terrain` (RFC-003, e.g. slip mitigation on ice), and
  the battlefield state multiplier `M_battle` (RFC-010). **Composition is normative:** an absent
  contributor is 1000‰, and

  ```
  M_outer = clamp( floor( floor(M_combo * M_terrain / 1000) * M_battle / 1000 ), 250, 4000 )
  ```

  — chained per-mille floors in that fixed order, so all implementations converge bit-for-bit.
  RFC-009 defines the slot, composition and clamp; the contributing values belong to those RFCs.
  Clamp `[250, 4000]` (tunable) is invariant in *existence*: some clamp must exist so stacked
  multipliers cannot overflow int16 HP.
- **DR stacking (Step 3)** is multiplicative in the complement — two 300‰ sources give 510‰, not
  600‰ — so no combination of sources ramps linearly into the cap.
- **DoT ticks** (`kIsDot` set) skip Steps 2–4 entirely and apply `base` directly, no chip floor
  (RFC-002's rule E2 — DoT never kills players — applies downstream). Rationale: DoT numbers are
  1–3 per tick; toughness ≥ 1 would silently zero every DoT in the game, and DR on DoT is
  double-dipping (the *application* hit already paid it).
- **Invariant I3 (chip floor):** any non-DoT hit whose *effective* packet carries damage deals at
  least `kChipDamage = 1` (tunable, but ≥ 1 is invariant). Combined with I4 and RFC-003 §4's
  ≥ 150‰ tier floor, this is the arithmetic form of "no absolute immunities" *within this
  pipeline*: every fight is winnable by attrition, however badly matched the kit. The one upstream
  exception is RFC-003's Spirit×physical 0‰ row, whose counterweight is owned there — see Q1.
- **Rounding:** floor at every marked division, never round-to-nearest, never accumulate
  fractions. Two implementations that follow the five steps literally produce identical int32
  results; that is the convergence test.

Ring scaling (today's `ring_hp_scale` ×1→×5 / `ring_damage_scale` ×1→×3.2) stays where it is: baked
into stats at spawn (RFC-004), **not** a step of this pipeline. The pipeline must not know where
the fight happens.

### 4.5 Effect build-up: the gain formula behind RFC-002's meters

The build-up *state and semantics* are **RFC-002 §§1–6**: the five gauges on the shared `[0,1000]`
scale for channels Cold / Heat / Shock / Earth / Stagger; thresholds `T1=300 / T2=600 / T3=900`
(one triple, identical for all five ladders); **exactly one active primary status** (RFC-002 §4's
promotion/eviction algorithm — gauges accumulate independently, but statuses never stack, preserving
P2's "combos are a decision" and GAME.md §7's new-status-overwrites-old rule); per-ladder decay
(Cold 6/tick … Stagger 14/tick) after a 15-tick grace, paused while primary; walk-down one rung at a
time. This RFC re-specifies none of that. It owns exactly what RFC-002 delegates to it: **the gain
formula, the affinity values (§4.3), the derived Stagger contribution, and the calibration
constants.**

| RFC-002 channel | Fed by | Affinity column |
|---|---|---|
| **Cold** | ice-school packets (BookIce), winter water (RFC-002/003) | Cold |
| **Heat** | fire school (BookFire), burning ground hazards (RFC-004) | Heat |
| **Shock** | thunder school (BookThunder), Conduct chains (RFC-002 §7) | Shock |
| **Earth** | rock school (BookRock), bog/rubble terrain `kAmbient` gain (RFC-002 §8, RFC-010) | Earth |
| **Stagger** | *derived* from physical damage (below) + the Crush combo's authored +800 (RFC-002 §7) | Stagger |

**Earth is Rock's ladder** (Encumbered → Mired → Root, RFC-002 §2), completing the one-book-one-
ladder symmetry: Ice→Cold, Fire→Heat, Thunder→Shock, Rock→Earth, with Stagger the non-elemental
fifth derived from physical channels. P2's Muddy slow is Earth stage 2 (Mired), not a coating.
**Poison is not a v1 channel** — the four element books are Fire/Ice/Rock/Thunder and no v1 content
inflicts Poison (RFC-002 Open Question 2 parks it as a candidate future gauge; the runtime array is
fixed `[5]`). The gain path below runs identically for all five channels, satisfying RFC-002's
"RFC-009 owns the gain formula."

**Gain (Power → meter).** On hit, for each `buildup[i]` rider (and the derived Stagger term), the
gain handed to RFC-002's `status_gain` is one `int64` product with **one** floor:

```
gain = floor( power
            * kStatusAffinity[mat][ch]        // §4.3, ‰
            * kTierGain[tier]                 // this RFC §4.6 "Build-up intake", ‰ — echoed in RFC-003 §4
            * soft_resist(ch)                 // RFC-002 §6: 1000 / 500 / 250 ‰
            * coating_mult(ch)                // RFC-002 §5 X2–X4: e.g. Wet→Heat 500, Wet→Shock 1500, Wet→Cold 1250; else 1000 ‰
            / 1'000'000'000'000 )
meter[ch] = min(1000, meter[ch] + gain)       // clamp is RFC-002's ([0,1000] scale)
```

RFC-002 sketches the multiplier argument as a float; **this integer product is the normative
arithmetic** — the float in RFC-002's signature is this per-mille product /10⁹, and no
implementation may compute it in floating point. RFC-002's coating multipliers and the soft-resist
schedule (150 ticks, ×0.5, ×0.25 on repeat inside the window — two levels, never deeper) are
normative *inputs*; this RFC does not re-derive them. The soft-resist factor never goes below
**250‰** — **RFC-002 §6's Invariant I5b**, defined there and consumed here, not restated as this
RFC's own. Second-freeze
pacing comes from soft-resist plus RFC-002 §3's rule that leaving a top rung empties the meter to
0; a Freeze can never be *extended* by more ice (stage 3 cannot escalate, and expiry always walks
down) — a window, not a lock.

**Stagger is derived, not only authored:** every non-DoT packet implicitly contributes

```
stagger_power = floor( (E.amount[Crush]*1000 + E.amount[Explosion]*800
                      + E.amount[Damage]*200 + E.amount[Pierce]*100) / 1000 )
```

(all four weights tunable) as Stagger Power, in addition to any explicit `buildup` rider. This is
how "hitting things hard interrupts them" emerges from the rules instead of per-skill stun flags —
today's `CrushBlow stun_ticks=20` migrates to a large authored Stagger rider. Mitigation is
deliberate and single-pass: the blend is computed from *effective* (post-RFC-003-matrix) amounts,
then mitigated once by the **Stagger affinity column** — never by any damage-matrix cell — so a
material's damage response (Stone fearing hammers) and its stagger response (Stone's 600‰ Stagger)
each apply exactly once, each through its own correct cell.

**LOD and catch-up (I7).** Every term above is linear or piecewise-linear in elapsed ticks: gains
are per-hit, hazard `power_per_tick` integrates as `gain = power_per_tick × elapsed` through the
same formula, and RFC-002's decay/DoT/walk-down are arithmetic in `dticks`. So `status_step`
(RFC-002 §9) at 1 Hz, or once on chunk wake, produces **bit-identical** meters and the same stage
crossings as 10 Hz stepping — crossings are evaluated once, in ascending order, each fired a
single time (a catch-up may legally jump a meter from 0 past several thresholds; RFC-002 §4's
end-of-tick promotion then lands the correct stage). Sleeping chunks clear `StatusState` entirely
— RFC-002 §9's stated invariant, unobservable by construction since no player is near. **No meter
state visible to any player is ever lost or drifted by LOD** — the invariant RFC-010 needs (I7).

**Stages, thresholds, durations, FX.** What stages *do* and how long they last is RFC-002 §2's
ladder table (Chilled/Frostbound/Frozen, Singed/Burning/Ablaze, Static/Shocked/Overloaded,
Encumbered/Mired/Root, Unsteady/Staggered/Knockdown); how they *look* is RFC-006's (tints and FX overlays,
never poses — the 66 monster sheets are walk-only). RFC-002 §11's compatibility contract keeps
stage II of each elemental ladder at today's P2 status strength, so existing combos
(Shatter/Blast/Conduct/Crush/Arc, `tiles.hpp`) re-key cleanly to "is at stage ≥ II" (Shatter
requires Frozen itself), preserving "chaining is a decision".

**Calibration (this RFC's constant, honoring RFC-002 §11 / Open Question 6; RECONCILIATION.md
ruling 4):** `kIceBoltPower = 600` (tunable) — one cast reaches exactly `T2 = 600` on a Medium
Flesh target with empty meters and no modifiers, so one Ice bolt still visibly chills hard
(HeavySlow) and two still freeze on baseline targets — the P2 two-cast-freeze contract. All
per-source Power tables (RFC-008) are stated relative to this anchor.

**Player asymmetry (chill guardrail):** the pipeline is symmetric — monsters build meters on
players through the identical math — but players in rings 0–1 take Stagger-channel Power at
`kHomeRingStunGain = 500‰` (tunable). A knockdown-chain in the meadow ring is the "difficulty
chasing the player" failure mode GAME.md §0 forbids; deep-ring and instance combat uses the full
values because the player opted in by walking there. This is one named constant folded into the
gain product, not a fork of the formula. RFC-002's decay constants already guarantee that a full
meter empties within ~10 s of disengagement and coatings within ~8 s; on top of that, respawn
hard-clears `StatusState` (proposed to RFC-002, matching its instance-teardown clear) — nothing
follows the player home.

### 4.6 Scale tiers — big things resist, nothing is immune

Six tiers (umbrella §10). Tier is a property of the **archetype** (RFC-005/RFC-007: one policy per
archetype, 10–15 policies total), so an RL policy always trains against consistent meter dynamics.

**The intake and mass numbers are RFC-003 §4's — quoted here for convenience, changed only there**
("this RFC only defines the numbers so tier semantics live in one place" is RFC-003's claim, and
this RFC honors it). The flat-toughness column is this RFC's own (§4.7).

| Tier | Build-up intake ‰ *(RFC-003 §4)* | Mass pts *(RFC-003 §4)* | Flat toughness `T` (this RFC) | Examples (aligned with RFC-003 §4) |
|---|---|---|---|---|
| Tiny | 1600 | 25 | 0 | summoned wisps, critters |
| Small | 1300 | 50 | 0 | most of the 66 walk-only monsters (slime, spider) |
| Medium | 1000 | 100 | 1 | **players**, ghost, skull, wolves |
| Large | 600 | 250 | 2 | bears, elite monsters, GiantFrog (smallest boss), Squids |
| Giant | 350 | 700 | 4 | GiantRedSamurai / GiantBlueSamurai (first RL boss), GiantBamboo, GiantRacoon (+Gold), DemonCyclop, Tengu phase 1 |
| Titan | 200 | 2000 | 6 | Tengu Trans phase 2; otherwise reserved for scripted set-pieces |

Toughness values (tunable). The invariant floor on intake — **I5: gain ≥ 250‰ for any future
tier** — is RFC-003 §4's: a Titan needs ~3× the Power (200‰ base intake, never below the 250‰
gain floor once affinity/coat multipliers are folded in), but it freezes. Terminal durations
scale by **RFC-002 §6's `kTierTerminalDur`** (Large 800‰, Giant 600‰, Titan 450‰, invariant
I5c ≥ 300‰), not by a second table here.

Dragons and GiantSlime/Flam/Spirit are absent from the RL rows by constraint (multi-part rigs /
idle+hit-only sheets — not RL agents); if used at all they are scripted and still get a tier, since
tier is pure math and needs no art.

**Worked example — hits to Frozen (ice bolt, `power = kIceBoltPower = 600`, no coating, no
soft-resist, hits landing faster than decay; gain per the single-floor product in §4.5, Frozen at
meter ≥ T3 = 900, primary slot free — RFC-002 §4). Script-verified, `scratchpad/verify.py` /
RECONCILIATION.md ruling 2:**

| Target | Tier | Gain/hit | Hits to Frozen | Second freeze (soft-resist ×0.5, meter emptied to 0 on leaving Frozen — RFC-002 §3/§6) |
|---|---|---|---|---|
| Slime (Slime mat: Cold 1250‰) | Small (1300‰) | floor(600×1250×1300/10⁶) = 975 | 1 | gain 487 → 2 |
| Ghost (Spirit mat: Cold 750‰) | Medium (1000‰) | 450 | 2 | 225 → 4 |
| GiantFrog (Flesh) | Large (600‰) | 360 | 3 | 180 → 5 |
| GiantRedSamurai (Flesh) | Giant (350‰) | 210 | 5 | 105 → 9 |
| (Titan, Flesh) | Titan (200‰) | 120 | 8 | 60 → 15 |

(Verification rule: hits = ⌈900 / gain⌉; second-freeze gain = floor(first-freeze product × 500 /
1000 at the same single-floor position). The Samurai walks Chilled → Frostbound → Frozen across
bolts 3/4/5 — the guide-level story in §3, made concrete.)

This table is the design goal: the same skill is a hard-CC tool against trash, a commitment
against the first RL boss, and never a dead button.

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
struct DefenderSheet {
    Material      mat;        // 1 byte (RFC-003 §3)
    ScaleTier     tier;       // 1 byte (RFC-003 §4)
    std::uint8_t  toughness;  // resolved: tier + gear
    std::uint16_t dr[2];      // resolved sources, ‰ (gear, stance); terrain cover passed per-hit
    StatusState   status;     // RFC-002 §1 — 14 bytes, shape owned by RFC-002
};
```

~21 bytes per combatant. Status replication is RFC-002 §10's (2 bytes per bystander creature, full
state only for the own player's HUD) — this RFC adds no replicated fields beyond mat/tier/
toughness/dr, which are archetype- or gear-static. A terminal state (Frozen, Knockdown) is fully
representable and LOD-steppable because it *is* RFC-002's `primary + stage + stage_ticks` —
`stage_ticks` stores the lock's remaining duration, decay is paused while primary, and a chunk
woken mid- or post-terminal reconstructs everything from one `status_step(dticks)` call. The
leader never needs meter state to validate anything (packets are validated at emission by the
trusted actor, exactly as today's check-and-debit in `abilities.hpp`); status lives and dies with
the chunk that owns the creature and is *not* persisted (same class of state as "monster
positions: not saved" — ARCHITECTURE.md §3).

Strike-path integration (normative sequence, replacing today's `strike()` body):

```
on packet arrival at owning chunk (payload already through RFC-003 §8 steps 1–2):
  1. RFC-002 status_step to now (closed-form dticks) -> walk-downs, expiry events
  2. RFC-002: detect combo from current stages       -> M_combo, flags
  3. compose M_outer (M_terrain from RFC-003, M_battle from RFC-010, absent = 1000):
       M_outer = clamp(floor(floor(M_combo*M_terrain/1000)*M_battle/1000), 250, 4000)
  4. §4.4 damage pipeline                            -> hp delta
  5. §4.5 gain per rider + derived Stagger           -> meter gains (promotion at end-of-tick, RFC-002 §4)
  6. emit events to RFC-002 (apply), RFC-006 (show), RFC-007 (observe)
```

Step 1 before Step 2 is load-bearing: a combo must be judged against the meters as they *are*, not
as they were when the chunk last ticked — otherwise 1 Hz chunks judge Shatter against stale
Freezes.

---

## 5. Interactions with Other RFCs

- **RFC-001 (Ability System):** computes attacker-side scaling and emits the payload per the
  "already computed by the trusted actor" rule; consumes `Staggered` stage events as
  wind-up/channel interrupts. Cooldowns, costs and unlock gates stay entirely in RFC-001.
- **RFC-002 (Status & Effect Framework):** owns `StatusState`, the five channels, thresholds,
  ladders and durations, the one-primary promotion rule, soft-resist semantics, coatings and
  cleansing, combo detection. RFC-009 owns the gain formula, the §4.3 affinity values, derived
  Stagger, and the §4.5 calibration — exactly the carve-out RFC-002's own interaction section
  specifies. RFC-002 hands back `M_combo` and flags.
- **RFC-003 (Physics & Material Interaction):** owns the channel taxonomy (§2), the
  material×channel damage matrix (§3.1, applied before this pipeline — never re-applied here),
  the tier intake/mass numbers (§4), `impulse` resolution, and `M_terrain`. Spirit's physical
  story (0‰ + impulse row) is settled there; see Q1.
- **RFC-004 (Terrain & Combat Entity):** every CombatEntity carries the same `DefenderSheet`
  (an Ice Pillar is Material Water, Tier Large, so a fire school burns it down faster —
  destructible counterplay priced by RFC-003's matrix); hazards emit packets/Power-per-tick
  through this pipeline; gear resolves `dr[]`/toughness.
- **RFC-005 (Boss Ability Authoring):** authors boss packets and picks (material, tier) per boss;
  guard-stance DR windows; uses the §4.6 hits-to-CC table as its tuning baseline for the Samurai.
- **RFC-006 (Visual FX & Telegraphs):** renders stage changes as tints/overlays/FX at the
  creature's position (walk-only sheets — never poses); must honour per-kind FX lifetimes (§7).
- **RFC-007 (RL Observation & Action Space):** consumes normalized meters within its 8 reserved
  indices (§6).
- **RFC-008 (Data-driven Skill Definition):** serializes the packet riders, the §4.3 affinity
  table, and every (tunable) table in this RFC as data files; channel taxonomy is RFC-003's.
- **RFC-010 (Battlefield Simulation):** relies on invariant I7 (linear-in-ticks arithmetic ⇒
  LOD-safe and sleep-safe status) and the once-only catch-up crossing rule; owns `M_battle`.

## 6. RL Considerations

- **Observation:** the proposal to RFC-007 fits its **8 reserved indices (104–111)** exactly —
  104–108: own five meters `/255`; 109: target primary-channel meter `/255` (the value RFC-002
  §10 already replicates at boss-bar granularity); 110: target primary channel `/5`; 111: own
  primary stage `/3`. Terminal-active information is *not* re-encoded — it is already derivable
  from RFC-007's existing status one-hots at indices 1–5 / 26–30, which RFC-007 re-keys to
  RFC-002 stages (avoiding a third status encoding). Anything wider than these 8 floats requires
  an RFC-007 `kObsVersion` bump and is explicitly deferred to RFC-007's owner. Meters are *dense,
  monotonic* signals: unlike a binary status, an agent gets gradient-like feedback ("ice is
  climbing") long before the discrete Freeze event, which is exactly the shape DQN reward
  propagation wants.
- **Reward shaping hooks:** stage events are clean discrete rewards (landing a terminal on the
  player: positive; own Knockdown terminal: negative). The derived-Stagger rule means "get hit less"
  and "don't get interrupted" are the same lesson.
- **Determinism:** the pipeline is pure integer math — the training environment
  (`CombatEnvironment` replacing RLDrive's `core/env/`) runs bit-identical to the live game on
  every toolchain, the same class of guarantee the project already proved for worldgen
  (ARCHITECTURE.md §2c). What is *not* comparable across runs is message order; the pipeline
  itself never depends on it (and RFC-002 §9's boundary — no cross-run meter equality — stands).
- **Per-archetype policies:** tier and material are archetype constants, so each of the 10–15
  policies trains against stationary meter dynamics; no per-individual state exists in this RFC
  beyond RFC-002's `StatusState`, which resets between episodes.
- **Cost:** evaluation is a few dozen int32/int64 multiplies per hit; at RLDrive's measured ~790
  steps/s CPU training rate the damage math is noise, and the dojo's `Priority<2>` budget
  (visible in-world dojos, RL bosses in 10×7 interior rooms) is unaffected.

## 7. Asset & Engine Constraints Honored

- **Tone (GAME.md §0):** all state decays to zero (RFC-002 §3); nothing follows the player home;
  home-ring Stagger asymmetry; nothing in this RFC runs a clock the player didn't start (§4.5).
- **66 monsters are 64×64 walk-only, zero attack frames:** no rule in this RFC requires a pose.
  Stage feedback is tint/overlay (frost rim, emissive burn, arcs — umbrella §15) rendered at the
  creature's position; RFC-006 owns the standard.
- **Boss sheets:** the tier table's Large/Giant/Titan rows name only the ~11 audited bosses with
  real Attack/Charge poses; Samurai (first RL boss) is the Giant-tier tuning baseline; Tengu's
  Trans sheet is its Titan-tier phase 2 (per RFC-003 §4). Dragons and GiantSlime/Flam/Spirit are
  excluded from RL rows.
- **Player kit:** basic attack + exactly two abilities; `buildup[2]` and the loadout-agnostic
  packet assume nothing wider (no hotbar).
- **Four elements exactly:** Fire/Ice/Thunder map to Heat/Cold/Electric; Rock maps to the
  physical trio per RFC-003 §2 — no fifth element, no Earth ladder; the six spare school icons
  are future work only (§4.1).
- **121 skill icons with Disabled twins:** cooldown UI is RFC-001/006's; nothing here needs new
  icon art — meters display as tints/pips (RFC-002 §10 / RFC-006), not icons.
- **FX lifetime gap:** the historical single `kEffectLife=6` truncated long strips (Rock's element
  FX is 14 frames; the fix — per-kind `effect_life_of` in `tiles.hpp` — already landed). This RFC
  adds new stage-change FX kinds; each MUST declare its own per-kind lifetime in that table
  (RFC-006), never inherit a global constant. No bespoke combo art exists; combo feedback stays
  text + existing FX composites.
- **Simulation LOD / sleep (1024×1024 world):** invariant I7 — every gain/decay term is linear in
  elapsed ticks, so a 1 Hz or waking chunk computes exact catch-up in O(1); sleeping chunks clear
  status per RFC-002 §9 (§4.5, §4.8).
- **Server-authoritative, first-node leader, no VPS:** packets validated at trusted emission;
  status replication per RFC-002 §10 (2 bytes/bystander), never persisted (§4.8).
- **RL substrate:** integer determinism and per-archetype constants fit the reused DQN core and
  the one-policy-per-archetype rule (§6); note RLDrive's hard-coded `kActionCount=15` remains
  RFC-007's hazard to handle, not ours.

## 8. Non-goals

- Status *behavior*, state shape, thresholds, ladders, slots, coatings (Wet), cleansing, and
  combo detection — RFC-002.
- The material×channel damage matrix, channel taxonomy, knockback, mass, tier intake numbers,
  conductivity, terrain interaction rules — RFC-003.
- Hazard/entity lifecycles, gear and socket definitions — RFC-004.
- Boss phase scripts and per-boss numbers — RFC-005. FX/telegraph presentation — RFC-006.
- Skill file syntax and validation — RFC-008. LOD scheduling policy — RFC-010.
- Healing, vitals economy (health/mana/stamina regen), and death rules.
- PvP tuning (PvP is off by default; no constant here was balanced for it).
- Elements beyond the four; Arcane/Holy for Spirit — future work only.

## 9. Open Questions

1. **Q1 — Spirit vs "immune to Physical Impact".** RFC-003 §3.1 settled this: Spirit is 0‰ on all
   four physical channels, counterweighted by ×1.3 energy and a 250‰ Explosion cell. This RFC
   accepts that reading (its chip floor applies only to post-matrix damage, so a pure-physical
   loadout genuinely cannot harm a Spirit). Residual question for the tone/content owners: should
   Spirit enemies be gated to rings/instances where element access is guaranteed, so no player
   meets one with a kit that cannot touch it?
2. **Q2 — Calibration anchor.** *Resolved by RECONCILIATION.md ruling 4:* `kIceBoltPower = 600`
   honors RFC-002 §11's "one cast = T2, Freeze in two on baseline (Medium Flesh) targets" — the P2
   two-cast-freeze contract, closing RFC-002 Open Question 6. Residual playtest tuning (whether a
   Small Slime-material target should take two bolts rather than freeze on the first, §4.6) drops
   the anchor toward T1 — which RFC-002 OQ6 says must be renegotiated with RFC-002, not silently
   changed here.
3. **Q3 — RL observation width.** The 8-float proposal (§6) drops per-channel target meters and
   terminal bits. If Samurai training shows the policy needs the full target meter array, RFC-007
   must bump `kObsVersion` — flagged so the cost lands in the right RFC.
4. **Q4 — Player-side Stagger in deep rings.** Full-rate Stagger on players in rings 2+ is
   opt-in-difficulty by this RFC's reading of GAME.md §0 — confirm the tone owner agrees
   knockdown-class CC on players is acceptable *anywhere*, or extend `kHomeRingStunGain`
   world-wide and reserve Knockdown for instances.
5. **Q5 — Terminal-burst self-synergy.** Authored burst packets that fire on a terminal (RFC-005
   patterns in the Combust style) deal elemental damage that could re-feed the same channel if
   authored carelessly; proposal is that terminal-burst packets carry no build-up riders (rule for
   RFC-005/008 authors). Should that be promoted from authoring rule to engine invariant?
6. **Q6 — Chip floor vs swarms.** `kChipDamage=1` × FanVolley × large packs could trivialize
   toughness-heavy content; if so, the fix is capping volley shot counts (RFC-001), not raising
   the floor — flagging so the tuning conversation lands in the right RFC.

## Review Record

Adversarial review 2026-07-23. Reviewer-Opus: **revise**. Reviewer-Sonnet: **revise**.

Applied:
- §4.3/§4.4: deleted RFC-009's own material×damage matrix and the Step-1 multiply; RFC-003 §3.1 owns the matrix (applied before this pipeline), Spirit=0‰ physical accepted as settled (Q1).
- §4.1/§4.2: channel taxonomy re-based on RFC-003 §2 (Damage/Pierce/Crush/Explosion/Heat/Cold/Electric; no Rock channel — Rock school = Crush+Impulse+Explosion).
- §4.5: gauge model replaced by RFC-002's meters ([0,1000], T=300/600/900, Cold/Heat/Shock/Earth/Stagger, one-primary rule, two-level soft-resist ≥250‰); Earth is Rock's ladder (Encumbered/Mired/Root); Poison deferred (RFC-002 OQ2 — no v1 source); RFC-009 keeps only gain formula, affinity values, derived Stagger, calibration.
- §4.6: tier intake/mass quoted from RFC-003 §4 (floor 150‰ inherited); duration scaling deferred to RFC-002 §6; examples aligned (Tengu phase 2 = Titan).
- Terminal lock: Gauge struct dropped; RFC-002 `StatusState` (`stage_ticks`) makes terminals representable and LOD-steppable, restoring I7.
- §4.4/§4.8: M_outer composition made normative (three contributors, chained per-mille floors, absent = 1000); pseudocode fixed to match.
- §4.6 worked example recomputed from the documented single-floor rule (script-verified).
- Derived Stagger mitigated once via the Stagger affinity column, not the Crush damage cell.
- §6: RL observation resized to RFC-007's 8 reserved indices; anything wider requires an obs-version bump (Q3).

Unresolved: none outstanding from this file's own review. (RFC-002 §6 already attributes the
intake numbers to RFC-003 §4, quoted here — that bullet was stale and is resolved; RFC-007's
status indices were re-keyed to the canonical set per RECONCILIATION.md ruling 1.)

Reconciliation: §4.5/§4.6 converted from the half-applied `{Frost/Heat/Shock/Poison/Stun}` on 0..255 (T=100/170/230, kIceBoltPower=170) to the canonical `{Cold/Heat/Shock/Earth/Stagger}` on [0,1000] (T=300/600/900, kIceBoltPower=600); Earth restored as Rock's ladder and Poison deferred (ruling 1); worked example fully recomputed and script-verified (ruling 2); calibration = 600, two-cast-freeze preserved (ruling 4); I5b referenced as RFC-002 §6's, not restated (ruling 3) — per RECONCILIATION.md rulings 1, 2, 3, 4. §4.6's tier-membership echo fixed (Squids moved Giant→Large to match RFC-003 §4's normative table) and the stale I5 floor (150‰) and RFC-002 §6 terminal-duration paraphrase (×0.75/×0.5) corrected to the current authoritative numbers (I5 ≥ 250‰; `kTierTerminalDur` 800/600/450‰) — per RECONCILIATION.md ruling 7.

Reconciliation: §4.5 gain-formula comment's ownership label corrected — `kTierGain` is this RFC's own §4.6 table (RFC-003 §4 echoes it, not the reverse), matching RFC-003 §4's own "RFC-009 §4.6 solely owns build-up tier scaling" statement; per RECONCILIATION.md ruling 14.
