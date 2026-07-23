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

// --- table-driven POI placement ----------------------------------------------------------------
// Each landmark prefab is scattered across the world in its NATIVE ring, one row per type. The
// placement loop in the constructor walks this table in order and drops each type on its own
// jittered grid — a poor-man's Poisson-disc, same as villages and camps — so adding a landmark is a
// table row, not a new code path. The ring is the constraint that carries the meaning: a camp is a
// forest thing, a frozen pond a snow thing, an orchard a meadow thing, and putting each only where
// it belongs is the world's one difficulty axis said a second way.
//
// ORDER MATTERS in exactly one way: kCampClearing is FIRST, and that is load-bearing, not
// cosmetic. When camps are placed nothing else has been scattered yet, so their fit test and their
// spacing test see precisely what they saw when this was camp-only code — the refactor moves no
// camp. Every later type is spaced against the camps too (the gap rule below is cross-type), which
// is the point: a cottage inside a camp's clearing would be wrong.
struct PoiPlacement {
    PrefabId id;
    Ring ring;                  // the only ring this prefab may land in
    int cell;                   // jittered-grid cell = the spacing floor, near enough (see kVillageCell)
    int gap;                    // min Chebyshev gap, enforced pairwise as max(gapA,gapB) against
                                // EVERY already-placed prefab — cross-type included
    std::uint8_t present_in_4;  // keep a cell's candidate this-many-times-in-four; the rest of a
                                // type's rarity comes from its cell and its gap
    bool allow_group_drop;      // false forces every optional cluster to survive (masked into the
                                // variant at placement) — for parcels that only read right whole
    std::uint16_t base_allow;   // bit per Terrain value: the base terrains the footprint may stand
                                // on. Not a nicety — the feather exposes the base at every dropped
                                // border cell, so a parcel on the wrong base shows it (the snow
                                // pond's white floor on the ring's tan stone was the tell).
    std::uint8_t skin_mask;     // bit per allowed skin (see kSkin* below); placement picks one from
                                // the instance variant. Bit 0 (base) is always set. A row whose
                                // parcel has no skins must be exactly kSkinBase.
    std::uint8_t salt_tag;      // decorrelates the placement grid of rows that REUSE a PrefabId in a
                                // second ring (a forest camp and a snow camp are both kCampClearing,
                                // so id alone would give them one grid). 0 for the original rows, so
                                // their scatter is byte-for-byte unchanged; distinct per reuse.
};

// Skin indices match the PrefabSkin order tools/build_atlas.py emits: 0 = base (the green parcel),
// 1 = autumn (the pack author's deep-forest palette twin), 2 = snow. A skin_mask is a bit per
// allowed skin, and placement picks one of the set bits from the variant. A parcel with skin_count 1
// (every type but the two forest set-pieces) may only be placed with kSkinBase.
inline constexpr std::uint8_t kSkinBase = 1u << 0;
inline constexpr std::uint8_t kSkinAutumn = 1u << 1;
inline constexpr std::uint8_t kSkinSnow = 1u << 2;

// The footprint whitelists for the table below. Every mask includes kTree (clearing wood is what
// placing a parcel in the wild means — stamp_prefab fells it) and none includes kWater.
[[nodiscard]] inline constexpr std::uint16_t terrain_mask() noexcept { return 0; }
template <typename... T>
[[nodiscard]] inline constexpr std::uint16_t terrain_mask(Terrain t, T... rest) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<int>(t)) | terrain_mask(rest...);
}
// "Any dry ground" — what the water-only veto used to mean, for the parcels whose own floor art is
// generic enough to sit on whatever the ring offers.
inline constexpr std::uint16_t kBaseAnyDry =
    terrain_mask(Terrain::kGrass, Terrain::kDirt, Terrain::kStone, Terrain::kSand, Terrain::kTree,
                 Terrain::kSnow, Terrain::kMarsh, Terrain::kAsh);

