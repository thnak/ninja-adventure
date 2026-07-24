# RFC-015: Client Replication & Interest-Set Protocol

> Status: **Accepted (revised after review)**
> Design canon: [GAME.md §11](../GAME.md) (20–50 concurrent players, PvP off by default, the
> "50–150ms latency giữa máy nhà" line) · [ARCHITECTURE.md §2b](../ARCHITECTURE.md) (the beacon/
> interest-set decision this RFC's client-facing half extends), [§4](../ARCHITECTURE.md) (the
> proposed, unimplemented 10/1/0 Hz simulation-LOD sketch — RFC-010 §9 OQ4 leaves its ownership
> open, RFC-014 explicitly does not build it — this RFC checks its own §4 against the shipped
> `kIdlePublish` mechanism instead), [§9](../ARCHITECTURE.md)
> (latency as a named risk) · [ROADMAP.md](../ROADMAP.md) P6 ("Interest set: client đăng ký chunk
> quanh mình, không nhận cả bản đồ" — the exact deliverable this RFC specifies ahead of P6)
> As-built source grounding: `src/world/protocol.hpp` (`PlayerBeacon`, `kBeaconPeriod=3`,
> `kBeaconLease=12`, `Teleport`), `src/world/map_director.hpp` (`fan_beacons()` — the shipped 5×5-chunk,
> `kSpan=2` fan-out window, its own comment: "5x5 CHUNKS, not 3x3... covers what the client can SEE"),
> `src/world/chunk_actor.hpp` (`handle(const PlayerBeacon&)` upsert at :154-155, `players_` roster;
> `kIdlePublish=32` at :107,119,134 — the *actually shipped* publish-cadence mechanism: full-rate
> publish whenever `players_` is non-empty, once per 32 ticks otherwise — the ground truth §4 checks
> its send-cadence reasoning against, not the unimplemented 10/1/0 Hz sketch),
> `src/world/snapshot.hpp` (`SnapshotBus`, `ChunkView{coord,tick,world_ms,terrain,creatures,shots,
> effects,zones,crops,buildings}`, `PlayerBus`, `PlayerView` — the full field list this RFC's wire
> projections narrow), `src/client_main.cpp:490,656` (`effect_tick`, a `kChunkCount`-sized
> per-chunk audio-dedup vector — one of the two structures RFC-014 §4 names as needing its sparse
> scheme), `src/world/tiles.hpp` (`Creature`, `Projectile`, `Effect`, `Zone`, `Crop`, `Building` —
> the full in-process records `ChunkView` embeds today; `kTicksPerSecond=10`, `kChunkTiles=32`,
> `kMapChunks=32`, `kChunkCount`, `chunk_index()`, `Door`), `src/world/account.hpp`,
> `src/world/player_actor.hpp` (session-slot binding) · `src/world/tiles.hpp:51` (`kMaxPlayers=8` —
> today's cold-registration ceiling, below this RFC's 20–50-player bandwidth target; see §Multiplayer)
> — confirmed: **no
> client-facing wire/network protocol exists in `src/` today.** `protocol.hpp` defines only
> in-process actor messages (`Tick`, `CreatureEnter`, `MoveIntent`, `HurtPlayer`, `Teleport`,
> `PlanAttack`/`AttackPlan`, `UseAbility`/`AbilityPlan`, `GetPlayer`, …) exchanged between actors on
> possibly-different machines via Quark's own transport, never a client. The renderer today reads
> `SnapshotBus`/`PlayerBus` **directly, in-process, on the same machine** (`client_main.cpp`) — there
> is no serialization step to study, only the in-process shapes this RFC must design a wire form for.
> Depends on: RFC-014 (Accepted, Instance & Realm Lifecycle — §4's sparse chunk-addressing scheme,
> cited not re-derived, adopted here for every wire-side chunk-keyed structure), RFC-022
> (accepted-with-revisions, Map System — `Portal`/`MapSession` data shapes this RFC's transition
> messages carry, cited not reproduced), RFC-021 (accepted-with-revisions, partially superseded by
> RFC-022 — §5.4's illustrative Map-marker budget contract, which this RFC satisfies), RFC-010
> (accepted-with-revisions, Battlefield Simulation — §4.5's `ChunkView` replication table and Budget
> B-1, extended here to the wire; Open Question 5 answered), RFC-004 (accepted-with-revisions,
> Terrain & Combat Entity — §10's `PublishedEntity`/`PublishedScar` packed-projection pattern, the
> precedent this RFC generalizes to `Creature`/`Projectile`/`Effect`), RFC-006
> (accepted-with-revisions — §1.3's danger-tier minimum lead times, the numbers this RFC's latency
> budget is checked against)
> Depended on by: RFC-011 (proposed, Combat HUD — needs a defined client-side data source before it
> can specify what the HUD reads), RFC-013 (proposed, Vitals/Death/Recovery — death/respawn
> visibility on the wire), RFC-016 (proposed, Persistence — orthogonal but shares the "what survives
> a reconnect" question at the edges, cited where relevant)

---

## Summary

Every accepted RFC in this project's set has been budgeting against a client-facing network
protocol that has never been specified. This RFC is that protocol's first specification. It is
**entirely green-field** — nothing named "wire format," "client packet," or "network delta" exists
in `src/` today, and this RFC says so at every turn rather than implying otherwise.

Three things this RFC does, precisely, and nothing more:

1. **Defines the interest set a client actually receives**, by re-using — not reinventing — the
   exact 5×5-chunk beacon fan-out window `map_director.hpp::fan_beacons()` already computes for a
   completely different reason (telling nearby chunks a player exists). That window's own shipped
   comment already states why it is 5×5 and not 3×3: "the radius has to cover what the client can
   SEE." This RFC's contribution is recognizing that the server-side chunk-wake window and the
   client-facing subscription window are, and should stay, the *same* window, computed once, not two
   independently-tuned radii that can drift apart.
2. **Defines a wire projection and a delta-encoding scheme** for the combat-relevant record families
   without one yet — `Creature`, `Projectile`, `Effect` (all three embedded in `ChunkView`), plus
   `PlayerView` (a sibling bus, not a `ChunkView` field) — following the precedent RFC-004 §10 already
   set for `CombatEntity` (`PublishedEntity`, 28 B; `PublishedScar`, 8 B) — because today's `ChunkView`
   embeds the *full* in-process `Creature` struct (wander state, grudge counters, windup target key —
   fields no client rendering ever reads) at zero cost in-process, and zero cost is not what shipping
   that struct over a socket to 20–50 clients, several times a second, would actually be.
   `ChunkView`'s other three vector fields — `zones`, `crops`, `buildings` — are **explicitly not**
   given a wire projection by this RFC (Non-goals, §2.1); this RFC narrows its scope to the
   combat/creature-facing subset, not all of `ChunkView`.
3. **States a concrete per-tick, per-view byte budget** derived from those wire projections and the
   5×5-chunk interest set, answering RFC-010's Open Question 5 ("does the budget need halving once
   views stream over TCP, or does interest-set delta encoding absorb it?") directly: delta encoding
   absorbs it, and this RFC shows the arithmetic rather than asserting it.

A fourth thing this RFC does not do, stated up front because it is easy to conflate: it does **not**
set the simulation's own tick rate or decide when a chunk sleeps (ARCHITECTURE.md §4's 10/1/0 Hz LOD
sketch, RFC-014's territory) — it only decides, given whatever the simulation is already doing, how
often and in what shape that state reaches a client's screen. The two are related but distinct
questions, and every section below says which one it is answering.

---

## Motivation

Three concrete gaps, each verified against source, not assumed from prose:

1. **Every accepted RFC in this series has a `RFC-015 owns this` placeholder, and nothing has filled
   it in.** RFC-010 §4.5 caps battlefield state at "≤ 1 KB per view" and Q5 explicitly asks whether
   that number survives a network hop unmodified. RFC-021 §5.4 states an "illustrative... budget"
   for Map markers and says "RFC-015 owns the actual encoding." RFC-004 §10 built a packed
   `PublishedEntity`/`PublishedScar` shape and left `Creature`/`Projectile`/`Effect` unaddressed.
   RFC-014 §4 fixed the in-memory sparse-addressing shape for `SnapshotBus` and named `effect_tick`
   as a sibling structure needing the identical scheme, and handed the *wire* shape built on top of
   it to this RFC by name. None of these are speculative dependencies — they are five different
   specs independently arriving at the same missing piece.

2. **The renderer today reads shared memory, not a socket, and that fact is currently load-bearing
   in a way nobody has stated plainly.** `client_main.cpp` calls `SnapshotBus::load(ChunkCoord)` and
   `PlayerBus::load(int)` directly — the "client" and "server" are the same process, the same
   address space, today. `ARCHITECTURE.md §10`'s own technical-debt list names this explicitly: "một
   người chơi. `PlayerActor` đang là singleton... multiplayer cần... một *interest set* không gian để
   client chỉ nhận chunk quanh mình" (a single player; multiplayer needs a spatial interest set so
   the client receives only the chunks around it). P6 in ROADMAP.md is the phase that turns this into
   a real cross-machine channel. This RFC exists so P6 has a contract to implement against instead of
   inventing one under deadline.

3. **The "50–150 ms home-cluster latency" line has been cited as a design constraint by three
   different documents — GAME.md §11 (justifying PvP-off), ARCHITECTURE.md §9's risk table, and this
   run's grounding brief — and none of them turn it into a rule any other RFC can check its own
   numbers against.** RFC-006 §1.3 sets telegraph minimum lead times (5/8/12/16 ticks at 10 Hz —
   500/800/1200/1600 ms) without stating whether those survive replication latency on top of the
   sim-side wind-up, and RFC-010 §4.5 states "latency 50–150 ms is acceptable for a 4 s burn patch;
   it is not a PvP game" as an aside, not a checked bound. This RFC turns the aside into arithmetic.

---

## Guide-level Explanation

### For a player

Nothing about how the game *feels* changes because of this RFC — it describes plumbing, not
gameplay. What it guarantees, invisibly: you see the chunks around you (roughly a screen-and-a-half
in every direction, the same window that already decides whether a chunk near you is awake), you see
other players and creatures update smoothly without your screen needing to know about the entire
1024×1024 world, and the Map screen's markers (villages, forts, gates) arrive as a separate trickle
that never competes for bandwidth with the fight you are currently in.

### For a designer

Nothing you author is affected directly. If you are the kind of designer who *also* touches
telegraph timing (RFC-006), the one number worth knowing is that this RFC's latency budget confirms
every existing telegraph tier already clears the worst-case network delay with margin — you do not
need to pad wind-ups for network reasons on top of what RFC-006 already requires for readability.

### For an engineer implementing P6

You need, in dependency order: (1) the interest-set definition (§1) — reusing the shipped 5×5
window, not inventing a second one; (2) the wire projections (§2) for every `ChunkView`/`PlayerView`
record family, following RFC-004's established packed-projection pattern for the three families that
don't have one yet; (3) the delta-encoding scheme (§3) — baseline-on-subscribe, id-keyed diffs
thereafter; (4) the send-cadence policy (§4), which throttles *below* whatever rate the sim is
already publishing at (`kIdlePublish` today), never above it; (5) the per-view byte budget (§5) and the arithmetic
that justifies it; (6) the separate, low-frequency Map-marker channel (§6) satisfying RFC-021 §5.4;
(7) the instance-crossing subscription swap (§7), built on RFC-014's `Teleport`-driven map change;
(8) the latency-as-constraint table (§8) your combat-adjacent RFCs can check future numbers against.

---

## Reference-level Design

### 1. The interest set: one window, reused, not reinvented

`map_director.hpp::fan_beacons()` already computes, every `kBeaconPeriod` (3) ticks, a 5×5-chunk
window (`kSpan = 2`) centered on each player's chunk — 160×160 tiles — and its own comment already
states the reason for that specific size: "the radius has to cover what the client can SEE... 5×5 is
comfortably more than a screen at minimum zoom." That window currently exists for a server-internal
purpose (telling nearby chunks a player is present, so they know they may publish and so hostile
creatures can find a target) — it was never designed as a client subscription boundary, but it
already *is* one in every way that matters: it is sized for visibility, computed once per player per
beacon period, and touches exactly the chunk set a client rendering that player's viewport would ever
need data from.

**This RFC's ruling: the client interest set *is* the beacon fan-out window, not a second,
independently-tuned radius.** Concretely:

```
ClientInterestSet(player) = { ChunkCoord c : |c.cx - home.cx| ≤ 2 and |c.cy - home.cy| ≤ 2
                               and c.map == player.map }
```

— identical to `fan_beacons()`'s own loop bounds (`map_director.hpp`), recomputed at the same
`kBeaconPeriod` cadence the beacon itself already uses, because a client's subscription set changing
in between beacon periods buys nothing: the beacon roster the destination chunks hold is only as
fresh as that same period anyway.

**Why not a wider or narrower window for replication specifically.** A wider replication-only window
would mean streaming data for chunks the beacon system itself doesn't consider "near" a player —
paying bandwidth for chunks that may not even be actively ticking as far as the player's own
presence is concerned. A narrower one would mean the client sees creatures pop into existence at the
edge of its own screen, because the chunk supplying them was outside the subscription boundary while
inside the visible one. Reusing the exact fan-out window sidesteps both failure modes for free —
this is the single largest simplification this RFC makes, and it costs nothing to state because the
window already exists for the right reason.

**One clarification this reuse requires, stated so it isn't silently assumed:** `fan_beacons()`
today sends the *beacon* (a `PlayerBeacon` — the player's own position, one small struct) to every
chunk in the window; it does not, today, cause those chunks to stream anything back to the client.
This RFC's contribution is the other half of that relationship — a chunk in a player's interest set
now also has a client subscriber, and §2–§4 below define what that subscriber receives.

**A boundary drawn on purpose.** `fan_beacons()`'s clamp `if (cx < 0 || cy < 0 || cx >= kMapChunks
|| cy >= kMapChunks) continue;` — bounded to the persistent overworld's `kMapChunks` (32) — is a
pre-existing gap RFC-014 §3.4 already names against instanced maps (a small instance's beacon
fan-out can lazily wake phantom chunk actors just outside its real `chunk_edge` footprint). This
RFC's `ClientInterestSet` inherits that same clamp behavior by construction (it computes bounds the
identical way) and therefore inherits the identical self-healing property RFC-014 §3.4 already
argues for the sim-side beacon: a subscription request for a chunk outside an instance's real
footprint resolves to a chunk that receives no simulation traffic either, so it publishes nothing new
to subscribe to. This RFC does not fix `fan_beacons()`'s clamp — RFC-014 Open Question 7 already owns
that — it only confirms the replication side inherits the same (already-argued-safe) gap rather than
introducing a second one.

### 2. Wire projections: extending RFC-004's packed-projection pattern

#### 2.1 The gap this section closes — and the one it deliberately does not

**Scope, stated precisely.** This section defines wire projections for `Creature`, `Projectile`, and
`Effect` — the three `ChunkView` vector fields with no packed-projection shape yet — plus `PlayerView`
(§2.5). It does **not** define one for `ChunkView.zones`, `ChunkView.crops`, or `ChunkView.buildings`,
shown below and struck through in the field-by-field reasoning that follows: those three are a farm/
economy replication problem, not a combat one, and no RFC in this project's set — this one included —
currently charters farm/building-economy systems as a whole (RFC-019 §Non-goals names the same gap
for the *authoring* side: "no RFC in this batch fully charters the building/crop economy"). Speccing
a wire format for a system with no owning design RFC yet would mean inventing both at once, which
this RFC declines to do (Non-goals, below; Open Questions §7).

`snapshot.hpp`'s `ChunkView` today is:

```cpp
struct ChunkView {
    ChunkCoord coord{};
    std::uint64_t tick = 0;
    std::int64_t world_ms = 0;
    std::uint8_t terrain[kChunkTiles * kChunkTiles] = {};   // 1024 B, kChunkTiles=32
    std::vector<Creature> creatures;      // full in-process record, tiles.hpp
    std::vector<Projectile> shots;        // full in-process record
    std::vector<Effect> effects;          // full in-process record
    std::vector<Zone> zones;              // OUT OF SCOPE — no wire projection defined by this RFC
    std::vector<Crop> crops;              // OUT OF SCOPE — no wire projection defined by this RFC
    std::vector<Building> buildings;      // OUT OF SCOPE — no wire projection defined by this RFC
};
```

`Creature`, `Projectile`, and `Effect` are given wire projections below (§2.2–§2.4). `Zone`, `Crop`,
and `Building` are not — see the scope note above.

Every vector here holds the **exact same struct the simulation itself operates on** — `Creature`
carries `wander_cd`, `wander_dx/dy`, `home_tx/ty`, `grudge`, `windup_target` (a full player key,
8 bytes), and more: fields a client rendering that creature never reads, present because copying the
whole struct into the view was free in-process (same address space, `std::vector` copy, no
serialization). RFC-004 §10 already hit this exact problem for `CombatEntity` and solved it with a
packed **published projection** — a distinct, smaller struct carrying only what a consumer needs,
explicitly *not* the in-memory record:

> "The in-memory record (~48 bytes with alignment) is *not* the wire shape; the view carries only
> what a consumer needs" — `PublishedEntity` (28 B, packed), `PublishedScar` (8 B, packed).

**This RFC generalizes that exact pattern to the three record families RFC-004 didn't need to touch**
— `Creature`, `Projectile`, `Effect` — plus `PlayerView`, which has the same problem (it carries
`items[kItemKinds]`, all four skill arrays, and ability cooldowns a *remote* player's client rarely
needs at full fidelity, though the owning client needs all of it for itself).

**Positions stay plain floats, deliberately not fixed-point-quantized — a boundary distinction worth
stating explicitly.** RFC-010 §5's "quantise at seams" rule governs anything crossing into an RL
observation, a checkpoint, or a **cross-node determinism comparison** — boundaries where two
different machines must independently compute the identical bit pattern. A wire replication packet
crosses no such boundary: §9 already establishes position updates as best-effort, latest-wins, and
nothing on the client ever needs to reproduce a server computation bit-for-bit from a replicated
position, only to draw it. Quantizing here would trade a real bug surface (choosing a fixed-point
range that must cover every legal coordinate — chunk-local for a `Creature`, but map-global up to
1024 tiles for a `PlayerView`, two different ranges that are easy to conflate) for a savings of at
most 2–3 bytes per record, not worth the risk. Every wire projection below uses the same `float x, y`
representation the in-process records already use.

#### 2.2 `PublishedCreature` (proposed, 24 B packed)

```cpp
struct PublishedCreature {          // 24 bytes, packed — wire shape, NOT tiles.hpp::Creature
    std::uint32_t id;               // continuity across deltas (renderer/audio dedup key)
    float         x, y;             // map-global tile position, same representation as tiles.hpp::Creature
    std::int16_t  hp, max_hp;       // renderer needs both for a health bar fraction
    std::uint8_t  kind;             // CreatureKind
    std::uint8_t  facing;           // Facing
    std::uint8_t  status;           // Status — the one-slot elemental state (RFC-002)
    std::uint8_t  windup;           // >0 while telegraphing (F2) — the ONLY attack-read signal
                                     // a walk-only monster has; must reach the client every tick
                                     // it's nonzero, never coalesced away (§4)
    std::uint8_t  boss_pose;        // BossPose, 0 (kIdle) for all non-boss creatures
    std::uint8_t  disposition;      // Disposition — the player-visible "is this thing calm" state
                                     // (GAME.md §5's "vạch đỏ nhỏ trên đầu" anger indicator reads this)
    std::uint16_t _pad = 0;
};
```

**Fields deliberately dropped, and why each is safe to drop:** `attack_cd` (server-authoritative
cooldown gating, never rendered — the client infers "can attack" from `windup` alone), `anger_ticks`/
`grudge` (drive `disposition`'s *transitions* server-side; the client only needs the resulting state,
not the countdown that produced it — dropping the countdown is also what keeps this consistent with
GAME.md §0's "nothing counts down where the player can see it" even by accident), `target`/
`windup_target` (an 8-byte player key with no rendering use — the client already knows who *it* is
being targeted by via `windup_x/y`, kept below), `home_tx/ty`, `wander_cd`, `wander_dx/dy` (pure
wildlife-AI internals, zero rendering use).

**One field kept that needs a note: the windup-commit geometry (`windup_x/y` in the full record).**
The full in-process record carries these as `float` (where the targeted player stood at commit —
"the spot a whiff slashes"). This RFC keeps that geometry on the wire but **not** as fields on
`PublishedCreature` itself — `windup` (`uint8`, above) is the only windup-related field that struct
carries, sent every tick it is nonzero (§4's hard exception). The target spot is instead carried by a
small, separate wire record, `WindupCommit` (§3.2), sent exactly once, the tick a windup starts —
once committed, RFC-006 T2 already guarantees the geometry is frozen, so re-sending an unchanging
value every subsequent tick would be pure waste; the client latches `WindupCommit`'s `(x, y)` once,
keyed by `creature_id`, and holds it through the windup's lifetime.

#### 2.3 `PublishedProjectile` (proposed, 24 B packed)

```cpp
struct PublishedProjectile {        // 24 bytes, packed
    std::uint32_t id;
    float         x, y;
    float         vx, vy;           // tiles/sec — lets the client extrapolate motion between
                                     // updates without a full record (§4)
    std::uint8_t  element;          // Element — selects the trail FX
    std::uint8_t  _pad[3] = {};
};
```

Dropped: `damage` (never rendered — RFC-009's damage formula is server-only; the client learns the
outcome from the `Effect` the impact produces, not from the projectile in flight), `life` (ticks
remaining is a server bookkeeping field; the client instead removes a projectile it stops receiving
updates for — an explicit remove event, §3.3, not an inferred timeout, because inferring from silence
risks a false despawn under one dropped packet), `owner` (kill-credit bookkeeping, RFC-019's
territory, never rendered to anyone but the shooter, who already knows).

#### 2.4 `PublishedEffect` (proposed, 12 B packed)

```cpp
struct PublishedEffect {            // 12 bytes, packed
    float         x, y;
    std::uint8_t  kind;             // EffectKind
    std::uint8_t  age;              // renderer maps age → strip frame, exactly as it does in-process
    std::uint16_t _pad = 0;
};
```

This is, byte-for-byte, the same size as the existing in-process `tiles.hpp::Effect` record —
`tiles.hpp`'s own comment already calls it "a handful of 12-byte records per chunk per tick, all of
them gone within a second." No field is dropped and no field is added; this RFC's only contribution
for `Effect` is naming it as part of the wire contract rather than inventing a change — it was
already the cheapest, most wire-ready record family in the system.

#### 2.5 `PublishedPlayer` (proposed, two tiers: 28 B packed for remote players, full `PlayerView` for self)

Unlike creatures, a `PlayerView` genuinely needs two different projections depending on *whose*
client is reading it — the owning client needs everything (its own inventory, skill XP, ability
cooldowns, to drive its own HUD without asking); every other client watching that player needs only
enough to render them.

```cpp
struct PublishedPlayerRemote {      // 28 bytes, packed — what OTHER clients receive about a player.
                                     // Genuinely packed, like every sibling struct in this section:
                                     // sizeof == sum of fields below, no implicit alignment padding.
    std::uint64_t id;               // 8
    float         x, y;             // 8
    std::int16_t  hp, max_hp;       // 4
    std::uint8_t  facing;           // 1
    std::uint8_t  mounted : 1;
    std::uint8_t  dead : 1;         // dead_ticks > 0 — enough to pick the "waiting to respawn" pose
    std::uint8_t  _reserved : 6;    // 1 (the three bitfields above share this one byte)
    std::uint8_t  _pad[2] = {};     // 2 — explicit, hand-added, same discipline as this section's
                                     //     other structs; not implicit compiler rounding
    std::uint32_t last_swing_tick;  // 4 — drives the deluxe attack-frame read for ANY remote player,
                                     //     exactly the field PlayerView already exists to carry for this
};                                   // 8+8+4+1+1+2+4 = 28

struct PublishedPlayerSelf {        // full fidelity, sent ONLY to the owning client's own subscription
    // items[], skill_level[]/skill_xp[]/skill_next[], ability[]/ability_cd[], mana, stamina,
    // deaths, respawn_tx/ty — every field PlayerView already has that a remote client never needs.
    // This is not a new struct with new fields — it is "send the existing PlayerView, narrowed to
    // one recipient," the cheapest possible answer for the one case where full fidelity is required.
};
```

**Why the split is safe and not a new trust boundary.** `PlayerView` is already published
per-session-slot by the server (`PlayerBus`, `snapshot.hpp`) — this RFC does not change who computes
what, only which subset of an already-server-authoritative struct is serialized to which recipient.
No client ever receives another player's inventory, exact mana, or skill XP — a narrower privacy
surface than exists in-process today (where the renderer, running in the same process as everything
else, could in principle read another session's full `PlayerView` off `PlayerBus`; nothing stops it
today because there is exactly one client on the same machine as the server in every existing build).
This RFC is also, incidentally, the point where that dormant privacy gap is closed — worth naming
explicitly rather than leaving as an implicit side effect.

#### 2.6 Terrain: baseline-only, not per-tick

`ChunkView.terrain` (1024 B, `kChunkTiles²`) is, per `ARCHITECTURE.md §6`, "a hàm thuần theo thời
gian" for the noise-derived component and a one-time worldgen write for the overlay component
(buildings, doors) — it changes only on a player build/destroy action, never per tick. This RFC's
ruling: **terrain is sent once, in the baseline (§3.1), and again only as a targeted single-tile
delta when a build/destroy event actually changes it** — never re-sent whole as part of a per-tick
update, which would otherwise be the single largest line item in every delta by a wide margin (1024 B
against a total per-chunk delta budget measured in tens of bytes, §5).

### 3. Delta encoding

#### 3.1 Baseline

The first message a client receives for any chunk newly entering its `ClientInterestSet` (§1) is a
**full baseline**: terrain (§2.6) plus the complete current record set (every `PublishedCreature`,
`PublishedProjectile`, `PublishedEffect` in that chunk, packed per §2). This is unavoidable — there
is no prior state to diff against — and it is also rare: a baseline fires only when a chunk *enters*
a client's window (walking, teleporting, logging in), not every tick. At the 5×5 = 25-chunk window
size and normal on-foot movement speed, the number of chunks entering a moving player's window per
second is small (crossing one chunk boundary adds at most one new row or column of up to 5 chunks to
the window, not all 25).

#### 3.2 Per-tick delta, id-keyed

After baseline, every subsequent update for a subscribed chunk is a delta keyed by record id:

```cpp
struct WindupCommit {               // 12 bytes, packed — the wire home for the windup-commit
                                     // geometry §2.2 promises; NOT a field on PublishedCreature
    std::uint32_t creature_id;      // matches the PublishedCreature this windup belongs to
    float         x, y;             // the frozen target spot, per RFC-006 T2 — latched once,
                                     // never re-sent for the windup's remaining lifetime
};
```

```
ChunkDelta {
    ChunkCoord coord;
    uint64 tick;
    added:   [ PublishedCreature | PublishedProjectile ]   // new id this tick
    updated: [ (id, PublishedCreature) | (id, PublishedProjectile) ]  // id already known, fields changed
    removed: [ id, ... ]                                    // creature died/left chunk, projectile expired
    effects: [ PublishedEffect, ... ]                       // effects have no persistent id — every
                                                             // effect in an active tick IS the delta,
                                                             // by construction (they live ≤ 14 ticks)
    windup_commits: [ WindupCommit, ... ]                   // fires exactly once, the tick a creature's
                                                             // PublishedCreature.windup transitions
                                                             // 0 → nonzero (§2.2) — reliable, ordered
                                                             // (§9), never re-sent for that windup
    terrain_patch: optional (tx, ty, new_tile)               // §2.6 — rare
}
```

**"Updated" is whole-record, not field-level, and that is a deliberate simplification, stated so it
is not mistaken for an oversight.** `PublishedCreature` is 24 B; a dirty-field bitmask plus only the
changed fields would save bytes only when fewer than roughly 3–4 of its 10 fields change in a given
tick, and a windup-driven telegraph (the case that matters most for readability) changes `windup`,
triggers a companion `WindupCommit` (§3.2, above), and sometimes `status` simultaneously — the
whole-record delta is both simpler to
implement correctly and, for this record's actual size, not meaningfully more expensive than a
bitmask scheme would be. This RFC's ruling: whole-record updates for `PublishedCreature`/
`PublishedProjectile`/`PublishedPlayerRemote`, field-level savings left as a future optimization
(Open Questions §3) if measurement ever shows it's warranted — not designed preemptively against a
cost that hasn't been demonstrated.

**A creature that does not change at all in a given tick is not included in that tick's delta at
all** — this is where the actual bandwidth saving comes from, not from field-level diffing. A chunk
with 6 creatures where 2 are actively fighting a player and 4 are wildlife idly standing still (the
common case away from combat) sends 2 records, not 6, every tick it has no baseline pending.

#### 3.3 Removal is explicit, never inferred from silence

A `removed` entry is sent exactly once, the tick a creature/projectile actually leaves the chunk
(dies, migrates to a neighbor, expires). A client never infers removal from "I haven't heard about
this id in N ticks" — that would reintroduce exactly the kind of soft-timeout guessing `PlayerBeacon`
uses *on purpose* server-side (where a missed beacon self-healing is the correct behavior for a
best-effort, ARP-like presence signal) but which is the *wrong* choice for a client's rendered-entity
list, where a guessed-wrong removal is a visible pop rather than a silent internal cleanup. This is a
genuine, deliberate asymmetry between the server's internal beacon discipline and this RFC's
client-facing delta discipline, and it is worth stating why the two differ rather than assuming
consistency for its own sake: `PlayerBeacon`'s lease exists because the *cost* of a stale entry
(a chunk briefly still considering an absent player "nearby") is invisible and self-correcting.
The cost of a client silently dropping a creature it *thinks* timed out but which the server still
considers alive is a visible, confusing disappearance — the wrong failure mode for a rendering
channel. `ChunkDelta.removed` is therefore an explicit, reliable-delivery event (§ Multiplayer), not
a lease.

#### 3.4 Sparse addressing: adopts RFC-014 §4 verbatim

Any structure this RFC introduces that is keyed by chunk — most concretely, a per-(client, chunk)
"last acknowledged tick / last sent record set" table needed to compute deltas — **must use RFC-014
§4's two-tier addressing scheme** (`persistent_index()` for `MapId < 16`, a per-open-session
`InstanceChunkBlock` sized to that map's own `chunk_edge` for the instanced band), never a
`kChunkCount`-sized dense array of its own. This is not a new design decision — it is RFC-014 §4.2's
own closing instruction, restated here as an explicit compliance point: "`effect_tick`... and any
other structure discovered to share this shape must adopt the identical two-tier scheme, not a
competing one." `client_main.cpp:490`'s `effect_tick` vector is itself one of the two named examples
in RFC-014 §4 and must be migrated to this scheme as part of implementing this RFC's audio-cue
dedup logic client-side, not treated as a pre-existing exception.

### 4. Send cadence: throttling below sim publish rate, never above it

**What this RFC does not decide, and what is not actually built yet.** ARCHITECTURE.md §4's 10/1/0
Hz tick-tier LOD sketch is **not implemented anywhere in `src/` today, and RFC-014 does not build
it either** — RFC-014 §Multiplayer states plainly that "LOD (10/1/0 Hz) is unaffected and reused
as-is" by its own two-timer teardown mechanism, which governs only whether an instance's chunk
actors *exist*, a layer above LOD, not a replacement for it. RFC-010 §9 Open Question 4 (accepted)
independently confirms the tick-tier split remains unimplemented and unowned by any RFC in this set:
"the 1 Hz background tier is specified here but not yet implemented (today only publish-rate LOD
exists). Which phase lands tick-tier LOD...?" This RFC does not answer that question and does not
depend on its answer — it checks its send-cadence reasoning below against what actually ships today,
not against the sketch.

**What actually ships today: `kIdlePublish` (`chunk_actor.hpp:107,119,134`).** A chunk publishes a
fresh `ChunkView` at full sim rate (every tick) whenever its `players_` roster is non-empty, and once
every 32 ticks (3.2 s) otherwise. A chunk only enters a client's `ClientInterestSet` (§1) because that
client's own `PlayerBeacon` reaches it via `fan_beacons()` — which is exactly what populates
`players_` — so **every chunk currently in a client's interest set is, by construction, in the
full-rate publish case.** There is today no such thing as a chunk inside a subscribed client's
interest set that is only publishing once per 32 ticks; the two conditions are mutually exclusive
under the shipped mechanism. This RFC's send-cadence policy below is therefore a genuine
*forwarding* throttle on top of a uniformly full-rate publish source, not a policy that reacts to a
sim-side background tier — because no such tier exists to react to yet. If tick-tier LOD is ever
built (RFC-010 §9 OQ4's open question), this RFC's forwarding throttle composes with it the same way
it composes with `kIdlePublish` today: it never forwards faster than the chunk is publishing, only
sometimes slower.

**What this RFC does decide.** Given a chunk actively publishing a fresh `ChunkView` every tick (the
case that always holds for any chunk in a client's interest set, per the mechanism above), the send
scheduler does not necessarily forward every single published tick to every subscribed client at full
rate. Two bands within the 5×5 interest set (§1), by Chebyshev distance from the client's own chunk:

```
Inner band  (distance ≤ 1, a 3×3 core — the "3x3" the fan_beacons() comment explicitly contrasts
             itself against, repurposed here as the FULL-rate replication core):
    forward every delta the moment the source chunk publishes it — up to 10 Hz, matching the sim.

Outer band  (distance 2, the remaining ring of the 5×5 window):
    forward once every kOuterBandSendPeriodTicks (tunable, default 3 — deliberately reusing
    kBeaconPeriod's own value verbatim, so this RFC introduces no new timing constant where an
    existing one already fits) — full detail, just less often (a 300 ms period at the 10 Hz sim
    rate). A creature only in the outer band is,
    by construction, not close enough to threaten the player this tick (RFC-006's telegraphs read at
    screen distance, not at the edge of a 160-tile window), so a slightly coarser update rate there
    costs nothing readability-wise.
```

**Why banding inside the interest set, rather than a single uniform rate.** The alternative — send
everything in the full 5×5 window at 10 Hz — spends the same bandwidth on a creature at the extreme
edge of visibility (which a player is not making split-second decisions about) as on one standing
next to them mid-swing. Banding is the cheapest lever available for cutting the steady-state
per-client byte rate without touching record shape or delta granularity again.

**Windup is never subject to outer-band throttling.** A telegraphing creature (`windup > 0`,
`PublishedCreature.windup`, §2.2) is always forwarded at full 10 Hz regardless of band — the entire
purpose of RFC-006's telegraph grammar is a reliable, on-time read, and a throttled windup update is
indistinguishable from a shortened lead time from the player's perspective. This is the one hard
exception to banding in this RFC, and it exists specifically so §8's latency arithmetic (which
assumes the windup signal reaches the client promptly) stays true in the implementation, not just on
paper.

### 5. Per-view byte budget

**Answering RFC-010 Open Question 5 directly: the in-process 1 KB `ChunkView` battlefield budget
(RFC-010 §4.5 Budget B-1) does not need halving for the wire — delta encoding already reduces the
steady-state per-tick cost to well under it, for the reason §3.2 gives (most records don't change
most ticks).** The arithmetic:

**Worst-case baseline, one chunk** (everything at its documented cap, all present at once — the
scenario that essentially never occurs in practice but is the correct number to budget against):

| Source | Cap | Per-record size | Worst-case bytes |
|---|---|---|---|
| Terrain | 1 (fixed) | 1024 B | 1024 B (baseline only, §2.6) |
| `PublishedCreature` | no shipped hard cap (§ Open Questions §1) | 24 B | unbounded — see below |
| `PublishedProjectile` | no shipped hard cap | 24 B | unbounded — see below |
| `PublishedEffect` | 24 (`chunk_actor.hpp:1442`, `kMaxEffects`) | 12 B | 288 B |
| `PublishedEntity` (RFC-004) | 16 (RFC-004 §5/§10) | 28 B | 448 B |
| `PublishedScar` (RFC-004) | 64 (RFC-004 §8/§10) | 8 B | 512 B |
| `TilePatch` (RFC-010) | 48 | 16 B | 768 B |
| `FieldState` (RFC-010) | 2 | 24 B | 48 B |

**A named gap this section surfaces, does not invent a fix for.** Unlike every RFC-004/RFC-010
record family, `Creature` and `Projectile` have **no shipped hard per-chunk cap** — they are plain
`std::vector`s that grow with whatever the simulation actually places there (a spawn wave, a dense
wildlife cluster). RFC-010 §4.5's 1 KB budget was scoped only to *its own* record families
(`TilePatch`, `FieldState`, `Effect`) and never claimed to bound `Creature`/`Projectile` — so this
gap is not a regression this RFC introduces, but it is one this RFC is the first to need an answer
for, because an unbounded creature count directly determines the worst-case wire baseline. This
RFC's position: a **per-chunk wire cap on `PublishedCreature`/`PublishedProjectile` count**
(`kWireCreatureCapPerChunk`, `kWireProjectileCapPerChunk`, both **tunable**, proposed default 40 and
20 respectively — generous enough that no observed gameplay scenario, including a stronghold raid
wave, is expected to hit it) is required for this RFC's budget arithmetic to hold, applied **only at
the replication layer** — the simulation keeps however many creatures it wants in a chunk unmodified;
if a chunk's live count ever exceeds the wire cap, the extra creatures are simply not included in
that tick's baseline/delta (prioritized by distance to the subscribing client, nearest first), a
graceful degradation rather than a hard error. This cap is a **new, proposed constant with no shipped
precedent**, flagged as such (Open Questions §1) rather than presented as an existing limit.

With that cap: worst-case creature/projectile contribution to a single chunk's baseline is
`40 × 24 + 20 × 24 = 960 + 480 = 1440 B`.

**Full worst-case single-chunk baseline:** `1024 + 1440 + 288 + 448 + 512 + 768 + 48 ≈ 4.5 KB`.
This is a **baseline** figure (§3.1) — sent once when a chunk enters a client's window, not every
tick.

**Steady-state per-tick delta, one chunk, typical case (no baseline pending):** in the common case
away from a dense fight — a handful of wildlife mostly idle, no active raid, no player nearby fighting
— the number of records that actually *change* in a given tick is small. A conservative estimate for
"something is happening" (one creature mid-combat with a telegraph active, one projectile in flight,
2–3 effects from a recent swing) is on the order of `1 × 24 + 1 × 24 + 3 × 12 ≈ 85 B` per tick per
chunk with activity, and **0 B** for a chunk with nothing changing (the empty-delta case, which is
the majority of subscribed chunks at any given moment away from combat — matching the existing P1
LOD principle "chunk rỗng publish thưa," ARCHITECTURE.md §4, extended here from publish-suppression to
delta-suppression).

**Per-view total, worst case (client near an active fight, all 25 interest-set chunks active,
none newly entering):** applying §4's banding — the 9 inner-band chunks forward at the full 10 Hz
rate, the 16 outer-band chunks at 10/3 Hz (once every `kOuterBandSendPeriodTicks`=3 ticks) — the
**effective** per-second rate is `(9 × 85 B × 10) + (16 × 85 B × 10/3) = 7650 + 4533 ≈ 12.2 KB/s per
client` in an active-combat worst case (well under the naive un-banded figure of `25 × 85 B × 10 ≈
21.25 KB/s`, which is what banding exists to avoid), and well under 1 KB/s in the common non-combat
case (mostly-empty deltas). At 20–50 concurrent players (GAME.md §11, subject to the `kMaxPlayers=8`
caveat above) this is comfortably within any home-cluster uplink this project targets — the
arithmetic exists to make that claim checkable, not to assert it.

**This RFC's budget rule, stated once:** `kPerTickDeltaBudget` **(tunable)**, default 2.5 KB per
client per tick — comfortably above the §5 worst-case steady-state estimate (~2.1 KB, every one of
25 interest-set chunks simultaneously active, itself a pessimistic scenario) while staying well
under the ~4.5 KB single-chunk baseline figure, so an ordinary busy moment never trips it — enforced
by **culling the outer band first, then dropping non-windup creature
updates by distance** if a pathological scenario (e.g. a full raid wave concentrated in one chunk)
would otherwise exceed it — never by dropping a `windup` update (§4's hard exception) and never by
delaying a `removed` event (§3.3). A budget breach is handled by graceful degradation of *what* gets
sent, never by slowing *when* — nothing about this system should introduce a player-visible stutter
tied to how much is happening on screen.

### 6. The Map-marker channel: satisfying RFC-021 §5.4

RFC-021 §5.4 states the contract this RFC must satisfy, not re-litigate: "a push-on-change model...
satisfies every marker kind... without polling," "this is explicitly decoupled from chunk/creature
replication... It must not multiplex it onto the same cadence," and an illustrative budget of "a few
hundred marker structs... a few KB" for a full discovered-set snapshot.

This RFC's compliance, concretely:

- **A second logical channel**, `MapMarkerChannel`, independent of `ClientInterestSet` (§1) entirely
  — a marker's visibility is governed by RFC-021 §5.2's fog/discovery bitset, not by chunk proximity,
  so it cannot ride the same per-chunk subscription mechanism at all, only (optionally) the same
  underlying transport connection.
- **Full snapshot once, at login** (RFC-021 §5.4's own first bullet) — every discovered marker the
  reconnecting/newly-logged-in player already has bits set for, sent as one batch.
- **Delta thereafter, on marker-state change only** — a village tier-up, a fort generation
  publication (RFC-007's checkpoint lifecycle), a raid announcement firing, a new marker crossing
  into the discovered set as the player explores. Every one of these is already a discrete,
  low-frequency server event per RFC-021 §5.4's own framing (village tier changes are deliberately
  slow, GAME.md §6; fort generations publish per training checkpoint, not per tick) — this RFC adds
  no new event source, only the wire delivery of ones that already exist.
- **No tick-driven traffic of any kind on this channel** — satisfying RFC-021 §5.4's explicit "the
  Map screen never needs a per-tick feed" by construction: nothing schedules a `MapMarkerChannel`
  send on a timer, only on the underlying state-change event firing.

### 7. Instance/realm crossing: subscription swap on `Teleport`

When a player crosses through a `Portal` (RFC-022 §2.2) and RFC-014's `resolve()`/`allocate_new()`
flow lands them on a new `MapId` via the shipped `Teleport{map, x, y}` message (`protocol.hpp`,
unmodified by this RFC), this RFC's client-facing consequence is a **full subscription swap**:

```
on Teleport{new_map, new_x, new_y} observed for a session:
    old_set = ClientInterestSet(old_map, old_position)   // §1
    new_set = ClientInterestSet(new_map, new_position)   // §1, recomputed against the new map
    for c in old_set - new_set: unsubscribe(client, c)   // drop delta tracking state for c
    for c in new_set:           send_baseline(client, c) // §3.1 — every chunk is "new" post-teleport,
                                                          // even ones that happen to share a MapId/
                                                          // coordinate with something previously seen,
                                                          // because an instanced MapId is never reused
                                                          // within a session (RFC-014 §3.1) and a
                                                          // persistent-band re-entry may have drifted
                                                          // since last visited
```

**Why a full swap rather than an incremental window shift, unlike ordinary walking.** An ordinary
step across a chunk boundary shifts the 5×5 window by at most one row/column (§3.1's "small" case);
a `Teleport` can move a player to a `MapId` with no relationship at all to the previous one (RFC-014
§3, an entirely different, freshly-primed instanced map). There is no meaningful "diff" between an
overworld window and a dungeon-instance window — treating it as a full unsubscribe/resubscribe rather
than attempting a partial diff is both simpler and correct, and costs nothing extra: a `Teleport`
event is already the same kind of infrequent, discrete event RFC-021 §5.4 treats Map markers as, not
a per-tick concern.

**Interaction with RFC-014 §6's reconnect-in-place.** A reconnecting player who resumes inside a
still-open instance (RFC-014 §6, "the player materializes exactly where they logged out") receives
this RFC's ordinary **login baseline** (§3.1, one per newly-subscribed chunk) for their resumed
position — reconnection is not a `Teleport` message in RFC-014's own model, but the wire-level
consequence for this RFC is identical to one: a fresh interest set, computed against wherever the
player actually is, baseline-loaded from scratch. This RFC does not need a special case for
reconnection; §3.1's ordinary "chunk newly enters this client's window" rule already covers it.

### 8. Latency as a design constraint, not a risk-table aside

**The number.** GAME.md §11 and ARCHITECTURE.md §9 both cite "50–150 ms" as the expected
home-cluster round-trip-adjacent latency figure, without stating whether that is one-way or
round-trip, and without checking it against anything. This RFC's working interpretation, stated so
the arithmetic below is checkable: **50–150 ms is the one-way message latency** between a player's
own machine and the leader/chunk-owning node (the more conservative reading, since it is the
harder-to-improve half of a request/response pair and the one that actually delays "an event
happened on the server, when does the client see it").

**What must tolerate it: RFC-006's telegraph lead times, checked.**

| RFC-006 tier | Min wind-up | One-way replication delay (worst case, 150 ms) | Effective lead time the player actually gets |
|---|---|---|---|
| 0 `light` | 500 ms | 150 ms | **350 ms** — still ≥ RFC-001 V2's 400 ms hostile-cast floor's neighborhood, but the *tightest* margin in the table (Open Questions §2) |
| 1 `moderate` | 800 ms | 150 ms | **650 ms** |
| 2 `heavy` | 1200 ms | 150 ms | **1050 ms** |
| 3 `deadly` | 1600 ms | 150 ms | **1450 ms** |

**Reading this table correctly.** A telegraph's windup is a **server-side sim state**
(`Creature.windup`, counting down) — the wind-up itself is not delayed by replication; what is
delayed is *when the client's screen shows it starting*. Worst case, a windup commits on the server
at sim-tick T, and the client's `ChunkDelta` carrying `windup > 0` (§4's hard full-rate exception)
arrives up to 150 ms later — so the player's *effective* reaction window is the tier's authored
lead time **minus** that one-way delay, not the full authored value. The table above is that
subtraction, and every tier still leaves a positive, playable window even at the pessimistic end of
the latency range — tier 0's 350 ms is the tightest, and still comfortably reactable (a stated,
tested-elsewhere threshold: RFC-001 V2 already requires ≥ 400 ms cast time on every hostile ability
as an *independent* floor for its own reasons, which happens to sit close to this number and is not
coincidental — both exist to keep a hostile action humanly reactable).

**The constraint this RFC states, for future RFCs to check against:** any future ability, boss
mechanic, or hazard with an *authored* reaction window below **~500 ms** (roughly 2× worst-case
one-way replication, leaving at least ~350 ms of actual player reaction time even in the worst
network case) must not be added without this RFC being revisited for client-side prediction — which
this RFC, like RFC-010 §4.5, explicitly does **not** build for v1 ("latency 50–150 ms is acceptable
for a 4 s burn patch; it is not a PvP game," RFC-010, restated here as this RFC's own position:
**no client-side prediction of any kind ships in this RFC's v1** — every wire update is a plain,
un-extrapolated echo of server-authoritative state, with the single narrow exception of
`PublishedProjectile.vx/vy` (§2.3), which exists only to let the renderer draw smooth in-flight
motion between updates, never to predict an outcome).

**PvP is already the sharper version of this problem, and it's off.** GAME.md §11's own reasoning
("Latency 50–150 ms của cluster máy gia đình không hợp PvP hành động") is a strictly harder bound
than anything a PvE telegraph needs (a PvE boss's wind-up is authored with margin by design, RFC-006
§1.3; a PvP opponent's reaction is not authored at all). This RFC's arithmetic only needed to clear
the PvE bar, and it does, with room to spare — it does not attempt to justify enabling PvP, which
stays exactly as out of scope as GAME.md §11 already put it.

### 9. Reliable vs. best-effort delivery

Two delivery classes, stated because §3.3's removal-reliability requirement and §6's marker-delta
requirement both depend on it:

```
Reliable, ordered   — ChunkDelta.removed entries, ChunkDelta.windup_commits entries (§3.2), baseline
                       sends (§3.1), MapMarkerChannel deltas (§6), Teleport-driven subscription swaps
                       (§7). A dropped or reordered message in this class is a correctness bug (a
                       creature that never disappears, a telegraph with no readable target spot, a
                       marker that never appears), not a cosmetic one.
Best-effort, latest-
wins                — ChunkDelta.added/updated entries, PublishedEffect batches. A dropped position
                       update is invisible by the next tick's update superseding it (the same
                       "lossy channel, draw the newer one" discipline `snapshot.hpp`'s own header
                       comment already states for the in-process renderer/SnapshotBus relationship,
                       extended here to the network hop).
```

This split mirrors, deliberately, the reliable-vs-lossy distinction the codebase already draws
in-process (`snapshot.hpp`: "this is deliberately a *lossy* channel... the simulation never stalls
waiting for a frame") rather than inventing a third policy — this RFC's only addition is drawing the
line at message *class*, not at the whole channel, because unlike the in-process case, `removed`
correctness actually matters over an unreliable network in a way it never could when sender and
reader shared an address space.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001..009 (accepted combat set)** | Not load-bearing here except through the records they produce (`Creature`/`Projectile`/`Effect` state, RFC-002 status, RFC-009 damage outcomes as HP deltas). This RFC replicates their *output*, never their math. |
| **RFC-004** (accepted-with-revisions) | §10's `PublishedEntity`/`PublishedScar` pattern is the direct precedent this RFC's §2 generalizes; those two structs are consumed unmodified, cited not redefined. |
| **RFC-006** (accepted-with-revisions) | §1.3's danger-tier minimum lead times are the numbers §8's latency arithmetic is checked against; this RFC adds no new telegraph timing, only confirms the existing floors clear replication delay. `windup`'s full-rate exception (§4) exists specifically to protect RFC-006's readability contract. |
| **RFC-007** (accepted-with-revisions) | Checkpoint/generation numbers this RFC's Map-marker channel (§6) displays are RFC-007's; this RFC only carries them, never computes or validates them. |
| **RFC-010** (accepted-with-revisions) | §4.5's `ChunkView` replication table and Budget B-1 are extended, not replaced, by this RFC's §5; Open Question 5 is answered directly (§5's opening line). `TilePatch`/`FieldState` caps are consumed unmodified in this RFC's budget arithmetic. |
| **RFC-014** (accepted) | §4's sparse chunk-addressing scheme is adopted verbatim (§3.4) for every wire-side chunk-keyed structure this RFC introduces, including the `effect_tick` migration RFC-014 named by number. §3's `Teleport`-driven instance lifecycle is this RFC's trigger for §7's subscription swap. This RFC does not touch instance allocation, idle timeout, or session state — purely a consumer of RFC-014's already-decided `MapId`/`Teleport` behavior. |
| **RFC-016** (proposed, Persistence) | Orthogonal — this RFC's wire state is entirely ephemeral (rebuilt from baseline on every subscribe), never persisted. Where RFC-016 defines what a reconnecting player's position/session resumes to, this RFC's §7 supplies the mechanical consequence (a fresh baseline) once that position is known — this RFC does not decide *where* a player resumes. |
| **RFC-019** (accepted-with-revisions) | XP mote/level-up display already rides `PlayerView`'s `skill_xp`/`skill_level` fields (`client_main.cpp`'s existing `last_xp`/`last_skill` audio-cue tracking, cited not modified); this RFC's `PublishedPlayerSelf` (§2.5) carries those fields to the owning client unchanged. Not modified by this RFC. |
| **RFC-020** (accepted-with-revisions) | Quest-participation gating rides the existing `PlayerBeacon` roster per RFC-020 §Multiplayer; this RFC's replication layer is the wire mechanism RFC-020 named as its own dependency, satisfied here without any quest-specific wire message — quest state is ordinary `PlayerView`/marker data, not a new channel. |
| **RFC-021** (accepted-with-revisions, partially superseded by RFC-022) | §5.4's Map-marker budget contract is satisfied by this RFC's §6, cited not re-derived; this RFC introduces no new marker kind or discovery rule, only the channel that carries RFC-021's already-defined ones. |
| **RFC-022** (accepted-with-revisions) | `Portal`/`MapSession` data shapes are cited, not reproduced, at §7; this RFC's subscription-swap trigger is `Teleport`, which RFC-022 §2.1 already establishes as the terminal verb every portal resolves to — this RFC needs nothing from `Portal` or `MapSession` directly, only their eventual `Teleport` output. |

---

## Multiplayer & Simulation-LOD Considerations

- **This RFC explicitly does not own simulation tick rate or chunk sleep.** ARCHITECTURE.md §4's
  10/1/0 Hz LOD sketch remains unimplemented and unowned by any RFC in this set (RFC-010 §9 OQ4;
  RFC-014 §Multiplayer disclaims building it) — §4 of this RFC checks its reasoning against what
  actually ships today, `kIdlePublish` (`chunk_actor.hpp:107,119,134`), under which every chunk in a
  client's `ClientInterestSet` is always in the full-rate publish case (§4). §4 of this RFC only
  throttles *forwarding* of whatever is already published, and only downward (never causing a chunk
  to publish more or less often than the sim already decided). Should tick-tier LOD ever ship, a
  background-tier chunk with nothing new to forward is handled identically to "nothing changed,"
  §3.2's ordinary empty-delta path, with no special-casing needed — but that case does not exist
  under today's shipped mechanism.
- **Interest set recompute cost is bounded by player count, not map size** — `ClientInterestSet` is
  computed per player, at `kBeaconPeriod` cadence, over a fixed 25-chunk window, exactly mirroring
  the cost profile `fan_beacons()` already has today (O(players), not O(chunks)) — this RFC adds no
  new O(map) cost anywhere.
- **20–50 concurrent players, no VPS (GAME.md §11).** §5's per-client budget (~12 KB/s worst case,
  well under 1 KB/s typical) times 50 clients is on the order of ~0.6 MB/s aggregate at the absolute
  worst case (everyone simultaneously in active combat, which does not happen in practice at this
  player count and this game's PvE-only combat model) — a bandwidth figure any home-cluster uplink
  this project targets clears without strain; this RFC does not propose any server-side
  bandwidth-shaping beyond §5's per-client cap.
- **A named dependency this arithmetic assumes and does not itself deliver: `kMaxPlayers=8`
  (`tiles.hpp:51`) is today's actual session-slot ceiling** — a cold-only registration limit
  (`Engine::register_activation` cannot spawn a new session actor while the world is running,
  ARCHITECTURE.md, ROADMAP.md:319-322), explicitly deferred to P6 ("bỏ giới hạn cần Quark cho phép
  spawn nóng, và đó là việc của P6" — lifting the limit needs Quark to allow hot spawn, and that is
  P6's job). Every 20–50-player figure in this section is checked against the *target*, not against
  what is playable today; this RFC's bandwidth arithmetic is correct at that target but does not
  lift, and is not blocked by, the `kMaxPlayers=8` ceiling — that remains P6's dependency to resolve
  before this RFC's numbers are exercised at more than 8 concurrent sessions.
- **`Require<Trusted>` unaffected.** This RFC introduces no new trusted-actor type — the send
  scheduler (§4) and delta-tracking state (§3.4) are properties of whatever node hosts the
  client-facing connection (P6's territory to place), not a new leader-resident actor; the *data*
  being replicated (`ChunkView`, `PlayerView`) is already produced by existing trusted/tier-B actors
  unchanged.
- **Determinism is not a concern for this RFC's own state.** Everything this RFC adds (delta
  tracking, interest sets, send schedules) is per-connection, ephemeral, client-relationship state —
  never part of the simulation's own cross-node-comparable state (ARCHITECTURE.md §2c's D1/D3
  distinction, RFC-010 §5's extension of it). A dropped or reordered best-effort packet (§9) never
  causes two nodes to disagree about simulation truth, only about what a screen currently shows.

---

## Tone Guardrail Compliance

Walking every mechanic this RFC introduces against GAME.md §0's test:

1. **Interest-set windowing and banding (§1, §4).** Entirely a rendering/bandwidth optimization —
   nothing about which chunks a client subscribes to, or how often it hears from them, is visible to
   the player as a countdown, a penalty, or a degrading resource. A chunk leaving the outer band
   still exists and is still fully simulated; the player simply isn't looking at it closely enough
   for the difference to be perceptible.
2. **The per-tick budget and graceful degradation (§5).** Explicitly designed to never manifest as a
   player-visible stutter or a "your connection is degraded" message — a budget breach silently
   drops the least-important updates (outer-band, non-windup, farthest-first), never slows the
   simulation, never delays a windup or a removal. There is no dial the player can watch tick down.
3. **The Map-marker channel (§6).** Directly inherits RFC-021 §5.4's own push-on-change, no-polling
   design — nothing here adds a refresh timer, a "markers may be stale" warning, or any element with
   elapsed-time semantics.
4. **Instance-crossing subscription swap (§7).** Instantaneous from the player's perspective — the
   same "short transition, then you're there" experience RFC-014 §Tone Guardrail Compliance already
   commits to; this RFC's baseline-resend is server-side bookkeeping with no player-visible loading
   bar beyond whatever transition RFC-022 §5.4 already describes.
5. **Latency handling (§8).** No mechanic in this RFC responds to high latency by penalizing the
   player (no "lag detected, damage reduced" or similar) — the constraint this RFC states is a design
   *floor* future content must respect (telegraphs must stay reactable), not a runtime punishment for
   a player whose connection happens to sit at the high end of the expected range.
6. **Reliable-delivery guarantees (§9).** Exist specifically to *prevent* a tone violation — an
   unreliable `removed` event could cause a creature to appear to never die (visually) even after
   the player killed it, which would read as a broken promise, not a chill design choice. Reliability
   here is in service of honesty about server state, not an added mechanic of its own.

No mechanic in this RFC creates a deadline the player watches, a decaying resource, a
login-frequency dependency, or a countdown of any kind.

---

## Open Questions

1. **`kWireCreatureCapPerChunk`/`kWireProjectileCapPerChunk` (§5).** Proposed defaults (40/20) are
   first guesses with no playtesting behind them, unlike most caps this RFC cites from other RFCs
   (RFC-004's 16/64, RFC-010's 48/2/24) which at least have those RFCs' own design reasoning behind
   the number even if untested. This RFC's caps are newer still — flagged as needing a real
   measurement pass once a raid wave or dense wildlife cluster can actually be observed at scale.

2. **Tier-0 telegraph's 350 ms effective lead time (§8).** The tightest margin in the latency table.
   It clears RFC-001 V2's independent 400 ms hostile-cast floor by coincidence of similar
   magnitude, not by a designed relationship between the two numbers — worth a dedicated check once
   real network conditions (not the GAME.md-cited estimate) are measurable, in case the two floors
   need to be explicitly reconciled into one governing constant rather than two that happen to agree.

3. **Field-level delta encoding for `PublishedCreature` (§3.2).** This RFC chose whole-record deltas
   over a dirty-field bitmask on the grounds that the record is small enough that the difference is
   marginal. If a future measurement pass (once real traffic exists to measure) shows creature-heavy
   scenes are a genuine bandwidth bottleneck, bitmask deltas are the first optimization to revisit —
   not designed here because the cost that would justify it hasn't been demonstrated.

4. **Compression.** This RFC specifies an uncompressed binary wire format throughout. Whether a
   generic transport-level compression pass (e.g., over the whole delta batch, not per-record) is
   worth its CPU cost given §5's already-modest byte figures is left unevaluated — the budget
   arithmetic in §5 does not assume compression, so adding it later is a pure improvement, never a
   correctness dependency.

5. **Connection/session establishment, reconnection at the transport level, and authentication over
   the wire.** This RFC assumes a client-facing connection exists (P6's territory) and specifies only
   what flows over it once established. The handshake itself, TLS/`SecureTransport` wiring
   (ARCHITECTURE.md §2's "chưa nối" — not yet connected — deferred item), and session-token exchange
   are explicitly P6's to design, not silently assumed solved here.

6. **`kOuterBandSendPeriodTicks` and `kPerTickDeltaBudget`'s actual values (§4, §5).** Both marked tunable
   with a stated first-guess rationale (matching `kBeaconPeriod`'s existing cadence; a round 2 KB
   figure with margin under the ~1.5 KB worst-case estimate) but neither has been measured against a
   real client. Flagged for a tuning pass once P6 has something running to observe.

7. **`Zone`/`Crop`/`Building` wire encoding (§2.1, Non-goals).** Explicitly out of scope here because
   no RFC yet charters the farm/building economy those three records belong to (RFC-019 §Non-goals
   names the same gap). Whichever future RFC takes on that system will need a wire projection
   following this RFC's §2 pattern — flagged here so it isn't silently forgotten, not designed here.

---

## Non-goals

- **Simulation tick rate, LOD sleep/wake mechanics, or chunk-actor lifecycle.** ARCHITECTURE.md §4's
  10/1/0 Hz sketch — unimplemented, and not built by RFC-014 either (RFC-014 §Multiplayer; RFC-010
  §9 OQ4 leaves its ownership open). This RFC only reacts to whatever the simulation already
  publishes (today, `kIdlePublish`, §4); it never decides when a chunk ticks, sleeps, or wakes.
- **Instance/realm allocation, group binding, idle timeout, or leader-death recovery for instanced
  maps.** Entirely RFC-014's. This RFC's §7 only defines the client-side subscription consequence of
  a `Teleport` RFC-014's flow already issues.
- **Save format, or anything about what survives a server restart.** RFC-016's (proposed). This
  RFC's entire wire state is rebuilt from a fresh baseline on every subscribe and persists nothing.
- **Transport-level connection establishment, authentication, encryption, or reconnection at the
  socket/session layer.** P6's (ROADMAP.md) and, for the password-in-transit gap specifically,
  ARCHITECTURE.md §2's own named deferred item (`SecureTransport`). This RFC assumes a connection
  exists and specifies only the application-level protocol riding on it.
- **Client-side prediction, interpolation smoothing beyond `PublishedProjectile`'s velocity hint
  (§2.3), or any lag-compensation scheme.** Explicitly out of scope for v1, consistent with RFC-010
  §4.5's own "no client-side battlefield prediction" position, generalized here to the whole
  protocol. §8 names the threshold at which this stance would need revisiting, but does not cross it.
- **PvP networking of any kind.** Off by default (GAME.md §11); this RFC's latency budget is checked
  only against PvE telegraph timing, never against a PvP reaction-time bar.
- **Combat HUD data consumption.** What RFC-011 (proposed) does with the `PublishedPlayerSelf`/
  `PublishedCreature` data this RFC delivers is RFC-011's concern; this RFC only guarantees the data
  arrives, not how it is drawn.
- **Chat, friends lists, or any non-world-state client-facing message class.** ROADMAP.md P6 lists
  these as separate P6 deliverables; this RFC's message taxonomy (§9) does not preclude adding them
  as further reliable-channel message kinds later, but does not design them here.
- **`ChunkView.zones`/`.crops`/`.buildings` wire encoding (§2.1).** No packed projection, delta rule,
  or budget-table entry is defined for `Zone`, `Crop`, or `Building` — a farm/economy replication
  problem with no owning authoring RFC yet (RFC-019 §Non-goals names the same gap on the design side).
  Left for whichever future RFC charters the building/farm economy as a whole (Open Questions §7);
  speccing the wire shape ahead of that design would mean inventing both at once.

---

## Review Record

Reviewer A: revise. Reviewer B: revise. Both converged on four majors; two further items were
mustFix for B only (sound on verification, applied).

Applied:
- Removed the false "operationalized by RFC-014"/LOD-sketch claims (header, §4, §Multiplayer,
  Non-goals, guide); §4 now grounds send cadence in shipped `kIdlePublish=32` and removes the
  impossible "1 Hz background chunk in the interest set" example.
- Added `WindupCommit` (12 B) wire record, `ChunkDelta.windup_commits`, and a §9 reliable-class
  entry — the windup-commit geometry §2.2 promised now has an actual wire path.
- Narrowed §2/§2.1/Summary scope to explicitly exclude `Zone`/`Crop`/`Building`; added a Non-goals
  bullet and Open Question 7 pointing at the unchartered farm/building-economy RFC that owns it.
- Corrected header's `kMaxPlayers=8` citation (`tiles.hpp:51`, not `player_actor.hpp`) and named it
  as a P6-deferred dependency in §Multiplayer, not silently assumed lifted.
- Fixed §5/§Multiplayer bandwidth arithmetic: ~15–20 KB/s → ~12.2 KB/s (banding formula shown);
  0.75–1 MB/s aggregate → ~0.6 MB/s.
- Fixed `PublishedPlayerRemote`'s packed/32 B self-contradiction: now genuinely packed, 28 B,
  per-field byte accounting shown.
- Removed leftover `x_q/y_q`/`vx_q/vy_q` quantization language contradicting §2.1's no-quantization
  ruling (§3.2, §8).

Unresolved: none of the mustFix items — all seven (four dual-reviewer, two single-reviewer-sound,
plus the merged kMaxPlayers item) were applied. Baseline-burst pacing on login/Teleport (both
reviewers' shared non-blocking should-fix) was not folded in as a new Open Question — left for a
future pass, not a blocker.
