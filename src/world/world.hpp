// World bring-up — everything that must happen before the first tick, in one place.
//
// This file is the entire difference between the headless runner and the graphical client. Both
// construct a `World`, both drive it with `step()`; only one of them draws. Keeping bring-up here
// (rather than in a main) is what makes "run the simulation with no display" a first-class mode
// instead of a debugging hack — which matters, because the cluster demo is headless by nature.
//
// SINGLE PROCESS TODAY, N PROCESSES LATER. Every actor here is registered through the same
// `register_actor` path a distributed node would use, and every cross-actor call already goes
// through `LocalRouter`. Swapping `LocalRouter` for the distributed router and handing each node a
// subset of the chunk keys is the port to a real cluster; the actors themselves are already written
// as if their peers were remote, because from inside a handler there is no way to tell.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

#include "world/chunk_actor.hpp"
#include "world/flow_field.hpp"
#include "world/map_director.hpp"
#include "world/player_actor.hpp"
#include "world/protocol.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"
#include "world/worldgen.hpp"

namespace mmo {

inline constexpr std::uint64_t kPlayerKey = 1;
inline constexpr std::uint64_t kDirectorKey = 1;

[[nodiscard]] inline std::uint32_t count_mobs(const SnapshotBus& bus) noexcept {
    std::uint32_t n = 0;
    for (int i = 0; i < kChunkCount; ++i) {
        if (ChunkViewPtr v = bus.load_index(i)) n += static_cast<std::uint32_t>(v->mobs.size());
    }
    return n;
}

class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // `workers` is explicit and small on purpose (CONVENTIONS.md machine safety) — never
    // hardware_concurrency.
    void build(std::uint32_t workers = 4) {
        // FIRST, before anything reads a tile. Generating the layout publishes the overlay that
        // `terrain_of` consults, so every terrain query after this line — the chunks' caches, the
        // flow field's walkability test, the renderer — sees villages and roads rather than bare
        // noise. Doing it later would give the chunks a cache of the land as it was before anyone
        // built on it, and nothing would ever correct them.
        layout_ = &world_layout(kWorldSeed);

        pool_ = std::make_unique<quark::detail::MessagePool>(1u << 16);

        quark::EngineConfig cfg{};
        cfg.worker_count = workers;
        cfg.shard_count = workers;
        cfg.drain_budget = 256;
        cfg.band_count = 2;  // Priority<0> (director/player) and Priority<1> (chunks)
        cfg.max_types = 16;
        cfg.pool_capacity = 1u << 14;
        engine_ = std::make_unique<quark::Engine<quark::PriorityBands<2>>>(cfg);

        router_ = std::make_unique<quark::LocalRouter>(engine_->post_courier(), *pool_);

        // One multi-source BFS, before any actor exists: distance to the nearest VILLAGE from every
        // tile. Read-only from here on — see flow_field.hpp for why handing every chunk a pointer
        // to it does not reintroduce shared mutable state.
        std::vector<std::pair<int, int>> targets;
        targets.reserve(layout_->villages().size());
        for (const Village& v : layout_->villages()) targets.emplace_back(v.tx, v.ty);
        flow_[0].build(kWorldSeed, kOverworld, targets);

        build_player();
        build_chunks();
        build_director();
    }

    void start() { engine_->start(); }

    void stop() { engine_->stop(); }

    // One simulation step. The caller owns the pacing — a fixed-step loop in the headless runner, a
    // frame-rate-independent accumulator in the client.
    void step(std::int64_t dt_ms) {
        director_ref_.tell(DirectorTick{dt_ms});
    }

    // A FIFO barrier on the director: the reply proves it has drained every DirectorTick posted
    // before it, and therefore has already fanned every `Tick` it was going to fan. Used instead of
    // sleeping so the headless runner is deterministic rather than timing-dependent.
    std::uint64_t sync_director() {
        quark::result<std::uint64_t> r =
            quark::block_on(director_ref_.ask<std::uint64_t>(GetWorldTick{}));
        return r.has_value() ? r.value() : 0;
    }

    // A barrier on the WHOLE world: the director first (so every Tick has been posted), then every
    // chunk (so every Tick has been drained). After this returns, every published snapshot reflects
    // the same tick — which is the only way a sampled number is worth printing.
    //
    // Note what this is NOT: the simulation does not need it. Chunks are free to run behind the
    // director and behind each other, and normally do — that lag IS the pipelining. It exists so a
    // *reader* can take a consistent sample.
    std::uint64_t sync_world() {
        const std::uint64_t t = sync_director();
        for (const ChunkCoord& c : chunk_coords_) {
            auto ref = router_->get<ChunkActor>(chunk_key(c));
            (void)quark::block_on(ref.ask<ChunkStats>(GetChunkStats{}));
        }
        return t;
    }

