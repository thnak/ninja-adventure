# RFC-004: Terrain & Combat Entity

> Status: **Accepted (revised after review)**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §3 (Combat Entity), §4
> (Battlefield Control), §5 (Terrain Evolution), §12 (Destructible Counterplay)
> Scope owner for: the `CombatEntity` chassis, the terrain scar layer, battlefield-control
> primitives, destructible counterplay, and their contract with the tile grid and the flow field.
> Supersedes: **`RFC-001-combat-entity-and-materials.md`** (a pre-series draft from the old
> IMPLEMENTATION_MAP numbering; its `ticks_left`-decrementing CombatEntity violates load-bearing
> rule 3 below and must never be implemented) and the "RFC-004 (surface tags)" dependency cited by
> the pre-series `RFC-010-rl-observation-schema.md`. Both pre-series files should be archived.
> Reconciliation: §2b is the normative single-ownership boundary with RFC-010 (Battlefield
> Simulation); RFC-010 requires the conforming amendment listed there.

---

## Summary

This RFC defines **CombatEntity** — the one chassis for every spawned battlefield thing that is not
a creature, a player, or a projectile: ice walls, rock spikes, smoke clouds, water pools, fire
patches, totems, and interceptable falling bodies. An entity is **chunk state**, exactly like
`Creature`, `Projectile`, and `Zone` already are: a fixed-size record in a fixed-cap array, owned by
the chunk that owns its tile, published in the chunk view, and replicated by copy.

It also defines the **terrain scar layer** — craters, rubble, cracked ground — as a second,
strictly-separate mechanism: scars are per-tile *modifiers* that decay, never entities and never
edits to `terrain_of`.

Three load-bearing rules keep everything else honest:

1. **Only entities block; scars never do.** A scar changes speed and material coefficients
   (RFC-003), never walkability. Walkability changes are always an entity — temporary, visible,
   destroyable.
2. **Nothing spawned in combat ever touches the global flow field.** Blocking entities block
   locally (movement collision + local monster reaction), the same way buildings already do. No
   25 ms BFS rebuild is ever triggered by combat.
3. **All entity and scar timing is absolute world ticks, not decrementing counters.** That is what
   makes them correct under simulation LOD: a chunk that slept for four minutes computes expiry on
   wake instead of having missed 2 400 decrements.

---

## Motivation

The umbrella document promises that "every spike, ice pillar, smoke cloud, boulder is a
CombatEntity" and that skills reshape the battlefield rather than only subtracting HP. Today the
codebase has three ad-hoc precedents pointing at that design without reaching it:

- `Zone` (`tiles.hpp`) — a lingering circle with a kind, a radius, and a countdown. It is two
  hard-coded kinds (`kWet`, `kSmokeSuppress`), it cannot be destroyed, it cannot block, and it
  cannot belong to a team.
- `Projectile` — chunk state with an owner and a life, but no HP and no collision footprint.
- `Effect` — a one-shot visual with a per-kind lifetime, which is the right *rendering* channel for
  entities but has no gameplay body behind it.

