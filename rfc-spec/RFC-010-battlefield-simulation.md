# RFC-010: Battlefield Simulation

**Status: Accepted (revised after review)**
**Umbrella sections: 4 (Battlefield Control), 16 (Battlefield States)**
**Depends on:** RFC-001 (ability pipeline creates battlefield state), RFC-002 (statuses hazards apply), RFC-003 (material/surface coefficient tables), RFC-004 (CombatEntity), RFC-009 (damage & build-up math)
**Depended on by:** RFC-005 (bosses author field states), RFC-006 (renders them), RFC-007 (observes them), RFC-008 (serialises their definitions)

---

## 1. Summary

This RFC specifies the battlefield as *simulated, replicated state*: the ephemeral layer of
tile surface changes (burning grass, ice glaze, mud), lingering hazard zones (smoke, wet
ground, fire patches), and battlefield-wide field states (earthquake) that combat writes
onto the world and the world ticks back toward baseline. Lasting ground *scars* — cracked
ground, rubble, craters, scorch marks — are **not** specified here: they are RFC-004 §8's
terrain scar layer, which this RFC consumes and stamps but never redefines.

It defines:

- a **three-layer state model** — baseline terrain (pure function, never touched), **tile
  patches** (per-tile surface state machine), and **field states** (area-wide modifiers) —
  and the fixed-capacity records each layer uses;
- the **tick order and budgets** for stepping that state inside a `ChunkActor`;
- the **earthquake field state** end-to-end: creation authority, propagation across chunk
  boundaries, projectile drift, accuracy loss, and expiry;
- **determinism classes** — which battlefield state must be bit-exact across nodes, which is
  authority-broadcast, and which is deliberately order-dependent and never compared;
- **server authority and replication**: single-writer chunk ownership, snapshot-copy
  replication with hard byte caps;
- **interaction with simulation LOD**: exactly what happens to every category of battlefield
  state at 10 Hz, at 1 Hz, and in a slept chunk, with an O(1) wake-up catch-up rule.

Everything with hit points (spikes, ice pillars, totems) is a CombatEntity and belongs to
RFC-004. This RFC owns state *without* hit points, plus the ticking, authority, LOD, and
budget rules that RFC-004 entities also obey (RFC-004 defers to the rules tables here).

## 2. Motivation

The umbrella spec's core promise is that skills "reshape the battlefield" (§4) and that
whole-battlefield states like earthquakes exist (§16). Today the codebase has fragments of
this — `Zone` (wet/smoke circles, capped at 8 per chunk), `Effect` (per-kind lifetimes),
per-creature statuses — but no terrain state machine, no field states, and no written rules
for what any of it does when a chunk drops to background rate or sleeps.

Three forces make a spec necessary before more combat state is added:

1. **The 1024×1024 overworld runs on simulation LOD** (ARCHITECTURE §4). Any battlefield
   state that assumes "I am ticked 10 times a second" silently breaks in a background or
   slept chunk. Crops already solved this (stage derived from elapsed time); battlefield
   state must adopt the same shape *by construction*, not by later retrofit.
2. **Replication is snapshot-copy.** A `ChunkView` is copied on every publish. Unbounded
   battlefield state is unbounded per-publish garbage. Every record here therefore has a
   fixed cap and a fixed byte size, chosen in this document.
3. **RL bosses train on this state** (RFC-007). A training episode must be reproducible;
   that forces the determinism rules in §5 to be written down now, because a drift formula
   that reads wall clock or unseeded randomness poisons every checkpoint trained on it.

And one force from the design, not the code: GAME.md §0 — *chill is the default*. A
battlefield system is exactly the kind of feature that can leak pressure into the cozy loop
(a fire that burns down your farm while you fish). §4.6 makes the guardrail mechanical.

## 3. Guide-level Explanation

### For a player