    [[nodiscard]] ChunkStats chunk_stats(ChunkCoord c) {
        auto ref = router_->get<ChunkActor>(chunk_key(c));
        quark::result<ChunkStats> r = quark::block_on(ref.ask<ChunkStats>(GetChunkStats{}));
        return r.has_value() ? r.value() : ChunkStats{};
    }

    [[nodiscard]] PlayerView player_view() {
        quark::result<PlayerView> r = quark::block_on(player_ref_.ask<PlayerView>(GetPlayer{}));
        return r.has_value() ? r.value() : PlayerView{};
    }

    // --- player-driven actions ---------------------------------------------------------------
    void move_player(float dx, float dy) { player_ref_.tell(MoveIntent{dx, dy}); }

    void plant(std::uint16_t map, std::uint16_t tx, std::uint16_t ty, CropKind k,
               std::int64_t now_ms) {
        if (!in_map(tx, ty)) return;
        chunk_ref(map, tx, ty).tell(PlantCrop{tx, ty, k, now_ms, kPlayerKey});
    }

    void harvest(std::uint16_t map, std::uint16_t tx, std::uint16_t ty) {
        if (!in_map(tx, ty)) return;
        chunk_ref(map, tx, ty).tell(HarvestAt{tx, ty, kPlayerKey});
    }

