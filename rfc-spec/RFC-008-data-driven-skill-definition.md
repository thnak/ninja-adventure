# RFC-008: Data-driven Skill Definition (JSON)

> Status: **Draft**
> Umbrella: [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §13 (Skill Composition)
> Scope: the serialization contract — file format, schemas, versioning, validation, referencing,
> hot-reload — for skills, statuses, combat entities, FX/icon/sound references, and boss kits.
> Runtime *semantics* of these documents belong to RFC-001/002/004/005/009 and are only referenced
> here by number.

---

## Summary

Every other combat RFC ultimately serializes into this one. RFC-008 defines:

- **Format:** strict JSON (not YAML), authored as small per-document files under `data/combat/`,
  compiled at build time by `tools/build_combat_pack.py` into one canonical, hashed
  `assets/_gen/combat_pack.json` that the sim, the trusted leader, and the renderer all load.
- **Numbers:** integers only — ticks, millitiles, permille. No floats anywhere in data files, which
  makes the canonical byte form (and therefore the content hash every node must agree on) trivial.
- **Identity:** dotted string ids (`skill.meteor`) plus an append-only `ids.lock.json` that pins
  each id to a stable `u16` for the wire protocol and for RL action/observation stability (RFC-007).
- **Structure:** a skill is a fixed seven-phase pipeline
  (`cast → channel → release → travel → impact → persist → expire`, umbrella §1) expressed as a
  declarative object — no scripting, no arbitrary graphs.
- **Two hashes:** a *structural* hash (ids, shapes, phase presence) and a *value* hash (everything).
  Hot reload and RL-checkpoint compatibility are defined in terms of which hash changed.
- **Worked examples:** Meteor and Spike authored fully in data, against real assets in this repo.

The existing constexpr tables (`abilities.hpp` ability table, `tiles.hpp` status/effect tables,
`boss.hpp` boss tuning) are the *predecessors* of this contract; their guarantee — "all three
readers agree to the last integer" — is preserved by a single build artifact plus hash agreement at
cluster join, instead of a single header.

---

## Motivation

Three forces converge:

1. **The umbrella's core promise** — "adding a new boss or skill is primarily data configuration —
   no new systems required" (umbrella, Core Design). Today adding an ability means editing a
   constexpr C++ table (`ability_def()` in `src/world/abilities.hpp`) and recompiling every binary.
   That was the right call for six abilities; it does not scale to the skill/status/entity/boss-kit
   surface that RFC-001…RFC-010 define, and it makes every balance change a C++ change.

2. **The agreement problem gets harder, not easier, with data.** The header-table argument in
   `abilities.hpp` is real: the trusted `PlayerActor` checks-and-debits, the untrusted chunk applies
   the resolved shape, the renderer greys the unaffordable slot — and all three must agree exactly.
   Moving to data files must not silently break this. The answer here is a *single canonical
   artifact with a content hash*, verified at process start and at cluster join. A node with a
   different pack cannot join (ARCHITECTURE.md: leader is the single source of truth).

3. **Known engine gaps are serialization gaps.** `kEffectLife=6` truncated the 14-frame Rock strip
   precisely because effect lifetime was a constant instead of data carried with the FX reference
   (since fixed by `effect_life_of()`, but the fix is another hand-maintained table). The audit also
   found melee playing `harvest.wav` because the skill→sound link exists only as scattered code.
   A schema in which a skill *must* reference an FX document that *carries* its frame count, and a
   sound document that *resolves* at build time, turns both classes of bug into build failures.

### Why this RFC exists separately from RFC-001

RFC-001 says what an ability *does* at runtime; RFC-008 says what an ability *is on disk*. Two
engineers implementing RFC-001 against this spec must produce byte-identical packs from the same
sources and reject the same invalid documents. Everything needed for that — canonical form, units,
id assignment, validation rules — is here and nowhere else.

---

## Guide-level Explanation

### For a designer

You add a skill by creating one small JSON file:

```
data/combat/skills/meteor.skill.json
```

In it you describe the skill as phases: how long the wind-up is, what telegraph circle appears on
the ground, what falls out of the sky, what happens on impact, what stays behind. Every visual is a
reference like `"fx": "fx.rock_fall"`, every icon `"icon": "icon.book_rock"`, every sound
`"sound": "snd.explosion_heavy"` — names that point at other small JSON documents which in turn
point at real files in the asset pack. You never type a pixel coordinate or a frame count in a
skill; the FX document owns those.

Then you run:

```
python3 tools/build_combat_pack.py
```

If you referenced an icon that doesn't exist, gave a monster a cast pose (monsters have no attack
animations — the tool knows), used the element `water` (not in v1), or wrote `1.4` instead of
`1400` permille, the build fails with a message naming your file, your line, and the rule you broke
(rules are numbered — "V12: skill.meteor phases.impact.radius_mt exceeds 8000"). If it passes, the
game picks it up; in a dev session with `--dev-data`, a running world picks up *tuning* changes
within a couple of seconds without restarting.

You never edit two places. The disabled/cooldown icon comes for free (the pack ships a `*Disabled`
twin for all 121 skill icons and the tool resolves it by name). The wire id comes for free
(appended to `ids.lock.json` automatically, and then frozen forever).

### For a player

Nothing in this RFC is visible directly — but its guarantees are: the telegraph circle you see is
exactly the circle the server tested, because both were deserialized from the same hashed pack. And
the tone guardrail (GAME.md §0) is structurally honored: **the schema has no wall-clock field and
no world-scheduling field.** All timing is sim ticks inside a fight the player started. There is no
document type through which combat data can put a countdown behind the player's back.

### For an engineer

You load one file, once, into an immutable `std::shared_ptr<const CombatPack>` — the same
publish-immutable-snapshot pattern the flow field already uses (ARCHITECTURE.md §5). Lookups are
array-indexed by the `u16` from `ids.lock.json`. You never parse per-document files at runtime; the
Python packer is the only parser of authored files, and the C++ loader parses only the canonical
pack (one code path, one grammar).

---

## Reference-level Design

### 1. Format decision: strict JSON, not YAML

| Criterion | JSON | YAML |
|---|---|---|
| Parser already in the stack | **yes** — RLDrive checkpoints are JSON (`NetworkCheckpoint`), `assets/tile_index.json` exists, Python packer is stdlib `json` | no — new C++ dependency (libyaml/rapidyaml) in an engine that vendors single-file libs on principle (Monocypher) |
| Canonical byte form for hashing | trivial (sorted keys, no whitespace) | hard (anchors, flow vs block, quoting styles) |
| Implicit-typing footguns | none | `no` → false, `0x1F` → int, version `1.10` → float (the Norway problem) |
| Diffable in review | good | good |
| Comments | no (see mitigation) | yes |

