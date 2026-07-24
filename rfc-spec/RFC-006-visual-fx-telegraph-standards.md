# RFC-006: Visual FX & Telegraph Standards

- **Status:** Accepted (revised after review)
- **Date:** 2026-07-23
- **Umbrella:** [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §2 (Telegraph-first), §14 (Asset Reuse), §15 (Visual Filters), §16 (Battlefield States — visual side)
- **Depends on:** RFC-001 (ability pipeline phases), RFC-002 (status framework), RFC-009 (damage numbers feeding danger tiers)
- **Depended on by:** RFC-005 (boss authoring must obey the timing table), RFC-008 (skill JSON carries this RFC's telegraph/FX vocabulary), RFC-007 (visual/obs parity rule)

---

## Summary

This RFC defines the **visual contract of combat**: the shape language, element palette,
and minimum lead times of telegraphs; the FX layer stack; the tint/filter recipes that
turn one sprite sheet into many skills; the status filter table; and the readability
rules that keep all of it legible on a 16×16-tile grid. It also specifies the remaining
spec-level requirements around the engine's effect-lifetime system (the former
`kEffectLife=6` gap, now partially fixed in code as a per-kind table).

The core rules, in one paragraph: **every hit is promised before it lands** — a ground
decal drawn in the element's hue, filling up as the wind-up runs, flashing in its last
three ticks, and resolving damage against *exactly the shape that was drawn*. Telegraph
ground decals are **procedural geometry** (circles, lines, cones drawn by the renderer),
not sprite sheets, so they are never blocked on unpacked art. Sprite-sheet FX are
reserved for impacts and flourishes, and are recolored via a two-pass tint+glow recipe
rather than new art.

## Motivation

Three hard facts force a telegraph-and-filter discipline rather than an animation one:

1. **All 66 monster sheets are 64×64, 4×4 walk-only — zero attack animations** (verified
   2026-07-23 asset audit). A monster cannot *look like* it is attacking. Its intent must
   be carried entirely by FX overlays rendered at and around its position. Only ~11 of 20
   boss sheets have real Attack/Charge poses, and even those need ground decals for area
   attacks.
2. **The tone guardrail** (GAME.md §0): challenge is opt-in, and inside a fight the
   player must always have been *told* — fairness is what keeps hard content compatible
   with a chill game. A hit that arrives unannounced is a countdown that ran behind the
   player's back, just compressed to one second. The umbrella (§2) makes this structural:
   Skill = Pose + FX + Gameplay Effect, and the FX **is** the announcement.
3. **The art budget is zero.** The umbrella's §14/§15 answer — tint, filter, layer — is
   only viable if the recipes are standardized. Without a spec, "red rock = meteor"
   decays into per-skill hand-tweaked colors that neither of two implementers would
   reproduce, and the RL observation/visual parity (RFC-007) becomes unverifiable.

Today's code (P2 + fight-system groundwork) already contains the seeds: `windup` is
published creature state, the renderer draws a red additive pulse and a shake, statuses
are multiplicative tints, and effect lifetimes are per-kind. What is missing is the
*grammar*: which shape means what, which hue means what, how long a warning must last at
each danger level, and what a boss author (RFC-005) or skill JSON author (RFC-008) is
allowed to write.

## Guide-level Explanation

### What the player experiences

You step into a dojo room. The Giant Samurai turns, and a **red cone** fans out from his
side on the floor — outline first, then a fill that sweeps outward over one second. You
have that second. You step off the cone; it flashes white-hot for a third of a second,
then the blow lands *on the drawn cone*, not on you, and a slash effect flowers where you
used to stand. Nothing ever hits you from a shape you were not shown.

Element attacks read by hue before you can read anything else: **orange-red is Fire,
cyan is Ice, amber is Rock, yellow is Thunder**. A red shape with no element hue is a
plain physical hit. A frozen creature is blue-washed and its animation freezes mid-frame;
a burning one flickers with small flames; a shocked one crackles with arcs. You never
need to read a nameplate to know what state anything is in.

Ordinary monsters — which have no attack animation at all — shake in place and pulse red
while a small decal marks the strike spot. Bosses do the same *plus* swap to their real
Attack/Charge pose, so a boss's promise is unmistakably bigger.

Everything stays quiet outside combat. Telegraphs exist only where something is actually
winding up at someone; there is no screen-wide alarm, no UI countdown, and screen shake
is capped and can be reduced in Options.

### What a designer writes

A designer authoring a skill (RFC-008) or boss ability (RFC-005) does not draw anything.
They declare:

```yaml
telegraph: { shape: cone, reach: 3.5, arc_deg: 90, tier: heavy }   # tier -> min windup
fx:        { impact: fx.slash_heavy_rock_red }   # an RFC-008 fx.* document carrying this RFC's recipe fields (§4)
```

The validator rejects a `heavy` ability whose wind-up is shorter than the tier's minimum
lead time. The renderer draws the cone from the same published state the simulation
resolves the hit from — there is no second source of truth to drift.

## Reference-level Design

### 0. Conventions

- **Tick** = simulation tick at 10 Hz (100 ms). All lifetimes and lead times are in
  ticks. Chunks at reduced LOD are addressed in §9.
- **Tile** = one 16×16-px world cell; positions are float tiles (see `tiles.hpp`).
- Element names: **Fire / Ice / Rock / Thunder**. Code spelling today is
  `Element::{kFire,kIce,kEarth,kShock}`; this RFC treats `kEarth`≡Rock, `kShock`≡Thunder
  and does not require a rename.
- All literal numbers below are **(tunable)** unless explicitly marked structural.

### 1. Telegraph grammar

#### 1.1 Shape vocabulary

Five shapes, and only five. A shape is a *promise about geometry*; adding a sixth shape
is an RFC change, not a data change, because players learn shapes, not skills.

| Shape | Geometry (params) | Means | Canonical users |
|---|---|---|---|
| `kCircle` | center `(x,y)`, `radius` | area impact at a point | meteor landing, Nova at a target, Squid shot landing |
| `kRing` | center, `r_inner`, `r_outer` | expanding/annular burst around a source | ElementalNova, boss shockwave |
| `kLine` | origin `(x,y)`, end `(ex,ey)`, `width` | a dash or projectile lane | Samurai charge, spinning projectile lane |
| `kCone` | origin, direction `(ex,ey)` as unit vector, `reach`, `arc_deg` (total angle) | directional swipe/breath | Samurai directional attack (L/R), melee cleaves |
| `kTiles` | up to 8 tile coords, grid-snapped | terrain about to become hazardous | cracking floor, spike rows (RFC-004 entities) |

Structural rules:

- `kTiles` is the **only** grid-snapped shape; the others are drawn in float tiles.
- `kLine` width minimum is 1.0 tile; a thinner lane cannot be read at 16 px.
- `kCone` `arc_deg` is restricted to {45, 90, 120, 180} (tunable set) — free angles
  produce cones players cannot distinguish.
- One ability phase produces **one** telegraph record. Multi-shape attacks (e.g. a charge
  that ends in a burst) are authored as sequential pipeline phases (RFC-001), each with
  its own telegraph.

#### 1.2 Element palette

One palette, defined once, used by telegraph decals, effect tints, status washes, and
skill-icon accents alike. Values are RGB; alpha is per-use (see §1.4, §6).

| Element | Hue (outline/glow) | Fill (same hue) | Motif (secondary channel) |
|---|---|---|---|
| Physical (none) | `255, 45, 30` | same | plain — matches the existing wind-up pulse color |
| Fire | `255, 120, 40` | same | 2–3 rising fleck particles inside the decal |
| Ice | `120, 210, 255` | same | static crystalline notches on the outline |
| Rock | `210, 160, 70` | same | radial crack lines from decal center |
| Thunder | `255, 230, 80` | same | one jagged polyline across the fill, redrawn every 3 ticks |

- The **motif column is mandatory**: hue must never be the only channel distinguishing
  elements (color-vision accessibility). Motifs are procedural draw calls, not sprites.
- Status tint colors (§6) are the *established* P2 values and stay; they are deliberately
  in the same families as their causing elements (Frozen↔Ice, Burning↔Fire,
  Shocked↔Thunder, Mired↔Rock) so the combo system (GAME.md §7) teaches its own mapping.
- Future schools (Plant/Water/Light/Darkness/Wind/Death) exist as icons only and get
  palette rows **only** when they become real elements — out of scope for v1.

#### 1.3 Danger tiers and minimum lead times

The single most load-bearing table in this RFC. A telegraph's **tier** is derived from
what the hit can do; the tier dictates the minimum wind-up. RFC-005 and RFC-008
validators MUST reject abilities that violate it.

Let `d = (base_damage × ring_damage_scale) / kPlayerMaxHp` — the ability's expected
damage after RFC-009's ring scaling (baked into monster stats at spawn), over the flat
global player-HP constant. No per-ring player-HP table exists or is needed. Let `cc` =
hard-control duration in ticks (stun/root/freeze from RFC-002).

| Tier | Qualifies when | Min wind-up | Required cues |
|---|---|---|---|
| 0 `light` | `d < 0.10` and `cc == 0` | **5 ticks** (0.5 s) (tunable) | source cue only (decal optional) |
| 1 `moderate` | `0.10 ≤ d < 0.25` or `0 < cc < 10` | **8 ticks** (0.8 s) (tunable) | source cue + ground decal |
| 2 `heavy` | `0.25 ≤ d < 0.50` or `10 ≤ cc < 20` | **12 ticks** (1.2 s) (tunable) | source cue + ground decal |
| 3 `deadly` | `d ≥ 0.50` or `cc ≥ 20` | **16 ticks** (1.6 s) (tunable) | source cue + ground decal + imminent flash mandatory; boss pose swap where the sheet has one |

Tier is computed at authoring/validation time from the ability's data (RFC-008), not at
runtime. Sanity check against shipped values: the Samurai's attack (`kBossAttackWindup
= 10`, 20 damage on a ~100 HP player → `d = 0.2` → tier 1, min 8 ✓) and charge
(`kBossChargeWindup = 14` ≥ 12 for tier 2 ✓) already conform.

Tier resolution and floors (structural):

- **Highest tier wins.** The `d`/`cc` clauses overlap by design (`d = 0.30, cc = 5`
  matches tier 1's control clause and tier 2's damage clause); an ability's tier is the
  **highest** tier any of its clauses qualifies for.
- **This table is a floor among floors.** The effective minimum wind-up is
  `max(tier minimum, RFC-001 V2, RFC-005 §R4)`. RFC-001 V2 layers `cast_ticks ≥ 4` on
  every hostile ability and `≥ 8` on heavy/committed hostile payloads; RFC-005 §R4 layers
  `max(kWindupFloor = 6, escape-distance formula)` on every boss ability. So tier 0's
  5-tick minimum is reachable only by a plain hostile instant-hit jab (4 ≤ 5 ✓); a tier-0
  *boss* ability really gets ≥ 6, and a tier-0 heavy/committed hostile payload gets ≥ 8.
  These floors never conflict — the binding one is simply the largest.
- **Where this is enforced.** RFC-005's validator implements the check (R7 #5: `windup ≥`
  this table's minimum, declared tier cross-checked against the RFC-009-resolved damage
  key; R7 #6 folds in §R4's floor). RFC-008's validator does **not** yet: V30's flat
  `cast.ticks ≥ 3` is weaker than every row here and never computes `d`/`cc`. RFC-008
  MUST add the equivalent check; until it does, only boss kits are machine-checked
  against this table.

Additional timing rules (structural):

- **T1 — No zero-tick damage.** Every damaging resolution is preceded by a telegraph of
  its tier's minimum, including trap/hazard activation (RFC-004 entities telegraph with
  `kTiles`). The only exceptions are damage-over-time ticks of an *already announced*
  status or zone.
- **T2 — Commitment.** At commit, the telegraph's geometry is frozen (the existing
  `windup_x/windup_y` behavior, generalized). A telegraph never re-aims, grows, or
  follows the player. Dodging out of the drawn shape always works. This is the combat
  form of "nothing chases the player".
- **T3 — Off-screen origins.** If the source is off-screen (ranged bosses, Squids), the
  ground decal at the destination carries the full lead time by itself; lead time never
  shrinks because the source was not visible.
- **T4 — Interrupt.** A cancelled wind-up (stun, RFC-002) kills the telegraph with a
  2-tick (tunable) grey fizzle-out and **no** impact FX — the player must be able to
  distinguish "I interrupted it" from "it fired and missed".
- **T5 — Whiff honesty.** An uninterrupted wind-up always resolves *at the drawn shape*,
  playing its impact FX there even if it hits nothing (the existing whiff-slash rule).

#### 1.4 Telegraph lifecycle (state machine)

```
            commit (sim)                      left == 3            left == 0
  [none] ────────────────► ARM ───► CHARGE ─────────────► IMMINENT ─────────► IMPACT ─► [gone]
                            │  2 ticks   │                    │                (1 tick:
                            │            │ stun               │ stun            decal dies,
                            │            ▼                    ▼                 impact FX
                            │         FIZZLE ◄────────────────┘                 one-shot)
                            │        (2 ticks, grey collapse, no impact FX)
                            └─ ARM is part of the wind-up count, not extra time
```

Per-state render rules (all alphas 0–255; all values tunable):

| State | Outline | Fill | Animation |
|---|---|---|---|
| ARM (first 2 ticks) | element hue, alpha 0→220 ramp | none | decal scales 0.85→1.0 |
| CHARGE | element hue, alpha 220 | element hue, alpha `96 * fill_frac` | fill sweeps from origin outward (cone/line) or center outward (circle/ring); `fill_frac = 1 - left/total` |
| IMMINENT (last 3 ticks) | white `255,255,255` alpha 240 | full, pulsing alpha 96↔150 at 6 Hz | motif redraw every tick |
| IMPACT (1 tick) | none | white flash alpha 180 | hand-off to impact `Effect` |
| FIZZLE (2 ticks) | grey `128,128,128` alpha 220→0 | collapses to origin | none |

`fill_frac` is a pure function of `(total, left)` — two integers already replicated —
so every client draws the identical countdown with no extra state or float drift.

Short and decal-less wind-ups (structural): at `total ≤ 5` (= ARM 2 + IMMINENT 3), CHARGE
has zero ticks and is skipped — ARM hands straight to IMMINENT. When a tier-0 ability
omits the decal, the same state machine drives the §1.6 source-cue stack instead: ARM
starts the shake, CHARGE ramps the pulse with `fill_frac`, IMMINENT flashes the pulse at
6 Hz, and FIZZLE desaturates the stack to grey for its 2 ticks. One lifecycle, with or
without a decal.

#### 1.5 Truth-in-advertising (hitbox = decal)

The resolution shape used by the simulation and the decal drawn by the renderer are
computed from the **same replicated record** (§2). Tolerance is asymmetric in the
player's favor: a player whose center is outside the shape by any margin is safe; a
player inside by less than **0.25 tile** (tunable) is also safe. Implementations MUST
NOT resolve hits from a different (e.g. padded) shape than the one drawn.

#### 1.6 Source cues on walk-only monsters

Because monster sheets have no attack frames, the source cue is standardized as an
overlay stack at the monster's position (all existing behaviors, now normative):

1. **Shake**: sprite jitters ±1.5 px world-space during wind-up (exists). The amplitude
   scales with the Options "reduce screen shake & motion" setting (R7) down to zero — a
   motion-sensitive player loses the jitter, never the pulse or the decal.
2. **Pulse**: additive overlay of the sprite silhouette in the telegraph's hue —
   physical red today, element hue when elemental attacks arrive — alpha ramping
   30→110 with `fill_frac` (exists as red; hue generalization is new).
3. **Charge glyph** (elemental attacks only, new): the element's 24-px skill-book icon
   (`BookFire`/`BookIce`/`BookRock`/`BookThunder`, already packed as UI icons) drawn
   1 tile above the sprite at alpha 200 during CHARGE. This is the "what school is
   coming" read at zero art cost.
4. **Bosses additionally** swap to `BossPose::kAttack/kCharge` where the sheet has the
   pose (the ~11 richer sheets). Sheets without a pose (never RL bosses per the
   shortlist) rely on 1–3 alone.

Additive, never multiplicative, for overlays 2–3: a multiplicative tint can only darken
(measured lesson, `raylib_bridge.cpp` comment near line 892).

### 2. Data shapes and replication

The telegraph is chunk state, published exactly like `Effect` — combat must be legible
to bystanders, and the replication path already exists and is cheap.

```cpp
enum class TelegraphShape : std::uint8_t { kCircle, kRing, kLine, kCone, kTiles };

struct Telegraph {                    // 35 B packed / 40 B with float alignment; replicated in the chunk view
    std::uint32_t id;                 // stable within the chunk for interpolation
    TelegraphShape shape;
    Element element;                  // kNone = physical red
    std::uint8_t tier;                // 0..3, from the table in §1.3
    std::uint8_t total;               // wind-up ticks at commit (structural: >= tier min)
    std::uint8_t left;                // ticks remaining; 0 has already resolved
    float x, y;                       // origin / center
    float ex, ey;                     // kLine end; kCone unit direction; else unused
    float radius;                     // circle/cone reach; ring outer; line width
    float r2;                         // ring inner; else 0
    std::uint8_t arc_deg_half;        // cone: arc_deg / 2 (fits 180 in a byte)
    std::uint8_t flags;               // bit0: fizzling; bit1: kTiles payload follows
};
```

- `kTiles` carries its tile list in a parallel fixed array (8 × packed 8-bit local
  offsets) — chunk-local, so a byte per axis suffices.
- **Cap: 8 live telegraphs per chunk** (tunable). The cap is a design budget, not just a
  memory one (§8 R4). Overflow resolves by tier, never by arrival order: if the newest
  commit's tier is strictly higher than the lowest-tier live telegraph's, the sim cancels
  that lowest-tier telegraph (oldest among ties) — a true interrupt: FIZZLE visual, no
  impact, cooldown refunded per RFC-005 R6 — and admits the new one; otherwise the newest
  commit is refused (the ability resolves as a Hold, cooldown not spent). A boss's deadly
  slam is never silently eaten by pre-existing minor hazard telegraphs, and a refused
  commit is a well-defined no-op in the RL action log, not noise. Never silently
  drawn-but-not-resolved or resolved-but-not-drawn, which would break §1.5.
  Authoring-side, RFC-005's validator must count the kit's worst case (abilities + ≤ 4
  adds + spawned RFC-004 entity arming telegraphs, `kMaxEntities = 16`/chunk) and reject
  kits that can legitimately exceed 8 in their own room.
- Wire cost at the cap: 8 × 40 B = 320 B, plus 16 B of `kTiles` tile arrays when every
  record carries one — ≈ 336 B/chunk/tick absolute worst case, comparable to the existing
  effect list, and only in chunks with active combat — acceptable for the
  leader-replicated model.

### 3. FX layering model

One canonical layer stack; every drawn combat visual belongs to exactly one layer.
Bottom to top:

| # | Layer | Contents | Sorting |
|---|---|---|---|
| L0 | Terrain & terrain overlay | tiles, craters, scorch decals (state owned by RFC-004) | tile order |
| L1 | Ground telegraph decals | §1 shapes; zone floor visuals (Wet/Smoke circles) | flat, no Y-sort — they are *on* the floor |
| L2 | World sprites | creatures, players, combat entities, trees, buildings | the existing single Y-sort list (R3) — combat entities (RFC-004) enter the same list, no special case |
| L3 | Attached overlays | status motif overlays, wind-up pulse, charge glyph, boss health bar | inherit their owner's Y; drawn immediately after the owner sprite |
| L4 | Impact & travel FX | `Effect` strips, projectiles' sprite pass | Y-sorted by their own `y` so a slash can happen behind a tree |
| L5 | Screen-space filters | weather, battlefield-state filters (§7), damage vignette | unsorted, whole-frame |

Structural rules:

- L1 never occludes L0 hazard reads: ground-decal fill alpha is capped at **150** even
  during IMMINENT (§1.4 respects this).
- L3 overlays never leave the owner's 2×2-tile neighborhood; anything bigger is either a
  telegraph (L1) or an effect (L4).
- L5 total accumulated wash alpha is capped at **130** (the measured P2 lesson: a
  full-screen alpha-130 red wash already overwhelms; two stacked filters must not exceed
  what one was allowed).

### 4. Tint/filter reuse — FX recipes

A **recipe** turns one packed sheet into a family of skills (umbrella §14). A recipe is
not a separate document kind and is never referenced by a `recipe:` key: its fields live
**on the `fx.*` document** (RFC-008 §7.1), and skills reference plain fx ids
(`"fx": "fx.rock_red"`) exactly as RFC-008 already does — one authoring pipeline, not
two. RFC-008 §7.1 today carries only the flat multiplicative `tint [r,g,b,a]`; it MUST
grow the fields below (glow pass, scale, frame window, play mode) — RFC-006 is the
schema authority for this block (see Interactions), and a variant stays what RFC-008
promises: a new ~8-line fx document, zero new art.

```cpp
struct FxRecipe {                    // the recipe fields of an fx.* document (RFC-008 §7.1)
    EffectKind base;                 // which packed strip to play
    std::uint8_t tint_r, tint_g, tint_b;   // multiplicative pass (255,255,255 = none)
    std::uint8_t glow_alpha;         // additive silhouette pass in the element hue; 0 = off, max 140
    Element glow_element;            // additive hue from §1.2; kInherit = resolved at spawn
                                     //   from the spawning Effect's `element` field (FX-4)
    std::uint8_t scale_eighths;      // 8 = 1.0x; range 4..24 (0.5x..3x)
    EffectKind trail;                // second strip played behind a moving user; kCount = none
    FxPlayMode mode;                 // §9, FX-3
};
```

Two-pass rule (structural): the multiplicative tint pass can only darken/shift the
sheet; brightening variants MUST use the additive glow pass. One pass alone cannot
express "red-hot rock".

Canonical v1 recipes — each row is an `fx.*` id (`fx.rock_red`, …); names structural,
numbers tunable:

| Recipe | base | tint | glow | Reads as |
|---|---|---|---|---|
| `rock_plain` | kEarth | none | — | Rock spell (as shipped) |
| `rock_red` | kEarth | `255,120,90` | Fire 90 | **Meteor** — red rock + heat |
| `rock_frost` | kEarth | `170,210,255` | Ice 70 | **Ice boulder** |
| `ice_flash` | kIce | none | Ice 60 | frost burst |
| `fire_burst` | kFire | none | Fire 80 | flame burst |
| `shock_arc` | kShock | none | Thunder 90 | thunder strike |
| `blast_combo` | kBlast | none | `kInherit` — resolved at detonation from `Effect.element` (FX-4), i.e. the detonating status's element | combo detonation, hue tells which combo |

(`Purple rock = cursed rock` from umbrella §14 is future-school work and deliberately
absent from v1.)

Frame-count tricks (umbrella §14 "reduce/reorder frames") are expressed as an optional
`frame_window: [first, last]` on the recipe — e.g. the last 5 frames of the 14-frame
Earth strip alone read as "debris settling" for a Persist phase. The window must be a
contiguous sub-range; reordering is out (it multiplies atlas entries for marginal gain).

### 5. Projectile and travel visuals

- A projectile (existing `Projectile` struct) renders as its element's small FX frame
  rotated to its velocity; spinning-projectile sheets, once packed (§9 FX-6), replace
  the rotation with authored frames.
- A projectile whose flight time exceeds tier-0 lead time (≥ 5 ticks) needs no landing
  decal (the visible travel *is* the telegraph). Lobbed/instant-fall attacks (meteor,
  Squid arcing shot) MUST place a `kCircle` decal at the landing point for their full
  tier lead time (rule T3).

### 6. Status visual filters

RFC-002's shipped model, rendered faithfully: at most one active **ladder primary**
(Cold/Heat/Shock/Earth/Stagger, stage 1–3) plus the binary **coating** (Wet) that
coexists freely with it — so one primary wash *and* the coating overlay can be live at
once, and the renderer must show the *stage*, not just the channel. The per-channel hues
are the shipped P2 values where P2 had them (canonical); Earth reuses P2's mud hue (the
old Muddy coating folds into the Earth ladder — RFC-002 §11). There is no Poison channel
in v1 (RFC-002 Open Question 2 / RECONCILIATION.md ruling 1).
**Stage scales the wash** — the "stage-scaled intensity" RFC-002 assigns here: wash
alpha ≈ 40 / 70 / 100 (tunable) at stage 1/2/3, motif density growing with stage. Motifs
are new, drawn on L3; every channel uses *both* wash and motif (accessibility, same
argument as §1.2).

| Channel (hue) | Stage 1 | Stage 2 | Stage 3 |
|---|---|---|---|
| Cold `140,210,255` | Chilled — light wash, 1 frost notch on the rim | Frostbound — deeper wash, 3 static rim notches | Frozen — full wash + **animation freeze-frame** (walk cycle halts on its current frame — free, and the strongest read in the game) |
| Heat `255,150,90` | Singed — faint wash, 1 ember fleck | Burning — Fire strip frames 0–3 at 0.5× scale looping at the feet | Ablaze — same loop at 0.75× + a shimmer ring (its Heat aura is real, RFC-002) |
| Shock `255,245,130` | Static — wash only | Shocked — one jagged polyline across the sprite, redrawn every 3 ticks (same generator as the Thunder decal motif) | Overloaded — two polylines + 1-tick white sprite flash every 5 ticks |
| Earth `180,150,110` | Encumbered — faint mud wash, 1 clod fleck | Mired — deeper wash + dark band over the lower third (P2's Muddy look) | Root — full wash + short stone spikes pinning the feet |
| Stagger (no wash) | Unsteady — subtle 1-px wobble, no wash | Staggered — 2-tick white flicker | Knockdown — desaturate 40% + 3 orbiting spark dots above the head |

Coatings coexist with any primary (a creature can be Wet **and** Frostbound):

| Coating | Wash (multiplicative) | Motif overlay (additive, L3) |
|---|---|---|
| Wet | `150,190,255` | 1-px drip line under the sprite every 8 ticks |

When a coating and a primary coexist, the two multiplicative washes compose (both are
mild by design) and the motifs stack on L3 within R3's motion budget — coating motifs are
static or slow precisely so they never compete with the primary's read.

Zones render on L1 as their element/status hue at fill alpha 60 (tunable), outline
alpha 140, with the status motif scattered inside — a Wet zone visibly *is* the Wet
coating painted on the floor. No new art.

### 7. Battlefield-state render filters (visual side of §16)

Battlefield states are owned by RFC-010; this RFC fixes only their render vocabulary so
implementations converge:

| State (from RFC-010) | Visual filter | Caps |
|---|---|---|
| Earthquake | camera shake ±2 px at 10 Hz max; L1 decals additionally tremble ±1 px at 8 Hz ("telegraphs tremble", umbrella §16) | shake respects the Options "reduce screen shake & motion" setting (R7); gameplay accuracy effects are RFC-010's, not the renderer's |
| Fog / smoke | L5 grey wash, alpha ≤ 100 | telegraph outlines clamp to min alpha 200 *through* fog — a state may hide the world but never the promise (fairness rule F1, structural) |
| Rain/storm (weather) | existing P1 particle layer; adds Wet-status motif to zones | — |

Rule **F1** generalizes: no L5 filter may reduce a telegraph outline below alpha 200 or
a source-cue pulse below alpha 80.

### 8. Readability rules on the 16×16 grid

Numbered so reviews can cite them:

- **R1 — Minimum feature size.** No telegraph dimension under 1.0 tile (16 px); no decal
  outline thinner than 1 px at 1× world scale, 2 px preferred at the shipped 2× draw
  scale.
- **R2 — Alpha discipline.** Ground fills ≤ 150 alpha (never hide the floor); outlines
  ≥ 200; every decal draws a 1-px dark rim (`20,16,16` alpha 160) *under* its hue
  outline so cyan-on-snow and amber-on-desert stay separable on all 11 terrain types.
- **R3 — Motion budget.** At most one shaking/pulsing element per threat: the source
  pulses, the decal fills steadily; only IMMINENT adds the 6 Hz flash. Two competing
  pulse frequencies on one threat read as noise.
- **R4 — Screen budget.** ≤ 6 (tunable) simultaneous telegraph decals on screen under
  normal authoring; the hard per-chunk cap of 8 (§2) backs this. RFC-005's validator
  enforces it for boss scripts (a boss room is one chunk, so the cap is airtight there).
- **R5 — No full-screen saturated washes.** The measured P2 lesson (death overlay at
  alpha 130 overwhelmed the frame) becomes a rule: L5 combined wash ≤ 130 alpha, and
  never pure red — pure red is reserved for threat reads.
- **R6 — Hue is never alone.** Element ⇒ hue + motif (§1.2); status ⇒ wash + motif
  (§6); danger ⇒ geometry + fill animation (luminance-based, colorblind-safe).
- **R7 — Options.** "Telegraph opacity" slider (75–150% of the §1.4 alphas) and the
  existing "reduce screen shake" toggle — widened to "reduce screen shake & motion",
  governing camera shake (§7), decal tremble, and the §1.6 sprite jitter alike — live in
  the shipped Options screen. Pulses, decals, and outlines are fairness reads and are
  never removed by any setting (F1 floors still apply).
- **R8 — Distance legibility.** The charge glyph (§1.6) and boss pose swap exist so a
  threat is classifiable at ≥ 8 tiles, where a 16-px sprite's silhouette detail is gone.

### 9. The effect-lifetime engine gap — state and requirements

**Current state (verified in source, `tiles.hpp:596`):** the original single
`kEffectLife = 6` truncated long strips (Earth is 14 frames, Ice 10, and the renderer
maps age→frame as `(age * frames) / life`). The code now ships a per-kind
`effect_life_of(EffectKind)` table mirroring the atlas frame counts, playing one frame
per tick at 10 Hz with `kMaxEffectLife = 14`. The *headline* gap is fixed; what follows
are the spec-level requirements that finish the job. Each is normative for the fight
system build-out:

- **FX-1 — Build-time frame-count parity.** `effect_life_of` mirrors
  `tools/build_atlas.py` by hand today; the packer MUST emit a generated header of
  per-strip frame counts that the table `static_assert`s against. A silently re-cut
  strip must fail the build, not truncate at runtime again.
- **FX-2 — Lifetime domain.** Effects age in sim ticks and cap at ~1.4 s. Telegraphs
  (§2) are a **separate record type** with their own countdown — they MUST NOT be
  implemented as long-lived `Effect`s, because effect age→frame mapping and telegraph
  fill math are different contracts.
- **FX-3 — Play modes.** Add `FxPlayMode { kOneShot, kLoop, kHold }` consumed by the
  renderer's age→frame map: `kOneShot` = today's behavior; `kHold` = clamp to last frame
  (e.g. debris settling via a recipe frame window, §4). `kLoop` is **not an `Effect`
  record**: it is the derived channel RFC-004 asked for, adopted verbatim — the renderer
  computes `frame = (t − state_tick) % frames` straight from the owner's already-published
  record (entity `active_fx`, zone id, status primary+stage), so a 400-tick entity loop
  costs no new sim state and no new message, occupies no slot in the FX-4 effect budget,
  and can never be evicted mid-fight by impact churn. A loop exists exactly as long as
  its owner record says so (zone expiry, status cure, entity death).
- **FX-4 — Effect record extension.** `Effect` grows `element` (for recipe hue and
  `kInherit` resolution, §4), `recipe` id (the `fx.*` document id, §4), and `rot` (uint8,
  256-step, for cones/projectiles). Budget stays small:
  ≤ 16 bytes/record, cap 24 effects/chunk (tunable) with a drop policy of *oldest
  cosmetic first*; records flagged as telegraph-adjacent (IMPACT flashes, FIZZLE) are
  never dropped before cosmetics — dropping a promise's resolution is a §1.5 violation.
- **FX-5 — LOD behavior.** Combat visibility is guaranteed by construction: RFC-010's
  Invariant L-1 keeps every chunk holding a player beacon at active LOD (10 Hz), boss
  rooms are pinned active during a fight, and background chunks spawn no effects — so
  every telegraph a player can see ticks at full rate, and no player can ever watch a
  demoted wind-up. The *fate* of in-flight combat on demotion is owned by RFC-010 and
  RFC-005, not here: a sleeping boss fight is **paused** (RFC-005 §R8) or leash-reset
  (RFC-010's boss row), never cancelled by the visual layer. RFC-006's own obligations
  are only: (a) cosmetic `Effect` records are dropped on demotion and never replayed on
  wake — they are sub-second transients, and replaying them is a lie about *when*; (b) a
  paused fight's `Telegraph` records freeze automatically (`left` decrements only when
  the chunk steps) and resume honestly on wake — FIZZLE is **never** used for LOD
  transitions, it is reserved for real interrupts (T4), and no cooldown-refund question
  arises because nothing is cancelled; (c) on chunk wake, any telegraph with
  `left > total` (corrupt/stale) is dropped without impact. This satisfies the "tolerate
  being ticked at reduced rate or slept" constraint with zero new mechanisms and zero
  overlap with RFC-010's ownership.
- **FX-6 — Unpacked sheets are not blockers.** The `Magic/*` family (Shield/Aura/
  Boost/Spark) and the spinning projectile sheets are not yet packed. Nothing in §1–§3
  depends on them: ground decals are procedural geometry, source cues reuse the packed
  book icons, and projectiles fall back to rotated element frames (§5). Packing those
  sheets upgrades flourish quality (buff auras, authored spin) and SHOULD land before
  RFC-005's tier-3 boss abilities ship, but no telegraph functionality may be gated on
  them.

## Interactions with Other RFCs

- **RFC-001 (Ability System):** the pipeline's Cast/Channel phases *produce* Telegraph
  records; Release/Impact consume them (IMPACT hand-off, §1.4). RFC-001 owns when
  phases run; RFC-006 owns what they look like and their minimum durations (§1.3 feeds
  back as a constraint on RFC-001 phase lengths).
- **RFC-002 (Status & Effects):** status identity, durations, and cure rules live
  there; §6 maps each ladder channel × stage and each coating to a wash+motif row and
  owns the stage-scaled intensity RFC-002 assigns here. New channels or coatings added
  by RFC-002 MUST add rows here (structural).
- **RFC-003 (Physics & Materials):** knockback/impulse outcomes are RFC-003's; this RFC
  only notes that materials may modulate *impact FX choice* via recipes (e.g. stone
  impacts use `rock_plain` debris) — no physics semantics here.
- **RFC-004 (Terrain & Combat Entity):** entity spawn/death FX and hazard-activation
  telegraphs (`kTiles`) use this grammar; crater/scorch decal *state* is RFC-004's,
  their L0 render slot is §3's. The looping visual its Active entities need is FX-3's
  derived channel — computed from the published entity record, no new sim state
  (RFC-004's own proposal, adopted verbatim).
- **RFC-005 (Boss Ability Authoring):** every authored boss ability declares
  `{shape, tier}`; the authoring validator enforces §1.3 minimums, §2's cap, and R4's
  screen budget. Pose availability per boss sheet is RFC-005's data; §1.6 rule 4 says
  when a pose is mandatory (tier 3, where the sheet has one).
- **RFC-007 (RL Observation):** see below.
- **RFC-008 (Data-driven Skills):** hosts `telegraph:` and `fx:` blocks in skill JSON
  using this RFC's vocabulary (shapes, tiers, fx ids). RFC-006 is the schema authority
  for those two blocks; RFC-008 for everything else. Two required deltas on RFC-008:
  §7.1 `fx.*` documents grow the §4 recipe fields (today: flat `tint` only), and its
  validator gains the §1.3 tier check (V30's `cast.ticks ≥ 3` is weaker than every tier
  minimum).
- **RFC-009 (Damage & Build-up):** owns `ring_damage_scale` and the expected-damage
  computation feeding §1.3's `d = (base_damage × ring_damage_scale) / kPlayerMaxHp`; no
  per-ring player-HP table exists or is asked for. The tier thresholds themselves are
  §1.3's.
- **RFC-010 (Battlefield Simulation):** owns battlefield-state lifecycles and LOD/replication
  policy; §7 fixes their render filters, FX-5 states the visual layer's LOD obligations.

## RL Considerations

- **Parity rule (structural):** the visual layer is a *pure function of replicated sim
  state* (`Telegraph`, `Effect`, `windup`, `boss_pose`, status). Nothing renderer-side
  may influence outcomes, and nothing outcome-relevant may exist only renderer-side.
  Consequence: an RL agent observing sim state (RFC-007) sees exactly what a player
  sees, no more — the boss cannot learn a tell the player cannot read, and headless
  dojo training (10×7-tile interior rooms) trains against the same information humans
  get.
- **Shared constants:** wind-up tick counts are both the RL action commitment window
  and the visual lead time — one header (the RFC-005/008 data), never two copies. The
  §1.3 tier table is the natural hook for RFC-007 reward shaping (dodge-window rewards)
  and for RFC-005 difficulty ceilings: "harder" may never mean "shorter than tier
  minimum".
- **Observation cost:** telegraphs add nothing *new* to the observation: RFC-007 already
  carries wind-up flags, slot cooldowns, and pipeline phase one-hots plus phase progress
  (equivalently `total`/`left`); the decal is derived data, and policies never consume
  pixels. Honesty note: RFC-007's target block carries **no telegraph geometry** — no
  committed aim point, shape kind, arc, or width — so a sparring defender can learn
  shape-exact dodges only for point/circle threats (via relative position at commit);
  cone/line shape-membership (§1.5) is not computable from the v1 obs. If defender
  training needs it, the geometry fields are an RFC-007 addition, not a telegraph-side
  change.
- **Whiff semantics** (T2/T5) make the dodge a stable learning signal in both
  directions: the boss's policy learns that commitment has a cost, and the defending
  agent (sparring) learns that escaping a committed threat always works — shape-exactly
  for point/circle under the v1 obs; see the Observation-cost caveat for cone/line.

## Asset & Engine Constraints Honored

| Constraint (2026-07-23 audit) | How this RFC honors it |
|---|---|
| 66 monsters walk-only, zero attack anims | All monster telegraphs are FX overlays at/around the monster (§1.6) + procedural ground decals (§1.1); no attack frames assumed anywhere |
| ~11/20 boss sheets with Attack/Charge; RL shortlist (Samurai first, no Dragons/GiantSlime/Flam/Spirit) | Pose swap is *additive* on top of the universal cue stack and only mandated where a sheet has the pose (§1.6 rule 4); nothing in the grammar needs multi-part rigs |
| Player kit = basic attack + exactly two abilities | §Guide and all examples use the shipped 6-ability/2-slot table; no hotbar visuals specified |
| Exactly 4 elements v1 | Palette has 4 element rows + physical (§1.2); spare schools explicitly future-only (§1.2, §4) |
| 121 skill icons at 24 px with Disabled twins | Cooldown UI needs no new spec (Disabled twins); book icons reused as charge glyphs (§1.6) |
| `kEffectLife=6` gap | §9: current per-kind fix acknowledged from source; FX-1..FX-6 finish it |
| No bespoke combo art | `blast_combo` recipe hue-codes the detonating status (§4) over the existing Blast strip |
| Magic/* FX and spin sheets unpacked | FX-6: explicitly non-blocking; procedural decals + rotated frames as fallback |
| Chill guardrail (GAME.md §0) | T2 (nothing chases), no screen-wide alarms, R5/R7 caps and options, telegraphs exist only inside opted-into combat |
| 1024² overworld with LOD / sleep | FX-5: cosmetics dropped on demote, paused fights freeze and resume honestly (sim fate owned by RFC-005/010), stale telegraphs dropped on wake |
| Server-authoritative, cheap replication | Telegraphs are capped chunk-view records (≤ ~336 B/chunk worst case, §2), same channel as effects |
| RL: one policy per archetype, dojos visible in-world | Parity rule keeps dojo spectating honest (players watching training see real telegraphs); nothing per-individual anywhere |

## Open Questions

1. **Tier `d` under gear spreads.** §1.3 computes `d` against the flat global
   `kPlayerMaxHp` (RFC-009 keeps ring scaling on monster stats; no player-side table
   exists). If gear-driven player-HP spreads > ~2× ever land, tiering may need a
   percentile definition (e.g. `d` against the 25th-percentile HP for the content's
   ring).
2. **`kTiles` payload size.** 8 tiles per record covers spike rows and cracking floors
   in a 10×7 room; large terrain events (RFC-004 multi-tile collapses) may need either
   multiple records or a rectangle variant. Decide when RFC-004's entity list is final.
3. **Charge glyph legibility.** The 24-px book icon above a 16-px monster is 1.5 tiles
   of UI in the world; playtest whether it reads as diegetic or as clutter at R4's
   6-decal budget. Fallback: drop rule-3 glyphs to bosses only.
4. **Telegraph interpolation at 10 Hz.** Fill sweeps advance in 10 discrete steps over a
   1 s wind-up; is renderer-side smoothing (interpolating `fill_frac` between ticks)
   worth the divergence risk from tick-exact truth? Proposed default: smooth the fill,
   never the outline geometry — needs a look on real hardware.
5. **Colour work vs. terrain palettes.** R2's dark under-rim is designed to survive all
   11 terrains, but snow + Ice cyan + white IMMINENT flash is the worst case; may need
   an Ice-on-snow special (deepen the hue 15%) after a visual pass.
6. **Sound.** The audit flagged audio wiring debt (melee plays `harvest.wav`). Telegraph
   audio cues (commit tick, imminent tick) are the natural pairing of this grammar but
   are unscoped here — do they belong in this RFC's revision or in a small RFC-006b?

## Non-goals

- **Gameplay semantics of anything.** Damage numbers, status behavior, physics,
  build-up ladders, entity HP — RFC-002/003/004/009. This RFC never changes what a hit
  *does*, only how it is announced and drawn.
- **Boss behavior and ability content** — RFC-005. This RFC constrains authored
  abilities; it authors none.
- **The skill data format** — RFC-008; this RFC only owns the `telegraph:`/`fx:` block
  vocabulary.
- **Observation/action tensors** — RFC-007.
- **New art.** No requirement in this RFC needs a sprite that does not exist in the
  packed set; unpacked sheets are quality upgrades only (FX-6).
- **Environmental/ambience particles** (leaves, rain visuals) — shipped P1 system,
  untouched except where weather intersects §7.
- **UI/HUD design** (health bars, cooldown slots, menus) — outside combat-world
  rendering; the Disabled-icon cooldown convention is noted, not specified.
- **PvP readability.** PvP is off by default (GAME.md §11); telegraphs for
  player-vs-player are not considered.

## Review Record

Adversarial review, 2026-07-23 — Reviewer-Opus: **revise**; Reviewer-Sonnet: **revise**.

Applied:
- FX-5 rewritten to visual-layer obligations only: cosmetics dropped on demote, paused fights freeze/resume (RFC-005 §R8), sim fate owned by RFC-010; FIZZLE reserved for real interrupts; the whiff-at-1-Hz clause deleted.
- §6 rebuilt on RFC-002's shipped model: five ladder channels × 3 stages with stage-scaled wash intensity, Poison/Stun rows added, coexisting Wet/Muddy coatings with a composition rule.
- §4 recipes unified with RFC-008: recipe fields live on `fx.*` documents, skills reference plain fx ids; the required §7.1 field delta is stated; `kInherit` sentinel resolves `blast_combo`'s glow from `Effect.element` at detonation.
- §1.3: `d = (base_damage × ring_damage_scale) / kPlayerMaxHp` (no per-ring player-HP table); "highest tier wins" rule; tier-1 control clause tightened to `0 < cc < 10`; RFC-001 V2 / RFC-005 §R4 floors reconciled via an explicit max-of-floors rule; enforcement stated honestly (RFC-005 R7 #5 implements it; RFC-008 must add a check — V30 is insufficient).
- §2: struct size corrected to 35 B packed / 40 B aligned; wire cost ≈ 336 B including `kTiles` arrays; cap overflow now tier-prioritized (higher tier cancels lowest with cooldown refund, else a clean refused-Hold no-op for RL); RFC-005 validator must budget entity/add telegraphs against the 8-cap.
- FX-3: `kLoop` is RFC-004's requested zero-sim-state derived channel (computed from the owner's published record), outside the FX-4 effect budget and unevictable.
- §1.4: CHARGE skipped at `total ≤ 5`; decal-less tier-0 lifecycle defined on the §1.6 source-cue stack.
- §1.6 / R7 / §7: sprite jitter and decal tremble folded under "reduce screen shake & motion", scalable to zero; fairness reads never removed.
- RL Considerations: observation claim corrected — RFC-007 v1 carries no telegraph geometry, so shape-exact dodge learning is scoped to point/circle; cone/line geometry flagged as a possible RFC-007 addition.

Unresolved: none. The reviewers' "clamp hostile tier-0 to 8" was applied against RFC-001's *current* two-tier V2 (4 baseline / 8 heavy-committed) rather than the stale flat-8 citation; RFC-005's validator already gained the §1.3 check the Sonnet-only finding said was missing, so only the RFC-008 half required a fix.

Reconciliation: §6 wash table re-keyed to the canonical channel set — Frost→Cold, the Poison row replaced by the Earth ladder (Encumbered/Mired/Root, reusing P2's mud hue), Stun→Stagger (Unsteady/Staggered/Knockdown); the Muddy coating row removed (folds into Earth/Mired), leaving Wet the sole coating — per RECONCILIATION.md ruling 1.
