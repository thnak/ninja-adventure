# RFC-003: Physics & Material Interaction

> Status: Accepted (revised after review)
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §6, §7, §8, §10, §11
> Depends on: RFC-001 (payload producer), RFC-002 (status tags & combo detonation, §7 there), RFC-004 (scar layer / entity substrate), RFC-009 (damage & build-up consumer), RFC-010 (tile patches, fire spread, LOD)
> Consumed by: RFC-005, RFC-006, RFC-007, RFC-008, RFC-010

---

## Summary

This RFC defines the **physical layer** of the unified combat system: the eight-channel
**attack payload** every hit carries (Damage / Impulse / Heat / Cold / Electric / Explosion /
Pierce / Crush), the four **terrain physical properties** every tile exposes (Friction / Grip /
Conductivity / Stability), the eight-value **material system** (Flesh / Stone / Spirit / Metal /
Wood / Plant / Water / Slime), **mass** and the `Knockback = Impulse / Mass` law, the six
**scale tiers** (Tiny … Titan) and the modifiers they apply automatically, and the **interaction
rule engine** from which cross-element effects (Water + Lightning, Fire + Plant, Ice + Shockwave,
…) emerge without a single per-skill special case.

The design contract in one sentence: **a skill declares only what it emits; what happens next is
decided entirely by the payload numbers, the target's material, the terrain's properties, and one
shared ordered rule table.** Two engineers implementing this spec independently must converge on
the same integers for the same inputs; everything here is specified in fixed-point integer math
for that reason.

---

## Motivation

Three forces demand a physical layer that is data, not code:

1. **The P2 combo system is the hard-coded ancestor this replaces.** Today
   `combo_of(status, heavy, by_projectile, by_element)` in `src/world/tiles.hpp` encodes exactly
   five interactions as five `if` statements. Every new boss ability, every new combat entity
   (RFC-004), and every terrain hazard would each multiply that function's arms. The umbrella spec
   (§7) forbids this: "No hard-coding per skill. Interactions emerge from rules." The migration
   is split by scope: the five combos move to RFC-002 §7's ladder-aware detonation table (their
   `tiles.hpp` numbers preserved there), while everything `combo_of()` could never express —
   terrain, materials, structures — becomes ordinary rows in the general rule table (§8).

2. **RL bosses need a mechanically consistent world.** A policy trained per archetype
   (10–15 policies total, ARCHITECTURE.md §7) generalises only if the same action has the same
   physical consequence everywhere: a charge into a mud tile must slow the boss by the same rule
   that slows the player. One rule table means one world model to learn.

3. **Bosses and players must be balanced by physics, not by flags.** "A big boss is hard to
   shove, but still receives the force" (umbrella §11) and "large bosses accumulate more slowly,
   but are never immune" (§9) are both statements about mass and scale, not about immunity bits.
   Making mass and scale first-class removes an entire category of per-boss tuning knobs.

What this RFC deliberately does **not** do: compute hit points lost (RFC-009), define what a
status *does* over time (RFC-002), or implement terrain conversion (RFC-004). It defines the
physical quantities those systems consume and the rules that connect them.

---

## Guide-level Explanation

### For a designer authoring a skill

You never write an interaction. You fill in up to eight numbers on the payload of each hit your
skill produces (in the RFC-008 skill file), pick nothing else, and the world does the rest:

```yaml
# A boss's "Boulder Toss" impact payload (RFC-008 syntax is illustrative)
payload:
  damage: 120
  crush: 180
  impulse: 220
  explosion: 60
```

- Against a **player** (Flesh, Medium scale, mass 100): full damage, a stagger contribution from
  Crush, and `220 impulse / 100 mass = 2.2 tiles` of knockback — more on ice, almost none in mud.
- Against an **Ice Pillar** combat entity (Water material, Cold gauge at the Freeze terminal):
  the Crush channel trips RFC-002 §7's **Shatter** — combat entities carry the same gauges as
  creatures (RFC-004), so the pillar breaks in one hit with no rule written here.
- Against the **Giant Samurai** (Flesh, Giant scale, mass 700): identical numbers, but
  `220 / 700 = 0.3 tiles` — it visibly rocks back yet holds its ground. No "knockback immune"
  flag exists anywhere.
- Landing **next to a water pool** while an Electric zone is active: nothing, because Boulder
  Toss carries no Electric. Channels only interact when they are present.

### For a player

The player is never shown a matrix. They experience consistent folk physics:

- **Mud swallows shoves but the hit hurts more; ice sends everyone skating.** (Terrain
  Friction/Grip, §5/§6.)
