#include "ui/screens.hpp"

#include "render/atlas_slots.hpp"
#include "render/ui_sprites.hpp"
#include "world/player_actor.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

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

// A text field, hand-rolled rather than raygui's.
//
// Two reasons, and the second is the real one. raygui's `GuiTextBox` has no password mode, so the
// field that most needs one would have had to be special-cased anyway — and a half-raygui,
// half-manual pair of boxes on the same form behaves inconsistently under focus. So both are the
// same twenty lines. Masking here means the plaintext never reaches the screen; it still reaches
// `AccountStore::login`, which is where it is turned into an Argon2 hash and forgotten.
void text_field(const Rectangle& r, char* buf, int cap, bool& editing, bool mask,
                const char* placeholder) {
    const Vector2 m = GetMousePosition();
    const bool over = CheckCollisionPointRec(m, r);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) editing = over;

    if (editing) {
        int c = GetCharPressed();
        while (c > 0) {
            const auto len = static_cast<int>(std::strlen(buf));
            if (c >= 32 && c < 127 && len < cap - 1) {
                buf[len] = static_cast<char>(c);
                buf[len + 1] = '\0';
            }
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            const auto len = static_cast<int>(std::strlen(buf));
            if (len > 0) buf[len - 1] = '\0';
        }
    }

    DrawRectangleRec(r, Color{26, 29, 38, 255});
    DrawRectangleLinesEx(r, 2.0f,
                         editing ? Color{214, 176, 106, 255} : Color{92, 96, 108, 255});
    const auto len = static_cast<int>(std::strlen(buf));
    char shown[kPassMax + 1];
    if (mask) {
        const int n = len < kPassMax ? len : kPassMax;
        for (int i = 0; i < n; ++i) shown[i] = '*';
        shown[n] = '\0';
    }
    const char* text = len == 0 ? placeholder : (mask ? shown : buf);
    DrawText(text, static_cast<int>(r.x) + 10, static_cast<int>(r.y) + 10, 20,
             len == 0 ? Color{110, 114, 124, 255} : Color{224, 228, 234, 255});
    // A caret, so an empty focused box does not look broken.
    if (editing && (static_cast<int>(GetTime() * 2.0) & 1)) {
        const int tw = MeasureText(len == 0 ? "" : text, 20);
        DrawRectangle(static_cast<int>(r.x) + 11 + tw, static_cast<int>(r.y) + 9, 2, 22,
                      Color{224, 228, 234, 255});
    }
}

// --- in-game HUD -----------------------------------------------------------------------------
// Deliberately small. Three bars, the day clock, and the hotbar — nothing else. Anything a player
// does not need every second belongs in a screen they open, not permanently over the world.

void bar(int x, int y, int w, int h, float frac, Color fill, const char* label) {
    DrawRectangle(x - 2, y - 2, w + 4, h + 4, Color{0, 0, 0, 150});
    DrawRectangle(x, y, static_cast<int>(static_cast<float>(w) * (frac < 0.0f ? 0.0f : frac)), h,
                  fill);
    if (label != nullptr) DrawText(label, x + 6, y + (h - 14) / 2, 14, RAYWHITE);
}

