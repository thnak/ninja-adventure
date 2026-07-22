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
    const PlayerBus* players = nullptr;  // read-only: where everyone is, published by the actors
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

        // The heartbeat. Every chunk and every player actor, every tick — including chunks with
        // nothing in them, which is cheap precisely because an empty `Sequential` handler that
        // touches a few empty vectors is a few hundred nanoseconds and never contends with anything.
        const Tick t{tick_, world_ms_, night};
        for (const ChunkCoord& c : chunks) {
            router->get<ChunkActor>(chunk_key(c)).tell(t);
        }
        for (int slot = 0; slot < kMaxPlayers; ++slot) {
            router->get<PlayerActor>(player_key(slot)).tell(t);
        }

        fan_beacons();
    }

    void handle(const Ask<GetWorldTick, std::uint64_t>& m) noexcept { m.respond(tick_); }

    [[nodiscard]] std::int64_t world_ms() const noexcept { return world_ms_; }

private:
    // Tell the chunks around each player where that player is.
    //
    // WHY THE DIRECTOR AND NOT THE PLAYER ACTOR. A `PlayerActor` cannot address a `ChunkActor`
    // without including chunk_actor.hpp, which includes player_actor.hpp — a genuine cycle, not a
    // stylistic one. The director already depends on both and already fans one message to every
    // chunk every tick, so the beacon rides the same fan-out it was always going to need. It reads
    // positions from the published `PlayerBus` rather than asking, for the reason set out in
    // snapshot.hpp: a tier-A actor must not block on N replies per tick.
    //
    // 5x5 CHUNKS, not 3x3. The radius has to cover what the client can SEE, not merely what a
    // creature can reach, because the same roster is what tells a chunk it is being watched and may
    // publish (chunk_actor.hpp, kIdlePublish). 5x5 is 160x160 tiles — comfortably more than a
    // screen at minimum zoom, and 25 tells every third tick per player.
    void fan_beacons() noexcept {
        if (players == nullptr || router == nullptr) return;
        if (tick_ % kBeaconPeriod != 0) return;
        constexpr int kSpan = 2;  // chunks either side
        for (int slot = 0; slot < kMaxPlayers; ++slot) {
            PlayerViewPtr v = players->load(slot);
            if (!v || !v->live()) continue;
            PlayerBeacon b{};
            b.player = v->id;
            b.map = v->map;
            b.x = v->x;
            b.y = v->y;
            b.hp = v->hp;
            b.tick = tick_;
            const ChunkCoord home = chunk_of(v->map, v->x, v->y);
            for (int dy = -kSpan; dy <= kSpan; ++dy) {
                for (int dx = -kSpan; dx <= kSpan; ++dx) {
                    const int cx = static_cast<int>(home.cx) + dx;
                    const int cy = static_cast<int>(home.cy) + dy;
                    if (cx < 0 || cy < 0 || cx >= kMapChunks || cy >= kMapChunks) continue;
                    const ChunkCoord c{v->map, static_cast<std::uint16_t>(cx),
                                       static_cast<std::uint16_t>(cy)};
                    router->get<ChunkActor>(chunk_key(c)).tell(b);
                }
            }
        }
    }

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
            const ChunkCoord c = chunk_of(home, static_cast<float>(tx), static_cast<float>(ty));
            router->get<ChunkActor>(chunk_key(c)).tell(raid_at(tx, ty, per_source, rng));
            ++sent;
        }
        // A night on which nothing at all stirs is fine and intended, but a run that never spawns
        // anything is indistinguishable from a broken director. So the FIRST night always raids,
        // from one source, and after that the dice decide.
        if (sent == 0 && wave_ == 1) {
            const auto& [tx, ty] = raid_sources[rng.below(static_cast<std::uint32_t>(
                raid_sources.size()))];
            router->get<ChunkActor>(chunk_key(chunk_of(home, static_cast<float>(tx),
                                                       static_cast<float>(ty))))
                .tell(raid_at(tx, ty, per_source, rng));
        }
    }

    // WHAT comes out of a stronghold depends on where the stronghold is. The outer rings do not
    // merely field tougher slimes — they field different creatures, so a ring reads as a different
    // place rather than the same place with bigger numbers on it. (The numbers get bigger too: the
    // chunk scales HP and damage by ring when it creates each one.)
    [[nodiscard]] SpawnWave raid_at(int tx, int ty, std::uint16_t count, Rng& rng) const noexcept {
        SpawnWave w{};
        w.count = count;
        w.seed = static_cast<std::uint32_t>(rng.next());
        w.kind = static_cast<std::uint8_t>(
            raid_kind_of(ring_of(world_seed, tx, ty), static_cast<std::uint32_t>(rng.next() >> 8)));
        w.tx = static_cast<std::uint16_t>(tx);
        w.ty = static_cast<std::uint16_t>(ty);
        w.radius = 3;
        return w;
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