// The one place a landmark's placement is tuned. Counts land in the ranges the design asks for by
// the interplay of cell (how many candidates), present_in_4 (how many of those are even tried) and
// gap (how many survive spacing) — measured against `mmo_worldmap`'s per-type tally.
inline constexpr PoiPlacement kPoiTable[] = {
    // kCampClearing MUST stay first (see the note above). cell 32 / gap 56 / 3-in-4 is the tuning
    // the camp-only code shipped with, kept verbatim so no camp moves. A camp is COMMON — the small
    // cell is deliberately DENSER than the gap floor wants (candidates are cheap; letting the gap do
    // the thinning packs the band near its floor rather than leaving lottery holes a sparse grid
    // rolled empty). The gap of 56 is set by what one screen shows (~40 tiles at default zoom, plus
    // margin), so two camps can never share it and read as a copy-paste.
    // A forest camp comes out summer-green OR deep-wood at random (base|autumn) — the exact variety
    // the variant system exists for, now reaching the palette as well as the mirror and the props.
    {PrefabId::kCampClearing, Ring::kForest, 32, 56, 3, true, kBaseAnyDry,
     kSkinBase | kSkinAutumn, 0},
    // A lone dwelling you remember stumbling on. Its rarity is EARNED, not dialled: the forest ring
    // is already blanketed by camps at gap 56, so a cottage only lands in the rare pocket no camp
    // claimed — the gap is set just above a camp's own spacing (max(60,56)=60), enough that a cottage
    // sits in its own clearing rather than sharing a camp's, and the dense camps do the thinning down
    // to a handful. A wider gap does not read as "rarer", it reads as "none": there is no 100-tile
    // camp-free pocket in that ring to find. Green or deep-wood, like the camps.
    {PrefabId::kForestCottage, Ring::kForest, 40, 60, 4, true, kBaseAnyDry,
     kSkinBase | kSkinAutumn, 0},
    // Frozen ponds to break up the snowfield. The pond is FLOOR art over walkable ice — the stamp
    // writes no kWater under it, so you skate across (deliberate; see prefab_fits and the note
    // there). Snowfield ONLY: the snow ring's base is a mix of snow and tan stone, and a white
    // parcel feathered over stone reads as a paper cut-out — the pond must sit where its own edge
    // colour IS the ground colour.
    // Cell 48, denser than the pond's own gap wants, for the camp reason: the snow-only base rule
    // rejects most candidates (the ring is half tan stone), so the grid over-generates and the base
    // test plus the gap do the thinning.
    {PrefabId::kSnowPond, Ring::kSnow, 48, 80, 4, true,
     terrain_mask(Terrain::kSnow, Terrain::kTree), kSkinBase, 0},
    // Orchard rows to interrupt the open meadow grass. Grass only, for the same reason the pond
    // wants snow: the meadow's ponds ring themselves with sand, and an orchard feathered into a
    // beach shows tan holes between its trees.
    {PrefabId::kSouthOrchard, Ring::kMeadow, 72, 80, 4, true,
     terrain_mask(Terrain::kGrass, Terrain::kTree), kSkinBase, 0},

    // The SAME two forest set-pieces, now in the snow ring wearing their snow skin. They come AFTER
    // every forest and meadow row on purpose: the placement loop walks the table in order, so a row
    // added here spaces itself against the snow ponds already laid (the cross-type gap rule) while
    // moving no camp, cottage, pond or orchard placed before it. They reuse kCampClearing /
    // kForestCottage — the parcel IS the same, only its palette differs — so each carries a distinct
    // salt_tag to keep its grid from correlating with its forest twin's.
    //
    // base_allow is snow|tree like the pond, for the same reason: a snow-skinned parcel's white
    // field only reads as part of the world where the ground its ragged edge exposes is snow, not
    // the ring's tan stone. A snow camp is a rare thing (a handful) — the cell is large and the gap,
    // spaced against the wide ponds, thins it further; a snow cottage is rarer still.
    {PrefabId::kCampClearing, Ring::kSnow, 40, 64, 3, true,
     terrain_mask(Terrain::kSnow, Terrain::kTree), kSkinSnow, 1},
    {PrefabId::kForestCottage, Ring::kSnow, 44, 72, 2, true,
     terrain_mask(Terrain::kSnow, Terrain::kTree), kSkinSnow, 2},

    // DEFERRED — deliberately not in the table, each its own task:
    //   kLakeIslands       its floor art contains OPEN WATER; stamping it needs overlay kWater
    //                      writes so the sim agrees with the picture. That is a design task, not a row.
    //   kWaterfallBridge   the world has no rivers for it to span.
    //   kFortGate,
    //   kFortCourtyard     stronghold-site integration, not free scatter.
    //   kStreetHouses,
    //   kMarketYard,
    //   kStairsPlaza,
    //   kNorthTreelineWell village pieces, laid by the village builder, not scattered as landmarks.
};

// A prefab type's spacing gap, by id — so the cross-type spacing test can ask "how far must I keep
// from THAT one?" and `mmo_worldmap`'s probe can check the pairwise rule from outside. Zero for a
// type not in the table (it is never placed, so it is never asked).
[[nodiscard]] inline constexpr int poi_gap(PrefabId id) noexcept {
    for (const PoiPlacement& p : kPoiTable) {
        if (p.id == id) return p.gap;
    }
    return 0;
}

class WorldLayout {
public:
    explicit WorldLayout(std::uint64_t seed) : seed_(seed) {
        overlay_.assign(static_cast<std::size_t>(kMapTiles) * kMapTiles, kNoOverlay);
        by_chunk_.resize(static_cast<std::size_t>(kChunksPerMap));
        place_villages();
        lay_roads();
        build_villages();
        place_strongholds();
        place_prefabs();  // AFTER strongholds: a prefab rejects any tile already built on, so it must
                          // be the last thing to read the overlay before the indexes are frozen.
        // The village-laid blocks join the scattered ones as one list from here on, so the renderer
        // draws all of them through the single prefab path and the worldmap boxes all of them. This
        // merge happens AFTER place_prefabs, never before: a POI is spaced against every prefab
        // already placed, and letting it space against a village's furniture too would move camps the
        // `kPoiTable` note insists never move. `poi_count_` remembers the split for the tally.
        poi_count_ = prefabs_.size();
        prefabs_.insert(prefabs_.end(), vparcels_.begin(), vparcels_.end());
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
    // Interior room indices that hold a dojo boss (F3), one per DOJO dwelling cell a village's
    // street_houses parcel laid. Every entry is a room a tier>=3 village's dojo door leads into, so
    // it is where a ChunkActor plants its Giant Red Samurai at bring-up. Sorted, deduplicated.
    [[nodiscard]] const std::vector<std::uint32_t>& dojo_rooms() const noexcept { return dojo_rooms_; }
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
        VillageBuilder b(seed_, overlay_.data(), structures_, vparcels_);
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
        VillageBuilder b(seed_, overlay_.data(), structures_, vparcels_);
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

    // --- 4b. landmark prefabs ------------------------------------------------------------------
    // Hand-composed parcels dropped across the world as landmarks, one type per row of `kPoiTable`
    // above, each in its native ring. A parcel is placed as a WHOLE unit — the floor, the tent or
    // house, the props all come from one cut of the pack author's own map, and the renderer draws
    // them from `prefabs.hpp`. What worldgen writes is only the BLOCKING — a structure's footprint
    // as `kBuilding` — because that is the one thing the simulation needs to agree with the picture
    // on. The floor is renderer-side, exactly as a house's walls are.
    //
    // The camp was the first of these; this loop is the same code generalized over the table, so
    // that a second landmark (a lone forest cottage, a frozen snow pond, a meadow orchard) is a row
    // and not a copy of this function. Camps are the first row on purpose — see the note on
    // `kPoiTable` for why that keeps them byte-identical to the camp-only code.
    void place_prefabs() {
        for (const PoiPlacement& row : kPoiTable) {
            const PrefabDef& def = kPrefabs[static_cast<int>(row.id)];
            const int cells = kMapTiles / row.cell;
            // Per-type salt so two types' grids do not correlate. kCampClearing is id 0 with
            // salt_tag 0, so its salt stays exactly 0xCA3B0000 — the value the camp-only code used —
            // and its scatter does not move. Later ids differ in the id byte; a row that reuses an id
            // in a second ring (the snow camp/cottage) differs in salt_tag, folded into bits the
            // cell/cx/cy hash below never touches, so its grid is its own rather than a copy of its
            // forest twin's.
            const std::uint64_t salt =
                0xCA3B'0000ull ^ (static_cast<std::uint64_t>(static_cast<std::uint8_t>(row.id)) << 8)
                ^ (static_cast<std::uint64_t>(row.salt_tag) << 40);
            const std::uint32_t drop = 4u - row.present_in_4;
            for (int cy = 0; cy < cells; ++cy) {
                for (int cx = 0; cx < cells; ++cx) {
                    Rng r(seed_ ^ salt ^ (static_cast<std::uint64_t>(cy) << 20) ^
                          static_cast<std::uint64_t>(cx));
                    // Presence roll is drawn FIRST and always, whatever the threshold, so the anchor
                    // draws that follow read the same stream position for every type — the camp's
                    // `r.below(4) == 0` is exactly this with present_in_4 = 3 (drop = 1).
                    if (r.below(4) < drop) continue;

                    // Jitter the ANCHOR (the parcel's centre) inside the cell, then place the parcel
                    // so its middle sits on the anchor. Insetting keeps two parcels in neighbouring
                    // cells from ending up back to back.
                    const int inset = row.cell / 6;
                    const int span = row.cell - 2 * inset;
                    const int ax = cx * row.cell + inset +
                                   static_cast<int>(r.below(static_cast<std::uint32_t>(span)));
                    const int ay = cy * row.cell + inset +
                                   static_cast<int>(r.below(static_cast<std::uint32_t>(span)));
                    const int tx = ax - def.w / 2;
                    const int ty = ay - def.h / 2;

                    // The instance's identity: a hash of (seed, anchor) and nothing that changes with
                    // the scan order, so every node derives the same variant for the same parcel. It
                    // drives the mirror, the kept clusters and the edge feather in prefab_stamp.hpp.
                    Rng vr(seed_ ^ 0xF00D'1234'0000ull ^
                           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ax)) *
                            0x9E37'79B9'7F4A'7C15ull) ^
                           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ay)) *
                            0xC2B2'AE3D'27D4'EB4Full));
                    std::uint32_t variant = static_cast<std::uint32_t>(vr.next());
                    // A parcel that only reads right whole keeps all its clusters — masked into the
                    // variant here, so the stored value is the whole of the shared state and the
                    // renderer needs no copy of the policy. A no-op when the type allows drops.
                    if (!row.allow_group_drop) variant = prefab_force_groups(def, variant);
                    // The skin: chosen from the variant AFTER any group masking, from bits the
                    // mirror/groups/feather do not consume (prefab_pick_skin). It reads the variant
                    // and does not touch it, so it changes no anchor and no variant — placement is
                    // decided entirely by what follows, exactly as before skins existed.
                    const std::uint8_t skin = prefab_pick_skin(variant, row.skin_mask);

                    if (!prefab_fits(tx, ty, def, row.ring, row.base_allow)) continue;
                    // Keep the whole scatter, drop only the collisions: a parcel too close to one
                    // already accepted is skipped, not nudged — nudging would make every later
                    // placement depend on the nudge, and this loop's scan order is the only order
                    // there is. The spacing is CROSS-TYPE: the required gap is the larger of the two
                    // types' gaps, so a small camp still keeps a wide cottage's distance and no pair
                    // of any types ends up on one screen.
                    bool crowded = false;
                    for (const PlacedPrefab& p : prefabs_) {
                        const int gx = p.tx > tx ? p.tx - tx : tx - p.tx;
                        const int gy = p.ty > ty ? p.ty - ty : ty - p.ty;
                        const int need = std::max(row.gap, poi_gap(p.id));
                        if (gx < need && gy < need) { crowded = true; break; }
                    }
                    if (crowded) continue;
                    stamp_prefab(tx, ty, def, variant, skin);
                    prefabs_.push_back(PlacedPrefab{tx, ty, row.id, variant, skin});
                }
            }
        }
    }

    // A parcel fits when every tile of its footprint PLUS a one-tile margin is inside its native
    // ring, stands on a base terrain its table row allows, and has nothing already built on it.
    // Trees are in every row's whitelist — clearing wood is exactly what pitching a camp in a forest
    // means, and `stamp_prefab` fells them — and water is in nobody's: you cannot pitch a tent, or
    // lay out an orchard, on a pond. (The snow pond's frozen water is FLOOR ART over walkable ice,
    // never a `kWater` tile, so its own footprint does not trip the rule.) The whitelist exists
    // because the feather exposes the base at every dropped border cell: a parcel only reads as part
    // of the world where the ground showing through its ragged edge is the ground its edge was drawn
    // against. A non-empty overlay covers roads, village squares and strongholds in one test, so a
    // parcel never lands on another feature.
    [[nodiscard]] bool prefab_fits(int tx, int ty, const PrefabDef& def, Ring ring,
                                   std::uint16_t base_allow) const noexcept {
        for (int dy = -1; dy <= def.h; ++dy) {
            for (int dx = -1; dx <= def.w; ++dx) {
                const int x = tx + dx;
                const int y = ty + dy;
                if (x < 1 || y < 1 || x >= kMapTiles - 1 || y >= kMapTiles - 1) return false;
                if (ring_of(seed_, x, y) != ring) return false;
                const auto base = terrain_base(seed_, kOverworld, x, y);
                if ((base_allow & (1u << static_cast<int>(base))) == 0) return false;
                if (peek(x, y) != kNoOverlay) return false;
            }
        }
        return true;
    }

    // Stamp one parcel: fell the wood over the footprint and its margin, then write `kBuilding` on
    // every tile this instance's variant blocks. Clearing follows the village's own `clear_trees` —
    // a tree left standing inside a cleared parcel reads as the generator having given up — but a
    // parcel clears the lot rather than leaving one in twelve: the parcel IS a cleared space, and its
    // floor art draws over the whole thing. In snow and meadow there is little or no wood under a
    // parcel, so the fell is a near no-op there; the ground it exposes is the ring's own grass.
    void stamp_prefab(int tx, int ty, const PrefabDef& def, std::uint32_t variant,
                      std::uint8_t skin) {
        // Blocking is skin-invariant (prefab_blocks scans only the layer-2 structures a skin never
        // touches), so passing the instance's own skin here writes exactly the footprint the
        // renderer will draw around — the two cannot disagree.
        const PrefabSkin& sk = prefab_skin_of(def, skin);
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
                if (prefab_blocks(def, sk, variant, x, y)) put(tx + x, ty + y, Terrain::kBuilding);
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
        doors_.reserve(structures_.size() + vparcels_.size());
        for (const Structure& s : structures_) {
            if (!is_dwelling(s.kind)) continue;
            doors_.push_back(Door{tile_key(door_tx(s), door_ty(s)), 0});
        }
        // A house a village stamped as part of a prefab block is a dwelling too, and "every house has
        // a door" would regress the moment one of these had none. Its door is the same construction a
        // Structure's is — the doorway tile the builder left walkable under the sprite — derived here
        // from the parcel's kept dwelling cells so the room is allocated by the very same sort below.
        // The DOJO doors, remembered by tile so the boss rooms can be picked out AFTER the sort below
        // renumbers every door into its room. A dojo cell is tagged at pack time (PrefabCell::dojo,
        // the red-temple sx=192 crop), so identifying it here is one flag read — the source rect the
        // packer measured is not carried into the engine, which is exactly why the flag exists.
        std::vector<std::uint32_t> dojo_tiles;
        for (const PlacedPrefab& pp : vparcels_) {
            const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
            const PrefabSkin& sk = prefab_skin_of(def, pp.skin);
            for (std::uint16_t i = 0; i < sk.cell_count; ++i) {
                const PrefabCell& c = sk.cells[i];
                if (!prefab_cell_is_dwelling(c)) continue;
                if (!prefab_cell_visible(def, sk, c, pp.variant)) continue;
                const std::uint32_t key = tile_key(pp.tx + prefab_door_dx(c), pp.ty + prefab_door_dy(c));
                doors_.push_back(Door{key, 0});
                if (c.dojo) dojo_tiles.push_back(key);
            }
        }
        std::sort(doors_.begin(), doors_.end(),
                  [](const Door& a, const Door& b) { return a.tile < b.tile; });
        for (std::uint32_t i = 0; i < doors_.size(); ++i) doors_[i].room = i;
        // Now the room a dojo door leads to is known (room == sorted index). Collect them.
        std::sort(dojo_tiles.begin(), dojo_tiles.end());
        for (const Door& d : doors_) {
            if (std::binary_search(dojo_tiles.begin(), dojo_tiles.end(), d.tile)) {
                dojo_rooms_.push_back(d.room);
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
    std::vector<Door> doors_;
    std::vector<std::uint32_t> dojo_rooms_;  // interior rooms that hold a boss (F3)
    std::vector<PlacedPrefab> prefabs_;
    std::vector<PlacedPrefab> vparcels_;  // the blocks villages lay as furniture, merged into
                                          // prefabs_ AFTER place_prefabs so the POI scatter — camps
                                          // especially — is spaced against exactly what it was before
    std::size_t poi_count_ = 0;           // prefabs_ entries that are scattered POIs, not village
                                          // parcels; the split the worldmap tally reads
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
