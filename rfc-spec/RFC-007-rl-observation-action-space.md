# RFC-007: RL Observation & Action Space

> Status: **Draft**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §17
> Depends on: RFC-001 (ability pipeline), RFC-002 (statuses), RFC-004 (combat entities), RFC-005
> (boss ability authoring), RFC-009 (damage fractions), RFC-010 (battlefield LOD)
> Grounded in: `src/world/boss.hpp` (the existing obs→action seam), RLDrive `core/`
> (`/home/nvthanh/works/windows-machine-self-learn`), ARCHITECTURE.md §7, GAME.md §0/§10

---

## Summary

This RFC fixes the **tensor interface** between the combat simulation and every learned policy in
the game: one observation vector layout (`kObsSize = 112` floats, version-stamped), one discrete
action space (**exactly 15 actions**, matching the `kActionCount = 15` hardcode in RLDrive's
`DqnAgent`), shared by **every** RL archetype — dojo bosses, dungeon monsters, and village guards
alike. One policy per archetype (10–15 policies total), never per individual.

The observation is **egocentric, identity-free, and stateless**: no boss ID, no room ID, no
absolute coordinates, no frame history. Entities appear as *behavior classes and elements* (RFC-004
tags), hazards as *ray closeness*, abilities as *slot cooldowns and pipeline phases* (RFC-001).
This is the concrete mechanism behind the umbrella's promise that *"RL learns patterns, not
individual bosses"*: a policy trained against ice walls and burn zones has literally no channel
through which to memorize a specific boss or room.

Rewards are shaped for **being a good fight, not a relentless one**, under the GAME.md §0 chill
guardrail: every reward term is scoped to an engaged opponent inside the training room; nothing
rewards pursuit, ambush, or off-screen aggression. Training runs only in 10×7 dojo/interior rooms
on the leader (`Priority<2>`), publishes immutable JSON checkpoints with generation metadata, and
gates each generation increment behind a frozen-opponent evaluation **and a difficulty ceiling**.

## Motivation

Three forces make this RFC necessary now, and necessary as *one* document:

1. **The seam already exists and is about to be replaced.** `src/world/boss.hpp` defines
   `boss_policy(const BossObs&) -> BossAction` — a pure function, explicitly written so that "F4's
   learned policy replaces the BODY of `boss_policy` and nothing else." But the current `BossObs`
   (6 fields) and `BossActionKind` (5 actions) were sized for the generation-0 hand script of a
   single melee boss. The unified combat system (RFC-001..010) adds channel stages, statuses,
   combat entities, hazards, and multi-slot boss kits (RFC-005). If each archetype grows its own
   ad-hoc obs, we get N incompatible tensor interfaces, no transfer between archetypes, and a
   training harness per boss. The interface must be fixed once, before the first network is trained
   against it.

2. **The DQN core has a loaded gun in it.** RLDrive's `DqnAgent.cpp:25` hardcodes
   `kActionCount = 15` and its epsilon-greedy sampler and `TrainBatch` write without bounds checks;
   an action space smaller than 15 segfaults immediately (reproduced under ASan — ARCHITECTURE.md
   §7). The action space design must confront this number explicitly, not discover it in a
   debugger.

3. **The chill guardrail is easiest to violate through a reward function.** A naive
   "maximize damage" objective, trained with self-play, will happily learn behaviors that read as
   harassment — body-blocking doors, chasing disengaging players, spawn-camping. GAME.md §0 says
   difficulty *waits to be found; it never chases the player*. That constraint has to be encoded in
   the reward structure and the episode design, in writing, before any training run — because a
   trained network cannot be code-reviewed afterward.

## Guide-level Explanation

### What a player experiences

Nothing in this RFC is a menu or a stat screen. The player-visible surface is:

- **Dojos are real places.** In a tier-≥3 village you can walk into the dojo and watch guards spar;
  in a dungeon you can find the training hall where monsters drill. What you are watching *is* the
  training loop of this RFC, ticking at low priority on the leader. The room shows
  "Generation 47" — a number this RFC defines precisely (a checkpoint that beat its predecessor in
  a frozen evaluation).
- **A generation-30 Samurai fights differently from generation 5** — it leads its charge to where
  you are going, it holds its swing until your dodge is spent — but *every* blow still comes with
  the same wind-up telegraph (RFC-006), because the action space gives the policy no verb that
  skips the pipeline. The network can choose *when* and *where*; it physically cannot choose to
  *not telegraph*.
