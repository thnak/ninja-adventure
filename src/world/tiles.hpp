// World geometry, entity PODs and the deterministic RNG shared by every actor.
//
// Everything here is plain data with no engine dependency — the simulation actors (chunk_actor.hpp,
// player_actor.hpp, map_director.hpp) and the renderer both include this and nothing else in common.
// That is deliberate: the render seam carries only these types, so swapping raylib for another
// backend touches no simulation code.
//
// COORDINATES. There is exactly one spatial unit in the simulation: the **tile**, as a float, in
// map-global space (`0 .. kMapTiles`). Pixels exist only inside the renderer (`kTilePx`), and chunk
// membership is a pure function of a tile coordinate (`chunk_of`). Keeping one unit removes the
// whole class of "which space is this in?" bug that a client/server split usually invites.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mmo {

// --- Geometry ------------------------------------------------------------------------------------
// A chunk is the unit of ACTOR OWNERSHIP: one ChunkActor owns exactly one chunk and is the single
// writer of everything inside it. Sizing is a trade — smaller chunks mean more actors (more
// parallelism, more cross-chunk migration traffic), larger chunks mean fatter sequential handlers.
// 32x32 keeps a chunk's snapshot comfortably under a page while still giving 64 actors per map.
inline constexpr int kTilePx = 32;      // renderer-only: pixels per tile
inline constexpr int kChunkTiles = 32;  // tiles per chunk edge
inline constexpr int kMapChunks = 32;   // chunks per map edge
inline constexpr int kMapTiles = kChunkTiles * kMapChunks;  // 1024 tiles per overworld edge
inline constexpr int kChunksPerMap = kMapChunks * kMapChunks;  // 1024 chunk actors

// ONE seamless overworld. Instanced realms behind gates get map ids allocated at runtime (ARCH §4);
// they are not a fixed enum any more, which is why `kMapCount` is 1 rather than 3.
inline constexpr int kMapCount = 1;
inline constexpr int kChunkCount = kMapCount * kChunksPerMap;

inline constexpr std::uint16_t kOverworld = 0;

// The world seed lives here, not in world.hpp, because `terrain_of` is the thing that consumes it
// and the renderer needs it too (to know which ring a tile is in without asking an actor).
inline constexpr std::uint64_t kWorldSeed = 0x5EED'0BEEF'CAFEull;

// Where a new player is dropped: the centre of the map, which is also the easiest ring. The tilled
// apron there is part of the terrain *function* (below), not an edit applied afterwards — see
// `terrain_of`. Once world generation lands (ROADMAP P1) this becomes "the starting village", and
// these constants go away.
inline constexpr int kHomeTx = kMapTiles / 2;
inline constexpr int kHomeTy = kMapTiles / 2;
inline constexpr int kFarmRadius = 6;

// --- Terrain -------------------------------------------------------------------------------------
enum class Terrain : std::uint8_t {
    kGrass = 0,
    kDirt = 1,   // tilled — the only terrain a crop may be planted on
    kWater = 2,  // impassable
    kStone = 3,
    kSand = 4,
    kTree = 5,  // impassable, harvestable for wood
    kSnow = 6,   // walkable; crops freeze without a hearth nearby
    kMarsh = 7,  // walkable but slow; no foundations without stilts
    kAsh = 8,    // walkable, barren — nothing grows here at all
    kCount = 9,
};

[[nodiscard]] inline constexpr bool is_walkable(Terrain t) noexcept {
    return t != Terrain::kWater && t != Terrain::kTree;
}

// Can a crop be planted here once tilled? Ash never; everything else walkable can.
[[nodiscard]] inline constexpr bool is_tillable(Terrain t) noexcept {
    return is_walkable(t) && t != Terrain::kAsh && t != Terrain::kStone;
}

// --- Rings: difficulty radiates out from the centre (Valheim-style) --------------------------------
// One rule the player never has to be told: further from the middle is harder. It also removes the
// hardest world-generation constraint for free — stronghold density is a function of radius, so a
// stronghold can never smother a central village. See GAME.md §4.
enum class Ring : std::uint8_t {
    kMeadow = 0,    // the chill ring. Deliberately huge: a player who never leaves has a whole home
    kForest = 1,
    kWetland = 2,   // swamp on one side of the map, desert on the other
    kSnow = 3,
    kWasteland = 4,
    kCount = 5,
};

