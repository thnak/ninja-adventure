// RFC-006 — Visual FX & Telegraph Standards.
//
// This is the SIM-SIDE half of the RFC: the replicated `Telegraph` record (§2), the danger-tier
// qualification formula (§1.3), the fill/lifecycle math (§1.4), and the element palette (§1.2) — all
// pure data + pure functions, no `ChunkActor` dependency, mirroring `boss_kit.hpp`/`battlefield.hpp`'s
// own shape. `ChunkActor` (chunk_actor.hpp) creates/ticks/publishes real `Telegraph` records from the
// boss's own commits, so a promise made in this pass is a promise a client COULD draw from replicated
// state — but no pixel is actually drawn this pass; see the header note below for why.
//
// DELIBERATE DIVERGENCES FROM THE RFC TEXT, each real and each documented at its point of use:
//   - NO ACTUAL RENDERING is implemented this pass. `raylib_bridge.cpp` is left untouched. §1.4's
//     alpha ramps, §1.6's pulse/glyph overlays, §4's two-pass tint+glow recipes, §6's status wash
//     colors, and §7's battlefield-state filters are all genuine GUI/graphics engineering that needs
//     visual QA on real hardware to get right — this headless session verifies correctness through
//     `mmo_sim`, which has no window and cannot judge whether a cone reads at 16px. Building pixel
//     code with nobody able to look at it is the "half-finished implementation" this project's own
//     conventions forbid more than it is progress. What this pass builds instead is everything the
//     RFC calls DATA rather than DRAWING: the replicated record, the tier formula, the lifecycle math
//     — real, tested, and exactly what a renderer would consume the day someone implements the draw
//     calls against it. `ElementPalette`'s RGB triples are included as real, tested constants (§1.2)
//     even though nothing paints with them yet, matching this session's own precedent for a real,
//     owned table with no wired consumer (`boss_kit.hpp`'s tier multiplier table before a second kit
//     exists to use it).
//   - `kTiles`-shaped telegraphs (terrain about to become hazardous — RFC-004 hazard-activation
//     telegraphs) are declared in `TelegraphShape` but never produced: no RFC-004 content authors a
//     `kTiles` telegraph yet (spike-row/cracking-floor authoring is unbuilt RFC-005/008 content).
//     Only `kCone` (cleave) and `kLine` (charge dash) are ever produced, by the one real boss kit.
//   - FX-1..FX-6 (the effect-lifetime engine-gap requirements) are almost entirely already satisfied
//     by shipped code (`effect_life_of`, `kMaxEffectLife` — tiles.hpp) or are asset-pipeline/RFC-008
//     concerns (frame-count-parity codegen, the `fx.*` schema, unpacked sheets) outside a C++-engine
//     session's reach. `FxRecipe` (§4) is declared here as a real struct shape, matching the same
//     "real shape, no producer yet" posture as the element palette — RFC-008's `fx.*` documents are
//     the format that would populate one, and that format does not exist.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "world/tiles.hpp"

