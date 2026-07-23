# RFC-006: Visual FX & Telegraph Standards

- **Status:** Draft
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
fx:        { impact: slash_heavy, recipe: rock_red }               # sheet + tint recipe
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
  Shocked↔Thunder, Muddy↔Rock) so the combo system (GAME.md §7) teaches its own mapping.
- Future schools (Plant/Water/Light/Darkness/Wind/Death) exist as icons only and get
  palette rows **only** when they become real elements — out of scope for v1.

#### 1.3 Danger tiers and minimum lead times

The single most load-bearing table in this RFC. A telegraph's **tier** is derived from
what the hit can do; the tier dictates the minimum wind-up. RFC-005 and RFC-008
validators MUST reject abilities that violate it.

Let `d` = expected damage as a fraction of the **reference player HP for the content's
ring/realm** (reference values owned by RFC-009), and `cc` = hard-control duration in
ticks (stun/root/freeze from RFC-002).

| Tier | Qualifies when | Min wind-up | Required cues |
|---|---|---|---|
| 0 `light` | `d < 0.10` and `cc == 0` | **5 ticks** (0.5 s) (tunable) | source cue only (decal optional) |
| 1 `moderate` | `0.10 ≤ d < 0.25` or `cc < 10` | **8 ticks** (0.8 s) (tunable) | source cue + ground decal |
| 2 `heavy` | `0.25 ≤ d < 0.50` or `10 ≤ cc < 20` | **12 ticks** (1.2 s) (tunable) | source cue + ground decal |
| 3 `deadly` | `d ≥ 0.50` or `cc ≥ 20` | **16 ticks** (1.6 s) (tunable) | source cue + ground decal + imminent flash mandatory; boss pose swap where the sheet has one |

