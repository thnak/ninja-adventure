# RFC-023: Character & NPC Roster System

> Status: **Accepted (revised after review)**
> Design canon: [GAME.md §1](../GAME.md), [§5](../GAME.md), [§6](../GAME.md), [§12](../GAME.md),
> [§13](../GAME.md) · [ARCHITECTURE.md §4](../ARCHITECTURE.md) · [ROADMAP.md](../ROADMAP.md)
> As-built source grounding: `src/world/tiles.hpp` (`Faction`, `Stance`), `src/world/village.hpp`
> (`StructureKind`, `plan_of`, `VillageBuilder`), `src/render/raylib_bridge.cpp` (`tint_of`, the
> `Color tint` parameter already threaded through every draw call), `assets/_src/ninja/Actor/
> Character*/`, `assets/CREDITS.md`
> Sibling RFC this run: RFC-022 (Map System) — this RFC's village NPC roster is sized from RFC-022's
> as-built structure count, not from GAME.md's illustrative population column
> Depends on: RFC-007 (accepted — RL observation/action space; owns guard behavior, this RFC only
> assigns guard NPCs a skin and a roster slot), RFC-008 (accepted — general data-document contract,
> reused by citation for NPC role/skin tables), RFC-021 (accepted — road graph and village tier
> gates that the wandering-merchant and quest-giver roles key off)
> Depended on by (proposed): RFC-020 (accepted, this project's sibling batch — §"village
> quest-giver assignment" and part of "escort-AI ownership" were left as open questions with no
> owning RFC; this RFC is that owner, see Interactions), RFC-011 (proposed — any "talk to NPC"
> prompt UI consumes the role/portrait data this RFC defines), RFC-013 (proposed — NPC
> injury/death, if ever added, extends the entity this RFC defines)

---

## Summary

This RFC has two halves that share one root cause: **the asset pack drew exactly one character with
a full open-world locomotion set, and ninety-one more with only a combat-pose set.**

1. **The player character.** Green Ninja (`Actor/CharacterAnimated/NinjaGreen/Separate/`) is the
   **sole player-controllable character**, because it is the only sprite folder in the entire pack
   with Climb, Swim, Pickup, Push, and Roll poses alongside the eight combat poses every other
   skin has. This RFC records that as a hard constraint, not a placeholder, and specifies the one
   customization the constraint still leaves room for: a **cosmetic-only palette swap**, applied at
   render time to the one rig, requiring zero new animation frames.