You fight a Giant Samurai in its dungeon room. Its slam attack cracks the floor tiles where
it lands; a second slam on cracked ground breaks them into rubble that slows anyone crossing
it (that lasting wear is RFC-004's scar ladder doing its work). At half health it roars and the whole room starts to shake: your arrows drift off line,
your spells miss more often, and even the boss's own telegraphs tremble on the floor. The
shaking is a *phase* — it lasts a fixed, visible duration and stops. When the fight is over
and you leave, the room heals: rubble crumbles away, scorch marks fade, and the next group
finds a clean arena.

On the overworld, the same rules apply in miniature. A stronghold raid's fire spell leaves
burning grass that spreads a few tiles and then burns out to scorch marks; the scorch fades
in a minute. Nothing combat does to the ground is permanent, nothing spreads without limit,
and nothing at all happens in places no player is anywhere near. Your farm cannot burn down
while you are away, because (a) hazards decay on a bounded clock, (b) fire spread is capped
and cannot cross into claims or village interiors, and (c) a chunk with no player nearby
does not run fights at all — with one named exception: a village raid (GAME.md §6) resolves
whether you attend or not, but raids target village walls, never your farm or your claim.

### For a designer

You get two tools, each a data record (RFC-008 schema):

| Tool | What it is | Example |
|---|---|---|
| **Tile patch** | one tile's surface temporarily in a non-baseline state, with an automatic decay chain | grass → *Burning* (4 s) → baseline + an RFC-004 scorched *scar* that heals on its own clock |
| **Field state** | a battlefield-wide modifier with an epicenter, radius, intensity, and duration | earthquake: shake + projectile drift + accuracy loss |

Two more ground tools exist but are owned elsewhere, per the RFC-004 §2b single-ownership split:
**hazard circles** — a footprint that applies a status or suppression to creatures inside it each
tick (smoke cloud, wet ground, fire patch) — are RFC-004 `CombatEntity` kinds
(`kSmokeCloud`/`kWaterPool`/`kFirePatch`), not a battlefield record here; and **lasting scars**
(cracked → rubble → crater, plus scorched) are RFC-004 §8's — you author *impacts*, and RFC-004's
escalation ladder and heal clock do the rest. This RFC never duplicates either.

You never script *how* they tick, replicate, or survive LOD — those rules are fixed here.
You author *which* ability phases create them (RFC-001 Persist), with what numbers, and the
tables in §4 say what each surface/field does. Adding "Cursed Ground" is a new `Surface`
enum row, a decay chain entry, and a coefficient row in RFC-003 — no new systems.

### For an engineer

All battlefield state lives inside the owning `ChunkActor`, is stepped in a fixed order
inside `handle(Tick)`, is published as part of the ordinary `ChunkView`, and stores
*expiry timestamps* (world ms) instead of countdown counters so a slept chunk catches up in
O(records), not O(missed ticks). Cross-chunk influence travels only as messages with
soft-state leases — the beacon/ARP pattern already proven in P2.

## 4. Reference-level Design

### 4.0 Coordinate and time conventions

- Tick rate: `kTicksPerSecond = 10` (existing, `tiles.hpp`). All durations below are given
  in ticks at 10 Hz but stored as **absolute world-time deadlines** — `end_ms :
  std::int64_t`, same clock as the existing `world_ms_`. Not `std::uint32_t`: a 32-bit ms
  offset wraps after ~49 days of world time, and a persistent world is exactly the kind of
  thing that reaches it.
- Tile coordinates are map-global `std::uint16_t` pairs, as in `Crop`/`Zone`.
- A chunk is 32×32 tiles; a boss room is a 10×7-tile interior room owned by one chunk.

### 4.1 The three layers

```
Layer 0  BASELINE   terrain_of(seed, map, x, y [, season])  — pure, never written by combat
Layer 0b SCARS      RFC-004 §8's ScarKind overlay (cracked/rubble/crater/scorched) — owned
                    and healed entirely by RFC-004; this RFC only stamps into it
Layer 1  PATCHES    per-tile transient Surface records, chunk-owned, bounded, self-decaying
Layer 2  FIELDS     area modifiers (earthquake), chunk-owned at epicenter, leased copies
                    in neighbours
(Creatures, projectiles, zones, effects, CombatEntities sit on top and read all layers.)
```

**Invariant P-1 (terrain purity).** Combat never writes the worldgen overlay and never
changes the result of `terrain_of`. Patches are a separate lookup consulted *before*
baseline by combat and rendering only.

**Invariant P-2 (pathing isolation).** Patches and fields never change walkability and never
invalidate the flow field. A flow-field rebuild costs ~25 ms at 1024²; combat state that
could trigger one is a frame-time grenade and is forbidden. (Consequence: a combat ice sheet
over water is *not* walkable in v1 — see Open Questions. The seasonal frozen lake stays a
P7 `season` parameter of `terrain_of`, out of scope here.)

**Invariant P-3 (bounded decay).** Every patch and field carries a finite `end_ms` at
creation; the maximum total dwell of any decay chain is 90 s (tunable). The battlefield
always returns to baseline with no player action.

### 4.2 Tile patches — the terrain state machine

```cpp
enum class Surface : std::uint8_t {
    kBurning = 0,   // fire on flammable ground; spreads (bounded), applies Burn build-up
    kMudded = 1,    // Rock-churned or rain-soaked ground; movement/knockback coefficients
    kIced = 2,      // Ice-glazed ground; movement/knockback coefficients
    kCount = 3,
};

struct TilePatch {                 // 16 bytes
    std::uint16_t tx, ty;          // map-global tile
    Surface s;
    std::uint8_t intensity;        // 0..3, reserved for RFC-009 build-up coupling
    std::int64_t end_ms;           // absolute deadline; then next_of(s) or baseline
};
inline constexpr std::size_t kMaxPatches = 48;   // per chunk (tunable)
```

**Ownership boundary (load-bearing).** Cracked, rubbled, cratered, and scorched ground are
**not** `Surface` values: they are RFC-004 §8's terrain scar layer (`ScarKind`,
`kMaxScars = 64`/chunk, lazy `heal_tick` decay, the cracked → rubble → crater escalation
ladder). Patches here are the *transient elemental coatings* that sit above scars; the two
layers never define the same ground state twice. Where a patch leaves a lasting mark, it
does so by stamping an RFC-004 scar at expiry, never by growing a longer chain here.

**Decay chain** is a pure function pair, the whole mechanism of the state machine:

```cpp
constexpr Surface next_of(Surface s);      // v1: all -> baseline (kBurning also stamps a scar)
constexpr std::int64_t dur_ms_of(Surface s);
```

