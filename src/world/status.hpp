// RFC-002 — the Status & Effect Framework: build-up ladders replacing the shipped P2 model of one
// `Status` enum + a tick counter, instantly overwritten by any elemental hit.
//
// Five shared gauges (Cold, Heat, Shock, Earth, Stagger) on [0,1000], thresholds T1/T2/T3 =
// 300/600/900, promote a target through named stages (e.g. Slow -> HeavySlow -> Freeze) instead of
// setting a state outright. Exactly one ladder ("primary") is active at a time — the shipped P2
// invariant that made combos a decision — while a small set of binary coatings (Wet) sit alongside
// it, unaffected by the one-slot rule. A two-level soft-resist window is the anti-chain mechanism:
// there are no immunities anywhere in this file.
//
// WHY THIS IS A SEPARATE HEADER, mirroring `combat_entity.hpp`/`ability_pipeline.hpp`: pure data
// plus pure functions, with NO dependency on `tiles.hpp`/`ChunkActor` — not even `Element`, so that
// `tiles.hpp` can include this header for `Creature`'s new fields without a cycle. Where RFC-002's
// own text needs an `Element` (Conduct's "a Thunder-element hit"), the signature takes a plain
// `bool` instead and the Element->Channel/bool mapping lives at the one call site that has both
// types (`chunk_actor.hpp`), exactly as `combat_entity.hpp` stores `periodic_cast` as a raw
// `uint16_t` rather than including `abilities.hpp` back for `AbilityId`.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - `Gauge` is the minimal RFC-009 §4.8 storage shape this RFC cannot function without (value,
//     last_gain_tick, resist_until, resist_level) — NOT the full `DefenderSheet` (material, scale
//     tier, toughness, DR), which stays RFC-009's/RFC-003's to build. Superseded, not
//     reinterpreted, once that RFC lands (the same posture `combat_entity.hpp` already documents
//     for its own RFC-002 stand-in).
//   - `status_gain`'s `mult_pm` parameter is RFC-009's own "caller-supplied per-mille product"
//     (material x scale-tier) taken at face value: every call site in this pass supplies the
//     identity value 1000 (Material/ScaleTier do not exist in code yet). Wiring a real value in
//     later is then a call-site change, not a signature change.
//   - Absolute-tick fields are `uint64_t`, matching `tick_` everywhere else in this engine, not the
//     RFC prose's `uint32_t` — the same divergence `combat_entity.hpp` already made for `Scar`.
//   - Decay rates: RFC-009 §4.5 names only Cold(6)/Stagger(14) explicitly ("Cold 6/tick ...
//     Stagger 14/tick"); Heat/Shock/Earth are chosen tunables in between (see `channel_decay_rate`).
//   - The Stagger one-slot exemption (stage 1-2 never claims the primary) fires no side effect here:
//     Unsteady is a gauge-derived passive multiplying knockback this engine does not have yet
//     (RFC-003), and Staggered is a one-shot interrupt with no distinct engine mechanic beyond the
//     windup-cancel a terminal already performs — both are named in RFC-002 §4 but have nothing to
//     hook to yet, so the exemption branch below does exactly what its name says: it does not
//     promote, and nothing else.
//   - Combust (Heat's terminal) "resolves immediately": this file cannot compute its burst damage
//     (`min(15% max_hp, 60)`) since it has no access to a creature's `max_hp`; `status_step` instead
//     reports `combust = true` in its `StepResult` and the caller applies the damage.
#pragma once

#include <algorithm>
#include <cstdint>

