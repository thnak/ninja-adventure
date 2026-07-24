# RFC-017: Balance Tuning & Test Harness

> Status: Accepted (revised after review)
> Design canon: [GAME.md §0](../GAME.md) (chill is default, challenge is opt-in — the guardrail this
> RFC's gate check exists to keep mechanical), [GAME.md §10](../GAME.md) (RL difficulty-ceiling
> constraint — cited by number, not re-argued), [ARCHITECTURE.md §2c](../ARCHITECTURE.md) (the
> determinism boundary this RFC formalizes into a tool: what must match bit-exact between GCC/Linux
> and MSVC/Windows, and what must not)
> Touches accepted combat canon: [RFC-003](RFC-003-physics-material-interaction.md) Open Question 5
> (the sweep-harness proposal this RFC owns), §3.1 (material transmission), §5 (`Knockback =
> Impulse / Mass`), §8 (interaction rule engine); [RFC-009](RFC-009-damage-resistance-buildup.md)
> §4.3 (status-affinity table), §4.4 (the fixed-order damage formula), §4.6 (tier gain); [RFC-002](RFC-002-status-effect-framework.md)
> (build-up gauges the sweep exercises, thresholds already fixed by Ruling 2); [RFC-008](RFC-008-data-driven-skill-definition.md)
> §7.6 (`skill.*` documents — the sweep's payload catalog reads these, never a second authored copy);
> [RFC-007](RFC-007-rl-observation-action-space.md) §6.3 (Gate A/B state machine and checkpoint
> `.meta.json` sidecar — consumed verbatim, not re-specified)
> As-built source grounding: `CMakeLists.txt:46-50` (`mmo_sim` is a real, linked executable;
> `add_test(NAME mmo_sim_smoke COMMAND mmo_sim 600)` is the entire test surface today);
> `src/sim_main.cpp` (the shipped headless runner — deterministic fixed-step world, `argv[1]` parsed
> as a tick count, one long narrative of `Check::expect` assertions and `std::printf` rows; no flag
> parsing, no sweep/gate/dump mode of any kind exists in this file today); `rfc-spec/RECONCILIATION.md`
> Ruling 4 (`kIceBoltPower = 600`) and its documented decision criteria — the worked precedent this
> RFC generalizes, not re-decides
>
> **Numbering note.** RFC-001..010, RFC-013..016 and RFC-019..023 are accepted canon and are cited by
> number, never re-specified. RFC-011, RFC-012, RFC-018 and RFC-024 are sibling proposals drafted in
> the same batch as this document; per `IMPLEMENTATION_MAP.md`'s own adjacency note (scoped to the
> RFC-013..016 batch), RFC-012 and RFC-017 are not cited by RFC-013/014/015/016's Interactions
> sections at all — this RFC is self-contained tooling, not a dependency anyone else's spec is
> blocked on.

---

## Summary

`mmo_sim` is a real, shipped binary (`CMakeLists.txt:46-50`) — a deterministic headless world runner
that today does exactly one job: run `N` ticks, assert a long list of gameplay invariants inline, and
exit 0/1. That is its entire surface; `ctest` runs it once, as `mmo_sim_smoke COMMAND mmo_sim 600`,
and nothing else calls it.

This RFC extends that binary with three new, independent modes and pins one process:

1. **`--sweep`** — the scripted payload-vs-material sweep [RFC-003 Open Question 5](RFC-003-physics-material-interaction.md)
   proposed and left unowned. Prints an "effective-channel table": for each authored `skill.*`
   payload (RFC-008 §7.6) fired at a fixed reference dummy under each of the eight RFC-003 materials,
   the effective damage (RFC-009 §4.4), build-up gain (RFC-009 §4.3) and knockback (RFC-003 §5) —
   as plain, git-diffable text, so a re-tune's blast radius is visible in a code review, not felt
   after ship.
2. **`--determinism-dump`** — formalizes the cross-compiler bit-exactness check `ARCHITECTURE.md §2c`
   already describes informally (the P1 GCC-vs-MSVC worldgen match, and the one 266-tile drift bug
   that check caught). Packages exactly the category of number `ARCHITECTURE.md §2c` names as
   invariant into one normalized, diffable block, and states — normatively, not just descriptively —
   which numbers must never be compared (message-order-dependent counters, chiefly chunk migrations).
3. **`--gate-check`** — a mechanical PASS/FAIL reader for [RFC-007](RFC-007-rl-observation-action-space.md)
   §6.3's Gate A (win-rate ≥ 0.55 vs incumbent over 200 episodes) and Gate B (win-rate ≤ 0.80 vs the
   persona suite) against the checkpoint `.meta.json` sidecar RFC-007 already specifies. It runs no
   training and owns no RL infrastructure — it only applies arithmetic RFC-007 already defined, the
   same way every time, instead of by eyeballing a JSON file.