**Decision: strict JSON everywhere** (authored files and generated pack). YAML's only real
advantage — comments — is recovered two ways: every document may carry a free-text `"notes"` field
(stripped from the canonical pack, so it never affects the hash), and the documents are small
enough (one skill per file) that the filename and `notes` carry authorial intent. No JSONC, no
JSON5: a second grammar is a second parser and a second canonicalization argument.

Authored-file constraints (enforced by the packer, rule V01):

- UTF-8, no BOM. Strings NFC-normalized before canonicalization.
- Top level is one object (one document per file).
- **No floats**: any number with a `.`, `e`, or `E` is a build error (V02). See §3.
- No `null`, no duplicate keys (V03). Optional = absent, never null.
- Max authored file size 64 KiB (V04) — a skill that big is a design smell, not a limit problem.

### 2. Directory layout and document kinds

```
data/combat/
  pack.json                     # pack manifest: schema version, pack name
  ids.lock.json                 # append-only id ledger (committed, machine-edited)
  skills/<name>.skill.json      # domain "skill"
  statuses/<name>.status.json   # domain "status"
  entities/<name>.entity.json   # domain "entity"   (CombatEntity archetypes, umbrella §3)
  bosses/<name>.boss.json       # domain "boss"     (boss kits, RFC-005)
  fx/<name>.fx.json             # domain "fx"       (sprite-strip descriptors, RFC-006 visuals)
  icons.json                    # domain "icon"     (icon name -> source PNG)
  sounds.json                   # domain "snd"      (sound name -> wav path list)
  capabilities/boss_poses.json  # measured pose capabilities per boss sheet (from the asset audit)
```

Six referenceable domains: `skill`, `status`, `entity`, `boss`, `fx`, `snd`, plus `icon`. A
document's id must be `<domain>.<file-stem>` (V05): `skills/meteor.skill.json` must contain
`"id": "skill.meteor"`. Ids are `[a-z0-9_]+` per segment, max 48 chars total (V06).

`capabilities/boss_poses.json` is *measured data checked in from the 2026-07-23 audit*, not
authored opinion: for each of the 20 boss sheets, which poses actually exist
(`idle/walk/attack_l/attack_r/charge_l/charge_r/shoot/trans/hit`). The validator uses it (V20–V22);
humans update it only by re-running the audit tool.

### 3. Units: integers only

Every number in every document is an integer in a declared unit. This is not a style preference —
it is what makes the canonical form (and thus the cross-node hash) unambiguous, and it matches the
codebase's existing habit of fixed-point at boundaries (`BossObs::hp_frac` is 0..1000).

| Suffix | Unit | Meaning | Range (V07) |
|---|---|---|---|
| `_ticks` | sim ticks | 10 Hz sim tick (100 ms) | 0..65535 |
| `_mt` | millitiles | 1/1000 of a 16 px tile; `2200` = 2.2 tiles | 0..32000 |
| `_pm` | permille | multiplier ×1000; `1400` = ×1.4 | 0..100000 |
| `_deg` | degrees | angles, fan spreads | 0..360 |
| `damage`, `hp`, `cost` | points | integer vital points | 0..32767 |
| `weight` | relative weight | boss action selection etc. | 1..1000 |
| `tint` | `[r,g,b,a]` | 0..255 each | — |

Field names must carry the suffix when a unit applies (V08) — a reviewer can spot a wrong unit in
a diff without opening the schema. All values are **(tunable)** unless a validation rule pins them.

The C++ loader converts at load time to whatever the sim wants (floats for positions, as
`tiles.hpp` documents) — the *data contract* is integer; the *runtime representation* is the sim's
business.

### 4. Identity and `ids.lock.json`

String ids are for humans and references; the wire, the snapshot bus, and RL tensors use `u16`.

```json
{
  "schema": 1,
  "next": {"skill": 12, "status": 9, "entity": 7, "boss": 4, "fx": 31, "snd": 18, "icon": 14},
  "assigned": {
    "skill.meteor": 10,
    "skill.spike": 11,
    "entity.rock_spike": 5
  },
  "retired": {"skill.old_test_nova": 3}
}
```

Rules:

- The packer assigns the next free number to any new id and rewrites the lock file; the rewrite is
  part of the commit that adds the document. **Numbers are never reused** (V09): deleting a
  document moves its entry to `retired`; the number stays burned. This is what keeps old RL
  checkpoints, old replays, and old client builds from silently reinterpreting a slot.
- Each domain has its own `u16` space. `0` is reserved as "none" in every domain (V10).
- Id space partition **(tunable)**: `1..49999` base game, `50000..65535` reserved for per-realm
  packs (GAME.md §3, one asset pack per realm) — future work, reserved now because it is free to
  reserve and expensive to retrofit.
- Manual edits to `ids.lock.json` are a build error unless they only add `retired` entries (V11) —
  the packer verifies `assigned ∪ retired` is dense-consistent with `next`.

### 5. Canonical form and the two hashes

The packer emits `assets/_gen/combat_pack.json` in **canonical form**:

1. One top-level object; all documents inlined under their domain, keyed by string id, sorted
   byte-lexicographically at every object level.
2. No insignificant whitespace. Integers in base 10, no leading zeros, no `+`.
3. `notes` fields stripped. Absent optionals stay absent (defaults are applied by the loader from
   the schema, *never* materialized into the pack — otherwise adding a default changes every hash).
4. UTF-8, NFC.

Two SHA-256 hashes are computed and embedded in a sibling `combat_pack.hash.json` (not inside the
pack — a file cannot contain its own hash):

- **`struct_hash`** — hash of the canonical pack with every *value-class* field replaced by `0`.
  Value-class fields are exactly those with a unit suffix from §3 plus `damage/hp/cost/weight/tint`.
  What remains: the set of ids, the numeric id assignments, which phases each skill has, every
  reference edge, every enum-valued field (element, shape kind, pose, material…).
- **`full_hash`** — hash of the canonical pack as-is.

Interpretation, used by §10 (hot reload) and §11/RL:

| Change | `struct_hash` | `full_hash` | Meaning |
|---|---|---|---|
| Retune Meteor damage 45→50 | same | differs | balance patch; hot-reloadable; RL fine-tunes |
| Add a phase, add a skill, change an element | differs | differs | structural; restart + RL re-eval required |

### 6. Schema versioning