- **Difficulty never chases you.** Bosses train in their rooms; monsters train in their halls. A
  trained checkpoint changes behavior only for creatures **spawned after** it is published. Nothing
  you already pulled changes mid-fight, nothing follows you home, and a player who never opens a
  gate never meets any of it.
- **Raiding a dungeon sets its training back** a few generations — a mechanical, visible
  consequence of an entirely optional act.

### What a designer experiences

A designer authoring a new RL boss (RFC-005) does **not** design an observation or a network. They:

1. Pick an **archetype** (or add one, max 15 — see the archetype table). The archetype fixes the
   policy; the boss contributes its stat block and its ability-slot bindings.
2. Bind up to **4 ability slots** from the RFC-005/RFC-008 catalog. Slot indices 0–3 are what the
   action space can cast; a boss with 2 abilities simply has two dead slots (safely coerced to
   Hold).
3. Ship. The archetype's existing checkpoint drives the boss on day one; the training loop
   continues improving it against the persona suite and self-play.

A designer never sees a float vector. But the *shape* of the obs is a design contract they can rely
on: "the policy can see status build-up, hazard rays, cooldowns, and channel phases — and nothing
else." If a boss mechanic is invisible in the obs, the policy is blind to it, and the designer
knows that from this document rather than from a confused training run.

## Reference-level Design

All tick counts are at the simulation rate `kTicksPerSecond = 10` (tiles.hpp). "(tunable)" marks
every proposed number that is a balance/training knob rather than a structural requirement.

### 1. Decision cadence

Policies do not act every simulation tick.

| Constant | Value | Meaning |
|---|---|---|
| `kDecisionPeriod` | **3 ticks** (tunable) | one policy decision per 300 ms |
| `kEpisodeTicks` | **600 ticks** (tunable) | 60 s per training episode = 200 decisions |

Rationale: the shortest committed wind-up in the game is ~10 ticks (`kBossAttackWindup`), so a
300 ms decision grain loses nothing tactically, cuts the DQN credit-assignment horizon by 3×, and
triples effective training throughput on the measured ~790 steps/s CPU budget (ARCHITECTURE.md §7).
Actions are **durative**: an action chosen at decision tick `t` is executed by the chunk for the
next `kDecisionPeriod` ticks (movement) or until its pipeline completes (casts).

At reduced simulation LOD (RFC-010: 1 Hz background chunks), live inference degrades by the same
rule — one decision per `kDecisionPeriod` *elapsed* ticks — and a slept chunk runs no inference at
all. This works only because the observation is **stateless** (§2.8): it is rebuilt entirely from
current chunk state, so a boss waking from sleep needs no warm-up history.

### 2. The observation vector

`kObsSize = 112` floats. `kObsVersion = 1`. Layout is fixed at these indices; two independent
implementations must produce bit-identical vectors from the same chunk state (§2.7).

Encoding conventions, used everywhere below:

- **Fractions** are in `[0, 1]`; **signed offsets** in `[-1, 1]`.
- **Closeness encoding** for distances: `closeness = max(0, (D - d) / D)` with horizon `D` — so
  *absent/far = 0*, which keeps "all zeros" meaning "nothing there" (important for padded slots).
- **One-hots** are all-zero when the category is "none".
- Positions are **egocentric tile offsets** (`target − self`), clamped to ±`kObsRange = 8` tiles
  (tunable) and divided by 8. Never absolute, never room coordinates.

#### Block S — Self (indices 0–19)

| idx | feature | source & encoding |
|---|---|---|
| 0 | own HP | fixed-point `hp_frac` 0..1000 → `/1000` |
| 1–5 | own status one-hot | RFC-002 status: Frozen, Burning, Wet, Muddy, Shocked |
| 6 | status ticks left | `/80` (the longest base status duration; clamp 1) |
| 7–10 | own pipeline phase one-hot | RFC-001: Idle, Windup(Cast/Channel), Active(Release/Travel), Recover |
| 11 | phase progress | `elapsed_ticks / total_ticks` of the current phase; 0 when Idle |
| 12–15 | ability cooldown fractions, slots 0–3 | `cd_ticks_left / cd_total`; 0 when ready **or slot absent** |
| 16–19 | facing one-hot | Down, Up, Left, Right |

Deliberately **excluded from S**: archetype constants (scale tier, mass, reach, element affinity).
They are constant for a given policy, therefore carry zero information for that policy and would
only waste network capacity. Per-archetype numbers live in the stat block (RFC-005/RFC-008), not in
the obs.

#### Block T — Primary target (indices 20–39)

