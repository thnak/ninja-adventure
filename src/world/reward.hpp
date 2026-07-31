// RFC-007 §5 — reward shaping. Pure, tested formulas; nothing in this repository calls them yet
// (no training loop exists — see `rl_obs.hpp`'s header note on RLDrive being an unvendored external
// dependency). Built anyway, matching this session's own precedent for a real, owned law with no
// wired producer yet (`physics.hpp`'s `scale_impulse_out_pm`, `battlefield.hpp`'s `kFieldPulsePeriod`
// leasing): the shape of the reward function is this RFC's normative content regardless of whether a
// trainer exists to sum it, and getting the formula right now means the trainer, when vendored, is a
// pure consumer rather than a second place these numbers could drift.
//
// R-honesty (the "no gradient ever favors an untelegraphed hit" guarantee) is a CALLER contract, not
// something this header can enforce: this engine has no per-hit "was this pipeline-delivered or
// contact damage" tag on a damage event, so `reward_damage_dealt` takes that as an explicit bool
// rather than inferring it — a future trainer must pass `false` for RFC-005's `contact_damage`,
// exactly as R-honesty requires.
#pragma once

namespace mmo {

inline constexpr float kRewardDamageDealtScale = 1.0f;    // (tunable)
inline constexpr float kRewardDamageTakenScale = -1.0f;   // (tunable)
inline constexpr float kRewardTerminalWin = 0.25f;        // (tunable)
inline constexpr float kRewardTerminalLoss = -0.25f;      // (tunable)
inline constexpr float kRewardStepCost = -0.001f;         // (tunable) — a training-room device only
inline constexpr float kRewardInvalidCast = -0.05f;       // (tunable)
inline constexpr float kRewardTurtlePerDecision = -0.02f;  // (tunable)
inline constexpr int kTurtleHoldThreshold = 6;             // consecutive Holds before the penalty starts
inline constexpr float kRewardClip = 1.0f;                 // per-decision clip, [-1, +1]

// R-honesty: `pipeline_delivered=false` (RFC-005's contact_damage) earns exactly zero — the whole
// point of the exclusion is that no gradient ever sees an untelegraphed hit pay.
[[nodiscard]] inline constexpr float reward_damage_dealt(float damage, float target_max_hp,
                                                          bool pipeline_delivered) noexcept {
    if (!pipeline_delivered || target_max_hp <= 0.0f) return 0.0f;
    return kRewardDamageDealtScale * (damage / target_max_hp);
}

[[nodiscard]] inline constexpr float reward_damage_taken(float damage, float own_max_hp) noexcept {
    if (own_max_hp <= 0.0f) return 0.0f;
    return kRewardDamageTakenScale * (damage / own_max_hp);
}

[[nodiscard]] inline constexpr float reward_terminal(bool won, bool lost) noexcept {
    if (won) return kRewardTerminalWin;
    if (lost) return kRewardTerminalLoss;
    return 0.0f;
}

// The guard condition protects legitimate post-strike recovery (`attack_cd` holding) from being
// punished: turtling only accrues while a target is present AND at least one slot is off cooldown —
// an agent correctly waiting out its own recovery is not turtling.
[[nodiscard]] inline constexpr float reward_turtling(int consecutive_holds, bool target_present,
                                                      bool any_slot_ready) noexcept {
    if (!target_present || !any_slot_ready) return 0.0f;
    if (consecutive_holds <= kTurtleHoldThreshold) return 0.0f;
    return kRewardTurtlePerDecision;
}

[[nodiscard]] inline constexpr float reward_invalid_cast(bool coerced_to_hold) noexcept {
    return coerced_to_hold ? kRewardInvalidCast : 0.0f;
}

[[nodiscard]] inline constexpr float clip_reward(float total) noexcept {
    if (total > kRewardClip) return kRewardClip;
    if (total < -kRewardClip) return -kRewardClip;
    return total;
}

}  // namespace mmo