- **Lightning loves water.** Shock anything wet or standing in water and it chains. (Conductivity;
  the standing-in-water extension of RFC-002's Conduct, §8.)
- **Fire eats plants.** A flame that touches grass or a vine monster spreads — a few tiles, then
  it burns out. (Flammability here; the spread itself is RFC-010's bounded, claim-fireproof rule.)
- **Frozen things are brittle.** A heavy blow shatters a frozen monster *or* a frozen wall (both
  via RFC-002 §7's Shatter on their gauges) *or* a frozen lake edge (one rule here, §8 row 4) —
  three payoffs, one law.
- **Big things budge less but are never unstoppable.** Enough Impulse moves a Titan an inch, and
  an inch on an ice sheet is a metre.

### The chill guardrail, honored here

Physics never starts anything on its own. Every rule in this RFC fires only in response to a
payload — and payloads exist only where combat is already happening. Fire ignited by a payload
becomes RFC-010 tile patches whose spread is budgeted, claim-fireproof, and self-extinguishing
(RFC-010 §4.2/§4.6); electricity chains once and is gone; no hazard seeks a player, persists past
its fixed lifetime, or ticks meaningfully in a sleeping chunk. A player who only ever waters crops will never observe this system acting on
them (GAME.md §0).

---

## Reference-level Design

All quantities are **integers** (fixed-point where fractional precision is needed) so that every
node — GCC/Linux and MSVC/Windows, per the P1 determinism standard (ARCHITECTURE.md §2c) —
computes identical results. Ticks are the simulation tick, **10 Hz** (established in P2:
`kAngerTicks = 200` = 20 s).

### 1. Units and conventions

| Quantity | Type | Unit | Range |
|---|---|---|---|
| Channel magnitude | `std::uint16_t` | "points"; 100 pts ≈ one standard light melee hit worth of that channel | 0–1000 |
| Transmission coefficient | `std::uint16_t` | per-mille (‰); 1000 = pass-through, 2500 = ×2.5, 0 = immune | 0–4000 |
| Mass | `std::uint16_t` | mass points; Medium creature = 100 | 10–4000 |
| Terrain property | `std::uint8_t` | 0–100 scale | 0–100 |
| Knockback distance | fixed-point `std::int32_t`, 1/256 tile | tiles | capped, §5 |

Division is integer division with explicit rounding stated per formula. Where a formula could
overflow `uint16`, intermediates are `int32`.

**The 0–1000 channel range is the authoring range, not a runtime invariant.** RFC-008 validates
skill data against it; at runtime, producer scaling (§2.1), rule responses (§8), and WallSlam
synthesis (§5) legitimately push channel values past 1000 in `int32` intermediates. There is
exactly **one clamp point**: a channel value is clamped to `[0, kChannelCeiling = 4000]` (tunable)
at the moment it is consumed — entering RFC-009's Step 1 for damage channels, entering the §5
impulse law for Impulse. No other clamp exists anywhere in the pipeline; an implementation that
clamps earlier (or never) diverges from this spec and is wrong.

### 2. The attack payload

Every hit that reaches the resolution pipeline — melee swing, arrow, spell impact, zone tick,
hazard contact, collision — carries one `AttackPayload`. RFC-001 defines *when and where* payloads
are produced (pipeline phases Cast→…→Impact); RFC-008 defines how they are written in skill data.
This RFC defines what the fields *mean* and how they resolve.

```cpp
struct AttackPayload {
    // The eight channels, in points (0-1000). Zero = channel absent.
    std::uint16_t damage    = 0;  // untyped physical harm — the baseline "a weapon hit you"
    std::uint16_t pierce    = 0;  // sharp, armour-defeating physical (arrows, spikes, blades)
    std::uint16_t crush     = 0;  // blunt, structure-breaking physical; feeds stagger build-up
    std::uint16_t impulse   = 0;  // momentum only; NEVER harms by itself — displaces (§5)
    std::uint16_t heat      = 0;  // thermal energy; Fire school (BookFire)
    std::uint16_t cold      = 0;  // thermal drain; Ice school (BookIce)
    std::uint16_t electric  = 0;  // charge; Thunder school (BookThunder)
    std::uint16_t explosion = 0;  // radial pressure; expands at impact (§2.2)

    // Geometry, filled by the delivery system (RFC-001), not by skill data.
    std::uint8_t  dir;            // impact direction, 16 steps of 22.5° (for impulse vector)
    std::uint16_t source_id;      // attacker entity id (attribution: XP, aggro, RL reward)
    std::uint8_t  team;           // RFC-004 team, for friendly-fire policy
};
```

**Semantics per channel:**

| Channel | Resolved by | Physical effect (this RFC) | Downstream (other RFCs) |
|---|---|---|---|
| Damage | RFC-009 | — | HP loss after armour |
| Pierce | RFC-009 | — | HP loss, ignores a fraction of armour (RFC-009 owns the fraction) |
| Crush | this + RFC-009 | terrain/entity stress (§7), shatter rules | HP loss + Stagger build-up |
| Impulse | this | knockback (§5), wall-slam, force-transfer (§6) | none — impulse never reaches the HP path directly |
| Heat | this + RFC-009 | ignition, melt/dry surface rules | fire damage + Burn build-up |
| Cold | this + RFC-009 | freeze surface rules, extinguish | cold damage + Freeze build-up |
| Electric | this + RFC-009 | conduction/propagation rules | shock damage + Shock build-up |
| Explosion | this + RFC-009 | geometric expansion (§2.2), radial impulse, terrain stress | HP loss as the Blast channel (crosswalk below) + Stagger weight |

**The four elements map onto channels with no fifth "element channel".** Fire→Heat, Ice→Cold,
Thunder→Electric, and **Rock is not an energy type at all**: Rock-school skills (code:
`Element::kEarth`) emit Crush + Impulse + Explosion. This keeps the asset constraint of exactly
four skill-book elements (BookFire/BookIce/BookRock/BookThunder) while needing zero new icon
families. Plant/Water/Light/Darkness/Wind/Death remain icons only and are out of scope for v1;
a future school would claim either a new channel or a new combination of existing ones
(future work, not specced here).

**Channel crosswalk (normative; shared with RFC-009 §4.1).** This payload and RFC-009's
`DamagePacket` are one wire object seen by two layers. Field for field: `damage` → `amount[0]`
(**Slash** — untyped weapon harm rides the Slash column; it has no other home), `pierce` →
`amount[1]`, `crush` → `amount[2]`, `explosion` → `amount[3]` (**Blast**), `heat` → `amount[4]`
(Fire), `cold` → `amount[5]` (Ice), `electric` → `amount[7]` (Thunder), and `impulse` → the
packet's separate `impulse` field (never a damage channel — RFC-009 §4.1 states the same).
`amount[6]` (Rock) is not synthesized by this RFC: rock-school skills may author it directly
(RFC-008) and it mitigates the Earth ladder (RFC-009 §4.5); the Crush+Impulse+Explosion guidance
above is additive to that column, not a replacement for it.

#### 2.1 Producer-side scaling

The payload written in skill data is the payload of a **Medium-scale** caster. At emission the
delivery system multiplies `impulse`, `crush`, and `explosion` by the caster's
`scale_impulse_out` (§4) — a Giant's shove carries Giant momentum without boss-by-boss numbers.
`damage`/`pierce`/`heat`/`cold`/`electric` are **not** auto-scaled; offensive power stays an
explicit stat (RFC-009), so a big boss is not automatically a hard-hitting one.

#### 2.2 Explosion expansion

Explosion is the only self-expanding channel, and it expands **geometrically, not
typologically**: at the impact tile, an `explosion = E` payload fans out into per-tile
sub-payloads over a disc, but the damaging value stays on the Explosion channel:

```
radius_tiles = clamp(1 + E / 150, 1, 4)                     (tunable)
falloff(d)   = (radius - d) / radius                        d = Chebyshev tile distance
per-tile:    explosion = E * falloff(d)      // stays ON the Explosion channel
             impulse  += E * 8/10 * falloff(d), dir = radial from centre   (tunable)
             stress   += E * falloff(d)      → terrain stress pool, §7
```

The channel identity is preserved on purpose: RFC-009 reads Explosion as **Blast** (§4.1 there),
so its Blast resistance column and Blast's weight in Stagger derivation (§4.5 there) stay live.
An earlier draft converted Explosion into Damage+Crush at expansion, which would have zeroed
RFC-009's Blast column on every routed hit; that conversion is deleted. Expansion happens
*before* per-target resolution — it decides which tiles and targets resolve, never what type
they resolve as.

The expansion happens once, at impact, inside the owning chunk's tick — no pressure wave is
simulated over time. Tiles in a neighbouring chunk receive their sub-payload via the existing
cross-chunk message path (same mechanism as arrow migration, P2); a lost message loses that rim
of the blast, which is acceptable soft state.

### 3. Materials

Every combat-relevant thing — creature, player, combat entity (RFC-004), destructible prop —
carries exactly one `Material` (4 bits of replicated state):

```cpp
enum class Material : std::uint8_t {
    kFlesh = 0,   // players, all 66 monsters, wildlife, bosses unless stated
    kStone = 1,   // boulders, spikes, statues, GiantBamboo's husk? (no — see assignment table)
    kSpirit = 2,  // ghosts/spirits; walk-only Spirit-family monsters
    kMetal = 3,   // armoured constructs, metal totems, portcullis-type entities
    kWood = 4,    // fences, siege props, trees as combat entities, totems
    kPlant = 5,   // vine monsters, crop-adjacent entities, brush
    kWater = 6,   // water elementals, rain pools-as-entities, Squid-family (aquatic)
    kSlime = 7,   // slimes, GiantSlime-family, gel walls
};
```

Default assignment (data, RFC-008; defaults here so independent implementations agree):
players and the 89 character rigs → Flesh; the 66 walk-only monsters → per-family, defaulting to
Flesh, with Ghost/Spirit sheets → Spirit, Slime sheets → Slime; RL boss shortlist: Samurai,
GiantFrog, GiantRacoon, Tengu, DemonCyclop → Flesh; GiantBamboo → Plant; Squids → Water.
Terrain **tiles** do not have a Material — they have the four physical properties (§6); combat
entities standing on tiles do (RFC-004 stores it).

#### 3.1 Material transmission — one owner per number

**Damage-channel material scaling has exactly one owner: RFC-009.** Its `kMaterialMult[8][8]`
(§4.3 there) is applied exactly once, in RFC-009's Step 1. This RFC pre-scales **nothing** on the
damage channels — an earlier draft carried a second, damage-facing matrix here, which both
double-applied material on every routed hit and disagreed with RFC-009 cell-for-cell; it is
deleted. Rule predicates in §8 therefore test **raw** payload points, and RFC-009 receives raw
channels (plus any Pass-A additions) under the §2 crosswalk.

What this RFC does own is the one channel RFC-009 explicitly hands over (its §4.1: "Impulse is
not a damage channel"): per-material **impulse transmission**, per-mille. All values (tunable):

| Material | Impulse ‰ | Reading |
|---|---|---|
| **Flesh** | 1000 | baseline |
| **Stone** | 400 | planted, hard to move |
| **Spirit** | — | **impulse-immune**: mass treated as infinite, per RFC-009 §4.3/I4 — the division yields zero displacement |
| **Metal** | 800 | heavy but rigid |
| **Wood** | 900 | |
| **Plant** | 1000 | |
| **Water** | 600 | gives way around the blow |
| **Slime** | 1400 | shoves send gel bouncing |

Reading rules this split preserves (a re-tune must keep them):

- **Spirit's physical *damage* is 150‰, never 0** (RFC-009 Invariant I4: no 0‰ cell exists) — a
  melee-committed player still kills any Spirit creature by attrition, honoring "no absolute
  immunities" (umbrella §9). Spirit's one true immunity is **displacement only**, above; its
  counterweight is ×1.3 from all four elements (RFC-009 §4.3). The umbrella's "vulnerable to
  Arcane/Holy" stays future work (those schools are icons only).
- Every other "what am I made of" damage number — Stone shrugging arrows and cracking under
  hammers, Metal's Thunder liability (plus the arc rule, §8 row 5), Slime's arrow-swallowing —
  lives in RFC-009 §4.3, the single source; this RFC cites those cells and never restates them.
- Impulse coefficients are per-material, never per-creature; a creature that needs to differ
  does it with RFC-009 stats.

#### 3.2 Material tags

Three derived boolean tags used by the rule engine, fixed per material (not data):

| Tag | Materials | Used by |
|---|---|---|
| `Flammable` | Wood (ignition ≥ 60 Heat), Plant (≥ 30 Heat) (tunable) | ignition rules |
| `Conductive` | Metal, Water | arc/chain rules |
| `Incorporeal` | Spirit | passes through Wall-type combat entities (RFC-004 consumes) |

One **terrain-side** flammability set completes the tag: the flammable *baseline terrain* set is
`{Grass, forest floor}` (tunable) — consumed by rule row 1 (§8) and by RFC-010 §4.2's fire-spread
rule ("flammable baseline" there is this set). Tilled soil (`kDirt`) is deliberately **not** in
the set: a farm plot cannot ignite even before the claim guardrail (§8 row 1) is consulted.
Terrain ignition threshold: `kTerrainIgnition = 30` Heat (tunable, same as Plant).

### 4. Mass and scale tiers

Every combat actor has a **scale tier** (3 bits, replicated; assigned per archetype in RFC-008
data). Tier ownership is split with RFC-009, stated here to kill a former duplication:
**RFC-009 §4.6 solely owns build-up tier scaling** — `kTierGain` (1600/1300/1000/600/350/200 ‰,
Tiny→Titan), terminal-duration scaling, tier toughness, and the invariant floor **I5:
gain ≥ 250‰** (a Titan needs ~3× the Power to freeze, never infinite; the "never immune" floor
lives in that one place). An earlier draft carried its own intake column with different numbers
and a different floor; it is deleted. **This RFC solely owns the physics quantities**: mass and
`scale_impulse_out`. Mass may be overridden per archetype (±50% max) but tier modifiers may not —
they are what keeps "big" meaning one thing everywhere.

| Tier | Examples | Mass (pts) | Impulse out ×‰ (`scale_impulse_out`) |
|---|---|---|---|
| Tiny | summoned wisps, critters | 25 | 500 |
| Small | slimes, chickens, spiders | 50 | 750 |
| Medium | players, most of the 66 monsters, wolves | 100 | 1000 |
| Large | bears, elite monsters, GiantFrog, Squids | 250 | 1250 |
| Giant | dojo bosses: Samurai, GiantBamboo, GiantRacoon, DemonCyclop, Tengu (both phases) | 700 | 1600 |
| Titan | reserved for future set-pieces | 2000 | 2000 |

All values (tunable). Tier membership follows RFC-009 §4.6 (Squids sit at Large with GiantFrog;
Tengu's two phases share Giant). Two hard invariants:

1. **Mass is normatively defined only here.** Any mass figure elsewhere — including RFC-009
   §4.6's informative "mass class" column — is a derived echo of `mass / 100`
   (0.25/0.5/1/2.5/7/20) and this table wins on conflict.
2. **Knockback has no tier modifier at all.** It needs none: `Impulse / Mass` (§5) already makes
   a Giant 7× harder to shove. Adding a second knob would double-count size; the umbrella's
   "scale automatically adjusts knockback" is satisfied *through* mass.

Tier gain is applied by RFC-009 inside its gain formula (§4.5 there); mass is applied here in §5.
No tier number lives in two documents any more.

### 5. Impulse resolution — knockback and the slam

The one law: **`kb_raw = impulse_effective / mass`**, in tiles (fixed-point 1/256). It runs after
impulse transmission (§3.1; Spirit's infinite-mass treatment means it simply never enters this
section) and after the terrain factor, which — like every multiplier in this document — is
**per-mille**:

```
kb_terrain = (100 - Friction_of(tile_under_target)) * 25        // ‰: 1000 = ×1.0
kb_tiles   = min(kb_raw * kb_terrain / 1000, kKnockbackCap)     // kKnockbackCap = 4 tiles (tunable)
```

At baseline grass (Friction 60, §6) `kb_terrain = 1000‰`: a 220-impulse heavy hit moves a
100-mass player 2.2 tiles. On an ice glaze (Friction 15) 2125‰; in marsh (Friction 95) 125‰ —
the umbrella §6 mud/ice examples are *outputs of the formula*, not rules.

**Knockback state machine** (per entity; at most one active, new knockback replaces remaining):

```
Grounded ──payload with kb_tiles ≥ 0.25──► Displaced { remaining, dir, ticks_left = kKbTicks }
Displaced ── per tick: move remaining/ticks_left along dir, re-check walkability per tile ──►
    ├─ path clear, ticks_left == 0 ──► Grounded
    ├─ next tile impassable (terrain or Wall entity) ──► WallSlam ──► Grounded
    └─ chunk ticking at 1 Hz or asleep ──► apply ALL remaining displacement this tick ──► Grounded
kKbTicks = 3 (tunable)
```

A slide whose next tile lies in a neighbouring chunk hands the entity across the seam carrying
its remaining displacement, via the existing migration message (the P2 arrow path); the receiving
chunk finishes the slide. This is ordinary cross-chunk debt (RFC-010): a lost message drops the
remainder — acceptable soft state.

Displacement below 0.25 tiles (tunable) is a **flinch**: no movement, renderer-only nudge
(RFC-006). This keeps micro-shoves from turning melee into jitter.

**WallSlam** converts the undelivered momentum back into harm, symmetrically:

```
slam_crush = remaining_tiles * mass * 5 / 10        // points (tunable)
```

applied as a Crush-only payload to **both** the displaced entity and, if the obstacle is a
destructible combat entity (RFC-004), to the obstacle. The synthesized value routinely exceeds
1000 points and is clamped only at consumption (§1). Throwing a monster into an Ice Pillar
hurts both; shattering the pillar with someone's body is the emergent reward. Terrain proper
takes the slam as stress (§7), not damage.

**Players and knockback.** Players are subject to the same law — refusing would break the "one
world model" promise to RL — but with `kPlayerKbScale = 500‰` (tunable) applied to incoming
`kb_tiles`, because being flung 4 tiles at 150 ms home-cluster latency reads as rubber-banding.
This scale lives here, once, not per skill.

### 6. Terrain physical properties

Every terrain tile exposes four scalars, as a pure function of `Terrain` (and surface overlay,
RFC-004), keeping the ARCHITECTURE.md "terrain is a pure function" property — no new per-tile
state is introduced by this RFC:

```cpp
struct TerrainPhys { std::uint8_t friction, grip, conductivity, stability; };
[[nodiscard]] constexpr TerrainPhys terrain_phys(Terrain t /*, SurfaceOverlay s — RFC-004 */);
```

**Semantics — the four are distinct on purpose:**

- **Friction** — how fast imposed motion dies. Consumed *only* by the knockback formula (§5).
  Low = slide far.
- **Grip** — how well feet hold. Consumed by (a) locomotion accel/turn scaling (RFC-010 owns
  movement; it reads this value) and (b) the *force-transfer* rule below. High grip = braced.
- **Conductivity** — whether electricity travels through the tile (§8 propagation).
- **Stability** — the stress threshold before the tile converts (crater, rubble, ice crack —
  conversion itself is RFC-004's; the threshold test is §7's).

All values (tunable):

| Terrain | Friction | Grip | Conductivity | Stability | Notes |
|---|---|---|---|---|---|
| Grass | 60 | 70 | 15 | 60 | the baseline; kb_terrain = 1.0 here by construction |
| Dirt (tilled) | 70 | 80 | 25 | 50 | |
| Water | — | — | 100 | — | impassable body; participates only in conduction & surface rules |
| Stone | 55 | 75 | 5 | 90 | hardest to crater |
| Sand | 80 | 55 | 10 | 40 | swallows shoves, poor footing |
| Tree | — | — | 10 | 35 | impassable; as a hit target it is a Wood-material entity (RFC-004) |
| Snow | 75 | 45 | 10 | 55 | |
| Marsh | 95 | 90 | 60 | 30 | the mud of the umbrella examples; damp = mid conductivity |
| Ash | 65 | 65 | 5 | 45 | |
| Path | 55 | 80 | 10 | 70 | |
| Building | — | — | 5 | 100 | never converts; buildings die by RFC-004 entity HP, not stress |
| *kMudded patch (RFC-010)* | =90 | =85 | =60 | −10 | churned / soaked ground; `=` sets, `−`/`+` add (clamped 0–100) |
| *kIced patch (RFC-010)* | =15 | =20 | =20 | =35 | ice glaze; the "RFC-003 ice row" RFC-010 §4.2 cites |
| *kRubbled patch / kRubble–kCrater scar* | =85 | =60 | =5 | =25 | broken ground; kCrater reports this row with Stability =20 (tunable) |
| *kCracked (patch or scar)* | +0 | +0 | +0 | −15 | precursor; the "stability↓" read RFC-004 §8 promises |
| *kScorched (patch or scar)* | +0 | +0 | −10 | +0 | the "dry/hot" read RFC-004 §8 promises |

Surface state is exactly two real vocabularies — RFC-010 §4.2 **tile patches**
(`kBurning/kScorched/kMudded/kIced/kCracked/kRubbled`) and RFC-004 §8 **scars**
(`kCracked/kRubble/kCrater/kScorched`). An earlier draft named bespoke WetFilm/IceSheet overlays
that exist in neither document; they are deleted, and every rule below writes only the real
kinds. The rows above are this RFC's contract for what those states report into `terrain_phys`;
`kBurning` adds no property deltas (its gameplay is Burn build-up, owned by RFC-010's patch
table).

**Force-transfer (the mud rule, generalised).** When terrain suppresses knockback, the
suppressed momentum hurts instead:

```
if kb_terrain < 500:                                     // ‰, same unit as §5
    bonus_crush = impulse_effective * (500 - kb_terrain) * grip / 100 / 1000 * 6/10   (tunable)
```

Marsh (kb_terrain 125‰, grip 90): a 220-impulse hit adds `220 × 375 × 90 / 100 / 1000 × 6/10 ≈
44` Crush points — "less push but more pain". Sand (kb_terrain 500‰, grip 55): zero bonus, it
merely absorbs. **Slip mitigation** is the mirror
rule: when the target's tile grip < 30 (ice), incoming Damage + Crush are scaled ×850‰ (tunable)
— the target gives way instead of taking the blow square, the umbrella's "ice reduces direct
damage".

**Ground-class projection (consumed by RFC-007 Block G).** The RL observation never sees these
four properties raw — RFC-007 collapses ground into a 4-way class one-hot and defers the mapping
here. The normative projection, precedence exactly as listed: **Conductive** if effective
Conductivity ≥ 50; else **Slow** if the tile carries a speed-reducing patch or scar
(kMudded/kRubbled/kRubble/kCrater) or Friction ≥ 80; else **Unstable** if it carries kCracked
(patch or scar); else **Normal**.

### 7. Terrain stress

Explosion expansion (§2.2), WallSlam on terrain (§5), and ground-targeted Crush each deposit
**stress points** on tiles. Stress is transient: it exists only within the resolving tick — there
is no persistent per-tile stress pool to replicate, sleep, or cheat with.

```
if stress_this_tick >= stability * kStressUnit:      // kStressUnit = 3 points (tunable)
    emit ConvertSurface(tile, verb)                  // executed by RFC-004
```

The conversion result is chosen by the owners from (terrain, existing state) — RFC-010 §4.2 for
tile patches, RFC-004 §8 for the scar ladder; this RFC only owes the
threshold test and the guarantee that **one tile converts at most once per tick**. Accumulating
stress across ticks (chipping a stone floor down over a fight) is explicitly rejected in v1: it
would be per-tile state that must replicate and survive LOD sleep for marginal gameplay
(see Open Questions).

### 8. The interaction rule engine

One ordered table, evaluated the same way everywhere. A rule is:

```cpp
struct InteractionRule {
    ChannelMask   trigger_channels;   // fires if any listed channel ≥ its threshold
    std::uint16_t threshold;          // points, tested against the RAW payload (material scaling
                                      // happens exactly once, later, in RFC-009); row 1 substitutes
                                      // §3.2's per-material ignition points (terrain branch:
                                      // kTerrainIgnition = 30, tunable)
    Predicate     subject;            // tag / material / surface predicate on the TARGET or TILE
    Response      response;           // one verb, parameters below
    bool          consumes_tag;       // tag-clearing rules (extinguish/dry) consume the tag (RFC-002 state)
};
```

Response verbs (closed set — adding a verb is an RFC change, adding a *rule* is data):

| Verb | Meaning | Executed by |
|---|---|---|
| `ScaleChannels(mask, ‰)` | multiply channels before RFC-009 | this RFC |
| `AddChannel(ch, pts)` | inject channel points (e.g. shatter bonus) | this RFC |
| `ApplyStatus(tag, potency)` | add status build-up | RFC-002 / RFC-009 |
| `ConvertSurface(verb)` | change tile state | RFC-010 (patches) / RFC-004 (scars) |
| `SpawnPayload(payload, shape)` | secondary explosion / splash | this RFC (recursion budget below) |
| `Propagate(ch, medium, decay‰, max_range)` | chain through a conductive region (fire spread is RFC-010's rule, never this verb) | this RFC |
| `RefundVital(which, pts)` | give the source stamina/mana | RFC-001 economy |

**Where the five combos live — not here.** Shatter, Blast, Conduct, Crush and Arc are owned
end-to-end by **RFC-002 §7** (`status_detonate`): detection, requirements, consumption, and the
numbers (×2.5 armour-ignoring Shatter, ×1.6 Blast, ×1.4 Conduct, ×1.3 Crush with Stagger build-up
+800, ×1.1 Arc with 10 mana refunded — the `tiles.hpp` values, preserved there). Their damage
scales reach the pipeline through RFC-009's `M_outer` slot (§4.4/§4.8 there). This table holds
**no combo rows and applies none of those multipliers** — an earlier draft duplicated them here,
which would have applied Shatter's ×2.5 twice; those rows are deleted. This RFC contributes
exactly two physics extensions to the combo system:

- **Standing in water counts as Wet.** An entity on a tile with effective Conductivity ≥ 50
  satisfies Conduct's Wet-coating requirement. The test extension is this RFC's; the values and
  3-tile chain stay RFC-002 §7's, and cross-chunk fan-out stays RFC-010's.
- **Frozen ground shatters** (row 4 below). Frozen *bodies* — creatures and CombatEntities
  alike — need nothing here: they carry gauges (RFC-004) and detonate through RFC-002 §7.

**The v1 rule table.** Ordered; first match per (channel, target) pair wins within a pass.
All thresholds/parameters (tunable):

| # | Trigger | Subject | Response | Notes |
|---|---|---|---|---|
| 1 | Heat ≥ ignition (§3.2) | target material `Flammable`, or flammable baseline tile (§3.2 set) with no patch — **never** a tile inside a player claim or village wall ring (per-tile query: RFC-010 §4.6 rule 3, the normative owner) and never `kDirt` (outside the set by construction) | `ApplyStatus(Burning, heat)` / `ConvertSurface(→ kBurning patch)` | **ignition only** — all ongoing spread is RFC-010 §4.2's budgeted, claim-fireproof, self-extinguishing rule; this table never propagates fire |
| 2 | Heat ≥ 30 | tile has kIced patch | `ConvertSurface(remove kIced)`, steam FX — matches RFC-010's trigger table (kIced → removed) | melting |
| 3 | Cold ≥ 40 | tile is Water edge or has kMudded patch | `ConvertSurface(→ kIced patch)` | winter synergy (GAME.md §9); never makes open water walkable (RFC-010 invariant P-2) |
| 4 | Crush ≥ 40 | tile has kIced patch | `ScaleChannels(stress, 2500‰)` on this tile's stress deposit; `ConvertSurface(remove kIced)` | **frozen ground shatters** — the lake-edge payoff; frozen bodies detonate via RFC-002 §7 instead |
| 5 | Electric ≥ 30 | target material Metal | `Propagate(Electric, nearest Metal entity ≤ 2 tiles, 800‰, 1 hop)` | metal arcs |
| 6 | Cold ≥ 20 | target has `Burning` tag OR tile has kBurning patch | remove tag / `ConvertSurface(extinguish)`; consumes | counterplay both ways |
| 7 | Heat ≥ 30 | target has `Wet` tag | remove tag (steam FX, RFC-006); consumes | drying |
| 8 | Explosion expanded | tile stress test (§7) | `ConvertSurface(...)` — patch or scar per §6/§7 owners | craters/rubble |

With the combos re-homed to RFC-002 §7, this table holds only what neither a formula nor an
entity-scoped detonation can express. Note what is **absent**: "Ice + Shockwave =
slide farther" and "mud absorbs shoves" are *not rules* — they emerge from §5/§6 formulas.

**Evaluation algorithm** (normative — this is where two implementations could diverge, so it is
pinned):

```
resolve(payload, target):
  1. Impulse transmission — impulse = impulse * impulse_‰[target.material] / 1000
               (§3.1); Spirit: impulse := 0 (infinite mass). Damage channels pass
               through UNSCALED — material scaling happens exactly once, inside
               RFC-009 (its Step 1).
  2. Pass A  — struck-target rules (rows 5,6,7), in table order, evaluated once
               against the struck target (row 6's tile clause reads the impact
               tile); each rule fires at most once per resolution; consumed tags
               are gone before later rows test them.
  3. RFC-002/RFC-009 — combo detection (RFC-002 §7, with this RFC's standing-in-
               water Wet extension) -> M_outer; then damage, resistances, build-up
               (RFC-009 §4.4–§4.6, which applies material and tier scaling itself).
  4. Impulse — §5 knockback (uses post-step-1 impulse).
  5. Pass B  — tile-scoped rules (rows 1,2,3,4,8) on the impact tile and, for
               expanded explosions, each affected tile in row-major tile order.
               Pass B iterates TILES: a target-clause binds to the struck target
               on the impact tile only and evaluates false on rim tiles; when a
               response touches occupants, entities on the tile are enumerated in
               entity-id order.
  6. Propagation queue — Propagate/SpawnPayload responses enqueue; the queue drains
               with a budget of kChainBudget = 16 rule firings per chunk per tick
               (tunable); the remainder carries to the next tick FIFO. Each
               propagation step re-enters resolve() with the decayed payload;
               SpawnPayload recursion depth is capped at 2 (tunable). A payload
               triggers at most ONE Propagate per channel per resolution, across
               both passes — first match wins, later matches are skipped.
```

Determinism: the queue is FIFO, tiles are visited row-major, entities in entity-id order, and a
propagation never revisits a (tile, channel) pair within the same wave (visited set per wave).
The chain budget is what keeps a lightning bolt into a lake a *moment*, not a lag spike — and
what keeps RL episode variance bounded.

### 9. Simulation LOD, sleep, and replication

Per ARCHITECTURE.md §4, chunks tick at 10 Hz / 1 Hz / asleep. This RFC's contract:

- **Everything here is impulse-response, not integration.** Payload resolution completes within
  the tick it lands; the only multi-tick constructs are the 3-tick knockback slide and the
  propagation queue, and both **collapse**: a 1 Hz or waking chunk applies remaining knockback
  displacement in one step (§5 state machine) and drains its propagation queue with a single
  enlarged budget of `4 × kChainBudget` (tunable) before resuming.
- **No new persistent state.** New replicated fields per entity: material (4 bits), scale tier
  (3 bits) — both static per archetype, so they compress to the archetype id in practice.
  Per-tile: nothing (stress is intra-tick; patches and scars are RFC-010 §4.2 / RFC-004 §8
  state, both already specced to survive LOD — deadline decay chains and lazy heal). A sleeping
  chunk stores zero physics state.
- **Combat in unwatched chunks.** Payloads only exist where creatures fight (village raids,
  faction skirmishes). At 1 Hz the whole §8 pipeline runs identically, just less often — rules
  are per-payload, not per-second, so outcomes stay fair; only fire *spread pacing* slows,
  which is invisible by definition (no player nearby).
- **Replication cost.** A resolved hit replicates as it does today (position/HP deltas) plus a
  1-byte FX id (RFC-006). The rule engine runs on the chunk's owning node only; results, not
  rule evaluations, are what leave the node — same trust shape as all P2 combat.

### 10. Worked example — Meteor (umbrella §5), end to end

RFC-001 delivers the falling-rock Impact with
`{damage 120, crush 180, impulse 220, explosion 200, heat 40}`:

1. Expansion (§2.2): radius `1 + 200/150 = 2` tiles; the centre tile keeps the listed channels
   (explosion 200 stays on the Explosion channel) plus `impulse +160 radial, stress 200`; ring
   tiles get the falloff share, still as Explosion.
2. A Medium Flesh monster at the centre: step 1 impulse transmission ×1000‰ (Flesh); Pass A: no
   tags — nothing fires. RFC-009 receives raw `{Slash 120, Crush 180, Blast 200, Fire 40}` via
   the §2 crosswalk, scales it once through its own matrix, and derives Stagger from Crush+Blast.
   Impulse `380/100 = 3.8` tiles radial on grass (1000‰) → capped slide; it slams a Stone spike
   after 2.1 tiles → `1.7 × 100 × 0.5 = 85` Crush to both; the spike's Stone row (Crush 1200‰,
   RFC-009 §4.3) makes that `102` — spikes die to meteors.
3. Centre tile: stress 200 ≥ grass stability 60×3=180 → `ConvertSurface` → RFC-004 §8 stamps the
   scar ladder (cracked, escalating toward rubble/crater on repeats); Heat 40 ≥ kTerrainIgnition
   30 → row 1 lays kBurning patches on the rim; RFC-010's spread rule (budgeted, claim-fireproof)
   takes it from there and burns out.
4. Nothing in steps 1–3 mentioned "Meteor". A different skill with the same payload behaves
   identically — that is the whole point.

---

## Interactions with Other RFCs

| RFC | Direction | Contract |
|---|---|---|
| **RFC-001** | in | The ability pipeline produces `AttackPayload`s at Impact (and per zone tick); geometry fields are producer-filled. Payload channel list is fixed by this RFC; RFC-001 never adds interaction logic. `RefundVital` verb is executed by RFC-001's resource economy. |
| **RFC-002** | both | Status states (`Wet`, `Burning`, `Frozen`, `Mired`, `Shocked`, …) are RFC-002 state; this RFC's rules test and consume them via `ApplyStatus`/remove verbs only. The five combo detonations — detection, numbers, consumption — are RFC-002 §7's alone (reaching damage via RFC-009 `M_outer`); this RFC only extends the Wet test (standing in water, §8) and shatters frozen *ground* (§8 row 4). The P2 one-status-per-creature rule and any relaxation of it are RFC-002's call. |
| **RFC-004** | both | Combat entities carry `Material`+scale+mass (and gauges, which is why frozen bodies detonate via RFC-002, not here); the scar layer (§8 there) executes `ConvertSurface` scar verbs, honoring the property columns in §6. Tile patches are RFC-010's (below). Wall-type entities are knockback obstacles (§5). |
| **RFC-005** | out | Boss authors write payload numbers only; §2.1's `scale_impulse_out` gives Giant hits weight for free. No boss-specific interaction hooks exist. |
| **RFC-006** | out | Every verb that fires emits exactly one FX id (tint/overlay per the asset rules); flinch vs slide threshold (§5) is the renderer contract. Reaction FX must fit `effect_life_of` budgets. |
| **RFC-007** | out | This RFC mandates **no new observation fields**. Material, tier and mass are archetype constants and stay out of the obs vector (RFC-007 Block S exclusion — constant per policy, zero information); terrain reaches the policy only as the §6 ground-class projection (Block G, whose mapping this RFC owns); no knockback-in-progress bit exists in v1 — slides observe as position deltas (Block T). Layout is RFC-007's. |
| **RFC-008** | out | Skill files carry the eight channel numbers, archetype files carry material/tier/mass override; the rule table itself ships as engine data with the same schema treatment. |
| **RFC-009** | out | Receives **raw** channels (this RFC pre-scales nothing — `kMaterialMult` applies exactly once, in RFC-009 Step 1) plus any Pass-A additions, under the §2 crosswalk (Explosion arrives as Blast). Tier gain and the armour-ignore flag (RFC-002's Shatter, via `M_outer`) are RFC-009/RFC-002's; this RFC contributes the terrain factors RFC-009's `M_outer` slot expects (§6 force-transfer, slip mitigation). Owns armour, resistance, HP, and all build-up ladders (§9 of the umbrella). |
| **RFC-010** | both | Owns the tick loop that calls `resolve()`, movement (consumes Grip), tile patches (§4.2 there — the patch verbs of rows 1–4, 6), the **fire-spread rule and its claim/village fireproofing** (§4.2/§4.6 there — row 1 only ignites), cross-chunk payload delivery and the knockback seam hand-off (§5), and the LOD schedule this RFC's §9 collapses under. |

---

## RL Considerations

- **One world model.** Because bosses and players resolve through the same matrix, formulas, and
  rule table, a policy's learned regularities ("charging a target on ice overshoots", "mud
  cancels my knockback opener") transfer across rooms and archetypes — a precondition for the
  10–15-policies-total budget.
- **Observation.** The §8 pipeline is driven by quantities that are either archetype constants —
  material, tier, mass, deliberately **excluded** from the obs vector (RFC-007 Block S: constant
  for a given policy, zero information) — or already in RFC-007's layout: ground as the 4-way
  class one-hot (Block G) via this RFC's §6 projection, hazards via Blocks R/E. No
  knockback-in-progress bit exists in v1: a slide is visible to the policy as position deltas
  (Block T vx/vy). This RFC asks nothing of the obs vector beyond the §6 projection.
- **Bounded variance.** `kChainBudget`, the propagation visited-set, and knockback caps bound the
  consequence of any single action — important both for replay-buffer reward variance and for
  keeping a 10×7-tile interior room (the RL training space) from becoming un-resettable.
  In-room hazards expire; a training episode reset clears surfaces (RFC-004/RFC-010 state).
- **Room geometry as curriculum.** WallSlam inside 10×7 rooms means positioning near walls is
  strategy the agent can discover; the 4-tile knockback cap guarantees no single hit crosses the
  whole room, so states remain reachable.
- **Action space unchanged.** This RFC adds zero actions; it enriches consequences. The
  `kActionCount` hard-coding hazard (ARCHITECTURE.md §7) is unaffected.
- **Reward attribution.** `source_id` on every payload — including secondary `SpawnPayload`s,
  which inherit it — keeps chain-lightning kills attributed to the caster for both XP and RL
  reward shaping (RFC-007).

---

## Asset & Engine Constraints Honored

- **Walk-only monsters (66 sheets, zero attack frames).** Nothing in this RFC requires a target
  animation: knockback is a position slide, flinch is a renderer nudge, every reaction is an FX
  overlay or tint at the entity/tile position (RFC-006). Shatter/ignite/freeze visuals are the
  tint-and-overlay treatments the umbrella §15 already prescribes.
- **FX lifetime budget.** The audit's `kEffectLife = 6` truncation gap has since been fixed
  in-engine as per-kind `effect_life_of()` (Rock/Earth now runs its full 14 frames). This RFC
  binds to that: every verb's FX declares a per-kind life ≤ 14 ticks (tunable); anything longer
  is a looping *surface* visual owned by RFC-004, not a one-shot effect.
- **No bespoke combo art; Magic/\* FX family and spinning projectiles not yet packed.** The v1
  rule table's eight rows emit only currently-packed FX kinds (`kFire/kIce/kEarth/kShock/
  kBlast/kSmoke/kSlash*`) with tints. Rules that would need unpacked art (a dedicated steam or
  arc sheet) reuse `kSmoke`/`kShock` tinted until RFC-006 packs more.
- **Exactly 4 elements.** Heat/Cold/Electric map to Fire/Ice/Thunder; Rock is the physical
  channel trio — no fifth element invented. Plant/Water/Light/Darkness/Wind/Death appear in this
  document only as future work.
- **Player kit = basic attack + two abilities.** This RFC adds no player actions or slots; it
  changes what existing hits *carry*.
- **RL boss shortlist respected.** Scale-tier examples name only shortlist bosses
  (Samurai/GiantBamboo/Squids/GiantFrog/GiantRacoon/Tengu/DemonCyclop); Dragons and
  GiantSlime/Flam/Spirit sheets are not designed around as agents (Spirit *material* is for the
  ordinary walk-only spirit monsters, which are valid non-RL creatures).
- **1024×1024 seamless world, LOD, leader-trusted replication.** §9: no persistent physics
  state, collapsible multi-tick constructs, per-chunk budgets, results-not-rules replication.
- **Chill tone (GAME.md §0).** Physics is strictly reactive; all propagation is budget- and
  radius-capped and self-extinguishing; nothing counts down off-screen or seeks a player. The
  farm-safety guardrail is **normative in the rule table, not prose**: row 1's predicate excludes
  claim and village tiles (RFC-010 §4.6 rule 3 owns the per-tile query) and `kDirt` sits outside
  the flammable set (§3.2) — a stray fight can never burn a farm the farmer is chilling on.

---

## Open Questions

1. **Water the material vs water the terrain.** Squids and pools both "are water", but one is an
   entity row in the matrix and the other a tile with properties. Is a swimming/aquatic-boss
   interaction (Squid diving into pool tiles) worth unifying these, or is the current split
   (material for bodies, properties+overlay for tiles) sufficient? Current answer: sufficient;
   revisit when an aquatic boss room is authored (RFC-005).
2. **Persistent terrain stress.** §7 rejects cross-tick stress accumulation to avoid per-tile
   replicated state. If playtests want "chip the stone floor over a long fight", the cheapest
   compliant design is stress-as-surface-overlay-ladder (intact → cracked → rubble), which is
   RFC-004 state that already survives LOD. Decide after first boss-room playtest.
3. **Player-vs-player physics.** PvP is off by default (GAME.md §11). When an opt-in arena
   arrives, does `kPlayerKbScale` apply symmetrically, and is Conduct chaining between wet
   players acceptable at 150 ms latency? Deferred with PvP itself.
4. **Slime division.** Classic slime-splitting (mass halving into two Small entities on Crush
   overkill) fits the material system beautifully but is an entity-lifecycle feature —
   RFC-004's call. Flagged here because the mass table already supports it (Small = 50 = half of
   Medium).
5. **Matrix tuning harness.** RFC-009's 8×8 matrix × this rule table is too many interactions to
   tune by feel alone.
   Proposal: extend `mmo_sim` with a scripted payload-vs-material sweep that prints the
   effective-channel table, so re-tunes diff as text (the worldgen determinism-check pattern).
   Needs an owner; not blocking the spec.
6. **Fixed-point audit.** All formulas here are integer, but RFC-009's damage math and RFC-010's
   movement are float today (positions are floats per `tiles.hpp`). The boundary — integers for
   anything cross-checked between nodes, floats for chunk-local combat — follows
   ARCHITECTURE.md §2c, but the exact seam (is `kb_tiles` compared across nodes? answer should
   be no) needs a one-page determinism note when implementation starts.

---

## Non-goals

- **No continuous physics.** No velocities, no rigid bodies, no per-tick force integration, no
  projectile ballistics (projectile motion is RFC-001's travel phase). This is impulse-response
  resolution on a tile grid.
- **No damage formula.** Armour, resistances, HP, crit, and every build-up ladder
  (Cold → Slow → Heavy Slow → Freeze) are RFC-009.
- **No status behaviour.** What `Burning` does per tick is RFC-002; this RFC only reads/writes
  tags.
- **No terrain conversion implementation.** Overlay storage, lifetimes, and the crater/rubble
  art are RFC-004; this RFC emits `ConvertSurface` and defines thresholds.
- **No per-skill interaction hooks, ever.** A skill file may not name a rule, a material, or
  another skill; if a design needs a bespoke interaction, the design is wrong or the rule table
  gains a general row via RFC review.
- **No new elements or schools.** Four elements; other school icons stay unused in v1.
- **No environmental *initiation*.** Weather setting the `Wet` tag (GAME.md §7/§9) is
  RFC-002/RFC-010 territory; physics here never fires without an attack payload.

---

## Review Record

Reviewer-Opus: **revise**. Reviewer-Sonnet: **revise**. All upheld mustFix items applied; status: Accepted (revised after review).

Applied:
- §3.1: deleted this RFC's damage-facing material matrix — RFC-009 §4.3 is sole owner, applied once (fixes double-application and cell conflicts); §3.1 keeps only impulse transmission.
- §3.1: Spirit 0‰ physical removed — 150‰ per RFC-009 I4; Spirit's immunity is displacement-only (infinite mass).
- §4: build-up intake column and 150‰ floor deleted — RFC-009 §4.6 `kTierGain` + I5 ≥ 250‰ is sole owner; mass declared solely owned here; tier examples aligned (Squids Large, Tengu Giant).
- §8: combo rows 1–5 deleted — RFC-002 §7 owns all five combos end-to-end via RFC-009 `M_outer`; kept only the standing-in-water Wet extension and frozen-ground shatter (row 4), restated on RFC-009's Freeze terminal.
- Interactions/RFC-007: dropped the mandated obs fields (material id, raw terrain, knockback bit); §6 now defines the Block G ground-class projection RFC-007 defers to.
- §6/§8: WetFilm/IceSheet vocabulary replaced with RFC-010 §4.2 tile patches and RFC-004 §8 scars; flammable terrain set defined in §3.2; fire spread ceded wholly to RFC-010.
- §2/§2.2: Explosion expansion made geometric-only (value stays on the channel; RFC-009 reads Blast — column and Stagger weight stay live); normative channel crosswalk published (Damage ≙ Slash).
- §8 row 1 + §3.2: farm-safety guardrail made normative in the rule predicate (claim/village exclusion via RFC-010 §4.6 rule 3; `kDirt` outside the flammable set).
- §1: single clamp point declared (`kChannelCeiling = 4000`, at consumption); 0–1000 is the authoring range only.
- §8: Pass B tile-iteration semantics defined (target binds on impact tile only; entity-id order); at most one Propagate per channel per resolution (kills the old rows 3/9 co-fire).
- §5/§6: `kb_terrain` per-mille end to end (marsh worked example ≈ 44 reproduces); knockback chunk-seam hand-off specified; per-material ignition thresholds stated for row 1.

Unresolved: none outstanding from this file's own review. (The "Mass class (RFC-003)" stale-ratio
note below was checked against the current RFC-009 §4.6 text and found already fixed — its "Mass
pts" column prints raw points (25/50/100/250/700/2000), the `mass / 100` echo this table
requires, not the old 0.5/1/1/3/6/12 ratios. What was still wrong was the Squids tier example and
an I5-floor/`kTierTerminalDur` paraphrase, both in RFC-009; see RECONCILIATION.md ruling 7.)

Reconciliation: channel-name touch-ups to the canonical set — "stagger/stun build-up" → "Stagger build-up" (§2 channel table), and the Crush combo restated as "Stagger build-up +800" to match RFC-002 §7 (was "Stun +180"); the P2 status-state list re-keyed Muddy→Mired (Earth stage 2) — per RECONCILIATION.md ruling 1. Tier-example agreement with RFC-009 §4.6 (Squids at Large) reverified per RECONCILIATION.md ruling 7.

Reconciliation: §4's `kTierGain` echo (1400/1150/1000/700/500/350‰) corrected to match RFC-009 §4.6's actual, script-verified table (1600/1300/1000/600/350/200‰) per RECONCILIATION.md ruling 14.
