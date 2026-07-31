// RFC-010 — Battlefield Simulation.
//
// The ephemeral layer combat writes onto the world and the world ticks back toward baseline:
// per-tile Surface patches (burning grass, ice glaze, mud) and area-wide FieldStates (earthquake).
// Pure data + pure functions, no `ChunkActor` dependency — mirrors `physics.hpp`/`status.hpp`'s own
// shape. Lasting ground *scars* (cracked/rubble/crater/scorched) stay RFC-004 §8's; this header only
// stamps into that layer at a patch's expiry, never redefines it.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - `flammable_of` is a genuinely new concept (RFC-003/009 have no flammability-by-material table
//     at all yet) — scoped to the two terrains the RFC's own guide-level text names ("grass/forest
//     floor"): kGrass and kTree.
//   - kMudded's only authored v1 trigger — "Rock churn; wet zone expiring on dirt" — is the RFC's own
//     text naming the RainCall-class weather source as "P7 — future trigger, not v1". The full
//     kMudded state machine (decay, eviction, the mud coefficients) is built and tested here for
//     real; nothing in this pass creates one. Superseded, not reinterpreted, when a weather/rock-churn
//     trigger lands.
//   - The Rock-impact row ("no patch — stamps/escalates an RFC-004 scar") and the "no fireproof-claim
//     lookup exists" gap are chunk_actor.hpp wiring concerns, not this header's — see its own note.
//   - The drift sine (§5 D1) is computed with `std::sin` at call time, not baked into a build-time
//     fixed-point lookup table. The RFC asks for the LUT specifically to guarantee GCC/MSVC bit-exact
//     replay; this engine already keeps knockback distance and every other physics quantity in
//     float (physics.hpp's own documented divergence), so this follows the same precedent rather than
//     being the first fixed-point quantity in the simulation. True cross-toolchain D1 conformance for
//     drift specifically is deferred alongside that.
//   - FieldState's creation authority (RFC-005 boss abilities, a `StrongholdActor` raid event) is not
//     built — neither dependency exists in this codebase yet. The record shape, decay, stacking rule,
//     drift, and the deterministic accuracy multiplier are real and unit-tested; `kMaxFieldTicks`/
//     `kFieldPulsePeriod`/cross-chunk `FieldPulse` leasing are declared but have no caller, mirroring
//     physics.hpp's own `scale_impulse_out_pm` precedent (a real law with no wired producer yet).
//   - The 1 Hz background / slept simulation tiers (§4.7) do not exist in this engine at all yet (only
//     publish-rate LOD is implemented) — this header's decay math is written LOD-agnostic (absolute
//     `end_ms` deadlines, never a countdown), so it is already correct under today's degenerate
//     "every chunk is active" case and needs no rework when tiering lands.
#pragma once

#include <cmath>
#include <cstdint>

#include "world/tiles.hpp"

