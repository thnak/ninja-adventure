// Authored maps — a hand-painted or algorithmically-generated small map (a room, a rest realm),
// stored and loaded as one flat binary file.
//
// Two producers write this exact format: a person using `map_editor`, and (once built)
// `dungeon_gen.hpp`'s `generate_dungeon()`. One shared struct/format lets the generator's output
// become an ordinary editable file the moment it's saved — there is no separate "generated" vs
// "authored" representation, only this one.
//
// Format follows `persistence.hpp`'s own established convention (magic + version header, plain
// `std::fwrite`/`std::fread` of POD data) rather than introducing a JSON dependency — that file's
// own header comment already rules JSON out for this codebase ("no JSON library exists anywhere
// ... adding one is out of scope"), and this is simply the second file that needs the same shape
// of record `WorldManifest`/`AccountStore` already established.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "render/atlas_slots.hpp"
#include "world/tiles.hpp"
#include "world/village.hpp"

namespace mmo {

// One room/realm, not the overworld — the same "cheap because it's small" scale kInterior's own
// arithmetic rooms already assume.
inline constexpr int kAuthoredMapMaxSide = 64;

struct AuthoredStructure {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    StructureKind kind = StructureKind::kHouseOrange;
};

struct AuthoredDecor {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    Slot slot = Slot::kTerrainGrass;  // decor has no semantic type today, only an appearance
};

struct AuthoredSpawn {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    CreatureKind kind = CreatureKind::kSlime;
};

// Standing on (x,y) on THIS map steps the player to (to_map, to_x, to_y). Kept separate from
// tiles.hpp's Door/portal_at — the ~440-entry sorted array sized and binary-searched for exactly
// the overworld<->kInterior pair every village dojo door already depends on — rather than
// generalizing that array's semantics. An authored map's handful of portals are a different shape
// of problem (arbitrary map-to-map, small count), so `portal_at` gets one additional linear check
// against this list instead of folding it into the existing one.
struct AuthoredPortal {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t to_map = 0;
    std::uint16_t to_x = 0;
    std::uint16_t to_y = 0;
};

struct AuthoredMap {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<Terrain> terrain;  // width*height, row-major
    std::vector<AuthoredStructure> structures;
    std::vector<AuthoredDecor> decor;
    std::vector<AuthoredSpawn> spawns;
    std::vector<AuthoredPortal> portals;

