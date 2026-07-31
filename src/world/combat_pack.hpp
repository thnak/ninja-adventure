// RFC-008 — the C++ side of the serialization contract: a real loader for the pack
// `tools/build_combat_pack.py` produces, proving "one file, once, into an immutable
// std::shared_ptr<const CombatPack>... lookups are array-indexed by the u16 from ids.lock.json"
// (RFC-008 "For an engineer") against the genuine generated artifact, not a mock.
//
// SCOPE: RFC-008's own Non-goals are explicit — "Runtime semantics... This RFC is the disk
// contract only." Nothing here rewires `PlayerActor`/`ChunkActor`/`boss.hpp` to execute pack data;
// the live sim keeps running on `abilities.hpp`/`tiles.hpp`'s existing constexpr tables (this
// RFC's own Summary calls those tables "the predecessors of this contract" — migrating the engine
// onto the pack is RFC-001/002/004/005/009's own future work against a pack that, as of this file,
// genuinely exists and genuinely loads). What this file DOES do for real: parse the canonical pack,
// verify its hash (catching a stale/corrupted artifact the way "the loader asserts it at startup"
// describes), and expose typed, array-indexed lookups for the two richest domains — `skill` and
// `entity` — matching RFC-008's own two worked examples (Meteor, Spike). `fx`/`icon`/`snd`/`status`
// get a lighter, generic `find(domain, id)` accessor rather than dedicated typed structs; nothing
// consumes those four domains' fields at runtime yet, so fully typing them now would be exactly
// the "half-finished implementation" this project's own conventions warn against.
//
// JSON PARSER: hand-rolled, not a vendored library, because it only has to read OUR OWN canonical
// output — a deliberately restricted grammar (§1: strict JSON, no floats, no null, sorted keys,
// no whitespace). A general-purpose parser would spend most of its code on grammar this project's
// own pack can never contain. `assets/_gen/combat_pack.json`'s raw bytes are already the canonical
// form (the packer writes exactly what it hashes) — hashing here re-hashes those same bytes
// directly, with no re-serialization step to risk diverging from Python's canonicalization.
//
// HASH ALGORITHM: BLAKE2b-256, matching the packer's own resolution of RFC-008 Open Questions §4 —
// this project already vendors Monocypher (`third_party/monocypher`, linked into every target via
// `mmo_crypto`) for `account.hpp`'s Argon2/BLAKE2b use, so `crypto_blake2b` costs nothing new here.
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "monocypher.h"

namespace mmo {

// --- a minimal JSON value for OUR OWN restricted canonical grammar ------------------------------

class JsonValue {
public:
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::string, Array, Object>;

    JsonValue() = default;
    JsonValue(Storage v) : v_(std::move(v)) {}

    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(v_); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(v_); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(v_); }
    [[nodiscard]] bool is_int() const noexcept { return std::holds_alternative<std::int64_t>(v_); }
    [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(v_); }

    [[nodiscard]] const Object& as_object() const { return std::get<Object>(v_); }
    [[nodiscard]] const Array& as_array() const { return std::get<Array>(v_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(v_); }
    [[nodiscard]] std::int64_t as_int() const { return std::get<std::int64_t>(v_); }
    [[nodiscard]] bool as_bool() const { return std::get<bool>(v_); }

    // Returns nullptr rather than throwing — every call site here is "look up an optional/known
    // field," never a place a missing key should abort the whole load.
    [[nodiscard]] const JsonValue* find(const std::string& key) const noexcept {
        if (!is_object()) return nullptr;
        const Object& o = as_object();
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::string get_string(const std::string& key, std::string fallback = "") const {
        const JsonValue* v = find(key);
        return (v != nullptr && v->is_string()) ? v->as_string() : fallback;
    }

    [[nodiscard]] std::int64_t get_int(const std::string& key, std::int64_t fallback = 0) const {
        const JsonValue* v = find(key);
        return (v != nullptr && v->is_int()) ? v->as_int() : fallback;
    }

private:
    Storage v_;
};

namespace detail {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        if (pos_ != s_.size()) throw std::runtime_error("trailing content after top-level value");
        return v;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;

    [[nodiscard]] char peek() const {
        if (pos_ >= s_.size()) throw std::runtime_error("unexpected end of JSON");
        return s_[pos_];
    }

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
                                     s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::string("expected '") + c + "'");
        ++pos_;
    }

