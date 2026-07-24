# RFC-005: Boss Ability Authoring

> Status: **Accepted (revised after review)**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §2 (Telegraph-first), §14 (Asset Reuse)
> Companion RFCs referenced by number; see the set list at the end of the umbrella spec.

---

## Summary

This RFC defines **how a designer turns a shortlisted boss sprite sheet into a playable,
RL-trainable boss with zero new art**. It specifies:

- the **pose-capability model**: what a boss sheet can legally be asked to show, measured at
  atlas-build time, never assumed from filenames;
- the **boss kit**: a per-boss data file binding 2–4 abilities to poses, telegraphs, tick-counted
  pipeline phases, and a declarative generation-0 behavior script;
- the **phase machine** for two-phase bosses (Trans sheets), and the FX-overlay telegraph rule for
  every pose-less actor a boss summons;
- **difficulty tiers** with a hard readability floor, so harder never means less readable;
- the **authoring workflow** from idea to committed boss: kit file → validator → dev room →
  gen-0 checkpoint → worldgen registration.

The existing hand-written Samurai boss (`src/world/boss.hpp`, `step_bosses` in
`src/world/chunk_actor.hpp`) is generation 0 of this system: this RFC generalizes exactly that
machinery into data, and the shipped Samurai's numbers are reproduced by the example kit in
§R2/§R5 (one deliberate generalization: the shipped boss resolves its strike the tick wind-up
hits zero; the kit's `active` phase generalizes that instant to a window, with `active: 2`
subsuming the shipped behavior).

## Motivation

The pack audit (2026-07-23) fixed the raw material once and for all:

- **20 boss sheets, only ~11 with real Attack/Charge poses.** A boss kit that asks for a pose the
  sheet does not have either shows the wrong frame or silently falls back — both are bugs a
  designer discovers late and expensively.
- **All 66 regular monsters are 64×64 4×4 walk-only.** Anything a boss summons cannot pose at all;
  its telegraphs must be FX overlays or they do not exist.
- **Zero art budget.** Every boss must be assembled from what is already in the pack, and the
  authoring system must make it *impossible* to reference art that is not there.

Today the one shipped boss is ~110 lines of hand-written C++ (`boss.hpp` + `step_bosses`). That
was the right way to build the first one; it is the wrong way to build the next seven. Hand-writing
each boss means: each boss's slot bindings drift (RFC-007's fixed action table needs slots
declared, not discovered);
telegraph readability depends on each author re-deriving the same dodge math; and nothing stops a
kit from referencing an unpacked FX strip — the exact class of bug the old `kEffectLife=6`
truncation was (§C, "Engine gaps").

A data-driven kit with a build-time validator turns "is this boss buildable with our art?" from a
playtest discovery into a build error.

## Guide-level Explanation

### As a designer

You want a new boss. The whole loop, no code:

1. **Pick a sheet from the shortlist** (§R1). The shortlist is not taste — it is the measured set
   of sheets with usable Attack/Charge poses. Dragons and the idle-only bosses are not on it and
   the validator will refuse them.