| Surface | Duration (tunable) | Decays to | Gameplay while active |
|---|---|---|---|
| kBurning | 40 ticks (4 s) | baseline + stamps an RFC-004 `kScorched` scar on the tile (heals per RFC-004 §8) | Burn build-up per tick to occupants (rate: RFC-009); spreads per spread rule |
| kMudded | 300 ticks (30 s) | baseline | RFC-003 mud row: knockback ×0.5, force-transfer damage ×1.25 |
| kIced | 300 ticks (30 s) | baseline | RFC-003 ice row: knockback ×1.5, direct damage ×0.8 |

**Transition triggers** (who writes a patch — all via the owning chunk, §4.4):

| Trigger (from RFC-001 Impact/Persist, RFC-009 thresholds) | On baseline | On existing patch |
|---|---|---|
| Fire impact ≥ ignition threshold, flammable tile (grass/forest floor per RFC-003 material of tile) | kBurning | kIced/kMudded → removed (steam, no patch); kBurning refreshed |
| Ice impact ≥ threshold | kIced | kBurning → removed; kMudded → kIced |
| Rock impact ≥ heavy threshold | *no patch* — stamps/escalates an RFC-004 §8 scar (kCracked → kRubble → kCrater ladder, owned there) | — |
| Rock churn; wet zone expiring on dirt (the RainCall-class weather source is P7 — future trigger, not v1) | kMudded | — |
| Thunder | *no patch* (Thunder interacts via Wet status/conduction, RFC-002) | — |

The exact impact thresholds are RFC-009's build-up numbers; this table only fixes *which
surface results*. Element set is exactly Fire/Ice/Rock/Thunder — no other school produces a
surface in v1.