The primary target is the nearest engaged player (or, in self-play/sparring, the opposing agent).

| idx | feature | source & encoding |
|---|---|---|
| 20 | presence·freshness | `max(0, 1 − beacon_age/kBeaconLease)` — presence and staleness are one feature; an expired beacon reads as absent (0). Lease = 12 ticks (existing) |
| 21–22 | dx, dy | tile offset, clamp ±8, `/8` |
| 23–24 | vx, vy | `(pos_now − pos_prev_decision) / kDecisionPeriod` in tiles/tick, clamp ±0.8, `/0.8` |
| 25 | target HP | fraction |
| 26–30 | target status one-hot | as Block S |
| 31 | target status ticks left | `/80` |
| 32–35 | target pipeline phase one-hot | the player's ability pipeline is observable too (RFC-001 — players telegraph); Idle, Windup, Active, Recover |
| 36 | target phase progress | as idx 11 |
| 37–39 | target last-verb one-hot | Melee, Ranged, Magic — the school of the target's most recent attack this episode |

Velocity is computed from positions at the last two *decision* ticks (not sim ticks), from the same
quantized beacon data both sides already replicate; no extra state is stored beyond one previous
position per tracked target.

#### Block T2 — Secondary target (indices 40–45)

Dungeons are group content (2–4 players). One compact slot for the second-nearest player:

| idx | feature |
|---|---|
| 40 | presence·freshness (as idx 20) |
| 41–42 | dx, dy (clamp ±8, `/8`) |
| 43 | HP fraction |
| 44 | winding-up flag (1 if in Windup/Active phase) |
| 45 | reserved, always 0 in v1 |

Players 3 and 4 are *not* individually observed in v1 (see Open Questions Q3); they still influence
the fight through damage and through whichever of them becomes nearest.

#### Block R — Terrain & hazard rays (indices 46–61)

8 rays from the agent's tile in compass order N, NE, E, SE, S, SW, W, NW; horizon `D = 8` tiles
(tunable). Two channels per ray:

| channel | meaning |
|---|---|
| blocked closeness | closeness to the first **impassable** tile (room wall, RFC-004 Wall-class entity, terrain block) |
| hazard closeness | closeness to the first tile inside a **hazard** (RFC-004 aura/zone with a damaging or hard-CC effect, e.g. burn ground, spike row) |

Indices: ray `i` (0..7) → `46 + 2i` blocked, `46 + 2i + 1` hazard. Rays walk tiles (Bresenham on
the tile grid), reading the same `terrain_of` + chunk entity state the movement code reads.

#### Block G — Ground & confinement (indices 62–67)

| idx | feature |
|---|---|
| 62–65 | ground class under self, one-hot: Normal, Slow (mud/rubble), Conductive (water/wet), Unstable (cracked) — the RFC-003 material projection into four gameplay classes |
| 66 | standing-in-hazard flag (inside any RFC-004 hazard aura) |
| 67 | wall closeness: `max(0, (4 − d)/4)` where `d` = Chebyshev distance to nearest impassable tile; 0 in open field |

Note the projection: the policy sees ground **classes**, not terrain identities. Water and rain-wet
grass are both "Conductive"; the learned Thunder-avoidance transfers between them. RFC-003 owns the
authoritative material→class mapping table.

#### Block E — Combat-entity slots (indices 68–103)

The 3 nearest relevant `CombatEntity` instances (RFC-004) within `kObsRange`, ordered by distance,
12 floats each. Slot `k` (0..2) starts at `68 + 12k`:

| offset | feature |
|---|---|
| +0 | present (1/0) |
| +1, +2 | dx, dy (clamp ±8, `/8`) |
| +3–+6 | class one-hot: **Barrier** (blocks movement, destroyable: ice wall, boulder), **HazardZone** (aura: burn ground, smoke, spike field), **Projectile** (in flight, interceptable), **Caster** (totem/summon that will act) |
| +7–+10 | element one-hot: Fire, Ice, Rock, Thunder (all-zero = elementless) |
| +11 | lifetime fraction: `ticks_left / 100`, clamp 1; 1.0 for entities without lifetime |

"Relevant" = hostile-or-neutral team relative to the agent, tagged observable by RFC-004 (decor
entities are excluded there, not here). Entity **class and element are the entire identity** the
policy receives — this is the pattern-learning contract (§ RL Considerations).

#### Reserved (indices 104–111)