Every new boss ability (RFC-005) and every Persist-phase skill product (RFC-001) would otherwise
grow another bespoke struct and another bespoke loop in `ChunkActor`. The cost of that path is the
exact cost GAME.md §5 already refused to pay for creatures ("one `Creature` struct, not many entity
types"). This RFC pays the unification cost once, so that "add a new boss hazard" becomes a table
row, not a system.

Terrain evolution needs its own mechanism because entities are the wrong shape for it: a crater is
not a thing standing on a tile, it is a *property of* the tile — it must be walkable, cheap in the
hundreds, and it must survive the entity that made it. But it must not be a write into terrain,
because `terrain_of` being a pure function of `(seed, map, x, y)` is the property that lets every
node agree on the world without messages (ARCHITECTURE.md §1, §6). Scars therefore live in a third
place: a chunk-owned, decaying overlay.

---

## Guide-level Explanation

### What a player sees

You are fighting the Giant Red Samurai in his 10×7 room. He slams the ground (a real Attack pose —
he is one of the ~11 boss sheets that have one); the floor in a line in front of him flashes a
warning for most of a second, then **rock spikes** erupt from those tiles. The spikes are solid: you
cannot walk through them, your arrows stick in them, and the room is suddenly two corridors instead
of one. But each spike has a small health bar when you hit it — three sword blows (or one Shatter
combo) breaks one, and breaking the *right* one reopens the lane the boss is trying to deny you.
When a spike breaks, the tile under it is left **cracked**; a second slam on cracked ground leaves
**rubble** that slows anyone crossing it; keep fighting on the same ground and it becomes a
**crater** you wade through at half speed. None of it is permanent: walk back into the room ten
minutes later and the floor has healed.

Your own kit does the same things in the other direction. An Ice ability drops a short **ice wall**
behind you as you retreat — monsters bump into it and start hitting it instead of you, because a
wall in the way is a thing to break, not a thing to path around. A **smoke cloud** makes archers
lose you. A **water pool** left by rain or by a broken ice wall is the conductor your Thunder combo
wants (the existing Wet → Conduct chain, now with ground truth).

And when a boss calls a boulder down on you, the boulder's shadow is a thing on the field with a
health bar of its own: **shoot it out of the air** and the impact never happens.

### What a designer does

A designer never writes entity code. They pick an **archetype** from the entity table (or add a
row): the row says whether it blocks movement, whether it blocks projectiles or sight, what aura it
carries, how much HP it has, what element breaks it fastest, what it leaves behind when it dies, and
which FX overlay draws it. Skills (RFC-001, RFC-008) and boss abilities (RFC-005) then reference the
archetype by id: "Persist: spawn `kIceWall` × 3 tiles perpendicular to cast direction."

### What keeps it chill (GAME.md §0)

- Entities and scars are **products of combat only**. Nothing spawns hazards near a player who has
  not engaged; the meadow ring's stray night monsters use none of this. In v1, only player casts
  and boss/dojo content (RFC-005) spawn entities.
- Scars **heal by themselves**. The world never accumulates damage behind the player's back, and a
  farm can never be permanently cratered by a fight that wandered past. Healing is restoration,
  not a countdown *against* the player — there is nothing the player is racing.
- Every dangerous entity **announces itself** before it is dangerous (the arming telegraph,
  RFC-006), consistent with the wind-up promise the walk-only monsters already make.

---

## Reference-level Design

### 1. The `CombatEntity` record

Chunk state, sibling of `Creature` / `Projectile` / `Zone` in `ChunkActor`. Ownership is derived
from position exactly as for creatures: the chunk that owns the tile owns the entity; there is no
stored owner-chunk field to forget.

```cpp
enum class EntityKind : std::uint8_t {
    kIceWall = 0,       // Ice    — blocking wall segment
    kRockSpike = 1,     // Rock   — blocking spike
    kSmokeCloud = 2,    // vision blocker (absorbs ZoneKind::kSmokeSuppress)
    kWaterPool = 3,     // wet aura (absorbs ZoneKind::kWet)
    kFirePatch = 4,     // burning-ground hazard
    kThunderTotem = 5,  // periodic caster
    kFallingRock = 6,   // interceptable Travel-phase body (meteor/boulder)
    kCount = 7,
    // APPEND-ONLY. Values are part of the RL observation contract (RFC-007) and of saved
    // checkpoints' world; never reorder, never reuse.
};

enum class EntityState : std::uint8_t {
    kArming = 0,   // telegraph running; see state machine
    kActive = 1,
    kDying = 2,    // death FX playing; already inert
};

struct CombatEntity {
    std::uint32_t id = 0;          // chunk-local monotonic, same scheme as creature ids
    EntityKind kind = EntityKind::kIceWall;
    EntityState state = EntityState::kArming;
    Faction team = Faction::kPlayer;   // "Team" from the umbrella == the existing Faction
    std::int16_t hp = 0;               // <0 means indestructible (def.destroyable == false)
    float x = 0.0f;                    // map-global tile coords, same space as Creature
    float y = 0.0f;                    // blocking kinds are tile-centre snapped (see §4)
    std::uint64_t owner = 0;           // player key or 0 (boss/world) — kill/assist credit
    std::uint32_t state_tick = 0;      // absolute world tick the current state began
    std::uint32_t expire_tick = 0;     // absolute world tick of natural expiry
    std::uint32_t next_aura_tick = 0;  // absolute; 0 for kinds with no aura
    std::uint32_t next_cast_tick = 0;  // absolute; kThunderTotem only, 0 otherwise — aura and
                                       // cast are distinct knobs, so each gets its own timestamp
    std::uint8_t radius_q = 0;         // aura/footprint radius, tiles ×16 fixed-point
                                       // (def default or spawn override; published, §10)
};
```

`max_hp`, radii, flags, and everything else invariant per kind live in the **archetype table**, not
in the record — the same argument `ability_def()` makes: one constexpr table shared verbatim by sim,
renderer, and the trusted checker, so all three agree to the last integer. (RFC-008 later makes this
table loadable data; the *shape* below is the contract either way.)

```cpp
enum class Collision : std::uint8_t { kNone, kGround, kGroundAndShot };
enum class AuraAffects : std::uint8_t { kEnemiesOfTeam, kEveryone };

// The RFC-007 Block E class taxonomy, defined HERE as real per-kind data (RFC-007 reads it):
//   kIceWall, kRockSpike                 -> kBarrier
//   kSmokeCloud, kWaterPool, kFirePatch  -> kHazardZone
//   kThunderTotem                        -> kCaster
//   kFallingRock                         -> kProjectile   (the ONLY CombatEntity in that class.
//     Engine `Projectile` records remain a sibling chunk-state type, NOT a CombatEntity kind;
//     RFC-007's "Projectile (in flight, interceptable)" Block E class means exactly kFallingRock.)
enum class ObsClass : std::uint8_t { kBarrier, kHazardZone, kProjectile, kCaster };

struct EntityDef {
    Collision collision;
    bool blocks_vision;          // participates in the vision bitmap (§6)
    bool destroyable;            // false => hp field ignored, only expiry removes it
    bool hittable_while_arming;  // true only for kFallingRock (interception window)
    bool observable;             // RFC-007 Block E eligibility — the "tag" RFC-007 depends on.
                                 // true for all v1 kinds; future decor kinds set false
    ObsClass obs_class;          // RFC-007 Block E class one-hot source (mapping above)
    std::int16_t base_hp;
    std::uint8_t arm_ticks;      // telegraph duration before kActive
    std::uint16_t life_ticks;    // kActive duration; 0 = never becomes kActive (v1: kFallingRock
                                 // only — its whole life is the arming window; see §3)
    // Aura (0 = none)
    Status aura_status;          // applied via RFC-002 rules
    AuraAffects aura_affects;
    float aura_radius;           // tiles
    std::uint8_t aura_period;    // ticks between applications to the same field
    // Products
    ScarKind death_scar;         // stamped on the entity's tile when it dies OR expires
    EntityKind death_spawn;      // kCount = none (e.g. kIceWall -> kWaterPool)
    AbilityId periodic_cast;     // kThunderTotem only; semantics owned by RFC-001
    std::uint16_t cast_period;   // ticks
    // Visuals — indices into RFC-006's overlay standard; no bespoke sprite exists or is needed
    EffectKind arm_fx, active_fx, death_fx;
};
```

### 2. The archetype table (v1 roster)

Every number **(tunable)** unless stated. HP values are pre-scaling; RFC-009 owns caster-tier and
ring scaling of entity HP and of damage dealt *to* entities.

| kind | collision | vision-block | HP | arm | life (ticks) | aura | death product |
|---|---|---|---|---|---|---|---|
| `kIceWall` | GroundAndShot | no | 40 | 8 | 300 (30 s) | — | scar: none; spawn `kWaterPool` (r 1.2, 60 ticks) |
| `kRockSpike` | Ground | no | 30 | 8 | 200 | — | scar `kCracked` |
| `kSmokeCloud` | None | **yes** | — (indestructible) | 2 | 50 | target-drop (§6) | — |
| `kWaterPool` | None | no | — | 0 | 100 | `Status::kWet`, Everyone, r 1.5, period 10 | — |
| `kFirePatch` | None | no | — | 5 | 80 | `Status::kBurning`, EnemiesOfTeam, r 1.2, period 10 | scar `kScorched` |
| `kThunderTotem` | Ground | no | 60 | 10 | 400 | periodic_cast (RFC-001), period 50 | scar `kCracked` |
| `kFallingRock` | None | no | 30 | 20 (= whole life) | 0 — never Active; Impact fires at arm elapse (§3) | hittable **by projectiles only** while arming | scar via §8.4 escalation on impact (`kCracked`, upgrading in-window); nothing if intercepted |

Rationale notes:

- **`kSmokeBomb` and `kRainCall` migrate onto this chassis**: `ZoneKind` and `struct Zone` are
  deprecated by this RFC; `kSmokeCloud`/`kWaterPool` reproduce their exact current numbers (50 and
  100 ticks, radii 3.0 and 4.0 as spawned by those abilities — the table radii above are the
  *default* spawn radii; a spawner may override radius within `[0.5, 4.0]` **(tunable)**).
- **`kIceWall` dying into a water pool** is the counterplay dividend: breaking the wall is also
  laying the conductor for the Thunder combo. Emergent, not scripted — it is one table cell.
- **`kFallingRock` is the umbrella's "shoot a meteor out of the air"** (§12). RFC-001 owns the
  Travel phase and the Impact payload; this RFC only supplies the interceptable body: an entity
  whose whole life is its arming telegraph, hittable only by projectiles, and whose destruction
  before `arm` completes cancels the Impact callback.

### 3. Entity state machine

```
            spawn accepted                    t >= state_tick + arm_ticks
  (checked, §5)  ──────────►  kArming  ──────────────────────────────────►  kActive
                                 │                                             │
                                 │ hp<=0 && hittable_while_arming              │ hp<=0  OR  t >= expire_tick
                                 ▼                                             ▼
                              kDying  ◄────────────────────────────────────────┘
                                 │   plays death_fx (effect_life_of(death_fx) ticks),
                                 │   stamps death_scar, spawns death_spawn — all AT ENTRY,
                                 │   so a chunk that sleeps mid-kDying loses only the visual
                                 ▼
                              removed (record freed at t >= state_tick + effect_life_of(death_fx))
```

Rules:

- **kArming**: no collision, no aura, no vision blocking. Visible as `arm_fx` on the footprint
  (RFC-006 telegraph standard). Not hittable unless `hittable_while_arming`.
- **kActive**: collision + aura + vision per def; hittable if `destroyable`.
- **kDying**: inert immediately on entry. All gameplay products (scar, death_spawn) are applied at
  the *entry tick*, never during the FX playback — LOD-safe by construction.
- Natural expiry (`t >= expire_tick`) and destruction take the same path; the only difference a
  rule may key on is `hp <= 0` at entry (v1 keys nothing on it — an expired ice wall also leaves
  its pool; simpler, and it reads as melting).

### 4. Collision, the tile grid, and the anti-trap rule

**Blocking entities are tile-snapped.** A `Collision != kNone` entity occupies exactly one tile;
its `x, y` are the tile centre. Multi-tile walls are **N separate single-tile entities** spawned by
the ability (RFC-001 owns the shape math). Per-tile granularity is what makes counterplay legible:
you break *a* segment and walk through *that* gap. Non-blocking entities (auras, smoke) keep float
positions and circular radii, like zones today.

**Occupancy bitmaps.** Each chunk maintains two derived 32×32 bitmaps (128 bytes each):
`block_bits` (tiles occupied by an Active `kGround`/`kGroundAndShot` entity) and `vision_bits`
(Active `blocks_vision` entities). Rebuilt incrementally on entity state changes; never published —
they are recomputed from the entity list by any consumer that needs them (renderer, probe), the
same "derived, not stored" discipline as chunk ownership.

**Movement.** The per-tile walk check that R7 introduced (`MoveIntent` consulting `terrain_of`)
gains one AND: a step onto a tile with `block_bits` set is refused, both axes independently (the
existing slide-along-walls behaviour). This applies to players and creatures alike — walls are
symmetric.

**The anti-trap rule.** At the `kArming → kActive` transition of a blocking entity, if any creature
or player stands on the footprint tile, **the entity dies instead of arming** (straight to kDying,
no scar, no death_spawn, `death_fx` plays as the whiff). A wall can therefore never crush, trap, or
imprison; standing your ground beats the wall. This is the telegraph-first principle applied to
terrain: the 8-tick arming window *is* the dodge window, and holding the tile is a valid dodge.
Corollary: no "wall the player into a corner" degenerate boss strategy can exist for RL to find
(RFC-007) — the player can always stand in the closing gap.

**Spawn placement validity** (checked at spawn request, §5): blocking entities require
`is_walkable(terrain_of(...))` and no existing blocking entity on the tile. Water, trees, and
buildings refuse them. (Ice bridges over water are explicitly future work — they would touch the
walkability/flow-field contract and are out of v1.)

### 5. Spawning, caps, and eviction

Spawn is a chunk verb (protocol message, same tier rules as `SpawnZone` today):

```
SpawnEntity { EntityKind kind; std::uint16_t tx, ty; Faction team; std::uint64_t owner;
              float radius_override;  /* 0 = def default; auras only */ }
```

- **Cap**: `kMaxEntities = 16` per chunk **(tunable)**; the published view carries the same 16.
  With 24 payload bytes each that is ≤ 384 bytes per chunk view — same order as the zone/effect
  arrays it partly replaces.
- **Refusal, not eviction, is the default**: a spawn into a full chunk is refused (the caster sees
  the whiff FX; the ability was still spent — RFC-001 owns whether refusal refunds). Deterministic
  and simple.
- **One exception**: a spawn tagged as boss-room content (RFC-005 sets a flag on the verb) may
  evict the **oldest non-boss entity** in the chunk. A boss fight must not be censorable by
  pre-littering the room.
- **Per-caster cap**: a player may have `kMaxEntitiesPerPlayer = 3` **(tunable)** blocking entities
  alive per map; the oldest dies (normal kDying) when a fourth is placed. This is the anti-cheese
  bound on walling a stronghold spawn shut — combined with entity lifetimes (≤ 40 s) permanent
  spawn-camping via walls is not constructible.

### 6. Vision blockers and targeting

`kSmokeCloud` generalises today's `kSmokeSuppress` zone from "circle that suppresses" to a proper
line-of-sight primitive:

- **Acquisition**: when a creature (or tower, P3) evaluates a candidate target, it DDA-walks the
  tile line between them sampling every 0.5 tiles; if any sampled tile has `vision_bits` set, the
  candidate is invisible. Acquisition already happens on a slow cadence (not every tick); the DDA
  over ≤ ~12 tiles of aggro range is a handful of bitmap tests.
- **Retention**: a held target is re-checked every `kVisionRecheck = 10` ticks **(tunable)**;
  invisible ⇒ target dropped (the current smoke behaviour, now directional: standing *behind*
  smoke hides you; standing beside it does not).
- Vision blocking is **team-blind** (smoke hides everyone from everyone) — one rule, no special
  cases, and it makes smoke a double-edged tool as intended.
- Cross-chunk seams: a vision line crossing into a neighbour chunk cannot read that chunk's bitmap
  today. v1 accepts the same seam the melee/zone systems already accept (documented tech debt #1/#2
  in ARCHITECTURE.md §10); the P3 neighbour-summary message that fixes towers fixes this too —
  the summary gains the neighbour's `vision_bits` + `block_bits` (256 bytes).

### 7. Hitting entities — destructible counterplay

Entities join the two existing hit loops; no third loop is added:

- **Melee/strike arcs**: the arc resolution that today iterates creatures also iterates
  `destroyable` Active entities (plus Arming interceptables) in reach. Friendly-fire on entities is
  **always on** regardless of team — walls do not dodge, and you must be able to break your own
  wall to reopen your lane. (Damage crediting to `owner` feeds RFC-009's assist rules.)
- **Projectiles**: each flight step first tests creatures (current behaviour), then tests the
  current tile against `block_bits` for `kGroundAndShot` entities: hit ⇒ the entity takes the
  arrow's damage and the projectile is consumed. `kGround`-only entities (spikes, totems) do *not*
  stop arrows — you can shoot over a spike, but not through an ice wall. `kFallingRock` is the
  special case: not in `block_bits` (it blocks nothing), but projectile steps test it directly by
  radius 0.6 **(tunable)** while it arms.

**Damage to entities** is deliberately dumber than damage to creatures: no statuses, no combos, no
build-up — entities have no `Status` slot. One multiplier table **(all tunable)**:

| | Fire | Ice | Rock | Thunder | non-elemental |
|---|---|---|---|---|---|
| `kIceWall` | ×2.0 | ×0.25 | ×1.0 | ×1.0 | ×1.0 |
| `kRockSpike` | ×0.75 | ×1.0 | ×0.5 | ×1.0 | ×1.0 (heavy melee ×1.5) |
| `kThunderTotem` | ×1.0 | ×1.0 | ×1.0 | ×0.5 | ×1.0 |
| `kFallingRock` | ×1.0 | ×1.0 | ×0.5 | ×1.0 | ×1.0 |

The full material system (Flesh/Stone/Spirit/…) is RFC-003's; this table is the v1 stand-in and
RFC-003 §materials supersedes it cell-for-cell when it lands. RFC-009 owns how entity damage
interacts with resistance curves.

**Monsters versus blockers.** A monster whose flow-field `descend()` step or beacon-chase step is
refused by `block_bits` for `kBlockedRepathTicks = 5` consecutive ticks **(tunable)** switches to
attacking the blocking entity if it is hostile-to-it and destroyable, using the *same* wind-up →
strike machinery it uses on players (the entity is simply a legal strike target). Wildlife instead
re-rolls its wander. This single rule is what makes an ice wall a *purchase of time, priced in the
wall's HP* rather than an exploit — and it needs no pathfinding change at all.

### 8. The terrain scar layer

**A scar is a per-tile modifier with an absolute heal tick.** Chunk state:

```cpp
enum class ScarKind : std::uint8_t {
    kNone = 0,
    kCracked = 1,    // no movement effect; escalation precursor; RFC-003 reads stability↓
    kRubble = 2,     // speed ×0.6
    kCrater = 3,     // speed ×0.45
    kScorched = 4,   // no movement effect; suppresses kWaterPool spawns on the tile; RFC-003 reads dry/hot
    kCount = 5,
};

struct Scar {
    std::uint8_t tx, ty;        // chunk-local tile
    ScarKind kind;
    std::uint32_t heal_tick;    // absolute world tick; at t >= heal_tick the scar downgrades
    std::uint32_t made_tick;    // absolute; escalation window bookkeeping
};
// kMaxScars = 64 per chunk (tunable); full => oldest (smallest made_tick) is overwritten.
```

**Invariants** (these are the section reviewers should attack; they are chosen to be defensible):

1. **Scars never change walkability.** `is_walkable` never consults scars. Therefore the flow
   field, `terrain_of` purity, cross-node determinism of layout, and the R7 movement check are all
   untouched. A crater is something you wade through, not around.
2. **Scars never touch `terrain_of` or the worldgen overlay.** They are a third layer, above
   noise + overlay, below entities.
3. **Scars decay toward `kNone`, stepwise**: at `heal_tick`, `kCrater → kRubble → kCracked →
   kNone`, each step re-arming `heal_tick += heal_ticks(kind)`. Healing is computed lazily — any
   read of a scar first fast-forwards it by however many whole heal steps `t` has passed, so a
   slept chunk needs no catch-up loop. Heal times **(tunable)**: cracked 1 500 ticks (2.5 min),
   rubble 3 000 (5 min), crater 6 000 (10 min), scorched 1 200 (2 min).
4. **Escalation ladder** (umbrella §5, the Meteor's Crater/Rubble): a Rock-element impact strong
   enough to scar (RFC-009 defines the impact threshold; v1: any `kFallingRock` impact, any
   `kRockSpike` death) stamps `kCracked`; a scarring impact on a tile already scarred *within*
   `kEscalateWindow = 600` ticks **(tunable)** of `made_tick` upgrades it one step
   (cracked → rubble → crater; crater stays crater). Outside the window it re-stamps `kCracked`.
   Repeatedly fighting on the same ground visibly wears it down; one stray hit does not.
5. **Speed composition**: scar speed multiplies with terrain and status multipliers
   (`speed = base × terrain × status_speed_scale(status) × scar_scale(kind)`), clamped below at
   0.25 **(tunable)** so stacked slows never become a de-facto root — roots must be explicit
   (RFC-002).

Scars render as tinted ground decals per RFC-006 (no dedicated art: cracked/rubble/crater are the
Earth-FX final frames + darkening tint, scorched a red-brown tint — asset-free by construction).

**Persistence**: scars and entities are both **"vị trí quái" class data** (ARCHITECTURE.md §3:
in-memory, not saved). A world reload heals every scar and despawns every entity. This is
tone-correct (the world never accrues damage while you are away) and makes P5 free.

### 9. Simulation LOD and sleep contract

Per ARCHITECTURE.md §4, a chunk runs at 10 Hz, 1 Hz, or sleeps. Contract, in order of the rules
above:

| concern | at 10 Hz | at 1 Hz | on wake from sleep |
|---|---|---|---|
| state transitions | exact | up to 0.9 s late — acceptable: arming windows only ever *lengthen* (never surprise-shorten) | fast-forwarded by tick math: `state`, expiry, and scar decay are all functions of `t` |
| auras | applied when `t >= next_aura_tick`, then `next_aura_tick = t + period` | same test, so at most 1 application per 1 Hz tick — auras thin out, never burst | `next_aura_tick = max(next_aura_tick, t)` — **no retroactive stacking, ever** |
| kFallingRock / totem casts | exact | a boss room always has a player in it, hence is always at 10 Hz — RFC-005 must only author interceptables and totems into player-adjacent content (stated there as a rule) | n/a |
| removal | exact | ≤ 0.9 s late | computed at wake |

The invariant to test: **for any (entity, t), the gameplay-visible state is a function of the
record and t alone** — never of how many times the chunk ticked in between. That is the whole
reason every timestamp in §1 is absolute.

### 10. Replication and determinism

- Entities and scars are published in the `ChunkView` as fixed arrays (16 entities × 24 published
  bytes; 64 scars × 8 bytes), copied not referenced — the established snapshot discipline.
- Spawn/damage/death all happen inside the owning chunk's tick; cross-chunk effects (an arc
  spilling over a seam) inherit the existing seam behaviour and its P3 fix path (§6).
- Entities **do not migrate**: they are stationary (even `kFallingRock` — it falls onto a fixed
  tile). An entity is born and dies in one chunk. This deletes the entire migration/hand-off
  class of bugs for this system.
- Like creature migration counts, entity ids and exact death ticks are message-order-dependent and
  therefore **not** cross-node comparable; scar state after heal fast-forward *is* comparable given
  the same stamp history (ARCHITECTURE.md §2c distinction).

---

## Interactions with Other RFCs

| RFC | boundary |
|---|---|
| **RFC-001 Ability System** | RFC-001's Persist phase is the *only* v1 producer of entities besides RFC-005. It owns spawn shapes (line/ring/point → per-tile `SpawnEntity` verbs), Travel-phase ownership of `kFallingRock` and its Impact-cancel-on-intercept, refund policy on refused spawns, and `periodic_cast` semantics for totems. |
| **RFC-002 Status & Effect Framework** | Auras apply statuses *through* RFC-002 (priority/overwrite rules, the one-status slot, future build-up). This RFC only says *when* and *to whom*; never *what happens next*. |
| **RFC-003 Physics & Material Interaction** | Owns materials of entities (superseding §7's stand-in table), conduction (WaterPool + Thunder), friction/grip/stability read from scars, and impulse vs blocking entities. |
| **RFC-005 Boss Ability Authoring** | Authors boss kits *in terms of* archetype ids; owns the boss-room eviction flag, and the rule that interceptables/totems appear only in always-hot content (§9). |
| **RFC-006 Visual FX & Telegraph Standards** | Owns `arm_fx / active_fx / death_fx` mapping, tinting, and the **looping-FX channel** entities need (see Asset & Engine Constraints). Arming telegraphs must meet RFC-006's minimum-read-time standard. |
| **RFC-007 RL Observation & Action Space** | Owns the normative obs encoding; §RL below is input to it. Requires an "attack blocking entity" affordance in monster/boss action spaces. |
| **RFC-008 Data-driven Skill Definition** | Owns the eventual serialized form of `EntityDef` and archetype references from skill JSON/YAML. v1 ships the constexpr table; the field set here is the schema. |
| **RFC-009 Damage, Resistance & Build-up** | Owns damage numbers into and out of entities, the scar-worthy impact threshold, entity-HP tier scaling, and kill credit from `owner`. |
| **RFC-010 Battlefield Simulation** | Consumes entities + scars as the substrate for field-wide states (earthquake spawning scar waves, etc.). Field-wide states are out of scope here. |

---

## RL Considerations

- **Observation (proposal to RFC-007).** In a 10×7 boss room, entities fit either encoding: (a)
  three 70-bit planes (blocking / hostile-hazard / friendly-hazard) or (b) a top-K list, K = 4
  **(tunable)**, of `(dx, dy, kind, hp_frac, ttl_frac)` — dx/dy room-local ints, fractions
  fixed-point 0..1000, matching the `BossObs` quantisation style already in `boss.hpp`. Scars can
  be a single "rough-ground fraction under me / under target" pair rather than a plane; they are
  second-order for policy quality.
- **Action space.** The boss needs at most: its existing actions + `kSpawnPattern[i]` entries
  (RFC-005 authors the patterns) + `kBreakBlocker` (attack the nearest hostile blocking entity —
  reuses the §7 monster rule). Remember `DqnAgent`'s hard-coded `kActionCount = 15`
  (ARCHITECTURE.md §7): pattern count per boss must keep total actions ≤ 15 or the agent param fix
  must land first.
- **Stability across generations.** `EntityKind` is append-only (§1) so a Gen-40 checkpoint's
  observation channels still mean what Gen-0's did. Archetype *tuning* changes (HP, lifetimes) are
  environment drift the league-training setup already tolerates; archetype *semantic* changes
  (blocking → non-blocking) require a new kind value instead.
- **Degeneracy guards already in the design**: the anti-trap rule (§4) removes wall-imprisonment
  strategies; per-caster caps and refusal-not-eviction (§5) bound entity spam so reward hacking
  via clutter is capped at 16 records; auras thin under LOD rather than burst (§9) so training in
  always-hot dojo rooms matches live behaviour.
- **Dojos**: dojo self-play rooms are ordinary chunks; entities work there unmodified. Training
  episodes should randomise initial scar state of the room floor **(tunable curriculum)** so
  policies do not overfit to clean floors.

---

## Asset & Engine Constraints honored

- **Walk-only monster sheets (66/66, zero attack anims)**: nothing in this RFC asks a monster
  sprite for a new pose. Monsters attacking a wall use the existing wind-up shake/tint + strike FX
  overlay. Entities themselves are pure FX/tile overlays.
- **Boss shortlist**: examples use GiantRedSamurai (first RL boss, real directional Attack poses).
  No design here depends on Dragons' segment rigs or idle-only sheets.
- **Player kit = basic attack + two abilities**: entity-spawning player skills live inside the two
  ability slots (RFC-001); this RFC adds no third slot and no hotbar.
- **Four elements only**: the roster and damage table use Fire/Ice/Rock/Thunder exclusively.
  Plant-spread ("Fire + Plant") from umbrella §7 is explicitly deferred with the Plant school.
- **`kEffectLife` truncation**: already superseded in code by per-kind `effect_life_of()` (Earth's
  14 frames play fully). This RFC relies on that and adds a **new, real gap**: `Effect` is
  one-shot (age-counted, evicted at life) but Active entities need a **looping/pulsing visual for
  up to 400 ticks**. That channel does not exist yet; RFC-006 must define it (proposal: renderer
  derives the loop from the published entity record itself — `active_fx` + `(t - state_tick) %
  life` — so no new sim state and no new message is needed).
- **Magic/* FX family and spinning projectile sheets are not packed**: the v1 roster draws only
  from the packed strips (Slash/Fire/Ice/Earth/Shock/Blast/Smoke + tints). Totem and pool visuals
  are tinted composites, not new sheets.
- **121 skill icons with Disabled twins**: entity-spawning abilities get cooldown UI for free;
  nothing extra needed here.
- **LOD / sleep (1024 chunks)**: §9 is the contract; all timing absolute, all catch-up O(1).
- **Server-authoritative, leader-trusted, cheap replication**: entities/scars are ordinary chunk
  state in the published view; spawn is a verb subject to the same trust tiers as `SpawnZone`;
  nothing here adds a cross-actor read to a hot path.
- **Chill guardrail**: scars self-heal; entities are combat-scoped and short-lived; nothing in
  this system runs a clock against a player who is farming.

---

## Open Questions

1. **Zone deprecation mechanics.** Should `Zone` be deleted in the same change that lands
   `CombatEntity` (touches `kSmokeBomb`/`kRainCall` resolution and the renderer), or coexist one
   phase with entities feature-flagged? Proposed: same change — two parallel lingering-circle
   systems is exactly the divergence this RFC exists to prevent — but that widens the first PR.
2. **Refused-spawn refunds.** When a full chunk refuses an entity spawn, is the ability cost
   refunded? This RFC leans "no refund, show the whiff" for simplicity; RFC-001 owns the ruling
   and it should be uniform across all refusal reasons (occupied tile at arm-time included).
3. **Entity HP scaling curve.** Flat base HP is fine for the first Samurai room; overworld ring
   scaling (×1..×5 like creature HP) needs RFC-009's decision on whether *player-spawned* walls
   also scale (probably by caster school level, not ring).
4. **Cross-seam vision/blocking summary size.** §6 assumes the P3 neighbour-summary message can
   absorb 256 bytes of bitmaps per neighbour per cadence. If P3 lands a smaller summary, vision
   lines should clamp at the chunk border (smoke at a seam over-hides) — needs a call once the P3
   message shape exists.
5. **Frozen-lake interaction.** Winter freezes lakes (P7, seasonal `terrain_of`). Should
   `kIceWall` on frozen water be legal then (walkable terrain, so §4 says yes automatically), and
   should `kFirePatch` on ice melt a temporary hole? The hole variant violates invariant §8.1
   (walkability change outside an entity) — if wanted, it must be modeled as a blocking "water
   hole" *entity*, not a scar. Deferred to P7 planning.
6. **Scar cap pressure in long boss fights.** 64 scars/chunk with 10-minute crater heals: can a
   deliberately ground-pounding boss cycle the cap and visually flicker old scars? If measured to
   happen, raise the cap for boss-room chunks only **(tunable)**.

---

## Non-goals

- **Damage formulas, resistances, build-up meters** — RFC-009. This RFC only defines who is
  hittable and the v1 element multiplier stand-in.
- **Status semantics** (what Wet/Burning do, stacking, freeze ladder) — RFC-002.
- **Materials, impulse, knockback, conduction physics** — RFC-003.
- **Ability pipeline** (cast/channel/travel/impact phases, shapes, costs, cooldowns) — RFC-001;
  **boss kit authoring** — RFC-005; **skill serialization** — RFC-008.
- **FX authoring, tint palettes, telegraph readability standards** — RFC-006.
- **Field-wide battlefield states** (earthquake, storms) — RFC-010.
- **Player construction** (hearths, farm plots, village buildings) — not combat; entities are not
  buildings and never persist.
- **Any change to `terrain_of`, the worldgen overlay, or the flow field.** Explicitly out of
  scope, and the invariants in §4/§8 exist to keep it that way.
- **Ice bridges / walkability-granting entities** — future work; noted in Open Question 5.