**Fire spread rule** (the umbrella's "Fire + Plant = fire spreads", bounded):

- Every `kSpreadPeriod = 5` ticks (tunable), each kBurning patch rolls once per 4-neighbour
  tile: if the neighbour is flammable baseline and unpatched, ignite with `p = 0.25`
  (tunable), drawing from the spread subsystem's own salted RNG stream (§5 D3).
- **Spread is array-layout-independent (D3).** Candidate tiles are gathered from all
  kBurning patches, deduplicated, **sorted `(tx, ty)` lexicographic**, and rolled in that
  canonical order, stopping when `kMaxBurning` is reached. The physical order of the patch
  array (fill-in-place vs compact-after-evict — deliberately unspecified) therefore can
  never change which tiles ignite; two conforming implementations produce the identical
  patch ledger, which is what §5's byte-exact CI test compares.
- Hard caps: at most `kMaxBurning = 12` (tunable) kBurning patches per chunk; ignition rolls
  stop when reached. Spread never crosses a chunk boundary in v1 (a fire is a local event,
  not a message fan-out; see Open Questions), never ignites tiles inside a village wall
  ring or a player claim (§4.6), and each ignited tile gets the *remaining* window of its
  igniter, not a fresh 4 s — so a fire front is self-extinguishing by construction.

**Storage and eviction.** Patches live in a flat array scanned linearly (48 × 16 B fits two
cache lines' worth of keys; a per-tile grid would be 32×32 and copied every publish —
rejected). One patch per tile: a new patch on a patched tile *replaces* it per the trigger
table. When the array is full, the new patch replaces the record with the smallest `end_ms`
(deterministic, no allocation); if the incoming patch's own `end_ms` is smaller than every
existing one, it is dropped. Two engineers implementing this must converge: compare by
`(end_ms, tx, ty)` lexicographically to break ties.

### 4.3 Field states — earthquake

```cpp
enum class FieldKind : std::uint8_t { kEarthquake = 0, kCount = 1 };  // v1: exactly one

struct FieldState {                 // 24 bytes
    FieldKind kind;
    std::uint8_t intensity;         // 1..3
    std::uint16_t cx, cy;           // epicenter, map-global tiles
    std::uint16_t radius;           // tiles; hard cap kMaxFieldRadius = 32 (= chunk width; see §4.3 propagation)
    std::int64_t end_ms;
    std::uint32_t source;           // creature/boss id for attribution & dedup
};
inline constexpr std::size_t kMaxFields = 2;   // per chunk (tunable)
```

**Effect table** (all values tunable; applied only to actors within `radius` of epicenter):

| Intensity | Camera shake (render-only, RFC-006) | Projectile drift amplitude `A` (tiles/tick, perpendicular) | Accuracy multiplier (deterministic per-mille contribution to RFC-009's `M_outer`) | Telegraph tremble (render-only, RFC-006) |
|---|---|---|---|---|
| 1 | 1 px | 0.02 | ×900‰ | subtle |
| 2 | 2 px | 0.04 | ×800‰ | visible |
| 3 | 3 px | 0.06 | ×700‰ | strong |

**The accuracy penalty is deterministic, not a roll.** RFC-009 §4.4 already reserves the
slot: "the battlefield state multiplier (RFC-010, e.g. earthquake accuracy)" is an input to
the clamped per-mille `M_outer` inside a pipeline that is explicitly "no floats, no RNG".
This RFC conforms: an active field contributes the table's per-mille value to `M_outer` for
every attack resolved inside `radius` — no miss coin-flip exists anywhere in the quake.
Determinism class D1 (§5): a pure function of intensity, nothing to seed, so RL episodes
(§6) and the cross-node hit ledger replay for free. Ranged attacks are hit twice *by
design*: projectile drift physically bends the shot (it can genuinely miss) **and** the
multiplier weakens whatever connects, while melee suffers only the multiplier — an
earthquake punishes precision at range more than brawling, and that asymmetry is intended.

**Projectile drift** is simulation state (the arrow really lands elsewhere; every machine
must agree), so it must be a pure function, not sampled noise:

```
phase(shot) = (shot.id * 7 + shot.launch_tick) mod P          // P = 20 ticks (tunable)
drift_perp(tick, shot) = A * sin(2*pi * ((tick + phase(shot)) mod P) / P)
```

Applied in `step_projectiles` as a per-tick perpendicular velocity offset while the shot is
inside an active field. Pure in `(shot id, launch tick, tick, intensity)` — replays and RL
episodes reproduce it exactly. Drift and the accuracy multiplier apply to *all* teams — the
boss's ranged attacks (e.g. Squids' Shoot) degrade too; an earthquake is weather, not a buff.

**Stacking:** overlapping fields do not sum. An actor inside several takes the single
highest intensity; a new field from the same `source` with the same kind *replaces* the old
record (refresh), from a different source occupies the second slot or is dropped if full
(smallest-`end_ms` eviction, same rule as patches).

**Creation authority.** In v1 only these may create a field state:

1. A **boss ability** (RFC-005) — resolved by the chunk that owns the boss. First user:
   the Giant Samurai's half-health slam phase; also suits GiantBamboo. Duration
   `kQuakeTicks = 80` ticks (8 s, tunable).
2. A **stronghold event** — the `StrongholdActor` (trusted) may attach a field to a raid it
   spawns, supplying `(kind, intensity 1..3, radius ≤ kMaxFieldRadius, duration)` from the
   raid's data definition (RFC-008 schema); no parameter is implicit.

**Both paths obey `kMaxFieldTicks = 80` (8 s, tunable) as a hard cap on the record**, not a
convention of one author — §4.7's "max field duration < any sleep threshold" argument must
hold for every creation path, so the cap is enforced where the `FieldState` is written.

Player abilities cannot create field states in v1. The player kit is basic attack + two
equipped abilities (RFC-001) and stays that way; none of those abilities produces a field.

**Cross-chunk propagation.** A field's epicenter chunk is its single writer. If
`radius` spills past the chunk border, the owner sends a `FieldPulse{FieldState}` message
to each intersecting neighbour. `kMaxFieldRadius = 32` — exactly one chunk width — is a
**load-bearing invariant**: it guarantees that even an epicenter on a chunk border reaches
at most the 3×3 neighbourhood, so the one-ring fan-out covers every actor inside the
field's own radius. Raising `kMaxFieldRadius` above 32 requires a deeper fan-out band and
a recomputed §4.8 message budget — it is not a free tunable. Pulses repeat every
`kFieldPulsePeriod = 5` ticks (tunable). Receivers hold a **leased copy**: applied like a
local field but dropped if no pulse arrives for `2 × kFieldPulsePeriod` ticks. No delete
message exists or is needed — expiry and lease-lapse both self-clean (the beacon/ARP
argument, verbatim). Interior boss rooms are single-chunk; no fan-out ever occurs there.

### 4.4 Tick order and authority

`ChunkActor::handle(Tick)` steps battlefield state in this canonical order (extending the
existing order; new steps in bold):

```
expire_beacons
grow_crops
step_status            (RFC-002)
**step_fields**        expiry + lease checks; computes this tick's active modifier set
step_creatures         reads patches (movement coeffs) + fields (accuracy) via the modifier set
step_projectiles       applies drift_perp from the modifier set
step_effects
**step_patches**       decay chains via next_of/dur_ms_of; fire spread (budgeted)
reap_dead
step_bosses
publish (per LOD rule)
```

`step_fields` runs before creatures and projectiles because a field changes what they do *this*
tick. The legacy `step_zones` step is retired: hazard circles are now RFC-004 `CombatEntity`
records (§2b there), stepped and LOD-governed entirely by RFC-004 §9, not by a battlefield step
this RFC owns.

**Authority.** The chunk actor owning a tile is the single writer of every patch, zone, and
field whose anchor lies in it. Requests arrive only as messages: ability resolutions
(check-and-debited by the trusted `PlayerActor` per the existing pattern), boss actions
(resolved in-chunk), `FieldPulse` leases. A chunk never reads a neighbour's state — the
same rule that already governs creatures and the two known items of cross-chunk debt.
Hazard-circle entities (RFC-004 `CombatEntity` kinds) that spill across a seam under-cover
exactly as they do today; the seam fan-out fix is RFC-004's cross-chunk entity summary, owned
there and not duplicated here.

### 4.5 Replication

Battlefield state replicates exclusively through the published `ChunkView` (copied, never
referenced), so caps are the replication contract:

| Record | Cap | Bytes | Worst case per view |
|---|---|---|---|
| TilePatch | 48 | 16 | 768 B |
| FieldState | 2 | 24 | 48 B |
| Effect (existing) | existing cap | 12 | unchanged |

**Budget B-1:** battlefield additions to a `ChunkView` ≤ 1 KB (tunable). Anything that
wants more must shrink a cap, not raise the budget.

Clients render *only* from views; there is no client-side battlefield prediction in v1
(latency 50–150 ms is acceptable for a 4 s burn patch; it is not a PvP game). The leader
persists none of this: patches and fields are in-memory state like creature
positions (ARCHITECTURE §3 storage table row "not saved") — a world reload starts with a
clean battlefield, which Invariant P-3 makes indistinguishable from waiting ≤ 90 s.

### 4.6 Tone guardrails, made mechanical

GAME.md §0 is a hard constraint; these rules are the enforcement, not aspiration:

1. **No spontaneous fields.** Every field state's `source` is a boss a player sought out or
   a stronghold raid event — a bounded, village-targeted world event (GAME.md §6) that never
   targets players or claims. Ambient earthquakes do not exist.
2. **No permanent scarring** (Invariant P-3). Patches and fields self-heal ≤ 90 s; RFC-004
   scars heal on their own bounded clock (§8 there, ≤ 10 min). Nothing is forever.
3. **Claims and village interiors are fireproof.** The spread rule refuses tiles inside a
   player claim or village wall ring. Direct impacts there still patch (your own Nova can
   scorch your own yard) but never spread.
4. **Nothing burns off-screen — with one named exception.** LOD rules (§4.7): non-active
   chunks apply no hazard damage and run no spread; combined with "monsters stay in their
   dungeons" there is no mechanism by which battlefield state harms an absent player. The
   exception is the village raid (GAME.md §6): tier ≥ 2 villages defend themselves — the
   RL-trained guards fight the raid whether or not any player attends — so a chunk hosting
   an active raid is **pinned active-tier for the raid's duration** (§4.7). Raids are rare,
   bounded events that target villages, never a player's claim, so the guardrail holds:
   nothing the *player* owns is ever harmed off-screen.

### 4.7 Simulation LOD

The three tiers of ARCHITECTURE §4 (active 10 Hz / background 1 Hz / slept). Note the
current code implements *publish*-rate LOD (`kIdlePublish = 32`) with full-rate simulation;
this section specifies battlefield behaviour for the full tiering when it lands, and is
written so battlefield state is already correct under today's degenerate case (all chunks
active).