Always 0 in v1. Reserved so that v2 features (RFC-009 build-up meters are the known candidate) can
be added **without changing `kObsSize`**, letting v1 checkpoints keep loading while retraining
catches up (see §6 versioning).

#### 2.7 Determinism & quantization rule

Every feature is computed **integer-first**: tile offsets as `int16`, HP as fixed-point 0..1000,
ticks as counts — then converted by the fixed divisors above. No feature may be derived from a
float intermediate that differs across machines. This extends the existing `BossObs` doctrine
("INTEGER-ish obs on purpose", boss.hpp) to the full vector: an obs built on the leader and one
built on a chunk host must match bit-for-bit, which is what makes inference placement a deployment
choice rather than a correctness question.

#### 2.8 Statelessness rule

The obs is a pure function of *current* chunk state plus exactly one remembered position pair per
tracked target (for velocity). **No frame stacking, no recurrent state.** This is not a modeling
preference; it is forced by RFC-010: chunks drop to 1 Hz or sleep, and any policy that needs an
observation history would need that history preserved and replayed across LOD transitions and
chunk re-placement after node death. A stateless obs makes LOD transitions free.

### 3. The action space

`kActionCount = 15`, **exactly** — deliberately equal to the hardcoded sampler bound in RLDrive's
`DqnAgent.cpp:25`, so the vendored core is usable **unmodified** and the documented
heap-buffer-overflow (action spaces < 15) is structurally impossible. If the upstream
`action_count` parameter lands later, this stays 15 anyway (see Open Questions Q1).

| id | action | semantics (executed by the chunk over the next decision period) |
|---|---|---|
| 0 | Hold | stand; recover; telegraph nothing |
| 1–4 | Step N / E / S / W | walk one direction at the archetype's walk speed, collision-clamped |
| 5 | Approach | steer toward primary target (local steering, room-clamped) — the gen-0 `kApproach` |
| 6 | Retreat | steer directly away from primary target |
| 7–10 | Cast slot 0–3, **Direct** aim | start the RFC-001 pipeline for that slot, aimed at the target's current position; facing derived from `sign(dx)` (picks the L/R attack/charge pose where the sheet has one) |
| 11–14 | Cast slot 0–3, **Lead** aim | as above, aimed at `target_pos + v · (windup + travel)` ticks (RFC-001 phase durations), clamped to `kObsRange` |

**Total-function execution rules** — every index is executable in every state, for every archetype:

1. **The promise rule.** If the agent is in Windup/Active (a committed telegraph), every action is
   coerced to Hold. A telegraph is a promise (umbrella §1, boss.hpp); only external interruption
   (RFC-001/RFC-002 stun) cancels it — never the caster's own next decision.
2. **Dead or cooling slots.** Casting an absent slot, or one on cooldown, or one whose resource
   check fails (RFC-005) coerces to Hold. In training this is additionally penalized (§5) so the
   policy learns its own action mask; at inference it is merely harmless.
3. **Movement never leaves the arena.** Steps/Approach/Retreat are clamped by the same collision
   the player uses (RFC-004); there is no action that exits the boss's room. Leash behavior on a
   vanished target stays the engine's (`kBossLeashTicks`), not the policy's.
4. **Aim mode is resolved at commit.** Lead aim samples target velocity once, at the cast decision;
   the dash/projectile then follows the committed point (matching the existing charge semantics:
   "dash to where the target stood at commit").

Generation-0 compatibility: the existing hand script maps exactly — `kApproach`→5,
`kAttackLeft/Right`→7 (facing now derived, not chosen), `kCharge`→11 (charge bound to slot 1),
`kHold`→0. The script remains the fallback brain and the behavior-cloning teacher (§6).

### 4. Archetypes — one policy each

An **archetype** = (policy network, obs v-version, slot-binding template, stat-block family). All
archetypes share §2's obs layout and §3's action table. Proposed roster — **13 of the 15-policy
budget** (tunable in membership, capped at 15 by ARCHITECTURE.md §7):

