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
//                        through a house; a road ends at a GATE, whose position `gates_of` knows
//                        from (centre, tier) alone long before the village is built.
//   3. Village stamps  — rampart, square, streets, houses. See world/village.hpp; the whole of what
//                        a settlement looks like lives there now, and this file only says where.
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

#include "world/prefab_stamp.hpp"
#include "world/tiles.hpp"
#include "world/village.hpp"

namespace mmo {

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
//
// It is a COARSE reject now, not the real test. The real one is per-village and lives at the call
// site, because the enclosure's size depends on tier and tier is not known until the site has been
// accepted: a tier-5 town's wall reaches 26 tiles from its centre, a tier-1 hamlet's only 11, and
// using the larger of the two everywhere empties the wasteland of exactly the hamlets that belong
// there. 13 is the widest half-extent of a tier-1 plan, so nothing that could ever be built is
// rejected here.
inline constexpr int kVillageEdgeMargin = 13;
// Likewise: a stronghold is only "too close" relative to the wall, not the square. 34 used to be a
// comfortable gap and would now put a monster camp against the palisade of a large town.
inline constexpr int kStrongholdKeepOut = 46;
inline constexpr int kMaxRoadLength = 300;        // do not join villages further apart than this

// Forest camps are scattered on their own jittered grid, the same poor-man's-Poisson the villages
// use. The cell is much smaller than a village's because a camp is a landmark you stumble on, not a
// settlement you travel to — it wants to be COMMON in the forest ring, not spaced a walk apart. The
// cell is deliberately DENSER than the spacing floor below wants: candidates are cheap, and letting
// kCampMinGap do the thinning packs the band close to its floor instead of leaving lottery gaps
// where a sparse grid rolled empty. Tuned against `mmo_worldmap`'s per-ring tally.
inline constexpr int kCampCell = 32;
// The jitter alone lets two camps in neighbouring cells land back to back (observed 19 tiles apart —
// both clearings on one screen, which reads as a copy-paste even when their variants differ). The
// floor is set by what a screen shows: ~40 tiles wide at default zoom, so anchors at least 56 apart
// can never share one. Enforced against already-accepted camps, in scan order, so it stays a pure
// function of the seed.
inline constexpr int kCampMinGap = 56;

class WorldLayout {
public:
    explicit WorldLayout(std::uint64_t seed) : seed_(seed) {
        overlay_.assign(static_cast<std::size_t>(kMapTiles) * kMapTiles, kNoOverlay);
        by_chunk_.resize(static_cast<std::size_t>(kChunksPerMap));
        place_villages();
        lay_roads();
        build_villages();
        place_strongholds();
        place_camps();  // AFTER strongholds: a camp rejects any tile already built on, so it must be
                        // the last thing to read the overlay before the indexes are frozen.
        index_structures();
        index_prefabs();
        index_doors();
        choose_spawn();
    }

    [[nodiscard]] const std::uint8_t* overlay() const noexcept { return overlay_.data(); }
    [[nodiscard]] const std::vector<Village>& villages() const noexcept { return villages_; }
    [[nodiscard]] const std::vector<Stronghold>& strongholds() const noexcept { return holds_; }
    [[nodiscard]] const std::vector<Structure>& structures() const noexcept { return structures_; }
    [[nodiscard]] const std::vector<Door>& doors() const noexcept { return doors_; }
    [[nodiscard]] const std::vector<PlacedPrefab>& prefabs() const noexcept { return prefabs_; }

    // Prefabs whose footprint (plus the room a prop's art needs above it) touches this chunk. Mirrors
    // `structures_in_chunk`: a parcel straddling a chunk border is listed in both, so a renderer
    // iterating the chunks it draws never misses half a camp.
    [[nodiscard]] const std::vector<std::uint32_t>& prefabs_in_chunk(int cx, int cy) const noexcept {
        static const std::vector<std::uint32_t> kEmpty;
        if (cx < 0 || cy < 0 || cx >= kMapChunks || cy >= kMapChunks) return kEmpty;
        return by_prefab_chunk_[static_cast<std::size_t>(cy) * kMapChunks + cx];
    }

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