2. **The NPC framework.** No `Npc` type exists in shipped code today — only `Faction::kVillager`,
   declared and wired into the stance matrix, and never instantiated. This RFC specifies it fresh:
   an `Npc` extension of the existing `Creature` doctrine (GAME.md §5's "one struct for everything
   alive that isn't the player"), a role taxonomy grounded in GAME.md's own mentions (guard,
   merchant, quest-giver, farmer, child, wanderer), a rule for which of the other 91 character skins
   each role draws from, a hand-authored state machine for the five civilian roles, and an explicit
   behavior-ownership line at RFC-007 for the guard roles — this RFC does not re-specify guard AI.
   Village NPC roster size is **derived from the village's actual built structure count**, not
   GAME.md's illustrative population numbers, so a roster never claims more residents than the
   village has houses for.

---

## Motivation

Three gaps, one shared cause:

1. **"Which character can the player be" was never written down as a rule, only as a fact of the
   asset pack.** `Actor/CharacterAnimated/NinjaGreen/Separate/` has twelve pose sheets — Attack,
   Climb, Dead, Hit, Idle, Item, Jump, Pickup, Push, Roll, Swim, Walk — plus `SpriteSheet.png` and
   preview GIFs. Every other character folder under `Actor/Character/*/SeparateAnim/` (91 verified
   sibling directories, counting `find assets/_src/ninja/Actor/Character/ -mindepth 1 -maxdepth 1
   -type d | grep -v NinjaGreen` — close to but not identical with GAME.md §13's illustrative "95"
   character-sprite figure, which likely rolls in a couple of non-directory or non-`SeparateAnim`
   entries this audit did not count; treat 91 as the normative directory count and 95 as GAME.md's
   rough total, not a contradiction worth resolving here) — including all eleven other ninja colors,
   `Villager`–`Villager6`, `OldMan`, `Woman`, `Boy`, `Noble`, `Monk`, `Samurai`, and so on — has
   exactly eight: Attack, Dead, Idle, Item, Jump, Special1,
   Special2, Walk. No Climb, no Swim, no Pickup, no Push, no Roll, no Hit. A player who can walk
   into shallow water, climb a ledge, push a boulder, or roll away from an attack — all things
   GAME.md's open world assumes — cannot be played as anything but Green Ninja without either a
   silent animation gap (a swimming NPC-skin player freezing mid-lake) or new art nobody has
   budgeted (GAME.md §13: **0 đồng** for character art through P3). This RFC makes the asset fact
   into a recorded rule so no future PR quietly offers a skin selector that breaks traversal.

2. **GAME.md promises a lived-in world with no NPC to live in it.** §1 opens on "hàng xóm là mấy cái
   làng nhỏ" (neighbors are a few small villages); §5 gives `Faction::kVillager` a friendly stance
   toward the player and specifically toward nothing else being hostile to it by default; §6 gives
   villages a population column (2–5 up to 100+) and says tier 2+ brings "chợ, nhiệm vụ" (market,
   quests); §10 explicitly carves out "Nông dân, thương nhân, trẻ con: cây hành vi viết tay, không
   RL" (farmers, merchants, children: hand-authored behavior trees, not RL) as a boundary against the
   very RL scope creep §10 spends a page warning about. None of this has a struct, a role list, or a
   sprite assignment today — `grep -rn "class Npc\|struct Npc"` returns nothing.

3. **RFC-020 shipped with two explicit unowned dependencies pointing at exactly this gap.** Its Open
   Questions read: *"Village quest-giver assignment... no RFC currently owns village population/NPC
   rosters"* and *"No accepted or proposed RFC currently owns friendly-NPC pathing/AI... should a
   future RFC own 'friendly NPC behavior' broadly (traders, escort targets, village population)?"*
   This RFC answers the first in full and the second in part (the general civilian actor and state
   machine now exist; a `Follow`/escort-specific state is deliberately left for a future revision,
   §Open Questions).

---

## Guide-level Explanation

### For a player

You are always Green Ninja. The Character screen (`C`, GAME.md §12) has one new control: a row of
swatches. Pick one and your outfit recolors instantly — no cost, no unlock, no cooldown, reselect as
often as you like. Nothing about how you move, fight, or animate changes; it's paint, not a new
character.

Walk into any village and it has people in it: a shopkeeper standing at a stall who doesn't move,
someone tending a garden patch who wanders between a few plots, kids playing near a house who scatter
indoors as evening comes on, someone gathered near the village hall who will hand you a quest at tier
2+, and — the moment a raid warning fires (GAME.md §6's one-day scout notice) — everyone quietly
heads indoors before the raid ever arrives, exactly the way the warning was designed to let *you*
prepare too. Guards drilling in the tier-3+ dojo are the same RL-trained defenders GAME.md §10
describes; this RFC just puts a face and a name-tag on them, it doesn't change how they fight.

Walk the roads between villages (RFC-021 §4.4) and you'll occasionally pass a wandering merchant
making the same trip you are. They're not following a scripted path to you — they're on the same road
graph you can see on the Map (`M`), just walking it themselves.

### For a designer

The role taxonomy (§4) is where you go to answer "what NPC do I need here" — it's a closed list, not
an ad hoc one, and every role names its behavior owner and its skin pool. Adding a new *skin* to an
existing role (a new merchant look) is a one-line addition to a data table (§9); adding a new *role*
is a spec change here, because a role carries behavior-ownership implications (does it need RFC-007's
RL pipeline, or does it get a state machine this RFC already defines).

### For an engineer implementing fresh

You need: (1) the `Npc` data shape (§ Reference-level Design 3) — an extension of `Creature`, not a
parallel entity type, per GAME.md §5's own stated doctrine; (2) the role→skin-pool table and its
deterministic-pick rule (§5); (3) the civilian FSM (§7) — four states, no timers that create player
obligation; (4) the roster-sizing formula (§8), which reads the *actual* `Structure` list a village
build produced (not the tier's aspirational house target) so the roster can never claim residents the
village has no houses for.

---

## Reference-level Design

### 1. The player character model

#### 1.1 Green Ninja as the sole player rig

| Fact | Source |
|---|---|
| `NinjaGreen` is the only folder with `Actor/CharacterAnimated/.../Separate/{Attack,Climb,Dead,Hit,Idle,Item,Jump,Pickup,Push,Roll,Swim,Walk}.png` | asset audit 2026-07-24 |
| Every other `Actor/Character/*/SeparateAnim/` folder (91 verified sibling directories, excluding `NinjaGreen`) has exactly `{Attack,Dead,Idle,Item,Jump,Special1,Special2,Walk}.png` — 8 states, no Climb/Swim/Pickup/Push/Roll/Hit | asset audit |
| `NinjaFire`, `NinjaLeaf`, `NinjaThunder`, `NinjaWater` have only 7 of those 8 — no `Idle.png` | asset audit |
| `Actor/Character/Child/` has no `SeparateAnim/` at all — only `Faceset.png` and an unsplit `SpriteSheet.png` | asset audit |
| `CREDITS.md` pins `Actor/Character/NinjaGreen/Faceset.png` as the Character screen portrait | `CREDITS.md`, "which sheet covers what" |

**Rule (normative):** the player-controlled rig is fixed to `NinjaGreen`'s `CharacterAnimated` set.
No other skin — ninja-colored or otherwise — is ever offered as a player-selectable base character in
v1. This is not an aesthetic choice this RFC is making; it is recording which single asset satisfies
the traversal contract GAME.md's open world requires (swimming across the Wetland ring, climbing,
pushing, rolling) and refusing to paper over the other 91 folders' gap with a silent animation
freeze. If a second fully-animated player rig is ever produced (new art, out of GAME.md §13's current
$0 budget), it is a new value in `base_rig` (§1.2) added by a future revision of this RFC, not a
reinterpretation of an existing skin.

#### 1.2 Cosmetic palette swap — data contract

Recoloring a sprite sheet is a **render-time or asset-variant operation** — it touches pixels, not
poses, and the pack already proves the technique is cheap: `tint_of(Status)` in
`raylib_bridge.cpp` already multiplies a `Color` through every `DrawTexturePro` call (status dye,
night overlay, the boss hurt-flash) with zero new frames. This RFC reuses exactly that mechanism for
player cosmetics.

```
struct PlayerAppearance {
    uint8_t base_rig;    // always 0 ("NinjaGreen") in v1; reserved for a future second rig
    uint8_t palette_id;  // 0..7 (tunable) — index into kPalette below
};
```

| `palette_id` | Name | Tint (illustrative, tunable) |
|---|---|---|
| 0 | Default (no tint) | `(255,255,255)` — the shipped green |
| 1 | Crimson | `(255,140,140)` |
| 2 | Azure | `(140,170,255)` |
| 3 | Amber | `(255,210,120)` |
| 4 | Slate | `(170,170,180)` |
| 5 | Violet | `(200,150,255)` |
| 6 | Snow | `(235,235,245)` |
| 7 | Ember | `(255,120,90)` |

**Rules:**

- Selectable and reselectable at will from the Character screen (`C`), free, no cooldown, no unlock
  quest, no crafting dependency — a cosmetic choice made once at the moment the player wants it, the
  same "one-off decision, not a schedule" test RFC-021 §4.3 applies to waypoint fees.
- `palette_id` replicates once per change (push-on-change), never per tick — the same near-static
  budget pattern RFC-021 §5.4 uses for map markers.
- The tint applies uniformly to every draw call for that player's body **and** their Character-screen
  Faceset portrait, so a red-dyed player also sees a red portrait — one field drives both, no
  separate art.
- **This RFC does not decide multiplicative-tint vs. a true per-color palette remap.** A flat
  multiply (the shipped mechanism) recolors skin tone and headband along with the outfit, which may
  or may not read as intended on this particular sprite; a true remap needs either a shader keyed off
  index colors or a small number of pre-baked recolored atlas variants (still zero new *animation*
  work — same poses, different pixels, baked once offline). Which technique ships is a rendering
  decision for `RENDER_SPEC.md` (§Non-goals); this RFC fixes only the data contract (`palette_id`,
  8 tunable values, free/instant selection) that either technique must satisfy.

### 2. NPC skin taxonomy

Every `Actor/Character/*/SeparateAnim/` folder **other than** `NinjaGreen`'s `CharacterAnimated` set
is eligible NPC-skin material — not just the eleven other ninja colors named in the motivation, but
the full 91-folder roster (`Villager`–`Villager6`, `OldMan`/`OldMan2`/`OldMan3`, `OldWoman`, `Woman`,
`Boy`, `Noble`, `Monk`/`Monk2`, `Sultan`/`Sultan2`, `Hunter`, `Samurai` family, and so on — every
non-`NinjaGreen` entry under `Actor/Character/`). The asset gap that disqualifies these skins for
player use (§1.1) is a **non-issue for NPCs**, because the civilian/guard behavior model this RFC
specifies (§6–§7) never calls Climb, Swim, Pickup, Push, or Roll — an NPC's whole locomotion surface
is Idle/Walk/Attack(guards only)/Item(work poses)/Special1/Special2, which the 8-state set covers
completely.

Two exclusions from the v1 eligible pool, both flagged rather than worked around silently:

- **The four elemental ninja variants** (`NinjaFire`, `NinjaLeaf`, `NinjaThunder`, `NinjaWater`) have
  no `Idle.png`. Since every civilian/guard role spends the plurality of its time in `Idle`
  (§7), these four are **excluded from the overworld village NPC pool in v1** rather than given a
  Walk-frame-0 fallback that hasn't been visually verified. They remain available for a future,
  out-of-scope use (a rest-realm "elemental spirit" NPC, matching GAME.md §1's "vùng đất linh hồn" —
  realm-interior NPCs are explicitly non-goal here, §Non-goals).
- **`Child`** has no `SeparateAnim/` split at all, only an unsplit `SpriteSheet.png`. The `kChild`
  role (§4) either runs the same splitting tool used for the other 91 folders against `Child`'s
  sheet before it ships, or uses `Boy` as its v1 stand-in. Flagged in Open Questions rather than
  silently defaulted, since it's an asset-pipeline task this RFC doesn't own the tooling for.

Every eligible skin carries exactly one `Faceset.png`, used identically as the in-world sprite's
dialogue portrait — the same rule RFC-020 already states for quest-givers ("one per character, not
per individual — a merchant giving three different quests over a save still shows the same
portrait"). This RFC generalizes that rule to every NPC role, not just quest-givers: **a skin id, not
an NPC instance id, determines the portrait.** Two farmers sharing a skin look and dialogue-portray
identically; this is expected, not a shortcut taken under pressure.

### 3. NPC entity representation

GAME.md §5's stated doctrine for the fauna/monster system is explicit: **one `Creature` struct for
everything alive that isn't the player**, specifically to avoid duplicating every `ChunkActor` loop
for what the design calls "đúng hai byte" of difference between entity kinds. `Faction::kVillager`
already exists in that struct's faction enum (`tiles.hpp:145`) and already participates correctly in
`stance_between` (friendly to `Player`) — it has simply never been instantiated by anything.

This RFC follows that doctrine rather than introducing a parallel `Npc` entity type — but doing so
requires one concrete engine change this RFC now specifies explicitly, because neither of the two
things a naive reading of "Creature with faction = kVillager" could mean actually exists today:
`Creature` has no stored `faction` field at all (faction is derived per-tick, read-only, via
`stats_of(c.kind).faction`, `tiles.hpp`/`chunk_actor.hpp`), and `CreatureKind`'s enum is a closed,
10-entry, combat-stat-keyed list (`kSlime..kBoss`) with no NPC-capable slot and an explicit
random-roll-pool ordering sensitivity (the `kBoss`-last comment) that makes silently appending to it
the wrong move.

**Normative requirement:** `Creature` gains a new stored field, `faction_override : optional<Faction>`,
checked first at every faction read site (`chunk_actor.hpp:827,925,1351` today) before falling back to
`stats_of(kind).faction` for creatures that don't set it. This is the mechanism by which an `Npc`
"has `faction = Faction::kVillager`" — it is a per-instance override, not a derived value, and every
existing faction read site must be updated to check it. This RFC picks this mechanism over adding new
`CreatureKind` entries specifically to avoid manufacturing meaningless `CreatureStats` combat rows
(attack/defense/HP numbers with no combat use) for the six non-combat civilian roles (§4), and to
leave the closed, order-sensitive `CreatureKind` enum untouched.

An `Npc` is a `Creature` with `faction_override = Faction::kVillager` and these additional fields
(illustrative shape; exact layout is an implementation detail, the fields are the contract):

```
role          : NpcRole    // §4 — closed enum
skin_id       : uint16_t   // index into the eligible skin table, §2
home_struct   : uint32_t   // index of the Structure (house/shopfront/hall) this NPC is anchored to
wander_radius : uint8_t    // tiles; role-dependent, §7 table; 0 for stationary roles
state         : NpcState   // Idle | MoveToWaypoint | WorkAction | Sheltering — §7
```

`home_x`/`home_y`/`territory_radius` — already fields on `Creature` for Wild fauna's home-anchored
wandering (GAME.md §5's engineering consequence 1) — are reused as-is for `wander_radius`'s anchor
point; this RFC introduces no second home-tracking mechanism.

`disposition` (the anger/grudge state GAME.md §5 defines for Wild fauna) **must be explicitly set to
`Disposition::kNeutral` at `Npc` construction, not left at its struct default.** `Creature::disposition`
defaults to `Disposition::kHostile` (`tiles.hpp`), and the natural construction path any `Creature` —
`Npc` included — goes through, `make_creature()`, explicitly writes `c.disposition = st.disposition;`
from the spawning `CreatureKind`'s stats row. Since `Npc` roles have no combat-stat row of their own
(they carry `faction_override`, not a dedicated `CreatureKind`), `Npc` construction must not rely on
`stats_of()`'s value for this field — it must set `disposition = Disposition::kNeutral` directly,
as a normative step of `Npc` construction, not an assumed default. `stance_between(Player, Villager)`
being unconditionally friendly means `disposition` never subsequently matters for player-facing
hostility, but it must start correct rather than start wrong and rely on being unread.

### 4. Role taxonomy

Grounded directly in GAME.md's own NPC mentions (§1 "hàng xóm", §5 `Villager` examples, §6's
population/garrison table, §10's "Nông dân, thương nhân, trẻ con: cây hành vi viết tay").

| Role | GAME.md grounding | Behavior owner | Combat-capable |
|---|---|---|---|
| `kGuardMilitia` | §6 tier-1 "1–2 dân binh" | RFC-007 (uses `guard.soldier` archetype at a reduced stat tier — no new archetype, §6 below) | yes (RFC-007) |
| `kGuardSpear` | §6 tier-2+ "vệ binh" | RFC-007 `guard.spear` | yes (RFC-007) |
| `kGuardBow` | §6 tier-2+ "vệ binh" | RFC-007 `guard.bow` | yes (RFC-007) |
| `kGuardSoldier` | §6 tier-3+ "vệ binh" | RFC-007 `guard.soldier` | yes (RFC-007) |
| `kGuardCaptain` | §6 tier-3+ garrison lead | RFC-007 `guard.captain` | yes (RFC-007) |
| `kMerchantShop` | §5 "thương nhân"; §6 "chợ" (tier 2+) | this RFC, §7 (stationary) | no |
| `kMerchantWander` | §1, §5 "thương nhân lang thang" | this RFC, §7 (road-graph roam) | no |
| `kQuestGiver` | §6 "nhiệm vụ" (tier 2+); RFC-020 §"Sources" | this RFC, §7 (stationary) | no |
| `kFarmer` | §10 "Nông dân" | this RFC, §7 (local wander + work pose) | no |
| `kChild` | §10 "trẻ con" | this RFC, §7 (local wander, nightfall shelter) | no |
| `kWanderer` | §1 "hàng xóm" (ambient population, unnamed individually) | this RFC, §7 (local wander) | no |

**Explicitly excluded from this taxonomy:** the dojo boss (`src/world/boss.hpp`'s "Dojo Master") is
`Faction::kMonster`, not `Faction::kVillager` — it is an RL combat entity owned by RFC-005/RFC-007,
not a village NPC, and this RFC does not touch it. Wild fauna (`Faction::kWild`) is fully owned by
GAME.md §5's existing `Creature` behavior and untouched here. Mount/animal creatures
(`Actor/Animal/*`) are out of scope per this run's assignment (§Non-goals).

### 5. Sprite-pack assignment

Each role names an **eligible skin pool**, not a single skin — flavor variety within a role, one
consistent behavior across the pool.

| Role | Skin pool (illustrative, tunable — expand freely, each addition is a one-line table edit) |
|---|---|
| `kGuard*` | `{Knight, KnightGold, Samurai, SamuraiBlue, SamuraiRed, GladiatorBlue, RedGladiator, FighterRed, FighterWhite}` |
| `kMerchantShop`, `kMerchantWander` | `{Villager, Villager2, Villager3, Villager4, Villager5, Villager6, Noble, Sultan, Sultan2, Woman}` |
| `kQuestGiver` | `{Villager, Villager2, ..., Villager6, OldMan, OldMan2, OldMan3, OldWoman, Noble}` |
| `kFarmer` | `{Villager, Villager2, ..., Villager6, Woman, Boy, OldMan, OldWoman}` |
| `kChild` | `{Boy, Child (§2 caveat), EggBoy, EggGirl}` |
| `kWanderer` | `{Hunter, Monk, Monk2, Eskimo, Shaman, Sultan, Villager, Villager2, ..., Villager6}` |

**Selection rule (deterministic, normative):** the skin assigned to a given NPC slot is
`kPool[role][ hash(world_seed, home_struct_or_road_segment_id) % len(kPool[role]) ]` — a pure
function of `(seed, structure/segment id)`, following the exact placement doctrine RFC-021 §1
establishes for every other worldgen decision ("everything derived from seed must match byte-for-byte
across GCC/Linux and MSVC/Windows"). No skin assignment is randomized at runtime or re-rolled on
NPC respawn; it is fixed once, at the same worldgen pass that builds the village (§8).

`guard.captain`'s skin is drawn from the same pool as the other guard roles rather than a bespoke
"captain look," to avoid a fifth skin table for a role that already shares `guard.soldier`'s stat
family (RFC-007 §4's archetype table lists `guard.captain` as its own policy but does not require a
visually distinct base skin — equipment/insignia, if any, is a rendering detail out of this RFC's
scope, same as boss telegraph FX is RFC-006's).

### 6. Behavior model ownership boundary

This is the load-bearing line the whole RFC exists to draw precisely:

| | Owner | What this RFC does |
|---|---|---|
| **Guard / village-defense roles** (`kGuardMilitia`, `kGuardSpear`, `kGuardBow`, `kGuardSoldier`, `kGuardCaptain`) | **RFC-007** (accepted) — the `guard.spear`/`guard.bow`/`guard.soldier`/`guard.captain` archetypes are already in RFC-007 §4's 13-policy roster (ids 10–13), one shared policy per archetype, never per individual, exactly as GAME.md §10 mandates | Assigns each guard NPC a `role` (→ archetype id, 1:1), a `skin_id` (§5), and a roster slot count (§8). **This RFC does not specify or alter any guard AI, reward shaping, observation vector, or training loop** — those are entirely RFC-007's. |
| **Militia (`kGuardMilitia`, tier-1 "dân binh")** | RFC-007's `guard.soldier` archetype, at a lower individual stat tier | This RFC does **not** introduce a fifth guard archetype for tier-1 militia — RFC-007 caps the roster at 15 policies (13 already assigned) and a militia is mechanically the same fight pattern as a line soldier, just fewer and weaker. `guard.soldier`'s existing checkpoint drives them. |
| **Civilian roles** (`kMerchantShop`, `kMerchantWander`, `kQuestGiver`, `kFarmer`, `kChild`, `kWanderer`) | **This RFC**, §7 | Full state-machine specification, hand-authored, zero RL involvement — matching GAME.md §10's explicit "không RL" line for these three role families. |

No civilian role is ever combat-capable in v1: civilians carry no HP/damage/ability data (RFC-001,
RFC-009's territory), so there is nothing for RFC-007's observation space to represent even if a
future revision wanted to. This is stated as a design decision, not an oversight — see §Non-goals and
§7's Sheltering-before-raid argument for why it doesn't create a hidden vulnerability the tone
guardrail would object to.

### 7. Civilian behavior state machine

One FSM, four states, parameterized per role. No state transition reads a wall-clock or
elapsed-real-time field.

```
                dwell timer expires (WorkAction) / arrived (MoveToWaypoint)
        ┌──────────────────────────────────────────────────────────┐
        │                                                          │
        ▼                                                          │
   ┌─────────┐   pick next waypoint from role's pool    ┌─────────────────┐
   │  Idle   │ ─────────────────────────────────────────▶│ MoveToWaypoint  │
   └─────────┘                                            └────────┬────────┘
        ▲                                                          │ arrived at waypoint
        │                                    role has a work pose  ▼
        │                              ┌─────────────────────────────────┐
        └──────────────────────────────│         WorkAction             │
                                        │ (plays Item/Special1/Special2  │
                                        │  for a fixed dwell time)       │
                                        └─────────────────────────────────┘

   From ANY of the above except `kMerchantWander` (see note below), on {raid-warning event fires
   for this village (GAME.md §6's scout notice, once the mechanism in the note below actually
   ships) OR a hostile-Monster-faction entity enters the NPC's beacon-proximity radius OR
   (kChild only) world clock crosses dusk}:
                                        ┌─────────────────────────────────┐
                                        │           Sheltering            │
                                        │ (paths to home_struct's interior│
                                        │  tile; stationary; HP set        │
                                        │  unreachable per the note below) │
                                        └─────────────────────────────────┘
                                                    │
                        threat clears (same ~20s no-contact cooldown GAME.md §5 already
                        uses for Wild fauna disposition) OR raid resolves OR (kChild) dawn
                                                    ▼
                                                  Idle
```

**`kMerchantWander` has no `home_struct` to shelter to.** §5's own selection-rule formula
(`home_struct_or_road_segment_id`) and this role's own table row below (waypoint pool = "the
road-segment sequence between two villages") confirm `kMerchantWander` is keyed by a road-segment
id, not a `Structure` index — the universal Sheltering transition above therefore does not apply to
it as written. This RFC does not invent an unfounded fallback (e.g., "flee to the nearest village")
without a source-verified anchor to path to. Instead: raids target village `Structure`s (GAME.md
§6), never the open road between them, so a merchant mid-transit is never a raid target in the first
place and is exempted from the raid-warning trigger entirely. It still reacts to the
hostile-Monster-proximity trigger (the one threat that can plausibly occur mid-road) by pathing to
whichever end of its current road segment is nearer, then resuming once the threat clears — flagged
as Open Question 8, since this fallback is this RFC's judgment call, not a verified existing
mechanism.

| Role | `wander_radius` (tunable) | Waypoint pool | Work pose | Dwell time (tunable) |
|---|---|---|---|---|
| `kMerchantShop` | 0 | none — fixed at `home_struct`'s door tile (the village's tier-≥2 `kMarketYard` parcel, §8.1) | `Idle` only, faces the street | n/a |
| `kMerchantWander` | n/a — follows RFC-021's road graph, not a radius | the road-segment sequence between two villages | none | n/a — continuous walk, reverses direction at each end |
| `kQuestGiver` | 0 | none — fixed at `home_struct`'s door tile: one of the village's own house `Structure`s (the same house-list slot §8.1 allocates the role from) below tier 3; the `kHouseRed` hall itself only from tier 3 on, since the hall doesn't exist before then (§8.1) | `Idle` only | n/a |
| `kFarmer` | 6 tiles | 2–4 points around `home_struct` (crop-plot tiles if the village has any nearby, else generic yard points) | `Item` (tending) | 4–8s per stop |
| `kChild` | 4 tiles | 2–3 points around `home_struct` | `Special1`/`Special2` alternating (playing) | 3–6s per stop |
| `kWanderer` | 10 tiles | any walkable tile within radius of `home_struct` | none | n/a |

**Why civilians never use the flow field.** GAME.md §5's engineering consequence 1 restricts flow-
field pathing to hostile creatures specifically because home-anchored local wandering is "rẻ hơn
nhiều" (much cheaper) and is what already lets ~620 Wild-faction creatures live on the map "gần như
không tốn gì" (at nearly no cost). Civilian NPCs apply the identical restriction for the identical
reason — a `kWanderer`'s destination pick is a random walkable tile within `wander_radius` of its own
`home_x/home_y`, never a global pathfind. Guards are the one NPC family that may need real pathing
(formation movement during a raid) — and that is RFC-007's concern, not specified here.

**Why Sheltering makes civilian "death" moot without banning it outright — and what still has to be
built for that to be true.** `Faction::kVillager` is already hostile-targeted by `Faction::kMonster`
in the shipped stance matrix (`stance_between(kMonster, b)` is hostile for every `b` except another
monster) — this RFC does not special-case that away, because doing so would mean civilians and the
player obey different physics for no stated reason. Instead, GAME.md §6 commits to a full game-day
scout warning before every raid lands, and this RFC's Sheltering transition is designed to fire on
that warning event — but two things this section previously asserted were already true are not, and
both are now flagged as explicit prerequisites rather than load-bearing assumptions:

- **The warning event is not shipped yet.** RFC-021 §3.4 (accepted) states the current placeholder
  (`map_director.hpp`: 10%/fort/night, no scout, no announcement) is a P1 stand-in, and the real
  scout/announcement system RFC-021 describes is "P3 scope in ROADMAP.md, unassigned to a numbered
  RFC." Until that system ships, this RFC's Sheltering-on-raid-warning transition has no event to
  subscribe to and cannot fire. This RFC's civilian FSM is specified against the warning event that is
  *designed* to exist, not one that exists today; it is blocked on that P3 work landing, and this
  dependency is now explicit rather than assumed away.
- **No targetability-suppression hook exists on `Creature` today.** `strike()` (`chunk_actor.hpp`,
  the sole HP-damage entry point) gates only on `c.hp <= 0 || damage <= 0` — it does not check
  faction, an `observable` flag, or anything resembling RFC-004's `obs_class` (which belongs to
  `EntityDef`/hazard-prop classification for RFC-007 Block E, a different chassis from `Creature`, and
  was misattributed here in an earlier draft of this section). Making a Sheltering NPC actually
  untargetable therefore requires a new, `Creature`-level check in `strike()` (or an equivalent guard
  at the same choke point) that rejects damage while `state == NpcState::kSheltering`. This RFC
  specifies that requirement normatively — `strike()` must gate on Sheltering state — rather than
  citing a mechanism that doesn't exist.

With both of those built, the vulnerability is structurally real but practically unreachable under the
warned-raid design GAME.md commits to; until then, this RFC does not claim civilians are safe, only
that they are designed to be once the two prerequisites above ship. If a future raid type is ever added
*without* warning, this guarantee breaks and RFC-013 (proposed, Vitals/Death/Recovery) would need to
decide what happens next — flagged in Open Questions, not resolved here.

**Farmer NPCs do not participate in the crop economy.** A `kFarmer`'s `WorkAction` pose is decoration
— it does not plant, grow, or harvest anything, and does not compete with or substitute for the
player's own farming loop (GAME.md §1's central activity). This is stated explicitly to prevent scope
creep into an NPC-run economy this RFC was never asked to design.

### 8. Village NPC roster sizing

**Rule (normative): roster size is read from the `Structure` list a village build actually produced,
never from GAME.md §6's population column.** `village.hpp`'s own comment records that `place()` can
silently fail ("a house that cannot fit simply is not built, which is the right failure") — the
`plan_of(tier).houses` value (4/6/8/10/12 for the code's tier 1–4 and its unlabeled higher default,
§8.1) is a *target*, not a guarantee. This RFC's roster formula reads the count of house-kind
`Structure` entries a specific village actually has (`kHouseOrange`/`Cream`/`Amber`/`Red`/`Blue`/
`Tan`/`Wood`, `village.hpp`'s enum) after worldgen finishes, so a roster can never claim more
residents than the village has houses for. This is the explicit tie to RFC-022: if RFC-022 ever
changes how many structures a village of a given size can fit, this RFC's roster resizes with it
automatically, because it was never an independent number.

#### 8.1 Civilian roster

For a village with `H` successfully-built houses (from the actual `Structure` list, §8):

| Slot | Count | Rule |
|---|---|---|
| `kMerchantShop` | 1 if village tier ≥ 2, else 0 | keyed to the actual tier-gated market feature — `village.hpp`'s `PrefabId::kMarketYard`, placed via `place_parcel(...kMarketYard)` only when `tier == 2` or `tier ≥ 3` — **not** `kHouseBlue`, which an earlier draft of this section cited: `house_for()` picks `kHouseBlue` as 1-of-6 equally-weighted house colors with no tier check at all, so it is not a reliable market signal. Whether the `kMarketYard` parcel is itself an indexable `Structure` entry (for `home_struct` to point at) needs a source check — flagged in Open Questions. |
| `kQuestGiver` | 1 if village tier ≥ 2, else 0 | matches GAME.md §6's tier-2 "nhiệm vụ" gate and RFC-021 §4.3's tier-2 waypoint gate — one consistent "this village has real infrastructure" threshold reused a third time. Allocated from the village's house `Structure` list (§7's `kQuestGiver` row), not from a hall, below tier 3. |
| `kFarmer` / `kChild` / `kWanderer` | remaining `H − (shop? 1 : 0) − (giver? 1 : 0)` houses, split **40% / 30% / 30%** (tunable), rounded, deterministic per house index | one civilian per remaining house — "a household lives here," not a headcount matching GAME.md's flavor-text population |

**Worked example (tier-2 Village, illustrative):** `plan_of(2).houses = 6` requested; assume all 6
place. 1 becomes `kMerchantShop` (village is tier ≥ 2, so its `kMarketYard` exists), 1 becomes
`kQuestGiver` (tier ≥ 2), leaving 4 split ≈ 2 `kFarmer` / 1 `kChild` / 1 `kWanderer`. Total civilian
NPCs: 6.

**Recomputation, not a one-shot freeze.** Because `kMerchantShop` and `kQuestGiver` are gated on
*tier*, and RFC-021 §4.3 (accepted) establishes that village tier is evaluated continuously at
runtime rather than fixed at worldgen ("village reaches tier ≥ 2, checked continuously"), this
roster formula is **re-evaluated whenever the village's tier changes or its `Structure` list gains a
house post-worldgen** — both are worldgen/gameplay-caused events (trading, defense contributions,
road connections, or a nearby fort closing, per GAME.md §6's tier-up triggers; none of them require a
new house to exist), never a clock. A village ticking from tier 1 to tier 2 gains its `kMerchantShop`
and `kQuestGiver` slots at that moment, not at the village's original worldgen pass. See §Multiplayer
for how this interacts with replication, and the corrected Tone Guardrail #2 below for why this is
still not a countdown.

#### 8.2 Guard roster

Read directly from GAME.md §6's table — this RFC does not introduce new numbers, only assigns the
already-fixed per-tier range to concrete archetype slots:

| Tier | Guard count (GAME.md §6) | Archetype split (tunable) |
|---|---|---|
| 0 (Camp) | 0 | — |
| 1 (Hamlet) | 1–2 "dân binh" | 100% `kGuardMilitia` |
| 2 (Village) | 3–6 "vệ binh" | 50% `kGuardSpear` / 50% `kGuardBow` |
| 3 (Town) | 8–15 "vệ binh" + wall | 45% `kGuardSpear` / 45% `kGuardBow` / 10% `kGuardCaptain` |
| 4 (Citadel) | 20+ "quân đội thường trực" | 40% `kGuardSpear` / 40% `kGuardBow` / 10% `kGuardSoldier` / 10% `kGuardCaptain` |

**Worked example (tier-2 Village, continued):** 3–6 guards, say 4 → 2 `kGuardSpear`, 2 `kGuardBow`.
Combined with §8.1's 6 civilians: **≈10 NPC actors** for a representative tier-2 village.

Like §8.1's civilian slots, this table is keyed on the village's *current* tier, not its tier at
worldgen — a village that ticks from tier 2 to tier 3 live gains its 5th–11th guard slots and its
`kGuardCaptain` slot at that moment (§8.1's recomputation note applies identically here), not never.
A roster formula that stayed frozen at worldgen-time tier would leave a live Citadel fielding a Camp's
garrison, which is not this RFC's intent.

**Map-wide cost, illustrative:** across ~50 villages (RFC-021 §2.2) at a rough average of 5–8 NPC
actors each (most villages sit at tier 0–1 per the ring-biased tier distribution GAME.md §6
describes; only inner-ring villages reach tier 3–4), the total NPC actor count is on the order of a
few hundred — the same order of magnitude as the ~620 Wild-faction creatures GAME.md §5 already
reports living on the map "gần như không tốn gì." Since civilian NPCs use the identical
home-anchored local-wander cost model as that already-proven fauna population (§7), this RFC does not
introduce a new performance risk class, only more instances of an already-validated one.

### 9. Determinism and data format

Role assignment, skin pool selection, and roster split percentages are pure functions of
`(world_seed, structure_id / road_segment_id)`, computed once at world generation and never
re-rolled — the same cross-platform bit-exactness requirement RFC-021 §1 states for every other
placement decision, extended here without modification.

Role/skin-pool tables (§4, §5) are authored data, not hardcoded C++ tables, following RFC-008's
(accepted) general data-document contract by citation — strict JSON, `ids.lock.json`, canonical
form and dual hashes, schema versioning — the same reuse pattern RFC-020 already established for
quest/giver documents. This RFC defines only the NPC-specific schema (`data/npc/roles.json`,
`data/npc/skin_pools.json`); it does not redefine RFC-008's general format.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001, RFC-002, RFC-003, RFC-004, RFC-009, RFC-010** (accepted combat set) | Not load-bearing here. No NPC in this RFC carries ability, status, damage, or physics data — civilians are explicitly non-combatant (§6), and guard combat stats/kit are RFC-001/RFC-009's, assigned to the archetypes RFC-007 already owns. |
| **RFC-005** (accepted, Boss Ability Authoring) | Owns the Dojo Master boss entirely — explicitly excluded from this RFC's taxonomy (§4) as `Faction::kMonster`, not a village NPC. |
| **RFC-006** (accepted, Visual FX & Telegraph Standards) | Owns any visual telegraph a guard NPC's attack needs; this RFC assigns guard skins only, never combat FX. |
| **RFC-007** (accepted, RL Observation & Action Space) | Owns all guard/village-defense behavior, training, and the archetype roster (§4, §5, §6 above cite it exhaustively). This RFC is a consumer: it assigns guard NPCs a `role` (1:1 with an RFC-007 archetype id), a skin, and a roster count, and specifies none of RFC-007's tensors, rewards, or checkpoint lifecycle. |
| **RFC-008** (accepted, Data-driven Skill Definition) | This RFC's role/skin-pool tables reuse RFC-008's general JSON contract by citation (§9), defining only the NPC-specific schema. |
| **RFC-019** (accepted, Progression & Skills) | Untouched. XP attribution for defeating a raid follows RFC-019's existing cause-of-death rules regardless of whether an NPC guard or the player lands the kill; this RFC adds no new XP source. |
| **RFC-020** (accepted, Mission & Quest System) | **This RFC is the owner RFC-020's Open Questions asked for.** RFC-020 §"Village quest-giver assignment": **mostly** resolved — §8.1 assigns exactly one `kQuestGiver` per tier-≥2 village, deterministically (the skin/individual is picked once and never re-rolled, §5), and §8.1's recomputation note now answers the survival-through-growth half of the question directly: the giver *appears* the moment a village crosses tier 2 (not only at worldgen), stays the same individual afterward, and gains no additional dialogue — RFC-020's own quest content system owns any dialogue growth if that's ever wanted, not this RFC. RFC-020 §"Escort-AI ownership gap": **partially** resolved — the general `Npc`/civilian-FSM substrate (§3, §7) now exists as a foundation, but a `Follow`-the-player state and an "arrival/survival" completion definition for `escort` objectives are **not** specified here (§Open Questions) and remain blocking for that quest type. Quest dialogue content and reward tables remain entirely RFC-020's; this RFC supplies only the role/portrait association a quest can reference (RFC-020's own §"NPC presentation" already assumed this). |
| **RFC-021** (accepted, World Structure, Map & Wayfinding) | This RFC's `kMerchantWander` role walks RFC-021's road graph (§4.4) directly, adding no new pathing data. `kQuestGiver`'s tier-≥2 gate reuses the same threshold RFC-021 §4.3 uses for waypoint eligibility — one gate, cited three times now (GAME.md §6, RFC-021, this RFC), never re-derived. |
| **RFC-022** (proposed, this batch's sibling, Map System) | This RFC's village roster size (§8) is read from RFC-022's as-built structure output, not from an independent number — if RFC-022 changes how many houses a village of a given footprint fits, this RFC's roster changes with it, by construction rather than by a second spec update. |
| **RFC-011** (proposed, Combat HUD) | If a "talk to this NPC" interaction prompt is ever specified, it is UI and belongs there; this RFC only guarantees the NPC, its position, and its portrait exist to be prompted about. |
| **RFC-013** (proposed, Vitals/Death/Recovery) | Would own NPC injury/death mechanics if the Sheltering-before-raid guarantee (§7) is ever weakened by an unwarned raid type. Not specified here. |
| **RFC-014** (proposed, Instance & Realm Lifecycle) | Realm/dungeon-interior NPCs are out of scope (§Non-goals) — this RFC covers overworld village NPCs only. |
| **RFC-015** (proposed, Client Replication & Interest-Set Protocol) | Owns the wire encoding for NPC state; this RFC states only that the roster is near-static (fixed at worldgen, §9) and fits the same push-on-change budget pattern RFC-021 §5.4 already established for map markers — not a new protocol. |

---

## Multiplayer & Simulation-LOD Considerations

- **Civilian NPCs are home-anchored local wanderers, never flow-field pathers** (§7) — the identical
  cost class GAME.md §5 already validated for ~620 Wild-faction creatures. A sleeping chunk (0 Hz)
  answers "what is this NPC doing" exactly as cheaply as an active one: the NPC's state machine
  simply stops advancing and resumes from wherever it left off when the chunk wakes — no catch-up
  simulation, no missed-tick reconciliation, because nothing about `Idle`/`MoveToWaypoint`/
  `WorkAction` is time-critical to anyone not standing nearby to watch it (GAME.md §3's general chunk
  LOD rule, applied here without modification).
- **Guard NPCs inherit whatever LOD tolerance RFC-007/RFC-010 already specify** for RL-driven
  entities and dojo/garrison training — this RFC does not add or relax any tick-rate requirement for
  them.
- **The roster is stable between tier/structure-change events and never grows or shrinks from a
  clock or player inaction** (§8, §9) — no per-tick NPC spawn/despawn logic, no population
  simulation. It *does* recompute the moment a village's tier crosses a roster-relevant threshold or
  its `Structure` list gains a house (§8.1/§8.2's recomputation notes), both of which are
  worldgen/gameplay-caused, event-driven changes — the same "checked continuously, not polled" model
  RFC-021 §4.3 already uses for tier itself. This keeps NPC replication in the same near-static bucket
  as RFC-021's map markers: a full roster snapshot at login/chunk-entry, deltas only on those
  role/skin/position-anchor/roster-count changes (rare, event-triggered, never per-tick), no polling.
- **Sheltering transitions piggyback on the existing beacon proximity check** (P2's `PlayerBeacon`,
  the same mechanism RFC-021 §"Multiplayer" cites for discovery/fog updates) for hostile-Monster
  detection. The raid-warning case has no event to subscribe to yet: RFC-021 §3.4 confirms the
  scout/announcement system is P3 scope, unassigned to a numbered RFC, and today's placeholder raid
  trigger fires with no warning at all — this RFC's raid-triggered Sheltering is therefore blocked on
  that P3 work landing, not "already shipped" as an earlier draft of this section claimed (§7's note
  on the same gap). No new subscription mechanism is needed once it does land; the beacon-style event
  subscription pattern is the same either way.
- **Determinism.** Role, skin, and roster-split assignment (§9) must diff to zero between GCC/Linux
  and MSVC/Windows builds, using the same integer-arithmetic discipline `village.hpp` already
  documents. This must be re-verified whenever the role/skin tables are populated, the same way
  village/fort placement was verified in R6/R7.

---

## Tone Guardrail Compliance

Checked against GAME.md §0's test: does anything here count down behind the player's back, or create
obligation pressure?

1. **Palette swap (§1.2).** Free, instant, reselectable without limit, no cooldown, no unlock quest.
   A one-off cosmetic choice, not a schedule — the same distinction RFC-021 §4.3 draws between a
   resource cost and a timer, applied here to a feature with no cost at all.

2. **NPC roster size (§8).** Set at worldgen and stable thereafter except when the village's tier or
   built structure count *increases* (§8.1/§8.2's recomputation notes) — both are player-caused or
   worldgen events (trade, defense, road connection, a nearby fort closing; per GAME.md §6's own
   tier-up triggers), never a clock, and the roster never shrinks from player inaction. This is the
   same "chững, không tụt" (stall, never decay from neglect) principle GAME.md §6 states for village
   tiers, applied here to who lives in the village — including the guard garrison, which must be able
   to grow when a village tiers up live or GAME.md §6's Citadel-tier "quân đội thường trực" promise
   would never actually be honored.

3. **Sheltering on raid warning (§7).** Triggered by the identical scout-warned event GAME.md §6
   defends at length (a full game-day of advance notice, chosen specifically so a raid reads as an
   invitation, not an ambush). This RFC adds no separate countdown for NPCs — they react to the same
   warning the player sees, at the same time. **This guarantee is only as real as the warning system
   itself, which is not shipped yet** (today's placeholder fires unwarned, per RFC-021 §3.4; the real
   scout/announcement system is P3, unassigned to a numbered RFC) — see §7 and §Multiplayer for the
   explicit dependency this now carries.

4. **Nightfall Sheltering for `kChild` (§7).** A recurring day/night behavior gate tied to the
   world's existing time-of-day clock, not a new timer with consequences: no reward, penalty, or
   quest state depends on whether a player happens to see a child NPC outdoors at a given hour. This
   is ambient flavor in the same non-blocking class as GAME.md §6c's particle weather layer — present
   for atmosphere, invisible to every system that matters for progression.

5. **No shop inventory/stock-reset mechanic is specified.** This RFC deliberately does not give
   `kMerchantShop` a "restocks daily" or "return tomorrow" behavior — that would be exactly the
   login-frequency-dependent pressure §0 forbids. Shop economy (if any) is explicitly non-goal here
   (§Non-goals), left for whichever future RFC specifies it to inherit this same discipline.

6. **No NPC ever gates content behind presence or frequency.** A quest-giver NPC is reachable any
   time the player chooses to visit (RFC-020 already established the quest state machine has no
   wall-clock transitions); this RFC adds a fixed *location* for that NPC, not a *window* during
   which they're available.

No mechanic in this RFC creates a deadline, a decaying resource, a login-frequency dependency, or a
countdown the player did not personally trigger by an action already defended elsewhere (visiting a
village, GAME.md's own raid-warning design).

---

## Open Questions

1. **Camp-tier (Bậc 0, GAME.md §6) worldgen path is unverified.** `village.hpp`'s `plan_of(tier)`
   switch handles cases 1–4 plus an unlabeled default; nothing in the read source confirms whether
   tier-0 "Trại lẻ" uses this same walled-village pipeline (falling through to `default`, which
   returns the *largest* plan — implausible for a 2–5 population camp) or the separate
   tent/stronghold-style builder (`kTentA/B/C`, shared via the same `place()` verb strongholds use).
   This RFC's §8.1/§8.2 roster tables assume a village-shaped Camp; if Camp is actually built via the
   tent pipeline, the civilian roster formula (which reads house-kind `Structure` entries) needs a
   parallel tent-kind reading rule. Needs a source check before implementation.
2. **`Child`'s unsplit `SpriteSheet.png` (§2).** Either run the existing per-folder splitting tool
   against it before this RFC ships, or commit to `Boy` as the v1 `kChild` skin and drop `Child` from
   the pool. Left open pending a decision on tooling effort.
3. **Multiplicative tint vs. true palette remap for the recolor system (§1.2).** The shipped `tint`
   mechanism recolors the whole sprite including skin tone and headband; whether that reads well on
   this rig or needs a true index-color remap (shader or pre-baked variants) is a visual-verification
   question this RFC cannot answer from the source alone. Needs a rendering pass and a look before
   `RENDER_SPEC.md` commits to one technique.
4. **Civilian split percentages and dwell timings (§7, §8.1)** — the 40/30/30 farmer/child/wanderer
   split and the 4–8s/3–6s dwell windows are first guesses with no playtest behind them. Needs a pass
   once a handful of villages are populated and visibly walked through.
5. **Escort-quest AI (RFC-020's open question, partially addressed here, §Interactions).** This RFC
   supplies the general `Npc` actor and a four-state civilian FSM, but no `Follow-the-player` state
   and no definition of "the escorted NPC survived to the destination." Should that be a small
   addition to this RFC's FSM (a fifth state, gated on an active escort quest) in a future revision,
   or does it belong to a new RFC entirely? Left for RFC-020's owner and this RFC's owner to agree on.
6. **Wandering-merchant pathing cost against the real pathfield engine.** §7 assumes a
   `kMerchantWander` NPC can walk a fixed, precomputed road-segment sequence without touching the
   global flow field `ARCHITECTURE.md §5`'s `PathfieldActor` maintains for hostile creatures. This is
   the cheap answer by design, but has not been checked against how `PathfieldActor` actually
   distinguishes "reuse a precomputed route" from "request a new BFS field" — flagged for an engine-
   side sanity check before implementation, not assumed safe on read-through alone.
7. **Should NPC casualties ever become possible (§7's Sheltering-before-raid argument)?** Today the
   guarantee holds only because every raid is warned a full day ahead; if an unwarned raid or a PvP-
   adjacent mechanic is ever added, this RFC's civilians would need either a real vulnerability model
   (RFC-013's territory) or an explicit invulnerability rule stated as policy rather than an emergent
   property of timing. Left for whichever RFC introduces the first unwarned threat to resolve.
8. **`kMerchantWander`'s hostile-Monster shelter fallback (§7) is this RFC's judgment call, not a
   verified mechanism.** Pathing to "whichever end of the current road segment is nearer" is a
   reasonable placeholder given the role has no `home_struct`, but it hasn't been checked against
   `PathfieldActor`'s actual segment/endpoint data (see Open Question 6, the same engine-side gap) or
   playtested for whether it reads as safe rather than confusing. Needs a pass alongside Open
   Question 6.
9. **Is `PrefabId::kMarketYard` (§8.1's corrected `kMerchantShop` gate) itself an indexable
   `Structure` entry, or a separate parcel/prefab concept `home_struct` can't point at as specified?**
   `village.hpp`'s `place_parcel(...kMarketYard)` call was read far enough to confirm the tier gate
   (`tier == 2` or `tier ≥ 3`) but not far enough to confirm whether the placed parcel is added to the
   same `Structure` list `home_struct` indexes into. If it isn't, `kMerchantShop`'s `home_struct`
   needs a different anchor (e.g. the nearest house `Structure`, or a new parcel-index field) before
   implementation. Needs a source check.

---

## Non-goals

- **Combat stats, kit, or ability data for any NPC.** RFC-001 (ability system) and RFC-009 (damage/
  resistance) own that; civilian roles carry none, guard roles inherit their combat identity entirely
  from the RFC-007 archetype they're assigned to.
- **Quest dialogue content, quest data schemas, reward tables.** RFC-020's (accepted) — this RFC
  supplies only the role/skin/portrait association a quest can reference (§2, §8.1's `kQuestGiver`
  assignment), never quest text, objectives, or rewards.
- **Guard/village-defense AI, RL training, reward shaping, or checkpoint lifecycle.** RFC-007's
  (accepted) in full — this RFC assigns roster slots and skins only (§4, §6, §8.2).
- **Boss authoring, the Dojo Master, or any `Faction::kMonster` entity.** RFC-005/RFC-007's — not a
  village NPC, explicitly excluded from the taxonomy (§4).
- **Mount and animal creatures** (`Actor/Animal/*`, GAME.md's livestock/wildlife). Explicitly out of
  scope for this run — this RFC is ninja-character-and-NPC-only.
- **Shop inventory, pricing, trade mechanics, or any village economy simulation.** Not specified by
  any current or proposed RFC; `kMerchantShop`/`kMerchantWander` exist here only as a presence and a
  behavior state, not an economy.
- **Escort-quest `Follow` behavior, arrival/survival semantics.** Partially unresolved, see Open
  Question 5 — not specified in this revision.
- **Realm- or dungeon-interior NPCs.** This RFC covers overworld village NPCs only; GAME.md's
  dungeon/mine interiors are monster-only content per existing design, and any future realm-specific
  NPC (a rest-realm attendant, say) is a future revision's concern, not this one's.
- **Rendering technique for the palette swap** (shader-based remap vs. pre-baked atlas variants vs.
  the shipped multiplicative tint). `RENDER_SPEC.md`'s call; this RFC fixes the data contract only
  (§1.2).
- **NPC injury, death, or respawn mechanics.** RFC-013's (proposed) territory if the Sheltering
  guarantee (§7) is ever insufficient; this RFC does not define what happens if an NPC is ever
  actually targeted.
- **Wire protocol, delta encoding, or per-view replication budgets for NPC state.** RFC-015's
  (proposed) — this RFC states only that the roster is near-static and fits the existing
  push-on-change pattern (§Multiplayer).
- **Village structure placement, footprint sizing, or the worldgen pipeline that decides how many
  houses a village gets.** RFC-022's (this batch's sibling) — this RFC is strictly a consumer of that
  output (§8).

---

## Review Record

**Votes:** Reviewer A — revise. Reviewer B — revise.

**Applied (upheld by both reviewers):**
- §3: `faction = kVillager` unimplementable — added normative `faction_override` field on `Creature`.
- §3: `disposition` claim backwards (defaults `kHostile`) — `Npc` construction now sets `kNeutral`.
- §7/Multiplayer: `obs_class` misattributed, no targetability hook exists — `strike()` must now gate on Sheltering state.
- §7/Multiplayer/Tone#3: "already-shipped raid-warning event" false (RFC-021 §3.4 says P1 stand-in, real system unassigned P3) — corrected to an explicit dependency.
- §8/§8.2/Multiplayer/Tone#2: "never grows/shrinks at runtime" contradicted the tier-keyed guard table — roster now recomputes on tier/structure-count increase (event-driven, not clock-driven).
- §7/§8.1: `kQuestGiver`'s "plaza/hall" anchor unreachable below tier 3 — now a house slot below tier 3, hall only from tier 3 on.
- §8.1: `kMerchantShop` gate corrected from untiered `kHouseBlue` roll to tier-≥2 `kMarketYard`.
- Interactions/RFC-020 row: softened "resolved" claim to match the tier-triggered presence rule.

**Applied (single-reviewer, proof sound):** §7 `kMerchantWander` had no `home_struct` for Sheltering (Reviewer A only) — exempted from the raid trigger, given a flagged fallback for the Monster trigger (Open Question 8).

**Unresolved:** whether `kMarketYard` is an indexable `Structure` (OQ9); whether `kMerchantWander`'s shelter fallback holds against `PathfieldActor` (OQ8/6) — both need a source check this pass couldn't complete.
