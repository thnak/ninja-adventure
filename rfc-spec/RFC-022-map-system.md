# RFC-022: Map System

> Status: **Accepted (revised after review)**
> Design canon: [GAME.md §3](../GAME.md) (world structure decision — one seamless overworld,
> instanced realms behind portals), [§4](../GAME.md) (ring geometry intent), [§6](../GAME.md)
> (village tiers), [§9](../GAME.md) (seasons/weather) · [ARCHITECTURE.md §4](../ARCHITECTURE.md)
> (`MapId`, `InstanceManager` sketch), [§8](../ARCHITECTURE.md) (worldgen passes) ·
> [ROADMAP.md](../ROADMAP.md) P3 (villages, shipped-adjacent), P4 (instance infrastructure, mines),
> P8 (dungeons, rest realms)
> As-built source grounding: `src/world/tiles.hpp` (`kChunkTiles`, `kMapChunks`, `kMapTiles`,
> `kMapCount`, `Ring`, `kRingEdge`, `Door`), `src/world/village.hpp` (`StructureKind`, `VillagePlan`,
> `plan_of`, `gates_of`), `src/world/worldgen.hpp` (placement order, `kVillageEdgeMargin` and the
> real per-village edge test), `src/world/protocol.hpp` (`Teleport`), `src/render/raylib_bridge.cpp`
> (`Weather`, `weather_of`), `assets/CREDITS.md` (tileset sourcing)
> Sibling RFC this run: RFC-023 (Character & NPC Roster System) — every population detail (who
> stands in a village, what monster fills a dungeon room) is RFC-023's; this RFC places the empty
> stage, RFC-023 populates it.
> Supersedes RFC-021 **in part** — see [Relationship to RFC-021](#relationship-to-rfc-021).
> Depends on: RFC-021 (ring geometry, worldgen placement order, no-build zones, the Map screen and
> discovery model — all cited by number, not reproduced, except where explicitly narrowed below)
> Depended on by: RFC-020 (accepted-with-revisions, Mission & Quest System — consumes the `Portal`
> data shape for rumor-triggered instances); (proposed) RFC-014 (Instance & Realm Lifecycle —
> consumes this RFC's `MapId` partition and `MapSession`/join-vs-create contract as the shape it
> implements allocation against), RFC-013 (Vitals, Death & Recovery — consumes portal endpoints as
> the anchor for respawn/ejection rules)

---

## Summary

The map is the fundamental spatial unit of Ninja Adventure, and it comes in exactly one *kind* —
a chunk-owned, integer-tile grid, `kChunkTiles = 32` tiles per chunk — at whatever *size* the
content calls for. This RFC specifies: (1) the map variant taxonomy, formalizing `MapId` as the
runtime-partitioned value `ARCHITECTURE.md §4` already sketched, and the one rule that makes "how
big should this map be" a non-question — 1024×1024 (`kMapTiles`, 32×32 chunks) is the *ceiling*,
reserved for the persistent overworld, and every smaller map is the identical system with fewer
chunk actors instantiated, never a second code path; (2) a `Portal` concept that generalizes the
two portal-shaped things already shipped — `Door{tile,room}` (house → interior room) and
`Teleport{map,x,y}` (the instant-position-change verb a portal ultimately invokes) — into one data
shape that can target *any* map, fixed or dynamically allocated; (3) where house/tent stamped
structures (`StructureKind`, `village.hpp`) end and where mine/dungeon zone entrances (`Portal`, not
`StructureKind`) begin, and what tileset each draws from; (4) the village-always-fits invariant,
proven with the actual `plan_of()` numbers rather than asserted — every village tier the game ships
fits inside a 2×2-chunk (64×64-tile) map with room to spare, and a practical 3×3-chunk (96×96-tile)
map fits all five with margin; and (5) how biome, weather, and a narrow session contract together
decide what a portal connects to and what crossing it feels like.

This is a mixed spec, and each part says which kind it is. Parts (1)'s `MapId` partition and (3)'s
structure/portal boundary **record and operationalize** decisions `ARCHITECTURE.md` and the shipped
code already made. Parts (2), (4), and (5) are **green-field** — nothing in the engine has a
`Portal` type, a village-fit formula, or a session-join rule today; every number introduced there is
marked **(tunable)**.

NPC and monster population of any map — village villagers, guards, wandering merchants, dungeon
monsters — is entirely out of scope; see RFC-023. Deep instance-lifecycle mechanics — allocation,
per-group binding, idle timeout, leader migration, per-realm atlas load/unload — are entirely out of
scope; see RFC-014 (proposed). This RFC stops at "here is the map, here is the portal that leads to
it, here is who may use it and whether they share it" — everything downstream of that line belongs
to those two specs.

---

## Motivation

Three gaps, none of them addressed by the currently-accepted spec set:

1. **`ARCHITECTURE.md §4` sketched a `MapId` future that no accepted RFC has actually specified.**
   The engine comment reads plainly: `MapId` must become a runtime `uint16_t` with a partition —
   "e.g. `0..15` for the persistent overworld, `16..65535` allocated to instances." That sentence is
   four years of implied design compressed into a parenthetical. Nobody has written down what a
   `MapId` in the instanced band actually points at, how a portal resolves one, or what happens when
   two players who want to fight the same dungeon step on the same gate one second apart. RFC-021,
   which owns "where a gate sits and what it shows on the map," never mentions `MapId` at all — a
   grep across its 691 lines returns zero hits. This RFC is the first to give the sketch a shape.

2. **RFC-021 specifies the overworld as if it were the only map the game has**, because at the time
   it was written that was true of the shipped engine. Its own Summary calls the 1024×1024 overworld
   "the shipped ... contract" without qualification. But GAME.md §3 has never described a
   single-map game — "sau cổng: NHIỀU CÕI riêng" (behind the gate: many separate realms) is stated
   as plainly as the seamless-overworld half of the same sentence, and `ARCHITECTURE.md §4`'s own
   `MapId` sketch budgets for 65,536 of them. RFC-021's silence on this is not a design decision, it
   is an artifact of scope — RFC-021 was asked to specify the overworld, and it specified the
   overworld, and correctly declined to invent instance internals it wasn't asked for. This RFC picks
   up exactly the thread RFC-021 left untied: what a map *is*, as a concept the overworld is one
   instance of, not the only one.

3. **`village.hpp`'s own header comment already states the design principle this RFC's §4 needs, and
   nothing has operationalized it yet.** Quoting it directly: *"`worldgen.hpp` answers WHERE things
   go ... This file answers WHAT A VILLAGE IS, and it reasons about one settlement in isolation."*
   That separation was written for a single 1024×1024 map, but nothing about it depends on that map's
   size — `VillageBuilder` takes a seed, an overlay buffer, and an output list; it has never once
   asked how big the map underneath it is. The "village always fits" invariant this RFC specifies is
   not a new capability bolted onto the codebase — it is the direct, previously-unstated consequence
   of a separation the codebase already made for an unrelated reason (keeping `worldgen.hpp` from
   growing a second class of global reasoning). This RFC is the first place that consequence is
   written down and checked against the actual numbers in `plan_of()`.

A fourth, cross-cutting motivation, inherited from RFC-021's own §8 and restated here because it
binds this RFC too: **every mechanic below must survive the GAME.md §0 test.** A mission portal that
closes on a schedule, an instance that quietly evicts a group after a countdown the player never
sees, a village that loses services because it landed on a "small" map — each of those is a clock or
a quiet penalty in different clothing, and §8 below walks through why this RFC introduces none of
them.

---

## Guide-level Explanation

### For a player

Walking the overworld is exactly as it is today: one seamless 1024×1024 field, no loading screen,
no seam. Scattered through it are fixed structures that lead somewhere else — a realm gate, a mine
mouth, a dungeon mouth. Step onto one and the world changes: a short transition, then you're
somewhere with its own sky, its own ground, sometimes its own asset pack entirely (GAME.md §3 — "một
cõi dùng Kenney, một cõi dùng Ninja Adventure"). Some of these places are shared with everyone who
steps through at the same time you do; some spin up a private copy just for your group, exactly the
way GAME.md §3 already promises for dungeons ("một bản riêng mỗi nhóm"). You never have to know
which is which in advance — the world tells you when you arrive, not before.

Not every place behind a portal is a fight. A hot spring, a fishing lake, a cloud isle — you walk in,
you fish or sit or look around, and you walk back out through the same portal whenever you want.
Nothing there is timed, nothing there closes on you.

If a portal ever leads to a place small enough to feel like "just a clearing," it might still have a
whole village in it — a market, a quest board, someone to sell your catch to. The rule you never see
but always benefit from: **a village never shows up half-built because the map it's on is small.** A
market that would exist on the big map exists here too, at the same tier, doing the same thing. What
changes with map size is how far you have to walk to leave the village, not what the village offers.

### For a designer

Adding a new map — a new mission, a new seasonal event space, a new realm variant — is a size choice
and a content choice, never a new system. Pick an edge length in whole chunks (from 1 up to the
overworld's ceiling of 32); pick whether it's a persistent fixture of the world or something players
step into and out of; pick its asset pack, its weather behavior, whether it hosts a village and at
what tier. Every one of those is a row in a table this RFC defines, in the same spirit as the
`kPoiTable` pattern RFC-021 §5 already documents for realm gates and mines — you are never asked to
write a new placement algorithm, only to fill in a row.

If the map needs a village, §4 below tells you the smallest size that will hold one at any tier the
game ships: two chunks square, with a comfortable third chunk of margin recommended. You do not need
to invent a "mini village" — you place the same `VillageBuilder` call worldgen already makes, on a
smaller canvas.

### For an engineer implementing map-to-map connectivity

You need: (1) the `MapId` partition (§1.1) — which numbers mean "this map always exists" versus
"this map was allocated for someone, right now"; (2) the `Portal` struct (§2.2) — the one data shape
that replaces having to reason about `Door` and `Teleport` as two unrelated things; (3) the
join-vs-create resolution rule (§2.3) — a five-line decision table, not a service to design from
scratch; (4) the village-fit formula (§4.2), computed directly from the `plan_of()` table that
already ships, so a mission-map author can check "does my map fit a tier-3 village" without asking
anyone; (5) the biome/weather/session determinants (§5) that decide what changes when a player
crosses a portal, cleanly separated from what RFC-014 and RFC-013 decide about *how long* an
instance lives and *what dying in it costs* — this RFC answers neither of those.

---

## Reference-level Design

### 1. Map identity and the variant taxonomy

#### 1.1 `MapId` partition (operationalizes `ARCHITECTURE.md §4`)

`MapId` becomes the runtime `std::uint16_t` value `ARCHITECTURE.md §4` already proposed, replacing
today's three-value enum (`kMapCount = 2`; `kOverworld = 0`, `kInterior = 1` — see `tiles.hpp`).
This RFC fixes the partition `ARCHITECTURE.md` left as an example:

```
MapId  ∈  [0, 65536)

[0, 16)      — PERSISTENT band. A map in this band exists for the life of the world, is never
               destroyed, and is at most one instance — there is exactly one running copy, ever.
                 0  = kOverworld   (unchanged — the 1024×1024 seamless world, RFC-021's subject)
                 1  = kInterior    (unchanged — the shared interior-room grid, kRoomPitch=16-
                                    subdivided; still persistent, still one map, not touched by
                                    this RFC)
                 2..15 = reserved for future persistent maps — e.g. GAME.md §3's "lục địa thứ hai"
                          (second continent), should the project ever add one. Unassigned today.

[16, 65536)  — INSTANCED band. A map in this band is created on demand and destroyed when its
               session ends (RFC-014, proposed, owns allocation/destruction mechanics). At any
               moment, only as many of these 65,520 slots are live as there are open instances —
               at the 20–50 concurrent player target (GAME.md §11) with dungeons sized for 2–4
               (GAME.md §3), that ceiling is never remotely approached; the width exists so
               allocation never needs a reuse/recycling scheme, not because the game needs it.
```

**This partition widens what `MapId` may *contain*; it does not, by itself, widen how the engine
*indexes* by `MapId` today, and this RFC does not claim otherwise.** The shipped chunk-addressing
path is dense and keyed on `map`: `chunk_index()` (`tiles.hpp:726-728`) computes
`c.map * kChunksPerMap + c.cy * kMapChunks + c.cx`, and `kChunkCount = kMapCount * kChunksPerMap`
(`tiles.hpp:39-40`) is the compile-time size both `SnapshotBus` (`snapshot.hpp:117-121`,
`slots_(kChunkCount)`, "sized for the whole world at construction") and `effect_tick`
(`client_main.cpp:490`) preallocate to. That scheme is correct and sufficient for `kMapCount = 2`;
fed the same formula unchanged against a 65,536-wide `MapId` space it would require on the order of
67 million preallocated slots, or produce out-of-bounds access the first time a `MapId` outside
`[0, kMapCount)` is actually used. This RFC does not resolve that redesign — it is a chunk-addressing/
storage concern, not a map-taxonomy one — but it names it explicitly as a dependency RFC-014 (or a
dedicated engine change ahead of RFC-014) must satisfy before any `MapId` in the instanced band
`[16, 65536)` can be used for real: the dense per-map array must become sparse (e.g. keyed only on
the set of currently-live `MapId`s, sized to the concurrent-instance count §1.1's own budget already
assumes, not to the full partition width) before `allocate_new` (§2.3) can hand out an instanced
`MapId` safely. `ARCHITECTURE.md §4`'s cited comment addresses *when* chunk actors spin up, not *how
they are indexed* once `MapId` stops being small and dense — the two are different problems, and this
RFC only solves the first.

This partition is a pure narrowing of `ARCHITECTURE.md §4`'s own suggested split (`0..15` /
`16..65535`) into a committed contract. `kOverworld` and `kInterior` keep their existing values —
this RFC does not renumber anything already shipped.

#### 1.2 `MapDescriptor` and the one sizing rule

Every map that exists — persistent or instanced — has one descriptor:

```
enum class MapCategory : uint8_t { kPersistent, kInstanced };
enum class WeatherMode : uint8_t { kAmbient, kFixed, kInherit };  // see §5.2

struct MapDescriptor {
    MapId          id;
    MapCategory    category;
    std::uint8_t   chunk_edge;       // 1..32 — the map is chunk_edge × chunk_edge chunks
    Ring           biome;            // a TAG, not a computed gradient — see §5.1
    WeatherMode    weather_mode;
    Weather        weather_fixed;    // meaningful only when weather_mode == kFixed
    bool           allow_free_build; // GAME.md §3's overworld-vs-realm build rule, recorded per map
};

edge_tiles(d)  = d.chunk_edge * kChunkTiles     // kChunkTiles = 32, unchanged
chunk_count(d) = d.chunk_edge * d.chunk_edge
```

**The sizing rule, stated once so it never needs restating:** `kMapTiles` (1024) is not a property
of "maps" — it is the value of `edge_tiles()` for exactly one `MapDescriptor`, the overworld's, at
`chunk_edge = kMapChunks = 32`. Every other map is the identical struct with a smaller
`chunk_edge`. There is no second terrain system, no second chunk-actor model, no second
`Terrain`/`Ring`/`Weather` type for "small maps" — a 3-chunk mission map and the 32-chunk overworld
are read, ticked, and LOD-managed by the same `ChunkActor` machinery (`ARCHITECTURE.md §4`'s
create/destroy-on-demand model, which RFC-014 will implement, already assumes exactly this: chunk
actors allocated per map, not per game). `chunk_edge` is a content parameter, full stop.

**Bound, not a target.** `chunk_edge = 32` (1024 tiles) is the ceiling GAME.md §3 fixed for the
persistent overworld and is not available to instanced maps by policy (§1.3) — not because the
system can't build one that large, but because nothing in the design calls for an instance that
size, and allocating 1024 chunk actors for a 2-player dungeon run would be a resourcing mistake this
RFC forecloses by table, not by hard limit (an author who genuinely needs a 32-chunk instance is not
stopped by any type in this RFC; §1.3's guidance table is a recommendation, not an enum constraint).

#### 1.3 Variant guidance table

All entries **(tunable)** except the `MapId` values and the overworld/interior rows, which are
as-built:

| Variant | `MapId` band | Typical `chunk_edge` | `edge_tiles` | Ring gradient? | Notes |
|---|---|---|---|---|---|
| Overworld | Persistent, `0` | 32 (fixed) | 1024 | **yes** — the only map that computes `ring_of()` | RFC-021's entire §1–§4 subject, unchanged |
| Interior room grid | Persistent, `1` | 32 (fixed) | 1024, room-subdivided | n/a | unchanged; `kRoomPitch = 16` |
| Second continent (GAME.md §3, not yet built) | Persistent, `2..15` | ≤32 | ≤1024 | its own, independently centred | see Open Questions §3 |
| Dungeon / trial instance | Instanced | 2–8 | 64–256 | no — single assigned biome tag | GAME.md §3: "phòng + hành lang" |
| Mine shaft floor | Instanced | 2–4 | 64–128 | no | one instance per depth floor (ROADMAP P4) |
| Rest realm (fishing lake, hot spring, cloud isle) | Instanced | 2–8 | 64–256 | no | hand-authored, per `ARCHITECTURE.md §8`: "cõi nghỉ có thể viết tay" |
| Mission/event map hosting a village | Instanced | 3+ (§4.2) | 96+ | no | this RFC's §4 |

**Why persistent maps get a ring gradient and instanced maps don't.** `ring_of()` (RFC-021 §2.1) is
parametrized on the map's own half-width — mathematically nothing stops it running on a 4-chunk map.
But the *design intent* behind it (GAME.md §4: Meadow must feel like "a whole country" you could
live in forever) does not scale down — a Meadow ring three chunks wide is not a country, it is a
lawn. Rather than let a formula produce a result its own justification contradicts, this RFC's rule
is definitional: only a map explicitly designated `category = kPersistent` with its own centre runs
the ring formula (RFC-021 §2.1, reused verbatim, re-centred). Every instanced map instead declares
one fixed `biome` tag (§5.1) — a single Ring value used purely to pick an atlas/particle flavor, the
same way a dungeon today has one tileset (`TilesetDungeon.png`), not five.

### 2. The portal system

#### 2.1 What already exists, generalized

Two portal-shaped things ship today and this RFC does not replace either — it names the shape they
both are an instance of:

- **`Door{tile, room}`** (`tiles.hpp`) — a sorted array, ~440 entries, mapping an overworld doorway
  tile to a fixed room index on `kInterior`. Statically known at world-gen time, never allocated,
  never shared/instanced.
- **`Teleport{map, x, y}`** (`protocol.hpp`) — the verb that actually moves a player: given a
  resolved destination, jump them there. `Teleport` does not decide *what* the destination is; it
  executes a destination someone else already chose.

Every portal this RFC adds — realm gates, mine mouths, mission portals, return portals — resolves to
exactly one `Teleport{map,x,y}` call once its target is known. `Teleport` is not touched by this RFC.
`Door` is the special case of `Portal` (§2.2) where the target is always known in advance and never
needs allocating — its existing sorted-array binary-search implementation can continue to serve that
one case unmodified; nothing here requires migrating the ~440 `Door` entries into the new struct on
day one.

#### 2.2 `Portal` data shape

```
using PortalId = std::uint32_t;   // stable identity, fixed-width so markers (RFC-021 §5.3), quest
                                   // binding (RFC-020), and replication (RFC-015) all key on the
                                   // same type; owned by this RFC, no other spec defines it
using GroupId  = std::uint16_t;   // player-group identity, fixed-width so `MapSession` can compare
                                   // it; this RFC only needs a key to compare for the join-vs-create
                                   // decision (§2.3) — what a "group" is, how it forms, belongs to a
                                   // future party/group RFC, unassigned in this batch (Open Question 8)

enum class PortalKind    : uint8_t { kInteriorDoor, kRealmGate, kMineMouth, kMissionPortal,
                                      kReturnPortal };
enum class RealmType     : uint8_t { kRest, kChallenge };          // RFC-021 §3.5, unchanged
enum class RealmFlavor   : uint8_t { kNone, kDungeon, kTrial, kFishingLake, kHotSpring,
                                      kCloudIsle, kSpiritRealm };   // display/content tag only
enum class PortalBinding : uint8_t { kFixedTarget, kAllocateOnUse };
enum class SessionScope  : uint8_t { kSharedPersistent, kGroupInstance, kSoloInstance };  // §2.3

struct Portal {
    PortalId       id;                    // stable identity — markers (RFC-021 §5.3), quest
                                           // binding (RFC-020), replication (RFC-015) all key on it
    MapId          from_map;
    std::uint16_t  from_x, from_y;        // the trigger tile
    PortalKind     kind;
    RealmType      realm_type;            // meaningful only when kind == kRealmGate
    RealmFlavor    flavor;                // meaningful only when kind == kRealmGate — see §3.2's
                                           // note for why kMineMouth does not use this field
    PortalBinding  binding;
    SessionScope   scope;                 // meaningful only when binding == kAllocateOnUse
    // kFixedTarget only:
    MapId          fixed_to_map;
    std::uint16_t  fixed_to_x, fixed_to_y;
};
```

Every `Portal` is a small, fixed-size struct with no text payload — the same replication-friendly
shape RFC-021 §5.3 already requires of every marker kind, satisfied here for the same reason (wire
cost, no localization lookup at replication time).

**`kInteriorDoor`** is `Door` restated in this shape: `binding = kFixedTarget`, `fixed_to_map =
kInterior`, `fixed_to_{x,y}` = the room's spawn tile. **`kRealmGate`** and **`kMineMouth`** are the
fixed overworld structures RFC-021 §3.5/§3.6 place (jitter grid, terrain requirement, ring bias,
keep-out) — this RFC does not re-specify their placement, only the data shape RFC-021's placement
rules populate. Both use `binding = kAllocateOnUse`, because their destination is not one fixed map —
it is "join or create an instance," resolved at the moment a player steps on them (§2.3).
**`kMissionPortal`** is the same struct used for a portal that does not come from worldgen's static
jitter-grid placement at all — one spawned at runtime by a quest or event (RFC-020's "rumor"
instances are the first example). Its lifecycle (when it appears, when it despawns) is entirely
RFC-020's / RFC-014's; this RFC only says that when one exists, it is a `Portal` like any other, not
a bespoke type. **`kReturnPortal`** is the exit every instanced map needs: `binding = kFixedTarget`,
`fixed_to_map`/`fixed_to_{x,y}` populated from the `MapSession` the player arrived through (§2.3) —
always exactly the tile they left, never a designer-chosen "exit point" that could differ from where
they entered.

#### 2.3 Session scope and the join-vs-create rule

This is the "lightweight session contract" the assignment calls for — narrow on purpose. It answers
one question: **when a player uses a portal with `binding = kAllocateOnUse`, do they join an
instance that already exists, or is a new one created?** It does not answer how long that instance
lives, what happens when it's idle, or how it survives a leader restart — RFC-014 (proposed) owns
all of that.

```
struct MapSession {
    MapId          map_id;         // the resolved target — a persistent id, or a freshly
                                    // allocated id in [16, 65536)
    PortalId       origin_portal;
    SessionScope   scope;
    GroupId        owner_group;    // 0 when scope == kSharedPersistent
    MapId          return_map;     // where this session's kReturnPortal sends a player
    std::uint16_t  return_x, return_y;
};
```

Resolution rule, evaluated on the trusted leader (§ Multiplayer):

```
resolve(portal, player, group):
    if portal.binding == kFixedTarget:
        return the one MapSession already bound to portal.fixed_to_map   // Door/kInteriorDoor,
                                                                          // kReturnPortal
    switch portal.scope:
        kSharedPersistent:
            return the single running MapSession for this portal, creating it once if it has
            never run (e.g. a future persistent-band map reached by an actual `Portal` — there is
            exactly one destination, shared by everyone who ever uses it, exactly like the overworld
            today; whether the second continent's own bến-tàu crossing is `Portal`-shaped at all is
            unresolved — see the caveat under the table below and Open Question 3)
        kGroupInstance:
            existing := find open MapSession where origin_portal == portal.id
                        and owner_group == group and not (full or closing)
            return existing if found, else allocate_new(portal, owner_group = group)
        kSoloInstance:
            return allocate_new(portal, owner_group = player)
```

`allocate_new` — picking an unused `MapId` in the instanced band, spinning up its chunk actors, and
registering the `MapSession` — is exactly the `InstanceManager` flow `ARCHITECTURE.md §4` sketches
and RFC-014 will specify in full; this RFC's contribution stops at "here is the decision that
precedes the call," stated as data (`SessionScope`) a designer sets per portal, not logic RFC-014
has to reverse-engineer from prose.

| Portal target | Recommended `scope` | Why |
|---|---|---|
| Second continent / other persistent maps | `kSharedPersistent` **(if reached via `Portal` at all — see caveat below)** | one world, one instance, matches the overworld's own model, *if* the connection turns out to be `Portal`-shaped |
| Dungeon / trial | `kGroupInstance` | GAME.md §3: "một bản riêng mỗi nhóm" — the explicit reason instances exist at all |
| Mine shaft floor | `kGroupInstance` | consistency with dungeons; a shared floor would let one group's clearing rob another's run **(tunable — could be `kSoloInstance` if solo mining turns out to be the common case)** |
| Rest realm | `kGroupInstance` **(tunable)** | no combat means less need for strict isolation, but keeping the group you walked in with together matches the cozy, walk-in-together framing of GAME.md §1; a designer may set `kSoloInstance` per realm if a solitary-fishing feel is wanted instead |
| Mission portal (RFC-020) | set per quest template | RFC-020 decides per content; this RFC only supplies the field |

**Caveat on the second-continent row.** GAME.md draws a deliberate distinction this table must not
paper over: line 149 states "Không có cổng nào nối hai vùng mặt đất với nhau" (no gate connects two
ground regions to each other), and line 209 describes a second continent, if built, as reached "nối
bằng bến tàu" (connected by ferry) — a different word for a different mechanic, not a `cổng`/portal.
The row above is this RFC's working assumption for *if* that crossing ends up `Portal`-shaped, not a
claim that it will — whether a ferry crossing uses `Portal`/`MapSession` machinery at all, or is an
entirely separate mechanic this RFC does not model, is open (Open Question 3), consistent with §1.3's
own "not yet built" framing for the same row.

#### 2.4 What this section does not decide

`allocate_new`'s internals (chunk-actor spin-up cost, idle timeout, capacity cap, what happens if the
leader dies mid-session) — RFC-014. Per-realm atlas load/unload on session start/end —
RFC-014/ARCHITECTURE.md §4, unchanged, only referenced here. What happens to a player who disconnects
or dies inside a `MapSession` — RFC-013. The wire encoding of a `Portal` or `MapSession` for
replication — RFC-015.

### 3. Structures within a map

#### 3.1 Stamped structures — house and tent (unchanged, recorded)

`StructureKind` (`village.hpp`) enumerates fifteen dwelling/decoration sprites plus the six rampart
pieces — every one of them a single multi-tile sprite, stamped whole, with solid collision over its
whole footprint (`village.hpp`'s own header comment: "a house is a house, drawn in one quad"). The
two families this RFC's scope names:

| Kind | Footprint | Source tileset | Placed by |
|---|---|---|---|
| `kHouseOrange/Cream/Amber/Red/Blue/Tan/Wood` (7 kinds) | 4×3 or 3×3 | `TilesetHouse.png`, 33×23 tiles (`CREDITS.md:39,83`) | `VillageBuilder` (`village.hpp`), any village on any map |
| `kTentA/B/C` (3 kinds) | 3×3 | `tileset_camp.png`, 23×9 tiles (`CREDITS.md:84`) | stronghold/camp builder, same file |

Nothing about this RFC changes `StructureKind`, `plan_of()`, or the builder. The only new fact this
RFC states is that **`VillageBuilder` is map-size-agnostic by construction** (§ Motivation point 3) —
it is called identically whether the overlay it writes into belongs to the 1024-tile overworld or a
96-tile mission map. §4 below is the direct consequence.

#### 3.2 Zone/instance entrances — mine and dungeon (not `StructureKind`)

`StructureKind` has no `kMine` and no `kDungeon` entry, and this RFC does not add one. The reason is
categorical, not an oversight: every `StructureKind` is a *building* — walkable-around, with a door,
with collision, sometimes with a dwelling interior a villager lives in. A mine mouth or dungeon mouth
is none of those things — it is a **`Portal`** (§2.2, `kind = kMineMouth` or `kind = kRealmGate,
flavor = kDungeon`), a small trigger area rendered with its own prop sprite, that does not participate
in `village.hpp`'s footprint/door/collision machinery at all.

| Entrance | Data type | Placed by | Overworld-side prop source | Interior-side tileset |
|---|---|---|---|---|
| Mine mouth | `Portal{kind: kMineMouth}` | worldgen jitter-grid (RFC-021 §3.6, unchanged) | not pinned by the 2026-07-24 asset audit — see Open Questions §5 | Roguelike Caves & Dungeons, `assets/_src/caves/` (`CREDITS.md:14`) |
| Dungeon mouth | `Portal{kind: kRealmGate, realm_type: kChallenge, flavor: kDungeon}` | worldgen jitter-grid (RFC-021 §3.5's realm-gate placement, unchanged) | not pinned — see Open Questions §5 | `TilesetDungeon.png`, 12×4 tiles (`CREDITS.md:87`); ambient track "21 - Dungeon.ogg" |

**A note on the mine-mouth marker's ore-tier icon.** RFC-021 §5.3 requires the Mine mouth marker to
show "position, ore-tier icon of the shallowest exposed tier" — that is per-instance dynamic data
(which ore tier currently sits nearest the surface), not a static display tag, so it does not belong
in `RealmFlavor` (§2.2, which `Portal` uses only for `kRealmGate`) or anywhere in `MapDescriptor` as
specified here. This RFC does not specify where that data lives; flagged as Open Question 7.

This table is the direct answer to the grounding fact that motivated it: mines and dungeons are
**zone/instance entrances**, specified here as `Portal` data, never as a new `StructureKind`
placement class. Adding `kMine`/`kDungeon` to `StructureKind` would conflate "a building with a door"
with "a trigger tile that resolves a `MapSession`" — two different systems that happen to render at
roughly the same footprint.

### 4. The village-always-fits invariant

#### 4.1 What "fit" means, precisely

A village template, at any of `plan_of()`'s shipped tiers, needs its full enclosure **plus the gate
approach tile `gates_of()` places three tiles beyond each wall** (`village.hpp`: `cx - p.hw - 3`
etc.) to be inside the map, or the village generates with a gate whose approach tile has nowhere to
stand — the exact class of bug RFC-021 §3.3 describes ROADMAP.md's R6 milestone hitting for
roads-vs-walls. That gives one footprint per tier:

```
full_width(tier)  = 2 * (plan_of(tier).hw + 3) + 1
full_height(tier) = 2 * (plan_of(tier).hh + 3) + 1
```

A map hosts a village at `tier` **iff** `edge_tiles(map) > max(full_width(tier), full_height(tier))`
— strict inequality, so at least one valid centre position exists with the whole footprint inside the
map on all four sides.

**This RFC uses a stricter margin than worldgen's own current per-village edge check, and says so
plainly.** `worldgen.hpp`'s real (non-coarse) edge test uses `plan.hw + 2` / `plan.hh + 2`, one tile
short of the `+3` `gates_of()` actually needs for the gate's approach tile. On the 1024-tile
overworld this one-tile gap has never been observed to matter — jittered placement essentially never
lands a village at the exact minimum margin. On a snug, purpose-built minimum-size mission map, it
is far more likely to matter, because the whole point of such a map is to sit close to the minimum.
This RFC's `+3` formula is therefore the correct one to build new tooling against; the pre-existing
`+2` in `worldgen.hpp` is flagged, not silently adopted (see Open Questions §1).

#### 4.2 The fit table

Computed directly from `plan_of()`'s shipped values (`village.hpp`) — not estimated:

| `plan_of()` tier | `hw`, `hh` | `full_width` × `full_height` | Theoretical min map edge | Min `chunk_edge` (2×2 = 64 tiles) | Practical min `chunk_edge` incl. 1-chunk margin **(tunable)** |
|---|---|---|---|---|---|
| 1 | 13, 11 | 33 × 29 | 33 | 2 | 3 (96 tiles) |
| 2 | 19, 16 | 45 × 39 | 45 | 2 | 3 (96 tiles) |
| 3 | 19, 21 | 45 × 49 | 49 | 2 | 3 (96 tiles) |
| 4 | 25, 21 | 57 × 49 | 57 | 2 | 3 (96 tiles) |
| 5 (default) | 25, 26 | 57 × 59 | 59 | 2 | 3 (96 tiles) |

**The load-bearing result: every village tier the game ships, including the largest, fits inside a
2×2-chunk (64×64-tile) map with margin to spare** (worst case, tier 5: 64 − 59 = 5 tiles of total
slack). A **3×3-chunk (96×96-tile)** map fits all five tiers comfortably, with room left over for a
road stub to the portal and the fort/mine-style no-build apron a mission map's own content might
want. There is no tier for which "the map is too small to hold a real village" is a genuine
constraint at any size a mission map would plausibly ship at — the constraint that actually binds is
almost always narrative (how big should this place feel), not geometric.

#### 4.3 Tier is independent of map size — services don't shrink

The village-tier service table (GAME.md §6, recorded normatively by RFC-019/RFC-020) is a function of
**tier**, not of host-map size: `VillageBuilder::parcels()` places the market yard from tier 2, the
hall from tier 3, and so on (`village.hpp`), and none of that code path reads the size of the map it
is writing into. **Consequence: a village built at the same shipped `plan_of()` tier has identical
market, quest-giver, and respawn-point services regardless of which map hosts it — a tier-2 village
on a 96-tile mission map is exactly as service-complete as a tier-2 village on the 1024-tile
overworld.** (This claim compares two villages at the same shipped `tier` value to each other; it does
not by itself say which GAME.md bậc either one renders as, or equate shipped `tier` with RFC-020's
`unlock.village_tier_min` bậc-scale field — that mapping is a separate, currently-unresolved question,
flagged in §4.4 below, not assumed here.) Nothing about hosting a village on a small map is a lesser
version of hosting one on the big map — it is the same village, the same builder call, a smaller walk
to its wall. A mission-map author who wants a given tier's services picks that tier from the table
above and gets a map at least 64×64 (practically 96×96) tiles; they do not get, and do not need, a
"compact village" variant.

Whether a village's respawn-point service is honored as an actual player-respawn target while the
player is inside an instanced map — versus death inside any challenge-type instance always ejecting
to the portal of origin, per GAME.md §3's dungeon/mine row ("bị đẩy ra ngoài, mất đồ mang theo") — is
RFC-013's (proposed) decision, not this RFC's; §4.3's claim is only that the *service exists
identically*, not that every consumer of it is reachable from every map category.

#### 4.4 A pre-existing tier-numbering caveat, flagged not resolved

GAME.md §6 and RFC-019/RFC-020 (`unlock.village_tier_min ∈ 0..4`) number village tiers **0–4** (Camp
/ Hamlet / Village / Town / Citadel — 0 = Camp has no rampart, no market, no quests). The shipped
`village.hpp`/`worldgen.hpp` `tier` field is **1–5** (`plan_of()`'s switch has no `case 0`;
`tier_for()` never returns 0), and every `plan_of()` entry — including tier 1 — builds a full
rampart, which is inconsistent with GAME.md bậc-0's explicit "no walls" framing. This RFC's §4.2
table is keyed to the shipped `1..5` range exactly as `plan_of()` defines it, and this RFC does
**not** assert a mapping from shipped `tier` to GAME.md bậc (e.g., "shipped tier 1 = bậc 0" would
contradict bậc 0's no-rampart rule; "shipped tier 1 = bậc 1" leaves bậc 0 with no code path at all).
Resolving that mapping is `VillageActor`'s concern (GAME.md §6, not owned by any numbered RFC in this
batch) — flagged here because §4.2's numbers are precise only against the shipped field, and a future
`VillageActor` spec must state which bậc each shipped `plan_of()` tier actually renders as before this
RFC's table can be read against GAME.md's bậc column directly.

### 5. Inter-map connectivity: biome, weather, session

#### 5.1 Biome — a tag, not a computed gradient

Every non-overworld `MapDescriptor` carries one `Ring` value as a flavor tag (§1.2) — it selects an
atlas/particle palette and, where relevant, the ambient hazard flavor (§ ARCHITECTURE.md-adjacent
systems like crop-freeze or Wet-application), but it is never computed from the map's own geometry
the way `ring_of()` computes it for the overworld (§1.3). A designer authoring a rest realm picks
`Ring::kSnow` because a frozen hot-spring courtyard is the content they want, not because the realm
sits at some notional distance from a centre it does not have.

#### 5.2 Weather mode

```
WeatherMode::kAmbient  — the map ticks weather exactly like the overworld: weather_of(biome) plus
                          the existing seasonal/time-of-day weather director (GAME.md §9). Suits a
                          rest realm meant to feel like an outdoor extension of the world.
WeatherMode::kFixed    — one Weather value, forever. This is the normative case for dungeon and
                          mine interiors: GAME.md §3's own table states weather is simply absent
                          underground ("không (trong lòng đất)") — weather_fixed = Weather::kNone.
WeatherMode::kInherit  — reserved for a realm whose ambience should track the overworld's current
                          weather at the moment of entry without ticking independently afterward
                          (tunable — no current content needs this; included because GAME.md §1's
                          "vùng đất linh hồn nơi thời gian trôi chậm" (a spirit realm where time
                          moves slowly) suggests a realm that freezes its weather at entry rather
                          than either running its own clock or having none, and this is the natural
                          third option once it's needed).
```

#### 5.3 What determines connectivity vs. what determines experience

Biome and weather are **presentational**, not gating — GAME.md §3's whole argument for instanced
realms ("mỗi cõi một bộ asset ... không hề chỏi — nó là lý do người chơi muốn đi") is that mismatch
between the overworld and a realm is the point, not a compatibility problem to solve. No portal in
this RFC checks the source and target `biome`/`weather_mode` against each other before allowing use.
What actually gates a crossing is narrower:

| Determinant | What it decides | Owner |
|---|---|---|
| `PortalKind` + `RealmType` (§2.2) | Where a portal is placed and what icon it shows on the Map (RFC-021 §5.3) | RFC-021 (placement), this RFC (data shape) |
| `SessionScope` (§2.3) | Whether crossing joins an existing instance or creates a fresh one | This RFC (contract), RFC-014 (mechanics) |
| `MapDescriptor.allow_free_build` (§1.2) | Whether the target map permits player building at all | GAME.md §3 (already decided: overworld free, dungeon/mine forbidden); this RFC only records it per map |
| Death/recovery rule at either endpoint | Respawn vs. ejection-with-item-loss | RFC-013 (proposed) — not this RFC |
| Who/what populates the target map | Villagers, guards, monsters | RFC-023 (sibling) — not this RFC |

#### 5.4 Crossing-a-portal experience checklist

What a player experiences the instant a `Teleport` resolved from a `Portal` lands them on the target
map, purely as a function of the target's `MapDescriptor` and this RFC's data — nothing here is new
mechanics, only the composition of mechanics that already exist:

1. **Atlas swap.** The target map's atlas loads, the source's frees if no player still needs it
   (GAME.md §3, `ARCHITECTURE.md §4` — unchanged, this RFC only confirms the trigger is "arrived on a
   `MapDescriptor` with a different atlas," which every non-overworld map has by construction).
2. **Ambience change.** Particle layer and weather state switch to the target's `WeatherMode` (§5.2)
   — instant for `kFixed`, continuous for `kAmbient`.
3. **Build-permission change**, if any, per `allow_free_build`.
4. **Combat-status interaction**, where applicable — e.g., a `kFixed` snow-flavored realm's status
   interplay (Wet/Frozen, RFC-002/RFC-009) behaves exactly as it does in the overworld's own winter,
   because it is driven by `Weather`, not by which map it's read on; this RFC introduces no new status
   math, only confirms the same enum drives it everywhere.
5. **Session membership**, per §2.3 — solo, shared with your group, or shared with the whole server.
6. **Everything this RFC explicitly does not decide** — death rules (RFC-013), who else is standing
   there (RFC-023), what loot exists (RFC-018) — is unaffected by crossing the portal itself; those
   systems read the target `MapId` the same way they would read any other.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001..010 (accepted combat set)** | Not load-bearing here. Where a `kFixed`-weather realm's ambience interacts with status effects (§5.4 point 4), the math is entirely RFC-002's/RFC-009's; this RFC only confirms the same `Weather` enum drives it off-overworld. |
| **RFC-007** (accepted, RL) | Owns the RL policy that would run inside a dungeon/fort map this RFC's `MapDescriptor` describes; this RFC places the map, RFC-007 decides who trains in it. |
| **RFC-013** (proposed, Vitals/Death/Recovery) | Owns what happens to a player who dies inside any map this RFC describes, at any portal endpoint — respawn-at-hearth vs. ejection-with-item-loss. This RFC only defines the portal/session that determines *where* a respawn or ejection would land, not the consequence itself. |
| **RFC-014** (proposed, Instance & Realm Lifecycle) | Owns everything §2.4 explicitly excludes: `allocate_new`'s internals, idle timeout, capacity, leader-death recovery, per-realm atlas load/unload mechanics. This RFC's `MapId` partition (§1.1) and `MapSession`/join-vs-create contract (§2.3) are the shape RFC-014 implements against — this RFC is a narrower, upstream contract, not a competing spec. |
| **RFC-015** (proposed, Client Replication) | Owns the wire encoding for `Portal` and `MapSession` state and for map-crossing transition messages; this RFC only fixes their in-memory shape and size (small, fixed, no text — §2.2). |
| **RFC-016** (proposed, Persistence & Save-File Format) | Owns how persistent-band `MapDescriptor`s (§1.1, ids 2–15 if any are added) and any per-player map-crossing state are serialized; instanced-band maps (§1.1) are, by definition, not persisted across a world restart unless RFC-014 says otherwise. |
| **RFC-018** (proposed, Loot, Essence & Reward Tables) | Owns what a mine or dungeon pays out; this RFC places the mine mouth/dungeon mouth (§3.2) and says nothing about their contents. |
| **RFC-019** (accepted, Progression & Skills) | Owns the XP/skill-branch consequence of any action taken on any map this RFC describes, including inside instanced mines/dungeons — unaffected by map size or category. |
| **RFC-020** (accepted, Mission & Quest System) | Owns when and why a `kMissionPortal` (§2.2) is spawned or torn down (its "rumor" dungeon-gate instances are the first concrete example); this RFC supplies the `Portal`/`MapSession` data shape RFC-020's quest triggers populate, not the trigger logic itself. |
| **RFC-021** (accepted-with-revisions, World Structure, Map & Wayfinding) | Partially superseded — see [Relationship to RFC-021](#relationship-to-rfc-021) below. |
| **RFC-023** (sibling, this batch, Character & NPC Roster System) | Owns every NPC/monster/villager population detail for any map this RFC describes — village residents, dungeon monsters, mine guards. This RFC places the stage (village footprint, portal, session); RFC-023 populates it. |

---

## Relationship to RFC-021

**This RFC supersedes RFC-021 in part: it narrows RFC-021's implicit scope claim from "the map" to
"the persistent overworld map," and it makes one small, justified amendment to RFC-021's Map-screen
marker taxonomy. It does not reopen, re-derive, or contradict any of RFC-021's geometry, worldgen
invariants, wayfinding mechanics, or discovery model — all of that stands as written, cited by
number.**

**What stands entirely unchanged, cited not reproduced:** RFC-021 §1 (world frame constants), §2
(the five-ring Chebyshev geometry and `kRingEdge` table), §3.1–§3.4 (worldgen placement order,
no-build zones, gates-aimed-at-gates rule, the raid-marker display contract), §3.5–§3.6 (realm-gate
and mine placement rules — jitter cell, terrain requirement, ring bias, keep-out), §4 (mounts, the
waypoint network, roads-as-legibility) and all of §5 except the one line amended below (the Map
screen, the discovery/fog model, the marker replication budget). Every one of these describes the
persistent overworld specifically, and this RFC's own §1.3 states explicitly that the overworld is
where they apply — RFC-022 does not narrow their *content*, only confirms they were never meant to
describe every map, because RFC-021 never claimed a second map existed to describe.

**What this RFC actively supersedes:**

1. **The framing that the 1024×1024 overworld is the game's entire map model.** RFC-021's Summary and
   Motivation describe the overworld as "the shipped ... contract" with no qualification, and its own
   `grep` for `MapId` (§ Motivation point 1) returns nothing. RFC-021 does discuss other maps by name —
   §3.5 states that stepping onto a realm gate "allocates an instance ... and teleports the player and
   their group into it," §5.3's marker table has dedicated rows for Dungeon mouth, Mine mouth, and
   Realm gate, and its own Non-goals section says "Instance lifecycle internals ... belong entirely to
   RFC-014." What RFC-021 never does, despite naming these destinations, is formalize a `MapId`
   partition, a `Portal` data shape, or a session join-vs-create contract for any of them — even though
   `ARCHITECTURE.md §4`'s own `MapId` sketch (which predates RFC-021) already assumed exactly that gap
   existed. This was not a wrong decision on RFC-021's part — its brief was the overworld, and RFC-021
   correctly declined to invent instance internals outside that brief. But that left a gap: nothing
   established the overworld as *one persistent map among a family*, with a defined partition, a
   defined portal data shape, and a defined session-join contract, rather than simply *the map*.
   RFC-022's §1 and §2 fill that gap. RFC-021's content is unchanged; its unstated singular-map framing
   is superseded.

2. **RFC-021 §5.3's marker taxonomy listed "Dungeon mouth" and "Realm gate" as two separate marker
   rows**, each with its own realm-type icon, without ever specifying two different placement
   mechanisms for them — §3.1 step 7 places every realm gate (rest and challenge alike) in one pass,
   and §3.5 is the only placement subsection for any of them; RFC-021 never wrote a §3.x for "dungeon
   mouths" as a distinct class. This RFC's `PortalKind::kRealmGate` with a `RealmFlavor` payload field
   (§2.2, §3.2) folds the two rows into one `PortalKind`, with "dungeon" as one `RealmFlavor` value
   among several — a marker-taxonomy amendment, not a placement-rule change: the Map screen still
   renders the same distinct icon per flavor RFC-021 §5.3 intended, satisfying its display contract
   unchanged; only the underlying type that produces the icon is unified.

**What is deliberately left alone.** RFC-021's Open Question 3 flags that its realm-gate/mine jitter
cells (160/140 tiles) were chosen by analogy, untested by simulation — this RFC does not touch that
tuning, because it is entirely about the *overworld's* placement density, outside this RFC's map-
taxonomy/portal-data scope. RFC-021's Review Record's one unresolved item (a possible missing
realm-gate no-build zone) is likewise untouched — it is a worldgen no-build-zone question, RFC-021's
domain, not this RFC's.

---

## Multiplayer & Simulation-LOD Considerations

- **Chunk-actor cost scales with `chunk_edge`, and instanced maps are cheap by construction — with
  one named exception.** A minimum village-hosting mission map (§4.2, `chunk_edge = 3`) is 9 chunk
  actors; the overworld is 1024. `ARCHITECTURE.md §4`'s create/destroy-on-demand model (which RFC-014
  implements) was already designed around exactly this per-map chunk-actor variance. What it was
  *not* designed around, and what this RFC does not claim is already solved, is the dense per-map
  indexing scheme (`chunk_index()`, `SnapshotBus`, `effect_tick` — §1.1's addressing note) that the
  current `kMapCount = 2` world relies on; widening `MapId`'s range (§1.1) requires that indexing to
  become sparse before instanced maps can be allocated safely, and that redesign is an explicit,
  named prerequisite for RFC-014, not load this RFC's plan already budgeted for.
- **LOD (10 Hz active / 1 Hz background / sleep) applies identically inside any map this RFC
  describes**, including instanced ones — a `PathfieldActor` per map (`ARCHITECTURE.md §5`, "một
  `PathfieldActor` mỗi bản đồ") already generalizes across map count, not just map size; this RFC
  adds no new field-rebuild trigger.
- **Portal/session resolution (§2.3) is a leader-only decision**, exactly mirroring the existing
  `Require<Trusted>` `InstanceManager` flow `ARCHITECTURE.md §4` already specifies — no new trust
  boundary is introduced, and `resolve()`'s output must be identical regardless of which node asks,
  since group rosters and open-session state are leader-owned facts, not locally computed ones.
- **Determinism.** Persistent-band maps beyond the overworld (§1.1, ids 2–15) that use a computed
  ring gradient (§1.3) inherit the overworld's cross-platform bit-exactness requirement
  (RFC-021 §1) unchanged. Instanced maps that are hand-authored (§1.3: rest realms, dungeon room
  layouts) trivially satisfy the same requirement by having no seed-derived placement to diverge on
  — a fixed layout is identical on every node by definition, which is precisely why
  `ARCHITECTURE.md §8` recommends hand-authoring small, frequently-visited realms in the first place.
- **`MapSession` join-vs-create resolution (§2.3) must be integer-deterministic**, consistent with
  the project-wide "everything derived from seed or from leader-observed state must not race on a
  tie-break" discipline `village.hpp`'s header comment states for placement math generally — two
  players stepping on the same `kGroupInstance` portal in the same tick must resolve to the same
  session (join, not double-create), which the leader-only evaluation above already guarantees by
  construction (single evaluator, no race).
- **20–50 concurrent players, no VPS.** Nothing in this RFC's data shapes assumes any node beyond the
  first-node-as-leader model already established; `MapSession` and `Portal` resolution add fixed,
  small structs to state the leader already owns (group rosters, open instances), not a new service.

---

## Tone Guardrail Compliance

Walking every mechanic this RFC introduces against GAME.md §0's test — does anything here count down
behind the player's back, or create obligation pressure:

1. **Map variant taxonomy and `MapDescriptor` (§1).** Entirely static content description — a map's
   size, biome tag, and weather mode are authored properties, not state that changes over time except
   through the same event-driven mechanisms (season change, village tier-up) GAME.md §6/§9 already
   defend elsewhere. Nothing here ticks.

2. **Portals and `PortalBinding`/`SessionScope` (§2).** A portal, once placed, does not close itself,
   does not have a use-count, and does not expire. `kAllocateOnUse` resolution is instantaneous
   (a lookup-or-create, not a wait); nothing about it depends on when in a real-time window a player
   acts. If a future content author wants a genuinely time-boxed community event map, that is a
   content-calendar decision entirely outside this RFC's scope — this RFC neither creates nor requires
   time-boxed maps, and the portal mechanism itself carries no expiry field.

3. **Return portals (§2.2, `kReturnPortal`).** No cost, no cooldown — unlike RFC-021 §4.3's waypoint
   network (which deliberately charges a flat resource fee as a one-off spending decision, argued
   clean there), a return portal is simply the way out of a place you walked into, and charging for it
   or delaying it would be pure friction with no design purpose. Always available the instant a player
   wants to leave.

4. **Instance idle/teardown (§2.4, explicitly deferred to RFC-014).** This RFC does not specify when
   an instance closes, and specifically does not specify a countdown for it. Whatever RFC-014
   eventually decides here must independently pass this same test — an idle-timeout policy that is
   silent (a session simply stops existing after the last player leaves and some grace period elapses,
   never displayed as a shrinking number) is consistent with the "chững, không tụt" pattern GAME.md §6
   already established for villages; a *displayed* countdown inside an instance would not be, and this
   RFC's own contract (§2.3) contains no field that could carry one.

5. **Village-always-fits (§4).** Placing a village on a small map never produces a *lesser* village —
   §4.3 is explicit that tier, not map size, determines services. There is no "the map was too small
   so you get fewer quest-givers" penalty for a player to discover and resent; a mission-map author
   either includes a tier's worth of services or doesn't, as an authored choice, never as decay.

6. **Biome/weather/session on crossing (§5).** Presentational only (§5.3) — nothing about crossing a
   portal starts a clock, and `WeatherMode::kFixed` (the normative dungeon/mine case) is explicitly
   the *absence* of any weather variable to decay or regrow, matching GAME.md §3's own "không (trong
   lòng đất)" line.

No mechanic in this RFC creates a deadline, a decaying resource, a login-frequency dependency, or a
countdown the player did not personally choose to start.

---

## Open Questions

1. **The `+2` vs `+3` gate-apron margin discrepancy in `worldgen.hpp` (§4.1).** This RFC's fit formula
   uses the stricter `+3` (matching `gates_of()`'s actual approach-tile placement); the shipped
   per-village edge test uses `+2`. On the 1024-tile overworld this has apparently never surfaced as a
   visible bug. Before any tooling builds mission maps at or near the theoretical minimum (§4.2's
   2-chunk row), this one-tile gap should get a real audit — it is exactly the kind of thing that is
   invisible on a spacious map and immediately visible on a snug one.

2. **The shipped `tier 1..5` vs. GAME.md `bậc 0..4` numbering (§4.4).** Left explicitly unresolved
   here — flagged as a fact a future `VillageActor` spec (unowned) needs to settle before §4.2's table
   can be read against GAME.md's bậc column with full confidence.

3. **Second-continent ring gradient (§1.3 table).** If GAME.md §3's "lục địa thứ hai" is ever built,
   does it get its own independently-centred five-ring gradient (this RFC's working assumption,
   consistent with `ring_of()`'s parametrization), or does a second landmass deliberately skip the
   ring model entirely (flat, or a different difficulty axis)? GAME.md §3 explicitly warns against
   starting with multiple regions ("đừng bắt đầu bằng nhiều vùng") and does not commit to an answer
   for when one eventually exists. Premature to pin down now; flagged so it isn't silently assumed
   later.

4. **Rest-realm default `SessionScope` (§2.3 table).** Proposed `kGroupInstance` by analogy to
   dungeons/mines, marked tunable. Worth a genuine design pass once P8 (ROADMAP.md) builds the first
   rest realm — a solitary-fishing feel might argue for `kSoloInstance` as the actual default, with
   `kGroupInstance` as the exception for realms explicitly meant to be shared.

5. **Overworld-side mine-mouth and dungeon-mouth prop sprite.** The 2026-07-24 asset audit pinned the
   *interior* tileset for both (`TilesetDungeon.png`, Kenney Caves & Dungeons) but did not identify a
   specific *exterior* mouth/entrance prop. This is an asset-inventory gap for a future audit pass, not
   a design gap this RFC can resolve from the grounding it was given.

6. **Should instanced maps ever be allowed the full 32-chunk ceiling?** §1.2 states nothing in the
   type system forbids it, and §1.3's guidance table is a recommendation, not a hard cap. No number is
   pinned here for an upper practical bound on instanced `chunk_edge` — left for RFC-014 or a future
   tuning pass once real chunk-actor allocation costs are measured under P4's instance infrastructure.

7. **Where does the mine-mouth ore-tier icon's data live? (§3.2)** RFC-021 §5.3 requires the Mine
   mouth marker to show an ore-tier icon of the shallowest exposed tier — per-instance dynamic data
   with no home in `RealmFlavor` (a static tag, §2.2) or `MapDescriptor` as specified here. Left for a
   future revision of this RFC or a `Portal`-adjacent field RFC-014/RFC-021 supplies.

8. **`GroupId`'s owning RFC (§2.2, §2.3).** This RFC defines `GroupId` only as a fixed-width key for
   the join-vs-create comparison in `resolve()` — what a "group"/party actually is, how it forms and
   dissolves, is not specified here and has no owning RFC in README.md's proposed table (RFC-011..018)
   today. Flagged so a future party/group RFC is not silently assumed to already exist.

---

## Non-goals

- **Deep instance-lifecycle mechanics.** Allocation internals, per-group binding beyond the
  join-vs-create decision (§2.3), idle timeout, capacity limits, leader-death recovery, and per-realm
  atlas load/unload mechanics — entirely RFC-014's (proposed).
- **NPC and monster population of any map.** Village residents, wandering merchants, guards, dungeon
  monsters — entirely RFC-023's (sibling, this batch).
- **Death, respawn, and recovery rules at any portal endpoint.** RFC-013's (proposed).
- **Loot, Essence, and reward tables for mines and dungeons.** RFC-018's (proposed).
- **Quest/mission content and triggers**, including exactly when and why a `kMissionPortal` spawns or
  despawns. RFC-020's (this RFC only supplies the `Portal` data shape it would use).
- **Wire encoding and replication budget for `Portal`/`MapSession` state.** RFC-015's (proposed).
- **Persistence/save-file encoding** of any `MapDescriptor`, `Portal`, or `MapSession` data.
  RFC-016's (proposed).
- **Worldgen placement algorithms for the overworld** — ring geometry, jitter grids, village/
  stronghold/mine/gate placement order and rules. RFC-021's, unchanged, cited not reproduced.
- **Rendering** — camera, sprites, icons, atlas packing mechanics. Out of this RFC, as it was out of
  RFC-021's (`RENDER_SPEC.md`).
- **Raid probability math and village-tier-up/tier-down mechanics.** GAME.md §6 direct / unassigned
  in this batch, referenced not re-specified.
- **PvP.** Off by default (GAME.md §11); no PvP-specific map or portal behavior specified.

---

## Review Record

**Reviewer A: revise.** **Reviewer B: revise.** Both reviewers converged on the same nine issues
(re-verified independently against `src/`, RFC-021, RFC-020, and GAME.md); all nine are applied below.

Applied changes:
- §1.1: added a named blocker — `chunk_index()`/`SnapshotBus`/`effect_tick` are dense arrays sized to `kMapCount=2`; widening `MapId` requires a sparse redesign this RFC defers to RFC-014, not "already budgeted for."
- Multiplayer & Simulation-LOD: corrected the "nothing adds load the existing plan didn't budget for" claim to name the same dense-addressing gap.
- Relationship to RFC-021 §1: replaced the false "RFC-021 never states other maps exist" claim with the accurate, narrower gap (no `MapId` partition/`Portal`/session contract), citing RFC-021 §3.5/§5.3/Non-goals.
- §2.3 table + `resolve()`: flagged the second-continent/ferry row as provisional, added a caveat citing GAME.md's cổng-vs-bến-tàu distinction.
- §4.3: reworded the worked example to compare same-tier villages only, explicitly disclaiming the tier↔bậc mapping §4.4 leaves open.
- §1.3 table: fixed "Second continent" row's cross-reference from Open Questions §1 to §3.
- §2.2: added explicit `PortalId`/`GroupId` typedefs; flagged `GroupId`'s owning RFC as unassigned (new Open Question 8).
- §4.1: fixed the R6 citation from "worldgen.hpp's own comments" to RFC-021 §3.3 (citing ROADMAP.md).
- §2.2/§3.2: narrowed `RealmFlavor` to `kRealmGate` only; added a note that the mine-mouth ore-tier icon has no data home yet (new Open Question 7).
- Header: split RFC-020 out of the "(proposed)" depended-on-by line — it is accepted-with-revisions.
- Status header updated to reflect this revision pass.

Unresolved: none of the two reviewers' final mustFix items were left unaddressed. One soft item raised only in Reviewer B's concessions (not in either final mustFix list) — `kFixedTarget` bootstrap timing for persistent-map `MapSession`s — was not required by either vote and is left as-is.
