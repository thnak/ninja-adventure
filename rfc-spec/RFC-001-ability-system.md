# RFC-001: Ability System

> Status: **Accepted (revised after review)**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §1, §2, §12, §13
> Depends on: none (this is the root of the combat RFC set)
> Depended on by: RFC-002, RFC-003, RFC-004, RFC-005, RFC-006, RFC-007, RFC-008, RFC-009,
> RFC-010 (battlefield simulation)
>
> **Numbering note.** RFC references in this document mean the current combat set:
> `RFC-002`…`RFC-009` plus `RFC-010-battlefield-simulation.md`. Earlier exploratory drafts that
> once collided with this numbering were removed; no file by those names exists in `rfc-spec/`.
> This set (RFC-001..010) is canonical.

---

## Summary

Every attack in the game — a player's katana swing, a Samurai boss's charge, a totem's lightning
bolt — is one **ability instance** walking the same seven-phase pipeline:

```
Cast → Channel → Release → Travel → Impact → Persist → Expire
```

This RFC specifies the pipeline as a **state machine with a fixed transition table**, the
**ownership split** of that machine across actors (the caster's actor owns the head, the chunk owns
the tail, exactly one message crosses between them), the **interrupt and counter-play rules for
each phase**, the **cooldown model**, the **player kit** (basic attack + exactly two equipped
abilities), and the four **targeting models** (self / point / direction / entity). Abilities are
instantiated from static `AbilityDef` data; the data schema itself is RFC-008 — this RFC defines
only what fields the runtime consumes and what invariants the data must satisfy.

The pipeline is a *semantic contract*, not a base class. Player abilities, monster attacks, and
boss moves already implement fragments of it in code (`abilities.hpp`, the creature
`windup` fields, `boss.hpp`); this RFC names the phases those fragments occupy and closes the gaps,
so that RFC-005 can author boss abilities and RFC-007 can observe them using one shared vocabulary.

---

## Motivation

Three forces demand a single pipeline:

1. **Composition over bespoke logic** (umbrella §13). "Rock + Sky + Fall + Explosion = Meteor"
   only works if Fall and Explosion are phases with defined seams, not code inside a `Meteor`
   class. With the pipeline, a new skill is a row of data (RFC-008) plus existing phase
   implementations.

2. **RL needs a uniform observation** (umbrella §17). A DQN policy learns patterns like "the boss
   is 4 ticks into a 14-tick Cast — move now." That is only learnable if every boss ability
   exposes the *same* phase enum and tick counter (RFC-007). One policy per archetype (10–15
   policies total) cannot afford per-ability observation shapes.

3. **Counter-play must be systematic** (umbrella §1, §12). "Every phase can be counter-played" is
   a checkable property only if phases are enumerable and each has a written interrupt rule. A
   meteor you can shatter mid-fall (umbrella §12) is a Travel-phase entity with HP (RFC-004) —
   not a special case.

And one force constrains everything: **the tone guardrail** (GAME.md §0). Nothing in this system
runs a clock behind the player's back. Abilities act only when a caster acts; cooldowns only exist
after a player chose to fight; hazards expire on their own. A player who never presses an ability
key never interacts with this RFC.

What exists today and is kept: the constexpr `AbilityDef` table with two equipped slots and the
check-and-debit / apply / display three-way agreement (`abilities.hpp`); the creature wind-up
telegraph with "a committed telegraph is a promise" (`chunk_actor.hpp`, `boss.hpp`); per-kind FX
lifetimes (`effect_life_of`, which already fixed the old `kEffectLife=6` truncation for the sim's
current FX set). What is new: Channel, Travel-as-destroyable, Persist-as-CombatEntity, the
interrupt table, targeting models beyond "ring or front", and data instantiation.

---

## Guide-level Explanation

### For a player

You fight with **three verbs**: a basic attack and two abilities.

- **Basic attack** — your equipped weapon's swing or shot. No resource cost, a short cooldown,
  always available. It uses the character rig's *Attack* row.
- **Ability A** and **Ability B** — two equipped skills. Each costs stamina or mana, has a real
  cooldown, and plays the rig's *Ability* / *Ability2* pose row. There is no hotbar and there
  never will be more than two slots: the art has exactly two ability poses per character, and the
  kit is designed around that, not despite it.

When you use an ability, it may wind up for a moment (Cast), or let you **hold the button to
charge it** (Channel) — release to fire at the charged strength, or just walk away to drop it
(you get your resource back if you barely started). Once it **fires** (Release), it is out of
your hands: a projectile flies (Travel), lands (Impact), and may leave something behind — a smoke
cloud, an ice wall, a wet patch (Persist) — which fades on its own (Expire).

Everything the enemy does walks the same road, **and you can fight back at every step**:

| The enemy is… | You can… |
|---|---|
| winding up (Cast) | hit it hard enough to break the cast, or step out of the marked area |
| charging (Channel) | same — and the longer it charges, the more it hurts when it lands |
| already fired (Travel) | dodge the projectile, or destroy it in the air (it has HP) |
| landing (Impact) | be somewhere else — aim froze when it fired, so moving *is* the dodge |
| left a hazard (Persist) | break it (spikes, pillars), leave the area, or wait it out |

Cooldowns show as the skill icon's greyed twin. Nothing recharges by calendar or requires
maintenance: log in after a month and your kit is exactly as ready as you left it.

### For a designer

You author an ability by filling a data record (RFC-008): which phases it uses, how long each
lasts, what it targets, what it costs, what it spawns. You never write phase logic. The questions
you answer:

