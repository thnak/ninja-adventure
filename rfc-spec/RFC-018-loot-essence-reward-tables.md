# RFC-018: Loot, Essence & Reward Tables

> Status: **Accepted (revised after review)**
> Design canon: [GAME.md §1](../GAME.md) (three resource axes — Đất/Đá/Tinh chất; "Tinh chất: chỉ có
> ở cõi thử thách → phép thuật, nâng cấp bậc cao, mở cổng mới"), [§4](../GAME.md) (ring adaptation
> table; Ashlands "chỉ khai mỏ; thanh tẩy bằng Tinh chất"), [§7](../GAME.md) (elemental status ladder
> — the five combo rows socket gems key off), [§8](../GAME.md) (material tiers Đồng→Sắt→Thép→Bí ngân;
> "Trang bị có độ bền và ổ khảm. Đá khảm rơi từ hầm ngục, cho hiệu ứng gắn với hệ combo ở trên"),
> [§12](../GAME.md) (Túi đồ screen: "vật phẩm, độ bền, ổ khảm")
> As-built source grounding (verified against `src/` on 2026-07-24): `src/world/chunk_actor.hpp:1319-1348`
> (`strike()` — the two placeholder drop paths this RFC replaces: the boss branch grants a flat
> `GrantXp{skill,400}` + `GrantItems{kProduce,10}` with the comment "P4 owns real loot tables...
> inventing a boss loot table now would be inventing it twice"; the general branch grants XP only,
> with the comment "Monsters drop nothing yet — loot tables are P4"; the `Faction::kWild` branch
> granting `GrantItems{kProduce,1}` is unchanged by this RFC), `src/world/tiles.hpp:357-359`
> (`enum class ItemKind : uint8_t { kWood, kStone, kSeed, kProduce, kCount=4 }` — confirmed, no
> equipment/durability/socket concept anywhere in this file), `src/world/tiles.hpp:108-134`
> (`enum class Ring`, `kRingEdge` — the five-ring geometry this RFC scales drops by, cited not
> re-derived), `src/world/tiles.hpp:300-395` (`CreatureKind`, `Faction`, `CreatureStats`, `stats_of()`
> — the four Monster-faction kinds this RFC's monster loot table covers: Slime/Spider/Ghost/Skull,
> plus the single scripted `kBoss`), `src/world/protocol.hpp:213-216,242-245`
> (`GrantItems{ItemKind,int32_t}`, `GrantXp{Skill,uint32_t}` — the only two reward-delivery messages
> that exist today; §10.2 adds a third, `GrantEquipment{EquipSlot,EquippedItem}`, for the boss-only
> finished-gear drop — everything else in this RFC still flows through the existing two),
> `src/world/chunk_actor.hpp:1429-1432` (`grant()` — the trusted
> hand-off from chunk to `PlayerActor` this RFC's rolls call through, unchanged)
> Depends on: RFC-002/RFC-008/RFC-009 (accepted — the closed six-document status set §7.4, the
> `[0,1000]` build-up scale RECONCILIATION.md ruling 5 fixes, and RFC-009's `DamagePacket` shape,
> cited and consumed, never modified), RFC-019 (accepted-with-revisions — the Essence-gate *shape*
> §5.7 already committed to, sized here; the co-op contribution ledger §5.8 this RFC's multi-recipient
> rolls reuse), RFC-013 (accepted-with-revisions — the `items_[kItemKinds]` array this RFC extends and
> the death/ejection rule that governs what a player can lose after this RFC hands it to them), RFC-021
> (accepted-with-revisions, partially superseded by RFC-022 — the ring→ore-tier correlation §3.6
> explicitly hands this RFC "the crafting/equipment material-tier table"), RFC-022 (accepted-with-
> revisions — `MapId`/`MapDescriptor`/`Portal`/`RealmType`/`PortalKind`, cited; this RFC names one
> small field gap in `MapDescriptor` it needs and does not already have, flagged not silently assumed
> — §6.6), RFC-020 (accepted-with-revisions — its `reward.kind: item_table` carries `ref: "loot.<name>"`
> and calls `RFC-018.roll(ref, recipient, share_pm)`; this RFC is the document format and the function
> that call resolves against, QV13/§7.2 of RFC-020, cited verbatim)
> Depended on by: RFC-016 (accepted-with-revisions — will need a schema addition for the equipped-
> item/durability/socket state this RFC defines; this RFC specifies the shape, RFC-016 the encoding),
> RFC-020 (its `item_table` reward resolves against this RFC's `loot.*` documents by id)
> Sibling RFCs this batch: RFC-011 (Combat HUD), RFC-012 (Combat Audio), RFC-017 (Balance Tuning &
> Test Harness), RFC-024 (Leader Failure & Session Recovery)

---

## Summary

Two placeholder drop paths exist in shipped code today and both are named, with their own comments,
as things P4 must replace: a boss kill pays a flat `400 XP + 10 produce`, and an ordinary monster kill
pays XP only, nothing else (`chunk_actor.hpp::strike()`). This RFC is that replacement, and it does
two linked jobs neither of which can exist without the other:

1. **Invents the equipment data shape** — weapon/armor slots, material tiers, durability, and socket
   gems — because there is currently nothing in `src/` for a loot table to drop *into*. `items_[]` is
   a four-entry stackable-resource array (Wood/Stone/Seed/Produce); this RFC extends it with new
   stackable ordinals (ore, gems, Essence) and adds an entirely new, non-stackable **equipped-item**
   concept alongside it — green-field, confirmed absent by RFC-013 §3.6.5's own audit.
2. **Specifies the drop-table contract** — a new RFC-008-style JSON document domain (`loot`),
   deterministic seed-based rolls, ring/tier scaling using RFC-021's ring geometry and the material
   tier ladder GAME.md §8 names (Copper→Iron→Steel→Mythril), and a mechanical definition of Essence
   and socket gems consistent with GAME.md §1's "Essence only in challenge realms" rule and GAME.md
   §8's "gems drop from dungeons, keyed to the combo table" rule.

Everything in this document is **green-field** except the two cited placeholder call sites it
replaces and the five-ring geometry/material-tier axis it scales against (both cited, not
re-derived). Every new number is marked **(tunable)**.

---

## Motivation

1. **The placeholders name their own replacement.** `chunk_actor.hpp`'s boss-branch comment says "P4
   owns real loot tables... inventing a boss loot table now would be inventing it twice" — that
   sentence is only true once a real one exists to point at. This RFC is that pointer resolving to
   real data.

2. **RFC-019, RFC-020, and RFC-021 all deferred specific numbers to this RFC by name and are waiting.**
   RFC-019 §5.7 sized its Essence-gate *shape* ("1 unit per level, 3 total") but explicitly flagged the
   *amount* as "sized against nothing, because RFC-018... doesn't exist yet" (Open Question 3). RFC-020
   §7's `item_table` reward kind already ships a `ref: "loot.<name>"` field and a
   `RFC-018.roll(ref, recipient, share_pm)` call contract with nothing on the other end. RFC-021 §3.6
   states plainly "the crafting/equipment material-tier table belongs to RFC-018" while placing only
   the mine mouth. Three accepted/finalized documents have an open socket shaped exactly like this RFC.

3. **GAME.md §8 promises a chain the codebase cannot walk yet.** "Trang bị có độ bền và ổ khảm... Đá
   khảm rơi từ hầm ngục" (equipment has durability and sockets; socket gems drop from dungeons) and
   ROADMAP.md P4's own Done-when line — "đi hết được chuỗi từ quặng thô → thanh kim loại → vũ khí →
   khảm đá, và trang bị đó tạo khác biệt đo được trong chiến đấu" (walk the whole chain from raw ore to
   a socketed weapon, and that gear makes a measurable combat difference) — names an equipment concept
   that RFC-013 already confirmed, by direct grep, does not exist. Loot cannot drop into a hole that
   has no shape.

---

## Guide-level Explanation

### For a player

Killing things is worth more than XP now. A slime might leave behind a scrap of ore; a skull in the
outer wastes leaves behind better ore than one in the meadow, because it's a genuinely tougher skull.
None of that ore is rare in the "you'll never see it" sense — it's the raw material a future forge
recipe (P4, out of this RFC's scope) will ask for, and the point of a loot table is that you get some
just by fighting, not that you have to farm a spreadsheet.

Two new things live in your bag alongside wood/stone/seed/produce: **socket gems**, which only come
from stepping through a challenge-realm gate — the same gate GAME.md always said gems came from — and
**Essence**, the challenge-realm currency that eventually pays for the last stretch of a skill branch
(RFC-019), and (later, once those systems exist) things like permanently closing a fort or settling the
Ashlands. You never earn Essence by farming the overworld; it is the one resource that says "you went
into a dungeon," on purpose.

Your gear — a weapon, and separately, armor — has durability and, at higher material tiers, socket
slots. Durability wears down as you fight, but a worn weapon never stops working; it just performs a
notch below its tier until you repair it (how repair actually happens is a forge-recipe question, not
this RFC's). Nothing about a broken weapon locks you out of a fight — that would be exactly the kind of
"you're stuck until you deal with this" pressure the game's tone rules out.

A boss kill is still the best reward in the game, and if you fight one with friends, **everyone who
landed a hit rolls their own loot** — nobody has to fight over what drops, the same "abundance, not a
split pool" rule RFC-019 already uses for XP.

### For a designer

You author a drop table exactly the way you'd author a skill file — one small strict-JSON document
per creature or boss, in RFC-008's own directory-and-id conventions, a new domain (`loot`) alongside
`skill`/`status`/`entity`/`boss`. Rows are weighted, integer-only, and reference items and gems by id,
never by inline numbers a second document might disagree with. RFC-020's quest rewards already point at
these documents by the `loot.<name>` id shape; you never write a second table for "quest version of the
same reward."

### For an engineer

You need: (1) two new `PlayerActor` fields — an extended stackable `items_[]` (new `ItemKind`
ordinals: ore per tier, gems per channel, Essence) and a brand-new `equipped_[kEquipSlots]` array
holding non-stackable `EquippedItem` records (§4); (2) a `loot.*` JSON document schema and a
`LootTable::roll(seed) -> RewardBundle` pure function (§7–§8) callable from both `strike()`'s two
existing branches and RFC-020's reward resolver; (3) one small, explicitly-flagged field addition to
RFC-022's `MapDescriptor` this RFC needs and does not invent alone (§6.6) — everything downstream of it
(Essence/gem gating) is inert without that field existing, so it is called out, not assumed.

---

## Reference-level Design

### 1. Item taxonomy — what this RFC adds to `items_[]`, and what it does not touch

`ItemKind` (`tiles.hpp:357`) is extended additively — new ordinals only, the existing four
(`kWood=0, kStone=1, kSeed=2, kProduce=3`) untouched, `kCount` grows accordingly:

```cpp
enum class ItemKind : std::uint8_t {
    kWood = 0, kStone = 1, kSeed = 2, kProduce = 3,           // unchanged
    // NEW — this RFC. Raw material, one ordinal per tier (§3).
    kOreCopper = 4, kOreIron = 5, kOreSteel = 6, kOreMythril = 7,
    // NEW — socket gems, one ordinal per RFC-002/RFC-008 §7.4's closed six-document status set (§5)
    // *times* grade (§5, Minor/Lesser/Greater/Major — 6 channels x 4 grades = 24 ordinals). Grade is
    // baked into the ordinal itself, not a separate field, so a stackable `GrantItems{kind, count}`
    // grant never loses which grade it is dropping — the same reason ore gets one ordinal per tier (§3)
    // rather than a tier field on a single `kOre` ordinal.
    kGemColdMinor = 8,    kGemColdLesser = 9,    kGemColdGreater = 10,   kGemColdMajor = 11,
    kGemHeatMinor = 12,   kGemHeatLesser = 13,   kGemHeatGreater = 14,   kGemHeatMajor = 15,
    kGemShockMinor = 16,  kGemShockLesser = 17,  kGemShockGreater = 18, kGemShockMajor = 19,
    kGemEarthMinor = 20,  kGemEarthLesser = 21,  kGemEarthGreater = 22, kGemEarthMajor = 23,
    kGemStaggerMinor = 24, kGemStaggerLesser = 25, kGemStaggerGreater = 26, kGemStaggerMajor = 27,
    kGemWetMinor = 28,    kGemWetLesser = 29,    kGemWetGreater = 30,   kGemWetMajor = 31,
    // NEW — the challenge-realm currency (§6).
    kEssence = 32,
    kCount = 33,   // was 4
};
```

This is the entire change to the *stackable* side of inventory. It is compatible, without a schema
change, with RFC-016's already-generic `player_items(account_id, item_kind, count)` table (RFC-016
§4.1) — new ordinals are new rows, not new columns, exactly the "additive, id-referenced" growth
pattern RFC-008 already uses for its own domains.

**What this RFC does not add to `items_[]`:** the equipped weapon and armor themselves. A stackable
count cannot hold durability or a socket array — this is the reason RFC-013 §3.6.5 found nothing to
exempt when it clears `items_[]` on ejection ("no separate 'equipped gear' concept yet"). §4 below is
the new, parallel state that concept needs.

### 2. Slot taxonomy — deliberately two, not a full paperdoll

**New decision, argued, not found in any prior document.** This RFC defines exactly two equip slots:

```cpp
enum class EquipSlot : std::uint8_t { kWeapon = 0, kArmor = 1, kCount = 2 };   // (tunable)
```

No GAME.md section specifies a helm/chest/boots/ring paperdoll; §12's Túi đồ screen only promises
"vật phẩm, độ bền, ổ khảm" (items, durability, sockets) without naming a slot count. Two slots is a
deliberate minimalism argument, not an omission: it mirrors the two-ability-kit economy the combat set
already commits to everywhere (RFC-001's two ability poses, RFC-019's two-slot loadout) — a project
this consistently small-surfaced in its combat kit gains nothing from a six-slot paperdoll it would
also have to draw six new inventory icons and durability bars for, on an asset budget GAME.md §13
states is explicitly zero. If playtesting later wants more granularity, `EquipSlot::kCount` is one
enum value away from growing (Open Questions §1).

### 3. Material tiers — the crafting/equipment table RFC-021 §3.6 hands to this RFC

Four tiers, matching GAME.md §8's ladder exactly (Đồng→Sắt→Thép→Bí ngân) and RFC-021 §3.6's
ring→ore-tier correlation, cited not re-derived:

```cpp
enum class MaterialTier : std::uint8_t { kCopper = 0, kIron = 1, kSteel = 2, kMythril = 3, kCount = 4 };
```

| Tier | Ring where ore appears (RFC-021 §3.6, cited) | Weapon `tier_damage_pm` **(tunable)** | Armor `tier_dr_bonus` **(tunable)** | Armor `tier_toughness_bonus` **(tunable)** | Max durability **(tunable)** | Socket slots **(tunable)** |
|---|---|---|---|---|---|---|
| Copper | 0–1 (Meadow/Forest) | 1000‰ (baseline, ×1.0) | +0 | +0 | 200 | 0 |
| Iron | 2 (Wetland) | 1150‰ (×1.15) | +4 | +0 | 350 | 1 |
| Steel | 3 (Snow) | 1350‰ (×1.35) | +9 | +1 | 550 | 1 |
| Mythril | 4 (Wasteland) | 1600‰ (×1.6) | +16 | +2 | 900 | 2 |

**How these three columns plug into accepted combat canon, without amending it.** RFC-009 §4.2's own
comment states that "attacker-side scaling (school damage, ability `damage_scale`, ring scaling, charge
multipliers) was applied at emission by RFC-001" — before a `DamagePacket` is even constructed. This
RFC's `tier_damage_pm` is a new input at that same emission point, composed multiplicatively with
RFC-019's `skill_scale` and the ability's own `damage_scale` — the exact same non-committal posture
RFC-019 §5.7 already took for its own passive ("this RFC does not specify how they combine" — RFC-001/
RFC-009's territory). `tier_dr_bonus` and `tier_toughness_bonus` fill RFC-009 §4.8's `DefenderSheet`'s
two named-but-previously-unsourced slots exactly: `tier_dr_bonus` resolves `dr[0]`, the "gear" half of
`dr[2]`'s `// resolved sources, ‰ (gear, stance)` comment (RFC-009 line 458, `dr[1]` stays boss-guard-
stance territory, RFC-005's); `tier_toughness_bonus` resolves the "gear" half of `toughness`'s
`// resolved: tier + gear` comment (RFC-009 line 457) — both new named numbers those formulas may read,
never a new struct field, formula, or order of operations in RFC-001/002/003/009/010, matching the
precedent RFC-019 §5.7 already set and RFC-003 §Interactions already argues generally ("no per-skill
interaction hooks, ever" — this is the entity-stat equivalent of that discipline).

**Flagging RFC-009's stale `RFC-004` gear citations.** RFC-009 §4.7's Sources column attributes armour
DR and heavy-armour toughness bonuses to "RFC-004 gear" (RFC-009 lines 432 and 457) and RFC-009's own
Non-goals list gear/socket definitions as "RFC-004" (RFC-009 line 587); a direct grep of
`RFC-004-terrain-combat-entity.md` for gear/armor/armour returns zero matches — RFC-004 never defined a
gear concept, and this RFC (§4, §1) is where "gear" as RFC-009 uses the word is actually defined. This
RFC does not have the authority to amend RFC-009's accepted text, so — matching the posture §13 already
takes toward RFC-022 — this is flagged, not silently left dangling: RFC-009's editor should retarget
those three citations from `RFC-004` to `RFC-018`.

Skill points and material tier remain orthogonal, restating RFC-019 §5.7 rule 1 for gear specifically:
a Melee-20 character in Copper gear and a Melee-1 character in Mythril gear are both valid, coexisting
states; this RFC introduces no unlock gate that ties tier access to skill level.

### 4. Equipped-item data shape (new — the entire answer to "what does loot drop into")

```cpp
// NEW state — not part of items_[]. Two fixed slots (§2), never a variable-length inventory.
struct SocketGem {
    ItemKind kind = ItemKind::kWood;   // one of the 24 kGem<Channel><Grade> ordinals (§1), which already
                                        // encode grade — no separate grade field needed, or kWood as the
                                        // "empty socket" sentinel — kWood can never itself occupy a socket,
                                        // so it is a safe "none" value requiring no new enum, mirroring
                                        // RFC-013 §6.2's zero-default-as-sentinel pattern
};

struct EquippedItem {
    std::uint16_t item_id = 0;          // 0 = "no item equipped in this slot" (a bare hand / bare skin baseline)
    MaterialTier  tier = MaterialTier::kCopper;
    std::int16_t  durability = 0;
    std::int16_t  max_durability = 0;   // set from §3's table at craft/drop time; snapshot, not recomputed live
    SocketGem     sockets[2];           // fixed at the Mythril ceiling (§3); tiers with fewer slots
                                          // simply leave the trailing entries at the kWood "empty" sentinel
};

// New PlayerActor field, mirrors items_[kItemKinds]'s existing shape.
EquippedItem equipped_[static_cast<int>(EquipSlot::kCount)];   // NEW, this RFC's entire contribution to PlayerActor
```

`item_id` references a new RFC-008 document domain, `gear` (`data/combat/gear/<name>.gear.json`,
following RFC-008 §2's directory convention exactly), which names the base slot (`weapon`/`armor`) and
a display icon — the crafting recipe that produces one, and the socketing/repair verbs that mutate one,
are both crafting workflow and explicitly out of this RFC's scope (Non-goals). This RFC only fixes the
*shape* a `gear.*` document and a runtime `EquippedItem` must agree on.

#### 4.1 Durability: wears, never breaks

- **Weapon** durability decrements by **1 per landed hit (tunable)**; **armor** decrements by **1 per
  hit received (tunable)**. Both floor at 0, never negative.
- **A durability of 0 does not disable the item, unequip it, or destroy it.** GAME.md §0's guardrail
  test applies here exactly as it does everywhere else in this batch: an item that stops functioning
  at 0 durability is a soft lockout the player didn't choose the timing of, the same shape problem
  RFC-013 already rejected for a "downed" combat state. Instead, durability interpolates the item's
  *effective* tier downward, floored at Copper, never at zero output:

  ```
  frac = durability / max_durability                      // 0..1, integer per-mille internally
  effective_tier_damage_pm = lerp(tier_damage_pm(tier_below(tier)), tier_damage_pm(tier), frac)
  effective_tier_dr_bonus  = lerp(tier_dr_bonus(tier_below(tier)),  tier_dr_bonus(tier),  frac)
  // tier_below(kCopper) = kCopper — a broken Copper item never drops below its own baseline.
  ```

  A fully-worn Mythril sword performs like a fresh Steel one, not like a stick — real, felt
  degradation, never a brick. Repairing back to `max_durability` is a forge-station action (P4
  crafting, out of scope) that this RFC names only as the hook a future RFC would restore the field
  through, the same "name the hook, don't design the system" move RFC-013 §8 already made for food
  healing via `GrantVitals`.
- Socket gem effects (§5) are **not** interpolated by durability — a gem's buildup rider fires at full
  strength regardless of the host item's wear, since a gem is a separate, discretely-acquired object
  socketed into the item, not a property of the base material.

### 5. Socket gems — the mechanical link to RFC-002's status ladder

GAME.md §8 names the mechanic directly: "đá Hoả cho vũ khí cận chiến tự đặt Bỏng, mở combo tự thân" (a
Fire gem makes a melee weapon auto-apply Burning, opening a self-combo). Six gem *channels* exist, one
per RFC-008 §7.4's **closed** six-document status set (five build-up channels plus the one coating) — no
seventh channel is inventable without RFC-002 shipping a seventh status first — each further split into
four grades (§1), for 24 concrete `ItemKind` ordinals total:

| Gem channel (§1's `kGem<Channel><Grade>` ordinals) | RFC-002/008 status id | What it applies on a landed hit |
|---|---|---|
| `kGemCold*` | `status.cold` | `{channel: "cold", amount: N}` build-up rider |
| `kGemHeat*` | `status.heat` | `{channel: "heat", amount: N}` |
| `kGemShock*` | `status.shock` | `{channel: "shock", amount: N}` |
| `kGemEarth*` | `status.earth` | `{channel: "earth", amount: N}` |
| `kGemStagger*` | `status.stagger` | `{channel: "stagger", amount: N}` |
| `kGemWet*` | `status.wet` | `{coating: "wet", ticks: N}` |

`amount`/`ticks` values, by **gem grade (tunable, new)**, on RECONCILIATION.md ruling 5's `[0,1000]`
absolute scale (no rescale, cited not re-derived): `*Minor` 150, `*Lesser` 300, `*Greater` 500, `*Major`
800 — the `<Grade>` suffix on §1's ordinal selects the row directly, so resolving a socketed gem's
`amount`/`ticks` is a lookup on `kind` alone, no separate grade field to read. Gem grade is a property of
the gem drop itself (§10's loot rows specify which `kGem<Channel><Grade>` ordinal, via a `gem.<channel>_
<grade>` item id), independent of the material tier of the item it gets socketed into — a Minor Cold gem
(`kGemColdMinor`) in a Mythril sword and a Major Cold gem (`kGemColdMajor`) in a Copper dagger are both
legal, and the socket, once created by a §4 tier's slot count, accepts any grade.

**How this composes with RFC-009's `DamagePacket`, without amending it.** `DamagePacket.buildup[2]`
(RFC-009 §4.2) is accepted canon, fixed at two riders, already documented as "matches the two-ability
kit; no skill in v1 needs more." This RFC does not add a third slot. Instead: a socketed weapon's gem
rider is written into whichever of `buildup[0]`/`buildup[1]` the *emitting attack* leaves at its default
"unused" (`channel = kNone`) value — the basic attack (which authors no buildup of its own) always has
both riders free, so a gemmed weapon's effect fires on every ordinary swing, exactly the "auto-apply on
your basic attack" reading GAME.md §8's own example describes. An ability that already authors two
buildup riders of its own (RFC-008 §7.4's `impact.statuses`) leaves no free slot; this RFC does not
invent a third one to force the gem through on that specific ability — a static authoring fact a
designer can see and design around (an ability's own doc already declares how many riders it uses),
not a runtime conflict this RFC has to arbitrate. Multiple gems on the same item (up to the tier's
socket count, §3) apply this same one-free-slot rule in socket order; a second gem is inert on any
attack that has already consumed both riders (its own author's ability rider, plus the first gem).

### 6. Essence — mechanical definition

Essence is `ItemKind::kEssence` — an ordinary stackable integer count in `items_[]`, no new struct,
consistent with GAME.md §1's framing of it as one of the game's three resource axes, not a special
currency system. Its two defining rules, both new to this RFC and both direct readings of GAME.md:

1. **Essence only comes from challenge-realm kills.** GAME.md §1 states this outright: "Tinh chất (các
   cõi): chỉ có ở cõi thử thách" (Essence, from the realms: only in challenge realms). §10's loot rows
   gate `essence` grants behind the running instance's `RealmType == kChallenge` (RFC-022 §2.2) — never
   from an overworld kill, a mine, or a rest realm, no exceptions. §6.6 below names the one small data
   gap this gate needs closed.
2. **Essence never decays or expires.** Same monotonic-count treatment as every other stackable item;
   this RFC adds no timer, no "use it within N days," matching the tone-guardrail argument RFC-019 §5.7
   already made for the capstone gate this currency feeds ("waits, it does not count down").

**Spend catalog: only the one already committed elsewhere.** RFC-019 §5.7 already sized the shape of
its own gate ("1 unit of Essence per level from 18 to 20, 3 total") and flagged the *amount* as pending
this RFC. §10.3 below proposes the acquisition-rate numbers that size against it. GAME.md also names two
other Essence sinks — permanently closing a fort (§4: "đóng vĩnh viễn bằng Tinh chất") and Ashlands
adaptation (§4: "thanh tẩy bằng Tinh chất") — **neither has an owning RFC yet** (RFC-019 §5.7 already
flagged the ring-adaptation *unlock mechanism* as "no RFC number chartered yet"). This RFC defines
Essence's *acquisition* fully; it does not invent those two spend mechanics, only confirms the currency
they will eventually draw from is the same one RFC-019 draws from — one resource, multiple future sinks,
not a separate currency per sink.

### 7. The `loot` document domain (RFC-008 conventions, extended)

New directory, new domain, same rules as every other RFC-008 domain (§1/§4 of that RFC: strict JSON,
integers-only, `ids.lock.json` entry, canonical-form hashing):

```
data/combat/loot/<name>.loot.json     # domain "loot", id = "loot.<file-stem>"
```

```json
{
  "schema": 1,
  "id": "loot.slime",
  "kind": "creature",
  "entries": [
    {"item": "item.ore_copper", "chance_pm": 250, "qty_min": 1, "qty_max": 2},
    {"item": "gem.cold_minor",  "chance_pm": 15,  "qty_min": 1, "qty_max": 1, "realm_gate": "challenge"}
  ],
  "essence_pm": 0
}
```

- `kind ∈ {creature, boss}` (new, closed enum, V-numbering deferred to RFC-008's own editor if this
  domain is folded into that RFC's validator table).
- Every `entries[i]` is independently rolled (§8) — a kill can produce zero, one, or several rows in
  the same table, never a single mutually-exclusive pick, matching how `GrantItems` is already an
  independent, unconditional grant per call (`chunk_actor.hpp`'s existing `grant()`).
- `chance_pm` is a straight per-mille roll against the entry, before ring/tier scaling (§9) multiplies
  it.
- `realm_gate?` (optional, `"challenge"` or absent) is this RFC's explicit authoring guard for
  challenge-realm-only rows (gems, Essence-adjacent materials) — the packer (V-rule, RFC-008's
  build pipeline) rejects a `gem.*`/`essence`-item row with no `realm_gate: "challenge"`, so a designer
  cannot accidentally author a gem drop for an overworld slime; the gate is enforced at author time,
  not left to a runtime check alone.
- `essence_pm` is a top-level, single value (not a per-entry row) — Essence is a currency grant, not an
  item stack, so it does not compete with the weighted `entries` list; it is added directly to
  `items_[kEssence]` when the roll succeeds and the realm gate (§6.6) is open.

### 8. Deterministic rolls — the seed function

**Roll seed, pure function of already-known values, no new persisted state:**

```cpp
std::uint64_t roll_seed = splitmix64(world_seed_ ^ creature_spawn_id
                                      ^ (static_cast<uint64_t>(kill_world_tick) << 32)
                                      ^ (static_cast<uint64_t>(contributor_account_id) << 20));
```

`creature_spawn_id` uses `Creature::id` — the per-chunk-monotonic, chunk-key-combined field already
shipped (`tiles.hpp:488`; assigned identically in both `make_creature()` at `chunk_actor.hpp:575` and
`spawn_boss()` at `chunk_actor.hpp:970`, `c.id = ++next_id_ | (chunk_key(coord) << 12)`) — this RFC adds
no new `spawn_id` field, it reuses the uniqueness scheme that already exists, so two creatures of the
same kind spawned in the same chunk at different ticks never collide on a seed. `contributor_account_id`
is the id of the player the roll is being resolved *for* — the striker on an ordinary kill, or (§10.2)
one of a boss's several qualifying contributors — so a seed is reproducible from data already local to
the chunk resolving the kill (`world_seed_`, the creature's shipped `id`, the tick `strike()` is already
running on, and the account id of the specific recipient being rolled for) — no cross-actor query,
matching the "reads only its own local state" discipline this batch already applies elsewhere (RFC-013
§3.6.1, RFC-021 §"Multiplayer" ring lookups).

**What this determinism claims, and what it does not.** Per ARCHITECTURE.md §2c's own distinction
(seed-pure state must match byte-for-byte across toolchains; message-order-dependent state, like
migration counts, is explicitly exempted): a roll is a pure function of `(world_seed_, spawn_id,
kill_tick, contributor_account_id)`, so **replaying an identical fight for the identical recipient**
(same creature, same spawn, same kill tick, same account) reproduces an identical drop on any node or
toolchain — useful for RFC-017's (sibling, Balance Tuning & Test Harness) sweep tooling and for a player
support question ("why did I get X"). It does **not** claim two different fights that happen to kill the
same creature kind at different ticks get the same drop, and it does **not** claim two different
contributors on the *same* kill get the same drop — both are supposed to differ, and the
`contributor_account_id` term is precisely what makes the second one differ (§10.2).

`splitmix64` (a well-known, dependency-free 64-bit mixer, already integer-only per RFC-008 §3's
discipline) is this RFC's proposed mixer; any fixed, deterministic, dependency-free 64-bit hash
satisfies the requirement equally and the exact choice is an implementation detail, not load-bearing
design (Open Questions §5).

### 9. Ring and tier scaling

Two independent scalars, both cited from RFC-021 rather than re-derived, applied multiplicatively to
`chance_pm` and additively to `qty_max` (never the reverse — chance stays a probability, quantity stays
a count):

| Ring (RFC-021 §2.1, cited) | Loot chance multiplier **(tunable)** | Qty bonus **(tunable)** | Ore tier this ring's Monster kills may drop (RFC-021 §3.6, cited) |
|---|---|---|---|
| 0 Meadow | ×1.0 | +0 | Copper |
| 1 Forest | ×1.2 | +0 | Copper |
| 2 Wetland | ×1.4 | +1 | Iron |
| 3 Snow | ×1.6 | +1 | Steel |
| 4 Wasteland | ×1.8 | +2 | Mythril |

A ring's monster-kill ore tier matches the tier its own mines produce (RFC-021 §3.6's correlation,
reused rather than re-derived) — a ring-4 Skull and a ring-4 mine both pay out in Mythril-grade
material, so a player fighting their way outward is never mechanically behind a player who only mines
outward. This is the ring/tier scaling the assignment's grounding calls for, expressed as one small,
cited table rather than a new formula.

### 10. Replacing the two placeholders

#### 10.1 Ordinary monster kills (`chunk_actor.hpp:1349-1352`'s replacement)

One `loot.<kind>` document per Monster-faction `CreatureKind` (Slime/Spider/Ghost/Skull — the four
`stats_of()` already enumerates, §Grounding), each a modest table of raw material scaled by the killing
creature's ring (§9), following the §7 worked example above. **Wildlife (`Faction::kWild`) is
unchanged** — the existing `GrantItems{kProduce,1}` grant (`chunk_actor.hpp`'s own "wildlife is food"
comment) is cited, not touched; this RFC's tables apply only to the `Faction::kMonster` branch `strike()`
already isolates.

Return type of `LootTable::roll()` (§8), shared by both replaced call sites:

```cpp
struct RewardBundle {
    struct ItemRow { ItemKind kind; std::int32_t count; };
    std::vector<ItemRow>          items;       // §7's `entries` rows that hit — ore and gems alike
    std::int32_t                  essence = 0; // §7's `essence_pm` roll; caller still applies §6.6's gate
    std::optional<EquippedItem>   equipment;   // NEW — only ever populated by a boss's `kind: "equipment"`
                                                // row (§10.2); always empty for the monster tables below,
                                                // which define no such row
};
```

`strike()`'s general branch (§Grounding) gains one new call after its existing `GrantXp` line:

```cpp
if (st.faction == Faction::kMonster) {
    RewardBundle b = loot_table_of(c.kind).roll(roll_seed_of(c, world_tick_, player.account_id));   // §7-§9
    for (const auto& row : b.items) grant(player, GrantItems{row.kind, row.count});
    if (b.essence > 0 && realm_allows_essence())                                 // §6.6's gate
        grant(player, GrantItems{ItemKind::kEssence, b.essence});
}
```

(`Faction::kWild`'s existing branch is untouched, shown for contrast only — not a code change.)

#### 10.2 Boss kills (`chunk_actor.hpp:1332-1334`'s replacement)

The flat `400 XP` stays exactly as shipped — RFC-019 §5.2 already records it as canon XP, and RFC-019
§5.8 already extended it to every qualifying co-op contributor via its ledger walk, neither renegotiated
here. **The `10 produce` placeholder is replaced** by a real `loot.boss_<name>` table (one per boss —
today, exactly one: the dojo Giant Red Samurai, RFC-005), structured like any other `loot.*` document
but with three differences the format already supports without a schema change:

- A guaranteed row (`chance_pm: 1000`) for a modest stack of the ring-appropriate ore tier — a boss
  kill is never a *worse* material payout than an ordinary kill of the same ring.
- A meaningful `essence_pm` **(tunable, proposed 350‰ — see §10.3)**, since dojo/dungeon bosses are
  definitionally challenge-realm content (§6.6).
- One `kind: "equipment"` row type, new to this format, for a low-chance (**5–10‰ tunable**) direct
  drop of a finished `EquippedItem` (§4) at the boss's own tier — the one place this RFC lets a loot
  table hand out gear directly rather than only its raw material, matching the "boss kills are the best
  reward" framing already used in the Guide-level section. The crafting chain (raw ore → forge → gear)
  remains the primary path (ROADMAP.md P4's own stated Done-when criterion); this is a rare
  supplementary path, not a replacement for it. Row shape (extends §7's schema; `gear` names a §4
  `gear.*` document, which already carries its own slot):

  ```json
  {"kind": "equipment", "gear": "gear.crimson_katana", "chance_pm": 8, "tier": "mythril"}
  ```

**Equipment-row delivery — the one new message this RFC adds.** A rolled `EquippedItem` is neither a
stackable count nor XP, so `GrantItems`/`GrantXp` (§Grounding) cannot carry it. This RFC adds one small
third message, over the same trusted chunk→`PlayerActor` hand-off `grant()` already provides for the
other two — not a new trust boundary, just a new payload shape on the existing one:

```cpp
// NEW — protocol.hpp addition, this RFC's own. Same trust boundary as GrantItems/GrantXp: an untrusted-OK
// chunk emits it, the trusted PlayerActor is the sole writer of equipped_[] that applies it.
struct GrantEquipment { EquipSlot slot; EquippedItem item; };
```

**Occupied-slot rule.** `equipped_[]` (§4) holds exactly one item per slot, never a spare-gear bag, so a
rare boss drop needs a rule for what happens when the slot is already full:

- If the drop's `tier` is strictly higher than what is currently equipped in that slot, it **auto-equips**,
  replacing the worn item outright — no prompt, no "keep or discard" dialog mid-fight, the same
  "never stall the player on a choice" posture this batch already commits to (Tone Guardrail Compliance §1).
- Otherwise (same tier or lower), the drop **does not equip** — it converts to a guaranteed stack of
  its own material tier's ore (§3), `qty = 3` **(tunable)**, delivered as an ordinary `GrantItems`, so
  the drop is never silently discarded and the player is never asked to decide anything.

```cpp
if (b.equipment) {
    EquipSlot slot = slot_of(b.equipment->item_id);                 // gear.* doc names its own slot (§4)
    if (b.equipment->tier > equipped_[slot].tier)
        grant(player, GrantEquipment{slot, *b.equipment});           // auto-upgrade
    else
        grant(player, GrantItems{ore_kind_of(b.equipment->tier), 3}); // refund, never discarded
}
```

This resolves once, synchronously, at the same point `grant()` already resolves an ordinary `GrantItems`
call (Multiplayer & Simulation-LOD Considerations, unchanged) — no new persisted state and no new
cross-actor round trip.

**Multiplayer distribution — every qualifying contributor rolls independently.** RFC-019 §5.8 already
ruled that the boss's flat 400 XP is shared with every qualifying contributor from its ledger
("abundance, not a split pool... dividing a kill's XP among contributors would turn 'bring a friend'
into a net loss for both of you") and explicitly deferred the *item* column of that same kill to this
RFC ("Item/loot distribution... is entirely RFC-018's table; this rule governs the XP column only").
**This RFC extends the identical abundance reasoning to loot**: every account in RFC-019 §5.8's
`Contribution` ledger that qualifies (landed a hit within `kAssistWindowTicks`) receives its own,
independent roll against the boss's `loot.boss_<name>` table — not a shared pool split N ways. This is
mechanically real, not just asserted: §8's seed formula folds `contributor_account_id` into the mix, so
resolving the ledger walk once per contributor (same creature, same spawn id, same kill tick, different
account each pass) produces a different `roll_seed` — and therefore a genuinely independent roll — per
contributor, not N copies of one shared outcome. A 3-player boss kill can yield three different sets of
drops, or three copies of the same rare row; nobody's presence reduces anybody else's odds. The
equipment-row chance (5–10‰) is calibrated low enough that "everybody rolls" does not make boss gear
common — the guaranteed-material row is what abundance is generous with; the rare equipment row stays
rare regardless of party size, by design.

#### 10.3 Essence acquisition rate — closing RFC-019's Open Question 3

RFC-019 §5.7 needs an Essence amount to size its capstone gate ("3 total") against. This RFC's first
proposal **(tunable, unvalidated by playtesting)**:

| Source | Essence **(tunable)** |
|---|---|
| Ordinary challenge-realm Monster kill | `essence_pm = 150` (15% chance) of 1 Essence |
| Boss kill (challenge-realm) | `essence_pm = 350` (35% chance) of 1–2 Essence |

At these rates, clearing a modest challenge-realm room (roughly 6–10 ordinary kills — **this RFC's own
unvalidated assumption, not sourced from any other RFC**: RFC-014 supplies party-size sizing only
("dungeons sized 2–4", RFC-014 lines 193/539) and no per-room monster-count guidance exists anywhere in
that document) plus its boss averages on the order of **2–3 Essence per run** — meaning RFC-019's
"3 total" capstone cost is roughly **one dungeon clear per branch**, not a grind and not free. This is a
starting proposal for RFC-019's own Open Question 3 to accept, adjust, or reject once both numbers exist
together; neither RFC commits the other to it, and the 6–10 figure specifically should be re-derived once
RFC-014 (or whichever RFC ends up owning room/encounter density) actually specifies it.

### 11. Mine loot (name-only — mines are P4 territory this RFC does not own)

Mines are RFC-014's instance mechanics and RFC-021 §3.6's placement, neither owned here. This RFC notes
only that a mine floor's ore yield should draw from the *same* `MaterialTier` table (§3) a monster kill
does, so the two sources stay numerically consistent (a Mythril ore stack means the same thing whether
it came from a Wasteland Skull or a depth-4 mine shaft) — a citation for whichever future RFC specifies
mine-floor extraction mechanics, not a table this RFC authors itself.

### 12. Reward-source recap: what's rare-gated, and why

| Reward | Overworld Monster kill | Mine (out of scope) | Challenge-realm Monster kill | Challenge-realm boss kill |
|---|---|---|---|---|
| Ore (tiered by ring/depth) | yes | yes (primary source) | yes | yes (guaranteed) |
| Socket gems | **no** | no | yes, low chance | yes, higher chance |
| Essence | **no** | **no** | yes, modest chance | yes, higher chance |
| Finished equipment | no | no | no | yes, rare |

This table is the mechanical expression of GAME.md §1's own axis assignment (Đá = mines and monsters
both feed the material axis; Tinh chất = challenge realms only) — nothing in this RFC lets an overworld
kill produce gems or Essence, by construction of §7's `realm_gate` authoring guard and §6.6's runtime
check, not by convention alone.

### 13. What this RFC needs from RFC-014/RFC-022, flagged not assumed

RFC-022's `MapDescriptor` (§1.3 of that RFC) carries `id`, `category`, `chunk_edge`, `biome`,
`weather_mode`, `weather_fixed`, `allow_free_build` — **no `RealmType` or `PortalKind` field** (verified
by direct read of RFC-022 §1.3 and §2.2; `RealmType`/`RealmFlavor` exist only on `Portal`, "meaningful
only when `kind == kRealmGate`," and are never copied onto the `MapDescriptor` of the instance that
portal ultimately allocates). This RFC's §6.1/§6.6 Essence gate and §7's `realm_gate` authoring guard
both need a chunk actor on an instanced map to know, **locally, from its own already-primed state**
(matching RFC-014 §3.2's Option 2 priming pattern, which already passes a `descriptor` at
`PrimeInstanceChunk` time), whether the map it is running on originated from a `kRealmGate` portal with
`RealmType::kChallenge`.

**Requested addition (not this RFC's struct to unilaterally amend):**

```cpp
// Proposed addition to RFC-022's MapDescriptor — meaningful only when category == kInstanced.
PortalKind origin_kind;        // kRealmGate, kMineMouth, kMissionPortal, ... (RFC-022 §2.2)
RealmType  origin_realm_type;  // meaningful only when origin_kind == kRealmGate
```

Populated once, at `allocate_new()` time (RFC-014 §3), copied from the triggering `Portal` — the same
moment `MapSession.origin_portal` (RFC-022 §2.3) is already recorded, so no new lookup is introduced,
only two more fields on a struct that is already constructed at that exact call site. This RFC treats
this as a **named, minimal, cross-RFC request**, not a silent assumption — flagged again in Open
Questions §2 and Interactions, for RFC-014's or RFC-022's own editor to accept, adjust, or reroute.
Until this field exists, §6.1/§10.1's Essence/gem gate has no data to read and this RFC's own
recommendation is to fail closed (grant nothing) rather than guess.

---

## Interactions with Other RFCs

| RFC | Boundary |
|---|---|
| **RFC-001/RFC-009** (accepted) | `DamagePacket`, the damage formula, and ability emission are untouched. This RFC supplies three new attacker/defender-side input numbers (`tier_damage_pm`, `tier_dr_bonus`, `tier_toughness_bonus`, §3 — the latter two fill `DefenderSheet.dr[0]`/`.toughness`'s named-but-previously-unsourced "gear" slots) and one new buildup-rider source (socket gems, §5) that compose at the same points RFC-019's `skill_scale` already does — never a new pipeline stage, never a modified struct. §3 also flags RFC-009's three stale `RFC-004` gear citations (RFC-009 lines 432, 457, 587) for that RFC's editor to retarget to this one. |
| **RFC-002/RFC-008** (accepted) | Socket gems are a direct, id-referenced consumer of the closed six-document status set (§7.4) — no seventh gem, no new status. The `loot` document domain (§7) follows RFC-008's directory/id/hashing conventions exactly, adding one domain to a pattern that already supports growth this way. |
| **RFC-013** (accepted-with-revisions) | This RFC extends `items_[]` (new `ItemKind` ordinals, §1) and adds `equipped_[]` (§4) — both new carried state RFC-013's ejection rule (§3.6.5, "clears `items_[]` to zero, in full") already applies unmodified to the stackable additions. **Equipped gear is explicitly not cleared by ejection** — RFC-013 §Open Questions #2 already flagged "when an equipment system ships, the ejection rule needs revisiting to exempt worn gear the same way it already exempts XP"; this RFC is that system, and this RFC's own position is that `equipped_[]` should be exempt from ejection's wipe (worn, not carried, the same distinction RFC-013's own flag anticipated) — RFC-013's editor, not this RFC, owns re-opening that ruling. |
| **RFC-014** (accepted-with-revisions) | Owns mine-depth instance mechanics and all instance allocation; this RFC only names one small, explicitly-flagged data need at `allocate_new()` time (§13) and does not specify mine-floor extraction itself (§11). |
| **RFC-016** (accepted-with-revisions) | Will need new persistence for `equipped_[]` (a small fixed-size struct per account, naturally a new table mirroring RFC-016 §4.1's existing `player_items` pattern) and the new `ItemKind` ordinals (already compatible with the existing generic `player_items` schema, §1). This RFC specifies the runtime shape; RFC-016 specifies the on-disk encoding, unchanged from that RFC's own stated division of labor. |
| **RFC-019** (accepted-with-revisions) | Consumes this RFC's §10.3 Essence-rate proposal to close its own Open Question 3. This RFC's `tier_damage_pm`/`tier_dr_bonus` are the material-tier axis RFC-019 §5.7 already named as orthogonal to skill points and deferred to "RFC-018 (proposed)" — delivered here. RFC-019 §5.8's co-op ledger is reused verbatim (§10.2) for loot distribution, not re-specified. |
| **RFC-020** (accepted-with-revisions) | Its `item_table` reward kind resolves directly against this RFC's `loot.*` documents (§7) via the `roll(ref, recipient, share_pm)` contract that RFC's QV13/§7.2 already named; `share_pm` is consumed as a weighting hint on `qty_max`/`chance_pm` for a party-shared quest reward, distinct from §10.2's per-contributor-rolls-independently rule (quests and kills are different reward events with different fairness rules, by RFC-020's own design). |
| **RFC-021** (accepted-with-revisions, partially superseded by RFC-022) | §3 and §9 consume its ring geometry (§2.1) and ring→ore-tier correlation (§3.6) verbatim, cited not re-derived — this RFC is the "crafting/equipment material-tier table" RFC-021 §3.6 explicitly deferred. |
| **RFC-022** (accepted-with-revisions) | `MapId`, `MapDescriptor`, `Portal`, `RealmType`, `PortalKind` cited. §13 names one small field addition this RFC needs and does not itself own — flagged, not assumed. |
| **RFC-005** (accepted-with-revisions) | Its non-goals named "loot tables, Essence rewards, dungeon economy" as future work; this RFC is that future work for the one boss RFC-005 authors (the dojo Giant Red Samurai). This RFC does not touch boss AI, telegraph, or kit authoring. |
| **RFC-017** (sibling, this batch) | This RFC's deterministic roll seed (§8) is the property RFC-017's sweep/replay tooling would rely on to reproduce a specific drop for balance testing; this RFC does not itself specify a test harness. |

---

## Multiplayer & Simulation-LOD Considerations

- **Rolls happen on the same actor and at the same moment `strike()` already resolves a kill** — no
  new message class, no new cross-actor call. `roll_seed_of()` (§8) reads only state already local to
  the chunk (`world_seed_`, the creature's own shipped `id`, the current tick, and the account id of the
  contributor already known from the packet being resolved) — the identical "reads
  only its own local state" discipline RFC-013 §3.6.1 and RFC-021's ring lookups already establish.
- **Reward delivery reuses the existing `grant()` hand-off** (`chunk_actor.hpp:1429-1432`) unmodified —
  `GrantItems` and this RFC's new `GrantEquipment` (§10.2) both flow from an untrusted-OK chunk to the
  trusted `PlayerActor`, which remains the sole writer of `items_[]`/`equipped_[]`, exactly the trust
  split RFC-019 §"Multiplayer" already states for `GrantXp`. A compromised chunk-hosting node could already forge a `GrantItems` message before this
  RFC existed; this RFC introduces no new attack surface, and — per the hard constraint this whole batch
  operates under (ARCHITECTURE.md §0 S1: "kick is enough... `Require<Trusted>` [is] a correctness
  mechanism, not a defense") — this RFC does not attempt to close that surface, matching the project's
  explicit, deliberate posture.
- **LOD-tolerant by the same construction as RFC-019's XP grants.** A roll fires once, synchronously, at
  the moment of a kill — there is no "loot accrual per tick" to reconcile against a chunk ticking at
  1 Hz or asleep. A creature fought inside a slow chunk simply takes longer in wall-clock time to kill;
  the roll at death is identical either way.
- **Boss co-op rolls (§10.2) are bounded by RFC-019 §5.8's existing `kMaxContributors = 4`** — at most 4
  independent rolls per boss kill, the same fan-out bound RFC-019 already argued is well inside existing
  per-chunk message budgets.
- **Equipped-item state (`equipped_[]`, §4) is two fixed-size structs per player** (`EquipSlot::kCount
  = 2`), not a growing collection — no per-player memory growth beyond what RFC-019's own contribution
  ledger already establishes as an acceptable, bounded, per-actor cost at 20–50 concurrent players.
- **Cross-platform determinism** for the roll seed function (§8) follows the same discipline ARCHITECTURE
  §2c already requires of every other seed-derived value in this project — an integer-only mixer over
  integer inputs, no floating point anywhere in the roll path.

---

## Tone Guardrail Compliance

1. **Nothing in this RFC decays.** Ore, gems, Essence, and equipped items are all monotonic-until-
   player-action state — durability only moves in response to the player's own combat actions (§4.1),
   never on a timer; Essence never expires (§6, rule 2); a loot table has no "log in today or miss the
   drop" clause, because a kill's roll happens once, at the moment of the kill, and is either received
   or (on a miss chance) simply not received — never queued behind a countdown.
2. **A broken weapon is never a lockout.** §4.1's durability-interpolation design was chosen specifically
   to avoid the shape GAME.md §0 forbids: a weapon at 0 durability keeps working, just at a lower
   effective tier, floored at the item's own base tier — never zero, never "you cannot fight until you
   repair this." This mirrors RFC-013 §3's explicit rejection of a "downed" state on the same tone
   ground.
3. **Essence's realm-only gate (§6) is opt-in, not a penalty.** A player who never sets foot in a
   challenge realm simply never acquires Essence — exactly the same "chill player pays nothing, opt-in
   player gets a real reward" framing RFC-019 §5.7 already argues for the capstone gate this currency
   feeds; this RFC does not add urgency to that choice, only supply.
4. **Boss loot's rare equipment row (§10.2) has no pity timer, no bad-luck protection counter, and no
   guaranteed-after-N-kills clause.** A flat, static chance every time is the "no clock, no schedule"
   reading of a rare drop; a pity-timer counter would be exactly the kind of hidden countdown-toward-a-
   reward GAME.md §0 rules out, even though it would read as generous — the guardrail is about mechanic
   shape, not generosity.
5. **Co-op loot abundance (§10.2) avoids the same competitive pressure RFC-019 §5.8 already argued
   against for XP.** Every qualifying contributor rolling independently, rather than splitting a fixed
   pool, means partying never costs a player expected loot relative to soloing — a design that could
   pressure players toward soloing content GAME.md §11 explicitly designs for groups would itself be a
   tone violation by that RFC's own standard, restated here for the item column.

No mechanic in this RFC reads real-world or world-clock time, decays a player-visible resource, or
creates a reason to log in on a schedule.

---

## Open Questions

1. **Two equip slots vs. a fuller paperdoll (§2).** Chosen for minimalism against a zero art budget;
   flagged for revisiting once P4 playtesting shows whether "weapon + armor" reads as too coarse for
   the "trang bị đó tạo khác biệt đo được trong chiến đấu" (gear makes a measurable difference)
   Done-when bar ROADMAP.md P4 sets.
2. **The `MapDescriptor.origin_kind`/`origin_realm_type` field request (§13).** This RFC names the need
   precisely but does not own RFC-022's struct; whether RFC-014 or RFC-022's own editor accepts this
   addition, reroutes it through a different mechanism (e.g., a lookup table keyed by `MapId` instead
   of a field on the descriptor itself), or finds a reason to reject it is unresolved here.
3. **Repair mechanics.** Named as a hook (§4.1) exactly the way RFC-013 §8 named `GrantVitals` for food
   healing, but not designed — cost, station requirement, and whether it is instant or takes time are
   all P4 crafting-system questions this RFC does not answer.
4. **Essence acquisition numbers (§10.3) are a first proposal with no playtesting behind them**, exactly
   like RFC-019's own numbers were when first proposed. Both RFCs' numbers need to be tuned together,
   not independently, once real dungeon clear times exist (RFC-017's sibling territory).
5. **Roll-seed mixer choice (§8).** `splitmix64` is a placeholder for "any fixed, dependency-free,
   integer-only 64-bit mixer" — the exact function is an implementation detail with no design
   consequence, flagged only so an implementer doesn't read the name as a hard requirement.
6. **Gem grade distribution across loot tables.** §5 fixes four grades (Minor/Lesser/Greater/Major) and
   their `amount`/`ticks` values, but not which grade appears in which `loot.*` document at which
   ring — that is per-table content authoring, not a rule this RFC pins.
7. **Equipment ejection exemption (Interactions, RFC-013 row).** This RFC states its own position
   (worn gear should be exempt from RFC-013's carried-item wipe) but does not have the authority to
   amend RFC-013's accepted text — flagged for RFC-013's editor to formally rule on.
8. **Should mine-floor ore yield (§11) share this RFC's exact `MaterialTier` table, or does mining
   deserve its own, richer yield curve** (e.g., higher volume per unit time than a monster kill, given
   mining is described as the "primary" source in ROADMAP.md P4)? Left to whichever future RFC owns
   mine-floor extraction; this RFC only asks that the tier *definitions* stay shared.

---

## Non-goals

- **Crafting UI, recipes, and the forge/workbench workflow.** ROADMAP.md P4's own territory, unowned by
  any RFC in this batch. This RFC defines the material this workflow will consume (ore, tiers) and the
  shape it will produce (`EquippedItem`), never the recipes, station requirements, or UI.
- **The socketing and repair verbs themselves.** Named as hooks this RFC's data shape must support
  (§4.1, §5), not designed — same "name the hook, not the system" posture RFC-013 §8 already set as
  precedent for food healing.
- **Persistence/save-file schema.** RFC-016's, cited not specified — this RFC names the runtime fields
  that will need a home (§1, §4) and leaves the encoding to that RFC.
- **Boss AI, telegraph design, and kit authoring.** RFC-005's, entirely untouched — this RFC only
  replaces what the one shipped boss's kill *pays out*.
- **Mine-floor placement, depth mechanics, and instance allocation.** RFC-014's and RFC-021's; this RFC
  only asks that mine yield eventually share its `MaterialTier` definitions (§11, Open Questions §8).
- **The ring-adaptation unlock mechanism and fort-closing mechanic that will eventually spend
  Essence.** Named in GAME.md §4 but unowned by any RFC today (RFC-019 §5.7 already flagged this); this
  RFC defines Essence's acquisition only, not either future spend system.
- **Full paperdoll itemization (helm/chest/boots/rings/etc.).** Deliberately scoped to two slots (§2);
  a future revision's territory if ever needed.
- **PvP loot implications.** PvP is off by default (GAME.md §11); no number in this RFC was chosen with
  PvP in mind.
- **A balance-tuning process or test harness for any number in this RFC.** RFC-017's (sibling), which
  this RFC's deterministic roll seed (§8) is written to support, not replace.

---

## Review Record

Reviewer A: revise. Reviewer B: revise. Both mustFix lists converged on two self-contradictions plus four completeness gaps; all applied below.

- Applied: §8 roll seed now folds `contributor_account_id` into the mix; determinism claim restated as per-(creature,tick,player), resolving the independent-rolls contradiction with §10.2.
- Applied: gem grade moved into `ItemKind` itself — 24 `kGem<Channel><Grade>` ordinals (§1) — so grade survives a stackable `GrantItems` grant; §4/§5 updated to match.
- Applied: added `RewardBundle.equipment`, a new `GrantEquipment{EquipSlot,EquippedItem}` message (§10.2), a worked JSON row, and an occupied-slot auto-upgrade-or-refund rule; header grounding corrected to say "adds a third" message.
- Applied: RFC-016 status citations (header, Interactions) corrected from "proposed, drafted separately" to "accepted-with-revisions".
- Applied: §10.3's "6–10 kills" figure relabeled as this RFC's own unvalidated assumption; RFC-014 misattribution removed.
- Applied: §3 gained `tier_toughness_bonus` and named exactly which `DefenderSheet.dr[]`/`.toughness` slots it and `tier_dr_bonus` fill; RFC-009's three stale `RFC-004` gear citations (lines 432, 457, 587) flagged for that RFC's editor to retarget.
- Status changed Draft → Accepted (revised after review), per instructions (not an approve vote).
- Unresolved: none of the two reviewers' mustFix items were rejected — all six converged items were applied. Minor/single-reviewer notes (item./gem. RFC-008 id-domain gap, Essence-vs-ejection asymmetry, QV13/§7.2 mislocation) were not mustFix and are left for a future pass.
