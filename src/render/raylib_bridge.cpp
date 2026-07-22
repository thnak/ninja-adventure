#include "render/raylib_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "raylib.h"
#include "render/atlas_slots.hpp"

namespace mmo {
namespace {

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
        // A tree is an OVERLAY: the base layer under it is whatever the surrounding ring uses, so
        // a forest in the snow ring stands on snow rather than on grass.
        case Terrain::kTree: return Slot::kTerrainGrass;
        case Terrain::kCount: break;
    }
    return Slot::kTerrainGrass;
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

[[nodiscard]] Anim anim_of(MobKind k) {
    switch (k) {
        case MobKind::kSlime: return Anim::kMobSlime;
        case MobKind::kSpider: return Anim::kMobSpider;
        case MobKind::kGhost: return Anim::kMobSpirit;
    }
    return Anim::kMobSlime;
}

[[nodiscard]] Slot slot_of(BuildKind k) {
    switch (k) {
        case BuildKind::kHearth: return Slot::kBuildHearth;
        case BuildKind::kWall: return Slot::kBuildWall;
        case BuildKind::kTurret: return Slot::kBuildTurret;
        case BuildKind::kPlot: return Slot::kBuildPlot;
    }
    return Slot::kBuildWall;
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

}  // namespace

struct RaylibBridge::Impl {
    Camera2D camera{};
    int width = 0;
    int height = 0;
    BuildKind selected = BuildKind::kWall;
    Texture2D atlas{};
    bool atlas_ok = false;
    int last_chunks = 0;
    int last_mobs = 0;
    std::vector<std::pair<int, int>> camps;

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

    // One frame of an animation sheet. `dir` is a `Facing`, `frame` a free-running counter — both
    // are wrapped inside `anim_frame`, so a two-facing animal can be handed a four-way facing.
    void anim(Anim a, int dir, int frame, float cx, float cy, float size,
              Color tint = WHITE) const {
        if (!atlas_ok) return;
        const AtlasRect r = anim_frame(a, dir, frame);
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
    if (impl_->atlas_ok) UnloadTexture(impl_->atlas);
    if (IsWindowReady()) CloseWindow();
}

bool RaylibBridge::begin_frame() { return !WindowShouldClose(); }

float RaylibBridge::frame_time() const { return GetFrameTime(); }

void RaylibBridge::draw(const SnapshotBus& bus, const WorldStatus& status,
                        const PlayerView& player) {
    Impl& im = *impl_;
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
    int drawn_mobs = 0;

    // --- Wall autotiling pre-pass -----------------------------------------------------------------
    // Which tile a wall segment uses depends on its NEIGHBOURS, and a wall run crosses chunk borders
    // freely. Gathering positions from every visible chunk first — rather than autotiling each chunk
    // in isolation — is what stops a wall from visibly changing style at a chunk boundary. The
    // boundaries are supposed to be invisible; that is the whole point of the demo.
    std::unordered_set<std::uint32_t> walls;
    std::unordered_set<std::uint32_t> fences;
    for (int cy = min_cy; cy <= max_cy; ++cy) {
        for (int cx = min_cx; cx <= max_cx; ++cx) {
            ChunkViewPtr v = bus.load(ChunkCoord{player.map, static_cast<std::uint16_t>(cx),
                                                 static_cast<std::uint16_t>(cy)});
            if (!v) continue;
            for (const Building& b : v->buildings) {
                const std::uint32_t key = (static_cast<std::uint32_t>(b.tx) << 16) | b.ty;
                if (b.kind == BuildKind::kWall) walls.insert(key);
                if (b.kind == BuildKind::kFence) fences.insert(key);
            }
        }
    }
    const auto in_set = [](const std::unordered_set<std::uint32_t>& set, int tx, int ty) {
        if (tx < 0 || ty < 0 || tx >= kMapTiles || ty >= kMapTiles) return false;
        return set.count((static_cast<std::uint32_t>(tx) << 16) |
                         static_cast<std::uint32_t>(ty)) != 0;
    };
    const auto is_wall = [&](int tx, int ty) { return in_set(walls, tx, ty); };
    const auto is_fence = [&](int tx, int ty) { return in_set(fences, tx, ty); };

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
                if (b.kind == BuildKind::kWall) {
                    // The crenellated tile runs ALONG the wall, so it is rotated to match the run's
                    // axis. A lone tile or a corner/T-junction has no single axis, so it falls back
                    // to plain brick, which has no directional edges to get wrong.
                    const bool h = is_wall(b.tx - 1, b.ty) || is_wall(b.tx + 1, b.ty);
                    const bool vert = is_wall(b.tx, b.ty - 1) || is_wall(b.tx, b.ty + 1);
                    if (h && !vert) {
                        im.tile(Slot::kBuildWallRun, b.tx, b.ty);
                    } else if (vert && !h) {
                        im.tile(Slot::kBuildWallRun, b.tx, b.ty, WHITE, 0, 90.0f);
                    } else {
                        im.tile(Slot::kBuildWall, b.tx, b.ty);
                    }
                } else if (b.kind == BuildKind::kFence) {
                    // Same scheme as the wall, but the fence sprite is VERTICAL by default, so the
                    // rotation is applied to horizontal runs instead.
                    const bool h = is_fence(b.tx - 1, b.ty) || is_fence(b.tx + 1, b.ty);
                    const bool vert = is_fence(b.tx, b.ty - 1) || is_fence(b.tx, b.ty + 1);
                    if (vert && !h) {
                        im.tile(Slot::kBuildFence, b.tx, b.ty);
                    } else if (h && !vert) {
                        im.tile(Slot::kBuildFence, b.tx, b.ty, WHITE, 0, 90.0f);
                    } else {
                        im.tile(Slot::kBuildFencePost, b.tx, b.ty);
                    }
                } else {
                    im.tile(slot_of(b.kind), b.tx, b.ty);
                }

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

            for (const Mob& m : v->mobs) {
                // A flat ellipse under each mob: with no height in the art, this is what stops the
                // sprites reading as decals lying on the ground.
                DrawEllipse(static_cast<int>(m.x * kTilePx), static_cast<int>(m.y * kTilePx + 6),
                            kTilePx * 0.28f, kTilePx * 0.14f, Color{0, 0, 0, 70});
                // Offsetting the frame by the mob id keeps a whole wave from stepping in unison,
                // which reads as one organism rather than a crowd.
                const int frame = static_cast<int>((v->tick / 3 + m.id) & 0xFF);
                im.anim(anim_of(m.kind), static_cast<int>(m.facing), frame, m.x * kTilePx,
                        m.y * kTilePx, kTilePx * 1.0f);
                ++drawn_mobs;
            }
        }
    }