1. **Head**: does it wind up (`cast_ticks`)? Can it charge (`channel` block)?
2. **Targeting**: self, point on the ground, direction, or an entity?
3. **Payload**: instant hit, projectile, dash (the caster's own body is the projectile — this is
   the Samurai charge; creature/boss casters only in v1, §1), zone, or spawned entity (wall,
   spike, totem — RFC-004)?
4. **Tail**: what happens at Impact (damage/status — RFC-009/002), and does anything Persist?

Monster and boss abilities answer the same questions, plus RFC-005/006 minimum-telegraph rules
(their Cast must be long enough to read, and its visual is an FX overlay, never a monster
animation frame — the monsters have none).

---

## Reference-level Design

All tick counts are at the simulation rate `kTicksPerSecond = 10` (`tiles.hpp`). Every number
introduced by this RFC is marked **(tunable)**; numbers quoted from existing code are that code's.

### 1. Phases

```cpp
enum class AbilityPhase : std::uint8_t {
    kIdle    = 0,  // no instance — not a phase of an instance, the absence of one
    kCast    = 1,  // committed wind-up; telegraph visible; fixed duration
    kChannel = 2,  // held empowerment; variable duration, caster-terminated
    kRelease = 3,  // atomic commit point; exactly 1 tick; aim freezes here
    kTravel  = 4,  // payload in motion (projectile / dash / falling meteor)
    kImpact  = 5,  // resolution at the frozen aim; damage & statuses apply here
    kPersist = 6,  // spawned aftermath lives (zone / hazard / entity)
    kExpire  = 7,  // teardown tick; end-of-life triggers fire; instance deleted
};
```

Phases are **optional per ability** but their **order is fixed**. An ability's data declares which
of {Cast, Channel, Travel, Persist} it uses; Release, Impact, and Expire always occur (possibly on
the same tick). The minimal ability — the basic melee swing — is `Release → Impact → Expire` in
one tick.

**Phase applicability by payload kind:**

| `PayloadKind` | Cast | Channel | Travel | Impact | Persist | Example |
|---|---|---|---|---|---|---|
| `kInstantHit` | opt | opt | — | ✔ | opt | WhirlCleave, CrushBlow, basic melee |
| `kProjectile` | opt | opt | ✔ | ✔ | opt | FanVolley, basic arrow, Squid shot |
| `kDash` | ✔ | — | ✔ | ✔ | — | Samurai charge (caster body is the travel body) |
| `kZone` | opt | opt | — | ✔ | ✔ | SmokeBomb, RainCall |
| `kSpawnEntity` | opt | opt | — | ✔ | ✔ | Ice wall, spike, totem (RFC-004) |

`kDash` requires Cast (a committed dash with no telegraph is unreadable) and forbids Channel in
v1 (a chargeable dash is a fun idea and an interrupt-rules headache; out of scope).

`kDash` is additionally **creature/boss-only in v1**. A dash makes the caster's own body the
Travel payload; for a player that would hand authority over the player's position from the
trusted `PlayerActor` to the chunk actor mid-flight — an authority transfer the §4 ownership
split and the PlayerBus/beacon replication path do not define. Creature and boss casters have no
such transfer: their head and tail already live in the same chunk actor (§4), so "the caster is
the body" costs nothing. A player dash/blink ability is deferred with the transfer question
(Non-goals).

### 2. The state machine

```
            ┌────────────────────────── voluntary cancel / interrupt ──────────────────────────┐
            │                                     (see §5)                                     │
            ▼                                                                                  │
 ┌──────┐  input   ┌──────┐ cast_ticks ┌─────────┐  hold ends  ┌─────────┐                     │
 │ Idle │────────▶ │ Cast │──elapsed──▶│ Channel │────────────▶│ Release │  (Channel absent:   │
 └──────┘ (§3 gate)└──────┘            └─────────┘             └────┬────┘   Cast→Release)     │
            ▲          │                    │                       │                          │
            │          └────────────────────┴───────────────────────┼──────────────────────────┘
            │                                                       │ payload handoff (§4)
            │                                              ┌────────▼────────┐
            │                                              │ Travel (if any) │◀─ body destroyed → Expire
            │                                              └────────┬────────┘
            │                                                       ▼
            │                                                   ┌────────┐
            │                                                   │ Impact │
            │                                                   └───┬────┘
            │                                                       ▼
            │                                            ┌───────────────────┐
            └──────────────  Expire  ◀───────────────────│ Persist (if any)  │
                        (instance deleted)               └───────────────────┘
```

**Transition table.** `now` is the owning actor's tick counter. All guards are evaluated by the
phase's owner (§4); no transition requires reading another actor's state.

