# Implementation Map — Unified Combat & Physics System → current engine

> Companion to `RFC_Unified_Combat_System.md` and the detailed RFC-001…010 set.
> For each umbrella section: what the engine already has, what is missing, and
> which detailed RFC owns it. Ends with a build order derived from the RFCs'
> declared dependencies.
>
> Ground rule carried over from everything built so far: the simulation stays
> **integer-only and deterministic** — every new mechanic must be a pure function
> of (seed, state, tick), because cross-compiler byte-equality (GCC vs MSVC) and
> the lock-free snapshot seam both depend on it.

## Umbrella section → engine status → owning RFC

| Umbrella § | Concept | Engine today | Gap | Owner |
|---|---|---|---|---|
| 1 | Ability pipeline (Cast→…→Expire) | Cast/wind-up = F2 telegraphs (`commit_windup`/`resolve_windup`, stun cancels). Impact = `AbilityStrike`. Persist = `Zone` with `effect_life_of`. | Channel stage, Travel as an interceptable entity, explicit Expire hooks. | RFC-001 |
| 2 | Telegraph-first | Already doctrine: windup 4/6/8 ticks, red pulse, whiff at stored aim, dodge-able; boss 10/14. | Standards, not structure. | RFC-005, RFC-006 |
| 3 | CombatEntity | Fragments: creatures (hp, team-by-kind), `Building::hp` (destroyable), zones (lifetime+aura, no hp/collision/team). | The unification — one chunk-owned chassis. | RFC-004 |
| 4 | Battlefield control | Zones already re-shape fights (kWet → `chain_shock`, kSmokeSuppress strips aggro). | Dynamic blockers (blocking today is static `prefab_blocks` + terrain); field states. | RFC-004, RFC-010 |
| 5 | Terrain evolution | Mechanism proven: `publish_overlay()` — same seam the deferred `lake_islands` item needs. | Combat→overlay scar writes (crater, rubble) + revert. | RFC-004 |
| 6, 11 | Attack physics, mass/knockback | Damage only; no knockback anywhere. | Impulse/mass columns, forced displacement in integer math. | RFC-003, RFC-009 |
| 7 | Interaction rules | **One rule already emergent**: kWet zone + Conduct skill → `chain_shock`. Hand-wired — exactly what the RFC forbids at scale. | The general rule table; kWet+shock becomes row #1. | RFC-003 |
| 8 | Materials | None. | Material enum + coefficient tables. | RFC-003 |
| 9, 10 | Build-up, decay, Scale | Binary stun exists (cancels wind-ups). | Fixed-point meters, freeze ladder, scale divisor. | RFC-002, RFC-009 |
| 12 | Destructible counterplay | `strike()` targets creatures/buildings. | Falls out of the CombatEntity chassis nearly free. | RFC-004 |
| 13 | Skill composition | `abilities.hpp` constexpr table, but behavior is hand-written branches. | Composition columns; six shipped abilities become rows. | RFC-001, RFC-008 |
| 14, 15 | Asset reuse, tint/filters | Renderer already layers (katana overlay, windup pulse, smoke 1.6×); raylib tint is one parameter. | Status→tint/overlay map. | RFC-006 |
| 16 | Battlefield states | Camera shake exists; zone particles render closed-form from world clock. | Chunk-level state channel (earthquake: shake amp, aim scatter). | RFC-010, RFC-006 |
| 17 | RL-friendly | **The seam is live**: `boss_policy(BossObs) → BossAction` pure function, built in F3 exactly so F4 can swap it. | BossObs v2 per RFC-007; versioned checkpoints. | RFC-007 |

## The detailed set (status as of 2026-07-23)

| RFC | Title | Status |
|---|---|---|
| 001 | Ability System | **Accepted** |
| 002 | Status & Effect Framework | Draft |
| 003 | Physics & Material Interaction | Draft |
| 004 | Terrain & Combat Entity | **Accepted** |
| 005 | Boss Ability Authoring | **Accepted** |
| 006 | Visual FX & Telegraph Standards | Draft |
| 007 | RL Observation & Action Space | **Accepted** |
| 008 | Data-driven Skill Definition (JSON) | Draft |
| 009 | Damage, Resistance & Effect Build-up | Draft |
| 010 | Battlefield Simulation | Draft |

## Build order (topological, from the RFCs' declared dependencies)

```
001 (root) ──▶ 004 ──▶ 002 ──▶ 009 ──▶ 003 ──▶ 010 ──▶ 005 ──▶ 007 ──▶ F4 training
                                          006 (renderer-side, parallel from 002 on)
                                          008 (serialization, last — freezes formats)
```

- **001 + 004 are both Accepted and form the implementable core**: pipeline
  phases + the CombatEntity chassis. Destructible counterplay (§12) ships with
  004 almost free because existing `strike()` verbs just gain a target class.
- **007 (RL spaces) is Accepted and should freeze early** even though it
  implements late — it is the contract F4 trains against, and re-training after
  an observation change is the expensive mistake. Its `ent[].kind` /
  surface-tag vocabularies must be pinned when 004/003 enumerate them.
- **008 lands last on purpose**: serialization freezes what the other RFCs
  stabilize.

## What this absorbs from the old board

- **F4 (RL boss)** → RFC-007 + the training loop; unchanged goal, richer obs.
- Deferred `lake_islands` → unblocked by RFC-004's terrain-scar overlay writes.
- The six shipped abilities → migrate to RFC-001 pipeline / RFC-008 rows;
  behavior identical, representation changes.
