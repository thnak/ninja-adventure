# RFC-021: World Structure, Map & Wayfinding

> Status: **Accepted (revised after review) — partially superseded by RFC-022** (RFC-022 narrows
> this RFC's implicit "the overworld is the game's only map" framing and folds its Dungeon-mouth /
> Realm-gate marker rows into one `PortalKind` with a `RealmFlavor` payload; every geometry,
> worldgen-invariant, wayfinding, and discovery-model clause below stands unchanged, cited by number
> — see RFC-022 §Relationship to RFC-021 for the clause-by-clause ruling)
> Design canon: [GAME.md §3](../GAME.md), [§4](../GAME.md), [§6](../GAME.md), [§12](../GAME.md) ·
> [ARCHITECTURE.md §4](../ARCHITECTURE.md), [§8](../ARCHITECTURE.md) · [ROADMAP.md](../ROADMAP.md) P1/R5–R8 (shipped), P3/P4/P8 (pending)
> As-built source grounding: `src/world/tiles.hpp`, `src/world/worldgen.hpp`, `src/world/village.hpp`,
> `src/world/player_actor.hpp`, `src/ui/screens.hpp`
> Sibling RFCs this run: RFC-019 (Progression & Skills), RFC-020 (Mission & Quest System)
> Depends on: RFC-001 (ability system — mount/combat exclusivity references its targeting/kit model
> only tangentially), none of the accepted combat set is load-bearing here
> Depended on by (proposed): RFC-011 (Combat HUD — the in-game raid-warning HUD element reads the
> data this RFC defines), RFC-014 (Instance & Realm Lifecycle — consumes the gate placement contract
> §3.5), RFC-015 (Client Replication — implements the wire protocol this RFC only budgets)

---

## Summary

This RFC codifies the shipped 1024×1024 seamless overworld as a normative contract: the five
concentric difficulty rings with their exact as-built geometry (square, noise-wobbled, area-corrected
— not the illustrative circular bands GAME.md §4 sketches), the deterministic worldgen invariants
(placement order, keep-out rules, no-build zones, cross-platform bit-exactness), and the two systems
that make a map this large *legible* and *reachable*: the player-facing Map screen (`M`) and the
wayfinding/travel layer (mounts, the village waypoint network GAME.md §3 requires, and roads).

It is a **records-and-operationalizes** spec, not a green-field design: every number that GAME.md or
the shipped engine already fixed is recorded here with its source; every number this RFC introduces
(waypoint costs, marker refresh rates, discovery radii) is new and marked **(tunable)**.

---

## Motivation

Three shipped facts create an obligation this RFC discharges:

1. **The ring geometry GAME.md §4 describes in prose does not match what worldgen actually builds.**
   `tiles.hpp`'s own comment records why: a naive circular-radius model (GAME.md's "0–120,
   120–220, …") wastes the map's four corners and gives Wasteland 44.6% of the area against
   Meadow's 4.6% — backwards, since Meadow is the ring a chill player should be able to live in
   forever. The shipped `ring_of()` uses **Chebyshev (square) distance** with **area-corrected**
   thresholds. Two engineers reading only GAME.md §4 would build two different worlds. This RFC is
   the single normative source for ring geometry from here forward.

2. **1024×1024 was shipped with a hard requirement GAME.md §3 states three times and the engine only
   half-delivers today.** Mounts exist (`SetMounted`, `kMountSpeed`). The **village waypoint
   network** GAME.md §3 calls "phải nằm trong giai đoạn dựng thế giới, không để dành" (must ship
   with the world stage, not deferred) does not exist in any form. Without it, a player who settles
   two rings out from a friend faces the walk time table in §4.4 on every visit — not a chill
   number.

3. **The Map screen (`M`) is specified in GAME.md §12 and appears in no code.** `src/ui/screens.hpp`
   has `kJournal`, `kCharacter`, `kOptions` — no `kMap`. At 1024×1024 with ~50 named villages, ~22
   forts, and (eventually) mines and realm gates, a player without a map screen cannot use the world
   GAME.md §4 designed. This RFC is the first complete spec for that screen.

A fourth, cross-cutting motivation: **every number in this RFC must survive the tone guardrail.**
A map or wayfinding feature that quietly punishes not checking in (a waypoint that decays, a raid
marker with a hidden countdown, fog that regrows) would violate GAME.md §0 exactly as directly as a
village that rots. §8 of this RFC argues, clause by clause, why nothing here does that.

---

## Guide-level Explanation

### For a player

You wake up in open field, ~39 tiles from the nearest village (GAME.md §6b, already shipped). You
have no map yet — the world is a grey silhouette with only your own position marked. You walk to the
village; arriving reveals it, permanently, on your Map screen (`M`): its name, its tier, and its
services. From here on, every village, fort, mine mouth, gate and dungeon entrance you physically
visit is added to your map the same way. Nothing you haven't seen is shown — except a coarse *rumor
layer* (§5.2) that sketches the ring/terrain silhouette (which biome, roughly how far) just past the
edge of where you've been, never a specific village, fort, or other feature — those stay fully hidden
until you physically visit them (§5.2, §5.3) — so you're never navigating one screen's radius blind,
but you also never learn a village exists before you find it.

Press `M` any time (it doesn't pause a shared world — see `Screen::kPlaying` semantics already
established). You see:

- Your position and facing.
- Every claim, fort (with its visible training "generation" number), dungeon mouth, mine mouth,
  gate, and teammate base you've discovered.
- Roads, drawn as the connective tissue between villages — the legibility layer GAME.md §4
  advertises ("further out = harder" needs no legend, and roads are the second half of that: they
  tell you *where to walk* to test that claim).
- Any active raid warning within your discovered area (§3.4) — a scout has run to a village and the
  raid is one game-day out. You can choose to go. Nothing forces you.

Getting around: on foot you cover the map edge-to-edge in ~2m51s (already measured, `kPlayerSpeed`).
Whistle for a mount (`R`, already shipped) and it drops to ~1m33s, but you can't fight while riding —
dismount before a fight finds you. For real distance, every village at tier ≥2 you've *visited*
becomes a **waypoint**: open the Map, click a discovered tier-≥2 village, pay a flat cost, arrive
instantly. No cooldown that punishes overuse, no unlock quest, no daily cap — the constraint is
entirely "have you been there and can you pay" (§4.3).