    // --- Trees ------------------------------------------------------------------------------------
    // One pass over the whole visible rect rather than per chunk, EXPANDED by the sprite footprint
    // (2 wide, 3 tall) so a tree anchored just off-screen still paints the canopy that reaches into
    // view. Row order is top-down, so a nearer tree's canopy overlaps the one behind it.
    for (int gy = min_ty; gy <= max_ty + 3; ++gy) {
        for (int gx = min_tx - 2; gx <= max_tx; ++gx) {
            if (!tree_anchor(player.map, gx, gy)) continue;
            im.big((tile_variant(player.map, gx, gy) & 1) ? Big::kTreePine : Big::kTreeBroad, gx, gy);
        }
    }

    // --- Spawn camps ------------------------------------------------------------------------------
    // Marked permanently, not only at night: knowing where a wave will come from is what makes
    // choosing where to spend wood a decision rather than a guess.
    for (const auto& [ctx, cty] : im.camps) {
        im.tile(Slot::kSpawnCamp, ctx, cty);
        DrawCircleLines(static_cast<int>((ctx + 0.5f) * kTilePx),
                        static_cast<int>((cty + 0.5f) * kTilePx), kTilePx * 1.4f,
                        Color{255, 90, 90, 140});
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
    if (ctx != ptx || cty != pty) {
        im.tile(slot_of(im.selected), ctx, cty, Color{255, 255, 255, 100});
        DrawRectangleLines(ctx * kTilePx, cty * kTilePx, kTilePx, kTilePx,
                           Color{255, 255, 255, 150});
    }

    // --- Player ----------------------------------------------------------------------------------
    DrawEllipse(static_cast<int>(player.x * kTilePx), static_cast<int>(player.y * kTilePx + 7),
                kTilePx * 0.30f, kTilePx * 0.15f, Color{0, 0, 0, 80});
    // `steps` is the authoritative move counter from PlayerActor, not a local frame timer — so the
    // walk cycle stays in step with the simulation rather than with this client's frame rate.
    im.anim(Anim::kPlayer, static_cast<int>(player.facing), static_cast<int>(player.steps / 4),
            player.x * kTilePx, player.y * kTilePx, kTilePx * 1.1f);

    // Night tint: one screen-space overlay over the finished world, rather than a darkened colour
    // per tile. Drawn inside the 2D camera would scale it with zoom, so it goes after EndMode2D.
    EndMode2D();
    if (night) DrawRectangle(0, 0, im.width, im.height, Color{20, 24, 80, 90});

    // NO HUD HERE. The world renderer draws the world; the shell (src/ui/screens.cpp) draws
    // everything a player reads. Keeping them apart is what let the engine counters move off the
    // play area and into the F3 overlay without touching a line of world-drawing code.
    //
    // The two counts the debug overlay wants are stashed for `drawn_chunks()` / `drawn_mobs()`.
    im.last_chunks = drawn_chunks;
    im.last_mobs = drawn_mobs;
}

void RaylibBridge::set_camps(const std::vector<std::pair<int, int>>& camps) {
    impl_->camps = camps;
}

BuildKind RaylibBridge::selected_build() const { return impl_->selected; }
int RaylibBridge::drawn_chunks() const { return impl_->last_chunks; }
int RaylibBridge::drawn_mobs() const { return impl_->last_mobs; }

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

    if (IsKeyPressed(KEY_ONE)) im.selected = BuildKind::kWall;
    if (IsKeyPressed(KEY_TWO)) im.selected = BuildKind::kTurret;
    if (IsKeyPressed(KEY_THREE)) im.selected = BuildKind::kPlot;
    if (IsKeyPressed(KEY_FOUR)) im.selected = BuildKind::kFence;
    in.build_kind = im.selected;

    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        im.camera.zoom = std::clamp(im.camera.zoom + wheel * 0.1f, 0.35f, 3.0f);
    }

    const Vector2 mw = GetScreenToWorld2D(GetMousePosition(), im.camera);
    const int tx = static_cast<int>(mw.x) / kTilePx;
    const int ty = static_cast<int>(mw.y) / kTilePx;
    if (tx >= 0 && ty >= 0 && tx < kMapTiles && ty < kMapTiles) {
        in.cursor_tx = static_cast<std::uint16_t>(tx);
        in.cursor_ty = static_cast<std::uint16_t>(ty);
        in.build = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        in.plant = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        in.harvest = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) || IsKeyPressed(KEY_E);
        in.till = IsKeyPressed(KEY_T);
        in.upgrade = IsKeyPressed(KEY_U);
    }

    (void)player;
    in.quit = WindowShouldClose();
    return in;
}

}  // namespace mmo
