// What a settlement is made of, and how one is built.
//
// This used to be forty lines inside `worldgen.hpp`, and it stopped being that the moment villages
// grew a rampart. `worldgen.hpp` answers WHERE things go — it is about spacing, rings and roads,
// and it reasons over the whole map at once. This file answers WHAT A VILLAGE IS, and it reasons
// about one settlement in isolation. Keeping them together meant one class holding both the
// map-wide Poisson grid and the pixel-level question of which column of a gate sprite is the hole.
//
// THE SHAPE COMES FROM THE ART, not the other way round. Rebuilding the asset pack author's own
// `Village.tscn` out of his scene file (tools/_study/godot/) showed a settlement enclosed by a
// palisade of huge log posts with plank wall strung between them — and it showed that this pack
// draws all four of those pieces already, which nothing in this project had noticed. So the
// generator below is built around the rhythm those four sprites can actually make:
//
//     LOG(3x5)  WALL(3x3)  LOG  WALL  ...  GATE  ...  WALL  LOG        <- north and south runs
//     LOG(3x5)                                                         <- east and west runs are
//     LOG                                                                 stacked logs, because a
//     STAKE STAKE STAKE  <- 2 rows                                        south-facing wall drawn
//     LOG                                                                 side-on reads as a wall
//                                                                         lying down.
//
// FOUR GATES, ALWAYS, and they are a pure function of (centre, tier) — `gates_of` below. That
// matters beyond the wall: roads are carved BEFORE the village is built, so unless a road can be
// told where the gates will be it arrives at the centre and the rampart is later stamped straight
// across it. A road that ends at a gate is not decoration; it is the only reason the wall can be
// continuous at all.
//
// EVERYTHING HERE IS INTEGER ARITHMETIC, for the reason spelled out over `block_noise` in
// worldgen.hpp: placement is iterative, so one disagreement between two compilers does not move one
// tile, it moves every tile decided after it.
#pragma once

#include <algorithm>
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
    // --- the rampart ---
    kLogPost = 15,  // 3x5 — the palisade post
    kRampart = 16,  // 3x3 — plank wall, strung between two posts
    kGate = 17,     // 3x3 — the same wall with an arch; the MIDDLE COLUMN of its lower two rows
                    // is transparent, measured off the sheet, and that is the hole you walk through
    kStakeA = 18,   // 1x2 — wooden stake fence
    kStakeB = 19,
    kStakeC = 20,
    kCount = 21,
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
        case StructureKind::kLogPost: return {3, 5};
        case StructureKind::kStakeA:
        case StructureKind::kStakeB:
        case StructureKind::kStakeC: return {1, 2};
        default: break;
    }
    return {3, 3};
}

// Which column of a building's footprint holds its door. MEASURED, not assumed: the doorway is the
// only run of near-black pixels in the bottom tile row of a building sprite, and scanning all
// fifteen of them for it puts the darkest 16px window at column 1 in every single case — 3-wide and
// 4-wide alike, houses, snow huts, ruins and tents. One constant, and the measurement is repeatable
// (see the note in tools/verify_structures.py).
//
// This is what a door portal needs to exist at all: nothing else in the project knows where a door
// is, because `size_of` gives a rectangle and a rectangle has no front.
inline constexpr int kDoorDx = 1;

// A placed building. `tx,ty` is its TOP-LEFT tile; the sprite is drawn over exactly (w,h) tiles
// from there, and every one of those tiles is `Terrain::kBuilding`.
struct Structure {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    StructureKind kind = StructureKind::kHouseOrange;
};

// Is this something a player can walk into? The rampart pieces are buildings to the collision
// system and scenery to everything else — no door, no interior, no villager.
[[nodiscard]] inline constexpr bool is_dwelling(StructureKind k) noexcept {
    return k < StructureKind::kLogPost;
}