    JsonValue parse_value() {
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return JsonValue(JsonValue::Storage(parse_string()));
            case 't':
                expect_literal("true");
                return JsonValue(JsonValue::Storage(true));
            case 'f':
                expect_literal("false");
                return JsonValue(JsonValue::Storage(false));
            default: return parse_number();
        }
    }

    void expect_literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) throw std::runtime_error("invalid literal");
        pos_ += lit.size();
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue::Object obj;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return JsonValue(JsonValue::Storage(std::move(obj)));
        }
        for (;;) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            break;
        }
        return JsonValue(JsonValue::Storage(std::move(obj)));
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue::Array arr;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return JsonValue(JsonValue::Storage(std::move(arr)));
        }
        for (;;) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            break;
        }
        return JsonValue(JsonValue::Storage(std::move(arr)));
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            char c = peek();
            ++pos_;
            if (c == '"') break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            char esc = peek();
            ++pos_;
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    // Our own canonicalizer never needs to escape non-ASCII (ensure_ascii=False,
                    // UTF-8 output) — \u appears only for control characters, which is all this
                    // narrow grammar has to decode correctly.
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = peek();
                        ++pos_;
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else throw std::runtime_error("bad \\u escape");
                    }
                    out.push_back(static_cast<char>(code & 0xFFu));
                    break;
                }
                default: throw std::runtime_error("bad escape");
            }
        }
        return out;
    }

    JsonValue parse_number() {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        bool any_digit = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            ++pos_;
            any_digit = true;
        }
        if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            throw std::runtime_error("float literal in canonical pack — packer contract violated");
        }
        if (!any_digit) throw std::runtime_error("expected a number");
        std::int64_t value = 0;
        const auto res = std::from_chars(s_.data() + start, s_.data() + pos_, value);
        if (res.ec != std::errc{}) throw std::runtime_error("integer out of range or malformed");
        return JsonValue(JsonValue::Storage(value));
    }
};

}  // namespace detail

// --- typed views over the two worked-example domains --------------------------------------------

struct SkillDoc {
    std::string id;
    std::string name;
    std::string element;
    std::string icon;
    bool player_castable = false;
    std::string pose;
    std::string cost_vital;
    std::int32_t cost_amount = 0;
    std::int32_t cooldown_ticks = 0;
    std::int32_t payload_damage = 0, payload_pierce = 0, payload_crush = 0, payload_impulse = 0;
    std::int32_t payload_heat = 0, payload_cold = 0, payload_electric = 0, payload_explosion = 0;
};

struct EntityDoc {
    std::string id;
    std::string material;
    std::string scale;
    std::int32_t hp = 0;
    std::int32_t arm_ticks = 0;
    std::int32_t lifetime_ticks = 0;
    std::string collision;
    std::string team;
    bool destroyable = false;
};