| # | policy id | members (from the audited RL shortlist) | slots bound (content per RFC-005) |
|---|---|---|---|
| 1 | `boss.melee_bruiser` | **GiantRedSamurai** (first RL boss), GiantBlueSamurai | 0 cleave, 1 charge-dash |
| 2 | `boss.artillery` | Squids (Shoot pose only — never melee-slotted) | 0 ink shot, 1 volley |
| 3 | `boss.zone_controller` | GiantBamboo | 0 strike, 1 spike-row zone |
| 4 | `boss.ambusher` | GiantFrog | 0 tongue strike, 1 leap |
| 5 | `boss.elite_bruiser` | GiantRacoon + Gold elite (same brain, elite stat block) | 0 slam, 1 rush |
| 6 | `boss.two_phase` | Tengu (Trans sheet) | phase-1 pair / phase-2 pair (see Q4) |
| 7 | `boss.baseline` | DemonCyclop | 0 swing, 1 stomp |
| 8 | `mob.melee_swarm` | walk-only melee monsters (dungeon trash) | 0 bite (FX-overlay strike) |
| 9 | `mob.skirmisher` | walk-only ranged monsters | 0 spit (FX-overlay projectile) |
| 10 | `guard.spear` | village spear guards | 0 thrust, 1 brace |
| 11 | `guard.bow` | village archers | 0 shot, 1 volley |
| 12 | `guard.soldier` | line soldiers | 0 strike, 1 shove |
| 13 | `guard.captain` | captains | 0 strike, 1 rally |

Explicitly **excluded as RL agents** (asset audit, 2026-07-23): Dragons (multi-part segment rigs),
GiantSlime / Flam / Spirit (idle+hit sheets only). They remain scripted or non-boss content.
Wild animals and non-combat villagers never get policies (GAME.md §5: hand-written behavior).

Individuals within an archetype differ **only** in stats and equipment (RFC-005/RFC-008 data),
never in weights. A new boss joining an existing archetype inherits its checkpoint on day one.

### 5. Reward shaping — under the chill guardrail

Structural rules first; they matter more than the coefficients:

- **R-scope.** Every reward term references only: the agent, entities it owns, and opponents
  **currently engaged inside the training arena**. There exists no term over players outside the
  room, structures, villages, or the overworld. A behavior cannot be learned toward a signal that
  does not exist — this is how "difficulty never chases the player" survives contact with an
  optimizer.
- **R-honesty.** Rewards flow only through the RFC-001 pipeline (§3 gives no off-pipeline verb),
  so no gradient ever favors an untelegraphed hit. Whiffs are **not** penalized beyond their
  opportunity cost: a visible miss is a designed outcome ("wind-up, strike, or a visible miss"),
  and punishing it teaches over-cautious, unreadable play.
- **R-symmetry.** Self-play and sparring use the same table with roles swapped; league/sparring
  opponents are frozen checkpoints (ARCHITECTURE.md §7), never co-learning.

Per-decision reward (clip final sum to [−1, +1] per step (tunable)):

| term | value | notes |
|---|---|---|
| damage dealt | `+1.0 ×` (damage / target max HP) (tunable) | damage fractions per RFC-009 |
| damage taken | `−1.0 ×` (damage / own max HP) (tunable) | |
| terminal win (opponent dead) | `+0.25` (tunable) | |
| terminal loss (agent dead) | `−0.25` (tunable) | |
| step cost | `−0.001` per decision (tunable) | a *training-room* device only; episodes exist only in dojos, so this countdown is invisible to players and violates no §0 rule |
| invalid cast (coerced, rule §3.2) | `−0.05` (tunable) | teaches the action mask |
| turtling | `−0.02` per consecutive Hold beyond 6 decisions, **only while** a target is present *and* ≥1 slot is off cooldown (tunable) | the guard condition protects legitimate post-strike recovery (`attack_cd` holds) from being punished |

Deliberate **non-rewards**: no bonus for kill speed beyond the step cost; no bonus for
status/combo application (statuses pay off through damage already — double-counting breeds
degenerate spam); no penalty for the opponent escaping (escape ends the episode neutrally —
rewarding pursuit is exactly the harassment gradient the guardrail forbids).

### 6. Training environment, checkpoints, generations

#### 6.1 Environment

`CombatEnvironment` (replacing RLDrive's `core/env/` car model; the seam is
`obs vector<float>` / `action int` / `reward float`, verified in ARCHITECTURE.md §7) wraps a
**headless arena simulation** of one 10×7-tile interior room (`kRoomW × kRoomH`, tiles.hpp),
running the identical chunk combat rules. It runs inside `TrainingActor`
(`Require<Trusted>`, `Priority<2>`, bounded drain budget) on the leader only — training is **not**
performed in live world chunks; the visible in-world dojo sparring *renders* the training
matches, it is not load-bearing for gradient quality. **We do not use `DqnTrainer`** (hardwired to
`CarEnvironment`); the ~20-line loop is rewritten around `DqnAgent` + `ReplayBuffer`, per
ARCHITECTURE §7.