2. **Write a kit file** — `assets/bosses/tengu.json` (format per RFC-008). You declare: the sheet,
   the archetype (which RL policy family it belongs to — RFC-007's `boss.*` policy ids), an
   element (one of the four), 2–4 abilities each bound to a pose the sheet really has, tick counts
   for wind-up / active / recover, a danger tier and anchor per RFC-006's telegraph standard
   (§1.3), and a short ordered rule list that is the boss's generation-0 brain.
3. **Run the validator.** It checks your kit against the *pose manifest* (measured from the PNGs at
   atlas build), the packed-FX list, the room geometry, the RL action budget, and the readability
   floor. Every failure is a one-line, actionable message ("`gust` binds pose `Shoot` but
   `Boss/Tengu` has no Shoot row").
4. **Fight it yourself.** `mmo_sim --boss tengu --tier 2` drops you in a dev copy of the 10×7 room
   with the dev fairness-queue skip. The gen-0 script is already playing it — no RL needed to
   playtest the kit.
5. **Pre-train generation 0** offline (RFC-007) and commit kit + checkpoint together.

At no point do you draw anything, and at no point *can* you accidentally require something drawn.

### As a player

You open a dojo door in a tier-3 village, or a challenge-realm gate, because you chose to. Inside
a 10×7 room a Giant Tengu waits. Every blow it throws is announced first: it snaps to a real
attack pose and a ground arc glows where the blade will land; you have most of a second to not be
there. At half health it stops, its Trans animation plays — visibly a transformation, not a stagger
— and phase two is faster and adds a move, but every wind-up is still at least as readable as the
slowest player dodge. If you leave and never come back, the boss stands in its room forever. It
never follows you out, never gets stronger on a clock, and its room simply sleeps when nobody is
there.

## Reference-level Design

Tick rate throughout: **10 Hz** (the sim's rate). All durations are ticks; none are wall-clock.

### R1. The shortlist and the pose-capability model

#### Pose vocabulary

```
PoseCap = { Idle, Walk, Attack, AttackL, AttackR, Charge, ChargeL, ChargeR, Shoot, Trans, Hit, Dead }
```

This is the *complete* vocabulary a kit may bind. It is a superset of the shipped
`BossPose {Idle, Walk, Attack, Charge}` (`boss.hpp`); `Shoot` and `Trans` are the two additions
this RFC introduces, and renderer support for them is part of implementing it.

`Attack` and `Charge` are the **non-directional** members: the pose-manifest pass records a
sheet's single attack (or charge) row as plain `Attack`/`Charge`, and a directional L/R pair as
`AttackL`+`AttackR` (`ChargeL`+`ChargeR`) — never both forms for one family. A kit always binds
the *family* name (`"Attack"`): with `directional: true` the manifest must contain both L and R
rows and the renderer resolves the side from facing (exactly the shipped `BossPose::kAttack`
behavior); with `directional: false` the manifest must contain the plain row, which is drawn for
either facing. Four of the eight shortlisted sheets (Bamboo, Frog, Racoon, Cyclop) are
plain-`Attack` sheets; only the Samurais are directional.

#### Pose manifest — measured, not asserted

`tools/build_atlas.py` gains a `--pose-manifest` pass that emits `assets/gen/boss_poses.json`:
for each boss sheet, the set of `PoseCap` rows actually present, with frame counts, measured from
the source PNGs/regions (the same measure-don't-guess discipline that found the door column in R7
and the 47-mask sets in R4). **The manifest is the only authority the validator consults.** The
table below is the audited snapshot for orientation; if it ever disagrees with the manifest, the
manifest wins.

| Sheet | Poses beyond Idle/Walk (audit 2026-07-23) | Archetype (RFC-007 policy id) | Element (tunable) | Role |
|---|---|---|---|---|
| `GiantRedSamurai` | AttackL/R, ChargeL/R (directional) | `boss.melee_bruiser` | Fire | **First RL boss** — shipped gen-0 |
| `GiantBlueSamurai` | AttackL/R, ChargeL/R (directional) | `boss.melee_bruiser` | Ice | Reskin of the same archetype/policy |
| `GiantBamboo` | Attack, Charge | `boss.zone_controller` | Rock | Rooter/bruiser (spikes, walls — RFC-004 entities) |
| `Squids` | **Shoot only** | `boss.artillery` | Ice | Ranged artillery; no melee ability permitted |
| `GiantFrog` | Attack; smallest body (~2.5 t) | `boss.ambusher` | None | Hopper — dash-kind ability skinned as a hop |
| `GiantRacoon` / `GiantRacoonGold` | Attack; Gold = elite reskin | `boss.elite_bruiser` | None | Trickster; Gold is tier-4 elite, same policy |
| `Tengu` | Attack, **Trans** | `boss.two_phase` | Thunder | **The** two-phase boss of v1 |
| `DemonCyclop` | Attack | `boss.baseline` | None | Baseline/teaching boss |

The `archetype` value **is** the RFC-007 §4 policy id, verbatim — no local short names, no lookup
table. A kit whose `archetype` is not a registered `boss.*` policy id fails validation.

**Hard exclusions (validator-enforced):** `Dragon*` (multi-part segment rigs, no baked poses),
`GiantSlime` / `Flam` / `Spirit` (idle+hit only). A kit naming any of these fails validation with
a pointer to this table. They may return as scripted set-pieces someday; they are not RL bosses
and not authorable kits in v1.

Red/Blue Samurai — and any future palette twins — share **one archetype and one policy**
(GAME.md §10: policy per archetype, never per individual). They differ only in element and stat
block. This is also why the archetype count stays inside the 10–15 policy budget: 8 kits above →
7 archetypes.

### R2. The boss kit — data shape

Kits are authored in the RFC-008 definition format and compiled by the asset pipeline into the
same kind of constexpr table the sim and renderer already share (`abilities.hpp` precedent —
whether the compiled form is codegen or runtime-loaded is RFC-008's decision; the invariant this
RFC requires is *one table, read identically by sim and renderer on every node*).

```jsonc
{
  "boss_id": "samurai_red",          // unique, stable — save files and checkpoints key on it
  "sheet": "Boss/GiantRedSamurai",   // must exist in the pose manifest AND in §R1's shortlist
  "archetype": "boss.melee_bruiser", // RFC-007 §4 policy id, verbatim; one policy per archetype
  "display_name_key": "boss.samurai_red",  // localization key, not literal text

  "element": "Fire",                 // Fire | Ice | Rock | Thunder | None — nothing else, v1
  "scale": "Giant",                  // umbrella §10 tier → build-up/knockback scaling (RFC-009)
  "material": "Flesh",               // RFC-003 material table

  "base": {                          // tier-1 stat block; RFC-009 owns the damage math
    "hp": 700,
    "contact_damage_key": "boss.samurai.contact",
    "approach_speed": 2.5,           // tiles/s
    "leash_ticks": 50,               // no target this long -> reset to spawn (existing behavior)
    "respawn_ticks": 3000            // world-tick based; see §R8 (LOD)
  },

  "room": { "w": 10, "h": 7 },       // the interior floor the kit is authored FOR; validator
                                     // rejects any range that does not fit (§R7 checks)

  "phases": [                        // 1 or 2 entries. 2 entries REQUIRE Trans in the manifest.
    { "hp_above": 0.5, "slots": ["cleave", "charge_dash"] },
    { "hp_above": 0.0, "slots": ["cleave", "charge_dash", "blade_flurry"],
      "trans": { "ticks": 16, "damage_taken_scale": 0.5, "clears_status": false },
      "modifiers": { "cooldown_scale": 0.8, "approach_speed_scale": 1.15 } }
      // `modifiers` keys are a CLOSED whitelist: { cooldown_scale, approach_speed_scale }.
      // Nothing that touches windup, active, telegraph windows, or dash speed is expressible
      // here — the §R4 readability floor holds in every phase, not just phase 1 (validator #6).
  ],

  "abilities": { /* see below */ },

  "script": [ /* generation-0 brain, see §R5 */ ],

  "tiers": "default"                 // or an inline override of §R4's table
}
```

#### Per-ability shape

```jsonc
"cleave": {
  "pose": "Attack",                  // PoseCap family; directional:true resolves to AttackL/
  "directional": true,               //   AttackR by target side (validator requires BOTH L and R
                                     //   rows); directional:false requires the plain Attack row
  "telegraph": {                     // RFC-006 owns the visual standard; the kit only DECLARES
    "tier": 1,                       //   the §1.3 danger tier (0–3) + anchor. Windup must meet
    "anchor": "self"                 //   the tier's minimum wind-up; required cues follow from
  },                                 //   the tier; drawn shape derives from shape.kind (below)
  "timing": { "windup": 10, "active": 2, "recover": 15 },   // ticks; windup >= floor (§R4)
  "shape": { "kind": "arc", "radius": 2.6, "arc_deg": 180 },
  "damage_key": "boss.samurai.cleave",   // RFC-009 table key — kits carry NO damage numbers
  "cooldown": 25,                    // ticks from commit; must be > active + recover (validated).
                                     //   The FSM re-enters ENGAGED only after windup+active+
                                     //   recover, so a cooldown shorter than that sum is simply
                                     //   already expired at the next decision — legal, not a bug
  "applies_status": null,            // or one of RFC-002's statuses, build-up per RFC-009
  "fx": { "impact": "kSlashHeavy" }, // EffectKind names; must be packed (validated); each FX
                                     //   phase <= its effect_life_of() — chain effects for longer
  "sfx": "Sword2"                    // optional; must be a packed sound (validated)
}
```

Ability `shape.kind` vocabulary (all resolved by the chunk, mirroring `AbilityShape`):

| kind | params | hit test | escape distance (for §R4 floor) |
|---|---|---|---|
| `arc` | radius, arc_deg | within radius AND within half-angle of facing | `radius` |
| `ring` | radius, hole | radius ≥ d > hole around boss | `radius − hole` |
| `dash` | speed, ticks | body contact during Active, path clamped to room | `0.5 + body_half_width` (side-step) |
| `line` | length, width | rectangle from boss along facing | `width/2 + 0.5` |
| `projectile` | speed, radius | travel via the existing arrow machinery; impact circle | `radius` |
| `zone` | radius, zone_kind, zone_ticks | RFC-004 lingering zone (reuses `SpawnZone`) | n/a (zones are dodged by not standing in them) |
| `summon` | monster_kind, count | spawns walk-only adds (§R3) | n/a |

Each `shape.kind` maps to an RFC-006 `TelegraphShape` for drawing — `arc`→`kCone`,
`ring`→`kRing`, `line`→`kLine`, `dash`→`kLine` (along the committed path),
`projectile`→`kCircle` (at the landing point, per RFC-006 §3's lobbed-shot rule),
`zone`→`kCircle`, `summon`→`kTiles`. The kit never declares the drawn shape; it is derived,
so hitbox = decal (RFC-006 §1.5) holds by construction.

Slot count: **2–4 abilities per kit** (min 2 so there is a decision to learn; max 4 because
RFC-007's fixed action table has exactly four Cast slots, §R5 — the cap originates there). The
player-facing mirror of this constraint (basic + two abilities) is RFC-001's; this bound is
chosen so boss fights stay pattern-readable, not for symmetry.

### R3. Telegraphs: pose-bound for bosses, FX-overlay for everything pose-less

Rules, in decreasing precedence:

1. **A boss ability whose sheet has a matching pose family MUST bind it** as its source cue — the
   pose swap RFC-006 §1.3 makes mandatory at tier 3 "where the sheet has one" is mandatory here
   at *every* tier where the sheet has one. The pose swap is the strongest read in the game and
   it is free — refusing it is authoring malpractice, so the validator enforces it. The ground
   decal (required by RFC-006 from tier 1 up) is drawn alongside, derived from `shape.kind`.
2. **A boss ability with no matching pose** (e.g. a summon, an aura, Squid's everything-but-Shoot)
   binds `pose: "Idle"` and relies on RFC-006's standardized overlay source cue (§1.6, the same
   one walk-only monsters use) plus the tier's required ground decal. At most **one** such
   pose-less *strike* ability per kit (tunable): a boss whose blows all look like idle-standing is
   unreadable, which is the same failure §R4's floor exists to prevent.
3. **Every summoned add is a walk-only monster and telegraphs exclusively by FX overlay** at/around
   its own position, plus the existing red-pulse wind-up tint (`Creature::windup`, already
   published state). Adds reuse the standard creature wind-up machinery unchanged; a kit cannot
   give an add abilities — adds fight with the basic creature attack only.
4. **Telegraph duration = wind-up duration**, always. A telegraph that outlives or undershoots its
   wind-up is lying; the chunk derives the telegraph window from `timing.windup`, and RFC-006's
   renderer contract draws it for exactly that window. Ground decals therefore must be drawable
   for up to the longest authored wind-up (Trans-heavy kits: ~16 ticks) — longer than
   `kMaxEffectLife = 14`; RFC-006 owns whether that is a looped effect or a dedicated telegraph
   channel, and this RFC only requires the window contract.

Summon caps: `count ≤ 4` alive adds per boss (tunable); a summon cast while at cap resolves as
`Hold` (no-op, cooldown not spent). The 10×7 room is 70 tiles; four Giants-plus-adds is already a
crowded read.

### R4. Difficulty tiers — and the readability floor

Two orthogonal axes, deliberately kept apart:

- **Tier** (this RFC): a *static* multiplier set + kit delta, fixed per room placement at worldgen.
  It never changes while the world runs.
- **Generation** (RFC-007): how good the *policy* is. It grows only through visible dojo training,
  stays in its dungeon, and is capped by checkpoint gating.

Default tier table (all values tunable; kits may override via `"tiers"`):

| Tier | Name | HP × | Damage × | Cooldown × | Slots available | Phases | Notes |
|---|---|---|---|---|---|---|---|
| 1 | Initiate | 1.0 | 1.0 | 1.15 | first 2 | phase 1 only | the teaching fight |
| 2 | Adept | 1.4 | 1.25 | 1.0 | first 3 | all | the intended fight |
| 3 | Master | 1.9 | 1.5 | 0.85 | all | all | |
| 4 | Elite | 2.4 | 1.75 | 0.7 | all | all | reskin sheet if one exists (`GiantRacoonGold`), else RFC-006 elite tint |

**The readability floor — the one rule tiers can never touch:**

> Tier multipliers apply to HP, damage, and cooldowns. They **never** apply to `windup`,
> `active`, telegraph windows, or dash speed. A blow at tier 4 is exactly as readable as at
> tier 1; there is only more of it and it costs more to eat.

Floor formula, checked by the validator per ability per kit (numbers tunable):

```
windup_ticks ≥ max( kWindupFloor,
                    tier_min_windup( telegraph.tier ),
                    ceil( kTickRate × escape_distance / kPlayerBaseSpeed ) + kDodgeGrace )

kWindupFloor     = 6    ticks   (tunable)
kTickRate        = 10   Hz
kPlayerBaseSpeed = 6.0  tiles/s (single sourced from `kPlayerSpeed`, tiles.hpp — the un-buffed
                                 on-foot walk speed; never hand-copied into the validator)
kDodgeGrace      = 2    ticks   (tunable — reaction margin)
escape_distance  = per shape.kind, table in §R2
tier_min_windup  = RFC-006 §1.3's danger-tier minimum (5 / 8 / 12 / 16 ticks for tiers 0–3) —
                   the floor RFC-006 explicitly delegates to this validator
```

Worked check against the shipped Samurai: cleave (tier 1) arc radius 2.6 → geometric term
`ceil(10 × 2.6 / 6.0) + 2 = 7`, tier-1 minimum 8 governs → wind-up 10 passes with margin.
Charge dash (tier 2): escape `0.5 + body_half_width` with the Giant-scale collision half-width
(≈ 1.25, RFC-003's body table — never a hand-typed constant) → `ceil(10 × 1.75 / 6.0) + 2 = 5`,
tier-2 minimum 12 governs → wind-up 14 passes. Both match RFC-006 §1.3's own sanity check of the
shipped values (attack ≥ 8 ✓, charge ≥ 12 ✓). The shipped boss is legal under its own spec,
which is the sanity test of the formula, and the formula is why `kBossChargeWindup = 14` being
"the biggest telegraph in the game" is now a checkable claim instead of a comment.

Ceilings (umbrella §9/§17, GAME.md §10 constraint 3): tier caps at 4; dash speed caps at
`9.0 tiles/s` (tunable — just above the shipped 8.5); no kit value may produce an unavoidable hit
(a `ring` with `hole = 0` and `radius >` room half-diagonal is a validation error). Build-up
scaling by `scale: Giant` is RFC-009's; kits only declare the scale word.

Tone guardrail restated as mechanics: tier is chosen by *where the room is* (worldgen ring /
challenge realm), the player chooses to open the door, the boss never exits its room
(`leash_ticks` is already shipped behavior), and nothing about a kit references wall-clock time.

### R5. The behavior seam: script and policy over one action space

#### Action space (fixed and shared — never generated)

The action space is **RFC-007 §3's fixed 15-action table, verbatim and unmodified**: Hold,
Step N/E/S/W, Approach, Retreat, Cast slot 0–3 × {Direct, Lead}. `kActionCount = 15`, exactly,
for every archetype. Nothing about it is generated from the kit; what a kit contributes is only
its **slot bindings** — `abilities` in kit order occupy Cast slots 0–3 (hence the ≤ 4 cap, §R2).
Directional poses need no extra actions: facing is derived from `sign(dx)` at commit (RFC-007 §3).

- **Why fixed, not "as many as the kit needs":** the vendored DQN sampler hard-codes
  `kActionCount = 15` (`DqnAgent.cpp:25`); an action space *smaller* than 15 makes epsilon-greedy
  emit out-of-range indices that `TrainBatch` writes unchecked — an ASan-confirmed
  heap-buffer-overflow (ARCHITECTURE.md §7). A per-kit enumeration would be exactly that crash.
  Pinning the table at 15 makes the segfault class structurally unreachable, and the vendored
  core runs unmodified.
- **Unused indices are total:** Cast actions for absent slots coerce to `Hold` (RFC-007 §3
  execution rule 2), so every archetype executes all 15 indices safely.
- **Masking rule (sim-side, unconditional):** any Cast action referencing a slot that is absent,
  on cooldown, not in the current phase's `slots`, or below the current tier's slot count,
  **resolves to `Hold`**. The sim is thereby safe against *any* policy output, scripted or
  learned; making the mask visible to the learner is RFC-007's business.

Decision cadence: the brain is consulted every `kDecisionPeriod = 3` ticks (tunable; matches the
beacon cadence) and never while committed to a wind-up, dash, or Trans — a committed telegraph is
a promise (`step_bosses` already enforces this; it stays the law).

#### Generation-0 script — declarative, in the kit

Every kit MUST ship a `script`: an ordered rule list evaluated top-down at each decision tick,
first match wins. This is the drop-in body for `boss_policy(BossObs) → BossAction`, so the RL
seam (obs in, action out) is untouched.

Condition vocabulary (closed, validator-checked): `winding_up`, `dist > N`, `dist <= N`,
`cd_ready(slot)`, `hp_below(frac)`, `phase_is(n)`, `adds_alive < n`, conjunction with `&&`.
Action vocabulary: `hold`, `approach`, `retreat`, `step <n|e|s|w>`, `use <slot>` — each verb is
one entry of RFC-007 §3's fixed table (`use` emits Cast-Direct; facing auto-resolved from target
side for directional slots — exactly how the shipped script picks AttackLeft/Right). `dist` and
target side are measured against the **primary target: the nearest engaged player** (RFC-007 §2,
Block T) — multi-player target selection is RFC-007's, not re-specified here.

The shipped Samurai brain, transliterated (this is the normative example — it must reproduce
`boss_policy` in `boss.hpp` decision-for-decision):

```jsonc
"script": [
  { "if": "winding_up",                      "do": "hold" },
  { "if": "cd_ready(charge_dash) && dist > 4", "do": "use charge_dash" },
  { "if": "dist <= 2.6 && cd_ready(cleave)", "do": "use cleave" },
  { "if": "dist <= 2.6",                     "do": "hold" },
  { "do": "approach" }
]
```

Why mandatory: it is the playtest brain (fight the kit before any training exists), the RL
fallback GAME.md §10 demands ("behavior table per generation, player can't tell"), and the reward
baseline RFC-007 trains against.

### R6. The boss state machine

One FSM per boss body, stepped by the owning chunk (generalizing `step_bosses`):

```
        ┌────────────────────────────────────────────────────────────┐
        ▼                                                            │
      IDLE ──target beacon in room──► ENGAGED ─────────────────────► LEASH-RESET
        ▲                              │  ▲        (no target for      │
        │                              │  │         leash_ticks)       │ walk home,
        │                     decision │  │ recover                    │ full heal,
        │                        tick  ▼  │ done                       ▼ statuses cleared
        │                            WINDUP(slot) ── stunned ──► (windup cancelled → ENGAGED)
        │                              │ windup ticks elapse
        │                              ▼
        │                            ACTIVE(slot)      // arc lands / dash runs / projectile
        │                              │ active ticks   //   spawns / zone drops / adds spawn
        │                              ▼
        │                            RECOVER(slot)     // post-blow hold; boss_pose holds the
        │                              │ recover ticks  //   strike frame, then back to ENGAGED
        │                              ▼
        │                            ENGAGED ──hp ≤ next phase threshold──► TRANSITION
        │                                                                    │ trans.ticks,
        │                                                                    │ Trans pose,
        │                                                                    │ dmg × 0.5,
        │                                                                    │ un-stunnable
        │                                                                    ▼
        │                                                                 ENGAGED (phase n+1)
        └── DEAD ── respawn (world-tick delta ≥ respawn_ticks, §R8) ──► IDLE
```

Rules that keep it honest:

- **Wind-up cancellation:** stun is the only canceller (existing law); a cancelled wind-up
  refunds the cooldown. Freeze (RFC-002 ladder) pauses the FSM clock rather than cancelling —
  a frozen telegraph visibly holds, which is both fair and legible.
- **Phase threshold is checked in ENGAGED only** — a blow already in flight lands before the
  transformation; a kill during WINDUP/ACTIVE is a kill (TRANSITION never rescues a dead boss).
- **TRANSITION is not an immunity** (umbrella §9): damage lands at
  `trans.damage_taken_scale = 0.5` (tunable), build-up continues to accumulate, but stun/freeze
  *interrupts* nothing — the Trans strip always completes. One-phase kits never enter it.
- **Thresholds strictly decrease** across `phases[]` (validated); v1 caps at 2 phases because
  exactly one shortlisted sheet (Tengu) has a Trans row, and the validator requires the row for
  any kit with `phases.length > 1`.
- LEASH-RESET restores full HP and clears statuses/adds — the shipped anti-corridor-kiting rule,
  kept verbatim.

### R7. The validator — the whole point

`tools/validate_boss_kit.py`, run by the build (a kit that does not validate does not compile into
the table). Checks, each with the constraint it defends:

| # | Check | Defends |
|---|---|---|
| 1 | `sheet` exists and is on the §R1 shortlist; hard-excluded sheets rejected by name | asset audit |
| 2 | every `pose` reference exists in the **measured** pose manifest; `directional: true` requires both L and R rows, `directional: false` requires the plain (non-directional) row | walk-only/pose reality |
| 3 | pose-capable sheet + strike ability ⇒ pose-bound telegraph (rule R3.1); ≤ 1 overlay-only strike ability | readability |
| 4 | every `fx` name is a packed `EffectKind`; every declared FX phase ≤ `effect_life_of(kind)`; every `sfx` is a packed sound | the `kEffectLife` class of bug |
| 5 | telegraph `tier` ∈ 0–3; `windup ≥` RFC-006 §1.3's tier minimum; the tier's required cues are satisfiable (tier 3 ⇒ pose swap bound where the sheet has one); declared tier cross-checked against the RFC-009-resolved `damage_key` (warning-only until RFC-009 lands, then error — same ratchet as #14) | RFC-006 §1.3 delegation; readable at every damage level |
| 6 | wind-up floor formula (§R4, including the tier minimum) per ability; tier table never scales wind-up/active/dash-speed; phase `modifiers` keys ∈ {`cooldown_scale`, `approach_speed_scale`} only; dash speed ≤ 9.0 | tone guardrail: readable at every tier and every phase |
| 7 | every `shape` fits the declared room: radius/length ≤ ⌊min(room.w, room.h)/2⌋ + 1 (room-derived, not a flat constant — 4 tiles for the 10×7 room), ring hole sane, dash clamp assumed; corner check: from every floor tile, some point ≥ `escape_distance` outside the shape is reachable within the room | 10×7 room; no cornered-and-undodgeable hits |
| 8 | `2 ≤ slots ≤ 4` bound onto RFC-007's Cast slots 0–3; `action_count == 15` always (never fewer — fewer is the segfault); gen-0 checkpoint `output_size == 15` when one is present | DQN hard-cap pitfall |
| 9 | `cooldown > active + recover` per ability; `phases[].hp_above` strictly decreasing; 2 phases ⇒ Trans row exists | FSM sanity |
| 10 | `summon.monster_kind` is a real walk-only monster; `count ≤ 4`; adds have no abilities | overlay-telegraph rule |
| 11 | `element ∈ {Fire, Ice, Rock, Thunder, None}` | 4-element scope |
| 12 | `script` parses against the closed vocabularies; every referenced slot exists; last rule is unconditional | gen-0 mandatory |
| 13 | all durations are tick integers; no field of wall-clock or real-date type exists in the schema | LOD/no-clock rule |
| 14 | `damage_key`s resolve against RFC-009's table (warning-only until RFC-009 lands, then error) | no damage numbers in kits |

### R8. Simulation-LOD, replication, and persistence

- **Room sleeps, fight freezes.** A boss room chunk with no players follows the standard LOD
  ladder (10 Hz → 1 Hz → sleep). Every kit timer is a relative tick count decremented only when
  the chunk steps, so a sleeping fight is simply paused — legal because no kit field references
  world wall-clock (validator #13).
- **Respawn is the one world-time value**, computed like crops: record `death_world_tick`; on any
  step (including the wake step), respawn iff `world_tick − death_world_tick ≥ respawn_ticks`.
  A room asleep for an hour respawns its boss on wake without ever having ticked.
- **Replicated per-boss state is small and flat** (server-authoritative; leader is trust root):
  the existing `Creature` fields plus `boss_pose (u8)`, `windup (u8)`, `phase (u8)`,
  `active_slot (u8)`, `slot_cd[4] (u8 each)`, `fsm_state (u8)` — ≤ 9 bytes over today's
  creature record. Kits themselves are static data on every node (same compiled table),
  identified by `boss_id` — never shipped over the wire.
- **Persistence:** a mid-fight boss is not saved (matching "monster positions: not saved" in
  ARCHITECTURE.md §3); on world load a boss room comes back at IDLE, full HP, phase 1. The only
  persisted boss facts are the dojo's checkpoint generation (RFC-007) and the room registration
  (worldgen).

## Interactions with Other RFCs

- **RFC-001 (Ability System):** the phase pipeline (Cast→…→Persist) and the shape-resolution
  primitives (`AbilityStrike`, `LaunchArrow`, `SpawnZone`) are RFC-001's; boss kits *reference*
  them via `shape.kind` and add nothing new to the pipeline. Player kit shape (basic + 2) is
  RFC-001's scope.
- **RFC-002 (Status & Effect):** `applies_status`, freeze-pauses-the-FSM, and status clearing on
  leash-reset use RFC-002's framework; build-up magnitudes are not authored in kits.
- **RFC-003 (Physics & Material):** `material` and knockback/impulse behavior of dash contact.
- **RFC-004 (Terrain & Combat Entity):** zones, spikes, walls a kit's `zone`/`persist` effects
  spawn are CombatEntities per RFC-004; the kit only names the entity kind.
- **RFC-006 (Visual FX & Telegraph Standards):** owns the telegraph model this RFC declares
  against — `TelegraphShape`, the §1.3 danger-tier table and its per-tier required cues, decal
  art standards, elite tint, and how a >14-tick telegraph is drawn (loop vs dedicated channel).
  This RFC owns only *which* tier/anchor an ability declares, the `shape.kind`→`TelegraphShape`
  mapping, the duration contract (window = windup), and enforcing §1.3's floor in its validator
  (the delegation RFC-006 §1.3 states explicitly).
- **RFC-007 (RL Observation & Action Space):** owns `BossObs` extensions (phase bit, slot
  cooldowns, add count), reward shaping, training loop, checkpoint gating, primary-target
  selection, and the fixed 15-action table plus the `kActionCount` fix. This RFC hands it only
  the per-kit slot bindings (≤ 4 slots onto Cast 0–3) and a mandatory scripted baseline — never
  an action enumeration of its own.
- **RFC-008 (Data-driven Skill Definition):** owns the file format, loading/codegen strategy, and
  schema-versioning; this RFC's §R2 is a schema *in* that format.
- **RFC-009 (Damage, Resistance & Build-up):** owns every damage number behind `damage_key`, the
  Giant-scale build-up curves, and tier damage multiplier semantics.
- **RFC-010 (Battlefield Simulation):** owns the LOD ladder and chunk stepping this RFC's §R8
  leans on, and multi-room/instance orchestration.

## RL Considerations

1. **The seam is unchanged.** `boss_policy(BossObs) → BossAction` stays a pure function; the kit
   script is its generation-0 body, a trained network its generation-N body. No chunk code moves
   between generations — this is the property `boss.hpp` was written to protect, kept.
2. **One policy per archetype**, 7 archetypes from the current shortlist, inside the 10–15 policy
   budget with room for guard prototypes. Palette twins (Red/Blue Samurai) and elite reskins
   (RacoonGold) share their archetype's policy; only stats and element differ.
3. **Action masking degrades to Hold** deterministically (§R5), so an off-policy or stale
   checkpoint can never crash or cheat the sim — at worst the boss hesitates, which reads as
   natural.
4. **Two-phase = one policy.** Phase is an observation bit, the action table stays the fixed 15
   (all phases' slots bound onto Cast 0–3), and phase-locked Cast actions mask to Hold. Two
   networks per boss would double the training budget for one sheet's worth of content.
5. **Gen-0 must not be stupid** (ARCHITECTURE.md §7): the mandatory script *is* the pre-training
   opponent and the shipped fallback; the committed generation-0 checkpoint must beat or match the
   script before it replaces it (gate specifics in RFC-007).
6. **Obs additions this RFC implies** (final shape is RFC-007's): `phase`, `slot_cd[4]`,
   `adds_alive`, room-local walls already implicit in dx/dy clamping. Everything stays quantized
   int-ish for cross-machine determinism, as `BossObs` already is.
7. **Difficulty tiers do not fork policies.** A tier changes stats and slot availability, not the
   network; slot-availability masking makes one policy serve all four tiers. If tier-1 play with a
   tier-4-trained policy proves degenerate, per-tier *checkpoint selection* (not per-tier
   training) is the fallback — flagged as an open question.

## Asset & Engine Constraints Honored

| Constraint (audit 2026-07-23) | Where honored |
|---|---|
| Chill default; nothing counts down; difficulty waits to be found | §R4 tone paragraph: tier fixed at worldgen, opt-in door, leash keeps bosses home, no wall-clock fields (validator #13), sleeping rooms freeze |
| 66 monsters walk-only, zero attack anims | §R3: adds telegraph by FX overlay + red pulse only; kits cannot give adds abilities (validator #10) |
| ~11/20 boss sheets with real poses; fixed shortlist; Samurai first | §R1 shortlist + hard exclusions (validator #1); Samurai kit is the normative example and reproduces the shipped gen-0 |
| No Dragons / GiantSlime / Flam / Spirit as RL agents | §R1 hard exclusions, validator #1 |
| Player kit = basic + exactly two abilities | Out of scope here (RFC-001); boss slot bound 2–4 chosen independently in §R2 |
| Exactly 4 elements v1 | validator #11; other schools mentioned nowhere except as non-goals |
| 121 skill icons with Disabled twins | Not consumed by this RFC (boss kits have no player-facing icons); noted so no reviewer looks for it |
| `kEffectLife=6` truncation | Already fixed in-tree as per-kind `effect_life_of()` (tiles.hpp); this RFC designs around the *class* of bug: validator #4 checks every FX phase against the per-kind life, and >14-tick telegraphs are contracted to RFC-006 (§R3.4) instead of abusing the effect channel |
| No bespoke combo art; Magic/* FX and spinning projectiles not packed | validator #4 makes unpacked FX a build error, not a runtime surprise; kits wanting them (e.g. a blade-wave projectile) are authoring-blocked until the packing task lands |
| RL: DQN from RLDrive, one policy per archetype, 10–15 total; dojos visible; boss rooms 10×7 interior | §R5 adopts RFC-007's fixed 15-action table verbatim — `action_count == 15` always, making the <15 segfault unreachable (validator #8); §R1 archetype sharing under RFC-007's `boss.*` ids; `room` field + room-derived range checks (validator #7); dojo/room plumbing already shipped (worldgen `dojo_rooms`) |
| 1024² world, LOD to 1 Hz/sleep | §R8: relative-tick timers, world-tick respawn on wake (crop pattern) |
| Server-authoritative, first-node leader, cheap replication | §R8: ≤ 9 bytes of extra replicated state; kits are node-local static data |

## Open Questions

1. **Trans damage scale vs. "no absolute immunities."** 0.5 damage-taken during TRANSITION is a
   compromise; is a *short* full-immunity window (≤ trans.ticks ≤ 16) acceptable to the umbrella's
   §9 philosophy, or should even 0.5 be 1.0 with the transition simply being short? Needs a
   playtest, not an argument.
2. **Ranged boss in a 10×7 room.** Squid's kit (Shoot + Retreat/Step kiting) may degenerate — either the
   player corners it trivially or the room is too small for range to matter. Options if so: a
   bigger room variant for ranged archetypes (12×9, worldgen change), or an ink `zone` ability
   that makes cornering costly. Decide after the first Squid kit playtest.
3. **Per-tier checkpoint selection** (RL Consideration #7): does one policy per archetype serve
   all four tiers acceptably, or does tier-4 training make tier-1 fights read as unfairly sharp
   despite the wind-up floor?
4. **Does RacoonGold share the racoon *stat* table or only the policy?** Elite-as-tier-4-reskin
   (§R4) says stats come from the tier table; if the Gold sheet's silhouette reads as a different
   *creature*, players may expect a different kit, not a thicker one.
5. **Telegraph channel for >14-tick windows** — looped Effect vs. new channel — is RFC-006's to
   answer, but the answer affects validator #4's exact rule; blocked-on noted here so the two
   RFCs land compatibly.
6. **Should the gen-0 script vocabulary grow a `random_weight` construct** (mixed strategies) or
   stay strictly deterministic? Deterministic is testable and replayable; a purely deterministic
   script is also exploitable by pattern memorization — which may be *fine* for gen-0 whose whole
   job is to be beaten by training.

## Non-goals

- **Player abilities, loadouts, or the ability pipeline itself** — RFC-001.
- **Telegraph/FX pixel standards, decal art, tint recipes** — RFC-006; this RFC only declares
  tiers, anchors, and windows.
- **Damage formulas, resistances, build-up curves** — RFC-009; kits carry keys, never numbers.
- **DQN internals, reward design, training schedules, checkpoint format** — RFC-007.
- **The kit file format, loading vs codegen, schema versioning** — RFC-008.
- **Open-world monster behavior, stronghold raids, guard prototypes** — bosses live in rooms;
  everything outside a room is other RFCs' and other phases'.
- **Dragons and multi-part segment rigs** — explicitly excluded from v1 authoring; a future
  set-piece system may revisit them.
- **Elements beyond Fire/Ice/Rock/Thunder** — Plant/Water/Light/Darkness/Wind/Death exist as
  icons only and may appear in future-work notes only.
- **Loot tables, Essence rewards, dungeon economy** — P4/P8 roadmap items, not authoring.

## Review Record

Votes: Reviewer-Opus **revise** (8 mustFix) · Reviewer-Sonnet **revise** (8 mustFix) — the same eight underlying issues; all verified against source and applied.

- §R5 + validator #8: per-kit action enumeration replaced with RFC-007 §3's fixed 15-action table; `action_count == 15` always — fewer than 15 is the ASan-confirmed DQN sampler overflow (ARCHITECTURE.md §7).
- §R2/§R3 + validator #5: nonexistent "telegraph class registry" replaced with RFC-006's real model — declared danger `tier` + anchor, required cues per §1.3, drawn shape derived via a new `shape.kind`→`TelegraphShape` mapping.
- §R4 + validators #5/#6: RFC-006 §1.3's damage-tier wind-up floor threaded into the floor formula (`tier_min_windup`), with the declared tier ratchet-checked against RFC-009-resolved damage.
- §R4: `kPlayerBaseSpeed` corrected to 6.0 (`kPlayerSpeed`, tiles.hpp:1089 — 3.5 was the Boar stat); both worked checks recomputed; dash `body_half_width` sourced from RFC-003's body table.
- §R1/§R2 + validator #2: PoseCap gains non-directional `Attack`/`Charge`; manifest-recording and family-binding rules stated for the four plain-Attack sheets.
- §R2 + validator #6: phase `modifiers` restricted to closed whitelist {`cooldown_scale`, `approach_speed_scale`} — no phase can dip below the readability floor.
- §R1/§R2: archetype ids adopt RFC-007 §4's `boss.*` policy-id namespace verbatim.
- Validator #7: flat 8-tile shape cap replaced with room-derived cap (⌊min(w,h)/2⌋+1) plus a corner-escape reachability check.
- Minor (conceded by both): Summary pointer §R6→§R2 with active-phase generalization note; cooldown-vs-FSM timing comment; script verbs now map 1:1 onto RFC-007's table; primary-target selection cross-referenced to RFC-007 §2.

Unresolved objections: none — every mustFix upheld by either reviewer was verified sound and applied; the two downgraded findings (multi-player targeting, cooldown formula) were resolved as the minor cross-reference fixes both reviewers settled on.