// One immutable, loaded pack — the `std::shared_ptr<const CombatPack>` RFC-008's own "For an
// engineer" section describes. `load()` is the only entry point; a failed load (missing file,
// hash mismatch, malformed JSON) returns nullptr rather than throwing past this file, so a caller
// can decide how to react (today: nothing calls it outside tests — see the header comment).
class CombatPack {
public:
    [[nodiscard]] static std::shared_ptr<const CombatPack> load(const std::string& gen_dir) {
        std::string pack_bytes;
        if (!read_file(gen_dir + "/combat_pack.json", pack_bytes)) return nullptr;
        std::string hash_bytes;
        if (!read_file(gen_dir + "/combat_pack.hash.json", hash_bytes)) return nullptr;

        std::uint8_t digest[32];
        crypto_blake2b(digest, sizeof digest,
                        reinterpret_cast<const std::uint8_t*>(pack_bytes.data()), pack_bytes.size());
        const std::string full_hash_computed = hex_of(digest, sizeof digest);

        JsonValue hash_doc;
        JsonValue pack_doc;
        try {
            hash_doc = detail::JsonParser(hash_bytes).parse();
            pack_doc = detail::JsonParser(pack_bytes).parse();
        } catch (const std::exception&) {
            return nullptr;
        }

        const std::string full_hash_recorded = hash_doc.get_string("full_hash");
        if (full_hash_recorded != full_hash_computed) {
            std::fprintf(stderr,
                         "CombatPack::load: hash mismatch (recorded %s, computed %s) — "
                         "stale or corrupted %s/combat_pack.json; refusing to load\n",
                         full_hash_recorded.c_str(), full_hash_computed.c_str(), gen_dir.c_str());
            return nullptr;
        }

        // struct_hash is trusted from the sidecar, not independently recomputed here: reproducing
        // it would mean re-implementing the packer's §5 value-field-zeroing walk a second time in
        // C++, a second place for the two languages' canonicalization to silently drift apart.
        // full_hash above IS independently verified, because it needs no re-serialization at all —
        // it is a direct re-hash of the exact bytes already on disk.
        auto pack = std::make_shared<CombatPack>();
        pack->struct_hash_ = hash_doc.get_string("struct_hash");
        pack->full_hash_ = full_hash_recorded;
        pack->build(pack_doc);
        return pack;
    }

    [[nodiscard]] const std::string& struct_hash() const noexcept { return struct_hash_; }
    [[nodiscard]] const std::string& full_hash() const noexcept { return full_hash_; }

    [[nodiscard]] std::uint16_t id_of(const std::string& dotted_id) const {
        auto it = ids_.find(dotted_id);
        return it == ids_.end() ? 0 : it->second;
    }

    // Array-indexed by u16 (RFC-008 "For an engineer": "lookups are array-indexed by the u16 from
    // ids.lock.json"). Id 0 is reserved (V10) and never populated, matching the closed-set
    // convention every domain uses.
    [[nodiscard]] const SkillDoc* skill_by_id(std::uint16_t id) const noexcept {
        return (id != 0 && id < skills_.size() && !skills_[id].id.empty()) ? &skills_[id] : nullptr;
    }
    [[nodiscard]] const EntityDoc* entity_by_id(std::uint16_t id) const noexcept {
        return (id != 0 && id < entities_.size() && !entities_[id].id.empty()) ? &entities_[id]
                                                                                : nullptr;
    }