Episode: reset room → spawn agent and opponent at opposed jittered positions (jitter ±2 tiles,
seeded) → run to death, arena exit, or `kEpisodeTicks` → terminal reward → next.

Opponent curriculum, per archetype, mixed per episode draw (weights tunable):

| opponent | draw | purpose |
|---|---|---|
| scripted **player personas**: Rusher (melee, closes), Kiter (ranged, max distance), Caster (statuses + combo attempts), Turtle (baits wind-ups, punishes recovery) | 50% | the floor: policies must beat readable human-like play |
| own frozen checkpoint (league, `generation − k`, k ∈ {1..5} seeded) | 30% | self-play without moving-target divergence |
| cross-faction frozen checkpoint (guard↔monster sparring) | 20% | the two-dojo exchange (GAME.md §10) |

Personas are pure functions over the same obs/action interface — they are, precisely, more
`boss_policy`-style scripts, and live beside it.

#### 6.2 Checkpoint format

Weights: RLDrive `NetworkCheckpoint` JSON, flat vector, layout
`[layer: weights out×in row-major][biases out]` in layer order; the ~10-line `flatten/unflatten`
is ours (the helper is Windows-gated) and already verified bit-exact round-trip. Network shape:
`112 → 64 → 64 → 15` (tunable) ≈ 12.2k parameters ≈ 160 KB JSON — committable and diffable.

Metadata: a **sidecar** `<checkpoint>.meta.json` (the upstream format is not ours to extend):

```json
{
  "policy_id": "boss.melee_bruiser",
  "obs_version": 1,
  "action_version": 1,
  "generation": 47,
  "parent_hash": "sha256:…",
  "weights_hash": "sha256:…",
  "eval": { "vs_incumbent_winrate": 0.58, "vs_persona_winrate": 0.63, "episodes": 200 }
}
```

Publication is the existing contract: **immutable checkpoint + hash**, announced as
`{policy_id, generation, hash}`; spawning creatures load by hash and run pure inference
(`PredictOnline` + argmax — no `DqnAgent`, no replay buffer allocation on the inference path).
Live creatures **never** hot-swap weights mid-fight; a new generation applies from the next spawn.

#### 6.3 Generation state machine

```
TRAINING ──(every kEvalInterval=10k decision steps, tunable)──► EVAL (ε=0, frozen incumbent)
   ▲                                                               │
   │   fail either gate                                            │ pass both gates
   └───────────────────────────────────────────────────────────────▼
                                                        PUBLISH generation+1
Gate A (progress):  win-rate vs incumbent  ≥ 0.55 over 200 episodes    (tunable)
Gate B (ceiling):   win-rate vs persona suite ≤ 0.80                    (tunable)
Cap:                generation ≤ kGenerationCap = 60                    (tunable)
```

Gate B **is** the mandatory difficulty ceiling (GAME.md §10 constraint 3), made mechanical: a
candidate that crushes the readable-play persona suite too hard is *not published*, regardless of
self-play strength; training continues but publication freezes at the ceiling. The generation cap
is the absolute backstop.

Generation 0 is **never random weights** (the "boss must not be dumb at spawn" rule): it is the
hand script behavior-cloned into the network offline (supervised on script decisions over persona
episodes until ≥ 98% action agreement (tunable)), committed to the repo. If a policy's training
regresses or misbehaves, rollback = republish an earlier hash; the scripted `boss_policy` remains a
permanent, indistinguishable fallback (the game does not bet on RL — GAME.md §10).

**Raid setback**: a successful dungeon raid rolls the dungeon-side policy back
`kRaidSetback = 3` generations (tunable), floor 0 — republish of the older hash, nothing retrained.

DQN hyperparameters (all tunable, listed for convergent implementation): replay 50k transitions,
batch 64, γ = 0.97 (≈ 9 s horizon at 3.3 decisions/s), lr 1e-3, target-net sync every 1k steps,
ε 1.0 → 0.05 over 20k steps (RLDrive's default schedule).

## Interactions with Other RFCs

- **RFC-001 (Ability System)**: the pipeline phases (Cast/Channel → Release/Travel → Recover) are
  the source of Block S idx 7–11 and Block T idx 32–36; the promise rule (§3.1) restates RFC-001's
  no-self-cancel rule at the action layer. Phase durations feed Lead aim (§3, id 11–14).
- **RFC-002 (Status & Effect)**: the 5-status one-hots and duration fractions read RFC-002's
  authoritative status set; if RFC-002 adds a status, that is an obs-version bump (§6.2), not a
  silent re-mapping.
- **RFC-003 (Physics & Material)**: Block G's four ground classes are a fixed projection of
  RFC-003 materials/terrain properties; RFC-003 owns the mapping table.
- **RFC-004 (Terrain & Combat Entity)**: Block E's class taxonomy (Barrier/HazardZone/Projectile/
  Caster) is derived from RFC-004 tags; RFC-004 defines which entities are observable. Ray hazard
  channels read RFC-004 auras.