inline constexpr int kRingCount = static_cast<int>(Ring::kCount);

// Outer edge of each ring, as a fraction of the map's half-width, measured in CHEBYSHEV distance
// (square rings, not circles).
//
// Two things were wrong with the obvious version and the world-map exporter showed both at a glance:
//
//   * **Euclidean rings waste the corners.** A circle inscribed in the map leaves four corners that
//     are all the outermost, harshest ring. The first render was an island of content in a sea of
//     ash. Square rings use the whole map.
//   * **Equal radius steps are nowhere near equal areas.** Area grows as r-squared, so evenly spaced
//     edges gave Meadow 4.6% and Wasteland 44.6% — precisely backwards, since Meadow is the ring a
//     player who never wants to leave has to live in. Measured, not guessed.
//
// So the edges below are the square roots of the CUMULATIVE AREA each ring should own:
//   Meadow 26% · Forest 22% · Wetland 20% · Snow 18% · Wasteland 14%.
// Meadow is deliberately the biggest: it is the chill ring, and it should feel like a whole country.
inline constexpr float kRingEdge[kRingCount] = {0.5099f, 0.6928f, 0.8246f, 0.9274f, 1.01f};

// --- Entities ------------------------------------------------------------------------------------
enum class MobKind : std::uint8_t { kSlime = 0, kSpider = 1, kGhost = 2 };
enum class CropKind : std::uint8_t { kWheat = 0, kCarrot = 1, kPumpkin = 2 };
enum class BuildKind : std::uint8_t {
    // The player's home fire. It marks where you live and where you respawn, and in the snow rings
    // it is what keeps crops alive. It is NOT a thing whose destruction ends anything — losing it
    // costs you a rebuild and a respawn point, nothing more. (It replaced a `kCore` that the whole
    // world had to defend; see ARCHITECTURE.md §0 S2 for why that was the wrong shape once you can
    // build anywhere and villages exist.)
    kHearth = 0,
    kWall = 1,
    kTurret = 2,
    kPlot = 3,
    kFence = 4,  // cheap, weak, but it still stops a mob — the early-game perimeter
    kCount = 5,
};

// Everything except a crop plot is solid: a mob cannot walk through it and must break it instead.
// This is what makes a perimeter mean anything — before it existed, walls were decorative and mobs
// walked straight past them to the core.
[[nodiscard]] inline constexpr bool blocks_movement(BuildKind k) noexcept {
    return k != BuildKind::kPlot;
}

// Buildings upgrade in place. Level is 1-based; kMaxLevel is the cap.
inline constexpr std::uint8_t kMaxLevel = 3;
enum class ItemKind : std::uint8_t { kWood = 0, kStone = 1, kSeed = 2, kProduce = 3, kCount = 4 };

inline constexpr int kItemKinds = static_cast<int>(ItemKind::kCount);

struct MobStats {
    std::int16_t max_hp;
    float speed;         // tiles per second
    std::int16_t damage;  // per attack
};

[[nodiscard]] inline constexpr MobStats stats_of(MobKind k) noexcept {
    switch (k) {
        case MobKind::kSlime: return {30, 1.2f, 4};
        case MobKind::kSpider: return {45, 2.6f, 7};
        case MobKind::kGhost: return {80, 1.8f, 12};
    }
    return {30, 1.2f, 4};
}

// Which way a creature is facing. The order matches the COLUMN order of the Ninja Adventure walk
// sheets so the renderer can pass it straight through as an atlas column — see atlas_slots.hpp.
enum class Facing : std::uint8_t { kDown = 0, kUp = 1, kLeft = 2, kRight = 3 };

// Facing from a movement vector. Whichever axis dominates wins, which is what reads correctly for
// a 4-direction sprite set moving diagonally.
[[nodiscard]] inline Facing facing_of(float dx, float dy) noexcept {
    if (std::abs(dx) > std::abs(dy)) return dx < 0.0f ? Facing::kLeft : Facing::kRight;
    return dy < 0.0f ? Facing::kUp : Facing::kDown;
}