    [[nodiscard]] Terrain tile_at(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
            return Terrain::kBuilding;
        }
        return terrain[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
    }
};

namespace detail_authored {
// "AMAP", written as one big-endian-independent uint32 the same way WorldManifest's kManifestMagic
// is — a byte pattern to eyeball in a hex dump, not a string.
inline constexpr std::uint32_t kAuthoredMapMagic = 0x414D'4150;
inline constexpr std::uint32_t kAuthoredMapVersion = 1;

template <typename T>
[[nodiscard]] inline bool write_vec(std::FILE* f, const std::vector<T>& v) {
    const auto count = static_cast<std::uint32_t>(v.size());
    if (std::fwrite(&count, sizeof count, 1, f) != 1) return false;
    return count == 0 || std::fwrite(v.data(), sizeof(T), count, f) == count;
}

// `max_count` is a sanity bound on a corrupt/truncated file, mirroring AccountStore::load's
// `kMaxRecords` guard — never trusted as large as the count field could claim.
template <typename T>
[[nodiscard]] inline bool read_vec(std::FILE* f, std::vector<T>& v, std::uint32_t max_count) {
    std::uint32_t count = 0;
    if (std::fread(&count, sizeof count, 1, f) != 1 || count > max_count) return false;
    v.assign(count, T{});
    return count == 0 || std::fread(v.data(), sizeof(T), count, f) == count;
}
}  // namespace detail_authored

[[nodiscard]] inline bool save_authored_map(const char* path, const AuthoredMap& m) {
    if (m.width == 0 || m.height == 0 ||
        m.terrain.size() != static_cast<std::size_t>(m.width) * m.height) {
        return false;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const std::uint32_t header[2] = {detail_authored::kAuthoredMapMagic,
                                     detail_authored::kAuthoredMapVersion};
    bool ok = std::fwrite(header, sizeof header, 1, f) == 1 &&
              std::fwrite(&m.width, sizeof m.width, 1, f) == 1 &&
              std::fwrite(&m.height, sizeof m.height, 1, f) == 1 &&
              std::fwrite(m.terrain.data(), sizeof(Terrain), m.terrain.size(), f) == m.terrain.size();
    ok = ok && detail_authored::write_vec(f, m.structures);
    ok = ok && detail_authored::write_vec(f, m.decor);
    ok = ok && detail_authored::write_vec(f, m.spawns);
    ok = ok && detail_authored::write_vec(f, m.portals);
    std::fclose(f);
    return ok;
}

[[nodiscard]] inline bool load_authored_map(const char* path, AuthoredMap& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::uint32_t header[2] = {};
    bool ok = std::fread(header, sizeof header, 1, f) == 1 &&
              header[0] == detail_authored::kAuthoredMapMagic &&
              header[1] == detail_authored::kAuthoredMapVersion &&
              std::fread(&out.width, sizeof out.width, 1, f) == 1 &&
              std::fread(&out.height, sizeof out.height, 1, f) == 1 && out.width > 0 &&
              out.width <= kAuthoredMapMaxSide && out.height > 0 &&
              out.height <= kAuthoredMapMaxSide;
    if (ok) {
        const std::size_t tiles = static_cast<std::size_t>(out.width) * out.height;
        out.terrain.assign(tiles, Terrain::kBuilding);
        ok = std::fread(out.terrain.data(), sizeof(Terrain), tiles, f) == tiles;
    }
    ok = ok && detail_authored::read_vec(f, out.structures, 4096);
    ok = ok && detail_authored::read_vec(f, out.decor, 8192);
    ok = ok && detail_authored::read_vec(f, out.spawns, 1024);
    ok = ok && detail_authored::read_vec(f, out.portals, 64);
    std::fclose(f);
    if (!ok) out = AuthoredMap{};
    return ok;
}

// --- Runtime registry: which persistent MapIds are authored, and their loaded content ----------
// Mirrors tiles.hpp's own `publish_overlay`/`publish_doors` shape exactly: derived once at world
// bring-up (here, loaded from disk instead of computed from the seed), written once before the
// engine starts, const from then on, and readable by any node/tile query without a layout handle.
namespace detail_authored {
inline constexpr int kMaxAuthoredMaps = 8;
inline AuthoredMap g_maps[kMaxAuthoredMaps];
inline std::uint16_t g_map_ids[kMaxAuthoredMaps] = {};
inline int g_map_count = 0;
}  // namespace detail_authored

// Called once by world bring-up, after `load_authored_map` succeeds for a given persistent MapId.
inline void publish_authored_map(std::uint16_t map_id, AuthoredMap m) noexcept {
    for (int i = 0; i < detail_authored::g_map_count; ++i) {
        if (detail_authored::g_map_ids[i] == map_id) {
            detail_authored::g_maps[i] = std::move(m);
            return;
        }
    }
    if (detail_authored::g_map_count >= detail_authored::kMaxAuthoredMaps) return;
    detail_authored::g_map_ids[detail_authored::g_map_count] = map_id;
    detail_authored::g_maps[detail_authored::g_map_count] = std::move(m);
    ++detail_authored::g_map_count;
}

[[nodiscard]] inline const AuthoredMap* authored_map_for(std::uint16_t map_id) noexcept {
    for (int i = 0; i < detail_authored::g_map_count; ++i) {
        if (detail_authored::g_map_ids[i] == map_id) return &detail_authored::g_maps[i];
    }
    return nullptr;
}

[[nodiscard]] inline Terrain authored_tile(std::uint16_t map_id, int gx, int gy) noexcept {
    const AuthoredMap* m = authored_map_for(map_id);
    return m == nullptr ? Terrain::kBuilding : m->tile_at(gx, gy);
}

// Portal lookup for authored maps only — a small linear scan, additive to tiles.hpp's
// binary-searched Door array (see AuthoredPortal's own comment for why the two stay separate).
struct AuthoredPortalHit {
    std::uint16_t map = 0;
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    bool valid = false;
};

[[nodiscard]] inline AuthoredPortalHit authored_portal_at(std::uint16_t map, int tx,
                                                          int ty) noexcept {
    const AuthoredMap* m = authored_map_for(map);
    if (m == nullptr) return {};
    for (const AuthoredPortal& p : m->portals) {
        if (static_cast<int>(p.x) == tx && static_cast<int>(p.y) == ty) {
            return {p.to_map, p.to_x, p.to_y, true};
        }
    }
    return {};
}

}  // namespace mmo
