# RECONCILIATION — STATUS-MATH canon (series-editor pass 1)

> Scope: the combat RFC set (`RFC-001`…`RFC-010`). This file records the cross-RFC arbitration the
> individual finalizers deferred "to the series editor." Each ruling picks ONE canonical position
> for a contradiction the concurrent finalizations produced, gives the rationale with evidence, and
> lists the files/sections edited to conform.
>
> Decision criteria, in order: (1) hard constraints and repo design docs (GAME.md / ARCHITECTURE.md
> / ROADMAP.md and the pack asset audit) win; (2) internal soundness of the mechanism; (3) least
> churn across the ten files.
>
> Ruling numbers match the references the finalizers already wrote into RFC-002's Open Questions and
> Review Record (rulings 2, 3, 4, 5) and RFC-002 §6 (ruling 3). Ruling 1 (the channel set) is the
> one those files did not need to cite because RFC-002 was already on the winning set.

---

## Ruling 1 — the five status channels are **{Cold, Heat, Shock, Earth, Stagger}** (CONFLICT A)

**Conflict.** The two owners swapped sets during concurrent finalization. RFC-002 rebased to
`{Cold, Heat, Shock, Earth, Stagger}` "per RFC-009 §4.5"; RFC-009 §4.5 rebased to
`{Frost, Heat, Shock, Poison, Stun}` "per RFC-002" — each adopted the other's *draft*. The aborted
arbitration pass had already re-converted RFC-009's §1 and §4.3 back to
`{Cold, Heat, Shock, Earth, Stagger}` but left §4.5/§4.6 on `{Frost/Poison/Stun}`, so RFC-009 was
internally split. RFC-006 (wash table), RFC-007 (observation one-hots) and RFC-008 (`status.*`
serialization, V41) encoded `{Frost, Heat, Shock, Poison, Stun}`; RFC-002, RFC-003 and RFC-010
encoded `{Cold, Heat, Shock, Earth, Stagger}`.

**Ruling.** The canonical set is **`{Cold, Heat, Shock, Earth, Stagger}`**. `Frost`→`Cold` and
`Stun`→`Stagger` are pure renames. `Poison` is **deferred** (RFC-002 Open Question 2), not deleted.
The P2 `Muddy` coating **folds into the Earth ladder** as stage 2 (**Mired**); v1 has exactly one
coating (Wet). `Root` (the Earth terminal) becomes representable.

**Rationale (evidence).**
- *Hard constraint — 4 elements Fire/Ice/Rock/Thunder.* The pack asset audit
  (`memory/fight-system-asset-constraints.md`) and GAME.md fix the four element books as
  **BookFire / BookIce / BookRock / BookThunder**; the spare schools are Plant/Water/Light/Darkness/
  Wind/Death — **there is no Poison book and no Poison anywhere in v1 content**. In the losing set,
  **Rock (a hard-constraint element) has no ladder** while **Poison (which nothing in v1 inflicts)
  owns a gauge** — exactly backwards. In the winning set each of the four books drives one ladder
  (Ice→Cold, Fire→Heat, Thunder→Shock, Rock→Earth) plus Stagger for physical, so every channel has
  a real v1 source and every element has a home.
- *Internal completeness.* RFC-009 §4.5's own losing-set table sourced Poison only from
  "spider-bite riders (RFC-005)" and "bog water" — neither exists in v1; that gauge would never
  fill. Earth, by contrast, is filled by the Rock school (BookRock ships) and by bog/rubble terrain
  (RFC-002 §8, RFC-010). Rock's control identity (Encumbered→Mired→Root) is the natural third
  member alongside the other element ladders; expressing it as a throwaway "Muddy" coating + a
  Stagger rider (the losing set's workaround, RFC-008 Worked Example 2) is strictly weaker.
- *Least churn is roughly even (3 files each way) and does not decide;* criteria (1) and (2) do.
- *Losing-set disposition (explicit, not silent):* Poison → RFC-002 OQ2 (parked as a candidate
  future gauge; the runtime array is fixed `[5]`). Muddy → Earth stage 2 (Mired), per RFC-002 §11
  migration. Frost/Stun → renamed to Cold/Stagger.

**Files/sections edited.** RFC-009 §4.5 (channel table, gain-formula clamp, derived-Stagger prose),
§4.5 stages list, §4.8, §5, Review Record, Q2; RFC-006 §6 (wash table + coating table + header);
RFC-007 Block S idx 1–5/8–9 and Block T idx 29–33/36–37 + prose; RFC-008 §7.4, §3 numeric table,
Worked Examples 1 & 2, entity aura, V41, Open Question 7, Review Record; RFC-003 (Stagger/Mired
naming touch-ups). RFC-002/RFC-010 already conform (no edit).

---

## Ruling 2 — gauge scale is **integer [0,1000]**, thresholds **300 / 600 / 900** (CONFLICT B)

**Conflict.** RFC-002 used integer `[0,1000]` with thresholds 300/600/900. RFC-009 §4.5 used
`meter[5]` on `0..255` with thresholds 100/170/230 (Stagger 100/200) and `kIceBoltPower=170`.
RFC-008 spoke of rescaling authored `buildup_max` to `[0,1000]` at load; RFC-002 had already deleted
`buildup_max`.

**Ruling.** The canonical scale is **integer `[0,1000]` per-mille, thresholds `T1=300 / T2=600 /
T3=900` for all five ladders (one triple)**, `min(1000, …)` clamp. Calibration anchor
**`kIceBoltPower = 600`** (see ruling 4). Decay is per-ladder (Cold 6/tick … Stagger 14/tick) after
a 15-tick grace — a full Cold gauge self-empties in ~17 s. No `buildup_max`, no load-time rescale
(see ruling 5).

**Rationale (evidence).**
- The entire pipeline is per-mille integer arithmetic (`1000 = ×1.0`); a `[0,1000]` gauge is the
  same number space as every other multiplier, so the single-floor gain product
  `power × affinity × tier × soft × coat / 1000⁴` lands directly on the gauge with no extra rescale.
  `0..255` is a byte-packing optimization that fights the arithmetic and forces a second scale to
  reason about.
- RFC-002 (the owner of status *state and semantics*, per its own carve-out and RFC-009 §5) was
  fully on `[0,1000]`; RFC-009 §1/§4.3 had already been re-converted to it; RFC-008's units table
  documents build-up amounts as `0..1000`. `[0,1000]` is therefore also the least-churn scale.
- RFC-002 Open Question 7 (single threshold triple; Stagger keeps three rungs) resolves under this
  ruling: one triple 300/600/900 for all ladders, the §4 slot exemption already makes Stagger's
  stage 1–2 cheap, so no per-ladder threshold fork is needed.

**Worked example (re-verified by script on the canonical scale, not eyeballed).**
Ice bolt `power = 600`, no coating, no soft-resist; Frozen at meter ≥ `T3 = 900`;
`gain = ⌊600 · affinity · tier / 10⁶⌋` (soft = coat = 1000 fold out):

| Target (mat, Cold affinity ‰) | Tier (intake ‰) | Gain/hit | Hits to Frozen ⌈900/gain⌉ | Second-freeze gain (×0.5) | Hits to re-Frozen |
|---|---|---|---|---|---|
| Slime (1250) | Small (1300) | 975 | 1 | 487 | 2 |
| Ghost (Spirit, 750) | Medium (1000) | 450 | 2 | 225 | 4 |
| GiantFrog (Flesh, 1000) | Large (600) | 360 | 3 | 180 | 5 |
| GiantRedSamurai (Flesh, 1000) | Giant (350) | 210 | 5 | 105 | 9 |
| Titan (Flesh, 1000) | Titan (200) | 120 | 8 | 60 | 15 |

Verification rule: `hits = ⌈900 / gain⌉`; second-freeze gain = `⌊first-freeze product × 500 / 1000⌋`
at the same single-floor position. (Script: `scratchpad/verify.py`.) Medium Flesh calibration:
`gain/cast = 600` → one cast lands exactly `T2 = 600` (HeavySlow), two casts → `min(1000, 1200) ≥
900` = Freeze.

**Files/sections edited.** RFC-009 §4.5 (scale, thresholds, decay, clamp, calibration constant),
§4.6 (worked table + verification rule fully recomputed), Q2, Review Record. RFC-002 already on the
canonical scale; its byte counts already recomputed around it (no edit). RFC-008 authored-range rule
handled under ruling 5.

---

## Ruling 3 — anti-chain is the **two-level soft-resist window** (×0.5 / ×0.25, floor 250‰); **I5b owned in RFC-002 §6** (CONFLICT C)

**Conflict.** RFC-002 had (in an earlier finalization) adopted "RFC-009's flat terminal refractory"
(×0.6 gain for 150 ticks over a post-trigger gauge floor of 450); RFC-009 had adopted "RFC-002's
two-level soft-resist ×0.5/×0.25, gain floor 250‰ (I5b)."

**Ruling.** The sole anti-chain mechanism is the **two-level soft-resist window**: on terminal
resolution the channel's gauge empties to 0 and `resist_level` increments (capped at 2);
while inside `kResistWindow = 150` ticks, gains on that channel are multiplied by
`{1000, 500, 250}‰` — never deeper than 250‰ (**Invariant I5b**). I5b is **defined in RFC-002 §6**;
RFC-009 §4.5 consumes it as a normative input and references it. The flat refractory + 450 floor is
rejected and deleted.

**Rationale (evidence).**
- *Soundness — the flat refractory produced de-facto the opposite of its goal.* With a post-trigger
  floor of 450, a second Freeze costs `(900 − 450)/0.6 = 750` effective Power versus 900 for the
  first — the "anti-chain" mechanism made the *second* terminal **cheaper** than the first. With the
  gauge emptied to 0 and soft-resist armed, a second Freeze costs ×2 the Power of the first and a
  third ×4 (geometric decay toward, but never reaching, immunity — floor 250‰). This is the
  behaviour the umbrella's "no absolute immunities, but earn it" demands.
- *Simplicity in integer math.* Soft-resist is one per-mille multiplier on the gain product plus a
  clamped `uint8 resist_level` and a `resist_until` tick — no gauge-floor special case interacting
  with the walk-down and terminal-exit branches. The flat floor had to be reconciled against §3's
  terminal-exit rule (which already sets the gauge to 0), a direct contradiction.
- Symmetric between players and monsters (tone guardrail: a wanderer can be beaten but never held
  down indefinitely).

**Files/sections edited.** None required for this ruling: RFC-002 §6 already owns I5b and the
two-level window; RFC-009 §4.5 already consumes it by reference. Confirmed both sides consistent and
the flat-refractory text absent from both bodies. (RFC-002's Review Record already records this
change and cites this ruling.)

---

## Ruling 4 — calibration anchor **`kIceBoltPower = 600`**; the P2 two-cast-freeze contract is preserved (CONFLICT E)

**Conflict.** RFC-002 Open Question 6 flagged that RFC-009 §4.6's earlier worked numbers (~4 casts
to stage 2, from a superseded Power-180-on-0..255 draft) were slower than the P2 "two-cast-freeze"
compatibility contract.