namespace mmo {

// --- §1.2: the element palette -----------------------------------------------------------------

struct PaletteColor {
    std::uint8_t r = 0, g = 0, b = 0;
};

// Indexed by Element (kNone=physical .. kShock=Thunder); one row per palette entry (§1.2's table).
[[nodiscard]] inline constexpr PaletteColor telegraph_hue_of(Element e) noexcept {
    switch (e) {
        case Element::kNone: return PaletteColor{255, 45, 30};    // Physical
        case Element::kFire: return PaletteColor{255, 120, 40};
        case Element::kIce: return PaletteColor{120, 210, 255};
        case Element::kEarth: return PaletteColor{210, 160, 70};  // Rock
        case Element::kShock: return PaletteColor{255, 230, 80};  // Thunder
        case Element::kCount: break;
    }
    return PaletteColor{255, 45, 30};
}

// --- §1.3: danger tiers ---------------------------------------------------------------------------

// `d = (base_damage * ring_damage_scale) / kPlayerMaxHp` — the expected-damage fraction. A flat,
// non-ring-scaled source (a scripted boss) passes `ring_damage_scale = 1.0f`.
[[nodiscard]] inline constexpr float expected_damage_fraction(float base_damage, float ring_damage_scale,
                                                               float player_max_hp) noexcept {
    if (player_max_hp <= 0.0f) return 0.0f;
    return (base_damage * ring_damage_scale) / player_max_hp;
}

// The qualification table, "highest tier wins" (structural). `cc` is hard-control ticks (RFC-002).
[[nodiscard]] inline constexpr std::uint8_t telegraph_tier_of(float d, std::uint16_t cc) noexcept {
    std::uint8_t tier = 0;
    if (d >= 0.10f || (cc > 0 && cc < 10)) tier = 1;
    if (d >= 0.25f || (cc >= 10 && cc < 20)) tier = 2;
    if (d >= 0.50f || cc >= 20) tier = 3;
    return tier;
}

// --- §2: the replicated telegraph record -----------------------------------------------------------

enum class TelegraphShape : std::uint8_t { kCircle, kRing, kLine, kCone, kTiles };

inline constexpr std::uint8_t kTelegraphFizzling = 1u << 0;
inline constexpr std::uint8_t kTelegraphTilesPayload = 1u << 1;

struct Telegraph {
    std::uint32_t id = 0;
    TelegraphShape shape = TelegraphShape::kCone;
    Element element = Element::kNone;
    std::uint8_t tier = 0;
    std::uint8_t total = 0;   // wind-up ticks at commit
    std::uint8_t left = 0;    // ticks remaining; while fizzling, ticks of the fizzle collapse instead
    float x = 0.0f, y = 0.0f;
    float ex = 0.0f, ey = 0.0f;
    float radius = 0.0f;
    float r2 = 0.0f;
    std::uint8_t arc_deg_half = 0;
    std::uint8_t flags = 0;
};

inline constexpr std::size_t kMaxTelegraphs = 8;  // per chunk (§2, §8 R4's screen-budget backstop)
inline constexpr std::uint8_t kFizzleTicks = 2;   // T4: interrupted wind-ups collapse over this long

// §1.4's `fill_frac`: a pure function of two already-replicated integers, so every client draws the
// identical countdown with no extra state or float drift.
[[nodiscard]] inline constexpr float telegraph_fill_frac(std::uint8_t total, std::uint8_t left) noexcept {
    if (total == 0) return 1.0f;
    return 1.0f - static_cast<float>(left) / static_cast<float>(total);
}

// §1.4's lifecycle state machine. IMPACT is not a distinct state here: it is the existing impact
// `Effect` hand-off the moment `left` reaches 0 (chunk_actor.hpp), and the `Telegraph` record itself
// is removed at that instant ("decal dies") — matching the table's own "hand-off to impact Effect".
enum class TelegraphState : std::uint8_t { kArm, kCharge, kImminent, kFizzle };

[[nodiscard]] inline constexpr TelegraphState telegraph_state_of(std::uint8_t total, std::uint8_t left,
                                                                  bool fizzling) noexcept {
    if (fizzling) return TelegraphState::kFizzle;
    if (left <= 3) return TelegraphState::kImminent;         // last 3 ticks
    if (total >= left && total - left < 2) return TelegraphState::kArm;  // first 2 ticks elapsed
    return TelegraphState::kCharge;
}

// §2's overflow rule: the newest commit's tier must be STRICTLY higher than the lowest-tier live
// record to evict it (a true interrupt — FIZZLE, no impact, cooldown refunded by the caller); ties
// among the lowest tier break on the OLDEST (smallest id). Returns the index to evict, or -1 if the
// incoming commit should simply be refused (a well-defined Hold no-op, never silently dropped).
template <std::size_t N>
[[nodiscard]] inline int telegraph_eviction_index(const std::array<Telegraph, N>& live, std::size_t count,
                                                   std::uint8_t incoming_tier) noexcept {
    if (count < N) return -1;  // room to just append — caller's job, not eviction
    std::size_t lowest = 0;
    for (std::size_t i = 1; i < count; ++i) {
        if (live[i].tier < live[lowest].tier ||
            (live[i].tier == live[lowest].tier && live[i].id < live[lowest].id)) {
            lowest = i;
        }
    }
    if (incoming_tier > live[lowest].tier) return static_cast<int>(lowest);
    return -1;
}

// --- §4: the FX recipe shape (declared; see header note — no `fx.*` document format exists yet) ----

enum class FxPlayMode : std::uint8_t { kOneShot, kLoop, kHold };  // FX-3

struct FxRecipe {
    EffectKind base = EffectKind::kSlash;
    std::uint8_t tint_r = 255, tint_g = 255, tint_b = 255;  // multiplicative pass; 255,255,255 = none
    std::uint8_t glow_alpha = 0;                             // additive silhouette pass, max 140
    Element glow_element = Element::kNone;
    std::uint8_t scale_eighths = 8;  // 8 = 1.0x
    FxPlayMode mode = FxPlayMode::kOneShot;
};

}  // namespace mmo
