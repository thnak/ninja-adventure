# RFC-014: Instance & Realm Lifecycle

> Status: **Accepted (revised after review)**
> Design canon: [GAME.md §3](../GAME.md) (instances behind portals, one copy per group), [§10](../GAME.md)
> (dojo/võ đường spectacle rooms) · [ARCHITECTURE.md §4](../ARCHITECTURE.md) (`MapId`,
> `InstanceManager`, chunk create/destroy-on-demand, LOD table — the sketch this RFC operationalizes)
> · [ROADMAP.md](../ROADMAP.md) P4 ("hạ tầng instance... khu mỏ cần nó trước, và hầm ngục sau đó chỉ
> việc dùng lại"), P8 (dungeons reuse P4's infrastructure)
> As-built source grounding: `src/world/tiles.hpp` (`chunk_key`, `chunk_index`, `kChunkCount`,
> `kMapCount`, `kRoomW/H/Pitch`), `src/world/world.hpp` (`World::build_chunks`, `build_players`,
> `build_bosses` — the eager, cold-only registration this RFC extends), `src/world/chunk_actor.hpp`
> (default-constructed then imperatively wired — the pattern lazy activation must fit),
> `src/world/map_director.hpp` (`MapDirector::chunks`, the plain fan-out list this RFC's priming/
> teardown hooks into), `src/world/snapshot.hpp` (`SnapshotBus`, `kChunkCount`-sized), `src/world/
> player_actor.hpp` (`Require<Trusted>`, session-slot binding model) · `/home/nvthanh/works/QuarkCpp`
> engine: `include/quark/core/engine.hpp` (`declare_lazy<A>`, `IdleTimeout<Ms>` → `idle_ticks`),
> `include/quark/core/policies.hpp` (`IdleTimeout<Ms>` policy struct), `decisions/ADR-028-lazy-
> activation-idle-timeout-eviction.md` (Accepted — the "Broker-Actor Serialization" design this RFC
> builds on)
> Depends on: RFC-022 (accepted-with-revisions, Map System — `MapId` partition §1.1, `MapDescriptor`
> §1.2, `Portal` §2.2, `MapSession`/`resolve()` join-vs-create contract §2.3, all cited not
> reproduced), RFC-008 (accepted-with-revisions, Data-driven Skill Definition — Q3 per-realm pack
> layering, cited not resolved), RFC-007 (accepted-with-revisions, RL Observation & Action Space —
> §6.1 `CombatEnvironment`/dojo room grounding for §8's boss-room ruling)
> Depended on by: RFC-013 (Vitals, Death & Recovery — dungeon/mine ejection needs an instance to
> eject *from*), RFC-015 (Client Replication — needs this RFC's sparse chunk-addressing scheme before
> any instanced `MapId` can be streamed to a client), RFC-016 (Persistence & Save-File Format — needs
> this RFC's save-before-teardown timing contract and its "instanced maps do not survive a leader
> restart" policy), RFC-020 (accepted-with-revisions, Mission & Quest System — `rumor` quests are
> satisfied by entering a dungeon-gate instance and expect this RFC to tear it down on quest
> conclusion)

---

## Summary

RFC-022 defined *what* a `MapSession` is and the one-line rule for deciding whether a portal use
joins an existing session or calls `allocate_new()` — and stopped there by design. This RFC is
`allocate_new()`: what actually happens between "a player stepped on a portal with
`binding = kAllocateOnUse`" and "that player is standing on a live, ticking, populated instanced
map." It specifies (1) the spin-up procedure — picking a `MapId`, materializing that map's chunk
actors, loading its atlas — built on a genuine, previously-unused engine primitive
(`declare_lazy<A>` + `IdleTimeout<Ms>`, QuarkCpp ADR-028) that this RFC is the first spec to point
at; (2) a sparse addressing scheme that lets `chunk_index()` support the `[16, 65536)` instanced
`MapId` band without inflating every `kChunkCount`-sized array to 67 million slots — the redesign
RFC-022 §1.1 named as its own explicit prerequisite and left unowned; (3) group membership over a
live session's lifetime — joining, leaving through a return portal, and disconnecting, and why those
three are not the same event; (4) the idle-teardown and leader-restart policy, argued clean against
GAME.md §0 the same way every sibling RFC in this batch has to; and (5) an explicit ruling on the
shipped 10×7 dojo boss room: it stays exactly what it is, a room on the persistent `kInterior` map,
and should **not** migrate to a `MapSession` — because it solves a different problem than the one
`MapSession` exists for.

Everything here is **green-field**. No `InstanceManager` class, no lazy-activation call, no idle
chunk-actor eviction exists in `src/` today — `World::build()` still creates all 1024+1024 chunk
actors up front in a cold, single-threaded pass before the world starts ticking, exactly as
ARCHITECTURE.md §4 already admits. What is *not* green-field is the engine feature this design is
built on: QuarkCpp's ADR-028 ("Broker-Actor Serialization") is an **Accepted** engine decision that
already ships `Engine::declare_lazy<A>()` and a working `IdleTimeout<Ms>` → idle-eviction path
(`engine.hpp`, verified by direct read, not by RFC assertion) — it has simply never been called by
this game's code. Every number in this RFC is marked **(tunable)**.

---

## Motivation

Three things this RFC exists to close, each verified against the shipped code, not assumed from
prose:

1. **RFC-022 named its own prerequisite and explicitly declined to solve it.** §1.1: "the dense
   per-map array must become sparse... before `allocate_new` (§2.3) can hand out an instanced `MapId`
   safely," and its Interactions table hands the whole of `allocate_new`'s internals — "spin-up cost,
   idle timeout, capacity cap, leader-death handling" — to this RFC by name. RFC-022 is not silent by
   oversight here; it is silent because it correctly scoped itself to the *decision* (join vs.
   create), not the *mechanism*. This RFC is that mechanism.

2. **The engine already has the primitive ARCHITECTURE.md §4's own sketch names, and nobody has
   pointed a spec at it.** ARCHITECTURE.md §4's LOD table literally reads "Ngủ | không ai gần trong N
   phút | 0 — `IdleTimeout` cho actor ngủ" (Sleep | nobody nearby for N minutes | 0 Hz — `IdleTimeout`
   for the actor to sleep). That line was written when `IdleTimeout<Ms>`-driven eviction was, per
   QuarkCpp's own ADR-028, "sim-proven but absent from `activation.hpp`/`engine.hpp`/`timer_wheel.hpp`"
   — a *design*, not yet code. It has since shipped (`engine.hpp`: `declare_lazy<A>()`, `idle_ticks`
   resolved from `IdleTimeout<Ms>` at registration, a per-shard `ActivationBroker` that lazily
   constructs an actor on its first message). Nothing in `src/world/` has ever called `declare_lazy`
   — `World::build_chunks()` still uses the cold, eager `register_actor` path
   (`world.hpp`: "The whole roster is registered before `start()`, because `Engine::
   register_activation` is cold-only"). This RFC is the first spec to connect the two: the mechanism
   ARCHITECTURE.md wished for in 2026-07-22 is sitting in the engine, unused.

3. **`chunk_key()` is already future-proof; `chunk_index()` is not, and the fix is smaller than it
   looks.** `tiles.hpp`'s own comment on `chunk_index()` says it plainly: "Distinct from `chunk_key`,
   which is **sparse-by-design** because it doubles as a placement key." The 64-bit actor-addressing
   key (`chunk_key`, `(map << 32) | (cy << 16) | cx`) already supports every value in RFC-022's
   65,536-wide `MapId` space with zero changes — the only thing that breaks at scale is the *dense
   array index* (`chunk_index()`, `c.map * kChunksPerMap + c.cy * kMapChunks + c.cx`) that
   `SnapshotBus` and `effect_tick` use for their flat per-chunk storage. This RFC's §4 fixes exactly
   that one function, and notes along the way that it has a second latent bug beyond the one RFC-022
   named: `chunk_index()`'s formula multiplies by the *global* `kMapChunks` (32) regardless of the
   target map's actual `chunk_edge` (RFC-022 §1.2) — so even a naive "just widen the array" fix would
   silently allocate 32×32-worth of index space for a 2×2-chunk mission map. §4 fixes both problems
   with one scheme.

---

## Guide-level Explanation

### For a player

Nothing about walking through a portal *feels* different from what RFC-022 already promised: step on
the gate, a short transition, you're somewhere else. What this RFC adds is invisible by design — the
map you land on didn't exist a moment ago; it does now, generated on the spot, and it will quietly
stop existing again once your group has been gone long enough that nobody is coming back this
session. You never see a "this instance closes in N minutes" — because there is no such display, on
purpose (§ Tone Guardrail Compliance). If your friend disconnects mid-dungeon, their character stays
"in" the instance for a while — long enough to lag-reconnect back into the same fight — before the
game quietly gives up and treats them as having left, the same way any other absence gets treated:
without penalty, without a countdown they could watch.

### For a designer

Adding a new instanced map (a new mission, a new mine floor, a new rest realm) costs you a
`MapDescriptor` row (RFC-022 §1.3) and a `SessionScope` choice (RFC-022 §2.3) — this RFC does not ask
you to write allocation code. The one new thing you *can* tune per content type is how strict its
group binding is (§5) and how long it lingers empty before closing (§3.3) — both single numbers, both
already given sane defaults.

### For an engineer

You need: (1) `InstanceManager` (§2) — a `Require<Trusted>` actor that owns the leader-side allocation
table and drives `allocate_new()` (§3); (2) the spin-up procedure (§3) — how `declare_lazy<ChunkActor>`
plus a priming Tick fan-out actually materializes a small map's chunk actors without touching the
persistent overworld's eager path at all; (3) the sparse chunk-addressing scheme (§4) — the two-tier
index that replaces `chunk_index()`'s single dense formula; (4) group membership and the join/leave/
disconnect state machine (§5–§6); (5) the atlas refcounting rule (§7); (6) the boss-room ruling (§8).

---

## Reference-level Design

### 1. What this RFC inherits, unchanged

Cited from RFC-022, not reproduced: the `MapId` partition (`[0,16)` persistent, `[16,65536)`
instanced, §1.1), `MapDescriptor` and the one-sizing-rule (§1.2), the variant guidance table (§1.3),
the `Portal` data shape and its five kinds (§2.2), and the `MapSession` struct plus the leader-only
`resolve()` join-vs-create decision (§2.3). This RFC's own contribution begins exactly where RFC-022's
§2.4 says it should: "`allocate_new`'s internals... — RFC-014."

### 2. `InstanceManager`

A new `Require<Trusted>` actor, one per world, placed identically to `MapDirector` (same
`Placement<HashById, Require<Trusted>>` shape, `map_director.hpp`) and for the same reason —
allocation decisions must be leader-observed facts, not locally computed, exactly the guarantee
RFC-022's Multiplayer section already assumes for `resolve()`.

```cpp
struct InstanceManager : quark::Actor<InstanceManager, quark::Sequential, quark::Priority<1>,
                                       quark::Placement<quark::HashById, Require<Trusted>>> {
    // Wired at bring-up, mirroring MapDirector's wiring pattern.
    quark::LocalRouter*  router  = nullptr;
    quark::ActorRef      director_ref;         // MapDirector's own id — messaged, never mutated directly, §3.4
    SnapshotBus*         bus     = nullptr;    // to register/release sparse instance blocks, §4
    std::uint64_t        world_seed = 0;

    // Leader-resident session table. Not persisted (§ leader-death handling, §3.6) — every entry
    // here is, by RFC-022 §1.1's own admission, ephemeral by construction.
    std::unordered_map<MapId, InstanceSession> sessions_;
    MapId next_map_id_ = 16;   // monotonic allocator, §3.1

    // Called by `resolve()` (RFC-022 §2.3) when it decides to create rather than join.
    MapSession allocate_new(const Portal& portal, GroupId owner_group);

    // Called every DirectorTick (or a coarser derived cadence, §3.3) to age out idle sessions.
    void sweep_idle(std::int64_t world_ms);
};
```

`InstanceManager` does not itself run the simulation inside an instance — chunk actors do that,
exactly as they do for the overworld. `InstanceManager`'s job is entirely bookkeeping: which
`MapId`s are live, who is in each, and when to stop routing traffic to one.

### 3. `allocate_new()` — spin-up

#### 3.1 `MapId` allocation: monotonic, never reused

```
next_id = next_map_id_
next_map_id_ = (next_map_id_ == 65535) ? 16 : next_map_id_ + 1
```

RFC-022 §1.1 already states the reason the instanced band is 65,520 wide: "so allocation never needs
a reuse/recycling scheme, not because the game needs it." This RFC takes that at its word — a fresh
`MapId` is drawn from a monotonically increasing counter and **never reused** within one leader
process's uptime. At the target scale (20–50 concurrent players, GAME.md §11; dungeons sized 2–4,
GAME.md §3), even a wildly pessimistic one-new-instance-per-second sustained rate would take **over
18 hours** to exhaust the band. This sidesteps an entire class of correctness problem for free: a
just-closed instance's chunk actors can finish idling out of memory at their own pace (§3.5) with zero
risk of a new instance's traffic arriving at the same `MapId` before they are gone, because no new
instance will ever be assigned that number again this session. Wraparound behavior once the band is
actually exhausted is explicitly not designed here (Open Questions §1) — it is not expected to matter
at this project's scale, and designing for it now would be solving a problem that has never once been
observed to occur.

`next_map_id_` sits inside `[16, 65536)` by construction of the increment above; ids `0..15` (the
persistent band, RFC-022 §1.1) are never touched by this counter.

#### 3.2 Chunk-actor materialization: `declare_lazy`, not eager registration

`World::build_chunks()` is unchanged for the persistent band (`MapId` 0 and 1) — it keeps calling the
cold, eager `register_actor<ChunkActor>()` path exactly as it does today, for exactly the reason
RFC-022 §1.3 gives: persistent maps "exist for the life of the world" and must never be destroyed, so
there is nothing to gain from lazy construction for them.

For the instanced band, this RFC proposes one new, one-time, cold declaration alongside
`build_chunks()`:

```cpp
// Cold, once, at bring-up — before start(), exactly where build_chunks() already runs.
// Declares the TYPE as lazily-activatable; no instance of ChunkActor is constructed by this call.
engine_->declare_lazy<ChunkActor>(&resource_scope_);
```

From this point on, **any** `chunk_key()` in the instanced `MapId` range that has never been touched
will, on its first inbound message, be constructed by the engine's per-shard `ActivationBroker`
(ADR-028 Phase 4) exactly the way `router->get<ChunkActor>(key).tell(...)` already resolves an
eagerly-registered chunk today — `router.get<A>()`'s existing fallthrough to the lazy id-table on a
miss (ADR-028 Phase 4, `engine.hpp`) means **no call site anywhere in the game needs to change** to
benefit from this; `MapDirector`'s existing `router->get<ChunkActor>(chunk_key(c)).tell(t)` (§3.4
below) already does the right thing whether `c`'s chunk was registered cold or activates lazily on
this very call.

**The unresolved engineering question this RFC surfaces rather than answers.** `ChunkActor` is
default-constructed today and then imperatively wired (`world.hpp`: `ch->coord = coord; ch->router =
router_.get(); ... ch->generate_terrain(kWorldSeed);`) — per-instance data (which coordinate this is,
which map's terrain seed to use, whether it has a flow-field pointer) is assigned by the caller after
construction, not derived by the actor itself. `declare_lazy`'s `wire(const ResourceScope&)` hook is
type-level (shared resources like `router`/`bus`/`status`), not instance-level — it has no natural
place to receive "this actor's own `ActorId`, decode it into a `ChunkCoord`, and look up its owning
`MapDescriptor` to know which seed/flow-field/build-permission to use." Two ways to close this gap,
neither built or chosen here:

1. Extend `ChunkActor::wire()` to accept its own `ActorId` (already implicitly available to the
   broker at construction time) and have the actor self-initialize from it plus a shared
   `MapRegistry` resource (`MapId → MapDescriptor`, itself wired the ordinary way).
2. Keep `wire()` untouched and instead have every lazily-constructed `ChunkActor` start "dormant" —
   correctly placed and addressable, but inert until an explicit first `PrimeInstanceChunk{coord,
   seed, descriptor}` message (a new protocol message this RFC would add) does the same imperative
   setup `world.hpp` does today, sent once by `InstanceManager` as part of §3.3's priming pass.

Option 2 changes nothing about `ChunkActor`'s existing construction discipline and reuses the exact
same field-assignment code path `world.hpp` already has, at the cost of one extra message per chunk at
spin-up (cheap: `chunk_edge² ≤ 256` for any instance this RFC's variant table allows, §RFC-022 §1.3).
This RFC's working assumption is Option 2 for that reason, but does not commit to it as final — it is
exactly the kind of thing a short engineering spike against the actual `ActivationBroker` code should
settle before implementation, not something an RFC should pretend to have already prototyped.

#### 3.3 Priming: materializing a whole small map at once

Unlike the overworld — where "which chunks currently exist" and "which chunks are currently ticking
hard" are two different questions answered by LOD (ARCHITECTURE.md §4's 10/1/0 Hz table, unchanged by
this RFC) — an instance is small enough (`chunk_edge` 2–8 for everything in RFC-022 §1.3's guidance
table, i.e. 4–64 chunk actors) that there is no benefit to per-chunk laziness *within* an already-open
instance: a player should never watch a room materialize as they walk toward it. `allocate_new()`
therefore **primes the whole instance eagerly, at session creation**, by sending one message
(`PrimeInstanceChunk`, §3.2 option 2, or an ordinary `Tick` if option 1 is chosen) to every
`chunk_key()` in the new map's `chunk_edge × chunk_edge` grid, forcing all of them to materialize
before the `Teleport` that lands the player there is issued. "Create-on-demand" in this RFC's scope is
about the **map**, not the **chunk** — whether these `chunk_edge²` chunk actors exist *at all* right
now, not whether each one inside an already-open instance is independently sleeping. Per-chunk LOD
inside a live instance is the same mechanism the overworld already uses (ARCHITECTURE.md §4), unmodified
by this RFC.

#### 3.4 `MapDirector` fan-out becomes dynamic for the instanced band

`MapDirector::chunks` (`map_director.hpp`) is, today, a plain `std::vector<ChunkCoord>` filled once
cold from the world layout, fanned a `Tick` every `DirectorTick` — "every chunk and every player
actor, every tick... cheap precisely because an empty `Sequential` handler... is a few hundred
nanoseconds." This RFC extends that list to be **mutable at runtime for the instanced band only**:

- **On priming (§3.3):** `InstanceManager` `tell()`s `MapDirector` a new `FanOutAdd{coords}` message
  listing the new map's `chunk_edge²` coordinates; `MapDirector`'s own mailbox appends them to
  `chunks` from within its own handler, the same as any other actor-owned-state mutation. This is a
  **message, not a direct call into `MapDirector::chunks`** — `Require<Trusted>` co-location
  guarantees shared trust, not a shared shard or thread (`shard_of(ActorId)`, `engine.hpp`, hashes
  each actor's own id independently), and `MapDirector::chunks` is a plain, non-atomic
  `std::vector<ChunkCoord>` that `MapDirector`'s own `handle(DirectorTick)` concurrently iterates
  (`map_director.hpp`). A raw mutating call from `InstanceManager` into that vector would be an
  unsynchronized data race, not a benign implementation-style choice — the codebase's own two
  existing examples of `Require<Trusted>` actors cooperating, `PlayerBus` (`snapshot.hpp`) and
  `PlayerActor::publish()` (`player_actor.hpp`), both restrict themselves to atomic publish/load of an
  immutable view for exactly this reason, and this RFC follows the same discipline via an ordinary
  message instead.
- **On teardown (§3.5):** `InstanceManager` `tell()`s `MapDirector` a symmetric `FanOutRemove{coords}`
  message removing those coordinates from the fan-out list, through the same mailbox, not a direct
  call. From that tick forward, the instance's chunk actors receive **no more `Tick` messages at
  all** — not throttled, not LOD-slept, genuinely zero. This is the mechanism that makes the next step
  possible.

The persistent band's portion of `MapDirector::chunks` (the 2048 overworld/interior coordinates,
`kMapCount = 2` today) is untouched — this RFC adds to the list, never restructures its existing
contents or fan-out order.

**A known, unaddressed gap this audit surfaces.** `MapDirector::fan_beacons()` (`map_director.hpp`)
clamps its own beacon fan-out with `if (cx < 0 || cy < 0 || cx >= kMapChunks || cy >= kMapChunks)
continue;` — bounded to the global `kMapChunks` (32), not to the target map's real `chunk_edge`
(RFC-022 §1.3). Once §3.2's `declare_lazy<ChunkActor>()` covers the instanced band, a player near the
edge of a small instanced map (`chunk_edge` 2–8) sits inside a beacon fan-out window that still
reaches `chunk_key()`s outside that instance's real footprint but inside `[0,32)`, lazily activating
phantom `ChunkActor`s that `InstanceManager` never allocated and `MapDirector::chunks` never lists.
This RFC does not fix `fan_beacons()` — it is a pre-existing call site this RFC's own sparse-addressing
audit (§4) did not originally cover — but names it as a self-healing gap: any such phantom actor
receives no further traffic (it is outside the real footprint, so no player message reaches it either)
and idles out via its own `IdleTimeout<kInstanceChunkIdleTimeoutMs>` (§3.5) exactly like any other
instanced chunk actor. See Open Questions §7.

#### 3.5 Teardown: two timers, not one

Two distinct decisions, on two distinct clocks, because they answer two distinct questions ("should
this session still be considered open" vs. "has this specific chunk actor's memory actually been
reclaimed"):

```
kInstanceIdleGraceMs      (tunable) = 300,000 ms (5 min)  — matches ARCHITECTURE.md §4's own
                                                              "trống > N phút" phrasing verbatim
kInstanceChunkIdleTimeoutMs (tunable) = 30,000 ms          — per-actor IdleTimeout<Ms> policy
```

1. **Session-level (`kInstanceIdleGraceMs`).** `InstanceManager::sweep_idle()` (called on a coarse,
   non-per-tick cadence — e.g. once per `DirectorTick`'s day/night check, not once per simulation
   tick) watches each open session's `present.size()` (§5). The instant it hits zero, a grace clock
   starts. If it is still zero `kInstanceIdleGraceMs` later, the session moves to `TEARING_DOWN`
   (§3.6's state machine): `InstanceManager` removes the map's chunk coordinates from `MapDirector`'s
   fan-out list (§3.4) and marks the atlas refcount for release (§7). **This timer alone never frees
   any memory** — it only stops new traffic from arriving.
2. **Chunk-actor-level (`kInstanceChunkIdleTimeoutMs`).** This RFC requires adding
   `IdleTimeout<kInstanceChunkIdleTimeoutMs>` to `ChunkActor`'s own `Actor<>` policy-pack declaration
   (`chunk_actor.hpp`) — a single, **type-level** policy, the same `ChunkActor` type used by both the
   persistent and instanced bands. This is not a per-band split of the type itself; it is an asymmetry
   in which of the engine's two registration entry points constructs a given instance, and the RFC
   must name it explicitly rather than assert the split as an inherent structural fact:
   - `quark::register_actor<A>()` (`spawn.hpp`) — the free function `World::build_chunks()` already
     calls, unchanged, for the persistent band (§3.2) — calls `engine.register_activation(id, act,
     band, budget)` **without forwarding `idle_ticks` at all**, so every eagerly-registered actor is
     hardcoded to `engine.hpp`'s default of 0 regardless of what `IdleTimeout<Ms>` the type declares.
     Persistent-band chunks stay immune to eviction for exactly this reason — not because `KeepAlive`
     is some separate policy applied only to them, but because the entry point that constructs them
     never reads the type's `IdleTimeout<Ms>` in the first place.
   - `declare_lazy<ChunkActor>()` + the per-shard `ActivationBroker` (§3.2) — the entry point every
     instanced-band chunk actor is constructed through — resolves `idle_timeout_ms_of<A>()` from the
     type's compiled `TypeRegistry` metadata (`metadata.hpp`) once at broker construction
     (`engine.hpp`) and applies it to every lazily-activated instance of that type.

   Concretely, this RFC's engineering task is: (a) add `IdleTimeout<kInstanceChunkIdleTimeoutMs>` to
   `ChunkActor`'s `Actor<>` template arguments, and (b) leave `World::build_chunks()`'s existing
   `register_actor<ChunkActor>()` call for the persistent band untouched. No second `ChunkActor` type
   and no per-instance override are needed — the persistent/instanced split falls out entirely from
   which entry point constructs which instance, an incidental consequence of `register_actor<A>()`'s
   behavior, not a property this RFC designs into the type.

   Once step 1 above cuts off `Tick` delivery, and assuming no player-driven message reaches that
   chunk either (true by construction — nobody is present, §5), the engine's own idle-eviction wheel
   fires `kInstanceChunkIdleTimeoutMs` after the chunk's *last* message, deactivating the actor and
   reclaiming its memory through the engine's existing, already-proven close-out path (ADR-028's
   Dekker-fence eviction — this RFC invents no new concurrency mechanism, it only supplies the policy
   value and the type declaration above). Once every chunk actor in the instance has independently
   deactivated this way, the map's footprint is fully reclaimed — there is deliberately no single
   "instance destroyed" event this RFC waits for; each chunk actor's own idle timer is authoritative
   for its own memory, the same ownership discipline `ChunkActor` already uses for everything else
   ("one actor owns one chunk").

This two-timer design is a direct, load-bearing consequence of one fact this RFC's source audit
turned up and RFC-022 did not have available to it: the engine has **no programmatic "deactivate this
actor now" call** — `Deactivate` is exclusively a timer-wheel-fired internal control descriptor
(`activation.hpp`, `kControlFlagDeactivate`), never something application code can invoke directly.
`InstanceManager` therefore cannot "command" a chunk actor to die; it can only stop feeding it and let
its own declared `IdleTimeout<Ms>` do the work. This is not a workaround — it is the only mechanism
the engine offers, and it happens to compose cleanly with everything else in this design.

#### 3.6 Session state machine

```
ALLOCATING ──(priming complete, §3.3)──▶ ACTIVE ──(present.size() hits 0)──▶ IDLE
                                            ▲                                   │
                                            └──(a member reconnects, §6)────────┘
                                                                                 │
                                          (kInstanceIdleGraceMs elapses, §3.5)   │
                                                                                 ▼
                                                                          TEARING_DOWN
                                                                                 │
                                          (SnapshotBus block released, §4;      │
                                           save hook fires if the map's         │
                                           RFC-016 persistence policy calls     │
                                           for it — most instanced maps do      │
                                           not, RFC-022 §Interactions)          │
                                                                                 ▼
                                                                             CLOSED
```

`ACTIVE` and `IDLE` are functionally identical for `resolve()`'s join purposes (RFC-022 §2.3) — a
returning member of the *same* `owner_group` can still walk back into an `IDLE` session, which is
exactly what makes the grace window meaningful rather than cosmetic. Only `TEARING_DOWN` and `CLOSED`
count as "closing" for RFC-022 §2.3's `find existing session where ... not (full or closing)` check —
once a session starts tearing down it is never reopened; a group that shows up after that point gets a
fresh `allocate_new()` call, a fresh `MapId`, and (per §3.1) no risk of colliding with the one still
finishing its chunk-actor eviction in the background.

`ALLOCATING` is expected to be effectively instantaneous in practice — priming (§3.3) is local
computation and message delivery, no network fetch (packs are trusted repo content per RFC-008), and
atlas load is a local file read — but it is named as its own state because `resolve()` must not hand a
`Teleport` target to a caller before priming has actually completed.

**Completion-detection gap this RFC surfaces, does not close.** §3.3's priming fan-out is a `tell()` —
fire-and-forget, per `quark::ActorRef::tell`'s own definition — to every `chunk_key()` in the new map;
as written, `InstanceManager` has no stated mechanism to learn that all `chunk_edge²` lazily-activated
actors have actually finished construction and wiring before declaring `ALLOCATING` complete and
handing `resolve()` a `Teleport` target. The engine's only request/reply primitive is `ask<R>(Q)`
(`actor_ref.hpp`); the working assumption is that priming should use `ask<PrimeAck>(PrimeInstanceChunk{...})`
to each chunk and have `InstanceManager` wait for all `chunk_edge²` replies before transitioning to
`ACTIVE`, rather than the plain `tell()` sketched in §3.3 — but this RFC does not commit to that as
final engineering (Open Questions §8).

---

### 4. Sparse chunk-actor addressing — this RFC's own contribution

RFC-022 §1.1 named the problem and stopped: "the dense per-map array must become sparse... before
`allocate_new` can hand out an instanced `MapId` safely," naming `chunk_index()`, `SnapshotBus`, and
`effect_tick` as the affected consumers. This section is that redesign.

#### 4.1 What stays exactly as it is

`chunk_key()` (`tiles.hpp:719-722`) needs **zero changes** — its own comment already states why:
"sparse-by-design because it doubles as a placement key," a 64-bit value that already accommodates
every `MapId` up to 65,535 with no collision risk. Every actor-addressing call site
(`router->get<ChunkActor>(chunk_key(c))`, `chunk_ref`, `chunk_ref_at` in `world.hpp`) is untouched.

#### 4.2 What changes: the dense-array index, in two tiers

```cpp
// Persistent band: UNCHANGED formula, but now bounded to the fixed partition width RFC-022 §1.1
// commits to (16), not to the live kMapCount (2) — a static 8x growth (2,048 → 16,384 slots per
// kChunkCount-sized structure), still trivially small, and it means every future persistent MapId
// (2..15, RFC-022 §1.1's reserved range) needs no further redesign when one is eventually added.
constexpr int kPersistentBandCount = 16;   // RFC-022 §1.1's own fixed number, not tunable here

int persistent_index(ChunkCoord c) noexcept {
    // unchanged formula, RFC-022's own citation, tiles.hpp:726-728 — c.map < kPersistentBandCount
    return c.map * kChunksPerMap + c.cy * kMapChunks + c.cx;
}

// Instanced band: one block per currently-OPEN session, sized to THAT map's own chunk_edge — not
// to the global kMapChunks (32). This is the second bug this RFC's Motivation §3 names: naively
// widening chunk_index()'s existing formula to a bigger MapId range would still multiply every
// instance's local index by 32 regardless of its real size, wasting index space for every map
// smaller than the overworld. The per-instance block fixes both problems together.
struct InstanceChunkBlock {
    std::uint8_t chunk_edge;                 // this map's own MapDescriptor.chunk_edge (RFC-022 §1.2)
    std::vector<quark::ChunkViewPtr> slots;  // chunk_edge² atomics — sized to THIS instance only
};

// Owned by SnapshotBus (extended, not replaced) and by InstanceManager symmetrically — allocated at
// ALLOCATING (§3.6), released at CLOSED.
std::unordered_map<MapId, InstanceChunkBlock> instance_blocks_;

int instance_local_index(ChunkCoord c, const InstanceChunkBlock& b) noexcept {
    return c.cy * b.chunk_edge + c.cx;
}
```

`SnapshotBus::publish`/`view` (`snapshot.hpp:129,135`) becomes a two-way branch on `c.map`:

```cpp
void publish(ChunkCoord c, ChunkViewPtr v) {
    if (c.map < kPersistentBandCount) {
        slots_[persistent_index(c)].store(std::move(v), std::memory_order_release);   // unchanged
    } else {
        auto it = instance_blocks_.find(c.map);
        if (it == instance_blocks_.end()) return;  // stale publish after CLOSED — dropped, not UB
        it->second.slots[instance_local_index(c, it->second)]
            .store(std::move(v), std::memory_order_release);
    }
}
```

The lookup-miss branch (a publish arriving for a `MapId` whose block has already been released) is
explicitly handled as a no-op, not a crash — it is the expected shape of a race between a chunk
actor's very last `Tick`-driven publish and `InstanceManager` releasing the block at `CLOSED`, and per
§3.5 it can only happen after that chunk's own `IdleTimeout` has already fired with no further
messages pending, so it is a rare, harmless straggler, not a steady-state path.

`effect_tick` (`client_main.cpp:490`, `kChunkCount`-sized) and any other structure discovered to share
this shape must adopt the **identical** two-tier scheme, not a competing one — this is the same
discipline RFC-022 §Interactions already assigns to RFC-015 for wire encoding, extended here to
in-memory replication-adjacent structures generally.

#### 4.3 Why this doesn't touch the sim hot path

`chunk_index()`'s consumers are `SnapshotBus` (replication-adjacent, read by the renderer/network
layer), `effect_tick` (client-side audio dedup), and one more this RFC's audit initially missed:
`World::build_bosses()` (`world.hpp`) also indexes into `World::chunks_` — a third, separate
`kChunkCount`-sized `std::vector<std::unique_ptr<ChunkActor>>` (`world.hpp`) — via `chunk_index()`.
This does not change §4.2's unchanged `persistent_index()` formula or require any further redesign,
because `World::chunks_` is populated only by the cold, persistent-band-only bring-up pass (dojo/võ
đường room placement, §8, comes from the persistent layout, never an instanced one) — but the RFC's
completeness claim is corrected here rather than left overstated: **no simulation code path** —
`ChunkActor`'s own combat/movement/migration logic, `MapDirector`'s fan-out (§3.4) — ever calls
`chunk_index()`; they all address chunks exclusively by `chunk_key()`, which is unaffected by any of
this. The sparse redesign is entirely confined to the snapshot/publish/bring-up layer, exactly the
boundary RFC-022 §1.1 already drew.

---

### 5. Group membership over a session's lifetime

RFC-022 Open Question 8 leaves `GroupId` as an opaque comparison key with no owning party/group RFC.
This RFC does not invent one — it defines the minimum membership bookkeeping `MapSession` lifecycle
actually needs, independent of how a "group" forms or dissolves elsewhere:

```cpp
struct InstanceSession {
    MapSession     session;         // RFC-022 §2.3's struct, unchanged
    SessionState   state;           // §3.6
    std::int64_t   idle_since_ms = -1;   // -1 while ACTIVE
    // Every account that has EVER been part of this session (for kGroupInstance: seeded from the
    // requesting group at ALLOCATING; grows only on a fresh join via resolve(), never on rejoin).
    std::vector<AccountId>  members;
    // The subset of `members` currently physically located on `session.map_id` (per PlayerActor's
    // own `map` field) — the quantity §3.5's idle-grace timer actually watches.
    std::vector<AccountId>  present;
};
```

`kSoloInstance` sessions carry exactly one entry in `members`, forever — no join-by-a-second-account
is ever possible (RFC-022 §2.3's `resolve()` routes `kSoloInstance` through a per-player `allocate_new`
call, never a lookup). `kSharedPersistent` sessions (the overworld's own model, or a future persistent
map reached by a `Portal`) have no membership cap and no idle-teardown at all — they behave exactly
like the overworld does today, permanently `ACTIVE`, `members`/`present` unused.

**Membership cap.** `kInstanceMemberCap` **(tunable)**, default 4 for `kGroupInstance` dungeon/trial
content — GAME.md §3's own "hầm ngục thiết kế cho nhóm 2-4" language, recorded here as the default a
content author may override per portal (RFC-022 §2.3's table already marks rest-realm scope as
per-realm tunable; this RFC extends that same per-portal tunability to the cap). `resolve()`'s "full"
check (RFC-022 §2.3) is `members.size() >= kInstanceMemberCap`.

---

### 6. Entry, exit, and disconnect — three different events

This is the distinction the assignment calls out by name, and it matters because conflating any two of
them either breaks reconnection or creates a punitive-feeling ejection neither GAME.md §0 nor RFC-022
§Tone Guardrail Compliance would tolerate.

| Event | Trigger | `members` | `present` | Session effect |
|---|---|---|---|---|
| **Join** | `resolve()` (RFC-022 §2.3) routes a new account into an existing or freshly-`allocate_new`'d session | account added if absent | account added | `ACTIVE` (or stays `ACTIVE`) |
| **Deliberate exit** | player steps on the instance's `kReturnPortal` (RFC-022 §2.2) | **unchanged** — a player who leaves through the front door is still "of" this run | account removed | if `present` now empty → idle clock starts (§3.5) |
| **Disconnect** | the player's connection drops with no `kReturnPortal` use — a **new** disconnect/unbind message this RFC requires (none exists today: `player_actor.hpp`'s `account_ = 0` at its declaration is only the field's default initializer, never reset by any shipped handler) reverts the session slot to unbound while their last known `map`/`x`/`y` is left exactly where it was | **unchanged** | account removed | identical bookkeeping effect to deliberate exit — `present` shrinks, idle clock may start — but see below for the difference that actually matters |
| **Reconnect** | the same account logs back in through a **new "resume" path** this RFC requires in `World::login()`/`BindAccount`, distinct from the always-runs "fresh account" path shipped today | — | account **re-added** to `present` if their persisted `map` still equals `session.map_id` **and** the session has not reached `CLOSED` | the player materializes exactly where they logged out, inside the still-live instance, no re-resolution through a portal at all — **proposed by this RFC**, not existing behavior |

**Both the disconnect trigger and position-preserving reconnect are new mechanisms this RFC requires,
not existing shipped precedent.** Verified against source: `World::login()` (`world.hpp`) is the sole
caller of `BindAccount` and sends the identical fresh-account `BindAccount{spawn_tx, spawn_ty, wood=40,
stone=25, seed=12}` on **every** call, including the "already logged in — take the same slot back"
branch for a returning account — there is no conditional resume path today.
`handle(const BindAccount&)` (`player_actor.hpp`) unconditionally resets `x_`/`y_` to spawn, `hp_`/
`mana_`/`stamina_` to max, and starter items, on every invocation. No handler anywhere sets `account_`
back to a sentinel, and no client-facing disconnect/network layer exists in `src/` at all yet
(RFC-015's territory). If implemented against `BindAccount`/`login()` unmodified, a reconnecting
player would be teleported to the overworld spawn with reset resources — the opposite of what this
section describes. This RFC therefore requires two new pieces, named here as concrete engineering
tasks rather than assumed solved: (a) a disconnect/unbind message that reverts a `PlayerActor`'s
session slot to unbound without touching position, and (b) a `World::login()` branch — keyed on
whether the reconnecting account's persisted `map`/`x`/`y` point at a still-open session — that
resumes in place instead of invoking the existing fresh-account `BindAccount` path. Both are
requirements for whichever pass wires up the (currently entirely absent) client-facing
network/disconnect layer, not something this RFC claims already works.

**Why disconnect and deliberate exit share bookkeeping but not player-facing consequence.** Both
remove an account from `present` — that part is symmetric, because `InstanceManager`'s idle-teardown
logic (§3.5) genuinely cannot distinguish "chose to leave" from "connection dropped" by watching
`present` alone, and should not try to: a session with zero people physically standing in it is idle
by definition, regardless of why. What differs is what happens to that account's *position*. A
deliberate exit already relocated the player (the `kReturnPortal`'s `Teleport` moved them before this
table's row even applies). A disconnect, under the new mechanism above, leaves the player's persisted
position **exactly where they were** — inside the instance — the cozy, non-punitive policy this RFC
wants, not a description of anything already running. If the instance is still open (`ACTIVE` or
`IDLE`, not yet `TEARING_DOWN`) when they reconnect, they walk back into the exact fight they left. If
the grace window (§3.5) has already elapsed and the session has moved to `TEARING_DOWN`/`CLOSED` by
the time they reconnect, §6.1 below defines the fallback.

#### 6.1 Reconnecting into a session that has already closed

If a reconnecting account's persisted `map` points at a `MapId` whose `InstanceSession` is `CLOSED` (or
whose entry has been fully forgotten — see §3.6's "no single destroyed event" note, this can happen
purely from the chunk-level timers finishing without any explicit signal), the player cannot simply
"appear" there — there is nothing there anymore. This RFC's ruling: relocate them to
`session.return_map` / `return_x` / `return_y` (RFC-022 §2.2's `MapSession` fields, which every
instanced session carries from creation) — the exact overworld tile their group's portal use
originated from. This is a **location fallback only**, explicitly not a death or an item-loss event
(that is RFC-013's exclusive territory per RFC-022 §5.3's own boundary table) — the player simply wakes
up back where they started, the same non-event a graceful `kReturnPortal` exit would have been, just
triggered automatically instead of by the player's own action.

**Dependency this RFC flags, does not resolve.** The fallback above requires `return_map`/`return_x`/
`return_y` to survive independently of the (deliberately ephemeral, §3.6) `InstanceSession` record
itself — otherwise a leader restart or a long-disconnected player has nothing to fall back to at all.
Whether that means a copy of those three fields riding along in the player's own persisted record
(RFC-016's territory), or some other minimal persisted breadcrumb, is not decided here — flagged as a
concrete, named requirement for RFC-016 rather than assumed solved (Open Questions §2).

---

### 7. Per-realm atlas/pack loading

RFC-022 §2.4 hands this RFC "per-realm atlas load/unload on session start/end"; RFC-008 Q3 ("per-realm
pack layering") reserves the id range and format question but explicitly leaves override semantics
unresolved — this RFC does not touch either of those, only the **lifecycle** around them.

**Refcounted by pack, not by session.** Two different friend groups can simultaneously be inside two
different `MapId`s of the *same* dungeon template (e.g., two concurrent `kGroupInstance` runs of
"Sunken Crypt") — each gets its own `MapId` and its own `InstanceChunkBlock` (§4), but both read from
the identical pack/atlas data. Atlas load/unload is therefore keyed by **pack id** (RFC-008's
namespace), not by `MapId`:

```
on ALLOCATING:   pack_refcount[descriptor.pack_id] += 1; load the pack if this was 0 → 1
on CLOSED:       pack_refcount[descriptor.pack_id] -= 1; free the pack if this reached 1 → 0
```

This generalizes RFC-022 §5.4 point 1's "the source's [atlas] frees if no player still needs it" from
a single-instance framing to the correct one — "no *instance of this pack*" rather than "no player,"
which is what actually determines whether the data is still needed anywhere in the running world.

---

### 8. The 10×7 boss room: stays as-is, not migrating

The shipped dojo boss room (`kRoomW=10, kRoomH=7`, `tiles.hpp:860-861`; boss placement in
`world.hpp::build_bosses()`, `chunk_actor.hpp::add_boss()`) is a room inside the single persistent
`kInterior` map (`MapId = 1`) today — addressed via the room-index scheme (`room_block_x/y`,
`kRoomPitch = 16`), reached through the existing `Door{tile,room}` mechanism, which RFC-022 §2.1
already names as the special case of `Portal` (`kInteriorDoor`) whose target never needs allocating.

**This RFC's explicit ruling: it stays exactly as-is. It does not migrate to a `MapSession`, now or at
P8.** The reasoning:

- **It is not private-per-group content.** GAME.md §10's own framing for the dojo/võ đường is
  spectacle, not instanced PvE: "ghé làng thấy vệ binh tập ở sân sau; vào hầm ngục thấy quái đấu tập" —
  any player who walks up can watch, and RFC-007 §6.1 confirms the in-world dojo rendering is not
  load-bearing for training itself ("the visible in-world dojo sparring *renders* the training
  matches, it is not load-bearing for gradient quality"). `MapSession`/instancing exists specifically
  to solve "each group needs its own private copy" (GAME.md §3: "một bản riêng mỗi nhóm") — a problem
  the dojo room does not have. Migrating it would trade a mechanism that is already correct
  (`kInteriorDoor`, always-visible, shared) for one built for a different problem.
- **Nothing about the room's own lifecycle needs create-on-demand or teardown.** It is placed once,
  at world-gen time (`build_bosses()`), and lives exactly as long as its owning village does — which
  is to say, forever, matching every other persistent-band structure. There is no idle-teardown
  question here because there is no idle state to detect: the room is not entered-and-left the way a
  dungeon run is, it is walked past and glanced at.

**What this ruling does not decide.** A future P8 player-facing "real dungeon" — private per-group,
`quái đã luyện`, Tinh chất payout, GAME.md §3's actual "hầm ngục" — is a **separate, new** content type
that *does* need `MapSession`/`kGroupInstance` semantics, exactly the `Dungeon / trial instance` row
RFC-022 §1.3 already reserves. This RFC's ruling only closes the question of whether the *existing,
shipped* 10×7 spectacle room is that content — it is not, and building the real P8 dungeon means
authoring a new `MapDescriptor` row and reusing this RFC's `allocate_new()` machinery from scratch, not
retrofitting the dojo room into it.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001..010 (accepted combat set)** | Not load-bearing here, except RFC-007 §6.1, cited in §8 for the dojo-room ruling — the boss-room's *combat* mechanics are entirely RFC-001/005/007's; this RFC only rules on whether the room is an instance. |
| **RFC-007** (accepted) | §6.1's `CombatEnvironment`/dojo grounding is cited (§8) but not re-specified; this RFC does not touch RL training, checkpoints, or the room's combat rules. |
| **RFC-008** (accepted) | Q3 (per-realm pack layering) is cited, not resolved — this RFC only specifies the refcounted load/unload *lifecycle* (§7); the pack format, override semantics, and id-range details remain RFC-008's. |
| **RFC-013** (proposed, Vitals/Death/Recovery) | Owns what dying *inside* an instance costs (ejection, item loss per GAME.md §3's dungeon/mine row) — this RFC only supplies the instance to eject *from* and the `kReturnPortal`/return-coordinate data (§6) RFC-013's ejection logic would target. Death is never this RFC's mechanism for removing a player from a session; only exit and disconnect are (§6). |
| **RFC-015** (proposed, Client Replication) | Consumes this RFC's sparse chunk-addressing scheme (§4) as the shape it must extend to `effect_tick` and any other `kChunkCount`-sized structure it owns — this RFC fixes the in-memory shape, RFC-015 owns the wire encoding on top of it. |
| **RFC-016** (proposed, Persistence & Save-File Format) | Owns the actual save-before-teardown write (§3.6's `TEARING_DOWN` state names the hook, does not implement it) and the minimal per-player breadcrumb §6.1 flags as a dependency. This RFC also states the policy RFC-016 must build against: **instanced maps do not survive a leader restart** by default (§ below), consistent with RFC-022 §Interactions' own phrasing ("not persisted across a world restart unless RFC-014 says otherwise") — this RFC does not say otherwise. |
| **RFC-019** (accepted) | Not load-bearing — XP/skill consequences of anything that happens inside an instance are entirely RFC-019's, unaffected by session lifecycle. |
| **RFC-020** (accepted) | `rumor` quests (RFC-020's Q6/§293) point at a dungeon-gate `kMissionPortal` and expect "the dungeon gate instance the rumor pointed at is torn down per RFC-014" on quest conclusion — this RFC's `TEARING_DOWN` transition (§3.6) is that teardown; RFC-020 triggers it by simply letting the session go idle (no player return trigger needed) the same as any other session, no special-case quest-driven teardown call is introduced. |
| **RFC-022** (accepted-with-revisions) | This RFC implements exactly what RFC-022 §2.4 named as out of its scope; every data shape used here (`MapId`, `MapDescriptor`, `Portal`, `MapSession`, `resolve()`) is RFC-022's, cited not reproduced. |

---

## Multiplayer & Simulation-LOD Considerations

- **`Require<Trusted>` co-location, unchanged pattern.** `InstanceManager` sits on the leader
  alongside `MapDirector` and `PlayerActor` for the identical reason RFC-022 §Multiplayer already
  argues for `resolve()` — allocation decisions are leader-observed facts (who is present, which
  sessions are open), never locally computed. No new trust boundary is introduced; this RFC only adds
  a second `Require<Trusted>` actor to a pattern the codebase already has two examples of.
- **Chunk-actor placement for instanced maps is unconstrained**, exactly like the overworld's
  `Placement<HashById>` (`chunk_actor.hpp`'s own comment: "a chunk may be hosted on a player's
  machine... safe by selection, not by hope"). Nothing about an instance being private-per-group
  changes its trust tier — a dungeon chunk actor is still Trust Tier B, still replayable from
  `(chunk key, tick)`, still safe to place on a player's own machine once cross-node placement exists
  (P6). This RFC does not special-case instanced chunks for trust.
- **LOD (10/1/0 Hz) is unaffected and reused as-is** for chunks *within* a live instance (§3.3) — the
  new mechanism this RFC adds (§3.5's two-timer teardown) only governs whether the instance's chunk
  actors exist at all, a layer above LOD, not a replacement for it.
- **`declare_lazy<ChunkActor>` costs nothing on the persistent band's hot path.** `router.get<A>()`'s
  existing fast path (an eagerly-registered actor's `by_id_` hit) is untouched by declaring the type
  lazy — the lazy id-table is only consulted on a miss (ADR-028 Phase 4), which never happens for the
  1024+1024 chunks `build_chunks()` still registers cold. This RFC adds a new *code path*, not new
  *load*, to every message the overworld already sends.
- **20–50 concurrent players, no VPS.** `kMaxConcurrentInstances` **(tunable)**, default 64 — a safety
  valve `InstanceManager` enforces at `allocate_new()`, expected to never actually trigger at this
  project's target scale (even 50 players each in their own private `kSoloInstance` is 50, well under
  64) and included purely against a bug (e.g. a leaked, never-torn-down session) rather than a real
  resource constraint. If it is ever hit, the response is a plain, one-time "the world is busy, try
  again in a moment" message — never a queue position, a wait-time estimate, or anything with a
  countdown shape (§ Tone Guardrail Compliance).
- **Leader-death handling.** ARCHITECTURE.md §2's existing policy — "leader chết thì sao: thế giới
  dừng" (the leader dying stops the world; there is no auto-failover, recovery is a manual restart from
  the last periodic/on-exit save) — applies to this RFC's state unmodified: `InstanceManager`'s entire
  `sessions_` table is in-memory, leader-resident, and **is not part of the portable save file**
  (unless RFC-016 later decides otherwise for some specific persistent-band case, which this RFC does
  not anticipate). On restart, every open instance is simply gone — no `MapId` in the instanced band
  survives a restart, matching RFC-022 §Interactions' own default. A player whose last persisted
  position was inside one lands via the §6.1 fallback (return-portal coordinates, if RFC-016 has
  preserved them) or, failing that, the same hearth-respawn fallback RFC-013 defines for any other
  "no sane position to resume at" case — this RFC does not invent a second fallback path, it reuses
  whichever one RFC-013 already specifies for an equivalent situation.
- **Determinism.** Priming (§3.3) for a hand-authored instance (rest realms, dungeon room layouts —
  RFC-022 §1.3) reads static pack data, not seed-derived placement, so it "trivially satisfies" the
  cross-platform bit-exactness bar RFC-022 §Multiplayer already argues for the same reason. This RFC
  introduces no new seed-derived computation of its own — `MapId` allocation (§3.1) is a leader-local
  counter, not something every node must independently derive and agree on.

---

## Tone Guardrail Compliance

Walking every mechanic this RFC introduces against GAME.md §0's test:

1. **Instance spin-up and priming (§3).** Instantaneous from the player's perspective — a portal
   resolves, a short transition plays, the map is ready. Nothing here is timed in a way the player
   perceives; "priming" is a server-side implementation detail with no player-visible duration beyond
   the transition RFC-022 §5.4 already describes.

2. **Idle-teardown (§3.5).** The two timers (`kInstanceIdleGraceMs`, `kInstanceChunkIdleTimeoutMs`)
   are **never displayed**, to anyone, under any circumstance. There is no "this instance closes in
   4:32" HUD element, no warning toast, no countdown of any kind — a session simply stops existing
   after everyone has been gone long enough, silently, exactly the shape RFC-022 §Tone Guardrail
   Compliance point 4 already committed this RFC to before it was written: "an idle-timeout policy
   that is silent... is consistent with the 'chững, không tụt' pattern... a *displayed* countdown
   inside an instance would not be." This RFC's own data shapes (§3.6, §5) contain no field that could
   carry a player-facing countdown — the same structural argument RFC-022 made about its own contract.

3. **Disconnect handling (§6).** A disconnected player loses nothing — their position is preserved
   exactly, their reconnection window is generous (the same five-minute grace the whole session gets,
   not a shorter, punitive one), and even a fully-expired reconnection (§6.1) costs them a location
   relocation, never an item, never a status effect, never a "you were kicked" message framed as
   failure. Disconnecting is not distinguished from any other reason a player might step away — the
   game does not ask why they left.

4. **Membership cap and instance-full behavior (§5).** A full `kGroupInstance` session simply cannot
   be joined by a stranger — this is a capacity fact stated once, at the moment of trying, never a
   queue, a wait-list, or anything with elapsed-time semantics.

5. **The safety-valve cap (`kMaxConcurrentInstances`, § Multiplayer).** Framed explicitly as a bug
   guard, not a resource-scarcity mechanic — and its one-time message is the same "try again" framing
   GAME.md §0 already tolerates elsewhere (a portal is always there; nothing about a momentary
   unavailability implies loss).

6. **The boss-room ruling (§8).** Changes nothing about the shipped dojo room's player-facing
   behavior — it remains exactly as visible, exactly as unlimited-access as it is today.

No mechanic in this RFC creates a deadline the player watches, a decaying resource, a login-frequency
dependency, or a countdown of any kind visible from inside the game.

---

## Open Questions

1. **`MapId` band exhaustion (§3.1).** Wraparound behavior once all 65,520 instanced ids have been
   used within one leader uptime is explicitly undesigned — not expected to matter at this project's
   scale (see §3.1's own arithmetic), but flagged so it is not silently assumed impossible if a much
   longer-running or much higher-churn deployment ever exists.

2. **The minimal per-player return-location breadcrumb (§6.1).** This RFC names the requirement
   (survive independently of the ephemeral `InstanceSession` record) but does not specify where it
   lives — a field on the player's own persisted record, or something else. RFC-016's to decide.

3. **`ChunkActor`'s lazy-construction wiring path (§3.2).** Two options sketched, neither chosen. A
   short engineering spike against the actual `ActivationBroker`/`wire()` mechanics should settle this
   before implementation begins — this RFC deliberately does not pretend to have already prototyped
   it.

4. ~~Whether `MapDirector::chunks` mutation (§3.4) should go through a message or a direct call.~~
   **Resolved during review:** it must be a message (`FanOutAdd`/`FanOutRemove`, §3.4) — a direct call
   into `MapDirector::chunks` (a plain, non-atomic `std::vector` `MapDirector`'s own `DirectorTick`
   handler concurrently iterates) is an unsynchronized data race, not a style choice; `Require<Trusted>`
   co-location guarantees shared trust, not a shared shard or thread (`shard_of(ActorId)`, `engine.hpp`).

5. **Per-portal `kInstanceMemberCap` override mechanism (§5).** This RFC states the default (4) and
   says a content author "may override per portal" but does not specify where that override value is
   stored — a new `Portal` field (which would require reopening RFC-022's struct, out of this RFC's
   authority) or a side table keyed by `PortalId`. Left for whichever RFC or implementation pass
   actually authors the first non-default-cap portal.

6. **`kInstanceIdleGraceMs` vs. `kInstanceChunkIdleTimeoutMs` tuning relationship.** The two are set
   independently here (5 min / 30 s) on the reasoning that the second only needs to be "short enough
   to not linger once traffic is already cut," but no playtesting has informed either number — both
   are first guesses in the same spirit as RFC-021's jitter-cell numbers, flagged as needing a real
   tuning pass once instances actually exist to observe.

7. **`fan_beacons()`'s `kMapChunks` clamp (§3.4).** `MapDirector::fan_beacons()` bounds beacon fan-out
   to `[0, kMapChunks)` (32) regardless of a target instanced map's real `chunk_edge`, which can lazily
   activate phantom `ChunkActor`s just outside a small instance's real footprint. Self-healing via
   §3.5's own `IdleTimeout` (no further traffic ever reaches a phantom actor), but not fixed by this
   RFC — flagged for whoever next touches `fan_beacons()`.

8. **Priming completion detection (§3.3/§3.6).** §3.3's priming fan-out is described as a fire-and-forget
   `tell()`, but `resolve()` must not hand out a `Teleport` target before every `chunk_edge²` actor has
   actually finished construction. The working assumption is `ask<PrimeAck>(...)` with `InstanceManager`
   awaiting all replies before leaving `ALLOCATING`, but this RFC does not commit to it as final —
   flagged as a concrete engineering decision for whoever implements §3.3.

---

## Non-goals

- **`Portal`/`PortalKind`/`SessionScope`/`MapSession`'s shape.** RFC-022's, accepted, cited not
  reproduced or modified anywhere in this RFC.
- **The save format used when an instance's state is persisted on teardown.** RFC-016's (proposed,
  drafted after this RFC). This RFC only names the hook (`TEARING_DOWN`, §3.6) and states the default
  policy that most instanced maps are not persisted at all (§ Multiplayer).
- **The replication wire format for instance state, `Portal`, or `MapSession`.** RFC-015's (proposed,
  drafted after this RFC). This RFC only fixes the in-memory sparse-addressing shape (§4) RFC-015 must
  build its wire format on top of.
- **Leader election or any automated failover.** ARCHITECTURE.md §2 explicitly defers this
  indefinitely ("Bầu leader tự động là chuyện của sau này, nếu có bao giờ cần"); this RFC's
  leader-death handling (§ Multiplayer) states only what happens to *this RFC's* state when the
  single leader restarts manually, not any election mechanism.
- **Death, respawn, and ejection-with-item-loss rules at any instance endpoint.** RFC-013's (proposed).
  This RFC's §6.1 fallback is a location relocation only, explicitly not a death consequence.
- **Per-realm pack format, override semantics, or id-range details.** RFC-008's (accepted, Q3
  explicitly unresolved there). This RFC only specifies the refcounted load/unload lifecycle (§7).
- **Party/group formation, membership rules, or any player-facing "group" concept beyond the opaque
  `GroupId` comparison key RFC-022 already defines.** Unowned by any RFC in this batch (RFC-022 Open
  Question 8); this RFC's §5 membership bookkeeping is deliberately independent of how a group forms.
- **Mine-depth material-tier access, dungeon content authoring, or loot/reward tables.** RFC-018's
  (proposed) and RFC-023's (accepted, population) territory; this RFC places and tears down the empty
  instance, never its contents.
- **RL training, checkpoints, or dojo/võ đường combat mechanics.** RFC-007's (accepted); §8's ruling
  only concerns whether the shipped boss room is a `MapSession`, not anything about how it fights.

---

## Review Record

Reviewer A: revise. Reviewer B: revise. Both converged after independent engine-source verification;
revised and accepted.

Applied (both reviewers upheld):
- §6: rewrote Reconnect/Disconnect rows + prose — false "shipped precedent" claim removed; named as new `World::login()` resume path + new disconnect/unbind message this RFC requires.
- §3.4: `MapDirector::chunks` mutation changed from a raw direct call to `FanOutAdd`/`FanOutRemove` messages; Open Question 4 marked resolved.
- §3.5: named the actual mechanism (`register_actor<A>()`'s hardcoded `idle_ticks=0` vs. `declare_lazy`+broker resolving `IdleTimeout<Ms>`) and the required `ChunkActor` policy-pack change.

Applied (single-reviewer, proof verified sound):
- Terminology: "giảng đường" → "võ đường" (3 occurrences; GAME.md's actual term).
- §4.3: added `World::build_bosses()`/`World::chunks_` as a third `chunk_index()` consumer, corrected overclaim.
- §3.4: added named gap for `fan_beacons()`'s `kMapChunks` clamp vs. real `chunk_edge`; new Open Question 7.
- §3.3/§3.6: added priming fire-and-forget completion-detection gap; new Open Question 8.

Unresolved: none — both reviewers' mustFix items were applied; remaining items (Open Questions 1,2,3,5,6,7,8) were already correctly scoped as open by the original draft and left as such.

Status: Accepted (revised after review).