Gates you've found show on the map as gate markers with a realm-type icon (rest vs. challenge) —
never the realm's internal layout, because the realm hasn't loaded for you and doesn't exist as
geometry until you step through (ARCHITECTURE.md §4).

### For a designer

The five-ring table (§2.1) is your placement authority: "how far out do I put this POI" has one
answer, the ring the design calls for, and the geometry is a pure function of seed and tile
coordinate — no hand-authored boundary to keep in sync. The prefab/POI placement pattern already
shipped in R8 (`kPoiTable`: prefab id → ring → allowed terrain → jitter cell/gap → cap) is the
template this RFC reuses for the two features worldgen doesn't build yet: mines and realm gates.
Nothing here asks you to write a new placement algorithm; it asks you to add rows to a table whose
shape already exists in the codebase.

### For an engineer implementing the Map screen fresh

You need: (1) the discovery/fog model (§3.2) — a per-player bitset over a coarse grid, set on visit,
never cleared; (2) the marker taxonomy (§3.3) — a fixed enum plus a small struct per marker kind;
(3) the replication contract (§3.6) — what the client is entitled to receive and at what refresh
rate, budgeted against the interest-set beacon mechanism P2 already ships (`PlayerBeacon`,
`player_actor.hpp`), with the actual wire encoding deferred to RFC-015 (proposed); (4) the waypoint
state machine (§4.3) — five states, no timers.

---

## Reference-level Design

### 1. World frame

| Constant | Value | Source |
|---|---|---|
| Map edge | 1024 tiles | `kMapTiles`, `tiles.hpp` |
| Chunk edge | 32 tiles | `kChunkTiles` |
| Chunk actors (overworld) | 1024 | `kMapChunks²` |
| Centre | (512, 512) | half of `kMapTiles` on each axis |
| Ring count | 5 | `Ring::kCount`, `tiles.hpp:114` |

All ring, road, village, fort, prefab and (proposed) mine/gate placement is a pure function of
`(world_seed, gx, gy)` or a small deterministic derivative (seeded jitter grids). This is the
property ARCHITECTURE.md §2c calls out as load-bearing: **everything derived from seed must match
byte-for-byte across GCC/Linux and MSVC/Windows.** Creature *migration counts* are the one
documented exception (order-dependent on message scheduling, not seed-pure) — worldgen, terrain, and
all placement in this RFC are **not** in that exception class and must diff to zero between
toolchains, exactly as R6/R7 already verified for villages/forts/roads.

### 2. The five rings

#### 2.1 As-built geometry (normative)

Ring membership is Chebyshev (square) distance from the centre, normalized to `[0, 1+]` by the map
half-width (512 tiles), with the boundary itself perturbed by 2-octave value noise so it reads as a
coastline, not a target:

```
half = kMapTiles / 2                              // 512
dx = (gx - half) / half
dy = (gy - half) / half
r  = max(|dx|, |dy|)                              // Chebyshev, not Euclidean — see rationale below
r += (fbm(seed ^ WOBBLE_KEY, gx, gy, scale=70) - 0.5) * 0.16   // wobble the RADIUS, range ±0.08
ring = first i such that r < kRingEdge[i]
```

`kRingEdge = {0.5099, 0.6928, 0.8246, 0.9274, 1.01}` (`tiles.hpp:134`) — these are the square roots
of the **cumulative area** each ring should own (26% / 22% / 20% / 18% / 14%), not evenly spaced
radii. An evenly-spaced version was tried and measured wrong: it gave Meadow 4.6% of the map and
Wasteland 44.6% — exactly backwards, since Meadow must feel like "a whole country" for a player who
never wants to leave it (GAME.md §4).

| Ring | Name | Edge (fraction) | Edge (tiles, cardinal) | Cumulative area | Biome | Hazard | Adaptation | Forts |
|---|---|---|---|---|---|---|---|---|
| 0 | Meadow | 0.5099 | ~261 | 26% | Meadow | none | starting region | none |
| 1 | Forest | 0.6928 | ~355 | +22% (48%) | Forest | undergrowth to clear; monsters hide | better axe; lanterns | sparse, weak |
| 2 | Wetland | 0.8246 | ~422 | +20% (68%) | Swamp / Desert (seed-side split) | pilings/poison (swamp), crop death (desert) | pilings + filter (swamp); wells + irrigation channel (desert) | moderate |
| 3 | Snow | 0.9274 | ~475 | +18% (86%) | Snow mountains | crops freeze, movement slows | hearths; crampons | dense |
| 4 | Wasteland | 1.01 | ~517 (map edge) | +14% (100%) | Ashlands | cannot farm | Essence-cleansing only | densest, strongest |

**Reconciling with GAME.md §4's prose table.** GAME.md's "0–120 / 120–220 / 220–320 / 320–420 /
420+" radii are the *design intent* (radiate difficulty from the centre, keep Meadow generously
large, degrade smoothly outward) stated before the corner-waste and area-skew bugs above were
measured and fixed in `tiles.hpp`. This RFC's table **is** that same design intent, corrected to
what the engine actually enforces. The *ordering*, the *biome list*, the *hazard/adaptation table*,
and "Meadow is deliberately oversized" all carry over unchanged; only the exact boundary numbers do
not, because GAME.md's numbers were never load-bearing — they illustrate a curve, and the curve the
engine draws is the one above. Any future edit to `kRingEdge` supersedes this table by definition;
this RFC's contract is "ring membership is `ring_of()`," not the specific eight numbers.

Cardinal-axis edges above are illustrative (they use `r == fraction` exactly on the x or y axis
before wobble). Two things move the true boundary a player experiences:

- **Diagonal stretch.** Chebyshev rings are squares, not circles: along the diagonal (45°), the same
  fraction covers a straight-line distance of `fraction × 512 × √2` — e.g. Meadow's corner extends to
  ≈369 tiles from centre along the diagonal versus ≈261 along an axis. This is intentional (§8 of
  ARCHITECTURE.md: "square rings use the whole map") and a player who walks a diagonal line will
  cross ring boundaries later than one who walks a cardinal line from the same start.