**Invariant L-1:** a chunk holding any player beacon is active tier. Therefore all
player-affecting battlefield math runs at exactly 10 Hz. Per-tick quantities (Burn DoT,
drift) are fixed per-tick amounts and are **never scaled by elapsed time** — a 1 Hz tick
must not deliver a 10× mega-dose. This bug class is forbidden by construction: background
and slept tiers simply do not run those steps.

| State | Active (10 Hz) | Background (1 Hz) | Slept | On wake |
|---|---|---|---|---|
| Tile patches | full: auras, spread, decay | decay only (expire by `end_ms`, advance chain); no spread, no DoT | none survive — sleep requires no player nearby for N minutes and max patch dwell ≤ 90 s (P-3), so the array is empty before sleep entry is possible | baseline; nothing to replay |
| Field states | full | expiry only; pulses to background neighbours stop (their leases lapse — correct: nobody is watching) | **dropped on sleep entry** | none — a field requires an audience by rule 4.6-1, and `kMaxFieldTicks` (8 s, every creation path, §4.3) < any sleep threshold anyway |
| Boss fights | always active (a fight implies a beacon; training rooms are pinned active, §6) | n/a — existing leash (`kBossLeashTicks`) resets an abandoned fight | instance idles out, room resets | fresh room |
| Village raids (GAME.md §6) | raid chunk **pinned active-tier for the raid's duration** (a bounded event; O(active raids) extra chunks, not O(map)) | n/a — pinned | n/a — pinned | raid resolves before the pin releases; normal tiering resumes |
| Effects | full | not spawned (nobody sees them) | — | — |

Hazard-circle entities (RFC-004 `CombatEntity` kinds) are absent from this table by design: they
obey RFC-004 §9's own LOD/sleep contract in full, not this one — see the boundary note in §3.

**Sleep is destruction, not pause.** ARCHITECTURE §4's slept tier is the `IdleTimeout`
policy, and `IdleTimeout` elsewhere in that document destroys the actor and recreates it on
demand — a slept chunk is *not* guaranteed resident. This RFC therefore may not rely on any
record surviving sleep, and the table above is written so nothing needs to: every
battlefield record's maximum dwell (patches ≤ 90 s, fields ≤ 8 s) sits below any plausible
sleep threshold (minutes), so a chunk eligible for sleep has an
empty battlefield by construction and wake starts from baseline — consistent with §4.5's
"not saved" and with the Boss Fights row's fresh-room reset. RFC-004 scars follow RFC-004
§9's own LOD contract, not this table.

Because every record stores `end_ms` rather than a countdown, the background rule is one
code path: "advance the deadline chain to now". This is the crop model
(stage derived from elapsed time), adopted as a requirement: **Invariant L-2 — no
battlefield record may store a per-tick countdown.** `Effect` is the sole pre-existing record
this touches, and it may keep `age` only, because effects are never present in non-active
chunks (they are pure presentation with a
≤ 14-tick life and are simply not spawned there).

### 4.8 Tick-rate and budget rules

- **Canonical rate** 10 Hz; all design durations in this spec are authored in ticks at
  10 Hz and stored as ms.
- **No tile scans.** Every battlefield step is O(records) over the capped arrays. The only
  neighbourhood examination is fire spread: ≤ `kMaxBurning × 4` tile lookups per
  `kSpreadPeriod`, i.e. ≤ 48 `terrain_of` calls per 5 ticks per chunk (tunable via caps).
