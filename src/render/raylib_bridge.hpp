// raylib implementation of the render seam.
//
// This is the ONLY file in the project that knows raylib exists. It reads published snapshots and
// draws them; it never touches an actor, never sends a message, and never blocks the simulation.
// A Godot/GDExtension backend would be a sibling of this file and nothing else would move.
//
// The `IRenderBridge` methods are deliberately dumb — `begin_frame` / `draw` / `end_frame`. Input is
// read separately (`poll_input`) and handed back to the caller as intent, so the client's main loop
// stays the place where "input becomes messages" happens. Keeping that translation out of the
// renderer is what stops the renderer from growing game logic.
#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "world/snapshot.hpp"
#include "world/tiles.hpp"
#include "world/worldgen.hpp"

namespace mmo {

// What the player asked for this frame, in world terms. No raylib types cross this boundary.
//
// The left mouse button means two different things, and which one is decided by a MODE rather than
// by a modifier. Combat is the default and building is a mode you enter with `B`, because the
// alternative — one button that builds when you click ground and swings when you click a creature —
// makes the most dangerous moment in the game (something is on top of you) the moment the control
// is most ambiguous.
struct InputFrame {
    float move_x = 0.0f;  // -1..1
    float move_y = 0.0f;
    bool plant = false;
    bool harvest = false;
    bool build = false;
    bool till = false;
    bool upgrade = false;
    BuildKind build_kind = BuildKind::kHearth;
    bool build_mode = false;

    // --- combat ---
    bool swing = false;
    bool heavy = false;
    bool shoot = false;
    bool cast = false;
    Element element = Element::kFire;
    bool mount = false;   // edge-triggered toggle
    float aim_x = 0.0f;   // cursor, in map tiles (fractional)
    float aim_y = 0.0f;

    std::uint16_t cursor_tx = 0;
    std::uint16_t cursor_ty = 0;
    bool quit = false;
};

class RaylibBridge final : public IRenderBridge {
public:
    RaylibBridge(int width, int height);
    ~RaylibBridge() override;

    RaylibBridge(const RaylibBridge&) = delete;
    RaylibBridge& operator=(const RaylibBridge&) = delete;

    [[nodiscard]] bool begin_frame() override;
    // `local_slot` is which player the camera follows. Every OTHER live slot is drawn as another
    // character in the world — which is the whole visible difference between a keyed PlayerActor
    // and the singleton it replaced.
    void draw(const SnapshotBus& bus, const WorldStatus& status, const PlayerBus& players,
              int local_slot) override;
    void end_frame() override;

    // Reads the keyboard/mouse and converts to world intent, using the last drawn camera to turn
    // the mouse position into a tile.
    [[nodiscard]] InputFrame poll_input(const PlayerView& player) const;

    [[nodiscard]] float frame_time() const;

    // What the last `draw` actually put on screen. Read by the F3 debug overlay, which lives in the
    // UI shell rather than here.
    [[nodiscard]] int drawn_chunks() const;
    [[nodiscard]] int drawn_creatures() const;

    // What the player has selected, and which mode they are in. Owned here because `poll_input`
    // sets them from the number keys; the shell mirrors them into the hotbar.
    [[nodiscard]] BuildKind selected_build() const;
    [[nodiscard]] Element selected_element() const;
    [[nodiscard]] bool build_mode() const;

    // The generated world: which buildings stand where. Const for the world's lifetime, so it is
    // handed over once at start-up rather than passed every frame.
    void set_layout(const WorldLayout* layout);

    // Writes the current framebuffer to a PNG. Used by the `--shot` mode so the renderer can be
    // verified on a headless box (and in CI) instead of only by looking at it.
    void screenshot(const char* path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmo
