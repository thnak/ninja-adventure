// RFC-017 §4 — a mechanical PASS/FAIL reader for RFC-007 §6.3's Gate A/B/generation-cap protocol.
//
// This header owns none of RFC-007's training loop, checkpoint format, or state machine — it reads
// one artifact RFC-007 §6.2 already specifies in full (the `<checkpoint>.meta.json` sidecar) and
// applies arithmetic RFC-007 §6.3 already specifies in full. No new balance number is introduced
// here (RFC-017 Non-goals): 0.55/0.80/200/60 are RFC-007's own, cited, not re-derived.
//
// DELIBERATE DIVERGENCE FROM THE RFC TEXT: no JSON library is vendored in this repo (confirmed by
// survey — CMakeLists.txt links none), and RFC-007 §6.2's sidecar shape is small, fixed, and known
// in full (six scalar fields, one of them nested one level under "eval"). A general JSON parser
// would be a second, unused-elsewhere dependency for a five-field reader; `parse_sidecar` below is
// a narrow field-scanner over that ONE known shape, not a general parser — it will not handle
// arbitrary JSON, and is not meant to. If RFC-007's sidecar shape grows a nested array or a variant
// field, this scanner is the thing to replace, not extend indefinitely.
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mmo {

// RFC-007 §6.3, verbatim thresholds — this file's only reason to exist is to apply them the same
// way every time instead of by a human reading JSON.
inline constexpr float kGateAWinrate = 0.55f;      // vs incumbent, progress gate
inline constexpr std::uint32_t kGateAEpisodes = 200;
inline constexpr float kGateBWinrate = 0.80f;      // vs persona suite, ceiling gate (GAME.md §10)
inline constexpr std::uint32_t kGenerationCap = 60;

struct CheckpointSidecar {
    std::string policy_id;
    std::uint32_t generation = 0;
    float vs_incumbent_winrate = 0.0f;
    float vs_persona_winrate = 0.0f;
    std::uint32_t episodes = 0;
};

// Finds `"key"` followed by `:`, then reads the token up to the next `,`/`}`/`\n`/`"` (for a quoted
// string) or up to the next non-numeric character (for a bare number). Returns the raw token text,
// trimmed of quotes and surrounding whitespace; empty if the key is not found. Deliberately
// tolerant of key order and of the nesting the "eval" object introduces — it does not track object
// depth, it just finds the next occurrence of the key text, which is safe ONLY because RFC-007's
// sidecar never repeats a field name across nesting levels (confirmed against the §6.2 shape).
[[nodiscard]] inline std::string_view find_json_field(std::string_view doc, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = doc.find(needle);
    if (pos == std::string_view::npos) return {};
    pos = doc.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return {};
    ++pos;
    while (pos < doc.size() && (doc[pos] == ' ' || doc[pos] == '\t')) ++pos;
    if (pos >= doc.size()) return {};
    if (doc[pos] == '"') {
        const std::size_t start = pos + 1;
        const std::size_t end = doc.find('"', start);
        if (end == std::string_view::npos) return {};
        return doc.substr(start, end - start);
    }
    std::size_t end = pos;
    while (end < doc.size() && (std::isdigit(static_cast<unsigned char>(doc[end])) || doc[end] == '.' ||
                                doc[end] == '-' || doc[end] == '+')) {
        ++end;
    }
    return doc.substr(pos, end - pos);
}

[[nodiscard]] inline std::optional<float> parse_float_field(std::string_view doc, std::string_view key) {
    const std::string_view tok = find_json_field(doc, key);
    if (tok.empty()) return std::nullopt;
    float v = 0.0f;
    const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v);
    if (res.ec != std::errc{}) return std::nullopt;
    return v;
}

[[nodiscard]] inline std::optional<std::uint32_t> parse_uint_field(std::string_view doc,
                                                                    std::string_view key) {
    const std::string_view tok = find_json_field(doc, key);
    if (tok.empty()) return std::nullopt;
    std::uint32_t v = 0;
    const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v);
    if (res.ec != std::errc{}) return std::nullopt;
    return v;
}

// Returns nullopt if any required field is missing or malformed — a `--gate-check` caller treats
// that as a hard failure (a checkpoint sidecar RFC-007 itself cannot produce is not this tool's to
// guess at).
[[nodiscard]] inline std::optional<CheckpointSidecar> parse_sidecar(std::string_view doc) {
    const std::string_view policy_id = find_json_field(doc, "policy_id");
    const auto generation = parse_uint_field(doc, "generation");
    const auto vs_incumbent = parse_float_field(doc, "vs_incumbent_winrate");
    const auto vs_persona = parse_float_field(doc, "vs_persona_winrate");
    const auto episodes = parse_uint_field(doc, "episodes");
    if (policy_id.empty() || !generation || !vs_incumbent || !vs_persona || !episodes) {
        return std::nullopt;
    }
    CheckpointSidecar s;
    s.policy_id = std::string(policy_id);
    s.generation = *generation;
    s.vs_incumbent_winrate = *vs_incumbent;
    s.vs_persona_winrate = *vs_persona;
    s.episodes = *episodes;
    return s;
}

struct GateVerdict {
    bool gate_a = false;
    bool gate_b = false;
    bool cap_ok = false;
    [[nodiscard]] bool publish() const noexcept { return gate_a && gate_b && cap_ok; }
};

[[nodiscard]] inline constexpr GateVerdict evaluate_gates(const CheckpointSidecar& s) noexcept {
    GateVerdict v;
    v.gate_a = s.vs_incumbent_winrate >= kGateAWinrate && s.episodes >= kGateAEpisodes;
    v.gate_b = s.vs_persona_winrate <= kGateBWinrate;
    v.cap_ok = s.generation <= kGenerationCap;
    return v;
}

}  // namespace mmo
