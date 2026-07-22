// The render seam — the ONLY thing the renderer is allowed to see.
//
// The simulation never calls into a renderer, and the renderer never calls into an actor. Instead
// each ChunkActor publishes an immutable `ChunkView` at the end of its tick, and the renderer reads
// whatever the latest published view happens to be. This is deliberately a *lossy* channel: if the
// renderer misses a tick it just draws the newer one, and the simulation never stalls waiting for a
// frame. No locks, no back-pressure from the display into the world.
//
// WHY NOT `ask`: an `ask` per visible chunk per frame would put `block_on` in the render loop —
// coupling frame rate to mailbox latency and, once chunks live on other machines, to the network.
// Publishing decouples them: the render loop's cost is independent of where a chunk is placed.
//
// THREADING: exactly one writer per slot (the owning ChunkActor, which is `Sequential`, so its
// handler never runs concurrently with itself) and any number of readers. `atomic<shared_ptr>`
// gives that safely and keeps a view alive for as long as a reader holds it.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "world/tiles.hpp"

namespace mmo {

struct PlayerView {
    std::uint64_t id = 0;
    std::uint16_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    Facing facing = Facing::kDown;
    std::uint32_t steps = 0;  // monotonic move count — the renderer's animation clock
    std::int32_t items[kItemKinds] = {};
};

// One chunk's renderable state at one tick. Terrain is included because a chunk may be re-placed
// onto another node and streamed to a client that has never seen it — the view is self-contained.
struct ChunkView {
    ChunkCoord coord{};
    std::uint64_t tick = 0;
    std::int64_t world_ms = 0;
    std::uint8_t terrain[kChunkTiles * kChunkTiles] = {};
    std::vector<Mob> mobs;
    std::vector<Crop> crops;
    std::vector<Building> buildings;
};

using ChunkViewPtr = std::shared_ptr<const ChunkView>;

// A flat slot array indexed by `chunk_index`. Sized for the whole world at construction: chunk
// count is a compile-time property of the world layout, so there is no growth path to race on.
class SnapshotBus {
public:
    SnapshotBus() : slots_(kChunkCount) {}

    SnapshotBus(const SnapshotBus&) = delete;
    SnapshotBus& operator=(const SnapshotBus&) = delete;

    // Called by the owning ChunkActor at the end of its tick. Release-ordered so a reader that
    // observes the pointer also observes every field the actor wrote into the view.
    void publish(ChunkCoord c, ChunkViewPtr v) noexcept {
        slots_[static_cast<std::size_t>(chunk_index(c))].store(std::move(v),
                                                               std::memory_order_release);
    }

    // Called by the renderer. May return null before a chunk has ticked once.
    [[nodiscard]] ChunkViewPtr load(ChunkCoord c) const noexcept {
        return slots_[static_cast<std::size_t>(chunk_index(c))].load(std::memory_order_acquire);
    }

    [[nodiscard]] ChunkViewPtr load_index(int i) const noexcept {
        return slots_[static_cast<std::size_t>(i)].load(std::memory_order_acquire);
    }

private:
    // `deque`-free: a vector of atomics sized once, never reallocated.
    std::vector<std::atomic<ChunkViewPtr>> slots_;
};

// The world-level state a client needs that is not per-chunk. Written by MapDirector, read by the
// renderer and by the HUD.
struct WorldStatus {
    std::atomic<std::int64_t> world_ms{0};
    std::atomic<std::uint64_t> tick{0};
    std::atomic<bool> night{false};
    std::atomic<std::uint32_t> wave{0};
    std::atomic<std::uint32_t> mobs_alive{0};
    std::atomic<std::uint32_t> mobs_killed{0};
    std::atomic<std::uint32_t> migrations{0};  // cross-chunk (and, once distributed, cross-node)
};

// NOTE: there is deliberately no "core HP" here any more. A single global health bar implies a
// single global loss condition, and this game has none — see GAME.md §0. Buildings carry their own
// HP in `Building::hp`; that is the only health the world tracks.

// The seam the render backend implements. `sim_main` links none of this; `client_main` links one
// implementation. Adding a Godot/GDExtension backend later means another implementation here and
// zero simulation changes.
class IRenderBridge {
public:
    virtual ~IRenderBridge() = default;

    // Returns false when the user has asked to close the window.
    [[nodiscard]] virtual bool begin_frame() = 0;
    virtual void draw(const SnapshotBus&, const WorldStatus&, const PlayerView&) = 0;
    virtual void end_frame() = 0;
};

}  // namespace mmo