**Ruling.** Set **`kIceBoltPower = 600`** on the canonical `[0,1000]` scale (ruling 2). One cast
lands exactly `T2 = 600` (HeavySlow) and two casts land Freeze on a Medium Flesh target with empty
meters — the P2 feel is retuned into the calibration constant, not superseded. RFC-002 OQ6 and
RFC-009 Q2 are closed with this value.

**Rationale (evidence).**
- The two-cast-freeze feel is the umbrella/P2 compatibility contract (RFC-002 §11 migration row):
  on baseline (Medium Flesh) targets the game should still feel like P2, with ladders becoming
  visible only against big/exotic/recently-proc'd targets. GAME.md/ROADMAP treat combat-feel
  continuity as load-bearing (retuning combat twice is the roadmap's most expensive waste), so the
  contract is *preserved* rather than overridden.
- The worked table (ruling 2) shows the intended spread this anchor produces: Slime freezes in 1,
  Medium in 2, Large in 3, first RL boss (Giant Samurai) in 5, Titan in 8 — a commitment curve, not
  an immunity, at every tier.

**Files/sections edited.** RFC-009 §4.5 calibration paragraph and Q2 (170→600). RFC-002 OQ6 already
records `kIceBoltPower = 600` and cites this ruling (no edit).

---

## Ruling 5 — RFC-008 serializes **absolute build-up amounts on [0,1000]; no `buildup_max`, no rescale** (CONFLICT J)

**Conflict.** RFC-008's par-7.4 / V41 area had to be checked end-to-end against rulings 1–2: channel
names in the closed set, authored range, and rescale rule. RFC-002's unresolved list historically
claimed RFC-008 "still declares `buildup_max: 100`."

**Ruling.** RFC-008 `status.*` documents and payloads carry **absolute build-up `amount`/`gain`
values on the shared `[0,1000]` scale**, with a **first-class `channel` key** drawn from the closed
set `{cold, heat, shock, earth, stagger}` plus the single coating `wet`. **There is no
`buildup_max` field and no load-time rescale.** V41 closes the `status.*` set at **five channels +
one coating (six documents)**. `Root` is representable (Earth terminal); Rock-school control authors
`earth` build-up.

**Rationale (evidence).**
- Grounding in current text: RFC-008 §7.4 already serializes absolute `amount` on the shared scale
  with a first-class `channel` key and carries **no** `buildup_max` field anywhere — so RFC-002's
  "still declares `buildup_max: 100`" note was **stale** (confirmed against the current file). The
  side that was wrong was the stale note, not RFC-008's serialization.
- Consistency with ruling 2: because the gain product already lands on `[0,1000]`, an authored
  `buildup_max` + rescale would be a redundant second scale — the exact double-bookkeeping RFC-002's
  Interactions section deleted. Absolute amounts keep one scale across RFC-002/008/009.
- The remaining RFC-008 work is ruling 1's channel/coating rename (frost→cold, poison→deferred,
  stun→stagger, muddy-coating→earth build-up, Root now representable), which also retires RFC-008's
  Open Question 7 (the channel-set divergence it was tracking).

