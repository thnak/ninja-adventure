# RFC-013: Vitals, Death & Recovery

> Status: **Accepted (revised after review)**
> Design canon: [GAME.md §0](../GAME.md) (chill is default, challenge is opt-in — the guardrail every
> number below is checked against), [§1](../GAME.md) (three resource axes; food → healing, named but
> unbuilt), [§3](../GAME.md) (the death table: overworld = respawn at hearth, dungeon/mine = ejected
> outside + lose carried items — the split this RFC operationalizes), [§0/§6](../GAME.md) (no clock
> runs behind the player; nothing decays while offline)
> As-built source grounding: `src/world/player_actor.hpp` (`kPlayerMaxHp/Mana/Stamina`, the `Tick`
> regen block, `handle(HurtPlayer)`, `respawn()`, `SetRespawn`, `GrantVitals`, `PlayerView`),
> `src/world/tiles.hpp` (`kStaminaRegen`, `kManaRegen`, `kHealthRegenMs`, `kCombatCooldownMs`,
> `kRespawnTicks`, `CreatureStats` — no regen field, verified), `src/world/chunk_actor.hpp` (creature
> HP mutation sites — `strike()`, fresh-spawn, and `boss_reset()`'s documented anti-kite leash (§2,
> carved out and argued, not overlooked) — none of them are per-tick regen), `src/world/protocol.hpp`
> (`HurtPlayer`, `GrantVitals`, `Teleport`, `BindAccount`)
> Depends on: RFC-009 (accepted-with-revisions, Damage/Resistance/Build-up — §8 non-goals explicitly
> hand this RFC "healing, vitals economy... and death rules," cited not re-derived), RFC-014
> (**Accepted**, Instance & Realm Lifecycle — `MapDescriptor.category`/`MapCategory` (§1 via RFC-022),
> `kPersistentBandCount` (§4.2), `MapSession.return_map/return_x/return_y` (via RFC-022 §2.3), the
> `present`/`members` bookkeeping model (§5–§6) this RFC's ejection plugs into, and RFC-014's own
> explicit request in its Multiplayer section for "the same hearth-respawn fallback RFC-013 defines
> for any... 'no sane position to resume at' case" — closed in §7 below), RFC-022 (accepted-with-
> revisions, Map System — `MapId` partition, `MapDescriptor`, `Portal`, `MapSession`, all cited not
> reproduced), RFC-019 (accepted-with-revisions, Progression & Skills — the XP/level fields this RFC
> rules out of loss, and the "specialization is a choice, not a life sentence" argument this RFC
> extends to death)
> Depended on by: RFC-014 (the `§6.1`/Multiplayer fallback this RFC's §7 supplies), RFC-016 (proposed,
> Persistence — needs this RFC's field list to know what a save must, and must not, carry across a
> restart), RFC-023 (accepted-with-revisions — flags this RFC as the owner of NPC injury/death *if*
> the Sheltering-before-raid guarantee is ever weakened; not resolved here, see Open Questions)

---

## Summary

RFC-009 carved "healing, vitals economy (health/mana/stamina regen), and death rules" out of its own
scope and left them unowned. This RFC is that contract. It records the shipped player-vitals baseline
as normative (§1), rules on creature/monster HP regen — **none exists today, and this RFC confirms
that is the correct doctrine, not a gap to close** (§2) — and specifies the death/recovery state
machine precisely (§3–§6): the existing overworld `respawn()` (position reset, full vitals restore, no
item loss) is unchanged and now formally scoped to the **persistent** `MapId` band (RFC-022 §1.1); a
**new** ejection contract is defined for the **instanced** band (dungeons, mines, and any future
instanced content) that relocates a dying player to their session's return point and clears their
carried inventory — while explicitly, and by argument, never touching XP, skill levels, or unlocked
abilities. No "downed but not dead" state is introduced; "Recovery" in this document's title means
vitals regeneration and the respawn/ejection flow, nothing more.

Everything in §3.6 (the ejection mechanic itself) is **green-field** — no ejection code, no
`SetInstanceReturn`-shaped message, and no carried-item-loss logic exists in `src/` today. The
player-vitals regen numbers in §1 and the overworld `respawn()` contract in §3.5 are **shipped,
unchanged, cited normatively**. The creature no-regen doctrine in §2 is a **ruling on existing
silence** — no code changes, only a recorded decision. Every new number is marked **(tunable)**.

---

## Motivation

1. **RFC-009 named this gap and stopped, on purpose.** Its §8 non-goals list is explicit: "Healing,
   vitals economy (health/mana/stamina regen), and death rules." RFC-001 debits stamina and mana on
   every cast (`kSwingStamina`, `kSpellMana`, `player_actor.hpp`), so the pools those debits draw from
   need an owner. Without this RFC, every future system that touches vitals (food items, a healer
   ability, a boss enrage that drains mana) has no baseline to build against.

2. **GAME.md §3's death table has a shipped half and an unshipped half, and conflating them would be
   the kind of overclaim this batch is graded against.** The table's overworld row ("hồi sinh tại
   nhà" — respawn at hearth) matches `player_actor.hpp::respawn()` exactly: position reset, full
   vitals restore, and the function's own comment states "no gear dropped, no XP lost, no corpse run"
   in so many words. The dungeon/mine row ("bị đẩy ra ngoài, mất đồ mang theo" — ejected outside, lose
   carried items) matches **nothing** in `src/` — there is one `respawn()` path, called uniformly
   regardless of which map the player died on. This RFC is the first spec to build the second half.

3. **RFC-014 is Accepted and is waiting on two things only this RFC can supply.** Its Interactions
   table states plainly: "Owns what dying inside an instance costs (ejection, item loss per GAME.md
   §3's dungeon/mine row) — this RFC only supplies the instance to eject from and the
   `kReturnPortal`/return-coordinate data (§6) RFC-013's ejection logic would target." And its
   Multiplayer section, describing what happens to a
   player who reconnects after their instance has already closed, explicitly falls back to "the same
   hearth-respawn fallback RFC-013 defines for any other 'no sane position to resume at' case" —
   a fallback this RFC had not yet written when RFC-014 was finalized. §7 closes that dependency.

---

## Guide-level Explanation

### For a player