4. **A documented balance-conflict resolution procedure** — the RFC's core, code-independent
   contribution. It generalizes `RECONCILIATION.md` Ruling 4 (the `kIceBoltPower = 600` calibration
   fix that resolved a live RFC-002/RFC-009 disagreement) into a repeatable process: who decides, what
   evidence a ruling must attach (a harness-produced table wherever the harness can compute one), and
   what the ruling record must contain — continuing `RECONCILIATION.md`'s existing numbered-ruling
   ledger (currently at Ruling 16) rather than inventing a new one.

Honest scoping up front: modes 1 and 3 are specified against systems that are themselves accepted
spec but **not yet implemented in code** (`IMPLEMENTATION_MAP.md` rows 6–10 read "None" / partial for
materials, damage formula and build-up; RFC-007's training loop has no shipped checkpoint pipeline
either). Mode 2 formalizes a practice that **is** already proven in shipped code. The procedure is
process-only and requires no code at all. None of the four pins a single new balance number.

---

## Motivation

**`mmo_sim` is shipped, but it is only a smoke test.** `CMakeLists.txt:46-50` links a real executable
and registers exactly one `ctest` case running it for 600 ticks. `src/sim_main.cpp` is genuinely
useful today — every `Check::expect` in it is a real invariant, not a placeholder — but it has no
mode for anything other than "run and assert." Nobody can ask it "what does an Ice bolt do to a Metal
target compared to a Flesh one," or "did this re-tune change anything for Wood," without reading
printed narrative output and doing the arithmetic by hand.

**RFC-003 already asked for the sweep and explicitly left it unowned.** Its Open Question 5 reads,
verbatim: *"extend `mmo_sim` with a scripted payload-vs-material sweep that prints the
effective-channel table, so re-tunes diff as text (the worldgen determinism-check pattern). Needs an
owner; not blocking the spec."* The 8-material × 5-status-channel affinity table (RFC-009 §4.3)
crossed with the disputed 8×8 material×damage-channel matrix (RFC-003 §3.1 — ownership itself
unresolved, see Open Questions #1) and the seven-channel damage formula (RFC-009 §4.4) is too many
interactions to hold in one's head or tune "by feel" — RFC-003's own words. This RFC is that owner.

**RFC-007's Gate A/B protocol is a real measurement contract with no runnable implementation of the
contract itself.** RFC-007 §6.3 specifies exact thresholds and an exact checkpoint sidecar shape. What
it does not specify — deliberately, it is a non-goal there — is a small tool that reads the sidecar
and says PASS or FAIL. Today that arithmetic, if done at all, is done by a human reading a JSON file.

**The `kIceBoltPower` conflict is a worked example of a cost this project will pay again.** RFC-002
Open Question 6 and RFC-009 Open Question 2 briefly disagreed on Ice's calibration constant before the
series-editor reconciliation pass fixed it at 600 (`RECONCILIATION.md` Ruling 4). That pass worked —
but it worked as a one-time, ad hoc, whole-batch review exercise, not a documented, repeatable
procedure any single future conflict can invoke on its own. `RECONCILIATION.md` already has sixteen
rulings and four numbered passes; it is not a one-off artifact, it is a living ledger. This RFC is
the one thing missing: a written answer to "the next time two accepted RFCs disagree on a number, who
resolves it, what has to be shown, and where does the answer get written down" — so the next conflict
does not need its own bespoke reconciliation pass to get an answer.

---

## Guide-level Explanation

### A designer chasing a re-tune

You believe Rock's `kMuddyKnockbackScale` (illustrative name, RFC-009's territory) should change.
Before touching the constant you run:

```
$ mmo_sim --sweep > /tmp/before.txt
# edit the constant
$ mmo_sim --sweep > /tmp/after.txt
$ diff /tmp/before.txt /tmp/after.txt
```

The diff shows every payload/material cell the change actually moved. If a row you did not intend to
touch changed (say, Thunder-vs-Metal shifted because two channels share a formula term), you see it
in the diff before it ships, not after a playtester reports Metal enemies feel wrong three weeks
later. This is exactly the "worldgen determinism-check pattern" RFC-003 Q5 named: a plain-text table
that is meaningful to `diff`, checked into version control as a golden file the same way the worldgen
invariants already are.

### An engineer verifying a cross-platform build

You build the project fresh on Linux (GCC) and, separately, on a Windows box (MSVC — the project's
other supported PAL). You run:

```
$ mmo_sim --determinism-dump | diff - tests/determinism_golden.txt
```

on each. Both diffs come back empty, or neither does — the same discipline `ARCHITECTURE.md §2c`
already describes for worldgen (the exact one that caught a 266-tile road drift from a stray float
comparison during P1), now applied to combat/economy invariants and packaged as one command instead
of eyeballing narrative stdout.

### An RL engineer publishing a generation

