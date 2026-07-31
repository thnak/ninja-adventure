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
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "world/abilities.hpp"
#include "world/battlefield.hpp"
#include "world/combat_entity.hpp"
#include "world/map_system.hpp"
#include "world/telegraph.hpp"
#include "world/tiles.hpp"

namespace mmo {

struct PlayerView {
    std::uint64_t id = 0;
    std::uint32_t account = 0;  // 0 = this session slot is empty
    std::uint16_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    std::int16_t max_hp = 0;
    std::int16_t mana = 0;
    std::int16_t stamina = 0;
    Facing facing = Facing::kDown;
    std::uint32_t steps = 0;  // monotonic move count — the renderer's animation clock
    std::uint16_t dead_ticks = 0;  // >0 while waiting to respawn
    std::uint32_t deaths = 0;
    bool mounted = false;
    std::uint16_t respawn_tx = 0;
    std::uint16_t respawn_ty = 0;
    std::int32_t items[kItemKinds] = {};
    std::uint8_t skill_level[kSkillCount] = {};
    std::uint32_t skill_xp[kSkillCount] = {};
    std::uint32_t skill_next[kSkillCount] = {};

    // The two equipped ability slots, as the HUD needs to draw them without an `ask`. `ability` is
    // which ability the fixed loadout resolved each slot to (AbilityId::kCount if the school is not
    // yet high enough — the HUD greys it), and `ability_cd` is the ticks of cooldown left. Everything
    // else the HUD wants (locked, affordable, cooldown fraction) it derives from `ability_def` and
    // the vitals already in this view.
    AbilityId ability[kAbilitySlots] = {AbilityId::kCount, AbilityId::kCount};
    std::uint16_t ability_cd[kAbilitySlots] = {};

    // World tick of this player's last SUCCESSFUL swing, published by PlayerActor when a PlanAttack
    // is granted. The renderer alone reads it: `world_tick - last_swing_tick` under a small window
    // is how it draws the deluxe attack frames + katana swoosh for ANY player (local or remote),
    // keyed off published state rather than off this client's own input. 0 means "never swung".
    std::uint64_t last_swing_tick = 0;

    [[nodiscard]] bool live() const noexcept { return account != 0; }
};

using PlayerViewPtr = std::shared_ptr<const PlayerView>;

// The players' equivalent of `SnapshotBus`, one slot per session.
//
// WHO READS IT AND WHY THAT IS SOUND. Two readers: the renderer (which must not `ask` in a frame,
// for the reasons at the top of this file), and MapDirector — which needs every player's position
// each tick in order to fan `PlayerBeacon`s to the chunks near them. The director could not obtain
// that by asking without blocking a tier-A actor on N replies per tick.
//
// This does NOT weaken the trust tiering, and the reason is specific: `MapDirector` and every
// `PlayerActor` both carry `Require<Trusted>`, so placement guarantees they are co-located on the
// leader. A shared in-process publication between two actors that are pinned to the same node is a
// local optimisation, not a channel. What crosses to an untrusted chunk host is always a real
// message — `PlayerBeacon` — and a chunk can no more read this array than it can read an
// inventory.
class PlayerBus {
public:
    PlayerBus() : slots_(kMaxPlayers) {}
    PlayerBus(const PlayerBus&) = delete;
    PlayerBus& operator=(const PlayerBus&) = delete;

    void publish(int slot, PlayerViewPtr v) noexcept {
        slots_[static_cast<std::size_t>(slot)].store(std::move(v), std::memory_order_release);
    }

    [[nodiscard]] PlayerViewPtr load(int slot) const noexcept {
        return slots_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
    }

private:
    std::vector<std::atomic<PlayerViewPtr>> slots_;
};

// One chunk's renderable state at one tick. Terrain is included because a chunk may be re-placed
// onto another node and streamed to a client that has never seen it — the view is self-contained.
struct ChunkView {
    ChunkCoord coord{};
    std::uint64_t tick = 0;
    std::int64_t world_ms = 0;
    std::uint8_t terrain[kChunkTiles * kChunkTiles] = {};
    std::vector<Creature> creatures;
    std::vector<Projectile> shots;
    std::vector<Effect> effects;
    std::vector<CombatEntity> entities;
    std::vector<Scar> scars;
    std::vector<Crop> crops;
    std::vector<Building> buildings;
    std::vector<TilePatch> patches;  // RFC-010 §4.2 — capped at kMaxPatches (48) by the owning chunk
    std::vector<FieldState> fields;  // RFC-010 §4.3 — capped at kMaxFields (2) by the owning chunk
    std::vector<Telegraph> telegraphs;  // RFC-006 §2 — capped at kMaxTelegraphs (8) by the owning chunk
};

using ChunkViewPtr = std::shared_ptr<const ChunkView>;

// RFC-014 §4: one block per currently-open instanced-band session, sized to THAT map's own
// chunk_edge (RFC-022 §1.2) — not to the global kMapChunks (32), which would waste 32x the index
// space for every instance smaller than the overworld (the second bug RFC-014's Motivation §3
// names). `slots` is `chunk_edge²` atomics, the same atomic-publish shape the persistent band uses.
struct InstanceChunkBlock {
    std::uint8_t chunk_edge = 0;
    std::vector<std::atomic<ChunkViewPtr>> slots;

