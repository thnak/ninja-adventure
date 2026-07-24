# RFC-024: Leader Failure & Session Recovery

> Status: **Accepted (revised after review)**
> Design canon: [ARCHITECTURE.md §2](../ARCHITECTURE.md) ("Leader chết thì sao" — the world stops on
> leader death, mitigated only by an exportable/portable save file and periodic + on-exit saves;
> automatic leader election explicitly deferred), [ARCHITECTURE.md §0 S1](../ARCHITECTURE.md) ("Kiến
> trúc tin cậy bị làm quá" — `Require<Trusted>` is kept for a correctness reason, one owner for
> authoritative state, not as an anti-fraud wall; "kẻ tấn công là bạn bè bạn"), [GAME.md §0](../GAME.md)
> (chill is the default, nothing counts down behind the player's back), [ROADMAP.md P5/P6](../ROADMAP.md)
> (P5 "Bền vững" done-bar: "tắt máy giữa lúc chơi, mở lại, thế giới đúng như cũ, sai lệch ≤ 60 giây";
> P6 "Multiplayer" done-bar: two machines, two accounts, one world, defending one wave together —
> **not yet built**, no `TcpTransport` cluster exists in `src/` today).
> Depends on (accepted): RFC-013 (Vitals, Death & Recovery — §7's universal hearth-respawn "no sane
> position to resume at" fallback, reused verbatim, not redefined), RFC-014 (Instance & Realm
> Lifecycle — §6/§6.1's disconnect/reconnect/session-closed fallback and its own Multiplayer-section
> leader-death ruling for instanced state, cited not modified), RFC-015 (Client Replication &
> Interest-Set Protocol — the `Tick`/`ChunkDelta` wire shapes and send-cadence this RFC's detection
> signal composes with, and its own explicit deferral of "reconnection at the socket/session layer"
> to P6), RFC-016 (Persistence & Save-File Format — §6.3's periodic checkpoint cadence and ≤60s
> arithmetic, §9's portable save-directory contract and its precise exclusion list, §7's
> `return_map`/`return_x`/`return_y` breadcrumb — all cited, none re-derived).
> As-built source grounding: `src/world/protocol.hpp:22-29` (`Tick{tick, world_ms, night}` — "the
> simulation heartbeat," fanned unconditionally, content-independent, distinct from the
> content-gated replication this RFC deliberately does *not* reuse for detection), `src/world/
> map_director.hpp:1-15` ("one Tick per chunk, per tick... authority flows downhill from trusted to
> untrusted"), `src/world/account.hpp` (`AccountStore` — the one persistence mechanism that ships
> today: saved on account-create/login and on clean exit only, no periodic save, no crash-signal
> handler — RFC-016's own characterization, cited not re-verified here), `src/client_main.cpp:135-138`
> (`World world; world.build(...); world.start();` — today's client and simulation share one process;
> there is no separate client machine yet, so this RFC's client-facing detection contract is written
> for P6's arrival, not against code running today), `src/world/tiles.hpp:51,1140` (`kMaxPlayers = 8`,
> `kTicksPerSecond = 10` — today's actual ceiling and tick rate, both cited by RFC-015 already), grep
> of `src/` for `election`/`quorum`: **zero hits**; `heartbeat` appears twice, both as descriptive
> comment language for the already-shipped `Tick` fan-out (`protocol.hpp:23`, `map_director.hpp:70`),
> not any liveness/timeout mechanism — no liveness, timeout, or election mechanism exists anywhere in
> the codebase today.

---

## Summary

This RFC answers one question ARCHITECTURE.md §2 posed honestly and left partly open: **the leader
dies — now what, exactly, does a player see, and what do they get back?** Scope is strictly
**accidental** failure — a crash, a network drop, a host closing the app — never a hostile actor. It
does four things:

1. **Detection.** No liveness or timeout mechanism exists anywhere today. This RFC adds one, kept as
   small as the problem allows: a content-independent liveness signal (new to this RFC, composing
   with RFC-015's accepted wire protocol rather than modifying it) at two observer tiers — a
   chunk-hosting peer node watching the already-shipped `Tick` fan-out, and a connected client
   watching a new, small, always-sent liveness cadence — each with its own tunable timeout.
2. **"The world stops," made concrete.** A precise state transition (`Connected → LeaderUnreachable`),
   exact client-visible behavior (frozen last frame, one plain-language banner, no countdown, no
   ETA, no auto-give-up), and a distinct graceful-shutdown notice for the case where the host quits
   on purpose rather than crashing.
3. **A precise recovery ledger.** Exactly which state RFC-016's portable save recovers (account,
   inventory, skills, quests, buildings/crops, and even mid-dungeon players' return coordinates —
   almost all at **zero** drift) versus what RFC-014 already rules reseeds from scratch (instance
   sessions, creature/wildlife positions, combat state, ability cooldowns) — assembled here as one
   table for a player-facing answer, not re-derived.
4. **Manual restart & resume, specified now; automatic election, reaffirmed deferred.** Now that
   RFC-016 has made persistence real, this RFC specifies the (trivial) manual "relaunch the same
   world" flow as the actual answer, and argues explicitly — at the same rigor as a Tone Guardrail
   argument — for why ARCHITECTURE.md §2's deferral of automatic leader election still holds even
   with real persistence in place, rather than silently building a failover system nobody asked for.

**Explicit non-goal, stated at the top because it governs every decision below:** this RFC is not,
and must never become, an anti-cheat or hostile-leader-defense system. See Non-goals for the argument
in full.

---

## Motivation

RFC-016 closed the one gap ARCHITECTURE.md §2 named as blocking anything better than "hope your save
is fresh": *"Save file phải xuất được và di chuyển được"* (the save file must be exportable and
portable) and *"lưu định kỳ + lúc thoát"* (periodic + on-exit saves) are no longer aspirational — they
are RFC-016 §9's portable `saves/<world_name>/` directory and §6.3's periodic checkpoint cadences,
accepted and specified down to the tick. What ARCHITECTURE.md §2 left as a single honest sentence —
*"Trung thực: thế giới dừng"* (honestly: the world stops) — never grew a player-facing contract for
**how a still-connected player finds out**, or **what they can trust survived**.

That gap is real and distinct from three adjacent gaps sibling RFCs already named and declined to
close, each for a stated reason:

- RFC-014 §6 built disconnect/reconnect *within a live session* but explicitly wrote "no
  client-facing disconnect/network layer exists in `src/` at all yet (RFC-015's territory)" —
  handing the question upward.
- RFC-015 accepted that hand-off for in-session replication, but its own Non-goals list "transport-
  level connection establishment, authentication, encryption, or reconnection at the socket/session
  layer" as P6's, deferring further — handing the *leader-is-simply-gone* case upward again, past
  RFC-015 itself.
- RFC-014's own Non-goals separately declines "leader election or any automated failover," citing
  ARCHITECTURE.md §2's deferral by name — correctly scoping *that* question out, but leaving the
  much smaller "how does a player find out, and what do they do next" question unanswered by anyone.

Nobody has fallen through this seam maliciously — each RFC scoped correctly against what it owned.
But the seam itself — the player-facing contract that sits **above** raw transport (P6/RFC-015) and
**below** instance/world semantics (RFC-014) — has no owner. A player whose screen simply freezes
mid-farming, with no message, no clock, and no idea whether it's lag, a bug, or the host's PC dying,
is a worse experience than any amount of designed difficulty GAME.md §0 is trying to avoid — the
problem there is not challenge, it is **uncertainty**, and uncertainty is exactly what a plain,
honest, non-alarming message fixes cheaply.

---

## Guide-level Explanation

### For a player (not the host)

You're farming, or mid-fight, and the game freezes. Within a few seconds a small banner appears:
*"Connection to the world lost — waiting for the host's game to come back."* Nothing else happens.
The screen holds its last frame — nobody keeps moving, nobody keeps swinging, no damage lands, no
death is recorded. Your inputs are disabled, not silently dropped — pressing a key shows the same
banner rather than doing nothing. You can walk away and come back in an hour; there is no "give up"
timer, no lost-connection screen that eventually gives up on you. If the host relaunches, you
reconnect exactly where the save says you were, give or take a few seconds. If the host closed the
world on purpose (not a crash), the banner says so plainly instead of making you wait out a timeout
to find out: *"The host closed the world."*

### For the host

If your own process crashes, there's nothing to see — the game is simply gone, same as any crashed
application. If you quit deliberately while friends are connected, you get a one-line courtesy
notice before the app exits ("N other players are connected — closing now will disconnect them") —
not a blocking confirmation dialog with a countdown, just a fact, because you are allowed to close
your own game whenever you want. On relaunch, you (and everyone else) get back almost everything —
see the recovery ledger in §4 of the Reference-level Design.

### For a designer

This RFC does not add a single new game mechanic, item, or system a player interacts with by choice.
It adds one message and a few tunable timers whose entire job is to make an unavoidable, honest fact
(the leader is gone) legible instead of ambiguous. Nothing here should ever look like a challenge,
a penalty, or a countdown — see Tone Guardrail Compliance.

### For an engineer implementing P6

This RFC specifies: the cadence and timeout constants for two new liveness observers (§2), the exact
client-visible state machine and copy (§3), a graceful-shutdown notice distinct from a timeout (§3.5),
a precise table of what's Sync-committed/checkpointed vs. reseeded on restart (§4, entirely assembled
from RFC-014/RFC-016's own rulings), and the manual restart/resume flow plus the reasoning for not
building automatic election (§5). It does **not** specify socket-level connection handling, TLS,
`RelayTransport`, or anything RFC-015/P6 already own — see Non-goals.

---

## Reference-level Design

### 1. Failure model — what counts as "accidental"

This RFC's detection and messaging apply uniformly to every case in this list, because a passive
observer (a peer node or a client) genuinely cannot and should not try to distinguish them:

- **Process crash.** Unhandled exception, segfault, OOM kill, power loss on the host machine.
- **Clean quit.** The host closes the game window or the app receives a normal shutdown signal —
  distinguished from a crash *only* where the leader itself can announce it before going away (§3.5);
  once it's gone, a client cannot tell a graceful quit it missed the announcement for apart from a
  crash, and is not asked to.
- **Network drop.** The host's machine loses its LAN/relay path (`RelayTransport`, P6/ROADMAP.md) —
  externally identical to a crash from a client's point of view: no traffic arrives.
- **Prolonged unresponsiveness.** The host machine locks up, sleeps, or is pegged hard enough that
  `Tick` fan-out stalls without the process actually exiting — externally identical to the two cases
  above, and deliberately **not** distinguished from them: this RFC's timeouts fire on "no signal for
  N ticks," not on "process confirmed dead," because the latter is not observable over a network by
  design (see Non-goals on why this RFC does not attempt to determine cause).

**Explicitly excluded from this list, by the hard scope constraint:** a leader that is *reachable and
responding* but behaving maliciously (feeding bad state, refusing to relay damage, impersonating an
account). That is not a failure this RFC detects or responds to at all — see Non-goals.

### 2. Detection

Two independent observer classes see leader loss differently, because they sit on different sides of
the trust boundary and receive different traffic today.

#### 2.1 Node-side detection (a chunk-hosting peer machine, once P6's cluster exists)

Reuses an already-shipped, content-independent signal rather than inventing one: `Tick{tick,
world_ms, night}` (`protocol.hpp:22-29`) is fanned by `MapDirector` to **every** chunk and player
actor **every tick, unconditionally** — `map_director.hpp`'s own header comment: "one Tick per chunk,
per tick," specifically so no chunk ever has to guess the time. Unlike replication (§2.2 below), this
signal does not get suppressed when nothing is happening — it *is* the world clock, and the world
clock advances at 10 Hz (`kTicksPerSecond`, `tiles.hpp:1140`) regardless of content.

This RFC adds one consumption rule, not a new message:

```
kNodeLeaderTimeoutTicks (tunable) = 50 ticks  (5 s at 10 Hz)
```

A peer node that has not received an expected `Tick` for `kNodeLeaderTimeoutTicks` consecutive ticks
declares the leader unreachable. It does **not** locally advance its own tick counter past the last
one it received — inventing a tick number would violate the exact determinism boundary ARCHITECTURE.md
§2c already draws (only migrate-order-dependent counters are allowed to drift between nodes; the tick
itself is a leader-assigned value every node is supposed to agree on, never something a node computes
for itself). A node in this state simply idles: it keeps whatever chunk actors it already has in
memory frozen at their last tick, and does not accept new player-verb traffic for them (there is
nothing authoritative to check that traffic against without the leader).

#### 2.2 Client-side detection — the trap this RFC deliberately avoids

The naive design — "the client declares the leader gone if it stops receiving `ChunkDelta`
messages" — is wrong, and worth stating why rather than silently avoiding it. RFC-015 §3.2 is
explicit: *"A creature that does not change at all in a given tick is not included in that tick's
delta at all"* — this is where RFC-015's actual bandwidth saving comes from. A quiet chill session
(nobody moving, nothing fighting, exactly the kind of session GAME.md §0 wants to protect) can
legitimately produce long stretches of silence on a subscribed chunk **while the leader is
completely healthy.** Reusing content silence for liveness detection would misfire precisely during
the calmest, most "in-tone" moments of play — the opposite of what this RFC exists to prevent.

This RFC therefore does not touch RFC-015's accepted delta/interest-set mechanism (§2/§3 there) at
all, and instead adds one small, explicit, content-independent signal on top of it — the same shape
of addition RFC-015 §4 already made for `windup_commits` (exempted from outer-band throttling for a
readability reason; this RFC's signal is exempted from *all* content-gating for a liveness reason):

```
kLeaderLivenessPeriodTicks (tunable) = 30 ticks  (3 s)   -- sent to every connected client,
                                                             unconditionally, regardless of chunk
                                                             content, world content, or whether
                                                             anything in that client's interest set
                                                             changed this tick
kClientLeaderTimeoutTicks  (tunable) = 100 ticks (10 s)  -- no traffic at all (content OR
                                                             liveness) for this long → client
                                                             declares "leader unreachable"
```

**What this RFC fixes and what it leaves open.** It fixes the cadence and the timeout — the contract
an implementation must satisfy. It deliberately leaves the exact wire carriage unspecified (Open
Questions §1): a dedicated tiny message (e.g. `WorldAlive{tick}`), or riding the `tick` field already
present on every `ChunkDelta` sent for the client's own standing chunk (always inner-band, per
RFC-015 §4, hence always eligible for full-rate forwarding) with a rule that an otherwise-empty
`ChunkDelta` is still sent for that one chunk at this cadence. Either satisfies this RFC's contract;
choosing between them is an implementation detail of whoever wires up RFC-015's transport, not a
redesign of RFC-015's accepted message shapes.

**Why 10 seconds, not something tighter.** A false positive here is worse than a few extra seconds of
uncertainty — declaring "the world is gone" to a player who is actually just experiencing a lag spike
is the exact kind of alarming, wrong message this RFC exists to prevent. 10 s is over 3× the liveness
period and generous against normal network jitter; it is a starting number, not a measured one (Open
Questions §4).

**Why not rely on TCP-level disconnect notification alone.** That depends entirely on P6's transport
layer, which does not exist yet (RFC-015 §Non-goals: "transport-level connection establishment...
deferred to P6"). An application-level timeout is the only thing this RFC can specify without doing
that work, and it stays useful even once sockets exist — an application can hang (§1's "prolonged
unresponsiveness" case) without its socket ever actually dropping.

#### 2.3 Today's reality: single process, nothing to detect

`client_main.cpp:135-138` builds and runs `World` in the same process as the renderer — there is no
separate client machine today, and therefore no detection problem to solve yet. This RFC's detection
design (§2.1, §2.2) is written for P6's arrival, the same way RFC-014/015/016 specified instance,
replication, and persistence behavior ahead of the multi-machine cluster they assume. Nothing above
claims to be running code.

### 3. "The world stops" — concretely

#### 3.1 Server-side: nothing happens automatically

ARCHITECTURE.md §2, quoted because this RFC changes none of it: *"Trung thực: thế giới dừng"* —
honestly, the world stops. No standby leader, no queued failover target, no automatic restart. The
process is simply gone or unresponsive. This RFC's contribution is entirely about what a still-open
client does with that fact, not about preventing it.

#### 3.2 Client state machine

```
Connected ──(kClientLeaderTimeoutTicks elapses with no traffic, §2.2)──▶ LeaderUnreachable
Connected ──(WorldClosing received, §3.5)───────────────────────────────▶ HostClosed
LeaderUnreachable ──(traffic resumes — a new leader process answers)────▶ Connected (reconnect
                                                                            flow, RFC-014 §6/§6.1)
```

There is no third, further state. Neither `LeaderUnreachable` nor `HostClosed` ever transitions to a
"gave up" or "world is dead" state on its own — see §3.4.

#### 3.3 `LeaderUnreachable` — exact client-visible behavior

- **Rendering holds the last received frame.** No interpolation or extrapolation into a guessed
  future position for any creature, projectile, or other player — that would be inventing
  information the leader never sent, indistinguishable from a bug once the world resumes and
  everything visibly "snaps."
- **A single banner appears**, plain language, no jargon: *"Connection to the world lost — waiting
  for the host's game to come back."* No spinner with a percentage, no estimated time, no retry
  counter.
- **Input is disabled but acknowledged**, not silently swallowed. Attempting to move or use an
  ability while `LeaderUnreachable` re-shows the same banner rather than doing nothing with no
  feedback — the failure mode this RFC is built to prevent is exactly "nothing happened and I don't
  know why."
- **No combat consequence.** Because rendering is frozen at the last authoritative tick, nothing
  "keeps happening" that could kill the player, drain a resource, or fail a channel — the world is,
  from the client's perspective, paused, not still running invisibly against them.

#### 3.4 No automatic "give up"

This RFC explicitly decides **not** to add a timeout after which the client stops trying or shows a
terminal "world is unreachable, come back later" screen. Doing so would invent a deadline exactly
where GAME.md §0 says none should exist: the player did nothing wrong, is owed no penalty for
waiting, and may legitimately want to leave the banner up and check back in an hour. The only
player-visible affordance beyond the banner is a low-key, always-available "Try to reconnect" action
(exact widget left to whichever RFC owns HUD chrome — Open Questions §3) — pressing it or not changes
nothing about whether reconnection eventually succeeds, it only lets an impatient player retry sooner
than the passive listener otherwise would.

#### 3.5 Graceful shutdown — a distinct, better message when the host quits on purpose

When the leader process receives a normal quit (not a crash) while other clients are connected, this
RFC adds one new, minimal, reliable broadcast the leader sends **before** tearing down:

```cpp
struct WorldClosing {};   // reliable, ordered — same delivery class as RFC-015 §9's
                          // `ChunkDelta.removed`/baseline sends, for the identical reason: a
                          // dropped WorldClosing is a correctness bug (a client waiting out a
                          // needless 10s timeout for something the host already announced), not
                          // a cosmetic one
```

A client receiving `WorldClosing` transitions straight to `HostClosed` and shows *"The host closed
the world."* — skipping `kClientLeaderTimeoutTicks`'s wait entirely, because the reason is already
known and honest. This is the one new wire message this RFC introduces; everything else in §2 is a
cadence/timeout rule on top of already-existing or already-planned traffic.

**Host-side courtesy, not a gate.** If the host quits with other clients connected, showing a
one-line notice first ("N other players are connected — closing now will disconnect them") is a
courtesy so the host makes an informed choice, never a blocking confirmation that could be read as
"you need permission to close your own game" — exact UX flagged, not designed (Open Questions §6).

### 4. The recovery ledger — what RFC-016's save recovers vs. what reseeds

Assembled entirely from RFC-014's and RFC-016's own rulings; nothing in this section is a new design
decision, only a single table a player-facing message can be honestly built from.

| State | Recovers? | Drift | Source ruling |
|---|---|---|---|
| Account, login, password | Yes | Zero | `AccountStore` (`account.hpp`), saved on login/clean-exit |
| Inventory items, skill levels/XP (RFC-019), quest state (RFC-020), equipped-ability slots | Yes | Zero — Sync-committed on the discrete event that changed it | RFC-016 §4/§6.2, `Persistent<Snapshot, Batched>` + per-event Sync commits |
| Buildings, crops, tilled ground (world overlay) | Yes | Zero | RFC-016 §6.2's `Persistent<EventSourced, Sync>` mechanism; quoted sentence at RFC-016 Summary point 3 — "every building placed, every tile tilled... survives a hard kill with zero drift by construction" |
| A building's combat damage in progress | Yes | Non-zero, **always favorable** (resolves in the player's favor) | RFC-016 §6.6, explicit named exception |
| Exact HP / mana / stamina / x / y position | Yes | Up to `kProgressionCheckpointIntervalTicks` = 300 ticks = **30 s**, worst case | RFC-016 §6.3 — "the number the ≤60s bar is actually about" |
| Return-portal coordinates for a player who was inside an instance | Yes | Zero — Sync-committed the moment the portal was used | RFC-016 §7, `return_map`/`return_x`/`return_y` columns |
| An open instance/dungeon session itself (who's inside, its layout state) | **No** — always gone | — | RFC-014 §Multiplayer: "no `MapId` in the instanced band survives a restart," matching RFC-022's own default |
| Creature and wildlife positions | **No** — reseeded | — | Deterministic from `kWorldSeed` (RFC-016 §9); "carries zero persistence obligation at all" per RFC-016's Summary point 3, itself citing ARCHITECTURE.md §3's "không lưu" table |
| Raid/combat state, in-progress fights | **No** — reseeded | — | RFC-016 §9's explicit exclusion list |
| Ability cooldowns | **No** — reset to ready | — | RFC-016 §9's explicit exclusion list |

**Consequence for a player who was standing inside a dungeon when the leader died.** On reconnect,
their persisted `map` points at a `MapId` whose session is now `CLOSED` (it did not survive the
restart, per the table above). RFC-014 §6.1's already-specified fallback applies unmodified: they
resume at `return_map`/`return_x`/`return_y` — the exact overworld tile their group's portal use
originated from, itself Sync-committed with zero drift (RFC-016 §7) — or, if that breadcrumb is
somehow absent, RFC-013 §7's universal hearth-respawn fallback. Both are explicitly **location
relocations, not death events** — RFC-014's own wording: "explicitly not a death consequence." No
item is lost, no death is recorded, no status effect carries over incorrectly, because none of those
categories were ever at stake in this fallback — only *where the player's body is standing*.

**The honest one-line summary this RFC's messaging (§6) should say:** *"Your farm, inventory, and
progress are saved. You might lose up to the last 30 seconds of movement, and any dungeon you were
inside will need a fresh start — but nothing you own or earned is gone."*

### 5. Manual restart & resume — decided now; automatic election — reaffirmed deferred

#### 5.1 Manual restart & resume: what this RFC specifies

RFC-016 closed the one prerequisite ARCHITECTURE.md §2 named for making leader death survivable at
all — a portable, periodically-updated save directory. What remained genuinely undecided was only the
**player-facing shape of resuming**, and that turns out to require no new mechanism:

1. The host (or, per §5.3, any friend with the save directory) relaunches the game pointed at the
   same `saves/<world_name>/` directory (RFC-016 §9 — "the directory *is* the export").
2. `World::build()`/`World::start()` run exactly as they do on any world load — this is not a new
   code path, it is the *existing* world-open flow, because a leader restart and "load a world you
   already have" are the same operation from the engine's point of view.
3. Every account reconnects through the existing login flow (`World::login()`); RFC-014 §6's
   resume-in-place path applies for anyone whose persisted `map` still points at a session that
   happens to still exist (never true for an instanced `MapId` immediately after a leader restart,
   per §4's table, but true for the persistent-band overworld) — otherwise §4's ledger is what they
   get back.

Nothing here is new engineering beyond §2's detection/messaging and §3.5's `WorldClosing` broadcast —
this RFC's actual "restart flow" contribution is naming it explicitly as the answer, rather than
leaving "now what?" implicit after the banner in §3.3 stops making sense.

#### 5.2 Reaffirming ARCHITECTURE.md §2's deferral of automatic election

Argued explicitly, at the rigor a reviewer will expect after §5.1's "it's just a restart" claim,
because the obvious follow-up question is "why not automate step 1?"

Automatic election solves one problem: *deciding who becomes the new leader without a human
choosing.* At this project's actual scale and trust model — 20–50 friends (GAME.md §11), no VPS, no
dedicated always-on host, `Require<Trusted>` kept for correctness not defense (ARCHITECTURE.md §0
S1) — the realistic failure is "the host's personal gaming PC crashed or they turned it off," and the
realistic fix is a human decision that already has an obvious, fast, zero-technical-debt answer: the
host restarts, or any friend who already has (or is handed) a copy of the save directory opens it
themselves. That is a thirty-second social action in a friend group, not a gap.

What automatic election would actually have to solve, and at what cost:

- **Split-brain prevention.** If the leader briefly appears dead (§1's "prolonged unresponsiveness")
  and a second node auto-promotes itself while the first is still alive, two nodes now believe they
  are the sole owner of authoritative state — precisely the failure mode `Require<Trusted>`'s
  single-owner guarantee (ARCHITECTURE.md §0 S1) exists to prevent in the first place. Solving this
  correctly needs a quorum or fencing protocol — real distributed-systems machinery.
- **Quorum sizing.** A meaningful quorum needs at least 3 nodes to tolerate 1 failure; a 2-node
  friend session (the common case at this project's low end) cannot form a majority at all, so
  "automatic election" would need a special-cased fallback for the most common deployment shape
  anyway — undermining the generality that would justify building it.
- **What it buys:** skipping one manual restart click, for a player base that already tolerates
  "the Minecraft/Valheim host closed the server" as a familiar, accepted genre convention
  (ARCHITECTURE.md §2's own framing: "người chơi hiểu và chấp nhận điều này" — players understand
  and accept this).

This is the same shape of argument ARCHITECTURE.md §0 S1 already made about OIDC — *"Tất cả đúng cho
một MMO thương mại. Với dự án này chúng là chi phí không đổi lại được gì"* (all correct for a
commercial MMO; for this project they're a cost that buys nothing) — applied here to election instead
of authentication. **Ruling: automatic/Byzantine-fault-tolerant leader election remains out of scope,
not because it is hard, but because at this project's stated scale and trust model it would spend
real distributed-systems complexity solving a problem a friend group already solves themselves in
thirty seconds.** If the project's scale assumption ever changes (a public, always-on, VPS-hosted
deployment — explicitly not the current design, ARCHITECTURE.md §2), this ruling should be revisited
from scratch, not patched.

#### 5.3 The one addition this RFC does make: naming "promote another machine" explicitly

Any player holding (or handed) a copy of `saves/<world_name>/` can start their own machine as the new
leader for that world — this is not a new mechanism, it is "open the game and load that world,"
already true of every world today. This RFC's only contribution is naming it as the explicit answer
to "the original host isn't coming back, what do we do," rather than leaving it undiscoverable. It
carries one named, unsolved risk (§5.4).

#### 5.4 Split-brain is named, not solved

If the original host's machine comes back online after a friend has already manually relaunched as
leader, both processes would independently believe they are canonical for the same `world_name`. This
RFC does not solve this — no lock file, no fencing token, no distributed check is proposed (Open
Questions §2). At this project's trust model, the mitigation is social, not technical: a friend group
that has already agreed "so-and-so is hosting now" simply does not also open the original copy — the
same "kick is enough" reasoning ARCHITECTURE.md §0 S1 already applies to hostile actors applies here
to an honest coordination mistake, which is a strictly easier problem.

### 6. Messaging content — tone-guardrail-safe copy guidance

Every player-facing string this RFC specifies avoids: percentages, ETAs, error codes, retry counts,
and any framing of "you failed" or "you lost." Concretely:

| Situation | Copy |
|---|---|
| `LeaderUnreachable` (crash/timeout) | "Connection to the world lost — waiting for the host's game to come back." |
| `HostClosed` (graceful `WorldClosing`) | "The host closed the world." |
| Reconnect succeeded | (no message needed — the world simply resumes) |
| Explaining what's safe (on-demand, e.g. a "what happened?" link from the banner) | "Your farm, inventory, and progress are saved. You might lose up to the last 30 seconds of movement, and any dungeon you were inside will need a fresh start — but nothing you own or earned is gone." |

---

## Interactions with Other RFCs

| RFC | Relationship |
|---|---|
| **RFC-013** (accepted) | §7's universal hearth-respawn "no sane position" fallback is reused verbatim as the final fallback in this RFC's §4 recovery ledger when RFC-014's own return-coordinate fallback is unavailable. This RFC adds no new death or vitals rule. |
| **RFC-014** (accepted) | Owns instanced-band session lifecycle and its own leader-death ruling ("no `MapId` in the instanced band survives a restart") — cited verbatim in §4's table, not modified. §6/§6.1's disconnect/reconnect/session-closed fallback is this RFC's basis for what a reconnecting player experiences; this RFC adds the *detection and messaging* layer RFC-014 explicitly declined to build ("no client-facing disconnect/network layer exists... RFC-015's territory"). This RFC also independently reaffirms RFC-014's own Non-goals deferral of automatic election, arguing the reasoning in full (§5.2) rather than only citing it. |
| **RFC-015** (accepted) | Supplies the `Tick`/`ChunkDelta` wire shapes and send-cadence this RFC's client-side liveness signal (§2.2) composes with. This RFC does not modify RFC-015's accepted message shapes, delta rules, or interest-set mechanism — it adds one small, content-independent cadence on top, and picks up RFC-015's own explicitly-deferred "reconnection at the socket/session layer" question only at the application level (never socket/transport), leaving that deferral to P6 intact. |
| **RFC-016** (accepted) | Entirely supplies §4's recovery ledger: §6.3's periodic checkpoint cadence and ≤60s arithmetic, §9's portable save-directory contract and exclusion list, §7's `return_map`/`return_x`/`return_y` breadcrumb. This RFC introduces no new persisted field and no new persistence model — it only assembles an existing ruling into a player-facing table and a restart flow (§5.1) that reads RFC-016's save directory unmodified. |
| **RFC-011** (proposed, not yet specced) | Owns the HUD/banner's exact visual presentation (widget, placement, styling) for the `LeaderUnreachable`/`HostClosed` states this RFC defines the copy and trigger conditions for. This RFC specifies *when* and *what text*, not the pixel-level banner design — flagged, not designed (Open Questions §3). |
| **ARCHITECTURE.md §2** (design canon) | This entire RFC is the operationalization of one paragraph there — the leader-death policy this RFC gives a detection mechanism, a client state machine, and a recovery ledger to, changing none of its substance. |

---

## Multiplayer & Simulation-LOD Considerations

- **20–50 concurrent players, `kMaxPlayers = 8` today.** Every mechanism this RFC specifies is O(1)
  per observer (one timeout comparison per node, one per client) — nothing here scales with player
  count, chunk count, or map size, so the gap between today's session-slot ceiling and the 20–50
  target (already noted by RFC-015 §Multiplayer as a P6-era concern, not this RFC's) does not change
  any number in this design.
- **LOD is orthogonal.** Simulation tick-tier LOD (10/1/0 Hz, ARCHITECTURE.md §4's sketch) governs
  whether a *chunk* ticks; this RFC's detection signals govern whether a *node or client* is still
  hearing from the leader at all — a chunk asleep due to LOD is a different, already-covered case
  (idle chunks simply publish rarely, per RFC-015's `kIdlePublish` framing) from a leader that has
  stopped sending `Tick` entirely to every chunk regardless of LOD tier. This RFC does not touch LOD
  policy in any way.
- **Single leader only — no multi-leader or sharded-authority case exists to consider.** The entire
  premise of this RFC (one trusted source of truth) is unchanged; nothing here anticipates or
  prepares for a future multi-leader architecture, because none is planned (§5.2).
- **Non-leader peer node failure is a different, already-solved problem, out of this RFC's scope.**
  A friend's machine hosting some chunks (not the leader) crashing is covered by existing
  architecture, not this RFC: terrain is a pure function of `(seed, map, x, y)` (ARCHITECTURE.md §1 —
  "re-place chunk sau khi node chết trở nên rẻ," cheap chunk re-placement after a node dies) and
  world-overlay state is already durable on the leader via RFC-016's `Persistent<EventSourced, Sync>`
  — a lost peer node's chunks are simply re-hosted elsewhere and replay from already-committed state,
  no new mechanism needed. This RFC is specifically about the leader — the one node whose loss has no
  such cheap self-healing path — and does not extend its detection/messaging design to ordinary peer
  nodes, which is a strictly smaller, already-covered problem.

---

## Tone Guardrail Compliance

Walked against GAME.md §0's test individually, because this is the section most likely to hide a
disguised countdown.

1. **`kClientLeaderTimeoutTicks`/`kNodeLeaderTimeoutTicks` are never displayed, to anyone, under any
   circumstance.** There is no "reconnecting in 7 seconds" element, no progress bar counting toward
   the timeout. The banner appears once, fully formed, after the threshold — the threshold itself is
   an internal engineering constant (identical framing to RFC-016 §Tone Guardrail Compliance's own
   ruling on its periodic-checkpoint cadences), never surfaced as a number a player watches tick down.
2. **No auto-give-up (§3.4).** There is structurally no terminal "connection failed permanently"
   state and no timer counting toward one — a player can leave the banner up indefinitely with zero
   consequence, the same "waiting costs nothing" shape RFC-020 §Tone Guardrail Compliance already
   established for quest expiry.
3. **Disconnection/leader loss is never framed as the player's failure.** The banner does not say
   "you were disconnected," "connection error," or anything implying fault — it states a fact
   ("connection to the world lost") and a next step ("waiting for the host's game to come back"),
   mirroring RFC-014 §Tone Guardrail Compliance point 3's own framing for a dropped connection:
   "the game does not ask why they left," extended here to "why the world stopped."
4. **The ≤30-second position/vitals drift (§4) is disclosed honestly, not hidden or minimized, and
   never framed as a penalty — a deliberate, narrow departure from RFC-016's own framing, argued here
   rather than dressed up as a citation of it.** RFC-016 §Tone Guardrail Compliance point 4 states,
   unconditionally: *"The ≤60-second bound is an internal engineering budget, never a player-facing
   number. Nothing in this RFC's design surfaces 'your last 30 seconds might not have saved' to a
   player under any circumstance."* That rule is correct for RFC-016's own scope — an ordinary save
   cadence during normal play that nobody should ever be made to watch or worry about. This RFC's
   situation differs in kind: the banner in §3.3 has already told the player something went wrong (the
   leader is unreachable), and the on-demand text in §6 answers a question the player has already
   started asking — a single, past-tense, one-time disclosure made *after* an incident is legible to
   the player, not an ambient countdown or a caveat attached to ordinary play, which is what RFC-016's
   rule guards against. Stating the number plainly here ("up to the last 30 seconds of movement") is
   this RFC's own honesty argument, made in its own voice, not an application of RFC-016's rule — see
   Open Questions §8 for the resulting open tension between the two RFCs' framings, which this RFC
   flags rather than resolves unilaterally.
5. **The graceful-shutdown notice (§3.5) is informational, not gatekeeping.** The host is never
   blocked from quitting by a confirmation the player must fight through — the courtesy notice is a
   single line the host can dismiss instantly; nothing about this RFC makes closing one's own game
   harder or slower.
6. **Manual restart (§5.1) carries no loss-framing beyond the honest ledger in §4.** Relaunching a
   world is presented as "load a world," identical in tone to any other world-open flow — never as
   "recovery from disaster," which would editorialize a routine action into an alarming one.
7. **Split-brain (§5.4) is a trust-model non-issue, not a hidden vulnerability being quietly accepted.**
   Consistent with ARCHITECTURE.md §0 S1's own stance that `Require<Trusted>` exists for correctness
   among friends, not defense against them — an honest coordination mistake between two friends
   deciding who's hosting is not dressed up as a security concern this RFC pretends to have solved.

---

## Open Questions

1. **Exact wire carriage of the client-side liveness signal (§2.2).** A dedicated `WorldAlive{tick}`
   message, or an otherwise-empty `ChunkDelta` still sent at `kLeaderLivenessPeriodTicks` for the
   client's own standing chunk — either satisfies this RFC's cadence/timeout contract; the choice is
   left to whoever implements RFC-015's transport layer.
2. **Split-brain guard.** No lock file, fencing token, or distributed check is proposed (§5.4). Is a
   trivially simple mitigation (e.g., a `world.lock` file the leader process holds exclusively while
   running, refused by a second process trying to open the same directory) worth adding cheaply now,
   or is the purely social mitigation genuinely sufficient at this scale? Flagged, not decided.
3. **Banner/UI ownership.** This RFC specifies triggers and copy; the pixel-level widget (placement,
   styling, whether it's a full-screen overlay or a corner toast) is left to whichever RFC owns HUD
   chrome (RFC-011, proposed) or to implementation discretion if RFC-011 declines it.
4. **Are `kLeaderLivenessPeriodTicks`/`kClientLeaderTimeoutTicks`/`kNodeLeaderTimeoutTicks` right, or
   just plausible starting numbers?** All three are marked (tunable) and none are measured against
   real network conditions — P6's actual `TcpTransport`/`RelayTransport` latency characteristics
   (unbuilt today) should inform a revision once they exist.
5. **Do solo-play sessions (no other clients connected) need this RFC's detection/messaging at all?**
   A single-player host who crashes has nothing to notify — there's no other client watching. Whether
   the detection machinery should simply not run in that case (an optimization) or run harmlessly
   with no observable effect is left to implementation.
6. **Exact host-side courtesy-notice UX (§3.5)** — a blocking-but-instant-dismiss dialog, a
   non-blocking toast, or something else — is named as desirable but not designed here.
7. **`RelayTransport`/NAT failure modes (P6, unbuilt).** Whether a relay-path failure (host reachable,
   relay down) should present identically to a true leader-unreachable state, or needs its own
   distinguishable message, cannot be answered before P6's relay design exists.
8. **Tension with RFC-016 §Tone Guardrail Compliance point 4 — closed.** RECONCILIATION.md Ruling 18
   accepts this RFC's own honesty argument above (a one-time post-incident disclosure differs in kind
   from an ambient caveat during ordinary play) as the resolution, and narrows RFC-016 point 4 to name
   this RFC's §6 disclosure as its sole exception. No further change needed here.

---

## Non-goals

- **Anti-cheat, hostile-leader defense, or any validation-hardening mechanism.** This is the central
  scope boundary, argued at the same rigor as a Tone Guardrail argument because getting it wrong here
  would contradict deliberate project philosophy, not just add unwanted scope. ARCHITECTURE.md §2
  states, verbatim: *"Chống gian lận. Với nhóm bạn thì kick là đủ. `Require<Trusted>` vẫn giữ nhưng vì
  lý do đúng đắn kỹ thuật, không phải phòng thủ"* — "Anti-cheat: for a friend group, kicking is
  enough. `Require<Trusted>` is kept, but for a correct technical reason [ensuring exactly one
  authoritative owner of state], not as a defense mechanism." This RFC's entire detection/messaging
  design assumes the leader that goes silent did so **by accident** — a crash, a network drop, a
  closed laptop lid. It does not attempt to distinguish "the leader is gone" from "the leader is
  lying," does not add any signature, challenge-response, or state-validation step to the liveness
  signal in §2, and would be a direct contradiction of ARCHITECTURE.md's design philosophy if it did.
  A future RFC targeting a genuinely different deployment shape (a public server, not a friend group)
  would need to revisit this from first principles — this RFC explicitly does not attempt to serve
  that case.
- **Automatic or Byzantine-fault-tolerant leader election.** Reaffirmed, not merely cited — §5.2
  argues in full why ARCHITECTURE.md §2's *"Bầu leader tự động là chuyện của sau này, nếu có bao giờ
  cần"* (automatic leader election is a problem for later, if it's ever even needed) still holds at
  this project's current scale, rather than treating the deferral as stale now that persistence is
  real.
- **PvP-specific concerns.** PvP is off by default (GAME.md); this RFC's detection/messaging applies
  identically regardless of PvP state and does not address any PvP-specific failure scenario (e.g.,
  "did I die to lag or to a real hit" — out of scope, and moot besides, since §3.3 rules that no
  combat consequence occurs while `LeaderUnreachable`).
- **Transport-level connection establishment, socket handling, TLS, or `RelayTransport`.** Entirely
  P6's (ROADMAP.md) and RFC-015's own named deferred territory. This RFC's detection signal (§2) is
  specified at the application level and assumes whatever connection P6 eventually establishes; it
  does not design, and does not need, anything below that layer.
- **Instance/session lifecycle mechanics, teardown timers, or the reconnect-in-place mechanism.**
  Entirely RFC-014's (§3.5, §6). This RFC only consumes RFC-014's own leader-death ruling and §6.1
  fallback, cited, never modified.
- **Persistence mechanics, save-file schema, or checkpoint cadence values.** Entirely RFC-016's. This
  RFC's recovery ledger (§4) is assembled from RFC-016's numbers, never re-derives or overrides them.
- **Non-leader (ordinary peer) node failure.** A different, already-solved problem — see Multiplayer
  & Simulation-LOD Considerations. This RFC's detection/messaging design is specific to the leader.
- **Load balancing, chunk migration policy, or any redistribution of chunk ownership.** Unrelated to
  leader failure; not addressed here.

---

## Review Record

- Reviewer A: revise (mustFix: Tone Guardrail point 4 fabricated RFC-016 quote).
- Reviewer B: revise (mustFix: fabricated quote; false "zero hits" heartbeat grep claim; two RFC-016
  citation-attribution slips in §4's ledger table).
- Applied: rewrote Tone Guardrail point 4 to quote RFC-016 point 4 accurately (unconditional, no
  exception clause) and argue the one-time-disclosure-vs-ambient-caveat distinction in this RFC's own
  voice.
- Applied: added Open Questions §8 flagging the resulting open tension with RFC-016 point 4 for
  reconciliation, per Reviewer B's option (a).
- Applied: fixed header grounding claim — `election`/`quorum` are zero hits, but `heartbeat` appears
  twice as comment language for the shipped `Tick` fan-out (`protocol.hpp:23`, `map_director.hpp:70`).
- Applied: §4 "Creature and wildlife positions" row now attributes the "zero persistence obligation"
  quote to RFC-016 Summary point 3 (which cites ARCHITECTURE.md §3), not to ARCHITECTURE.md directly.
- Applied: §4 "Buildings, crops, tilled ground" row now cites RFC-016 §6.2 for the mechanism and
  RFC-016 Summary point 3 for the quoted sentence, not §6.2 for both.
- Unresolved: none — all mustFix items from both reviewers had sound proof and were applied.

---