- **Per-chunk budget:** battlefield steps (`step_fields` + `step_patches`, plus the patch/field
  reads `step_creatures` makes through the modifier set, §4.4) ≤ 25 µs p95 per active tick
  (tunable), inside an overall chunk-tick target of 100 µs p95 (tunable). At worst-case caps
  that is 48+2 records — trivially within budget; the budget exists to reject future designs
  that are not.
- **Active-set invariant:** active-tier chunk count is O(players) (≤ 25 per player from the
  5×5 beacon fan-out, overlapping) plus O(active raids) raid-pinned chunks (§4.7), never
  O(map). Battlefield cost therefore scales with the 20–50 player target, not with 1024
  chunks.
- **Message budget:** field fan-out ≤ 8 neighbours × 2 pulses/s × `kMaxFields` = 32 msg/s
  per field-owning chunk worst case (tunable via `kFieldPulsePeriod`).

## 5. Determinism Requirements

Four classes, extending ARCHITECTURE §2c's measured boundary. Every piece of battlefield
state must be assigned to exactly one class by its implementer:

| Class | Battlefield members | Requirement |
|---|---|---|
| **D1 Seed-pure** | baseline terrain, flammability of a tile, `next_of`/`dur_ms_of` chains, drift formula, the earthquake accuracy multiplier (per-mille function of intensity, §4.3), spread probability function | bit-exact across nodes and toolchains (the GCC/MSVC per-tile standard P1 established); pure functions of integers — the drift sine is computed once into a per-intensity lookup table of `P` fixed-point entries at build time to avoid libm divergence |
| **D2 Authority-broadcast** | field-state creation, stronghold raid attachments, weather (out of scope, P7) | exactly one writer decides; everyone else receives the record verbatim (message or leased pulse); never recomputed independently |
| **D3 Chunk-deterministic** | ignition rolls (in the canonical candidate order, §4.2), patch eviction, zone application order | pure function of `(chunk_key, tick, subsystem salt)`. **Each D3 consumer constructs its own `Rng` instance** from its salted key — spread uses `Rng((chunk_key ^ 0x5EED'F17E) * 0x9E37'79B9'7F4A'7C15 + tick)` (salt tunable) — rather than sharing one threaded stream. This is a deliberate departure from today's pattern (`chunk_actor.hpp:123` builds a single unsalted per-tick `Rng` and passes it by reference into `step_creatures`): a shared stream makes every consumer's draws depend on every other consumer's draw *count*, so adding one shifts all; per-consumer instances make adding a consumer draw-neutral, which is the guarantee this row exists to give. `step_creatures` may keep its legacy stream; every battlefield consumer must not share it. Reproducible given the same message arrival sequence |
| **D4 Order-dependent** | which tick a `FieldPulse` lease is refreshed, cross-chunk under-coverage at seams, patch counts summed across a run | explicitly *not* reproducible across runs; excluded from any cross-node checksum or test assertion, exactly as chunk-migration counts already are |

Hard rules:

- **No wall clock.** The only time is `Tick::world_ms`.
- **No unseeded randomness.** All draws from the D3 stream.
- **Quantise at seams.** Anything crossing into an RL observation, a checkpoint, or a
  cross-node comparison is integers/fixed-point (the `BossObs` precedent: tile offsets,
  ticks, hp per-mille). Float positions stay chunk-internal.
- **Test hook:** a headless run with a scripted message sequence must reproduce the full
  patch/field ledger of a chunk byte-for-byte (D1+D3); CI compares Linux/MSVC on that
  ledger, never on D4 quantities.

## 6. RL Considerations

- **Observation surface (value set fixed here; consumed and packed by RFC-007, the sole F4
  tensor contract — no separate BossObs schema exists).** This RFC defines *no* grid layout,
  window, or per-tile plane of its own. RFC-007 §2 currently exposes terrain and hazards through
  two egocentric projections, not a tile plane: Block R (8 compass rays × blocked/hazard
  closeness) and Block G (a 4-way ground-class one-hot — Normal/Slow/Conductive/Unstable, RFC-003's
  material projection). This RFC's `Surface` values (burning/mud/ice) and RFC-004's `ScarKind`
  scars therefore reach the policy only as coarse signal today — e.g. a burning or mudded tile
  reads as "hazard" on Block R and "Slow"/"Conductive" on Block G, not as a distinguishable
  per-tile value. Hazard-circle entities (RFC-004 `kSmokeCloud`/`kWaterPool`/`kFirePatch`) are
  already first-class in RFC-007 Block E (the 3-nearest-entity slots, `HazardZone` class one-hot)
  — no gap there. Whether the ray/class projection is too lossy for terrain surfaces and scars,
  and if not what a per-tile encoding would cost, is RFC-007's open question (see RFC-007 §Open
  Questions), not a layout invented here; any future surface/scar feature is claimed from RFC-007's
  reserved block (§2, indices 107–119), append-only, never a new encoding this RFC defines. All
  integers — the D-class rules above are what make any future hand-off safe across machines.
- **Episode reproducibility.** A training episode seeds the room chunk's D3 stream from
  `(room_key, episode_index)` instead of wall-derived tick; with D1 drift and D2-free rooms
  (no external field sources), an episode replays exactly. This is required for debugging
  reward regressions, not just niceness.
- **Training placement.** Dojo/training rooms are pinned to the leader
  (`Require<Trusted>`, `Priority<2>` per ARCHITECTURE §7) and are always active-tier; the
  LOD table's "fields dropped on sleep" can therefore never truncate an episode. Training
  must still tolerate *pauses* (drain-budget preemption) — episodes end on in-world
  conditions, never on elapsed real time.
