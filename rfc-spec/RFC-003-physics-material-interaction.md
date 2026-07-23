# RFC-003: Physics & Material Interaction

> Status: Draft
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §6, §7, §8, §10, §11
> Depends on: RFC-001 (payload producer), RFC-002 (status tags), RFC-004 (terrain/entity substrate), RFC-009 (damage & build-up consumer)
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
   (§7) forbids this: "No hard-coding per skill. Interactions emerge from rules." This RFC shows
   the migration: all five existing combos fall out of the general rule table (§R10 below) as
   ordinary rows.

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
- Against an **Ice Pillar** combat entity (Water material, Frozen tag): the Crush channel trips
  the *brittle shatter* rule and the pillar breaks in one hit.
- Against the **Giant Samurai** (Flesh, Giant scale, mass 700): identical numbers, but
  `220 / 700 = 0.3 tiles` — it visibly rocks back yet holds its ground. No "knockback immune"
  flag exists anywhere.
- Landing **next to a water pool** while an Electric zone is active: nothing, because Boulder
  Toss carries no Electric. Channels only interact when they are present.

### For a player

The player is never shown a matrix. They experience consistent folk physics:

- **Mud swallows shoves but the hit hurts more; ice sends everyone skating.** (Terrain
  Friction/Grip, §R6.)
- **Lightning loves water.** Shock anything wet or standing in water and it chains. (Conductivity,
  rule table.)
- **Fire eats plants.** A flame that touches grass or a vine monster spreads — a few tiles, then
  it burns out. (Flammability, spread caps.)
- **Frozen things are brittle.** A heavy blow shatters a frozen monster *or* a frozen wall *or* a
  frozen lake edge — one rule, three payoffs.
- **Big things budge less but are never unstoppable.** Enough Impulse moves a Titan an inch, and
  an inch on an ice sheet is a metre.

### The chill guardrail, honored here

Physics never starts anything on its own. Every rule in this RFC fires only in response to a
payload — and payloads exist only where combat is already happening. Fire spreads at most
`kFireSpreadRadius` tiles from its ignition point and always burns out; electricity chains once
and is gone; no hazard seeks a player, persists past its fixed lifetime, or ticks meaningfully in
a sleeping chunk. A player who only ever waters crops will never observe this system acting on
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
| Crush | this + RFC-009 | terrain/entity stress (§7), shatter rules | HP loss + stagger/stun build-up |
| Impulse | this | knockback (§5), wall-slam, force-transfer (§6) | none — impulse never reaches the HP path directly |
| Heat | this + RFC-009 | ignition, melt/dry surface rules | fire damage + Burn build-up |
| Cold | this + RFC-009 | freeze surface rules, extinguish | cold damage + Freeze build-up |
| Electric | this + RFC-009 | conduction/propagation rules | shock damage + Shock build-up |
| Explosion | this | expands into per-tile Damage+Impulse+Crush (§2.2), terrain stress | via the expanded channels |

**The four elements map onto channels with no fifth "element channel".** Fire→Heat, Ice→Cold,
Thunder→Electric, and **Rock is not an energy type at all**: Rock-school skills (code:
`Element::kEarth`) emit Crush + Impulse + Explosion. This keeps the asset constraint of exactly
four skill-book elements (BookFire/BookIce/BookRock/BookThunder) while needing zero new icon
families. Plant/Water/Light/Darkness/Wind/Death remain icons only and are out of scope for v1;
a future school would claim either a new channel or a new combination of existing ones
(future work, not specced here).

#### 2.1 Producer-side scaling

The payload written in skill data is the payload of a **Medium-scale** caster. At emission the
delivery system multiplies `impulse`, `crush`, and `explosion` by the caster's
`scale_impulse_out` (§4) — a Giant's shove carries Giant momentum without boss-by-boss numbers.
`damage`/`pierce`/`heat`/`cold`/`electric` are **not** auto-scaled; offensive power stays an
explicit stat (RFC-009), so a big boss is not automatically a hard-hitting one.

#### 2.2 Explosion expansion

Explosion is the only self-expanding channel. At the impact tile, an `explosion = E` payload is
replaced by per-tile sub-payloads over a disc:

```
radius_tiles = clamp(1 + E / 150, 1, 4)                     (tunable)
falloff(d)   = (radius - d) / radius                        d = Chebyshev tile distance
per-tile:    damage  += E * 6/10 * falloff(d)               (tunable)
             impulse += E * 8/10 * falloff(d), dir = radial from centre   (tunable)
             crush   += E * 4/10 * falloff(d)               (tunable)
             stress  += E * falloff(d)     → terrain stress pool, §7
```

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

