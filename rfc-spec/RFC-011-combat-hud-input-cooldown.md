# RFC-011: Combat HUD, Input & Cooldown UI

> **Status: Accepted (revised after review)**
> Design canon: [GAME.md §12](../GAME.md) (Giao diện — the minimal in-game HUD list: "máu/mana/thể
> lực, thanh kỹ năng, đồng hồ ngày/mùa, cảnh báo đợt tấn công" [health/mana/stamina, ability bar,
> day/season clock, raid warning] — no enemy HUD element is on this list, a boundary this RFC must
> justify crossing, not ignore; the Character (`C`)/Options screens table; "Công cụ: raygui... +
> Kenney UI Pack RPG Expansion... đã tải" — the toolkit this RFC assumes, no new art) ·
> [ARCHITECTURE.md §2](../ARCHITECTURE.md) (leader-death/Trusted framing, cited only where it bears
> on server-authoritative input handling; not otherwise this RFC's concern) · GAME.md §0 (chill
> guardrail — nothing counts down behind the player)
> As-built source grounding: `src/ui/screens.hpp:26-34` (`Screen` enum: `kLogin, kMainMenu,
> kPlaying, kPaused, kJournal, kCharacter, kOptions` — **no `kMap` in this file**; the Map screen is
> tracked elsewhere, RFC-022/RFC-021 territory, untouched here), `src/ui/screens.cpp:198-246`
> (`bar()` the shared bar-drawing primitive; `draw_ability_slots()` — the two-slot HUD, the
> Disabled-icon "greyed twin" convention at the `draw_ui_icon(..., disabled, ...)` call, and the
> cooldown-wipe rectangle computed from `player.ability_cd[i]` vs `def.cooldown`), `screens.cpp:
> 318-332` (`draw_hud` — the three vitals bars: HP red, stamina green, mana blue, bottom-left, own
> player only), `screens.cpp:821` ("Key rebinding is still owed — it lands with the input pass." —
> the debt this RFC pays down), `screens.cpp:546-571` (the shipped screen-toggle keys: `F3` debug
> overlay, `ESC` pause/back, `J` journal, `C` character), `src/render/raylib_bridge.cpp:1800-1876`
> (the full shipped hardcoded input map this RFC's rebind table is seeded from: WASD/arrows move,
> `B` build toggle, `1..4` context-sensitive build-kind/element slot, `R` mount, `E`/middle-mouse
> harvest, `T` till, `U` upgrade, `Shift` heavy-attack modifier, `Q`/`Space` shoot, `F`/`G` ability
> slots), `src/render/raylib_bridge.cpp:404-411` + `src/render/atlas_slots.hpp:312-351` (`draw_ui_icon`,
> `icon_rect(Icon, disabled)` — confirms the "greyed twin" is a **pre-baked second icon packed one
> atlas cell to the right**, not a runtime tint; exactly 6 `Icon` entries exist today, one per shipped
> `AbilityId`), `src/world/tiles.hpp:487-494` (`Creature{id,x,y,hp,max_hp,damage,kind,...}` — the
> comment "`max_hp` already ring-scaled, so a health bar means something" is the shipped struct
> anticipating exactly this RFC), `src/world/tiles.hpp:300-323` (`CreatureKind::kBoss = 9` — the
> wire-visible discriminator this RFC uses to detect a boss without inventing a targeting concept),
> `src/world/abilities.hpp:22-137` (`AbilityId` — 6 entries; `kAbilitySlots = 2`; `AbilityDef`;
> `ability_def()` — **note:** `ability_def(AbilityId::kCount)` falls through its `switch` and
> recurses to `ability_def(AbilityId::kWhirlCleave)`, §5.4 explains why this is a live landmine this
> RFC's loadout picker must defuse; `equipped_ability(levels, slot)` — always resolves to a concrete
> `AbilityId`, never `kCount`, under the shipped auto-pick, comment: "the array shape is ready for
> player-chosen loadouts; only the picker is deferred"), `src/world/player_actor.hpp:297-298,382-383`
> (`equipped_ability(level_, slot)` — auto-pick call sites), `src/render/raylib_bridge.cpp:673-704`
> (`draw_boss_bar()` — the **already-shipped** boss HP bar: `Boss::kFace` portrait, red fill bar,
> hardcoded `DrawText("DOJO MASTER", ...)` name label, `hp / max_hp` readout; called unconditionally
> from `RaylibBridge::draw` at `raylib_bridge.cpp:1773`), `raylib_bridge.cpp:1305-1313` (`frame_boss`
> — populated whenever a `CreatureKind::kBoss` creature shares the player's active room,
> `player.map != kOverworld && room_index_at(...) == im.active_room`; a same-room trigger, not a
> chunk-interest-set one — §3 corrects itself against this), `src/client_main.cpp:29-67`
> (`kClientCfgPath = "client.cfg"` — the local, gitignored, plain-text, tolerant-parsing,
> defaults-on-missing settings file this RFC's key-bind storage extends verbatim, not reinvents)
> Depends on: RFC-001 (accepted — the two-slot kit, cooldowns, admission/`kLocked` rule this HUD
> reads, not re-specifies), RFC-002 (accepted — status ladders, coatings, and Open Question 4, which
> §5.2 of this RFC answers), RFC-015 (accepted — the client-side wire data source this RFC's HUD
> reads: `PublishedCreature`, `PublishedPlayerSelf`), RFC-016 (accepted — the reserved
> `equipped_ability_0`/`equipped_ability_1` save columns this RFC's loadout picker writes into),
> RFC-019 (accepted — skill levels/unlock tiers gating what a player may equip, and §5.4/§5.6's
> explicit hand-off of the manual-loadout decision to this RFC), RFC-005/RFC-006 (accepted — boss
> kits and the telegraph/tint vocabulary this RFC's status-pip indicator rides on top of, never
> redefines)
> Depended on by: none yet chartered

---

## Summary

This RFC does two things. First, it takes the combat HUD and cooldown UI that already ship —
three player-vitals bars, the two-slot ability display with its Disabled-icon "greyed twin"
convention and cooldown-wipe rectangle — and writes them down as a **normative contract**: the
exact data they read, the exact rule that drives each visual state, and the exact boundary with
RFC-006 (which draws the *world*) and RFC-015 (which delivers the *data*). That normative-baseline
treatment now also covers a fourth already-shipped element this audit initially missed and both
reviewers caught: the boss HP bar (`raylib_bridge.cpp:678-704`'s `draw_boss_bar`, a portrait, red
fill bar, and hardcoded "DOJO MASTER" name label, already drawn every frame a boss shares the
player's room) — §3 formalizes it exactly as §1/§2 formalize the vitals bars and ability slots, and
scopes the genuinely open gap correctly instead of re-describing it as new. Second, it closes three
real gaps the shipped code and the accepted RFC set both name but nobody owns: RFC-002's Open
Question 4 (should ordinary creatures show status gauges, not just bosses), a manual two-slot
ability loadout picker (RFC-019 §5.4 explicitly defers this to "RFC-011's call"), and key rebinding
(`screens.cpp:821`'s own comment: "still owed").

Every new number here is marked **(tunable)**. Every claim that something already ships cites the
exact file and line it ships at. Where this RFC found the accepted set disagreeing with itself —
RFC-002's designed 2-byte status-replication budget vs. RFC-015's actually-frozen 1-byte
`PublishedCreature.status` field — it says so explicitly rather than assuming the wider budget
still exists.

---

## Motivation

1. **The HUD baseline shipped without a spec.** `draw_hud`, `draw_ability_slots`, and the
   Disabled-icon convention are real, working code — but no RFC says what data they are allowed to
   depend on, what "disabled" formally means, or what happens when RFC-015's wire protocol replaces
   the in-process reads they use today. RFC-006's own non-goals name this gap and decline to fill
   it ("the Disabled-icon cooldown convention is noted, not specified" — RFC-006 Non-goals).

2. **RFC-002 asked a question nobody answered.** Open Question 4: "should ordinary creatures show
   [status] gauges too, or is stage-tint enough? ... RFC-006 decides, but the replication budget
   here caps the option at the existing 2 bytes." RFC-006 never decided (it is a rendering RFC, not
   a HUD one) — the question fell through the boundary between two RFCs that each correctly said
   "not me."

3. **Two accepted RFCs both point at this one and wait.** RFC-015's own header names itself
   "Depended on by: RFC-011 ... needs a defined client-side data source before it can specify what
   the HUD reads" — that dependency is now satisfiable, RFC-015 having landed. RFC-016 reserved
   `equipped_ability_0`/`equipped_ability_1` columns, currently `NULL` and unread by any shipped
   code, specifically so that "landing RFC-011 later is a code change to WRITE this column, never a
   schema migration to ADD it" (RFC-016 §4.1). Both are debts this RFC pays.

4. **A boss fight's health bar shipped without a spec, the same gap as points 1-2 above.**
   `tiles.hpp::Creature`'s own comment says `max_hp` is "already ring-scaled, so a health bar means
   something," and `raylib_bridge.cpp:678-704`'s `draw_boss_bar` already draws exactly that bar —
   portrait, fill, name, readout — every frame a boss shares the player's room. What's missing is
   not the bar; it's a normative contract for it (trigger semantics, multi-boss behavior, what the
   hardcoded name means once a second boss kit ships) — the same documentation gap points 1-2
   describe for the vitals bars and ability slots, now closed for this element too.

5. **"Key rebinding is still owed" is a direct quote from the shipped code's own Options screen.**
   It is not a nice-to-have; it is a stub with a comment pointing at this RFC by description ("it
   lands with the input pass").

---

## Guide-level Explanation

### For a player

Nothing about the vitals bars or the two ability slots changes in feel — they are the same three
bars and two slots you already see, now written down so every future RFC that touches them has to
agree with this one instead of guessing. Two new things appear:

- **Fighting a boss, a bar appears across the top of the screen** showing its health. It disappears
  when no boss is in view. It is not there for ordinary monsters — a slime still just visibly
  flinches and tints when hurt, exactly as today.
- **Hurt monsters near a status threshold show small pips**, not just a color tint — a slime one hit
  from freezing shows a faint 2-of-3 pip readout above it, using data the game already sends for
  every creature (RFC-002's stage byte), not new network traffic.

Open the Character screen (`C`) and, once you have at least one unlocked ability beyond the
game's automatic pick, a new "Loadout" panel lets you choose freely which two abilities sit in your
F/G slots — cross-branch, no cost, no cooldown to swap, only usable when you are out of combat. If
you never open it, nothing changes: the game keeps auto-picking for you, forever, exactly as today.

Open Options and Key Bindings finally works: click a bindable action, press the key you want, done.
Movement's arrow-key fallback and Escape always work no matter what you rebind, so you can never
lock yourself out.

### For a designer

The HUD reads a fixed, small set of fields from `PlayerView`/`PublishedCreature` (§2, §3) and
derives everything else — locked/poor/cooling, boss-in-view, stage pips — from data already
replicated for other reasons. No content author ever touches this RFC; it has no JSON schema and no
`sounds.json`/`fx.json`-style authored table (contrast RFC-006/RFC-012).

### For an engineer implementing this RFC

Nothing here proposes a new wire message class beyond the one loadout-set intent (§5.3) and the
already-`(proposed)` `PublishedCreature`/`PublishedPlayerSelf` RFC-015 defines. Nothing here
proposes new art — every new visual (§3's boss bar, §5.2's stage pips) is built from the existing
`bar()` primitive and the existing `Icon`/atlas convention, or from plain rectangles matching the
cooldown-wipe idiom already in `draw_ability_slots`.

---

## Reference-level Design

### 1. Player vitals bars — normative baseline (already shipped)

`screens.cpp::draw_hud` (lines 318–332) is confirmed and made normative, unchanged:

- Three bars, bottom-left: HP (red, `Color{196,72,72,255}`), stamina (green,
  `Color{120,190,96,255}`), mana (blue, `Color{96,140,220,255}`), each a fraction of
  `player.hp/max_hp`, `player.stamina/kPlayerMaxStamina`, `player.mana/kPlayerMaxMana` drawn through
  the shared `bar()` helper (`screens.cpp:198-203`).
- Own player only — no remote player ever gets a vitals-bar readout under this RFC (§Non-goals).
- **This RFC does not touch what hp/stamina/mana regen mean.** Their semantics — unconditional
  stamina/mana regen, out-of-combat-only HP regen — are RFC-013 §1's confirmed normative baseline.
  This RFC only formalizes that the HUD's job is to draw the fraction it is given, never to compute
  vitals itself.

Data source: `PlayerView.hp/max_hp/stamina/mana` (own session slot only). Under RFC-015's wire
contract this is the narrowed `PublishedPlayerSelf` projection (RFC-015 §2.5); today, pre-network,
it is the full in-process `PlayerView` the renderer already reads directly (RFC-015's own header
confirms "no client-facing wire/network protocol exists in `src/` today ... the renderer reads
`SnapshotBus`/`PlayerBus` directly, in-process, on the same machine"). Both are the same fields; this
RFC does not care which transport carries them.

### 2. Two-slot ability display — normative baseline (already shipped)

`draw_ability_slots` (`screens.cpp:208-246`) is confirmed and made normative:

- Exactly `kAbilitySlots = 2` slots (`abilities.hpp:37`), keyed `F` and `G` (fixed labels, matching
  the fixed shipped keys — rebindable under §6, in which case the drawn label must track the bound
  key, not stay hardcoded `"F"`/`"G"` text; §6.4).
- **Disabled-icon convention (formalized).** A slot is `disabled` iff any of three conditions holds
  — `locked` (`skill_level[school] < unlock_level`), `poor` (insufficient stamina/mana for `cost`),
  or `cooling` (`ability_cd[i] > 0`). `disabled` selects the **pre-baked greyed-twin icon**, packed
  one atlas cell to the right of the lit icon (`atlas_slots.hpp:347-351`) — never a runtime alpha or
  tint shader. This RFC's contract: **any future ability added to the game must ship both a lit and
  a greyed-twin 24×24 icon cell**, exactly matching the pattern all 6 shipped abilities already
  follow (`atlas_slots.hpp:337-344`). A lit-only icon is a content bug under this RFC, the same class
  of bug as a missing sound file under RFC-012.
- **Cooldown-wipe rectangle (formalized).** While `cooling`, a dark panel (`Color{10,12,18,170}`)
  covers the top of the icon, height = `cd/cooldown` fraction of the slot, emptying downward as the
  cooldown drains (`screens.cpp:230-234`). This is a **fraction of a known, server-published integer
  countdown the player is fighting toward being allowed to act again** — not a hidden clock; see
  §Tone Guardrail Compliance point 2 for why this does not violate GAME.md §0.
- Keybind glyph (top-left of the slot) and unlock-level requirement (bottom, while locked) are drawn
  from the same source (`screens.cpp:239-244`), formalized unchanged except the keybind glyph must
  track §6's rebind table (§6.4).

Data source: `PlayerView.ability[kAbilitySlots]`, `PlayerView.ability_cd[kAbilitySlots]`,
`PlayerView.skill_level[kSkillCount]`, all already present, plus the constexpr `ability_def()` table
(`abilities.hpp`) both client and server already share.

### 3. Boss HP bar — normative baseline (already shipped) plus the real remaining gap

**Correction from an earlier draft of this RFC.** An earlier pass of this section claimed no boss
HP bar existed anywhere in the client and specified one from scratch. That claim was false:
`raylib_bridge.cpp:678-704`'s `draw_boss_bar()` is a fully shipped, unconditionally-called function
(invoked every frame from `RaylibBridge::draw`, `raylib_bridge.cpp:1773`) that draws a top-center
room-fight banner — a 40 px `Boss::kFace` portrait, a dark trough with a red HP-fill bar and
highlight band, a hardcoded name label (`DrawText("DOJO MASTER", ...)`, line 703), and an
`hp / max_hp` text readout — fed by `frame_boss`, populated at `raylib_bridge.cpp:1305-1313`. This
RFC now treats it exactly the way §1/§2 treat the vitals bars and ability slots: confirmed and made
normative, corrected where its as-drafted description diverged from the shipped behavior, gaps
scoped to what is genuinely still open.

**Scope, as shipped.** The bar renders for **bosses only** (`CreatureKind::kBoss`, `tiles.hpp:319`),
never for ordinary creatures. This RFC ratifies that scope rather than inventing it, for the same
three reasons a from-scratch design would have reached it: (a) GAME.md §12's minimal in-game HUD
list omits any enemy HUD element entirely, so extending numeric readouts to every slime would be a
bigger tone departure than this RFC is chartered to make; (b) RFC-005's design investment (pose
model, telegraph tiers, boss state machine) already singles bosses out as the encounters worth extra
legibility spend; (c) `hp`/`max_hp` remain visible per-creature as a tint/flinch cue (RFC-006) for
everything else, so trash mobs are not left mute, just not given a numeric readout.

**Trigger — corrected to match the shipped rule.** The bar appears while `frame_boss` is non-null:
a living (`hp > 0`) `CreatureKind::kBoss` creature that **shares the player's current room**
(`player.map != kOverworld && room_index_at(...) == im.active_room`, `raylib_bridge.cpp:1310-1312`).
This is a **same-room** trigger, not the chunk-interest-set/view trigger an earlier draft of this
section proposed — that proposal is withdrawn as an unacknowledged, unjustified behavior change
against shipped code, and RFC-005's dojo-room-scoped boss encounters are exactly why same-room is
the correct trigger: a boss fight is already contained to one room, so "in view" and "in the fight's
room" coincide by construction today. **At most one boss bar is shown at a time** (`frame_boss`
holds a single pointer, last-write-wins in the creature-iteration order if more than one boss shares
a room). No multi-boss encounter is authored today (RFC-005's shortlist), so this has no live test
case; revisiting the tie-break rule if one is ever authored is Open Questions §1, unchanged.

**Layout, as shipped (not re-specified as tunable).** Top-center, bar width `clamp(screen_w * 2/5,
320, 720)` px, height 20 px, `y = 16` px from the top edge, an 44 px portrait box to the left
(`raylib_bridge.cpp:680-684`). This sits clear of the day/season clock (top-right, GAME.md §12).
Drawn with the same `bar`-style dark-trough/fill/frame idiom the vitals bars use conceptually
(`screens.cpp:198-203`), red fill, no new drawing primitive introduced — though the call itself
lives in `raylib_bridge.cpp`'s world-draw side, not `screens.cpp`'s HUD shell, per the shipped
code's own comment distinguishing the two (`raylib_bridge.cpp:673-676`). This RFC's HUD data-and-
meaning contract governs the boss bar regardless of which translation unit renders it, the same way
it governs `draw_hud`/`draw_ability_slots` in `screens.cpp` — the boundary is about which file owns
the draw call, not which RFC owns the contract.

**Label — already a specific name, not a generic placeholder; the real gap is per-kit identity.**
The shipped bar already draws a proper name and portrait (`"DOJO MASTER"`, `Boss::kFace`) at zero
wire cost — an earlier draft's claim that v1 "must" ship a generic `"BOSS"` label because no
boss-kit identity is replicated is false as a description of the shipped behavior: the name is a
hardcoded client-side string, not something the wire needs to carry. It is correct today only
because exactly one boss sprite/name is shipped (`atlas_slots.hpp`'s `Boss` enum is pose frames for
one kit, not a roster). The **genuine remaining gap**: `PublishedCreature`/`Creature` carry only
`kind == CreatureKind::kBoss`, one value for every boss, so a second boss kit (e.g. RFC-005 §R1's
`GiantRedSamurai` vs. `GiantBlueSamurai` `sheet` field) would still draw "DOJO MASTER" — a
mislabeling bug, not a missing feature. This RFC does not invent a new wire field to fix that (RFC-
015's territory, a non-goal here, §Non-goals); it is recorded, updated from "no name exists" to "the
one hardcoded name won't scale past one boss kit," in Open Questions §2.

**Leash-reset handling.** RFC-013 §2 carves out `boss_reset()`'s full-HP reset as the one named
exception to "no creature regens HP by a tick-based rule." When this fires, the bar simply redraws
the new `hp/max_hp` fraction on the next published tick — a discrete jump, not an animated regen,
and therefore not a tone-guardrail concern (§Tone Guardrail Compliance point 4).

Data source: `Creature.hp/max_hp/kind` today (in-process, `tiles.hpp:487-494`); `PublishedCreature.
hp/max_hp/kind` (RFC-015 §2.2) once the wire protocol lands. Both already carry everything this bar
needs — **zero new replicated bytes**, confirming the remaining gap here is a labeling-scale
question, not a data one.

### 4. Ordinary-creature status gauge visibility — RFC-002 Open Question 4, answered

**The question, precisely.** RFC-002 §10: bystander creatures render tint/overlay from `stage`
alone; "a targeted boss shows its primary gauge." Open Question 4 asks whether *ordinary* creatures
should also get a gauge-style HUD readout, "capped at the existing 2 bytes."

**A discrepancy this RFC found and must confront rather than paper over.** RFC-002 §10 *designed* a
2-byte per-creature status budget: byte 1 = `primary`(3 bit) + `stage`(2 bit) + `coatings`(2 bit);
byte 2 = the primary gauge quantized to 5 bits (plus 3 spare), specifically so a "targeted boss"
could show a fine, continuously-filling meter. **RFC-015 §2.2 — accepted, frozen, the actual wire
struct — allocates exactly one byte to this** (`PublishedCreature.status`, commented "the one-slot
elemental state (RFC-002)"). The quantized-gauge byte RFC-002 reserved is not present in the frozen
`PublishedCreature`. This RFC's non-goal is "no wire protocol details" (§Non-goals) — it cannot
silently assume RFC-015 secretly carries a byte its own published struct does not have, and it
cannot add one itself. So the honest answer to Q4 has to be scoped to **what is actually on the
wire today**, not to RFC-002's original 2-byte design intent.

**Ruling.** Byte 1 — `primary`, `stage` (0–3), `coatings` — is already replicated for *every*
creature, bystander or boss, not gated behind a "targeted" concept (RFC-015 §2.2's `status` field
has no such gate). This RFC's answer: **yes, ordinary creatures show gauges too**, in the only form
the actually-replicated data supports — a **3-pip discrete ladder indicator** (one pip lit per
`stage` value, 0–3, so up to 3 pips) drawn above any on-screen hostile or neutral creature whose
`stage > 0`, tinted by `primary`'s element (reusing RFC-006's existing element palette, not a new
one). Pip size 6 px (tunable), gap 2 px (tunable), drawn only while `stage > 0` — a creature at
stage 0 shows nothing extra, exactly as today.

**What this ruling explicitly does NOT do.** It does not give bosses (or anyone) a fine, continuously
-filling gauge bar, because the byte that would drive one does not exist on the wire. A "targeted
boss shows its primary gauge" in the fine-grained sense RFC-002 §10 originally described is **not
implementable against RFC-015's frozen struct as it stands today** — flagged as Open Question 3, a
follow-up for whoever next revises RFC-015's wire shape, not solved here.

This stays inside the *actual* existing budget (1 byte, not the 2 RFC-002 designed for) at **zero
new replicated bytes** — the stage/primary/coatings byte is already sent for every creature in view
regardless of what this RFC decides.

### 5. Ability loadout picker (new)

**The decision this RFC is asked to make.** RFC-019 §5.4 states plainly: "manual vs. auto, free vs.
costed, cross-branch or not, is RFC-011's ... call," and frames the assumed future state as "a
player with all four tiers of Melee unlocked still carries exactly two abilities total, possibly
zero from Melee, at their own choice." This RFC adopts exactly that framing as the answer.

#### 5.1 The rule

- **Manual, free, instant, cross-branch.** A player may assign any ability their `skill_level`
  makes eligible (`skill_level[def.school] >= def.unlock_level`, the same test `draw_ability_slots`
  already runs for `locked`) to either slot F or G, regardless of which school it belongs to. No
  gold/Essence cost, no cooldown to swap.
- **Out-of-combat only (tunable gate).** A loadout change is only accepted while the player is
  out of combat — reusing the exact same flag RFC-013 §1's HP-regen rule already computes
  (`world_ms - last_hurt_ms > kCombatCooldownMs`), not a new timer. This sidesteps a balance question
  (instant elemental counter-swapping mid-fight) this RFC is not chartered to arbitrate — see
  §Non-goals and RFC-017.
- **No duplicate slot.** The same `AbilityId` may not occupy both F and G simultaneously (rejected
  client-side before send, and re-validated server-side, §5.3).
- **Opt-in, reversible, and defaults to today's behavior forever.** A player who never opens the
  picker keeps the shipped auto-pick (`equipped_ability()`) unchanged, permanently. The picker
  offers an explicit "Reset to Auto" action that clears the manual choice back to the auto-pick rule
  (§5.3's `NULL` semantics). This is the tone-guardrail argument for the whole feature: it adds
  optional depth, never a new mandatory screen (§Tone Guardrail Compliance point 5).

#### 5.2 Where it lives

The existing Character screen (`kCharacter`, opened with `C`), extending the read-only unlock-tier
list RFC-019 §5.10 already puts there into a clickable picker for eligible abilities. No new
`Screen` enum value is introduced.

#### 5.3 Wire and persistence

Client sends a `SetLoadout{slot: uint8, ability: AbilityId}` intent (named at the message-kind
level only — wire encoding is RFC-015's territory, a non-goal here, matching how RFC-019 and
others cite `protocol.hpp` message names without specifying bytes). `PlayerActor` (trusted,
server-authoritative) re-validates eligibility and the no-duplicate rule before accepting — the
client-side check in §5.1 is UX, not the security boundary. On acceptance, `PlayerActor` writes the
resolved `AbilityId` into the same in-memory field `ability[]` (`PlayerView`) already reads for
display, and the value round-trips into RFC-016's reserved `equipped_ability_0`/`equipped_ability_1`
columns (`RFC-016 §4.1`, currently `NULL`) via the same event-triggered + periodic persistence path
already specified for `level_*`/`xp_*`/`player_items` (RFC-016 §4.2). "Reset to Auto" writes `NULL`
back to both columns, which RFC-016's existing comment already documents as the "use the shipped
auto-pick" sentinel — no new column semantics, only the first code that actually writes to columns
RFC-016 left reserved for this exact purpose.

#### 5.4 A landmine this feature would otherwise walk into

`ability_def(AbilityId::kCount)` does not return a safe "empty" value — its `switch` falls through
`case AbilityId::kCount: break;` and **recurses to `ability_def(AbilityId::kWhirlCleave)`**
(`abilities.hpp:81-109`). Under the shipped auto-pick this is dead code: `equipped_ability()` always
resolves to one of the 6 concrete `AbilityId`s, never `kCount` (verified: `abilities.hpp:120-138`,
`best` starts at `Skill::kMelee` and is only ever reassigned to a *strictly greater* level, so even
an all-zero fresh character resolves to `kMelee`'s pair, never `kCount`). **This RFC's picker is the
first code path that can put a genuine `kCount` into a live `PlayerView.ability[i]`** — before a
player's first manual pick is made in a slot that has no eligible fallback, or after a respec clears
a slot per RFC-019 §5.6's "the equipped slot is cleared" rule. Left unguarded, `draw_ability_slots`
would silently render a phantom WhirlCleave-shaped slot (icon, cost, cooldown, the works) instead of
a genuinely empty one.

**Fix, scoped to this RFC.** `draw_ability_slots` must special-case `id == AbilityId::kCount`
*before* calling `ability_def`: draw a plain empty outline (existing `DrawRectangleLines` call,
`screens.cpp:236-237`, dimmed color) with no icon and no `F`/`G` glyph disabled-state logic —
distinct from `locked` (which still shows a specific, real ability, just greyed). This is a
one-branch guard, not a redesign of the function.

### 6. Key rebinding (new)

#### 6.1 Bindable action table (default = the exact shipped hardcoded map)

The default table is not invented; it is the current hardcoded map, named as bindable actions:

| Action | Default key(s) | Source |
|---|---|---|
| Move Up/Down/Left/Right | `W`/`S`/`A`/`D` (primary, rebindable) + Arrow keys (fixed secondary, §6.3) | `raylib_bridge.cpp:1800-1803` |
| Heavy-attack modifier | `Left Shift` / `Right Shift` | `raylib_bridge.cpp:1860` |
| Shoot | `Q` or `Space` | `raylib_bridge.cpp:1871` |
| Ability slot F / G | `F` / `G` | `raylib_bridge.cpp:1875-1876` |
| Harvest | `E` (or middle mouse, unchanged, not rebindable — a mouse-button rebind table is out of scope, §Non-goals) | `raylib_bridge.cpp:1844` |
| Till | `T` | `raylib_bridge.cpp:1845` |
| Upgrade | `U` | `raylib_bridge.cpp:1846` |
| Toggle build mode | `B` | `raylib_bridge.cpp:1812` |
| Hotbar slot 1–4 (context: build-kind or element, per mode) | `1`/`2`/`3`/`4` | `raylib_bridge.cpp:1819-1825` |
| Mount/dismount | `R` | `raylib_bridge.cpp:1829` |
| Open Character screen | `C` | `screens.cpp:565` |
| Open Journal screen | `J` (only while `kPlaying`) | `screens.cpp:561` |

**Reserved, never rebindable:** `ESC` (universal pause/back, `screens.cpp:550-556` — fixed so a bad
rebind can never lock a player out of every menu) and `F3` (debug overlay, `screens.cpp:546` — a
developer tool, not a player-facing binding, matching GAME.md §12's own framing of engine counters
as debug-only). Arrow-key movement stays fixed as the secondary binding for the same reason: even a
completely broken WASD rebind leaves basic navigation intact.

#### 6.2 Rebind flow

The Options screen's existing stub (`screens.cpp:821`) becomes a scrollable list of the table above.
Selecting an action and pressing a key rebinds it, with one guard: **binding a key already assigned
to a different bindable action is blocked with an inline warning**, not silently swapped (tunable
UX choice — swap-on-conflict was considered and rejected because a silent unbind of an action the
player forgot they were mid-list on is a worse failure mode than an extra click to resolve it
themselves).

#### 6.3 Scope: one primary key per action, no per-action secondary bindings in v1

Each rebindable action gets exactly one bindable key in v1. The two fixed exceptions (arrow keys for
movement, `ESC` for back) are not "secondary bindings" in the general sense — they are safety rails,
not a feature, and are not exposed in the rebind UI at all. Whether general secondary bindings (e.g.,
"also fire Shoot on left-click") are wanted is Open Question 4.

**Left/Right Shift is one logical input, not two.** The shipped Heavy-attack modifier already treats
`Left Shift` and `Right Shift` as a single either-key check (`IsKeyDown(KEY_LEFT_SHIFT) ||
IsKeyDown(KEY_RIGHT_SHIFT)`, `raylib_bridge.cpp:1860`), the same way arrow-key movement is one
logical direction bound to two physical keys. Rebinding Heavy-attack modifier under this RFC's
one-key-per-action rule replaces *both* Shift keys with whatever single new key is bound — it does
not split "Left Shift" and "Right Shift" into two separately rebindable actions, and does not leave
one Shift key live after a rebind.

#### 6.4 Downstream effect on §2's drawn glyphs

`draw_ability_slots`'s `kKeys[kAbilitySlots] = {"F", "G"}` (`screens.cpp:209`) becomes a read of the
live rebind table instead of a compile-time constant — the glyph drawn in each slot's corner must
always match whatever key currently fires that ability, or the HUD lies to the player about their
own input configuration.

#### 6.5 Storage

Extends `client.cfg` (`client_main.cpp:29-67`) with one `key.<action>=<raylib keycode int>` line per
bindable action, following the exact same tolerant-parsing contract already in place for
`volume=`/`music=`/`join=`: unknown keys ignored (forward compatibility), a missing or garbled file
means the shipped defaults from §6.1's table, never a crash. This is machine-local client
preference, not account/progression state — it is deliberately **not** routed through RFC-016's
server-side `SqliteStore` persistence, matching why `volume`/`music`/`join_addr` aren't either.

---

## Interactions with Other RFCs

| RFC | Relationship |
|---|---|
| **RFC-001** (accepted) | Supplies the two-slot kit, cooldown ticks, and admission `kLocked` rule §2/§5 read. This RFC never changes admission — it only draws its outcome and, in §5, lets a player choose *which* eligible ability occupies a slot, never whether it's eligible. |
| **RFC-002** (accepted) | §4 answers Open Question 4 within the *actually replicated* budget, and documents the RFC-002-vs-RFC-015 byte-budget discrepancy discovered while doing so (Open Question 3, this RFC). Does not touch RFC-002's gauge math, ladders, or thresholds. |
| **RFC-005/RFC-006** (accepted) | §3's boss bar is additive to, never a replacement for, RFC-006's telegraph/tint vocabulary; §4's status pips reuse RFC-006's element palette rather than defining a new one. Boss AI, kit content, and telegraph timing remain entirely RFC-005/006's. |
| **RFC-013** (accepted) | §1 cites RFC-013 §1's vitals-regen semantics as the thing this HUD draws, not computes. §5.1's out-of-combat loadout gate reuses RFC-013 §1's existing combat-cooldown flag rather than inventing a new timer. |
| **RFC-015** (accepted) | The data source for every field this RFC reads (§1–§4). §4 explicitly does not assume RFC-015 carries more than its frozen `PublishedCreature.status` byte actually does. §5.3's `SetLoadout` intent is named at the message-kind level only; wire encoding is RFC-015's to specify if/when it formalizes new client→server intents. |
| **RFC-016** (accepted) | §5.3 is the first code to actually write RFC-016's reserved, currently-`NULL` `equipped_ability_0`/`equipped_ability_1` columns, using the persistence cadence RFC-016 §4.2 already defined for progression fields — no new persistence mechanism. |
| **RFC-019** (accepted) | §5.4/§5.6 explicitly deferred the manual-vs-auto loadout decision to this RFC; §5 adopts RFC-019's own forward-looking framing as the answer. §5.10's read-only Character-screen ability list becomes §5.2's picker surface, not a new screen. |
| **RFC-012** (proposed, sibling) | Audio cues for slot-ready / cooldown-complete, if any, are RFC-012's to define; this RFC only specifies the visual cooldown-wipe state RFC-012 would hook a sound to. No shared numbers are claimed here. |
| **RFC-017** (proposed, sibling) | §5.1's out-of-combat loadout gate is a scope-avoidance choice, not a balance ruling — if playtesting says mid-combat swapping should be allowed, that is a balance call for RFC-017's process to make, not a silent change to this RFC. |
| **RFC-022/RFC-021** (accepted) | The Map screen (`M`) is untouched — confirmed absent from the `Screen` enum this RFC reads (`screens.hpp:26-34`) and out of scope; this RFC does not add it, rebind it, or assume its existence. |

---

## Multiplayer & Simulation-LOD Considerations

- **Own-player-only vitals, no exceptions.** §1's vitals bars never read another player's `PlayerView`
  even where it happens to be reachable in-process today (RFC-015 §2.5 names this exact dormant
  privacy gap and closes it at the wire level; this RFC's contract is the same restriction applied
  at the drawing layer, belt-and-suspenders).
- **Boss bar is per-viewer, not per-world.** Each client independently computes "is a boss sharing my
  current room" from its own `frame_boss`/room-membership check (§3, `raylib_bridge.cpp:1305-1313`);
  two players in the same room as a boss each draw their own bar off the same replicated
  `hp`/`max_hp`, no additional server bookkeeping, no "who's fighting the boss" concept introduced.
- **Status pips cost nothing extra under LOD.** §4's pips read the same `stage` byte already
  replicated for tint purposes at whatever cadence RFC-015's send-cadence rules already apply to a
  chunk (inner/outer band, RFC-015 §4) — a sleeping or 1 Hz chunk's creatures simply show stale pips
  exactly as they show stale tint, which is already the accepted behavior for a background chunk.
- **The loadout picker's server-side validation is the trust boundary**, not the client-side UI gate
  — a modified client cannot equip an ineligible ability by skipping the `SetLoadout` client check;
  `PlayerActor` re-validates (§5.3). This is not framed as anti-cheat (see §Non-goals) — it is the
  same "server is authoritative, client is a rendering surface" discipline every other write path in
  this project already follows (RFC-001's admission chain, RFC-013's vitals), applied consistently
  here rather than as a new defensive posture.
- **Key rebinding is purely client-local and has zero multiplayer surface** — it changes which key
  fires which already-existing intent, never what intents exist or what a server accepts.

---

## Tone Guardrail Compliance

1. **The boss bar and status pips are current-state readouts, not countdowns.** A bar shows `hp/
   max_hp` *right now*; it does not predict, does not tick down on a timer independent of combat,
   and only ever changes because a hit (or, per RFC-013's carved-out exception, a leash reset)
   actually happened. Nothing here runs while the player is not engaged with it.
2. **The cooldown-wipe rectangle is not a hidden clock — it is the opposite.** It exists specifically
   so the player *can see* exactly how much longer a cost they already paid keeps an ability
   unavailable; hiding it would be the tone violation. GAME.md §0 bans countdowns that run *behind*
   the player's back, not visible feedback on state the player caused by choosing to act.
3. **The equipped-ability-persistence "worst case" is asymmetric in the player's favor**, mirroring
   RFC-016's own argument about `ability_cd_` not being persisted: if a `SetLoadout` write is lost to
   a crash between the discrete-event save and the next periodic checkpoint (RFC-016 §4.2's existing
   cadence), the world resumes with the *previous* loadout — never a state the player didn't choose,
   never a worse outcome than "your last confirmed pick."
4. **Status pips reveal current stage, never a hidden threshold or progress-to-threshold number.**
   §4's ruling deliberately stops at a discrete 0–3 pip count (what RFC-002's `stage` byte actually
   contains) rather than reaching for a fine percentage-to-next-stage readout — which the wire data
   doesn't carry anyway (§4), but which would also read uncomfortably close to "a meter counting up
   to something bad happening to the monster you're fighting," a framing this RFC avoids on both the
   data-availability and the tone grounds independently.
5. **The loadout picker is optional depth, not a new chore.** §5.1's "opt-in, reversible, defaults
   to today's auto-pick forever" rule is this RFC's own instance of GAME.md §0's central bargain
   applied to UI complexity rather than world pressure: a player who wants zero management overhead
   gets exactly that, permanently, and a player who wants to hand-tune their kit gets to, with no
   penalty for not doing so and no way to end up worse off than the auto-pick would have left them.

---

## Open Questions

1. **Multi-boss view.** §3's "nearest boss wins the single bar slot" rule has no authored encounter
   to test it against today (RFC-005's shipped shortlist has no multi-boss fight). Revisit if/when
   one is authored — possibly a stacked mini-bar list instead of a single bar.
2. **Per-kit boss names.** §3's shipped bar already draws a specific name (`"DOJO MASTER"`), correct
   only because one boss kit exists today. Once a second boss kit ships, `kind ==
   CreatureKind::kBoss` alone cannot distinguish which name/portrait to draw. The fix is a small
   RFC-015 amendment (a `boss_kit_id: u8` or similar on `PublishedCreature`) — this RFC does not
   propose the field itself, only flags the need, updated from "no name exists" to "the one hardcoded
   name won't scale past one boss kit."
3. **The RFC-002/RFC-015 status-byte discrepancy (§4).** RFC-002 §10 designed for 2 bytes; RFC-015
   §2.2 shipped 1. Does a future RFC-015 revision restore the quantized-gauge byte (enabling a true
   fine-grained "targeted" gauge as RFC-002 originally envisioned), or does RFC-002 §10 get a
   follow-up edit narrowing its own claim to match what's actually replicated? This RFC takes no
   position beyond "the discrepancy is real and both documents currently disagree with the shipped
   struct or each other."
4. **Secondary key bindings.** §6.3 ships one bindable key per action in v1 (plus the two fixed
   safety rails). Is a second, player-chosen binding per action (not just the fixed arrow-key/ESC
   rails) wanted for accessibility, and if so, does §6.5's `client.cfg` line format need a list
   rather than a scalar?
5. **Out-of-combat loadout gate, exact value.** §5.1 reuses RFC-013's existing `kCombatCooldownMs`
   flag rather than defining a new threshold — is reusing the HP-regen combat window the right
   feel for "safe to swap," or does loadout-swapping deserve its own, possibly shorter or longer,
   window? Flagged for playtest, not decided here.

---

## Non-goals

- **Rendering internals** (sprite batching, camera projection, atlas packing mechanics beyond citing
  the existing `icon_rect`/twin-icon convention) — `RENDER_SPEC.md` territory, untouched.
- **Any wire protocol byte layout, message encoding, or new `PublishedX` struct field.** RFC-015 owns
  every byte on the wire; this RFC names *what data it needs* and *where a gap exists* (§Open
  Questions 2, 3) but never specifies the fix itself.
- **Boss AI, boss kit authoring, or telegraph timing/geometry.** Entirely RFC-005/RFC-006. §3's bar
  reads `hp`/`max_hp`/`kind` and nothing about *how* those numbers change.
- **Anti-cheat, client-input validation beyond the ordinary server-authoritative discipline every
  other RFC in this set already applies.** §5.3's server-side re-validation of `SetLoadout` is not a
  new defensive system — it is the same "server decides, client asks" pattern RFC-001's admission
  chain already uses, applied to one more intent. This RFC introduces no detection, logging, or
  punitive mechanism for a malformed or hostile client.
- **Balance tuning of ability costs, cooldowns, or damage** — RFC-009/RFC-017's territory. §5's
  loadout picker changes *which* eligible ability a player equips, never any ability's numbers.
- **Mouse-button rebinding.** §6's table covers keyboard bindings only. Every mouse-driven verb stays
  a fixed physical button, not rebindable under this RFC: Swing (left-click held,
  `raylib_bridge.cpp:1859`), Cast (right-click, `raylib_bridge.cpp:1870`), Build placement
  (left-click, `raylib_bridge.cpp:1848`), Plant (right-click, `raylib_bridge.cpp:1849`), and
  harvest's middle-mouse alternate trigger (`raylib_bridge.cpp:1844`).
- **The Map screen (`M`) and any of its bindings.** Confirmed absent from the `Screen` enum this RFC
  reads; RFC-021/RFC-022's territory.
- **Loot, Essence, or reward-table display.** RFC-018's territory; this RFC's HUD shows vitals,
  cooldowns, and boss HP, never inventory or drop content.
- **Chat, party UI, or any social/grouping HUD element.** Not named by any grounding this RFC was
  scoped against; ROADMAP.md P6 territory if/when chartered.

---

## Review Record

Votes: Reviewer A — revise (blocker: §3 boss bar drafted as new when shipped; 2 minor: mouse-verb
Non-goals, Shift-key rebind ambiguity). Reviewer B — revise (same blocker, independently verified;
concurred on both minors in rationale without formal mustFix listing).

Applied:
- §3 rewritten from "(new)" to "normative baseline (already shipped) plus the real remaining gap,"
  citing `draw_boss_bar` (raylib_bridge.cpp:678-704), `frame_boss` (1305-1313), the unconditional
  call site (1773), and the shipped `"DOJO MASTER"`/`Boss::kFace` label — trigger corrected from
  proposed chunk-interest-set to shipped same-room; layout numbers corrected to shipped values.
- Summary and Motivation point 4 corrected: boss bar no longer claimed to not exist.
- Open Question 2 reframed: "no name replicated" to "one hardcoded name won't scale past one kit."
- Multiplayer §"Boss bar is per-viewer" corrected from interest-set framing to same-room framing.
- As-built grounding header extended to cite `draw_boss_bar`/`frame_boss`/line 1773.
- Non-goals mouse-button bullet now names Swing/Cast/Build/Plant with exact line cites.
- §6.3 gained an explicit Left/Right-Shift-is-one-logical-input clarification.

Unresolved: none — both mustFix items had sound proof and were applied; the two minor items had
sound proof from direct source verification and were folded in as requested.
