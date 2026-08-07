// A native visual editor for `AuthoredMap` files (world/authored_map.hpp) — paint terrain,
// structures, decor and creature spawns by hand, or run the procedural generator
// (world/dungeon_gen.hpp) as a starting canvas and keep editing on top of it. Modeled on
// worldmap_main.cpp's precedent (a second raylib executable, no actor engine — this one adds a
// window/UI loop, but still links nothing beyond raylib/raygui + the pure world/ headers).
//
// Run: build/map_editor.exe [path.amap]
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "render/atlas_slots.hpp"
#include "world/authored_map.hpp"
#include "world/dungeon_gen.hpp"
#include "world/tiles.hpp"
#include "world/village.hpp"

using namespace mmo;

namespace {

constexpr int kWindowW = 1400;
constexpr int kWindowH = 900;
constexpr int kPanelW = 340;
constexpr const char* kDefaultPath = "assets/maps/dojo_annex.amap";

// Same dark theme screens.cpp already established for the client's own raygui panels — reused
// verbatim so the two tools read as one family rather than two different apps.
void apply_theme() {
    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);
    const auto hex = [](unsigned v) { return static_cast<int>(v); };
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, hex(0x14161cff));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, hex(0x6e6046ff));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, hex(0x232732ff));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, hex(0xcfd3d9ff));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, hex(0xd6b06aff));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, hex(0x33384aff));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, hex(0xf0e2beff));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, hex(0xf0d08aff));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, hex(0x3f4459ff));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, hex(0xfff3d0ff));
}

// Flat terrains have a matching atlas fill; kTree does not (trees are the multi-tile `Big` sprites
// elsewhere in the pack, not a flat ground tile), so it falls back to a plain colour swatch — the
// same honestly-labelled compromise worldmap_main.cpp already makes for its whole-map export.
[[nodiscard]] std::optional<Slot> terrain_slot_of(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return Slot::kTerrainGrass;
        case Terrain::kDirt: return Slot::kTerrainDirt;
        case Terrain::kWater: return Slot::kTerrainWater;
        case Terrain::kStone: return Slot::kTerrainStone;
        case Terrain::kSand: return Slot::kTerrainSand;
        case Terrain::kTree: return std::nullopt;
        case Terrain::kSnow: return Slot::kTerrainSnow;
        case Terrain::kMarsh: return Slot::kTerrainMarsh;
        case Terrain::kAsh: return Slot::kTerrainAsh;
        case Terrain::kPath: return Slot::kTerrainPath;
        case Terrain::kBuilding: return Slot::kTerrainBuilding;
        case Terrain::kCount: break;
    }
    return std::nullopt;
}

[[nodiscard]] const char* terrain_name(Terrain t) {
    switch (t) {
        case Terrain::kGrass: return "Grass";
        case Terrain::kDirt: return "Dirt";
        case Terrain::kWater: return "Water";
        case Terrain::kStone: return "Stone";
        case Terrain::kSand: return "Sand";
        case Terrain::kTree: return "Tree";
        case Terrain::kSnow: return "Snow";
        case Terrain::kMarsh: return "Marsh";
        case Terrain::kAsh: return "Ash";
        case Terrain::kPath: return "Path/Crossway";
        case Terrain::kBuilding: return "Wall/Building";
        case Terrain::kCount: break;
    }
    return "?";
}

[[nodiscard]] const char* structure_name(StructureKind k) {
    switch (k) {
        case StructureKind::kHouseOrange: return "House Orange";
        case StructureKind::kHouseCream: return "House Cream";
        case StructureKind::kHouseAmber: return "House Amber";
        case StructureKind::kHouseRed: return "House Red";
        case StructureKind::kHouseBlue: return "House Blue";
        case StructureKind::kHouseTan: return "House Tan";
        case StructureKind::kHouseWood: return "House Wood";
        case StructureKind::kHutSnowA: return "Snow Hut A";
        case StructureKind::kHutSnowB: return "Snow Hut B";
        case StructureKind::kHutSnowC: return "Snow Hut C";
        case StructureKind::kRuinA: return "Ruin A";
        case StructureKind::kRuinB: return "Ruin B";
        case StructureKind::kTentA: return "Tent A";
        case StructureKind::kTentB: return "Tent B";
        case StructureKind::kTentC: return "Tent C";
        case StructureKind::kLogPost: return "Log Post";
        case StructureKind::kRampart: return "Rampart";
        case StructureKind::kGate: return "Gate";
        case StructureKind::kStakeA: return "Stake A";
        case StructureKind::kStakeB: return "Stake B";
        case StructureKind::kStakeC: return "Stake C";
        case StructureKind::kCount: break;
    }
    return "?";
}