// The tile you stand on to go inside: the doorway itself, which the generator leaves walkable under
// the sprite. Only meaningful for a dwelling.
[[nodiscard]] inline constexpr int door_tx(const Structure& s) noexcept {
    return static_cast<int>(s.tx) + kDoorDx;
}
[[nodiscard]] inline constexpr int door_ty(const Structure& s) noexcept {
    return static_cast<int>(s.ty) + size_of(s.kind).h - 1;
}

// --- The enclosure ----------------------------------------------------------------------------
// Half-extents of the rampart rectangle, and the interior plan, both by tier. These are a TABLE and
// not a formula because the two of them have to agree exactly: the wall runs are built out of
// fixed-size sprites, so a half-extent that is one tile off does not shrink the village by a tile —
// it leaves a hole in the wall or overlaps two posts.
//
//   hw : the run of logs and wall must divide into 3-tile slots, so (2*hw + 1) % 3 == 0.
//   hh : the side runs must divide into 5-tile log slots, so (2*hh - 7) % 5 == 0, and the slot
//        count must be ODD so that one of them is the middle and can be the gate.
//
// Both are static_assert-ed below rather than trusted.
struct VillagePlan {
    std::uint8_t hw;     // half-width of the enclosure, in tiles
    std::uint8_t hh;     // half-height
    std::uint8_t plaza;  // radius of the square in the middle
    std::uint8_t rows;   // street rows on EACH side of the square
    std::uint8_t per_side;  // houses each side of the spine, per street
    std::uint8_t houses;    // how many to try for
};

[[nodiscard]] inline constexpr VillagePlan plan_of(int tier) noexcept {
    switch (tier) {
        case 1: return {13, 11, 2, 1, 1, 4};
        case 2: return {19, 16, 2, 1, 2, 6};
        case 3: return {19, 21, 3, 2, 2, 8};
        case 4: return {25, 21, 3, 2, 3, 10};
        default: break;
    }
    return {25, 26, 4, 3, 3, 12};
}

// Distance between one village street and the next: three tiles of house plus a two-tile
// carriageway. Every house in this pack is exactly three tiles tall (`size_of`), so a row of them
// fits the gap with nothing left over and no terrace is ever half a house deep.
inline constexpr int kStreetPitch = 5;
inline constexpr int kLogW = 3;   // a log post is three tiles wide ...
inline constexpr int kLogH = 5;   // ... and five tall, which sets the pitch of the side runs
inline constexpr int kWallH = 3;  // the plank wall is three tiles tall, inset one from the post

// Where the four gates are, as the OUTSIDE approach tile of each. Pure, so `lay_roads` can aim at a
// gate long before the village exists.
//
// Order is N, S, W, E and is not incidental — `gate_at` indexes it.
enum : int { kGateN = 0, kGateS = 1, kGateW = 2, kGateE = 3, kGateCount = 4 };

struct GateSet {
    std::int16_t x[kGateCount];
    std::int16_t y[kGateCount];
};

[[nodiscard]] inline constexpr GateSet gates_of(int cx, int cy, int tier) noexcept {
    const VillagePlan p = plan_of(tier);
    GateSet g{};
    // North and south sit on the spine, in the arch of a gate sprite. The arch's hole is its middle
    // column, so the gate is centred on `cx` and the approach is two tiles beyond the wall band.
    g.x[kGateN] = static_cast<std::int16_t>(cx);
    g.y[kGateN] = static_cast<std::int16_t>(cy - p.hh - 3);
    g.x[kGateS] = static_cast<std::int16_t>(cx);
    g.y[kGateS] = static_cast<std::int16_t>(cy + p.hh + 3);
    // East and west sit on the middle street, in the one-tile slot the middle log slot leaves when
    // its stakes are placed at top and bottom — see `side_run`.
    g.x[kGateW] = static_cast<std::int16_t>(cx - p.hw - 3);
    g.y[kGateW] = static_cast<std::int16_t>(cy);
    g.x[kGateE] = static_cast<std::int16_t>(cx + p.hw + 3);
    g.y[kGateE] = static_cast<std::int16_t>(cy);
    return g;
}