- **RFC-005 (Boss Ability Authoring)**: binds concrete abilities to the 4 action-space slots per
  archetype; consequently **RFC-005 kits are capped at 4 slots** — that cap originates here and
  RFC-005 must honor it. Wind-up/recover durations authored there surface in the obs unchanged.
- **RFC-006 (Visual FX & Telegraphs)**: the telegraph a player sees and the phase feature a policy
  sees are projections of the *same* pipeline state — one source of truth, two renderings. This RFC
  adds no visual requirements.
- **RFC-008 (Data-driven Skills)**: archetype definitions (policy id, slot bindings, stat family)
  are RFC-008 data; a data change may rebind slots but must not renumber them under a live
  checkpoint (that is a new archetype).
- **RFC-009 (Damage/Resistance/Build-up)**: damage fractions in the reward table are RFC-009's
  outputs; build-up meters are the designated v2 obs candidate for the reserved block.
- **RFC-010 (Battlefield Simulation)**: LOD/sleep constraints motivate statelessness (§2.8) and
  decision-cadence degradation (§1); RFC-010 owns replication of the (small) per-boss state this
  RFC adds: current action id, decision-tick phase, one previous target position.

## RL Considerations

**How this design makes RL learn patterns, not per-boss scripts** — the umbrella §17 claim, made
mechanical:

1. **Identity-free inputs.** No boss id, room id, opponent id, or absolute position exists in the
   obs. A policy cannot overfit to "this room's corner" or "player X" because those are not
   representable.
2. **Class-and-element entity encoding.** An ice wall from a player, from another boss, or from a
   future skill are indistinguishable in Block E: Barrier + Ice. Whatever the policy learns about
   Barriers transfers to every Barrier ever authored, including ones that do not exist yet.
3. **One shared verb set.** All 13 policies act through the same 15 verbs; tactics are expressed in
   pipeline-and-position vocabulary, not per-boss special cases. This also allows warm-starting a
   new archetype from its nearest neighbor's checkpoint (a bruiser seeds a new bruiser).
4. **Curriculum over personas, not over encounters.** Policies train against play *styles*; they
   never see scripted per-boss encounter logic, because none exists to see (RFC-005 bosses are data
   over the same systems).

**Known RL risks and their mitigations here**: reward hacking is bounded by R-scope/R-honesty and
per-step clipping; turtling by the guarded hold penalty; divergence by frozen-opponent league play;
catastrophic regressions by the eval gate + immutable rollback; "boring convergence" (GAME.md §14
risk 3: learns to stand-and-swing and stops) is surfaced by Gate B's persona suite — Turtle in
particular punishes stand-and-swing — and the ultimate fallback is the generation-parameterized
script table, which this interface makes drop-in by construction.

**Determinism boundary** (ARCHITECTURE.md §2c): training is centralized (one node, one writer)
precisely because float training diverges across nodes; what crosses nodes is only the immutable
checkpoint + hash. The obs's integer-first rule keeps *inference* reproducible everywhere.

## Asset & Engine Constraints Honored

