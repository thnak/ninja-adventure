# Ninja Adventure — Combat RFC Index

Specs for the Unified Combat System of **Ninja Adventure**, a cozy farm/adventure MMO on the
QuarkCpp engine (top-down 2D, 16×16 tile grid, 1024×1024 seamless overworld, server-authoritative
with the first node as trusted leader). Design context lives in the repo root:
[GAME.md](../GAME.md) (tone: *chill is the default, challenge is opt-in*),
[ARCHITECTURE.md](../ARCHITECTURE.md), [ROADMAP.md](../ROADMAP.md).

Ground rules every spec in this directory honors:

- Nothing counts down behind the player's back; combat difficulty waits to be found (GAME.md §0).
- Monster sprites are walk-only — all telegraphs and strikes are FX overlays, never bespoke frames.
- Player kit is basic attack + exactly **two** equipped abilities (the rigs have two ability poses).
- Four elements only in v1: Fire / Ice / Rock / Thunder.
- Everything must tolerate simulation LOD: chunks tick at 10 Hz / 1 Hz / asleep.
- One RL policy per archetype (10–15 total), DQN core reused from RLDrive.

## Documents

| Doc | Role |
|---|---|
| [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) | Umbrella overview — the 17 concepts the detailed set decomposes |
| [IMPLEMENTATION_MAP.md](IMPLEMENTATION_MAP.md) | Umbrella section → current engine status → owning RFC, plus the topological build order |

## The RFC set (finalized 2026-07-23)

All ten RFCs went through review and landed as **accepted-with-revisions**.

| RFC | Title | Status | Summary |
|---|---|---|---|
| [RFC-001](RFC-001-ability-system.md) | Ability System | accepted-with-revisions | The seven-phase pipeline (Cast → Channel → Release → Travel → Impact → Persist → Expire) as one state machine: check-and-debit admission, per-phase interrupts, charge channels, targeting models, cooldowns, and the two-slot player kit |
| [RFC-002](RFC-002-status-effect-framework.md) | Status & Effect Framework | accepted-with-revisions | Status ladders driven by build-up gauges (fill / decay / walk-down), the deterministic one-primary-slot rule, coatings (Wet), detonation combos, and a two-level soft-resist window instead of immunities |
| [RFC-003](RFC-003-physics-material-interaction.md) | Physics & Material Interaction | accepted-with-revisions | Integer impulse/mass/knockback on the tile grid, the material coefficient matrix, terrain physical properties, and the general interaction rule engine (no per-skill hooks, ever) |
| [RFC-004](RFC-004-terrain-combat-entity.md) | Terrain & Combat Entity | accepted-with-revisions | One chunk-owned `CombatEntity` chassis (HP, lifetime, collision, team, tags) for spikes, walls, smoke, and totems; destructible counterplay; and the terrain scar overlay layer (crater, rubble) with revert |
| [RFC-005](RFC-005-boss-ability-authoring.md) | Boss Ability Authoring | accepted-with-revisions | Boss kits as data over the measured pose-capability manifest of the ~11 usable boss sheets (Samurai first, Dragons excluded); telegraph tiers with a readability floor, generation-0 scripts, and the kit validator |
| [RFC-006](RFC-006-visual-fx-telegraph-standards.md) | Visual FX & Telegraph Standards | accepted-with-revisions | The telegraph grammar (shape vocabulary, element palette, danger tiers, minimum lead times), the hitbox-equals-decal truth rule, FX layering and tint recipes that make walk-only monsters read as attackers |
| [RFC-007](RFC-007-rl-observation-action-space.md) | RL Observation & Action Space | accepted-with-revisions | The frozen observation vector and 15-verb action space (the RLDrive `kActionCount` wall), one policy per archetype, reward shaping under the chill guardrail, and the checkpoint/generation lifecycle with quality gates |
| [RFC-008](RFC-008-data-driven-skill-definition.md) | Data-driven Skill Definition (JSON) | accepted-with-revisions | The disk contract: strict JSON, canonical form and dual hashes, `ids.lock.json`, schema versioning, and document schemas for fx / sounds / icons / status / entity / skill / boss kits |
| [RFC-009](RFC-009-damage-resistance-buildup.md) | Damage, Resistance & Effect Build-up | accepted-with-revisions | The damage formula's fixed order of operations, DR vs flat reduction with caps, the build-up gain math behind RFC-002's gauges, and scale tiers — big things resist, nothing is immune |
| [RFC-010](RFC-010-battlefield-simulation.md) | Battlefield Simulation | accepted-with-revisions | Chunk-owned battlefield layers: tile patches (fire, mud, ice) with a deterministic spread machine, field states (earthquake), tick order and authority, replication budgets, and LOD/sleep tolerance |