[[nodiscard]] const char* creature_name(CreatureKind k) {
    switch (k) {
        case CreatureKind::kSlime: return "Slime";
        case CreatureKind::kSpider: return "Spider";
        case CreatureKind::kGhost: return "Ghost";
        case CreatureKind::kSkull: return "Skull";
        case CreatureKind::kBoar: return "Boar";
        case CreatureKind::kWolf: return "Wolf";
        case CreatureKind::kBear: return "Bear";
        case CreatureKind::kHare: return "Hare";
        case CreatureKind::kChicken: return "Chicken";
        // A map-authored spawn of this kind is a plain stats-only creature, not a real village-roster
        // guard (npc_init's role/home_struct packing, step_guard's patrol/rally behavior) — this editor
        // only authors AuthoredSpawn entries, which never set the Npc marker bit.
        case CreatureKind::kGuard: return "Guard";
        case CreatureKind::kBoss: return "Boss";
        case CreatureKind::kCount: break;
    }
    return "?";
}

enum class Layer { kTerrain, kStructures, kDecor, kSpawns };

[[nodiscard]] AuthoredMap blank_map(int w, int h) {
    AuthoredMap m;
    m.width = static_cast<std::uint16_t>(w);
    m.height = static_cast<std::uint16_t>(h);
    m.terrain.assign(static_cast<std::size_t>(w) * h, Terrain::kGrass);
    return m;
}

// A "-"/value/"+" row using ONLY GuiButton (the one raygui widget this codebase's own client
// already relies on) plus raw raylib text — no GuiValueBox/GuiSlider, which nothing here has
// exercised before.
bool stepper_int(Rectangle row, const char* label, int& value, int step, int lo, int hi) {
    bool changed = false;
    DrawText(label, static_cast<int>(row.x), static_cast<int>(row.y) + 4, 14, RAYWHITE);
    const Rectangle minus{row.x + 150, row.y, 26, 24};
    const Rectangle plus{row.x + 250, row.y, 26, 24};
    if (GuiButton(minus, "-") && value - step >= lo) {
        value -= step;
        changed = true;
    }
    if (GuiButton(plus, "+") && value + step <= hi) {
        value += step;
        changed = true;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", value);
    DrawText(buf, static_cast<int>(row.x) + 182, static_cast<int>(row.y) + 4, 16, RAYWHITE);
    return changed;
}

bool stepper_float(Rectangle row, const char* label, float& value, float step, float lo, float hi) {
    bool changed = false;
    DrawText(label, static_cast<int>(row.x), static_cast<int>(row.y) + 4, 14, RAYWHITE);
    const Rectangle minus{row.x + 150, row.y, 26, 24};
    const Rectangle plus{row.x + 250, row.y, 26, 24};
    if (GuiButton(minus, "-") && value - step >= lo - 0.001f) {
        value -= step;
        changed = true;
    }
    if (GuiButton(plus, "+") && value + step <= hi + 0.001f) {
        value += step;
        changed = true;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(value));
    DrawText(buf, static_cast<int>(row.x) + 182, static_cast<int>(row.y) + 4, 16, RAYWHITE);
    return changed;
}

}  // namespace