| # | From → To | Guard | Effects |
|---|---|---|---|
| T1 | Idle → Cast | request passes admission (§3) | debit cost (§3); publish `{phase, ability, ticks_left, aim}`; start telegraph FX (RFC-006) |
| T2 | Cast → Channel | `cast_elapsed == cast_ticks` and ability has a channel block | begin per-tick channel drain (§6) |
| T3 | Cast → Release | `cast_elapsed == cast_ticks` and no channel block | — |
| T4 | Channel → Release | caster releases input with `channel_elapsed ≥ kChannelGraceTicks` (an earlier release is a grace tap and routes to T12 instead — §6); or `channel_elapsed == channel_max_ticks`; or `channel_elapsed` reaches a release tick pre-committed at T1 (RL duration-bucketed casters — RL Considerations); or a `release_on_move` caster moves (grace routing applies here too — a move before `kChannelGraceTicks` cancels via T12, honoring "walk away to drop it") | record charge fraction `f` (§6) |
| T5 | Release → Travel | payload kind ∈ {kProjectile, kDash} | freeze aim (§7); spawn travel body in the target chunk via handoff message (§4); start cooldown (§8) |
| T6 | Release → Impact | payload kind ∈ {kInstantHit, kZone, kSpawnEntity} | freeze aim; handoff message; start cooldown (§8); Impact resolves same tick in the owning chunk |
| T7 | Travel → Impact | body reaches frozen aim / max range; or collides (RFC-003/004) | — |
| T8 | Travel → Expire | body destroyed (HP ≤ 0, RFC-004) or lifetime cap hit | no Impact occurs — a shattered meteor deals nothing |
| T9 | Impact → Persist | ability declares persist payload | spawn zone / CombatEntity with `expires_at_tick = now + persist_ticks` |
| T10 | Impact → Expire | no persist payload | — |
| T11 | Persist → Expire | `now ≥ expires_at_tick`; or persist entity destroyed (RFC-004); or dispelled (RFC-002) | end-of-life triggers fire (e.g., a zone's fade FX) |
| T12 | Cast/Channel → Idle | voluntary cancel, grace tap (§6), or forced interrupt (§5) | refund per §5; set `staggered_until_tick = now + kStaggerTicks` on the caster actor (§5 — the head is deleted, so the field lives beside it); if a forced interrupt broke a Channel, write `ready_at_tick[slot] = now + ⌈cooldown/2⌉` (§5, §8); publish phase = Idle; telegraph FX removed |

Invariants:

- **I1 — one head per caster.** A caster hosts at most one instance in {Cast, Channel, Release}.
  A second activation while one is in flight is rejected with `kBusy` (§3). The basic attack is
  also gated by I1 (you cannot swing mid-cast). Tail phases (Travel/Persist) are *not* gated:
  fire an arrow, then cast — the arrow is the chunk's problem now.
- **I2 — Release is atomic.** Exactly one tick; no input, damage, movement, or interrupt is
  processed against the instance during it. Its counter-play is upstream (interrupt the Cast) or
  downstream (dodge/destroy) — this satisfies umbrella §1 "every phase" by construction, because
  Release is a boundary, not a duration.
- **I3 — aim freezes at Release.** No payload retargets after Release. Homing does not exist in
  v1. This is what makes "moving is the dodge" true, keeps replication to a single frozen
  `(x, y, dir)` per payload, and is exactly the semantics the existing creature wind-up already
  has (`windup_x/windup_y` captured at commit; a moved target is whiffed at — the visible miss).
- **I4 — absolute deadlines in the tail.** Persist lifetimes are stored as `expires_at_tick`
  (absolute world tick), never as a decrementing counter, so a chunk that dropped to 1 Hz or
  slept and woke evicts correctly by comparison (ARCHITECTURE.md §4 LOD).

### 3. Admission: check-and-debit

The trusted caster-owning actor (`PlayerActor` for players; the chunk itself for creatures/bosses,
whose stats are already chunk state) admits an activation:

```
reject_of(slot, target):
    if dead or mounted or unbound            → kUnavailable
    if school_level < def.unlock_level       → kLocked
    if now < ready_at_tick[slot]             → kCooldown
    if vital(def.cost_kind) < def.cost       → kResource
    if head_phase != kIdle                   → kBusy        // NEW — invariant I1
    if now < staggered_until_tick            → kBusy        // NEW — §5 post-interrupt recovery
    if !target_valid(def.targeting, target)  → kBadTarget   // NEW — §7; kEntity-only in v1
    else                                     → kOk; debit def.cost
```

`staggered_until_tick` is an absolute tick stored on the caster's actor (not in the head — T12
deletes the head, and the stagger must outlive it). `kBadTarget` is reachable only through
`kEntity` targeting in v1: `kSelf` takes no target input, `kPoint` clamps instead of rejecting,
and `kDirection` normalizes any vector (§7).

`AbilityReject` (`abilities.hpp`) grows `kBusy = 5` and `kBadTarget = 6`. The cost is debited **at
Cast start** (T1), not at Release — the trusted actor can refund (§5), and debit-first means an
untrusted chunk never needs to touch player vitals. Cooldown, by contrast, starts at Release (§8).

### 4. Ownership split and the single handoff

The pipeline spans two actors, split between Release and the tail:

```
 HEAD — caster's actor                      TAIL — owning chunk actor
 (PlayerActor: trusted, ticks with player)  (untrusted OK; ticks at chunk LOD)
 Cast, Channel, Release                     Travel, Impact, Persist, Expire
        │
        └── exactly ONE message at Release: AbilityPayload ──▶
```

