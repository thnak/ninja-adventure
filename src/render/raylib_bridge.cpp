#include "render/raylib_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "raylib.h"
#include "render/atlas_slots.hpp"
#include "render/ui_sprites.hpp"

namespace mmo {
namespace {

// The one texture in the process, so the UI shell can borrow it for a portrait without loading a
// second copy (see render/ui_sprites.hpp). Set by the bridge's constructor, cleared by its
// destructor; there is exactly one bridge.
Texture2D g_atlas{};
bool g_atlas_ok = false;


// --- game concept -> atlas slot -------------------------------------------------------------------
// The only place game concepts meet sprites. WHICH pixels a slot points at is decided by the
// manifest in tools/build_atlas.py, not here — so re-arting the game never touches this file.

// What a tree stands on, per ring — so a wood in the snow ring is rooted in snow rather than
// transplanted onto a lawn.
[[nodiscard]] Terrain ground_under_tree(Ring r) {
    switch (r) {
        case Ring::kMeadow:
        case Ring::kForest: return Terrain::kGrass;
        case Ring::kWetland: return Terrain::kMarsh;
        case Ring::kSnow: return Terrain::kSnow;
        case Ring::kWasteland:
        case Ring::kCount: break;
    }
    return Terrain::kAsh;
}

// The FIRST of a terrain's variant run. Variants are packed consecutively (kTerrainGrass,
// kTerrainGrass1, kTerrainGrass2), so `slot_of(t) + n` selects one.
[[nodiscard]] Slot slot_of(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return Slot::kTerrainGrass;
        case Terrain::kDirt: return Slot::kTerrainDirt;
        case Terrain::kWater: return Slot::kTerrainWater;
        case Terrain::kStone: return Slot::kTerrainStone;
        case Terrain::kSand: return Slot::kTerrainSand;
        case Terrain::kSnow: return Slot::kTerrainSnow;
        case Terrain::kMarsh: return Slot::kTerrainMarsh;
        case Terrain::kAsh: return Slot::kTerrainAsh;
        case Terrain::kPath: return Slot::kTerrainPath;
        // A building's footprint is drawn as trodden ground and then covered by the building's own
        // multi-tile sprite — the same trick trees use, and the reason no wall tile is needed.
        case Terrain::kBuilding: return Slot::kTerrainBuilding;
        // A tree is an OVERLAY: the base layer under it is whatever the surrounding ring uses, so
        // a forest in the snow ring stands on snow rather than on grass.
        case Terrain::kTree: return Slot::kTerrainGrass;
        case Terrain::kCount: break;
    }
    return Slot::kTerrainGrass;
}

// --- Terrain edges -----------------------------------------------------------------------------
// WHICH OF TWO TERRAINS DRAWS THE BOUNDARY. The higher one lays its own blob tile over a fill of the
// lower one, so this order is the whole per-pair table: 11 numbers instead of 55 hand-authored sets.
//
// WATER IS AT THE TOP, and it used to be at the bottom. The old order read as "what sits under
// what" and put water lowest, on the reasoning that a bank overhangs a shore. That is a fine
// description of a shore and it is not how the art is drawn: `TilesetWater#18` is a WATER blob, and
// the brown bank and white foam are painted on the water tile, not on the grass one. With water
// lowest it was never the terrain doing the drawing, so its shoreline was the only thing on screen
// still stepping tile by tile while the generated boundaries beside it wandered — visible in
// shot_ring0 before this line changed. The rule now matches the art: read it as "what is cut INTO
// what". Water is cut into land; a road is cut into whatever it crosses.
[[nodiscard]] int terrain_priority(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return 0;
        case Terrain::kTree: return 0;  // a tree stands ON ground; see tile_terrain
        case Terrain::kSand: return 1;
        case Terrain::kAsh: return 2;
        case Terrain::kStone: return 3;
        case Terrain::kMarsh: return 4;
        case Terrain::kSnow: return 5;
        case Terrain::kDirt: return 6;
        // Water sits above the road, not below it. A village square stamped around a pond used to
        // cut it into a rectangle — the square was the higher terrain, so it drew the boundary with
        // a generated contour and the pond's own bank and foam were never used. Putting water on top
        // means the pond keeps its shoreline and the road simply stops at it, which is also what a
        // road does.
        case Terrain::kPath: return 7;
        case Terrain::kWater: return 8;
        case Terrain::kBuilding: return 9;
        case Terrain::kCount: break;
    }
    return 0;
}


// The terrain one TILE is drawn as, which is not always the terrain the simulation reports.
//
// SAMPLED PER TILE, NOT PER CORNER, and that is a reversal from the first version of this code. The
// corner lattice was a way to make a boundary cross a tile's interior when every tile could only be
// one flat colour. It is unnecessary once the edge art is a nine-bit blob set: the pack's own tiles
// already carry the rounded corner and the overhanging bank, and they line up with their neighbours
// because the author drew them to. Asking a blob set for a corner mask it was never drawn for is
// precisely the failure RENDER_SPEC.md §3.1 warns about, so the mask has to be built the way the art
// expects — from the eight neighbours, with Godot's minimal corner rule.
//
// The two rewrites below are unchanged from the corner version, because they were never about
// corners:
[[nodiscard]] Terrain tile_terrain(std::uint16_t map, int gx, int gy) {
    const Terrain o = terrain_of(kWorldSeed, map, gx, gy);

    // Indoors, `kBuilding` is a WALL and not a footprint, so it keeps its own identity — see
    // `ground`, which then declines to draw it at all. Remapping it to path here is what would
    // otherwise carpet the whole interior map in road, room and void alike.
    if (map != kOverworld) return o;

    // A footprint and the square around it are the same trodden earth and share art, so they are one
    // terrain as far as the boundary is concerned. Keeping them separate put a seam between every
    // house and the ground it stands on.
    if (o == Terrain::kPath || o == Terrain::kBuilding) return Terrain::kPath;

    // NOTHING ERODES THE RIM OF A PLACED REGION, and that was tried and removed rather than tuned.
    // Roads and village squares are stamped as rectangles by world generation, and the worry was
    // that a blob set rounds a rectangle into a rounded rectangle and no further — `shot_village`
    // did measure 0.265 under the corner lattice and 0.462 immediately after the rewrite. So the rim
    // was eroded with a noise field to give the art something to be ragged about.
    //
    // It was the wrong fix for both halves of that number. The rectangular POND was the priority
    // order (see `terrain_priority`); the ruled ROAD EDGES were the generated art displacing the
    // field value instead of the sample point (see `edge_generated` in the packer). With those two
    // corrected the erosion bought nothing the metric could see and cost a great deal: a road here
    // is two tiles wide, so every tile of it is a rim tile, and a field eroding both sides at once
    // ate it into a dashed line of crumbs. Guarding on thickness did not save it — the roads run
    // diagonally, so their axis-aligned cross-section is wider than the road is.

    // A tree is not a terrain you can stand on — it is something standing on one. Resolving it to
    // its ring's ground here is what lets a forest floor take part in transitions at all.
    return (o == Terrain::kTree) ? ground_under_tree(ring_of(kWorldSeed, gx, gy)) : o;
}

// --- The Y-sorted draw list -------------------------------------------------------------------
// Everything that STANDS on the ground, in one list. Terrain is not here — it is flat, so nothing
// can be in front of it — and neither are combat flashes, which are meant to be read over the top
// of whatever they hit.
enum class SpriteKind : std::uint8_t {
    kCrop,
    kBuilding,
    kCreature,
    kShot,
    kBig,
    kPlayer,
    // A single cell of a placed prefab (a forest camp's tent, crate or campfire). Unlike kBig it
    // carries no enum id into the atlas — it points straight at the atlas rect its PrefabCell names,
    // because prefab art is addressed by pixel rect, not by a game-concept slot.
    kPrefabCell,
};

struct Sprite {
    float sort;     // world-pixel Y of this sprite's FEET; the sort key, see the sorted pass
    float x, y;     // world-pixel draw position
    const void* p;  // the source record for the kinds that have one, else null
    std::uint16_t a;      // Big id, player slot, or (for kPrefabCell) the horizontal-flip flag
    std::uint16_t frame;  // animation frame, for creatures
    SpriteKind kind;
};

// The world-pixel rectangle one prefab cell occupies, with its mirror already resolved into the
// position — `flip` says only whether to draw the SOURCE flipped. A prefab is authored at the atlas'
// 16px resolution and scaled to the game's 32px tile, so `scale` below is always 2.
struct PrefabQuad {
    float x, y, w, h;
};

[[nodiscard]] inline PrefabQuad prefab_quad(const PrefabDef& def, const PlacedPrefab& pp,
                                            const PrefabCell& c, bool mir) {
    const int scale = kTilePx / kAtlasTile;  // 2: a 16px atlas tile fills one 32px game tile
    // Godot origin=1 (centred) draws a cell bigger than one tile centred on that tile; origin<>1
    // draws from the tile's top-left. This mirrors tools/build_atlas.py's own prefab_proof.
    const int offx = c.centred ? (8 - c.pw / 2) : 0;
    const int offy = c.centred ? (8 - c.ph / 2) : 0;
    const int local_x = c.dx * kAtlasTile + offx;  // atlas px from the parcel's left edge
    const int local_y = c.dy * kAtlasTile + offy;
    const float w = static_cast<float>(c.pw * scale);
    const float h = static_cast<float>(c.ph * scale);
    const float parcel_left = static_cast<float>(pp.tx * kTilePx);
    const float top = static_cast<float>(pp.ty * kTilePx) + static_cast<float>(local_y * scale);
    // Mirrored: reflect the cell about the parcel's horizontal centre. The parcel is def.w tiles
    // wide, so its atlas width is def.w*16 and the cell's mirrored left edge is (that - local_x - pw).
    const float left = mir ? parcel_left + static_cast<float>(
                                 (def.w * kAtlasTile - local_x - c.pw) * scale)
                           : parcel_left + static_cast<float>(local_x * scale);
    return PrefabQuad{left, top, w, h};
}

// A generated structure to its sprite. The two enums are kept in the same order on purpose (see the
// note over BIG_MANIFEST in tools/build_atlas.py) so this is an offset rather than a switch that
// could silently drift out of step with the art.
static_assert(static_cast<int>(Big::kCount) - static_cast<int>(Big::kHouseOrange) ==
                  static_cast<int>(StructureKind::kCount),
              "Big and StructureKind have gone out of step — re-run tools/build_atlas.py");

[[nodiscard]] Big big_of_structure(StructureKind k) {
    return static_cast<Big>(static_cast<int>(Big::kHouseOrange) + static_cast<int>(k));
}

// --- Tree placement -------------------------------------------------------------------------------
// Trees in this pack are 2 tiles WIDE and 3 TALL, but terrain is per-tile, so a run of tree tiles
// has to be resolved into whole trees. The first attempt drew one wherever `(gx + gy)` was even —
// that is a CHECKERBOARD, not a left-edge test, so trees landed diagonally on top of each other and
// every other tree tile drew nothing at all. That is the overlapping-and-sliced look.
//
// A tile is an anchor iff it is a tree and a MULTIPLE OF `kTreeStride` trees precede it in its row,
// so a run of N tree tiles yields ceil(N/3) trees. Terrain is a pure function, so this can be asked
// about any tile without a chunk view — including tiles just off-screen, which is what stops
// canopies being clipped at the view edge.
//
// The stride is 3 while the sprite is 4 wide, deliberately. Spacing them at their own width gives an
// orchard: even gaps, every canopy separate. One tile of overlap is what makes neighbouring crowns
// read as a single mass, which is what a wood looks like — and it is only affordable because the
// canopies are now whole (see BIG_MANIFEST). Two half-trees overlapping just looked broken.
inline constexpr int kTreeStride = 3;