namespace mmo {

// --- Layer 1: tile patches ------------------------------------------------------------------------

enum class Surface : std::uint8_t {
    kBurning = 0,  // fire on flammable ground; spreads (bounded), applies Heat build-up to occupants
    kMudded = 1,   // rock-churned/rain-soaked ground; knockback/force-transfer coefficients
    kIced = 2,     // ice-glazed ground; knockback/direct-damage coefficients
    kCount = 3,
};

struct TilePatch {          // 16 bytes
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    Surface s = Surface::kBurning;
    std::uint8_t intensity = 0;  // 0..3, reserved for RFC-009 build-up coupling — unused this pass
    std::int64_t end_ms = 0;     // absolute deadline; then next_of(s) or baseline
};

inline constexpr std::size_t kMaxPatches = 48;  // per chunk

inline constexpr int kBurningDurTicks = 40;   // 4 s
inline constexpr int kMuddedDurTicks = 300;   // 30 s
inline constexpr int kIcedDurTicks = 300;     // 30 s

[[nodiscard]] inline constexpr std::int64_t dur_ms_of(Surface s) noexcept {
    switch (s) {
        case Surface::kBurning: return static_cast<std::int64_t>(kBurningDurTicks) * kTickMs;
        case Surface::kMudded: return static_cast<std::int64_t>(kMuddedDurTicks) * kTickMs;
        case Surface::kIced: return static_cast<std::int64_t>(kIcedDurTicks) * kTickMs;
        case Surface::kCount: break;
    }
    return 0;
}

// v1: every surface decays straight to baseline (§4.2's table — kBurning additionally stamps a
// kScorched scar at expiry, a chunk_actor.hpp concern since scars are owned there).
[[nodiscard]] inline constexpr bool decays_to_baseline(Surface) noexcept { return true; }

// "flammable tile (grass/forest floor per RFC-003 material of tile)" — see header note: no
// flammability-by-material table exists yet, so this is scoped directly to the two named terrains.
[[nodiscard]] inline constexpr bool flammable_of(Terrain t) noexcept {
    return t == Terrain::kGrass || t == Terrain::kTree;
}

// --- §4.2's trigger table: what a Fire/Ice impact does to the tile it lands on ---------------------

enum class SurfaceEvent : std::uint8_t {
    kSet,        // baseline (or a converted tile) becomes the new surface
    kRemoved,    // steamed/extinguished away — no patch remains
    kRefreshed,  // same surface, deadline extended
};

struct SurfaceOutcome {
    SurfaceEvent event = SurfaceEvent::kSet;
    Surface result = Surface::kBurning;  // meaningful only when event != kRemoved
};

// Fire impact >= ignition threshold, flammable tile. On baseline: kBurning. On an existing patch:
// kIced/kMudded are steamed away (no patch); kBurning is refreshed.
[[nodiscard]] inline constexpr SurfaceOutcome fire_impact(bool has_existing, Surface existing) noexcept {
    if (!has_existing) return {SurfaceEvent::kSet, Surface::kBurning};
    if (existing == Surface::kBurning) return {SurfaceEvent::kRefreshed, Surface::kBurning};
    return {SurfaceEvent::kRemoved, Surface::kBurning};
}

// Ice impact >= threshold. On baseline: kIced. On an existing patch: kBurning is extinguished (no
// patch); kMudded freezes into kIced; kIced is refreshed.
[[nodiscard]] inline constexpr SurfaceOutcome ice_impact(bool has_existing, Surface existing) noexcept {
    if (!has_existing) return {SurfaceEvent::kSet, Surface::kIced};
    if (existing == Surface::kBurning) return {SurfaceEvent::kRemoved, Surface::kIced};
    return {SurfaceEvent::kSet, Surface::kIced};  // kMudded -> kIced, kIced -> refreshed (same result)
}

// --- Fire spread (§4.2) -----------------------------------------------------------------------------

inline constexpr int kSpreadPeriod = 5;          // ticks between spread rolls
inline constexpr std::uint16_t kSpreadChancePm = 250;  // 25% per candidate neighbour, per mille
inline constexpr std::size_t kMaxBurning = 12;   // hard cap on live kBurning patches per chunk

// §4.2's eviction rule: when full, the new patch replaces the record with the smallest `end_ms`;
// ties (and the "which one is oldest" comparison generally) break on `(end_ms, tx, ty)` lexicographic
// order so two conforming implementations converge regardless of physical array layout.
[[nodiscard]] inline constexpr bool patch_expires_before(const TilePatch& a, const TilePatch& b) noexcept {
    if (a.end_ms != b.end_ms) return a.end_ms < b.end_ms;
    if (a.tx != b.tx) return a.tx < b.tx;
    return a.ty < b.ty;
}

// --- §4.2's coefficient rows: knockback/damage while a creature stands on a live patch --------------

struct SurfaceCoeff {
    std::uint16_t knockback_pm = 1000;       // multiplies RFC-003's knockback_tiles result
    std::uint16_t force_transfer_pm = 1000;  // multiplies RFC-003's force_transfer_crush bonus (mud)
    std::uint16_t direct_damage_pm = 1000;   // multiplies the struck damage before resolve_damage (ice)
};

[[nodiscard]] inline constexpr SurfaceCoeff surface_coeff(bool has_surface, Surface s) noexcept {
    if (!has_surface) return SurfaceCoeff{};
    switch (s) {
        case Surface::kMudded: return SurfaceCoeff{500, 1250, 1000};
        case Surface::kIced: return SurfaceCoeff{1500, 1000, 800};
        case Surface::kBurning:
        case Surface::kCount: break;
    }
    return SurfaceCoeff{};
}

// --- Layer 2: field states (earthquake) -------------------------------------------------------------

enum class FieldKind : std::uint8_t { kEarthquake = 0, kCount = 1 };

struct FieldState {           // 24 bytes
    FieldKind kind = FieldKind::kEarthquake;
    std::uint8_t intensity = 1;  // 1..3
    std::uint16_t cx = 0, cy = 0;  // epicenter, map-global tiles
    std::uint16_t radius = 0;      // tiles; hard cap kMaxFieldRadius
    std::int64_t end_ms = 0;
    std::uint32_t source = 0;      // creature/boss id for attribution & dedup
};

inline constexpr std::size_t kMaxFields = 2;               // per chunk
inline constexpr std::uint16_t kMaxFieldRadius = 32;        // = chunk width (§4.3's load-bearing cap)
inline constexpr int kQuakeTicks = 80;                       // 8 s — the Giant Samurai's authored duration
inline constexpr int kMaxFieldTicks = 80;                    // 8 s — hard cap on every creation path
inline constexpr int kFieldPulsePeriod = 5;                  // ticks between cross-chunk FieldPulse leases

// §4.3's effect table. Camera shake/telegraph tremble are render-only (RFC-006) and have no engine
// representation here.
[[nodiscard]] inline constexpr float field_drift_amplitude(std::uint8_t intensity) noexcept {
    switch (intensity) {
        case 1: return 0.02f;
        case 2: return 0.04f;
        case 3: return 0.06f;
        default: break;
    }
    return 0.0f;
}

// The deterministic accuracy contribution (§4.3: "no miss roll, no RNG"), folded into a struck hit's
// damage at the call site — this engine has no separate `M_outer` battlefield-multiplier slot to plug
// into (combat_math.hpp's `resolve_damage` takes none), so callers multiply it in directly.
[[nodiscard]] inline constexpr std::uint16_t field_accuracy_pm(std::uint8_t intensity) noexcept {
    switch (intensity) {
        case 1: return 900;
        case 2: return 800;
        case 3: return 700;
        default: break;
    }
    return 1000;
}

inline constexpr int kDriftPeriodTicks = 20;  // P, tunable

[[nodiscard]] inline constexpr std::uint32_t drift_phase(std::uint32_t shot_id,
                                                          std::uint32_t launch_tick) noexcept {
    return (shot_id * 7u + launch_tick) % static_cast<std::uint32_t>(kDriftPeriodTicks);
}

// Perpendicular drift velocity offset for a shot inside an active field, pure in
// (shot id, launch tick, tick, intensity) — see header note on the float-vs-fixed-point divergence.
[[nodiscard]] inline float drift_perp(std::uint32_t tick, std::uint32_t shot_id, std::uint32_t launch_tick,
                                      std::uint8_t intensity) noexcept {
    const float a = field_drift_amplitude(intensity);
    if (a <= 0.0f) return 0.0f;
    const std::uint32_t phase = drift_phase(shot_id, launch_tick);
    const auto t = static_cast<float>((tick + phase) % static_cast<std::uint32_t>(kDriftPeriodTicks));
    constexpr float kTwoPi = 6.283185307f;
    return a * std::sin(kTwoPi * t / static_cast<float>(kDriftPeriodTicks));
}

// §4.3 stacking: an actor inside several fields takes the single highest intensity.
[[nodiscard]] inline constexpr std::uint8_t highest_intensity(std::uint8_t a, std::uint8_t b) noexcept {
    return a > b ? a : b;
}

}  // namespace mmo