- `pack.json` carries `"schema": {"major": 1, "minor": 0}`.
- **Minor** bumps add optional fields with defaults. The packer *rejects unknown fields* (V12) —
  strictness at author time is how typos die — so an authored file using a new optional field
  requires a packer at least that minor. The C++ loader accepts any pack with its known major and
  minor ≤ its own (it can't see fields it doesn't know because defaults are never materialized;
  a loader older than the pack's minor refuses, since silently ignoring unknown data is how two
  nodes disagree while both "loading successfully").
- **Major** bumps may rename/restructure. Each major bump ships `tools/migrate_combat_vN.py` that
  rewrites authored files in place; `ids.lock.json` survives majors untouched (id stability
  outlives schema shape).
- The pack's `(major, minor, struct_hash, full_hash)` 4-tuple is exchanged in the cluster join
  handshake; any mismatch with the leader refuses the join (ARCHITECTURE.md: leader is truth). No
  negotiation, no partial compatibility — with a 20–50 player friends-server, "restart with the
  same build" is the correct simplicity.

### 7. Document schemas

Notation: `?` = optional. Every document may carry `"notes": "<string>"` (stripped, §5). Every
document carries `"schema": 1` (the major; V13 requires it to match `pack.json`).

#### 7.1 FX document (`fx.*`) — the visual atom

Owns everything about one sprite strip. RFC-006 owns *how* telegraphs/strikes must read on screen;
this schema is the container it standardizes into.

```json
{
  "schema": 1,
  "id": "fx.rock_fall",
  "sheet": "ninja/FX/Elemental/Rock/SpriteSheet.png",
  "frames": 14,
  "ticks_per_frame": 1,
  "anchor": "center",
  "tint?": [255, 96, 64, 255],
  "loop?": false,
  "flip_h?": false,
  "sound?": "snd.meteor_fall"
}
```

- `sheet` is a path under `assets/_src/`, resolved by `tools/build_atlas.py` into an atlas strip;
  V14: the file must exist at pack-build time and its frame grid must divide evenly into `frames`.
- **Lifetime is derived, never authored twice**: `life_ticks = frames × ticks_per_frame`, and V15
  requires `life_ticks ≤ 255` (the sim ages effects in a `u8`). This retires the `kEffectLife`
  class of bug permanently: `effect_life_of()`'s hand-mirrored table is replaced by pack data, so a
  14-frame Rock strip *cannot* be truncated by a constant someone forgot.
- `loop` may be true only for FX referenced from `persist` phases or entity `render` (V16) — a
  one-shot impact flash that loops is a rendering bug expressed as data.
- `tint` is the umbrella-§14 reuse lever: `fx.rock_fall` is the plain Rock strip tinted hot;
  a cursed variant is a new 8-line FX document, zero new art.
- `anchor ∈ {center, ground}`: `ground` renders the strip bottom-aligned at the tile (spikes,
  pillars); `center` at the position (explosions, projectiles).

#### 7.2 Sound map (`sounds.json`, domain `snd`)

```json
{
  "schema": 1,
  "sounds": {
    "snd.explosion_heavy": {"files": ["ninja/Audio/Sounds/Elemental/Explosion2.wav",
                                       "ninja/Audio/Sounds/Elemental/Explosion3.wav"],
                             "gain_pm": 900},
    "snd.rock_break":      {"files": ["ninja/Audio/Sounds/Hit & Impact/Hit3.wav"], "gain_pm": 1000}
  }
}
```

V17: every listed file exists. Multiple files are cosmetic variation; the client picks
`files[effect_seed % n]` where `effect_seed` is the published effect's `(x,y,tick)` hash — sound is
presentation, so cross-client variation divergence is acceptable *and* this rule still makes two
clients watching the same fight usually hear the same take. This section also pays the audit's
audio debt: skills reference sounds by name, so "melee plays `harvest.wav`" becomes impossible to
recreate without writing it down in a reviewed file.

#### 7.3 Icon map (`icons.json`, domain `icon`)

```json
{
  "schema": 1,
  "icons": {
    "icon.book_rock":    "ninja/Ui/Skill Icon/Spell/BookRock.png",
    "icon.book_fire":    "ninja/Ui/Skill Icon/Spell/BookFire.png",
    "icon.book_ice":     "ninja/Ui/Skill Icon/Spell/BookIce.png",
    "icon.book_thunder": "ninja/Ui/Skill Icon/Spell/BookThunder.png"
  }
}
```

V18: the PNG exists **and** its `*Disabled.png` sibling exists (all 121 pack icons have one — the
cooldown state comes free, and the validator makes sure it stays free). The renderer derives the
disabled atlas entry by name; no schema field for it.

#### 7.4 Status document (`status.*`)

The serialized shape of RFC-002's status framework; build-up/decay *semantics* and scale-tier
multipliers are RFC-002/RFC-009's — this schema is where their parameters live.

```json
{
  "schema": 1,
  "id": "status.rooted",
  "buildup_max": 100,
  "decay_per_s": 20,
  "duration_ticks": 30,
  "speed_pm": 0,
  "dot?": {"damage": 0, "period_ticks": 0},
  "tint": [150, 100, 60, 120],
  "overlay_fx?": "fx.root_debris",
  "stacks_with?": []
}
```

- `decay_per_s` (points of build-up lost per second) rather than per-tick, so a chunk waking from
  sleep computes `buildup = max(0, buildup - decay_per_s * elapsed_s)` in closed form — the LOD
  rule of §11 applied to statuses. V19: every status must have `decay_per_s ≥ 1` (no absolute
  immunities *and* no permanent marks; umbrella §9).
- `tint` is the umbrella-§15 filter: statuses render as a sprite tint (the shipped P2 combo system
  already established this), `overlay_fx` optionally adds a looping overlay (frost, arcs).
- The current five statuses (`Frozen/Burning/Wet/Muddy/Shocked` in `tiles.hpp`) migrate into five
  documents; `status_ticks_of`/`status_speed_scale` become pack data.

#### 7.5 Combat entity document (`entity.*`)

The umbrella-§3 checklist (HP, Lifetime, Collision, Aura/Status, Destroyable, Team, Tags) is this
schema, field for field. Runtime behavior is RFC-004's.

```json
{
  "schema": 1,
  "id": "entity.rock_spike",
  "material": "stone",
  "scale": "small",
  "hp": 30,
  "lifetime_ticks": 300,
  "collision": "blocks_ground",
  "team": "caster",
  "destroyable": true,
  "tags": ["rock", "obstacle"],
  "aura?": {
    "radius_mt": 600,
    "period_ticks": 5,
    "statuses": [{"id": "status.rooted", "buildup": 40}]
  },
  "render": {"fx": "fx.rock_spike_idle", "anchor": "ground"},
  "on_death?":  {"fx": "fx.rock_break", "sound": "snd.rock_break", "spawn": []},
  "on_expire?": {"fx": "fx.rock_crumble"}
}
```