[[nodiscard]] bool tree_at(std::uint16_t map, int x, int y) {
    if (x < 0 || y < 0 || x >= kMapTiles || y >= kMapTiles) return false;
    return terrain_of(kWorldSeed, map, x, y) == Terrain::kTree;
}

[[nodiscard]] bool tree_anchor(std::uint16_t map, int x, int y) {
    if (!tree_at(map, x, y)) return false;
    int run = 0;
    while (run < 64 && tree_at(map, x - 1 - run, y)) ++run;
    return (run % kTreeStride) == 0;
}

// A stable per-tile pixel offset, so trees do not sit on the 32px lattice in rows and columns. The
// range is deliberately small: ±5px at kTilePx=32 is enough to break the grid without letting a
// trunk drift off the tile the simulation says is blocked.
[[nodiscard]] std::uint32_t scatter_hash(int x, int y, std::uint32_t salt) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E37'79B9u ^
                      static_cast<std::uint32_t>(y) * 0x85EB'CA6Bu ^ salt;
    h ^= h >> 15;
    h *= 0xC2B2'AE35u;
    h ^= h >> 13;
    return h;
}

[[nodiscard]] float scatter_offset(int x, int y, std::uint32_t salt, float range) {
    const auto v = static_cast<float>(scatter_hash(x, y, salt) & 0xFFFFu) / 65535.0f;
    return (v * 2.0f - 1.0f) * range;
}

// Whether this tile shows a TEXTURED fill rather than a plain one.
//
// The old rule was "always", and that is the whole of defect D4: a grass tuft on every grass tile,
// at the same offset, is wallpaper. The author's own hand-built village map runs about one
// non-ground element per four ground tiles, and clumps them.
//
// Two stages, and the first is the one that matters. A per-tile roll alone gives uniform scatter,
// which at 24% looks exactly like the 100% case with holes in it — still no shape. The coarse noise
// field decides WHERE detail lives, so the ground gets bare stretches and thick patches, and the
// roll then breaks up the patch edge so it does not read as a blob with a boundary of its own.
[[nodiscard]] bool textured_here(std::uint16_t map, int gx, int gy) {
    const float patch = fbm(kWorldSeed ^ 0xDEC0'0DEC'0DEC'0DECull, static_cast<float>(gx),
                            static_cast<float>(gy), 9.0f);
    if (patch < 0.52f) return false;
    return (scatter_hash(gx, gy, 0xD3C0u + map) & 3u) != 0u;
}

[[nodiscard]] Anim anim_of(CreatureKind k) {
    switch (k) {
        case CreatureKind::kSlime: return Anim::kMobSlime;
        case CreatureKind::kSpider: return Anim::kMobSpider;
        case CreatureKind::kGhost: return Anim::kMobSpirit;
        case CreatureKind::kSkull: return Anim::kMobSkull;
        case CreatureKind::kBoar: return Anim::kBoar;
        case CreatureKind::kWolf: return Anim::kWolf;
        case CreatureKind::kBear: return Anim::kBear;
        case CreatureKind::kHare: return Anim::kRacoon;
        case CreatureKind::kChicken: return Anim::kChicken;
        case CreatureKind::kCount: break;
    }
    return Anim::kMobSlime;
}

[[nodiscard]] Fx fx_of_effect(EffectKind k) {
    switch (k) {
        case EffectKind::kSlash: return Fx::kSlash;
        case EffectKind::kFire: return Fx::kFire;
        case EffectKind::kIce: return Fx::kIce;
        case EffectKind::kEarth: return Fx::kEarth;
        case EffectKind::kShock: return Fx::kShock;
        case EffectKind::kBlast: return Fx::kBlast;
        // The ability flashes (F1a): a 360 arc, a curved finisher, a smoke puff.
        case EffectKind::kSlashHeavy: return Fx::kSlashHeavy;
        case EffectKind::kSlashCombo: return Fx::kSlashCombo;
        case EffectKind::kSmoke: return Fx::kSmoke;
        case EffectKind::kCount: break;
    }
    return Fx::kSlash;
}

// A creature carrying a status is TINTED rather than given its own sprite. Nine species times five
// statuses is forty-five sprites this pack does not have; a colour wash reads instantly, costs
// nothing, and — the part that matters — is the same colour as the spell that put it there, so the
// player learns the mapping without being told it.
[[nodiscard]] Color tint_of(Status s) {
    switch (s) {
        case Status::kFrozen: return Color{140, 210, 255, 255};
        case Status::kBurning: return Color{255, 150, 90, 255};
        case Status::kWet: return Color{150, 190, 255, 255};
        case Status::kMuddy: return Color{180, 150, 110, 255};
        case Status::kShocked: return Color{255, 245, 130, 255};
        case Status::kNone:
        case Status::kCount: break;
    }
    return WHITE;
}


// Growth reads as "sprout -> bigger plant -> the ripe crop's own colour", so only the last stage
// differs per crop kind.
[[nodiscard]] Slot slot_of(const Crop& c) {
    if (c.stage < kCropStages - 1) {
        return c.stage == 0 ? Slot::kCropGrowing : Slot::kCropSeedling;
    }
    switch (c.kind) {
        case CropKind::kWheat: return Slot::kCropWheatRipe;
        case CropKind::kCarrot: return Slot::kCropCarrotRipe;
        case CropKind::kPumpkin: return Slot::kCropPumpkinRipe;
    }
    return Slot::kCropWheatRipe;
}

// --- Ambience -------------------------------------------------------------------------------
// Leaves drifting across a meadow, rain over a swamp, snow in the north.
//
// NONE OF THIS PASSES THROUGH THE SIMULATION, and that is the whole design. A particle's position
// is a closed-form function of (its index, the world clock): no state is stored, nothing is
// updated, no actor knows these exist, and there is nothing to synchronise when the world is spread
// across machines. The cost of the feature to the architecture is exactly zero, which is why it is
// worth doing early — it is the cheapest thing in the project per unit of "this world is alive".
//
// The grid is anchored to the CAMERA, not to the map: particles are laid out on a wrapping tile of
// `kFxSpan` world-pixels and each is offset into the cell nearest the viewer, so a constant number
// of them is always on screen no matter where the player is or how far they have walked.
// 160 world pixels is five tiles, so at the default zoom a 1280x720 window holds roughly 8x5 cells
// and every cell contributes one leaf or three drops. Sparser than this and the effect reads as
// dust on the monitor rather than as weather — measured by looking, at 320, which was too thin.
inline constexpr int kFxSpan = 160;   // world pixels between particles of one layer
inline constexpr int kFxCells = 11;   // cells across; kFxCells^2 particles per layer

// Which ambience a ring gets. The wetland is the swamp/desert ring — rain suits the swamp half and
// is a reasonable lie over the desert half at this scale.
enum class Weather : std::uint8_t { kLeaves, kRain, kSnow, kNone };

[[nodiscard]] Weather weather_of(Ring r) {
    switch (r) {
        case Ring::kMeadow:
        case Ring::kForest: return Weather::kLeaves;
        case Ring::kWetland: return Weather::kRain;
        case Ring::kSnow: return Weather::kSnow;
        case Ring::kWasteland:
        case Ring::kCount: break;
    }
    return Weather::kNone;
}

}  // namespace

void draw_ui_fx(int fx, int frame, float cx, float cy, float size) {
    if (!g_atlas_ok || fx < 0 || fx >= static_cast<int>(Fx::kCount)) return;
    const AtlasFx& s = fx_of(static_cast<Fx>(fx));
    const AtlasRect r = fx_frame(static_cast<Fx>(fx), frame);
    const float longest = static_cast<float>(std::max(s.w, s.h));
    const float w = static_cast<float>(s.w) / longest * size;
    const float h = static_cast<float>(s.h) / longest * size;
    DrawTexturePro(g_atlas,
                   Rectangle{static_cast<float>(r.x), static_cast<float>(r.y),
                             static_cast<float>(s.w), static_cast<float>(s.h)},
                   Rectangle{cx, cy, w, h}, Vector2{w * 0.5f, h * 0.5f}, 0.0f, WHITE);
}

void draw_ui_icon(int icon, bool disabled, float x, float y, float size) {
    if (!g_atlas_ok || icon < 0 || icon >= static_cast<int>(Icon::kCount)) return;
    const AtlasRect r = icon_rect(static_cast<Icon>(icon), disabled);
    DrawTexturePro(g_atlas,
                   Rectangle{static_cast<float>(r.x), static_cast<float>(r.y),
                             static_cast<float>(kIconPx), static_cast<float>(kIconPx)},
                   Rectangle{x, y, size, size}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
}

struct RaylibBridge::Impl {
    Camera2D camera{};
    int width = 0;
    int height = 0;
    BuildKind selected = BuildKind::kHearth;
    Texture2D atlas{};
    bool atlas_ok = false;
    Element element = Element::kFire;
    bool building = false;
    mutable float swing_cd = 0.0f;
    // Seconds left to hold the local player's swing pose. Set when a swing is polled, counted down
    // each drawn frame; while it is positive the local player is drawn mid-swing instead of walking.
    // A render-only flourish — the simulation neither knows nor needs to.
    mutable float attack_pose = 0.0f;
    int last_chunks = 0;
    int last_creatures = 0;
    const WorldLayout* layout = nullptr;
    // Idle-vs-walk detection for the deluxe rig: `steps` is a monotonic move counter, so a change
    // since last frame means the player moved. We keep walking briefly past the last step so a
    // stop between tiles does not flicker to idle. Per-slot, driven by the published view alone.
    mutable std::uint32_t player_prev_steps[kMaxPlayers] = {};
    mutable double player_walk_until[kMaxPlayers] = {};

    // --- Hit feedback, which is CLIENT-SIDE and holds no simulation state (F2) -------------------
    // The pack author's damage_fx is flash + shake + debris. We reproduce the first two per creature
    // by watching its published HP frame-to-frame (creatures carry a stable id), and the debris by a
    // smoke puff when a creature vanishes. None of this is in a snapshot: a hit flash is a fact about
    // what THIS viewer just saw change, not about the world, so it lives here and costs no bandwidth.
    struct CreatureTrack {
        std::int16_t hp = 0;
        float x = 0.0f;  // last-seen position, in TILES — where a death puff goes
        float y = 0.0f;
        float flash = 0.0f;  // seconds of hit-flash + shake remaining
        bool seen = false;   // touched this frame; an untouched track means the creature is gone
    };
    // Keyed by creature id. Only creatures in view are ever inserted (the gather loop is the only
    // writer), so this stays the size of what is on screen, not of the world.
    mutable std::unordered_map<std::uint32_t, CreatureTrack> creature_tracks;
    // A client-side smoke burst where a creature died. The sim reaps a dead creature the same tick it
    // hits 0 HP — recon confirmed — so a client NEVER sees hp==0 in a view: a kill reads as "the id
    // was here last frame and is gone now". That last-seen vanish is the only honest death signal,
    // and this is where it is drawn.
    struct DeathPuff {
        float x = 0.0f;  // tiles
        float y = 0.0f;
        float age = 0.0f;  // seconds; drawn until kDeathPuffLife
    };
    static constexpr float kDeathPuffLife = 0.55f;
    mutable std::vector<DeathPuff> death_puffs;

    // Per-frame scratch, kept as members so a frame costs no allocations once the vectors have
    // reached their working size. `frame_views` and `frame_players` exist to keep the shared_ptrs
    // alive: `frame_sprites` holds RAW pointers into the creature and building vectors they own,
    // and the sorted pass runs after the loop that loaded them has gone out of scope.
    std::vector<ChunkViewPtr> frame_views;
    std::vector<Sprite> frame_sprites;
    std::vector<std::uint32_t> frame_structures;
    std::vector<std::uint32_t> frame_prefabs;
    PlayerViewPtr frame_players[kMaxPlayers];

    // This frame's view rect, one Terrain per TILE, with a one-tile skirt so that every drawn tile
    // can read all eight of its neighbours without falling off the edge.
    //
    // Not an optimisation for its own sake: a tile's terrain is read nine times, once by itself and
    // once by each neighbour, and it is not cheap to compute. `terrain_base` runs three two-octave
    // noise fields and `ring_of` a fourth, so a naive `ground()` would do about eighty noise
    // evaluations per tile. Filling the rect once cuts that to eight.
    std::vector<std::uint8_t> terrain_cache;
    int cache_x0 = 0, cache_y0 = 0, cache_w = 0, cache_h = 0;
    // Which room the camera is in, or -1 outdoors. The floor of every OTHER room is blanked, for the
    // same reason only one room's walls are drawn: the interior map is a lattice and a house has one
    // room in it. Blanking the walls alone left the neighbours' floors hanging in the dark.
    int active_room = -1;

    void build_terrain(std::uint16_t map, int x0, int y0, int x1, int y1) {
        cache_x0 = x0 - 1;
        cache_y0 = y0 - 1;
        cache_w = x1 - x0 + 3;
        cache_h = y1 - y0 + 3;
        terrain_cache.resize(static_cast<std::size_t>(cache_w) * cache_h);
        for (int gy = 0; gy < cache_h; ++gy) {
            for (int gx = 0; gx < cache_w; ++gx) {
                const int tx = cache_x0 + gx;
                const int ty = cache_y0 + gy;
                const bool other_room = map != kOverworld && active_room >= 0 &&
                                        room_index_at(tx, ty) != active_room;
                terrain_cache[static_cast<std::size_t>(gy) * cache_w + gx] = static_cast<std::uint8_t>(
                    other_room ? Terrain::kBuilding : tile_terrain(map, tx, ty));
            }
        }
    }

    [[nodiscard]] Terrain terrain_at(int gx, int gy) const {
        const int cx = gx - cache_x0;
        const int cy = gy - cache_y0;
        if (cx < 0 || cy < 0 || cx >= cache_w || cy >= cache_h) return Terrain::kGrass;
        return static_cast<Terrain>(terrain_cache[static_cast<std::size_t>(cy) * cache_w + cx]);
    }

    // One sprite, scaled from the atlas' 16px cell to whatever world size the caller wants.
    // `size` in pixels, `cx/cy` the CENTRE in world pixels — centring is what lets a crop grow by
    // drawing bigger without drifting off its tile.
    // `variant` 0-3 mirrors the source rect (x, y, both); see `tile_variant`.
    void sprite(Slot s, float cx, float cy, float size, Color tint = WHITE, int variant = 0,
                float rotation = 0.0f) const {
        if (!atlas_ok) return;
        const AtlasRect r = rect_of(s);
        const float t = static_cast<float>(kAtlasTile);
        // A negative width/height in the source rect tells raylib to flip that axis.
        const Rectangle src{static_cast<float>(r.x), static_cast<float>(r.y),
                            (variant & 1) ? -t : t, (variant & 2) ? -t : t};
        // Origin at the sprite centre so `rotation` spins in place rather than about a corner.
        const Rectangle dst{cx, cy, size, size};
        DrawTexturePro(atlas, src, dst, Vector2{size * 0.5f, size * 0.5f}, rotation, tint);
    }

    // One frame of an animation sheet.
    //
    // A SHEET WITH ONE ROW HAS NO FACINGS. Ninja Adventure's four-direction actors are 4x4 grids
    // (column = facing, row = frame), but its small animals ship as a single row of two frames —
    // a side view and its walk pose. Handing `dir` to those as a column makes the animal snap
    // between two poses as the player circles it, which reads as a glitch. So a one-row sheet is
    // driven by `frame` instead and simply bobs. One rule, decided here rather than as a flag in
    // the manifest, because it is a property of the ART and the packer already records it.
    void anim(Anim a, int dir, int frame, float cx, float cy, float size,
              Color tint = WHITE) const {
        if (!atlas_ok) return;
        const AtlasRect r =
            (anim_of(a).rows == 1) ? anim_frame(a, frame, 0) : anim_frame(a, dir, frame);
        const float t = static_cast<float>(kAtlasTile);
        const Rectangle src{static_cast<float>(r.x), static_cast<float>(r.y), t, t};
        const Rectangle dst{cx, cy, size, size};
        DrawTexturePro(atlas, src, dst, Vector2{size * 0.5f, size * 0.5f}, 0.0f, tint);
    }

    // One frame of the DELUXE 32px player rig (a body sheet or its frame-aligned katana overlay).
    // Same column=facing, row=frame wrap rules as `anim`, but the cell is 32px. The 16px body is
    // centred in that cell, so drawing the whole cell at `size` = 2x the 16px sprite quad lands the
    // body at the same on-screen size AND its feet on the same point — the overflow room is what a
    // swing swings into. `size` is the FULL 32px-quad size; the caller doubles the 16px quad.
    void deluxe(Deluxe d, int dir, int frame, float cx, float cy, float size,
                Color tint = WHITE) const {
        if (!atlas_ok) return;
        const AtlasRect r = deluxe_frame(d, dir, frame);
        const float t = static_cast<float>(kDeluxeTile);
        const Rectangle src{static_cast<float>(r.x), static_cast<float>(r.y), t, t};
        const Rectangle dst{cx, cy, size, size};
        DrawTexturePro(atlas, src, dst, Vector2{size * 0.5f, size * 0.5f}, 0.0f, tint);
    }

    // The katana CARRIED on the back when not swinging. This is the pack's own weapon.gd rule: the
    // 6x10 in-hand sprite is offset along the facing and rotated to the facing angle minus 90 deg,
    // and it sits BEHIND the body when facing up (the blade is on the far side of the shoulders) and
    // in front otherwise. `back` selects which of those two draws this call is — the caller makes
    // both, once before the body and once after, and only the matching one paints.
    void carry_katana(Facing facing, float cx, float cy, float px_scale, bool back,
                      Color tint = WHITE) const {
        if (!atlas_ok) return;
        const bool facing_up = facing == Facing::kUp;
        if (facing_up != back) return;  // draw behind only when facing up; in front otherwise
        const AtlasSprite& s = kKatanaCarry;
        // Facing unit vector in screen space (y grows downward).
        float fx = 0.0f, fy = 0.0f;
        switch (facing) {
            case Facing::kDown:  fy =  1.0f; break;
            case Facing::kUp:    fy = -1.0f; break;
            case Facing::kLeft:  fx = -1.0f; break;
            case Facing::kRight: fx =  1.0f; break;
        }
        // ~(5,5) native-pixel offset along the facing, scaled to our pixels (the pack's weapon.gd
        // uses ~(10,10) on a scene where the body is drawn at half our scale, so this is the same
        // displacement). It rides toward the facing side of the body so it does not cover the face.
        const float off = 5.0f * px_scale;
        const float wx = cx + fx * off;
        const float wy = cy + fy * off;
        const float angle = std::atan2(fy, fx) * 180.0f / 3.14159265f - 90.0f;
        const float w = static_cast<float>(s.w) * px_scale;
        const float h = static_cast<float>(s.h) * px_scale;
        const Rectangle src{static_cast<float>(s.x), static_cast<float>(s.y),
                            static_cast<float>(s.w), static_cast<float>(s.h)};
        DrawTexturePro(atlas, src, Rectangle{wx, wy, w, h}, Vector2{w * 0.5f, h * 0.5f}, angle,
                       tint);
    }

    // A multi-tile sprite, anchored bottom-centre on tile (tx,ty): the trunk sits on its own tile
    // and the canopy overhangs the tiles above.
    void big(Big b, int tx, int ty, Color tint = WHITE) const {
        if (!atlas_ok) return;
        const AtlasBig& s = big_of(b);
        const Rectangle src{static_cast<float>(s.x), static_cast<float>(s.y),
                            static_cast<float>(s.w * kAtlasTile),
                            static_cast<float>(s.h * kAtlasTile)};
        const float w = static_cast<float>(s.w * kTilePx);
        const float h = static_cast<float>(s.h * kTilePx);
        const Rectangle dst{(static_cast<float>(tx) + 0.5f) * kTilePx,
                            static_cast<float>(ty + 1) * kTilePx, w, h};
        DrawTexturePro(atlas, src, dst, Vector2{w * 0.5f, h}, 0.0f, tint);
    }

    // One cell of a placed prefab, at `q` (its mirror already resolved into the position). A
    // negative source width flips the ART left-to-right when the instance is mirrored. Floor cells
    // (layers 0/1) call this directly during the flat pass; structure/prop cells go through the
    // sorted list and are drawn by `draw_sprite`, which reconstructs the same rect from the cell.
    void prefab_cell(const PrefabCell& c, const PrefabQuad& q, bool flip) const {
        if (!atlas_ok) return;
        const float pw = static_cast<float>(c.pw);
        const Rectangle src{static_cast<float>(c.ax), static_cast<float>(c.ay), flip ? -pw : pw,
                            static_cast<float>(c.ph)};
        const Rectangle dst{q.x, q.y, q.w, q.h};
        DrawTexturePro(atlas, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    }

    // One ground tile: this tile's own terrain, blob-masked against its eight neighbours.
    //
    // The tile the simulation reports decides the common case — a tile of grass surrounded by grass
    // draws a grass fill, exactly as before. What changes is the boundary. A shore tile is drawn
    // twice: the strongest LOWER-priority neighbour supplies the fill, and this tile's own edge art
    // for the mask its neighbourhood produces goes over the top. Because the mask is Godot's minimal
    // rule and the art is the pack's own blob set, the piece drawn here is the piece the author drew
    // to meet the piece its neighbour will draw.
    //
    // A neighbour counts as "mine" when its priority is at least this tile's, not when it is equal.
    // Equality alone would cut a hole in a lake wherever a road ran down to it: the road tile would
    // read as outside the water, so the water would draw itself a shoreline against its own bank.
    // Ordering by priority instead means the boundary is always drawn once, by the weaker side.
    void ground(std::uint16_t map, int gx, int gy) const {
        // NOT MIRRORED, and that is a reversal worth explaining. Mirroring a fill per tile is free
        // and used to be here to break the repeat of a single motif — but a motif that reaches the
        // tile's edge does not survive being flipped: the two halves of the pattern no longer meet,
        // so mirroring writes a discontinuity onto EVERY tile boundary. On a lake, where the fill is
        // one large rippling motif, that is thousands of colour boundaries pinned exactly to the
        // grid, which is the defect this whole change is measured against. Seamless repetition beats
        // mirrored variety; the clustered textured patches supply the variety instead.
        const int variant = tile_variant(map, gx, gy);
        const int pick = textured_here(map, gx, gy) ? 1 + ((variant >> 2) & 1) : 0;

        const Terrain self = terrain_at(gx, gy);
        // Everything on the interior map that is not floor is either wall or the void between one
        // room and the next, and BOTH are drawn by leaving the tile alone: the room sprite supplies
        // the wall, and what is between rooms should be the black the pack's own Interior map has
        // around its room. Painting a fill here and covering it is the same picture at twice the
        // cost, and painting a fill and NOT covering it is a floor where the outdoors should be.
        if (map != kOverworld && self == Terrain::kBuilding) return;
        const int mine = terrain_priority(self);
        const int mask = edge_mask([&](int dx, int dy) {
            return terrain_priority(terrain_at(gx + dx, gy + dy)) >= mine;
        });

        if (mask == kEdgeFull) {
            im_tile_fill(self, gx, gy, pick);
            return;
        }

        // The fill is the strongest neighbour weaker than this tile. Three or more terrains can meet
        // at one tile and only the strongest of the weak ones is drawn — a lie confined to the
        // handful of tiles where three biomes touch, and invisible at any zoom.
        Terrain base = self;
        int best = -1;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const Terrain n = terrain_at(gx + dx, gy + dy);
                const int p = terrain_priority(n);
                if (p < mine && p > best) {
                    best = p;
                    base = n;
                }
            }
        }
        im_tile_fill(base, gx, gy, pick);
        trans(self, mask, gx, gy);
    }

    // The plain-or-textured fill for one terrain, with the variant run resolved.
    //
    // The mirror is the exception that proves the rule above. Mirroring is off everywhere it can be,
    // because it writes a discontinuity onto every tile boundary — but stone and ash have no plain
    // fill anywhere in the pack (`kTerrainHasPlain`, derived from the manifest), so their three
    // variants are all whole-tile masonry. Left unmirrored those tile into a flawless brick lattice
    // and the wasteland reads as a cathedral floor. Mirrored, the seams are the lesser evil: broken
    // rubble is at least the right kind of wrong for scorched ground. The real fix is art this CC0
    // pack does not contain.
    void im_tile_fill(Terrain t, int gx, int gy, int pick) const {
        const auto first = static_cast<int>(slot_of(t));
        const int mirror = kTerrainHasPlain[static_cast<int>(t)] ? 0 : (tile_variant(0, gx, gy) & 3);
        tile(static_cast<Slot>(first + (pick % kTerrainVariants)), gx, gy, WHITE, mirror);
    }

    // One edge tile: `t`'s own art for the neighbourhood `mask` describes, over whatever has
    // already been drawn on this tile. Never mirrored — the mask IS the orientation.
    void trans(Terrain t, int mask, int tx, int ty) const {
        if (!atlas_ok) return;
        const AtlasRect r = trans_rect(static_cast<int>(t), mask);
        const float a = static_cast<float>(kAtlasTile);
        const Rectangle src{static_cast<float>(r.x), static_cast<float>(r.y), a, a};
        const Rectangle dst{static_cast<float>(tx * kTilePx), static_cast<float>(ty * kTilePx),
                            static_cast<float>(kTilePx), static_cast<float>(kTilePx)};
        DrawTexturePro(atlas, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    }

    // A full tile at its grid position.
    void tile(Slot s, int tx, int ty, Color tint = WHITE, int variant = 0,
              float rotation = 0.0f) const {
        sprite(s, (static_cast<float>(tx) + 0.5f) * kTilePx,
               (static_cast<float>(ty) + 0.5f) * kTilePx, static_cast<float>(kTilePx), tint, variant,
               rotation);
    }

    // A multi-tile sprite anchored bottom-centre on an arbitrary WORLD-PIXEL point, rather than on
    // a tile. Structures use the tile form; the hearth uses this so a two-tile-wide fire can sit
    // centred on the one tile it occupies.
    void big_at(Big b, float cx, float bottom_y, Color tint = WHITE) const {
        if (!atlas_ok) return;
        const AtlasBig& s = big_of(b);
        const Rectangle src{static_cast<float>(s.x), static_cast<float>(s.y),
                            static_cast<float>(s.w * kAtlasTile),
                            static_cast<float>(s.h * kAtlasTile)};
        const float w = static_cast<float>(s.w * kTilePx);
        const float h = static_cast<float>(s.h * kTilePx);
        DrawTexturePro(atlas, src, Rectangle{cx, bottom_y, w, h}, Vector2{w * 0.5f, h}, 0.0f, tint);
    }

    // One particle sprite at a world-pixel position, drawn at its native pixel size scaled to the
    // tile grid (a 12x7 leaf is roughly two-thirds of a tile wide, which is what it should be).
    void fx(Fx f, int frame, float cx, float cy, float alpha, float rotation = 0.0f,
            float zoom = 1.0f) const {
        if (!atlas_ok) return;
        const AtlasFx& s = fx_of(f);
        const AtlasRect r = fx_frame(f, frame);
        const float scale = zoom * static_cast<float>(kTilePx) / static_cast<float>(kAtlasTile);
        const float w = static_cast<float>(s.w) * scale;
        const float h = static_cast<float>(s.h) * scale;
        const Rectangle src{static_cast<float>(r.x), static_cast<float>(r.y),
                            static_cast<float>(s.w), static_cast<float>(s.h)};
        const auto a = static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
        DrawTexturePro(atlas, src, Rectangle{cx, cy, w, h}, Vector2{w * 0.5f, h * 0.5f}, rotation,
                       Color{255, 255, 255, a});
    }

    // The whole ambience layer. `t` is WORLD time in seconds, not frame time — so two machines
    // watching the same place see leaves in the same position, and a screenshot taken at world
    // t=20s is reproducible. That property costs nothing here and is the difference between an
    // effect you can debug and one you cannot.
    // One entry of the Y-sorted list. Each case draws exactly what its old dedicated pass drew —
    // the change is WHEN it runs, not what it paints.
    void draw_sprite(const Sprite& sp, int local_slot) const {
        switch (sp.kind) {
            case SpriteKind::kCrop: {
                const auto& cr = *static_cast<const Crop*>(sp.p);
                // Size tracks growth: a seedling is drawn at 45% of a tile, a ripe crop fills it.
                const float ratio = 0.45f + 0.55f * (static_cast<float>(cr.stage) /
                                                     static_cast<float>(kCropStages - 1));
                sprite(slot_of(cr), sp.x, sp.y, kTilePx * ratio);
                return;
            }
            case SpriteKind::kBuilding: {
                const auto& b = *static_cast<const Building*>(sp.p);
                tile(b.kind == BuildKind::kHearth ? Slot::kBuildHearth : Slot::kBuildPlot, b.tx,
                     b.ty);
                // Level pips: one small dot per level above 1, so an upgraded building is
                // identifiable at a glance without a UI panel.
                for (int lv = 1; lv < b.level; ++lv) {
                    DrawRectangle(b.tx * kTilePx + 3 + (lv - 1) * 6, b.ty * kTilePx + 2, 4, 4,
                                  Color{255, 220, 90, 255});
                }
                // Health bar, only when damaged — no clutter on an untouched base.
                const std::int16_t full = max_hp_of(b.kind);
                if (b.hp < full && full > 0) {
                    const int w = std::max(1, (b.hp * kTilePx) / full);
                    DrawRectangle(b.tx * kTilePx, b.ty * kTilePx - 4, w, 3,
                                  Color{220, 70, 70, 255});
                }
                return;
            }
            case SpriteKind::kCreature: {
                const auto& m = *static_cast<const Creature*>(sp.p);
                // A flat ellipse under each creature: with no height in the art, this is what stops
                // the sprites reading as decals lying on the ground. The shadow stays PUT while the
                // body shakes, so a hit/telegraph reads as the creature flinching, not sliding.
                DrawEllipse(static_cast<int>(sp.x), static_cast<int>(sp.y + 6), kTilePx * 0.28f,
                            kTilePx * 0.14f, Color{0, 0, 0, 70});
                // --- Hit + telegraph feedback (F2), entirely client-side ---------------------------
                // Both reads are ADDITIVE overlays, not tints: a multiplicative tint can only darken,
                // so it cannot redden an already-blue slime — but adding colour onto the lit pixels
                // shows on any sprite. A creature winding up to a swing has no attack frame in this
                // walk-only pack, so its whole "incoming" read is a pulsing warm glow plus a 1px
                // micro-shake; together with the smoke puff thrown at commit, that IS the tell. A
                // creature struck this instant flashes bright white and jitters 2px — the pack
                // author's own damage_fx of flash + shake.
                const auto tk = creature_tracks.find(m.id);
                const float flash = (tk != creature_tracks.end()) ? tk->second.flash : 0.0f;
                const float fi = std::clamp(flash / 0.15f, 0.0f, 1.0f);  // flash intensity, 0..1
                float shake = 0.0f;
                const float wobble = (static_cast<int>(GetTime() * 40.0) & 1) ? 1.0f : -1.0f;
                if (m.windup > 0) shake = wobble;
                if (flash > 0.05f) shake = wobble * 2.0f;  // a struck flinch overrides the wind-up wobble
                const float bx = sp.x + shake;
                // A struck creature POPS bigger for the length of the flash. Additive white can only
                // brighten a coloured sprite toward a lighter version of itself, never to true white,
                // so the size pop is what makes the hit unmistakable on any base colour — brighten AND
                // swell, the way the pack's own damage_fx sells a blow.
                const float size = kTilePx * (1.0f + 0.18f * fi);
                anim(anim_of(m.kind), static_cast<int>(m.facing), sp.frame, bx, sp.y, size,
                     tint_of(m.status));
                if (m.windup > 0 && flash <= 0.05f) {
                    // A red charge that pulses so it reads as "winding up" and not merely "on fire".
                    // Kept off its trough (0.7..1.0) so the glow is unmistakable at game scale on any
                    // sprite colour, not merely present.
                    const float pulse = 0.85f + 0.15f * std::sin(static_cast<float>(GetTime()) * 12.0f);
                    const auto a = static_cast<unsigned char>(pulse * 235.0f);
                    BeginBlendMode(BLEND_ADDITIVE);
                    anim(anim_of(m.kind), static_cast<int>(m.facing), sp.frame, bx, sp.y,
                         kTilePx * 1.0f, Color{255, 40, 30, a});
                    EndBlendMode();
                }
                if (flash > 0.05f) {
                    BeginBlendMode(BLEND_ADDITIVE);  // adds toward white without touching transparent px
                    const auto a = static_cast<unsigned char>(fi * 255.0f);
                    // Two passes: additive saturates faster, so a light slime actually reaches a white
                    // pop rather than a slightly brighter blue.
                    anim(anim_of(m.kind), static_cast<int>(m.facing), sp.frame, bx, sp.y, size,
                         Color{255, 255, 255, a});
                    anim(anim_of(m.kind), static_cast<int>(m.facing), sp.frame, bx, sp.y, size,
                         Color{255, 255, 255, a});
                    EndBlendMode();
                }
                // A health bar only once it has been hurt: an untouched field of animals with bars
                // over every one of them reads as a bestiary rather than as a meadow.
                if (m.hp < m.max_hp && m.max_hp > 0) {
                    const int w = std::max(1, (m.hp * (kTilePx - 6)) / m.max_hp);
                    DrawRectangle(static_cast<int>(sp.x) - kTilePx / 2 + 3,
                                  static_cast<int>(sp.y) - kTilePx / 2 - 3, kTilePx - 6, 3,
                                  Color{0, 0, 0, 120});
                    DrawRectangle(static_cast<int>(sp.x) - kTilePx / 2 + 3,
                                  static_cast<int>(sp.y) - kTilePx / 2 - 3, w, 3,
                                  Color{220, 70, 70, 255});
                }
                // One pixel of intent: an angry creature gets a mark. Disposition is state, and
                // state the player cannot see is state that might as well not exist — this is the
                // difference between "the boar attacked me for no reason" and "I annoyed a boar".
                if (m.anger_ticks > 0 && m.disposition != Disposition::kTimid) {
                    DrawRectangle(static_cast<int>(sp.x) - 1,
                                  static_cast<int>(sp.y) - kTilePx / 2 - 9, 3, 5,
                                  Color{255, 90, 60, 230});
                }
                return;
            }
            case SpriteKind::kShot: {
                // Arrows, pointed the way they are travelling. `Fx::kArrow` is one sprite drawn
                // rotated — the only rotated sprite in the game, and the reason arrows live in the
                // FX strip rather than the tile grid.
                const auto& s = *static_cast<const Projectile*>(sp.p);
                fx(Fx::kArrow, 0, sp.x, sp.y, 1.0f,
                   std::atan2(s.vy, s.vx) * 57.2957795f + 90.0f);
                return;
            }
            case SpriteKind::kBig:
                big_at(static_cast<Big>(sp.a), sp.x, sp.y);
                return;
            case SpriteKind::kPrefabCell: {
                // The gather stored the destination top-left in x,y and the flip in `a`; the atlas
                // rect and on-screen size come from the cell itself.
                const auto& c = *static_cast<const PrefabCell*>(sp.p);
                const int scale = kTilePx / kAtlasTile;
                prefab_cell(c, PrefabQuad{sp.x, sp.y, static_cast<float>(c.pw * scale),
                                          static_cast<float>(c.ph * scale)},
                            sp.a != 0);
                return;
            }
            case SpriteKind::kPlayer: {
                // From the frame's own snapshot, not a second `players.load()`. Re-loading would be
                // a fresh atomic read of a bus the simulation is still writing, so a player could
                // be gathered at one position and drawn at another — sorted by where they were and
                // painted where they now are.
                const PlayerViewPtr& pv = frame_players[sp.a];
                if (!pv) return;
                const PlayerView& p = *pv;
                // A dead player is a faint ghost lying where they fell, not an absence: watching a
                // friend go down and waiting for them is a moment, and despawning them deletes it.
                const bool dead = p.dead_ticks > 0;
                DrawEllipse(static_cast<int>(sp.x), static_cast<int>(sp.y + 7), kTilePx * 0.30f,
                            kTilePx * 0.15f,
                            Color{0, 0, 0, static_cast<unsigned char>(dead ? 40 : 80)});
                if (p.mounted) {
                    // The mount is drawn under the rider, one tile lower and slightly larger.
                    anim(Anim::kHorse, static_cast<int>(p.facing), static_cast<int>(p.steps / 5),
                         sp.x, sp.y + 3.0f, kTilePx * 1.3f);
                }
                // `steps` is the authoritative move counter from PlayerActor, not a local frame
                // timer — so the walk cycle stays in step with the simulation rather than with this
                // client's frame rate. It is also what makes a REMOTE player's animation correct
                // for free.
                const float body_y = sp.y - (p.mounted ? 5.0f : 0.0f);
                const Color body_tint = dead ? Color{255, 255, 255, 90} : WHITE;
                if (static_cast<int>(sp.a) == local_slot && attack_pose > 0.0f && atlas_ok) {
                    // Mid-swing: the attack pose, one frame per facing. Addressed by FACING as the
                    // column via anim_frame directly — not through `anim`, whose rows==1 path reads a
                    // one-row sheet's columns as animation frames (right for a two-frame animal,
                    // wrong for four facings). Only the local player: a remote client cannot know
                    // when someone else swung, and the effect the chunk publishes already shows it.
                    const AtlasRect ar = anim_frame(Anim::kPlayerAttack, static_cast<int>(p.facing), 0);
                    const float t = static_cast<float>(kAtlasTile);
                    const float size = kTilePx * 1.1f;
                    DrawTexturePro(
                        atlas, Rectangle{static_cast<float>(ar.x), static_cast<float>(ar.y), t, t},
                        Rectangle{sp.x, body_y, size, size}, Vector2{size * 0.5f, size * 0.5f}, 0.0f,
                        body_tint);
                } else {
                    anim(Anim::kPlayer, static_cast<int>(p.facing), static_cast<int>(p.steps / 4),
                         sp.x, body_y, kTilePx * 1.1f, body_tint);
                }
                if (static_cast<int>(sp.a) != local_slot) {
                    // Somebody else's health bar, always shown — you cannot help a stranger you
                    // cannot read.
                    const int w =
                        std::max(1, (p.hp * (kTilePx - 4)) / std::max<int>(1, p.max_hp));
                    DrawRectangle(static_cast<int>(sp.x) - kTilePx / 2 + 2,
                                  static_cast<int>(sp.y) - kTilePx / 2 - 6, kTilePx - 4, 3,
                                  Color{0, 0, 0, 140});
                    DrawRectangle(static_cast<int>(sp.x) - kTilePx / 2 + 2,
                                  static_cast<int>(sp.y) - kTilePx / 2 - 6, w, 3,
                                  Color{90, 220, 120, 255});
                }
                return;
            }
        }
    }

    void draw_ambience(std::uint16_t map, float cam_x, float cam_y, double t) const {
        if (map != kOverworld) return;  // it does not snow indoors
        const Weather w = weather_of(ring_of(kWorldSeed, static_cast<int>(cam_x / kTilePx),
                                             static_cast<int>(cam_y / kTilePx)));
        if (w == Weather::kNone) return;

        // Snap the grid origin to the camera so the same number of particles is always in view.
        const float ox = std::floor(cam_x / kFxSpan) * kFxSpan - (kFxCells / 2) * kFxSpan;
        const float oy = std::floor(cam_y / kFxSpan) * kFxSpan - (kFxCells / 2) * kFxSpan;

        for (int gy = 0; gy < kFxCells; ++gy) {
            for (int gx = 0; gx < kFxCells; ++gx) {
                // A per-cell hash gives each particle a fixed personality: where in its cell it
                // sits, how fast it falls, how big its sway is. Deterministic, so a particle does
                // not teleport when the grid origin moves under it.
                Rng r(static_cast<std::uint64_t>(gx) * 0x9E37'79B9ull ^
                      (static_cast<std::uint64_t>(gy) << 24) ^ 0x1EAFull);
                const float jx = r.unit() * kFxSpan;
                const float jy = r.unit() * kFxSpan;
                const float speed = 0.5f + r.unit();
                const float phase = r.unit() * 6.2831853f;

                float px = ox + gx * kFxSpan + jx;
                float py = oy + gy * kFxSpan + jy;

                switch (w) {
                    case Weather::kLeaves: {
                        // Drift on the wind and bob: slow, sideways, unhurried. This is the chill
                        // ring's whole personality in four lines.
                        px += static_cast<float>(t) * 22.0f * speed;
                        py += std::sin(static_cast<float>(t) * 0.7f + phase) * 26.0f;
                        px = ox + std::fmod(px - ox + kFxSpan * kFxCells * 4.0f,
                                            static_cast<float>(kFxSpan * kFxCells));
                        fx(r.unit() < 0.35f ? Fx::kLeafPink : Fx::kLeaf,
                           static_cast<int>(t * 6.0 + gx * 3 + gy), px, py, 0.85f,
                           std::sin(static_cast<float>(t) * 1.1f + phase) * 25.0f);
                        break;
                    }
                    case Weather::kRain: {
                        py += static_cast<float>(t) * 620.0f * speed;
                        px += static_cast<float>(t) * 90.0f;
                        py = oy + std::fmod(py - oy + kFxSpan * kFxCells * 8.0f,
                                            static_cast<float>(kFxSpan * kFxCells));
                        px = ox + std::fmod(px - ox + kFxSpan * kFxCells * 8.0f,
                                            static_cast<float>(kFxSpan * kFxCells));
                        // Three drops per cell: rain has to be dense or it reads as debris.
                        for (int k = 0; k < 3; ++k) {
                            fx(Fx::kRain, static_cast<int>(t * 14.0) + k,
                               px + static_cast<float>(k) * 37.0f,
                               py + static_cast<float>(k) * 113.0f, 0.55f);
                        }
                        break;
                    }
                    case Weather::kSnow: {
                        py += static_cast<float>(t) * 46.0f * speed;
                        px += std::sin(static_cast<float>(t) * 0.5f + phase) * 34.0f;
                        py = oy + std::fmod(py - oy + kFxSpan * kFxCells * 8.0f,
                                            static_cast<float>(kFxSpan * kFxCells));
                        for (int k = 0; k < 3; ++k) {
                            fx(Fx::kSnow, static_cast<int>(t * 4.0) + k * 2,
                               px + static_cast<float>(k) * 61.0f,
                               py + static_cast<float>(k) * 97.0f, 0.8f);
                        }
                        break;
                    }
                    case Weather::kNone: break;
                }
            }
        }
    }
};

RaylibBridge::RaylibBridge(int width, int height) : impl_(std::make_unique<Impl>()) {
    impl_->width = width;
    impl_->height = height;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(width, height, "Quark MMO — farm, build, survive");
    SetTargetFPS(60);
    impl_->camera.zoom = 1.0f;
    impl_->camera.offset = Vector2{static_cast<float>(width) / 2.0f,
                                   static_cast<float>(height) / 2.0f};

    // Look for the atlas relative to the CWD and to the executable, so running from either the
    // repo root or the build directory works.
    for (const char* p : {"assets/atlas.png", "../assets/atlas.png", "../../assets/atlas.png"}) {
        if (!FileExists(p)) continue;
        impl_->atlas = LoadTexture(p);
        impl_->atlas_ok = impl_->atlas.id != 0;
        if (impl_->atlas_ok) {
            // POINT filtering keeps 16px pixel art crisp at any zoom; the packer's 1px extrusion
            // handles the fractional-coordinate sampling the continuous zoom produces.
            SetTextureFilter(impl_->atlas, TEXTURE_FILTER_POINT);
            g_atlas = impl_->atlas;
            g_atlas_ok = true;
            TraceLog(LOG_INFO, "ATLAS: loaded %s (%dx%d)", p, impl_->atlas.width,
                     impl_->atlas.height);
            break;
        }
    }
    if (!impl_->atlas_ok) {
        TraceLog(LOG_WARNING, "ATLAS: assets/atlas.png not found — run tools/fetch_assets.sh");
    }
}

RaylibBridge::~RaylibBridge() {
    g_atlas_ok = false;
    if (impl_->atlas_ok) UnloadTexture(impl_->atlas);
    if (IsWindowReady()) CloseWindow();
}

bool RaylibBridge::begin_frame() { return !WindowShouldClose(); }

float RaylibBridge::frame_time() const { return GetFrameTime(); }

void RaylibBridge::draw(const SnapshotBus& bus, const WorldStatus& status,
                        const PlayerBus& players, int local_slot) {
    Impl& im = *impl_;
    const float dt = GetFrameTime();
    im.attack_pose = std::max(0.0f, im.attack_pose - GetFrameTime());
    // Age the client-side hit feedback (F2): fade each creature's flash/shake, march each death puff
    // toward its end, and mark every track unseen — the gather loop below re-marks the ones still on
    // screen, and whatever is left unseen afterwards is a creature that has left the view or died.
    // The aging step is clamped: a single slow or hitched frame must not swallow a whole 0.15s flash,
    // and on a headless screenshot the frame time is long enough that it otherwise would.
    const float fb_dt = std::min(dt, 0.033f);
    for (auto& kv : im.creature_tracks) {
        kv.second.flash = std::max(0.0f, kv.second.flash - fb_dt);
        kv.second.seen = false;
    }
    for (std::size_t i = im.death_puffs.size(); i-- > 0;) {
        im.death_puffs[i].age += fb_dt;
        if (im.death_puffs[i].age >= Impl::kDeathPuffLife)
            im.death_puffs.erase(im.death_puffs.begin() + static_cast<std::ptrdiff_t>(i));
    }
    PlayerViewPtr me = players.load(local_slot);
    static const PlayerView kNobody{};
    const PlayerView& player = me ? *me : kNobody;
    im.width = GetScreenWidth();
    im.height = GetScreenHeight();
    im.camera.offset = Vector2{static_cast<float>(im.width) / 2.0f,
                               static_cast<float>(im.height) / 2.0f};
    im.camera.target = Vector2{player.x * kTilePx, player.y * kTilePx};

    // Night tints the whole scene rather than dimming each sprite — one cheap overlay instead of a
    // per-entity colour path.
    const bool night = status.night.load(std::memory_order_relaxed);
    BeginDrawing();
    ClearBackground(night ? Color{12, 14, 30, 255} : Color{28, 34, 40, 255});
    BeginMode2D(im.camera);

    // --- Cull to the visible tile rect, then to the chunks that overlap it ----------------------
    const float half_w = static_cast<float>(im.width) / (2.0f * im.camera.zoom * kTilePx);
    const float half_h = static_cast<float>(im.height) / (2.0f * im.camera.zoom * kTilePx);
    const int min_tx = static_cast<int>(std::floor(player.x - half_w)) - 1;
    const int max_tx = static_cast<int>(std::ceil(player.x + half_w)) + 1;
    const int min_ty = static_cast<int>(std::floor(player.y - half_h)) - 1;
    const int max_ty = static_cast<int>(std::ceil(player.y + half_h)) + 1;

    const int min_cx = std::max(0, min_tx / kChunkTiles);
    const int max_cx = std::min(kMapChunks - 1, max_tx / kChunkTiles);
    const int min_cy = std::max(0, min_ty / kChunkTiles);
    const int max_cy = std::min(kMapChunks - 1, max_ty / kChunkTiles);

    int drawn_chunks = 0;
    int drawn_creatures = 0;

    // (A wall/fence autotiling pre-pass used to run here, gathering neighbour sets across chunk
    // borders so a wall run did not change style at a boundary. It is gone with per-tile walls
    // themselves: a structure is one sprite, so there are no neighbours to consult and no seam to
    // hide. Deleting an entire cross-chunk pre-pass is what "buildings are whole structures" buys
    // in the renderer.)

    // --- Layer 1: the floor -------------------------------------------------------------------
    // The only pass that is NOT Y-sorted, and the only one that does not need to be: ground is flat,
    // so nothing can stand in front of it. Every chunk view loaded here is kept alive in
    // `im.frame_views` for the rest of the frame, because the sorted pass below holds raw pointers
    // into their creature and building vectors.
    im.frame_views.clear();
    im.frame_sprites.clear();
    for (PlayerViewPtr& slot : im.frame_players) slot.reset();
    // One extra vertex past each edge: a tile at the view's right edge reads the corner beyond it.
    im.active_room = (player.map == kOverworld)
                         ? -1
                         : room_index_at(static_cast<int>(player.x), static_cast<int>(player.y));
    im.build_terrain(player.map, min_tx, min_ty, max_tx, max_ty);
    for (int cy = min_cy; cy <= max_cy; ++cy) {
        for (int cx = min_cx; cx <= max_cx; ++cx) {
            const ChunkCoord c{player.map, static_cast<std::uint16_t>(cx),
                               static_cast<std::uint16_t>(cy)};
            ChunkViewPtr v = bus.load(c);
            if (!v) continue;  // this chunk has not ticked yet (or is still in flight from a peer)
            ++drawn_chunks;
            im.frame_views.push_back(v);

            const int base_x = cx * kChunkTiles;
            const int base_y = cy * kChunkTiles;

            for (int ly = 0; ly < kChunkTiles; ++ly) {
                const int gy = base_y + ly;
                if (gy < min_ty || gy > max_ty) continue;
                for (int lx = 0; lx < kChunkTiles; ++lx) {
                    const int gx = base_x + lx;
                    if (gx < min_tx || gx > max_tx) continue;
                    im.ground(c.map, gx, gy);
                }
            }
            // --- Layer 2: everything that stands on the ground ------------------------------
            // Gathered, not drawn. See the sorted pass below.
            for (const Crop& cr : v->crops) {
                im.frame_sprites.push_back(
                    Sprite{static_cast<float>(cr.ty + 1) * kTilePx,
                           (static_cast<float>(cr.tx) + 0.5f) * kTilePx,
                           (static_cast<float>(cr.ty) + 0.5f) * kTilePx, &cr, 0, 0,
                           SpriteKind::kCrop});
            }
            for (const Building& b : v->buildings) {
                im.frame_sprites.push_back(Sprite{static_cast<float>(b.ty + 1) * kTilePx, 0.0f,
                                                  0.0f, &b, 0, 0, SpriteKind::kBuilding});
            }
            for (const Creature& m : v->creatures) {
                // Offsetting the frame by the id keeps a whole wave from stepping in unison, which
                // reads as one organism rather than a crowd.
                const auto frame = static_cast<std::uint16_t>((v->tick / 3 + m.id) & 0xFF);
                im.frame_sprites.push_back(Sprite{m.y * kTilePx + kTilePx * 0.5f, m.x * kTilePx,
                                                  m.y * kTilePx, &m, 0, frame,
                                                  SpriteKind::kCreature});
                ++drawn_creatures;
                // Hit feedback (F2): a drop in published HP since last frame flashes the sprite. New
                // creatures just start a track; the position is kept for a death puff if it vanishes.
                Impl::CreatureTrack& tr = im.creature_tracks[m.id];
                if (tr.seen) {
                    // Already touched this frame (a creature at a chunk seam is listed by both) — do
                    // not double-read its HP.
                } else if (tr.hp != 0 && m.hp < tr.hp) {
                    tr.flash = 0.15f;  // the pack's damage_fx: a bright frame and a shake
                }
                tr.hp = m.hp;
                tr.x = m.x;
                tr.y = m.y;
                tr.seen = true;
            }
            for (const Projectile& shot : v->shots) {
                im.frame_sprites.push_back(Sprite{shot.y * kTilePx + kTilePx * 0.5f,
                                                  shot.x * kTilePx, shot.y * kTilePx, &shot, 0, 0,
                                                  SpriteKind::kShot});
            }
        }
    }

    // Death puffs (F2): every track the gather loop did NOT re-mark is a creature that is no longer
    // in view. If its last position is still inside the chunk rectangle we just swept, it did not
    // scroll off — it died where it stood (the sim reaps on death, so a kill IS a vanish), and it
    // earns a smoke burst. A track whose creature merely left the view is dropped without one.
    for (auto it = im.creature_tracks.begin(); it != im.creature_tracks.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        const int lcx = static_cast<int>(it->second.x) / kChunkTiles;
        const int lcy = static_cast<int>(it->second.y) / kChunkTiles;
        if (lcx >= min_cx && lcx <= max_cx && lcy >= min_cy && lcy <= max_cy) {
            im.death_puffs.push_back(Impl::DeathPuff{it->second.x, it->second.y, 0.0f});
        }
        it = im.creature_tracks.erase(it);
    }

    // The room the player is in, and no other.
    //
    // Drawing every room in view was the first version and it is wrong in a way that is obvious the
    // moment you see it: the rooms are a lattice, so standing in one you look out at the neighbours'
    // living rooms laid out in a grid with black between them. A house has one room in it. What the
    // camera should find outside these four walls is nothing at all — which is exactly what the
    // pack's own Interior.tscn has around its room.
    if (player.map != kOverworld) {
        const int room = room_index_at(static_cast<int>(player.x), static_cast<int>(player.y));
        if (room >= 0 && room < door_count()) {
            const AtlasBig& rs = big_of(Big::kRoom);
            const float bottom =
                static_cast<float>(room_block_y(room) + kRoomY0 - 1 + rs.h) * kTilePx;
            im.frame_sprites.push_back(
                Sprite{bottom,
                       (static_cast<float>(room_block_x(room) + kRoomX0 - 1) +
                        static_cast<float>(rs.w) * 0.5f) * kTilePx,
                       bottom, nullptr, static_cast<std::uint16_t>(Big::kRoom), 0,
                       SpriteKind::kBig});
        }
    }

    // Structures. Gathered over the chunks that overlap the view, EXPANDED downward by one chunk
    // because a house anchored below the view still paints its roof into it.
    //
    // OVERWORLD ONLY. `structures_in_chunk` is indexed by chunk and knows nothing about maps, so
    // without this every interior would have the village that happens to share its coordinates
    // stamped through it.
    if (im.layout != nullptr && player.map == kOverworld) {
        std::vector<std::uint32_t>& vis = im.frame_structures;
        vis.clear();
        for (int cy = min_cy; cy <= max_cy + 1; ++cy) {
            for (int cx = min_cx; cx <= max_cx; ++cx) {
                const auto& list = im.layout->structures_in_chunk(cx, cy);
                vis.insert(vis.end(), list.begin(), list.end());
            }
        }
        // A structure straddling a chunk border is listed by both, so dedup before drawing —
        // otherwise it is painted twice, which is invisible for an opaque sprite and shows up as a
        // double-darkened edge on a translucent one.
        std::sort(vis.begin(), vis.end());
        vis.erase(std::unique(vis.begin(), vis.end()), vis.end());
        const auto& all = im.layout->structures();
        for (const std::uint32_t idx : vis) {
            const Structure& st = all[idx];
            const StructureSize sz = size_of(st.kind);
            // Anchored bottom-LEFT of its footprint, unlike a tree: a structure occupies a known
            // rectangle of tiles and the sprite must land exactly on it.
            im.frame_sprites.push_back(
                Sprite{static_cast<float>(st.ty + sz.h) * kTilePx,
                       (static_cast<float>(st.tx) + static_cast<float>(sz.w) * 0.5f) * kTilePx,
                       static_cast<float>(st.ty + sz.h) * kTilePx, nullptr,
                       static_cast<std::uint16_t>(big_of_structure(st.kind)), 0,
                       SpriteKind::kBig});
        }
    }

    // Forest camps. Same chunk gather + dedup as structures — a parcel straddling a border is listed
    // by both chunks. FLOOR cells (layers 0/1) are drawn flat right here, so they land on top of the
    // ground already painted and under everything the sorted pass draws; STRUCTURE and PROP cells
    // (layers 2/3) join the y-sorted list, so the player walks in front of a tent and behind it. What
    // a cell shows and where it blocks agree because both sides read the same `variant` through the
    // same helpers in prefab_stamp.hpp.
    if (im.layout != nullptr && player.map == kOverworld) {
        std::vector<std::uint32_t>& vis = im.frame_prefabs;
        vis.clear();
        for (int cy = min_cy; cy <= max_cy + 1; ++cy) {
            for (int cx = min_cx; cx <= max_cx; ++cx) {
                const auto& list = im.layout->prefabs_in_chunk(cx, cy);
                vis.insert(vis.end(), list.begin(), list.end());
            }
        }
        std::sort(vis.begin(), vis.end());
        vis.erase(std::unique(vis.begin(), vis.end()), vis.end());
        const auto& all = im.layout->prefabs();
        for (const std::uint32_t idx : vis) {
            const PlacedPrefab& pp = all[idx];
            const PrefabDef& def = kPrefabs[static_cast<int>(pp.id)];
            // Draw from the instance's SKIN, not def.cells: a forest camp may be its deep-forest
            // twin and a snow-ring camp its snow twin. The skin only swaps each cell's atlas rect and
            // drops the tiles a palette cannot voice (a sunflower under snow); dx/dy/layer/group are
            // the base's, so the mirror, the y-sort and the feather all read exactly as before.
            const PrefabSkin& sk = prefab_skin_of(def, pp.skin);
            const bool mir = prefab_mirrored(def, pp.variant);
            for (std::uint16_t ci = 0; ci < sk.cell_count; ++ci) {
                const PrefabCell& c = sk.cells[ci];
                if (!prefab_cell_visible(def, sk, c, pp.variant)) continue;
                const PrefabQuad q = prefab_quad(def, pp, c, mir);
                if (c.layer <= 1) {
                    im.prefab_cell(c, q, mir);  // flat, under the sorted pass
                } else {
                    im.frame_sprites.push_back(Sprite{q.y + q.h, q.x, q.y, &c,
                                                      static_cast<std::uint16_t>(mir ? 1 : 0), 0,
                                                      SpriteKind::kPrefabCell});
                }
            }
        }
    }

    // Trees. Expanded by the full sprite footprint on every side, so a 4-wide, 3-tall tree anchored
    // just outside the view still paints the part that reaches in.
    for (int gy = min_ty - 1; gy <= max_ty + 3; ++gy) {
        for (int gx = min_tx - 4; gx <= max_tx; ++gx) {
            if (player.map != kOverworld) break;  // no woods indoors
            if (!tree_anchor(player.map, gx, gy)) continue;
            // Centred over the middle of the three tiles it claims, then jittered. Anchoring at
            // tx+0.5 was right for a 2-wide sprite and puts a 4-wide one a tile to the left.
            const float bottom =
                static_cast<float>(gy + 1) * kTilePx + scatter_offset(gx, gy, 0x1EAFu, 7.0f);
            im.frame_sprites.push_back(
                Sprite{bottom,
                       (static_cast<float>(gx) + 0.5f * kTreeStride) * kTilePx +
                           scatter_offset(gx, gy, 0x7EEEu, 6.0f),
                       bottom, nullptr,
                       static_cast<std::uint16_t>((tile_variant(player.map, gx, gy) & 1)
                                                      ? Big::kTreePine
                                                      : Big::kTreeBroad),
                       0, SpriteKind::kBig});
        }
    }

    // Players. Every live slot, not just the local one — read from the same published bus the
    // camera uses, so a second player costs one shared_ptr load per frame and nothing else.
    for (int s = 0; s < kMaxPlayers; ++s) {
        PlayerViewPtr pv = players.load(s);
        if (!pv || !pv->live() || pv->map != player.map) continue;
        im.frame_players[s] = pv;  // keeps it alive for the sorted pass
        im.frame_sprites.push_back(Sprite{pv->y * kTilePx + kTilePx * 0.55f, pv->x * kTilePx,
                                          pv->y * kTilePx, nullptr,
                                          static_cast<std::uint16_t>(s), 0, SpriteKind::kPlayer});
    }

    // --- The sorted pass ------------------------------------------------------------------------
    // ONE list, sorted once, drawn once. This is the structural difference between this renderer and
    // the one it replaces, and no amount of art substitutes for it: with terrain, then props, then
    // actors in three fixed passes, a player can only ever be in front of every tree or behind every
    // tree. Sorting them together means walking north puts you behind a trunk and walking south puts
    // you in front of it, with no special case for either.
    //
    // The key is the sprite's FEET, not its top-left corner — `Sprite::sort` is set by each producer
    // above, because where a sprite's feet are is a property of that sprite and not a global
    // constant. A tree's feet are at the bottom of its trunk, three tiles below the top of its art.
    //
    // stable_sort, not sort: two things standing on exactly the same row must keep the order they
    // were gathered in, otherwise they swap depth from frame to frame and flicker.
    std::stable_sort(im.frame_sprites.begin(), im.frame_sprites.end(),
                     [](const Sprite& a, const Sprite& b) { return a.sort < b.sort; });
    for (const Sprite& sp : im.frame_sprites) im.draw_sprite(sp, local_slot);

    // --- Cursor ---------------------------------------------------------------------------------
    // Over the sorted pass, not inside it. The ghost building and the aim ring are UI drawn in world
    // space, not objects standing in the world, so giving them a depth would be a category error —
    // an aim ring that a tree can hide is an aim ring you cannot aim with.
    const Vector2 mw = GetScreenToWorld2D(GetMousePosition(), im.camera);
    const int ctx = static_cast<int>(mw.x) / kTilePx;
    const int cty = static_cast<int>(mw.y) / kTilePx;
    // Only preview when the cursor is somewhere the player is NOT standing. Previously the ghost
    // building was drawn unconditionally, which meant a translucent wall was permanently stuck to
    // the character — most obvious in screenshots, where the mouse sits at screen centre, i.e.
    // exactly on the player.
    const int ptx = static_cast<int>(player.x);
    const int pty = static_cast<int>(player.y);
    if (im.building && (ctx != ptx || cty != pty)) {
        im.tile(im.selected == BuildKind::kHearth ? Slot::kBuildHearth : Slot::kBuildPlot, ctx,
                cty, Color{255, 255, 255, 100});
        DrawRectangleLines(ctx * kTilePx, cty * kTilePx, kTilePx, kTilePx,
                           Color{255, 255, 255, 150});
    } else if (!im.building) {
        // In combat mode the cursor is an aim point, not a placement. It gets a ring rather than a
        // ghost building, and the ring is the SPELL's radius — so the player can see what a cast
        // would cover before spending the mana.
        DrawCircleLines(static_cast<int>(mw.x), static_cast<int>(mw.y), kSpellRadius * kTilePx,
                        Color{200, 220, 255, 110});
    }

    // --- Ability zones, over the fighters ---------------------------------------------------------
    // Rain over a wet zone, drifting smoke over a smoke zone. Like the weather ambience, every
    // particle's position is a closed-form function of (its index, the zone centre, the WORLD clock)
    // — no stored state, no per-frame RNG — so two machines watching the same zone see the same
    // haze and a screenshot at a given world time is reproducible. The per-particle Rng is seeded
    // from the index and the zone's tile, which is fixed, not from the frame.
    {
        const double zt =
            static_cast<double>(status.world_ms.load(std::memory_order_relaxed)) / 1000.0;
        for (int cy = min_cy; cy <= max_cy; ++cy) {
            for (int cx = min_cx; cx <= max_cx; ++cx) {
                ChunkViewPtr v = bus.load(ChunkCoord{player.map, static_cast<std::uint16_t>(cx),
                                                     static_cast<std::uint16_t>(cy)});
                if (!v) continue;
                for (const Zone& z : v->zones) {
                    const float cxp = z.x * kTilePx;
                    const float cyp = z.y * kTilePx;
                    const float rpx = z.radius * kTilePx;
                    const std::uint64_t base =
                        (static_cast<std::uint64_t>(static_cast<int>(z.x)) << 20) ^
                        static_cast<std::uint64_t>(static_cast<int>(z.y)) ^
                        (z.kind == ZoneKind::kWet ? 0x5A17ull : 0x5A2Bull);
                    // Fade the whole zone out over its last second so it does not pop off.
                    const float life = z.ticks_left < 10 ? static_cast<float>(z.ticks_left) / 10.0f
                                                         : 1.0f;
                    if (z.kind == ZoneKind::kWet) {
                        // A scatter of drops falling through the circle; density tracks area.
                        const int drops = std::clamp(static_cast<int>(z.radius * z.radius * 2.5f), 8, 44);
                        for (int i = 0; i < drops; ++i) {
                            Rng r(static_cast<std::uint64_t>(i) * 0x9E37'79B9ull ^ base);
                            const float ang = r.unit() * 6.2831853f;
                            const float rad = std::sqrt(r.unit()) * rpx;
                            const float bx = cxp + std::cos(ang) * rad;
                            const float span = rpx * 1.2f + static_cast<float>(kTilePx);
                            const float fall = std::fmod(static_cast<float>(zt) * 520.0f * (0.6f + r.unit()) +
                                                             r.unit() * span,
                                                         span);
                            const float by = cyp + std::sin(ang) * rad * 0.6f - rpx * 0.6f + fall;
                            im.fx(Fx::kRain, static_cast<int>(zt * 14.0) + i, bx, by, 0.55f * life);
                        }
                    } else {
                        // A few overlapping puffs, drifting slowly and cycling their frames.
                        const int puffs = std::clamp(static_cast<int>(z.radius * z.radius * 1.1f), 5, 16);
                        for (int i = 0; i < puffs; ++i) {
                            Rng r(static_cast<std::uint64_t>(i) * 0x85EB'CA6Bull ^ base);
                            const float ang = r.unit() * 6.2831853f;
                            const float rad = std::sqrt(r.unit()) * rpx;
                            const float drift = std::sin(static_cast<float>(zt) * 0.6f + r.unit() * 6.28f) *
                                                static_cast<float>(kTilePx) * 0.4f;
                            const float bx = cxp + std::cos(ang) * rad + drift;
                            const float by = cyp + std::sin(ang) * rad * 0.7f;
                            im.fx(Fx::kSmoke, static_cast<int>(zt * 6.0) + i * 2, bx, by, 0.5f * life,
                                  0.0f, 1.5f);
                        }
                    }
                }
            }
        }
    }

    // --- Players ---------------------------------------------------------------------------------
    // Every live slot, not just the local one. They are read from the same published bus the camera
    // uses, so a second player costs one more shared_ptr load per frame and nothing else.
    //
    // The world tick, read once: `world_tick - p.last_swing_tick` under a small window is how the
    // deluxe attack animation fires for ANY player, local or remote, off published state alone. That
    // is what lets a --shot of a staged swing show the swing (the pose no longer lives in poll_input).
    const std::uint64_t world_tick = status.tick.load(std::memory_order_relaxed);
    constexpr std::uint64_t kAttackWindowTicks = 4;  // 4 frames, one per tick (~0.4s at 10 t/s)
    const double now = GetTime();
    for (int s = 0; s < kMaxPlayers; ++s) {
        PlayerViewPtr pv = players.load(s);
        if (!pv || !pv->live() || pv->map != player.map) continue;
        const PlayerView& p = *pv;
        // A dead player is a faint ghost lying where they fell, not an absence: watching a friend
        // go down and waiting for them is a moment, and despawning them deletes it.
        const bool dead = p.dead_ticks > 0;
        DrawEllipse(static_cast<int>(p.x * kTilePx), static_cast<int>(p.y * kTilePx + 7),
                    kTilePx * 0.30f, kTilePx * 0.15f,
                    Color{0, 0, 0, static_cast<unsigned char>(dead ? 40 : 80)});
        const Color tint = dead ? Color{255, 255, 255, 90} : WHITE;
        if (p.mounted) {
            // Mounted: unchanged. The mount is drawn under the rider, one tile lower and slightly
            // larger, and the rider stays on the plain 16px rig — the deluxe rig is a walk/idle/swing
            // set that has no saddle pose, and a katana swing from horseback is disallowed anyway.
            im.anim(Anim::kHorse, static_cast<int>(p.facing), static_cast<int>(p.steps / 5),
                    p.x * kTilePx, p.y * kTilePx + 3.0f, kTilePx * 1.3f);
            im.anim(Anim::kPlayer, static_cast<int>(p.facing), static_cast<int>(p.steps / 4),
                    p.x * kTilePx, p.y * kTilePx - 5.0f, kTilePx * 1.1f, tint);
        } else {
            // On foot: the pack's DELUXE 32px rig. Drawn at 2x the 16px quad so the centred body
            // lands at the same size and its feet on the same point (see `deluxe`).
            const float cx = p.x * kTilePx;
            const float cy = p.y * kTilePx;
            const float size = kTilePx * 1.1f * 2.0f;
            const float px_scale = static_cast<float>(kTilePx) / static_cast<float>(kAtlasTile);
            const int facing = static_cast<int>(p.facing);

            // Idle vs walk: a change in the authoritative `steps` since last frame means moving; we
            // hold "walking" briefly past the last step so a pause between tiles does not flicker.
            if (p.steps != im.player_prev_steps[s]) {
                im.player_prev_steps[s] = p.steps;
                im.player_walk_until[s] = now + 0.18;
            }
            const bool walking = now < im.player_walk_until[s];

            // Attacking: derived purely from published state, so it is correct for a remote player
            // and provable in a --shot. Frame steps once per tick across the window.
            const bool attacking =
                !dead && p.last_swing_tick != 0 && world_tick >= p.last_swing_tick &&
                (world_tick - p.last_swing_tick) < kAttackWindowTicks;
            const int attack_frame =
                attacking ? static_cast<int>(world_tick - p.last_swing_tick) : 0;

            if (!attacking) {
                // Katana on the back BEHIND the body when facing up (blade on the far shoulder).
                im.carry_katana(p.facing, cx, cy, px_scale, /*back=*/true, tint);
                const Deluxe body = walking ? Deluxe::kWalk : Deluxe::kIdle;
                // `steps` drives the walk exactly as the 16px rig did; idle cycles slowly on the
                // wall clock (it is ambient, not simulation-authoritative).
                const int frame =
                    walking ? static_cast<int>(p.steps / 4) : static_cast<int>(now * 2.5);
                im.deluxe(body, facing, frame, cx, cy, size, tint);
                // Katana on the back IN FRONT of the body for every other facing.
                im.carry_katana(p.facing, cx, cy, px_scale, /*back=*/false, tint);
            } else {
                // Swing: attack body + its frame-aligned katana overlay (blade + baked swoosh).
                im.deluxe(Deluxe::kAttack, facing, attack_frame, cx, cy, size, tint);
                im.deluxe(Deluxe::kKatanaAttack, facing, attack_frame, cx, cy, size, tint);
            }
        }
        if (s != local_slot) {
            // Somebody else's health bar, always shown — you cannot help a stranger you cannot read.
            const int w = std::max(1, (p.hp * (kTilePx - 4)) / std::max<int>(1, p.max_hp));
            DrawRectangle(static_cast<int>(p.x * kTilePx) - kTilePx / 2 + 2,
                          static_cast<int>(p.y * kTilePx) - kTilePx / 2 - 6, kTilePx - 4, 3,
                          Color{0, 0, 0, 140});
            DrawRectangle(static_cast<int>(p.x * kTilePx) - kTilePx / 2 + 2,
                          static_cast<int>(p.y * kTilePx) - kTilePx / 2 - 6, w, 3,
                          Color{90, 220, 120, 255});
        }
    }

    // --- Combat effects, over the fighters ---------------------------------------------------------
    // A second sweep of the same chunks rather than drawing these inline, because an arc has to land
    // ON TOP of whatever it hit — including the player, who is drawn after the creatures are.
    for (int cy = min_cy; cy <= max_cy; ++cy) {
        for (int cx = min_cx; cx <= max_cx; ++cx) {
            ChunkViewPtr v = bus.load(ChunkCoord{player.map, static_cast<std::uint16_t>(cx),
                                                 static_cast<std::uint16_t>(cy)});
            if (!v) continue;
            for (const Effect& e : v->effects) {
                // `age` is simulation ticks; the sprite has more frames than that, so the strip is
                // stepped proportionally. The effect therefore plays at the same rate on every
                // machine no matter what frame rate it is running at.
                const int frames = fx_of(fx_of_effect(e.kind)).frames;
                const int life = effect_life_of(e.kind);
                const int f = std::min<int>(frames - 1, (e.age * frames) / life);
                const float fade = 1.0f - static_cast<float>(e.age) / static_cast<float>(life);
                // 0.6x for the elemental strips: the pack draws them for a 1:1 pixel game, so at this
                // game's 2x tile scale an explosion came out two and a half tiles across and buried
                // the creature it went off on — the whole point of a flash is to show you what
                // happened to something, so it must not hide it. Smoke is the EXCEPTION and draws big
                // (1.6x): it is a monster's attack telegraph (F2), the puff thrown at a wind-up's
                // commit, and it has to read as "incoming" against grass at game scale — a half-tile
                // wisp did not. It sits over the creature on purpose; that IS the tell.
                const bool is_smoke = e.kind == EffectKind::kSmoke;
                im.fx(fx_of_effect(e.kind), f, e.x * kTilePx, e.y * kTilePx,
                      (is_smoke ? 0.7f : 0.55f) + 0.45f * fade, 0.0f, is_smoke ? 1.6f : 0.6f);
            }
        }
    }

    // Death puffs (F2): the client-side smoke burst where a creature vanished, in the same sweep so
    // it sits over the fighters. Drawn a touch larger (2x) than a combat flash — with no corpse and
    // no death animation in the pack, this puff is the entire "it died" read and has to carry it.
    {
        const AtlasFx& smoke = fx_of(Fx::kSmoke);
        for (const Impl::DeathPuff& d : im.death_puffs) {
            const float t = std::clamp(d.age / Impl::kDeathPuffLife, 0.0f, 1.0f);
            const int f = std::min(smoke.frames - 1, static_cast<int>(t * smoke.frames));
            im.fx(Fx::kSmoke, f, d.x * kTilePx, d.y * kTilePx, 0.85f * (1.0f - t), 0.0f, 2.0f);
        }
    }

    // --- Ambience, over everything in the world ---------------------------------------------------
    // Last inside the camera so leaves pass in FRONT of the player and the houses, which is what
    // sells them as being between the viewer and the world rather than painted on the ground.
    // Driven by the world clock, not the frame clock — see `draw_ambience`.
    im.draw_ambience(player.map, im.camera.target.x, im.camera.target.y,
                     static_cast<double>(status.world_ms.load(std::memory_order_relaxed)) / 1000.0);

    // Night tint: one screen-space overlay over the finished world, rather than a darkened colour
    // per tile. Drawn inside the 2D camera would scale it with zoom, so it goes after EndMode2D.
    EndMode2D();
    if (night) DrawRectangle(0, 0, im.width, im.height, Color{20, 24, 80, 90});
    // Being hit and being dead are the two states a player must never have to read a number to
    // know. A red wash for the countdown, a heavier one while down.
    // Measured by looking: the first attempt used alpha 130, which does not read as "you are hurt",
    // it reads as "the grass is red". A wash over the whole frame is far stronger than it looks in
    // the source — the eye has nothing unwashed left to compare it against.
    if (player.dead_ticks > 0) {
        DrawRectangle(0, 0, im.width, im.height, Color{90, 10, 10, 70});
    } else if (player.live() && player.hp * 4 < player.max_hp) {
        DrawRectangle(0, 0, im.width, im.height, Color{140, 20, 20, 40});
    }

    // NO HUD HERE. The world renderer draws the world; the shell (src/ui/screens.cpp) draws
    // everything a player reads. Keeping them apart is what let the engine counters move off the
    // play area and into the F3 overlay without touching a line of world-drawing code.
    //
    // The two counts the debug overlay wants are stashed for `drawn_chunks()` / `drawn_mobs()`.
    im.last_chunks = drawn_chunks;
    im.last_creatures = drawn_creatures;
}

void RaylibBridge::set_layout(const WorldLayout* layout) { impl_->layout = layout; }

BuildKind RaylibBridge::selected_build() const { return impl_->selected; }
Element RaylibBridge::selected_element() const { return impl_->element; }
bool RaylibBridge::build_mode() const { return impl_->building; }
int RaylibBridge::drawn_chunks() const { return impl_->last_chunks; }
int RaylibBridge::drawn_creatures() const { return impl_->last_creatures; }

void RaylibBridge::end_frame() { EndDrawing(); }

void RaylibBridge::screenshot(const char* path) const { TakeScreenshot(path); }

InputFrame RaylibBridge::poll_input(const PlayerView& player) const {
    Impl& im = *impl_;
    InputFrame in{};

    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) in.move_y -= 1.0f;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) in.move_y += 1.0f;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) in.move_x -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) in.move_x += 1.0f;

    // Normalise so diagonal movement is not faster than orthogonal.
    const float len = std::sqrt(in.move_x * in.move_x + in.move_y * in.move_y);
    if (len > 0.0f) {
        in.move_x /= len;
        in.move_y /= len;
    }

    if (IsKeyPressed(KEY_B)) im.building = !im.building;
    in.build_mode = im.building;

    // The number row means different things in the two modes, and only two of the four slots exist
    // in build mode. That asymmetry is the honest one: there are two things to build and four
    // schools of magic.
    if (im.building) {
        if (IsKeyPressed(KEY_ONE)) im.selected = BuildKind::kHearth;
        if (IsKeyPressed(KEY_TWO)) im.selected = BuildKind::kPlot;
    } else {
        if (IsKeyPressed(KEY_ONE)) im.element = Element::kFire;
        if (IsKeyPressed(KEY_TWO)) im.element = Element::kIce;
        if (IsKeyPressed(KEY_THREE)) im.element = Element::kEarth;
        if (IsKeyPressed(KEY_FOUR)) im.element = Element::kShock;
    }
    in.build_kind = im.selected;
    in.element = im.element;
    in.mount = IsKeyPressed(KEY_R);

    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        im.camera.zoom = std::clamp(im.camera.zoom + wheel * 0.1f, 0.35f, 3.0f);
    }

    const Vector2 mw = GetScreenToWorld2D(GetMousePosition(), im.camera);
    const int tx = static_cast<int>(mw.x) / kTilePx;
    const int ty = static_cast<int>(mw.y) / kTilePx;
    in.aim_x = mw.x / static_cast<float>(kTilePx);
    in.aim_y = mw.y / static_cast<float>(kTilePx);
    if (tx >= 0 && ty >= 0 && tx < kMapTiles && ty < kMapTiles) {
        in.cursor_tx = static_cast<std::uint16_t>(tx);
        in.cursor_ty = static_cast<std::uint16_t>(ty);
        in.harvest = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) || IsKeyPressed(KEY_E);
        in.till = IsKeyPressed(KEY_T);
        in.upgrade = IsKeyPressed(KEY_U);
        if (im.building) {
            in.build = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
            in.plant = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        } else {
            // Held, not pressed: a fight is a rhythm, and a cooldown is what paces it rather than
            // how fast the player can click.
            //
            // The cooldown here is a CLIENT convenience, not the rule. The rule is stamina, which
            // the trusted actor debits atomically — a client that ignored this timer would simply
            // exhaust itself and be refused. What the timer buys is that holding the button does
            // not fire sixty asks a second to be told "no" fifty-seven times.
            im.swing_cd = std::max(0.0f, im.swing_cd - GetFrameTime());
            const bool want = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            const bool heavy = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            in.swing = want && im.swing_cd <= 0.0f;
            if (in.swing) {
                im.swing_cd = static_cast<float>(heavy ? kHeavyCooldown : kSwingCooldown) /
                              static_cast<float>(kTicksPerSecond);
                // Hold the swing pose briefly. Heavy uses the same pose — the pack ships one attack
                // frame per facing and no separate charged one.
                im.attack_pose = 0.18f;
            }
            in.heavy = in.swing && heavy;
            in.cast = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
            in.shoot = IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_SPACE);
            // The two ability slots. F and G, edge-triggered — a discrete event like a swing, not a
            // held rhythm, so `Pressed` not `Down`. Both keys are otherwise free (see the input map
            // in the journal). The cooldown that paces them lives in the trusted actor, not here.
            in.ability_a = IsKeyPressed(KEY_F);
            in.ability_b = IsKeyPressed(KEY_G);
        }
    }

    (void)player;
    in.quit = WindowShouldClose();
    return in;
}

}  // namespace mmo