- **Wobble.** The `±0.08` normalized-radius perturbation is a constant additive term in `r`-space, not
  a function of `r` or of distance from the centre — `fbm(seed ^ WOBBLE_KEY, gx, gy, scale=70)` depends
  only on `(gx, gy)`, and the result is added to `r` *before* the threshold compare (`tiles.hpp:837-838`).
  Converted to tile space by the same conversion the ring edges themselves use, the wobble is
  **≈±41 tiles along a cardinal axis / ≈±58 tiles along the diagonal (×√2), at every ring boundary
  including Meadow's inner edge near the centre** — it does **not** taper toward the centre. No two points on a ring boundary are the same distance
  from the centre; the boundary has headlands and bays of constant scale everywhere, never a straight
  edge and never one that sharpens near (512, 512).

#### 2.2 Village/fort counts (as-built, illustrative not contractual)

Worldgen places villages, forts (strongholds), roads and prefab parcels once at world creation,
deterministically from `world_seed`. Recorded counts across the shipped history (`ARCHITECTURE.md
§2c`, `ROADMAP.md` R6/R7 determinism checks) have varied build-to-build as placement rules were
tuned: 49–51 villages, 22–27 strongholds. This RFC does not pin an exact count (that is worldgen
tuning, not a game contract) — the contract is: **the count and every placement is deterministic per
seed, and Linux/GCC and Windows/MSVC produce the identical count and the identical set of tile
coordinates.** This has been verified through R0–R8 and must continue to hold for every extension
this RFC proposes (mines, gates).

### 3. Deterministic worldgen invariants

#### 3.1 Placement order (as-built, normative — do not reorder)

```
1. Village SITES   — jittered grid, cell 112 tiles, 9×9 = 81 candidates → filtered to viable land
2. ROADS           — each village linked to its 2 nearest neighbours; roads are carved BEFORE
                      buildings exist, so buildings grow around a road that has already committed to
                      its path — a building can never be "in the way" of a road
3. Village BUILD   — square (kPath) at centre, then houses/wall/gates around it (village.hpp)
4. STRONGHOLDS     — jittered grid, cell 124 tiles; the ONLY placement class allowed to be dropped
                      rather than moved: a candidate within kStrongholdKeepOut=46 tiles of any
                      village wall is rejected outright, never relocated
5. PREFAB PARCELS  — author-composed set-pieces (R8: camp_clearing, forest_cottage, snow_pond,
                      south_orchard, …), placed last because a parcel rejects any tile already built
                      on — it is the last reader of the placement overlay before the index freezes
```

**Why this order, recorded because it is easy to get backwards and silently wrong:** placing
strongholds before villages once meant a stronghold could land on the only buildable ground in a
region and silently delete a village candidate — a hole in the map nothing in-game explains. Placing
roads before buildings for the same reason: a road that must dodge existing houses can also cut
through them. Ordering removes the class of bug instead of validating against it.

This RFC extends the pipeline with two more stages, both **proposed** (P4 scope, not yet built) and
both required to slot in by the same rule — after everything they must not overwrite, before nothing
depends on them:

```
6. MINES           — inserted AFTER strongholds (mines sit in rock/mountain terrain the ring
                      formula already places; they do not compete for village-quality land the way
                      strongholds do, so they do not need the village keep-out)
7. REALM GATES     — inserted LAST, after every other placement overlay write, because a gate is a
                      single-tile structure with the loosest terrain requirement (any dry, buildable
                      tile) and should never block something with a tighter requirement
```

**Road spurs to mines and gates are NOT step 2.** Step 2's roads are carved before any building
exists, which is why §3.1's "a building can never be in the way of a road" holds for them. Mines
(step 6) and gates (step 7) are placed *after* villages, strongholds, and prefabs have already
frozen the placement overlay, so their approach spurs cannot reuse that guarantee — "the same way a
village gets a road" (an earlier draft's phrasing) is false. Instead, each spur is carved in its own
pass, immediately after its structure is placed, under a different, explicit rule: the pass treats
every already-placed building/overlay tile as impassable and routes the shortest legal path to the
nearest existing road (constrained pathfind over the frozen overlay); if no route under a tunable
tile budget exists, the candidate mine mouth or gate site is rejected and re-rolled rather than the
spur being allowed to overwrite a building. This reintroduces a route-around cost the step-2 roads
never pay, but it does not reintroduce the *bug* step 2 eliminates (a road silently cutting through a
building) — a rejected-and-re-rolled site is visible in the determinism logs the same way any other
placement rejection is (§2.2), never a silent overwrite.

#### 3.2 No-build zones

| Zone | Rule | Why | Status |
|---|---|---|---|
| Fort (stronghold) | no player building within **radius 12 tiles (fixed by GAME.md §4 — not tunable; this RFC records the number, it does not introduce it)** of the fort's footprint | prevents sealing a monster spawn shut, which would kill the raid loop this RFC's §3.4 and GAME.md §6 depend on; a *deliberate* attack on the fort is still allowed and is the one documented way a village can lose ground (GAME.md §6) | proposed — GAME.md fixes the number, no-build enforcement is P3 scope, not yet in code |
| Dungeon interior | no building anywhere inside | it's an instanced realm, not overworld ground — building state has nowhere to persist between instance lifecycles (ARCHITECTURE.md §4) | as-built pattern (interiors already forbid building; realm dungeons will inherit the same rule) |
| Mine (overworld mouth + shaft instances) | no building; extraction machines only | resource access must stay contested/visitable by everyone, not privately walled | proposed, P4 |
| Village wall footprint + the 3-tile street/gate apron `village.hpp` already carves | implicitly no-build — it's worldgen-owned overlay | keeps `terrain_of`'s overlay-then-noise contract intact (ARCHITECTURE.md §8) | as-built |

#### 3.3 Roads aimed at gates, not centres (as-built, R6)

