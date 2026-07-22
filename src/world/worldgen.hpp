// World generation — where the villages, the roads between them and the strongholds actually are.
//
// WHY THIS IS NOT A PURE FUNCTION, when terrain is. Terrain answers "what is at (x,y)?" from noise
// alone, and that property is load-bearing: any node can evaluate any tile without asking anyone.
// A village cannot work that way. Deciding whether a village belongs at (x,y) requires knowing
// where the OTHER villages are, and a road requires knowing both of its ends. Those are global
// questions, so they get a global answer computed once.
//
// It stays compatible with the distributed model for the same reason the flow field does: the
// layout is derived from the world seed and nothing else, so every node computes a byte-identical
// copy on its own. It is broadcast by construction rather than by message. Once it exists it is
// const, and `tiles.hpp` reads it through one pointer that is written before the engine starts.
//
// GENERATION ORDER, and why it is this order:
//
//   1. Village sites   — the only thing with a hard placement constraint (needs buildable land and
//                        distance from its neighbours). Everything else adapts to it.
//   2. Roads           — the skeleton. Laid BEFORE the buildings so a road can never be routed
//                        through a house; the houses are placed around a square the road already
//                        reaches.
//   3. Village stamps  — square, then houses ringing it.
//   4. Strongholds     — last, because they are the only feature allowed to be pushed around: a
//                        candidate too close to a village is simply dropped.
//
// Doing strongholds before villages was the mistake worth naming: a stronghold that lands on the
// only buildable ground in a region silently deletes a village, and the map ends up with a hole in
// it that nothing in the game can explain.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "world/tiles.hpp"

namespace mmo {

// --- What can stand on the map --------------------------------------------------------------
// Each of these is ONE multi-tile sprite, cropped from the Ninja Adventure tilesets after looking
// at it (tools/verify_structures.py). There is no per-tile wall art in this pack and there is no
// autotiling here — a house is a house, drawn in one quad, and its whole footprint is solid.
enum class StructureKind : std::uint8_t {
    kHouseOrange = 0,  // 4x3 — the common village house
    kHouseCream = 1,   // 4x3
    kHouseAmber = 2,   // 4x3
    kHouseRed = 3,     // 4x3 — the red hall; one per village, and only from tier 3
    kHouseBlue = 4,    // 3x3 — shopfront
    kHouseTan = 5,     // 3x3
    kHouseWood = 6,    // 3x3
    kHutSnowA = 7,     // 3x3 — snow ring
    kHutSnowB = 8,     // 3x3
    kHutSnowC = 9,     // 3x3
    kRuinA = 10,       // 3x3 — wasteland, overgrown
    kRuinB = 11,       // 3x3
    kTentA = 12,       // 3x3 — stronghold
    kTentB = 13,       // 3x3
    kTentC = 14,       // 3x3 — torn
    kCount = 15,
};

struct StructureSize {
    std::uint8_t w;
    std::uint8_t h;
};

[[nodiscard]] inline constexpr StructureSize size_of(StructureKind k) noexcept {
    switch (k) {
        case StructureKind::kHouseOrange:
        case StructureKind::kHouseCream:
        case StructureKind::kHouseAmber:
        case StructureKind::kHouseRed: return {4, 3};
        default: break;
    }
    return {3, 3};
}

// A placed building. `tx,ty` is its TOP-LEFT tile; the sprite is drawn over exactly (w,h) tiles
// from there, and every one of those tiles is `Terrain::kBuilding`.
struct Structure {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    StructureKind kind = StructureKind::kHouseOrange;
};

// A settlement. `tier` is 1..5 and drives how many houses it has — it is also the hook P3 grows
// into (GAME.md §6: villages level up when helped and stall when not).
struct Village {
    std::uint16_t tx = 0;  // centre of the square
    std::uint16_t ty = 0;
    std::uint8_t tier = 1;
    Ring ring = Ring::kMeadow;
    std::uint16_t first = 0;  // index into WorldLayout::structures()
    std::uint16_t count = 0;
};

// Where monsters live. Raids come out of these, and building inside one is forbidden (P3).
struct Stronghold {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    Ring ring = Ring::kMeadow;
    std::uint16_t first = 0;
    std::uint16_t count = 0;
};

// --- Tuning ------------------------------------------------------------------------------------
// Densities are per CELL of a jittered grid, which is a poor man's Poisson-disc: one candidate per
// cell, jittered inside it, gives an even scatter with no clumping and no rejection loop. Cell size
// is therefore the real knob — it is the minimum spacing, near enough.
//
// 112 tiles between villages is not arbitrary: it is about 20 seconds of walking, so a village is
// always plausibly "over there" rather than in sight of the last one.
inline constexpr int kVillageCell = 112;
inline constexpr int kStrongholdCell = 124;
// Keep settlements off the map border — but this number is not free to choose. The outermost ring
// starts at 0.9274 of the map's half-width, which puts the wasteland band within ~37 tiles of the
// edge. A margin of 40 was therefore WIDER THAN THE RING, and the wasteland could not hold a
// village no matter what its density said: the map reported 0 there and the density knob had no
// effect at all, which is what gave the bug away. 18 leaves room for a village's houses (which
// reach ~15 tiles out) and still lets the outer ring be inhabited.
inline constexpr int kVillageEdgeMargin = 18;
inline constexpr int kStrongholdKeepOut = 34;     // no stronghold this close to a village
inline constexpr int kMaxRoadLength = 300;        // do not join villages further apart than this

class WorldLayout {
public:
    explicit WorldLayout(std::uint64_t seed) : seed_(seed) {
        overlay_.assign(static_cast<std::size_t>(kMapTiles) * kMapTiles, kNoOverlay);
        by_chunk_.resize(static_cast<std::size_t>(kChunksPerMap));
        place_villages();
        lay_roads();
        build_villages();
        place_strongholds();
        index_structures();
        choose_spawn();
    }

