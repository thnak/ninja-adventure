// RFC-024 — Leader Failure & Session Recovery.
//
// Confirmed before writing a line of this file (matching RFC-015's own opening discipline,
// `replication.hpp:1-11`): grep of `src/` for `election`/`quorum` is zero hits, and `client_main.cpp`
// builds and runs `World` in the SAME process as the renderer (`client_main.cpp:133-139`) — there is
// no separate client machine today, and therefore no leader-unreachable condition to actually observe
// (RFC-024 §2.3, "nothing above claims to be running code"). What this file builds is the same shape
// of thing `replication.hpp` built for RFC-015: the real, pure, testable STATE MACHINE and tunables a
// future P6 transport layer would drive, so the migration from "single process" to "leader on one
// machine, clients on others" touches wiring, not logic.
//
// SCOPE NARROWED TO WHAT IS REAL TODAY. RFC-024 §2's detection signals (a peer node watching `Tick`
// fan-out, a client watching a liveness cadence) have no live transport to observe yet — `ClientLiven-
// essTracker`/`NodeLivenessTracker` below are exercised directly by tests (`sim_main.cpp`), the same
// way RFC-014's `Unbind`/`Rebind` are "exercised directly by tests/tools" per `world.hpp`'s own
// precedent (`world.hpp:309-312`), not by a live detector nobody has built. §3.5's `WorldClosing`
// broadcast (`protocol.hpp`) is the one new wire message this RFC specifies; it is defined here in the
// data-level shape RFC-014's `Unbind` already established for "a future detector would send this,"
// and `World::connected_player_count()` (`world.hpp`) plus `host_closing_notice()` below give
// `client_main.cpp`'s real shutdown path a genuine courtesy line (§3.5) rather than an inert type with
// no caller. §4's recovery ledger and §6's tone-guardrail copy are reproduced verbatim as named string
// constants — the "assembled...for a player-facing answer" RFC-024 itself describes — for whichever
// RFC eventually owns HUD chrome (RFC-011, proposed, not yet built) to consume unmodified.
//
// DEFERRED, NAMED EXPLICITLY: §5.1's manual restart/resume flow is "not a new code path" per the
// RFC's own text (`World::build()`/`start()`/`login()` already do it) — nothing to add. §5.2's
// automatic election and §5.4's split-brain guard are the RFC's own reaffirmed non-goals / open
// question (Open Questions §2: "flagged, not decided") — no lock file or fencing token is added here.
// §2's exact wire carriage of the client liveness signal (dedicated message vs. riding `ChunkDelta`)
// is Open Question 1, left to whoever wires up RFC-015's transport — this file specifies the
// cadence/timeout CONTRACT (`kLeaderLivenessPeriodTicks`/`kClientLeaderTimeoutTicks`), not the framing.
#pragma once

#include <cstdint>
#include <string>

namespace mmo {

// --- §2.1: node-side detection (a chunk-hosting peer watching the Tick fan-out) --------------------

inline constexpr std::uint64_t kNodeLeaderTimeoutTicks = 50;  // 5 s at kTicksPerSecond=10

// --- §2.2: client-side detection ---------------------------------------------------------------

inline constexpr std::uint64_t kLeaderLivenessPeriodTicks = 30;   // 3 s — sent unconditionally
inline constexpr std::uint64_t kClientLeaderTimeoutTicks = 100;   // 10 s — no traffic at all

// §3.2's state machine, exactly three states, exactly the edges drawn there:
//   Connected --(kClientLeaderTimeoutTicks elapses with no traffic)--> LeaderUnreachable
//   Connected --(WorldClosing received)-------------------------------> HostClosed
//   LeaderUnreachable --(traffic resumes)------------------------------> Connected
// HostClosed has no outgoing edge in §3.2's diagram — a fresh relaunch (§5.1) is a new connection,
// not a transition of this tracker.
enum class ConnectionState : std::uint8_t {
    kConnected = 0,
    kLeaderUnreachable = 1,
    kHostClosed = 2,
};

// One connected client's view of leader liveness (§2.2/§3.2). Pure state, no socket, no timers of
// its own — a future transport calls `on_traffic()` whenever anything (content or the liveness
// cadence) arrives, and `advance()` once per local frame/tick to evaluate the timeout.
class ClientLivenessTracker {
public:
    // Any inbound traffic — content or the liveness cadence — resets the timeout and, per §3.2,
    // moves a LeaderUnreachable session straight back to Connected. `HostClosed` is terminal: a
    // graceful announcement is never un-said by more traffic arriving after it (there shouldn't be
    // any — the leader is tearing down — but this keeps the state machine's own edges honest even if
    // some did).
    void on_traffic(std::uint64_t tick) noexcept {
        if (state_ == ConnectionState::kHostClosed) return;
        last_traffic_tick_ = tick;
        has_traffic_ = true;
        state_ = ConnectionState::kConnected;
    }