#### 3.1 The material response matrix

When a payload reaches a target, each channel is scaled by the target material's transmission
coefficient (per-mille) **before** RFC-009 sees it. This matrix is the single place "what am I
made of" turns into numbers. All values (tunable):

| ‰ | Damage | Pierce | Crush | Impulse | Heat | Cold | Electric | Explosion |
|---|---|---|---|---|---|---|---|---|
| **Flesh**  | 1000 | 1250 | 1000 | 1000 | 1000 | 1000 | 1000 | 1000 |
| **Stone**  | 600 | 250 | 1500 | 400 | 300 | 400 | 200 | 1250 |
| **Spirit** | 0 | 0 | 0 | 0 | 1300 | 1300 | 1300 | 250 |
| **Metal**  | 700 | 400 | 1000 | 800 | 600 | 600 | 2000 | 900 |
| **Wood**   | 900 | 800 | 1100 | 900 | 1500 | 500 | 300 | 1100 |
| **Plant**  | 1100 | 1000 | 800 | 1000 | 2000 | 800 | 600 | 1000 |
| **Water**  | 500 | 250 | 500 | 600 | 600 | 1500 | 1750 | 600 |
| **Slime**  | 800 | 500 | 1200 | 1400 | 1100 | 1200 | 1100 | 900 |

Reading rules the matrix encodes (these are the *reasons* for the numbers; a re-tune must
preserve them):

- **Spirit is immune to all four physical channels** — the umbrella §8 requirement — and its
  Explosion row is 250‰, not 0: the blast's *elemental* character is gone but a pressure wave
  still disturbs it slightly, so no attack loadout is a strict zero against anything that isn't
  pure physical. Its counterweight: ×1.3 to all three energies. The umbrella's "vulnerable to
  Arcane/Holy" is **future work** (those schools are icons only); in v1 the energy
  vulnerability is the spirit answer.
- **Stone laughs at arrows (250‰ Pierce) and cracks under hammers (1500‰ Crush, 1250‰
  Explosion)** — the anti-structure loadout is blunt, exactly what makes destructible
  counterplay (umbrella §12) legible.
- **Metal at 2000‰ Electric** plus the *arc* rule (§R8-row 9) makes metal the lightning
  liability it should be.
- **Slime at 1400‰ Impulse, 500‰ Pierce**: arrows pass through gel; shoves send it bouncing.
- **No row and no column is all-zero except Spirit×physical**, preserving "no absolute
  immunities" (§9) everywhere else.

Coefficients >1000‰ are *vulnerabilities*, not resistances — RFC-009's armour/resistance layer
applies after this matrix and owns per-creature deviation from the material default. The matrix
is per-material, never per-creature; a creature that needs to differ does it with RFC-009 stats.

#### 3.2 Material tags

Three derived boolean tags used by the rule engine, fixed per material (not data):

| Tag | Materials | Used by |
|---|---|---|
| `Flammable` | Wood (ignition ≥ 60 Heat), Plant (≥ 30 Heat) (tunable) | ignition rules |
| `Conductive` | Metal, Water | arc/chain rules |
| `Incorporeal` | Spirit | passes through Wall-type combat entities (RFC-004 consumes) |

### 4. Mass and scale tiers

Every combat actor has a **scale tier** (3 bits, replicated; assigned per archetype in RFC-008
data). Tier fixes the default mass and the automatic modifiers the umbrella §10 requires. Mass
may be overridden per archetype (±50% max) but tier modifiers may not — they are what keeps
"big" meaning one thing everywhere.

| Tier | Examples | Mass (pts) | Build-up intake ×‰ (Freeze/Stun/Root/Slow) | Impulse out ×‰ (`scale_impulse_out`) |
|---|---|---|---|---|
| Tiny | summoned wisps, critters | 25 | 1600 | 500 |
| Small | slimes, chickens, spiders | 50 | 1300 | 750 |
| Medium | players, most of the 66 monsters, wolves | 100 | 1000 | 1000 |
| Large | bears, elite monsters, GiantFrog | 250 | 600 | 1250 |
| Giant | dojo bosses: Samurai, GiantBamboo, GiantRacoon, DemonCyclop, Squids, Tengu phase 1 | 700 | 350 | 1600 |
| Titan | Tengu Trans phase 2; reserved for future set-pieces | 2000 | 200 | 2000 |

All values (tunable). Two hard invariants:

1. **The build-up intake multiplier is clamped ≥ 150‰** for any future tier — a Titan freezes
   five times slower than a player, never zero times. "Never immune" is a floor in the type, not
   a convention.
