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

#include <algorithm>
#include <cstdint>
#include <utility>
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
    // Where raids come from. Filled from the world layout at bring-up; the director never reads the
    // layout itself, so it stays a plain list of tiles that could equally have arrived over a wire.
    std::vector<std::pair<int, int>> raid_sources;

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

        // Nightfall: roll for a raid at every stronghold. Most of them stay asleep.
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
    // Raids come out of STRONGHOLDS, and only some of them, chosen at random each night.
    //
    // The old version emptied all five fixed camps every single night, which is a schedule, and a
    // schedule is the opposite of what this game is for: if you know a wave lands at nightfall you
    // are never relaxing, you are waiting. The design decision (GAME.md §9) is that a settlement
    // has a chance of being raided, not an appointment — so most nights nothing happens where you
    // are, and the one that does is a surprise rather than a countdown.
    //
    // The odds rise with the ring, because the strongholds themselves are denser out there: the
    // meadow gets the occasional nuisance, the wasteland is genuinely hostile, and no table
    // anywhere had to say so.
    void spawn_wave() noexcept {
        if (raid_sources.empty()) return;
        Rng rng(world_seed ^ 0xDEAD'BEEFull ^
                (static_cast<std::uint64_t>(wave_) * 0x9E37'79B9'7F4A'7C15ull));

        // Mobs per raiding stronghold. Grows slowly with the night count and is capped, because at
        // 1024 chunks the interesting number is how many places are raided, not how deep one pile is.
        const std::uint16_t per_source =
            static_cast<std::uint16_t>(std::min<std::uint32_t>(8u + 3u * (wave_ - 1u), 40u));

        const auto home = kOverworld;
        std::uint32_t sent = 0;
        for (const auto& [tx, ty] : raid_sources) {
            if (rng.unit() > kRaidChance) continue;
            SpawnWave w{};
            w.count = per_source;
            w.seed = static_cast<std::uint32_t>(rng.next());
            w.kind = static_cast<std::uint8_t>(rng.below(3));
            w.tx = static_cast<std::uint16_t>(tx);
            w.ty = static_cast<std::uint16_t>(ty);
            w.radius = 3;
            const ChunkCoord c = chunk_of(home, static_cast<float>(tx), static_cast<float>(ty));
            router->get<ChunkActor>(chunk_key(c)).tell(w);
            ++sent;
        }
        // A night on which nothing at all stirs is fine and intended, but a run that never spawns
        // anything is indistinguishable from a broken director. So the FIRST night always raids,
        // from one source, and after that the dice decide.
        if (sent == 0 && wave_ == 1) {
            const auto& [tx, ty] = raid_sources[rng.below(static_cast<std::uint32_t>(
                raid_sources.size()))];
            SpawnWave w{};
            w.count = per_source;
            w.seed = static_cast<std::uint32_t>(rng.next());
            w.kind = static_cast<std::uint8_t>(rng.below(3));
            w.tx = static_cast<std::uint16_t>(tx);
            w.ty = static_cast<std::uint16_t>(ty);
            w.radius = 3;
            router->get<ChunkActor>(chunk_key(chunk_of(home, static_cast<float>(tx),
                                                       static_cast<float>(ty))))
                .tell(w);
        }
    }

    // Per stronghold, per night. Tuned so that with ~25 strongholds a typical night wakes two or
    // three of them somewhere on a 1024x1024 map — which, from where the player is standing, means
    // most nights are quiet.
    static constexpr float kRaidChance = 0.10f;

    std::int64_t world_ms_ = 0;
    std::uint64_t tick_ = 0;
    std::uint32_t wave_ = 0;
    bool was_night_ = false;
};

}  // namespace mmo