// A mob lives in map-global tile space. Its owning chunk is derived, never stored — so migration is
// "recompute the owner and forward", with no field to forget to update.
struct Mob {
    std::uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    MobKind kind = MobKind::kSlime;
    std::uint8_t attack_cd = 0;  // ticks until this mob may strike again
    Facing facing = Facing::kDown;
};

struct Crop {
    std::uint16_t tx = 0;  // map-global tile
    std::uint16_t ty = 0;
    CropKind kind = CropKind::kWheat;
    std::uint8_t stage = 0;      // 0..kCropStages-1; kCropStages-1 == ripe
    std::int64_t planted_ms = 0;
    std::int64_t ripe_ms = 0;
};

inline constexpr std::uint8_t kCropStages = 4;

[[nodiscard]] inline constexpr std::int64_t grow_ms_of(CropKind k) noexcept {
    switch (k) {
        case CropKind::kWheat: return 20'000;
        case CropKind::kCarrot: return 35'000;
        case CropKind::kPumpkin: return 60'000;
    }
    return 20'000;
}

struct Building {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    BuildKind kind = BuildKind::kWall;
    std::int16_t hp = 0;
    std::uint8_t cooldown = 0;  // turret fire cooldown, in ticks
    std::uint8_t level = 1;
};

[[nodiscard]] inline constexpr std::int16_t base_hp_of(BuildKind k) noexcept {
    switch (k) {
        case BuildKind::kHearth: return 400;
        case BuildKind::kWall: return 200;
        case BuildKind::kTurret: return 120;
        case BuildKind::kPlot: return 20;
        case BuildKind::kFence: return 60;
        case BuildKind::kCount: break;
    }
    return 100;
}

// Level 1/2/3 -> x1 / x1.8 / x3. Upgrading is meant to beat rebuilding: three level-1 walls have
// 600 HP spread over three tiles, one level-3 wall has 600 HP on the tile that matters.
[[nodiscard]] inline constexpr std::int16_t max_hp_of(BuildKind k, std::uint8_t level = 1) noexcept {
    const int base = base_hp_of(k);
    const int scaled = (level >= 3) ? base * 3 : (level == 2 ? (base * 9) / 5 : base);
    return static_cast<std::int16_t>(scaled);
}

// Turret stats per level. Range grows slowly and damage fast, so upgrading is about killing what
// already walks into the field rather than covering more ground.
[[nodiscard]] inline constexpr float turret_range(std::uint8_t level) noexcept {
    return level >= 3 ? 8.5f : (level == 2 ? 7.0f : 6.0f);
}
[[nodiscard]] inline constexpr std::int16_t turret_damage(std::uint8_t level) noexcept {
    return level >= 3 ? 34 : (level == 2 ? 22 : 14);
}
[[nodiscard]] inline constexpr std::uint8_t turret_cooldown(std::uint8_t level) noexcept {
    return level >= 3 ? 4 : (level == 2 ? 5 : 6);
}

// --- Chunk addressing ----------------------------------------------------------------------------
struct ChunkCoord {
    std::uint16_t map = 0;
    std::uint16_t cx = 0;
    std::uint16_t cy = 0;

    friend constexpr bool operator==(ChunkCoord, ChunkCoord) = default;
};

// The actor key. One flat integer so `router.get<ChunkActor>(key)` addresses a chunk directly, and
// so the same key is a stable placement input once chunks are distributed across nodes.
[[nodiscard]] inline constexpr std::uint64_t chunk_key(ChunkCoord c) noexcept {
    return (static_cast<std::uint64_t>(c.map) << 32) | (static_cast<std::uint64_t>(c.cy) << 16) |
           static_cast<std::uint64_t>(c.cx);
}

// Dense index into a flat per-chunk array (the snapshot bus). Distinct from `chunk_key`, which is
// sparse-by-design because it doubles as a placement key.
[[nodiscard]] inline constexpr int chunk_index(ChunkCoord c) noexcept {
    return c.map * kChunksPerMap + c.cy * kMapChunks + c.cx;
}

[[nodiscard]] inline constexpr bool in_map(float tx, float ty) noexcept {
    return tx >= 0.0f && ty >= 0.0f && tx < static_cast<float>(kMapTiles) &&
           ty < static_cast<float>(kMapTiles);
}