`gates_of(centre, tier)` is a pure function producing exactly four village-wall gate positions
(north/south/east/west). Roads are carved to terminate **at a gate**, never at the village centre —
carving toward the centre first and discovering the wall later was the R6 bug (23/51 villages had a
gate visible but not walkable because the road stopped short and the wall closed over it). This RFC
adopts the same *terminus* rule for the two proposed placement classes: a road spur to a mine mouth
terminates at the mouth tile; a road spur to a realm gate terminates at the gate's designated approach
tile **(tunable — one tile south of the gate, matching the "approach from the south" convention every
building in this pack already uses, per R5/R7's door-column findings)**. The *collision* handling for
these two spurs — how each routes around structures already placed by the time it is carved — is a
separate rule from this terminus rule and from the step-2 village-road guarantee; see §3.1 steps 6–7.

Note: `gates_of`'s "gate" is the **village wall entrance** — a different concept from the **realm
gate** (§3.5) that leads to an instanced realm. Both are called "gate" in casual language and in
GAME.md's Vietnamese ("cổng"); this RFC uses "village gate" and "realm gate" throughout to keep them
unambiguous, since they now share a placement-order relationship (§3.1 step 7 explicitly runs after
village gates already exist).

#### 3.4 Raids (display contract only — mechanics owned elsewhere)

GAME.md §6 defines village raid mechanics: ~30%/game-month base chance per village (a probability
product of ring, nearby-fort behavior, village defense, and an anti-streak "grudge" term), a scout
announcement exactly one game-day ahead, and tier-≥2 villages defending themselves whether or not the
player shows up. **This RFC does not own or re-specify any of that math** — no RFC currently does
(it is P3 scope in ROADMAP.md, unassigned to a numbered RFC in this batch). What this RFC owns is
narrower: **once a raid warning exists, how it appears on the Map (§4.4)**. The shipped placeholder
(`map_director.hpp`: 10%/fort/night, no scout, no announcement) is a P1 stand-in for the GAME.md §6
model and is expected to be replaced, not extended, when the village-tier system ships.

#### 3.5 Realm gates (proposed, P4)

A realm gate is a **fixed overworld structure** (GAME.md §3: "cổng là công trình cố định trên mặt
đất"). Stepping onto it allocates an instance (ARCHITECTURE.md §4's `InstanceManager` flow) and
teleports the player and their group into it. No realm-to-realm gates exist; every gate connects
overworld ↔ one realm.

Placement rule, following the R8 `kPoiTable` pattern (prefab id → ring → terrain mask → jitter
cell/gap → cap), all **(tunable)**:

| Field | Rule |
|---|---|
| Realm type | rest (fishing lake, hot springs, cloud isles — no combat) or challenge (dungeon, mine-adjacent trial) |
| Ring bias | ARCHITECTURE.md §8: "rải đều mọi vòng — cõi nghỉ ở gần, cõi thử thách ở xa." Rest-gate jitter grid weighted toward rings 0–2; challenge-gate jitter grid weighted toward rings 2–4. Neither type is ring-exclusive — a chill player deep in Meadow can still find a nearby challenge gate and choose not to use it |
| Terrain requirement | any dry, buildable tile (loosest of any placement class — §3.1) |
| Jitter cell | 160 tiles (tunable) — sparser than village (112) or fort (124) grids; gates are meant to be a discovery, not a landmark you trip over |
| Keep-out from villages | none required — a gate 40 tiles from a village wall is a feature (a village that "has a gate nearby" is a distinct village identity, matching GAME.md §6's regional-flavor goal) |
| Per-instance atlas | each realm loads its own atlas on entry, frees it on exit (ARCHITECTURE.md, GAME.md §3) — out of scope for this RFC; owned by RFC-014 (proposed) |

#### 3.6 Mines (proposed, P4)

Mines sit in rock/mountain terrain the existing ring-biased terrain noise already produces (denser
at higher rings — GAME.md §8 crafting table). Placement follows the same jitter-grid-with-rejection
pattern as strongholds:

| Field | Rule |
|---|---|
| Jitter cell | 140 tiles (tunable) |
| Terrain requirement | majority stone/rock tiles in the candidate footprint (mirrors `buildable_site`'s dry-land majority check, inverted for rock) |
| Ore tier by ring | Copper (rings 0–1) → Iron (ring 2) → Steel (ring 3) → Mythril (ring 4), each additionally gated by mine **depth** (deeper instance floors), per GAME.md §8. Mine-depth access and instance mechanics belong to RFC-014 (proposed, Instance & Realm Lifecycle); the crafting/equipment material-tier table belongs to RFC-018 (proposed, Loot, Essence & Reward Tables). This RFC places the mine mouth only and records the ring→ore-tier correlation as a worldgen fact, not a claim on either RFC's scope |
| Multiple mouths per ore band | yes — a single ring is not one mine, worldgen places several mouths per ring band at the jitter spacing above |

### 4. Wayfinding and travel

#### 4.1 Speeds (as-built)

| Mode | Speed | Edge-to-edge (1024 tiles) | Source |
|---|---|---|---|
| On foot | 6.0 tiles/s | ≈170.7s ≈ **2m51s** | `kPlayerSpeed`, `tiles.hpp:1089` |
| Mounted | 11.0 tiles/s | ≈93.1s ≈ **1m33s** | `kMountSpeed`, `tiles.hpp:1090` |

The on-foot figure matches GAME.md §3's own edge-to-edge measurement (85s / "2 phút 51", diagonal
~4 minutes); GAME.md §3 states no mounted figure, so the 1m33s mounted number above is this RFC's own
derivation from the shipped `kMountSpeed` (1024 / 11.0 ≈ 93.1s). Together they confirm the mount speed
already shipped satisfies the "mounts are not optional at this scale" requirement without further
tuning.

#### 4.2 Mounts (as-built — recorded here for completeness, not re-specified)

- Toggled by `R` (`client_main.cpp`); server state is `SetMounted` on `PlayerActor`.
- **Cannot fight while mounted** — `player_actor.hpp` gates both the melee and ranged attack
  handlers on `!mounted_` (lines 234, 292). This matches GAME.md §2's "R: nhanh hơn, không đánh
  được" exactly; this RFC does not change it.
- Auto-dismounts on death (`mounted_ = false` in the death handler).
- No stamina cost, no cooldown, no unlock gate visible in code today — mounting is unconditional.
  This RFC does not propose adding one (§8: any future unlock gate must not be a clock).

#### 4.3 The village waypoint network (proposed — GAME.md §3's required, not-yet-built system)

GAME.md §3 states plainly that at 1024×1024, "Ngựa (hoặc lướt kiểu ninja) + điểm dịch chuyển ở làng
phải nằm trong giai đoạn dựng thế giới, không để dành" — mounts *and* village teleport points must
ship with the world stage, not be deferred. Mounts shipped (§4.2); the waypoint half did not. This
RFC specifies it.

**Eligibility.** A village becomes a waypoint candidate for a given player when both hold:

1. The village is **tier ≥2** (GAME.md §6: tier 2 is where a respawn point exists; a waypoint
   network riding on the same "this village has real infrastructure" threshold is consistent, not
   arbitrary).
2. The player has **physically visited** it at least once — defined as exactly one predicate, used
   consistently everywhere this RFC needs "has this player seen X": the Feature-detail visit-grade
   proximity trigger (§5.2 — within ~8 tiles of the village, tightened from the beacon's ~80-tile
   publish radius). No waypoint can be unlocked from a menu, a quest reward, or by proxy through a
   teammate's visit; visiting is a chill activity in itself (walking through a village *is* the game,
   per GAME.md §1), not a chore gating fast travel.

**First-login home village.** GAME.md §6 / ROADMAP.md P3 ("Chọn làng xuất phát lúc đăng nhập lần
đầu") make the player's chosen starting village their first respawn point. This RFC states
explicitly, as the one documented exception to rule 2 above: choosing a home village at first login
seeds it as `DISCOVERED` immediately — it appears on the Map from the very first session, no walk
required — but does **not** grant `USABLE` for free; the tier-≥2 gate in rule 1 still applies exactly
as it would for any other village, and a fresh home village typically starts below that tier. Every
other village on the map still requires the physical-visit trigger above; this exception exists only
because the player never chose to "discover" their own starting point, they chose to start there.

**State machine.** A waypoint, once eligible, is eligible **forever**. There is no re-verification,
no distance decay, no "waypoint goes stale if unused." Two states only:

```
UNDISCOVERED  ──(the visit-grade proximity trigger above, or first-login home-village
                  selection — see above)──▶  DISCOVERED
DISCOVERED    ──(village reaches tier ≥2, checked continuously — a discovered tier-1 
                  village that grows to tier 2 becomes usable without a second visit)──▶  USABLE
```

A village discovered below tier 2 sits at `DISCOVERED` (shown on the Map, not selectable as a
waypoint target) until its tier rises — village tiers only rise or stall (GAME.md §6), never fall
except the single deliberate-provocation exception, so `USABLE` can be lost only in that same
exceptional case (a village provoking a fort into a retaliation raid severe enough to drop it below
tier 2 — expected to be vanishingly rare and is itself a player-caused, not clock-caused, event; see
§8).

**Cost, all (tunable):**

| Parameter | Value | Rationale |
|---|---|---|
| Base fee | 15 Stone or 10 Wood (player's choice of resource) **(tunable)** | a flat, affordable material cost — not gold, not a currency this RFC would have to invent, and not free (a free unlimited teleport network makes roads and mounts pointless; GAME.md §4's whole "further = harder, further = a trip" framing needs travel to cost *something*) |
| Distance scaling | none | a flat cost regardless of ring-to-ring distance keeps the mental model simple ("waypointing costs a little wood") and avoids inventing a formula that quietly punishes the outer-ring players GAME.md §4 explicitly wants to reward for going out there |
| Cooldown | none | no timer of any kind between uses — see §8 |
| Same-map only | yes | waypoints connect points on the overworld only; a realm gate is a separate mechanism (§3.5) and waypoints never target realm interiors |
| Mounted while waypointing | irrelevant | teleport is an instantaneous position change (already-shipped `Teleport` verb, distinct from `MoveIntent` per R7), mount state carries through unchanged |

**Why no cooldown and no distance-scaled cost, said plainly for the reviewer:** a per-use timer is
the textbook shape of a hidden clock (GAME.md §0) — "you may travel again at 14:32" is exactly the
kind of thing the tone guardrail exists to forbid. A resource cost is not a clock; it's a one-time
choice the player makes at the moment they act, spends something they chose to gather, and has no
further consequence. That distinction — timers create obligation pressure, resource costs create a
one-off decision — is the same test §8 applies to every other mechanic in this RFC.

#### 4.4 Roads as the legibility layer (as-built + this RFC's map treatment)

Roads are already carved deterministically between nearest-neighbour villages (§3.1). This RFC adds
one rule for their map presentation: **a road segment is drawn on the Map screen only where both its
endpoints (or the segment's midpoint, for long inter-ring roads) fall within the player's discovered
area (§5.2).** Roads are not secret — walking along an undiscovered road reveals the tiles under foot
like any other terrain — but a road is not drawn as a line connecting two villages the player hasn't
found yet, because that would leak village *existence* (not their contents) ahead of discovery. This
is a soft rule, not a security boundary (any player could walk the road to find out anyway); it
exists purely so the Map screen reads as "what I've found," not "the whole world with question
marks."

### 5. The Map screen (`M`)

#### 5.1 Screen semantics

`Screen::kMap` is added to `src/ui/screens.hpp`'s enum alongside the existing `kJournal`,
`kCharacter`, `kOptions`. Like every non-blocking screen already there, opening the Map **does not
pause the shared world** — chunks keep ticking, other players keep moving, a raid you're watching for
keeps its one-day countdown running in world time. This is a direct consequence of the world being
persistent and multiplayer (GAME.md §11); it also means the Map is safe to open compulsively without
it becoming a way to "pause and think forever," which would be a soft clock-avoidance exploit rather
than a clock.

#### 5.2 Discovery / fog model

A per-player discovery bitset, **one bit per 8×8-tile cell** (128×128 cells covering the 1024×1024
map = 16,384 bits = 2 KiB per player — cheap to keep resident and to persist per RFC-016's, proposed,
save format). A cell's bit sets permanently the first time any tile inside it enters the player's
existing beacon/interest radius (i.e., the same proximity the simulation already tracks for LOD and
combat, `PlayerBeacon`, P2) — **discovery never requires an explicit "look at the map" action**,
walking past is enough, matching how the shipped world already treats "have you been near this chunk."

Two visibility layers, not one:

| Layer | What it shows | How it fills | Can it regress? |
|---|---|---|---|
| **Terrain fog** | the biome/terrain silhouette (which ring, rough biome colour) — never a marker | a bounded frontier: each **discovered** cell also reveals a fixed **"rumor" halo of the 3 nearest undiscovered cells (tunable)** around it, so the player always sees a little of what's just past the edge of where they've been — never a whole ring, however small a sliver of it has been discovered. The halo is computed at read time (a bounded radius-3 scan from set bits), not a second persisted layer — the 2 KiB bitset above (§5.2 opening) stays the single source of truth for what is discovered; the halo is always derived from it, never stored | no |
| **Feature detail** | the actual marker (village name/tier, fort, dungeon mouth, mine mouth, gate, teammate base) | only on physical visit — proximity via the beacon radius (walking within ~8 tiles, matching the beacon's 5×5-chunk = ~80-tile publish radius already used for interest, tightened to a visit-grade proximity check) | no |

**No regrowth, ever.** Once a cell's terrain fog is lifted or a feature is detailed, it never reverts
— not on logout, not after N days unvisited, not after a raid changes the village. This is the same
"chững, không tụt" (stall, never decay) principle GAME.md §6 applies to villages, applied here to
knowledge: the game never takes back something you learned. A village that takes raid damage still
shows on your map at its last-known tier until you revisit and see the current one — it is not hidden
or greyed out for being stale, because staleness-as-punishment is exactly the shape of pressure §0
forbids.

#### 5.3 Marker taxonomy

| Marker kind | Data shown | Refresh trigger | Discovery gate |
|---|---|---|---|
| **Claim** | owner name, boundary outline | on claim create/resize (event-driven, not polled) | visited once (own claims always shown; other players' claims shown once you've been near them, matching claim visibility elsewhere in the design) |
| **Fort (stronghold)** | position, and its **visible training generation** number ("Giảng Đường Hoả — Thế hệ 47", GAME.md §10) | generation number updates when a `NetworkCheckpoint` is published (event-driven) — no polling needed, this is already a rare, discrete event | visited once |
| **Dungeon mouth** | position, realm-type icon (challenge) | static once placed (worldgen is one-time) | visited once |
| **Mine mouth** | position, ore-tier icon of the shallowest exposed tier | static | visited once |
| **Realm gate** | position, realm-type icon (rest / challenge) — **never** the realm's internal layout | static | visited once |
| **Teammate base** | teammate name, hearth position | on hearth placement/relocation | automatic for anyone with build/view rights on that claim — no separate "visit the base" requirement, since a teammate sharing their base location is itself the discovery act |
| **Active raid warning** | target village, one-day-out countdown *displayed as remaining game-time*, not a ticking real-time number that could be misread as urgency (§8) | appears exactly when the scout announcement fires (GAME.md §6); disappears when the raid resolves or expires | visible to anyone with the target village discovered — this is the one marker type explicitly meant to travel past "your immediate area," since GAME.md §6's whole point is "rare for your home, frequent somewhere on the map" |
| **Village (general)** | name, tier, tier-appropriate icon | tier-change event | visited once |

Each marker kind is a small fixed-size struct (position, a kind tag, and at most one small payload
field like a generation number or tier) — no marker carries a text blob, a description, or anything
requiring localization lookup at replication time; display strings resolve client-side from the
kind+id, the same pattern `HudFeedback` already uses for XP-mote school names (`screens.hpp`).

#### 5.4 Map data cost under interest-set replication

Full wire protocol is RFC-015's (proposed) to define. This RFC specifies the **budget contract**
RFC-015 must satisfy, because the shape of that contract affects what the Map screen can promise:

- **Discovered-marker set is near-static.** After the initial burst of visiting a home region, new
  markers arrive at the rate the player explores plus the rate villages tier up / forts generation-up
  / raids get announced — all discrete, low-frequency events (village tier changes are, per GAME.md
  §6, deliberately slow; fort generations publish per training checkpoint, not per tick). **The Map
  screen never needs a per-tick feed.** A push-on-change model (server sends a marker delta the
  instant a discovered marker's state changes, plus the full discovered set once at login) satisfies
  every marker kind in §5.3 without polling.
- **Budget, illustrative (tunable, RFC-015 owns the actual encoding):** at a worst-case ~50 villages
  + ~25 forts + ~15 mines + ~10 gates + a handful of teammate bases and active raids, the entire
  discovered-set snapshot for one player is on the order of **a few hundred marker structs**, each
  small enough (position + kind + one payload field, §5.3) that the whole set fits comfortably under
  a few KB — this is not a bandwidth problem even at the low end of the 20–50 concurrent player
  target and 50–150ms home-cluster latency (GAME.md §11).
- **This is explicitly decoupled from chunk/creature replication.** The Map's marker feed does not
  ride the `PlayerBeacon`/interest-set chunk stream P2 already ships (which is per-tick, per-nearby-
  chunk, and irrelevant to a fort three rings away). It is a separate, much lower-frequency channel.
  RFC-015 may choose to multiplex it over the same transport; it must not multiplex it onto the same
  cadence.
- **Fog/discovery bitset (§5.2) is per-player persistent state**, not per-tick replicated at all — it
  updates only in the direction of "set more bits," is owned by the same node that owns the player's
  account record, and is written to the save format RFC-016 (proposed) owns.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001..010 (accepted combat set)** | Not load-bearing here. This RFC touches no ability, status, damage, or RL-observation math. Where a marker shows a training generation number (§5.3), the number itself is RFC-007's (accepted) checkpoint/generation lifecycle — this RFC only displays it. |
| **RFC-011** (proposed, Combat HUD) | Owns the in-combat raid-warning HUD element if one exists; this RFC owns only the Map-screen raid marker (§5.3). The two must agree on the same underlying raid-state data, not duplicate it. |
| **RFC-013** (proposed, Vitals/Death/Recovery) | Owns what happens to a player who dies while using a waypoint or mid-realm-gate transition (mid-teleport death is out of this RFC's scope); this RFC only defines when a waypoint/gate is *available*, not death consequences at either endpoint. |
| **RFC-014** (proposed, Instance & Realm Lifecycle) | Owns everything that happens *after* a realm gate is stepped on — instance allocation, group binding, per-realm atlas load/unload, timeout/idle destruction — and owns mine-depth instance mechanics that gate material-tier access (§3.6). This RFC owns only where a gate or mine mouth sits on the overworld and what it shows on the Map (§3.5, §3.6, §5.3); it explicitly does not specify instance internals. |
| **RFC-015** (proposed, Client Replication & Interest-Set Protocol) | Owns the actual wire encoding, delta format, and per-view budget enforcement for everything this RFC's §5.4 states as a contract. This RFC is a consumer of that protocol, not its author. |
| **RFC-016** (proposed, Persistence & Save-File Format) | Owns how the discovery bitset (§5.2), waypoint-eligibility state (§4.3), and any per-player Map preferences are actually serialized to disk. This RFC defines the data shapes; RFC-016 defines their on-disk encoding. |
| **RFC-018** (proposed, Loot, Essence & Reward Tables) | Owns what a mine or dungeon actually pays out, and the crafting/equipment material-tier table (Copper→Iron→Steel→Mythril, §3.6); this RFC places the mine mouth (§3.6) and dungeon entrance (§3.1) on the map and says nothing about their contents. |
| **RFC-019** (Progression & Skill System, this batch) | Owns the XP/skill-branch consequence of what a player does while traveling or mining (e.g., which branch credits a kill); does **not** own material-tier gating by mine depth — that is RFC-014's (instance mechanics) and RFC-018's (the material table). This RFC places mine mouths by ring (§3.6) and says nothing about what happens once you're inside. |
| **RFC-020** (Mission & Quest System, this batch) | If a quest ever references "go to village X" or "the scout has announced a raid," it points at the marker/discovery data this RFC defines rather than inventing its own world-location concept. This RFC does not require quests to exist and places no quest-giver logic. |

---

## Multiplayer & Simulation-LOD Considerations

- **Ring/terrain lookups are a pure function of `(seed, gx, gy)`** and require no synchronization
  between nodes at any tick rate — a sleeping chunk (0 Hz, ARCHITECTURE.md §4) answers "what ring am
  I in" exactly as cheaply as an active one, because the answer was never state, only arithmetic.
- **Discovery/fog updates piggyback on the existing beacon proximity check** (P2's `PlayerBeacon`),
  not a new per-tick scan — a chunk already knows which players are near it every 3 ticks; the map
  discovery write is a side effect of that existing message, not a new subscription.
- **The waypoint network adds no new hot-path load.** A waypoint use is a `Teleport` (already
  shipped, distinct from `MoveIntent`, R7) plus an item-spend check (`Ask<SpendItems, bool>`,
  already shipped on `PlayerActor`) — no new actor type, no new message class, one existing verb
  composed with one existing verb.
- **The two new no-build/overlay classes this RFC proposes (fort radius, §3.2; mines, §3.6) must
  trigger the flow field's existing debounced rebuild** the same "priority: yield to gameplay"
  discipline as everything else that touches the shared multi-source BFS field (ARCHITECTURE.md
  §5's `PathfieldActor` `Priority<2>` pattern) — a mine mouth or an enforced fort no-build ring is a
  new *build-blocking* overlay entry, and the flow field already treats the overlay as authoritative
  for pathing, so nothing new needs inventing here, only reuse.
- **Map marker replication (§5.4) is explicitly LOD-exempt in the opposite direction from chunk
  simulation**: chunk LOD drops ticking for chunks nobody is near; map markers must be visible for
  chunks nobody is near (that is the entire point of a map). The two systems must not be conflated —
  a fort's chunk may be asleep at 0 Hz while its generation number is still shown correctly on every
  player's map, because the generation number is a value published on change, not read live from a
  ticking chunk.
- **Cross-platform determinism is a hard requirement, not a nice-to-have, for every placement this
  RFC adds (mines, gates).** The existing verification pattern (ARCHITECTURE.md §2c: match village/
  fort/building counts and tile coordinates byte-for-byte between GCC/Linux and MSVC/Windows) must
  be re-run whenever §3.5/§3.6 are implemented, using the same integer-arithmetic discipline
  `village.hpp` already documents (the 266-tile road regression that motivated it).

---

## Tone Guardrail Compliance

Walking through every mechanic this RFC introduces or extends, checking each against GAME.md §0's
test ("does anything here count down behind the player's back, or create obligation pressure"):

1. **Ring geometry, worldgen placement, roads, forts, mines, gates (§2–§3).** All static once the
   world is generated. Nothing here changes over time at all — a fort's *no-build radius* doesn't
   shrink, a mine doesn't run out on a timer, a gate doesn't close itself. The one thing that changes
   post-generation (fort training generation, village tier) is owned by GAME.md §6/§10, already
   argued clean there ("quái tự luyện... chờ bạn tới lấy — không phải cái đồng hồ hẹn giờ") and only
   *displayed*, not driven, by this RFC.

2. **Discovery/fog (§5.2).** Strictly monotonic — bits only set, never clear. There is no "return
   within N days or lose what you found" rule, no fog regrowth, no reason to log in on a schedule to
   preserve map knowledge. This is the map applying the exact same non-decay principle GAME.md §6
   states for villages ("chững, không tụt") to the player's own knowledge.

3. **Waypoint network (§4.3).** The two possible objections and why neither holds:
   - *"Does eligibility ever expire?"* No — `USABLE` is lost only if the underlying village drops
     below tier 2, which per GAME.md §6 requires the player to have *deliberately provoked* a nearby
     fort into a retaliation raid. That is a consequence of an action the player chose to take, the
     same category GAME.md §6 already carves out as the sole non-clock-shaped way a village can
     regress. It is not decay from absence.
   - *"Is the flat resource cost a disguised timer?"* No — a cost paid once, at the moment of use,
     from a stockpile the player accumulated on their own schedule, is a spending decision, not a
     schedule. There is no cooldown, no daily cap, no resource that only regenerates over real time.
     The player can waypoint ten times in a row if they can afford it.

4. **Mounts (§4.2).** Unchanged from what's shipped — no stamina drain, no cooldown, no unlock quest
   proposed. Recorded, not altered.

5. **Raid warning marker (§5.3).** This RFC does not invent the raid mechanic (§3.4) — GAME.md §6
   already defends its shape at length (random "weather," one-day scout notice as invitation not
   ambush, self-defending tier-≥2 villages, no-tier-loss-from-bad-luck). This RFC's only addition is
   *display*: the countdown shown is framed as "the scout says tomorrow," a fixed piece of game-time
   information the player can act on or ignore, never a shrinking real-time number designed to
   create urgency. Choosing not to render a real-time ticking clock for this marker was a deliberate
   design choice in §5.3, made specifically because a literal countdown widget is the single most
   recognizable clock-pressure UI pattern that exists, and GAME.md §0 rules it out categorically.

6. **Map screen itself (§5.1).** Non-pausing by design, matching every other non-modal screen already
   shipped — opening it costs the player nothing and is available at will, with no "map costs stamina
   to check" or similar friction that would discourage checking it, which would itself create a
   different kind of pressure (avoidance anxiety) this RFC is careful not to introduce.

No mechanic in this RFC creates a deadline, a decaying resource, a login-frequency dependency, or a
countdown the player did not personally start by taking an action (visiting a place, spending a
resource, or the pre-existing raid scout event GAME.md §6 already designed and defended).

---

## Open Questions

1. **Rumor-halo cap for terrain fog (§5.2)** — "3 undiscovered cells" is a first guess for how much
   of the world silhouette should be visible past the player's actual footprint. Too generous and the
   Map stops feeling earned; too stingy and 1024×1024 feels like navigating with a flashlight. Needs
   a playtest pass once the Map screen exists.
2. **Waypoint fee resource choice (§4.3)** — Stone/Wood was picked because both are gatherable from
   turn one with no dungeon dependency, keeping the network usable before a player has ever fought
   anything. Should the fee scale with anything at all (e.g., a small flat scaling by *how many rings*
   the destination differs from the player's current ring, still with zero time component)? Left flat
   in this draft to keep the "one clean rule" property; revisit if playtesting shows waypointing
   trivializes the outer-ring adaptation loop GAME.md §4 wants to reward.
3. **Realm gate jitter cell (160 tiles) and mine jitter cell (140 tiles) (§3.5, §3.6)** — chosen by
   analogy to the existing village (112) / fort (124) spacing, scaled up because gates and mines are
   meant to feel rarer than either. No simulation of the resulting count has been run; worldgen
   authors should treat these as a starting point to tune against the actual generated map, the same
   way village/fort density was tuned in P1 (ROADMAP.md's "measured, not guessed" density fixes).
4. **Does the Map show *other players'* live positions at all?** GAME.md §11 gives every player a
   claim and mentions teammate bases (§5.3), but says nothing about a live blip for a friend's current
   position. This RFC deliberately did not add one — it would be a new, continuously-updating
   replication cost (§5.4 exists specifically to keep the Map feed low-frequency) and raises a
   privacy-shaped design question (does a teammate always want their live position visible?) this RFC
   is not positioned to answer. Left for a future revision or for RFC-015 to decide as a protocol
   opt-in.
5. **Interaction between diagonal ring stretch (§2.1) and village/fort ring-based density tables.**
   Density formulas in `worldgen.hpp` key off `ring_of()` directly, so they already inherit the
   diagonal stretch automatically — but no one has visually audited whether the diagonal quadrants of
   Meadow feel disproportionately safe as a result. Flagged for the same kind of visual audit R1–R4
   already applied to autotiling.

---

## Non-goals

- **Rendering.** How the Map screen is drawn — camera projection, icon sprites, zoom/pan, colour
  palette — belongs to `RENDER_SPEC.md`. This RFC specifies *what data* the screen shows and *when*,
  never pixels.
- **Instance lifecycle internals.** Allocation, per-group binding, idle timeout, and atlas load/
  unload for realms behind gates belong entirely to RFC-014 (proposed). This RFC stops at "a gate
  exists at this tile and shows this icon."
- **Worldgen algorithm re-specification.** This RFC records the placement *order*, *invariants*, and
  *geometry formula* that the game's contract needs (so a road-to-gate rule or a no-build zone can be
  stated precisely), but does not re-derive or replace the noise functions, jitter-grid mechanics, or
  terrain-tally algorithms already implemented and documented in `worldgen.hpp`/`tiles.hpp`/
  ARCHITECTURE.md §8. Tuning constants inside those algorithms (noise scales, candidate grid density)
  remain worldgen's own concern.
- **Raid probability math, scout AI, and village combat outcomes.** Owned by GAME.md §6 directly
  (unassigned to a numbered RFC in this batch); this RFC only specifies how an already-decided raid
  warning is *displayed* (§3.4, §5.3).
- **Loot/reward tables for mines and dungeons, and the crafting/equipment material-tier table.**
  RFC-018 (proposed).
- **Mine-depth access and instance mechanics.** RFC-014 (proposed). This RFC only notes that ore tier
  correlates with ring (§3.6); the XP/skill-branch consequence of mining is RFC-019's (this batch).
- **Quest content that references map locations.** RFC-020 (this batch) — this RFC supplies the data
  a quest could point at, not the quests themselves.
- **PvP, guild/alliance territory display.** PvP is off by default (GAME.md §11); no PvP-specific
  map layer is specified.

---

## Review Record

**Votes:** Opus — revise (11 mustFix). Sonnet — revise (5 mustFix, converging with Opus on all 5).

**Applied (both reviewers upheld):**
- §5.2 terrain fog: replaced self-contradicting global/local rule with one bounded frontier-halo model, decoupled from the 2 KiB bitset.
- §2.1 wobble: corrected "tapers toward centre" claim — wobble is a constant ±41/±58-tile term at every ring boundary.
- §3.6 + Interactions + Non-goals: mine-depth/tier-access now attributed to RFC-014, material table to RFC-018; RFC-019 keeps only the XP/skill-branch axis.
- Guide vs §4.4/§5.3: rumor layer now sketches ring/terrain silhouette only, never villages; fixed §3.2→§5.2 cross-ref.
- §3.1/§3.3/§3.6: specified the late mine/gate road-spur pass (route-around frozen overlay, reject-and-reroll on no route), separated from the step-2 village-road guarantee.

**Applied (one reviewer's mustFix, proof verified sound, other reviewer conceded it in review):**
- §4.3/§5.2 "visited" predicate unified to the §5.2 visit-grade proximity trigger; fixed wrong §3.1 cross-ref; added first-login home-village discovery/waypoint-status carve-out.
- §3.2 fort no-build radius: dropped self-contradicting "(tunable)" label — GAME.md-fixed, recorded not introduced.
- §3.5 ring-bias citation corrected from GAME.md §3 to ARCHITECTURE.md §8 (verified by grep).
- §4.1 speeds: on-foot figure attributed to GAME.md §3; mounted figure now stated as this RFC's own derivation.

**Unresolved:**
- Opus's §3.2/§3.5 "missing realm-gate no-build zone" mustFix: not applied. Sonnet's rebuttal is sound on inspection — ROADMAP.md P4 (lines 99, 124) places realm gates' instance infrastructure at P4, so P3's "cổng" no-build line (line 188) more plausibly denotes the already-shipped village gate, already covered by §3.2's wall/apron row; Opus's proof does not survive that context check, so this is left as a documentation gap for a future revision rather than a contradiction fixed here.
