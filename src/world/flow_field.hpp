// A distance-to-the-nearest-settlement field, and the reason it does not break the actor model.
//
// THE PROBLEM. Mobs steer greedily toward the core and slide along obstacles when blocked. That is
// fine on open ground and fails completely against a concave shape: once coherent terrain gave the
// map real lakes, a traced mob walked 45 tiles, entered a bay, and then sat at exactly the same
// position for the rest of the run — every direction it wanted to go was water, and a greedy
// steerer has no way to go *backwards* to get around. Measured, not theorised: see `mmo_probe`.
//
// THE FIX. Every mob is heading for *a* settlement, so instead of N searches there is one
// breadth-first sweep outward from all of them at once over walkable tiles. A mob then just steps
// to whichever neighbour is closer. No local minima, no per-mob search, and lakes get walked around.
//
// WHY THIS IS NOT SHARED MUTABLE STATE. The field is a pure function of (world seed, map, target
// list) — all inputs every node already has, because terrain and the world layout are both derived
// from the seed alone.
// Any node can compute a byte-identical field on its own, and nothing ever writes to it after
// construction. It is shared the way a lookup table is shared, not the way state is shared: chunks
// hold a `const FlowField*`, there is no message, no lock, and no coherence problem when chunks
// later live on different machines. Buildings deliberately do NOT affect it — walls are meant to be
// attacked, not routed around.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "world/tiles.hpp"

namespace mmo {

class FlowField {
public:
    static constexpr std::uint16_t kUnreachable = 0xFFFF;

    // BFS outward from EVERY target at once, over walkable terrain. 8-connected so diagonal travel
    // costs the same as orthogonal — mobs then take visually straight lines across open ground
    // instead of staircases.
    //
    // MULTI-SOURCE is what makes one field enough now that there are fifty villages instead of one
    // core. Seeding the frontier with all of them at distance 0 gives, in a single sweep, the
    // distance to the NEAREST target from every tile — so a mob descending it walks to whichever
    // village is closest to it, with no per-mob search and no field per village. One BFS costs the
    // same as it did with one source; fifty separate fields would cost fifty times as much and
    // 100 MB of memory.
    void build(std::uint64_t world_seed, std::uint16_t map,
               const std::vector<std::pair<int, int>>& targets) {
        map_ = map;
        dist_.assign(static_cast<std::size_t>(kMapTiles) * kMapTiles, kUnreachable);

        std::vector<int> frontier;
        std::vector<int> next;
        frontier.reserve(4096);
        next.reserve(4096);

        for (const auto& [tx, ty] : targets) {
            if (tx < 0 || ty < 0 || tx >= kMapTiles || ty >= kMapTiles) continue;
            const int start = ty * kMapTiles + tx;
            if (dist_[static_cast<std::size_t>(start)] == 0) continue;  // duplicate target
            dist_[static_cast<std::size_t>(start)] = 0;
            frontier.push_back(start);
        }
        if (frontier.empty()) return;

        std::uint16_t d = 0;
        while (!frontier.empty()) {
            ++d;
            next.clear();
            for (int p : frontier) {
                const int x = p % kMapTiles;
                const int y = p / kMapTiles;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= kMapTiles || ny >= kMapTiles) continue;
                        const auto q = static_cast<std::size_t>(ny) * kMapTiles + nx;
                        if (dist_[q] != kUnreachable) continue;
                        if (!is_walkable(terrain_of(world_seed, map, nx, ny))) continue;
                        dist_[q] = d;
                        next.push_back(static_cast<int>(q));
                    }
                }
            }
            frontier.swap(next);
        }
    }

    [[nodiscard]] std::uint16_t at(int tx, int ty) const noexcept {
        if (tx < 0 || ty < 0 || tx >= kMapTiles || ty >= kMapTiles) return kUnreachable;
        return dist_[static_cast<std::size_t>(ty) * kMapTiles + tx];
    }

    [[nodiscard]] bool ready() const noexcept { return !dist_.empty(); }
    [[nodiscard]] std::uint16_t map() const noexcept { return map_; }

    // The downhill step from a tile: the neighbouring tile with the smallest distance. Returns
    // false when this tile is unreachable or already the target, in which case the caller keeps its
    // own heading.
    [[nodiscard]] bool descend(int tx, int ty, int& out_dx, int& out_dy) const noexcept {
        const std::uint16_t here = at(tx, ty);
        if (here == kUnreachable || here == 0) return false;
        std::uint16_t best = here;
        bool found = false;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const std::uint16_t n = at(tx + dx, ty + dy);
                if (n >= best) continue;
                best = n;
                out_dx = dx;
                out_dy = dy;
                found = true;
            }
        }
        return found;
    }

    [[nodiscard]] std::size_t reachable_count() const noexcept {
        std::size_t n = 0;
        for (std::uint16_t v : dist_) n += (v != kUnreachable) ? 1 : 0;
        return n;
    }

private:
    std::uint16_t map_ = 0;
    std::vector<std::uint16_t> dist_;
};

}  // namespace mmo