// Which chunk owns this tile coordinate? Caller must have checked `in_map` first.
[[nodiscard]] inline constexpr ChunkCoord chunk_of(std::uint16_t map, float tx, float ty) noexcept {
    return ChunkCoord{map, static_cast<std::uint16_t>(static_cast<int>(tx) / kChunkTiles),
                      static_cast<std::uint16_t>(static_cast<int>(ty) / kChunkTiles)};
}

// Tile index local to a chunk, for the terrain array.
[[nodiscard]] inline constexpr int local_tile_index(int tx, int ty) noexcept {
    return (ty % kChunkTiles) * kChunkTiles + (tx % kChunkTiles);
}

// --- Deterministic RNG ---------------------------------------------------------------------------
// splitmix64. Every stochastic decision in the simulation draws from a seed derived from
// (chunk key, tick) — so a replay of the same tick sequence produces the same world, on any node.
// That property is what makes redundant execution across untrusted nodes checkable at all.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : s_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9E37'79B9'7F4A'7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, n). Modulo bias is irrelevant at these magnitudes.
    constexpr std::uint32_t below(std::uint32_t n) noexcept {
        return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n);
    }

    constexpr float unit() noexcept {  // [0,1)
        return static_cast<float>(next() >> 40) * (1.0f / 16'777'216.0f);
    }

private:
    std::uint64_t s_;
};

// --- Terrain generation --------------------------------------------------------------------------
// Terrain is a PURE FUNCTION of (world seed, map, global tile) — no neighbour lookups, no smoothing
// pass, no global state. Two consequences that matter more than the visual result:
//
//   * Any node can compute any tile without owning it. A chunk checking whether the tile a mob is
//     about to step onto is walkable does not have to ask the neighbouring chunk — which would be a
//     synchronous cross-actor (and eventually cross-machine) read on the movement hot path.
//   * A chunk re-placed after a node failure regenerates its terrain from its key alone; only the
//     mutable overlay (crops, buildings, mobs) has to be recovered from persistence.
//
// A chunk still caches its own 32x32 block, because the common case is a tile it owns.
// One lattice sample in [0,1). The whole noise field is built from this, so it is the only place
// randomness enters terrain.
[[nodiscard]] inline float lattice(std::uint64_t seed, int ix, int iy) noexcept {
    Rng r(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ix)) * 0x9E37'79B9ull) ^
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(iy)) * 0x85EB'CA6Bull));
    return r.unit();
}

// Smoothstep-interpolated value noise. `scale` is the lattice spacing in tiles: bigger means
// broader features. INTERPOLATION is the whole point — the previous version hashed each tile
// independently, which has no spatial correlation at all, so water/stone came out as isolated
// single-tile confetti instead of lakes and outcrops.
[[nodiscard]] inline float value_noise(std::uint64_t seed, float x, float y, float scale) noexcept {
    const float fx = x / scale;
    const float fy = y / scale;
    const float ffx = std::floor(fx);
    const float ffy = std::floor(fy);
    const int ix = static_cast<int>(ffx);
    const int iy = static_cast<int>(ffy);

    const float dx = fx - ffx;
    const float dy = fy - ffy;
    const float sx = dx * dx * (3.0f - 2.0f * dx);  // smoothstep: C1-continuous across lattice cells
    const float sy = dy * dy * (3.0f - 2.0f * dy);

    const float n00 = lattice(seed, ix, iy);
    const float n10 = lattice(seed, ix + 1, iy);
    const float n01 = lattice(seed, ix, iy + 1);
    const float n11 = lattice(seed, ix + 1, iy + 1);

    const float a = n00 + (n10 - n00) * sx;
    const float b = n01 + (n11 - n01) * sx;
    return a + (b - a) * sy;
}