namespace mmo {

// The five ladder channels. Cold/Heat/Shock/Earth map to the Ice/Fire/Thunder/Rock schools;
// Stagger is derived from physical hits, not authored by any school. Poison is deliberately absent
// (RFC-002 Open Question 2 parks it; the array is fixed at 5 for v1).
enum class Channel : std::uint8_t {
    kNone = 0,
    kCold = 1,
    kHeat = 2,
    kShock = 3,
    kEarth = 4,
    kStagger = 5,
    kCount = 6,
};

// Binary, coexist-freely layer. v1 has exactly one (Wet); the view byte reserves room for a second.
enum class Coating : std::uint8_t { kWet = 0, kCount = 1 };

enum class GainFlag : std::uint8_t {
    kNone = 0,
    kAmbient = 1 << 0,  // weather/terrain trickle — capped at stage 1 by the (unbuilt) caller, E1
    kOpposed = 1 << 1,  // set internally while a gain is being spent draining the opposite gauge
};

// 6 bytes; one per combatant (this pass: `Creature` only — see header note on player/entity scope).
struct StatusState {
    Channel primary = Channel::kNone;  // the ONE active ladder status
    std::uint8_t stage = 0;            // 1..3 while primary != kNone, else 0
    std::uint8_t stage_ticks = 0;      // ticks left at this rung (counts down)
    std::uint8_t coatings = 0;         // bitmask of Coating
    std::uint8_t coating_ticks[2] = {};  // Wet + one reserved slot (RFC-002 Open Question 5)
};

// The minimal RFC-009 §4.8 storage stand-in (header note). One per channel, five per combatant.
struct Gauge {
    std::uint16_t value = 0;          // [0, 1000]
    // Tick fields are `uint32_t`, NOT this engine's usual `uint64_t tick_` convention (a documented
    // divergence from `combat_entity.hpp`'s own precedent): `Creature` embeds `Gauge gauges[5]`
    // directly and migrates whole through `CreatureEnter`, a pooled message capped at a fixed 192
    // bytes (`quark::detail::MessagePool::kMaxPayload`) — five `uint64_t`-timestamped gauges alone
    // would overflow that budget before the rest of `Creature` is even counted. `uint32_t` wraps at
    // ~13.6 years of continuous uptime at 10 Hz, which is not a real constraint, and matches RFC-009
    // §4.8's own literal `uint32_t` `Gauge` more closely than the 64-bit choice would have.
    std::uint32_t last_gain_tick = 0;  // absolute tick of the most recent write
    std::uint32_t resist_until = 0;    // absolute tick the soft-resist window expires
    std::uint8_t resist_level = 0;     // 0..2, clamped — never increments unboundedly
};

// The runtime gain packet — one shape produced by every source (spells, physical derived-Stagger,
// auras). `source` is the player/creature key to credit a kill to; 0 = environment.
struct BuildupPacket {
    Channel channel = Channel::kNone;
    std::uint16_t amount = 0;  // Power on the [0,1000] gauge scale, pre-multiplier
    std::uint8_t flags = 0;    // GainFlag bits
    std::uint64_t source = 0;
};

struct CoatingPacket {
    Coating coating = Coating::kWet;
    std::uint8_t ticks = 0;
};

// The five P2 combos, moved here (same values/meaning) since detonation is now a ladder query.
enum class Combo : std::uint8_t {
    kNone = 0,
    kShatter = 1,  // Cold terminal (Freeze) + heavy melee: x2.5, ignores armour
    kBlast = 2,    // Heat stage >= 2 (Burning) + projectile: x1.6
    kConduct = 3,  // Wet coating + a Shock-element hit: x1.4, chains to nearby Wet enemies
    kCrush = 4,    // Earth stage >= 2 (Mired) + heavy melee: x1.3, Stagger build-up
    kArc = 5,      // Shock stage >= 1 + melee: x1.1, refunds mana, drops one rung
};

[[nodiscard]] inline constexpr float combo_damage_scale(Combo c) noexcept {
    switch (c) {
        case Combo::kShatter: return 2.5f;
        case Combo::kBlast: return 1.6f;
        case Combo::kConduct: return 1.4f;
        case Combo::kCrush: return 1.3f;
        case Combo::kArc: return 1.1f;
        case Combo::kNone: break;
    }
    return 1.0f;
}

[[nodiscard]] inline const char* describe(Combo c) noexcept {
    switch (c) {
        case Combo::kShatter: return "SHATTER";
        case Combo::kBlast: return "BLAST";
        case Combo::kConduct: return "CONDUCT";
        case Combo::kCrush: return "CRUSH";
        case Combo::kArc: return "ARC";
        case Combo::kNone: break;
    }
    return "";
}

// --- thresholds, durations, decay ------------------------------------------------------------------

inline constexpr std::uint16_t kThreshold1 = 300;
inline constexpr std::uint16_t kThreshold2 = 600;
inline constexpr std::uint16_t kThreshold3 = 900;

inline constexpr std::uint8_t kColdStage1Ticks = 80, kColdStage2Ticks = 50;   // Slow, HeavySlow
inline constexpr std::uint8_t kHeatStage1Ticks = 60, kHeatStage2Ticks = 50;   // Singed, Burning
inline constexpr std::uint8_t kShockStage1Ticks = 60, kShockStage2Ticks = 30; // Static, Shocked
inline constexpr std::uint8_t kEarthStage1Ticks = 60, kEarthStage2Ticks = 50; // Encumbered, Mired

// Terminal durations (creature/baseline values). Player-halved variants are part of the deferred
// player-target work (see header note in the .hpp this pass does not touch: `player_actor.hpp`).
inline constexpr std::uint8_t kFreezeTicks = 25;
inline constexpr std::uint8_t kParalyzeTicks = 12;
inline constexpr std::uint8_t kRootTicks = 20;
inline constexpr std::uint8_t kKnockdownTicks = 15;

inline constexpr std::uint32_t kDecayDelay = 15;    // grace ticks before a gauge starts draining
inline constexpr std::uint32_t kResistWindow = 150; // soft-resist window, ticks (15s @ 10Hz)
inline constexpr std::uint16_t kSoftResist[3] = {1000, 500, 250};  // per-mille, indexed by resist_level
inline constexpr std::uint16_t kThawBreakPower = 80;  // X1: a single Heat packet this big breaks Freeze

[[nodiscard]] inline constexpr int gauge_index_of(Channel ch) noexcept {
    return static_cast<int>(ch) - 1;  // kCold=0 .. kStagger=4; kNone/kCount are never indexed
}

[[nodiscard]] inline constexpr std::uint16_t channel_decay_rate(Channel ch) noexcept {
    switch (ch) {
        case Channel::kCold: return 6;     // RFC-009 §4.5's own named endpoint
        case Channel::kHeat: return 8;     // tunable — the RFC text only pins Cold/Stagger
        case Channel::kShock: return 10;   // tunable
        case Channel::kEarth: return 7;    // tunable
        case Channel::kStagger: return 14; // RFC-009 §4.5's own named endpoint
        case Channel::kNone:
        case Channel::kCount: break;
    }
    return 0;
}

// Non-terminal stage durations, and the terminal durations for stage 3 (except Heat, whose
// terminal resolves immediately — see `status_step`'s `combust` handling).
[[nodiscard]] inline constexpr std::uint8_t stage_duration(Channel ch, std::uint8_t stage) noexcept {
    switch (ch) {
        case Channel::kCold:
            if (stage == 1) return kColdStage1Ticks;
            if (stage == 2) return kColdStage2Ticks;
            if (stage == 3) return kFreezeTicks;
            break;
        case Channel::kHeat:
            if (stage == 1) return kHeatStage1Ticks;
            if (stage == 2) return kHeatStage2Ticks;
            break;  // stage 3 (Combust) has no duration; it resolves the instant it is entered
        case Channel::kShock:
            if (stage == 1) return kShockStage1Ticks;
            if (stage == 2) return kShockStage2Ticks;
            if (stage == 3) return kParalyzeTicks;
            break;
        case Channel::kEarth:
            if (stage == 1) return kEarthStage1Ticks;
            if (stage == 2) return kEarthStage2Ticks;
            if (stage == 3) return kRootTicks;
            break;
        case Channel::kStagger:
            if (stage == 3) return kKnockdownTicks;  // stages 1-2 never claim the slot (§4)
            break;
        case Channel::kNone:
        case Channel::kCount: break;
    }
    return 0;
}

[[nodiscard]] inline constexpr std::uint16_t threshold_of(std::uint8_t stage) noexcept {
    switch (stage) {
        case 1: return kThreshold1;
        case 2: return kThreshold2;
        case 3: return kThreshold3;
        default: return 0;
    }
}

[[nodiscard]] inline constexpr std::uint8_t would_stage(std::uint16_t value) noexcept {
    if (value >= kThreshold3) return 3;
    if (value >= kThreshold2) return 2;
    if (value >= kThreshold1) return 1;
    return 0;
}

// Closed-form decay (§3): no grace-period catch-up loop, one arithmetic step from the last write.
[[nodiscard]] inline constexpr std::uint16_t value_at(const Gauge& g, Channel ch,
                                                       std::uint64_t now) noexcept {
    if (now <= g.last_gain_tick) return g.value;
    const std::uint64_t elapsed = now - g.last_gain_tick;
    if (elapsed <= kDecayDelay) return g.value;
    const std::uint64_t drop = (elapsed - kDecayDelay) * channel_decay_rate(ch);
    return static_cast<std::uint16_t>(drop >= g.value ? 0 : g.value - drop);
}

// Movement multiplier the ladder imposes. Replaces `status_speed_scale(Status)` at its one call
// site. Terminals return 0.0 (a full stop) exactly as P2's Frozen did; a creature already in reach
// can still commit a swing before the movement code ever reads this, matching P2's own shape (see
// `chunk_actor.hpp`'s `step_creatures` — commit happens before the speed composition).
[[nodiscard]] inline constexpr float speed_scale_of(const StatusState& s) noexcept {
    switch (s.primary) {
        case Channel::kCold:
            if (s.stage == 1) return 0.85f;
            if (s.stage == 2) return 0.50f;
            if (s.stage == 3) return 0.0f;  // Freeze
            break;
        case Channel::kHeat:
            if (s.stage == 2) return 1.15f;  // Burning panics — runs faster
            break;                            // Singed: no speed effect
        case Channel::kShock:
            if (s.stage == 1) return 0.90f;
            if (s.stage == 2) return 0.70f;
            if (s.stage == 3) return 0.0f;  // Paralyze
            break;
        case Channel::kEarth:
            if (s.stage == 1) return 0.80f;
            if (s.stage == 2) return 0.45f;  // Mired, P2's Muddy, unchanged
            if (s.stage == 3) return 0.0f;   // Root: cannot move (may still act, per §2)
            break;
        case Channel::kStagger:
            if (s.stage == 3) return 0.0f;  // Knockdown
            break;
        case Channel::kNone:
        case Channel::kCount: break;
    }
    return 1.0f;
}

// --- coating multipliers (§5 X2-X4) ----------------------------------------------------------------

[[nodiscard]] inline constexpr std::uint16_t coating_mult_pm(Channel ch, std::uint8_t coatings) noexcept {
    const bool wet = (coatings & (1u << static_cast<std::uint8_t>(Coating::kWet))) != 0;
    if (!wet) return 1000;
    switch (ch) {
        case Channel::kHeat: return 500;    // X2: Wet douses Heat gain
        case Channel::kShock: return 1500;  // X3: Wet conducts Shock gain
        case Channel::kCold: return 1250;   // X4: wet things freeze faster
        default: return 1000;
    }
}

// The §3 terminal-exit rule, given precedence over the generic walk-down by every caller: the
// gauge empties, the soft-resist window arms, and the target exits THROUGH stage 1 rather than to
// stage 0 — the readable step-down is preserved as a stage, not banked gauge.
inline constexpr void resolve_terminal_exit(StatusState& s, Gauge& g, Channel ch,
                                            std::uint64_t now) noexcept {
    g.value = 0;
    g.last_gain_tick = static_cast<std::uint32_t>(now);
    g.resist_level = static_cast<std::uint8_t>(std::min(2, static_cast<int>(g.resist_level) + 1));
    g.resist_until = static_cast<std::uint32_t>(now + kResistWindow);
    s.stage = 1;
    s.stage_ticks = stage_duration(ch, 1);
    // s.primary is left as `ch` -- fading FROM stage 1, not cleared.
}

// Fold one gain into the state (§3/§5/§6): X1's opposed drain first, then the coating multiplier,
// then soft-resist, one floor-divided product. `mult_pm` is RFC-009's own caller-supplied
// material x scale-tier product (header note: 1000 = identity at every call site this pass).
inline void status_gain(StatusState& s, Gauge (&gauges)[5], const BuildupPacket& pkt,
                        std::uint16_t mult_pm, std::uint64_t now) noexcept {
    if (pkt.channel == Channel::kNone || pkt.amount == 0) return;
    const Channel ch = pkt.channel;

    // X1: Heat<->Cold are opposed and absolute — one never out-accumulates the other's gauge.
    if (ch == Channel::kHeat || ch == Channel::kCold) {
        const Channel other = (ch == Channel::kHeat) ? Channel::kCold : Channel::kHeat;
        Gauge& og = gauges[gauge_index_of(other)];
        const std::uint16_t other_now = value_at(og, other, now);
        if (other_now > 0 || s.primary == other) {
            const std::uint32_t drain =
                std::min<std::uint32_t>(other_now, static_cast<std::uint32_t>(pkt.amount) * 2);
            og.value = static_cast<std::uint16_t>(other_now - drain);
            og.last_gain_tick = static_cast<std::uint32_t>(now);
            // X1: a single Heat packet this big breaks an active Freeze early (the one sanctioned
            // break of the terminal lock).
            if (ch == Channel::kHeat && s.primary == Channel::kCold && s.stage == 3 &&
                pkt.amount >= kThawBreakPower) {
                resolve_terminal_exit(s, og, Channel::kCold, now);
            }
            return;  // no Heat/Cold accumulates until the other is empty and not primary
        }
    }

    Gauge& g = gauges[gauge_index_of(ch)];
    if (now >= g.resist_until) g.resist_level = 0;  // window expires in place (§6's read rule)
    const std::uint16_t resist_pm = kSoftResist[g.resist_level];
    const std::uint16_t coat_pm = coating_mult_pm(ch, s.coatings);
    const std::int64_t product =
        static_cast<std::int64_t>(pkt.amount) * mult_pm * resist_pm * coat_pm;
    const std::int64_t gain = product / 1'000'000'000LL;  // three chained per-mille factors
    const std::uint16_t cur = value_at(g, ch, now);
    g.value = static_cast<std::uint16_t>(std::min<std::int64_t>(1000, cur + gain));
    g.last_gain_tick = static_cast<std::uint32_t>(now);
}

// C1: re-applying a coating sets ticks to the max, never adds.
inline constexpr void status_coat(StatusState& s, const CoatingPacket& pkt) noexcept {
    const auto idx = static_cast<std::uint8_t>(pkt.coating);
    s.coatings = static_cast<std::uint8_t>(s.coatings | (1u << idx));
    if (idx < 2) s.coating_ticks[idx] = std::max(s.coating_ticks[idx], pkt.ticks);
}

struct StepResult {
    std::int16_t dot_damage = 0;       // owed this call; caller applies to hp
    bool primary_expired_rung = false;  // a stage transition happened (future FX hook)
    bool combust = false;               // Heat's terminal fired; caller applies min(15%hp,60) burst
};

// A newly-entered stage's duration, tier-scaled ONLY for terminals (stage 3) per RFC-002 §6's
// `kTierTerminalDur` (RFC-009 §4.6 owns the tier table itself) — non-terminal rungs never scale.
[[nodiscard]] inline constexpr std::uint8_t tiered_stage_duration(Channel ch, std::uint8_t stage,
                                                                  std::uint16_t tier_terminal_pm) noexcept {
    const std::uint16_t base = stage_duration(ch, stage);
    if (stage != 3) return static_cast<std::uint8_t>(base);
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(base) * tier_terminal_pm) / 1000);
}

// Advance one tick (or `dticks` under LOD — this pass only ever calls with 1, since no chunk sleeps
// yet; see header note). Coating decay, DoT, the one-rung walk-down (with the terminal-exit
// precedence rule), then the §4 promotion algorithm, evaluated fresh against the now-decayed gauges.
// `tier_terminal_pm` is RFC-009 §4.6's tier-terminal-duration product (`combat_math.hpp`'s
// `tier_terminal_dur_pm`); defaults to 1000 (no scaling) for callers with no known tier (tests, or
// a future non-Creature combatant).
[[nodiscard]] inline StepResult status_step(StatusState& s, Gauge (&gauges)[5], std::uint8_t dticks,
                                            std::uint64_t now,
                                            std::uint16_t tier_terminal_pm = 1000) noexcept {
    StepResult result{};

    for (int i = 0; i < 2; ++i) {
        if (s.coating_ticks[i] == 0) continue;
        if (s.coating_ticks[i] <= dticks) {
            s.coating_ticks[i] = 0;
            s.coatings = static_cast<std::uint8_t>(s.coatings & ~(1u << i));
        } else {
            s.coating_ticks[i] = static_cast<std::uint8_t>(s.coating_ticks[i] - dticks);
        }
    }

    // DoT: Heat stage 1 (1/5t) and stage 2 (3/5t), Shock stage 2 only (2/5t) — the same cadence P2
    // used (`tick_ % 5 == 0`), generalized to `dticks` ticks ending at `now`.
    if ((s.primary == Channel::kHeat && (s.stage == 1 || s.stage == 2)) ||
        (s.primary == Channel::kShock && s.stage == 2)) {
        const std::int16_t per_tick =
            (s.primary == Channel::kHeat) ? (s.stage == 2 ? 3 : 1) : 2;
        for (std::uint8_t i = 0; i < dticks; ++i) {
            if ((now - dticks + 1 + i) % 5 == 0) {
                result.dot_damage = static_cast<std::int16_t>(result.dot_damage + per_tick);
            }
        }
    }

    if (s.primary != Channel::kNone) {
        if (s.stage_ticks <= dticks) {
            Gauge& g = gauges[gauge_index_of(s.primary)];
            if (s.stage == 3) {
                resolve_terminal_exit(s, g, s.primary, now);
            } else {
                const auto new_stage = static_cast<std::uint8_t>(s.stage - 1);
                if (new_stage == 0) {
                    s.primary = Channel::kNone;
                    s.stage = 0;
                    s.stage_ticks = 0;
                    g.value = 0;
                } else {
                    s.stage = new_stage;
                    s.stage_ticks = stage_duration(s.primary, new_stage);
                    g.value = static_cast<std::uint16_t>(threshold_of(new_stage) - 1);
                }
                g.last_gain_tick = static_cast<std::uint32_t>(now);
            }
            result.primary_expired_rung = true;
        } else {
            s.stage_ticks = static_cast<std::uint8_t>(s.stage_ticks - dticks);
        }
    }

    // Promotion (§4, normative pseudocode): find the channel with the highest would-be stage, ties
    // to the most-recent gain then the lowest channel index (the loop order below IS that order).
    std::uint8_t best_stage = 0;
    Channel best_ch = Channel::kNone;
    std::uint64_t best_gain_tick = 0;
    for (int ch_i = 1; ch_i <= 5; ++ch_i) {
        const auto ch = static_cast<Channel>(ch_i);
        Gauge& g = gauges[gauge_index_of(ch)];
        if (now >= g.resist_until) g.resist_level = 0;
        const std::uint8_t stg = would_stage(value_at(g, ch, now));
        if (stg > best_stage || (stg == best_stage && stg > 0 && g.last_gain_tick > best_gain_tick)) {
            best_stage = stg;
            best_ch = ch;
            best_gain_tick = g.last_gain_tick;
        }
    }

    if (best_ch == Channel::kStagger && best_stage <= 2) {
        // §4 exemption: Unsteady/Staggered never claim the slot, and (header note) have no distinct
        // side effect to fire in this engine yet.
    } else if (s.primary == Channel::kNone && best_stage >= 1) {
        s.primary = best_ch;
        s.stage = best_stage;
        s.stage_ticks = tiered_stage_duration(best_ch, best_stage, tier_terminal_pm);
    } else if (best_ch == s.primary && best_stage > s.stage) {
        s.stage = best_stage;
        s.stage_ticks = tiered_stage_duration(s.primary, s.stage, tier_terminal_pm);
    } else if (best_ch != s.primary && best_stage > s.stage) {
        s.primary = best_ch;  // eviction: the stronger claim wins the slot
        s.stage = best_stage;
        s.stage_ticks = tiered_stage_duration(best_ch, best_stage, tier_terminal_pm);
    }
    // equal stages never swap — no branch above matches, and none is needed.

    if (s.primary == Channel::kHeat && s.stage == 3) {
        result.combust = true;
        resolve_terminal_exit(s, gauges[gauge_index_of(Channel::kHeat)], Channel::kHeat, now);
    }

    return result;
}

