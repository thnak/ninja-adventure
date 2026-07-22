// The graphical client.
//
// Note what this loop does NOT do: it never waits for the simulation. It posts input as messages,
// advances the world clock on a fixed accumulator, and draws whatever snapshots happen to be
// published. If a chunk is mid-tick — or, later, mid-flight from another machine — the renderer
// draws that chunk's previous frame and moves on. Frame rate and simulation rate are independent
// by construction, which is the property that survives the move to a real cluster.
//
// Build:  cmake -B build -DMMO_BUILD_CLIENT=ON && cmake --build build -j4 --target mmo_client
// Run  :  taskset -c 0-3 build/mmo_client
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "render/raylib_bridge.hpp"
#include "ui/audio.hpp"
#include "ui/screens.hpp"
#include "world/world.hpp"

using namespace mmo;

int main(int argc, char** argv) {
    // `--shot <seconds> <file.png>` runs the client unattended, fast-forwards the world by the
    // requested number of world-seconds, writes one frame to disk and exits. It is how the render
    // path gets verified without a display.
    int shot_seconds = 0;
    const char* shot_path = nullptr;
    int look_camp = -1;            // --camp N: point the camera at spawn camp N instead of the farm
    const char* shot_screen = nullptr;  // --screen NAME: force a shell screen for the screenshot
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 2 < argc) {
            shot_seconds = std::atoi(argv[i + 1]);
            shot_path = argv[i + 2];
        } else if (std::strcmp(argv[i], "--camp") == 0 && i + 1 < argc) {
            look_camp = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            shot_screen = argv[i + 1];  // menu | journal | paused — for verifying the shell
        }
    }

    World world;
    world.build(/*workers*/ 4);
    world.start();

    RaylibBridge bridge(1280, 720);
    {
        const auto camps = World::camps(world.player_view().map);
        bridge.set_camps(std::vector<std::pair<int, int>>(camps.begin(), camps.end()));
    }

    // Fixed-step accumulator: the world advances in whole `kTickMs` steps regardless of frame time,
    // so simulation behaviour does not change with frame rate. Capped so a stall (window drag, alt-
    // tab) replays a bounded number of steps instead of a spiral of death.
    float accumulator = 0.0f;
    constexpr float kStepSec = static_cast<float>(kTickMs) / 1000.0f;
    constexpr int kMaxStepsPerFrame = 5;

    PlayerView player = world.player_view();
    float since_player_sync = 0.0f;

    // The shell owns which screen is up. In `--shot` mode it starts straight in the world so the
    // screenshot is of the game, not of a menu.
    ui::Audio audio;
    audio.start_music();

    ui::ShellState shell;
    if (shot_path != nullptr) {
        shell.screen = ui::Screen::kPlaying;
        if (shot_screen != nullptr) {
            if (std::strcmp(shot_screen, "menu") == 0) shell.screen = ui::Screen::kMainMenu;
            else if (std::strcmp(shot_screen, "journal") == 0) shell.screen = ui::Screen::kJournal;
            else if (std::strcmp(shot_screen, "paused") == 0) shell.screen = ui::Screen::kPaused;
        }
    }

    // Unattended mode: fast-forward, then seed a farm so the frame actually exercises every sprite
    // (crops at all four growth stages, walls, turrets, the core, mobs mid-siege) instead of showing
    // an empty field. Crops are back-dated by staggered amounts so one row spans seedling to ripe.
    if (shot_path != nullptr) {
        const int steps = shot_seconds * kTicksPerSecond;
        for (int i = 0; i < steps; ++i) world.step(kTickMs);
        world.sync_world();

        const auto map = player.map;
        const std::int64_t now = world.status().world_ms.load(std::memory_order_relaxed);
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 8; ++col) {
                const auto kind = static_cast<CropKind>(row % 3);
                // Age spans 0 .. full grow time across the row, so all four stages are on screen.
                const std::int64_t age = (grow_ms_of(kind) * col) / 7;
                world.plant(map, static_cast<std::uint16_t>(kHomeTx - 4 + col),
                            static_cast<std::uint16_t>(kHomeTy + 2 + row), kind, now - age);
            }
        }
        // A bracket around the core rather than a bare line: it exercises horizontal runs, vertical
        // runs and corners, which is exactly what the wall autotiling has to get right.
        for (int dx = -5; dx <= 5; ++dx) {
            world.build_at(map, static_cast<std::uint16_t>(kHomeTx + dx),
                           static_cast<std::uint16_t>(kHomeTy - 3), BuildKind::kWall);
        }
        for (int dy = -2; dy <= 2; ++dy) {
            world.build_at(map, static_cast<std::uint16_t>(kHomeTx - 5),
                           static_cast<std::uint16_t>(kHomeTy + dy), BuildKind::kWall);
            world.build_at(map, static_cast<std::uint16_t>(kHomeTx + 5),
                           static_cast<std::uint16_t>(kHomeTy + dy), BuildKind::kWall);
        }
        for (int dx = -4; dx <= 4; dx += 4) {
            world.build_at(map, static_cast<std::uint16_t>(kHomeTx + dx),
                           static_cast<std::uint16_t>(kHomeTy - 1), BuildKind::kTurret);
        }
        // Fence the flanks and upgrade one turret, so the demo frame shows every defensive system.
        for (int dy = -2; dy <= 3; ++dy) {
            world.build_at(map, static_cast<std::uint16_t>(kHomeTx - 7),
                           static_cast<std::uint16_t>(kHomeTy + dy), BuildKind::kFence);
            world.build_at(map, static_cast<std::uint16_t>(kHomeTx + 7),
                           static_cast<std::uint16_t>(kHomeTy + dy), BuildKind::kFence);
        }
        for (std::uint8_t lvl = 1; lvl < kMaxLevel; ++lvl) {
            world.upgrade(map, static_cast<std::uint16_t>(kHomeTx),
                          static_cast<std::uint16_t>(kHomeTy - 1), BuildKind::kTurret, lvl);
        }
        for (int i = 0; i < 20; ++i) world.step(kTickMs);  // let the chunks tick and publish
        world.sync_world();
        player = world.player_view();

        // The camera follows the player, so looking at a spawn camp means walking there. The move
        // is relative, hence the delta.
        if (look_camp >= 0 && look_camp < kSpawnCamps) {
            const auto camps = World::camps(player.map);
            const auto [ctx, cty] = camps[static_cast<std::size_t>(look_camp)];
            // Stand a few tiles off the camp so the portal is not hidden behind the player sprite.
            world.move_player(static_cast<float>(ctx) - player.x,
                              static_cast<float>(cty) + 6.0f - player.y);
            world.sync_world();
            player = world.player_view();
        }
    }
    int frames = 0;

    while (bridge.begin_frame()) {
        const float dt = std::min(bridge.frame_time(), 0.25f);

        // --- input becomes messages ---------------------------------------------------------------
        // The shell gets first refusal. When a menu is open it returns true and the world sees no
        // input at all — otherwise walking around behind the pause menu would still work.
        audio.update();
        const bool shell_took_input = ui::handle_shell_keys(shell);
        const InputFrame in =
            shell_took_input ? InputFrame{} : bridge.poll_input(player);
        if (in.quit) break;

        if (in.move_x != 0.0f || in.move_y != 0.0f) {
            world.move_player(in.move_x * kPlayerSpeed * dt, in.move_y * kPlayerSpeed * dt);
        }
        if (in.build) {
            if (world.build_at(player.map, in.cursor_tx, in.cursor_ty, in.build_kind)) {
                audio.play(ui::Sfx::kBuild);
            }
        }
        if (in.plant) {
            world.plant(player.map, in.cursor_tx, in.cursor_ty, CropKind::kWheat,
                        world.status().world_ms.load(std::memory_order_relaxed));
        }
        if (in.harvest) {
            world.harvest(player.map, in.cursor_tx, in.cursor_ty);
            audio.play(ui::Sfx::kHarvest);
        }
        if (in.till) {
            if (world.till(player.map, in.cursor_tx, in.cursor_ty)) audio.play(ui::Sfx::kBuild);
        }
        if (in.upgrade) {
            // The client has to look up what is standing on the tile, because the upgrade price
            // depends on the building's kind and current level. It reads that from the published
            // snapshot — the same lossy, read-only channel the renderer uses. Worst case the
            // snapshot is a tick stale and the trusted inventory rejects the debit, which is
            // exactly the outcome the ask-then-tell ordering exists to guarantee.
            const ChunkCoord cc = chunk_of(player.map, static_cast<float>(in.cursor_tx),
                                           static_cast<float>(in.cursor_ty));
            if (ChunkViewPtr cv = world.bus().load(cc)) {
                for (const Building& b : cv->buildings) {
                    if (b.tx != in.cursor_tx || b.ty != in.cursor_ty) continue;
                    world.upgrade(player.map, in.cursor_tx, in.cursor_ty, b.kind, b.level);
                    break;
                }
            }
        }

        // --- advance the world ---------------------------------------------------------------------
        // Note the world advances in EVERY screen except the main menu. Pausing the menu but not
        // the world is deliberate: the world is shared, and it cannot stop because one player
        // opened their journal.
        if (shell.screen == ui::Screen::kMainMenu) accumulator = 0.0f;
        accumulator += dt;
        int steps = 0;
        while (shell.screen != ui::Screen::kMainMenu && accumulator >= kStepSec &&
               steps < kMaxStepsPerFrame) {
            world.step(kTickMs);
            accumulator -= kStepSec;
            ++steps;
        }
        if (steps == kMaxStepsPerFrame) accumulator = 0.0f;  // drop the debt rather than chase it

        // --- read back ------------------------------------------------------------------------------
        // The player's position is the one value the camera cannot approximate, so it is asked for
        // ~20x a second rather than every frame — an ask is a round-trip, and at 60 fps most of
        // those round-trips would return a position the player has not visibly moved from.
        since_player_sync += dt;
        if (since_player_sync >= 0.05f) {
            player = world.player_view();
            since_player_sync = 0.0f;
        }

        world.status().mobs_alive.store(count_mobs(world.bus()), std::memory_order_relaxed);

        bridge.draw(world.bus(), world.status(), player);

        shell.selected_hotbar = static_cast<int>(bridge.selected_build()) - 1;
        if (shell.selected_hotbar < 0) shell.selected_hotbar = 0;
        const ui::Action act = ui::draw(shell, world.status(), player);
        if (shell.debug_overlay) {
            ui::draw_debug_overlay(world.status(), player, bridge.drawn_chunks(),
                                   bridge.drawn_mobs());
        }
        bridge.end_frame();
        if (act == ui::Action::kQuit) break;

        if (shot_path != nullptr && ++frames >= 3) {  // let the swap chain settle
            bridge.screenshot(shot_path);
            std::printf("wrote %s at world t=%.1fs\n", shot_path,
                        static_cast<double>(world.status().world_ms.load()) / 1000.0);
            break;
        }
    }

    world.stop();
    std::printf("client exited cleanly\n");
    return 0;
}
