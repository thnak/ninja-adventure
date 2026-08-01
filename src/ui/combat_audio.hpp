// RFC-012 — Combat Audio & Sound Cue Standards.
//
// The pure, stateless half of this RFC: the §2.2 wind-up-length band, the §3 impact-cue table, and
// the §4.4/§4.5 priority/positional-audio math — plain data + pure functions, no raylib dependency,
// mirroring `telegraph.hpp`'s own split between "real, tested logic" and the raylib-bound glue that
// actually calls `Audio::play_world_cue` (client_main.cpp). The per-tick, per-creature/per-telegraph
// dedup bookkeeping (§2.1: "one dedup entry per id, so a re-published unchanged view never re-fires
// a cue") is inherently frame-loop state, not a pure function, and stays in client_main.cpp,
// mirroring the existing `effect_tick` map's own precedent.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ui/audio.hpp"
#include "world/tiles.hpp"

namespace mmo::ui {

// --- §4.4 priority table (highest first) ---------------------------------------------------------
enum class CuePriority : std::uint8_t { kP0 = 0, kP1 = 1, kP2 = 2, kP3 = 3 };

// A single world-sourced cue candidate for this tick, before culling/ranking/budgeting (§4.3).
struct WorldCueEvent {
    Sfx sfx = Sfx::kHit;
    CuePriority priority = CuePriority::kP2;
    float x = 0.0f, y = 0.0f;  // world position, for §4.5's pan/gain
    float pitch = 1.0f;
    float band_gain = 1.0f;  // pre-distance gain (§2.2's band, or 1.0 for dedicated/impact cues)
};

inline constexpr float kAudioMaxRadius = 14.0f;      // tiles (tunable, §4.3 step 1)
inline constexpr std::size_t kAudioCueBudget = 8;    // per tick, world-sourced (tunable, §4.3 step 3)
inline constexpr float kSelfThreatRadius = 1.0f;     // tiles (tunable, §4.4 P0)
inline constexpr float kPanHalfWidthTiles = 10.0f;   // tunable, §4.5

// --- §4.5 positional audio -------------------------------------------------------------------------
[[nodiscard]] inline float world_cue_falloff(float dist_tiles) noexcept {
    return std::clamp(1.0f - dist_tiles / kAudioMaxRadius, 0.0f, 1.0f);
}
[[nodiscard]] inline float world_cue_pan(float source_x, float player_x) noexcept {
    return std::clamp(0.5f + (source_x - player_x) / kPanHalfWidthTiles / 2.0f, 0.0f, 1.0f);
}
[[nodiscard]] inline bool world_cue_in_range(float dist_tiles) noexcept {
    return dist_tiles <= kAudioMaxRadius;
}

// --- §2.2 the interim wind-up-length band, ordinary creatures only (bosses get dedicated files,
// §2.3 — never banded). Retires the day RFC-006 §2's `Telegraph.tier` is replicated for every
// creature, not just the boss (Open Question 1); until then this is the one thing that already
// varies by threat size in shipped data.
enum class WindupBand : std::uint8_t { kLight, kHeavy };

[[nodiscard]] inline constexpr WindupBand windup_band_of(std::uint8_t windup_total) noexcept {
    return windup_total >= 6 ? WindupBand::kHeavy : WindupBand::kLight;
}
[[nodiscard]] inline constexpr float band_pitch(WindupBand b) noexcept {
    return b == WindupBand::kLight ? 1.12f : 0.92f;
}
[[nodiscard]] inline constexpr float band_gain(WindupBand b) noexcept {
    return b == WindupBand::kLight ? 0.9f : 1.1f;
}

// --- §3 impact cue identity — exhaustive over EffectKind (structural: a new EffectKind value must
// add a row here, same as RFC-002's own Interactions table rule). Returns false for kSmoke (a
// utility puff, not a hit — §3's fix for "throwing smoke sounds like a hit landing") and for any
// value with no cue, meaning the caller adds no candidate this tick.
[[nodiscard]] inline constexpr bool impact_cue_of(EffectKind k, Sfx& out) noexcept {
    switch (k) {
        case EffectKind::kSlash: out = Sfx::kHit; return true;
        case EffectKind::kFire: out = Sfx::kImpactFire; return true;
        case EffectKind::kIce: out = Sfx::kImpactIce; return true;
        case EffectKind::kEarth: out = Sfx::kImpactRock; return true;
        case EffectKind::kShock: out = Sfx::kImpactThunder; return true;
        case EffectKind::kBlast: out = Sfx::kCombo; return true;
        case EffectKind::kSlashHeavy: out = Sfx::kHitHeavy; return true;
        case EffectKind::kSlashCombo: out = Sfx::kHitHeavy; return true;
        case EffectKind::kSmoke: return false;
        case EffectKind::kCount: break;
    }
    return false;
}

// §4.3 steps 2-3: rank surviving (already distance-culled) candidates by priority then ascending
// distance, and keep at most `budget` — the rest are dropped, never queued (a stale combat cue a
// tick late is worse than silence, matching RFC-006 §2's own telegraph-cap reasoning).
inline void select_world_cues(std::vector<WorldCueEvent>& events, float player_x, float player_y,
                               std::size_t budget) {
    std::stable_sort(events.begin(), events.end(), [&](const WorldCueEvent& a, const WorldCueEvent& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        const float dxa = a.x - player_x, dya = a.y - player_y;
        const float dxb = b.x - player_x, dyb = b.y - player_y;
        return (dxa * dxa + dya * dya) < (dxb * dxb + dyb * dyb);
    });
    if (events.size() > budget) events.resize(budget);
}

}  // namespace mmo::ui
