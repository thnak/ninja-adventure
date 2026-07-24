# RFC-012: Combat Audio & Sound Cue Standards

- **Status:** Accepted (revised after review)
- **Date:** 2026-07-24
- **Umbrella:** [RFC_Unified_Combat_System.md](RFC_Unified_Combat_System.md) §2 (Telegraph-first),
  §14 (Asset Reuse) — this RFC is the audio half of the reuse doctrine RFC-006 already applies to
  visuals
- **As-built source grounding:** `src/ui/audio.hpp:17-29` (the shipped `Sfx` enum, 10 entries),
  `src/ui/audio.cpp:20-31,88-92` (`kSfxFiles` table, `Audio::play` — one `Sound` object per `Sfx`,
  called through raylib's `PlaySound` directly, no `LoadSoundAlias` anywhere in the file),
  `src/client_main.cpp:559-570,576-579` (swing/cast/shoot/ability already play distinct SFX —
  verified, see Motivation), `src/client_main.cpp:485-491,648-666` (the `effect_tick` dedup vector
  and the loop that plays `kHit`/`kCombo` off newborn `Effect` records — the only place combat
  audio is triggered off *world* state today, as opposed to local input), `src/world/tiles.hpp:
  361-399` (`CreatureStats::windup` per `CreatureKind`, values 4/6/8), `src/world/tiles.hpp:
  487-531` (`Creature::windup`/`windup_x`/`windup_y`/`windup_target`, the only replicated
  telegraph state today), `src/world/tiles.hpp:568-627` (`EffectKind`, `effect_life_of`),
  `src/world/boss.hpp:34-35` (`kBossAttackWindup=10`, `kBossChargeWindup=14`),
  `src/world/chunk_actor.hpp:714,722-723,873-904,1007-1020,1090-1093` (`commit_windup`,
  `resolve_windup`, the stun-cancel path that zeroes `windup` without resolving), `src/ui/
  screens.cpp:792-808` (the shipped Options-screen master-volume slider), and the vendored
  `build/_deps/raylib-src/src/raudio.c:1172-1174` / `raylib.h:1639-1656` (`PlaySound` →
  `PlayAudioBuffer`, which resets the **same** buffer's `frameCursorPos` to 0 — no per-`Sound`
  polyphony unless the caller explicitly allocates `LoadSoundAlias` instances, which nothing in
  this codebase does today; `SetSoundVolume`/`SetSoundPitch`/`SetSoundPan` are linked and unused).
- **Depends on:** RFC-006 (telegraph grammar, danger tiers §1.3, state-machine lifecycle §1.4,
  the FX-recipe reuse philosophy §4 this RFC mirrors for audio, and the still-open Telegraph
  `struct` from §2 this RFC's fizzle cue needs), RFC-008 §7.2 (the `sounds.json` sound-map format
  this RFC's cue catalog is authored into, unmodified) and §7.6 (the `cast` phase block this RFC
  adds one optional field to), RFC-005 (the per-ability `telegraph:`/`sfx` authoring surface this
  RFC's schema delta extends), RFC-015 §3.2 (the `WindupCommit{creature_id, x, y}` wire record the
  commit cue and the P0 self-threat rule are keyed off, §2.1/§4.4)
- **Depended on by:** none yet — no accepted RFC names this one as a hard dependency (confirmed:
  `IMPLEMENTATION_MAP.md`'s instance/vitals/persistence-batch section states plainly "RFC-012 and
  RFC-017 are not cited by RFC-013/014/015/016 at all")

---

## Summary

This RFC is the audio pairing RFC-006 named and left unscoped (Q6, "do they belong in this RFC's
revision or in a small RFC-006b?" — this is that RFC-006b, numbered RFC-012 per the series index).
It defines a **cue catalog** for combat telegraphs and impacts, a **priority/voice-management
scheme** for the 20–50-concurrent-player target, and an explicit ruling on **boss vs. ordinary-
monster audio identity** — all authored inside RFC-008 §7.2's existing sound-map format, with one
small, additive schema delta.

Two things are confirmed, not re-derived, from the 2026-07-23 audit this batch works from:

1. **The `harvest.wav` mis-wiring is already fixed.** `client_main.cpp:559-570` shows swing,
   swing-heavy, cast, and shoot each playing their own distinct `Sfx` today; the code even carries
   a comment noting the fix ("no longer the harvest jingle standing in for it"). This RFC does not
   touch that bug and does not claim credit for fixing it.
2. **A different, still-live gap exists one level down.** The only place *world-caused* combat
   audio is triggered today — the `effect_tick` dedup loop at `client_main.cpp:663` — collapses
   every `EffectKind` except `kBlast` onto the single generic `kHit` cue. A Fire spell, an Ice
   spell, a Rock spell, a Thunder spell, a physical slash, a WhirlCleave, and a SmokeBomb throw all
   currently sound identical (or, in the SmokeBomb case, sound like a blow landing when nothing was
   hit). Separately, no sound plays anywhere when a creature or boss *commits* to a wind-up or
   reaches the last-3-tick IMMINENT window RFC-006 §1.4 defines — telegraph audio is a true blank,
   not a mis-wire.

This RFC closes both gaps with a small, additive cue catalog; adds a priority/ducking scheme
because the shipped `Audio::play()` has **zero polyphony** (verified against the vendored raylib
source, not assumed) and this project's own multiplayer target will trigger simultaneous identical
cues routinely; and answers "does a boss sound different" with **yes, by dedicated files for
bosses and cheap pitch/gain banding for ordinary monsters**, mirroring RFC-006 §4's zero-new-art
recipe philosophy where the audio budget allows it and spending real assets where it doesn't.

## Motivation

1. **RFC-006 named this gap and explicitly declined to own it.** RFC-006 Q6: "Telegraph audio cues
   (commit tick, imminent tick) are the natural pairing of this grammar but are unscoped here."
   RFC-002 Q4 separately notes the *visual* gauge-visibility
   question is RFC-006's to decide, capped at "the existing 2 bytes" of replication budget — that
   question is RFC-011's (HUD), not this RFC's; it is cited here only to be explicit about the
   boundary and not reopen it.
2. **The gap is real and grep-confirmed, not assumed.** No source file in `src/` contains
   `telegraph` or `Telegraph` anywhere near `audio`/`sound`/`sfx` (checked). The only combat audio
   trigger in the client is the `effect_tick` loop, and it fires **only on `Effect` birth** — i.e.
   only at *impact*, never at *commit* or *imminent*. A monster's entire wind-up — the one thing
   RFC-006 built its whole visual grammar around, because these sheets have no attack frame — is
   completely silent today.
3. **RFC-008 §7.2 already ships the container this RFC needed and never used.** The `sounds.json`
   domain, the `files[]` variant list, the `effect_seed % n` selection rule, and the "sound is
   presentation, divergence is acceptable" principle are all shipped spec, sitting unused by any
   telegraph-shaped cue. This RFC is what finally puts content into that container for telegraphs.
4. **The shipped audio wrapper has no polyphony, and this project's own concurrency target
   guarantees that will matter.** `Audio::play(Sfx s)` calls raylib's `PlaySound` on one `Sound`
   object per `Sfx` kind; `PlaySound` → `PlayAudioBuffer` (`raudio.c:645-654`) just resets that
   **same** buffer's play cursor to 0. Two `kHit` events in the same tick — trivially reachable
   the moment two of GAME.md §11's 20–50 concurrent players are fighting different creatures near
   each other — do not mix; the second silently cuts off the first. `LoadSoundAlias`,
   `SetSoundVolume`, `SetSoundPitch`, and `SetSoundPan` are all linked in via raylib but called
   nowhere in this codebase. At 1 player this is inaudible. At this project's stated population it
   is a routine, silent event-drop with no control over *which* event wins. This RFC is written
   before that population exists specifically so the fix is a spec, not a hotfix.

## Guide-level Explanation

### What the player hears

A slime notices you and stops moving — a short, light *thock* plays at its position, low in the
mix. Three ticks before the blow lands, a brighter warning ping sounds — same cue family, same
short lead time RFC-006 already promises visually. You have already seen the red pulse; the sound
confirms it and helps you hear a threat that is off to the side of your screen.

A dojo boss winds up its charge. The cue that plays is not the slime's *thock* pitched down — it is
its own dedicated sound, louder, and its imminent warning **repeats once more** in the last tick of
the window, the audio equivalent of the visual's 6 Hz flash. You do not need to look at the boss to
know something big just committed.

A Fire spell lands and it *sounds* like fire — not the same generic hit thud an Ice spell, a Rock
spell, and a plain sword swing all currently share. A SmokeBomb throw no longer sounds like
something got hit, because nothing did.

If a stun cancels a creature's wind-up, a short, quiet fizzle plays instead of the swing landing —
mirroring RFC-006's grey FIZZLE visual, so "I interrupted it" is audible, not just visible.

In a crowded fight — a village raid, a full 2–4-party dungeon room — the mix does not turn into a
wall of noise, and a threat aimed directly at you is never the cue that gets dropped to make room
for someone else's hit landing across the room.

### What a designer authors

Nothing changes for ordinary combat sound — `sounds.json` entries and skill `sound`/`impact.sound`
references work exactly as RFC-008 §7.2/§7.6 already specify. The one new thing a boss-kit or
skill author can write is an optional imminent-moment cue on the telegraph block already there:

```jsonc
"telegraph": {
  "fx": "fx.telegraph_ring_rock",
  "at": "target",
  "shape": { "kind": "circle", "radius_mt": 1800 },
  "imminent_sound?": "snd.meteor_imminent"      // NEW — this RFC's delta, §5
}
```

Leaving it absent is not a mistake — it means "use the generic tier-banded cue," which is what
every ordinary monster does for free today and forever, exactly the way RFC-006's ordinary
monsters get the generic shake-and-pulse stack for free without any author writing a line of FX.

## Reference-level Design

### 0. Conventions

- Tick = simulation tick at 10 Hz (100 ms), reused unchanged from RFC-006 §0.
- All literal numbers are **(tunable)** unless marked **structural**.
- "World-sourced cue" = a cue triggered by state published by the simulation (a creature's
  `windup`, an `Effect`, a boss pose). "Local-input cue" = a cue triggered directly by the local
  player's own input this frame (`in.swing`, `in.cast`, …), exactly as the eight existing verb
  cues fire today. This split is structural to this RFC — see §4.2.

### 1. The two authoring tiers

Every cue this RFC adds lives in exactly one of two places, mirroring RFC-006's split between
"universal cue stack, free" (§1.6) and "authored per-skill FX" (§4):

**Tier A — engine-level fallback.** A fixed, client-resident cue plus a pitch/gain band, keyed off
state every creature already publishes (`windup`, `EffectKind`). Requires no authoring, applies to
every creature and boss that exists today, and is what plays when Tier B is silent. This is where
the `Sfx` enum grows (§2, §3).

**Tier B — data-driven override.** A `snd.*` id authored on a skill or boss ability's telegraph
block. Present ⇒ used instead of Tier A's generic band for that one ability's imminent moment.
Absent (the default, and the only option that exists for content not yet migrated onto RFC-008
skill JSON) ⇒ Tier A's fallback plays. **An authored cue never leaves a gap — the precedence is
"authored, else generic tier band, never silence,"** the audio-side restatement of RFC-006 §1.4's
own "one lifecycle, with or without a decal" rule.

### 2. Telegraph lifecycle cues — commit, imminent, fizzle

RFC-006 §1.4's state machine (ARM → CHARGE → IMMINENT → IMPACT, or → FIZZLE) is the timing
authority this section keys off; nothing here redefines it. Three moments get a cue:

| Moment | Sim-side trigger (today) | Cue |
|---|---|---|
| **Commit** | `windup` transitions from 0 to the creature's wind-up length (`commit_windup`/`boss_commit`) | `Sfx::kTelegraphCommit` (ordinary) or `kBossTelegraphCommit` (boss), band-selected (§2.2) |
| **Imminent** | `windup == 3` (RFC-006 §1.4's IMMINENT window is the last 3 ticks; `windup` counts down, so this is the identical boundary, not a new one) | `Sfx::kTelegraphImminent`/`kBossTelegraphImminent`, or the authored `imminent_sound` (§1 Tier B) if present |
| **Fizzle** | wind-up cancelled before reaching 0 (stun) rather than resolving | `Sfx::kInterrupt` — **blocked**, see §2.4 |

#### 2.1 Reading the trigger client-side without new wire state

**Commit.** RFC-015 §2.2 deliberately drops `Creature.windup_target` from the wire (`PublishedCreature`
carries only `windup`, "an 8-byte player key with no rendering use") and replaces the commit-time
target geometry with a purpose-built record: `WindupCommit{creature_id, x, y}` (RFC-015 §3.2), sent
"exactly once, the tick a creature's `PublishedCreature.windup` transitions 0 → nonzero... reliable,
ordered... never re-sent." This RFC keys the commit cue off `WindupCommit`'s arrival, not off
client-side inference from `windup` deltas — the same "a client never infers removal from silence"
discipline this RFC itself invokes for the fizzle case two sections later (§2.4, citing RFC-015
§3.3), applied here to *appearance* instead of removal. `WindupCommit.(x, y)` — the frozen target
spot, per RFC-006 T2 — also supplies the geometry §4.4's P0 rule and §4.5's positional audio need;
neither requires a separate signal.

**Imminent.** No RFC-015 wire record exists for the imminent boundary — that moment is read directly
off `Creature.windup` (published every tick the chunk is active, per the existing `ChunkView`) and
the same `stats_of(CreatureKind)`/`kBossAttackWindup`/`kBossChargeWindup` tables the sim uses (shared
header, both link `tiles.hpp`/`boss.hpp`), computing "this is the imminent tick" as `windup == 3`
purely from state already on the wire. This mirrors the `effect_tick` pattern already shipped for
impact cues (§3): one dedup entry per creature id, remembering the last `windup` value seen, so a
re-published unchanged view (e.g. a 1 Hz background-tier chunk republishing the same value) never
re-fires a cue.

#### 2.2 Audio bands — an interim proxy, not a new tier system

RFC-006 §1.3 already defines a canonical 0–3 danger tier from `d` (expected damage) and `cc`
(control duration), and its future `Telegraph` record (§2) carries that resolved `tier` as a
replicated `u8`. This RFC does **not** recompute `d`/`cc` client-side — that needs damage/HP fields
the renderer path does not cheaply have — and does not wait for `Telegraph.tier` to ship, because
today's telegraphs (every creature and the boss) have no `Telegraph` record at all, only `windup`.

Instead, this RFC uses the one thing that already varies by threat size in shipped data — wind-up
length — as an **interim** proxy, banded from the real values in `tiles.hpp`/`boss.hpp`:

| Band | Wind-up ticks (shipped values) | Which creatures (today) | Pitch | Gain |
|---|---|---|---|---|
| Light | 4 (Slime, Spider, Hare, Chicken) | small/timid | ×1.12 (tunable) | ×0.9 (tunable) |
| Heavy | 6–8 (Ghost, Boar, Wolf, Skull, Bear) | mid/large | ×0.92 (tunable) | ×1.1 (tunable) |
| Boss | 10 / 14 (`kBossAttackWindup`/`kBossChargeWindup`) | the dojo boss only | dedicated files, §2.3 | — |

Light and Heavy share the same two `Sfx` files (`kTelegraphCommit`/`kTelegraphImminent`) and are
distinguished only by `SetSoundPitch`/`SetSoundVolume` on the alias in play (§4.6) — zero new
assets for the two ordinary-monster bands, matching RFC-006 §4's "one packed sheet, many skills"
philosophy applied to audio.

**This table is explicitly retired the moment RFC-006 §2's `Telegraph.tier` is replicated.** Once
every telegraph carries an authoritative tier, the client reads `tier` directly and this
windup-length banding becomes historical — flagged as Open Question 1.

#### 2.3 Boss identity — real files, not just a pitch shift

Bosses get **two dedicated `Sfx` entries** (`kBossTelegraphCommit`, `kBossTelegraphImminent`)
rather than a pitched-down monster cue. This is a deliberate divergence from RFC-006's own
zero-new-art rule for visuals: that rule exists because the *sprite* budget is genuinely zero
(GAME.md §13), but the *audio* budget is not — a 188-file CC0-licensed source pack is already
licensed for the project (GAME.md §13's asset table), with headroom for two more semantic roles.
That pack is not itself committed (`.gitignore` excludes `assets/_src/`; only 11 files are copied
by hand into the committed `assets/audio/` today, per `src/ui/audio.hpp`'s own header comment), so
spending two more on the boss band is an asset-pass copy against an already-cleared license, not a
new budget or licensing ask. Spending two real files on the boss is cheap and buys unmistakable
identity that a shared-file pitch trick cannot fully deliver, especially at 8-bit-ish translated
pitch shifts on short one-shot clips.

One additional structural rule for the boss band only: the imminent cue **repeats once more at
`windup == 1`** (tunable tick, but the repeat itself is structural to the Boss band) — an audible
second warning that has no equivalent in the Light/Heavy bands, mirroring the extra visual urgency
RFC-006 §1.4 gives IMMINENT (the 6 Hz flash) without literally trying to sync a one-shot clip to a
flash rate, which would just produce a buzz.

A boss kit author may still override either moment per-ability via Tier B (`imminent_sound`, §1)
for a truly bespoke ability (e.g., a named ultimate) — the dedicated boss band is the *floor* every
boss gets for free, not a ceiling. This override is only writable once a boss kit is authored (or
migrated) onto RFC-008 skill JSON: `imminent_sound` lives on RFC-008 §7.6's `cast.telegraph` block
(§5), and RFC-005's own native `telegraph` object has no cue field at all — just `{tier, anchor}`
(RFC-005's worked example). A boss kit still on RFC-005's native, not-yet-migrated format has no
field to hold a bespoke imminent cue and gets Tier A's dedicated boss band only, until migration.

#### 2.4 Fizzle — a stated, not silently ignored, gap

Distinguishing "the wind-up resolved" from "the wind-up was cancelled" client-side needs a signal
that does not exist on the wire today. `resolve_windup` (natural resolution) publishes an `Effect`
the same tick `windup` hits 0; the stun-cancel path (`chunk_actor.hpp:714`) sets `windup = 0`
directly and publishes **nothing** distinguishing that tick from a resolution the client simply
didn't see an effect for yet. Correlating "windup hit 0" against "was an effect born at that
position this tick" is exactly the kind of inference-from-absence RFC-015 rules out for its own
removal events (§3.3, "a client never infers removal from silence") — the same argument applies
here: a missed correlation reads as a silently-wrong cue, not a dropped one.

RFC-006 §2 already designs the fix and has not shipped it: the (not-yet-built) `Telegraph.flags`
field reserves **bit0 for exactly this** ("bit0: fizzling"). This RFC's fizzle cue is therefore
**correctly triggerable only once RFC-006 §2's `Telegraph` record ships**, and until then plays no
cue at all — an interrupted wind-up is silently absorbed rather than either lying (playing a
resolution sound) or guessing. This is a stated interim gap, not a design decision this RFC is
making; §9 Open Question 1 tracks it alongside the tier-banding retirement, since both resolve the
same day `Telegraph` lands.

### 3. Impact cue identity

`client_main.cpp:663` today: `audio.play(e.kind == EffectKind::kBlast ? kCombo : kHit)`. Every
`EffectKind` other than `kBlast` — `kSlash`, `kFire`, `kIce`, `kEarth`, `kShock`, `kSlashHeavy`,
`kSlashCombo`, and `kSmoke` — collapses onto the single `kHit` cue. This RFC replaces that
one-line ternary with a per-kind table:

| `EffectKind` | Meaning | Cue |
|---|---|---|
| `kSlash` (0) | plain physical hit | `kHit` (unchanged) |
| `kFire` (1) | Fire spell impact | `kImpactFire` (new) |
| `kIce` (2) | Ice spell impact | `kImpactIce` (new) |
| `kEarth` (3) | Rock spell impact | `kImpactRock` (new) |
| `kShock` (4) | Thunder spell impact | `kImpactThunder` (new) |
| `kBlast` (5) | combo detonation | `kCombo` (unchanged) |
| `kSlashHeavy` (6) | WhirlCleave | `kHitHeavy` (new) |
| `kSlashCombo` (7) | CrushBlow finisher | `kHitHeavy` (new, reused) |
| `kSmoke` (8) | SmokeBomb throw — a **utility** effect, not a hit | **no cue** (fixes the "throwing smoke sounds like a hit landing" bug this table exposes) |

This table is exhaustive over the shipped `EffectKind` enum (structural: a new `EffectKind` value
must add a row here, the audio-side twin of RFC-006 §Interactions' "new channels/coatings MUST add
rows" rule). The element cues pair with RFC-006 §1.2's element palette one-to-one (Fire/Ice/
Rock/Thunder), giving spell impacts the audio identity their visuals already have and the input
cues (`kCast`) already imply but do not currently differentiate by element either — that
differentiation is out of scope here (§10) since `kCast` fires on local input before the element
resolves to a target/effect, not on impact.

### 4. Voice management at 20–50 players

#### 4.1 The constraint, verified

`Audio::play(Sfx)` plays one `Sound` per kind through raylib's `PlaySound`, which calls
`PlayAudioBuffer` on that same buffer (`raudio.c:645-654`): sets `playing = true` and resets
`frameCursorPos = 0`. A second `play(kHit)` while the first is still sounding does not mix with
it — it restarts the same buffer, truncating whatever was mid-playback. `LoadSoundAlias`,
`SetSoundVolume`, `SetSoundPitch`, and `SetSoundPan` are all present in the vendored raylib
(`raylib.h:1639-1656`) and used nowhere in `src/`. This is not a hypothetical scaling concern —
it is the literal, current behavior the moment two `Effect`s of the same kind are born in the same
tick within one client's scan radius, which the game's own 20–50-player target (GAME.md §11) makes
routine, not rare.

#### 4.2 Local-input cues are exempt

The seven distinct `Sfx` kinds that already fire off the local player's own input this frame
(`kUiClick`, `kBuild`, `kHarvest`, `kSwing`, `kSwingHeavy`, `kCast`, `kShoot` — the two ability
slots, F and G, both reuse `kCast` rather than adding a distinct kind, per `client_main.cpp:576,579`)
are bounded by one player's own action rate — they cannot scale with population, because each
client only ever plays *its own* input cues (verified: the swing/cast/shoot/ability calls at
`client_main.cpp:559-579` are all gated on `in.*`, the local `poll_input` result, never on another
player's action). **This RFC's voice
management applies only to world-sourced cues** (§0) — telegraph commit/imminent/fizzle and impact
cues (§2, §3) — which scale with how many other players' and creatures' effects are visible to a
client, not with the local player's own input rate. Local-input cues are unchanged and always play
immediately, exactly as today.

#### 4.3 World-sourced cue pipeline

Every tick, for the same 5×5-chunk neighborhood the existing `effect_tick` loop already scans
(`client_main.cpp:648-666` — no new radius invented), candidate world-sourced cue events (a fresh
telegraph commit/imminent, a newborn `Effect`) are collected, then:

1. **Distance cull.** Events whose source is beyond `kAudioMaxRadius = 14` tiles (tunable) from the
   local player are dropped before anything else — sound that far away is not diegetically
   audible, and this bounds the candidate set independent of how many creatures exist on the wider
   1024² map.
2. **Priority rank.** Surviving events are ranked by §4.4's table.
3. **Per-tick budget.** At most `kAudioCueBudget = 8` (tunable) world-sourced cues actually call
   `Audio::play` in one tick; the rest are dropped, never queued. Queueing a stale combat cue a
   tick late is worse than silence — the same reasoning RFC-006 §2 applies to its own 8-telegraph
   chunk cap, and license for treating a drop as correct (not a bug) comes directly from RFC-008
   §7.2's own stated principle: "sound is presentation... cross-client variation divergence is
   acceptable."
4. **Alias pool + steal.** Each of the eight high-concurrency `Sfx` groups — `kHit`, `kCombo`,
   `kHitHeavy`, `kTelegraphCommit`, `kTelegraphImminent`, `kBossTelegraphCommit`,
   `kBossTelegraphImminent`, and the shared `kImpactFire/Ice/Rock/Thunder` pool (one pool since
   they are mutually exclusive per event) — matching §4.4's P1/P2 priority rows in full, is backed
   by a small fixed pool of `kAliasesPerCue = 4` (tunable) `LoadSoundAlias` instances, loaded once
   at `Audio` construction.
   Playing a cue claims a free alias if one exists; if all are busy, it **steals** the
   lowest-priority currently-sounding alias in that pool (never a higher- or equal-priority one —
   a steal that would lose information is refused, and the new event is simply dropped instead).
5. **Ducking.** While a P0 event (§4.4) is sounding, every concurrently-sounding P2/P3 alias in any
   pool has its gain multiplied by `kDuckFactor = 0.5` (tunable) for `kDuckMs = 300` (tunable) ms
   via `SetSoundVolume`, restored after. This is the one place this RFC asks for real-time mixing
   beyond "play a clip" — a threat aimed at the local player must never be masked by ambient
   impact noise from someone else's fight across the room.

#### 4.4 Priority table (highest first)

| Priority | Event |
|---|---|
| **P0** | Telegraph imminent where the creature's latched `WindupCommit.(x, y)` (RFC-015 §3.2 — the frozen commit-time target spot, §2.1) is within `kSelfThreatRadius` (tunable, ~1 tile) of the local player's own position — a threat aimed at *you*, inferred geometrically since RFC-015 §2.2 drops the `windup_target` player-key field from the wire |
| **P1** | Boss commit/imminent not aimed at the local player; tier-2/3 authored (Tier B) imminent cues |
| **P2** | Ordinary monster commit/imminent (Light/Heavy bands); impact cues (`kHit`, `kHitHeavy`, `kImpactFire/Ice/Rock/Thunder`, `kCombo`) |
| **P3** | Fizzle/interrupt (informational — the news is always good news, it never needs to compete for a voice) |

Ties within a priority band break by ascending distance to the local player. This table is the
audio-side analogue of RFC-006 §2's tier-priority telegraph-cap overflow rule (higher tier cancels
lower, never arrival order) — same shape, independently tunable numbers.

#### 4.5 Positional audio

World-sourced cues pan and attenuate by position relative to the local player (this is a
single-local-player-per-client, top-down 2D game — no 3D audio graph is needed):

- **Gain:** linear falloff, `gain = clamp(1 - distance_tiles / kAudioMaxRadius, 0, 1)` (§4.3's
  same 14-tile radius as the hard cutoff, so gain reaches 0 exactly at the cull boundary).
- **Pan:** `pan = clamp(0.5 + (source_x - player_x) / kPanHalfWidthTiles / 2, 0, 1)` (tunable
  `kPanHalfWidthTiles = 10`; raylib's `SetSoundPan` convention is 0=left, 0.5=center, 1=right —
  the formula is written against that convention, not a signed -1..1 one).

These multiply into the band gain (§2.2) and the ducking factor (§4.3), never replace them.

#### 4.6 API delta to `ui/audio.hpp`

This RFC's one concrete code-shaped ask: `Audio::play` grows an overload taking gain and pan,
and the ten new `Sfx` entries (§2, §3) are appended after the existing ten (never inserted between
— `Sfx` is not persisted to disk, so reordering is safe, but appending is the smaller diff):

```cpp
enum class Sfx : std::uint8_t {
    kUiClick, kBuild, kHarvest, kHit, kSwing, kSwingHeavy, kCast, kShoot, kCombo, kLevelUp,  // unchanged (10)
    kTelegraphCommit, kTelegraphImminent,           // ordinary monster, Light/Heavy pitch-banded
    kBossTelegraphCommit, kBossTelegraphImminent,   // boss, dedicated files
    kInterrupt,                                     // fizzle — see §2.4 for when this can fire
    kImpactFire, kImpactIce, kImpactRock, kImpactThunder,
    kHitHeavy,
    kCount,                                          // 20
};

void play(Sfx s, float gain = 1.0f, float pitch = 1.0f, float pan = 0.5f) const;
```

The existing zero-argument call sites (`audio.play(ui::Sfx::kSwing)`, etc.) are unaffected by the
default arguments. Internally, the eight groups listed in §4.3 step 4 (`kHit`, `kCombo`,
`kHitHeavy`, `kTelegraphCommit`, `kTelegraphImminent`, `kBossTelegraphCommit`,
`kBossTelegraphImminent`, `kImpactFire/Ice/Rock/Thunder`) route through the alias pool; the
remaining nine kinds (the seven local-input cues of §4.2, `kLevelUp`, `kInterrupt`) keep today's
single-`Sound` behavior, since they are exempt from voice contention by construction (§4.2) or are
low-frequency enough (P3 fizzle) not to need one. Together the pooled and exempt kinds cover all
20 `Sfx` entries.

### 5. Data shape — the RFC-008 schema delta

One optional field, added to RFC-008 §7.6's `cast.telegraph` object:

```jsonc
"telegraph": {
  "fx": "fx.<id>",
  "at": "self" | "target",
  "shape": { /* unchanged */ },
  "imminent_sound?": "snd.<id>"    // NEW. Resolved snd.* id (RFC-008 §7.2), unchanged format.
}
```

- Resolution: a build error if the referenced `snd.*` id does not exist (the same dangling-
  reference discipline RFC-008 §8 already applies to every other reference edge; this RFC asks
  RFC-008's editor to add the edge `skill ──► telegraph.imminent_sound ──► snd` to §8's table and
  a numbered V-rule for it, the same pattern RFC-006 used for its own required §7.1 delta).
- **Not a new field on RFC-005's per-ability shape.** RFC-005's existing `sfx` field (validated by
  its own R7 #4, "must be a packed sound") sits beside `fx: { impact: ... }` and reads as the
  *impact* cue — already covered once boss kits route through RFC-008 skill documents
  (`impact.sound`, §7.6). This RFC's `imminent_sound` lives on the *telegraph* block instead
  (`{ tier, anchor }` in RFC-005's own worked example), which has no cue field today — it does not
  duplicate or compete with RFC-005's `sfx`.
- **No change to `sounds.json`'s own shape.** The `files[]`/`gain_pm` structure and the
  `effect_seed % n` variant-selection rule (RFC-008 §7.2) are reused verbatim; this RFC adds a
  *reference site*, not a new document kind.
- **Precedence against RFC-008 §7.6's existing `cast.sound?` field.** The same `cast` phase block
  already carries an optional `sound?` field (RFC-008 §7.6, used unmodified by this RFC's own
  worked examples — Meteor's `"sound": "snd.cast_heavy"`, Spike's `"sound": "snd.cast"`), which
  fires at cast/commit start. This is a **separate slot from `imminent_sound`**, not a duplicate:
  `cast.sound` is the commit-moment override and, when present, supersedes Tier A's generic
  `kTelegraphCommit`/`kBossTelegraphCommit` engine fallback for that skill, the same "authored,
  else generic tier band, never silence" precedence §1 already states for the imminent moment.
  Absent ⇒ Tier A's commit fallback plays, as today. The two fields never fire for the same moment
  and are never additive — `cast.sound` governs commit, `imminent_sound` governs imminent, and a
  skill author may set either, both, or neither independently.

## Interactions with Other RFCs

- **RFC-006 (Visual FX & Telegraph Standards):** the timing authority throughout — §1.3's tiers
  (cited, not recomputed, §2.2), §1.4's IMMINENT-is-last-3-ticks boundary (§2), §1.6's universal
  cue-stack philosophy (mirrored for audio's Tier A), and §2's not-yet-shipped `Telegraph` record
  (`tier` and `flags` bit0) are what this RFC's §2.2 banding and §2.4 fizzle gap are waiting on.
  This RFC closes RFC-006's own Q6.
- **RFC-008 (Data-driven Skill Definition):** §7.2's sound-map format is adopted unchanged; §7.6's
  `cast.telegraph` block gains the one `imminent_sound` field this RFC owns as schema authority for
  (mirroring RFC-006's own "RFC-006 is the schema authority for those two blocks" clause) — RFC-008
  remains authority for everything else in the document, including the pre-existing `cast.sound?`
  field, whose precedence against this RFC's Tier A commit fallback is stated in §5.
- **RFC-005 (Boss Ability Authoring):** RFC-005's native `sfx`/`telegraph:{tier,anchor}` authoring
  surface has no cue field on its `telegraph` object today — a boss kit author can place a bespoke
  `imminent_sound` override (§1 Tier B, §5) only once the kit is authored or migrated onto RFC-008
  skill JSON's `cast.telegraph` block; RFC-005's native, not-yet-migrated format gets Tier A's
  dedicated boss band only (§2.3). No change to RFC-005's validator rules is required by this RFC
  beyond what §5 asks of RFC-008's own V-table.
- **RFC-002 (Status & Effect Framework):** this RFC deliberately adds no looping status audio
  (Burning crackle, Shocked buzz) — see Non-goals. RFC-002's model stays audio-silent beyond
  whatever one-shot `impact.sound` an author already attaches.
- **RFC-015 (Client Replication & Interest-Set Protocol):** no wire-format change is requested by
  this RFC's commit/imminent cues — both read state RFC-015 already replicates: the commit cue and
  §4.4's P0 self-threat rule key off RFC-015 §3.2's `WindupCommit{creature_id, x, y}` record, the
  imminent cue off `Creature.windup` (§2.1), and impact cues off `Effect.kind`. The one dependency
  this RFC has on new replicated state (the fizzle flag, §2.4) belongs to RFC-006 §2's `Telegraph`
  record, not to RFC-015. `IMPLEMENTATION_MAP.md` confirms RFC-015 does not name this RFC as a
  dependency either way.
- **RFC-011 (Combat HUD, Input & Cooldown UI):** no overlap — HUD is a visual-only sibling RFC in
  this batch; the reserved `equipped_ability_0/1` persistence column (RFC-016) is RFC-011's
  concern, not audio's.
- **RFC-019 (Progression & Skill System):** `kLevelUp`'s existing trigger (`client_main.cpp`'s
  `last_skill`/`last_xp` tracking, already cited by RFC-015 §Interactions as untouched
  infrastructure) is unmodified by this RFC.

## Multiplayer & Simulation-LOD Considerations

- **Audio is 100% client-local presentation with zero replication cost and zero leader
  involvement**, same as GAME.md §12's "Options: shortcuts, sound, display" being a purely
  client-local settings screen. This RFC adds no leader responsibility and no new message type.
- **LOD tolerance is inherited, not re-derived.** A chunk with no nearby player publishes at most
  once per `kIdlePublish=32` ticks (RFC-015 §4); such a chunk is also outside every client's audio
  cull radius (§4.3, 14 tiles ≪ a chunk edge) by construction, so this RFC never needs to reason
  about a telegraph audio cue "ticking" at a demoted rate — if a client can hear it, RFC-015's
  inner-band guarantee already has that chunk at full 10 Hz.
- **Outer-band slop is accepted, not fought.** RFC-015 §4's outer band forwards at up to a 300 ms
  period (`kBeaconPeriod`); an imminent cue for a creature at the edge of a client's interest set
  could fire up to ~300 ms late relative to the true tick boundary. This RFC does not ask RFC-015
  for a tighter guarantee — RFC-006 itself accepts comparable granularity for the equivalent visual
  case, and a cue that is a few frames late is a non-issue for a fairness read whose real
  ground-truth (the visual telegraph) is unaffected.
- **Determinism:** cue *selection* among a `snd.*` document's file variants continues to use RFC-008
  §7.2's `effect_seed % n` rule unchanged — this RFC adds no new randomness and does not need two
  clients to hear byte-identical audio (RFC-008 already licenses cross-client divergence here).
- **The 20–50-player target is the entire reason §4 exists.** Everything in §4 is sized against
  that number, not against a single-player smoke test — the raylib polyphony ceiling this RFC
  works around (§4.1) is invisible below roughly a handful of simultaneous same-kind events, which
  a solo player essentially never produces and a raid-sized fight produces constantly.

## Tone Guardrail Compliance

- **No cue is time-gated, scheduled, or wall-clock-driven.** Every cue in this RFC fires off sim
  tick state a fight the player is already in produces (`windup`, `Effect`) — the same structural
  argument RFC-008 §11 makes for its own schema ("no timer, no spawner-on-schedule, no wall-clock
  field in any schema"). This RFC adds no field that could express one.
- **Audio is reinforcement, never the sole channel.** RFC-006 R7 already rules that visual fairness
  reads (pulse, decal, outline) are never removed by any Options setting. This RFC extends the same
  rule to audio explicitly: muting all SFX (master volume to 0, or a future dedicated combat-cue
  slider, §9 Open Question 5) must never reduce the fairness information a telegraph carries — the
  visual channel alone remains fully sufficient per RFC-006, by construction, in every case this
  RFC touches. No cue in this catalog is the only warning a player gets.
- **Boss audio identity is recognition, not dread.** The dedicated boss files and the double
  imminent-repeat (§2.3) exist so a boss is *identifiable*, at parity with RFC-006 §1.6's pose-swap
  and R8's "classifiable at ≥ 8 tiles" goal — never to imply urgency beyond what the tier's real
  lead time already promises. A boss telegraph is not audibly scarier because of a ticking clock;
  it sounds bigger because the threat it represents is bigger, and the lead time is identical to
  any other ability of the same RFC-006 tier.
- **Nothing here creates an off-screen alarm.** Every cue this RFC defines is anchored to a
  specific telegraph or impact already happening at a specific position a player has, per RFC-006,
  already been shown visually — there is no ambient "something is coming" sting anywhere in this
  catalog.

## Open Questions

1. **When does the interim windup-length banding (§2.2) and the fizzle gap (§2.4) retire?** Both
   are blocked on the same RFC-006 §2 `Telegraph` record (`tier`, `flags` bit0) shipping. Should
   this RFC's revision simply be re-issued the day that record lands, or should §2.2/§2.4 be
   written now as forward-looking rules that self-obsolete (as currently drafted) and left alone?
2. **Ordinary-monster elemental telegraphs do not exist yet.** No shipped `Creature` attack carries
   an `Element` — only player-cast spells do. §3's per-element impact table is ready the day a
   monster or boss authors an elemental ability (RFC-005/008 build-out), but this RFC has not
   pre-assigned commit/imminent variants *per element* for creature-sourced telegraphs, only the
   band-per-size table (§2.2). Worth deciding now, or deferred until such content is actually
   authored?
3. **Exact file assignment.** This RFC names semantic roles (`kBossTelegraphCommit`, `kImpactFire`,
   …) and does not pin them to specific files inside the 188-file licensed audio set (GAME.md §13)
   — that selection is an asset pass, not a spec decision. Should this RFC's next revision pin
   candidates, or is "semantic role only" the right level for a spec to stop at?
4. **The §4 numbers (`kAudioCueBudget=8`, `kAliasesPerCue=4`, `kDuckFactor=0.5`, `kDuckMs=300`,
   `kAudioMaxRadius=14`) are reasoned from the verified raylib constraint but not measured against
   a real 20–50-player load.** RFC-017's proposed `mmo_sim` sweep harness (once it exists) would be
   a natural place to script an audio-load stress scenario and tune these against real numbers —
   this RFC does not depend on RFC-017 and does not block on it, but flags the fit.
5. **Master-volume-only vs. a dedicated combat-cue slider.** Today's Options screen
   (`screens.cpp:792-808`) has one master-volume slider plus a music on/off toggle — no separate
   category for combat SFX, unlike RFC-006 R7's dedicated "Telegraph opacity" visual slider. Should
   this RFC ask for a parallel "Combat cue volume" slider, or is master volume sufficient given
   §4's ducking already keeps the mix from drowning out fairness-critical cues?
6. **Battlefield-state audio (Earthquake rumble, Fog muffle) is untouched by this RFC** — RFC-010
   owns the states, RFC-006 §7 owns their visual filters, and there is no audio equivalent
   specified anywhere. Left as an open question rather than folded in here, since it sits right at
   this RFC's "no ambience" non-goal boundary (§Non-goals) and a one-shot Earthquake rumble is
   arguably combat-adjacent rather than ambient — worth a ruling, not assumed either way.

## Non-goals

- **Music and ambience.** `theme_day.ogg`, the P1 environmental particle system (leaves, weather),
  and any future additional music tracks are untouched. This RFC governs one-shot combat cues only.
- **RFC-006's visual telegraph grammar.** Shapes, tiers, lead times, the FX layering stack, and the
  status-tint tables are RFC-006's; this RFC only *pairs* audio to timing boundaries RFC-006
  already defines, and never changes what a telegraph looks like or how long it lasts.
- **The `sounds.json` file format itself.** RFC-008 §7.2's `files[]`/`gain_pm` shape and its
  variant-selection rule are reused as-is; this RFC adds one reference site (§5), not a new
  document kind, and requests no change to the format's own fields.
- **Positional audio beyond 2D pan/gain.** No reverb zones, no occlusion-by-wall simulation, no
  HRTF — a flat linear falloff and a simple left/right pan (§4.5) is the entire spatial model, sized
  for a top-down 2D game with one local listener per client.
- **Status-effect looping audio** (a Burning crackle, a Shocked buzz loop). RFC-002's status model
  stays audio-silent beyond a one-shot `impact.sound` an author already attaches; a continuous
  per-status audio loop is deferred, flagged in Open Questions rather than specified here, to avoid
  drifting into the "ambience" boundary this RFC deliberately does not cross.
- **PvP audio.** PvP is off by default (GAME.md §11); no cue in this catalog is authored with
  player-vs-player readability in mind, mirroring RFC-006's identical PvP non-goal.
- **Village-raid rumor/warning audio** (GAME.md §6's "a scout runs back, word spreads" one-day
  advance notice). That is narrative/quest-system content (RFC-020 territory), not a combat
  telegraph, and is out of scope here.
- **Voice acting / localized lines.** Nothing in this catalog is spoken dialogue; all cues are
  non-verbal SFX, consistent with the project's zero-voice-budget asset set (GAME.md §13).

## Review Record

- **Votes:** Reviewer A — revise; Reviewer B — revise. Both re-verified against source/cited RFCs;
  this revision applies every mustFix both upheld, plus one sound single-reviewer item.
- Applied: §4.4 P0 rewritten off `WindupCommit.(x,y)` (RFC-015 §3.2) — `windup_target` is dropped
  from RFC-015's wire and can no longer back P0.
- Applied: §2.1 commit-cue detection rewritten to key off RFC-015's exactly-once `WindupCommit`
  record instead of client-side `windup`-delta inference; imminent detection unchanged (polls
  `windup==3`, no wire record exists for it).
- Applied: §2.3/§5/Interactions clarified — `imminent_sound` is only writable via RFC-008
  `cast.telegraph`; RFC-005's native `telegraph:{tier,anchor}` has no cue field until migration.
- Applied: §5 adds an explicit precedence rule between RFC-008's existing `cast.sound?` and this
  RFC's Tier A commit fallback.
- Applied: §4.3/§4.4/§4.6 alias-pool lists corrected to include `kCombo`, `kHitHeavy`,
  `kBossTelegraphCommit` ("six" → "eight" groups; pooled + exempt now account for all 20 `Sfx`).
- Applied: §2.3 "188 files already committed" corrected — 188 is the licensed source-pack size
  (`assets/_src/`, gitignored); 11 files are actually committed in `assets/audio/` today.
- Applied: Motivation item 1's false claim that RFC-006's Non-goals list mentions audio — removed.
- Applied: §4.2 "eight" local-input cues corrected to seven distinct `Sfx` kinds (ability slots
  reuse `kCast`).
- Applied: header "Depends on" and RFC-015 Interactions bullet updated to name `WindupCommit`
  (RFC-015 §3.2) as the actual dependency the commit cue and P0 rule read.
- Unresolved: none — every mustFix from both reviewers, plus the single-reviewer local-input-count
  item with sound proof, was applied; no objection was rejected.