- `material ∈ {flesh, stone, spirit, metal, wood, plant, water, slime}` (umbrella §8);
  `scale ∈ {tiny, small, medium, large, giant, titan}` (umbrella §10). Both are closed enums (V23);
  their *effects* (knockback resistance, build-up scaling) are RFC-003/RFC-009 formulas keyed off
  these fields.
- `collision ∈ {none, blocks_ground, blocks_all, blocks_projectiles}`.
- `team ∈ {caster, monster, player, neutral}` — `caster` means "inherits the spawner's team", which
  is how the same spike document serves a player and a boss.
- **Replication shape (why this stays cheap):** a live entity on the wire/snapshot is exactly
  `(entity_u16, x, y, spawn_tick, hp)` — everything else is derived from the pack, which every node
  holds byte-identically (§5/§6). Five fields per spike is what "combat state must be replicable
  and cheap" serializes to.
- **Sleep rule (V24, the LOD contract):** `lifetime_ticks` is absolute — expiry is
  `spawn_tick + lifetime_ticks` in *world* ticks, so an entity in a slept chunk expires correctly
  on wake with zero catch-up work. Auras apply only on ticks the chunk actually runs (at 1 Hz a
  spike roots less often; nobody is there to care — that is the design, not a bug). No entity field
  may express per-tick accumulation that would need replaying missed ticks; the validator rejects
  any `period_ticks` of 0 with a non-empty status list.

#### 7.6 Skill document (`skill.*`) — the phase pipeline

The umbrella-§1 pipeline is a **fixed linear machine**, not a free graph:

```
cast ──► channel? ──► release ──► travel? ──► impact ──► persist? ──► expire
  │          │                       │
  └──────────┴───────────────────────┴────────── interrupt ──► expire
```

Schema constraints (V25): `cast`, `release`, `impact` are mandatory; `channel`, `travel`, `persist`
optional; no phase appears twice; nothing else is a phase. Interrupt/refund *semantics* are
RFC-001's; destroying a `travel` body (shoot the meteor down, umbrella §12) is RFC-004's; the
schema only guarantees the shapes exist to hang those rules on. A fixed pipeline is a deliberate
anti-Turing-tar-pit stance: two engineers cannot diverge on graph evaluation order because there is
no graph.

```json
{
  "schema": 1,
  "id": "skill.example",
  "name": "Example",
  "element": "rock",
  "icon": "icon.book_rock",
  "tags": ["magic", "aoe"],
  "player?": {
    "pose": "ability2",
    "cost": {"vital": "mana", "amount": 35},
    "cooldown_ticks": 200,
    "unlock": {"school": "magic", "level": 6}
  },
  "phases": { "cast": {}, "release": {}, "impact": {} }
}
```

Top-level rules:

- `element ∈ {none, fire, ice, rock, thunder}` — **closed** (V26). `plant/water/light/darkness/
  wind/death` are icon-only in the asset pack and rejected in v1; a future minor may widen the enum
  (that is a `struct_hash` change, correctly forcing re-evaluation everywhere).
- `player` present ⇔ the skill is player-castable. `pose ∈ {attack, ability1, ability2}` (V27) —
  the 89 character rigs have exactly rows for these three; there is no third ability row, so the
  schema cannot express a third equipped ability. The two-slot kit (basic + 2, `kAbilitySlots = 2`
  in `abilities.hpp`) is thereby enforced at the data layer, not just the UI.
- `cost.vital ∈ {stamina, mana}` — never health (V28; `tiles.hpp` doctrine: health is only what the
  world takes from you).
- Monsters get **no** per-skill pose field anywhere: the 66 monster sheets are walk-only, so a
  monster using a skill telegraphs *exclusively* via the `cast.telegraph` FX (V29 rejects a `pose`
  key outside `player`/boss-kit blocks). Boss poses live in the boss kit (§7.7) because pose
  availability is per-sheet, not per-skill.

Phase blocks:

**`cast`** — the telegraph. This is where telegraph-first (umbrella §2) is structural: you cannot
author a skill without a cast block, and V30 requires `ticks ≥ 3` for any skill whose `impact.damage
> 0` — nothing damaging is untelegraphed.

```json
"cast": {
  "ticks": 8,
  "telegraph": {
    "fx": "fx.telegraph_ring",
    "at": "target",
    "shape": {"kind": "circle", "radius_mt": 1800}
  },
  "interruptible": true,
  "sound?": "snd.cast"
}
```

`at ∈ {self, target}`; `shape.kind ∈ {circle, line, cone, tile}` with kind-specific fields
(`circle: radius_mt`; `line: length_mt, width_mt`; `cone: radius_mt, arc_deg`; `tile:` none).
V31: telegraph shape must cover the impact shape (same kind, telegraph dims ≥ impact dims) — the
promise shown is at least the promise kept.

**`channel?`** — optional held charge; semantics RFC-001.

```json
"channel": {"max_ticks": 20, "curve": "linear", "damage_bonus_pm_at_max": 500}
```

**`release`** — how the skill leaves the caster.

```json
"release": {"kind": "spawn_projectile", "count": 1, "spread_deg": 0}
```

`kind ∈ {strike, volley, spawn_projectile, spawn_entity, zone, from_sky}`; `count` 1..8 (V32);
`spawn_entity` requires `"entity": "entity.<id>"`. `from_sky` is the Meteor verb: the travel body
enters above the target rather than from the caster.

**`travel?`** — the in-flight segment. Present ⇔ `release.kind ∈ {spawn_projectile, from_sky}`
(V33).

```json
"travel": {
  "ticks": 6,
  "fx": "fx.rock_fall",
  "body?": {"entity": "entity.meteor_body"}
}
```

`body` makes the projectile a real CombatEntity (HP, team, destroyable) — the umbrella-§12
counterplay ("shoot a Meteor out of the air") is *opt-in per skill* by giving the travel phase a
body. No body = un-shootable arrow (cheap), body = destructible boulder (RFC-004 handles the kill).

**`impact`** — the only phase that deals damage (umbrella §1).

```json
"impact": {
  "damage": 45,
  "impulse_mt": 3000,
  "shape": {"kind": "circle", "radius_mt": 1800},
  "statuses": [{"id": "status.muddy", "buildup": 60}],
  "fx": "fx.rock_blast",
  "sound": "snd.explosion_heavy",
  "terrain?": {"effect": "crack", "radius_mt": 1200}
}
```