`AbilityPayload` (the generalization of today's `AbilityStrike` / `LaunchArrow` / `SpawnZone`):

```cpp
struct AbilityPayload {
    std::uint64_t caster;      // player key or creature id — attribution for XP/aggro
    std::uint8_t  team;        // RFC-004 team filter
    std::uint16_t ability;     // index into the resolved ability table (RFC-008)
    float x, y;                // frozen aim point (I3)
    float dir_x, dir_y;        // frozen unit direction (kDirection/kDash/kProjectile)
    std::uint16_t charge_mil;  // charge fraction f as fixed-point 0..1000 (§6); always 1000
                               // for abilities without a channel block — never 0-defaulted
    std::uint64_t release_tick;
};
```

Rules:

- The payload is sent to the chunk owning the **aim point** (zones, instant hits, spawns) or the
  **caster's position** (projectiles, dashes — the body starts where the caster stands). Cross-
  chunk travel hand-off of a moving body reuses the arrow/creature migration path and its fan-out
  limits are RFC-010's scope.
- The payload is **fire-and-forget**: the caster's death, logout, or interrupt after Release does
  not recall it. The head instance is deleted the tick after Release; the tail is a chunk fact.
- For **creatures and bosses**, head and tail live in the same chunk actor, but the phases and
  transition table are identical — `commit_windup`/`resolve_windup` *are* T1/T3/T6, and
  `boss_policy`'s "already winding up → hold" is I1 enforced. RFC-005 authors boss abilities
  purely as data over this machine; the machine does not know whose brain requested T1
  (scripted `boss_policy` or a DQN checkpoint — the seam in `boss.hpp` is unchanged).

**LOD tolerance** (hard constraint — 1024×1024 world, chunks sleep):

- Head phases progress on the **caster's** ticks. A player's actor always ticks; a creature's
  head only progresses when its chunk ticks — which is fine, because a creature only *starts* a
  cast when it has a target, and targets are players, and chunks with players nearby run at 10 Hz.
  Combat never happens in a slept chunk by construction (GAME.md §0: difficulty waits to be found).
- Travel bodies exist only in active chunks. If a chunk demotes below 10 Hz with a live travel
  body (last player left mid-flight), the body is **dropped at demotion** — no impact, no
  damage-at-a-distance behind anyone's back. Cheap, invisible, and tone-correct. (A `kDash`
  body never faces this rule: dashes are creature/boss-cast in v1 (§1) and a boss room always
  has a player in it, hence stays at 10 Hz — RFC-004.)
- Persist entities survive demotion and sleep via I4 (absolute expiry). A woken chunk evicts
  expired persists before its first publish; a 1 Hz chunk applies persist auras (RFC-002) with
  `dt` scaling so per-second effect rates are preserved. **Migration note:** the shipped `Zone`
  stores a decrementing `ticks_left` (`tiles.hpp`) — exactly the counter form I4 forbids.
  Adopting Persist onto the zone path requires replacing that field with absolute
  `expires_at_tick`; reusing the counter as-is would silently mis-expire on a 1 Hz or slept
  chunk.

**Replication.** The head publishes `{phase:u8, ability:u16, phase_ticks_left:u8, aim x,y:f32}`
in the caster's existing published view (players: via `PlayerBus`/beacon; creatures: the
already-published `windup*` fields, renamed onto this shape). The tail is ordinary chunk state
(effects, projectiles, zones, entities), all copied views, all capped: **≤ 16 travel bodies and
≤ 8 persist records per chunk (tunable)** — the persist cap is today's `kMaxZones`. Cap policy
follows RFC-004 §5, which owns the persist entity/zone spec: **refusal, not eviction** — a
payload arriving at a full chunk is refused, and the caster sees the whiff FX. (This matches
shipped behavior: `chunk_actor.hpp`'s `SpawnZone` handler already refuses at `kMaxZones`; it
never evicted.) The one exception is RFC-004's boss-room-flagged spawn, which may evict the
oldest non-boss record so a boss fight is never censorable by clutter. **Refund ruling**
(answering RFC-004 Open Q2, delegated here): a refused spawn is **not refunded**. Refusal
happens on the tail side, after the fire-and-forget handoff has deleted the head — a refund
would need a second cross-actor message, which the one-message contract forbids. This ruling is
uniform across all refusal reasons (full chunk, occupied tile at arm-time).

### 5. Interrupts and counter-play, per phase

Interrupt sources, in the only priority order that needs stating (higher wins on the same tick):

1. **Death** of the caster (head phases only — see fire-and-forget above).
2. **Hard control**: stun (RFC-002); knockback whose impulse exceeds the phase's break threshold
   (RFC-003).
3. **Poise break**: a single hit ≥ `poise(phase)` fraction of the caster's max HP. Defaults:
   Cast **12%**, Channel **8%** (tunable) — Channel is squishier because it is the player-facing
   answer to "the boss is charging something big: hit it hard *now*". Bosses scale these via
   RFC-009 build-up/scale tiers; they are never immune (umbrella §9), a Titan just needs more.
4. **Voluntary cancel**: caster moves (if `move_cancels` — default true for Cast) or presses the
   ability key again during Cast **(tunable per ability via RFC-008 flags)**.

The full per-phase contract:

| Phase | Owner | Caster may move? | Interruptible by | Counter-play for the defender | On interrupt/cancel |
|---|---|---|---|---|---|
| Cast | caster actor | no (rooted; `move_cancels` makes moving a cancel) | 1–4 | break the cast (poise/stun); step out of the telegraph | **full refund**, no cooldown, +`kStaggerTicks = 5` (tunable) recovery: T12 sets `staggered_until_tick = now + kStaggerTicks` on the caster actor and admission (§3) returns `kBusy` until it passes — prevents free cast-fishing loops |
| Channel | caster actor | no (v1) | 1–3; T4 covers voluntary exit | as Cast; or simply let it release weak — early release is *their* loss | forced interrupt: **no refund** — the T1 base cost *and* the ticks already drained (§6) are both forfeited (the mercy is the cooldown, not the resource); **half cooldown**, written at the interrupt by T12: `ready_at_tick[slot] = interrupt_tick + ⌈cooldown/2⌉` (tunable; the only cooldown write outside Release — §8) — a broken channel hurts but does not delete the kit |
| Release | caster actor | — (atomic, I2) | nothing (I2) | upstream/downstream — see I2 | — |
| Travel | chunk | n/a (caster is free; except kDash — the caster *is* the body and is committed, uncontrollable, un-steerable until Impact) | body destroyed: it is a CombatEntity with HP, team, tags (RFC-004) | dodge (I3 — it flies at frozen aim); destroy it (umbrella §12: shoot the meteor); block it with a wall/pillar (RFC-004 collision) | T8: Expire, **no Impact** |
| Impact | chunk | n/a | not interruptible (single tick) | spatial: not being at the frozen aim. There are no i-frames in this game; position is the whole defense | — |
| Persist | chunk | n/a | entity destroyed (RFC-004); dispel (RFC-002); expiry (I4) | destroy it (spike, pillar, totem); leave the area; wait — every persist has a finite `persist_ticks` | T11: Expire |
| Expire | chunk | n/a | nothing (teardown) | none needed — it is already over | — |