- **One policy per archetype** (10–15 total) is unaffected: battlefield state enters the
  observation, not the policy count. The earthquake penalty applies to the agent's own
  ranged attacks too (weather, not a buff). Note the honest v1 scope: the only quake author
  is the melee Samurai, and no ranged archetype shares its room, so "hold fire during
  quakes" is not a v1 training claim — the property is stated so a future ranged quake-user
  or multi-boss room inherits it, the kind of legible emergent behaviour the feature exists
  to enable.
- **First user:** the Giant Samurai (first RL boss) with the half-health quake phase;
  its directional Attack L/R poses carry the slam telegraph (RFC-005/006).

## 7. Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001** | The Persist phase is the only player-side creator of zones/patches; ability records name a patch/zone/field product by id. Pipeline phases and counterplay live there. |
| **RFC-002** | Hazard zones and kBurning apply *statuses/build-up*; the status machine, override rules, and tint rendering are RFC-002. This RFC only says *when* a zone fires (each active tick, occupants only). |
| **RFC-003** | Coefficient tables (friction, grip, knockback multipliers, conductivity, flammability-by-material) are RFC-003 data; `Surface` values here are the row keys into them. |
| **RFC-004** | Everything with HP/collision/team (spike, pillar, totem, destroyable meteor) is a CombatEntity there; it obeys this RFC's tick order, LOD table, D-classes, and view budget. RFC-004 §8 also **owns the terrain scar layer** (`ScarKind`: cracked/rubble/crater/scorched, caps, heal clock, escalation ladder) — this RFC's `Surface` set is disjoint (burning/mud/ice) and only *stamps* scars through RFC-004's mechanism (burn-out → `kScorched`; heavy Rock impact → the ladder). Cross-chunk entity summaries (seam coverage) are RFC-004's. |
| **RFC-005** | Boss authoring references field states by `FieldKind` + parameters; phase triggers (half-health quake) are authored there. |
| **RFC-006** | Camera shake, telegraph tremble, patch/field visuals (tint + FX overlay only — monsters have no attack frames), and per-kind FX lifetimes are RFC-006. This RFC's render-only columns are inputs to it. |
| **RFC-007** | Feature-vector observation/action concerns; RFC-007 §2 is the sole normative F4 tensor contract (no separate BossObs schema exists). §6 fixes only *what* the battlefield exposes (Surface values, field state, hazard-circle entities via RFC-004); RFC-007 owns how — or whether — each is packed. |
| **RFC-008** | The JSON/YAML schema for patch chains, zone kinds, and field parameters; the tables in §4.2–4.3 are the v1 content of that schema. |
| **RFC-009** | Impact thresholds that trigger surface transitions, Burn DoT rates, and the `M_outer` slot/clamp into which this RFC's deterministic per-mille accuracy multiplier is folded (§4.4 there — no roll, no RNG). |

## 8. Asset & Engine Constraints Honored

- **Walk-only monsters (66 sheets, zero attack frames):** nothing here requires monster
  art. Patches and fields render as ground tints/overlays and FX at positions; monster
  interaction with hazards shows as RFC-002 status tints.
- **FX lifetime truncation (the kEffectLife=6 audit gap):** already fixed in-tree as the
  per-kind `effect_life_of` table (Earth = 14 frames plays fully). This RFC *requires*
  per-kind lifetimes for any new battlefield FX kind and caps sim-side effect life at
  `kMaxEffectLife = 14`; longer presentations must loop render-side (RFC-006), never
  extend sim records.