Damage/resistance math is RFC-009 (this field is the *base* it starts from); impulse-vs-mass is
RFC-003; `terrain` effects are RFC-003/RFC-004 and the schema only carries the closed enum
`{none, crack, rubble, scorch, wet}` (V34).

**`persist?`** — what stays behind.

```json
"persist": {
  "spawn": [{"entity": "entity.rubble_patch", "at": "impact", "offset_mt": [0, 0]}]
}
```

Only entity spawns — lingering *behavior* is always an entity (with its own document, lifetime,
sleep rule), never loose skill state. This is what makes battlefield state LOD-tolerant: after
`expire`, the skill instance is gone and everything left is §7.5 entities.

**`expire`** — implicit; carries no data (V35 rejects an authored `expire` key).

#### 7.7 Boss kit document (`boss.*`)

The authoring surface RFC-005 standardizes; RFC-007 consumes the action list.

```json
{
  "schema": 1,
  "id": "boss.giant_red_samurai",
  "name": "Giant Red Samurai",
  "sheet": "ninja/Actor/Boss/GiantRedSamurai",
  "material": "flesh",
  "scale": "giant",
  "hp": 700,
  "contact_damage": 20,
  "kit": [
    {"skill": "skill.samurai_cleave", "pose": "attack", "cooldown_ticks": 15, "weight": 60},
    {"skill": "skill.samurai_dash",   "pose": "charge", "cooldown_ticks": 40, "weight": 40}
  ],
  "phase2?": {"hp_below_pm": 500, "kit": []},
  "rl": {"trainable": true, "archetype": "samurai", "room": "interior_10x7"}
}
```

- V20: every `pose` in the kit must exist in `capabilities/boss_poses.json` for this sheet.
  `pose ∈ {attack, charge, shoot, none}`; `attack`/`charge` resolve to the directional L/R rows at
  runtime by facing (exactly `BossPose` in `boss.hpp` today).
- V21: `rl.trainable: true` requires the sheet to be on the audited shortlist
  (GiantRedSamurai, GiantBlueSamurai, GiantBamboo, Squids, GiantFrog, GiantRacoon, GiantRacoonGold,
  Tengu, DemonCyclop). Dragons (multi-part segment rigs) and GiantSlime/Flam/Spirit (idle+hit only)
  are rejected as RL agents at build time — a designer cannot accidentally spec a boss the art
  cannot animate or the RL room cannot host.
- V22: `phase2` is permitted only for sheets whose capability entry lists `trans` (today: Tengu).
- `rl.archetype` groups bosses onto **one policy per archetype** (GAME.md §10: 10–15 policies
  total, never per-individual); V36: the count of distinct archetypes across the pack must be ≤ 15.
