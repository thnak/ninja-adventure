// Screens — the shell every other system plugs into.
//
// The prototype printed its controls straight onto the world and put engine counters in the corner.
// That is a debug harness, not a game. This replaces it with an actual screen stack: you arrive at
// a main menu, you pause into a menu, and the help text lives in a journal you open rather than
// permanently over the play area.
//
// WHY A SEPARATE TU: raygui is a single-header immediate-mode library, and exactly one translation
// unit may define `RAYGUI_IMPLEMENTATION`. Keeping that in `screens.cpp` means raygui's ~4k lines
// are compiled once and no other file needs to know it exists.
//
// This file deliberately exposes no raylib or raygui types — `client_main` drives the state machine
// and never learns which UI toolkit is underneath, the same seam `IRenderBridge` gives the world
// renderer.
#pragma once

#include <cstdint>

#include "world/snapshot.hpp"

namespace mmo::ui {

// Where the player is. The world keeps simulating in every state except `kMainMenu` — pausing the
// menu but not the world is deliberate for a multiplayer game, where the world is shared and cannot
// stop just because one player opened their inventory.
enum class Screen : std::uint8_t {
    kMainMenu,
    kPlaying,
    kPaused,
    kJournal,   // help, controls, and (later) quests
    kOptions,
};

// What the shell wants the caller to do next. Returned by `draw`, acted on by `client_main`.
enum class Action : std::uint8_t {
    kNone,
    kStartGame,
    kResume,
    kQuit,
};

struct ShellState {
    Screen screen = Screen::kMainMenu;
    bool debug_overlay = false;  // F3
    int selected_hotbar = 0;
};

// Handles the keys that belong to the shell rather than to gameplay: Esc pauses/backs out, J opens
// the journal, F3 toggles the debug overlay. Returns true if the shell consumed input this frame,
// in which case the caller must NOT also feed it to the world.
bool handle_shell_keys(ShellState& st);

// Draws whichever screen is current, plus the in-game HUD when playing. Call between the world
// renderer's draw and its end_frame, so UI lands on top.
[[nodiscard]] Action draw(ShellState& st, const WorldStatus& status, const PlayerView& player);

// The F3 overlay: engine and simulation counters that used to sit permanently on the HUD. They are
// genuinely useful — just not to a player who is trying to farm.
void draw_debug_overlay(const WorldStatus& status, const PlayerView& player, int drawn_chunks,
                        int drawn_mobs);

}  // namespace mmo::ui