    explicit InstanceChunkBlock(std::uint8_t edge)
        : chunk_edge(edge), slots(static_cast<std::size_t>(edge) * edge) {}
};

// A flat slot array for the PERSISTENT band, indexed by `chunk_index` (tiles.hpp — unchanged
// formula), now sized to RFC-022 §1.1's fixed partition width (`kPersistentBandEnd * kChunksPerMap`)
// rather than the live `kMapCount` — a static, trivially-small over-allocation (16,384 atomics vs.
// today's 2,048) that means a future persistent MapId (2..15) needs no further redesign here — plus
// a sparse per-instance map for the INSTANCED band (RFC-014 §4). `instance_blocks_` is genuinely
// concurrently accessed: many `ChunkActor`s publish from worker threads while `InstanceManager`
// registers/releases blocks from `World`'s own thread — `std::unordered_map` is not safe under that
// without a guard, so a `shared_mutex` protects the MAP STRUCTURE (find/insert/erase) while the
// atomics inside an already-found block need no lock at all, exactly like the persistent slots. The
// RFC's own §4.2 pseudocode does not show this lock (it sketches the branch, not the concurrency
// control) — added here because this engine's chunk actors are genuinely multi-threaded, not
// documented by the RFC's own pseudocode, so noted as this file's own addition.
class SnapshotBus {
public:
    SnapshotBus() : slots_(static_cast<std::size_t>(kPersistentBandEnd) * kChunksPerMap) {}

    SnapshotBus(const SnapshotBus&) = delete;
    SnapshotBus& operator=(const SnapshotBus&) = delete;

    // Called by the owning ChunkActor at the end of its tick. Release-ordered so a reader that
    // observes the pointer also observes every field the actor wrote into the view.
    void publish(ChunkCoord c, ChunkViewPtr v) noexcept {
        if (c.map < kPersistentBandEnd) {
            slots_[static_cast<std::size_t>(chunk_index(c))].store(std::move(v),
                                                                   std::memory_order_release);
            return;
        }
        std::shared_lock lock(instance_mutex_);
        auto it = instance_blocks_.find(c.map);
        if (it == instance_blocks_.end()) return;  // stale publish after CLOSED — dropped, not UB
        const int li = instance_local_index(c, it->second.chunk_edge);
        if (li < 0 || static_cast<std::size_t>(li) >= it->second.slots.size()) return;
        it->second.slots[static_cast<std::size_t>(li)].store(std::move(v), std::memory_order_release);
    }

    // Called by the renderer. May return null before a chunk has ticked once.
    [[nodiscard]] ChunkViewPtr load(ChunkCoord c) const noexcept {
        if (c.map < kPersistentBandEnd) {
            return slots_[static_cast<std::size_t>(chunk_index(c))].load(std::memory_order_acquire);
        }
        std::shared_lock lock(instance_mutex_);
        auto it = instance_blocks_.find(c.map);
        if (it == instance_blocks_.end()) return nullptr;
        const int li = instance_local_index(c, it->second.chunk_edge);
        if (li < 0 || static_cast<std::size_t>(li) >= it->second.slots.size()) return nullptr;
        return it->second.slots[static_cast<std::size_t>(li)].load(std::memory_order_acquire);
    }

    // Persistent-band only — every existing caller (`count_creatures`, `probe_main.cpp`) iterates
    // exactly `kChunkCount` (the live `kMapCount`'s worth) of these, which is still correct: the
    // array grew, but maps 0/1 still occupy indices [0, kChunkCount) exactly as before.
    [[nodiscard]] ChunkViewPtr load_index(int i) const noexcept {
        return slots_[static_cast<std::size_t>(i)].load(std::memory_order_acquire);
    }

    // RFC-014 §3.6 ALLOCATING / §3.5 CLOSED — called by InstanceManager, never by a ChunkActor.
    // Registering twice for the same still-open MapId is a caller bug (asserted away by construction
    // in InstanceManager: a MapId is only ever registered once, at allocation, per §3.1's "never
    // reused" guarantee), so this simply overwrites — cheap insurance, not a silently-tolerated race.
    void register_instance_block(MapId id, std::uint8_t chunk_edge) {
        std::unique_lock lock(instance_mutex_);
        instance_blocks_.erase(id);
        instance_blocks_.emplace(id, InstanceChunkBlock{chunk_edge});
    }

    // RFC-014 §3.6 CLOSED. After this, any straggling publish for `id` is dropped (see `publish()`).
    void release_instance_block(MapId id) {
        std::unique_lock lock(instance_mutex_);
        instance_blocks_.erase(id);
    }

private:
    // `deque`-free: a vector of atomics sized once, never reallocated.
    std::vector<std::atomic<ChunkViewPtr>> slots_;

    mutable std::shared_mutex instance_mutex_;
    std::unordered_map<MapId, InstanceChunkBlock> instance_blocks_;
};

// The world-level state a client needs that is not per-chunk. Written by MapDirector, read by the
// renderer and by the HUD.
struct WorldStatus {
    std::atomic<std::int64_t> world_ms{0};
    std::atomic<std::uint64_t> tick{0};
    std::atomic<bool> night{false};
    std::atomic<std::uint32_t> wave{0};
    std::atomic<std::uint32_t> creatures_alive{0};
    std::atomic<std::uint32_t> creatures_killed{0};
    std::atomic<std::uint32_t> player_kills{0};  // of those, how many the players did themselves
    std::atomic<std::uint32_t> player_deaths{0};
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
    virtual void draw(const SnapshotBus&, const WorldStatus&, const PlayerBus&, int local_slot) = 0;
    virtual void end_frame() = 0;
};

}  // namespace mmo