    // The lighter-weight accessor for fx/icon/snd/status — a generic lookup, not a typed struct
    // (see the header comment on why those four domains stop here this pass).
    [[nodiscard]] const JsonValue* find(const std::string& domain, const std::string& dotted_id) const {
        const JsonValue* d = root_.find("domains");
        if (d == nullptr) return nullptr;
        const JsonValue* dom = d->find(domain);
        return dom == nullptr ? nullptr : dom->find(dotted_id);
    }

private:
    static bool read_file(const std::string& path, std::string& out) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0) {
            std::fclose(f);
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        const std::size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        return got == out.size();
    }

    static std::string hex_of(const std::uint8_t* bytes, std::size_t n) {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(kHex[bytes[i] >> 4]);
            out.push_back(kHex[bytes[i] & 0x0Fu]);
        }
        return out;
    }

    void build(const JsonValue& root) {
        root_ = root;
        if (const JsonValue* ids = root.find("ids")) {
            for (const auto& [dotted_id, num] : ids->as_object()) {
                if (num.is_int()) ids_[dotted_id] = static_cast<std::uint16_t>(num.as_int());
            }
        }
        const JsonValue* domains = root.find("domains");
        if (domains == nullptr) return;

        if (const JsonValue* skills = domains->find("skill")) {
            std::uint16_t max_id = 0;
            for (const auto& [dotted_id, _] : skills->as_object()) max_id = std::max(max_id, id_of(dotted_id));
            skills_.assign(static_cast<std::size_t>(max_id) + 1, SkillDoc{});
            for (const auto& [dotted_id, doc] : skills->as_object()) {
                const std::uint16_t id = id_of(dotted_id);
                if (id == 0) continue;
                skills_[id] = parse_skill(dotted_id, doc);
            }
        }
        if (const JsonValue* entities = domains->find("entity")) {
            std::uint16_t max_id = 0;
            for (const auto& [dotted_id, _] : entities->as_object()) max_id = std::max(max_id, id_of(dotted_id));
            entities_.assign(static_cast<std::size_t>(max_id) + 1, EntityDoc{});
            for (const auto& [dotted_id, doc] : entities->as_object()) {
                const std::uint16_t id = id_of(dotted_id);
                if (id == 0) continue;
                entities_[id] = parse_entity(dotted_id, doc);
            }
        }
    }

    static SkillDoc parse_skill(const std::string& dotted_id, const JsonValue& doc) {
        SkillDoc s;
        s.id = dotted_id;
        s.name = doc.get_string("name");
        s.element = doc.get_string("element");
        s.icon = doc.get_string("icon");
        if (const JsonValue* player = doc.find("player")) {
            s.player_castable = true;
            s.pose = player->get_string("pose");
            s.cooldown_ticks = static_cast<std::int32_t>(player->get_int("cooldown_ticks"));
            if (const JsonValue* cost = player->find("cost")) {
                s.cost_vital = cost->get_string("vital");
                s.cost_amount = static_cast<std::int32_t>(cost->get_int("amount"));
            }
        }
        if (const JsonValue* phases = doc.find("phases")) {
            if (const JsonValue* impact = phases->find("impact")) {
                if (const JsonValue* payload = impact->find("payload")) {
                    s.payload_damage = static_cast<std::int32_t>(payload->get_int("damage"));
                    s.payload_pierce = static_cast<std::int32_t>(payload->get_int("pierce"));
                    s.payload_crush = static_cast<std::int32_t>(payload->get_int("crush"));
                    s.payload_impulse = static_cast<std::int32_t>(payload->get_int("impulse"));
                    s.payload_heat = static_cast<std::int32_t>(payload->get_int("heat"));
                    s.payload_cold = static_cast<std::int32_t>(payload->get_int("cold"));
                    s.payload_electric = static_cast<std::int32_t>(payload->get_int("electric"));
                    s.payload_explosion = static_cast<std::int32_t>(payload->get_int("explosion"));
                }
            }
        }
        return s;
    }

    static EntityDoc parse_entity(const std::string& dotted_id, const JsonValue& doc) {
        EntityDoc e;
        e.id = dotted_id;
        e.material = doc.get_string("material");
        e.scale = doc.get_string("scale");
        e.hp = static_cast<std::int32_t>(doc.get_int("hp"));
        e.arm_ticks = static_cast<std::int32_t>(doc.get_int("arm_ticks"));
        e.lifetime_ticks = static_cast<std::int32_t>(doc.get_int("lifetime_ticks"));
        e.collision = doc.get_string("collision");
        e.team = doc.get_string("team");
        const JsonValue* d = doc.find("destroyable");
        e.destroyable = (d != nullptr && d->is_bool() && d->as_bool());
        return e;
    }

    JsonValue root_;
    std::string struct_hash_;
    std::string full_hash_;
    std::map<std::string, std::uint16_t> ids_;
    std::vector<SkillDoc> skills_;
    std::vector<EntityDoc> entities_;
};

}  // namespace mmo
