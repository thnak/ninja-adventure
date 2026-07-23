# RFC-001 — CombatEntity & Materials

> Status: Draft. Implements umbrella §3 (Combat Entity), §8 (Material System),
> and gets §12 (Destructible Counterplay) for free.
> Depends on: nothing. Everything else depends on this.

## Problem

The engine has three half-entities: creatures (hp, team-by-kind, no tags),
buildings (hp, static), and zones (lifetime + aura, but no hp, no collision, no
team). A spike, ice pillar, totem, or falling meteor fits none of them, so today
each would need bespoke logic — exactly what the umbrella RFC forbids.

## Design

### The struct (sim side, `src/world/tiles.hpp`)

```cpp
enum class Material : std::uint8_t { kFlesh, kStone, kSpirit, kMetal, kWood, kPlant, kWater, kSlime };

enum EntityTag : std::uint16_t {            // bitmask
    kTagSolid   = 1 << 0,  // blocks movement (dynamic blocker — movement consults per tick)
    kTagWall    = 1 << 1,  // also blocks projectiles
    kTagRoot    = 1 << 2,  // aura: pins entities in aura radius
    kTagBlind   = 1 << 3,  // aura: strips aggro (generalizes kSmokeSuppress)
    kTagWet     = 1 << 4,  // aura: applies wet (generalizes Zone kWet)
    kTagCaster  = 1 << 5,  // periodically casts its ability (totem)
    kTagAirborne= 1 << 6,  // in Travel phase; only ranged can hit (meteor falling)
};

struct CombatEntityKind {   // constexpr table, one row per entity type — data, not code
    std::int16_t  hp;           // 0 = intangible (pure aura body, e.g. smoke)
    Material      material;
    std::uint16_t tags;
    std::uint16_t lifetime;     // ticks; 0 = until destroyed
    std::uint8_t  aura_radius;  // tiles; 0 = none
    std::uint8_t  scale;        // RFC-002 tier; divisor for build-up/knockback
    std::uint16_t mass;         // RFC-003; knockback = impulse / mass
};

struct CombatEntity {
    std::uint32_t id;           // chunk-local, monotonic
    std::uint8_t  kind;         // index into the kind table
    std::uint8_t  team;         // 0 neutral / 1 players / 2 monsters
    std::uint16_t x, y;         // tile coords (entities are tile-anchored, like zones)
    std::int16_t  hp;
    std::uint16_t ticks_left;   // 0 = permanent
    std::uint64_t owner;        // spawning player/boss id, for kill credit
};
```

First rows of the kind table (all pure data):

| kind | hp | material | tags | lifetime | aura |
|---|---|---|---|---|---|
| Spike | 30 | Stone | Solid+Root | 300 | 1 |
| IceWall | 80 | Water | Solid+Wall | 600 | 0 |
| SmokeCloud | 0 | Spirit | Blind | 150 | 2 |
| Totem | 60 | Wood | Caster | 900 | 0 |
| Boulder | 50 | Stone | Solid | 0 | 0 |

### Ownership & determinism

- Chunk-owned, exactly like zones: `std::vector<CombatEntity>` in ChunkActor,
  stepped in `step_entities()` (lifetime decrement, aura application, caster
  ticks), spawned only via verbs inside the chunk tick → append order is
  deterministic, ids monotonic per chunk.
- Cross-seam adoption reuses the zone pattern (circle-vs-AABB, already proven).
- Published in `ChunkView::entities`; renderer draws by kind → atlas slot.

### What existing systems do with it (small diffs, big payoff)

- **strike() / AbilityStrike**: target scan becomes nearest-of(creature, entity
  with hp>0, hostile team). → §12 counterplay: players can break a Spike,
  shatter a Boulder, kill a Totem. No new verbs.
- **Movement**: `can_step(tile)` additionally checks solid entities in the
  chunk. Entity counts are small (a fight spawns a handful), linear scan is fine.
- **Auras**: replace the two hand-wired zone effects. `kWet`/`kSmokeSuppress`
  zones become SmokeCloud/WaterPool entity kinds. `Zone` stays as a thin legacy
  wrapper until RFC-005 finishes the migration, then dies.

### Materials (§8)

One constexpr matrix, fixed-point /16:

```cpp
// material_mult[material][damage_tag] — damage tags arrive with RFC-003;
// until then only kPhysical and kArcane columns are populated.
constexpr std::uint8_t material_mult[8][8] = { /* 16 = 1.0x */ };
```

Spirit: physical column 0 (immune), arcane/holy 24 (1.5×). This is the only
place type-effectiveness lives; no per-skill special cases.

## Non-goals here

Build-up meters (RFC-002), impulse/knockback (RFC-003), the interaction rule
table (RFC-004), Travel-phase spawning of entities from abilities (RFC-005).
This RFC only makes the noun exist everywhere.

## Verification

- Sim scenario: spawn a Spike wall between player and slime → slime cannot
  path through (Solid works); player strikes Spike 2× → destroyed, path opens
  (counterplay works); SmokeCloud strips aggro exactly as the old zone did.
- Determinism gate: double-run byte-identical, GCC vs MSVC identical.
