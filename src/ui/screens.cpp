#include "ui/screens.hpp"

#include "world/player_actor.hpp"  // kPlayerMaxHp

#include <cmath>
#include <cstdio>

#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

namespace mmo::ui {
namespace {

constexpr int kPanelW = 340;
constexpr int kBtnH = 42;
constexpr int kGap = 10;

// Centred column of buttons; returns the index clicked, or -1.
int button_column(const char* const* labels, int count, int top) {
    const int x = (GetScreenWidth() - kPanelW) / 2;
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        const Rectangle r{static_cast<float>(x), static_cast<float>(top + i * (kBtnH + kGap)),
                          static_cast<float>(kPanelW), static_cast<float>(kBtnH)};
        if (GuiButton(r, labels[i])) clicked = i;
    }
    return clicked;
}

void dim_background() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 170});
}

void title(const char* text, int y) {
    const int w = MeasureText(text, 40);
    DrawText(text, (GetScreenWidth() - w) / 2, y, 40, Color{240, 226, 190, 255});
}

void subtitle(const char* text, int y) {
    const int w = MeasureText(text, 16);
    DrawText(text, (GetScreenWidth() - w) / 2, y, 16, Color{160, 170, 180, 255});
}

// --- in-game HUD -----------------------------------------------------------------------------
// Deliberately small. Health, the day clock, and the hotbar — nothing else. Anything a player does
// not need every second belongs in a screen they open, not permanently over the world.
void draw_hud(const ShellState& st, const WorldStatus& status, const PlayerView& player) {
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();

    // Health, bottom-left.
    const float hp_frac = static_cast<float>(player.hp) / static_cast<float>(kPlayerMaxHp);
    DrawRectangle(16, h - 46, 204, 22, Color{0, 0, 0, 150});
    DrawRectangle(18, h - 44, static_cast<int>(200 * (hp_frac < 0.0f ? 0.0f : hp_frac)), 18,
                  Color{196, 72, 72, 255});
    DrawText(TextFormat("%d", player.hp), 24, h - 42, 16, RAYWHITE);

    // Day clock, top-right. `world_ms` is the world's own clock, not this client's.
    const std::int64_t ms = status.world_ms.load(std::memory_order_relaxed);
    const bool night = status.night.load(std::memory_order_relaxed);
    const int day = static_cast<int>(ms / kCycleMs) + 1;
    const char* phase = night ? "Night" : "Day";
    const char* label = TextFormat("%s  -  day %d", phase, day);
    const int lw = MeasureText(label, 20);
    DrawRectangle(w - lw - 32, 12, lw + 20, 30, Color{0, 0, 0, 150});
    DrawText(label, w - lw - 22, 18, 20,
             night ? Color{178, 190, 240, 255} : Color{255, 236, 176, 255});

    // Hotbar, bottom-centre. Five slots; the selected one is outlined.
    constexpr int kSlots = 5;
    constexpr int kSlotPx = 44;
    const int total = kSlots * kSlotPx + (kSlots - 1) * 6;
    const int hx = (w - total) / 2;
    for (int i = 0; i < kSlots; ++i) {
        const int x = hx + i * (kSlotPx + 6);
        DrawRectangle(x, h - 60, kSlotPx, kSlotPx, Color{0, 0, 0, 150});
        DrawRectangleLines(x, h - 60, kSlotPx, kSlotPx,
                           i == st.selected_hotbar ? Color{240, 214, 130, 255}
                                                   : Color{110, 110, 118, 255});
        DrawText(TextFormat("%d", i + 1), x + 4, h - 58, 12, Color{170, 170, 176, 255});
    }
}

}  // namespace

bool handle_shell_keys(ShellState& st) {
    if (IsKeyPressed(KEY_F3)) {
        st.debug_overlay = !st.debug_overlay;
        return true;
    }
    for (int i = 0; i < 5; ++i) {
        if (IsKeyPressed(KEY_ONE + i)) st.selected_hotbar = i;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        switch (st.screen) {
            case Screen::kPlaying: st.screen = Screen::kPaused; return true;
            case Screen::kPaused: st.screen = Screen::kPlaying; return true;
            case Screen::kJournal:
            case Screen::kOptions: st.screen = Screen::kPaused; return true;
            case Screen::kMainMenu: return false;
        }
    }
    if (IsKeyPressed(KEY_J) && st.screen == Screen::kPlaying) {
        st.screen = Screen::kJournal;
        return true;
    }
    // While a menu is open the world must not receive movement or build clicks.
    return st.screen != Screen::kPlaying;
}