- **No Magic/* FX family or spinning-projectile sheets packed yet:** v1 field/patch visuals
  use only currently packed strips plus tints (umbrella §14–15's reuse doctrine). The
  quake's visual is shake + tremble + existing Earth FX — no new art on the critical path.
- **Elements: exactly Fire/Ice/Rock/Thunder.** The `Surface` and trigger tables reference
  no other school. Plant/Water/etc. remain icon-only future work.
- **Player kit = basic attack + two abilities:** unchanged; no field-creating player
  ability, no hotbar growth.
- **RL shortlist respected:** field states are authored for Samurai (first), GiantBamboo;
  nothing here depends on Dragons or idle+hit-only bosses. 10×7 rooms fit the 32-tile
  radius cap trivially (single-chunk, no fan-out).
- **1024×1024 + LOD:** every record is deadline-based (L-2), so background/sleep are
  correct by construction; caps keep the copied view small; active set is O(players).
- **First-node leader, no VPS:** authority is the existing chunk single-writer +
  trusted-actor pattern; no new trust tier, nothing persisted, replication stays
  snapshot-copy cheap.
- **Chill guardrail (GAME.md §0):** §4.6 — no ambient fields, bounded decay, fireproof
  claims, nothing simulated at a player's expense off-screen.

## 9. Open Questions

1. ~~**Zone unification.**~~ **Resolved by RFC-004 §2b:** the existing `Zone` (wet/smoke)
   neither folds into a battlefield-record family here nor stays its own array here — it dies
   entirely, replaced by RFC-004 `CombatEntity` kinds (`kSmokeCloud`/`kWaterPool`/`kFirePatch`)
   with their own storage, eviction, and LOD rules (RFC-004 §1/§9). Battlefield records in this
   RFC are patches and fields only.
2. **Ice bridges.** Invariant P-2 forbids combat patches from changing walkability, which
   kills "freeze the pond and cross it" as a *combat* trick (it survives as the P7 seasonal
   mechanic). Is a narrow exception worth a scheme where walkability-changing patches are
   restricted to interior rooms (no flow field, no cross-node pathing)? Deferred; needs a
   pathing owner's sign-off.
3. **Cross-chunk fire.** Spread stops at chunk borders in v1. A fire front dying at an
   invisible line is readable as "it burned out" at 12-patch scale, but if playtests notice,
   the fix is a bounded `IgniteRequest` message to the neighbour — decide after P3's seam
   summary work ships.
4. **Tier thresholds.** The 1 Hz background tier is specified here but not yet implemented
   (today only publish-rate LOD exists). Which phase lands tick-tier LOD, and does
   battlefield ship before it (safe: today's world is all-active) or after?
5. **View budget vs. P6 networking.** 1 KB of battlefield per view is fine in-process;
   once views stream over TCP to clients, does the budget need halving, or does interest-set
   delta encoding (P6) absorb it?
6. **Scorched/mud gameplay depth.** The scorched *scar* (RFC-004 §8's `kScorched`, which
   burn-out stamps) is cosmetic-plus-coefficients in v1. Whether it should slow regrowth of
   forage or interact with farming is a design question that must pass the GAME.md §0 gate
   (it must never punish an absent player) — parked until P4 economy exists, and RFC-004's
   to implement.

## 10. Non-goals

- **Destroyable battlefield objects** (spikes, walls, shootable meteors) — RFC-004.
- **Damage numbers, resistances, build-up thresholds** — RFC-009.
- **Status effects on creatures** (Wet, Burn, Freeze ladder) — RFC-002.
- **Weather and seasons** (rain, frozen lakes, MapDirector broadcast) — P7 systems; this
  RFC only guarantees its records compose with them (weather may create kMudded via the
  same trigger table when P7 lands).
- **Telegraph art, shake implementation, FX packing** — RFC-006.
- **Skill/ability authoring schema** — RFC-008; **boss phase scripting** — RFC-005.
- **Client-side prediction or rollback** of battlefield state; **PvP** interactions.
- **Persistence** of battlefield state across world reloads (deliberately ephemeral).
- **Multi-part bosses (Dragons)** and any second batch of field kinds (blizzard,
  sandstorm) — future work, mentioned only to reserve the `FieldKind` enum space.

## Review Record

Votes: Opus — revise. Sonnet — revise. All mustFix proofs verified against RFC-004 §8,
RFC-009 §4.4, GAME.md §6, ARCHITECTURE.md §4, BossObs v2, and `chunk_actor.hpp`. Applied:

- Scar ownership: `Surface` shrunk to burning/mud/ice; cracked/rubble/crater/scorched ceded
  to RFC-004 §8, which this RFC now only stamps (§1, §3, §4.1, §4.2, §4.6, §7, §9).
- Deleted "six-ability kit"; §4.3 states basic attack + two abilities only.
- Fire spread made array-layout-independent: canonical `(tx,ty)`-sorted candidates on a
  salted stream (§4.2), satisfying §5's byte-exact ledger test.
- §5 D3 mandates one salted `Rng` instance per consumer; departure from
  `chunk_actor.hpp:123`'s shared stream flagged.
- Accuracy penalty is now a deterministic per-mille `M_outer` contribution per RFC-009 §4.4
  (no miss roll), classed D1; ranged double-penalty called out as intended.
- §6 defers observation layout to the frozen BossObs v2 schema (`surface[49]` bytes),
  contributing only the value set; Squids hold-fire example rescoped as non-v1. (Superseded —
  see Reconciliation below: that schema was an early draft, never shipped as a file, and is
  retracted by RFC-007.)
- Village-raid carve-out: raid chunks pinned active-tier for the raid's duration (§3, §4.6
  rule 4, §4.7, §4.8), matching GAME.md §6's off-screen raid resolution.
- `kMaxFieldRadius` 40 → 32 (= chunk width) so the 3×3 fan-out covers the radius.
- Slept tier = IdleTimeout destruction; all records expire before sleep is reachable, wake
  = baseline (§4.7). Stronghold fields fully parameterised, capped by `kMaxFieldTicks`
  (§4.3). §4.8 budget line relabelled; RainCall marked P7.

Unresolved: none outstanding from this file's own review.

Reconciliation: §6 and §7's RFC-007 row repointed from the nonexistent `RFC-010-rl-observation-
schema.md` (never a real file — an early "BossObs v2" draft, retracted in full by RFC-007) to
RFC-007 §2's actual ray/ground-class/entity-slot encoding; since that encoding has no per-tile
surface/scar plane, the gap is now RFC-007's open question rather than an invented grid here, per
RECONCILIATION.md ruling 6. §3's designer-tools table, §4.4's tick order (`step_zones` retired),
§4.5's replication table, §4.7's LOD table, and §9 OQ1 brought into conformance with RFC-004 §2b's
hazard-circle-entity/scar ownership split (Zone dies; it is not "migrated to end_ms semantics")
per RECONCILIATION.md ruling 11. IMPLEMENTATION_MAP's status table and labels refreshed
separately, per RECONCILIATION.md ruling 12.
