// RFC-001 — the seven-phase ability pipeline: Cast -> Channel -> Release -> Travel -> Impact ->
// Persist -> Expire, as a state machine with a fixed transition table (rfc-spec/RFC-001-ability-
// system.md). Every attack in the game is meant to walk this road eventually; today only the
// PLAYER ability path (player_actor.hpp) is wired through it — creature/boss wind-up
// (chunk_actor.hpp's commit_windup/resolve_windup, boss.hpp) already implements the same phases by
// hand, and RFC-001 treats that as prior art to NAME, not to rewrite ("a semantic contract, not a
// base class").
//
// WHY THIS IS A SEPARATE HEADER FROM abilities.hpp, AND WHY THE FUNCTIONS TAKE PLAIN VALUES rather
// than an AbilityId. abilities.hpp is a content table: six hand-authored moves. This file is the
// ENGINE those moves run on, and per RFC-001 Section 9 that engine's whole point is to be a pure
// function of DATA ("a new skill is a row of data plus existing phase implementations"), not of
// which specific ability is running. Taking `cast_ticks`, `has_channel`, etc. as parameters (rather
// than reaching into `ability_def(id)` itself) means this machine is exercised directly by tests
// with synthetic tick counts even though none of the six shipped abilities has a cast time or a
// channel block yet (RFC-001 Section 9's own ruling: they "map onto this machine with
// cast_ticks = 0, no channel" until RFC-008 lands real authored content).
#pragma once

#include <cstdint>

#include "world/abilities.hpp"

