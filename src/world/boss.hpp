// The dojo BOSS's brain (F3), and the seam a learned policy (F4) drops into unchanged.
//
// This file is deliberately tiny and pure. It holds ONE function — `boss_policy(const BossObs&)` —
// and the two plain-data types on either side of it. That is the whole interface between "what the
// boss can see" and "what the boss does", and it is written this way ON PURPOSE:
//
//   * `boss_policy` is a PURE FUNCTION of an observation. It reads no world, sends no message, holds
//     no state. Everything it needs is in `BossObs`; everything it decides is a `BossAction`. The
//     chunk builds the obs each decision tick, calls this, and executes the action through the SAME
//     wind-up/strike machinery every creature uses (see step_bosses in chunk_actor.hpp).
//   * Because the seam is an obs->action function, F4's learned policy replaces the BODY of
//     `boss_policy` and nothing else. The observation it reads and the action space it returns are
//     already the tensor interface an RL agent wants: a fixed-width float/int vector in, a small
//     discrete action out. Generation 0 is the hand-written script below; generation 1 is a network
//     with the identical signature. No call site changes.
//
// INTEGER-ish obs on purpose. dx/dy are room-local tile offsets, hp is a fixed-point fraction, the
// cooldowns are tick counts — all the kinds of number that survive being handed to a policy on
// another machine without a float-determinism argument. Combat itself stays in floats (the chunk),
// exactly as the rest of the fight system does; the obs is the quantised view the brain sees.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace mmo {

// --- Boss tuning (the design numbers, in one place) ------------------------------------------
// Kept here beside the policy because they ARE the policy's world: the reach it attacks within, the
// distance past which it prefers to charge, the wind-ups that make its blows the most readable in
// the game. HP/damage/xp live in stats_of(kBoss) (tiles.hpp); the rest are boss-only and here.
inline constexpr std::int16_t kBossMaxHp = 700;
inline constexpr std::int16_t kBossDamage = 20;
inline constexpr std::uint8_t kBossAttackWindup = 10;   // ticks frozen before an attack lands
inline constexpr std::uint8_t kBossChargeWindup = 14;   // the biggest telegraph in the game
inline constexpr std::uint8_t kBossAttackCd = 15;       // ticks between blows (the post-strike hold)
inline constexpr std::uint8_t kBossChargeCd = 40;       // ticks before it may charge again
inline constexpr std::uint8_t kBossChargeDashTicks = 9; // ticks the committed dash runs
inline constexpr float kBossReach = 2.6f;               // tiles: how close a blow connects
inline constexpr int kBossChargeRange = 4;              // charge only when the target is beyond this
inline constexpr float kBossApproachSpeed = 2.5f;       // tiles/second closing on the target
inline constexpr float kBossChargeSpeed = 8.5f;         // tiles/second during the committed dash
inline constexpr std::uint16_t kBossRespawnTicks = 3000;  // 5 min at 10 Hz, same room
inline constexpr std::uint16_t kBossLeashTicks = 50;      // ticks with no target -> reset to spawn

// The pose the renderer draws, published on Creature::boss_pose. The walk-only creatures have no
// attack frame and telegraph with a red pulse alone; the boss additionally SWAPS to a real
// attack/charge pose, so the read is unmistakable. Left/Right are chosen by the boss's facing.
enum class BossPose : std::uint8_t {
    kIdle = 0,
    kWalk = 1,
    kAttack = 2,   // an attack wind-up or the blow itself; facing picks AttackLeft/AttackRight
    kCharge = 3,   // a charge wind-up or the dash; facing picks ChargeLeft/ChargeRight
};

// --- The RL interface -------------------------------------------------------------------------

// What the boss sees at a decision tick. Room-local, quantised, fixed width — the observation an
// agent is handed. The chunk fills this in from the boss body and its target's beacon.
struct BossObs {
    std::int16_t dx = 0;        // target minus boss, in tiles (room-local); sign picks left/right
    std::int16_t dy = 0;
    std::uint16_t hp_frac = 0;  // own HP as fixed-point 0..1000 (=1.0) — for F4; the script ignores it
    std::uint8_t attack_cd = 0; // ticks until it may attack again
    std::uint8_t charge_cd = 0; // ticks until it may charge again
    bool winding_up = false;    // already committed to a wind-up this tick
};

enum class BossActionKind : std::uint8_t {
    kHold = 0,        // stand and telegraph nothing — recover, or wait for the target to come in
    kApproach = 1,    // close the distance, clamped to the room
    kAttackLeft = 2,  // commit an attack wind-up, facing left
    kAttackRight = 3, // ... facing right
    kCharge = 4,      // commit a charge wind-up, then dash to where the target stood at commit
};

struct BossAction {
    BossActionKind kind = BossActionKind::kHold;
};

// GENERATION-0 SCRIPT. This is the seam F4's learned policy replaces: same `BossObs` in, same
// `BossAction` out, so swapping the body for a network is a drop-in and no chunk code moves.
//
// The hand rules, in priority order:
//   1. Already winding up -> hold. A committed telegraph is a promise; let it land (stun is the only
//      thing that cancels it, and the chunk handles that before ever calling here).
//   2. Target beyond `kBossChargeRange` tiles and the charge is off cooldown -> charge. The big
//      readable dash is the boss's answer to a fleeing player.
//   3. Within reach -> attack toward the target's side (left if it is to the boss's left), unless
//      still recovering from the last blow (attack_cd) -> hold briefly. This is the "hold after a
//      strike" the design asks for.
//   4. Otherwise -> approach.
[[nodiscard]] inline constexpr BossAction boss_policy(const BossObs& o) noexcept {
    if (o.winding_up) return {BossActionKind::kHold};
    const int dist = std::abs(static_cast<int>(o.dx)) > std::abs(static_cast<int>(o.dy))
                         ? std::abs(static_cast<int>(o.dx))
                         : std::abs(static_cast<int>(o.dy));  // Chebyshev, room-local
    if (o.charge_cd == 0 && dist > kBossChargeRange) return {BossActionKind::kCharge};
    if (static_cast<float>(dist) <= kBossReach) {
        if (o.attack_cd > 0) return {BossActionKind::kHold};  // recovering from the last blow
        return {o.dx < 0 ? BossActionKind::kAttackLeft : BossActionKind::kAttackRight};
    }
    return {BossActionKind::kApproach};
}

}  // namespace mmo