void draw_hud(const ShellState& st, const WorldStatus& status, const PlayerView& player) {
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();

    // Three bars, bottom-left, in the order they are spent: health is what the world takes,
    // stamina is what swinging costs, mana is what casting costs.
    const auto frac = [](std::int16_t v, std::int16_t max) {
        return max <= 0 ? 0.0f : static_cast<float>(v) / static_cast<float>(max);
    };
    bar(18, h - 74, 200, 20, frac(player.hp, kPlayerMaxHp), Color{196, 72, 72, 255},
        TextFormat("%d", player.hp));
    bar(18, h - 48, 140, 12, frac(player.stamina, kPlayerMaxStamina), Color{120, 190, 96, 255},
        nullptr);
    bar(18, h - 30, 140, 12, frac(player.mana, kPlayerMaxMana), Color{96, 140, 220, 255}, nullptr);

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

    // Hotbar, bottom-centre. It shows four schools when you are fighting and two buildings when you
    // are building, because those are two different bars and pretending otherwise would mean six
    // slots of which four are always dead.
    static const char* kSchools[4] = {"Fire", "Ice", "Earth", "Shock"};
    static const Color kSchoolTint[4] = {Color{255, 150, 90, 255}, Color{140, 210, 255, 255},
                                         Color{180, 150, 110, 255}, Color{255, 245, 130, 255}};
    static const char* kBuilds[2] = {"Hearth", "Plot"};
    const int slots = st.build_mode ? 2 : 4;
    constexpr int kSlotPx = 44;
    const int total = slots * kSlotPx + (slots - 1) * 6;
    const int hx = (w - total) / 2;
    for (int i = 0; i < slots; ++i) {
        const int x = hx + i * (kSlotPx + 6);
        DrawRectangle(x, h - 60, kSlotPx, kSlotPx, Color{0, 0, 0, 150});
        DrawRectangleLines(x, h - 60, kSlotPx, kSlotPx,
                           i == st.selected_hotbar ? Color{240, 214, 130, 255}
                                                   : Color{110, 110, 118, 255});
        if (!st.build_mode) {
            DrawRectangle(x + 10, h - 44, 24, 24, kSchoolTint[i]);
        }
        DrawText(TextFormat("%d", i + 1), x + 4, h - 58, 12, Color{170, 170, 176, 255});
        const char* name = st.build_mode ? kBuilds[i] : kSchools[i];
        DrawText(name, x + (kSlotPx - MeasureText(name, 10)) / 2, h - 12, 10,
                 Color{170, 174, 182, 255});
    }
    // Above the slots rather than beside them: at the narrowest supported window the hotbar reaches
    // most of the way across and a label to its left lands on top of slot one.
    {
        const char* mode = st.build_mode ? "BUILD  (B)" : "FIGHT  (B)";
        DrawText(mode, (w - MeasureText(mode, 16)) / 2, h - 92, 16,
                 st.build_mode ? Color{240, 214, 130, 255} : Color{212, 172, 172, 255});
    }

    // Death. A countdown, in the middle, with nothing else competing for attention.
    if (player.dead_ticks > 0) {
        const char* down = "You went down.";
        DrawText(down, (w - MeasureText(down, 44)) / 2, h / 2 - 60, 44,
                 Color{244, 214, 200, 255});
        const char* back =
            TextFormat("Waking at your hearth in %.0f...",
                       static_cast<double>(player.dead_ticks) / kTicksPerSecond + 0.9);
        DrawText(back, (w - MeasureText(back, 20)) / 2, h / 2, 20, Color{210, 180, 175, 255});
    }
}

// --- Character sheet -------------------------------------------------------------------------
static const char* kSkillNames[kSkillCount] = {"Melee", "Ranged", "Magic", "Craft"};

void draw_character(const PlayerView& player) {
    dim_background();
    const int panel_w = 560;
    const int x = (GetScreenWidth() - panel_w) / 2;
    int y = 90;
    DrawRectangle(x - 24, y - 34, panel_w + 48, GetScreenHeight() - 150, Color{22, 24, 30, 240});
    DrawRectangleLines(x - 24, y - 34, panel_w + 48, GetScreenHeight() - 150,
                       Color{110, 96, 70, 255});

    // The portrait. Ninja Adventure ships one beside every actor; this is the only thing on the
    // screen that is art rather than a number, and it is what stops the sheet reading as a form.
    draw_ui_fx(static_cast<int>(Fx::kFacePlayer), 0, static_cast<float>(x + 48),
               static_cast<float>(y + 42), 96.0f);
    DrawRectangleLines(x, y - 6, 96, 96, Color{110, 96, 70, 255});

    DrawText("Character", x + 118, y - 4, 32, Color{240, 226, 190, 255});
    DrawText(TextFormat("deaths %u", player.deaths), x + 118, y + 36, 16,
             Color{160, 170, 180, 255});
    y += 112;

    DrawText(TextFormat("Health   %3d / %d", player.hp, player.max_hp), x, y, 18,
             Color{226, 150, 150, 255});
    DrawText(TextFormat("Stamina  %3d / %d", player.stamina, kPlayerMaxStamina), x + 250, y, 18,
             Color{160, 214, 150, 255});
    y += 26;
    DrawText(TextFormat("Mana     %3d / %d", player.mana, kPlayerMaxMana), x, y, 18,
             Color{150, 175, 226, 255});
    y += 44;

    // Skills. The cap is displayed next to the total on purpose: a limit the player only discovers
    // by hitting it is a bug report, not a design.
    int spent = 0;
    for (int i = 0; i < kSkillCount; ++i) spent += player.skill_level[i];
    DrawText("Skills", x, y, 20, Color{230, 206, 140, 255});
    const char* cap = TextFormat("%d / %d points  -  you level what you use", spent,
                                 static_cast<int>(kSkillPointCap));
    DrawText(cap, x + panel_w - MeasureText(cap, 15), y + 5, 15, Color{150, 158, 168, 255});
    y += 32;

    for (int i = 0; i < kSkillCount; ++i) {
        DrawText(kSkillNames[i], x, y, 18, Color{212, 216, 222, 255});
        DrawText(TextFormat("%2u", player.skill_level[i]), x + 92, y, 18,
                 Color{240, 226, 190, 255});
        const float f = player.skill_next[i] == 0
                            ? 1.0f
                            : static_cast<float>(player.skill_xp[i]) /
                                  static_cast<float>(player.skill_next[i]);
        bar(x + 128, y + 2, 300, 14, f, Color{150, 130, 200, 255}, nullptr);
        DrawText(TextFormat("%u/%u", player.skill_xp[i], player.skill_next[i]), x + 440, y + 1, 15,
                 Color{150, 158, 168, 255});
        y += 30;
    }
    y += 18;
    DrawText("Magic sets a status. A physical blow detonates it.", x, y, 17,
             Color{176, 190, 176, 255});
    y += 24;
    static const char* kCombos[] = {
        "Frozen  + heavy melee   ->  Shatter    x2.5",
        "Burning + arrow         ->  Blast      splash",
        "Wet     + shock         ->  Conduct    chains",
        "Muddy   + heavy melee   ->  Crush      stuns",
        "Shocked + melee         ->  Arc        returns mana",
    };
    for (const char* c : kCombos) {
        DrawText(c, x + 8, y, 16, Color{196, 200, 208, 255});
        y += 21;
    }
}

}  // namespace

bool handle_shell_keys(ShellState& st) {
    // The sign-in screen swallows everything: its text fields want the keyboard.
    if (st.screen == Screen::kLogin) return true;

    if (IsKeyPressed(KEY_F3)) {
        st.debug_overlay = !st.debug_overlay;
        return true;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        switch (st.screen) {
            case Screen::kPlaying: st.screen = Screen::kPaused; return true;
            case Screen::kPaused: st.screen = Screen::kPlaying; return true;
            case Screen::kJournal:
            case Screen::kCharacter:
            case Screen::kOptions: st.screen = Screen::kPlaying; return true;
            case Screen::kLogin:
            case Screen::kMainMenu: return false;
        }
    }
    if (IsKeyPressed(KEY_J) && st.screen == Screen::kPlaying) {
        st.screen = Screen::kJournal;
        return true;
    }
    if (IsKeyPressed(KEY_C)) {
        if (st.screen == Screen::kPlaying) {
            st.screen = Screen::kCharacter;
            return true;
        }
        if (st.screen == Screen::kCharacter) {
            st.screen = Screen::kPlaying;
            return true;
        }
    }
    // While a menu is open the world must not receive movement or attack clicks.
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
        case Screen::kLogin: {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{18, 20, 26, 255});
            title("Ninja Adventure", GetScreenHeight() / 2 - 200);
            subtitle("A name this world has not seen before becomes an account.",
                     GetScreenHeight() / 2 - 148);

            const int x = (GetScreenWidth() - kPanelW) / 2;
            int y = GetScreenHeight() / 2 - 90;
            DrawText("Name", x, y - 22, 16, Color{160, 170, 180, 255});
            text_field(Rectangle{static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(kPanelW), 40.0f},
                       st.name, kNameMax, st.editing_name, false, "who are you?");
            y += 62;
            DrawText("Password", x, y - 22, 16, Color{160, 170, 180, 255});
            text_field(Rectangle{static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(kPanelW), 40.0f},
                       st.pass, kPassMax, st.editing_pass, true, "");
            y += 58;

            if (st.login_message != nullptr) {
                const int mw = MeasureText(st.login_message, 16);
                DrawText(st.login_message, (GetScreenWidth() - mw) / 2, y, 16,
                         Color{224, 150, 140, 255});
            }
            y += 26;

            // Tab moves between the fields, Enter submits — the two things anyone typing a login
            // will try without being told.
            if (IsKeyPressed(KEY_TAB)) {
                st.editing_pass = !st.editing_pass;
                st.editing_name = !st.editing_pass;
            }
            const bool submit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
            static const char* items[] = {"Sign in", "Quit"};
            const int clicked = button_column(items, 2, y);
            if (clicked == 1) return Action::kQuit;
            if (clicked == 0 || submit) return Action::kSignIn;

            subtitle("Passwords are hashed with Argon2 and never stored. See assets/CREDITS.md.",
                     GetScreenHeight() - 60);
            return Action::kNone;
        }

        case Screen::kPlaying:
            draw_hud(st, status, player);
            return Action::kNone;

        case Screen::kMainMenu: {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{18, 20, 26, 255});
            title("Ninja Adventure", GetScreenHeight() / 2 - 190);
            subtitle("You wake in open country. Somewhere out there, people.",
                     GetScreenHeight() / 2 - 138);
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
            static const char* items[] = {"Resume", "Character", "Journal", "Quit to menu"};
            switch (button_column(items, 4, GetScreenHeight() / 2 - 70)) {
                case 0: st.screen = Screen::kPlaying; return Action::kResume;
                case 1: st.screen = Screen::kCharacter; break;
                case 2: st.screen = Screen::kJournal; break;
                case 3: st.screen = Screen::kMainMenu; break;
                default: break;
            }
            return Action::kNone;
        }

        case Screen::kCharacter:
            draw_character(player);
            return Action::kNone;

        case Screen::kJournal: {
            dim_background();
            const int x = (GetScreenWidth() - 620) / 2;
            int y = 70;
            DrawRectangle(x - 24, y - 34, 668, GetScreenHeight() - 110, Color{22, 24, 30, 240});
            DrawRectangleLines(x - 24, y - 34, 668, GetScreenHeight() - 110,
                               Color{110, 96, 70, 255});
            DrawText("Journal", x, y, 32, Color{240, 226, 190, 255});
            y += 48;

            DrawText("Controls", x, y, 20, Color{230, 206, 140, 255});
            y += 28;
            static const char* rows[] = {
                "WASD / arrows      move",
                "B                  swap between fighting and building",
                "Left mouse         swing   (hold Shift: a heavy blow)",
                "Right mouse        cast the selected school at the cursor",
                "Q / Space          loose an arrow at the cursor",
                "1 - 4              fire / ice / earth / shock",
                "R                  mount up - faster, but you cannot fight",
                "E                  harvest a ripe crop",
                "T / U              till a tile / upgrade what is under the cursor",
                "C                  character sheet",
                "J                  this journal",
                "Esc                pause",
            };
            for (const char* r : rows) {
                DrawText(r, x + 8, y, 17, Color{206, 210, 216, 255});
                y += 22;
            }
            y += 10;
            DrawText("Getting started", x, y, 20, Color{230, 206, 140, 255});
            y += 28;
            static const char* tips[] = {
                "Nothing is counting down. Nothing is chasing you.",
                "You start with nothing. Walk until you find a village.",
                "Most animals want nothing to do with you. Crowd one and it will mind.",
                "Hit a wolf and the whole pack minds.",
                "Further from the middle of the world is harder. That is the only rule.",
            };
            for (const char* t : tips) {
                DrawText(t, x + 8, y, 17, Color{176, 190, 176, 255});
                y += 22;
            }

            const Rectangle back{static_cast<float>(x),
                                 static_cast<float>(GetScreenHeight() - 108), 160.0f,
                                 static_cast<float>(kBtnH)};
            if (GuiButton(back, "Back")) st.screen = Screen::kPlaying;
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
                        int drawn_creatures) {
    const int w = 340;
    DrawRectangle(GetScreenWidth() - w - 12, 52, w, 208, Color{0, 0, 0, 190});
    int y = 60;
    const auto line = [&](const char* text) {
        DrawText(text, GetScreenWidth() - w - 2, y, 16, Color{150, 220, 150, 255});
        y += 20;
    };
    line("-- debug (F3) ------------------");
    line(TextFormat("fps               %d", GetFPS()));
    line(TextFormat("world tick        %llu",
                    static_cast<unsigned long long>(status.tick.load(std::memory_order_relaxed))));
    line(TextFormat("chunk actors      %d drawn", drawn_chunks));
    line(TextFormat("creatures shown   %d", drawn_creatures));
    line(TextFormat("creatures killed  %u",
                    status.creatures_killed.load(std::memory_order_relaxed)));
    line(TextFormat("  by players      %u", status.player_kills.load(std::memory_order_relaxed)));
    line(TextFormat("player deaths     %u", status.player_deaths.load(std::memory_order_relaxed)));
    // The headline number of the engine work: every one of these was a creature or an arrow handed
    // from one actor to another, and will be a network frame once chunks live on separate machines.
    line(TextFormat("chunk migrations  %u", status.migrations.load(std::memory_order_relaxed)));
    line(TextFormat("player tile       %d, %d", static_cast<int>(player.x),
                    static_cast<int>(player.y)));
}

}  // namespace mmo::ui