    [[nodiscard]] const std::uint8_t* overlay() const noexcept { return overlay_.data(); }
    [[nodiscard]] const std::vector<Village>& villages() const noexcept { return villages_; }
    [[nodiscard]] const std::vector<Stronghold>& strongholds() const noexcept { return holds_; }
    [[nodiscard]] const std::vector<Structure>& structures() const noexcept { return structures_; }

    // Structures whose footprint touches this chunk. A 4x3 house straddling a border is listed in
    // both chunks, so a renderer iterating the chunks it is drawing never misses half a building.
    [[nodiscard]] const std::vector<std::uint32_t>& structures_in_chunk(int cx, int cy) const noexcept {
        static const std::vector<std::uint32_t> kEmpty;
        if (cx < 0 || cy < 0 || cx >= kMapChunks || cy >= kMapChunks) return kEmpty;
        return by_chunk_[static_cast<std::size_t>(cy) * kMapChunks + cx];
    }

    // Where a new player wakes up: walkable open ground a couple of minutes' walk from the nearest
    // village, deliberately not in sight of it. See GAME.md §6b.
    [[nodiscard]] int spawn_tx() const noexcept { return spawn_tx_; }
    [[nodiscard]] int spawn_ty() const noexcept { return spawn_ty_; }

    // Nearest settlement to a tile, by squared Euclidean distance. Linear over ~50 villages, which
    // is cheaper than any index at this count.
    [[nodiscard]] const Village* nearest_village(int tx, int ty) const noexcept {
        const Village* best = nullptr;
        long long best_d = 0;
        for (const Village& v : villages_) {
            const long long dx = static_cast<long long>(v.tx) - tx;
            const long long dy = static_cast<long long>(v.ty) - ty;
            const long long d = dx * dx + dy * dy;
            if (best == nullptr || d < best_d) {
                best = &v;
                best_d = d;
            }
        }
        return best;
    }

private:
    // --- overlay writes ------------------------------------------------------------------------
    void put(int x, int y, Terrain t) noexcept {
        if (x < 0 || y < 0 || x >= kMapTiles || y >= kMapTiles) return;
        overlay_[static_cast<std::size_t>(y) * kMapTiles + x] = static_cast<std::uint8_t>(t);
    }
    [[nodiscard]] std::uint8_t peek(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= kMapTiles || y >= kMapTiles) return kNoOverlay;
        return overlay_[static_cast<std::size_t>(y) * kMapTiles + x];
    }
    // Ground as it will actually be walked on: the overlay if generation has written there,
    // otherwise the raw land. Generation must never call `terrain_of` — that reads the overlay
    // pointer, which is not published until this object is finished.
    [[nodiscard]] Terrain ground(int x, int y) const noexcept {
        const std::uint8_t o = peek(x, y);
        if (o != kNoOverlay) return static_cast<Terrain>(o);
        return terrain_base(seed_, kOverworld, x, y);
    }