// The geometry the two run builders share, asserted once here instead of being rediscovered as a
// hole in a wall on screen.
static_assert((2 * 13 + 1) % kLogW == 0 && (2 * 19 + 1) % kLogW == 0 && (2 * 25 + 1) % kLogW == 0,
              "a horizontal run must divide into whole 3-tile slots");
static_assert((2 * 13 + 1) / kLogW % 2 == 1 && (2 * 19 + 1) / kLogW % 2 == 1 &&
                  (2 * 25 + 1) / kLogW % 2 == 1,
              "the slot count must be odd, so that one slot is the middle one and can be the gate");
static_assert((2 * 11 - 7) % kLogH == 0 && (2 * 16 - 7) % kLogH == 0 && (2 * 21 - 7) % kLogH == 0 &&
                  (2 * 26 - 7) % kLogH == 0,
              "a side run must divide into whole 5-tile log slots");
static_assert((2 * 11 - 7) / kLogH % 2 == 1 && (2 * 16 - 7) / kLogH % 2 == 1 &&
                  (2 * 21 - 7) / kLogH % 2 == 1 && (2 * 26 - 7) / kLogH % 2 == 1,
              "the side-run slot count must be odd too");

// --- The builder ------------------------------------------------------------------------------
// Writes through a borrowed overlay and structure list rather than owning them, because a village
// is not a thing that exists on its own: it is a patch of the one map, and it has to be able to see
// the road that was already carved through it.
class VillageBuilder {
public:
    VillageBuilder(std::uint64_t seed, std::uint8_t* overlay, std::vector<Structure>& out) noexcept
        : seed_(seed), overlay_(overlay), out_(&out) {}

    // Builds one settlement and returns how many structures it added.
    //
    // ORDER: rampart, then square, then streets and houses. The wall first because it is the only
    // part whose position is fixed — everything inside adapts to it, and a house that cannot fit
    // simply is not built, which is the right failure. Doing it the other way round means either
    // the wall cuts a house in half or the wall has to move, and a wall that moves is a wall whose
    // gates the roads have already been aimed at.
    std::size_t build(int cx, int cy, int tier, Ring ring) {
        const std::size_t first = out_->size();
        const VillagePlan p = plan_of(tier);
        Rng r(seed_ ^ 0xB00C'0000ull ^ (static_cast<std::uint64_t>(cx) << 16) ^
              static_cast<std::uint64_t>(cy));

        rampart(cx, cy, p);
        clear_trees(cx, cy, p, ring);
        square(cx, cy, p);
        streets(cx, cy, p, ring, tier, r);
        gardens(cx, cy, p, ring, r);
        approaches(cx, cy, p);
        link_gates(cx, cy, p);
        return out_->size() - first;
    }

    // Stamp a building if its whole footprint is free, dry land — plus a one-tile margin, so two
    // houses never end up with their walls touching and reading as one long building.
    //
    // PUBLIC because strongholds are built out of the same verb. They are not villages and they get
    // no wall, but a tent is placed by exactly this rule, and having worldgen.hpp keep a second
    // copy of it is how the two would drift apart.
    bool place(int tx, int ty, StructureKind kind) {
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
        // The doorway is left WALKABLE under the sprite. That is what makes a door a door rather
        // than a painted-on rectangle: you walk into the dark arch and you are inside. The sprite
        // is opaque over that tile either way, so nothing about the picture changes.
        put(tx + kDoorDx, ty + s.h - 1, Terrain::kPath);
        // A doorstep: the row under the front wall is paved, so a house always connects to the
        // street instead of standing in long grass.
        for (int dx = 0; dx < s.w; ++dx) {
            if (peek(tx + dx, ty + s.h) == kNoOverlay) put(tx + dx, ty + s.h, Terrain::kPath);
        }
        out_->push_back(Structure{static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty),
                                  kind});
        return true;
    }