2. **Knockback has no tier modifier at all.** It needs none: `Impulse / Mass` (§5) already makes
   a Giant 7× harder to shove. Adding a second knob would double-count size; the umbrella's
   "scale automatically adjusts knockback" is satisfied *through* mass.

The build-up multiplier is applied by RFC-009 when accumulating Freeze/Stun/Root/Slow pools;
this RFC only defines the numbers so tier semantics live in one place.

### 5. Impulse resolution — knockback and the slam

The one law: **`kb_raw = impulse_effective / mass`**, in tiles (fixed-point 1/256). It runs after
material transmission (a Spirit's 0‰ Impulse row means it simply never enters this section) and
after the terrain factor:

```
kb_terrain = (100 - Friction_of(tile_under_target)) / 40        // per-mille math in impl.
kb_tiles   = min(kb_raw * kb_terrain, kKnockbackCap)            // kKnockbackCap = 4 tiles (tunable)
```

At baseline grass (Friction 60, §6) `kb_terrain = 1.0`: a 220-impulse heavy hit moves a
100-mass player 2.2 tiles. On an ice sheet (Friction 15) ×2.1; in marsh (Friction 95) ×0.125 —
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

Displacement below 0.25 tiles (tunable) is a **flinch**: no movement, renderer-only nudge
(RFC-006). This keeps micro-shoves from turning melee into jitter.

**WallSlam** converts the undelivered momentum back into harm, symmetrically:

```
slam_crush = remaining_tiles * mass * 5 / 10        // points (tunable)
```

applied as a Crush-only payload to **both** the displaced entity and, if the obstacle is a
destructible combat entity (RFC-004), to the obstacle. Throwing a monster into an Ice Pillar
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
| *WetFilm overlay* | −10 | −15 | =80 | +0 | additive deltas, clamped 0–100 |
| *IceSheet overlay* | =15 | =20 | =20 | =35 | frozen water / winter lakes; `=` sets, `−` adds |
| *Rubble overlay* | 85 | 60 | 5 | 25 | already-broken ground; re-breaks easily into deeper crater |

Surface overlays are RFC-004 state; the columns above are this RFC's contract for what those
overlays must report.

**Force-transfer (the mud rule, generalised).** When terrain suppresses knockback, the
suppressed momentum hurts instead:

```
if kb_terrain < 0.5:                                     // 500‰
    bonus_crush = impulse_effective * (500 - kb_terrain‰) * grip / 100 / 1000 * 6/10   (tunable)
```

Marsh (kb_terrain 0.125, grip 90): a 220-impulse hit adds ~44 Crush points — "less push but more
pain". Sand (kb 0.5, grip 55): zero bonus, it merely absorbs. **Slip mitigation** is the mirror
rule: when the target's tile grip < 30 (ice), incoming Damage + Crush are scaled ×850‰ (tunable)
— the target gives way instead of taking the blow square, the umbrella's "ice reduces direct
damage".

### 7. Terrain stress

Explosion expansion (§2.2), WallSlam on terrain (§5), and ground-targeted Crush each deposit
**stress points** on tiles. Stress is transient: it exists only within the resolving tick — there
is no persistent per-tile stress pool to replicate, sleep, or cheat with.

```
if stress_this_tick >= stability * kStressUnit:      // kStressUnit = 3 points (tunable)
    emit ConvertSurface(tile, verb)                  // executed by RFC-004
```

The conversion verb is chosen by RFC-004 from (terrain, overlay); this RFC only owes the
threshold test and the guarantee that **one tile converts at most once per tick**. Accumulating
stress across ticks (chipping a stone floor down over a fight) is explicitly rejected in v1: it
would be per-tile state that must replicate and survive LOD sleep for marginal gameplay
(see Open Questions).

### 8. The interaction rule engine

One ordered table, evaluated the same way everywhere. A rule is:

```cpp
struct InteractionRule {
    ChannelMask   trigger_channels;   // fires if any listed channel ≥ its threshold
    std::uint16_t threshold;          // points, after material transmission
    Predicate     subject;            // tag / material / surface predicate on the TARGET or TILE
    Response      response;           // one verb, parameters below
    bool          consumes_tag;       // detonation rules consume their status tag (RFC-002)
};
```

Response verbs (closed set — adding a verb is an RFC change, adding a *rule* is data):

| Verb | Meaning | Executed by |
|---|---|---|
| `ScaleChannels(mask, ‰)` | multiply channels before RFC-009 | this RFC |
| `AddChannel(ch, pts)` | inject channel points (e.g. shatter bonus) | this RFC |
| `ApplyStatus(tag, potency)` | add status build-up | RFC-002 / RFC-009 |
| `ConvertSurface(verb)` | change tile overlay | RFC-004 |
| `SpawnPayload(payload, shape)` | secondary explosion / splash | this RFC (recursion budget below) |
| `Propagate(ch, medium, decay‰, max_range)` | chain through a conductive/flammable region | this RFC |
| `RefundVital(which, pts)` | give the source stamina/mana | RFC-001 economy |

**The v1 rule table.** Ordered; first match per (channel, target) pair wins within a pass.
All thresholds/parameters (tunable):

| # | Trigger | Subject | Response | Notes |
|---|---|---|---|---|
| 1 | Crush ≥ 40 | target has `Frozen` tag | `ScaleChannels(Damage+Crush, 2500‰)`, armour-ignore flag to RFC-009; consumes tag | **Shatter** — works on frozen monsters, ice walls, and (as stress ×2.5) frozen lake tiles alike |
| 2 | Pierce ≥ 20 | target has `Burning` tag | `SpawnPayload({explosion: 45}, at target)`; consumes tag | **Blast** — the P2 fire-arrow pop |
| 3 | Electric ≥ 20 | target has `Wet` tag OR stands on Conductivity ≥ 50 tile | `Propagate(Electric, conductive-region, 700‰/tile, 6 tiles)` | **Conduct** — Water + Lightning spreads |
| 4 | Crush ≥ 40 | target has `Muddy` tag | `ApplyStatus(Stun, potency = crush/2)`; consumes tag | **Crush combo** |
| 5 | Damage ≥ 10 (melee flag) | target has `Shocked` tag | `RefundVital(mana, 8)`; consumes tag | **Arc** |
| 6 | Heat ≥ ignition | target material `Flammable`, or tile flammable surface (grass/brush, RFC-004 list) | `ApplyStatus(Burning, heat)` / `ConvertSurface(ignite)`; then `Propagate(Heat, flammable-region, 600‰/tile, kFireSpreadRadius = 5)` | **Fire + Plant spreads** — bounded, always burns out (RFC-004 lifetime) |
| 7 | Heat ≥ 30 | tile has IceSheet overlay | `ConvertSurface(melt → WetFilm)` | |
| 8 | Cold ≥ 40 | tile is Water edge or WetFilm | `ConvertSurface(freeze → IceSheet)` | player-made ice bridges; winter synergy (GAME.md §9) |
| 9 | Electric ≥ 30 | target material Metal | `Propagate(Electric, nearest Metal entity ≤ 2 tiles, 800‰, 1 hop)` | metal arcs |
| 10 | Cold ≥ 20 | target has `Burning` tag OR tile burning surface | remove tag / `ConvertSurface(extinguish)`; consumes | counterplay both ways |
| 11 | Heat ≥ 30 | target has `Wet` tag | remove tag (steam FX, RFC-006); consumes | drying |
| 12 | Explosion expanded | tile stress test (§7) | `ConvertSurface(...)` | craters/rubble |

Rows 1–5 are the **complete migration of `combo_of()`** — the five P2 combos become five ordinary
rows, generalised (Shatter now applies to any Frozen thing, not only creatures; Conduct now also
fires standing *in* water, not only when tagged Wet). Note what is **absent**: "Ice + Shockwave =
slide farther" and "mud absorbs shoves" are *not rules* — they emerge from §5/§6 formulas. The
table holds only what a formula cannot express.

**Evaluation algorithm** (normative — this is where two implementations could diverge, so it is
pinned):

```
resolve(payload, target):
  1. effective = payload × material_matrix[target.material]        (§3.1)
  2. Pass A  — target-scoped rules (rows 1,2,4,5,9,10,11), in table order;
               each rule fires at most once per resolution; consumed tags are gone
               before later rows test them.
  3. RFC-009 — damage, resistances, build-up (receives scale-tier intake ‰, §4).
  4. Impulse — §5 knockback (uses post-Pass-A impulse).
  5. Pass B  — tile-scoped rules (rows 3,6,7,8,12) on the impact tile and, for
               expanded explosions, each affected tile in row-major tile order.
  6. Propagation queue — Propagate/SpawnPayload responses enqueue; the queue drains
               with a budget of kChainBudget = 16 rule firings per chunk per tick
               (tunable); the remainder carries to the next tick FIFO. Each
               propagation step re-enters resolve() with the decayed payload;
               SpawnPayload recursion depth is capped at 2 (tunable).
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
  Per-tile: nothing (stress is intra-tick; overlays are RFC-004's, already specced to survive
  LOD). A sleeping chunk stores zero physics state.
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

1. Expansion (§2.2): radius `1 + 200/150 = 2` tiles; centre tile takes the listed channels plus
   `damage +120, impulse +160 radial, crush +80, stress 200`.
2. A Medium Flesh monster at the centre: matrix ×1000‰ throughout; Pass A: no tags — nothing
   fires. RFC-009 eats damage/crush/heat. Impulse `380/100 = 3.8` tiles radial on grass ×1.0 →
   capped slide; it slams a Stone spike after 2.1 tiles → `1.7 × 100 × 0.5 = 85` Crush to both;
   the spike's Stone matrix makes that `127` — spikes die to meteors.
3. Centre tile: stress 200 ≥ grass stability 60×3=180 → `ConvertSurface` → RFC-004 makes rubble;
   Heat 40 ≥ grass ignition → row 6 ignites the rim, spreads ≤ 5 tiles, burns out.
4. Nothing in steps 1–3 mentioned "Meteor". A different skill with the same payload behaves
   identically — that is the whole point.

---

## Interactions with Other RFCs

| RFC | Direction | Contract |
|---|---|---|
| **RFC-001** | in | The ability pipeline produces `AttackPayload`s at Impact (and per zone tick); geometry fields are producer-filled. Payload channel list is fixed by this RFC; RFC-001 never adds interaction logic. `RefundVital` verb is executed by RFC-001's resource economy. |
| **RFC-002** | both | Status tags (`Wet`, `Burning`, `Frozen`, `Muddy`, `Shocked`, …) are RFC-002 state; this RFC's rules test and consume them via `ApplyStatus`/remove verbs only. The P2 one-status-per-creature rule and any relaxation of it are RFC-002's call; the rule table is agnostic (it tests tags individually). |
| **RFC-004** | both | Combat entities carry `Material`+scale+mass; surface overlays (WetFilm/IceSheet/Rubble/Burning) and all `ConvertSurface` execution are RFC-004's, honoring the property columns in §6. Wall-type entities are knockback obstacles (§5). |
| **RFC-005** | out | Boss authors write payload numbers only; §2.1's `scale_impulse_out` gives Giant hits weight for free. No boss-specific interaction hooks exist. |
| **RFC-006** | out | Every verb that fires emits exactly one FX id (tint/overlay per the asset rules); flinch vs slide threshold (§5) is the renderer contract. Reaction FX must fit `effect_life_of` budgets. |
| **RFC-007** | out | Observation must expose: own/target material id, scale tier, and the four terrain properties of the tiles under both (quantised); knockback-in-progress bit. Layout is RFC-007's. |
| **RFC-008** | out | Skill files carry the eight channel numbers, archetype files carry material/tier/mass override; the rule table itself ships as engine data with the same schema treatment. |
| **RFC-009** | out | Receives *effective* channels post-matrix and post-Pass-A, plus the tier build-up intake ‰ (§4) and the armour-ignore flag (row 1). Owns armour, resistance, HP, and all build-up ladders (§9 of the umbrella). |
| **RFC-010** | both | Owns the tick loop that calls `resolve()`, movement (consumes Grip), cross-chunk payload delivery, and the LOD schedule this RFC's §9 collapses under. |

---

## RL Considerations

- **One world model.** Because bosses and players resolve through the same matrix, formulas, and
  rule table, a policy's learned regularities ("charging a target on ice overshoots", "mud
  cancels my knockback opener") transfer across rooms and archetypes — a precondition for the
  10–15-policies-total budget.
- **Observation.** The §8 pipeline is fully determined by observable quantities: materials, tiers
  and terrain properties are static or slow-changing, so RFC-007 can expose them as small
  categorical/quantised fields rather than streams. `BossObs` today is int-ish and room-local
  (`boss.hpp`); the additions stay in that style.
- **Bounded variance.** `kChainBudget`, the propagation visited-set, and knockback caps bound the
  consequence of any single action — important both for replay-buffer reward variance and for
  keeping a 10×7-tile interior room (the RL training space) from becoming un-resettable.
  In-room hazards expire; a training episode reset clears surfaces (RFC-004 verb).
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
  rule table's twelve rows emit only currently-packed FX kinds (`kFire/kIce/kEarth/kShock/
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
- **Chill tone (GAME.md §0).** Physics is strictly reactive; all propagation is radius-capped and
  self-extinguishing; nothing counts down off-screen or seeks a player. Additionally, fire
  spread (row 6) is forbidden from entering tilled soil (`kDirt`) and player claim radii
  (tunable guardrail — a stray fight can never burn a farm the farmer is chilling on).

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
5. **Matrix tuning harness.** 8×8×(rule table) is too many interactions to tune by feel alone.
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