Refund rationale, stated so reviewers can attack it: refunds are decided by the **trusted** actor
that debited (§3), so they cannot be forged; full-refund-on-broken-Cast is the chill choice
(being interrupted already cost you the moment; losing the resource too would be a double
punishment), and `kStaggerTicks` is what keeps that from being exploitable as a free poke-probe.

`kDash` commitment is deliberate and matches shipped boss behavior (`kBossChargeDashTicks = 9`,
dash to where the target stood at commit): the charge is the boss's biggest promise, and a promise
you can steer mid-flight is a lie. A dashing caster in Travel can still be *hit* (it is a body
with HP) but not stunned out of the dash in v1 — stun applies on arrival (open question Q4).

### 6. Channel semantics (charge model)

v1 supports **charge channels only** (hold to empower, release to fire). Sustained channels
(beam/aura-while-held) are deferred (Non-goals).

- Drain: `channel_cost_per_tick` (tunable per ability; typical **2/tick** stamina or mana) is
  debited by the caster's actor each Channel tick. Hitting zero forces T4 at the current charge.
- Charge fraction at Release: `f = min(1.0, channel_elapsed / channel_full_ticks)`.
- Power scaling is applied at Impact by **RFC-009**, which consumes `charge_mil` and owns the
  charge→power curve. This RFC guarantees only that `charge_mil` is monotone in channel time and
  that a partial charge still deals *something* — the curve's shape and floor are RFC-009's.
- Abilities **without** a channel block always send `charge_mil = 1000` (`f = 1.0`): a basic
  attack or uncharged ability is at full power by definition, never curve-penalized.
- Grace tap: releasing before `kChannelGraceTicks = 3` **(tunable)** routes the release to T12
  instead of T4 (the guard is in the transition table) — a voluntary cancel: full refund
  including drained ticks, no cooldown. An accidental tap must never spend a 200-tick cooldown;
  that is the tone guardrail applied to input. (T12's `kStaggerTicks` recovery still applies —
  half a second, uniform across all T12 exits, and what keeps the tap from being a free probe.)
- Bounds: `channel_full_ticks` ≤ `channel_max_ticks` ≤ **30** (3 s) **(tunable)** for players.
  Boss channels may run to 50 (RFC-005), because a long boss channel is *content* (a big readable
  window), while a long player channel is standing still in an action game.

### 7. Targeting models

```cpp
enum class TargetingModel : std::uint8_t {
    kSelf      = 0,  // aim := caster position; dir := facing. Ring strikes, self-zones, novas.
    kPoint     = 1,  // aim := requested ground point, CLAMPED to max_range along the caster→point
                     //        ray (never rejected for range — chill UX: the game does what you
                     //        meant, at the edge of what it can). Zones, spawns, meteor targets.
    kDirection = 2,  // dir := normalized requested vector (zero vector → facing);
                     //        aim := caster + dir * max_range. Projectiles, dashes, front strikes.
    kEntity    = 3,  // a creature/player key, validated at CAST start (in range, hostile team per
                     //        RFC-004 filter, visible in the caster's published view). Resolves to
                     //        that entity's POSITION at Release (I3) — thereafter it is a point.
};
```

Rules:

- Validation happens at admission (§3, `kBadTarget`) against the caster's own knowledge (its
  chunk's creatures, its beacon roster) — never a cross-actor ask. Only `kEntity` can fail it in
  v1 (§3): the other three models always produce a usable aim.
- Targeting model and payload kind are **orthogonal axes**. The Samurai charge is authored as
  `kEntity` targeting + `kDash` payload: the acquired target's position (the shipped
  `windup_x`/`windup_y` capture) resolves at Release to a point, and the dash derives its frozen
  direction from it — not a caller-supplied free vector. A `kDirection` dash is equally
  expressible for casters that aim a heading.
- `kEntity` is *soft*: if the entity died or left the caster's knowledge during Cast/Channel, the
  Release falls back to the entity's **last known position** — the cast is not wasted, the shot
  just whiffs where they were. (Existing melee `kFront` "nearest ahead within radius" is `kSelf`
  targeting with an entity-picking Impact resolver, and stays that way — the pick is part of
  Impact, not targeting.)
