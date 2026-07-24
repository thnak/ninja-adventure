# RFC-016: Persistence & Save-File Format

> Status: **Accepted (revised after review)**
> Design canon: [ROADMAP.md P5](../ROADMAP.md) ("Bền vững" — `Persistent<EventSourced>` for world
> state with periodic snapshot to the trusted node, `Persistent<Snapshot>` for player progression,
> multi-world create/load/delete, and the stated recovery bar: "tắt máy giữa lúc chơi, mở lại, thế
> giới đúng như cũ, sai lệch ≤ 60 giây") · [ARCHITECTURE.md §2](../ARCHITECTURE.md) ("Leader chết thì
> sao": the world stops on leader death, mitigated only by an exportable/portable save file and
> periodic + on-exit saves; auto leader-election explicitly deferred) · [ARCHITECTURE.md §3](../ARCHITECTURE.md)
> (storage-model sketch table — accounts/progression on the leader via SQLite, world state
> event-sourced with snapshot to the leader, creature/raid state explicitly not saved) ·
> [GAME.md §0](../GAME.md) (chill guardrail — nothing counts down behind the player)
> As-built source grounding: `src/world/account.hpp` (`AccountStore` — the one persistence mechanism
> that already ships: a raw fixed-record binary file, magic+version+flat array, saved on
> account-create/login and on clean exit only, no periodic save, no crash-signal handler; its own
> header comment: "It is not persistence — P5 owns that, and will key its saves by `AccountId` rather
> than by session slot for exactly this reason"), `src/client_main.cpp:32,756,793` (`kAccountsPath =
> "accounts.dat"`, the two call sites that currently save it — post-login and on `world.stop()`),
> `src/world/player_actor.hpp` (the full persistence-candidate field set: `account_`, `x_`/`y_`,
> `hp_`/`mana_`/`stamina_`, `level_[4]`/`xp_[4]`, `items_[kItemKinds]`, `deaths_`, `mounted_`,
> `respawn_tx_`/`respawn_ty_`, `ability_cd_[]`; `equipped_ability()` computed from `level_`, never
> stored — no manual loadout field exists), `src/world/tiles.hpp` (`kPlayerMaxHp/Mana/Stamina`,
> `kRespawnTicks=30`, `kHealthRegenMs=3000`, `kCombatCooldownMs=5000`, `kMapCount=2`, `kTicksPerSecond
> =10`, `Crop{tx,ty,kind,stage,planted_ms,ripe_ms}`, `Building{tx,ty,kind,hp,cooldown,level}` — the
> exact structs a chunk actor's dynamic overlay state is made of), `src/world/chunk_actor.hpp`
> (`handle(PlaceBuilding)`, `handle(UpgradeBuilding)`, `handle(PlantCrop)`, `handle(HarvestAt)` — the
> shipped, already-`Described` protocol messages that mutate `buildings_`/`crops_`; `SetRespawn` fired
> back to `PlayerActor` when a hearth is placed), `src/world/world.hpp` (`kWorldSeed` used as a single
> compile-time constant threaded into every `generate_terrain`/worldgen call — not yet a runtime,
> per-world value)
> Engine grounding (`/home/nvthanh/works/QuarkCpp`, spec 012-Persistence + ADR-009 + ADR-024):
> `include/quark/core/persistence.hpp` (`Persistent<Snapshot|EventSourced, PersistMode::Sync|Batched>`
> policy tags, the `Store` concept, `FenceToken`, `InMemoryStore` reference adapter — genuinely
> shipped, unit-verified, and currently used by **zero** actors anywhere in `src/` — grepped, zero
> hits for `Persistent<` in this game's code), `include/quark/core/event_log.hpp` (`EventLog::stage/
> commit/rollback`, `replay_tail`, `recover_event_sourced` — the commit-at-handler-completion hook is
> explicitly "REPORTED rather than wired," i.e. an actor's own handler code must call `commit()`, the
> engine does not do it automatically), `include/quark/core/snapshot.hpp` (`save_snapshot`,
> `snapshot_sequential`, `load_snapshot(_migrated)` — likewise explicit calls a handler drives, not an
> automatic per-message hook), `include/quark/core/file_store.hpp` (`FileStore` — std-only, always
> available), `include/quark/adapters/sqlite_store.hpp` (`SqliteStore` — opt-in via
> `QUARK_WITH_SQLITE`, WAL journal mode, `PRAGMA synchronous=FULL`, one DB file, schema `meta`/
> `snapshots`/`events` keyed by a `"{type:x}-{key:x}"` actor-id text key), `decisions/ADR-024-
> eventsourced-compaction-cadence-and-checkpoint-mode.md` (**Accepted** engine decision: wall-clock
> `CompactEvery<T>` cadence + background `CompactionJob`/`CompactionApplied` close-out won the
> red-team/prover process — but grepped, **zero** occurrences of `CompactEvery`/`CompactionJob`
> anywhere in `include/`: the decision is accepted, the mechanism is not yet built, matching the exact
> "designed but unwired" shape RFC-014 found for `declare_lazy`/`IdleTimeout`, except here the wiring
> genuinely does not exist yet even at the engine layer, not just at this game's call sites)
> Depends on: RFC-014 (Instance & Realm Lifecycle, accepted-with-revisions — the `TEARING_DOWN` save
> hook §3.6 this RFC's world-state mechanism plugs into, the "instanced maps do not survive a leader
> restart" default policy this RFC does not override, and RFC-014 §6.1's named-but-unresolved
> requirement for a per-player return-location breadcrumb, resolved here at §7), RFC-022
> (accepted-with-revisions — `MapId`/`MapDescriptor`/`Portal`/`MapSession` shapes, cited not
> reproduced), RFC-019 (accepted-with-revisions — the XP/skill-level state shape §5.1 this RFC
> persists verbatim), RFC-020 (accepted-with-revisions — the quest-instance state machine and
> objective-progress shape §6 this RFC persists verbatim), RFC-007 (accepted-with-revisions — the
> checkpoint/`meta.json` sidecar format §6.2 and the generation-0/publish/raid-setback lifecycle §6.3
> this RFC gives a storage location and retention policy to, explicitly left unspecified there), RFC-008
> (accepted-with-revisions — non-goals naming "leader SQLite" as the assumed future name for
> player-authored loadout storage and excluding live-state serialization from the pack contract; this
> RFC is the spec that actually commits to that choice)
> Depended on by: RFC-013 (proposed, drafted concurrently this run — Vitals, Death & Recovery; needs
> this RFC's ruling on which vitals/death fields are persisted vs. ephemeral, §5), RFC-015 (proposed,
> drafted concurrently this run — Client Replication; a reconnecting client's resumed position and
> vitals ultimately read from this RFC's schema, a minor touchpoint only)

---

## Summary

Three things this project has never had to decide before, decided here, all green-field:

1. **The storage backend is `quark::adapters::SqliteStore`** — QuarkCpp's already-vendored,
   already-verified, opt-in SQLite adapter behind the engine's `Store` seam. This is not a new
   dependency choice; it is *formalizing* RFC-008's own assumed-future name ("leader SQLite") against
   a concrete, already-shipped engine primitive rather than a hypothetical one, and rejecting the
   temptation to invent a bespoke format when one line of `#define QUARK_WITH_SQLITE=ON` gets ACID
   transactions, crash-durable `fsync`, and a single portable file for free.

2. **Two different persistence *models* for two different kinds of state, exactly as ARCHITECTURE.md
   §3's own sketch table already separates them** — `Persistent<Snapshot, Batched>` for player
   progression/account-linked state (one row per account, whole-state overwrite, cheap and simple),
   and `Persistent<EventSourced, Sync>` for world overlay state (buildings, crops, tilled ground — a
   durable log of the exact shipped `PlaceBuilding`/`UpgradeBuilding`/`PlantCrop`/`TillGround`/
   `HarvestAt` messages chunk actors already handle, replayed in commit order on recovery). This RFC
   does not invent a new event *vocabulary* for world state — it reuses the protocol messages that
   already exist and are already `Described` (serializable), because they are already the exact shape
   of "a discrete, replayable world mutation." It does add one thin, map-aware envelope around them
   (§6.2) — the one piece of wiring that is new.

3. **A concrete answer to "≤60 seconds" that is mostly zero.** `PersistMode::Sync` durably commits a
   world-state mutation *before the handler that issued it returns* — so every building placed, every
   tile tilled, every crop planted, every quest completed, every level gained survives a hard kill with
   **zero** drift by construction, not by luck. The only state that can drift is state this RFC
   deliberately does not Sync-commit on every tick (a player's exact HP/position, the world clock's
   millisecond counter, and — by explicit ruling, §6.6 — a building's *combat damage in progress*,
   which always resolves in the player's favor on recovery) — for the position/clock category this RFC
   names a periodic checkpoint cadence and sets it tighter than the 60-second bar with margin (§6).
   Creature and raid state carries **zero** persistence obligation at all, because ARCHITECTURE.md §3's
   own table already excludes it ("không lưu") — restart reseeds wildlife deterministically from the
   world seed, the same mechanism that already makes worldgen itself reproducible, not a new mechanism
   this RFC has to build.

Everything specified below is **green-field**: `Persistent<...>` is used by zero actors in `src/`
today, `accounts.dat` is the only file that survives a restart, and there is no multi-world directory
layout, no RL-checkpoint storage location, and no chunk-overlay event log anywhere in the codebase.
Every new number is marked **(tunable)**.

---

## Motivation

1. **RFC-008 named a placeholder and explicitly declined to fill it.** Its non-goals say plainly:
   "Player-authored loadout storage... is player progression data (leader SQLite), not pack data." That
   sentence assumes a storage system that does not exist. This RFC is the spec that makes "leader
   SQLite" a real, committed decision rather than a name borrowed from nowhere.

2. **ROADMAP P5 states a recovery bar with a number in it, and nothing computes whether any design
   meets it.** "sai lệch ≤ 60 giây" is a testable claim, not a mood — a spec that does not show its
   arithmetic against that number has not actually satisfied P5's stated Done-when condition. §6 below
   does that arithmetic explicitly, term by term.

3. **RFC-007 built an entire checkpoint lifecycle — generations, gates, immutable-hash publication,
   raid setback — and stopped exactly at the filesystem boundary.** Its own §6.2 gives the JSON shape
   and the sidecar fields; its Motivation never claims a path. Without a path and a retention policy,
   "raid setback rolls the policy back `kRaidSetback=3` generations" (RFC-007 §6.3) is a sentence with
   nothing to roll back *to* the moment more than 3 generations have ever been produced and nothing
   has been kept.

4. **ARCHITECTURE.md §2 names the leader-death mitigation and does not specify what it contains.**
   "Save file phải xuất được và di chuyển được" (the save file must be exportable and movable) is a
   requirement with no checklist behind it. A player who tries to hand a friend "the save file" today
   would hand them `accounts.dat` and nothing else — no progression, no world state, no checkpoints —
   and get back a world that remembers who everyone is and nothing about what they did.

5. **RFC-014 named a dependency on this RFC by number and left it as an open question.** §6.1: "Whether
   [the return-location breadcrumb] means a copy of those three fields riding along in the player's own
   persisted record... is not decided here — flagged as a concrete, named requirement for RFC-016."
   §7 below is that decision.

---

## Guide-level Explanation

### For a player

Nothing about playing changes. What changes is invisible until the day it matters: if the leader's
machine crashes mid-session, the world that comes back when it restarts remembers everything you built,
everyone you fought, and everything you learned — not "roughly," but exactly, for anything that was a
deliberate action (a building placed, a quest turned in, a level gained). The only thing that can be a
few seconds stale is your exact position and health at the instant of the crash, and even that error
bar is small and shrinking, never something you're asked to manage. Your friends can zip up the whole
world folder, hand it to someone else, and that person's own machine picks up exactly where yours left
off — the same portability a Minecraft world folder already has.

Creating, loading, and deleting a whole world is an explicit menu action — a name you type, a folder
that either exists or doesn't. Deleting one is a confirmed, deliberate choice, never something that
happens because you didn't log in for a while (GAME.md §0 — nothing here decays on a clock).

### For a designer

Every new persisted concept — a new item kind, a new quest field, a new building type — is a column or
a small table, added the same way RFC-008 already adds a JSON field: additively, with a schema-version
bump, never by silently reinterpreting old bytes. You do not design storage; you tell this RFC's schema
what shape your data is and it tells you which of the two models (Snapshot or EventSourced) it belongs
in, using the same two-bucket test every time: "is this a whole-state fact that's cheap to overwrite
wholesale" (Snapshot) or "is this a sequence of discrete things that happened, worth replaying in
order" (EventSourced).

### For an engineer

You need: (1) the storage backend and directory layout (§1–§2) — one `SqliteStore`-backed DB per world
for account-linked state, one leader-resident `WorldPersistenceActor` fronting a per-persistent-chunk
`EventLog` for overlay state; (2) the concrete schema for accounts/progression/loadout/respawn-death/
quest state (§3–§5), all keyed by the existing `AccountId`, not by the ephemeral session slot; (3) the
world-overlay event vocabulary (§6), which reuses the shipped `PlaceBuilding`/`UpgradeBuilding`/
`PlantCrop`/`TillGround`/`HarvestAt` message *shapes* verbatim, wrapped in one thin envelope that
carries the map disambiguator none of them carry today (§6.2) — no new event *semantics*; (4) the
checkpoint cadence and triggers that make the
≤60s bar an arithmetic fact, not a hope (§6.3); (5) the RL checkpoint path and rotation policy (§8),
closing RFC-007's named gap; (6) the portable save file's exact contents and the one SQLite-specific
gotcha (WAL sidecar files) that makes a naive `cp` of the `.db` file alone an incomplete copy (§9); (7)
the concrete, currently-unwired engineering gaps this RFC surfaces rather than pretends are solved —
`EventLog::commit()`/`snapshot_sequential()` are calls a handler must drive explicitly, and this RFC
names exactly where those calls belong (§6.2, §10).

---

## Reference-level Design

### 1. Storage backend: `SqliteStore`, one file per world for account-linked state

**Decision, with reasoning, not asserted.** Three storage options exist in the engine today behind the
identical `Store` seam: `InMemoryStore` (not durable — disqualified by definition), `FileStore`
(std-only, always available, an append-only WAL — a legitimate fallback with no extra build
dependency), and `SqliteStore` (opt-in, `QUARK_WITH_SQLITE`, ACID transactions, WAL journal mode,
queryable). This RFC picks `SqliteStore` as the default for account-linked state (§3–§5) for three
concrete reasons, none of them "because RFC-008 already used the name":

- **Queryability is a real player-facing feature for an open-source game with no ops team.** A player
  who wants to know "why did my inventory look wrong" can open `progression.db` with any SQLite
  browser and *look*. A `FileStore` WAL is opaque without writing a reader.
- **The schema (§3) is genuinely relational** — one row per account, a variable number of quest
  instances per account, a variable number of item stacks per account. SQLite's native table/foreign-key
  shape fits this data better than a flat per-actor blob would.
- **A single portable file is exactly the shape the exportable-save-file requirement (§9) needs**, and
  `SqliteStore`'s own header comment names this directly: "a good default 'real database' when
  RocksDB's write throughput isn't needed," which this project's 20–50-player, low-write-frequency
  workload never approaches.

`FileStore` remains this RFC's fallback for the world-overlay event log (§6) specifically, discussed
there — not because `SqliteStore` cannot serve it too (it can, and is the default), but because the
overlay log's access pattern (append-mostly, replay-on-recovery, never queried ad hoc by a human) does
not need SQLite's query surface, and this RFC does not want to force `QUARK_WITH_SQLITE` to be a hard
build requirement for a game that otherwise builds with zero extra dependencies (`GAME.md`/
`ARCHITECTURE.md`'s repeated "0 đồng, không cần gì thêm" framing). **This RFC's ruling: `SqliteStore`
is the default for both, `FileStore` is an explicitly supported fallback for the overlay log only, and
this is a build-configuration choice, not a schema choice** — either backend implements the identical
`Store` concept, so nothing in §3–§6's schemas changes based on which one is compiled in.

### 2. Directory layout and multi-world create/load/delete

Green-field — no such layout exists today; `accounts.dat` lives at a single fixed relative path with no
world concept at all.

```
saves/
  <world_name>/
    manifest.json          # world_seed, display name, created_at, schema_version — §2.1
    accounts.dat            # UNCHANGED format (account.hpp) — moved here, not redesigned
    progression.db           # SqliteStore — §3–§5's tables
    progression.db-wal       # SQLite WAL sidecar (present while the leader is running)
    progression.db-shm       # SQLite shared-memory sidecar (present while the leader is running)
    world/
      overlay.db              # SqliteStore (or overlay.wal + overlay.snap under FileStore, §1) — §6
    checkpoints/
      <policy_id>/
        gen_0.json  gen_0.meta.json     # copied from the repo's committed bootstrap, §8
        gen_47.json gen_47.meta.json    # trained live in this world
        ...
```

#### 2.1 `manifest.json`

```json
{
  "schema_version": 1,
  "world_name": "Thung Lũng Sương",
  "world_seed": 20260722,
  "created_at_unix": 1784800000,
  "engine_persistence_backend": "sqlite"
}
```

`world_seed` is the field this RFC's directory layout exists to carry. **Named code change, not
designed here:** `kWorldSeed` (`world.hpp`) is a single compile-time `constexpr` today, threaded into
every `generate_terrain`/worldgen call at every call site. Supporting more than one world means it
becomes a runtime value read from `manifest.json` at bring-up and passed down the same call chain —
mechanically similar in shape to RFC-014's own "two ways to close this gap, neither chosen here" pattern
(RFC-014 §3.2): this RFC names the requirement and defers the exact threading mechanism (constructor
parameter vs. a `WorldConfig` resource) to implementation, because settling it needs to look at every
`kWorldSeed` call site, which is an engineering pass, not a design decision (Open Questions §2).

#### 2.2 Create / load / delete — explicit, never automatic

| Action | Mechanism |
|---|---|
| **Create** | Player names a world (a menu text field, mirroring the account-name flow account.hpp already has); `mkdir saves/<name>/`, write `manifest.json` with a fresh `world_seed` (a real random draw, not a fixed constant — the compile-time `kWorldSeed` becomes this world's *default only if the player does not override it*, useful for reproducing the exact shipped 51-village layout on demand), copy repo-committed generation-0 RL checkpoints (§8) into `checkpoints/`. Empty `progression.db`/`accounts.dat`/`overlay.db` are created lazily on first write, not pre-populated. |
| **Load** | Player picks an existing `saves/<name>/` from a directory listing (the Main Menu's "Chọn thế giới" entry, GAME.md §12, already reserves this UI slot); the leader opens `accounts.dat`, `progression.db`, `overlay.db`, reads `world_seed` from `manifest.json`, and runs recovery (§6.4, §10). |
| **Delete** | An explicit, confirmed menu action (`rm -rf saves/<name>/`, behind a "type the world's name to confirm" guard — the same weight as any other destructive, irreversible action in a tool this casual). **Never automatic, never time-triggered, never triggered by "nobody has loaded this world in N days"** — an idle-decay deletion policy would be exactly the kind of clock GAME.md §0 forbids, applied to the *player's own save* rather than to in-game content, which would be worse, not better. |

### 3. Accounts: unchanged format, relocated only

`AccountStore`'s binary format (`account.hpp`) is **not touched by this RFC** — it already works, it is
already the "one thing not compromised on" (its own header comment) for password-hash security, and
churning a security-sensitive file format for the sake of schema uniformity would be a net risk for
zero player-facing benefit. This RFC's only change is *where* the file lives (`saves/<world_name>/
accounts.dat` instead of a fixed relative path) and *when* it saves — folded into this RFC's periodic
checkpoint cadence (§6.3) in addition to the two triggers it already has (post-login, clean exit,
`client_main.cpp:756,793`), so a hard kill no longer depends on the clean-exit path having run.

`AccountId` (1-based, `account.hpp`) is the foreign key every table in §4–§5 is keyed by — never the
session slot, per `account.hpp`'s own stated intent ("will key its saves by `AccountId` rather than by
session slot").

### 4. Player progression, loadout, respawn/death state — `Persistent<Snapshot, Batched>`

**Model choice.** One row per account is a whole-state fact — reading it needs the *current* value of
every field, never "replay every HP change since 2019." `Persistent<Snapshot, PersistMode::Batched>` is
the correct model: `PersistMode::Batched` because a snapshot fired on every regen tick (every player,
every 100 ms) would be 80+ writes/second at the 8-slot roster's ceiling for a value that changes by at
most a handful of points between snapshots — exactly the workload `PersistMode::Batched`'s own
definition exists for ("persist asynchronously, coalescing writes; ack before durable, bounded loss
window" — `persistence.hpp`). The bounded-loss window this mode accepts is the source of §6's ≤60s
arithmetic, not a hidden cost.

#### 4.1 Schema

```sql
CREATE TABLE players (
  account_id     INTEGER PRIMARY KEY,   -- account.hpp's AccountId, 1-based
  -- MapId (RFC-022 §1.1). Normally persistent-band, [0,16) — but transiently holds an instanced-band
  -- value ([16,65536)) while the player is inside an open MapSession: §7's ruling stamps this column
  -- with the live session's MapId the moment a portal use joins/creates one, and RFC-014 §6's
  -- reconnect-in-place path (rfc-spec/RFC-014-instance-realm-lifecycle.md, "persisted map still
  -- equals session.map_id") depends on this column holding that instanced value while the session is
  -- open, not a persistent-band fallback. return_map/x/y below are the persistent-band fallback,
  -- read only once the session has closed.
  map            INTEGER NOT NULL,
  x              REAL NOT NULL,
  y              REAL NOT NULL,
  hp             INTEGER NOT NULL,
  mana           INTEGER NOT NULL,
  stamina        INTEGER NOT NULL,
  deaths         INTEGER NOT NULL DEFAULT 0,
  respawn_tx     INTEGER NOT NULL,
  respawn_ty     INTEGER NOT NULL,
  -- RFC-014 §6.1's return-location breadcrumb — see §7. NULL when the player was not last inside
  -- an instance, or the instance's session was still open at last checkpoint (in which case the
  -- reconnect path (RFC-014 §6) is the one that matters, not this fallback).
  return_map     INTEGER,
  return_x       INTEGER,
  return_y       INTEGER,
  level_melee    INTEGER NOT NULL DEFAULT 0,
  xp_melee       INTEGER NOT NULL DEFAULT 0,
  level_ranged   INTEGER NOT NULL DEFAULT 0,
  xp_ranged      INTEGER NOT NULL DEFAULT 0,
  level_magic    INTEGER NOT NULL DEFAULT 0,
  xp_magic       INTEGER NOT NULL DEFAULT 0,
  level_craft    INTEGER NOT NULL DEFAULT 0,
  xp_craft       INTEGER NOT NULL DEFAULT 0,
  -- Reserved, not read by shipped logic today (RFC-019 §5.4: loadout is auto-picked from level_[]
  -- until RFC-011 decides otherwise). NULL = "use the shipped auto-pick." The column exists NOW so
  -- that whenever RFC-011 lands a manual picker, it is a code change to WRITE this column, never a
  -- schema migration to ADD it — the exact gap RFC-008's non-goals named.
  equipped_ability_0  INTEGER,
  equipped_ability_1  INTEGER,
  schema_version INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE player_items (
  account_id  INTEGER NOT NULL REFERENCES players(account_id),
  item_kind   INTEGER NOT NULL,     -- ItemKind enum ordinal (tiles.hpp)
  count       INTEGER NOT NULL,
  PRIMARY KEY (account_id, item_kind)
);
```

`ability_cd_[]` (per-ability cooldown remaining) is **deliberately not persisted** — it is transient
combat state, not progression, and its worst-case loss (a cooldown silently clearing on restart) is
strictly *in the player's favor*, never a loss, so it needs no recovery story (§ Tone Guardrail
Compliance point 3). `dead_ticks_` (the 30-tick respawn countdown, `kRespawnTicks`) is likewise not
persisted — §5 below gives the exact reason and the recovery rule. `mounted_` (`player_actor.hpp`) is
likewise **deliberately not persisted**: a restart always resumes unmounted, which is harmless (a mount
is a local, freely-re-obtainable state, never a possession that can be lost) and needs no recovery story
beyond that same asymmetry.

#### 4.2 Persistence policy field-by-field

- **`level_*`/`xp_*`, `player_items`, `equipped_ability_*`.** Persisted via the periodic checkpoint
  (§6.3) *and* immediately on every discrete progression event — a level-up (RFC-019 §5.1's
  `GrantXp`-driven loop), an item grant/spend. This mirrors the shipped precedent exactly:
  `accounts.dat` already saves on the discrete "account created/logged in" event *and* on clean exit
  (`client_main.cpp:756,793`); this RFC extends the same shape (event-triggered + periodic) to
  progression rather than inventing a different one.
- **`hp`/`mana`/`stamina`/`x`/`y`.** Persisted **only** on the periodic cadence (§6.3) — Sync-committing
  these on every regen tick is the workload §4's Model choice already rejected. This is the field group
  §6's arithmetic is actually about.
- **`respawn_tx`/`respawn_ty`.** Persisted immediately on `SetRespawn` (fired the moment a hearth is
  placed, `chunk_actor.hpp::handle(PlaceBuilding)`) — a rare, meaningful event; Sync cost here is
  negligible and losing a just-placed hearth's respawn point to a crash would be a real, avoidable
  loss, not a cosmetic one.
- **`deaths`.** Persisted immediately on death (`HurtPlayer` reducing `hp_` to 0, `player_actor.hpp`) —
  a display statistic (GAME.md nowhere ties consequence to it), cheap and rare enough to Sync without
  a workload concern.

### 5. What this RFC decides about death state, pending RFC-013

This RFC does not own death *rules* — RFC-013 (proposed, drafted concurrently) does. It owns only what
survives a restart, using the shipped fields that exist today (`player_actor.hpp`'s `respawn()`,
`dead_ticks_`, `deaths_`) as the baseline, and states one explicit ruling RFC-013 must either accept or
override, not leave ambiguous:

**Ruling: `dead_ticks_` (the mid-respawn countdown) is never persisted, and a recovering player whose
last-checkpointed `hp <= 0` is loaded as already-respawned** — `x`/`y` set to `respawn_tx`/`respawn_ty`,
`hp`/`mana`/`stamina` set to max, exactly `respawn()`'s own shipped logic (`player_actor.hpp:440-447`).
The reasoning: a leader restart landing mid-countdown is already an edge case rare enough that
resuming the *exact* remaining tick count buys nothing perceptible (`kRespawnTicks = 30`, three
seconds, `tiles.hpp:1114`) against real engineering cost (persisting and correctly resuming a countdown
that is otherwise deliberately ephemeral, `player_actor.hpp` never marks it `Persistent`-worthy on its
own). Skipping straight to "already respawned" is strictly in the player's favor — it can only ever
save them a few seconds they would have waited anyway, never cost them anything — so it needs no
justification beyond that asymmetry (§ Tone Guardrail Compliance). **RFC-013 may specify something more
precise if it has a reason to; this RFC's default requires nothing from RFC-013 to be correct on its
own.**

### 6. World overlay state — `Persistent<EventSourced, Sync>`, reusing shipped messages

#### 6.1 What actually needs event-sourcing

Not everything ARCHITECTURE.md §3 calls "world state" needs the same treatment. Concretely, per-chunk
dynamic state is two shipped structs (`tiles.hpp`) plus one shipped terrain overlay that has no struct
of its own:

```cpp
struct Crop     { uint16_t tx, ty; CropKind kind; uint8_t stage; int64_t planted_ms, ripe_ms; };
struct Building { uint16_t tx, ty; BuildKind kind; int16_t hp; uint8_t cooldown, level; };
// Tilled ground has no struct — TillGround{tx,ty,player} (protocol.hpp) writes Terrain::kDirt
// directly into ChunkActor's own terrain_[] cache (chunk_actor.hpp::handle(TillGround)). It is a
// mutation to the terrain grid itself, not an entry in a per-chunk array, but it is exactly as
// durable a fact as a Crop or Building: since GAME.md's starting farmland apron is gone, every tile
// of dirt beyond wild terrain in the world was tilled by a player, and generate_terrain() (below)
// has no memory of that — it only knows the seed-derived function.
```

Both are already **derivable-in-part from time**: `Crop::stage` is computed from `(now_ms -
planted_ms)` against `grow_ms_of(kind)` (`chunk_actor.hpp`'s own crop-growth logic) — so the durable
fact worth logging is "a crop of kind K was planted at tile (x,y) at time T," not "a crop is currently
at stage 2," and recovery recomputes `stage` from the persisted `planted_ms` against the restored world
clock (§6.5), the same way a chunk that goes to sleep and wakes up (LOD, ARCHITECTURE.md §4) already
"catches up for free" per that section's own claim. This RFC reuses that existing determinism property
rather than fighting it.

**The event vocabulary is not new.** `PlaceBuilding{tx,ty,kind,player}`, `UpgradeBuilding{tx,ty}`,
`PlantCrop{tx,ty,kind,now_ms}`, `TillGround{tx,ty,player}`, `HarvestAt{tx,ty}` (`protocol.hpp`, already
`Described` — required for ordinary actor messaging, which the durable-record encoding in
`snapshot.hpp`/`event_log.hpp` reuses verbatim, `encode_durable<T>` over any `Described T`) are
**already** exactly "a discrete, replayable world mutation." This RFC's contribution is the wiring that
additionally makes each of these messages a durable, ordered log entry — not a new message format for
any of the five, and one small, necessary addition on top: none of the five carry a `map` field
(verified field-by-field, `protocol.hpp:59-94`), so §6.2 wraps each in a thin envelope that adds it.

#### 6.2 `WorldPersistenceActor` — why the durability lives off `ChunkActor` itself

`ChunkActor` cannot safely carry `Persistent<...>` directly. Its placement is `Placement<HashById>`
with **no** `Require<Trusted>` — its own file's comment, cited by RFC-014 §Multiplayer, states plainly
"a chunk may be hosted on a player's machine... safe by selection, not by hope." A `Persistent<...>`
actor opens a real `Store` connection (a `SqliteStore` file, or `FileStore`'s local WAL) — handing that
capability to an actor type that can be placed on an *untrusted* player's machine would mean an
untrusted node holds a write handle to durable world state, exactly the failure mode `Require<Trusted>`
exists to prevent everywhere else in this codebase (RFC-014 §2's `InstanceManager`, the shipped
`PlayerActor`/`MapDirector` pair).

**This RFC's design: a new `Require<Trusted>` actor, `WorldPersistenceActor`, placed identically to
`InstanceManager` and `MapDirector` (RFC-014 §2's pattern, cited not re-derived), holds the `Store`
connection and a keyed collection of per-chunk `EventLog`s — one per **persistent-band** chunk
(`MapId ∈ [0,16)`, RFC-022 §1.1) that has ever received a mutating message:**

**The one piece of new wiring: a map-carrying envelope.** `chunk_key(ChunkCoord{map,cx,cy})`
(`tiles.hpp:717-722`) is deliberately map-aware — it is sparse-by-design specifically so it can double
as a placement key across every persistent-band `MapId`. But none of `PlaceBuilding`/`UpgradeBuilding`/
`PlantCrop`/`TillGround`/`HarvestAt` carry a `map` field (verified field-by-field, `protocol.hpp:59-94`)
— the call site knows `map` (it is the routing argument to `chunk_ref(map, tx, ty)`, `world.hpp:369,374,
384,397,411`) but never puts it in the payload, because `ChunkActor` itself doesn't need to be told
which map it's on — it already knows, from its own `coord`. `WorldPersistenceActor` is a different actor
keyed by the same tile coordinates across *every* persistent-band map at once, so it does need `map` in
the message. This RFC's fix: a thin envelope, not a new event vocabulary — each variant is
byte-identical to the shipped message plus the one field:

```cpp
template <class Msg>
struct PersistOverlayEvent { std::uint16_t map; Msg event; };  // envelope only — Msg is unchanged

struct WorldPersistenceActor
    : quark::Actor<WorldPersistenceActor, quark::Sequential, quark::Priority<1>,
                   quark::Placement<quark::HashById, Require<Trusted>>> {
    using protocol = Protocol<Tick,
        PersistOverlayEvent<PlaceBuilding>, PersistOverlayEvent<UpgradeBuilding>,
        PersistOverlayEvent<PlantCrop>, PersistOverlayEvent<TillGround>,
        PersistOverlayEvent<HarvestAt>>;

    SqliteStore*  store = nullptr;   // or FileStore*, §1's build-config choice — identical Store concept
    std::unordered_map<ChunkKey, EventLog<ChunkMutationEvent, SqliteStore>> logs_;  // one per chunk

    template <class Msg>
    void handle(const PersistOverlayEvent<Msg>& e) noexcept {
        commit_one(chunk_key({e.map, chunk_of_tile(e.event.tx), chunk_of_tile(e.event.ty)}), e.event);
    }
};
```

**How it receives the same message `ChunkActor` does, without a new fan-out mechanism.** The caller
(`World::place_building()`, `world.hpp`, or whichever future crafting/building system emits these)
already resolves a target chunk and `tell()`s it directly (`chunk_ref(map, tx, ty).tell(PlaceBuilding{...})`,
`world.hpp:411`). This RFC's ruling: that same call site additionally `tell()`s `WorldPersistenceActor`
with `PersistOverlayEvent{map, PlaceBuilding{...}}` — the identical message data, wrapped, since `map`
is already a local at that call site — **two sends from one call site, not a chunk-to-leader forwarding
hop** — because forwarding through `ChunkActor` first would mean an untrusted node's mailbox ordering
decides whether a durable commit happens, exactly the authority inversion `Require<Trusted>` exists to
prevent. This is the same "message, not a direct call, and never routed through the untrusted party"
discipline RFC-014 §3.4 already established for `MapDirector::chunks` mutation.

**Commit timing — the engineering gap this RFC must name, not pretend is automatic.** `EventLog::commit()`
is, per the engine's own header comment, "REPORTED rather than wired" — no `Activation::drain_step` hook
calls it automatically. This RFC's ruling: `WorldPersistenceActor`'s own handler for each of the five
messages stages the event (`log.stage(ev)`) and calls `log.commit()` **synchronously, before the
handler returns** — this is what makes the mode `Sync` in practice (the policy tag documents the
*contract*, "persist before the message completes"; this RFC's handler body is what actually
*fulfills* that contract, since the engine does not do it for you). A handler that returns without
committing would silently violate its own declared `PersistMode::Sync` — this RFC treats "call
`commit()` at the end of every mutating handler, unconditionally" as a hard implementation requirement,
not a suggestion (Open Questions §5 names the remaining, more general engine-wiring question this local
rule sidesteps rather than solves).

#### 6.3 Periodic snapshot: bounding replay length, and the ≤60s arithmetic

Two independent cadences, deliberately not one, matching the same "two distinct questions" pattern
RFC-014 §3.5 used for its own two teardown timers:

```
kProgressionCheckpointIntervalTicks (tunable) = 300 ticks = 30 s at kTicksPerSecond=10
kOverlaySnapshotIntervalTicks       (tunable) = 1800 ticks = 3 min
```

1. **`kProgressionCheckpointIntervalTicks`** drives a leader-side Tick handler (on `WorldPersistenceActor`
   or a small sibling actor — implementation detail, not load-bearing here) that, for every bound
   session slot, `ask<GetPlayer>`s the current `PlayerView` and writes it into `players` (§4) via
   `Persistent<Snapshot, Batched>`. **This is the number the ≤60s bar is actually about.** At 30s,
   worst-case drift on a hard kill for the *only* category of state that is not Sync-committed on every
   discrete change (a player's exact `hp`/`x`/`y` between checkpoints) is bounded at **30 seconds**,
   half the stated 60-second budget, with the other half held as margin against this being a first
   guess (Open Questions §7). Every other persisted field (progression, items, quest state, buildings,
   crops, respawn point) is Sync-committed on its own discrete event and therefore has **zero** drift,
   independent of this number entirely.
2. **`kOverlaySnapshotIntervalTicks`** drives `WorldPersistenceActor` to call `snapshot_sequential()`
   for each open per-chunk `EventLog`, folding its committed tail into a single `SnapshotRecord` (the
   engine's own "compaction checkpoint" framing, `event_log.hpp`). This bounds **replay time on
   restart**, not data loss — every event is already durable the instant it committed (§6.2); this
   cadence only keeps `recover_event_sourced()`'s tail-replay from growing unboundedly over a
   long-running world. 3 minutes at this project's building/planting action rate (a low-frequency
   player-driven action, not a hot simulation path) keeps any one chunk's un-compacted tail in the tens
   of events, not thousands.

#### 6.4 Recovery

On leader bring-up, for every persistent-band chunk that has an entry in `progression.db`/`overlay.db`
(i.e., has ever been mutated — an untouched chunk needs no recovery, its state is purely the seed-derived
terrain function every node already computes for free):

```
state, last_seq = recover_event_sourced<ChunkMutationEvent>(store, chunk_key, initial={})
                      // engine call, event_log.hpp — load latest snapshot, replay tail, fold in order
apply each recovered Building/Crop into the freshly-constructed ChunkActor's buildings_/crops_, and
each recovered TillGround into terrain_[] (Terrain::kDirt at that tile) — before it starts ticking
```

This is a direct application of `event_log.hpp`'s own `recover_event_sourced()` — this RFC supplies the
`ChunkMutationEvent` type (a tagged union over the five message shapes, §6.1 — `PlaceBuilding`,
`UpgradeBuilding`, `PlantCrop`, `TillGround`, `HarvestAt`) and the `apply()` fold function (literally the
same logic `chunk_actor.hpp::handle(PlaceBuilding)`/`handle(TillGround)` etc. already contain, run once
per recovered event instead of once per live message, and run *after* `generate_terrain()` so a
recovered `TillGround` always overwrites the seed-derived terrain underneath it, never the reverse), not
a new recovery mechanism.

#### 6.5 World clock

`MapDirector` is already `Require<Trusted>` and leader-resident (unlike `ChunkActor`), so unlike §6.2's
case it **can** safely carry `Persistent<Snapshot, Sync>` directly on its own small clock state
(`world_ms`, current day/season index) — no indirection actor needed. Sync cost here is negligible: a
day/season transition is a rare, discrete event (GAME.md §9's four-season, ~20-minute-day cadence), and
Sync-committing on every such transition gives the world clock **zero** drift, not merely ≤60s — a
strictly better bound than §6.3 gives player position, achieved for free because the calendar changes
orders of magnitude less often than a player's HP does. The running millisecond counter between
transitions still rides the periodic cadence (§6.3) for the same reason player position does — nobody
needs the clock accurate to the tick on restart, only accurate to "which day/season it visually is,"
which the Sync-committed transition events already guarantee exactly.

#### 6.6 Building HP: deliberately excluded from the event vocabulary, by ruling

`Building::hp` is mutated outside all five named messages: combat damage from creatures
(`chunk_actor.hpp::attack_blocking_building()`, `b.hp -= c.damage`, erasing the entry from `buildings_`
on death at `hp <= 0`) happens on the tick-paced creature-movement path, not through
`PlaceBuilding`/`UpgradeBuilding`. That path is never staged or committed by §6.2's mechanism.

**This RFC's ruling: combat damage to a placed building is deliberately not persisted.** A building's
`hp` recovers to `max_hp_of(kind, level)` — full health at its last Sync-committed `level` — never to
its last-known damaged value, and a building destroyed by combat but not yet re-placed by a player
reappears at full HP after a leader restart. This is not an oversight; it is the same category of state
as creature/raid combat itself, which ARCHITECTURE.md §3's own table already excludes from durability
("không lưu") — building HP-in-combat is downstream of exactly that excluded state, not independent of
it. It is also, by construction, strictly in the player's favor (§ Tone Guardrail Compliance point 3):
the only two outcomes a restart can produce for a damaged or destroyed building are "as strong as a
player last built it" or "a building lost to a siege comes back," never a worse outcome than what the
player actually had. **This narrows Summary point 3's "zero drift" claim**: placement, upgrade, and
removal-by-player are zero-drift; combat damage in progress is the one category of building state this
RFC accepts non-zero (but always favorable) drift for, named here rather than left for a reader to
discover in `chunk_actor.hpp`.

### 7. Resolving RFC-014's flagged breadcrumb requirement

RFC-014 §6.1: a reconnecting player whose persisted `map` points at a `MapId` whose `InstanceSession`
has already closed needs a fallback location — "the exact overworld tile their group's portal use
originated from" — and that data must "survive independently of the ephemeral `InstanceSession` record
itself." **This RFC's ruling: `return_map`/`return_x`/`return_y` are columns on `players` (§4.1),
written whenever a player's persisted `map` is about to move into the instanced band** — i.e., at the
moment `resolve()` (RFC-022 §2.3) hands out a `Teleport` into a freshly joined or created `MapSession`,
the *same* checkpoint write that updates `map`/`x`/`y` also stamps `return_map`/`return_x`/`return_y`
with the portal's origin tile, Sync-committed (a rare, discrete event — every portal use, not every
tick). These three columns are the sole mechanism RFC-014 §6.1's fallback needs; they persist
independently of `InstanceSession` by construction, because they live in a completely different table
in a completely different persistence model (`players`, Snapshot-model, leader-restart-durable) than
`InstanceSession` (in-memory only, `InstanceManager`'s own table, explicitly not part of the portable
save file per RFC-014 §Multiplayer).

### 8. RL checkpoint storage and retention — closing RFC-007's gap

**Location**, per §2's directory layout: `saves/<world_name>/checkpoints/<policy_id>/gen_<N>.json` +
`gen_<N>.meta.json`, the exact two-file shape RFC-007 §6.2 already specifies (weights JSON + sidecar,
"the upstream format is not ours to extend" — this RFC does not touch that format, only where the pair
lives on disk).

**Bootstrap.** Repo-committed generation-0 checkpoints (RFC-007 §6.3: "never random weights," a
hand-scripted behavior-cloned network committed to the repo) live at a *separate*, source-controlled
path outside any `saves/` directory (e.g. `data/checkpoints/<policy_id>/gen_0.{json,meta.json}` — the
exact path is a build-layout detail, not specified further here). World creation (§2.2) copies these
into the new world's own `checkpoints/<policy_id>/gen_0.*` as that world's starting point — every world
begins training from the identical, already-verified bootstrap, but each world's *subsequent* training
(generations 1+) is entirely local to that world and never shared or merged across worlds.

**Retention policy.**

```
kCheckpointRetentionCount (tunable) = 6   -- per policy_id
```

Rule: always keep generation 0 (never rotated out — it is the permanent fallback RFC-007 §6.3 already
requires: "the scripted `boss_policy` remains a permanent, indistinguishable fallback"), plus the
current published generation, plus the `kCheckpointRetentionCount - 2` generations immediately before
it. On every new `PUBLISH generation+1` (RFC-007 §6.3's own state-machine transition — this RFC hooks
into that exact event, does not invent a new one), delete the oldest retained generation beyond this
window (never generation 0).

**Why 6, not fewer.** RFC-007 §6.2's raid setback rolls a published policy back
`kRaidSetback = 3` generations. A retention window narrower than `kRaidSetback + 1` could leave the
setback with nothing to roll back *to* the moment a dungeon has been raided more than once in quick
succession relative to its training cadence. `6` gives a comfortable margin above the `4`-generation
floor that arithmetic implies, not a tight fit against it.

**Integrity check on load.** Each `.meta.json`'s `weights_hash` (RFC-007 §6.2's own field) is verified
against a fresh hash of its paired `.json` on leader bring-up, for the currently-published generation of
every policy only (not the whole retained window — an O(policies) check, not O(policies ×
retained-generations)). A mismatch (the shape of a crash mid-write leaving a torn file — this RFC's own
periodic-cadence framing applies here too: a checkpoint publish is a rare, discrete, already-Sync-shaped
event, so a torn write is an edge case, not a steady-state risk) falls back to the next-newest generation
still on disk within the retention window — which is exactly what retaining more than one generation
was for. If *no* generation in the retained window verifies (a scenario this RFC judges pathological
enough not to design further), the policy falls back to generation 0, the one file that is never rotated
out and is re-copyable from the repo besides.

### 9. The portable save file

**What it is:** the entire `saves/<world_name>/` directory (§2), copied or zipped as a unit. Nothing
about this RFC's design requires a bespoke export format — the directory *is* the export, matching
ARCHITECTURE.md §2's own framing ("save file phải xuất được và di chuyển được") literally rather than
inventing a container format on top of files that are already portable individually.

**What is deliberately *not* in it, and why that is fine, not a gap:**

- **Instanced-band map state** (`MapId ∈ [16, 65536)`, RFC-022 §1.1) — RFC-014's own default policy is
  that instanced maps never survive a leader restart at all (RFC-014 §Multiplayer: "no `MapId` in the
  instanced band survives a restart"). This RFC's world-overlay mechanism (§6) is scoped to the
  persistent band only for exactly that reason — there is nothing to export because RFC-014 has already
  ruled it should not exist across a restart.
- **`InstanceManager`'s session table, creature positions, raid combat state, ability cooldowns.** All
  explicitly excluded from durability by RFC-014 §Multiplayer or ARCHITECTURE.md §3's own table
  ("không lưu"). Restart reseeds all of it — wildlife deterministically from the world seed (already a
  shipped property, `seed_wildlife(kWorldSeed)`), creature spawns from the same worldgen/spawn logic
  every fresh world already runs.

**The one SQLite-specific gotcha this RFC must name explicitly, or the export is silently wrong.**
`SqliteStore` opens its database in WAL journal mode (`sqlite_store.hpp`: `PRAGMA journal_mode=WAL`).
Under WAL mode, recently-committed data can live in the `-wal` sidecar file rather than the main `.db`
file until a checkpoint operation folds it back — **a naive copy of only `progression.db` (or
`overlay.db`) while the leader is running can omit committed data that is still sitting in the `-wal`
file.** This RFC's ruling: an export **must** either (a) run only while the leader process is stopped
(the simple, always-correct case — every sidecar file is present and consistent, copy the whole
directory including `-wal`/`-shm`), or (b) if a live export is ever supported, issue `PRAGMA
wal_checkpoint(TRUNCATE)` against every open `SqliteStore` connection immediately before copying, folding
the WAL back into the main file and truncating it to zero. This RFC specifies the requirement and the
exact SQLite pragma that satisfies it; it does not design the live-export UI flow itself (Open Questions
§3) — the safe, always-available baseline is simply "export while stopped," which is already sufficient
to satisfy ARCHITECTURE.md §2's stated requirement.

### 10. Schema versioning

Reuses the engine's own convention, cited not reinvented: every durable record `encode_durable<T>`
writes already carries a `{type_key, schema_version}` header (`snapshot.hpp`'s own comment, "016 durable
records are the canonical tagged encoding"), and `decode_durable_migrated`/`read_migrated` fold an older
schema forward on read. This RFC's own top-level `players.schema_version`/`manifest.json.schema_version`
integer columns exist *in addition* to that per-record header for the same reason RFC-008 §6 keeps a
pack-level schema version alongside per-document versions: a human (or a migration script) needs to ask
"what shape is this whole save" without decoding every row first. The discipline is identical to RFC-008
§6's, cited by name and not reproduced: additive changes bump the field's own version, breaking changes
bump the whole schema and ship a migration pass.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001..010 (accepted combat set)** | Not load-bearing, except RFC-007 (below). Ability cooldowns, in-flight combat state, and everything RFC-002/003/004/009/010 define as live simulation state are explicitly not persisted by this RFC (§4.1, § Multiplayer). |
| **RFC-007** (accepted) | §6.2's checkpoint format and §6.3's generation lifecycle are cited, not modified. This RFC supplies the storage path and retention policy (§8) RFC-007's own Motivation explicitly left open. |
| **RFC-008** (accepted) | Non-goals named "leader SQLite" and "per-instance/live state serialization" as this RFC's territory; §1/§3–§6 are that commitment. Pack/definition data itself is untouched — this RFC never stores a copy of pack content, only ids/references into it (`item_kind`, `quest_id` as RFC-008/RFC-020-style string ids). |
| **RFC-013** (proposed, concurrent) | Owns death/vitals *rules*; this RFC owns what of that state survives a restart (§5) and states one explicit default ruling RFC-013 is free to override with a reason, not required to. |
| **RFC-014** (accepted-with-revisions) | §3.6's `TEARING_DOWN` state names the save hook this RFC's `WorldPersistenceActor` does *not* need to serve, because instanced maps are out of scope by RFC-014's own default policy (§9). §6.1's flagged breadcrumb requirement is resolved at §7. |
| **RFC-015** (proposed, concurrent) | Minor touchpoint only — a reconnecting client's resumed position/vitals are read from this RFC's schema (§4), not redefined by it. |
| **RFC-019** (accepted-with-revisions) | §5.1's XP/level state shape is persisted verbatim (§4.1's `level_*`/`xp_*` columns); this RFC introduces no new progression mechanic. |
| **RFC-020** (accepted-with-revisions) | §6's quest state machine and objective-progress shape map directly onto §5's schema (below) — `QI2`'s "Abandoned/Expired instances are deleted, not archived" is honored literally: this RFC's `quest_instances`/`quest_objectives` tables hold only `Accepted` (and briefly `Complete`-pending-payout) rows, matching RFC-020's own statement that Abandoned/Expired leave no trace. |
| **RFC-021 / RFC-022** (accepted-with-revisions) | `MapId`/`Portal`/`MapSession` cited, not reproduced. §4.1's `map`/`return_map` columns store `MapId` values exactly as RFC-022 §1.1 defines them. |

### Quest state — the schema §5's family omitted above, stated here for completeness

```sql
CREATE TABLE quest_instances (
  account_id     INTEGER,           -- NULL for community-scope (village/fort) instances — RFC-020 §6
  quest_id       TEXT NOT NULL,     -- e.g. "quest.village_well_repair" — RFC-020 §2's own id shape
  -- Emergent quests: the bound village/fort/gate id (RFC-020 §1, §6 — "an emergent document is a
  -- template... instantiated many times at runtime with a bound_entity filled in"). 0 = sentinel for
  -- authored (non-emergent) quests, which are never bound and never collide on quest_id alone. Part
  -- of the key, not just a data column: two villages both raided the same night both instantiate
  -- quest.raid_defense independently (GAME.md's stated raid cadence makes this a normal occurrence,
  -- not an edge case), and without bound_entity in the key those two instances collide on
  -- (NULL, 'quest.raid_defense').
  bound_entity   INTEGER NOT NULL DEFAULT 0,
  accepted_tick  INTEGER NOT NULL,  -- absolute world tick; DISPLAY ONLY per RFC-020 QI1 — never a guard
  PRIMARY KEY (account_id, quest_id, bound_entity)
);
CREATE TABLE quest_objectives (
  account_id     INTEGER,
  quest_id       TEXT NOT NULL,
  bound_entity   INTEGER NOT NULL DEFAULT 0,  -- mirrors quest_instances.bound_entity, same sentinel
  objective_idx  INTEGER NOT NULL,
  count_current  INTEGER NOT NULL,
  PRIMARY KEY (account_id, quest_id, bound_entity, objective_idx)
);
```

Only `Accepted` instances get a row — RFC-020's own state machine (§6, Q1/Q4/Q5/Q6) deletes the
in-memory `QuestInstance` on every terminal transition (Complete/Abandoned/Expired); this RFC's
persistence follows that lifecycle exactly: a row is written when a quest is Accepted, updated as
`GameplayFact`s progress it (mirroring RFC-020 Q3, one `UPDATE` per fact — cheap, and this RFC treats it
as Sync-committed for the same reason quest completion is meaningful state, not regenerable), and
deleted the instant the in-memory instance is (Complete/Abandoned/Expired all map to `DELETE`, matching
RFC-020's QI2 "Abandon and Expire are behaviorally identical" — this RFC adds no distinction the
gameplay layer itself does not make). Village/fort-owned community-scope aggregate standing itself
(the `village_standing` reward kind, RFC-020 §7), and the **per-player contribution ledger** RFC-020
§Multiplayer requires for community quests (capped at `kMaxParticipants = 50`), are **not** in this
table — RFC-020 §"For an engineer" itself places that ledger on "the trusted actor that will eventually
own the bound village/fort's authoritative state," i.e. a not-yet-designed `VillageActor` tier, not on
the per-account tables this RFC owns. No RFC in this batch yet defines a `VillageActor` state shape to
persist (Non-goals, Open Questions §4) — this RFC's `quest_instances`/`quest_objectives` intentionally
stop at per-account objective progress and do not attempt to own that ledger.

---

## Multiplayer & Simulation-LOD Considerations

- **`Require<Trusted>` co-location, the established pattern extended once more.** `WorldPersistenceActor`
  sits on the leader alongside `PlayerActor`, `MapDirector`, and RFC-014's `InstanceManager` — the
  fourth `Require<Trusted>` actor this codebase has, not a new trust boundary. Every `Store` connection
  this RFC opens (accounts, progression, overlay, checkpoints) lives exclusively on the leader; no
  player-hosted node ever touches durable state directly, for the identical reason RFC-014 §2 already
  argues for `InstanceManager`.
- **LOD is unaffected.** A sleeping chunk (ARCHITECTURE.md §4's 10/1/0 Hz tiers) has no pending
  mutations to persist by definition — nothing sleeps mid-`PlaceBuilding`. This RFC adds messages only
  on the already-rare "a player deliberately mutated the overlay" path, never on the tick path any LOD
  tier throttles.
- **20–50 concurrent players, no VPS.** The write workload this RFC's design produces at that scale is
  small by construction: discrete Sync commits fire only on deliberate player actions (building,
  quest completion, leveling — human-paced, not tick-paced), and the two periodic cadences (§6.3) are
  bounded, small, leader-local writes regardless of player count (one snapshot per bound session slot
  every 30s, one compaction pass per touched chunk every 3 minutes) — nowhere near `SqliteStore`'s
  actual ceiling, which this RFC does not need to benchmark to be confident is not the bottleneck at
  this project's target scale.
- **Leader-death handling — this RFC is the mechanism ARCHITECTURE.md §2 named and left unspecified.**
  "Save file phải xuất được và di chuyển được" + "lưu định kỳ + lúc thoát" (periodic + on-exit saves)
  are literally §9 (the portable directory) and §6.3/§4.2 (the periodic cadences) respectively. Auto
  leader-election remains explicitly out of scope, per ARCHITECTURE.md §2's own deferral ("Bầu leader tự
  động là chuyện của sau này, nếu có bao giờ cần") — this RFC only makes a *manual* restart resume
  correctly, never attempts automated failover.
- **Determinism.** This RFC introduces no new seed-derived computation and no cross-node agreement
  requirement — persistence is entirely leader-local I/O, off the simulation's deterministic hot path,
  the same boundary RFC-014 §Multiplayer already drew for its own allocation counter.

---

## Tone Guardrail Compliance

1. **The periodic checkpoint cadences (§6.3) are never displayed, to anyone, under any circumstance.**
   No "last saved 12s ago" HUD element, no save-in-progress spinner, no warning about an upcoming
   checkpoint. A snapshot happens silently in the background of a leader-only actor; the only
   player-visible consequence of this entire RFC, ever, is that the game remembers things correctly —
   never a countdown, a wait, or a status the player is asked to track.
2. **World/account deletion (§2.2) is the only destructive action this RFC introduces, and it is
   exclusively player-initiated, explicit, and confirmed.** Nothing here ever deletes a world, an
   account, or a checkpoint because time passed, a player was offline, or a threshold was crossed —
   the single deletion trigger anywhere in this RFC is a human clicking "delete" and confirming it.
3. **Every field this RFC chooses *not* to persist (`ability_cd_`, `dead_ticks_`, `mounted_`,
   creature/raid state, a building's combat-damage-in-progress) resolves in the player's favor on
   recovery, never against it** (§4.1, §5, §6.6, § Multiplayer) — a cooldown clearing early, a respawn
   countdown skipping ahead, a raid resetting to "no attack in progress," a besieged building coming
   back are all strictly better outcomes for the player than the alternative, so none of them need a
   mitigation, an apology message, or a "you lost progress" framing anywhere in the UI.
4. **The ≤60-second bound (§6.3) is an internal engineering budget, never an ambient player-facing
   caveat during ordinary play.** Nothing in this RFC's own design surfaces "your last 30 seconds
   might not have saved" as a running caveat, countdown, or thing the player is asked to manage or
   wait out — it is a property this RFC proves true of the system, not a number attached to play as it
   happens. The sole named exception is RFC-024's one-time, past-tense, on-demand disclosure made only
   after a leader-failure incident has already occurred and already been banner-announced
   (RECONCILIATION.md Ruling 18) — a retrospective answer to a question the player has already started
   asking, not the ambient caveat this rule guards against.
5. **RL checkpoint rotation (§8) never changes what a player already sees mid-fight.** RFC-007 §6.2's
   own rule — "live creatures never hot-swap weights mid-fight; a new generation applies from the next
   spawn" — is untouched by this RFC's storage/retention layer; rotation only affects which *file* the
   *next* spawn reads, never anything about a fight already in progress.

No mechanic in this RFC creates a deadline the player watches, a decaying resource, a login-frequency
dependency, or a countdown of any kind visible from inside the game.

---

## Open Questions

1. **The exact `EventLog::commit()`/`snapshot_sequential()` wiring generalization.** §6.2 names a local
   rule ("call `commit()` unconditionally at the end of every mutating handler") sufficient for
   `WorldPersistenceActor`'s own five message types. Whether the engine ever grows the automatic
   handler-completion hook its own header comments describe as a "reported seam" is an engine-level
   question this RFC does not decide — if it ever lands, this RFC's local rule becomes redundant but
   not incorrect.

2. **`kWorldSeed` becoming a runtime, per-world value.** §2.1 names the requirement; the exact threading
   mechanism through every `generate_terrain`/worldgen call site is an implementation pass this RFC does
   not carry out, mirroring RFC-014 §3.2's own "named, not chosen" pattern for `ChunkActor`'s lazy-wiring
   gap.

3. **Live (leader-running) save export.** §9 names the correctness requirement
   (`PRAGMA wal_checkpoint(TRUNCATE)` before copying) but does not design the UI flow or the
   coordination point that would make a live export safe without briefly pausing writes. The
   always-correct fallback — export while stopped — is sufficient to satisfy ARCHITECTURE.md §2 as
   written and is this RFC's default; a live-export feature is future work.

4. **Village/fort persistent state, including the community-quest contribution ledger.** No RFC in this
   batch defines `VillageActor`'s state shape (tier, population, standing per visitor, and the
   per-player raid/assault contribution ledger RFC-020 §Multiplayer assigns to that same not-yet-designed
   tier) — this RFC's schema has nothing to attach any of it to yet. When a future RFC specifies
   `VillageActor`, its persistence should follow §1's Snapshot-vs-EventSourced test the same way this
   RFC's own tables do, not invent a third model; whether the contribution ledger even needs to survive
   a restart (it is scoped to one active raid's short lifetime) is a question that RFC should also
   settle, not this one.

5. **Whether progression state should ever move from Snapshot to a hybrid with an audit trail.** §4's
   Model choice picks Snapshot for simplicity and workload fit; an anti-cheat or dispute-resolution
   case for "show me every XP grant this account ever received" would argue for EventSourced instead.
   Not designed here — no accepted or proposed RFC in this set currently needs it.

6. **`progression.db`/`overlay.db`/`accounts.dat` cross-consistency after a partial manual copy.** §9
   treats the whole `saves/<world_name>/` directory as one atomic unit for export purposes, but nothing
   in this RFC detects or repairs the case where a player manually copies only some of the files (e.g.
   `accounts.dat` alone). No corruption results (each file is independently well-formed), but the
   *combination* would be semantically wrong (accounts with no matching progression rows). Not guarded
   against here; flagged for whichever pass builds the actual export/import UI.

7. **Checkpoint-cadence tuning.** `kProgressionCheckpointIntervalTicks = 300`,
   `kOverlaySnapshotIntervalTicks = 1800`, `kCheckpointRetentionCount = 6` are first-guess numbers
   computed against stated budgets (§6.3, §8), not measured against a running leader under real
   building/questing load — the same caveat RFC-014 Open Question 6 already carries for its own
   first-guess timers.

---

## Non-goals

- **Death, respawn, and ejection *rules*.** RFC-013's (proposed). This RFC only states what of the
  shipped vitals/death field set survives a restart (§5) and one explicit default the concurrent draft
  is free to override.
- **Pack/definition data format.** RFC-008's (accepted), explicitly excluded there and not touched
  here — this RFC stores only ids/references into pack content, never a copy of the content itself.
- **Automatic leader election or any failover protocol.** ARCHITECTURE.md §2 explicitly defers this
  indefinitely; this RFC's leader-death handling (§ Multiplayer) states only what a *manually restarted*
  leader can resume, never an election mechanism.
- **Instance save-before-teardown timing.** RFC-014's call entirely (§3.6's `TEARING_DOWN` state names
  the hook). This RFC's default policy is that instanced maps are simply out of scope for durability
  (§9) — if RFC-014 or a future revision ever decides some instanced content *should* persist, the
  format it would use is this RFC's Snapshot/EventSourced choice (§1), but the decision of *whether* and
  *when* remains RFC-014's.
- **Client-facing replication or reconnection wire protocol.** RFC-015's (proposed). This RFC only
  supplies the schema a reconnecting client's resumed state is read from.
- **`VillageActor`/fort persistent state shape.** No owning RFC yet exists (Open Questions §4).
- **A live, leader-running save-export UI flow.** §9 names the correctness requirement; the flow itself
  is future work (Open Questions §3).
- **An automated migration *tool*.** §10 states the versioning discipline (additive vs. breaking, per
  RFC-008 §6's precedent); writing an actual migration runner for a breaking schema change is an
  implementation task for whenever the first breaking change is needed, not designed here.

---

## Review Record

Both reviewers voted **revise**. Five upheld findings (two independently converged, two from Reviewer A
alone, one from Reviewer B alone), one downgraded to minor, one partially conceded — all applied below.

- Quest PK omitted `bound_entity` → both reviewers, converged: added `bound_entity` (sentinel `0`) to
  `quest_instances`/`quest_objectives` PKs (§6.4 quest tables).
- `WorldPersistenceActor`/`chunk_key_of` had no `map` field to disambiguate persistent-band maps →
  Reviewer A: added `PersistOverlayEvent<Msg>{map, event}` envelope (§6.2); corrected "no new message
  types" claims (Summary, engineer's checklist).
- `TillGround` omitted from the persisted event vocabulary → both reviewers, converged (blocker): added
  throughout §6.1/§6.2/§6.4 as a fifth event type; noted it mutates `terrain_[]` directly, not a struct.
- `Building.hp` combat mutation never staged/committed → Reviewer A: added explicit ruling §6.6 (combat
  damage deliberately not persisted, player-favorable); narrowed Summary point 3's "zero drift" claim.
- `players.map` comment contradicted §7/RFC-014's reconnect requirement → Reviewer B: rewrote the
  column comment to state it transiently holds instanced-band values while a session is open.
- `mounted_` named in header but never ruled on → Reviewer B (minor): added explicit not-persisted
  ruling next to `ability_cd_`/`dead_ticks_` (§4.1).
- Reviewer A's call for a dedicated `quest_contribution` table → partially conceded by Reviewer B
  (ledger belongs to a future `VillageActor` tier per RFC-020's own text): not built as a table; instead
  added an explicit ruling + Open Questions §4 note assigning it there.

No unresolved objections — all mustFix items from both reviewers were applied or (for the contribution
ledger) resolved via the reviewers' own agreed concession.