// A physical or elemental blow resolves (§7): consult the combo table against the target's
// *current* primary/stage/coating (never a banked gauge), consume state, return the combo.
// `by_shock_element` stands in for RFC-002's `Element by_element == kShock` test (header note: no
// `Element` dependency in this file).
[[nodiscard]] inline Combo status_detonate(StatusState& s, Gauge (&gauges)[5], bool heavy,
                                           bool by_projectile, bool by_shock_element,
                                           std::uint64_t now) noexcept {
    const bool wet = (s.coatings & (1u << static_cast<std::uint8_t>(Coating::kWet))) != 0;
    if (by_shock_element) {
        if (!wet) return Combo::kNone;
        s.coatings = static_cast<std::uint8_t>(s.coatings & ~(1u << static_cast<std::uint8_t>(Coating::kWet)));
        s.coating_ticks[static_cast<std::uint8_t>(Coating::kWet)] = 0;
        return Combo::kConduct;
    }

    if (s.primary == Channel::kCold && s.stage == 3 && heavy && !by_projectile) {
        resolve_terminal_exit(s, gauges[gauge_index_of(Channel::kCold)], Channel::kCold, now);
        return Combo::kShatter;
    }
    if (s.primary == Channel::kHeat && s.stage >= 2 && by_projectile) {
        Gauge& g = gauges[gauge_index_of(Channel::kHeat)];
        g.value = 0;
        g.last_gain_tick = static_cast<std::uint32_t>(now);
        s.primary = Channel::kNone;
        s.stage = 0;
        s.stage_ticks = 0;
        return Combo::kBlast;
    }
    if (s.primary == Channel::kEarth && s.stage >= 2 && heavy && !by_projectile) {
        Gauge& g = gauges[gauge_index_of(Channel::kEarth)];
        g.value = 0;
        g.last_gain_tick = static_cast<std::uint32_t>(now);
        s.primary = Channel::kNone;
        s.stage = 0;
        s.stage_ticks = 0;
        return Combo::kCrush;
    }
    if (s.primary == Channel::kShock && s.stage >= 1 && !by_projectile) {
        Gauge& g = gauges[gauge_index_of(Channel::kShock)];
        const auto new_stage = static_cast<std::uint8_t>(s.stage - 1);
        if (new_stage == 0) {
            s.primary = Channel::kNone;
            s.stage = 0;
            s.stage_ticks = 0;
            g.value = 0;
        } else {
            s.stage = new_stage;
            s.stage_ticks = stage_duration(Channel::kShock, new_stage);
            g.value = static_cast<std::uint16_t>(threshold_of(new_stage) - 1);
        }
        g.last_gain_tick = static_cast<std::uint32_t>(now);
        return Combo::kArc;
    }
    return Combo::kNone;
}

}  // namespace mmo
