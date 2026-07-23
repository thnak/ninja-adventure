# RFC-010 — RL Observation Schema (BossObs v2)

> Status: Draft. Implements umbrella §17. **This is the F4 contract.**
> Listed last in the series but frozen early: retraining after an observation
> change is the expensive mistake, so this schema versions from day one.
> Depends on: RFC-001 (entities appear in the obs), RFC-004 (surface tags).

## Principle

"RL learns patterns, not individual bosses" (§17): every boss shares ONE
observation layout and ONE action vocabulary. A new boss is a new kind-table
row + a policy checkpoint — no new obs code. The seam is unchanged from F3:

```cpp
BossAction boss_policy(const BossObs&);   // pure function — the swap point
```

## BossObs v2 (fixed-size, integer-only)

```cpp
struct BossObs {
    // -- self --
    std::uint16_t hp_frac;          // 0..1000
    std::uint8_t  channel_stage;    // 0 idle / 1 windup / 2 channel / 3 recover (§1 pipeline)
    std::uint8_t  cooldown[4];      // ticks/8, saturated — up to 4 ability slots
    std::uint8_t  status[6];        // build-up meters 0..255 (RFC-002 order: cold,heat,shock,stun,root,blind)
    std::uint8_t  scale;            // own scale tier (RFC-002)

    // -- target (nearest hostile player) --
    std::int16_t  dx, dy;           // tiles, clamped ±64
    std::uint16_t t_hp_frac;        // 0..1000
    std::uint8_t  t_facing;         // Facing
    std::uint8_t  t_windup;         // player telegraph ticks remaining (they telegraph too)

    // -- local field: 7×7 tile sample centred on boss, two planes --
    std::uint8_t  surface[49];      // terrain/surface tag (RFC-004: normal/mud/ice/water/rubble/crater…)
    std::uint8_t  occupancy[49];    // 0 empty / 1 solid-entity / 2 hazard-aura / 3 friendly / 4 hostile

    // -- nearest combat entities (RFC-001), fixed 4 slots, nearest-first, zero-padded --
    struct { std::uint8_t kind; std::uint8_t team; std::int8_t dx, dy; std::uint8_t hp_frac8; } ent[4];

    // -- arena --
    std::int8_t   wall_dist[4];     // room-clamp distance N/E/S/W, clamped 0..127
};

inline constexpr std::uint16_t kBossObsVersion = 2;
```

Flattening for training: `obs_encode(const BossObs&) -> std::array<std::int16_t, kBossObsLen>`
— field order is part of the contract; append-only forever after freeze.

## Action space v2

```cpp
enum class BossAction : std::uint8_t {
    kHold, kApproach, kRetreat,
    kAttackLeft, kAttackRight, kCharge,     // v1 actions, unchanged semantics
    kAbility0, kAbility1,                   // kind-table ability slots (RFC-009 rows)
    kCount
};
```

Illegal actions (ability on cooldown, charge mid-channel) are masked by the
caller, not punished by reward — the policy never learns "which actions error".

## Versioning & checkpoints

- Checkpoint JSON (per ARCHITECTURE: leader-only TrainingActor writes it)
  records `obs_version`; a loader that sees a mismatch refuses and falls back
  to the scripted `boss_policy` gen-0. The game must never crash or misplay
  because a checkpoint is stale — worst case it plays the F3 script.
- `kBossObsLen` is a compile-time constant; the flat array is the only thing
  serialized between sim and trainer.

## Freeze checklist (before F4 training starts)

- [ ] RFC-001 entity kinds enumerated (obs `ent[].kind` indexes that table)
- [ ] RFC-004 surface-tag vocabulary enumerated (obs `surface[]` values)
- [ ] RFC-002 status meter order fixed (obs `status[]` order)
- [ ] `obs_encode` field order reviewed and tagged v2 — append-only thereafter