Build order (from the RFCs' declared dependencies — see IMPLEMENTATION_MAP.md):

```
001 ─▶ 004 ─▶ 002 ─▶ 009 ─▶ 003 ─▶ 010 ─▶ 005 ─▶ 007 ─▶ F4 training
                                     006 (renderer-side, parallel from 002 on)
                                     008 (serialization, last — freezes formats)
```

## The world & progression set (finalized 2026-07-23)

All three RFCs went through the same dual-model review as the combat set and landed as
**accepted-with-revisions**. Numbering continues from RFC-019 — RFC-011..018 remain reserved by the
proposed table below and are not renumbered or absorbed by this batch.

| RFC | Title | Status | Summary |
|---|---|---|---|
| [RFC-019](RFC-019-progression-and-skills.md) | Progression & Skill System | accepted-with-revisions | The player-growth contract: XP attribution across Melee/Ranged/Magic/Trades by cause-of-death, skill points as continuous passives plus discrete ability-unlock tiers feeding RFC-001's admission gate, the 34-point cap and a respec policy, and a multiplayer damage-contribution credit ledger — with an explicit argument that none of it creates a clock behind the player's back |
| [RFC-020](RFC-020-mission-quest-system.md) | Mission & Quest System | accepted-with-revisions | A quest as an invitation the world already extends, written down: authored templates and emergent opportunities (raid defense, fort assault, dungeon rumors) through one Offered → Accepted → Complete state machine with zero-cost Abandoned and event-only Expired (no wall-clock transitions), solo/party/community credit sharing, and reward hooks that reference RFC-019 XP, village standing, and RFC-018 item tables by id only |
| [RFC-021](RFC-021-world-map-wayfinding.md) | World Structure, Map & Wayfinding | accepted-with-revisions — **partially superseded by RFC-022** (see below) | Codifies the shipped 1024×1024 overworld as a normative contract: the as-built Chebyshev/area-corrected ring geometry (not GAME.md's illustrative circles), deterministic worldgen invariants, the Map screen (`M`) with a visit-only discovery model and a coarse rumor layer, and the wayfinding/travel layer (mounts, the required village waypoint network, roads) — with a clause-by-clause argument that no map or travel feature decays or hides a countdown |

## The map & character set (finalized 2026-07-24)

Both RFCs went through the same dual-model review as the earlier sets and landed as
**accepted-with-revisions**. Numbering continues from RFC-021 — RFC-011..018 remain reserved by the
proposed table below and are not renumbered or absorbed by this batch.

| RFC | Title | Status | Summary |
|---|---|---|---|
| [RFC-022](RFC-022-map-system.md) | Map System | accepted-with-revisions | One map *kind* — a chunk-owned, integer-tile grid — at whatever size the content calls for, with 1024×1024 as the ceiling, not the only size: a runtime `MapId` partition, a unified `Portal` concept generalizing the shipped `Door`/`Teleport` structs, the house/tent-structure-vs-portal boundary, a village-always-fits invariant proven from `plan_of()`, and a narrow `MapSession` join-vs-create contract that stays out of RFC-014's instance-lifecycle territory. Supersedes RFC-021 in part — see its own "Relationship to RFC-021" section |
| [RFC-023](RFC-023-character-npc-roster.md) | Character & NPC Roster System | accepted-with-revisions | Two halves of one root cause — only Green Ninja ships the full open-world locomotion set (Climb/Swim/Pickup/Push/Roll), so it is codified as the sole player-controllable character with cosmetic-only palette swaps as its one customization axis; and a fresh `Npc` extension of the `Creature` doctrine with a GAME.md-grounded role taxonomy (guard, merchant, quest-giver, farmer, child, wanderer), hand-authored state machines for the five civilian roles, an explicit hand-off to RFC-007 for guard AI, and village roster sizes derived from RFC-022's actual built structure count rather than GAME.md's illustrative population figures |

## The instance, vitals & persistence set (finalized 2026-07-24)

All four RFCs went through the same dual-model review as the earlier sets and landed as
**accepted-with-revisions**. RFC-014 was drafted and finalized first, as the foundation the other
three build on — RFC-013's instanced-band ejection needs an instance to eject *from*, RFC-015 adopts
RFC-014's sparse chunk-addressing scheme, and RFC-016 builds on RFC-014's save-before-teardown timing
and its "instanced maps do not survive a leader restart" policy.

| RFC | Title | Status | Summary |
|---|---|---|---|
| [RFC-013](RFC-013-vitals-death-recovery.md) | Vitals, Death & Recovery | accepted-with-revisions | The shipped player-vitals baseline (unconditional stamina/mana regen, out-of-combat-only health regen) confirmed normative; a ruling that no `Creature` regens HP by a tick-based rule, with `boss_reset()`'s leash-gated full-HP reset carved out and argued as the one named exception; and GAME.md §3's death split finally built — the unchanged, zero-loss hearth respawn on the persistent `MapId` band, and a new ejection contract on the instanced band (full carried-item wipe, return to the session's portal, XP/skill levels/unlocked abilities never touched) |
| [RFC-014](RFC-014-instance-realm-lifecycle.md) | Instance & Realm Lifecycle | accepted-with-revisions | `allocate_new()`: the `InstanceManager` spin-up/teardown machinery built on QuarkCpp's `declare_lazy`/`IdleTimeout` primitive, a sparse two-tier chunk-addressing scheme replacing `chunk_index()`'s dense formula, group join/deliberate-exit/disconnect/reconnect semantics, per-realm atlas refcounting, and an explicit ruling that the shipped 10×7 dojo boss room stays a room on the persistent `kInterior` map and does not migrate to a `MapSession` |
| [RFC-015](RFC-015-client-replication-protocol.md) | Client Replication & Interest-Set Protocol | accepted-with-revisions | The project's first client-facing wire protocol: the interest set reuses the shipped `fan_beacons()` 5×5-chunk window rather than a second radius, packed wire projections for `Creature`/`Projectile`/`Effect`/`Player` with id-keyed delta encoding, inner/outer-band send cadence, a per-view byte budget that answers RFC-010's Open Question 5, a decoupled Map-marker channel satisfying RFC-021 §5.4, and a latency budget checked against RFC-006's telegraph lead times |
| [RFC-016](RFC-016-persistence-save-format.md) | Persistence & Save-File Format | accepted-with-revisions | Commits RFC-008's placeholder "leader SQLite" to a real backend (QuarkCpp's `SqliteStore`): `Persistent<Snapshot, Batched>` for player progression/loadout/quest state, `Persistent<EventSourced, Sync>` for world-overlay events (building/crop/till-ground), an explicit ≤60-second recovery-drift arithmetic against ROADMAP.md P5's bar, RL-checkpoint storage and retention closing RFC-007's named gap, and the portable per-world save-directory contract ARCHITECTURE.md §2 asks for |

## Review process

Each RFC was finalized through a **dual-model adversarial debate**:

1. Two independent reviewers — Claude **Opus** and Claude **Sonnet** — reviewed each RFC against
   GAME.md / ARCHITECTURE.md / ROADMAP.md, the verified 2026-07-23 asset audit, and the shipped
   engine code, filing must-fix findings with proofs.
2. Findings were verified (proofs checked against the actual docs and code, not accepted on
   assertion), contested points debated, and the surviving revisions applied to the spec.
3. Each reviewer then cast a vote. Both reviewers voted **revise** on all ten RFCs; the required
   revisions were applied, yielding a final status of **accepted-with-revisions** across the set.

Every RFC records its outcome in a `## Review Record` section at the end of the file, listing the
votes, the applied revisions, and any cross-RFC issues left for another spec's editor.

Because the ten finalizations ran concurrently, a handful of RFC pairs adopted each other's draft
positions and ended up contradicting each other's final ones; a follow-up **series-editor
reconciliation pass** arbitrated those cross-RFC conflicts (status-math canon, ownership wording,
dangling references) and recorded every ruling, with rationale and the files it touched, in
[RECONCILIATION.md](RECONCILIATION.md).

## Proposed future RFCs

Gaps the finalized set references but never defines, plus systems the design docs require that no
spec covers. Numbering continues from RFC-011. Each is something the finalized RFCs or the roadmap
explicitly depends on — not speculative process.

| Proposed | Title | Why it is needed |
|---|---|---|
| RFC-011 | Combat HUD, Input & Cooldown UI | RFC-006 declares UI/HUD out of scope, yet RFC-002 Q4 punts enemy-gauge visibility to "RFC-006 decides" — the decision has no owner. The two-slot kit, Disabled-icon cooldown convention, boss bars, and the key-rebinding debt from P0 need one spec |
| RFC-012 | Combat Audio & Sound Cue Standards | RFC-006 Q6 explicitly proposes this ("RFC-006b"): telegraph commit/imminent audio cues are the natural pairing of the telegraph grammar but are unscoped, and the audit flagged audio wiring debt (melee plays `harvest.wav`). RFC-008 §7.2 already defines the sound-map format with no cue standards behind it |
| RFC-017 | Balance Tuning & Test Harness | RFC-003 Q5 proposes a scripted payload-vs-material sweep harness and says "needs an owner; not blocking the spec." The RFC-002 Q6 / RFC-009 Q2 calibration conflict — two specs briefly disagreeing on freeze pacing before the series editor's reconciliation pass fixed `kIceBoltPower` at one value (RECONCILIATION.md ruling 4) — is the worked example for why a documented resolution procedure is needed *before* the next one. Pins the `mmo_sim` sweep format, the determinism-diff pattern, and RFC-007's Gate A/B measurement protocol |
| RFC-018 | Loot, Essence & Reward Tables | RFC-005's non-goals punt "loot tables, Essence rewards, dungeon economy" to P4/P8, and monsters currently drop nothing by explicit deferral ("đặt một bảng tạm bây giờ là đặt nó hai lần"). Combat needs a data-driven drop contract — deterministic rolls, ring/tier scaling, Essence and socket-gem sources — before dungeons pay out |

Not proposed on purpose: modding/untrusted-pack sandboxing (explicitly excluded by RFC-008's
non-goals — the pack is trusted repo content), PvP specs (off by default per GAME.md §11), and a
localization RFC (RFC-008 Q2 tracks the one open decision; a whole spec would be bureaucracy at
this team size).