// Headless generation, no window — for batch/CI use and for driving the generator without a mouse
// (map_editor's own "Generate" panel calls the identical generate_dungeon()/save_authored_map()
// pair; this is that same path exposed as a command line, not a second implementation).
// Usage: map_editor --gen-cli OUT.amap [width height seed portal_count]
int run_gen_cli(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: map_editor --gen-cli OUT.amap [width height seed portals]\n");
        return 2;
    }
    GenConfig cfg{};
    const std::string out_path = argv[2];
    if (argc > 3) cfg.width = static_cast<std::uint16_t>(std::atoi(argv[3]));
    if (argc > 4) cfg.height = static_cast<std::uint16_t>(std::atoi(argv[4]));
    if (argc > 5) cfg.seed = std::strtoull(argv[5], nullptr, 0);
    if (argc > 6) cfg.portal_count = std::atoi(argv[6]);
    const AuthoredMap m = generate_dungeon(cfg);
    if (!save_authored_map(out_path.c_str(), m)) {
        std::fprintf(stderr, "failed to save %s\n", out_path.c_str());
        return 1;
    }
    std::printf("generated %dx%d, %zu structures, %zu decor, %zu spawns, %zu portals -> %s\n",
               m.width, m.height, m.structures.size(), m.decor.size(), m.spawns.size(),
               m.portals.size(), out_path.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--gen-cli") == 0) return run_gen_cli(argc, argv);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(kWindowW, kWindowH, "Quark MMO — map_editor");
    SetTargetFPS(60);
    apply_theme();

    Texture2D atlas{};
    bool atlas_ok = false;
    for (const char* p : {"assets/atlas.png", "../assets/atlas.png", "../../assets/atlas.png"}) {
        if (!FileExists(p)) continue;
        atlas = LoadTexture(p);
        atlas_ok = atlas.id != 0;
        if (atlas_ok) {
            SetTextureFilter(atlas, TEXTURE_FILTER_POINT);
            break;
        }
    }

    AuthoredMap doc = blank_map(24, 24);
    std::string path = (argc > 1) ? argv[1] : kDefaultPath;
    std::string status_line = "New 24x24 map. F1 Paint / F2 Generate. Left=paint Right=erase.";

    Layer layer = Layer::kTerrain;
    Terrain active_terrain = Terrain::kGrass;
    StructureKind active_structure = StructureKind::kHouseOrange;
    Slot active_decor = Slot::kTerrainGrass2;
    CreatureKind active_spawn = CreatureKind::kSlime;
    bool portal_tool = false;
    AuthoredPortal* selected_portal = nullptr;

    GenConfig gen{};
    bool show_generate = false;
    int palette_scroll = 0;

    while (!WindowShouldClose()) {
        const int screen_w = GetScreenWidth();
        const int screen_h = GetScreenHeight();
        if (IsKeyPressed(KEY_F1)) show_generate = false;
        if (IsKeyPressed(KEY_F2)) show_generate = true;

        // --- canvas geometry: fit the whole map in the viewport, no scrolling needed at the 64x64
        // cap (kAuthoredMapMaxSide) this format enforces --------------------------------------------
        const Rectangle canvas_rect{static_cast<float>(kPanelW), 0.0f,
                                    static_cast<float>(screen_w - kPanelW), static_cast<float>(screen_h)};
        const float tile_px = std::max(
            4.0f, std::min((canvas_rect.width - 16.0f) / static_cast<float>(doc.width),
                           (canvas_rect.height - 16.0f) / static_cast<float>(doc.height)));
        const float ox = canvas_rect.x + 8.0f;
        const float oy = 8.0f;

        const Vector2 mouse = GetMousePosition();
        const bool over_canvas = CheckCollisionPointRec(mouse, canvas_rect);
        int mx = -1, my = -1;
        if (over_canvas) {
            mx = static_cast<int>((mouse.x - ox) / tile_px);
            my = static_cast<int>((mouse.y - oy) / tile_px);
            if (mx < 0 || mx >= doc.width || my < 0 || my >= doc.height) mx = my = -1;
        }

        // --- painting ---------------------------------------------------------------------------
        if (mx >= 0 && !show_generate) {
            const bool paint = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            const bool erase = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
            switch (layer) {
                case Layer::kTerrain:
                    if (paint) {
                        doc.terrain[static_cast<std::size_t>(my) * doc.width + static_cast<std::size_t>(mx)] =
                            active_terrain;
                    }
                    break;
                case Layer::kStructures:
                    if (paint || erase) {
                        auto& v = doc.structures;
                        v.erase(std::remove_if(v.begin(), v.end(),
                                               [&](const AuthoredStructure& s) {
                                                   return s.x == mx && s.y == my;
                                               }),
                               v.end());
                        if (paint) {
                            v.push_back(AuthoredStructure{static_cast<std::uint16_t>(mx),
                                                          static_cast<std::uint16_t>(my),
                                                          active_structure});
                        }
                    }
                    break;
                case Layer::kDecor:
                    if (paint || erase) {
                        auto& v = doc.decor;
                        v.erase(std::remove_if(v.begin(), v.end(),
                                               [&](const AuthoredDecor& d) {
                                                   return d.x == mx && d.y == my;
                                               }),
                               v.end());
                        if (paint) {
                            v.push_back(
                                AuthoredDecor{static_cast<std::uint16_t>(mx), static_cast<std::uint16_t>(my),
                                             active_decor});
                        }
                    }
                    break;
                case Layer::kSpawns:
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || erase) {
                        if (portal_tool) {
                            auto& v = doc.portals;
                            v.erase(std::remove_if(v.begin(), v.end(),
                                                   [&](const AuthoredPortal& p) {
                                                       return p.x == mx && p.y == my;
                                                   }),
                                   v.end());
                            selected_portal = nullptr;
                            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                                v.push_back(AuthoredPortal{static_cast<std::uint16_t>(mx),
                                                           static_cast<std::uint16_t>(my), 0, 0, 0});
                                selected_portal = &v.back();
                            }
                        } else {
                            auto& v = doc.spawns;
                            v.erase(std::remove_if(v.begin(), v.end(),
                                                   [&](const AuthoredSpawn& s) {
                                                       return s.x == mx && s.y == my;
                                                   }),
                                   v.end());
                            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                                v.push_back(AuthoredSpawn{static_cast<std::uint16_t>(mx),
                                                          static_cast<std::uint16_t>(my), active_spawn});
                            }
                        }
                    }
                    break;
            }
        }

        BeginDrawing();
        ClearBackground(Color{10, 11, 15, 255});

        // --- canvas -------------------------------------------------------------------------------
        DrawRectangleRec(canvas_rect, Color{18, 19, 24, 255});
        for (int y = 0; y < doc.height; ++y) {
            for (int x = 0; x < doc.width; ++x) {
                const Terrain t = doc.tile_at(x, y);
                const Rectangle dest{ox + static_cast<float>(x) * tile_px, oy + static_cast<float>(y) * tile_px,
                                     tile_px + 0.5f, tile_px + 0.5f};
                const std::optional<Slot> slot = terrain_slot_of(t);
                if (atlas_ok && slot.has_value()) {
                    const AtlasRect r = rect_of(*slot);
                    const Rectangle src{static_cast<float>(r.x), static_cast<float>(r.y),
                                       static_cast<float>(kAtlasTile), static_cast<float>(kAtlasTile)};
                    DrawTexturePro(atlas, src, dest, Vector2{0, 0}, 0.0f, WHITE);
                } else {
                    DrawRectangleRec(dest, t == Terrain::kTree ? Color{40, 90, 48, 255}
                                                               : Color{60, 20, 60, 255});
                }
            }
        }
        // grid lines every tile keep dense small rooms readable
        for (int x = 0; x <= doc.width; ++x) {
            DrawLine(static_cast<int>(ox + static_cast<float>(x) * tile_px), static_cast<int>(oy),
                    static_cast<int>(ox + static_cast<float>(x) * tile_px),
                    static_cast<int>(oy + static_cast<float>(doc.height) * tile_px), Color{0, 0, 0, 40});
        }
        for (int y = 0; y <= doc.height; ++y) {
            DrawLine(static_cast<int>(ox), static_cast<int>(oy + static_cast<float>(y) * tile_px),
                    static_cast<int>(ox + static_cast<float>(doc.width) * tile_px),
                    static_cast<int>(oy + static_cast<float>(y) * tile_px), Color{0, 0, 0, 40});
        }
        // structures/decor/spawns/portals as coloured markers — this tool's own preview does not
        // need the live game's multi-tile sprites to be useful for placement/authoring.
        for (const AuthoredStructure& s : doc.structures) {
            DrawRectangle(static_cast<int>(ox + static_cast<float>(s.x) * tile_px + 1),
                          static_cast<int>(oy + static_cast<float>(s.y) * tile_px + 1),
                          static_cast<int>(tile_px - 2), static_cast<int>(tile_px - 2),
                          Color{196, 140, 70, 220});
        }
        for (const AuthoredDecor& d : doc.decor) {
            DrawCircle(static_cast<int>(ox + (static_cast<float>(d.x) + 0.5f) * tile_px),
                      static_cast<int>(oy + (static_cast<float>(d.y) + 0.5f) * tile_px),
                      tile_px * 0.22f, Color{120, 210, 120, 230});
        }
        for (const AuthoredSpawn& s : doc.spawns) {
            DrawCircle(static_cast<int>(ox + (static_cast<float>(s.x) + 0.5f) * tile_px),
                      static_cast<int>(oy + (static_cast<float>(s.y) + 0.5f) * tile_px),
                      tile_px * 0.3f, Color{220, 70, 70, 230});
        }
        for (const AuthoredPortal& p : doc.portals) {
            DrawRing(Vector2{ox + (static_cast<float>(p.x) + 0.5f) * tile_px,
                            oy + (static_cast<float>(p.y) + 0.5f) * tile_px},
                    tile_px * 0.22f, tile_px * 0.4f, 0, 360, 16, Color{110, 170, 255, 240});
        }
        if (mx >= 0) {
            DrawRectangleLines(static_cast<int>(ox + static_cast<float>(mx) * tile_px),
                               static_cast<int>(oy + static_cast<float>(my) * tile_px),
                               static_cast<int>(tile_px), static_cast<int>(tile_px), YELLOW);
        }

        // --- left panel -----------------------------------------------------------------------
        DrawRectangle(0, 0, kPanelW, screen_h, Color{20, 22, 28, 255});
        int py = 10;
        if (GuiButton(Rectangle{10, static_cast<float>(py), 155, 26}, "Paint (F1)")) show_generate = false;
        if (GuiButton(Rectangle{175, static_cast<float>(py), 155, 26}, "Generate (F2)")) show_generate = true;
        py += 34;

        if (!show_generate) {
            const char* layer_labels[4] = {"Terrain", "Structures", "Decor", "Spawns/Portals"};
            for (int i = 0; i < 4; ++i) {
                const Rectangle r{10.0f + static_cast<float>(i % 2) * 165.0f,
                                  static_cast<float>(py + (i / 2) * 30), 155, 26};
                if (GuiButton(r, layer_labels[i])) layer = static_cast<Layer>(i);
            }
            py += 68;
            DrawRectangle(0, py, kPanelW, 2, Color{110, 96, 70, 255});
            py += 10;

            // Save / Load
            if (GuiButton(Rectangle{10, static_cast<float>(py), 155, 26}, "Save")) {
                status_line = save_authored_map(path.c_str(), doc) ? ("Saved " + path)
                                                                   : ("FAILED to save " + path);
            }
            if (GuiButton(Rectangle{175, static_cast<float>(py), 155, 26}, "Load")) {
                AuthoredMap loaded;
                if (load_authored_map(path.c_str(), loaded)) {
                    doc = std::move(loaded);
                    status_line = "Loaded " + path;
                } else {
                    status_line = "FAILED to load " + path;
                }
            }
            py += 36;
            DrawText(path.c_str(), 10, py, 12, Color{160, 160, 170, 255});
            py += 24;

            // Palette for the active layer — a scrollable grid of GuiButtons, wheel-scrolled.
            if (over_canvas == false && CheckCollisionPointRec(mouse, Rectangle{0, static_cast<float>(py), kPanelW, static_cast<float>(screen_h - py)})) {
                palette_scroll -= static_cast<int>(GetMouseWheelMove() * 30);
            }
            palette_scroll = std::max(0, palette_scroll);
            BeginScissorMode(0, py, kPanelW, screen_h - py);
            int gy = py - palette_scroll;
            const int col_w = 165;
            const int row_h = 30;
            if (layer == Layer::kTerrain) {
                for (int i = 0; i < static_cast<int>(Terrain::kCount); ++i) {
                    const Terrain t = static_cast<Terrain>(i);
                    const Rectangle r{10.0f + static_cast<float>(i % 2) * col_w,
                                      static_cast<float>(gy + (i / 2) * row_h), 155, 26};
                    if (GuiButton(r, terrain_name(t))) active_terrain = t;
                }
            } else if (layer == Layer::kStructures) {
                for (int i = 0; i < static_cast<int>(StructureKind::kCount); ++i) {
                    const auto k = static_cast<StructureKind>(i);
                    const Rectangle r{10.0f + static_cast<float>(i % 2) * col_w,
                                      static_cast<float>(gy + (i / 2) * row_h), 155, 26};
                    if (GuiButton(r, structure_name(k))) active_structure = k;
                }
            } else if (layer == Layer::kDecor) {
                for (int i = 0; i < static_cast<int>(Slot::kCount); ++i) {
                    const auto s = static_cast<Slot>(i);
                    char label[24];
                    std::snprintf(label, sizeof label, "slot %d", i);
                    const Rectangle r{10.0f + static_cast<float>(i % 2) * col_w,
                                      static_cast<float>(gy + (i / 2) * row_h), 155, 26};
                    if (GuiButton(r, label)) active_decor = s;
                    if (atlas_ok) {
                        const AtlasRect ar = rect_of(s);
                        DrawTexturePro(atlas,
                                       Rectangle{static_cast<float>(ar.x), static_cast<float>(ar.y),
                                                static_cast<float>(kAtlasTile),
                                                static_cast<float>(kAtlasTile)},
                                       Rectangle{r.x + r.width + 4, r.y, 22, 22}, Vector2{0, 0}, 0.0f,
                                       WHITE);
                    }
                }
            } else {
                if (GuiButton(Rectangle{10, static_cast<float>(gy), 320, 26}, portal_tool ? "[Portal] (click again for creatures)" : "Portal tool")) {
                    portal_tool = !portal_tool;
                }
                gy += row_h + 4;
                if (!portal_tool) {
                    for (int i = 0; i < static_cast<int>(CreatureKind::kCount); ++i) {
                        const auto k = static_cast<CreatureKind>(i);
                        const Rectangle r{10.0f + static_cast<float>(i % 2) * col_w,
                                          static_cast<float>(gy + (i / 2) * row_h), 155, 26};
                        if (GuiButton(r, creature_name(k))) active_spawn = k;
                    }
                } else if (selected_portal != nullptr) {
                    int to_map = selected_portal->to_map;
                    int to_x = selected_portal->to_x;
                    int to_y = selected_portal->to_y;
                    if (stepper_int(Rectangle{10, static_cast<float>(gy), 300, 26}, "to_map", to_map, 1, 0, 65535)) {
                        selected_portal->to_map = static_cast<std::uint16_t>(to_map);
                    }
                    gy += row_h;
                    if (stepper_int(Rectangle{10, static_cast<float>(gy), 300, 26}, "to_x", to_x, 1, 0, 4095)) {
                        selected_portal->to_x = static_cast<std::uint16_t>(to_x);
                    }
                    gy += row_h;
                    if (stepper_int(Rectangle{10, static_cast<float>(gy), 300, 26}, "to_y", to_y, 1, 0, 4095)) {
                        selected_portal->to_y = static_cast<std::uint16_t>(to_y);
                    }
                }
            }
            EndScissorMode();
        } else {
            // --- Generate panel -------------------------------------------------------------------
            int w = gen.width, h = gen.height;
            int seed_lo = static_cast<int>(gen.seed & 0xFFFF);
            int min_room = gen.min_room_size;
            int portals = gen.portal_count;
            float mine_frac = gen.mine_room_fraction;
            float decor_d = gen.decor_density;

            if (stepper_int(Rectangle{10, static_cast<float>(py), 300, 26}, "width", w, 4, 8, kAuthoredMapMaxSide)) {
                gen.width = static_cast<std::uint16_t>(w);
            }
            py += 32;
            if (stepper_int(Rectangle{10, static_cast<float>(py), 300, 26}, "height", h, 4, 8, kAuthoredMapMaxSide)) {
                gen.height = static_cast<std::uint16_t>(h);
            }
            py += 32;
            if (stepper_int(Rectangle{10, static_cast<float>(py), 300, 26}, "seed", seed_lo, 1, 0, 65535)) {
                gen.seed = (gen.seed & ~0xFFFFull) | static_cast<std::uint64_t>(seed_lo);
            }
            py += 32;
            if (stepper_int(Rectangle{10, static_cast<float>(py), 300, 26}, "min room size", min_room, 1, 4, 16)) {
                gen.min_room_size = min_room;
            }
            py += 32;
            if (stepper_int(Rectangle{10, static_cast<float>(py), 300, 26}, "portal count", portals, 1, 1, 4)) {
                gen.portal_count = portals;
            }
            py += 32;
            if (stepper_float(Rectangle{10, static_cast<float>(py), 300, 26}, "mine fraction", mine_frac, 0.05f, 0.0f, 1.0f)) {
                gen.mine_room_fraction = mine_frac;
            }
            py += 32;
            if (stepper_float(Rectangle{10, static_cast<float>(py), 300, 26}, "decor density", decor_d, 0.02f, 0.0f, 0.5f)) {
                gen.decor_density = decor_d;
            }
            py += 40;
            if (GuiButton(Rectangle{10, static_cast<float>(py), 155, 30}, "Generate")) {
                doc = generate_dungeon(gen);
                selected_portal = nullptr;
                status_line = "Generated a new layout — keep editing, then Save.";
                show_generate = false;
            }
            if (GuiButton(Rectangle{175, static_cast<float>(py), 155, 30}, "Blank")) {
                doc = blank_map(gen.width, gen.height);
                status_line = "New blank map.";
                show_generate = false;
            }
        }

        DrawText(status_line.c_str(), 10, screen_h - 22, 12, Color{170, 200, 170, 255});
        EndDrawing();
    }

    if (atlas_ok) UnloadTexture(atlas);
    CloseWindow();
    return 0;
}
