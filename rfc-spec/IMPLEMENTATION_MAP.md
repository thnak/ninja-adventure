# Implementation Map — RFC: Unified Combat & Physics System → current engine

> Companion to `RFC_Unified_Combat_System.md`. For each numbered section of the
> umbrella RFC: what the engine already has, what is missing, and where it would
> live. Ends with the proposed RFC-001…010 split and a build order.
>
> Ground rule carried over from everything built so far: the simulation stays
> **integer-only and deterministic** — every new mechanic must be a pure function
> of (seed, state, tick), because cross-compiler byte-equality (GCC vs MSVC) and
> the lock-free snapshot seam both depend on it.

## Section-by-section status

| RFC § | Concept | Today | Gap |
|---|---|---|---|
| 1 | Ability pipeline (Cast→…→Expire) | Cast/wind-up = F2 telegraphs (`commit_windup`/`resolve_windup`, stun cancels). Impact = `AbilityStrike`. Persist = `Zone` (kWet, kSmokeSuppress) with `effect_life_of` lifetimes. | **Channel** stage, **Travel as an interceptable entity** (projectiles exist but can't be hit), explicit Expire hooks. |
| 2 | Telegraph-first | Already doctrine: windup 4/6/8 ticks, red pulse, whiff at stored aim, dodge-able. Boss telegraphs 10/14. | Nothing structural — new skills just follow it. |
| 3 | CombatEntity | Fragments exist: creatures have hp/team-by-kind; `Building::hp` (destroyable); zones have lifetime+aura but **no hp, no collision, no team**. | The unification. One chunk-owned `CombatEntity{hp,lifetime,collision,aura,team,tags,material}`; spike/ice-wall/totem become rows, not systems. |
| 4 | Battlefield control | Zones already re-shape fights (kWet feeds `chain_shock`, kSmokeSuppress strips aggro). | **Dynamic blockers**: blocking is currently static (`prefab_blocks` + terrain). CombatEntities need a "solid" tag that movement/pathing consults per tick. |
| 5 | Terrain evolution | The mechanism exists and is proven: `publish_overlay()` — same seam the deferred `lake_islands` item needs. | Combat→overlay writes (crater, rubble) with revert timers. Renderer already redraws from the view, no extra work. |
| 6+8 | Attack physics + materials | Damage only. `CreatureStats` is the natural carrier. | Add `impulse, heat, cold, shock, pierce, crush` to attacks and `Material` (flesh/stone/spirit/…) to entities. Pure data columns. |
| 7 | Interaction rules | **One rule already emergent**: Water + Lightning (kWet zone + Conduct skill → `chain_shock`). It was hand-wired, which is exactly what the RFC forbids at scale. | Generalize into a rule table: `(effect_tag, surface/material_tag) → outcome`. The kWet case becomes row #1. |
| 9 | Effect accumulation | Binary stun exists (cancels wind-ups). | Per-entity build-up meters with decay (`Cold→Slow→Freeze` ladder). Fixed-point integers, sized by Scale tier. |
| 10 | Scale tiers | Boss is just a big creature (kBossScale ~2.2 render-side). | `Scale` enum on stats; divides build-up and knockback. Cheap once §9/§11 exist. |
| 11 | Mass & knockback | No knockback at all. | `knockback = impulse / mass` in integer math, applied as forced displacement respecting collision. Boss mass makes it shrug shoves *without* immunity — matches §9's "no absolute immunities". |
| 12 | Destructible counterplay | `strike()` targets creatures/buildings. | Falls out of §3 free: once a spike/meteor is a CombatEntity with hp, existing verbs hit it. |
| 13 | Skill composition | `abilities.hpp` is a constexpr table but each ability's behavior is a hand-written handler branch. | Recast as composition columns: `Visual + Spawn + Motion + Impact + AfterEffect + Status`. The six shipped abilities become rows; Meteor = `rock+sky+fall+explosion` is a data row too. |
| 14+15 | Asset reuse, tint/filters | Renderer already layers (katana overlay, windup red pulse, smoke drawn 1.6×). raylib tints are one parameter. | A small filter map: status→tint/overlay (freeze=blue+frost, burn=emissive, shock=arcs). Renderer-only, zero sim cost. |
| 16 | Battlefield states | Camera shake exists (hit feedback). Zone particles already render closed-form from the world clock. | Chunk-level state channel in `ChunkView` (earthquake: shake amp, aim scatter seedable + deterministic). |
| 17 | RL-friendly observation | **The seam is live**: `boss_policy(BossObs) → BossAction` is a pure function, built in F3 exactly so F4 can swap it. | `BossObs` v2: ability/channel stage, nearby hazards (CombatEntities), surface tags, own statuses/cooldowns. "RL learns patterns, not bosses" = obs schema shared across all bosses. |

## Proposed RFC series

- **RFC-001 — CombatEntity & Materials** (§3, §8): the data model everything else rides on. Chunk-owned, snapshot-published like creatures/zones.
- **RFC-002 — Status build-up, decay & Scale** (§9, §10): fixed-point meters, freeze ladder, scale divisor.
- **RFC-003 — Impulse, mass & knockback** (§6, §11): attack physics columns, forced displacement.
- **RFC-004 — Interaction rule table** (§7): generalize kWet+Conduct into data; terrain surface tags (mud/ice/water/rubble → friction/grip/conductivity).
- **RFC-005 — Ability pipeline completion** (§1, §12): Channel stage, Travel-as-CombatEntity (shootable meteors), Expire hooks.
- **RFC-006 — Terrain evolution** (§5): combat overlay writes (crater/rubble) + revert timers. *Side effect: unblocks the deferred `lake_islands` worldgen item.*
- **RFC-007 — Visual filter compositor** (§14, §15): status→tint/overlay map, purely renderer.
- **RFC-008 — Battlefield states** (§16): earthquake channel in ChunkView.
- **RFC-009 — Skill composition format** (§13): abilities as composition rows; existing six migrated.
- **RFC-010 — RL observation schema** (§17): BossObs v2. **This is the F4 contract** — freeze it early so training can start while 005–009 land.

## Build order

```
001 ──▶ 002 ──▶ 003 ──▶ 004 ──▶ 005 ──▶ 009
  │                                       
  ├──▶ 012-counterplay (free with 001)    006, 007, 008: parallel any time
  └──▶ 010 (spec can freeze after 001+004 shapes are known) ──▶ F4 training
```

001 is the keystone: entities-with-hp gives destructible counterplay for free,
gives Travel something to be, and gives the rule table things to match on.
010 should be *specced* early even though it's listed last — it is the contract
F4 trains against, and re-training after an obs change is the expensive mistake.

## What this absorbs from the old board

- **F4 (RL boss)** → becomes RFC-010 + the training loop; unchanged goal,
  richer observation.
- Deferred `lake_islands` → unblocked by RFC-006's overlay-write machinery.
- The six shipped abilities → migrate to RFC-009 rows; behavior identical,
  representation changes.