**Files/sections edited.** RFC-008 §7.4 (channel/coating enums, migration note, divergence block),
§3 numeric-conventions table, Worked Examples 1 & 2 and the rock-spike entity aura (muddy→earth),
V41, Open Question 7, Review Record. RFC-002 Interactions already states "no `buildup_max`, no
rescale" and its Review Record cites this ruling (no edit).

---

## Files edited by this pass

- **RFC-006** — §6 status-wash table and coating table converted to the canonical channel set.
- **RFC-007** — Block S / Block T status one-hots and coating bits renamed (widths unchanged).
- **RFC-008** — §7.4, numeric table, worked examples, entity aura, V41, OQ7, Review Record.
- **RFC-009** — §4.5/§4.6 converted to the canonical set + `[0,1000]` scale; worked example
  recomputed and script-verified; §4.8/§5/Q2/Review Record aligned.
- **RFC-003** — minor Stagger/Mired naming touch-ups.
- **RECONCILIATION.md** — this file.

Already-consistent, not edited: **RFC-002** (owner of status semantics — already on the canonical
set, scale, and soft-resist; its Review Record and Open Questions already cite rulings 2–5),
**RFC-010** (its only "Earth" references are the Rock element's FX strip, not the channel),
**RFC-001**, **RFC-004**, **RFC-005**.

---

# RECONCILIATION — OWNERSHIP WORDING and REFERENCES (series editor pass 2)

> Scope: the six directed amendments left for the series editor by name (F, D, G, H, I, B2) plus
> the two housekeeping documents (K, L). Numbering continues from pass 1 (rulings 1–5 above);
> pass-1 rulings are not reopened. Same decision criteria as pass 1: (1) hard constraints and repo
> design docs win; (2) internal soundness; (3) least churn.

---

## Ruling 6 — the two pre-series filenames never existed; every reference to them is replaced with a clean supersession sentence (AMENDMENT F)

**Conflict.** `RFC-001-combat-entity-and-materials.md` and `RFC-010-rl-observation-schema.md` are
named throughout the set — in header "Supersedes"/"Numbering note" blocks, Interactions rows, and
Review Records of RFC-001, RFC-004, RFC-007, RFC-008, and (normatively, not just historically)
RFC-010 §6/§7 — as pre-series drafts to archive or delete. `find` over the repo confirms **neither
file exists anywhere**. The worst instance is normative, not cosmetic: RFC-010 §6 defers its
observation-surface layout to "the frozen BossObs v2 schema (`RFC-010-rl-observation-schema.md`,
`surface[49]` bytes, 7×7 window)" and §7's RFC-007 interaction row repeats the same pointer — RFC-010
hands its RL contract to a document that does not exist, and RFC-007 (the file that actually owns
the F4 tensor contract) is not the file named.

**Ruling.** Every reference to the two nonexistent filenames is removed. Where a sentence needed
the context, it is replaced with a single clean line: "Earlier exploratory drafts were removed;
this set (RFC-001..010) is canonical." RFC-010 §6 and its §7 RFC-007 row are **repointed at
RFC-007 §2** — the sole F4 tensor contract — instead of the retracted draft. Because RFC-007's
actual encoding (§2 Block R terrain/hazard rays + Block G 4-way ground class + Block E entity
slots) has **no per-tile grid/window** the way the retracted "BossObs v2" draft did, RFC-010's
`Surface` values and RFC-004's `ScarKind` scars are re-described against what RFC-007 *actually*
exposes (ray hazard-closeness, ground class, and — for hazard-circle entities — Block E's
`HazardZone` class one-hot, which already covers them with no gap). The open question of whether
that coarser ray/class signal is sufficient for F4 training, or needs a dedicated per-tile
surface/scar feature later, is recorded as **new RFC-007 Open Question 7**, claimed from the
existing reserved block (§2, indices 107–119) if it is ever needed — not invented as a new grid
layout in RFC-010, which does not own the tensor contract.

**Rationale (evidence).** Criterion (1): a spec that normatively points at a file which does not
exist is not a spec, it is a broken link — this is a correctness defect, not a style question, so
it wins on constraint grounds alone (the observation contract must be buildable). Criterion (2):
inventing a new grid contract inside RFC-010 would violate RFC-007's own explicit ownership claim
("this RFC is the only F4 tensor contract") and would let a second file define tensor layout,
exactly the fragmentation RFC-007's Motivation section was written to prevent; recording the
genuine gap as RFC-007's own open question keeps ownership singular. Least churn (3) favored
editing the reference, not the referrer, wherever a clean rewrite sufficed.

**Files/sections edited.** RFC-001 (header numbering note, Review Record bullet + unresolved);
RFC-004 (header, Review Record bullet); RFC-007 (header, Interactions supersession bullet, Review
Record applied/unresolved bullets, new Open Question 7, Review Record reconciliation line); RFC-008
(header numbering note, Review Record bullet); RFC-010 (§6 RL Considerations bullet, §7 Interactions
RFC-007 row, Review Record applied bullet + unresolved + reconciliation line).

---

## Ruling 7 — RFC-003 §4 is the single normative tier/mass table; RFC-009's echo, floor wording, and the Squids example are brought into line with it (AMENDMENT D)

**Conflict.** RFC-003 §4 declares itself sole owner of tier intake ‰ and mass; RFC-009 §4.6 quotes
those numbers "for convenience, changed only there." Three echoes had drifted from the source of
truth they claim to quote: (a) RFC-009 §4.6's Giant-tier example row listed **Squids**, while
RFC-003 §4's own table and its "Tier membership follows RFC-009 §4.6 (Squids sit at Large with
GiantFrog...)" line both say **Large** — RFC-003's Review Record even claims this was already
"aligned," which was false against RFC-009's actual body text; (b) RFC-009 §4.6's prose floor —
"the invariant floor on intake — ≥ 150‰... is RFC-003 §4's, inherited unchanged" — quotes a number
RFC-003 §4's own body does not have (RFC-003 §4 states **I5: gain ≥ 250‰**, and its own Titan row
is 200‰ base intake, so a 150‰ floor is neither RFC-003's number nor internally consistent with
250‰); (c) RFC-009 §4.6 paraphrased RFC-002 §6's terminal-duration rule as "×0.75 at Large, ×0.5
at Giant/Titan," which matches neither RFC-002 §6's actual `kTierTerminalDur` (Large 800‰ = ×0.8,
Giant 600‰ = ×0.6, Titan 450‰ = ×0.45 — three distinct values, not two). Separately, RFC-003's own
Review Record's unresolved item — that RFC-009's informative "Mass class" column "still prints
stale ratios (0.5/1/1/3/6/12 vs mass/100 = 0.25/0.5/1/2.5/7/20)" — no longer matches RFC-009's
current table, which already prints raw mass points (25/50/100/250/700/2000, the correct
`mass/100` echo); that unresolved bullet was itself stale.

