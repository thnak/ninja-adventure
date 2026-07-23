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

// Per-machine connection state: which mode the player last chose (host vs join) and the leader
// address they last typed. It sits beside accounts.dat and is treated the same way — machine state,
// not source, so it is gitignored. Plain text and failure-tolerant on purpose: a missing or garbled
// file means defaults, never a crash. P5 (the real cluster join) owns anything better.
constexpr const char* kClientCfgPath = "client.cfg";

void load_client_cfg(const char* path, ui::ShellState& shell) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return;  // no file yet -> defaults, which is the intended first-run behaviour
    char line[128];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::strncmp(line, "join=", 5) == 0) {
            shell.join_mode = std::atoi(line + 5) != 0;
        } else if (std::strncmp(line, "join_addr=", 10) == 0) {
            char* v = line + 10;
            v[std::strcspn(v, "\r\n")] = '\0';  // strip the newline fgets leaves on
            std::snprintf(shell.join_addr, sizeof shell.join_addr, "%s", v);
        }
        // Unknown keys are ignored — a newer build's cfg must not trip an older one.
    }
    std::fclose(f);
}

void save_client_cfg(const char* path, const ui::ShellState& shell) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;  // best-effort; a write failure here is never worth aborting a login
    std::fprintf(f, "join=%d\njoin_addr=%s\n", shell.join_mode ? 1 : 0, shell.join_addr);
    std::fclose(f);
}

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
    bool stage_ability = false;         // --ability: level into Magic and fire Nova + RainCall
    bool stage_telegraph = false;       // --telegraph: one slime frozen mid-wind-up, for the F2 read
    bool stage_flash = false;           // --flash: one slime, struck between frames, for the F2 flash
    bool stage_walk = false;            // --walk: keep the player moving so a shot catches mid-stride
    int look_door = -1;                 // --door N: step onto door N, which takes you inside it
    int look_at_tx = -1;                // --at TX TY: park the camera on an arbitrary tile
    int look_at_ty = -1;
    const char* connect_addr = nullptr;  // --connect HOST:PORT: prefill the login screen's join mode
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
        } else if (std::strcmp(argv[i], "--ability") == 0) {
            stage_ability = true;  // level into Magic, then fire Nova + RainCall for the shot
        } else if (std::strcmp(argv[i], "--telegraph") == 0) {
            stage_telegraph = true;  // one slime, stepped to its wind-up and left frozen there
        } else if (std::strcmp(argv[i], "--flash") == 0) {
            stage_flash = true;  // one slime, struck inside the render loop so its hit flash shows
        } else if (std::strcmp(argv[i], "--walk") == 0) {
            stage_walk = true;
        } else if (std::strcmp(argv[i], "--at") == 0 && i + 2 < argc) {
            look_at_tx = std::atoi(argv[i + 1]);
            look_at_ty = std::atoi(argv[i + 2]);
        } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            connect_addr = argv[i + 1];  // HOST:PORT of a friend's leader
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

    // Restore the last connection choice, then let `--connect` override it. A flag is an explicit
    // request for this run, so it wins over whatever the file remembered.
    load_client_cfg(kClientCfgPath, shell);
    if (connect_addr != nullptr) {
        shell.join_mode = true;
        std::snprintf(shell.join_addr, sizeof shell.join_addr, "%s", connect_addr);
    }

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
            // Step only until a slime has COMMITTED to a telegraphed swing (F2): the shot then catches
            // it frozen mid-wind-up — warm-tinted, micro-shaking, with the smoke puff at its feet —
            // which is the whole point the walk-only art cannot show on its own. Bounded so a chunk
            // that never produced a commit (all slimes died) still ends. A slime's wind-up is on the
            // published creature, read from the same bus the renderer draws.
            const auto any_winding_up = [&]() -> bool {
                const int pcx = static_cast<int>(player.x) / kChunkTiles;
                const int pcy = static_cast<int>(player.y) / kChunkTiles;
                for (int cy = std::max(0, pcy - 1); cy <= std::min(kMapChunks - 1, pcy + 1); ++cy) {
                    for (int cx = std::max(0, pcx - 1); cx <= std::min(kMapChunks - 1, pcx + 1); ++cx) {
                        ChunkViewPtr v = world.bus().load(
                            ChunkCoord{player.map, static_cast<std::uint16_t>(cx),
                                       static_cast<std::uint16_t>(cy)});
                        if (!v) continue;
                        for (const Creature& c : v->creatures)
                            if (c.windup > 0) return true;
                    }
                }
                return false;
            };
            for (int i = 0; i < 16; ++i) {
                world.step(kTickMs);
                world.sync_world();
                if (any_winding_up()) break;
            }
            player = view();
            // No elemental cast here on purpose: an ice/fire status recolours the sprite and would
            // mask the warm wind-up tint the shot is meant to show. An arrow in flight is neutral.
            world.shoot(me, player.x + 8.0f, player.y + 1.0f);
            world.sync_world();
        }

        // Stage the ability layer for a screenshot: level the shot account into Magic (so both magic
        // slots are live), drop a wave, then call rain and fire a nova. The shot then shows the wet
        // ZONE rendering, the nova RING going off, and both ability slots mid-cooldown in the HUD.
        if (stage_ability) {
            world.grant_xp(me, Skill::kMagic, 20000);  // Magic past 10 — Nova (A) and RainCall (B)
            world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            world.sync_world();
            player = view();
            world.spawn_wave_at(static_cast<std::uint16_t>(player.x),
                                static_cast<std::uint16_t>(player.y), CreatureKind::kSlime, 6);
            for (int i = 0; i < 4; ++i) world.step(kTickMs);
            world.sync_world();
            player = view();
            // RainCall first so its zone is established and raining by the time the frame is taken;
            // then the nova, one tick before the shot so its ring is caught mid-animation.
            world.use_ability(me, 1, Element::kShock, player.x, player.y);
            for (int i = 0; i < 4; ++i) world.step(kTickMs);
            world.sync_world();
            // Both magic abilities together cost more than one mana bar holds, and four ticks of six
            // slimes have dented the photographer — so refill before the nova. The staging is showing
            // off both abilities at once, not testing the economy or survival.
            world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            world.use_ability(me, 0, Element::kShock, player.x, player.y);
            world.step(kTickMs);
            world.sync_world();
        }

        // Telegraph read (F2): a single slime placed just in reach and stepped ONLY until it commits
        // to a swing, then left there. The world is not advanced again before the shot (no per-frame
        // swinging as --fight does), so the slime stays frozen mid-wind-up — warm-glowing,
        // micro-shaking, its commit smoke puff still at its feet — which is exactly the telegraph the
        // walk-only art cannot show on its own. One creature, so nothing occludes the read.
        if ((stage_telegraph || stage_flash) && slot >= 0) {
            world.grant_vitals(me, kPlayerMaxHp, kPlayerMaxMana, kPlayerMaxStamina);
            world.sync_world();
            player = view();
            // Find OPEN grass: a 3x3 of plain grass, so no tree canopy draws over the read and the
            // white puff has a green backdrop rather than a stone floor or snow to vanish into.
            const auto all_grass = [&](int tx, int ty) {
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (terrain_of(kWorldSeed, player.map, tx + dx, ty + dy) != Terrain::kGrass)
                            return false;
                return true;
            };
            int ptx = static_cast<int>(player.x), pty = static_cast<int>(player.y);
            for (int r = 0; r < 60; ++r) {
                bool found = false;
                for (int dy = -r; dy <= r && !found; ++dy)
                    for (int dx = -r; dx <= r && !found; ++dx) {
                        if (std::abs(dx) != r && std::abs(dy) != r) continue;
                        const int tx = static_cast<int>(player.x) + dx;
                        const int ty = static_cast<int>(player.y) + dy;
                        if (tx > 1 && ty > 1 && tx < kMapTiles - 1 && ty < kMapTiles - 1 &&
                            all_grass(tx, ty)) {
                            ptx = tx;
                            pty = ty;
                            found = true;
                        }
                    }
                if (found) break;
            }
            world.teleport_player(me, player.map, static_cast<float>(ptx) + 0.5f,
                                  static_cast<float>(pty) + 0.5f);
            world.sync_world();
            // Telegraph: adjacent (distance 1.0), inside the slime's reach so it commits and puffs.
            // Flash: diagonal (distance ~1.41), OUTSIDE the slime's 1.0 reach so it never winds up and
            // never puffs — but inside the player's 1.9 swing reach, so the strike lands on a clean
            // sprite and its white flash is not buried under its own attack smoke.
            const auto stx = static_cast<std::uint16_t>(ptx + 1);
            const auto sty = static_cast<std::uint16_t>(stage_flash ? pty + 1 : pty);
            world.spawn_one_at(stx, sty, CreatureKind::kSlime, player.map);
            const ChunkCoord sc =
                chunk_of(player.map, static_cast<float>(stx), static_cast<float>(sty));
            if (stage_telegraph) {
                bool committed = false;
                for (int i = 0; i < 24 && !committed; ++i) {
                    world.step(kTickMs);
                    world.sync_world();
                    if (ChunkViewPtr v = world.bus().load(sc))
                        for (const Creature& c : v->creatures)
                            if (c.windup == stats_of(CreatureKind::kSlime).windup) committed = true;
                }
                // A few ticks past the commit: the wind-up counter is still above zero (the slime is
                // 4 ticks, so three steps leaves it at 1 and still red-glowing), while the commit puff
                // has begun to disperse so it frames the slime rather than burying it. This is the
                // frame that reads as "a slime rearing back to hit" — smoke AND a warm charge, both.
                if (committed) {
                    for (int i = 0; i < 2; ++i) world.step(kTickMs);
                    world.sync_world();
                }
            } else {
                // Flash mode: step twice so the slime is PUBLISHED at full HP before the render loop
                // begins — a creature does not appear in a view until a tick has published it, and the
                // flash needs a full-HP baseline frame to measure the drop against. The slime is out
                // of its own reach so it never attacks; the only thing that touches it is the player's
                // strike, landed inside the render loop (below) so the HP drop falls BETWEEN two drawn
                // frames, which is the one thing the client turns into a hit flash.
                for (int i = 0; i < 2; ++i) world.step(kTickMs);
                world.sync_world();
            }
        }
    }
    int frames = 0;

    // Audio bookkeeping for the sim-driven cues below. `effect_tick` remembers, per chunk, the last
    // published tick whose effects were already sounded, so a combo/hit cue fires once per event and
    // not once per render frame. `last_skill` mirrors the local player's levels so a level-up can be
    // heard the frame it happens. Both are fixed-size and allocated once — no per-frame allocation.
    std::vector<std::uint64_t> effect_tick(static_cast<std::size_t>(kChunkCount), ~0ull);
    std::uint8_t last_skill[kSkillCount] = {};
    bool have_skills = false;

    while (bridge.begin_frame()) {
        const float dt = std::min(bridge.frame_time(), 0.25f);
        const PlayerView player = view();

        // --walk: a screenshot fast-forwards the world OUTSIDE this loop, so the renderer never sees
        // the move counter change across its own frames and always reads the player as standing.
        // Nudge the player east and tick the world every frame instead, so `steps` advances between
        // draws and the shot catches the walk cycle. Shot-only staging; a live client never sets it.
        if (stage_walk && shot_path != nullptr && slot >= 0) {
            world.move_player(me, kPlayerSpeed * 0.08f, 0.0f);
            world.step(kTickMs);
            world.sync_world();
        }

        // --fight shot: swing and step ONCE PER RENDERED FRAME so a slime's HP drops BETWEEN two
        // draws — that frame-to-frame drop is exactly what the client turns into a hit flash (F2), and
        // it can only be photographed if the hit lands inside the render loop, not before it. The
        // fixed accumulator otherwise advances the world zero ticks across the three settle frames.
        if (stage_fight > 0 && shot_path != nullptr && slot >= 0) {
            // Skip the swing on the FIRST rendered frame so that frame establishes the creatures'
            // baseline HP, then land it on the next — the HP drop now falls BETWEEN two draws, which
            // is the only thing the client turns into a hit flash. Swinging on frame zero would bake
            // the drop into the baseline and no flash would ever show.
            if (frames >= 1) world.swing(me, /*heavy*/ true);
            world.step(kTickMs);
            world.sync_world();
        }
        // --flash shot: the single-slime version of the same idea — one clean creature struck EXACTLY
        // ONCE after the baseline frame, so it survives to keep flashing (a second heavy swing would
        // kill it and the shot would catch a death puff instead of the flash).
        if (stage_flash && shot_path != nullptr && slot >= 0 && frames == 1) {
            world.swing(me, /*heavy*/ false);  // LIGHT: a heavy blow can one-shot a meadow slime
            world.step(kTickMs);
            world.sync_world();
        }

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
            // The swing's own whoosh, light or heavy — no longer the harvest jingle standing in for
            // it. (Harvest keeps harvest.wav, below.) The hit that lands is a separate cue, played
            // off the effect the chunk publishes rather than guessed here, because a swing that
            // connects is decided by the simulation and not by this client.
            if (world.swing(me, in.heavy))
                audio.play(in.heavy ? ui::Sfx::kSwingHeavy : ui::Sfx::kSwing);
        }
        if (can_act && in.cast) {
            if (world.cast(me, in.element, in.aim_x, in.aim_y)) audio.play(ui::Sfx::kCast);
        }
        if (can_act && in.shoot) {
            if (world.shoot(me, in.aim_x, in.aim_y)) audio.play(ui::Sfx::kShoot);
        }
        // The two ability slots (F / G). Which ability each is, and whether it may fire, is the
        // trusted actor's call — the client just names the slot and passes the current element (for
        // Nova) and the cursor (to aim FanVolley). A refusal (cooldown, locked, empty bar) is silent.
        if (can_act && in.ability_a) {
            if (world.use_ability(me, 0, in.element, in.aim_x, in.aim_y)) audio.play(ui::Sfx::kCast);
        }
        if (can_act && in.ability_b) {
            if (world.use_ability(me, 1, in.element, in.aim_x, in.aim_y)) audio.play(ui::Sfx::kCast);
        }

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
        // The single-creature F2 shots (--telegraph / --flash) freeze the accumulator so ONLY their
        // own explicit per-frame steps advance the world. The accumulator is driven by real frame
        // time, which on a headless render varies run to run — left running it would step the world a
        // different number of ticks each time and the slime would sometimes die instead of flash, or
        // drift out of its wind-up. Freezing it makes these staged shots deterministic.
        const bool world_frozen =
            shell.screen == ui::Screen::kMainMenu || shell.screen == ui::Screen::kLogin ||
            ((stage_flash || stage_telegraph) && shot_path != nullptr);
        if (world_frozen) accumulator = 0.0f;
        accumulator += dt;
        int steps = 0;
        while (!world_frozen && accumulator >= kStepSec && steps < kMaxStepsPerFrame) {
            world.step(kTickMs);
            accumulator -= kStepSec;
            ++steps;
        }
        if (steps == kMaxStepsPerFrame) accumulator = 0.0f;  // drop the debt rather than chase it

        // --- combat audio, driven by what the simulation published ---------------------------------
        // The renderer already reads effects out of the visible chunks; audio does the same, but a
        // cue must fire ONCE per event, not once per render frame. Effects carry no id, so the robust
        // key is the immutable (chunk, tick) pair: a watched chunk republishes a fresh view every
        // tick, and within one view the just-born effects are exactly those at age 1 — one
        // step_effects has run since the verb that made them (the swing/cast is told before the tick,
        // the effect ages by one, then the chunk publishes). Recording the last tick reacted to per
        // chunk means each simulation frame is handled exactly once whatever the frame rate, and a
        // plain `!=` is immune to the (never-reached) wrap of a uint64 tick. Only the player's own
        // chunk neighbourhood is scanned — every effect a player causes lands within a chunk or two.
        if (slot >= 0) {
            const int pcx = static_cast<int>(player.x) / kChunkTiles;
            const int pcy = static_cast<int>(player.y) / kChunkTiles;
            for (int cy = std::max(0, pcy - 2); cy <= std::min(kMapChunks - 1, pcy + 2); ++cy) {
                for (int cx = std::max(0, pcx - 2); cx <= std::min(kMapChunks - 1, pcx + 2); ++cx) {
                    const ChunkCoord cc{player.map, static_cast<std::uint16_t>(cx),
                                        static_cast<std::uint16_t>(cy)};
                    ChunkViewPtr v = world.bus().load(cc);
                    if (!v) continue;
                    std::uint64_t& seen = effect_tick[static_cast<std::size_t>(chunk_index(cc))];
                    if (v->tick == seen) continue;  // this simulation frame is already sounded
                    seen = v->tick;
                    for (const Effect& e : v->effects) {
                        if (e.age != 1) continue;  // only on the tick it was born
                        // A combo detonation is its own cue; every other flash is a blow landing —
                        // the kHit sound (hit.wav) that until now was loaded and never played.
                        audio.play(e.kind == EffectKind::kBlast ? ui::Sfx::kCombo : ui::Sfx::kHit);
                    }
                }
            }

            // A skill going up a level is worth a fanfare. The first sighting only seeds the baseline
            // so signing in does not itself sound like a level-up.
            if (!have_skills) {
                for (int i = 0; i < kSkillCount; ++i) last_skill[i] = player.skill_level[i];
                have_skills = player.live();
            } else {
                for (int i = 0; i < kSkillCount; ++i) {
                    if (player.skill_level[i] > last_skill[i]) audio.play(ui::Sfx::kLevelUp);
                    last_skill[i] = player.skill_level[i];
                }
            }
        }

        // --- draw -----------------------------------------------------------------------------------
        // No `ask` anywhere in this loop any more. The player's position used to be fetched ~20x a
        // second with a blocking round-trip; it now arrives on the same published-snapshot channel
        // as everything else (PlayerBus), which is what snapshot.hpp said all along and is the shape
        // that survives the player actor living on another machine.
        world.status().creatures_alive.store(count_creatures(world.bus()),
                                             std::memory_order_relaxed);

        bridge.draw(world.bus(), world.status(), world.players(), slot < 0 ? 0 : slot);

        // The renderer is the single owner of the element/build selection and the mode (it reads the
        // number keys and B). Read them once here and pass them straight to the HUD, rather than
        // mirroring them into the shell where a second copy could drift.
        const bool build_mode = bridge.build_mode();
        const int selected_slot = build_mode ? static_cast<int>(bridge.selected_build())
                                             : static_cast<int>(bridge.selected_element()) - 1;
        const ui::Action act =
            ui::draw(shell, world.status(), player, build_mode, selected_slot);
        if (shell.debug_overlay) {
            ui::draw_debug_overlay(world.status(), player, bridge.drawn_chunks(),
                                   bridge.drawn_creatures());
        }
        bridge.end_frame();

        if (act == ui::Action::kSignIn) {
            // Remember the connection choice for next launch, regardless of how the attempt below
            // goes. It is machine convenience state, the same category as accounts.dat.
            save_client_cfg(kClientCfgPath, shell);

            if (shell.join_mode) {
                // JOIN cannot connect yet, and this build refuses to pretend it can. Every
                // cross-actor call still goes through `LocalRouter` (see world.hpp): swapping it
                // for the distributed router and handing each node a subset of the chunk keys is
                // the P5 step, and the node #2..N model in ARCHITECTURE §2 rides on exactly that.
                // Until then, joining a remote leader is impossible, so we say so on the same
                // message channel a failed login already uses rather than silently hosting.
                shell.login_message =
                    "Joining a remote world is the P5 step - this build hosts. Pick 'Host'.";
            } else {
                LoginOutcome out{};
                const int s = world.login(shell.name, shell.pass, out);
                if (s >= 0) {
                    slot = s;
                    me = world.key_of(slot);
                    shell.screen = ui::Screen::kPlaying;
                    shell.login_message = nullptr;
                    world.save_accounts(kAccountsPath);
                    std::printf("signed in as '%s' (%s) -> slot %d\n", shell.name, describe(out),
                                slot);
                } else {
                    shell.login_message = describe(out);
                }
            }
            // Wipe the password out of process memory the moment the attempt is over. It is a small
            // thing and it is free, and the alternative is a plaintext password sitting in a UI
            // struct for the rest of the session. Done for the join path too: the box was filled.
            std::memset(shell.pass, 0, sizeof shell.pass);
        }
        if (act == ui::Action::kQuit) break;

        // The flash shot fires the moment AFTER the hit (frame 3 == one draw past the strike landed on
        // frame 2), catching the flash at its freshest; other shots take the usual three-frame settle.
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