// Two octaves: a broad shape plus a finer one, so coastlines and forest edges are not perfectly
// smooth blobs.
[[nodiscard]] inline float fbm(std::uint64_t seed, float x, float y, float scale) noexcept {
    return 0.68f * value_noise(seed, x, y, scale) +
           0.32f * value_noise(seed ^ 0xA5A5'5A5Aull, x, y, scale * 0.42f);
}

// Which ring a tile is in. Noise on the radius keeps the boundaries from reading as a dartboard —
// rings get headlands and bays, and you cross them without noticing a line.
[[nodiscard]] inline Ring ring_of(std::uint64_t world_seed, int gx, int gy) noexcept {
    const float half = static_cast<float>(kMapTiles) * 0.5f;
    const float dx = (static_cast<float>(gx) - half) / half;
    const float dy = (static_cast<float>(gy) - half) / half;
    // Chebyshev, so the rings are squares and the map's corners are used — see kRingEdge.
    float r = std::max(std::abs(dx), std::abs(dy));
    // Wobble the radius, not the thresholds — that way the whole boundary moves coherently instead
    // of dissolving into per-tile speckle.
    r += (fbm(world_seed ^ 0x5150'5150ull, static_cast<float>(gx), static_cast<float>(gy), 70.0f) -
          0.5f) * 0.16f;
    for (int i = 0; i < kRingCount; ++i) {
        if (r < kRingEdge[i]) return static_cast<Ring>(i);
    }
    return Ring::kWasteland;
}

[[nodiscard]] inline Terrain terrain_of(std::uint64_t world_seed, std::uint16_t map, int gx,
                                        int gy) noexcept {
    // The starting apron is tilled soil by definition. Folding it into the function (rather than
    // stamping it over generated terrain) keeps the function total: a chunk querying a tile just
    // across its border gets the same answer the owning chunk would give.
    {
        const int dx = gx - kHomeTx;
        const int dy = gy - kHomeTy;
        if (dx >= -kFarmRadius && dx <= kFarmRadius && dy >= -kFarmRadius && dy <= kFarmRadius) {
            return Terrain::kDirt;
        }
    }

    const std::uint64_t base = world_seed ^ (static_cast<std::uint64_t>(map) << 48);
    const auto x = static_cast<float>(gx);
    const auto y = static_cast<float>(gy);
    const Ring ring = ring_of(world_seed, gx, gy);

    // Three fields, shared by every ring; only the THRESHOLDS change per ring. Using the same noise
    // everywhere is what makes a biome boundary look like the same land changing rather than two
    // maps stitched together.
    const float wet = fbm(base ^ 0x1111'2222ull, x, y, 26.0f);   // low = water
    const float rock = fbm(base ^ 0x7777'8888ull, x, y, 34.0f);  // high = stone
    const float flora = fbm(base ^ 0x3333'4444ull, x, y, 11.0f);  // high = trees

    switch (ring) {
        case Ring::kMeadow:
            if (wet < 0.24f) return Terrain::kWater;   // small ponds
            if (wet < 0.28f) return Terrain::kSand;
            if (flora > 0.74f) return Terrain::kTree;  // sparse copses
            return Terrain::kGrass;

        case Ring::kForest:
            if (wet < 0.26f) return Terrain::kWater;
            if (wet < 0.30f) return Terrain::kSand;
            if (rock > 0.80f) return Terrain::kStone;
            if (flora > 0.63f) return Terrain::kTree;  // dense, but with gaps you can walk through
            return Terrain::kGrass;

        case Ring::kWetland: {
            // Swamp on one side, desert on the other. The split is NOISY, not a meridian — the
            // first version compared `gx > centre` and drew a dead-straight line down the middle of
            // the map, which the exporter made impossible to miss. Now the boundary wanders, and
            // the seam between the two is a real place you can walk along.
            const float lean = (x - static_cast<float>(kHomeTx)) / (static_cast<float>(kMapTiles) * 0.25f);
            const float wander = (fbm(base ^ 0x2B2B'3C3Cull, x, y, 90.0f) - 0.5f) * 3.0f;
            const bool desert = (lean + wander) > 0.0f;
            if (desert) {
                if (wet < 0.20f) return Terrain::kWater;  // rare oasis
                if (rock > 0.82f) return Terrain::kStone;
                if (flora > 0.86f) return Terrain::kTree;  // the odd palm
                return Terrain::kSand;
            }
            if (wet < 0.42f) return Terrain::kWater;  // lots of standing water
            if (flora > 0.66f) return Terrain::kTree;
            return Terrain::kMarsh;
        }

        case Ring::kSnow:
            if (wet < 0.22f) return Terrain::kWater;  // frozen over in winter (P7)
            if (rock > 0.66f) return Terrain::kStone;
            if (flora > 0.78f) return Terrain::kTree;
            return Terrain::kSnow;

        case Ring::kWasteland:
        case Ring::kCount: break;
    }
    if (wet < 0.18f) return Terrain::kWater;
    if (rock > 0.55f) return Terrain::kStone;
    return Terrain::kAsh;
}

// Which of four mirror orientations to draw a base terrain tile in. A single 16x16 grass tile
// repeated across a 256x256 map shows an obvious grid; mirroring per-tile breaks the repeat at
// zero cost (a negative source rect, no extra texture). Renderer-only — the simulation neither
// knows nor cares.
[[nodiscard]] inline int tile_variant(std::uint16_t map, int gx, int gy) noexcept {
    Rng r((static_cast<std::uint64_t>(map) << 40) ^
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 20) ^
          static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy)));
    return static_cast<int>(r.below(4));
}

// --- Spawn camps ---------------------------------------------------------------------------------
// Waves used to spawn at a random walkable tile in EVERY rim chunk, which meant monsters arrived
// from all 360 degrees at once. That quietly made defence pointless: no wall matters if the only
// way to use one is to enclose the entire perimeter. A small number of fixed camps turns the map
// into a handful of approach lanes — and because mobs follow the flow field, those lanes are
// predictable, so a wall placed across one actually changes the outcome.
//
// Camp positions are a pure function of (seed, map), like everything else about the world, so every
// node agrees on where they are without exchanging a message.
inline constexpr int kSpawnCamps = 5;
inline constexpr int kCampInset = 6;    // tiles in from the map edge
inline constexpr int kCampRadius = 3;   // mobs spawn within this many tiles of the camp

[[nodiscard]] inline void camp_tile(std::uint64_t world_seed, std::uint16_t map, int index, int& tx,
                                    int& ty) noexcept {
    // Walk the map's border rectangle (inset by kCampInset) and take evenly spaced points, jittered
    // per map so the three maps do not have identical layouts.
    const int span = kMapTiles - 2 * kCampInset;
    const int perimeter = 4 * span;
    Rng r(world_seed ^ (static_cast<std::uint64_t>(map) << 32) ^ 0xCA'11'CA'11ull);
    const int jitter = static_cast<int>(r.below(static_cast<std::uint32_t>(perimeter)));
    int t = (jitter + (perimeter * index) / kSpawnCamps) % perimeter;

    if (t < span) {  // top edge, left to right
        tx = kCampInset + t;
        ty = kCampInset;
    } else if (t < 2 * span) {  // right edge, top to bottom
        tx = kMapTiles - 1 - kCampInset;
        ty = kCampInset + (t - span);
    } else if (t < 3 * span) {  // bottom edge, right to left
        tx = kMapTiles - 1 - kCampInset - (t - 2 * span);
        ty = kMapTiles - 1 - kCampInset;
    } else {  // left edge, bottom to top
        tx = kCampInset;
        ty = kMapTiles - 1 - kCampInset - (t - 3 * span);
    }

    // Nudge to the nearest walkable tile: a camp inside a lake would spawn nothing.
    if (is_walkable(terrain_of(world_seed, map, tx, ty))) return;
    for (int radius = 1; radius <= 12; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int nx = tx + dx;
                const int ny = ty + dy;
                if (nx < 1 || ny < 1 || nx >= kMapTiles - 1 || ny >= kMapTiles - 1) continue;
                if (!is_walkable(terrain_of(world_seed, map, nx, ny))) continue;
                tx = nx;
                ty = ny;
                return;
            }
        }
    }
}

// --- Simulation cadence --------------------------------------------------------------------------
inline constexpr int kTicksPerSecond = 10;
inline constexpr std::int64_t kTickMs = 1000 / kTicksPerSecond;
inline constexpr std::int64_t kDayMs = 45'000;    // daylight: farm and build
inline constexpr std::int64_t kNightMs = 30'000;  // night: waves attack the core
inline constexpr std::int64_t kCycleMs = kDayMs + kNightMs;

[[nodiscard]] inline constexpr bool is_night(std::int64_t world_ms) noexcept {
    return (world_ms % kCycleMs) >= kDayMs;
}

}  // namespace mmo