Tier is computed at authoring/validation time from the ability's data (RFC-008), not at
runtime. Sanity check against shipped values: the Samurai's attack (`kBossAttackWindup
= 10`, 20 damage on a ~100 HP player → `d = 0.2` → tier 1, min 8 ✓) and charge
(`kBossChargeWindup = 14` ≥ 12 for tier 2 ✓) already conform.

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

#### 1.5 Truth-in-advertising (hitbox = decal)

The resolution shape used by the simulation and the decal drawn by the renderer are
computed from the **same replicated record** (§2). Tolerance is asymmetric in the
player's favor: a player whose center is outside the shape by any margin is safe; a
player inside by less than **0.25 tile** (tunable) is also safe. Implementations MUST
NOT resolve hits from a different (e.g. padded) shape than the one drawn.

#### 1.6 Source cues on walk-only monsters

Because monster sheets have no attack frames, the source cue is standardized as an
overlay stack at the monster's position (all existing behaviors, now normative):

1. **Shake**: sprite jitters ±1.5 px world-space during wind-up (exists).
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

struct Telegraph {                    // 28 bytes; replicated in the chunk view
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
  memory one (§8 R4). On overflow the *newest* commit is refused by the sim (the ability
  fails to cast) — never silently drawn-but-not-resolved or resolved-but-not-drawn,
  which would break §1.5.
- Wire cost at the cap: 8 × 28 B ≈ 224 B/chunk/tick in the worst case, comparable to the
  existing effect list, and only in chunks with active combat — acceptable for the
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

A **recipe** turns one packed sheet into a family of skills (umbrella §14). Recipes are
data (hosted in RFC-008 skill definitions); this RFC defines their fields and legality.

```cpp
struct FxRecipe {                    // referenced by name from skill JSON
    EffectKind base;                 // which packed strip to play
    std::uint8_t tint_r, tint_g, tint_b;   // multiplicative pass (255,255,255 = none)
    std::uint8_t glow_alpha;         // additive silhouette pass in the element hue; 0 = off, max 140
    Element glow_element;            // picks the additive hue from §1.2
    std::uint8_t scale_eighths;      // 8 = 1.0x; range 4..24 (0.5x..3x)
    EffectKind trail;                // second strip played behind a moving user; kCount = none
    FxPlayMode mode;                 // §9, FX-3
};
```

Two-pass rule (structural): the multiplicative tint pass can only darken/shift the
sheet; brightening variants MUST use the additive glow pass. One pass alone cannot
express "red-hot rock".

Canonical v1 recipes (names structural, numbers tunable):

| Recipe | base | tint | glow | Reads as |
|---|---|---|---|---|
| `rock_plain` | kEarth | none | — | Rock spell (as shipped) |
| `rock_red` | kEarth | `255,120,90` | Fire 90 | **Meteor** — red rock + heat |
| `rock_frost` | kEarth | `170,210,255` | Ice 70 | **Ice boulder** |
| `ice_flash` | kIce | none | Ice 60 | frost burst |
| `fire_burst` | kFire | none | Fire 80 | flame burst |
| `shock_arc` | kShock | none | Thunder 90 | thunder strike |
| `blast_combo` | kBlast | none | element of the *detonating* status | combo detonation, hue tells which combo |

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

One status per creature (P2 decision), so filters never stack. The multiplicative wash
colors are the shipped P2 values (canonical); the motif overlays are new and drawn on
L3. All statuses use *both* channels (accessibility, same argument as §1.2).

| Status | Wash (multiplicative) | Motif overlay (additive, L3) | Extra rule |
|---|---|---|---|
| Frozen | `140,210,255` | 3 static frost notches on the sprite rim | **animation freeze-frame**: the walk cycle halts on its current frame — free, and the strongest read of the five |
| Burning | `255,150,90` | Fire strip frames 0–3 at 0.5× scale, looping at the sprite's feet | — |
| Wet | `150,190,255` | 1-px drip line under the sprite every 8 ticks | — |
| Muddy | `180,150,110` | dark band, alpha 90, over the lower third of the sprite | — |
| Shocked | `255,245,130` | one jagged polyline across the sprite, redrawn every 3 ticks (same generator as the Thunder decal motif) | — |

Zones render on L1 as their element/status hue at fill alpha 60 (tunable), outline
alpha 140, with the status motif scattered inside — a Wet zone visibly *is* the Wet
status painted on the floor. No new art.

### 7. Battlefield-state render filters (visual side of §16)

Battlefield states are owned by RFC-010; this RFC fixes only their render vocabulary so
implementations converge:

| State (from RFC-010) | Visual filter | Caps |
|---|---|---|
| Earthquake | camera shake ±2 px at 10 Hz max; L1 decals additionally tremble ±1 px at 8 Hz ("telegraphs tremble", umbrella §16) | shake respects the Options "reduce screen shake" toggle; gameplay accuracy effects are RFC-010's, not the renderer's |
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
  existing "reduce screen shake" toggle live in the shipped Options screen.
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
  renderer's age→frame map: `kOneShot` = today's behavior; `kLoop` = `frame = age %
  frames` for channel/zone/status motifs; `kHold` = clamp to last frame (e.g. debris
  settling via a recipe frame window, §4). Sim-side eviction still uses the record's
  lifetime; `kLoop` records are evicted by their owner (zone expiry, status cure), not
  by age.
- **FX-4 — Effect record extension.** `Effect` grows `element` (for recipe hue),
  `recipe` id, and `rot` (uint8, 256-step, for cones/projectiles). Budget stays small:
  ≤ 16 bytes/record, cap 24 effects/chunk (tunable) with a drop policy of *oldest
  cosmetic first*; records flagged as telegraph-adjacent (IMPACT flashes, FIZZLE) are
  never dropped before cosmetics — dropping a promise's resolution is a §1.5 violation.
- **FX-5 — LOD behavior.** Combat visibility is guaranteed by construction: the beacon
  interest set keeps every chunk within 5×5 of a player at active LOD (10 Hz), so any
  telegraph a player can see ticks at full rate. Requirements for the edges: (a) a chunk
  demoted below active LOD MUST clear its effect list and fizzle its telegraphs (they
  are sub-second transients; replaying them on wake is a lie about *when*); (b) monsters
  whose beacons expired resolve pending wind-ups as whiffs at 1 Hz — cheap, and no
  player can observe the difference; (c) on chunk wake, any telegraph with
  `left > total` (corrupt/stale) is dropped without impact. This satisfies the
  "tolerate being ticked at reduced rate or slept" constraint with zero new mechanisms.
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
  there; §6 maps each status id to exactly one wash+motif row. New statuses added by
  RFC-002 MUST add a row here (structural).
- **RFC-003 (Physics & Materials):** knockback/impulse outcomes are RFC-003's; this RFC
  only notes that materials may modulate *impact FX choice* via recipes (e.g. stone
  impacts use `rock_plain` debris) — no physics semantics here.
- **RFC-004 (Terrain & Combat Entity):** entity spawn/death FX and hazard-activation
  telegraphs (`kTiles`) use this grammar; crater/scorch decal *state* is RFC-004's,
  their L0 render slot is §3's.
- **RFC-005 (Boss Ability Authoring):** every authored boss ability declares
  `{shape, tier}`; the authoring validator enforces §1.3 minimums, §2's cap, and R4's
  screen budget. Pose availability per boss sheet is RFC-005's data; §1.6 rule 4 says
  when a pose is mandatory (tier 3, where the sheet has one).
- **RFC-007 (RL Observation):** see below.
- **RFC-008 (Data-driven Skills):** hosts `telegraph:` and `fx:` blocks in skill JSON
  using this RFC's vocabulary (shapes, tiers, recipe names). RFC-006 is the schema
  authority for those two blocks; RFC-008 for everything else.
- **RFC-009 (Damage & Build-up):** owns the reference-HP tables and expected-damage
  computation that §1.3's `d` is evaluated against; tier thresholds live here.
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
- **Observation cost:** telegraphs add nothing to the observation beyond what RFC-007
  already carries (`winding_up`, cooldowns, committed target offsets); the decal is
  derived data. Policies never consume pixels.
- **Whiff semantics** (T2/T5) make the dodge a stable learning signal in both
  directions: the boss's policy learns that commitment has a cost, the defending agent
  (sparring) learns that leaving the shape always works.

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
| 1024² overworld with LOD / sleep | FX-5: clear-on-demote, whiff-at-1 Hz, drop-stale-on-wake |
| Server-authoritative, cheap replication | Telegraphs are capped chunk-view records (≤ 224 B/chunk worst case, §2), same channel as effects |
| RL: one policy per archetype, dojos visible in-world | Parity rule keeps dojo spectating honest (players watching training see real telegraphs); nothing per-individual anywhere |

## Open Questions

1. **Tier reference HP.** §1.3 evaluates `d` against a per-ring/realm reference player
   HP owned by RFC-009. If RFC-009 lands with gear-driven HP spreads > ~2×, tiering by
   expected fraction may need a percentile definition (e.g. `d` against the 25th-
   percentile HP for the content's ring) — needs RFC-009's numbers first.
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