    // §3.5: the one message this RFC adds. Skips the timeout wait entirely, because the reason is
    // already known and honest.
    void on_world_closing() noexcept { state_ = ConnectionState::kHostClosed; }

    // Call once per local tick/frame. Before the first `on_traffic()` (a session that has not yet
    // received anything) never times out on its own — there is nothing to have gone silent FROM yet.
    [[nodiscard]] ConnectionState advance(std::uint64_t tick) noexcept {
        if (state_ == ConnectionState::kConnected && has_traffic_ &&
            tick - last_traffic_tick_ >= kClientLeaderTimeoutTicks) {
            state_ = ConnectionState::kLeaderUnreachable;
        }
        return state_;
    }

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }

private:
    ConnectionState state_ = ConnectionState::kConnected;
    std::uint64_t last_traffic_tick_ = 0;
    bool has_traffic_ = false;
};

// A chunk-hosting peer node's view of the same question (§2.1), against the already-shipped `Tick`
// fan-out rather than a dedicated liveness message — deliberately does not invent a tick number for
// ticks it never received (ARCHITECTURE.md §2c's determinism boundary, cited in §2.1).
class NodeLivenessTracker {
public:
    void on_tick(std::uint64_t tick) noexcept {
        last_tick_ = tick;
        has_tick_ = true;
    }

    [[nodiscard]] bool leader_unreachable(std::uint64_t now_tick) const noexcept {
        if (!has_tick_) return false;
        return now_tick - last_tick_ >= kNodeLeaderTimeoutTicks;
    }

    [[nodiscard]] std::uint64_t last_tick() const noexcept { return last_tick_; }

private:
    std::uint64_t last_tick_ = 0;
    bool has_tick_ = false;
};

// --- §6: tone-guardrail-safe copy, reproduced verbatim -----------------------------------------

inline constexpr const char* kLeaderUnreachableBanner =
    "Connection to the world lost - waiting for the host's game to come back.";
inline constexpr const char* kHostClosedBanner = "The host closed the world.";

// §4's recovery ledger, condensed into the single honest line the RFC itself proposes as the
// on-demand "what happened?" answer.
inline constexpr const char* kRecoverySummaryText =
    "Your farm, inventory, and progress are saved. You might lose up to the last 30 seconds of "
    "movement, and any dungeon you were inside will need a fresh start - but nothing you own or "
    "earned is gone.";

// §3.5: "Host-side courtesy, not a gate." A single fact-stating line, not a blocking confirmation —
// `client_main.cpp`'s shutdown path prints this (never withholds shutdown on it) when other accounts
// are still bound.
[[nodiscard]] inline std::string host_closing_notice(int other_players) {
    std::string s = std::to_string(other_players);
    s += other_players == 1 ? " other player is connected" : " other players are connected";
    s += " - closing now will disconnect them.";
    return s;
}

}  // namespace mmo