    // --- Coherent noise, in integers only ------------------------------------------------------
    // Every value in this file that shapes the world comes through here, and the reason is a
    // measured cross-platform divergence, not caution:
    //
    //   Linux/GCC and Windows/MSVC generated the SAME 49 villages, the SAME 23 strongholds, the
    //   same 493 buildings and the same spawn tile — and 266 different road tiles. The road carver
    //   compared a float `fbm(...) - 0.5f` against a threshold, and the two compilers disagreed on
    //   a handful of those comparisons (contraction, x87 spills, library sin/cos — it does not
    //   matter which). One flipped comparison does not move a road by one tile: it changes the
    //   step, and every step after it, so the road walks somewhere else entirely and 267 tiles that
    //   one machine left as lake the other paved as causeway.
    //
    // TERRAIN gets away with floats because a disagreement there is LOCAL — a threshold flip moves
    // one tile and stops. (Verified: ring and terrain tallies are identical on both toolchains.)
    // Anything ITERATIVE amplifies instead, and worldgen is iterative by nature: roads walk, houses
    // are placed by rejection, and each decision moves the state the next one reads.
    //
    // So generation uses this instead. `block` gives it spatial coherence — one value across a
    // block of tiles, which is what makes a road bend over a stretch instead of jittering per
    // tile — and it is pure integer arithmetic, which every compiler on every platform agrees on.
    [[nodiscard]] std::uint32_t block_noise(std::uint64_t salt, int x, int y, int block,
                                            std::uint32_t n) const noexcept {
        const auto bx = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x / block));
        const auto by = static_cast<std::uint64_t>(static_cast<std::uint32_t>(y / block));
        Rng r(seed_ ^ salt ^ (bx * 0x9E37'79B9'7F4A'7C15ull) ^ (by * 0xC2B2'AE3D'27D4'EB4Full));
        return r.below(n);
    }

    // --- 1. village sites ----------------------------------------------------------------------
    void place_villages() {
        const int cells = kMapTiles / kVillageCell;
        for (int cy = 0; cy < cells; ++cy) {
            for (int cx = 0; cx < cells; ++cx) {
                Rng r(seed_ ^ 0x5169'0000ull ^ (static_cast<std::uint64_t>(cy) << 20) ^
                      static_cast<std::uint64_t>(cx));
                // Jitter inside the cell, but keep away from the cell edge so two neighbours cannot
                // end up back to back.
                const int inset = kVillageCell / 5;
                const int span = kVillageCell - 2 * inset;
                const int tx = cx * kVillageCell + inset +
                               static_cast<int>(r.below(static_cast<std::uint32_t>(span)));
                const int ty = cy * kVillageCell + inset +
                               static_cast<int>(r.below(static_cast<std::uint32_t>(span)));
                if (tx < kVillageEdgeMargin || ty < kVillageEdgeMargin ||
                    tx >= kMapTiles - kVillageEdgeMargin || ty >= kMapTiles - kVillageEdgeMargin) {
                    continue;
                }
                if (!buildable_site(tx, ty, 9)) continue;

                const Ring ring = ring_of(seed_, tx, ty);
                // Fewer settlements the further out you go. This is NOT the same knob as
                // buildability, and conflating the two was the bug: the first version relied on
                // terrain alone to thin out the map, and produced seventeen villages in the snow
                // ring against five in the forest — precisely backwards. Snow is *easy* ground to
                // build on (open, flat, no trees) and forest is hard, so terrain-only filtering
                // rewards exactly the hostile places that should be emptiest.
                if (r.below(100) >= village_chance(ring)) continue;

                Village v{};
                v.tx = static_cast<std::uint16_t>(tx);
                v.ty = static_cast<std::uint16_t>(ty);
                v.ring = ring;
                v.tier = tier_for(ring, r);
                villages_.push_back(v);
            }
        }
    }

    // A site is buildable when a square around it is mostly dry land. The radius is generous on
    // purpose: a village crammed onto a peninsula has its houses in the sea.
    //
    // WATER AND TREES ARE NOT THE SAME OBSTACLE, and treating them as one is what tilted the whole
    // map toward the snow ring. You cannot found a village in a lake, but you absolutely can found
    // one in a wood — clearing trees is what people do. So water is nearly a veto and trees are
    // merely a preference, which lets forests hold settlements while lake shores still do not.
    [[nodiscard]] bool buildable_site(int tx, int ty, int radius) const noexcept {
        int wet = 0;
        int wooded = 0;
        int total = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const Terrain t = terrain_base(seed_, kOverworld, tx + dx, ty + dy);
                ++total;
                if (t == Terrain::kWater) ++wet;
                else if (t == Terrain::kTree) ++wooded;
            }
        }
        return wet * 100 <= total * 6 && wooded * 100 <= total * 45;
    }

    // How likely a viable site actually gets settled, by ring. See the note at the call site: this
    // is the knob that makes the outer map lonely, and it has to be separate from buildability.
    // Percent, not a float. `Rng::unit()` happens to be exact (an integer scaled by 2^-24), but
    // "happens to be exact" is not the standard this file holds itself to — see `block_noise`.
    [[nodiscard]] static std::uint32_t village_chance(Ring ring) noexcept {
        switch (ring) {
            case Ring::kMeadow: return 100;
            case Ring::kForest: return 85;
            case Ring::kWetland: return 62;
            case Ring::kSnow: return 42;
            case Ring::kWasteland:
            case Ring::kCount: break;
        }
        return 45;  // the wasteland keeps a few holdouts, and they are worth finding
    }

    // Difficulty radiates outward, so prosperity radiates inward: the sheltered middle of the map
    // grows towns, the wasteland manages a couple of huts. This is the same one rule the rings
    // already teach, said a second way — a player reads "things are worse out here" without a word
    // of text.
    [[nodiscard]] static std::uint8_t tier_for(Ring ring, Rng& r) noexcept {
        switch (ring) {
            case Ring::kMeadow: return static_cast<std::uint8_t>(3 + r.below(3));   // 3..5
            case Ring::kForest: return static_cast<std::uint8_t>(2 + r.below(3));   // 2..4
            case Ring::kWetland: return static_cast<std::uint8_t>(2 + r.below(2));  // 2..3
            case Ring::kSnow: return static_cast<std::uint8_t>(1 + r.below(2));     // 1..2
            case Ring::kWasteland:
            case Ring::kCount: break;
        }
        return 1;
    }

    // --- 2. roads ------------------------------------------------------------------------------
    // Each village is joined to its two nearest neighbours. Two, not one: with one you get a
    // spanning tree that dead-ends everywhere, and with three the map turns into a spider web. Two
    // gives loops without clutter, and it means most villages have a road in and a road out.
    void lay_roads() {
        const int n = static_cast<int>(villages_.size());
        for (int i = 0; i < n; ++i) {
            // Two nearest, found by a partial selection — n is ~50, so this is nothing.
            int best[2] = {-1, -1};
            long long bd[2] = {0, 0};
            for (int j = 0; j < n; ++j) {
                if (j == i) continue;
                const long long dx = static_cast<long long>(villages_[j].tx) - villages_[i].tx;
                const long long dy = static_cast<long long>(villages_[j].ty) - villages_[i].ty;
                const long long d = dx * dx + dy * dy;
                if (d > static_cast<long long>(kMaxRoadLength) * kMaxRoadLength) continue;
                if (best[0] < 0 || d < bd[0]) {
                    best[1] = best[0];
                    bd[1] = bd[0];
                    best[0] = j;
                    bd[0] = d;
                } else if (best[1] < 0 || d < bd[1]) {
                    best[1] = j;
                    bd[1] = d;
                }
            }
            for (int k = 0; k < 2; ++k) {
                if (best[k] < 0) continue;
                if (best[k] < i) continue;  // each pair laid once
                carve_road(villages_[i], villages_[static_cast<std::size_t>(best[k])]);
            }
        }
    }

    // Walk from A to B one tile at a time, preferring the straight step but sidestepping water.
    //
    // A road is not a line: a perpendicular wobble from the same fbm the terrain uses makes it bend
    // with the land instead of cutting across it. When the straight step lands in water the walk
    // tries the two sideways alternatives first, and only lays a CAUSEWAY over the water when it is
    // properly boxed in — a road that simply stopped at the shore would read as a bug, and one that
    // ignored water entirely would run across the middle of a lake.
    void carve_road(const Village& a, const Village& b) {
        int x = a.tx;
        int y = a.ty;
        const int gx = b.tx;
        const int gy = b.ty;
        const int budget = 4 * (std::abs(gx - x) + std::abs(gy - y)) + 64;

        for (int step = 0; step < budget; ++step) {
            if (std::abs(gx - x) <= 2 && std::abs(gy - y) <= 2) break;

            // Half the blocks go diagonally, a quarter each hold one axis. A COARSE field turned
            // the map into a circuit board — one axis stayed suppressed for dozens of steps and
            // every road came out as two long right-angled runs — so the block is small (7 tiles),
            // which makes a road read as a diagonal that wanders rather than as plumbing.
            const std::uint32_t wobble = block_noise(0x2EED'C0DEull, x, y, 7, 4);
            int sx = (gx > x) - (gx < x);
            int sy = (gy > y) - (gy < y);
            if (sx != 0 && sy != 0) {
                if (wobble == 2) sy = 0;
                else if (wobble == 3) sx = 0;
            }

            const int cand[3][2] = {{x + sx, y + sy}, {x + sx, y}, {x, y + sy}};
            int nx = x + sx;
            int ny = y + sy;
            bool dry = false;
            for (const auto& c : cand) {
                if (c[0] == x && c[1] == y) continue;
                if (terrain_base(seed_, kOverworld, c[0], c[1]) == Terrain::kWater) continue;
                nx = c[0];
                ny = c[1];
                dry = true;
                break;
            }
            (void)dry;  // wet steps are allowed — that is the causeway
            if (nx == x && ny == y) break;
            x = nx;
            y = ny;
            paint_road(x, y);
        }
    }

    // Two tiles wide, and never over a building: roads are laid before the houses go up, but a
    // village square placed later must be able to overwrite a road rather than the reverse.
    void paint_road(int x, int y) {
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                if (peek(x + dx, y + dy) != kNoOverlay) continue;
                put(x + dx, y + dy, Terrain::kPath);
            }
        }
    }

    // --- 3. village squares and houses ---------------------------------------------------------
    void build_villages() {
        for (Village& v : villages_) {
            v.first = static_cast<std::uint16_t>(structures_.size());
            const int plaza = 3 + v.tier;  // tier 1 -> 4 tiles, tier 5 -> 8

            // The square. Its edge is noisy so it reads as trodden ground rather than as a stamp.
            // Compared as SQUARED distances against an integer radius, so there is no sqrt and no
            // float threshold anywhere in it — see `block_noise`.
            for (int dy = -plaza; dy <= plaza; ++dy) {
                for (int dx = -plaza; dx <= plaza; ++dx) {
                    const int wob =
                        static_cast<int>(block_noise(0x9101'ACE5ull, v.tx + dx, v.ty + dy, 3, 5)) - 2;
                    const int edge = plaza + wob;
                    if (dx * dx + dy * dy > edge * edge) continue;
                    if (terrain_base(seed_, kOverworld, v.tx + dx, v.ty + dy) == Terrain::kWater) {
                        continue;  // never pave over open water
                    }
                    put(v.tx + dx, v.ty + dy, Terrain::kPath);
                }
            }

            // Houses ring the square. The count is what `tier` actually MEANS today — a hamlet has
            // four buildings and a town has a dozen, and that is legible from a distance without a
            // label, which is the whole point of putting the difficulty gradient in the map.
            const int want = 3 + 2 * v.tier;
            Rng r(seed_ ^ 0xB00C'0000ull ^ (static_cast<std::uint64_t>(v.tx) << 16) ^ v.ty);
            int placed = 0;
            bool hall = false;
            for (int attempt = 0; attempt < want * 14 && placed < want; ++attempt) {
                // A SQUARE annulus just outside the square: close enough to belong to it, far
                // enough that the square itself stays open. Rejection sampling over integer offsets
                // rather than polar cos/sin, for the reason in `block_noise` — trig is the other
                // place two compilers are free to disagree, and one disagreement here shifts a
                // house, which shifts every later rejection test in the village.
                const int span = plaza + 7;
                const int hx = v.tx - span + static_cast<int>(r.below(
                                                static_cast<std::uint32_t>(2 * span + 1)));
                const int hy = v.ty - span + static_cast<int>(r.below(
                                                static_cast<std::uint32_t>(2 * span + 1)));
                if (std::max(std::abs(hx - v.tx), std::abs(hy - v.ty)) < plaza + 1) continue;

                StructureKind kind = house_for(v.ring, r);
                if (!hall && v.tier >= 3 && placed == 0) {
                    kind = StructureKind::kHouseRed;  // the hall, once, and first
                    hall = true;
                }
                if (!try_place(hx, hy, kind)) continue;
                ++placed;
            }
            v.count = static_cast<std::uint16_t>(structures_.size() - v.first);
        }
    }

    // Architecture follows the ring, so crossing a boundary looks like crossing a border rather
    // than like the tileset changing.
    [[nodiscard]] static StructureKind house_for(Ring ring, Rng& r) noexcept {
        switch (ring) {
            case Ring::kMeadow:
            case Ring::kForest: {
                static constexpr StructureKind kSet[] = {
                    StructureKind::kHouseOrange, StructureKind::kHouseCream,
                    StructureKind::kHouseAmber, StructureKind::kHouseBlue,
                    StructureKind::kHouseTan, StructureKind::kHouseWood};
                return kSet[r.below(6)];
            }
            case Ring::kWetland: {
                static constexpr StructureKind kSet[] = {
                    StructureKind::kHouseTan, StructureKind::kHouseWood,
                    StructureKind::kHouseCream};
                return kSet[r.below(3)];
            }
            case Ring::kSnow: {
                static constexpr StructureKind kSet[] = {
                    StructureKind::kHutSnowA, StructureKind::kHutSnowB, StructureKind::kHutSnowC};
                return kSet[r.below(3)];
            }
            case Ring::kWasteland:
            case Ring::kCount: break;
        }
        static constexpr StructureKind kSet[] = {StructureKind::kRuinA, StructureKind::kRuinB};
        return kSet[r.below(2)];
    }

    // Stamp a structure if its whole footprint is free, dry land — plus a one-tile margin, so two
    // houses never end up with their walls touching and reading as one long building.
    bool try_place(int tx, int ty, StructureKind kind) {
        const StructureSize s = size_of(kind);
        for (int dy = -1; dy <= s.h; ++dy) {
            for (int dx = -1; dx <= s.w; ++dx) {
                const int x = tx + dx;
                const int y = ty + dy;
                if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return false;
                if (peek(x, y) == static_cast<std::uint8_t>(Terrain::kBuilding)) return false;
                const Terrain base = terrain_base(seed_, kOverworld, x, y);
                if (base == Terrain::kWater || base == Terrain::kTree) return false;
            }
        }
        for (int dy = 0; dy < s.h; ++dy) {
            for (int dx = 0; dx < s.w; ++dx) put(tx + dx, ty + dy, Terrain::kBuilding);
        }
        // A doorstep: the row under the front wall is paved, so a house always connects to the
        // square instead of standing in long grass.
        for (int dx = 0; dx < s.w; ++dx) {
            if (peek(tx + dx, ty + s.h) == kNoOverlay) put(tx + dx, ty + s.h, Terrain::kPath);
        }
        Structure st{};
        st.tx = static_cast<std::uint16_t>(tx);
        st.ty = static_cast<std::uint16_t>(ty);
        st.kind = kind;
        structures_.push_back(st);
        return true;
    }

    // --- 4. strongholds ------------------------------------------------------------------------
    // Density is a function of the ring, which is the whole reason the rings are worth having: it
    // means "the outer map is dangerous" needs no scripting and no per-region tuning table. A
    // candidate too near a village is dropped rather than moved — moving it is how you end up with
    // a ring of camps at exactly `kStrongholdKeepOut` from every settlement.
    void place_strongholds() {
        const int cells = kMapTiles / kStrongholdCell;
        for (int cy = 0; cy < cells; ++cy) {
            for (int cx = 0; cx < cells; ++cx) {
                Rng r(seed_ ^ 0xF0E0'0000ull ^ (static_cast<std::uint64_t>(cy) << 20) ^
                      static_cast<std::uint64_t>(cx));
                const int inset = kStrongholdCell / 6;
                const int span = kStrongholdCell - 2 * inset;
                const int tx = cx * kStrongholdCell + inset +
                               static_cast<int>(r.below(static_cast<std::uint32_t>(span)));
                const int ty = cy * kStrongholdCell + inset +
                               static_cast<int>(r.below(static_cast<std::uint32_t>(span)));
                if (tx < 12 || ty < 12 || tx >= kMapTiles - 12 || ty >= kMapTiles - 12) continue;

                const Ring ring = ring_of(seed_, tx, ty);
                if (r.below(100) >= stronghold_chance(ring)) continue;
                if (!buildable_site(tx, ty, 5)) continue;
                if (too_close_to_village(tx, ty, kStrongholdKeepOut)) continue;

                Stronghold h{};
                h.tx = static_cast<std::uint16_t>(tx);
                h.ty = static_cast<std::uint16_t>(ty);
                h.ring = ring;
                h.first = static_cast<std::uint16_t>(structures_.size());

                // Scorched ground, then two or three tents on it.
                for (int dy = -4; dy <= 4; ++dy) {
                    for (int dx = -4; dx <= 4; ++dx) {
                        if (dx * dx + dy * dy > 18) continue;
                        if (terrain_base(seed_, kOverworld, tx + dx, ty + dy) == Terrain::kWater) {
                            continue;
                        }
                        if (peek(tx + dx, ty + dy) != kNoOverlay) continue;
                        put(tx + dx, ty + dy, Terrain::kDirt);
                    }
                }
                const int tents = 2 + static_cast<int>(r.below(2));
                static constexpr StructureKind kTents[] = {
                    StructureKind::kTentA, StructureKind::kTentB, StructureKind::kTentC};
                for (int i = 0, tries = 0; i < tents && tries < 24; ++tries) {
                    const int ox = -5 + static_cast<int>(r.below(11));
                    const int oy = -5 + static_cast<int>(r.below(11));
                    if (std::max(std::abs(ox), std::abs(oy)) < 2) continue;  // keep the middle clear
                    if (try_place(tx + ox, ty + oy, kTents[r.below(3)])) ++i;
                }
                h.count = static_cast<std::uint16_t>(structures_.size() - h.first);
                if (h.count == 0) continue;  // nowhere to pitch a tent — not a stronghold
                holds_.push_back(h);
            }
        }
    }

    [[nodiscard]] static std::uint32_t stronghold_chance(Ring ring) noexcept {
        switch (ring) {
            case Ring::kMeadow: return 18;  // the chill ring keeps a couple, for flavour
            case Ring::kForest: return 45;
            case Ring::kWetland: return 70;
            case Ring::kSnow: return 88;
            case Ring::kWasteland:
            case Ring::kCount: break;
        }
        return 100;
    }

    [[nodiscard]] bool too_close_to_village(int tx, int ty, int dist) const noexcept {
        for (const Village& v : villages_) {
            if (std::abs(static_cast<int>(v.tx) - tx) < dist &&
                std::abs(static_cast<int>(v.ty) - ty) < dist) {
                return true;
            }
        }
        return false;
    }

    // --- 5. index and spawn --------------------------------------------------------------------
    void index_structures() {
        for (std::uint32_t i = 0; i < structures_.size(); ++i) {
            const Structure& s = structures_[i];
            const StructureSize sz = size_of(s.kind);
            // A sprite is anchored bottom-centre and 3 tiles tall, so its art reaches ABOVE its
            // footprint. Listing it one chunk higher too is what stops a roof vanishing when its
            // trunk chunk is off screen.
            const int cx0 = std::max(0, (s.tx - 1) / kChunkTiles);
            const int cx1 = std::min(kMapChunks - 1, (s.tx + sz.w) / kChunkTiles);
            const int cy0 = std::max(0, (s.ty - 3) / kChunkTiles);
            const int cy1 = std::min(kMapChunks - 1, (s.ty + sz.h) / kChunkTiles);
            for (int cy = cy0; cy <= cy1; ++cy) {
                for (int cx = cx0; cx <= cx1; ++cx) {
                    by_chunk_[static_cast<std::size_t>(cy) * kMapChunks + cx].push_back(i);
                }
            }
        }
    }

    // "You wake in open country and you have to find people." The spawn is deliberately 30-odd
    // tiles from the nearest village — far enough that the first thing the game asks you to do is
    // walk and look around, close enough that walking works.
    void choose_spawn() {
        const Village* home = nearest_village(kHomeTx, kHomeTy);
        if (home == nullptr) {  // a seed with no village at all: fall back to the map centre
            spawn_tx_ = kHomeTx;
            spawn_ty_ = kHomeTy;
            return;
        }
        Rng r(seed_ ^ 0x57A1'7000ull);
        for (int attempt = 0; attempt < 256; ++attempt) {
            const int ox = -42 + static_cast<int>(r.below(85));
            const int oy = -42 + static_cast<int>(r.below(85));
            const int cheb = std::max(std::abs(ox), std::abs(oy));
            if (cheb < 28 || cheb > 42) continue;  // a square annulus, in integers
            const int x = home->tx + ox;
            const int y = home->ty + oy;
            if (x < 2 || y < 2 || x >= kMapTiles - 2 || y >= kMapTiles - 2) continue;
            if (peek(x, y) != kNoOverlay) continue;  // not on a road, and not inside a house
            if (!is_walkable(ground(x, y))) continue;
            spawn_tx_ = x;
            spawn_ty_ = y;
            return;
        }
        spawn_tx_ = home->tx;
        spawn_ty_ = home->ty;
    }

    std::uint64_t seed_;
    std::vector<std::uint8_t> overlay_;
    std::vector<Village> villages_;
    std::vector<Stronghold> holds_;
    std::vector<Structure> structures_;
    std::vector<std::vector<std::uint32_t>> by_chunk_;
    int spawn_tx_ = kHomeTx;
    int spawn_ty_ = kHomeTy;
};

// The one layout this process uses.
//
// FIRST CALL WINS — the seed of the first call is the seed of the world, and later calls with a
// different seed get the same object. That is not a limitation in the game (there is one world per
// process) but it IS a trap for tools, so `mmo_worldmap` calls this before anything else.
//
// Deliberately leaked. `terrain_of` holds the overlay pointer for the life of the process, and a
// static destructor running while any thread could still be evaluating terrain is a use-after-free
// with a very confusing symptom. One allocation, freed by exit.
[[nodiscard]] inline const WorldLayout& world_layout(std::uint64_t seed) {
    static const WorldLayout* layout = [seed] {
        auto* l = new WorldLayout(seed);
        publish_overlay(l->overlay());
        return l;
    }();
    return *layout;
}

}  // namespace mmo