| constraint (2026-07-23 audit / engine survey) | how this RFC honors it |
|---|---|
| Chill guardrail (GAME.md §0) | R-scope reward rule; no pursuit/off-arena terms; episodes and step costs exist only inside dojo rooms; checkpoints apply at next spawn only; bosses have no action that leaves the room |
| 66 monsters are 64×64 walk-only, zero attack animations | actions 7–14 are *pipeline* verbs; their visible form is an FX overlay at the monster's position (RFC-006). Nothing in the action space presumes an attack frame; `mob.*` archetypes bind FX-overlay strikes only |
| Only ~11/20 boss sheets have Attack/Charge poses; shortlist fixed | archetype roster (§4) draws exclusively from the shortlist; Samurai is archetype #1 and the first RL boss; Dragons and GiantSlime/Flam/Spirit are explicitly excluded as agents |
| Player kit = basic attack + exactly TWO abilities | player-facing kit untouched (RFC-001); the *boss* 4-slot cap is a separate, boss-only budget and never implies a player hotbar |
| Exactly 4 elements (Fire/Ice/Rock/Thunder) | all element one-hots are width 4; other schools are out of scope and unrepresentable in obs v1 |
| `kActionCount = 15` hardcode + unchecked writes in DqnAgent/TrainBatch | action space is exactly 15 by design; the vendored core runs unmodified and the segfault class is unreachable |
| kEffectLife/FX gaps (long FX truncation, unpacked Magic/* family) | no dependency: this RFC consumes pipeline *state*, not FX; visual standards live in RFC-006 |
| DQN core from RLDrive, JSON checkpoints, ~790 steps/s CPU | obs is a flat float vector, action one int; MLP-sized (12.2k params); decision cadence ×3 throughput; `DqnTrainer` bypassed; flatten layout specified |
| One policy per archetype, 10–15 total | 13-policy roster, hard cap 15 |
| Dojos train in-world & visible; RL bosses in 10×7 interior rooms | training arena is exactly `kRoomW × kRoomH = 10×7`; `TrainingActor` on leader at `Priority<2>`; in-world sparring is the rendered face of the same loop |
| 1024² overworld with simulation LOD (1 Hz / sleep) | stateless obs (§2.8); cadence degrades with elapsed ticks; no inference in slept chunks; no history to migrate |
| Server-authoritative, first-node leader, cheap replication | training leader-only (`Require<Trusted>` — an untrusted trainer could learn deliberately weak monsters); distribution = immutable checkpoint + hash; per-boss replicated state is 3 small fields |

## Open Questions

1. **Patch upstream or stay at 15 forever?** Adding `action_count` to `DqnAgentParams` (and the
   mirror hardcode in `gpu/GpuDqnTrainer.cpp:77`) is a small upstream fix to a live project we
   deliberately do not fork. Until it lands, 15 is a hard wall; if it lands, do we ever *want* more
   than 15 verbs, or is 15 a healthy forcing function for readable bosses?
2. **Obs v2 migration policy.** When RFC-009 build-up meters claim reserved indices 104–111: do v1
   checkpoints keep running (they see zeros — safe but blind) while retraining proceeds per
   archetype, or do we gate the RFC-009 rollout on all 13 policies re-passing Gate A? Needs a
   decision before the reserved block is first used.
3. **Is one secondary-target slot enough for 4-player groups?** v1 bets that nearest-two coverage
   plus damage signals suffice. If group bosses train into degenerate "tunnel the nearest" play,
   the candidates are a third slot (reserved block) or aggregate features (party centroid, count).
4. **Tengu two-phase**: one policy with the HP feature implicitly signaling phase (current design),
   or two checkpoints swapped at the Trans threshold? One policy is simpler and the obs already
   carries HP; two policies cost a roster slot but train each phase cleanly. Measure gen quality
   first.
5. **Gate B measures win-rate, not fun.** A boss can stay under the 0.80 ceiling while being
   miserable to fight (e.g., pure evasion). Do we need auxiliary published-generation checks —
   minimum aggression rate, telegraph-per-minute floor — and if so, who owns those thresholds
   (this RFC or RFC-005 per-boss)?
6. **Guard formation play.** Guards train formations (GAME.md §6); the 15 verbs have no explicit
   formation/coordination action. v1 bets emergent coordination from shared-policy self-play in
   multi-agent episodes is enough for village-raid quality. If not, guard archetypes may need a
   variant action table — which breaks the "one action space" simplification and must be its own
   amendment.

## Non-goals

- **No per-individual networks** — ever. Individuals differ in stats/equipment only.
- **No learning outside the dojo.** Live bosses run pure inference; no online updates, no replay
  collection from player fights in v1 (player-fight telemetry as a curriculum source is future
  work and carries its own consent/skew questions).
- **No recurrent nets, frame stacking, or continuous actions.** Excluded by the LOD statelessness
  rule and the DQN core, respectively — not merely deferred.
- **No RL for wild animals, farmers, merchants, or any non-combat NPC** (GAME.md §5).
- **No policy-vs-policy matchmaking, ladders, or PvP applications.** PvP is off by default and out
  of scope.
- **Not specifying** ability content or slot semantics (RFC-005), telegraph visuals (RFC-006),
  damage math (RFC-009), entity taxonomy details (RFC-004), or LOD/replication machinery
  (RFC-010). Where those change, this RFC consumes their outputs via the versioned obs contract.
- **No GPU training path** in v1; the CPU budget suffices at this network size, and `gpu/` is
  Windows-gated upstream.