                // NOW the real border test, against this village's OWN enclosure rather than
                // against the largest one that exists. A single constant margin has to be the
                // worst case — 27 tiles, the half-height of a tier-5 town — and that is wider than
                // the usable part of the wasteland band, so it silently emptied the outer ring of
                // the hamlets that are the whole reason to go out there. Exactly the failure the
                // note over `kVillageEdgeMargin` describes, reintroduced by a bigger village.
                //
                // A wasteland village is tier 1, and tier 1 needs 12 tiles, which fits.
                const VillagePlan plan = plan_of(v.tier);
                const int mx = plan.hw + 2;
                const int my = plan.hh + 2;
                if (tx < mx || ty < my || tx >= kMapTiles - mx || ty >= kMapTiles - my) continue;
                if (!buildable_site(tx, ty, 9)) continue;
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

    // Which of a village's four gates faces a given point, as the outside approach tile. This is
    // the piece that lets a road be laid before the wall exists: `gates_of` is a pure function of
    // (centre, tier), so the road knows where the hole in the wall is going to be.
    //
    // Nearest by squared distance, not by quadrant. A quadrant test picks the "logically correct"
    // gate, which for a village almost due north of its neighbour means the north gate even when
    // the two are 200 tiles apart east-west and the east gate is plainly closer.
    static void gate_facing(const Village& v, int tx, int ty, int& gx, int& gy) noexcept {
        const GateSet g = gates_of(v.tx, v.ty, v.tier);
        int best = 0;
        long long bd = 0;
        for (int i = 0; i < kGateCount; ++i) {
            const long long dx = static_cast<long long>(g.x[i]) - tx;
            const long long dy = static_cast<long long>(g.y[i]) - ty;
            const long long d = dx * dx + dy * dy;
            if (i == 0 || d < bd) {
                best = i;
                bd = d;
            }
        }
        gx = g.x[best];
        gy = g.y[best];
    }

