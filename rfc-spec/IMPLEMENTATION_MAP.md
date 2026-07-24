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
| 17 | RL-friendly | **The seam is live**: `boss_policy(BossObs) → BossAction` pure function, built in F3 exactly so F4 can swap it. | RFC-007's 120-float observation vector (`kObsVersion`) + 15-action space (`kActionCount`) replacing today's `BossObs`; versioned checkpoints. | RFC-007 |
| 18 | HUD, cooldown UI & key rebinding | Vitals bars (`draw_hud`, `screens.cpp:318-332`), the two-slot ability display with its Disabled-icon "greyed twin" convention + cooldown-wipe rectangle (`draw_ability_slots`, `screens.cpp:206-238`), and the boss HP bar (`draw_boss_bar`, `raylib_bridge.cpp:678-704`, called unconditionally at `:1773`) are all real, shipped code with no RFC behind them. Key rebinding is stubbed: `screens.cpp:821`'s own comment says "still owed." | Normative data-and-meaning contract for all four already-shipped elements; RFC-002 Q4 (ordinary-creature status gauges) answered within RFC-015's actually-frozen 1-byte budget, not RFC-002's originally-designed 2-byte one; a manual two-slot ability loadout picker (RFC-019 §5.4 hands this off by name); a key-rebind table + `client.cfg` storage. | RFC-011 |
| 19 | Combat audio & telegraph sound standard | 10-entry `Sfx` enum shipped (`audio.hpp:17-29`); swing/cast/shoot already play distinct SFX (`client_main.cpp:559-561` — the RFC-006 Q6 "melee plays harvest.wav" bug is already fixed, not this RFC's to fix again). No telegraph commit/imminent/fizzle cues exist; every non-`kBlast` `EffectKind` collapses onto the single `kHit` impact cue; no voice-contention management for 20-50 concurrent players. | Tier A (engine fallback)/Tier B (`snd.*` authored override) cue split; 10 new `Sfx` entries; a per-`EffectKind` impact table; an alias-pool + priority + ducking voice-management pipeline; one new `imminent_sound` field on RFC-008 §7.6's `cast.telegraph` block. | RFC-012 |
| 20 | Balance tuning process & test harness | `mmo_sim` is a real, shipped binary (`CMakeLists.txt:46-50`, `add_executable`/`add_test mmo_sim_smoke ... 600`) but only a smoke test today — no sweep, determinism-dump, or gate-check mode exists. | `--sweep` (the payload-vs-material effective-channel table RFC-003 Q5 left unowned), `--determinism-dump` (formalizes the cross-platform tile-for-tile check `ARCHITECTURE.md §2c` already proves by hand), `--gate-check`/`--gate-report` (a mechanical reader of RFC-007's Gate A/B thresholds), and a named conflict-resolution procedure extending `RECONCILIATION.md`'s numbered-ruling ledger (generalizing Ruling 4's worked `kIceBoltPower` precedent). | RFC-017 |

## The detailed set (status as of 2026-07-23)

| RFC | Title | Status |
|---|---|---|
| 001 | Ability System | **Accepted (revised after review)** |
| 002 | Status & Effect Framework | **Accepted (revised after review)** |
| 003 | Physics & Material Interaction | **Accepted (revised after review)** |
| 004 | Terrain & Combat Entity | **Accepted (revised after review)** |
| 005 | Boss Ability Authoring | **Accepted (revised after review)** |
| 006 | Visual FX & Telegraph Standards | **Accepted (revised after review)** |
| 007 | RL Observation & Action Space | **Accepted (revised after review)** |
| 008 | Data-driven Skill Definition (JSON) | **Accepted (revised after review)** |
| 009 | Damage, Resistance & Effect Build-up | **Accepted (revised after review)** |
| 010 | Battlefield Simulation | **Accepted (revised after review)** |

All ten finalized through dual-model adversarial review 2026-07-23; see each RFC's own
`## Review Record` and [RECONCILIATION.md](RECONCILIATION.md) for the cross-RFC arbitration pass
that followed.

## Build order (topological, from the RFCs' declared dependencies)

```
001 (root) ──▶ 004 ──▶ 002 ──▶ 009 ──▶ 003 ──▶ 010 ──▶ 005 ──▶ 007 ──▶ F4 training
                                          006 (renderer-side, parallel from 002 on)
                                          008 (serialization, last — freezes formats)
                                          011 (HUD — after 001/002/005/006 stabilize, plus
                                               already-accepted RFC-015/016 outside this set)
                                          012 (audio — after 006, the timing authority it keys
                                               off; plus RFC-008's schema and RFC-015's wire shape)
                                          017 (harness — tooling, not a pipeline stage; its
                                               --sweep has nothing to sweep until 003/009 land,
                                               its --gate-check needs 007's checkpoint format)
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
- **011 is a normative-baseline-plus-three-new-features RFC, not a new pipeline
  stage** — three of its four sections (vitals bars, ability slots, boss HP bar)
  document already-shipped code; only the loadout picker and key rebinding are
  new engineering, and both are additive on top of 001/002/005/019 without
  touching pipeline order.
- **012 depends on 006 for timing (the ARM→CHARGE→IMMINENT→IMPACT state
  machine) and on 008 for the `snd.*` authoring surface it extends**, but adds
  no new simulation-side state — it is a client-only cue-selection layer, so it
  does not gate on 003/009/010 the way 006 itself does.
- **017 can start any time but its four CLI modes gate independently**: `--sweep`
  needs the RFC-003/RFC-009 material and damage-formula code it reads (not yet
  built, per the umbrella table above); `--determinism-dump` and
  `--gate-check`/`--gate-report` need only already-shipped `sim_main.cpp` output
  and RFC-007's already-accepted checkpoint format, respectively, so those two
  modes are implementable today independent of 003/009's landing.

## What this absorbs from the old board

- **F4 (RL boss)** → RFC-007 + the training loop; unchanged goal, richer obs.
- Deferred `lake_islands` → unblocked by RFC-004's terrain-scar overlay writes.
- The six shipped abilities → migrate to RFC-001 pipeline / RFC-008 rows;
  behavior identical, representation changes.

## Instance, vitals & persistence set (RFC-013/014/015/016)

> Companion to the second detailed batch (RFC-013, RFC-014, RFC-015, RFC-016), all four
> **Accepted (revised after review)** as of 2026-07-24. Unlike RFC-021/022, this set is
> mostly green-field: the gaps below are genuinely unbuilt, not already-shipped behavior
> being written down after the fact — cited as "proposed by [RFC]" throughout.

### Gap table

| Concept | Engine today | Gap | Owner |
|---|---|---|---|
| Player vitals/regen | Three `int16_t` pools `hp_`/`mana_`/`stamina_` (`player_actor.hpp:462-464`), capped at `kPlayerMaxHp=100`/`kPlayerMaxMana=60`/`kPlayerMaxStamina=100` (`tiles.hpp:1086-1088`); unconditional stamina+2/tick, mana+1/tick, HP+1/3000ms gated on `world_ms_ - last_hurt_ms_ > kCombatCooldownMs=5000ms` (`tiles.hpp:1092-1098`, `player_actor.hpp:88-100`). Creatures (`tiles.hpp:361-401`) carry `hp`/`max_hp` with no per-tick regen field anywhere in `chunk_actor.hpp`, save one named exception: `boss_reset()` (`chunk_actor.hpp:1242-1257`) fully heals a leashed dojo boss after `kBossLeashTicks=50` ticks with no target present. | No RFC had codified any of the above as a binding contract before now, and no food/consumable healing exists — `GrantVitals` (`player_actor.hpp:178-183`) is shipped plumbing with no caller for it yet. | RFC-013 |
| Death/respawn | `handle(HurtPlayer)` (`player_actor.hpp:166-176`) sets `dead_ticks_=kRespawnTicks=30` (`tiles.hpp:1114`) uniformly for every map; `respawn()` (`player_actor.hpp:440-446`) resets position to `respawn_tx_`/`respawn_ty_` and refills all three vitals pools, touching inventory not at all, regardless of `map`. | GAME.md:136's dungeon/mine death row ("bị đẩy ra ngoài, mất đồ mang theo") has no code behind it — the instanced-band ejection fork (`pending_eject_`, `SetInstanceReturn`, the merged `respawn()` listing that clears `items_[]`) is entirely new, not a rewrite of shipped logic. | RFC-013 |
| Instance allocation | `kMapCount=2` (`tiles.hpp:39`); no `InstanceManager` class anywhere in `src/`; `World::build()` still constructs every chunk actor up front (`ARCHITECTURE.md:300-301`'s own admission). RFC-022 fixes the `MapId` partition, `Portal`, `MapSession`, and a leader-only `resolve()` that stops deliberately before `allocate_new()`'s internals (RFC-022 §2.4). | `InstanceManager` (a new `Require<Trusted>` actor), the monotonic `MapId` counter, `declare_lazy<ChunkActor>` activation, `MapDirector` `FanOutAdd`/`FanOutRemove` messaging, and the two-timer teardown (`kInstanceIdleGraceMs`=5min / `kInstanceChunkIdleTimeoutMs`=30s) are all proposed, none shipped. | RFC-014 |
| Chunk-actor sparse addressing | `chunk_index()` (`tiles.hpp:726-728`) is a dense `c.map*kChunksPerMap+c.cy*kMapChunks+c.cx` formula sized for `kMapCount=2`. Three `kChunkCount`-sized consumers exist: `SnapshotBus::publish`/`view` (`snapshot.hpp:129,135`), `effect_tick` (`client_main.cpp:490`), and `World::build_bosses()`/`World::chunks_` (`world.hpp`) — the third caught only by this run's own audit, not RFC-022's original flag. | The two-tier scheme (`persistent_index()` bounded to a fixed `kPersistentBandCount=16`, plus a per-open-session `InstanceChunkBlock` sized to that map's own `chunk_edge`) is proposed by RFC-014 §4, not built. | RFC-014 |
| Client wire protocol | `protocol.hpp` defines only in-process actor messages (`Tick`, `CreatureEnter`, `MoveIntent`, `HurtPlayer`, `Teleport`, `PlanAttack`/`AttackPlan`, `UseAbility`/`AbilityPlan`, `GetPlayer`, etc.) — no client-facing wire/network layer exists in `src/` at all. `PlayerBeacon` (`protocol.hpp:109-116`, `kBeaconPeriod=3`, `kBeaconLease=12`, `protocol.hpp:118-119`), upserted by chunks at `chunk_actor.hpp:154-155`, is the only existing interest-set-shaped mechanism. | `PublishedCreature`/`PublishedProjectile`/`PublishedEffect`/`PublishedPlayer{Remote,Self}` packed wire structs, `ChunkDelta` id-keyed delta encoding, the `ClientInterestSet` (reusing `fan_beacons()`'s existing 5×5 window), and a per-view byte budget are all proposed by RFC-015, none shipped. | RFC-015 |
| Save/persistence | Zero matches for "sqlite" (case-insensitive) anywhere in `src/` or `tools/`; the only persisted state is `account.hpp`'s `AccountStore` (`accounts.dat`, fixed relative path, no world concept). RFC-007's checkpoint format (§6) has no filesystem path or retention policy. | A `SqliteStore`-backed `progression.db` schema, a `WorldPersistenceActor` (`Require<Trusted>`, event-sourced overlay log wrapping the five mutating messages), a `saves/<world_name>/` multi-world layout with explicit create/load/delete, and RL-checkpoint storage/retention are all proposed by RFC-016, none shipped. | RFC-016 |

### Build order

```
RFC-022 (accepted) ──▶ RFC-014 ──▶ ┬─ RFC-013
                                    ├─ RFC-015
                                    └─ RFC-016
```

- **RFC-022 is the fixed foundation, already accepted before this batch.** It commits the
  `MapId` partition, `MapDescriptor`, `Portal`/`PortalKind`/`PortalBinding`, `SessionScope`,
  and `MapSession` shapes, and its own §2.4 stops explicitly at `allocate_new()`'s internals
  — naming that gap as RFC-014's to fill, not something either RFC leaves ambiguous.
- **RFC-014 must land before the other three because each of them names a concrete
  RFC-014 output as a dependency, in its own text, not just as a topic overlap:**
  - RFC-013 §6.2 keys its persistent-vs-instanced death fork directly on
    `MapDescriptor.category`/`kPersistentBandCount` and targets ejection at
    `MapSession.return_*` — both RFC-014 outputs (RFC-013 §Interactions: "Supplies
    `MapDescriptor.category`/`kPersistentBandCount`... `MapSession.return_*`... the
    `present`/`members` bookkeeping this RFC's ejection plugs into").
  - RFC-015 §3.4 "adopts RFC-014 §4 verbatim" for every wire-side chunk-keyed structure it
    introduces, including the `effect_tick` migration RFC-014 named by number — it does not
    re-derive a competing sparse-addressing scheme.
  - RFC-016 §7 ("Resolving RFC-014's flagged breadcrumb requirement") and §6.2's citation of
    RFC-014 §3.6's `TEARING_DOWN` state both build directly against RFC-014 decisions RFC-014
    §6.1 itself flagged as unresolved dependencies for RFC-016 to close.
- **RFC-013, RFC-015, and RFC-016 run in parallel after RFC-014** because none of the three
  names a hard dependency on either of the other two in its own Interactions table — RFC-013
  calls RFC-015 "no new replication format is required," RFC-015 calls RFC-016 "orthogonal,"
  and RFC-016 calls RFC-013 "concurrent... states one explicit default ruling RFC-013 is free
  to override, not required to." They cite each other's field lists and rulings for
  consistency, but none blocks on the others being finished first.

### Adjacency to the still-proposed set (RFC-011/012/017/018)

Of the four still-proposed RFCs, only RFC-011 and RFC-018 are named anywhere in this batch's
Interactions sections — RFC-012 and RFC-017 are not cited by RFC-013/014/015/016 at all.
RFC-015 names itself as a hard upstream dependency of RFC-011 ("Depended on by: RFC-011
(proposed, Combat HUD) — needs a defined client-side data source," header; its Non-goals
reiterate that consuming `PublishedPlayerSelf`/`PublishedCreature` for the HUD is "RFC-011's
concern," not this RFC's), and RFC-016 §4.1 reserves (but leaves `NULL`, unpopulated) an
`equipped_ability_0`/`equipped_ability_1` column against a future RFC-011 manual-loadout
picker, purely so that landing RFC-011 later is "a code change to WRITE this column, never a
schema migration to ADD it." RFC-013 and RFC-014 both flag RFC-018 (proposed, loot/rewards) as
orthogonal rather than dependent — RFC-013 §Interactions: "RFC-018 governs what a creature
*drops*; this RFC governs what a player *loses*. Neither reads the other's tables," and
RFC-014's Non-goals excludes "loot/reward tables" from its own scope by name.

## Loot, economy & leader-recovery set (RFC-018, RFC-024)

> RFC-018 (Loot, Essence & Reward Tables) and RFC-024 (Leader Failure & Session Recovery), both
> **Accepted (revised after review)** as of 2026-07-24, are not combat-engine-gap-shaped the way
> RFC-001…010 or the HUD/audio/harness trio above are: RFC-018 is an economy/itemization system,
> RFC-024 is an ops/infrastructure concern. They get their own small table rather than being
> forced into the combat umbrella's numbering.

### Gap table

| Concept | Engine today | Gap | Owner |
|---|---|---|---|
| Loot/equipment/Essence economy | Bosses drop a flat 400 XP + 10 produce placeholder, explicitly commented as provisional ("P4 owns real loot tables," `chunk_actor.hpp:1332-1334`); ordinary monsters drop nothing, only wildlife grants 1 produce (`chunk_actor.hpp:1349-1352`); `items_[]` is a 4-kind stackable array only (`kItemKinds` = wood/stone/seed/produce, `tiles.hpp:357`) — RFC-013 §401-406 confirms "there is no separate equipped-gear concept yet." | New `ItemKind` ordinals (4 ore tiers, 24 grade-baked socket gems, Essence); the `EquipSlot`/`EquippedItem`/`SocketGem` data shape as genuinely new `PlayerActor` state; the material-tier table (`tier_damage_pm`/`tier_dr_bonus`/`tier_toughness_bonus`) RFC-021 §3.6 explicitly hands off; a new `loot.*` RFC-008 document domain; a deterministic per-contributor roll seed; a `GrantEquipment` protocol message. | RFC-018 |
| Leader failure detection & session recovery | Zero election/heartbeat/quorum machinery anywhere in `src/` (grepped, no hits); `Tick{tick,world_ms,night}` already fans out unconditionally every tick from `MapDirector` (`protocol.hpp:23`, `map_director.hpp:70`) but is consumed for simulation timing, not liveness detection. `ARCHITECTURE.md §2` accepts, as a stated design choice, that the world simply stops when the leader dies, naming only a portable save file and periodic saves as mitigation — both now real via RFC-016. | A node-side `Tick`-timeout rule and a client-side content-independent liveness cadence (deliberately *not* keyed off `ChunkDelta` silence, which is legitimate during a quiet chill session); a `WorldClosing` graceful-shutdown broadcast; a client `Connected → LeaderUnreachable/HostClosed` state machine with tone-guardrail-safe copy; a recovery ledger assembled from RFC-016's save-exclusion rulings; a named "any friend can relaunch as the new leader" manual-restart flow. **Explicitly not built**: automatic/Byzantine-fault-tolerant election or any anti-cheat/hostile-leader defense — reaffirmed out of scope per `ARCHITECTURE.md §0 S1`/`§2`'s deliberate "kick is enough, Trusted is technical not defensive" posture, which this RFC does not contradict. | RFC-024 |

### Build order

RFC-018 depends only on already-accepted RFC-016/019/021/022; RFC-024 depends only on
already-accepted RFC-014/015/016. Neither names the other as a dependency in either direction —
they are independent of each other and of the HUD/audio/harness trio above, and may land in any
order relative to one another.
