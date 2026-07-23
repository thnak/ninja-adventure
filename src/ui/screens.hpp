// Screens — the shell every other system plugs into.
//
// The prototype printed its controls straight onto the world and put engine counters in the corner.
// That is a debug harness, not a game. This replaces it with an actual screen stack: you sign in,
// you arrive at a main menu, you pause into a menu, and the help text lives in a journal you open
// rather than permanently over the play area.
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

// Where the player is. The world keeps simulating in every state except `kMainMenu` and `kLogin` —
// pausing the menu but not the world is deliberate for a multiplayer game, where the world is
// shared and cannot stop just because one player opened their inventory.
enum class Screen : std::uint8_t {
    kLogin,
    kMainMenu,
    kPlaying,
    kPaused,
    kJournal,    // help, controls, and (later) quests
    kCharacter,  // portrait, vitals, skills
    kOptions,
};

// What the shell wants the caller to do next. Returned by `draw`, acted on by `client_main`.
enum class Action : std::uint8_t {
    kNone,
    kSignIn,  // the name/password boxes were submitted — the caller must call World::login
    kStartGame,
    kResume,
    kQuit,
};

inline constexpr int kNameMax = 24;
inline constexpr int kPassMax = 40;
inline constexpr int kJoinAddrMax = 64;  // "255.255.255.255:65535" and a hostname both fit

// Client-side, glanceable progression feedback the HUD owns and draws: the drifting "+N" XP motes
// and the centred level-up banner. Both are FIXED-SIZE pools filled by `client_main` (which already
// keeps a per-frame snapshot of the local player's skills for the level-up fanfare — the spawn calls
// below ride that same comparison rather than a second one) and animated inside `draw`. No raylib
// types cross this header, the same rule the rest of the shell keeps, so the pool is plain numbers
// and pre-formatted strings — the school name, level and any unlocked ability are resolved from the
// ability table at spawn time and stored, so drawing a frame touches no game state.
struct HudFeedback {
    struct XpMote {
        bool live = false;
        float age = 0.0f;          // seconds since it spawned; it rises and fades over ~1s
        int school = 0;            // which school gained the XP — tints the mote
        std::uint32_t amount = 0;  // the "+N"
        float dx = 0.0f;           // a little horizontal scatter so several do not stack
    };
    static constexpr int kMaxMotes = 24;
    XpMote motes[kMaxMotes] = {};

    // The banner. `age` starts past its lifetime so nothing draws until a level-up seeds it to 0.
    float banner_age = 1e9f;
    int banner_school = 0;
    char banner_top[24] = {};     // e.g. "MELEE 6"
    char banner_bottom[48] = {};  // e.g. "New ability: Crush Blow [G]", empty when none unlocked
};

// Called by `client_main` when the local player's `skill_xp` rose without a level (the level case is
// the banner's, below). Parks a "+N" mote in the pool, reusing the oldest slot when full.
void hud_spawn_xp_mote(HudFeedback& fx, int school, std::uint32_t amount);

// Called by `client_main` on any `skill_level` increase — the same frame the kLevelUp fanfare plays.
// Fills the banner and, if `level` is an ability's unlock level (read from the ability table, never
// hardcoded), the "New ability" line naming it and the slot key it lands on.
void hud_spawn_level_up(HudFeedback& fx, int school, int level);

struct ShellState {
    Screen screen = Screen::kLogin;
    bool debug_overlay = false;  // F3

    // NOTE: the selected element/building and build-vs-fight mode used to be MIRRORED here from the
    // renderer every frame, which was two sources of one truth waiting to drift. They are owned by
    // the bridge alone now (it is where the number keys and `B` are read), and passed straight into
    // `draw` for the one frame that needs them — see `draw`.

    // The sign-in box. Held here rather than in `client_main` because raygui is immediate-mode: the
    // text buffer has to survive between frames and the widget has to own the edit flag.
    char name[kNameMax] = "player";
    char pass[kPassMax] = {};
    bool editing_name = false;
    bool editing_pass = false;
    const char* login_message = nullptr;  // set by the caller after a failed attempt

    // The connection block, drawn ABOVE name/password. A multiplayer-in-a-cluster game has to ask
    // WHICH world you are signing into: host your own (default) or join a friend's leader. This is
    // the Minecraft/Valheim model from ARCHITECTURE §2 — the first node is the trusted leader and
    // friends join as nodes #2..N. `join_addr` is that leader's ip:port. Held here for the same
    // immediate-mode reason as name/pass: the buffer must outlive the frame.
    bool join_mode = false;
    char join_addr[kJoinAddrMax] = {};
    bool editing_addr = false;

    // Options that actually do something now. Both persist to client.cfg beside join/join_addr and
    // are applied live by `client_main` (which owns the Audio device the shell must not see). The
    // Options screen mutates them; the caller watches for the change and calls into Audio.
    int master_volume = 100;  // 0..100, straight into raylib's SetMasterVolume as 0..1
    bool music_on = true;

    // Progression feedback drawn over the HUD. Lives here because, like the sign-in buffers, it must
    // survive between immediate-mode frames.
    HudFeedback hud;
};

// Handles the keys that belong to the shell rather than to gameplay: Esc pauses/backs out, J opens
// the journal, C the character sheet, F3 toggles the debug overlay. Returns true if the shell
// consumed input this frame, in which case the caller must NOT also feed it to the world.
bool handle_shell_keys(ShellState& st);

// Draws whichever screen is current, plus the in-game HUD when playing. Call between the world
// renderer's draw and its end_frame, so UI lands on top. `build_mode` and `selected_slot` are the
// renderer's selection state (the single source of truth), passed in for the HUD rather than mirrored
// into the shell: `selected_slot` is the lit element (0..3) or building (0..1) in that mode.
[[nodiscard]] Action draw(ShellState& st, const WorldStatus& status, const PlayerView& player,
                          bool build_mode, int selected_slot);

// The F3 overlay: engine and simulation counters that used to sit permanently on the HUD. They are
// genuinely useful — just not to a player who is trying to farm.
void draw_debug_overlay(const WorldStatus& status, const PlayerView& player, int drawn_chunks,
                        int drawn_creatures);

}  // namespace mmo::ui