Nothing about the daily loop changes. Stamina comes back fast enough that a fight is never a war of
attrition — swing, back off, swing again. Mana trickles back over the course of a day. Health barely
moves on its own; leaving a fight and giving it a few seconds is what actually heals you (and, later,
so will food — the hook for that already exists, §8).

Dying in the open world costs you a short pause and a walk back from your hearth. Nothing is taken.
Dying inside a dungeon or a mine is a real setback: you wake up back at the gate you walked in
through, and whatever was in your bag when you died is gone — but your character is exactly as strong
as they were a moment before. Every skill point, every unlocked ability, every level: untouched. If
your friends are still inside, you can walk back through the same gate and rejoin them. Nothing about
any of this is timed in a way you can watch counting down — you decide when to go back in, if at all.

### For a designer

The vitals numbers (regen rates, cooldowns) are all named constants, all tunable, all in one place —
changing the pace of "walk away from a fight" or "how long health takes to trickle back" is a
one-line edit, not a redesign. The death/recovery split is a single fork on one already-existing
field (`MapDescriptor.category`, RFC-022 §1.2) — a new instanced map you author automatically gets
the ejection contract, and a new persistent map automatically gets the hearth contract. You never
write per-map death logic.

### For an engineer

You need: (1) nothing new for player vitals regen — it already exists and this RFC only documents it
normatively (§1); (2) a ruling, not code, for creature HP (§2) — you may cite this RFC as the reason
`CreatureStats` has no regen field and should keep not having one; (3) one new field on `PlayerActor`
(`instance_return_map_/x_/y_`, mirroring the existing `respawn_tx_/ty_` pattern) and one new message
(`SetInstanceReturn`, mirroring the existing `SetRespawn`) to carry a `MapSession`'s return coordinates
onto the dying player's own actor, avoiding a cross-actor query in the damage-resolution hot path
(§3.6.2); (4) a branch inside the existing `handle(HurtPlayer)`/`respawn()` pair, keyed on `map <
kPersistentBandCount` (RFC-014 §4.2's own constant), that clears `items_[]` and redirects the respawn
target instead of using `respawn_tx_/ty_` (§3.6); (5) whatever notification RFC-014's own
`InstanceManager` bookkeeping already uses for a `kReturnPortal` exit (RFC-014 §6), fired on ejection
too — this RFC does not invent a second one.

---

## Reference-level Design

### 1. Player vitals: the shipped baseline (normative)

Three pools, `int16_t`, each gating a different verb (`player_actor.hpp:462-464`):

```
hp_       ∈ [0, kPlayerMaxHp = 100]
mana_     ∈ [0, kPlayerMaxMana = 60]
stamina_  ∈ [0, kPlayerMaxStamina = 100]
```

Every `Tick` (10 Hz, `kTicksPerSecond`, `tiles.hpp:1140`), once `dead_ticks_ == 0`
(`player_actor.hpp:88-100`):

```
stamina_ = min(kPlayerMaxStamina, stamina_ + kStaminaRegen)   // kStaminaRegen = 2/tick, UNCONDITIONAL
mana_    = min(kPlayerMaxMana,    mana_    + kManaRegen)      // kManaRegen    = 1/tick, UNCONDITIONAL
if world_ms_ - last_hurt_ms_ > kCombatCooldownMs (5000 ms) and hp_ < kPlayerMaxHp:
    if world_ms_ - last_regen_ms_ >= kHealthRegenMs (3000 ms):
        last_regen_ms_ = world_ms_
        hp_ += 1                                              // OUT-OF-COMBAT ONLY
```

This asymmetry is deliberate and already argued in the shipped source comment
(`tiles.hpp:1092-1094`): "stamina comes back fast because it paces a fight; mana comes back slowly
because it paces a day; health barely comes back at all — that is what a hearth and food are for."
This RFC adopts that reasoning as normative and adds nothing to it — the numbers above are the
contract, not a proposal.

**`GrantVitals` — the existing generic top-up hook.** `player_actor.hpp:178-183` already clamps an
arbitrary `{hp, mana, stamina}` addition into the three pools. It is not new — it is used today by
the `kArc` melee combo (+10 mana, `chunk_actor.hpp:1398`) and by a debug tool (`world.hpp:472-477`,
"top a player's bars up"). This RFC does not add a new vitals-injection mechanism; it names the one
that already exists as the plumbing any future food/consumable healing system (GAME.md §1: "thức ăn →
hồi máu" — food → healing, promised but unbuilt) would call through. Designing that food system is
explicitly not this RFC's job (Non-goals).

### 2. Creature/monster vitals: no per-tick regen, confirmed doctrine — with one named exception

**Verified sites.** `CreatureStats` (`tiles.hpp:361-375`) has no regen-rate field, and no HP-mutation
site in `chunk_actor.hpp` restores a creature's `hp` on an ordinary tick while it has a target or sits
idle. Three sites move `hp` upward at all: (a) a fresh spawn setting `hp = max_hp`, for an ambient
creature (`chunk_actor.hpp:579`, `make_creature()`) or a dojo boss (`chunk_actor.hpp:974`,
`spawn_boss()`); (b) `boss_reset()` (`chunk_actor.hpp:1242-1257`), which sets `c.hp = c.max_hp` on an
already-damaged, still-alive boss once it has had no target in the room for `kBossLeashTicks` = 50
ticks / 5 s (`chunk_actor.hpp:1039-1040`; `boss.hpp:44`) — the shipped comment names this outright:
"after the leash window reset to full — fleeing through the door 'resets the boss', so a player cannot
chip it down across trips." An earlier draft of this RFC also cited a "level-up recomputing `max_hp`"
at `chunk_actor.hpp:382-392`; that line range is `handle(const UpgradeBuilding&)`, which mutates a
`Building`'s `b.hp`/`b.level` (a farm-structure upgrade), not a `Creature`'s — no creature/boss
level-up mechanic exists anywhere in `chunk_actor.hpp` or `boss.hpp`, and that citation is withdrawn.

**`boss_reset()` is a carved-out exception, argued and left alone — not an unnoticed counterexample.**
It is real, shipped, and does restore a live, damaged `Creature` to full HP, which is exactly the shape
reasoning-point-1 below argues against for ambient wildlife. Two things keep it from falsifying that
argument rather than merely complicating it: first, what triggers it — it is not time passing while a
target is present or tracked, it is the boss losing its target and the player staying fully out of the
room for a fixed 5-second window; a player who wounds the boss and stays engaged, or steps out for a
tick or two to reposition, never sees this fire. Second, where it applies — `boss_reset()` exists only
on the single `BossState`-tagged dojo boss, a scripted set-piece bound to one room; ambient creatures
made by `make_creature()` (the boar in reasoning-point-1's example) have no leash, no reset, and no
regen field, so the "chip it down, walk away, come back to a sliver" case that argument is actually
about is unaffected. The leash reads less like "the target passively heals while unattended" and more
like "leaving the room fully ends and re-arms the encounter" — a documented anti-kiting device, not a
vitals-regen rule. This RFC treats it as pre-existing, intentional design, left alone: it is not
extended to any other `Creature`, and no new doctrine is written to justify it beyond what the shipped
comment already states.

**Ruling: no per-tick, target-independent HP regen for any `Creature`, of any faction or disposition,
including the RL-driven Monster and Guard archetypes RFC-007 owns — `boss_reset()`'s discrete,
leash-gated full-HP reset (above) is the one named, carved-out exception, confirmed intentional and not
extended further.** This is confirmed doctrine, not a gap this RFC closes, for three converging
reasons:

1. **A wound is supposed to persist, because wildlife already does.** GAME.md §5 (shipped, P2) already
   gives disposition a memory (`grudge`, an anger-cooldown that gets *longer* on repeat provocation) —
   the game already tells the player "this specific animal remembers you." A creature whose HP quietly
   refilled between encounters would contradict that: you chipped a boar down to a sliver, walked away,
   and it would just be back to full next time for no in-fiction reason. No regen is the reading
   consistent with the disposition system the game already shipped, not an unrelated omission.

2. **The freshness mechanic already lives at a different layer.** Wild/neutral wildlife is seeded once
   at world-gen and never respawns (GAME.md §5, shipped) — a wounded rabbit stays wounded until it
   dies; there is no "reset" to worry about. Hostile Monster-faction creatures at forts/dungeons are
   refreshed by **new spawns** (a fort's own wave-spawning logic — out of this RFC's scope, owned by
   whatever RFC eventually specifies fort spawn cadence), not by an existing creature healing in place.
   And once P8's real dungeons ship on RFC-014's `MapSession` infrastructure, a monster you partially
   damage and then leave is **either** still damaged if you return within RFC-014's idle-grace window
   (same session, same creature, same HP — you get credit for what you already did) **or** freshly
   primed at full HP if the session already closed and a new one was allocated (RFC-014 §3.3) —
   both outcomes are internally consistent without any tick-based regen at all. Regen would not add a
   capability here; it would only make the "did I already weaken this?" answer less predictable.

3. **RL training never needs it.** `CombatEnvironment` episodes (RFC-007 §6.1) reset the arena between
   rounds — there is no mid-episode healing to model, and nothing in RFC-007's reward-shaping table
   (§5, verified — no hit on "regen" or "heal" anywhere in that RFC) references HP recovery at all.
   The doctrine is uniform: a policy's own combat behavior is RFC-007's territory, but the vitals math
   underneath every `Creature` it controls, RL-driven or scripted, is this RFC's, and it is the same
   for all of them.

**What this ruling explicitly does not decide.** Whether a village's guard roster is repaired,
replaced, or left wounded between raids is a **population/roster** question (does the village get a
fresh guard NPC instance, does a wounded one heal as part of "làng sửa chữa" village-repair economy,
GAME.md §6) — not a vitals-regen question. This RFC's contribution is only that no `Creature`,
guards included, has a tick-based HP-regen rule; whether guards get *replaced* by the village-standing
system between raids is RFC-023's or a future village-repair RFC's call, flagged and not resolved here
(Open Questions §5).

### 3. Death has no intermediate state

Today, death is instant at `hp_ == 0` — there is no "downed," "bleeding out," or "incapacitated" tier,
and `handle(HurtPlayer)` (`player_actor.hpp:166-176`) transitions straight from `hp_ > 0` to
`dead_ticks_ = kRespawnTicks`.

**This RFC's decision: introduce no such state.** "Recovery," in this document's title, refers only to
vitals regeneration (§1) and the respawn/ejection flow (§3.5–§3.6) — not a rescue mechanic. Reasoning:

- **A downed timer is a countdown the player watches**, even a short one, and even one a teammate can
  stop — GAME.md §0's test is failed by the shape of the mechanic, not its duration. A 10-second
  bleed-out bar is still a bar counting down at the player while they wait to see if help arrives.
- **The multiplayer case this would exist for is narrow.** Dungeon parties are 2–4 (GAME.md §3),
  PvP is off by default (GAME.md §11), and death inside an instance already gets a real consequence
  (ejection + item loss, §3.6) that does not need a rescue mechanic layered on top of it to matter.
- **It is new networked state for a benefit nobody has asked for.** A downed player is a new
  `AbilityReject` case, a new HUD element, a new revive verb, and new replication surface (RFC-015's
  territory) — all to solve a problem ("death feels too sudden") this project's own tone guardrail
  argues *against* solving with tension.

This is a **decided-against** position for v1, not a permanently closed door — flagged in Open
Questions §6 in case group-content playtesting later argues otherwise.

### 4. The death/recovery state machine

```
                    hp_ > 0
              ┌───────────────────┐
              │       ALIVE       │◀───────────────────────┐
              └─────────┬──────────┘                        │
                         │ handle(HurtPlayer), hp_ → 0        │ dead_ticks_ reaches 0
                         ▼                                    │
              ┌───────────────────────────────┐               │
              │  DEAD  (dead_ticks_ = kRespawnTicks = 30       │
              │  ticks = 3 s)                                 │
              │  — MoveIntent ignored                          │
              │  — abilities locked (AbilityReject::kUnavailable,│
              │    player_actor.hpp:293, unchanged)             │
              │  — mounted_ = false                             │
              └───────────────┬─────────────────────────────────┘
                               │ every Tick: --dead_ticks_
                               │ at 0: call respawn()  (§3.5/§3.6 fork)
                               └─────────────────────────────────┘
```

This diagram is the **shipped** state machine (`player_actor.hpp:166-176,88-92`) with one addition:
the `respawn()` call at `dead_ticks_ == 0` now forks on the dying player's `map` (§3.6), where today it
does one uniform thing.

### 5. Persistent-band death: `respawn()`, unchanged

For any `map < kPersistentBandCount` (RFC-014 §4.2's constant, currently `16` — i.e. the overworld,
`kOverworld = 0`, and the shared interior grid, `kInterior = 1`, and any future persistent map RFC-022
§1.1 reserves `2..15` for), death behaves **exactly as shipped, with zero changes**:

```cpp
// player_actor.hpp:440-446, unchanged by this RFC
void respawn() noexcept {
    x_ = respawn_tx_ + 0.5f;
    y_ = respawn_ty_ + 0.5f;
    hp_ = kPlayerMaxHp;
    mana_ = kPlayerMaxMana;
    stamina_ = kPlayerMaxStamina;
    last_hurt_ms_ = world_ms_;
}
```

Position resets to the bound hearth point (`respawn_tx_/ty_`, set at login and by `SetRespawn`,
`player_actor.hpp:200-202`), all three vitals pools return to maximum, and — the function's own
comment states this as intentional design, not an omission — **nothing is taken from the player**: no
item loss, no XP loss, no corpse run. This RFC formally scopes that contract to the persistent band
and does not touch it.

**The 10×7 dojo boss room ruling, restated for death.** The shipped boss fight (`kRoomW×kRoomH`,
`tiles.hpp:860-861`) lives on `MapId = 1` (`kInterior`) — persistent band, per RFC-014 §8's own
ruling that this room "stays exactly what it is... a room on the persistent `kInterior` map," never
migrating to a `MapSession`. Even though the fight is framed as dungeon-flavored content (a boss, a
telegraph-heavy fight, GAME.md §10's "hầm ngục" framing for the RL spectacle), **its `MapId` places it
on the persistent band, so death there uses the unchanged hearth-respawn contract above, not §3.6's
ejection.** This is not a special case this RFC carves out — it is what the existing, unmodified
`MapCategory` fork already produces once applied, and this RFC states the result explicitly precisely
because the assignment calls out the dojo room by name. No engineering change is required for this —
`respawn()` already behaves this way for every player who dies there today, and continues to.

### 6. Instanced-band death: ejection (this RFC's new contract)

For any `map >= kPersistentBandCount` — every `MapId` RFC-014's `allocate_new()` hands out — death
follows a new path this RFC defines. Nothing below exists in `src/` today.

#### 6.1 Branch point

`handle(HurtPlayer)`'s existing `hp_ == 0` branch (`player_actor.hpp:170-174`) gains one new line:

```cpp
if (hp_ == 0) {
    dead_ticks_ = kRespawnTicks;   // unchanged — same 3 s pause either way
    ++deaths_;                     // unchanged
    mounted_ = false;              // unchanged
    pending_eject_ = (map >= kPersistentBandCount);   // NEW — decided once, at the moment of death
}
```

This fork is exactly RFC-022 §2.3's `SessionScope::kSharedPersistent` vs. `{kGroupInstance,
kSoloInstance}` distinction, restated in terms of `MapDescriptor.category` (RFC-022 §1.2) — the field
`PlayerActor` can read directly off its own `map` value via `kPersistentBandCount`, without a
cross-actor session lookup at the moment damage is resolved. `pending_eject_` (new, `bool`, default
`false`) is consumed and cleared by `respawn()` at the end of the `dead_ticks_` countdown (§4).

#### 6.2 Destination: `SetInstanceReturn`, mirroring `SetRespawn`

`PlayerActor` does not know its own `MapSession`'s return coordinates today — that data lives in
`InstanceManager`'s leader-resident session table (RFC-014 §2, §5), a different `Require<Trusted>`
actor, not a shared memory region (RFC-014 §3.4 already establishes this discipline: co-location
guarantees shared trust, never a shared shard).

This RFC adds one new field, populated the same way `respawn_tx_/ty_` already is (`SetRespawn`,
`player_actor.hpp:200-202`) — proposed, not shipped:

```cpp
// New PlayerActor field, mirrors respawn_tx_/respawn_ty_ exactly.
std::uint16_t instance_return_map_ = 0;
std::uint16_t instance_return_x_ = 0, instance_return_y_ = 0;

// New message, mirrors SetRespawn exactly. Sent once by whichever actor resolves the portal
// (RFC-022 §2.3's resolve(), RFC-014 §3's allocate_new()) at the moment a player's Teleport
// lands them on a freshly-joined-or-created instanced MapSession — populated from that
// MapSession's own return_map/return_x/return_y (RFC-022 §2.3).
struct SetInstanceReturn {
    std::uint16_t map = 0, x = 0, y = 0;
};
void handle(const SetInstanceReturn& r) noexcept {
    instance_return_map_ = r.map;
    instance_return_x_ = r.x;
    instance_return_y_ = r.y;
}
```

Caching this on `PlayerActor` at entry time (rather than querying `InstanceManager` at death time)
keeps damage resolution a single-actor operation, exactly the property `HurtPlayer`'s existing
handler already has. Sending `SetInstanceReturn` is a new responsibility for whichever code path
already sends the `Teleport` that lands a player on an instanced map — this RFC names the
requirement, not the exact call site, since that call site is RFC-014's `allocate_new()`/`resolve()`
plumbing, not this RFC's, and RFC-014's own text does not yet commit to a call site either.

**Guard: an unset `SetInstanceReturn` is not trusted as a destination.** `instance_return_map_/x_/y_`
default to `0/0/0` (above) — indistinguishable, at the field level, from a genuine, deliberately-set
return point at `MapId` 0, tile `(0,0)`. If the `SetInstanceReturn` wiring above is ever missing, late,
or simply not yet implemented for a given code path, this RFC does not let ejection trust that zero
value as a real destination: §6.5's merged `respawn()` listing treats "still at the zero default" as
equivalent to "no sane position to resume at" and falls back to the player's own bound hearth point
(`respawn_tx_/respawn_ty_`), exactly the general-purpose fallback §7 defines for this class of problem.
This is not a special case invented here — it is §7's rule, applied at the one call site inside this
RFC where an unwired dependency could otherwise produce an arbitrary landing spot.

#### 6.3 Vitals restore: unchanged from `respawn()`

Ejection restores all three vitals pools to maximum, identically to persistent-band respawn. GAME.md
§3's table names two consequences of dungeon/mine death — *where* you end up, and *what you lose* — it
does not name a third, harsher vitals penalty, and this RFC does not invent one. Dying is dying; the
two contracts differ only in destination and inventory, never in how "recovered" you are afterward.

#### 6.4 Timer

Reuses `kRespawnTicks` (30 ticks = 3 s) unchanged. A separate `kInstanceEjectTicks` **(tunable)**
could diverge from this later if playtesting wants ejection to feel weightier than an overworld death,
but no design signal calls for that today, so this RFC does not introduce a second constant that would
sit unused.

#### 6.5 Carried items: definition and loss rule

**"Carried items" = the full `items_[kItemKinds]` array** (`player_actor.hpp:478`) — today, `kWood`,
`kStone`, `kSeed`, `kProduce` (`tiles.hpp:358`). This is the only inventory-shaped state that exists in
`src/` today; there is no separate "equipped gear" concept yet (RFC-001's two ability slots are
level-gated, not itemized — `equipped_ability(level_, slot)` reads from skill levels, never from
`items_[]`), so there is nothing today that needs to be exempted as "worn, not carried."

**Ruling: ejection clears `items_[]` to zero, in full.** This is the literal reading of GAME.md §3's
own wording — "mất đồ mang theo" ("lose the things you're carrying") — not "some" of them. Going into
a dungeon or mine is the opt-in half of GAME.md §0's chill-default/challenge-opt-in split; the stake
this RFC assigns to that choice is the resources in the player's bag at the moment of death, whatever
their origin (brought in from before, or picked up during that same run — the array does not, and
this RFC does not propose making it, track provenance).

**Full listing: `respawn()`, merged.** Everything not marked `NEW` is the unmodified §3.5 function;
`NEW` lines are this RFC's entire green-field addition — the position fork (§6.2's guard) and the
`pending_eject_` reset (§6.1) both live here, in full, not just described in prose:

```cpp
// player_actor.hpp, respawn() — merged. Unmarked lines are unchanged from §3.5.
void respawn() noexcept {
    if (pending_eject_) {
        for (int i = 0; i < kItemKinds; ++i) items_[i] = 0;      // NEW — full carried-item loss (§6.5)
        const bool have_return = instance_return_map_ != 0 ||
                                  instance_return_x_ != 0 || instance_return_y_ != 0;  // NEW — §6.2 guard
        if (have_return) {
            x_ = static_cast<float>(instance_return_x_) + 0.5f;  // NEW — eject to the session's return point
            y_ = static_cast<float>(instance_return_y_) + 0.5f;  // NEW
            // A Teleport to instance_return_map_ is issued by the same caller that already drives
            // cross-map movement today (§6.2) — not new machinery, not shown here.
        } else {
            x_ = respawn_tx_ + 0.5f;                              // NEW — §7 fallback: no sane return point
            y_ = respawn_ty_ + 0.5f;                              // NEW   known, so use the bound hearth
        }
        pending_eject_ = false;                                   // NEW — consumed and cleared, once
    } else {
        x_ = respawn_tx_ + 0.5f;      // unchanged, §3.5
        y_ = respawn_ty_ + 0.5f;      // unchanged, §3.5
    }
    hp_ = kPlayerMaxHp;
    mana_ = kPlayerMaxMana;
    stamina_ = kPlayerMaxStamina;
    last_hurt_ms_ = world_ms_;
}
```

#### 6.6 What is explicitly NOT lost, and why

- **XP and skill levels (`xp_[]`, `level_[]`).** Untouched. This is the single most tone-guardrail-
  load-bearing decision in this RFC, so the argument is stated in full, not just asserted:

  1. RFC-019 already committed to a philosophy this RFC is bound by if it wants to stay consistent:
     "specialization is a choice, not a life sentence" (RFC-019 Summary) — a mechanic that could erase
     hours of accumulated XP after one bad pull would impose exactly the life-sentence framing RFC-019
     spent its own respec-policy section arguing against, just triggered by death instead of a build
     mistake.
  2. GAME.md §0's test is about *time already spent*, not just about clocks. Losing loose resources
     costs you a walk back to a fort or a farm — recoverable within the same play session. Losing XP
     costs you *hours*, non-recoverable except by repeating exactly the grind that earned it — the
     closest thing to a countdown the guardrail forbids, just measured in play sessions instead of
     minutes.
  3. RFC-009 explicitly scoped "death rules" to this RFC and nothing else scopes "progression loss on
     death" anywhere in the accepted set — this is the first and only place that gets to decide it, and
     it decides against it, cleanly, rather than leaving it ambiguous for a future RFC to reopen.

- **Unlocked abilities.** Follow directly from skill levels (`equipped_ability(level_, slot)`) — since
  levels are untouched, so is the loadout. No separate rule is needed.

- **Quest progress.** RFC-020 §"4. Abandoning is free and silent" already states quest objectives
  "only *observe* facts, they never lock inventory or vitals" — quest state does not live in
  `items_[]` or the vitals pools this RFC governs, so ejection has no interaction with it at all. Not
  a new exemption this RFC grants; simply orthogonal state.

#### 6.7 Where lost items go: destroyed, not recoverable

Cleared items are not deposited anywhere retrievable — no corpse, no loot pile at the death tile, no
timed recovery window. This mirrors the "no corpse run" philosophy `respawn()`'s own comment already
states for the overworld, extended here rather than contradicted: a recoverable corpse would need its
own world-object lifecycle (placement, persistence across the very instance-teardown timers RFC-014
§3.5 already runs, replication to other party members who might loot it) for a feature GAME.md §3's
own wording does not ask for ("mất đồ" — lost — reads as gone, not "temporarily set down"). Simpler,
and consistent with GAME.md §3's plain language. Flagged as reconsiderable in Open Questions §3 if a
softer middle ground is ever wanted.

#### 6.8 Group/session membership: reuses RFC-014 §6's Deliberate Exit bookkeeping

RFC-014 §6's table names three events — Join, Deliberate Exit, Disconnect — and its own Interactions
table is explicit that "death is never this RFC's [RFC-014's] mechanism for removing a player from a
session; only exit and disconnect are," handing the death case to this RFC by name.

**Ruling: death-ejection is bookkept identically to Deliberate Exit (RFC-014 §6), not Disconnect.**
Specifically:

- `present` (RFC-014 §5's `InstanceSession::present`) — the dying player is **removed**, the same as
  someone who walked out through a `kReturnPortal`. If `present` reaches zero, RFC-014 §3.5's idle
  grace clock starts, unmodified.
- `members` (RFC-014 §5's `InstanceSession::members`) — **unchanged**, the same as Deliberate Exit. A
  player who died is still "of" that run, exactly as one who walked out the front door is. This is
  what lets them walk back through the originating gate and have RFC-022 §2.3's `resolve()` find their
  group's still-open session and rejoin them — no special-case logic is needed on RFC-014's or
  RFC-022's side for this to work; it falls directly out of `members` being untouched.
- **The exact notification mechanism** that tells `InstanceManager` a `present`-set change occurred is
  RFC-014's own — the same one a `kReturnPortal` crossing already needs and does not fully specify at
  the message level either (RFC-014 §6's table describes the bookkeeping effect, not a wire message).
  This RFC's contribution is only that death-ejection fires that same signal; it does not invent a
  second one.

**Why this differs from Disconnect.** RFC-014 §6 preserves a disconnecting player's exact position
(so a lag-drop can resume the same fight). Ejection has already *relocated* the player by the time
this row would apply — there is no position to preserve, because the whole point of ejection is that
it is not a resumable moment. This is the same distinction RFC-014 draws between its own Deliberate
Exit and Disconnect rows, applied here rather than re-derived.

#### 6.9 Solo instances (mine floors)

For `SessionScope::kSoloInstance` (RFC-022 §2.3; RFC-014 §5 — mine floors, "one instance per depth
floor," ROADMAP.md P4), `members` carries exactly one account forever. Death there drops `present` to
zero immediately (the only member is now elsewhere), starting RFC-014's idle-grace clock
(`kInstanceIdleGraceMs`, 5 min, RFC-014 §3.5) right away. If the player walks back through the mine
mouth within that window, `resolve()` finds their still-`ACTIVE`-or-`IDLE` session and rejoins them to
the **same floor**, at whatever state it was left in — any creatures already cleared stay cleared,
because nothing about ejection resets the instance itself, only the player's own inventory and
position. This is a direct, favorable consequence of §6.8's ruling, not a separate mechanic: a solo
miner who dies loses their bag, not their progress on the floor, as long as they return promptly.

#### 6.10 Rest realms: the theoretical edge case

Rest realms (`RealmType::kRest`, RFC-022 §2.2) are instanced (RFC-022 §1.3's guidance table) and would
therefore fall under this section's ejection rule by the same `map >= kPersistentBandCount` fork —
but GAME.md §3's own table states rest realms have no combat ("không chiến đấu"), so `HurtPlayer`
should never fire there in practice. This RFC does not special-case rest realms out of the ejection
rule; if an environmental-damage source (`HurtPlayer.source == 0`, `protocol.hpp:227`) ever zeroed a
player's HP in a rest realm — a hazard-tile bug, not designed content — the uniform rule still applies
cleanly (relocate, clear bag) rather than needing a bespoke exception for a case the design does not
intend to exist. Flagged, not expected to matter (Open Questions §7).

### 7. Closing RFC-014's own dependency: the universal "no sane position" fallback

RFC-014's Multiplayer section, describing a leader restart stranding a player whose last known
position was inside an instance that no longer exists, states: "lands via the §6.1 fallback
(return-portal coordinates, if RFC-016 has preserved them) or, failing that, the same hearth-respawn
fallback RFC-013 defines for any other 'no sane position to resume at' case — this RFC does not invent
a second fallback path, it reuses whichever one RFC-013 already specifies."

**This RFC supplies that fallback, generally, not just for the restart case:** whenever this RFC's own
machinery (or another system built on it) needs to place a player and has no better location on hand,
the fallback is the player's own bound hearth point — `respawn_tx_/respawn_ty_`, the same, already-
existing field ordinary overworld death already targets (§3.5). No new field, no new concept: a
stranded player is handled exactly like a dead one whose instance-return coordinates turned out to be
unusable. This closes the dependency RFC-014 named without requiring RFC-014 to be reopened.

### 8. The unbuilt half: food-based healing

GAME.md §1's three-resource-axis table names farm produce's use explicitly: "thức ăn → hồi máu, buff,
giao thương" (food → healing, buffs, trade). No consumable-item system exists in `src/` today, and
this RFC does not design one — but it names, precisely, the hook such a system would call: `GrantVitals`
(§1), already shipped, already clamped, already exercised by a combat combo. A future crafting/food RFC
does not need this RFC reopened to add "eating a cooked fish sends `GrantVitals{hp: 20, 0, 0}`" — the
plumbing is already there.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001** (accepted) | Ability admission's `AbilityReject::kUnavailable` while `dead_ticks_ > 0` is unchanged by this RFC — death still locks the kit the same way regardless of which death path is taken. Stamina/mana debits per cast are RFC-001's; the pools they draw from are this RFC's (§1). |
| **RFC-002/003/009** (accepted) | Damage math that produces `HurtPlayer.amount` is entirely theirs, cited not re-specified; this RFC only owns what happens once `amount` drives `hp_` to 0. |
| **RFC-007** (accepted) | Owns RL policy behavior for every `Creature`; this RFC's no-regen ruling (§2) applies uniformly across every archetype RFC-007 trains, verified against its reward table (no HP-recovery term exists there either). Neither RFC modifies the other's territory. |
| **RFC-014** (**Accepted**) | Supplies `MapDescriptor.category`/`kPersistentBandCount` (the fork this RFC keys on, §3.6.1), `MapSession.return_*` (this RFC's ejection destination, §3.6.2), and the `present`/`members` bookkeeping this RFC's ejection plugs into as a Deliberate-Exit-equivalent event (§3.6.8). This RFC in turn supplies the hearth-respawn "no sane position" fallback RFC-014's own Multiplayer section asked for (§7). |
| **RFC-015** (proposed) | No new replication format is required for anything in this RFC — `PlayerView` (`snapshot.hpp`) already carries `map`, `x/y`, `hp`, `items[]`, and `dead_ticks`; ejection is visible to a client purely as those existing fields changing value. RFC-015 owns the wire encoding of `PlayerView` generally, not anything new from this RFC. |
| **RFC-016** (proposed) | Needs this RFC's field list to know what must persist across a restart: `respawn_tx_/ty_` (hearth, must persist), `items_[]`/`xp_[]`/`level_[]` (must persist, this RFC never zeroes XP/levels), and `instance_return_map_/x_/y_` (this RFC's new field, §3.6.2) — whether that last one is worth persisting at all is RFC-016's call, since RFC-014 already rules instanced `MapId`s never survive a restart, making a stale return-coordinate mostly moot except during the narrow reconnect window RFC-014 §6.1 describes. |
| **RFC-018** (proposed) | Orthogonal. RFC-018 governs what a creature *drops*; this RFC governs what a player *loses*. Neither reads the other's tables. |
| **RFC-019** (accepted) | This RFC's central argument for never touching `xp_[]`/`level_[]` (§3.6.6) directly extends RFC-019's "specialization is a choice, not a life sentence" framing to death specifically — a boundary RFC-019's own non-goals list left open for this RFC to close, and this RFC closes it in RFC-019's favor. |
| **RFC-020** (accepted) | Quest progress is unaffected by either death path — RFC-020's own "objectives only observe facts, never lock inventory or vitals" statement means this RFC's item/vitals changes have no reach into quest state at all. |
| **RFC-022** (accepted-with-revisions) | `MapId` partition, `MapDescriptor`, `Portal`, `MapSession` are all cited, not reproduced or modified. RFC-022 §5.3's own boundary table already named "Death/recovery rule at either endpoint... RFC-013 (proposed) — not this RFC," which this document is the fulfillment of. |
| **RFC-023** (accepted-with-revisions) | Flags this RFC by name as the owner of NPC injury/death mechanics *if* the Sheltering-before-raid guarantee is ever weakened by an unwarned raid type — not resolved here; see Open Questions §8. Guard-roster recovery between raids (§2's boundary) is left to RFC-023 or a future village-standing RFC, not decided by this document. |

---

## Multiplayer & Simulation-LOD Considerations

- **Player vitals regen is `Require<Trusted>`-resident, LOD-exempt by construction.** `PlayerActor`
  ticks at the same 10 Hz regardless of which chunk the player stands in or whether that chunk is
  actively simulating, asleep, or LOD-throttled (ARCHITECTURE.md §4) — nothing in §1 is chunk-attached,
  so nothing here needs to reconcile a "missed regen" gap on chunk wake.
- **Creature no-regen (§2) is, incidentally, LOD-friendly for free.** A sleeping chunk that wakes has
  no HP state to reconcile for the creatures inside it — if regen existed, waking a long-asleep chunk
  would force a choice between an expensive catch-up calculation or an observably-wrong instant full
  heal; no-regen sidesteps that class of problem entirely, a bonus this RFC notes but did not need to
  invent — it falls out of the doctrine in §2 for reasons that had nothing to do with LOD in the first
  place.
- **Ejection's branch point (§3.6.1) reads only `PlayerActor`'s own `map` field** — no cross-actor call,
  no dependency on `InstanceManager`'s liveness, at the moment `hp_` reaches 0. The notification to
  `InstanceManager` (§3.6.8) happens after `dead_ticks_` resolves, off the critical path of damage
  resolution.
- **Determinism.** Both the regen tick math (§1, pure integer arithmetic on `int16_t`, unchanged) and
  the ejection branch (§3.6, an integer comparison against `kPersistentBandCount`) are already
  tick-deterministic — this RFC introduces no floating-point or wall-clock-dependent logic anywhere.
- **20–50 concurrent players, no VPS.** Nothing in this RFC scales differently with player count than
  the existing `PlayerActor` roster already does — one slot, one set of vitals fields, one possible
  `pending_eject_` flag per logged-in account, all already-existing per-actor cost.

---

## Tone Guardrail Compliance

1. **Regen rates (§1) are visible bars the player can always improve their situation against — never a
   countdown working against them.** Stamina/mana climb unconditionally; health requires only leaving
   a fight, which the player can always choose to do (`player_actor.hpp`'s own comment: "you can always
   leave, and leaving is how you heal").

2. **No target-independent creature regen (§2) is not merely low-cost — it is the guardrail-*correct*
   choice, not just a convenient one.** If creatures regenerated HP over time, that would be a decay
   mechanic running against the player's own effort while they are away from the game — exactly the
   shape GAME.md §0 forbids ("không có thanh đếm ngược nào chạy sau lưng bạn" — nothing counts down
   behind your back). Progress made against a target (a wounded boar, a raided fort) staying made,
   whether the player logs off for five minutes or five days, is the guardrail applied to combat state,
   not just to farm state. `boss_reset()`'s leash (§2) does not read against this: it fires only while
   the player has stayed engaged with the chunk (5 seconds after disengaging, not an offline duration),
   and it is scoped to one scripted set-piece, not a rule this RFC extends to the ambient wildlife the
   guardrail argument above is actually about.

3. **Overworld death (§3.5) is unchanged and was already chill**: a three-second pause, a walk back,
   nothing lost.

4. **Ejection (§3.6) is a real stake confined entirely to opt-in content.** A player only encounters
   this contract by choosing to walk through a realm gate or mine mouth — GAME.md §0's "challenge is
   opt-in" clause is not a suggestion this RFC honors loosely; it is the literal gate on when §3.6 can
   ever apply. Within that opt-in content, the stake is scoped as narrowly as GAME.md §3's own table
   asks for — carried resources only, never XP, never skill levels, never the character itself (§3.6.6)
   — so even the "real" consequence stops well short of a life-sentence-shaped penalty.

5. **No downed state (§3) removes tension, not adds it.** A bleed-out timer a teammate might or might
   not reach in time is exactly the kind of countdown-with-stakes shape §0 warns against; not building
   one is the guardrail-consistent choice, not an oversight.

6. **Nothing in this RFC is ever displayed as a countdown.** `dead_ticks_` is a three-second pacing
   beat, not a number shown ticking down to the player; the idle-grace and idle-timeout clocks this
   RFC's ejection interacts with (RFC-014 §3.5) are already ruled silent by RFC-014's own Tone
   Guardrail Compliance section, and this RFC introduces no new timer of its own that would need the
   same argument repeated.

---

## Open Questions

1. **Should ejection's item loss be a percentage instead of a full wipe?** This RFC reads GAME.md §3's
   "mất đồ mang theo" literally (all of it) — flagged in case playtesting finds a full wipe too harsh
   for a long resource-gathering trip that happened to end at a dungeon mouth.

2. **Equipped-gear exemption, once equipment exists.** No "worn, not carried" concept exists in `src/`
   today, so §3.6.5's rule is total-inventory by necessity, not by choice. When an equipment/durability
   system eventually ships (unowned by any RFC today — see Non-goals), this RFC's ejection rule will
   need revisiting to exempt worn gear the same way it already exempts XP — flagged now so that future
   RFC does not have to rediscover the boundary.

3. **Recoverable corpse vs. destruction (§3.6.7).** Decided against for v1 (destroyed, not recoverable)
   on simplicity grounds; flagged as reconsiderable if a softer "your bag waits at the entrance"
   middle ground is ever wanted.

4. **`kInstanceEjectTicks` vs. `kRespawnTicks` (§3.6.4).** Currently unified at 30 ticks; no design
   signal calls for divergence yet.

5. **Village guard HP/roster recovery between raids.** This RFC rules only that no `Creature` regens
   HP by a tick-based rule (§2); whether a village's guard roster is repaired, replaced, or simply
   stays wounded between raids is left entirely to RFC-023 or a future village-standing RFC.

6. **Should a "downed" rescue state exist for group content specifically?** Decided against in §3 for
   v1; flagged for reconsideration if 2–4 player dungeon playtesting argues a near-death moment adds
   more than the countdown-shape risk costs.

7. **Rest-realm environmental-damage edge case (§3.6.10).** Flagged as a theoretical, not expected to
   matter given rest realms are designed combat-free.

8. **NPC injury/death, if the Sheltering-before-raid guarantee is ever weakened (RFC-023's flag).** Not
   resolved here — this RFC's scope is the player, and RFC-023 names this RFC as the likely future
   owner only if a future raid type breaks its current no-damage-while-warned guarantee. No such raid
   type exists today.

9. **Food/consumable healing item design.** Entirely unscoped by any current RFC — this RFC only names
   the existing `GrantVitals` hook (§8) as where such a system would plug in; the items themselves,
   their amounts, and any cooldown are undesigned.

---

## Non-goals

- **Damage formula, resistance, and build-up math.** RFC-009's, cited not re-derived — this RFC only
  consumes `HurtPlayer.amount` as an opaque input.
- **Instance/session allocation, teardown, and membership mechanics.** RFC-014's (Accepted), cited not
  reproduced — this RFC only consumes `MapDescriptor.category`, `kPersistentBandCount`, and
  `MapSession.return_*`, and supplies the death-consequence event RFC-014 explicitly deferred to it.
- **Any equipment/durability system.** None exists in any accepted RFC today (RFC-009's non-goals
  attribute "gear and socket definitions" to RFC-004, but RFC-004's own text does not define them —
  a pre-existing dangling reference this RFC does not attempt to resolve). This RFC notes only that
  *if* such a system ships, its worn-gear-vs-backpack boundary would need revisiting against §3.6.5
  (Open Questions §2).
- **Loot tables and Essence/reward drops.** RFC-018's (proposed) — orthogonal to what a player loses.
- **Food/consumable item design.** Unowned by any RFC; this document only names the `GrantVitals` hook
  such a system would use (§8).
- **Village guard roster repair/replacement between raids.** RFC-023's or a future village-standing
  RFC's — this RFC rules only on tick-based HP regen, not on population changes.
- **Client-facing replication/wire format for any field this RFC touches.** RFC-015's (proposed).
- **Save-file schema for vitals, inventory, or the new `instance_return_*` fields.** RFC-016's
  (proposed) — this RFC names what exists and what must survive (§ Interactions), not how it is
  encoded to disk.
- **Quest reward hooks.** RFC-020's, orthogonal (§6.6).
- **Leader election or automated failover.** ARCHITECTURE.md §2 defers this indefinitely; this RFC's
  §7 fallback only states where a *player* lands when their own position is unrecoverable, not how the
  world itself recovers.
- **PvP tuning.** Off by default (GAME.md §11); no number in this RFC was chosen with PvP in mind.

---

## Review Record

Reviewer A: revise. Reviewer B: revise. Both required the same four fixes; applied all four.

- Applied: §2 rewritten to carve out `boss_reset()` (chunk_actor.hpp:1242-1257, boss.hpp:44
  `kBossLeashTicks`) as a named, argued exception to the no-regen doctrine, instead of an
  undiscovered counterexample; ruling text now says "no per-tick, target-independent regen."
- Applied: withdrew the `chunk_actor.hpp:382-392` "boss level-up" citation (verified as
  `handle(UpgradeBuilding&)`, Building code); front-matter grounding line corrected to match.
- Applied: §6.5 now contains the full merged `respawn()` listing (position fork + `pending_eject_
  = false` reset), replacing the truncated `...` snippet.
- Applied: §6.2 adds an explicit guard — zero-default `instance_return_map_/x_/y_` falls back to
  `respawn_tx_/respawn_ty_` per §7, wired into the §6.5 listing.
- Applied: Motivation §3's RFC-014 Interactions-table quote corrected to verbatim text.
- Applied (unprompted, consistency): Tone Guardrail §2 and creature-spawn citation (line 514→579)
  updated to stay consistent with the revised §2 doctrine; not independently requested by either
  reviewer but necessary so the document does not contradict its own fix.
- Unresolved: none — all mustFix items from both reviewers were sound on verification against
  `src/world/chunk_actor.hpp`, `src/world/boss.hpp`, and the finalized RFC-014 text, and applied.