    // Base expansion: reclaim a tile as farmland. Costs a little wood so it is a real choice
    // against building with it.
    bool till(std::uint16_t map, std::uint16_t tx, std::uint16_t ty) {
        if (!in_map(tx, ty)) return false;
        quark::result<bool> paid =
            quark::block_on(player_ref_.ask<bool>(SpendItems{ItemKind::kWood, kTillCost}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(TillGround{tx, ty, kPlayerKey});
        return true;
    }

    // Upgrade whatever building is on this tile. Same ask-then-tell ordering as build_at: the
    // trusted inventory decides affordability before the (possibly untrusted) chunk is told.
    bool upgrade(std::uint16_t map, std::uint16_t tx, std::uint16_t ty, BuildKind k,
                 std::uint8_t current_level) {
        if (!in_map(tx, ty) || current_level >= kMaxLevel) return false;
        const BuildCost c = upgrade_cost_of(k, current_level);
        quark::result<bool> paid = quark::block_on(player_ref_.ask<bool>(SpendItems{c.kind, c.count}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(UpgradeBuilding{tx, ty, kPlayerKey});
        return true;
    }

    // The generated world, for anything that needs to know where things ARE rather than what a
    // chunk currently holds: the renderer (which buildings to draw), the map exporter, the tests.
    [[nodiscard]] const WorldLayout& layout() const noexcept { return *layout_; }

    // Placement costs resources, so it is a two-step: ASK the trusted inventory to debit, and only
    // tell the (possibly untrusted) chunk to build if the debit succeeded. Doing it in this order
    // is what makes a compromised chunk host unable to mint free buildings.
    bool build_at(std::uint16_t map, std::uint16_t tx, std::uint16_t ty, BuildKind k) {
        if (!in_map(tx, ty)) return false;
        const BuildCost c = cost_of(k);
        quark::result<bool> paid = quark::block_on(player_ref_.ask<bool>(SpendItems{c.kind, c.count}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(PlaceBuilding{tx, ty, k, kPlayerKey});
        return true;
    }

    [[nodiscard]] SnapshotBus& bus() noexcept { return bus_; }
    [[nodiscard]] const SnapshotBus& bus() const noexcept { return bus_; }
    [[nodiscard]] WorldStatus& status() noexcept { return status_; }
    [[nodiscard]] const WorldStatus& status() const noexcept { return status_; }
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

private:
    [[nodiscard]] quark::ActorRef<ChunkActor> chunk_ref(std::uint16_t map, std::uint16_t tx,
                                                        std::uint16_t ty) {
        return router_->get<ChunkActor>(
            chunk_key(chunk_of(map, static_cast<float>(tx), static_cast<float>(ty))));
    }

    void build_player() {
        player_ = std::make_unique<PlayerActor>();
        player_->id = kPlayerKey;
        player_->map = kOverworld;
        // Open country, a good half-minute's walk from the nearest village. Not a farm, not a
        // tutorial, not a hearth already lit — GAME.md §6b.
        player_->set_position(static_cast<float>(layout_->spawn_tx()) + 0.5f,
                              static_cast<float>(layout_->spawn_ty()) + 0.5f);
        // Enough to light a fire and turn a few tiles of soil once you get somewhere. Deliberately
        // not enough to live on: the point of the walk is that you arrive needing people.
        player_->set_start_items(/*wood*/ 40, /*stone*/ 25, /*seed*/ 12);
        player_act_ = std::make_unique<quark::Activation>(player_.get(),
                                                          PlayerActor::dispatch_table(),
                                                          pool_->sink());
        quark::register_actor<PlayerActor>(*engine_, kPlayerKey, *player_act_);
        player_ref_ = router_->get<PlayerActor>(kPlayerKey);
    }

    void build_chunks() {
        chunks_.reserve(kChunkCount);
        chunk_acts_.reserve(kChunkCount);

        for (int map = 0; map < kMapCount; ++map) {
            for (int cy = 0; cy < kMapChunks; ++cy) {
                for (int cx = 0; cx < kMapChunks; ++cx) {
                    const ChunkCoord coord{static_cast<std::uint16_t>(map),
                                           static_cast<std::uint16_t>(cx),
                                           static_cast<std::uint16_t>(cy)};
                    auto ch = std::make_unique<ChunkActor>();
                    ch->coord = coord;
                    ch->router = router_.get();
                    ch->bus = &bus_;
                    ch->status = &status_;
                    ch->player = player_ref_;
                    ch->flow = &flow_[static_cast<std::size_t>(map)];
                    // Fallback heading for a mob the flow field cannot route (an island, a pocket
                    // walled in by cliffs): the village nearest this chunk's own centre.
                    const int mid_x = cx * kChunkTiles + kChunkTiles / 2;
                    const int mid_y = cy * kChunkTiles + kChunkTiles / 2;
                    if (const Village* v = layout_->nearest_village(mid_x, mid_y)) {
                        ch->home_x = static_cast<float>(v->tx) + 0.5f;
                        ch->home_y = static_cast<float>(v->ty) + 0.5f;
                    }
                    ch->generate_terrain(kWorldSeed);
                    // Villages and roads are already in the terrain the line above cached — they
                    // are part of the world, not entities placed on top of it. Nothing is seeded
                    // here: a new world starts with no player buildings anywhere in it.
                    ch->publish_now();

                    auto act = std::make_unique<quark::Activation>(
                        ch.get(), ChunkActor::dispatch_table(), pool_->sink());
                    quark::register_actor<ChunkActor>(*engine_, chunk_key(coord), *act);

                    chunk_coords_.push_back(coord);
                    chunks_.push_back(std::move(ch));
                    chunk_acts_.push_back(std::move(act));
                }
            }
        }
    }

    void build_director() {
        director_ = std::make_unique<MapDirector>();
        director_->router = router_.get();
        director_->status = &status_;
        director_->world_seed = kWorldSeed;
        director_->chunks = chunk_coords_;
        for (const Stronghold& h : layout_->strongholds()) {
            director_->raid_sources.emplace_back(h.tx, h.ty);
        }
        director_act_ = std::make_unique<quark::Activation>(
            director_.get(), MapDirector::dispatch_table(), pool_->sink());
        quark::register_actor<MapDirector>(*engine_, kDirectorKey, *director_act_);
        director_ref_ = router_->get<MapDirector>(kDirectorKey);
    }

    std::unique_ptr<quark::detail::MessagePool> pool_;
    std::unique_ptr<quark::Engine<quark::PriorityBands<2>>> engine_;
    std::unique_ptr<quark::LocalRouter> router_;

    const WorldLayout* layout_ = nullptr;
    SnapshotBus bus_;
    WorldStatus status_;
    std::array<FlowField, kMapCount> flow_{};

    std::unique_ptr<PlayerActor> player_;
    std::unique_ptr<quark::Activation> player_act_;
    quark::ActorRef<PlayerActor> player_ref_{};

    std::vector<std::unique_ptr<ChunkActor>> chunks_;
    std::vector<std::unique_ptr<quark::Activation>> chunk_acts_;
    std::vector<ChunkCoord> chunk_coords_;

    std::unique_ptr<MapDirector> director_;
    std::unique_ptr<quark::Activation> director_act_;
    quark::ActorRef<MapDirector> director_ref_{};
};

}  // namespace mmo
