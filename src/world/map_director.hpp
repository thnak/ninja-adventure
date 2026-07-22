// MapDirector — the world clock and the wave scheduler. One director per world.
//
// It owns two things no chunk can own alone: the day/night phase, and the decision of *where* a
// night wave enters the map. Everything else it does is fan-out — one `Tick` per chunk, per tick.
//
// TRUST TIER A (`Require<Trusted>`): the director decides how hard the world hits the player, so a
// player's machine must not host it. Note the asymmetry that makes the whole tiering work — the
// director tells 192 chunks what time it is, but never reads their state back. Authority flows
// downhill from trusted to untrusted; the only path back up is a chunk asking PlayerActor to change
// an inventory, and that actor is free to disbelieve it.
//
// WHY THE DIRECTOR FANS THE TICK rather than each chunk running its own timer: one timer means one
// world clock. If every chunk scheduled itself, chunks on different machines would drift apart and
// a mob crossing a boundary would step twice or not at all. Fan-out from a single scheduled actor
// makes the tick number a globally agreed value that rides along with every message.
#pragma once

#include <cstdint>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/placement_policies.hpp"

#include "world/chunk_actor.hpp"
#include "world/player_actor.hpp"
#include "world/protocol.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"

namespace mmo {

struct MapDirector : quark::Actor<MapDirector, quark::Sequential, quark::Priority<0>,
                                  quark::Placement<quark::HashById, Require<Trusted>>> {
    using protocol = Protocol<DirectorTick, Ask<GetWorldTick, std::uint64_t>>;

    // Wired at bring-up.
    quark::LocalRouter* router = nullptr;
    WorldStatus* status = nullptr;
    std::uint64_t world_seed = 0;
    std::vector<ChunkCoord> chunks;  // every chunk in the world, in index order

    void handle(const DirectorTick& d) noexcept {
        world_ms_ += d.dt_ms;
        ++tick_;

        const bool night = is_night(world_ms_);
        const bool dawn_to_dusk = night && !was_night_;
        was_night_ = night;

        if (status != nullptr) {
            status->world_ms.store(world_ms_, std::memory_order_relaxed);
            status->tick.store(tick_, std::memory_order_relaxed);
            status->night.store(night, std::memory_order_relaxed);
        }

        // Nightfall: pick the rim chunks of the home map and seed a wave into each. Wave N is
        // bigger than wave N-1 — the reason to keep building during the day.
        if (dawn_to_dusk) {
            ++wave_;
            if (status != nullptr) status->wave.store(wave_, std::memory_order_relaxed);
            spawn_wave();
        }

        // The heartbeat. Every chunk, every tick — including chunks with nothing in them, which is
        // cheap precisely because an empty `Sequential` handler that touches three empty vectors is
        // a few hundred nanoseconds and never contends with anything.
        const Tick t{tick_, world_ms_, night};
        for (const ChunkCoord& c : chunks) {
            router->get<ChunkActor>(chunk_key(c)).tell(t);
        }
    }

    void handle(const Ask<GetWorldTick, std::uint64_t>& m) noexcept { m.respond(tick_); }

    [[nodiscard]] std::int64_t world_ms() const noexcept { return world_ms_; }

private:
    // Waves come out of the map's fixed spawn CAMPS, not out of every rim chunk. See the note on
    // `camp_tile` in tiles.hpp: uniform rim spawning meant monsters arrived from all 360 degrees,
    // which made any perimeter shorter than the whole map border pointless. Five camps turn the map
    // into five approach lanes that a player can actually fortify.
    void spawn_wave() noexcept {
        Rng rng(0xDEAD'BEEFull ^ (static_cast<std::uint64_t>(wave_) * 0x9E37'79B9'7F4A'7C15ull));
        // Difficulty curve: 14 mobs per camp on wave 1, +8 per wave, capped so a long session does
        // not turn into an allocation benchmark.
        const std::uint16_t per_camp =
            static_cast<std::uint16_t>(std::min<std::uint32_t>(14u + 8u * (wave_ - 1u), 120u));

        const auto home = static_cast<std::uint16_t>(MapId::kHomeValley);
        for (int i = 0; i < kSpawnCamps; ++i) {
            int tx = 0;
            int ty = 0;
            camp_tile(world_seed, home, i, tx, ty);
            SpawnWave w{};
            w.count = per_camp;
            w.seed = static_cast<std::uint32_t>(rng.next());
            w.kind = static_cast<std::uint8_t>(rng.below(3));
            w.tx = static_cast<std::uint16_t>(tx);
            w.ty = static_cast<std::uint16_t>(ty);
            w.radius = kCampRadius;
            const ChunkCoord c = chunk_of(home, static_cast<float>(tx), static_cast<float>(ty));
            router->get<ChunkActor>(chunk_key(c)).tell(w);
        }
    }

    std::int64_t world_ms_ = 0;
    std::uint64_t tick_ = 0;
    std::uint32_t wave_ = 0;
    bool was_night_ = false;
};

}  // namespace mmo