- `rl.room` is closed: `{interior_10x7}` — RL bosses train inside 10×7-tile interior rooms; the
  validator ensures nobody specs a kit whose telegraph shapes cannot fit
  (V37: no kit skill may have a telegraph or impact `radius_mt > 3500`, line `length_mt > 9000` —
  half the room's short axis and its long axis respectively **(tunable)**).
- Squids' only attack pose is `shoot`; V20 therefore forces its kit to ranged skills — the schema
  encodes the audit instead of trusting memory.

### 8. Reference resolution and the validation rule table

Reference edges, all resolved at pack build (dangling reference = build error):

```
skill ──► icon ──► PNG (+Disabled twin)
skill ──► fx   ──► sheet PNG (+ frame-count check) ──► snd (optional)
skill ──► status ──► overlay fx (optional)
skill ──► entity (travel body / persist spawns / release spawn_entity)
entity ──► fx, snd, status, entity (on_death spawns; V38: spawn-chain depth ≤ 2, no cycles)
boss  ──► skill (kit), capability sheet entry
```

Numbered rules (build errors; the packer prints `V<nn>: <doc-id> <json-path>: <message>`):

| Rule | Check |
|---|---|
| V01 | UTF-8, single top-level object, no BOM |
| V02 | no float literals anywhere |
| V03 | no `null`, no duplicate keys |
| V04 | authored file ≤ 64 KiB |
| V05 | `id` = `<domain>.<file-stem>`, file in the domain's directory |
| V06 | id charset `[a-z0-9_.]`, ≤ 48 chars |
| V07 | every value within its unit range (§3 table) |
| V08 | unit-bearing fields carry the unit suffix |
| V09 | `ids.lock.json`: numbers append-only, never reused |
| V10 | wire id 0 reserved in every domain |
| V11 | lock-file edits limited to packer-generated + `retired` additions |
| V12 | unknown fields rejected |
| V13 | per-document `schema` matches `pack.json` major |
| V14 | fx sheet exists; grid divides into `frames` |
| V15 | fx `frames × ticks_per_frame ≤ 255` |
| V16 | `loop` only on persist-/render-referenced fx |
| V17 | every sound file exists |
| V18 | every icon PNG exists **with** its `Disabled` twin |
| V19 | every status `decay_per_s ≥ 1` (no permanent marks) |
| V20 | boss kit poses ∈ measured capabilities of the sheet |
| V21 | `rl.trainable` only for shortlisted sheets |
| V22 | `phase2` only for sheets with `trans` |
| V23 | material/scale/element/collision/team enums closed |
| V24 | entity lifetimes absolute; aura `period_ticks ≥ 1` |
| V25 | phase set: cast+release+impact mandatory, channel/travel/persist optional, nothing else |
| V26 | element ∈ {none, fire, ice, rock, thunder} |
| V27 | player pose ∈ {attack, ability1, ability2} |
| V28 | player cost vital ∈ {stamina, mana}, never health |
| V29 | no `pose` field outside `player` block / boss kit |
| V30 | damaging skills: `cast.ticks ≥ 3` |
| V31 | telegraph shape covers impact shape |
| V32 | release `count` ∈ 1..8 |
| V33 | `travel` ⇔ release kind ∈ {spawn_projectile, from_sky} |
| V34 | terrain effect ∈ {none, crack, rubble, scorch, wet} |
| V35 | no authored `expire` phase |
| V36 | distinct RL archetypes ≤ 15 |
| V37 | RL-boss kit shapes fit the 10×7 room (radius ≤ 3500 mt, line ≤ 9000 mt) (tunable) |
| V38 | entity spawn-chain depth ≤ 2, acyclic |
| V39 | every referenced id exists in its domain |
| V40 | reachability: every fx/snd/status/entity is referenced by something (warning, not error) |

### 9. Build pipeline and generated artifacts

```
data/combat/**            tools/build_combat_pack.py           assets/_gen/combat_pack.json
   authored JSON   ──────►  validate (V01..V40)        ──────►  canonical pack
                            assign ids (ids.lock)               combat_pack.hash.json
                            canonicalize + hash                 src/world/combat_ids.hpp   (generated)
```

- `combat_ids.hpp` is a *generated* header of `inline constexpr std::uint16_t` constants
  (`kSkillMeteor = 10;`) so C++ call sites that need a specific skill by name (e.g. the fixed F1a
  loadout logic) reference it symbolically. Generated, never hand-edited; a stale header vs pack is
  caught because the header embeds `struct_hash` and the loader asserts it at startup.
- The packer runs in CI and as a pre-commit step; determinism requirement: same inputs → same
  bytes on GCC/Linux and MSVC/Windows machines (Python, sorted keys, integers only — nothing
  platform-dependent survives; this matches the project's worldgen determinism bar).
- `tools/build_atlas.py` consumes the pack's fx sheet list, so an fx document is also the atlas
  manifest entry — one source of truth for "which strips get packed", which closes the audit gap
  "Magic/* FX family and spinning projectile sheets not yet packed": they get packed the day a
  document references them, and not before.

### 10. Runtime loading and hot reload

- The engine loads the canonical pack once at bring-up into `std::shared_ptr<const CombatPack>`
  (arrays indexed by `u16`; O(1) lookups; no string maps on hot paths).
- **Hot reload is a dev feature, leader-only, values-only.** Under `--dev-data`, the leader watches
  `combat_pack.json`'s mtime (the designer re-runs the packer; the packer is the only writer).
  On change: reload, compare hashes.
  - `struct_hash` equal → swap the `shared_ptr` at the next tick boundary and broadcast the new
    pack bytes to connected nodes (they verify `full_hash` and swap likewise). In-flight skill
    instances and live entities keep the pack snapshot they started with — the exact
    `shared_ptr<const FlowField>` argument from ARCHITECTURE.md §5: versioned derived data, not
    shared mutable state. Expected latency: pack rebuild ~100 ms + one tick **(tunable)**.
  - `struct_hash` differs → log "structural change; restart required" and *do nothing*. Swapping
    structure under live entities is where use-after-free style bugs live, and a restart on a dev
    box costs seconds.
- Production (non-dev) never reloads: the pack is fixed for the process lifetime, and the join
  handshake (§6) guarantees cluster-wide identity.

### 11. Simulation-LOD and replication guarantees (contract summary)

Everything this schema can express must survive a chunk ticking at 1 Hz or sleeping (GAME.md §3):

1. All lifetimes absolute (`spawn_tick + lifetime_ticks`); expiry computed on wake (V24).
2. All decays closed-form per-second rates (`decay_per_s`), fast-forwardable (§7.4).
3. Periodic behavior (auras) is best-effort per actually-run tick — degrading gracefully at 1 Hz
   by *doing less*, never by queueing catch-up work.
4. No document type can schedule anything outside an active skill instance or a live entity —
   there is deliberately no "timer", "spawner-on-schedule", or wall-clock field in any schema.
   This is the tone guardrail (GAME.md §0) made structural: combat data physically cannot count
   down behind the player's back.
5. Wire/snapshot shape per live object: `(u16 type, position, spawn_tick, hp[, status buildup])`;
   the pack hash agreement (§5–§6) is what licenses deriving everything else locally.

---

## Worked example 1: Meteor, fully authored

Umbrella §5: `Cast → Levitate → Fall → Impact → Earthquake → Crater/Rubble`, composed per §13 as
`Rock + Sky + Fall + Explosion`. Four files (plus icon/sound map entries). Every number (tunable).

`data/combat/skills/meteor.skill.json`

```json
{
  "schema": 1,
  "id": "skill.meteor",
  "name": "Meteor",
  "notes": "Rock siege spell. Red-tinted Rock strip + sky drop = Meteor (umbrella §14).",
  "element": "rock",
  "icon": "icon.book_rock",
  "tags": ["magic", "aoe", "siege"],
  "player": {
    "pose": "ability2",
    "cost": {"vital": "mana", "amount": 35},
    "cooldown_ticks": 200,
    "unlock": {"school": "magic", "level": 6}
  },
  "phases": {
    "cast": {
      "ticks": 8,
      "telegraph": {
        "fx": "fx.telegraph_ring_rock",
        "at": "target",
        "shape": {"kind": "circle", "radius_mt": 1800}
      },
      "interruptible": true,
      "sound": "snd.cast_heavy"
    },
    "release": {"kind": "from_sky", "count": 1},
    "travel": {
      "ticks": 6,
      "fx": "fx.rock_fall_hot",
      "body": {"entity": "entity.meteor_body"}
    },
    "impact": {
      "damage": 45,
      "impulse_mt": 3000,
      "shape": {"kind": "circle", "radius_mt": 1800},
      "statuses": [{"id": "status.muddy", "buildup": 60}],
      "fx": "fx.rock_blast",
      "sound": "snd.explosion_heavy",
      "terrain": {"effect": "rubble", "radius_mt": 1200}
    },
    "persist": {
      "spawn": [{"entity": "entity.rubble_patch", "at": "impact", "offset_mt": [0, 0]}]
    }
  }
}
```

`data/combat/entities/meteor_body.entity.json` — the umbrella-§12 counterplay: the falling rock is
a destroyable entity; kill it mid-fall and there is no impact.

```json
{
  "schema": 1,
  "id": "entity.meteor_body",
  "notes": "The falling meteor itself. 40 HP in a 6-tick window: shootable, not trivially so.",
  "material": "stone",
  "scale": "medium",
  "hp": 40,
  "lifetime_ticks": 6,
  "collision": "none",
  "team": "caster",
  "destroyable": true,
  "tags": ["rock", "airborne"],
  "render": {"fx": "fx.rock_fall_hot", "anchor": "center"},
  "on_death": {"fx": "fx.rock_crumble", "sound": "snd.rock_break", "spawn": []}
}
```

`data/combat/entities/rubble_patch.entity.json` — the battlefield-control residue (umbrella §4).

```json
{
  "schema": 1,
  "id": "entity.rubble_patch",
  "material": "stone",
  "scale": "small",
  "hp": 60,
  "lifetime_ticks": 600,
  "collision": "none",
  "team": "neutral",
  "destroyable": true,
  "tags": ["rock", "rubble", "slow_zone"],
  "aura": {
    "radius_mt": 1200,
    "period_ticks": 5,
    "statuses": [{"id": "status.muddy", "buildup": 15}]
  },
  "render": {"fx": "fx.rubble_idle", "anchor": "ground"},
  "on_expire": {"fx": "fx.dust_puff"}
}
```

`data/combat/fx/rock_fall_hot.fx.json` — zero new art: the pack's 14-frame Rock strip, hot tint.

```json
{
  "schema": 1,
  "id": "fx.rock_fall_hot",
  "sheet": "ninja/FX/Elemental/Rock/SpriteSheet.png",
  "frames": 14,
  "ticks_per_frame": 1,
  "anchor": "center",
  "tint": [255, 110, 70, 255],
  "sound": "snd.meteor_whoosh"
}
```

Note `frames: 14` × 1 tick = 14-tick life — legal now (V15 caps at 255), whereas the old
`kEffectLife=6` constant would have shown a third of the strip. The schema absorbs the audit's
engine gap by construction.

Tick timeline (10 Hz), for convergence checking: player presses ability2 at tick T. Cast T..T+7
(telegraph ring visible from T). Release at T+8 spawns `entity.meteor_body` above the target tile;
travel T+8..T+13. If the body survives, impact resolves at T+14: 45 damage base (RFC-009 applies
resistance/material), impulse 3000 mt (RFC-003 divides by mass), Muddy build-up 60 in the circle,
rubble terrain effect (RFC-003), and `entity.rubble_patch` spawns with absolute expiry T+14+600.
Skill instance expires; nothing else remembers it.

## Worked example 2: Spike, fully authored

Umbrella §13: `Rock + Ground + Rise + Root = Spike`; umbrella §3: "Spike: Root + Destroyable".

`data/combat/skills/spike.skill.json`

```json
{
  "schema": 1,
  "id": "skill.spike",
  "name": "Spike",
  "notes": "Ground spike: telegraphed crack, then a rooting destroyable pillar. Monster-usable: telegraph is pure FX (walk-only sheets).",
  "element": "rock",
  "icon": "icon.book_rock",
  "tags": ["magic", "control", "single_tile"],
  "player": {
    "pose": "ability1",
    "cost": {"vital": "mana", "amount": 30},
    "cooldown_ticks": 90,
    "unlock": {"school": "magic", "level": 2}
  },
  "phases": {
    "cast": {
      "ticks": 5,
      "telegraph": {
        "fx": "fx.ground_crack",
        "at": "target",
        "shape": {"kind": "tile"}
      },
      "interruptible": true,
      "sound": "snd.cast"
    },
    "release": {"kind": "spawn_entity", "count": 1, "entity": "entity.rock_spike"},
    "impact": {
      "damage": 12,
      "impulse_mt": 0,
      "shape": {"kind": "tile"},
      "statuses": [{"id": "status.rooted", "buildup": 70}],
      "fx": "fx.rock_spike_rise",
      "sound": "snd.rock_break"
    }
  }
}
```

`data/combat/entities/rock_spike.entity.json`

```json
{
  "schema": 1,
  "id": "entity.rock_spike",
  "material": "stone",
  "scale": "small",
  "hp": 30,
  "lifetime_ticks": 300,
  "collision": "blocks_ground",
  "team": "caster",
  "destroyable": true,
  "tags": ["rock", "obstacle", "root_source"],
  "aura": {
    "radius_mt": 600,
    "period_ticks": 5,
    "statuses": [{"id": "status.rooted", "buildup": 40}]
  },
  "render": {"fx": "fx.rock_spike_idle", "anchor": "ground"},
  "on_death": {"fx": "fx.rock_break", "sound": "snd.rock_break"},
  "on_expire": {"fx": "fx.rock_crumble"}
}
```

`data/combat/fx/rock_spike_rise.fx.json` — the pack's dedicated RockSpike strip, untinted.

```json
{
  "schema": 1,
  "id": "fx.rock_spike_rise",
  "sheet": "ninja/FX/Elemental/RockSpike/SpriteSheet.png",
  "frames": 9,
  "ticks_per_frame": 1,
  "anchor": "ground"
}
```

(`frames` here is illustrative — V14 makes the packer *measure* the sheet and fail the build if the
authored count disagrees, so the number in the committed file is verified, not trusted.)

Spike also demonstrates the monster path: a monster archetype using `skill.spike` shows the
`fx.ground_crack` telegraph for 5 ticks at the target tile and then the spike rises — no monster
pose is referenced anywhere, because none exists to reference.

---

## Interactions with Other RFCs

| RFC | Relationship |
|---|---|
| **RFC-001** (Ability System) | Executes the phase machine of §7.6: interrupt/refund rules, channel curves, cooldown enforcement. RFC-008 fixes the serialized shape and structural invariants (V25, V30–V33); RFC-001 fixes what each field *does* per tick. |
| **RFC-002** (Status & Effect) | Consumes `status.*` documents (§7.4). Build-up ladders, replacement-vs-stacking (`stacks_with`), and the freeze ladder are RFC-002 semantics over RFC-008 fields. |
| **RFC-003** (Physics & Material) | Consumes `impulse_mt`, `material`, `terrain.effect`. The impulse/mass formula and material interaction rules are RFC-003's; this schema only guarantees the closed enums (V23, V34). |
| **RFC-004** (Terrain & Combat Entity) | Consumes `entity.*` documents (§7.5), including the travel-body counterplay and the replication 5-tuple. |
| **RFC-005** (Boss Ability Authoring) | Authors against `boss.*` (§7.7). The pose-capability table and the RL-shortlist gate (V20–V22) are the mechanical rails RFC-005's process runs on. |
| **RFC-006** (Visual FX & Telegraph Standards) | Owns what a telegraph must look like (readability, color language, tint conventions); RFC-008 owns the `fx.*` container it standardizes into, and V30/V31 make its "always telegraphed, honestly telegraphed" rules non-optional. |
| **RFC-007** (RL Observation & Action) | Reads the boss kit's ordered `kit` list as the discrete action space and pack `u16` ids in observations. Id stability (§4) and the hash regime (§5) are what keep checkpoints valid; see RL Considerations. |
| **RFC-009** (Damage, Resistance, Build-up) | Starts from `impact.damage`, `statuses[].buildup`, `material`, `scale`. All multipliers/curves are RFC-009's; if RFC-009 needs a new per-skill knob, it lands as a minor-version optional field here. |
| **RFC-010** (Battlefield Simulation) | Relies on §11: absolute lifetimes, closed-form decay, best-effort auras — the properties that let battlefield state tick at 1 Hz or sleep. |

---

## RL Considerations

- **Action-space stability.** A boss archetype's action space is its kit list plus movement
  primitives (RFC-007). Kit entries reference skills by `u16`; because ids are append-only (V09),
  adding content never renumbers an existing action. Reordering or resizing a kit changes
  `struct_hash`.
- **Checkpoint binding.** Every `NetworkCheckpoint` JSON gains two fields:
  `"pack_struct_hash"` and `"pack_full_hash"`. The `TrainingActor` rules:
  - struct mismatch → checkpoint is *incompatible*; do not resume; fall back to the scripted
    generation-0 policy (`boss_policy()` seam in `boss.hpp`) and schedule fresh pre-training. The
    game never fields a policy against a world it wasn't trained for.
  - full mismatch only (retune) → resume training (fine-tune); log the delta. Balance patches
    degrade a policy gracefully, they do not invalidate it.
- **Determinism of what RL sees.** Telegraph and phase durations are integers in the pack, so the
  timing structure an agent learns (e.g. "8-tick cast means dodge window at +8") is exact and
  identical on every node — no float drift in the things a policy conditions on, matching the
  integer-obs stance already taken in `BossObs`.
- **Budget rails in data.** V36 caps archetypes at 15 (one policy per archetype, GAME.md §10);
  V37 keeps every trainable kit physically inside the 10×7 interior room. Both are enforced at
  build time, before a single training step is spent on an impossible spec.

---

## Asset & Engine Constraints Honored

| Constraint (2026-07-23 audit) | Where honored |
|---|---|
| Chill guardrail — nothing counts down behind the player | §11.4: no wall-clock/scheduling fields exist in any schema; combat data cannot reach outside an active fight |
| 66 monsters walk-only, zero attack anims | V29: no pose field for monsters; monster telegraphs are cast-phase FX overlays only (§7.6, Spike example) |
| ~11/20 boss sheets with real poses; RL shortlist; no Dragons/GiantSlime/Flam/Spirit as RL agents | `capabilities/boss_poses.json` + V20–V22 (measured capabilities), V21 (shortlist gate) |
| Player rigs: exactly two ability poses → basic + 2 equipped, no hotbar | V27: `pose ∈ {attack, ability1, ability2}`; the schema cannot express a third ability slot |
| Elements: exactly Fire/Ice/Rock/Thunder; other schools icon-only | V26 closed enum; other books mentioned only as future minor-version work |
| 121 skill icons at 24 px with Disabled twins | §7.3 + V18: disabled state resolved by convention, verified at build |
| `kEffectLife=6` truncation (Rock = 14 frames) | §7.1: lifetime derived from fx `frames × ticks_per_frame`, cap 255 (V15); per-kind life becomes pack data |
| No bespoke combo art; Magic/* FX and spinning projectiles unpacked | §7.1 tint reuse for variants; §9: atlas manifest driven by referenced fx documents — sheets get packed when referenced |
| RL: DQN from RLDrive, JSON checkpoints, one policy per archetype (10–15), 10×7 rooms, visible dojos | RL Considerations; V36, V37; checkpoint hash binding matches RLDrive's JSON checkpoint format |
| 1024×1024 world, chunks at 1 Hz or asleep | §11 LOD contract: absolute lifetimes, closed-form decay, best-effort auras (V24) |
| Server-authoritative, first-node leader, cheap replicable state | §5–§6 hash agreement at join; §7.5 five-field wire shape; leader refuses mismatched packs |

---

## Open Questions

1. **Basic attacks in or out of the pack?** The basic melee/ranged/spell verbs currently live in
   `tiles.hpp` constants. Pulling them into `skill.*` documents maximizes uniformity but puts the
   most latency-sensitive verbs behind a data lookup and turns P2's shipped tuning into a
   migration. Lean: migrate them (uniformity wins; lookups are array-indexed), but this needs a
   perf check on the chunk hot path first.
2. **Localization of `name`.** GAME.md is Vietnamese-first; the schema currently holds a single
   display string. Options: `name` as a key into a separate strings file (clean, more moving
   parts) vs `name_vi`/`name_en` fields (self-contained, denormalized). Undecided; affects
   `struct_hash` composition either way.
3. **Per-realm pack layering.** Realms use different asset packs (GAME.md §3). Reserved id range
   `50000+` (§4) assumes *overlay packs* (base + realm additions, hashed together). Whether a
   realm may *override* a base document (same id, new values) or only *add* is deliberately
   unresolved — override semantics complicate the hash story.
4. **Canonical hash algorithm.** SHA-256 is specified; Monocypher provides BLAKE2b already
   vendored. If the C++ side ends up needing to verify canonical bytes itself (not just compare
   packer-emitted hashes), switching to BLAKE2b avoids a second crypto dependency. Cosmetic to the
   design; must be pinned before first ship.
5. **Sound-variation determinism.** §7.2 makes variation selection a pure function of the effect
   seed, so co-watching clients usually agree. Is "usually" enough, or should the effect carry an
   explicit 2-bit variation index in the published view? (Costs wire bytes for a cosmetic
   guarantee.)
6. **How much of `channel` ships in v1?** The schema reserves the block (§7.6); no v1 skill uses
   it yet. Shipping the field unused risks bit-rot; cutting it means a minor bump later. Lean:
   reserve but mark "no v1 consumer" in the schema docs, and let RFC-001 decide.

---

## Non-goals

- **Runtime semantics.** How phases execute, interrupt, refund (RFC-001); how statuses build and
  decay (RFC-002); damage math (RFC-009); physics (RFC-003); entity behavior (RFC-004); LOD
  execution (RFC-010). This RFC is the disk contract only.
- **A scripting language.** No expressions, no conditionals, no event hooks in data. The phase
  pipeline is fixed; anything the declarative fields cannot express is a code change plus a
  minor/major schema bump — on purpose. Emergent interactions come from the rules engine
  (umbrella §7), not from per-skill scripts.
- **YAML, JSONC, JSON5, or any second grammar.** One parser, one canonical form.
- **Modding/sandboxing untrusted packs.** The pack is trusted content from the repo; the leader's
  hash check enforces *identity*, not *safety* of third-party packs.
- **Other element schools** (Plant/Water/Light/Darkness/Wind/Death) — icons exist in the asset
  pack; explicitly out of scope for v1 (V26). Future work: widening the element enum in a minor
  version.
- **Per-instance/live state serialization.** Save files, snapshots, and the event log (P5) are the
  persistence layer's contract; this RFC covers *definitions*, and its only touch-point with live
  state is the five-field replication shape in §7.5.
- **Player-authored loadout storage.** Which two abilities a player equips is player progression
  data (leader SQLite), not pack data.