namespace mmo {

// ------------------------------------------------------------------------------------------------
// Phases (RFC-001 Section 1). kIdle is the absence of an instance, not a phase of one.
enum class AbilityPhase : std::uint8_t {
    kIdle = 0,
    kCast = 1,
    kChannel = 2,
    kRelease = 3,
    kTravel = 4,
    kImpact = 5,
    kPersist = 6,
    kExpire = 7,
};

// TargetingModel (Section 7) is declared in abilities.hpp: it is an AbilityDef field, so the
// content table needs it before this header is reachable from there.

// The payload taxonomy (Section 1). The shipped content only ever produces the first three; kDash
// and kSpawnEntity are reserved values that need RFC-004's CombatEntity chassis to mean anything
// (a travel body or a persist entity with HP/collision/team) and are unused until it lands.
enum class PayloadKind : std::uint8_t {
    kInstantHit = 0,
    kProjectile = 1,
    kDash = 2,
    kSpawnEntity = 3,
    kZone = 4,
};

// Maps today's coarse dispatch selector (abilities.hpp's AbilityKind, which decides which CHUNK
// MESSAGE carries a resolved ability) onto the RFC's payload vocabulary (which decides which PHASES
// apply, Section 1's table). The two enums answer different questions, so the mapping is a real
// function, not a rename: AbilityKind is "how do I tell the chunk", PayloadKind is "which phases
// does this data invariant-check against."
[[nodiscard]] inline constexpr PayloadKind payload_kind_of(AbilityKind k) noexcept {
    switch (k) {
        case AbilityKind::kStrike: return PayloadKind::kInstantHit;
        case AbilityKind::kVolley: return PayloadKind::kProjectile;
        case AbilityKind::kZone: return PayloadKind::kZone;
    }
    return PayloadKind::kInstantHit;
}

// ------------------------------------------------------------------------------------------------
// Tunables (RFC-001, each marked (tunable) in the spec).
inline constexpr std::uint16_t kChannelGraceTicks = 3;   // Section 6 — sub-grace release cancels
inline constexpr std::uint16_t kStaggerTicks = 5;        // Section 5 — uniform post-T12 recovery
inline constexpr float kCastPoiseFrac = 0.12f;           // Section 5 — hit fraction that breaks Cast
inline constexpr float kChannelPoiseFrac = 0.08f;        // Section 5 — Channel is squishier
inline constexpr std::uint16_t kMinEnemyTelegraph = 4;   // Section 9 V2 — baseline hostile floor
inline constexpr std::uint16_t kMinHeavyTelegraph = 8;   // Section 9 V2 — heavy/committed floor
inline constexpr std::uint16_t kMaxPersistTicks = 600;   // Section 9 V3 — 60s, nothing persists forever

// ------------------------------------------------------------------------------------------------
// The head instance (Section 9's exact shape) — lives on the caster's actor; at most one per
// caster (Invariant I1). `ability` is an opaque index into whatever content table is calling this
// machine (abilities.hpp's AbilityId today), never interpreted here.
struct AbilityHead {
    std::uint16_t ability = 0;
    AbilityPhase phase = AbilityPhase::kIdle;
    std::uint8_t phase_elapsed = 0;
    std::uint16_t charge_elapsed = 0;      // Channel only
    float aim_x = 0.0f, aim_y = 0.0f;      // provisional until frozen at Release (I3)
    float dir_x = 0.0f, dir_y = 0.0f;
    std::uint64_t target_key = 0;          // kEntity only
};

// The one message that crosses from the head's owner to the tail's owner at Release (Section 4).
struct AbilityPayload {
    std::uint64_t caster = 0;
    std::uint8_t team = 0;
    std::uint16_t ability = 0;
    float x = 0.0f, y = 0.0f;              // frozen aim point (I3)
    float dir_x = 0.0f, dir_y = 0.0f;      // frozen unit direction
    std::uint16_t charge_mil = 1000;       // fixed-point 0..1000; 1000 when no channel block (Section 6)
    std::uint64_t release_tick = 0;
};

// ------------------------------------------------------------------------------------------------
// Admission (Section 3) — check-and-debit's "check" half, evaluated in the RFC's fixed priority
// order. A pure function so every reject reason is directly testable without a caster actor at
// all: build the seven booleans/phase and read off the answer.
[[nodiscard]] inline constexpr AbilityReject reject_of(bool unavailable, bool locked,
                                                       bool on_cooldown, bool lacks_resource,
                                                       AbilityPhase head_phase, bool staggered,
                                                       bool bad_target) noexcept {
    if (unavailable) return AbilityReject::kUnavailable;
    if (locked) return AbilityReject::kLocked;
    if (on_cooldown) return AbilityReject::kCooldown;
    if (lacks_resource) return AbilityReject::kResource;
    if (head_phase != AbilityPhase::kIdle) return AbilityReject::kBusy;  // Invariant I1
    if (staggered) return AbilityReject::kBusy;                          // post-T12 recovery window
    if (bad_target) return AbilityReject::kBadTarget;
    return AbilityReject::kOk;
}

// T1 — Idle -> Cast. Caller has already run reject_of() and debited the cost; this just seats the
// head. Returns false (and leaves `head` alone) if I1 is somehow violated, which should never
// happen if reject_of() was honored — the check exists so a caller bug fails loud, not silent.
inline constexpr bool try_start_cast(AbilityHead& head, std::uint16_t ability_ref, float aim_x,
                                     float aim_y, float dir_x, float dir_y) noexcept {
    if (head.phase != AbilityPhase::kIdle) return false;
    head = AbilityHead{};
    head.ability = ability_ref;
    head.phase = AbilityPhase::kCast;
    head.aim_x = aim_x;
    head.aim_y = aim_y;
    head.dir_x = dir_x;
    head.dir_y = dir_y;
    return true;
}

// T2/T3/T4's clock — called once per tick (or once synchronously at admission time for a
// cast_ticks == 0 ability, which is every shipped ability today) while `head.phase` is kCast or
// kChannel. Advances the elapsed counter for the current sub-phase and applies the transition when
// its duration is reached. `channel_max_ticks` is T4's forced-release bound (a held charge cannot
// run forever); the caller's own input loop is what would normally fire T4 early via
// release_channel() instead.
inline constexpr AbilityPhase advance_head(AbilityHead& head, std::uint16_t cast_ticks,
                                           bool has_channel, std::uint16_t channel_max_ticks) noexcept {
    if (head.phase == AbilityPhase::kCast) {
        ++head.phase_elapsed;
        if (head.phase_elapsed >= cast_ticks) {
            head.phase = has_channel ? AbilityPhase::kChannel : AbilityPhase::kRelease;
            head.charge_elapsed = 0;
        }
    } else if (head.phase == AbilityPhase::kChannel) {
        ++head.charge_elapsed;
        if (head.charge_elapsed >= channel_max_ticks) {
            head.phase = AbilityPhase::kRelease;  // T4: max charge reached, forced release
        }
    }
    return head.phase;
}

// The client's "let go" action during Channel. Sub-grace release is a voluntary cancel, not a fire
// (Section 6): the transition table routes T4 -> T12 instead when `charge_elapsed <
// kChannelGraceTicks`. Returns true if it actually released (phase is now kRelease), false if it
// routed to cancel (phase is now kIdle — the caller still owes the grace-tap refund, see
// apply_interrupt's Channel row, which a grace tap ALSO uses per Section 6's own cross-reference).
inline constexpr bool release_channel(AbilityHead& head) noexcept {
    if (head.phase != AbilityPhase::kChannel) return false;
    if (head.charge_elapsed < kChannelGraceTicks) {
        head.phase = AbilityPhase::kIdle;
        return false;
    }
    head.phase = AbilityPhase::kRelease;
    return true;
}

// Charge fraction at Release, as the fixed-point 0..1000 AbilityPayload::charge_mil carries
// (Section 6). Never called for a channel-less ability — those always send 1000 (full power),
// never curve-penalized, by construction (the caller simply never enters kChannel).
[[nodiscard]] inline constexpr std::uint16_t charge_mil_of(std::uint16_t charge_elapsed,
                                                           std::uint16_t channel_full_ticks) noexcept {
    if (channel_full_ticks == 0) return 1000;
    const std::uint32_t f = (static_cast<std::uint32_t>(charge_elapsed) * 1000u) / channel_full_ticks;
    return static_cast<std::uint16_t>(f > 1000u ? 1000u : f);
}

// T12's refund/cooldown split (Section 5), uniform across every reason an interrupt fires (death,
// poise-break, voluntary cancel all call this the same way for a Cast; only a FORCED break during
// Channel calls it for Channel — a voluntary Channel exit is release_channel()'s grace-tap path,
// above, which is refund-everything by a separate rule). `stagger_ticks` is always kStaggerTicks:
// the RFC is explicit that the post-T12 recovery window does not vary by interrupt reason.
struct InterruptResult {
    bool refund_base = false;          // refund the cost debited at T1
    bool refund_channel_drain = false; // also refund stamina/mana drained during Channel ticks
    bool charge_half_cooldown = false; // caller sets ready_at_tick = now + ceil(cooldown / 2)
    std::uint16_t stagger_ticks = kStaggerTicks;
};

[[nodiscard]] inline constexpr InterruptResult apply_interrupt(AbilityPhase phase_before) noexcept {
    if (phase_before == AbilityPhase::kChannel) {
        // "the mercy is the cooldown, not the resource" — both the base cost and the drained
        // ticks are forfeit; a broken channel still only costs half a cooldown, never a full one.
        return InterruptResult{false, false, true, kStaggerTicks};
    }
    // Cast (and anything else caught mid-interrupt): full refund, no cooldown ever started.
    return InterruptResult{true, false, false, kStaggerTicks};
}

// A grace tap (release_channel() returning false) is a voluntary T12 exit, not a forced one, and
// Section 6 rules it refund-everything: the base cost AND the ticks drained so far, no cooldown.
[[nodiscard]] inline constexpr InterruptResult grace_tap_result() noexcept {
    return InterruptResult{true, true, false, kStaggerTicks};
}

// ------------------------------------------------------------------------------------------------
// Data invariants (Section 9, V1/V3 — V2 and V4 are documented but have no caller yet: V2 gates
// HOSTILE ability data, which is still hand-scripted creature/boss code, not this table; V4 is
// already satisfied by tiles.hpp's effect_life_of and has nothing new to check until RFC-006 adds
// FX families this table can reference). Both remaining checks are constexpr so the shipped table
// is validated at COMPILE time, the same way abilities.hpp's own constexpr table already is.

// V1 — a kDash payload must have a cast (an uncommitted dash is unreadable) and must not have a
// channel (out of scope in v1). Only meaningful once kDash content exists (RFC-004); for today's
// three shipped payload kinds every combination of cast/channel is legal, so this always passes.
[[nodiscard]] inline constexpr bool valid_phase_applicability(PayloadKind payload,
                                                               std::uint16_t cast_ticks,
                                                               bool has_channel) noexcept {
    if (payload == PayloadKind::kDash) return cast_ticks > 0 && !has_channel;
    return true;
}

// V3 — nothing persists indefinitely.
[[nodiscard]] inline constexpr bool valid_persist_ticks(std::uint16_t persist_ticks) noexcept {
    return persist_ticks <= kMaxPersistTicks;
}

}  // namespace mmo