private:
    // --- overlay ------------------------------------------------------------------------------
    void put(int x, int y, Terrain t) noexcept {
        if (x < 0 || y < 0 || x >= kMapTiles || y >= kMapTiles) return;
        overlay_[static_cast<std::size_t>(y) * kMapTiles + x] = static_cast<std::uint8_t>(t);
    }
    [[nodiscard]] std::uint8_t peek(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= kMapTiles || y >= kMapTiles) return kNoOverlay;
        return overlay_[static_cast<std::size_t>(y) * kMapTiles + x];
    }
    [[nodiscard]] bool wet(int x, int y) const noexcept {
        return terrain_base(seed_, kOverworld, x, y) == Terrain::kWater;
    }

    // One paved tile, unless a building or open water is already there.
    void pave(int x, int y) {
        if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return;
        if (peek(x, y) == static_cast<std::uint8_t>(Terrain::kBuilding)) return;
        if (wet(x, y)) return;
        put(x, y, Terrain::kPath);
    }

    // --- stamping -----------------------------------------------------------------------------
    // The rampart pieces are laid EDGE TO EDGE — a post and the wall beside it share a border, and
    // a one-tile margin between them would be a one-tile hole. So they get this rather than
    // `try_place`, which exists to keep two houses from reading as one long building.
    bool stamp(int tx, int ty, StructureKind kind) {
        const StructureSize s = size_of(kind);
        for (int dy = 0; dy < s.h; ++dy) {
            for (int dx = 0; dx < s.w; ++dx) {
                const int x = tx + dx;
                const int y = ty + dy;
                if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return false;
                // Water is the one thing a wall will not cross. A village on a lake shore gets a
                // gap where the water is, and that reads as intended rather than as a bug: the
                // pack author's own village has a lake INSIDE its palisade for the same reason.
                if (wet(x, y)) return false;
            }
        }
        for (int dy = 0; dy < s.h; ++dy) {
            for (int dx = 0; dx < s.w; ++dx) put(tx + dx, ty + dy, Terrain::kBuilding);
        }
        out_->push_back(Structure{static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty),
                                  kind});
        return true;
    }

    // --- 1. the rampart -----------------------------------------------------------------------
    void rampart(int cx, int cy, const VillagePlan& p) {
        // North and south: the wall band is three tiles deep and the posts stand one tile proud of
        // it at each end, which is exactly how the pack's own village is assembled.
        wall_run(cx, cy - p.hh, p);
        wall_run(cx, cy + p.hh - (kWallH - 1), p);
        // East and west: stacked posts. A plank wall drawn side-on reads as a wall that has fallen
        // over, so the side runs use the only piece that has no front — the log.
        side_run(cx - p.hw, cy, p);
        side_run(cx + p.hw - (kLogW - 1), cy, p);
    }

    // A horizontal run, `wy` being the top row of its wall band. Slots of three tiles: posts on the
    // even ones, wall on the odd ones, and the arch in the middle. The slot count is odd (asserted
    // above), so both ends are posts and the middle slot is the gate.
    void wall_run(int cx, int wy, const VillagePlan& p) {
        const int slots = (2 * p.hw + 1) / kLogW;
        const int mid = slots / 2;
        for (int i = 0; i < slots; ++i) {
            const int x = cx - p.hw + i * kLogW;
            if (i == mid) {
                if (stamp(x, wy, StructureKind::kGate)) {
                    // Punch the arch back out. The footprint was stamped solid a line ago and this
                    // is the one place that is deliberately undone.
                    //
                    // ALL THREE ROWS, not the two the sprite draws as transparent. The arch's top
                    // row is the wall's cap and it is opaque in the art, so the first version left
                    // it solid — and a reachability flood found that twenty-three of fifty-one
                    // villages then had a gate you could see through and not walk through, which is
                    // a wall with a picture of a hole in it. What the sprite shows is the FRONT of
                    // the wall; what the middle column is, is a passage through its thickness. The
                    // player vanishes behind the cap for exactly one tile on the way through, which
                    // is the same thing that happens walking into a doorway.
                    for (int dy = 0; dy < kWallH; ++dy) put(x + 1, wy + dy, Terrain::kPath);
                }
            } else if (i % 2 == 0) {
                // A post is 3x5 and `stamp` is all-or-nothing over water, so one wet tile at the
                // edge of a pond used to drop the whole slot and leave a fifteen-tile hole in the
                // wall. Fall back to the shorter piece, then to the shortest: a post, else a wall,
                // else a rail. The reachability check counts what is left, and this is what took
                // the worst leak down from seventeen tiles to a handful.
                if (!stamp(x, wy - 1, StructureKind::kLogPost) &&
                    !stamp(x, wy, StructureKind::kRampart)) {
                    stake_band(x, wy + 1);
                }
            } else if (!stamp(x, wy, StructureKind::kRampart)) {
                stake_band(x, wy + 1);
            }
        }
    }

    // A vertical run, `wx` being its left column. Slots of five tiles — the height of a post — with
    // the middle slot given over to the gate: stakes across its top two rows and its bottom two,
    // leaving the single row between them open. That is a one-tile gateway flanked by fence and
    // capped by two enormous posts, which is the same width as the arch on the other two sides.
    void side_run(int wx, int cy, const VillagePlan& p) {
        const int top = cy - p.hh + (kLogH - 1);  // clear of the corner post on the north run
        const int slots = (2 * p.hh - 7) / kLogH;
        const int mid = slots / 2;
        for (int i = 0; i < slots; ++i) {
            const int y = top + i * kLogH;
            if (i != mid) {
                if (!stamp(wx, y, StructureKind::kLogPost)) {
                    stake_band(wx, y);      // same fallback ladder as the horizontal runs
                    stake_band(wx, y + 3);
                }
                continue;
            }
            stake_band(wx, y);
            stake_band(wx, y + 3);
            for (int dx = 0; dx < kLogW; ++dx) put(wx + dx, y + 2, Terrain::kPath);
        }
    }

    // Which of the three near-identical stakes stands here. Derived from the POSITION rather than
    // drawn from the village's Rng, and that is not a style choice: a fallback ladder means the
    // number of stakes a village places depends on where its ponds are, so a shared Rng would make
    // every later decision in the village — including which houses it gets — a function of its
    // coastline. Position-hashing decouples them.
    [[nodiscard]] StructureKind stake_kind(int x, int y) const noexcept {
        return static_cast<StructureKind>(static_cast<int>(StructureKind::kStakeA) +
                                          static_cast<int>(block_noise(0x57A4'E000ull, x, y, 1, 3)));
    }

    // Three stakes side by side, so a run does not read as one sprite stamped over and over.
    void stake_band(int x, int y) {
        for (int dx = 0; dx < kLogW; ++dx) stamp(x + dx, y, stake_kind(x + dx, y));
    }

    // --- 1b. clearing -------------------------------------------------------------------------
    // People who put a palisade up cut the wood down first. Until the wall existed this did not
    // matter — a scattering of trees among scattered houses is a village in a wood — but a rampart
    // draws a line around the settlement and says "this is the inside", and the inside being denser
    // forest than the outside reads as the generator having given up rather than as a clearing.
    //
    // ONE TREE IN TWELVE is left, and the ratio is that severe because of how trees are DRAWN, not
    // because of how many there are. A canopy sprite is four tiles wide but is anchored by a single
    // tile, and `tree_anchor` in the renderer emits one anchor per three-tile run — so a dense wood
    // of 212 tiles draws about 70 canopies, and thinning it to a quarter leaves 53 tiles that are
    // now nearly all isolated and therefore nearly all anchors. Keeping three quarters of the
    // canopies was the measured result of deleting three quarters of the tiles. Twelve is the number
    // that actually reads as a clearing with a few shade trees left standing.
    void clear_trees(int cx, int cy, const VillagePlan& p, Ring ring) {
        for (int y = cy - p.hh; y <= cy + p.hh; ++y) {
            for (int x = cx - p.hw; x <= cx + p.hw; ++x) {
                if (peek(x, y) != kNoOverlay) continue;
                if (terrain_base(seed_, kOverworld, x, y) != Terrain::kTree) continue;
                if (block_noise(0xC1EA'8000ull, x, y, 1, 12) == 0) continue;
                put(x, y, cleared_ground(ring));
            }
        }
    }

    // What is under a tree once it is gone. The ring decides, so a cleared patch in the snow is
    // snow and not a rectangle of meadow grass.
    [[nodiscard]] static constexpr Terrain cleared_ground(Ring ring) noexcept {
        switch (ring) {
            case Ring::kWetland: return Terrain::kMarsh;
            case Ring::kSnow: return Terrain::kSnow;
            case Ring::kWasteland: return Terrain::kAsh;
            case Ring::kMeadow:
            case Ring::kForest:
            case Ring::kCount: break;
        }
        return Terrain::kGrass;
    }

    // --- 2. the square ------------------------------------------------------------------------
    void square(int cx, int cy, const VillagePlan& p) {
        for (int dy = -p.plaza; dy <= p.plaza; ++dy) {
            for (int dx = -p.plaza; dx <= p.plaza; ++dx) {
                const int wob = static_cast<int>(block_noise(0x9101'ACE5ull, cx + dx, cy + dy, 3, 5)) - 2;
                const int edge = p.plaza + wob;
                if (dx * dx + dy * dy > edge * edge) continue;
                if (wet(cx + dx, cy + dy)) continue;
                if (peek(cx + dx, cy + dy) == static_cast<std::uint8_t>(Terrain::kBuilding)) continue;
                put(cx + dx, cy + dy, Terrain::kPath);
            }
        }
    }

    // --- 3. streets and houses ----------------------------------------------------------------
    void streets(int cx, int cy, const VillagePlan& p, Ring ring, int tier, Rng& r) {
        int placed = 0;
        bool hall = false;
        int street_lo = cy;
        int street_hi = cy;

        // The hall faces the SQUARE, not a street. It is the one building a village has only from
        // tier 3, so it is the thing that should be looked at on arrival — and a square with
        // nothing addressing it is just a large paved nothing.
        if (tier >= 3) {
            const StructureSize s = size_of(StructureKind::kHouseRed);
            hall = place(cx - s.w / 2, cy - p.plaza - s.h, StructureKind::kHouseRed);
            if (hall) ++placed;
        }

        for (int row = 1; row <= p.rows && placed < p.houses; ++row) {
            for (int side = 0; side < 2 && placed < p.houses; ++side) {
                const int sy = (side == 0) ? cy + p.plaza + row * kStreetPitch
                                           : cy - p.plaza - row * kStreetPitch;
                if (sy < 4 || sy >= kMapTiles - 4) continue;

                // Houses first, then the street over what is left. The other order would pave the
                // ground a house is about to stand on and `try_place` would still take it, which is
                // how you get a doorway opening into the middle of a carriageway.
                //
                // A terrace grows OUTWARD FROM THE CROSSROADS, east side then west, rather than
                // sweeping from one end. Sweeping piles every house against the western edge,
                // because the loop stops as soon as the village has its quota.
                int lo = cx;
                int hi = cx + 1;
                for (int dir = 0; dir < 2; ++dir) {
                    int on_side = 0;
                    int hx = (dir == 0) ? cx + 3 : cx - 3;
                    while (on_side < p.per_side && placed < p.houses) {
                        StructureKind kind = house_for(ring, r);
                        if (!hall && tier >= 3) {
                            kind = StructureKind::kHouseRed;  // the hall, once, and first
                            hall = true;
                        }
                        const StructureSize s = size_of(kind);
                        const int x0 = (dir == 0) ? hx : hx - s.w;
                        // Clipped to the enclosure, with a two-tile alley inside the wall so a
                        // terrace never abuts the rampart.
                        if (x0 < cx - p.hw + 4 || x0 + s.w > cx + p.hw - 3) break;
                        if (place(x0, sy - s.h, kind)) {
                            ++placed;
                            ++on_side;
                            lo = std::min(lo, x0);
                            hi = std::max(hi, x0 + s.w - 1);
                            const int gap = 1 + static_cast<int>(r.below(2));
                            hx = (dir == 0) ? x0 + s.w + gap : x0 - gap;
                        } else {
                            hx += (dir == 0) ? 2 : -2;
                        }
                    }
                }
                if (hi - lo < 2) continue;  // nothing stood up here; lay no street either
                street(lo - 2, hi + 2, sy);
                street_lo = std::min(street_lo, sy);
                street_hi = std::max(street_hi, sy + 1);
            }
        }

        // The spine, and the through road. Between them they join every street to every other and
        // all four gates to all four — which is the whole reason a wall with gates in it does not
        // simply cut the settlement into quarters.
        for (int y = std::min(street_lo, cy - p.hh); y <= std::max(street_hi, cy + p.hh); ++y) {
            pave(cx, y);
            pave(cx + 1, y);
        }
        for (int x = cx - p.hw; x <= cx + p.hw; ++x) {
            pave(x, cy);
            pave(x, cy + 1);
        }
    }

    // A village street: two tiles of carriageway from `x0` to `x1` with its north kerb at `y`.
    void street(int x0, int x1, int y) {
        for (int x = x0; x <= x1; ++x) {
            pave(x, y);
            pave(x, y + 1);
        }
    }

    // --- 4. allotments ------------------------------------------------------------------------
    // Tilled ground behind a stake rail, wherever the enclosure has room left over.
    //
    // This exists because the wall made the interior legible AND too empty: the half-height of the
    // enclosure can only move in steps of five (see the static_asserts), so a tier-3 village gets a
    // band four rows deep that the streets cannot reach, and open grass inside a palisade reads as
    // a village that has not been finished rather than as a common.
    //
    // The ground is `kDirt`, which is the one terrain a crop may be planted on. That is not a
    // flourish: it means the allotments are already the thing P3's farming needs, rather than
    // scenery that will have to be replaced by it.
    void gardens(int cx, int cy, const VillagePlan& p, Ring ring, Rng& r) {
        if (ring == Ring::kWasteland) return;  // nothing grows out here, and the ruins say so
        const int want = p.rows + 1;
        for (int i = 0, tries = 0; i < want && tries < 40; ++tries) {
            const int w = 4 + static_cast<int>(r.below(3));
            const int x = cx - p.hw + 4 + static_cast<int>(r.below(
                              static_cast<std::uint32_t>(2 * p.hw - 10 - w)));
            const int y = cy - p.hh + 5 +
                          static_cast<int>(r.below(static_cast<std::uint32_t>(2 * p.hh - 12)));
            if (!plot(x, y, w)) continue;
            ++i;
        }
    }

    // One allotment: `w` by 2 of tilled earth with a rail along its northern edge, and nothing at
    // all unless every tile of it is untouched ground. Rejecting rather than adjusting is what
    // keeps this from nudging a plot into a street — a plot that does not fit simply is not there.
    bool plot(int x, int y, int w) {
        for (int dy = 0; dy < 4; ++dy) {
            for (int dx = -1; dx <= w; ++dx) {
                if (peek(x + dx, y + dy) != kNoOverlay) return false;
                const Terrain t = terrain_base(seed_, kOverworld, x + dx, y + dy);
                if (t == Terrain::kWater || t == Terrain::kTree) return false;
            }
        }
        for (int dx = 0; dx < w; ++dx) {
            stamp(x + dx, y, stake_kind(x + dx, y));  // the rail: two tiles tall, rows y and y+1
            put(x + dx, y + 2, Terrain::kDirt);
            put(x + dx, y + 3, Terrain::kDirt);
        }
        return true;
    }

    // --- 5. the gate approaches ---------------------------------------------------------------
    // Six tiles of road out of every gate, so a road carved earlier meets something, and a stake
    // fence down each side of it. The fence is what makes a gate read as a gate from outside: two
    // posts and a hole in a wall could be damage, but a fenced approach could not.
    void approaches(int cx, int cy, const VillagePlan& p) {
        for (int d = 0; d <= 6; ++d) {
            pave(cx, cy - p.hh - d);
            pave(cx, cy + p.hh + d);
            pave(cx - p.hw - d, cy);
            pave(cx - p.hw - d, cy + 1);
            pave(cx + p.hw + d, cy);
            pave(cx + p.hw + d, cy + 1);
        }
        // Fenced from the wall outward, not starting three tiles clear of it: a rail that begins in
        // the middle of a field reads as debris rather than as an approach.
        //
        // A STAKE IS TWO TILES TALL, and the north-side rail on the east and west approaches has to
        // be placed at `cy - 2` for that reason — at `cy - 1` its lower tile lands on `cy`, which is
        // the carriageway, and the probe caught it as a gate whose approach tile was a building.
        // The rails flanking the north and south gates are one tile WIDE, so they have no such
        // trap; the asymmetry is in the sprite, not in the geometry.
        for (int d = 1; d <= 5; ++d) {
            fence_at(cx - 1, cy - p.hh - d);
            fence_at(cx + 1, cy - p.hh - d);
            fence_at(cx - 1, cy + p.hh + d);
            fence_at(cx + 1, cy + p.hh + d);
            fence_at(cx - p.hw - d, cy - 2);
            fence_at(cx - p.hw - d, cy + 2);
            fence_at(cx + p.hw + d, cy - 2);
            fence_at(cx + p.hw + d, cy + 2);
        }
    }

    // Never over anything already built or paved — and the test is over the WHOLE footprint, which
    // for a stake is the tile below as well. Checking only the anchor tile is the same mistake in a
    // second place.
    // --- 6. and last, make sure all four gates are actually usable ----------------------------
    // The spine and the through road are laid with `pave`, which refuses to touch open water — and
    // a village with a pond on one of its two axes therefore had a gate that led to a lake and a
    // road that stopped at the shore. A reachability flood over all fifty-one villages found
    // twenty-three of them with at least one gate you could not walk to from the square.
    //
    // So the two axes get one more pass that WILL cross water. This is the same causeway `carve_road`
    // lays between villages, and for the same reason: a road that stops dead reads as a bug, and a
    // gate that cannot be walked through is not a gate. It still refuses to touch a building, so it
    // cannot punch through its own wall anywhere except the openings, which are already paved.
    void link_gates(int cx, int cy, const VillagePlan& p) {
        for (int y = cy - p.hh - 6; y <= cy + p.hh + 6; ++y) causeway(cx, y);
        for (int x = cx - p.hw - 6; x <= cx + p.hw + 6; ++x) causeway(x, cy);
    }

    void causeway(int x, int y) {
        if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return;
        if (peek(x, y) == static_cast<std::uint8_t>(Terrain::kBuilding)) return;
        put(x, y, Terrain::kPath);
    }

    void fence_at(int x, int y) {
        const StructureSize s = size_of(StructureKind::kStakeA);
        for (int dy = 0; dy < s.h; ++dy) {
            if (peek(x, y + dy) != kNoOverlay) return;
        }
        stamp(x, y, stake_kind(x, y));
    }

    // --- helpers ------------------------------------------------------------------------------
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

    [[nodiscard]] std::uint32_t block_noise(std::uint64_t salt, int x, int y, int block,
                                            std::uint32_t n) const noexcept {
        const auto bx = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x / block));
        const auto by = static_cast<std::uint64_t>(static_cast<std::uint32_t>(y / block));
        Rng r(seed_ ^ salt ^ (bx * 0x9E37'79B9'7F4A'7C15ull) ^ (by * 0xC2B2'AE3D'27D4'EB4Full));
        return r.below(n);
    }

    std::uint64_t seed_;
    std::uint8_t* overlay_;
    std::vector<Structure>* out_;
};

}  // namespace mmo
