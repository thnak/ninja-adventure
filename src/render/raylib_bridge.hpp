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

namespace mmo {

// What the player asked for this frame, in world terms. No raylib types cross this boundary.
struct InputFrame {
    float move_x = 0.0f;  // -1..1
    float move_y = 0.0f;
    bool plant = false;
    bool harvest = false;
    bool build = false;
    bool till = false;
    bool upgrade = false;
    BuildKind build_kind = BuildKind::kWall;
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
    void draw(const SnapshotBus& bus, const WorldStatus& status, const PlayerView& player) override;
    void end_frame() override;

    // Reads the keyboard/mouse and converts to world intent, using the last drawn camera to turn
    // the mouse position into a tile.
    [[nodiscard]] InputFrame poll_input(const PlayerView& player) const;

    [[nodiscard]] float frame_time() const;

    // What the last `draw` actually put on screen. Read by the F3 debug overlay, which lives in the
    // UI shell rather than here.
    [[nodiscard]] int drawn_chunks() const;
    [[nodiscard]] int drawn_mobs() const;

    // The build type the player has selected. Owned here because `poll_input` sets it from the
    // number keys; the shell mirrors it into the hotbar highlight.
    [[nodiscard]] BuildKind selected_build() const;

    // Spawn-camp tiles for the current map. Static for the world's lifetime (a pure function of the
    // seed), so this is set once at start-up rather than passed every frame.
    void set_camps(const std::vector<std::pair<int, int>>& camps);

    // Writes the current framebuffer to a PNG. Used by the `--shot` mode so the renderer can be
    // verified on a headless box (and in CI) instead of only by looking at it.
    void screenshot(const char* path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmo
