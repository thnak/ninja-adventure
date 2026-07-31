// RFC-007 — RL Observation & Action Space, §2: the observation vector.
//
// Pure constants + tiny encoding helpers, no `ChunkActor` dependency — mirrors `boss_kit.hpp`'s own
// shape. The actual per-tick ASSEMBLY of a real `RlObs` needs chunk-local state (terrain, entities,
// nearby players) this header deliberately does not have, exactly the way `physics.hpp`'s formulas
// are pure but `strike()` (chunk_actor.hpp) does the assembling — `ChunkActor::build_boss_obs`
// (chunk_actor.hpp) is that assembly, and is the only place that fills an `RlObs` for real.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - No RL/DQN training infrastructure exists ANYWHERE in this repository (confirmed by an
//     exhaustive repo-wide grep for DqnAgent/ReplayBuffer/TrainingActor/CombatEnvironment/
//     NetworkCheckpoint — zero hits outside prose). RLDrive, the vendored core this RFC's §6 builds
//     on, lives at `/home/nvthanh/works/windows-machine-self-learn` on a different machine entirely
//     and was never imported here. §6 (training environment, checkpoints, generation state machine)
//     is therefore not implementable this pass by definition, not by choice — it is the strongest,
//     most literal case of "genuinely blocked on an unavailable external dependency" this session
//     has had. What this pass builds instead is the CONTRACT §6 would train against: the obs vector
//     layout (this header + `build_boss_obs`) and the action space (`rl_action.hpp`), assembled and
//     exercised for real by the existing generation-0 script (`boss_policy`, unchanged) — so the
//     seam is live and correct the day a real trainer is vendored in, per this RFC's own §3 promise
//     that "F4's learned policy replaces the BODY of `boss_policy` and nothing else."
//   - Block T's target pipeline-phase/progress (idx 38-42) and Block T2's winding-up flag (idx 47)
//     are left at 0 (never fabricated): a chunk (trust tier B) has no route to a player's own
//     `AbilityHead`/cooldowns (trust tier A, `PlayerActor`) — `PlayerBeacon` carries only position/
//     hp/tick (confirmed by survey), and no cross-actor Ask for it exists. Exposing it would mean
//     widening the beacon or adding a new Ask, real engineering this RFC's job is not to invent
//     unrequested. A future beacon field is the natural fix; until then these floats read "absent",
//     which is honest — the target genuinely IS unobserved on this axis, not secretly present.
//   - Block T2's target velocity (idx 26-27's counterpart is not even in T2's own layout, but Block
//     T's own idx 26-27) and the general "one remembered position per tracked target" statelessness
//     rule (§2.8) are NOT implemented this pass: no previous-decision-tick position is stored
//     anywhere on `BossState`. Velocity floats (Block T idx 26-27) are left at 0. This also means
//     the action space's **Lead** aim mode (§3, ids 11-14) has no velocity to extrapolate with —
//     `execute_rl_action` (chunk_actor.hpp) commits Lead to the same point Direct does, a named,
//     total-function-safe scope reduction, not a crash or a silent wrong answer.
//   - There is no self "own pipeline phase" enum anywhere on `Creature`/`BossState` (confirmed by
//     survey: only `Creature.windup`/`attack_cd` and boss-only `BossState.charging`/
//     `winding_charge`/`charge_cd` exist — no unified Idle/Windup/Active/Recover field). Block S's
//     phase one-hot (idx 10-13) is therefore SYNTHESIZED from those disjoint fields at the call
//     site, not read off a stored enum — real, correct, but derived rather than looked up.
//   - Block R's 8-ray terrain/hazard scan is genuinely new code: no Bresenham/DDA/line-of-sight
//     helper exists anywhere in this engine (confirmed by survey). The walk here steps whole tiles
//     along each compass direction (not sub-tile-accurate DDA) — sufficient for a coarse ray
//     encoding at `kObsRange = 8` tiles, and consistent with every other hit-test in this engine
//     being a plain circle/tile check rather than continuous geometry.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace mmo {

inline constexpr int kObsVersion = 1;
inline constexpr std::size_t kObsSize = 120;
inline constexpr float kObsRange = 8.0f;  // egocentric clamp, tiles

// Block start indices (§2's layout, verbatim).
inline constexpr std::size_t kObsBlockS = 0;     // Self, 0-22
inline constexpr std::size_t kObsBlockT = 23;    // Primary target, 23-42
inline constexpr std::size_t kObsBlockT2 = 43;   // Secondary target, 43-48
inline constexpr std::size_t kObsBlockR = 49;    // Terrain & hazard rays, 49-64
inline constexpr std::size_t kObsBlockG = 65;    // Ground & confinement, 65-70
inline constexpr std::size_t kObsBlockE = 71;    // Combat-entity slots, 71-106 (3 x 12)
inline constexpr std::size_t kObsBlockReserved = 107;  // 107-119, always 0 in v1

struct RlObs {
    std::array<float, kObsSize> v{};
};

// §2's closeness encoding: absent/far = 0, so an all-zero vector reads as "nothing there".
[[nodiscard]] inline constexpr float obs_closeness(float horizon, float d) noexcept {
    const float c = (horizon - d) / horizon;
    return c > 0.0f ? c : 0.0f;
}

// §2's egocentric offset encoding: clamp to +/-kObsRange, divide by the range.
[[nodiscard]] inline constexpr float obs_offset(float delta, float range) noexcept {
    const float clamped = delta < -range ? -range : (delta > range ? range : delta);
    return clamped / range;
}

[[nodiscard]] inline constexpr float obs_frac(float value, float max) noexcept {
    if (max <= 0.0f) return 0.0f;
    const float f = value / max;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

}  // namespace mmo