Training finished; you have `boss_melee_bruiser.gen47.meta.json` (RFC-007's sidecar shape). You run:

```
$ mmo_sim --gate-check boss.melee_bruiser boss_melee_bruiser.gen47.meta.json
policy_id:             boss.melee_bruiser
generation:            47
vs_incumbent_winrate:  0.58   (Gate A: >= 0.55, PASS)
vs_persona_winrate:    0.63   (Gate B: <= 0.80, PASS)
episodes:              200    (>= 200 required, PASS)
generation_cap:        47/60  (PASS)
verdict:               PUBLISH
```

Numbers above are RFC-007's own worked example (its §6.2 sample sidecar), not new figures invented
here. The tool applies the gate arithmetic RFC-007 already specified; it does not decide policy, run
episodes, or manage the TRAINING → EVAL → PUBLISH state machine — that machine is RFC-007's, and stays
RFC-007's.

### Two RFC authors who disagree on a number, six months from now

Author A's draft and author B's shipped spec assign different values to the same named constant. Under
this RFC's procedure, whoever holds series-editor duty at the time: (1) quotes both sources
exactly, (2) if the value is harness-computable, attaches a `--sweep` before/after table or a
`--gate-check` report as evidence, (3) picks a resolution under `RECONCILIATION.md`'s existing decision
order (hard constraints first, mechanism soundness second, least churn last), and (4) appends
**Ruling 17** to `RECONCILIATION.md` in the format the first sixteen already established. No new
committee, no new document, no new vote process for the ordinary case — the two carve-outs where a
second reviewer is required are named in §5 below.

---

## Reference-level Design

### 1. `mmo_sim` CLI surface

Today, `argv[1]` is parsed unconditionally as a tick count (`std::atoi`, default `1200` if absent);
`add_test(NAME mmo_sim_smoke COMMAND mmo_sim 600)` depends on exactly that behavior and must keep
working unmodified. The three new modes are selected by checking `argv[1]` against a small fixed set
of flag strings **before** falling through to the numeric-ticks path, so `mmo_sim 600` is unaffected
and `mmo_sim --sweep` never reaches `std::atoi`:

```cpp
// argv[1] dispatch, illustrative (tunable exact flag spelling):
//   mmo_sim [N]                  → existing smoke-test behavior, unchanged
//   mmo_sim --sweep              → §2
//   mmo_sim --determinism-dump   → §3
//   mmo_sim --gate-check <policy_id> <meta.json path>  → §4
//   mmo_sim --gate-report <dir of *.meta.json>         → §4
```

All four modes reuse the **same** `World` construction and the **same** production combat/formula
functions the live 10 Hz simulation calls — never a parallel or simplified re-implementation. This
mirrors `sim_main.cpp`'s own stated purpose #3 ("it proves the render seam is honest — if anything in
`world/` had reached into a renderer, this would not link"): a sweep computed by a second, hand-copied
damage-formula implementation would be lying about what ships, and would rot the first time RFC-009's
real formula changed and the copy did not.

Each mode exits 0 on success (dump/report produced, gate PASSED) and a distinct non-zero code on
failure, so both are usable as `ctest` cases later — registering those `add_test` lines is an
implementation-time CMake change, out of this spec-only session's scope (see Non-goals), but the exit
contract is specified here so that change is a one-line addition when it happens.

### 2. `--sweep`: the payload-vs-material effective-channel table

**Status: green-field.** The system this mode sweeps — the `Material` enum, the RFC-003 §3.1
transmission table, and the RFC-009 §4.3/§4.4 damage formula — is accepted spec, not shipped code
(`IMPLEMENTATION_MAP.md` rows 6–10: materials "None," damage formula not built, build-up "binary stun
exists" only). This mode's contract is specified now so RFC-003/RFC-009's implementers wire it up
incrementally as each piece lands, rather than bolting a test harness on after the fact; it has
nothing to sweep until then, and should fail loudly (not silently print zeros) if invoked before the
material system exists.

**Payload catalog — read, never duplicated.** The sweep does not author its own set of test payloads.
It iterates the authored `skill.*` documents (RFC-008 §7.6, `data/combat/skills/*.skill.json`) — the
same disk contract the live game loads from — filtered to a fixed reference subset for v1: the basic
attack (light/heavy melee), the ranged basic (arrow), and the four element casts (Fire/Ice/Rock/
Thunder — the only four skill-books RFC-002 canon supports; `memory/fight-system-asset-constraints.md`
and Ruling 1 both fix this set). Boss-kit payloads (RFC-005) are an explicit future extension, not v1
(§ Open Questions). This is a design choice, not a limitation of convenience: a second, sweep-only copy
of channel amounts would be exactly the kind of duplicated source of truth RFC-008 exists to prevent.

**Reference dummy.** Every payload is evaluated against one fixed target, defined once so every cell
in the table is comparable to every other:

| Field | Value | Source |
|---|---|---|
| Scale tier | Medium (mass 100, tier gain 1000‰) | RFC-003 §4 / RFC-009 §4.6 |
| DR | 0 | reference — isolates the payload from a specific creature's armor |
| Toughness | 0 | reference |
| Existing build-up | empty on all five channels | reference — first-hit numbers, not a combo chain |
| Ring | Meadow (no ring HP/damage scaling) | GAME.md §4 / RFC-003 |
| Terrain / Friction | Grass, Friction 60 (`kb_terrain = 1000‰` by construction) | RFC-003 §5–§6 |
| Simulation cadence | 10 Hz (active LOD tier) only | see Multiplayer & LOD Considerations §below |

`kReferenceDummy` (name tunable) is a single constant the harness owns; it is **not** a new creature
archetype and is never spawned in the live game.

**Table shape.** One row per (payload id, material) pair, three value columns:

```
# mmo_sim sweep v1 (tunable format version)
# reference dummy: tier=Medium mass=100 dr=0 toughness=0 ring=Meadow buildup=empty cadence=10Hz
payload                material   dmg   buildup            knockback_tiles
skill.basic_melee_light Flesh      —     —                  —
skill.basic_melee_light Stone      —     —                  —
...
skill.ice_bolt          Flesh      —     cold:600           —
skill.ice_bolt          Stone      —     cold:300           —
...
```

Cell values are computed at run time from the authored `skill.*` amounts plus the current build of
RFC-003/RFC-009's formulas — this RFC specifies the pipeline, not the numbers (they are not this
RFC's to invent; `—` above stands for "computed," not "zero"). The pipeline, in order:

1. Read raw per-channel `amount`/`buildup` from the payload's `skill.*` document (RFC-009 §4.2's
   `DamagePacket` shape: `amount[7]` over Damage/Pierce/Crush/Explosion/Heat/Cold/Electric — RFC-009
   §4.1's channel table and its Review Record both fix this order — plus `impulse` and up to two
   build-up riders).
2. Apply the material coefficient table to the physical channels (RFC-003 §3.1 for impulse; the
   material×damage-channel matrix for the rest — **flagged as an unresolved ownership conflict that
   is live in both RFCs' current normative text, not a stale artifact: RFC-003 §3.1 states RFC-009
   owns the matrix, RFC-009 §4.2/§4.3 state RFC-003 §3.1 owns it; see Open Questions #1. The
   sweep must apply the matrix exactly once regardless of which file's prose is treated as
   authoritative — this is a correctness requirement on the implementer, not a new ruling this RFC
   makes**).
3. Run RFC-009 §4.4 Steps 1–5 (sum channels, scale by `M_outer`, apply DR complement stacking at the
   dummy's DR=0, apply flat reduction=0, apply the `kChipDamage` floor) to get effective damage.
4. Apply RFC-009 §4.3's status-affinity ‰ to any build-up riders to get effective build-up gain.
5. Apply RFC-003 §5's `Knockback = Impulse / Mass` law at the dummy's mass to get knockback tiles.

**Acceptance check this mode owes `RECONCILIATION.md` Ruling 4.** Ruling 2's worked table states the
intended commitment curve for `kIceBoltPower = 600`: Slime freezes in 1 cast, Medium in 2, Large in 3,
Giant Samurai in 5, Titan in 8. Once the sweep exists, running it against a Medium Flesh dummy with
`skill.ice_bolt` must reproduce "2 casts to Freeze" exactly — this is the concrete, harness-checkable
acceptance test the sweep owes the ruling that named it as a worked precedent, and the first thing its
implementation should verify.

### 3. `--determinism-dump`: formalizing the worldgen determinism-check pattern

**Status: formalizes an already-proven practice.** `ARCHITECTURE.md §2c` documents, as fact, that P1
proved worldgen matches "tile for tile" between GCC/Linux and MSVC/Windows, after catching a
266-tile road drift caused by one stray float comparison, and lists the exact category of number that
must (and does) match:

```
51 làng / 27 cứ điểm / 522 công trình · spawn (458,538) · 11 tally địa hình
619 con thú lúc bring-up · trận dàn dựng: 10 sinh vật, 8 thù địch, 2 watcher
máu slot 0 = 100, slot 1 = 36 · 18 nhát trúng / 22 bị từ chối · 8 mạng · 32 XP
```

— "51 villages / 27 strongholds / 522 buildings · spawn (458,538) · 11 terrain tallies · 619
creatures at bring-up · staged fight: 10 creatures, 8 hostile, 2 watchers · hp slot 0 = 100, slot 1 =
36 · 18 hits landed / 22 refused · 8 casts · 32 XP." Every one of these numbers is already printed by
`sim_main.cpp` today, scattered across narrative `std::printf` calls interleaved with `Check::expect`
output. This mode's entire job is to pull exactly this category of number into one normalized,
`grep`/`diff`-able block, and nothing else:

```
$ mmo_sim --determinism-dump
worldgen.villages=51
worldgen.strongholds=27
worldgen.buildings=522
worldgen.spawn=458,538
worldgen.terrain_tally.0=...
...
worldgen.terrain_tally.10=...
wildlife.bring_up=619
staged_fight.creatures=10
staged_fight.hostile=8
staged_fight.watchers=2
staged_fight.hp_slot0=100
staged_fight.hp_slot1=36
staged_fight.hits_landed=18
staged_fight.hits_refused=22
staged_fight.casts=8
staged_fight.xp=32
```

**The normative rule this mode enforces, not just documents.** `ARCHITECTURE.md §2c` explains *why*
one number is excluded — chunk migration counts — and the reasoning generalizes: any value whose
computation depends on cross-actor message arrival order (chunk A's `CreatureEnter` reaching chunk B
before or after B's own `Tick`, scheduler-decided) is **forbidden** from this dump, permanently, not
just today's migrations counter. `ARCHITECTURE.md §2c`'s own evidence for this is that the same binary,
on the same machine, produced 3407 then 3432 migrations across two runs — a value that varies run to
run on one machine can never be a cross-platform invariant, and belongs in `sim_main.cpp`'s existing
narrative output, never in this dump. A future contributor adding a line to `--determinism-dump` must
justify, in the commit, that the value is a pure function of `(seed, state, tick)` — the same
"integer-only and deterministic" ground rule `IMPLEMENTATION_MAP.md`'s header already states for every
new mechanic.

**Golden-file contract.** A golden file (illustrative path `tests/determinism_golden.txt`, tunable)
checked into the repo, produced once on the reference Linux/GCC build. `mmo_sim --determinism-dump |
diff - tests/determinism_golden.txt` must return empty on every supported PAL/toolchain combination
(today: GCC/Linux, MSVC/Windows). Wiring this into an actual CI job is out of this spec-only session's
scope (no build/CI files may be touched here) — this RFC specifies the contract an implementer wires
up, matching the same boundary §1 draws around `add_test`.

### 4. `--gate-check` / `--gate-report`: a mechanical reader for RFC-007's gates

**Status: green-field** (RFC-007's checkpoint pipeline itself is not yet built in code), **thin by
design**. This mode owns none of RFC-007 §6.3's TRAINING → EVAL → PUBLISH state machine, runs no
episodes, and trains nothing. It reads one artifact RFC-007 already fully specifies — the
`<checkpoint>.meta.json` sidecar (RFC-007 §6.2) — and applies arithmetic RFC-007 already fully
specifies (§6.3):

```
Gate A (progress):  vs_incumbent_winrate ≥ 0.55, episodes ≥ 200   (RFC-007 §6.3, tunable there)
Gate B (ceiling):   vs_persona_winrate   ≤ 0.80                    (RFC-007 §6.3, tunable there —
                                                                     GAME.md §10's mandatory difficulty
                                                                     ceiling, made mechanical)
Cap:                generation ≤ kGenerationCap = 60                (RFC-007 §6.3, tunable there)
```

`mmo_sim --gate-check <policy_id> <meta.json>` reads one sidecar and prints PASS/FAIL per gate plus an
overall verdict (`PUBLISH` / `HOLD`), exit code 0/1. `mmo_sim --gate-report <dir>` runs the same check
over every `*.meta.json` in a directory and prints one line per policy — its purpose is regression
scanning across the whole roster (RFC-007's 10–15 archetype policies, per GAME.md §6's "one policy per
archetype" ruling and RFC-007 §4), e.g. "list every published policy whose *current* sidecar would
fail Gate B" after an unrelated formula change shifted persona-suite results. This RFC does not
restate why 0.55/0.80/200/60 are the right numbers — RFC-007 already argues that, and GAME.md §10
already states the ceiling is mandatory; this mode exists solely so the check is run identically every
time instead of by a human reading JSON.

### 5. The balance-conflict resolution procedure

This is the part of the RFC that requires no code and is actionable immediately.

**Trigger conditions** (any one is sufficient):

1. Two accepted RFCs or rulings assign incompatible values to the same named constant or mechanic
   (`RECONCILIATION.md` Ruling 4's `kIceBoltPower` conflict is the precedent).
2. A tunable value fails the harness mechanically — a `--sweep` cell that violates a stated invariant
   (e.g. RFC-009 Invariant I4: no 0‰ affinity cell), or a `--gate-check`/`--gate-report` result that
   contradicts an RFC's stated design intent (e.g. Gate B passing only because the generation cap, not
   the ceiling, is what stopped training).
3. A concrete, specific playtest or chill-guardrail report flags a shipped number as wrong (out of
   scope for the harness to catch on its own; the procedure still applies).

**Who decides.** The existing informal role that already produced `RECONCILIATION.md`'s sixteen
rulings across four numbered passes (2026-07-23/24) — this RFC gives it a name, "series editor," but
creates no new title, committee, or headcount. Per `README.md`'s own stated position on process weight
for this team size ("a whole spec would be bureaucracy at this team size," said of a hypothetical
localization RFC), the default path is a single decision-maker acting on the evidence below — not a
standing board.

Two named carve-outs require the same dual-model (Opus + Sonnet) debate-and-vote the original ten- and
five-RFC batches used, because they carry cost beyond a single constant:
- The ruling would change a value a **published, Gate-passed RL checkpoint** already depends on
  (republishing costs a training/eval cycle, not just an edit).
- The ruling touches **Gate B's ceiling number itself** (0.80) or otherwise loosens the mandatory
  difficulty ceiling (GAME.md §10) — never approved on a single editor's say-so, exactly because it is
  the one mechanical guardrail standing between RL self-play and a "game that cannot be won" (GAME.md
  §10's own words for what an unbounded ceiling produces).

**What evidence is required.** Reused verbatim from `RECONCILIATION.md`'s own preamble, not
reinvented: *"(1) hard constraints and repo design docs (GAME.md / ARCHITECTURE.md / ROADMAP.md and
the pack asset audit) win; (2) internal soundness of the mechanism; (3) least churn across the...
files."* This RFC adds one binding requirement on top: **for any value the harness can compute, the
ruling must attach a harness-produced artifact** — a `--sweep` before/after table, or a
`--gate-check`/`--gate-report` result — as the evidence for criterion (2), not prose assertion alone.
Ruling 4 already did this informally (Ruling 2's worked freeze-count table is exactly this kind of
artifact); this codifies the habit as a requirement so future rulings cannot skip straight to
assertion. Where a value is not harness-computable (a purely qualitative FX-readability call, for
instance), the ruling must say so explicitly rather than silently omitting the artifact.

**What the ruling record must contain.** A new entry appended to `RECONCILIATION.md` — the ledger
*continues*, it does not get replaced — in the format its existing sixteen rulings already use:

```
## Ruling <N> — <one-line resolution> (<conflict tag>)

**Conflict.** <exact quoted text + file:line/§ from each conflicting source>

**Ruling.** <the resolved value/mechanism, stated as a single canonical position>

**Rationale (evidence).**
- <criterion 1: hard constraints / design docs>
- <criterion 2: mechanism soundness — MUST cite a --sweep or --gate-check/--gate-report
  artifact here if the value is harness-computable, or state explicitly why it is not>
- <criterion 3: least churn, only if 1-2 do not already decide>

**Files/sections edited.** <every file/section touched to conform>
```

Numbering continues sequentially from the ledger's current last entry (Ruling 16 as of this RFC's
2026-07-24 grounding audit) — a future ruling using this procedure is Ruling 17, not a new file.

**What the procedure explicitly does not do.** It does not reopen values `RECONCILIATION.md` has
already ruled on (`kIceBoltPower = 600` stays 600 unless a *new* conflict names it — this RFC does not
relitigate Ruling 4). It does not replace the dual-model batch-review process used to finalize whole
RFCs (`README.md`'s "Review process" section: each RFC finalized through Opus + Sonnet adversarial
debate, findings verified, then both reviewers vote) — that process governs whether an RFC is accepted
at all; this one governs a single number's drift after acceptance. It creates no standing board.

---

## Interactions with Other RFCs

- **RFC-003 (Physics & Material Interaction).** Owns the material enum, the impulse-transmission
  table (§3.1), the knockback law (§5), and is the direct source of Open Question 5, which this RFC
  closes by taking ownership of the sweep. This RFC specifies the sweep's pipeline and contract; it
  never restates or overrides a coefficient RFC-003 owns.
- **RFC-009 (Damage, Resistance & Effect Build-up).** Owns the fixed-order damage formula (§4.4) and
  the status-affinity table (§4.3) the sweep's "effective damage"/"effective build-up" columns compute
  through. This RFC flags (Open Questions #1), but does not resolve, a live contradiction between
  RFC-003 §3.1's current normative text and RFC-009 §4.2/§4.3's current normative text over which file
  owns the material×damage-channel matrix — a live candidate for this RFC's own resolution procedure,
  not something this RFC decides by fiat.
- **RFC-002 (Status & Effect Framework).** The build-up gauges, thresholds (Ruling 2: [0,1000],
  300/600/900), and the closed five-channel set (Ruling 1) are what the sweep's `buildup` column
  reports against; this RFC touches none of those numbers.
- **RFC-008 (Data-driven Skill Definition).** The sweep's payload catalog reads `skill.*` documents
  (§7.6) directly — it is a consumer of RFC-008's disk contract, never a second authored source of the
  same numbers.
- **RFC-007 (RL Observation & Action Space).** Supplies the Gate A/B thresholds and the checkpoint
  `.meta.json` sidecar shape this RFC's `--gate-check`/`--gate-report` consume verbatim. This RFC owns
  none of RFC-007's training loop, observation vector, action space, or generation state machine.
- **RECONCILIATION.md.** This RFC's resolution procedure extends the ledger's existing numbered-ruling
  format rather than replacing it; Ruling 4 is cited throughout as the one worked precedent being
  generalized, not re-decided.
- **RFC-011/RFC-012 (Combat HUD, Combat Audio — proposed siblings).** No interaction; per
  `IMPLEMENTATION_MAP.md`'s own adjacency note, RFC-012 and RFC-017 are not named by any RFC-013..016
  Interactions section, and this RFC introduces nothing either of them would consume.
- **RFC-018 (Loot, Essence & Reward Tables — proposed sibling).** No dependency in either direction as
  drafted, but the sweep's "authored data, one source of truth, deterministic table diff" pattern is a
  natural future fit for loot-table tuning (drop-rate-vs-tier sweeps) once RFC-018 lands — flagged as
  a future extension, not claimed here.
- **RFC-024 (Leader Failure & Session Recovery — proposed sibling).** No interaction. `--determinism-
  dump` is explicitly a developer/CI correctness tool run on trusted dev machines, never a runtime
  check performed on a live leader node — see Non-goals.

---

## Multiplayer & Simulation-LOD Considerations

- **This tooling never runs inside a live 20–50 player world.** All four modes are offline/dev/CI
  invocations of the headless `mmo_sim` binary; nothing here changes runtime behavior for a live
  leader or its peers.
- **The harness must call production code, never a parallel implementation.** §1 states this as a
  hard requirement, not a preference: if the sweep needed its own copy of the damage formula, it would
  silently drift from what actually ships the moment RFC-009's real implementation changed and the
  copy did not — the same failure mode `sim_main.cpp`'s own header explains for the render seam.
- **The sweep's reference dummy runs at the 10 Hz "active" LOD tier only (§2), by design, not by
  oversight.** Build-up gain math is tick-based (fill/decay per RFC-002); comparing a sweep run at
  10 Hz against one accidentally run at the 1 Hz "background" tier would report a spurious difference
  that has nothing to do with the constant under test. Whether the harness should *also* run a
  deliberate 1 Hz sweep — specifically to catch LOD-cadence-dependent bugs, a class of bug nothing
  currently tests for — is an open question (§ Open Questions), not part of this v1 contract.
- **The determinism-diff pattern exists because of multiplayer, not despite it.** `ARCHITECTURE.md
  §1` describes a cluster of peer machines — the leader plus friends' machines hosting chunks — which
  in practice may mix GCC/Linux and MSVC/Windows in one running world. Anything replicated between
  those nodes (chunk state, combat resolution) must already agree bit-for-bit; `--determinism-dump`
  formalizes the exact check that property depends on, it does not introduce a new requirement.

---

## Tone Guardrail Compliance

This RFC produces developer- and CI-facing tooling only; none of it is visible inside a player's
session, so GAME.md §0's pacing guardrail does not apply to the tool's *existence*. It does constrain
what the tool is permitted to certify:

- **Gate B's ceiling is load-bearing and this RFC does not relax it.** `--gate-check` reports Gate B's
  0.80 win-rate ceiling mechanically; it is GAME.md §10's mandatory difficulty ceiling made checkable,
  never a lever this RFC opens for renegotiation. §5's carve-out makes this explicit: a ruling that
  would loosen Gate B requires the full dual-model review, not a single editor's sign-off, specifically
  because loosening it risks the "game that cannot be won" GAME.md §10 names as the failure mode, not a
  legitimate difficulty option.
- **The harness is not, and must not become, a justification for escalating content over time.**
  Nothing in `--sweep` or `--gate-check` measures elapsed real time or player absence; both operate on
  static authored data and a point-in-time checkpoint. A ruling produced through §5's procedure that
  cited "the harness said it was fine" to justify content getting harder the longer a player is away
  would violate GAME.md §0 regardless of what the harness printed — the harness measures numbers, not
  intent, and a ruling still has to independently clear the chill guardrail on its own merits.
- **`--determinism-dump` is a correctness tool, not a fraud detector.** It exists to catch a stray
  float comparison producing divergent GCC/MSVC results (the P1 precedent), run by a developer on
  trusted dev/CI machines. It has no relationship to `Require<Trusted>`, the leader-election question,
  or any hostile-actor scenario — conflating the two would misapply this RFC into territory
  `ARCHITECTURE.md:170-171` explicitly reserves for "kick is enough," a boundary this RFC does not
  cross (see Non-goals, and contrast with RFC-024's explicit scope).

---

## Open Questions

1. **The RFC-003 §3.1 / RFC-009 §4.2–§4.3 ownership contradiction.** This is live in both RFCs'
   current normative bodies, not a stale artifact of either: RFC-003 §3.1 states plainly that
   "damage-channel material scaling has exactly one owner: RFC-009," citing a `kMaterialMult[8][8]`
   applied in RFC-009's Step 1 — but RFC-009 §4.2/§4.3 state the opposite today: "RFC-003 §8's
   `resolve()` applies its §3.1 material matrix... before RFC-009 sees the hit" and "this RFC defines
   no material×damage-channel table." RFC-009's Review Record independently restates the same
   RFC-003-owns-it position. Neither file was edited by this RFC (out of scope — this session may only
   touch `rfc-spec/`, but not re-open accepted RFCs without a ruling). Flagged here as the first
   concrete candidate for §5's resolution procedure, not resolved by it.
2. **Should the sweep's v1 payload catalog include boss kits?** RFC-005's ~11-boss shortlist has its
   own kit data (RFC-008 `boss.*` documents) with multi-skill loadouts; the v1 catalog here is
   deliberately limited to the six base player verbs. Expanding it is straightforward once boss kits
   are authored, but is not committed to a version here.
3. **Should the harness run a deliberate 1 Hz LOD-cadence sweep alongside the 10 Hz one**, specifically
   to catch build-up gain math that silently depends on tick cadence? Flagged in Multiplayer & LOD
   Considerations; not part of the v1 contract.
4. **Cross-platform CI is not itself specified here.** The `--determinism-dump` golden-file contract
   (§3) assumes *someone* runs it on both a Linux and a Windows build and diffs the result; whether
   that becomes an automated CI job, and where such a job would live, is an implementation-time
   decision this spec-only session cannot make (no build/CI files in scope).
5. **Does a Gate A/B regression caught by `--gate-report` (§4) need an automatic block on publish**, or
   is a human-read report sufficient? RFC-007's own publish path is unbuilt, so this is deferred to
   whichever RFC eventually specifies the training loop's integration points — not claimed here.

---

## Non-goals

- **No new balance number.** Every constant referenced in this RFC (`kIceBoltPower = 600`, Gate A/B's
  0.55/0.80/200/60) is cited from an existing accepted RFC or ruling, never re-derived or re-argued.
  This RFC pins zero new gameplay numbers.
- **No RL training infrastructure.** `--gate-check`/`--gate-report` read an artifact RFC-007 already
  specifies and apply arithmetic RFC-007 already specifies. Training loops, self-play, the
  TRAINING/EVAL/PUBLISH state machine, checkpoint storage/retention (RFC-016's territory) and the
  observation/action space (RFC-007's) are entirely out of scope here.
- **No CI or build-system implementation.** This is a spec-only session; `CMakeLists.txt` is not
  touched. §1's CLI contract, §3's golden-file contract, and the `add_test` registrations they imply
  are specified for an implementer to wire up, not built here.
- **No anti-cheat or hostile-node defense.** `--determinism-dump` is a correctness tool run by a
  developer on trusted machines. It performs no runtime check of a live leader, makes no claim about
  detecting a compromised or malicious node, and must not be read as a step toward one —
  `ARCHITECTURE.md:170-171`'s "kick is enough, Trusted is technical not defensive" stance governs that
  territory and this RFC does not touch it (see RFC-024 for the actual scope of leader-failure
  handling, which is about accidental crashes, not adversarial behavior).
- **No standing committee or new governance process.** §5 names the existing informal series-editor
  practice; it creates no board, no mandatory multi-reviewer sign-off outside the two named carve-outs,
  and no new document beyond continuing `RECONCILIATION.md`'s existing ledger.
- **No relitigating Ruling 4 or any existing `RECONCILIATION.md` entry.** `kIceBoltPower` stays 600;
  the five status channels stay `{Cold, Heat, Shock, Earth, Stagger}`; nothing already ruled on is
  reopened by this spec.
- **No loot, HUD, or audio content.** RFC-011, RFC-012, and RFC-018 own their respective territory in
  full; this RFC's only relationship to them is the "no interaction" and "future extension" notes in
  Interactions with Other RFCs, above.

---

## Review Record

Reviewer A: revise. Reviewer B: revise. Both upheld on final vote.

Applied:
- §2 step 1: fixed `DamagePacket` channel order to RFC-009 §4.1/Review Record's Damage/Pierce/Crush/Explosion/Heat/Cold/Electric.
- §2 step 2, §Interactions (RFC-009), Open Question 1: fixed broken "Open Questions #3" pointers to #1; reworded to state the RFC-003/RFC-009 conflict is live in both RFCs' current normative text, not a stale Review Record artifact.
- Motivation: fixed "8×8 material/status-affinity table" to correctly describe the settled 8×5 status-affinity table (RFC-009 §4.3) vs. the separate, disputed 8×8 material matrix (RFC-003 §3.1).
- §2 reference-dummy table: added a pinned Terrain/Friction row (Grass, Friction 60) so `knockback_tiles` is reproducible.
- §4: fixed "GAME.md §10's one policy per archetype" to §6 (correct section).
- Tone Guardrail Compliance and Non-goals (both instances): fixed `ARCHITECTURE.md:180-181` to the correct `:170-171`.
- Numbering note: narrowed the IMPLEMENTATION_MAP.md adjacency claim to the RFC-013..016 batch it actually covers, matching the (already-correct) Interactions-section bullet.
- §2 step 3: extended "RFC-009 §4.4 Steps 1–4" to "Steps 1–5" to include the `kChipDamage` floor.
- Header: Draft → Accepted (revised after review).

Unresolved: none — all mustFix items from both reviewers were applied.