    // Walk from A to B one tile at a time, preferring the straight step but sidestepping water.
    //
    // A road is not a line: a perpendicular wobble from the same fbm the terrain uses makes it bend
    // with the land instead of cutting across it. When the straight step lands in water the walk
    // tries the two sideways alternatives first, and only lays a CAUSEWAY over the water when it is
    // properly boxed in — a road that simply stopped at the shore would read as a bug, and one that
    // ignored water entirely would run across the middle of a lake.
    void carve_road(const Village& a, const Village& b) {
        // Gate to gate, not centre to centre. A road aimed at the centre is later cut by the wall
        // that gets stamped on top of it, and the village ends up with a road that stops dead
        // against a palisade three tiles from where it was going.
        int x = 0;
        int y = 0;
        int gx = 0;
        int gy = 0;
        gate_facing(a, b.tx, b.ty, x, y);
        gate_facing(b, a.tx, a.ty, gx, gy);
        const int budget = 4 * (std::abs(gx - x) + std::abs(gy - y)) + 64;
        paint_road(x, y);

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

    // One paved tile, unless a building or open water is already there. `put` overwrites whatever
    // it finds, which is right for a road crossing a meadow and wrong for a street being laid
    // between houses that are already up.
    void pave(int x, int y) {
        if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return;
        if (peek(x, y) == static_cast<std::uint8_t>(Terrain::kBuilding)) return;
        if (terrain_base(seed_, kOverworld, x, y) == Terrain::kWater) return;
        put(x, y, Terrain::kPath);
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

    // --- 3. village stamps ---------------------------------------------------------------------
    // All of it — rampart, square, streets, houses — belongs to `VillageBuilder` in village.hpp.
    // This loop is the whole of what worldgen has left to say about a village: which one, where,
    // and how big.
    void build_villages() {
        VillageBuilder b(seed_, overlay_.data(), structures_);
        for (Village& v : villages_) {
            v.first = static_cast<std::uint16_t>(structures_.size());
            v.count = static_cast<std::uint16_t>(b.build(v.tx, v.ty, v.tier, v.ring));
        }
    }

    // --- 4. strongholds ------------------------------------------------------------------------
    // Density is a function of the ring, which is the whole reason the rings are worth having: it
    // means "the outer map is dangerous" needs no scripting and no per-region tuning table. A
    // candidate too near a village is dropped rather than moved — moving it is how you end up with
    // a ring of camps at exactly `kStrongholdKeepOut` from every settlement.
    void place_strongholds() {
        // A tent is stamped by the same rule a house is, so it borrows the village generator's
        // verb rather than keeping a second copy of it here. A stronghold gets no wall: the whole
        // point of a monster camp is that you can walk into it.
        VillageBuilder b(seed_, overlay_.data(), structures_);
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
                    if (b.place(tx + ox, ty + oy, kTents[r.below(3)])) ++i;
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

    // --- 4b. forest camps ----------------------------------------------------------------------
    // Hand-composed clearings dropped into the forest ring as landmarks. A camp is the first thing
    // from the pack author's own map to be placed as a whole PARCEL rather than tile by tile: the
    // floor, the tent, the campfire and the crates all come from one cut of his village, and the
    // renderer draws them from `prefabs.hpp`. What worldgen writes is only the BLOCKING — the tent's
    // footprint as `kBuilding` — because that is the one thing the simulation needs to agree with the
    // picture on. The floor is renderer-side, exactly as a house's walls are.
    //
    // Forest ONLY, and the reason is the same one that shapes every other feature: the ring is the
    // world's one difficulty axis said out loud. A camp is a forest thing — a clearing hacked out of
    // dense wood — so it belongs to the ring where `terrain_base` grows trees thickly and nowhere
    // else. Meadow copses are too sparse to hide one and the outer rings are not forest at all.
    void place_camps() {
        const PrefabDef& def = kPrefabs[static_cast<int>(PrefabId::kCampClearing)];
        const int cells = kMapTiles / kCampCell;
        for (int cy = 0; cy < cells; ++cy) {
            for (int cx = 0; cx < cells; ++cx) {
                Rng r(seed_ ^ 0xCA3B'0000ull ^ (static_cast<std::uint64_t>(cy) << 20) ^
                      static_cast<std::uint64_t>(cx));
                if (r.below(4) == 0) continue;  // drop one cell in four, so the scatter is not a grid

                // Jitter the ANCHOR (the parcel's centre) inside the cell, then place the parcel so
                // its middle sits on the anchor. Insetting keeps two camps in neighbouring cells from
                // ending up back to back.
                const int inset = kCampCell / 6;
                const int span = kCampCell - 2 * inset;
                const int ax = cx * kCampCell + inset + static_cast<int>(r.below(
                                   static_cast<std::uint32_t>(span)));
                const int ay = cy * kCampCell + inset + static_cast<int>(r.below(
                                   static_cast<std::uint32_t>(span)));
                const int tx = ax - def.w / 2;
                const int ty = ay - def.h / 2;

                // The instance's identity: a hash of (seed, anchor) and nothing that changes with the
                // scan order, so every node derives the same variant for the same camp. It drives the
                // mirror, the kept clusters and the edge feather in prefab_stamp.hpp.
                Rng vr(seed_ ^ 0xF00D'1234'0000ull ^
                       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ax)) *
                        0x9E37'79B9'7F4A'7C15ull) ^
                       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ay)) *
                        0xC2B2'AE3D'27D4'EB4Full));
                const auto variant = static_cast<std::uint32_t>(vr.next());

                if (!camp_fits(tx, ty, def)) continue;
                // Keep the whole scatter, drop only the collisions: a camp too close to one already
                // accepted is skipped, not nudged — nudging would make every later placement depend
                // on the nudge, and this loop's scan order is the only order there is.
                bool crowded = false;
                for (const PlacedPrefab& p : prefabs_) {
                    const int gx = p.tx > tx ? p.tx - tx : tx - p.tx;
                    const int gy = p.ty > ty ? p.ty - ty : ty - p.ty;
                    if (gx < kCampMinGap && gy < kCampMinGap) { crowded = true; break; }
                }
                if (crowded) continue;
                stamp_camp(tx, ty, def, variant);
                prefabs_.push_back(PlacedPrefab{tx, ty, PrefabId::kCampClearing, variant});
            }
        }
    }

    // A camp fits when every tile of its footprint PLUS a one-tile margin is inside the forest ring,
    // is not open water, and has nothing already built on it. Trees are deliberately NOT a
    // rejection — clearing wood is exactly what pitching a camp in a forest means, and `stamp_camp`
    // fells them. Water is a veto (you cannot pitch a tent on a pond) and a non-empty overlay covers
    // roads, village squares and strongholds in one test, so a camp never lands on another feature.
    [[nodiscard]] bool camp_fits(int tx, int ty, const PrefabDef& def) const noexcept {
        for (int dy = -1; dy <= def.h; ++dy) {
            for (int dx = -1; dx <= def.w; ++dx) {
                const int x = tx + dx;
                const int y = ty + dy;
                if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return false;
                if (ring_of(seed_, x, y) != Ring::kForest) return false;
                if (terrain_base(seed_, kOverworld, x, y) == Terrain::kWater) return false;
                if (peek(x, y) != kNoOverlay) return false;
            }
        }
        return true;
    }