// raygui ships a light-grey default that fights a dark fantasy palette. Set once, on first draw.
void apply_theme() {
    static bool done = false;
    if (done) return;
    done = true;
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

Action draw(ShellState& st, const WorldStatus& status, const PlayerView& player) {
    apply_theme();

    switch (st.screen) {
        case Screen::kPlaying:
            draw_hud(st, status, player);
            return Action::kNone;

        case Screen::kMainMenu: {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{18, 20, 26, 255});
            title("Ninja Adventure", GetScreenHeight() / 2 - 190);
            subtitle("A retired ninja comes home to farm.", GetScreenHeight() / 2 - 138);
            static const char* items[] = {"Play", "Journal", "Options", "Quit"};
            switch (button_column(items, 4, GetScreenHeight() / 2 - 80)) {
                case 0: st.screen = Screen::kPlaying; return Action::kStartGame;
                case 1: st.screen = Screen::kJournal; break;
                case 2: st.screen = Screen::kOptions; break;
                case 3: return Action::kQuit;
                default: break;
            }
            return Action::kNone;
        }

        case Screen::kPaused: {
            draw_hud(st, status, player);
            dim_background();
            title("Paused", GetScreenHeight() / 2 - 170);
            subtitle("The world keeps going while you are here.", GetScreenHeight() / 2 - 120);
            static const char* items[] = {"Resume", "Journal", "Options", "Quit to menu"};
            switch (button_column(items, 4, GetScreenHeight() / 2 - 70)) {
                case 0: st.screen = Screen::kPlaying; return Action::kResume;
                case 1: st.screen = Screen::kJournal; break;
                case 2: st.screen = Screen::kOptions; break;
                case 3: st.screen = Screen::kMainMenu; break;
                default: break;
            }
            return Action::kNone;
        }

        case Screen::kJournal: {
            dim_background();
            const int x = (GetScreenWidth() - 620) / 2;
            int y = 90;
            DrawRectangle(x - 24, y - 34, 668, GetScreenHeight() - 150, Color{22, 24, 30, 240});
            DrawRectangleLines(x - 24, y - 34, 668, GetScreenHeight() - 150,
                               Color{110, 96, 70, 255});
            DrawText("Journal", x, y, 32, Color{240, 226, 190, 255});
            y += 54;

            DrawText("Controls", x, y, 20, Color{230, 206, 140, 255});
            y += 30;
            static const char* rows[] = {
                "WASD / arrows      move",
                "Left mouse         build the selected structure",
                "Right mouse        plant wheat (tilled soil only)",
                "E                  harvest a ripe crop",
                "T                  till a tile - expand the farm",
                "U                  upgrade the building under the cursor",
                "1 - 5              hotbar",
                "Mouse wheel        zoom",
                "J                  this journal",
                "F3                 debug overlay",
                "Esc                pause / back",
            };
            for (const char* r : rows) {
                DrawText(r, x + 8, y, 17, Color{206, 210, 216, 255});
                y += 24;
            }
            y += 14;
            DrawText("Getting started", x, y, 20, Color{230, 206, 140, 255});
            y += 30;
            static const char* tips[] = {
                "Nothing is counting down. Farm at your own pace.",
                "Monsters wander in at night; a wooden fence stops them.",
                "Gates lead elsewhere - some to a challenge, some to a quiet place.",
            };
            for (const char* t : tips) {
                DrawText(t, x + 8, y, 17, Color{176, 190, 176, 255});
                y += 24;
            }

            const Rectangle back{static_cast<float>(x),
                                 static_cast<float>(GetScreenHeight() - 150), 160.0f,
                                 static_cast<float>(kBtnH)};
            if (GuiButton(back, "Back")) {
                st.screen = Screen::kPaused;
            }
            return Action::kNone;
        }

        case Screen::kOptions: {
            dim_background();
            title("Options", GetScreenHeight() / 2 - 150);
            subtitle("Volume and key rebinding land with the audio pass.",
                     GetScreenHeight() / 2 - 100);
            static const char* items[] = {"Back"};
            if (button_column(items, 1, GetScreenHeight() / 2 - 40) == 0) {
                st.screen = Screen::kPaused;
            }
            return Action::kNone;
        }
    }
    return Action::kNone;
}

void draw_debug_overlay(const WorldStatus& status, const PlayerView& player, int drawn_chunks,
                        int drawn_mobs) {
    const int w = 330;
    DrawRectangle(GetScreenWidth() - w - 12, 52, w, 168, Color{0, 0, 0, 190});
    int y = 60;
    const auto line = [&](const char* text) {
        DrawText(text, GetScreenWidth() - w - 2, y, 16, Color{150, 220, 150, 255});
        y += 20;
    };
    line("-- debug (F3) ------------------");
    line(TextFormat("fps            %d", GetFPS()));
    line(TextFormat("world tick     %llu",
                    static_cast<unsigned long long>(status.tick.load(std::memory_order_relaxed))));
    line(TextFormat("chunk actors   %d drawn", drawn_chunks));
    line(TextFormat("mobs on screen %d", drawn_mobs));
    line(TextFormat("mobs killed    %u", status.mobs_killed.load(std::memory_order_relaxed)));
    // The headline number of the engine work: every one of these was a mob handed from one actor to
    // another, and will be a network frame once chunks live on separate machines.
    line(TextFormat("chunk migrations %u", status.migrations.load(std::memory_order_relaxed)));
    line(TextFormat("player tile    %d, %d", static_cast<int>(player.x),
                    static_cast<int>(player.y)));
}

}  // namespace mmo::ui
