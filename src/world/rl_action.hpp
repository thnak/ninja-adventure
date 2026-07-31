// RFC-007 §3 — the action space. `kActionCount = 15`, exactly, so a future vendored DQN core (see
// `rl_obs.hpp`'s header note — none is vendored into this repo yet) sized against the same bound
// RLDrive's sampler hardcodes stays crash-safe by construction rather than by convention.
#pragma once

#include <cstdint>

#include "world/boss.hpp"

namespace mmo {

inline constexpr int kActionVersion = 1;
inline constexpr int kActionCount = 15;

enum class RlActionId : std::uint8_t {
    kHold = 0,
    kStepN = 1,
    kStepE = 2,
    kStepS = 3,
    kStepW = 4,
    kApproach = 5,
    kRetreat = 6,
    kCastSlot0Direct = 7,
    kCastSlot1Direct = 8,
    kCastSlot2Direct = 9,
    kCastSlot3Direct = 10,
    kCastSlot0Lead = 11,
    kCastSlot1Lead = 12,
    kCastSlot2Lead = 13,
    kCastSlot3Lead = 14,
    kCount = 15,
};

// The upstream bound this mirrors is not vendored into this repo (see rl_obs.hpp's header note);
// this assertion is therefore self-referential — it guards against a FUTURE edit to this enum
// silently drifting from the documented 15, the same failure mode §3 warns the real upstream
// constant is vulnerable to.
static_assert(static_cast<int>(RlActionId::kCount) == kActionCount,
             "RFC-007 SS3: the action space is exactly 15, matching RLDrive's DqnAgent sampler bound");

// §3's "Generation-0 compatibility" table, verbatim: the existing hand script's 5-action output
// maps onto the new 15-id space with facing DERIVED at the dispatch site (chunk_actor.hpp), not
// chosen by the id — kAttackLeft and kAttackRight both resolve to the same Cast-slot-0-Direct id.
[[nodiscard]] inline constexpr RlActionId to_rl_action(BossActionKind k) noexcept {
    switch (k) {
        case BossActionKind::kHold: return RlActionId::kHold;
        case BossActionKind::kApproach: return RlActionId::kApproach;
        case BossActionKind::kAttackLeft:
        case BossActionKind::kAttackRight: return RlActionId::kCastSlot0Direct;
        case BossActionKind::kCharge: return RlActionId::kCastSlot1Direct;
    }
    return RlActionId::kHold;
}

}  // namespace mmo