    // Stamp one camp: fell the wood over the footprint and its margin, then write `kBuilding` on
    // every tile this instance's variant blocks. Clearing follows the village's own `clear_trees` —
    // a tree left standing inside a cleared parcel reads as the generator having given up — but a
    // camp clears the lot rather than leaving one in twelve: the parcel IS a clearing, and its floor
    // art draws over the whole thing. The ring is forest, so the ground under a felled tree is grass.
    void stamp_camp(int tx, int ty, const PrefabDef& def, std::uint32_t variant) {
        for (int dy = -1; dy <= def.h; ++dy) {
            for (int dx = -1; dx <= def.w; ++dx) {
                const int x = tx + dx;
                const int y = ty + dy;
                if (peek(x, y) != kNoOverlay) continue;
                if (terrain_base(seed_, kOverworld, x, y) != Terrain::kTree) continue;
                put(x, y, Terrain::kGrass);
            }
        }
        // Blocking only. The floor tiles, the tent and the props are drawn by the renderer straight
        // from the prefab data — writing them here would duplicate the art in two representations
        // that could drift apart, which is the trap `Terrain::kBuilding` under a house avoids.
        for (int y = 0; y < def.h; ++y) {
            for (int x = 0; x < def.w; ++x) {
                if (prefab_blocks(def, variant, x, y)) put(tx + x, ty + y, Terrain::kBuilding);
            }
        }
    }

    // Which chunks a camp touches. Expanded like `index_structures`: a prop's art reaches above its
    // anchor tile (its canopy overhangs), so the parcel is listed one chunk higher, and one tile of
    // slack on every other side covers a cell that straddles a border.
    void index_prefabs() {
        by_prefab_chunk_.resize(static_cast<std::size_t>(kChunksPerMap));
        for (std::uint32_t i = 0; i < prefabs_.size(); ++i) {
            const PlacedPrefab& pp = prefabs_[i];
            const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
            const int cx0 = std::max(0, (pp.tx - 1) / kChunkTiles);
            const int cx1 = std::min(kMapChunks - 1, (pp.tx + def.w) / kChunkTiles);
            const int cy0 = std::max(0, (pp.ty - 3) / kChunkTiles);
            const int cy1 = std::min(kMapChunks - 1, (pp.ty + def.h) / kChunkTiles);
            for (int cy = cy0; cy <= cy1; ++cy) {
                for (int cx = cx0; cx <= cx1; ++cx) {
                    by_prefab_chunk_[static_cast<std::size_t>(cy) * kMapChunks + cx].push_back(i);
                }
            }
        }
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

    // One room per dwelling, and the room index IS the index into this array. That identity is why
    // the array is sorted first and numbered second: `portal_at` searches it by tile going in, and
    // indexes it directly by room going out, and both only work if the two orders are the same one.
    //
    // The rampart pieces are skipped — `is_dwelling` is the whole test. A log post has no door, and
    // giving one a room would put four thousand empty interiors behind the walls.
    void index_doors() {
        doors_.reserve(structures_.size());
        for (const Structure& s : structures_) {
            if (!is_dwelling(s.kind)) continue;
            doors_.push_back(Door{tile_key(door_tx(s), door_ty(s)), 0});
        }
        std::sort(doors_.begin(), doors_.end(),
                  [](const Door& a, const Door& b) { return a.tile < b.tile; });
        for (std::uint32_t i = 0; i < doors_.size(); ++i) doors_[i].room = i;
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
    std::vector<Door> doors_;
    std::vector<PlacedPrefab> prefabs_;
    std::vector<std::vector<std::uint32_t>> by_chunk_;
    std::vector<std::vector<std::uint32_t>> by_prefab_chunk_;
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
        publish_doors(l->doors().data(), static_cast<int>(l->doors().size()));
        return l;
    }();
    return *layout;
}

}  // namespace mmo