- `max_range` per ability **(tunable, RFC-008)**; defaults: point/entity **8 tiles**, projectiles
  by lifetime (today's arrow: 18 tiles/s × 12 ticks ≈ 20 tiles), dash by `dash_ticks × speed`.
- Line-of-sight is **not** a targeting precondition in v1. Walls stop payloads physically in
  Travel (RFC-003/004 collision), and vision blockers gate *acquisition* (RFC-004); a point cast
  over a wall that the projectile cannot reach simply impacts the wall. One rule, no LoS raycast
  in admission.

### 8. Cooldowns

- Stored per slot on the trusted caster actor as **absolute** `ready_at_tick[slot]` (I4 argument
  again: survives saves, LOD, and never ticks while anything sleeps — it is a comparison, not a
  timer).
- Set at **Release** (T5/T6): `ready_at_tick = release_tick + def.cooldown`. A cancelled or
  broken Cast never starts cooldown (§5). The single exception to set-at-Release is the broken
  Channel, which never reaches Release: T12 writes `ready_at_tick[slot] = interrupt_tick +
  ⌈cooldown/2⌉` at the interrupt (§5).
- Ranges: basic attack **6 ticks** melee / **8 ticks** ranged **(tunable)**; abilities keep the
  shipped table's range, 60–200 ticks (`abilities.hpp` — those numbers remain authoritative).
- **No global cooldown.** I1 (one head at a time) plus per-phase durations already serialize the
  kit; a GCD would only add input latency to a two-slot kit. The only cross-slot coupling is
  `kStaggerTicks` after any T12 exit (§5).
- UI is free: every one of the 121 skill icons ships a Disabled twin — cooldown display is an
  icon swap plus a tick count, no new art (RFC-006 owns the exact presentation).
- Tone check: cooldowns only exist because the player (or a monster the player sought out) acted.
  Nothing accrues, decays, or expires for a player who is farming. A cooldown that outlives a
  session is simply *ready* next session.

### 9. Instantiation from data

Runtime instantiation consumes a resolved, immutable `AbilityDef` record (superset of today's
struct; serialized schema, validation, and hot-reload are **RFC-008**). Fields this RFC's machine
reads, grouped by phase:

```
identity   : ability id, school, unlock_level, icon
admission  : cost_kind, cost, cooldown, targeting_model, max_range, team_filter (RFC-004)
head       : cast_ticks, move_cancels, poise overrides,
             channel? { full_ticks, max_ticks, cost_per_tick, release_on_move }
payload    : payload_kind, speed, lifetime_ticks, body {hp, radius, material} (RFC-004, kProjectile/kDash)
impact     : damage/status/impulse block — OPAQUE here, consumed by RFC-009/002/003
persist    : persist_ticks, entity/zone spec — OPAQUE here, consumed by RFC-004/002
telegraph  : FX ids per phase — OPAQUE here, consumed by RFC-006
```

Data invariants the loader must enforce (RFC-008 implements, this RFC owns the *rules*):

- V1: declared phases respect the applicability matrix (§1) — a `kDash` with a channel block is a
  load-time error, not a runtime surprise.
- V2: the telegraph-first rule (umbrella §2) is a *validated property of data*, not a
  convention, in two tiers. Every hostile-faction ability has `cast_ticks ≥ kMinEnemyTelegraph
  = 4` **(tunable)** — the floor is the smallest shipped windup, so the shipped roster
  (Slime/Spider 4, Ghost 6, Skull 8, Boss 10 — `stats_of`, `tiles.hpp`) loads unchanged, by
  design and not by grandfather-exception. **Heavy/committed** hostile abilities — any with a
  payload kind other than `kInstantHit`, or with a persist block — additionally need
  `cast_ticks ≥ kMinHeavyTelegraph = 8` **(tunable)**; the shipped boss telegraphs (10 attack /
  14 charge) already clear it. Player abilities may be 0 (instant).
- V3: every ability with `persist_ticks > 0` has finite `persist_ticks ≤ 600` (60 s) **(tunable)**
  — nothing persists indefinitely; the battlefield always heals (tone guardrail, and cap I4's
  memory).
- V4: telegraph FX lifetimes come from the per-kind frame table (`effect_life_of` — frames × 1
  tick at 10 Hz). Any new FX family RFC-006 adds (Magic/*, spinning projectiles — currently
  unpacked) must extend that table rather than reintroduce a flat constant; a phase whose duration
  exceeds its FX strip's life **loops the strip**, never stretches it.

The runtime instance is small and unserialised (head) or ordinary chunk state (tail):

```cpp
struct AbilityHead {                  // lives on the caster's actor; at most one (I1)
    std::uint16_t ability;
    AbilityPhase  phase;              // kCast / kChannel / kRelease only
    std::uint8_t  phase_elapsed;
    std::uint16_t charge_elapsed;     // Channel only
    float aim_x, aim_y, dir_x, dir_y; // provisional until frozen at Release (I3)
    std::uint64_t target_key;         // kEntity only
};
```

Two per-caster fields live **beside** the head on the caster's actor, never inside it, because
both must survive the head's deletion at T12/Release: `staggered_until_tick` (§5) and
`ready_at_tick[slot]` (§8). Both are absolute ticks (I4 discipline).

The scripted six (`abilities.hpp`) map onto this machine with `cast_ticks = 0`, no channel, and
their existing kind → payload mapping (`kStrike→kInstantHit`, `kVolley→kProjectile ×3`,
`kZone→kZone`); their constexpr table remains the interim data source until RFC-008 lands, and
`equipped_ability`'s fixed loadout remains the interim picker.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-002** | Statuses/auras are *applied* at Impact and by Persist auras; stun/dispel are *interrupt inputs* to §5. This RFC defines when; RFC-002 defines what. |
| **RFC-003** | Impulse/knockback resolve at Impact; knockback-as-interrupt threshold (§5 source 2) is computed by RFC-003's impulse/mass rules. Material interactions of travel bodies (a fire bolt over water) are RFC-003. |
| **RFC-004** | Travel bodies and Persist entities *are* CombatEntities (HP, lifetime, collision, team, tags). T8/T11 destruction guards are RFC-004 facts. Team filters for targeting/impact are RFC-004's. |
| **RFC-005** | Boss abilities are data over this machine (§4); RFC-005 owns which phases each boss move uses, per-boss numbers, and multi-move sequencing. It may not add phases or reorder them. |
| **RFC-006** | Owns every visual: telegraph FX per phase, the FX-overlay rule for walk-only monsters, cooldown presentation, V4's FX table extensions. This RFC only mandates *that* Cast/Channel publish a telegraph and hands over the FX ids opaquely. |
| **RFC-007** | Observes `{phase, phase_ticks_left, ability}` from the published head (§4 replication) — the fields exist for RL as much as for the renderer. Ability activations are single discrete actions (commit at T1); see RL Considerations. |
| **RFC-008** | Owns the serialized schema, loader, and validation *implementation* for §9; the invariants V1–V4 are normative from here. |
| **RFC-009** | Consumes `charge_mil` and the opaque impact block; owns damage, resistance, and the build-up math behind poise/interrupt thresholds (§5 source 3) and boss interrupt scaling. |
| **RFC-010** (battlefield simulation) | Owns cross-chunk fan-out: travel bodies crossing seams, zones spilling over chunk borders (today's known under-coverage), persist entities at scale, and the battlefield-state effects (umbrella §16) that perturb Travel. |

---

## RL Considerations

- **One machine, two brains.** The phase machine is driven, never bypassed: `boss_policy` (or its
  DQN replacement) can only *request* T1 and choose targeting inputs. Everything a policy does is
  therefore telegraphed by the same rules that bind scripted monsters — a learned boss cannot
  discover an untelegraphed attack, because none is expressible. This is the fairness floor that
  keeps self-play training (GAME.md §10) from optimizing into unreadable play.
- **Observation.** RFC-007 gets, for each observed combatant, `{phase (3 bits), ability id,
  phase_ticks_left}` plus frozen aim once Released — a fixed-width, integer-quantized extension
  of `BossObs`, safe to hand across machines without float-determinism arguments (`boss.hpp`'s
  existing rationale).
- **Action space stays small and fixed.** An ability activation is ONE discrete action
  (T1 commit); Cast/Travel then run without further decisions, and Channel duration for RL
  casters is expressed as k duration-bucketed variants of the action (e.g., charge-short/full)
  rather than a hold/release pair — DQN is discrete and the `kActionCount` sizing pitfall
  (ARCHITECTURE.md §7) demands a fixed count known at network-construction time. The bucketed
  variant pre-commits its release tick at T1; T4's pre-committed-release guard is the mechanism.
- **I1 simplifies credit assignment**: at most one head instance means the reward for an ability
  is attributable to a single recent action, not an interleaving.
- **Interrupts are learnable both ways**: a policy sees its own `phase` and learns that casting
  point-blank gets poise-broken; the defender-side policy (village guards sparring, GAME.md §6)
  sees the attacker's phase and can learn to punish Channel. Same observation field serves both.

---

## Asset & Engine Constraints Honored

| Constraint (2026-07-23 audit) | How this RFC honors it |
|---|---|
| Player rigs have exactly **two** ability pose rows (Ability, Ability2) | The kit is basic attack + two slots, structurally (§ Player kit, I1). **The pose row is a property of the slot, not the ability**: whatever is equipped in slot A plays the Ability row; slot B plays Ability2. No third slot can exist without new art, and none is specified. |
| All 66 monsters are 64×64 walk-only, **zero attack animations** | Cast/Channel telegraphs are *published state* rendered as FX overlays at/around the monster (the shipped `windup` red-pulse read); the machine never assumes a caster animation exists. Visual standards: RFC-006. |
| Only ~11/20 bosses have Attack/Charge poses; Samurai first; no Dragons/GiantSlime/Flam/Spirit as RL agents | The machine is pose-agnostic (`BossPose` is a rendering output of phase, published separately); `kDash` exists precisely because the Samurai charge is the flagship boss move. Boss roster and authoring: RFC-005. |
| Exactly 4 elements — Fire / Ice / Rock / Thunder | This RFC is element-agnostic; element lives in the opaque impact block (RFC-009). Note: code's `Element::kEarth` ≡ asset "Rock" (BookRock), `kShock` ≡ "Thunder" (BookThunder). Other schools are icons only and appear nowhere in this spec. |
| 121 skill icons @24px with Disabled twins | Cooldown UI is an icon swap (§8) — zero new art. |
| `kEffectLife=6` truncation (Rock FX is 14 frames) | Already fixed in `tiles.hpp` as the per-kind `effect_life_of` table; V4 makes per-kind lifetime + loop-don't-stretch normative for all future FX, including the unpacked Magic/* family and spinning projectiles. |
| No bespoke combo art | Release/Impact visuals are composed from existing FX with tint/layering (umbrella §14–15); this RFC introduces no visual that needs new sheets. |
| RL substrate: DQN, one policy per archetype | Fixed discrete action count, fixed-width integer obs, commit-point actions (§ RL). |
| 1024² world, chunks at 1 Hz or asleep | §4 LOD rules: head on caster ticks, travel dropped at demotion, persist on absolute expiry (I4). |
| Server-authoritative, first-node leader, cheap replication | Trusted-side check-and-debit and cooldowns (§3, §8); one handoff message; tail is capped copied chunk state (§4). |
| Tone guardrail (GAME.md §0) | No clocks behind the player's back: everything in this system is downstream of a deliberate act; hazards self-expire (V3); refunds are forgiving (§5, §6 grace tap); range clamps instead of rejections (§7). |

---

## Open Questions

1. **Q1 — Channel while moving.** v1 roots the caster during Channel. Is a `walk_speed × 0.4`
   slow-walk channel worth the extra animation read (there is no walking-cast pose row), or does
   rooting stay? Leaning: rooted, revisit after the first playtest.
2. **Q2 — kEntity targeting in v1.** With no PvP, no homing (I3), and Impact-side nearest-picking
   already covering melee, does any launch ability actually need `kEntity`, or should v1 ship
   only self/point/direction and reserve the enum value? Cutting it removes the last-known-
   position fallback path entirely.
3. **Q3 — Channel interrupt refund.** §5 rules that a forced interrupt forfeits both the T1 base
   cost and the drained ticks, with half cooldown. An alternative is refund-everything, full
   cooldown. Both are defensible; needs a playtest against the first boss that punishes
   channeling.
4. **Q4 — Stun vs. committed dash.** v1 lets a dash complete through a stun (stun applies on
   arrival). If players find stunning a charging Samurai and watching it dash anyway illegible,
   the alternative is dash-brake-in-place with the stun — but that weakens "a promise is a
   promise" and needs RFC-005/006 sign-off on how a braked dash reads.
5. **Q5 — Payload chunk at the seam.** Zones centered near a chunk border under-cover the
   neighbor (known F1a limitation). This RFC keeps single-chunk ownership and defers the fan-out
   fix to RFC-010 — confirm RFC-010 accepts Persist seam coverage in its scope.
6. **Q6 — `kStaggerTicks` vs. chill.** The 5-tick post-interrupt recovery is anti-exploit, but it
   is also the only place this RFC ever takes control from the player as a *penalty*. If it
   reads as punishment in play, the alternative is making the *enemy* poise thresholds stingier
   instead.

---

## Non-goals

- **The data schema, file format, loader, and hot-reload** — RFC-008. This RFC only fixed the
  consumed fields and invariants V1–V4.
- **Damage numbers, resistances, build-up/poise math** — RFC-009. `charge_mil` and the impact
  block cross that boundary opaquely.
- **Status effects, auras, dispel semantics** — RFC-002.
- **Physics: impulse, mass, materials, terrain interaction of payloads** — RFC-003.
- **The CombatEntity model** (HP/lifetime/collision/team/tags of travel bodies and persists) —
  RFC-004.
- **Boss ability content, per-boss numbers, move sequencing** — RFC-005.
- **All visual/telegraph presentation standards** — RFC-006.
- **Observation/action tensor layouts** — RFC-007. **Cross-chunk fan-out and battlefield-scale
  states** — RFC-010.
- **Sustained (beam/aura) channels** — charge-only in v1 (§6).
- **Player-cast dashes/blinks** — `kDash` is creature/boss-only in v1 (§1); a player dash needs
  a defined transfer of position authority from `PlayerActor` to the chunk during Travel, and
  that transfer is deferred with it.
- **Homing or post-Release retargeting** — excluded by I3, not deferred.
- **A hotbar, a third ability slot, ability swapping mid-combat UI** — the kit is two slots
  (asset-bound); loadout *picking* UI beyond the fixed `equipped_ability` rule is future work
  outside the combat RFC set.
- **Other element schools** (Plant/Water/Light/Darkness/Wind/Death) — icons exist; mechanics are
  explicitly out of scope for v1 across the whole RFC set.
- **PvP interactions and i-frames** — PvP is off by default (GAME.md §11); dodging is spatial.

---

## Review Record

Adversarial review, 2026-07-23 — Reviewer-Opus: **revise**; Reviewer-Sonnet: **revise**.

Applied:
- V2 split into two tiers: baseline `≥ 4` (loads shipped Slime/Spider/Ghost/Skull unchanged), heavy/committed `≥ 8`.
- Persist cap corrected to refusal-not-eviction per RFC-004 §5 (matches shipped `kMaxZones` behavior); ruled no-refund on refused spawns (answers RFC-004 Open Q2), uniform across refusal reasons.
- `kStaggerTicks` given a real write path: per-caster `staggered_until_tick`, §3 admission clause, T12 effect.
- Broken-Channel half cooldown given its write: T12 sets `ready_at_tick = interrupt_tick + ⌈cooldown/2⌉` (§5, §8).
- `kDash` scoped to creature/boss casters in v1; player dash deferred (Non-goals) — closes the player-position-authority gap.
- T1 base cost ruled forfeited on forced Channel interrupt (§5; Q3 updated).
- Grace tap wired into the transition table: sub-grace release (incl. `release_on_move`) routes T4 → T12.
- Charge→power curve handed wholly to RFC-009; `charge_mil = 1000` default for non-channel abilities.
- I4 migration note added: shipped `Zone.ticks_left` must become absolute `expires_at_tick`.
- T4 gained the pre-committed (RL duration-bucketed) release guard; `kBadTarget` stated as `kEntity`-only in v1.
- Header numbering note simplified: the two draft filenames it disambiguated against were never real files in this directory, and IMPLEMENTATION_MAP.md's status table is refreshed separately (RECONCILIATION.md ruling 12).
- Clarity note (Sonnet-only; Opus withdrew it as a blocker): Samurai charge = `kEntity` targeting + `kDash` payload; targeting and payload are orthogonal axes (§7).

Unresolved: none outstanding from this file's own review.

Reconciliation: header numbering note and Review Record dangling references to the two
nonexistent pre-series filenames removed; IMPLEMENTATION_MAP.md's status table refreshed
separately — per RECONCILIATION.md rulings 6 and 12.
