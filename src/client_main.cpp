// The graphical client.
//
// Note what this loop does NOT do: it never waits for the simulation. It posts input as messages,
// advances the world clock on a fixed accumulator, and draws whatever snapshots happen to be
// published. If a chunk is mid-tick — or, later, mid-flight from another machine — the renderer
// draws that chunk's previous frame and moves on. Frame rate and simulation rate are independent
// by construction, which is the property that survives the move to a real cluster.
//
// SIGN IN FIRST. The world is built and started before the login screen, because the world is not
// the player's — it belongs to the node, and at P6 it will already be running when someone connects.
// Until a slot is bound the client simply has nobody to follow.
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

namespace {

// Where the account table lives. One file beside the executable; P5 owns anything better.
constexpr const char* kAccountsPath = "accounts.dat";

}  // namespace

int main(int argc, char** argv) {
    // `--shot <seconds> <file.png>` runs the client unattended, fast-forwards the world by the
    // requested number of world-seconds, writes one frame to disk and exits. It is how the render
    // path gets verified without a display.
    int shot_seconds = 0;
    const char* shot_path = nullptr;
    int look_hold = -1;                 // --hold N: point the camera at stronghold N
    int look_village = -1;              // --village N: point the camera at village N
    const char* shot_screen = nullptr;  // --screen NAME: force a shell screen for the screenshot
    int look_ring = -1;                 // --ring N: park the camera in biome ring N
    int stage_fight = 0;                // --fight N: drop N creatures on the player before the shot
    int look_door = -1;                 // --door N: step onto door N, which takes you inside it
    int look_at_tx = -1;                // --at TX TY: park the camera on an arbitrary tile
    int look_at_ty = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 2 < argc) {
            shot_seconds = std::atoi(argv[i + 1]);
            shot_path = argv[i + 2];
        } else if (std::strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            look_hold = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--village") == 0 && i + 1 < argc) {
            look_village = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
            look_ring = std::atoi(argv[i + 1]);  // 0..4 — put the camera in that biome ring
        } else if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            shot_screen = argv[i + 1];  // menu | journal | character | paused | login
        } else if (std::strcmp(argv[i], "--door") == 0 && i + 1 < argc) {
            look_door = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--fight") == 0 && i + 1 < argc) {
            stage_fight = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--at") == 0 && i + 2 < argc) {
            look_at_tx = std::atoi(argv[i + 1]);
            look_at_ty = std::atoi(argv[i + 2]);
        }
    }

    World world;
    world.build(/*workers*/ 4);
    world.load_accounts(kAccountsPath);
    world.start();

    RaylibBridge bridge(1280, 720);
    bridge.set_layout(&world.layout());

    // Fixed-step accumulator: the world advances in whole `kTickMs` steps regardless of frame time,
    // so simulation behaviour does not change with frame rate. Capped so a stall (window drag, alt-
    // tab) replays a bounded number of steps instead of a spiral of death.
    float accumulator = 0.0f;
    constexpr float kStepSec = static_cast<float>(kTickMs) / 1000.0f;
    constexpr int kMaxStepsPerFrame = 5;

    int slot = -1;  // which session slot this client is signed into; -1 until login
    std::uint64_t me = 0;

    ui::Audio audio;
    audio.start_music();
    ui::ShellState shell;

    // Unattended mode signs itself in, so a screenshot is of the game rather than of a login box.
    if (shot_path != nullptr) {
        LoginOutcome out{};
        slot = world.login("screenshot", "screenshot", out);
        me = world.key_of(slot);
        shell.screen = ui::Screen::kPlaying;
        if (shot_screen != nullptr) {
            if (std::strcmp(shot_screen, "menu") == 0) shell.screen = ui::Screen::kMainMenu;
            else if (std::strcmp(shot_screen, "journal") == 0) shell.screen = ui::Screen::kJournal;
            else if (std::strcmp(shot_screen, "character") == 0) shell.screen = ui::Screen::kCharacter;
            else if (std::strcmp(shot_screen, "paused") == 0) shell.screen = ui::Screen::kPaused;
            else if (std::strcmp(shot_screen, "login") == 0) shell.screen = ui::Screen::kLogin;
        }
    }

    const auto view = [&]() -> PlayerView {
        if (slot < 0) return PlayerView{};
        PlayerViewPtr p = world.players().load(slot);
        return p ? *p : PlayerView{};
    };

    // Unattended mode: fast-forward, then point the camera somewhere worth photographing.
    //
    // It no longer seeds a farm. It used to build a walled compound with turrets and rows of crops
    // around the map centre, because that was the only content in the game — and it quietly meant
    // every screenshot was of a thing the demo had just constructed rather than of the world. Now
    // the world generates villages, so the honest shot is one of a village.
    if (shot_path != nullptr) {
        const int steps = shot_seconds * kTicksPerSecond;
        for (int i = 0; i < steps; ++i) world.step(kTickMs);
        world.sync_world();
        PlayerView player = view();

        // The camera follows the player, so looking at something means walking there. The move is
        // relative, hence the delta.
        // Park the camera at the MIDDLE radius of the requested ring. An earlier version walked
        // outward a step at a time and capped the step count, which could not physically reach the
        // outer rings along a diagonal — rings 2, 3 and 4 all produced the same screenshot.
        if (look_ring >= 0 && look_ring < kRingCount) {
            const float inner = look_ring == 0 ? 0.0f : kRingEdge[look_ring - 1];
            const float outer = std::min(kRingEdge[look_ring], 0.99f);
            const float mid = (inner + outer) * 0.5f;
            const float half = static_cast<float>(kMapTiles) * 0.5f;
            // Chebyshev radius `mid` means max(|dx|,|dy|) == mid*half; lead with x and take a
            // fraction of it in y so the shot is not on a perfect diagonal.
            const float bx = half + mid * half;
            const float by = half + mid * half * 0.35f;
            world.teleport_player(me, kOverworld, bx, by);
            world.sync_world();
            player = view();
        }
        const auto& lay = world.layout();
        if (look_village >= 0 && look_village < static_cast<int>(lay.villages().size())) {
            const Village& v = lay.villages()[static_cast<std::size_t>(look_village)];
            // Stand a little south of the square: the houses are drawn from their bottom edge, so
            // looking from below is what shows their faces rather than their roofs.
            world.teleport_player(me, kOverworld, static_cast<float>(v.tx),
                                  static_cast<float>(v.ty) + 5.0f);
            world.sync_world();
            player = view();
        }
        // Stepping onto a doorway is all it takes — the portal is in the player actor, so this
        // exercises the real path rather than a screenshot-only teleport into the room.
        if (look_door >= 0 && look_door < static_cast<int>(lay.doors().size())) {
            const Door& d = lay.doors()[static_cast<std::size_t>(look_door)];
            world.teleport_player(me, kOverworld, static_cast<float>(d.tile & 0xFFFFu) + 0.5f,
                                  static_cast<float>(d.tile >> 16) + 0.5f);
            world.sync_world();
            player = view();
        }
        if (look_hold >= 0 && look_hold < static_cast<int>(lay.strongholds().size())) {
            const Stronghold& h = lay.strongholds()[static_cast<std::size_t>(look_hold)];
            world.teleport_player(me, kOverworld, static_cast<float>(h.tx),
                                  static_cast<float>(h.ty) + 6.0f);
            world.sync_world();
            player = view();
        }
        // An arbitrary tile, the same relative-teleport style as --village/--hold. This is what shoots
        // a forest camp: mmo_worldmap prints camp coordinates, and --at drops the camera on one.
        if (look_at_tx >= 0 && look_at_ty >= 0) {
            world.teleport_player(me, kOverworld, static_cast<float>(look_at_tx) + 0.5f,
                                  static_cast<float>(look_at_ty) + 0.5f);
            world.sync_world();
            player = view();
        }
        for (int i = 0; i < 20; ++i) world.step(kTickMs);  // let the chunks tick and publish
        world.sync_world();
        player = view();

        // Stage a fight for the combat screenshot: creatures on top of the player, one of each
        // effect going off, so the shot shows the system rather than an empty field.
        if (stage_fight > 0) {
            world.spawn_wave_at(static_cast<std::uint16_t>(player.x),
                                static_cast<std::uint16_t>(player.y), CreatureKind::kSlime,
                                static_cast<std::uint16_t>(stage_fight));
            // Only long enough for the beacon to reach the chunk and the slimes to close. Any
            // longer and they simply kill the photographer — the first attempt ran 30 ticks with
            // ten slimes and produced a screenshot of the respawn timer.
            for (int i = 0; i < 5; ++i) world.step(kTickMs);
            world.sync_world();
            player = view();
            world.cast(me, Element::kIce, player.x + 2.0f, player.y - 1.0f);
            world.shoot(me, player.x + 8.0f, player.y + 1.0f);
            world.swing(me, /*heavy*/ true);
            world.step(kTickMs);
            world.sync_world();
        }
    }
    int frames = 0;

    while (bridge.begin_frame()) {
        const float dt = std::min(bridge.frame_time(), 0.25f);
        const PlayerView player = view();

        // --- input becomes messages ---------------------------------------------------------------
        // The shell gets first refusal. When a menu is open it returns true and the world sees no
        // input at all — otherwise walking around behind the pause menu would still work.
        audio.update();
        const bool shell_took_input = ui::handle_shell_keys(shell);
        const InputFrame in = shell_took_input ? InputFrame{} : bridge.poll_input(player);
        if (in.quit) break;

        // Dead players do not act. The countdown is the entire cost of dying, and being able to
        // keep swinging through it would delete it.
        const bool can_act = slot >= 0 && player.live() && player.dead_ticks == 0;

        if (can_act && (in.move_x != 0.0f || in.move_y != 0.0f)) {
            const float speed = player.mounted ? kMountSpeed : kPlayerSpeed;
            world.move_player(me, in.move_x * speed * dt, in.move_y * speed * dt);
        }
        if (can_act && in.mount) world.set_mounted(me, !player.mounted);

        if (can_act && in.swing) {
            if (world.swing(me, in.heavy)) audio.play(ui::Sfx::kHarvest);
        }
        if (can_act && in.cast) world.cast(me, in.element, in.aim_x, in.aim_y);
        if (can_act && in.shoot) world.shoot(me, in.aim_x, in.aim_y);

        if (can_act && in.build) {
            if (world.build_at(me, player.map, in.cursor_tx, in.cursor_ty, in.build_kind)) {
                audio.play(ui::Sfx::kBuild);
            }
        }
        if (can_act && in.plant) {
            world.plant(me, player.map, in.cursor_tx, in.cursor_ty, CropKind::kWheat,
                        world.status().world_ms.load(std::memory_order_relaxed));
        }
        if (can_act && in.harvest) {
            world.harvest(me, player.map, in.cursor_tx, in.cursor_ty);
            audio.play(ui::Sfx::kHarvest);
        }
        if (can_act && in.till) {
            if (world.till(me, player.map, in.cursor_tx, in.cursor_ty)) audio.play(ui::Sfx::kBuild);
        }
        if (can_act && in.upgrade) {
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
                    world.upgrade(me, player.map, in.cursor_tx, in.cursor_ty, b.kind, b.level);
                    break;
                }
            }
        }

        // --- advance the world ---------------------------------------------------------------------
        // Note the world advances in EVERY screen except the main menu and the sign-in box. Pausing
        // the menu but not the world is deliberate: the world is shared, and it cannot stop because
        // one player opened their journal.
        const bool world_frozen =
            shell.screen == ui::Screen::kMainMenu || shell.screen == ui::Screen::kLogin;
        if (world_frozen) accumulator = 0.0f;
        accumulator += dt;
        int steps = 0;
        while (!world_frozen && accumulator >= kStepSec && steps < kMaxStepsPerFrame) {
            world.step(kTickMs);
            accumulator -= kStepSec;
            ++steps;
        }
        if (steps == kMaxStepsPerFrame) accumulator = 0.0f;  // drop the debt rather than chase it

        // --- draw -----------------------------------------------------------------------------------
        // No `ask` anywhere in this loop any more. The player's position used to be fetched ~20x a
        // second with a blocking round-trip; it now arrives on the same published-snapshot channel
        // as everything else (PlayerBus), which is what snapshot.hpp said all along and is the shape
        // that survives the player actor living on another machine.
        world.status().creatures_alive.store(count_creatures(world.bus()),
                                             std::memory_order_relaxed);

        bridge.draw(world.bus(), world.status(), world.players(), slot < 0 ? 0 : slot);

        shell.build_mode = bridge.build_mode();
        shell.selected_hotbar = shell.build_mode
                                    ? static_cast<int>(bridge.selected_build())
                                    : static_cast<int>(bridge.selected_element()) - 1;
        const ui::Action act = ui::draw(shell, world.status(), player);
        if (shell.debug_overlay) {
            ui::draw_debug_overlay(world.status(), player, bridge.drawn_chunks(),
                                   bridge.drawn_creatures());
        }
        bridge.end_frame();

        if (act == ui::Action::kSignIn) {
            LoginOutcome out{};
            const int s = world.login(shell.name, shell.pass, out);
            if (s >= 0) {
                slot = s;
                me = world.key_of(slot);
                shell.screen = ui::Screen::kPlaying;
                shell.login_message = nullptr;
                world.save_accounts(kAccountsPath);
                std::printf("signed in as '%s' (%s) -> slot %d\n", shell.name, describe(out), slot);
            } else {
                shell.login_message = describe(out);
            }
            // Wipe the password out of process memory the moment it has been used. It is a small
            // thing and it is free, and the alternative is a plaintext password sitting in a UI
            // struct for the rest of the session.
            std::memset(shell.pass, 0, sizeof shell.pass);
        }
        if (act == ui::Action::kQuit) break;

        if (shot_path != nullptr && ++frames >= 3) {  // let the swap chain settle
            bridge.screenshot(shot_path);
            std::printf("wrote %s at world t=%.1fs\n", shot_path,
                        static_cast<double>(world.status().world_ms.load()) / 1000.0);
            break;
        }
    }

    world.stop();
    if (world.accounts().size() > 0) world.save_accounts(kAccountsPath);
    std::printf("client exited cleanly\n");
    return 0;
}
