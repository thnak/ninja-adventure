#include "render/raylib_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
// A tile is an anchor iff it is a tree and an EVEN number of trees precede it in its row, so a run
// of N tree tiles yields floor(N/2) non-overlapping trees. Terrain is a pure function, so this can
// be asked about any tile without a chunk view — including tiles just off-screen, which is what
// stops canopies being clipped at the view edge.
[[nodiscard]] bool tree_at(std::uint16_t map, int x, int y) {
    if (x < 0 || y < 0 || x >= kMapTiles || y >= kMapTiles) return false;
    return terrain_of(kWorldSeed, map, x, y) == Terrain::kTree;
}

[[nodiscard]] bool tree_anchor(std::uint16_t map, int x, int y) {
    if (!tree_at(map, x, y)) return false;
    int run = 0;
    while (run < 64 && tree_at(map, x - 1 - run, y)) ++run;
    return (run % 2) == 0;
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
    int last_chunks = 0;
    int last_creatures = 0;
    const WorldLayout* layout = nullptr;

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
    void draw_ambience(std::uint16_t map, float cam_x, float cam_y, double t) const {
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

    for (int cy = min_cy; cy <= max_cy; ++cy) {
        for (int cx = min_cx; cx <= max_cx; ++cx) {
            const ChunkCoord c{player.map, static_cast<std::uint16_t>(cx),
                               static_cast<std::uint16_t>(cy)};
            ChunkViewPtr v = bus.load(c);
            if (!v) continue;  // this chunk has not ticked yet (or is still in flight from a peer)
            ++drawn_chunks;

            const int base_x = cx * kChunkTiles;
            const int base_y = cy * kChunkTiles;

            // Base terrain first, in its own pass. Trees follow in a second pass because a tree is
            // two tiles tall — its canopy occupies the tile ABOVE, which must already be painted.
            for (int ly = 0; ly < kChunkTiles; ++ly) {
                const int gy = base_y + ly;
                if (gy < min_ty || gy > max_ty) continue;
                for (int lx = 0; lx < kChunkTiles; ++lx) {
                    const int gx = base_x + lx;
                    if (gx < min_tx || gx > max_tx) continue;
                    const auto t = static_cast<Terrain>(v->terrain[ly * kChunkTiles + lx]);
                    // Trees are drawn over the ring's own ground, so a forest in the snow ring
                    // stands on snow. Without this every wood looked like it had been transplanted
                    // onto a lawn.
                    const Slot first = (t == Terrain::kTree)
                                           ? slot_of(ground_under_tree(ring_of(kWorldSeed, gx, gy)))
                                           : slot_of(t);
                    // Pick one of the terrain's textured variants, then mirror it. Mirroring alone
                    // was not enough: these motifs are legible enough that a single repeated tile
                    // reads as wallpaper no matter how it is flipped.
                    const int v = tile_variant(c.map, gx, gy);
                    const int pick = (v >> 2) % kTerrainVariants;
                    const auto base = static_cast<Slot>(static_cast<int>(first) + pick);
                    im.tile(base, gx, gy, WHITE, v & 3);
                }
            }
            for (const Crop& cr : v->crops) {
                // Size tracks growth: a seedling is drawn at 45% of a tile, a ripe crop fills it.
                const float ratio = 0.45f + 0.55f * (static_cast<float>(cr.stage) /
                                                     static_cast<float>(kCropStages - 1));
                im.sprite(slot_of(cr), (static_cast<float>(cr.tx) + 0.5f) * kTilePx,
                          (static_cast<float>(cr.ty) + 0.5f) * kTilePx, kTilePx * ratio);
            }

            for (const Building& b : v->buildings) {
                im.tile(b.kind == BuildKind::kHearth ? Slot::kBuildHearth : Slot::kBuildPlot,
                        b.tx, b.ty);

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
                    DrawRectangle(b.tx * kTilePx, b.ty * kTilePx - 4, w, 3, Color{220, 70, 70, 255});
                }
            }

            for (const Creature& m : v->creatures) {
                // A flat ellipse under each creature: with no height in the art, this is what stops
                // the sprites reading as decals lying on the ground.
                DrawEllipse(static_cast<int>(m.x * kTilePx), static_cast<int>(m.y * kTilePx + 6),
                            kTilePx * 0.28f, kTilePx * 0.14f, Color{0, 0, 0, 70});
                // Offsetting the frame by the id keeps a whole wave from stepping in unison, which
                // reads as one organism rather than a crowd.
                const int frame = static_cast<int>((v->tick / 3 + m.id) & 0xFF);
                im.anim(anim_of(m.kind), static_cast<int>(m.facing), frame, m.x * kTilePx,
                        m.y * kTilePx, kTilePx * 1.0f, tint_of(m.status));

                // A health bar only once it has been hurt: an untouched field of animals with bars
                // over every one of them reads as a bestiary rather than as a meadow.
                if (m.hp < m.max_hp && m.max_hp > 0) {
                    const int w = std::max(1, (m.hp * (kTilePx - 6)) / m.max_hp);
                    DrawRectangle(static_cast<int>(m.x * kTilePx) - kTilePx / 2 + 3,
                                  static_cast<int>(m.y * kTilePx) - kTilePx / 2 - 3, kTilePx - 6, 3,
                                  Color{0, 0, 0, 120});
                    DrawRectangle(static_cast<int>(m.x * kTilePx) - kTilePx / 2 + 3,
                                  static_cast<int>(m.y * kTilePx) - kTilePx / 2 - 3, w, 3,
                                  Color{220, 70, 70, 255});
                }
                // One pixel of intent: an angry creature gets a mark. Disposition is state, and
                // state the player cannot see is state that might as well not exist — this is the
                // difference between "the boar attacked me for no reason" and "I annoyed a boar".
                if (m.anger_ticks > 0 && m.disposition != Disposition::kTimid) {
                    DrawRectangle(static_cast<int>(m.x * kTilePx) - 1,
                                  static_cast<int>(m.y * kTilePx) - kTilePx / 2 - 9, 3, 5,
                                  Color{255, 90, 60, 230});
                }
                ++drawn_creatures;
            }

            // Arrows, pointed the way they are travelling. `Fx::kArrow` is one sprite drawn
            // rotated — the only rotated sprite in the game, and the reason arrows live in the FX
            // strip rather than the tile grid.
            for (const Projectile& p : v->shots) {
                const float deg = std::atan2(p.vy, p.vx) * 57.2957795f;
                im.fx(Fx::kArrow, 0, p.x * kTilePx, p.y * kTilePx, 1.0f, deg + 90.0f);
            }
        }
    }

    // --- Tall things: trees and buildings ---------------------------------------------------------
    // One pass over the whole visible rect rather than per chunk, EXPANDED by the sprite footprint
    // so a tree or a house anchored just off-screen still paints the part that reaches into view.
    //
    // Trees and structures are interleaved by ROW rather than drawn in two passes, because they
    // stand in the same world: a house behind a tree has to be drawn first, and a house in front of
    // one has to be drawn second. Two separate passes get one of those two cases wrong no matter
    // which order they run in.
    std::vector<std::uint32_t> vis;
    if (im.layout != nullptr) {
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
        std::stable_sort(vis.begin(), vis.end(), [&](std::uint32_t a, std::uint32_t b) {
            return all[a].ty < all[b].ty;
        });
    }

    std::size_t next_structure = 0;
    for (int gy = min_ty; gy <= max_ty + 3; ++gy) {
        if (im.layout != nullptr) {
            const auto& all = im.layout->structures();
            while (next_structure < vis.size() && all[vis[next_structure]].ty < gy) {
                ++next_structure;  // anchored above the view; its art does not reach in
            }
            while (next_structure < vis.size() && all[vis[next_structure]].ty == gy) {
                const Structure& s = all[vis[next_structure]];
                const StructureSize sz = size_of(s.kind);
                // Anchored bottom-LEFT of its footprint, unlike a tree: a structure occupies a
                // known rectangle of tiles and the sprite must land exactly on it.
                im.big_at(big_of_structure(s.kind),
                          (static_cast<float>(s.tx) + static_cast<float>(sz.w) * 0.5f) * kTilePx,
                          static_cast<float>(s.ty + sz.h) * kTilePx);
                ++next_structure;
            }
        }
        for (int gx = min_tx - 2; gx <= max_tx; ++gx) {
            if (!tree_anchor(player.map, gx, gy)) continue;
            im.big((tile_variant(player.map, gx, gy) & 1) ? Big::kTreePine : Big::kTreeBroad, gx, gy);
        }
    }

    // --- Cursor (drawn BEFORE the player, so the build preview never hides them) ------------------------------------------------------------------------------------
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

    // --- Players ---------------------------------------------------------------------------------
    // Every live slot, not just the local one. They are read from the same published bus the camera
    // uses, so a second player costs one more shared_ptr load per frame and nothing else.
    for (int s = 0; s < kMaxPlayers; ++s) {
        PlayerViewPtr pv = players.load(s);
        if (!pv || !pv->live() || pv->map != player.map) continue;
        const PlayerView& p = *pv;
        // A dead player is a faint ghost lying where they fell, not an absence: watching a friend
        // go down and waiting for them is a moment, and despawning them deletes it.
        const bool dead = p.dead_ticks > 0;
        DrawEllipse(static_cast<int>(p.x * kTilePx), static_cast<int>(p.y * kTilePx + 7),
                    kTilePx * 0.30f, kTilePx * 0.15f, Color{0, 0, 0, dead ? 40 : 80});
        if (p.mounted) {
            // The mount is drawn under the rider, one tile lower and slightly larger.
            im.anim(Anim::kHorse, static_cast<int>(p.facing), static_cast<int>(p.steps / 5),
                    p.x * kTilePx, p.y * kTilePx + 3.0f, kTilePx * 1.3f);
        }
        // `steps` is the authoritative move counter from PlayerActor, not a local frame timer — so
        // the walk cycle stays in step with the simulation rather than with this client's frame
        // rate. It is also what makes a REMOTE player's animation correct for free.
        im.anim(Anim::kPlayer, static_cast<int>(p.facing), static_cast<int>(p.steps / 4),
                p.x * kTilePx, p.y * kTilePx - (p.mounted ? 5.0f : 0.0f), kTilePx * 1.1f,
                dead ? Color{255, 255, 255, 90} : WHITE);
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
                const int f = std::min<int>(frames - 1, (e.age * frames) / kEffectLife);
                const float fade = 1.0f - static_cast<float>(e.age) / static_cast<float>(kEffectLife);
                // 0.6x. The pack's elemental strips are drawn for a 1:1 pixel game, so at this
                // game's 2x tile scale an explosion came out two and a half tiles across and buried
                // the creature it went off on — the whole point of the flash is to show you what
                // happened to something, so it must not hide it.
                im.fx(fx_of_effect(e.kind), f, e.x * kTilePx, e.y * kTilePx, 0.55f + 0.45f * fade,
                      0.0f, 0.6f);
            }
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
            }
            in.heavy = in.swing && heavy;
            in.cast = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
            in.shoot = IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_SPACE);
        }
    }

    (void)player;
    in.quit = WindowShouldClose();
    return in;
}

}  // namespace mmo