**Ruling.** RFC-003 §4's table is confirmed sole normative source (no change to RFC-003's numbers).
RFC-009 §4.6 is corrected to match it verbatim: Squids moved from the Giant example row to the
Large row; the intake floor restated as **I5: gain ≥ 250‰** with the Titan arithmetic corrected
to "~3× the Power" (not 5×, matching RFC-003's own rationale); the terminal-duration paraphrase
replaced with RFC-002 §6's actual `kTierTerminalDur` values (800/600/450‰, invariant I5c ≥ 300‰).
RFC-003's stale "Mass class" unresolved bullet is closed as already-fixed, and its Review Record's
false "tier examples aligned (Squids Large...)" claim is corrected by fixing the file it was
claiming agreement with.

**Rationale (evidence).** Criterion (3) (least churn) directly decides which file moves: RFC-003
is the declared, dependency-respected single source (RFC-009 §4.6 itself says so — "changed only
there"), so the copy conforms to the original, not the reverse. Fixing numbers to match a file's
own stated source is not a judgment call, it is closing a copy/paste drift; no new design decision
was made.

**Files/sections edited.** RFC-009 (§4.6 table Large/Giant example rows, floor wording, terminal-
duration paraphrase, Unresolved, Review Record reconciliation line); RFC-003 (Review Record
Unresolved bullet, Review Record reconciliation line).

---

## Ruling 8 — RFC-004's `EntityDef.aura_status` legacy field is replaced with RFC-002 §8's mapped `AuraSpec` shape (AMENDMENT G)

**Conflict.** RFC-002 §6/§8's finalization deleted the old whole-`Status` emit model and specified
the exact mapping RFC-004's aura field must land on: a single-entry apply list (channel-or-coating
+ absolute gain) plus the existing `AuraAffects`/`radius`/`period` fields. RFC-004's `EntityDef`
still carried the pre-mapping field, `Status aura_status;` — a type (`Status`) that no longer
exists in RFC-002's model — and the `kWaterPool`/`kFirePatch` archetype-table rows still read
`Status::kWet`/`Status::kBurning`, both retired enumerators.

**Ruling.** `EntityDef.aura_status` is replaced with the mapped shape: `Channel aura_channel`
(`kNone` if the aura applies a coating instead), `Coating aura_coating` (`kCount` if it applies
build-up instead), and `std::uint16_t aura_gain` (absolute build-up points on [0,1000] per pulse,
or coating ticks per pulse) — mirroring RFC-008 §7.5's `{channel|coating, gain}` aura block
one-to-one, as RFC-002 §8's `AuraApply` already specifies. `kWaterPool`'s row is re-expressed as a
Wet-coating aura (gain 80 ticks/pulse, matching RFC-002's default Wet duration); `kFirePatch`'s row
as a Heat build-up aura (gain 60/pulse, tunable — no calibration for this exists elsewhere in the
set, so it is marked tunable rather than invented as fact). Every other RFC's prose describing the
old field name (RFC-002 §8 Interactions/AuraSpec discussion, RFC-008 §7.5's EntityDef alignment
note) is re-keyed to the new field names.

**Rationale (evidence).** Criterion (1): this is executing a mapping RFC-002 — the owner of status
semantics — already specified as normative ("RFC-004's v1 `Status aura_status` field maps onto
this as a single-entry apply list"); the ruling is mechanical, not a new design choice. Criterion
(2): giving `EntityDef` the mapped fields directly (rather than leaving a dead `Status` reference
to be translated at some unspecified boundary) removes a phantom type from a `struct` that must
compile identically for sim/renderer/trusted-checker, the same one-source-of-truth argument the
struct's own comment makes for every other field.

**Files/sections edited.** RFC-004 (`EntityDef` struct, `kWaterPool`/`kFirePatch` archetype rows,
Review Record Unresolved/Reconciliation); RFC-002 (§8 `AuraSpec` prose, Interactions RFC-004 row,
Review Record Unresolved bullet closed, Reconciliation line added); RFC-008 (§7.5 `EntityDef`
alignment note, Review Record reconciliation line).

---

## Ruling 9 — RFC-007's class-taxonomy wording is re-keyed from "RFC-004 tags" to RFC-004's real `obs_class`/`observable` fields (AMENDMENT H, part 1)

**Conflict.** RFC-004's finalization gave `EntityDef` dedicated, typed fields for RL observability
— `bool observable` and `ObsClass obs_class` (§1) — replacing an earlier looser "tags" concept.
RFC-007 still described the same data twice as derived "from RFC-004 tags" (Summary section and
the Interactions RFC-004 row), which no longer names anything real in RFC-004 (there is no `tags`
field; the closest thing, `EntityKind`, is not what §1's comment calls "the tag RFC-007 depends
on" — that phrase itself refers to the typed fields).

**Ruling.** Both RFC-007 mentions are reworded to name the actual fields: "Entities appear as
*behavior classes and elements* (RFC-004's `obs_class`/`observable` fields)" and "Block E's class
taxonomy... is sourced from RFC-004's `EntityDef.obs_class` field, gated by `EntityDef.observable`
(RFC-004 §1)."

**Rationale (evidence).** Criterion (1)/(2): RFC-004 is the owner of this data and already named
its fields precisely; RFC-007 consumes them and should cite them by name for anyone tracing the
obs pipeline back to its source, exactly as RFC-007 already does for every other cross-RFC input
(`Channel`, `Coating`, `Collision`, etc., all cited by concrete type name, not generic "tags").
Least churn (3): two-sentence wording fix, no structural change.

**Files/sections edited.** RFC-007 (Summary sentence, Interactions RFC-004 row, Review Record
reconciliation line).

---

## Ruling 10 — the RFC-005/RFC-007 15-action table: verified identical, no drift found (AMENDMENT H, part 2 / AMENDMENT I)

**Check.** RFC-005 §R5's directed amendment ("RFC-005 §R5 must be amended to reference §3 as the
single enumeration") and RFC-007's parallel unresolved bullet ("must be executed in RFC-005... not
editable this session") both predate the current file states. Reading RFC-005 §R5 as it stands
today: it already states the action space is "RFC-007 §3's fixed 15-action table, verbatim and
unmodified," lists no separate per-kit or 11-action enumeration, and its own Review Record already
records the fix as applied ("§R5 + validator #8: per-kit action enumeration replaced with RFC-007
§3's fixed 15-action table"). A full read of RFC-007 §2 also turned up no leftover P2-era status
names (Frozen/Burning/Wet/Muddy/Shocked) outside the already-converted Block S/T — the "Rooted"/
"Wet-and-Frozen" mentions in Block S's prose are canonical RFC-002 terminal names, not P2 relics.

**Ruling.** No edit required to either file's normative content — the amendment was already
executed during RFC-005's own finalization. RFC-007's stale "Unresolved" bullet claiming this was
still outstanding is corrected to reflect the verified-consistent state, since leaving it would
misdirect a future reader into re-doing already-complete work.

**Rationale (evidence).** Verification, not arbitration: both files independently converged on the
same table with RFC-005 correctly deferring to RFC-007 as sole enumerator — the intended outcome —
so criterion (3) (least churn) is satisfied by leaving the substance alone and only correcting the
one stale status claim.

**Files/sections edited.** RFC-007 (Review Record unresolved bullet, reconciliation line). RFC-005
unedited — confirmed already conformant.

---

## Ruling 11 — RFC-010's residual `Zone`/`step_zones`/hazard-zone content is retired to match RFC-004 §2b's ownership split (AMENDMENT B2)

**Conflict.** RFC-004 §2b (a later, more complete finalization) gives hazard circles (smoke cloud,
wet ground, fire patch) wholly to RFC-004 as `CombatEntity` kinds and states plainly that "`Zone`
is not 'migrated', it dies" and that RFC-010's "Hazard-zone" designer-tool row and its retained
`step_zones` tick step "are superseded." RFC-010's own finalization fixed the **scar** half of the
§2b split (shrinking `Surface` to burning/mud/ice, ceding cracked/rubble/crater/scorched) but never
touched the **hazard-circle** half: its guide-level "three tools" table still listed "Hazard zone"
as one of *its* tools; its tick order still ran `step_zones` "hazard auras (existing Zone, migrated
to end_ms semantics)" — the literal phrase RFC-004 says is false; its replication table still
budgeted an 8-slot "Zone (existing, re-based on end_ms)" record; its LOD table still had a "Zones"
row; its Invariant L-2 paragraph still required `Zone::ticks_left` to migrate; and Open Question 1
("Zone unification") asked whether `Zone` should merge with patches or stay separate — a question
RFC-004 had already answered a third way (neither; it dies).

**Ruling.** All of the above is retired from RFC-010, matching RFC-004 §2b exactly: the designer
tools table drops to two tools (tile patch, field state) with hazard circles and scars both noted
as RFC-004's; `step_zones` is removed from the tick order (with a note that hazard-circle LOD is
now wholly RFC-004 §9's, not a step this RFC schedules); the Zone replication row and its 8-record
budget term are deleted; the LOD table's "Zones" row is dropped with a pointer to RFC-004 §9; the
`Zone::ticks_left` migration clause is removed from Invariant L-2 (only `Effect::age` remains, the
one pre-existing record this RFC still owns); Open Question 1 is marked resolved by RFC-004 §2b
rather than left open. Two stray "Zone(s)" mentions that were actually describing `FieldState`
cross-chunk coverage (a wording leftover from before the split) are corrected to say "Fields" /
"hazard-circle entities" respectively, without changing their claims.

**Rationale (evidence).** Criterion (1): RFC-004 §2b is the later, explicit, named single-ownership
boundary and directly instructs "RFC-010 must be amended to conform" — this is executing a standing
directive, not re-litigating it. Criterion (2): RFC-004 §9 already fully self-specifies a LOD/sleep
contract for `CombatEntity` auras (state transitions, aura pulses, removal, all as functions of
absolute tick), so RFC-010 keeping a parallel "Zones" LOD row was true duplication, not redundancy
for safety — exactly the two-owners-for-one-fact problem §2b exists to prevent. Nothing in RFC-004
depends on RFC-010 also carrying zone state, so removal is safe.

**Files/sections edited.** RFC-010 (§3 designer-tools table + prose, §4.4 tick order + authority
paragraph wording, §4.5 replication table, §4.7 LOD table + Invariant L-2 paragraph, §4.8 budget
line, §9 Open Question 1, Review Record applied bullet + unresolved + reconciliation line). RFC-004
verified consistent — no edit required beyond ruling 8's.

---

## Ruling 12 — IMPLEMENTATION_MAP.md's status table refreshed to match all ten RFCs' actual headers (AMENDMENT K)

**Conflict.** IMPLEMENTATION_MAP.md is a mid-run snapshot: its "detailed set" status table marked
RFC-002, 003, 006, 008, 009, and 010 as "Draft" and only 001/004/005/007 as "Accepted." Every one
of the ten files' own header now reads "Accepted (revised after review)" — the snapshot predates
the finalization pass entirely. RFC-010's Review Record flagged this staleness in its own
unresolved list ("IMPLEMENTATION_MAP's RFC-007/010 labels need a repo-wide renumber").

**Ruling.** The status column is refreshed to "Accepted (revised after review)" for all ten rows,
with a one-line note pointing to each RFC's own Review Record and to this file for the cross-RFC
arbitration that followed. No other content in IMPLEMENTATION_MAP.md (the umbrella-section
mapping, build order, absorbed-old-board list) required correction — it was checked against the
current RFC set and still holds.

**Rationale (evidence).** Criterion (1): a companion document that misreports every RFC's status
except four is actively misleading about what is safe to build against; correcting it to the
ground truth in each RFC's own header is not a judgment call.

**Files/sections edited.** IMPLEMENTATION_MAP.md (status table + refresh note). RFC-010's
unresolved bullet referencing this item closed under ruling 6/11's combined Review Record edit.

---

## Ruling 13 — README.md corrected where it no longer told the truth, and the reconciliation pass recorded in Review process (AMENDMENT L)

**Conflict.** Two README claims had gone stale under pass 1's own rulings: (a) the RFC-002 summary
row advertised "refractory windows instead of immunities," but ruling 3 (pass 1) rejected the flat
refractory mechanism in favor of the two-level soft-resist window — RFC-002's own body now uses
"refractory" only to name the *rejected* design; (b) the RFC-017 proposal row cited "the RFC-002
Q6 / RFC-009 Q2 calibration conflict" as a presently-unresolved disagreement, but ruling 4 (pass 1)
closed exactly that conflict (`kIceBoltPower = 600`, both Q6/Q2 closed). Separately, the Review
process section never mentioned that a second, cross-RFC reconciliation pass happened at all.

**Ruling.** (a) reworded to "a two-level soft-resist window instead of immunities," matching
RFC-002's canonical mechanism; (b) reworded to cite the same conflict as the *worked example* of
why RFC-017's harness is needed — resolved this time by an ad hoc series-editor pass, motivating a
repeatable procedure — rather than as an open contradiction. A new paragraph in the Review process
section states that finalizations ran concurrently, some pairs adopted each other's draft
positions, and a follow-up reconciliation pass arbitrated the result, linking RECONCILIATION.md.
The rest of the index (per-RFC status column, one-line summaries, build order, proposed-RFC list)
was checked line-by-line against the current RFC bodies and IMPLEMENTATION_MAP.md and found
accurate — no further edits.

**Rationale (evidence).** Criterion (1): an index that describes a rejected mechanism or a closed
conflict as current is the same class of defect as a dangling file reference (ruling 6) — it
misleads a reader about the state of the accepted spec set. Recording the reconciliation pass
itself in the Review process section is what the task directive for L explicitly asked for.

**Files/sections edited.** README.md (RFC-002 summary row, RFC-017 proposal row, Review process
section).

---

## Files edited by pass 2 (summary)

- **RFC-001** — header numbering note, Review Record bullet + Unresolved/Reconciliation (ruling 6).
- **RFC-002** — §8 aura prose ×2, Interactions RFC-004 row, Review Record Unresolved/Reconciliation
  (ruling 8).
- **RFC-003** — Review Record Unresolved bullet + Reconciliation line (ruling 7).
- **RFC-004** — header, `EntityDef` struct + archetype rows, Review Record (rulings 6, 8).
- **RFC-005** — verified consistent, not edited (ruling 10).
- **RFC-007** — header, Summary, Interactions (RFC-004 row), new Open Question 7, Review Record
  applied/unresolved/reconciliation (rulings 6, 9, 10).
- **RFC-008** — header numbering note, §7.5 aura prose, Review Record (rulings 6, 8).
- **RFC-009** — §4.6 table + floor/terminal-duration wording, Review Record (ruling 7).
- **RFC-010** — §3, §4.4, §4.5, §4.7, §4.8, §6, §7, §9 OQ1, Review Record (rulings 6, 11).
- **IMPLEMENTATION_MAP.md** — status table refresh (ruling 12).
- **README.md** — RFC-002/RFC-017 rows, Review process section (ruling 13).
- **RECONCILIATION.md** — this section.

Already-consistent, not edited beyond what is listed above: **RFC-006** (no dangling-file or
ownership-wording issues found in this pass's scope).

---

# Pass 3 — residual fixes (post-pass-2 verification)

> Scope: contradictions two independent verifiers found surviving pass 2. Each is fixed in the
> direction of the pass-1/pass-2 rulings already on file — none of those rulings are re-litigated
> here. One reported item was a duplicate (same line, two verifiers) and is resolved by a single
> ruling (15).

## Ruling 14 — RFC-003 §4's `kTierGain` echo corrected to match RFC-009 §4.6's actual table (residual of ruling 7)

**Conflict.** Ruling 7 confirmed RFC-009 §4.6 as sole owner of `kTierGain` (RFC-003 §4 deleted its
own intake column and cedes the value in its own body: "RFC-009 §4.6 solely owns build-up tier
scaling"). But RFC-003 §4's prose still *echoed* the pre-ruling-7 numbers — `1400/1150/1000/700/
500/350` ‰ — while RFC-009 §4.6's real table reads `1600/1300/1000/600/350/200` ‰. Only Medium
(1000) agreed; the other five tiers diverged. RFC-009's own gain-formula comment (§4.5) compounded
the confusion by mislabeling the constant as "RFC-003 §4 ... quoted in §4.6," backwards from the
ownership RFC-003 §4 itself now declares.

**Ruling.** RFC-009 §4.6's table is confirmed the load-bearing source: its worked "hits to Frozen"
example (Slime 975 / Ghost 450 / GiantFrog 360 / Samurai 210 / Titan 120) is script-verified
(RECONCILIATION.md ruling 2) and recomputes correctly only from `1600/1300/1000/600/350/200`
(re-verified this pass: `floor(600×1000×600/1e6) = 360` for GiantFrog's Large tier; RFC-003's
stale `700` would give `420`). RFC-003 §4's echo is corrected to `1600/1300/1000/600/350/200` to
match. RFC-009 §4.5's gain-formula comment is re-labeled to name itself (§4.6) as the source and
RFC-003 §4 as the echo, matching RFC-003's own ownership sentence instead of contradicting it.

**Rationale (evidence).** Criterion (2): RFC-009's numbers are the ones actually exercised by a
script-verified worked example; RFC-003's were an un-exercised, stale citation. Criterion (3): both
files already agree RFC-009 owns this value (RFC-003 §4's own sentence says so) — the fix is
conforming the copy to the declared source, not a new design decision, exactly the pattern ruling 7
already established for this same table's other drift (Squids, floor wording, terminal-duration
paraphrase).

**Files/sections edited.** RFC-003 (§4 `kTierGain` echo, Review Record reconciliation line).
RFC-009 (§4.5 gain-formula comment ownership label, Review Record reconciliation line).

---

## Ruling 15 — IMPLEMENTATION_MAP.md no longer names the retracted "BossObs v2" contract (residual of ruling 12)

**Conflict.** IMPLEMENTATION_MAP.md's umbrella row 17 (RL-friendly) still names "BossObs v2 per
RFC-007" as the deliverable to build. RFC-007 explicitly retracts that name: its Interactions
section and Review Record both state an early "BossObs v2" sketch (7×7 grid, 8-action enum,
`kBossObsVersion = 2`) "predates this RFC and is superseded in full: this RFC is the only F4 tensor
contract." RFC-007's actual, accepted contract is a 120-float observation vector (`kObsSize = 120`,
`kObsVersion = 1`) plus a 15-action space (`kActionCount = 15`) — never called "BossObs v2"
anywhere in its own text. Ruling 12's IMPLEMENTATION_MAP refresh (pass 2) touched only the RFC
status table, not this row, so the stale name survived into pass 2 untouched — reported
independently by two verifiers as the same line.

**Ruling.** Row 17's Gap column is reworded to name RFC-007's actual contract instead of the
retracted draft name: "RFC-007's 120-float observation vector (`kObsVersion`) + 15-action space
(`kActionCount`) replacing today's `BossObs`; versioned checkpoints." (`BossObs` — no version
suffix — is kept only in the "Engine today" column, where it correctly names the engine's current
placeholder function signature, not RFC-007's deliverable.)

**Rationale (evidence).** Criterion (1): a companion index pointing readers at a named/versioned
schema the owning RFC says does not exist is the same stale-reference defect ruling 6/13 corrected
elsewhere in this same document set. Criterion (3): IMPLEMENTATION_MAP.md is the only file with the
wrong name — RFC-007 already states its retraction clearly — so only the index moves.

**Files/sections edited.** IMPLEMENTATION_MAP.md (row 17, Gap column). RFC-007 unedited — confirmed
already conformant, its retraction language is exactly what the index should have deferred to.

---

## Ruling 16 — RFC-002's own Review Record bullet on the §3 terminal-exit rule no longer states the rejected 450 floor (residual of ruling 3)

**Conflict.** RFC-002 §3's normative terminal-exit rule sets `gauges[primary].value := 0` on exit.
RFC-002's own Review Record "Applied" bullet describing that same §3 change instead read "(exit
floor 450, through stage 1)" — 450 is not the current rule's value; it is the exact number RFC-002
§6 names, in its own text, as the mechanism ruling 3 rejected ("a flat refractory ... over a
post-trigger gauge floor of 450 ... a second Freeze cost `(900 − 450)/0.6 = 750`"). The neighboring
Review Record bullet on `soft_resist` deletion was already annotated mid-pass-2 as superseded; this
bullet, restating the same rejected number about the same rule, was missed.

**Ruling.** The bullet is corrected to "(gauge emptied to 0, through stage 1)," matching §3's actual
code and this same Review Record's later Reconciliation bullet ("terminal exit now empties the
gauge and exits through stage 1 ... flat refractory + 450 floor deleted").

**Rationale (evidence).** Criterion (1)/(2): this is not a new design call — ruling 3 already
settled the mechanism and rationale; the Review Record's own later bullet already states the
correct value. The earlier "Applied" bullet was simply never updated when the design changed out
from under it, the same class of drift ruling 7 fixed for RFC-009's tier-table echoes.

**Files/sections edited.** RFC-002 (Review Record "Applied" bullet on §3, new Reconciliation line).

---

## Files edited by pass 3 (summary)

- **RFC-002** — Review Record "Applied" bullet on §3 terminal-exit corrected; new Reconciliation
  line added (ruling 16).
- **RFC-003** — §4 `kTierGain` echo corrected; Review Record reconciliation line added (ruling 14).
- **RFC-009** — §4.5 gain-formula comment ownership label corrected; Review Record reconciliation
  line added (ruling 14).
- **IMPLEMENTATION_MAP.md** — row 17 Gap column reworded off the retracted "BossObs v2" name
  (ruling 15).
- **RECONCILIATION.md** — this section.

Verified as false positives / already consistent, no edit made: none — all four reported residuals
(one a duplicate of another) reproduced against the current files and were fixed above.

---

# Pass 4 — instance, vitals & persistence set (post-finalization check, 2026-07-24)

> Scope: RFC-013 (Vitals, Death & Recovery), RFC-014 (Instance & Realm Lifecycle), RFC-015 (Client
> Replication & Interest-Set Protocol), and RFC-016 (Persistence & Save-File Format) — checked against
> each other and against the already-accepted RFC-001..010/019..023 set for genuine content
> contradictions, not wording drift. RFC-014 was finalized first as the dependency root for the other
> three; this pass checks the four specific fault lines that dependency shape makes most likely to
> break, plus a general read of all four files' Interactions tables and schemas.

**Checked, no conflict found:**

1. **RFC-013's death/item-loss rule against RFC-019's/RFC-020's "never lose progress" tone-guardrail
   arguments.** RFC-013 §6.6 rules ejection clears only `items_[]` (loose carried resources) and
   explicitly never touches `xp_[]`/`level_[]`/unlocked abilities, arguing directly from RFC-019's own
   "specialization is a choice, not a life sentence" framing (RFC-019 Summary) rather than against it.
   RFC-020's "objectives only observe facts, never lock inventory or vitals" claim (cited by RFC-013
   §6.6) is preserved — quest state lives in neither `items_[]` nor the vitals pools RFC-013 governs, so
   ejection has no reach into it. No RFC in the accepted set promises carried *items* survive death —
   only progression — so RFC-013's item wipe does not contradict a guarantee anyone actually made.

2. **RFC-016's save-schema field shapes against RFC-019/RFC-020/RFC-008's already-committed data
   shapes.** RFC-016 §4.1's `level_melee/xp_melee/level_ranged/xp_ranged/level_magic/xp_magic/
   level_craft/xp_craft` columns match RFC-019's `Skill` enum ordering and internal naming exactly
   (`kMelee=0, kRanged=1, kMagic=2, kCraft=3` — RFC-019 §5.1; "Trades" is confirmed as `kCraft`'s
   *display* name only, not its identifier, so RFC-016 using `craft` rather than `trades` in the schema
   is correct, not a drift). The `equipped_ability_0/1` columns' comment ("reserved... loadout is
   auto-picked from `level_[]` until RFC-011 decides otherwise") matches RFC-019 §5.4's own text
   verbatim. RFC-016's `quest_instances`/`quest_objectives` schema (`account_id, quest_id, bound_entity`
   composite key) matches RFC-020 §6's emergent-quest instantiation model and its own QI2 "Abandoned/
   Expired instances are deleted, not archived" rule exactly (already the subject of this RFC's own
   Review Record fix, not reopened here). `player_items.item_kind` as an integer ordinal matches
   RFC-008's own stated convention that "the wire and the snapshot bus use `u16`" for item identity,
   string ids being a pack/authoring-side-only concern.

3. **RFC-015's replication cadence against RFC-006's telegraph-timing latency assumptions.** RFC-015 §8
   performs this check itself, explicitly and with arithmetic (a table of all four RFC-006 danger tiers
   against worst-case one-way replication delay), and finds every tier clears with a positive reaction
   window (tightest: tier 0 at 350 ms, flagged in RFC-015's own Open Question 2 as worth a dedicated
   look, not left as a silent risk). §4's hard exception — a telegraphing creature's `windup` field is
   never subject to outer-band throttling — exists specifically to keep that arithmetic true in
   implementation. No contradiction: RFC-015 already reconciled itself against RFC-006 before this pass
   started.

4. **RFC-014's boss-room-stays-persistent-band ruling against IMPLEMENTATION_MAP.md's/RFC-005's boss-
   authoring assumptions.** RFC-005 §"Replicated state" (its own §R8-adjacent persistence note) already
   states a mid-fight boss is not saved and "the only persisted boss facts are the dojo's checkpoint
   generation (RFC-007) and the room registration (worldgen)" — i.e., RFC-005 already assumed
   worldgen-time, persistent-band placement for the boss room, which is exactly what RFC-014 §8 rules.
   The two are independently consistent, not merely non-contradictory. One pre-existing wording
   imprecision was noted, not fixed: RFC-005's Interactions table attributes "multi-room/instance
   orchestration" to RFC-010, a label written before RFC-014 existed to claim that territory precisely.
   This is not a factual contradiction — RFC-014 does not dispute anything RFC-005 asserts about the
   boss room's own behavior, and RFC-005 makes no claim that depends on which RFC number owns future
   instance orchestration — so it is flagged here rather than ruled on, and left unedited: `IMPLEMENTATION_MAP.md`
   is outside this pass's editing scope, and RFC-005's own content requires no correction.

**Ruling: none required.** All four candidate fault lines were checked against the actual RFC text (not
assumed from summaries), and each resolves to already-consistent or self-reconciled, not contradictory.
No file edit was made by this pass beyond this section and the corresponding README.md update (moving
RFC-013/014/015/016 from "Proposed future RFCs" to their own finalized-set section).

## Files edited by pass 4

- **README.md** — removed RFC-013/014/015/016 from the Proposed future RFCs table; added "The
  instance, vitals & persistence set (finalized 2026-07-24)" section.
- **RECONCILIATION.md** — this section.

No RFC file required an edit — all four candidate contradictions checked out as already consistent or
already self-reconciled within the RFCs' own text.

---

# Pass 5 — HUD, audio, balance & recovery set (post-finalization check, 2026-07-24)

> Scope: RFC-011 (Combat HUD, Input & Cooldown UI), RFC-012 (Combat Audio & Sound Cue Standards),
> RFC-017 (Balance Tuning & Test Harness), RFC-018 (Loot, Essence & Reward Tables), and RFC-024
> (Leader Failure & Session Recovery — the first RFC filed outside the original reserved 001–023
> numbering) — checked against each other and against the already-accepted set for genuine content
> contradictions. Two of the four directed checks turned up nothing; the other two turned up a real
> gap each, both already self-flagged by the RFC that found them rather than hidden.

**Checked, no conflict found:**

1. **RFC-011's HUD data needs against what RFC-015 actually replicates.** RFC-011 was drafted, then
   corrected against the frozen `PublishedCreature`/`PublishedPlayerSelf` structs rather than the
   wider byte budget RFC-002 originally sketched: its own §4 answers RFC-002 Open Question 4 "within
   the actually replicated budget" (one `status` byte, not the two RFC-002's design assumed), and it
   opens a new **Open Question 3** of its own naming the RFC-002-vs-RFC-015 discrepancy explicitly as
   a follow-up for whoever next revises RFC-015's wire shape — not a silent mismatch. Every other
   field RFC-011 reads (`hp`/`max_hp`/`kind`/`windup`/`boss_pose` for the boss bar; the existing
   `status` byte for pips) is a field `PublishedCreature` (RFC-015 §2.2) already carries. No edit
   required — RFC-011 already reconciled itself against RFC-015's real shape before finalizing.

2. **RFC-017's proposed balance-conflict resolution procedure against how RECONCILIATION.md itself
   already operates.** RFC-017 §5 names "the existing informal role that already produced
   RECONCILIATION.md's sixteen rulings across four numbered passes" as the decision-maker, quotes
   this file's own three-criterion preamble verbatim as the evidence standard, states plainly it
   "does not replace the dual-model batch-review process" and "no relitigating Ruling 4 or any
   existing RECONCILIATION.md entry," and extends (never replaces) "the ledger's existing
   numbered-ruling format." Its two dual-model-debate carve-outs (a published Gate-passed RL
   checkpoint's dependency, or Gate B's 0.80 ceiling itself) add a threshold this file's rulings never
   needed before but do not contradict anything already on record. No edit required.

**Checked, genuine gap found — ruled below:**

3. Equipped-gear ejection exemption — RFC-013 vs. RFC-018 (Ruling 17).
4. The ≤30s post-incident disclosure vs. RFC-016's "never a player-facing number" rule — RFC-016 vs.
   RFC-024 (Ruling 18, closing RFC-024's own Open Question 8).

---

## Ruling 17 — equipped gear (`equipped_[]`) is exempt from RFC-013's ejection wipe, closing RFC-013 Open Question 2

**Conflict.** RFC-013 §6.5 rules ejection "clears `items_[]` to zero, in full," reading GAME.md §3's
"mất đồ mang theo" literally, and its own Open Question 2 flagged — before any equipment system
existed — that "when an equipment/durability system eventually ships... this RFC's ejection rule will
need revisiting to exempt worn gear the same way it already exempts XP." RFC-018 is that system: it
adds a new `equipped_[EquipSlot::kCount]` array (§4) alongside `items_[]`, entirely outside the loop
RFC-013 §6.5's code block zeroes, and states its own position in its Interactions table ("equipped
gear is explicitly not cleared by ejection") while explicitly declining to close RFC-013's open
question itself — deferring that to "RFC-013's editor." Left unruled, the two documents point at each
other without either committing: `equipped_[]` would be ejection-wiped by neither an explicit rule nor
an explicit exemption, an accidental gap rather than a decision.

**Ruling.** `equipped_[]` is exempt from ejection's wipe, matching RFC-018's own stated position and
closing RFC-013 Open Question 2 with that answer. `items_[]` continues to clear in full on ejection,
unmodified — RFC-013 §6.5's code and ruling text needed no change, since `equipped_[]` was never
inside the loop it describes; only the open question needed closing.

**Rationale (evidence).**
- *Internal soundness — the boundary RFC-013 itself drew.* RFC-013 already exempts XP/skill levels
  from ejection on a "worn, not carried" logic (progression is not a bag item); worn *gear* is the
  same category by the same logic RFC-013's own Open Question 2 anticipated, not a new argument.
- *Tone guardrail.* Durability (RFC-018 §4.1) is the mechanism this RFC already assigns to gear loss
  — a soft, player-caused, never-a-lockout curve. Adding a second, instant, all-or-nothing loss
  channel (ejection) for the same equipment would double-punish the same stake GAME.md §0 already
  argues against stacking, and would make a dungeon trip strictly worse for a geared player than an
  ungeared one for no stated reason.
- *Least churn.* No `src/` shape or RFC-013 normative text changes — `equipped_[]` living outside
  the `items_[]` loop is already true; this ruling only removes the ambiguity of an unanswered
  question sitting next to a shipped answer.

**Files/sections edited.** RFC-013 (Open Question 2, closed with this ruling's citation).
RFC-018 unedited — its own stated position is confirmed as canon, not revised.

---

## Ruling 18 — RFC-016 Tone Guardrail point 4 is narrowed to ordinary play; RFC-024's one-time post-incident disclosure is a stated exception, not a violation

**Conflict.** RFC-016's Tone Guardrail Compliance point 4 states, unconditionally: "The ≤60-second
bound (§6.3) is an internal engineering budget, never a player-facing number... under any
circumstance." RFC-024 §6's incident-explanation copy discloses "you might lose up to the last 30
seconds of movement" on demand after a leader-failure banner has already appeared — and RFC-024's own
recovery ledger (§4) traces that 30s figure to the exact same source: `kProgressionCheckpointIntervalTicks
= 300 ticks = 30s` (RFC-016 §6.3), "the number the ≤60s bar is actually about." RFC-024 §Tone Guardrail
Compliance point 4 argues at length that its situation differs in kind from RFC-016's (a one-time,
past-tense, post-incident disclosure the player already knows something is wrong when they read, vs.
an ambient caveat attached to ordinary play) but explicitly declines to resolve the tension
unilaterally, opening its own **Open Question 8** asking for reconciliation against RFC-016's authors.
Read literally, "under any circumstance" in RFC-016 forbids exactly what RFC-024 §6 does.

**Ruling.** RFC-016's point 4 is narrowed to what its own rationale was actually protecting against —
an ambient, ongoing caveat during normal play (matching its sibling point 1's identical framing for
the periodic-checkpoint cadence: "never displayed, to anyone, under any circumstance," said of a
running "last saved Ns ago" indicator). It does not reach RFC-024's on-demand, past-tense,
post-incident disclosure, which is the sole named exception. RFC-024's own honesty argument (§Tone
Guardrail Compliance point 4) is accepted as the resolution to its Open Question 8, not merely as an
unrebutted claim.

**Rationale (evidence).**
- *Hard constraint — GAME.md §0.* The guardrail's actual target is a countdown or caveat the player
  is made to track *during play* ("nothing counts down behind the player's back"). A single sentence
  answering a question the player has already asked, after an incident that already happened and is
  already visible via RFC-024's banner, is retrospective, not a countdown — it is closer to the class
  of things GAME.md's own tone welcomes (honest information on request) than the class it forbids
  (ambient pressure). RFC-016's own point 2 draws exactly this distinction for deletion ("exclusively
  player-initiated, explicit, and confirmed" is fine; unprompted/timed is not) — RFC-024's disclosure
  is player-initiated in the same sense (reached via an explicit "what happened?" link, §4).
- *Internal soundness.* Hiding the number here would not remove the drift, only the player's ability
  to understand what a "the leader is gone" banner already told them was true — worse for trust, not
  better, and inconsistent with RFC-024's own point 3 ruling ("never framed as the player's failure...
  states a fact"), which requires telling the truth about what happened.
- *Least churn.* One clause added to RFC-016's existing point 4; no renumbering, no change to §6.3's
  actual cadence or arithmetic, no change to RFC-024's already-written copy.

**Files/sections edited.** RFC-016 (Tone Guardrail Compliance point 4, narrowed with the named
exception). RFC-024 (Open Questions 8, closed with this ruling's citation) unedited beyond that one
line — its own argument stands as written.

---

## Files edited by pass 5

- **RFC-013** — Open Question 2 closed, citing Ruling 17.
- **RFC-016** — Tone Guardrail Compliance point 4 narrowed to ordinary play, citing Ruling 18.
- **RFC-024** — Open Question 8 closed, citing Ruling 18.
- **README.md** — removed RFC-011/012/017/018 from the Proposed future RFCs table (now empty, replaced
  with a short note); added "The HUD, audio, balance & recovery set (finalized 2026-07-24)" section
  covering RFC-011/012/017/018/024; corrected two now-stale "remain reserved by the proposed table
  below" sentences in the earlier world/progression and map/character sections.
- **RECONCILIATION.md** — this section.
