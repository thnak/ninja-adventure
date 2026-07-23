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
//
// EVERY PLAYER VERB TAKES A KEY. There is no "the player" here, even though a single-process run
// only ever has one. That is ROADMAP principle 2 held to: the shape that costs nothing today and
// weeks at P6.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

#include "world/account.hpp"
#include "world/chunk_actor.hpp"
#include "world/flow_field.hpp"
#include "world/map_director.hpp"
#include "world/player_actor.hpp"
#include "world/protocol.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"
#include "world/worldgen.hpp"

namespace mmo {

inline constexpr std::uint64_t kDirectorKey = 1;

// How far from the player a spell may be aimed. The client says where the cursor is; the trusted
// actor says where the player is; this clamps the difference. Without it the mouse would be a
// sniper rifle with no cooldown.
inline constexpr float kSpellRange = 8.0f;

[[nodiscard]] inline std::uint32_t count_creatures(const SnapshotBus& bus) noexcept {
    std::uint32_t n = 0;
    for (int i = 0; i < kChunkCount; ++i) {
        if (ChunkViewPtr v = bus.load_index(i)) n += static_cast<std::uint32_t>(v->creatures.size());
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
        cfg.band_count = 2;  // Priority<0> (director/players) and Priority<1> (chunks)
        cfg.max_types = 64;
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

        build_players();
        build_chunks();
        build_director();
    }

    void start() { engine_->start(); }

    void stop() { engine_->stop(); }

    // --- accounts ---------------------------------------------------------------------------------

    // Load an existing account table, if there is one. Missing file is not an error: the first run
    // of a new world has no accounts, and the first name typed into it creates one.
    void load_accounts(const char* path) { (void)accounts_.load(path); }
    bool save_accounts(const char* path) const { return accounts_.save(path); }
    [[nodiscard]] const AccountStore& accounts() const noexcept { return accounts_; }

    // Authenticate (or create), then bind the account to a free session slot. Returns the slot, or
    // -1 with `out` explaining why. Safe to call while the world is running: nothing is registered
    // here, only bound.
    int login(std::string_view name, std::string_view password, LoginOutcome& out) {
        const AccountId id = accounts_.login(name, password, out);
        if (id == kNoAccount) return -1;
        int slot = -1;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (bound_[i] == kNoAccount) {
                slot = i;
                break;
            }
            if (bound_[i] == id) {  // already logged in — take the same slot back
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            out = LoginOutcome::kFull;
            return -1;
        }
        bound_[slot] = id;
        BindAccount b{};
        b.account = id;
        // Open country, a good half-minute's walk from the nearest village. Not a farm, not a
        // tutorial, not a hearth already lit — GAME.md §6b.
        b.spawn_tx = static_cast<std::uint16_t>(layout_->spawn_tx());
        b.spawn_ty = static_cast<std::uint16_t>(layout_->spawn_ty());
        // Enough to light a fire and turn a few tiles of soil once you get somewhere. Deliberately
        // not enough to live on: the point of the walk is that you arrive needing people.
        b.wood = 40;
        b.stone = 25;
        b.seed = 12;
        player_ref(slot).tell(b);
        return slot;
    }

    [[nodiscard]] std::uint64_t key_of(int slot) const noexcept { return player_key(slot); }

    // One simulation step. The caller owns the pacing — a fixed-step loop in the headless runner, a
    // frame-rate-independent accumulator in the client.
    void step(std::int64_t dt_ms) { director_ref_.tell(DirectorTick{dt_ms}); }

    // A FIFO barrier on the director: the reply proves it has drained every DirectorTick posted
    // before it, and therefore has already fanned every `Tick` it was going to fan. Used instead of
    // sleeping so the headless runner is deterministic rather than timing-dependent.
    std::uint64_t sync_director() {
        quark::result<std::uint64_t> r =
            quark::block_on(director_ref_.ask<std::uint64_t>(GetWorldTick{}));
        return r.has_value() ? r.value() : 0;
    }

    // A barrier on the WHOLE world: the director first (so every Tick has been posted), then every
    // player and every chunk (so every Tick has been drained). After this returns, every published
    // snapshot reflects the same tick — which is the only way a sampled number is worth printing.
    //
    // Note what this is NOT: the simulation does not need it. Chunks are free to run behind the
    // director and behind each other, and normally do — that lag IS the pipelining. It exists so a
    // *reader* can take a consistent sample.
    std::uint64_t sync_world() {
        const std::uint64_t t = sync_director();
        for (int i = 0; i < kMaxPlayers; ++i) {
            (void)quark::block_on(player_ref(i).ask<PlayerView>(GetPlayer{}));
        }
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

    // The authoritative read. Prefer `players().load(slot)` anywhere a stale-by-one-tick answer is
    // acceptable — which is every renderer, and the reason `PlayerBus` exists.
    [[nodiscard]] PlayerView player_view(int slot) {
        quark::result<PlayerView> r = quark::block_on(player_ref(slot).ask<PlayerView>(GetPlayer{}));
        return r.has_value() ? r.value() : PlayerView{};
    }

    // --- player-driven actions ---------------------------------------------------------------
    void move_player(std::uint64_t player, float dx, float dy) {
        player_ref_by_key(player).tell(MoveIntent{dx, dy});
    }

    // Debug and bring-up only — see the note on `Teleport`. Nothing the player can press reaches it.
    void teleport_player(std::uint64_t player, std::uint16_t map, float x, float y) {
        player_ref_by_key(player).tell(Teleport{map, x, y});
    }

    void set_mounted(std::uint64_t player, bool on) {
        player_ref_by_key(player).tell(SetMounted{on});
    }

    // A melee swing. Ask the TRUSTED actor whether it may happen and how hard it lands, then tell
    // the chunks. The order is the whole security argument, and it is the same one `build_at` makes
    // about wood: the untrusted side is told the outcome, never consulted about it.
    bool swing(std::uint64_t player, bool heavy) {
        const AttackPlan p = plan(player, heavy ? AttackKind::kHeavy : AttackKind::kLight);
        if (!p.ok) return false;
        MeleeSwing s{};
        s.x = p.x;
        s.y = p.y;
        s.facing = p.facing;
        s.reach = p.reach;
        s.damage = p.damage;
        s.heavy = heavy;
        s.player = player;
        fan_to_neighbours(p.map, p.x, p.y, s);
        return true;
    }

    // An arrow, aimed by the client but launched from where the trusted actor says the player is.
    bool shoot(std::uint64_t player, float aim_x, float aim_y) {
        const AttackPlan p = plan(player, AttackKind::kShoot);
        if (!p.ok) return false;
        float dx = aim_x - p.x;
        float dy = aim_y - p.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f) {
            dx = facing_x(p.facing);
            dy = facing_y(p.facing);
        } else {
            dx /= len;
            dy /= len;
        }
        LaunchArrow a{};
        a.x = p.x;
        a.y = p.y;
        a.vx = dx * kArrowSpeed;
        a.vy = dy * kArrowSpeed;
        a.damage = p.damage;
        a.player = player;
        if (!in_map(p.x, p.y)) return false;
        chunk_ref_at(p.map, p.x, p.y).tell(a);
        return true;
    }

    // A spell, landing where the cursor is — but no further from the player than `kSpellRange`. The
    // clamp happens against the position the TRUSTED actor reported, not the one the client claims.
    bool cast(std::uint64_t player, Element element, float tx, float ty) {
        const AttackPlan p = plan(player, AttackKind::kCast, element);
        if (!p.ok) return false;
        float dx = tx - p.x;
        float dy = ty - p.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > kSpellRange) {
            dx = dx / len * kSpellRange;
            dy = dy / len * kSpellRange;
        }
        CastSpell s{};
        s.x = p.x + dx;
        s.y = p.y + dy;
        s.element = p.element;
        s.radius = kSpellRadius;
        s.damage = p.damage;
        s.player = player;
        fan_to_neighbours(p.map, s.x, s.y, s);
        return true;
    }

    // An ability. Ask the TRUSTED actor whether slot A/B may fire and how it lands, then fan the
    // resolved shape to the chunks BY THE PLAYER'S MAP — the same ask-then-tell ordering and the
    // same interior-aware fan-out as the basic verbs. One entry point for all six; the ability's
    // `kind` decides which chunk message carries it.
    bool use_ability(std::uint64_t player, std::uint8_t slot, Element element, float aim_x,
                     float aim_y) {
        UseAbility q{};
        q.slot = slot;
        q.element = element;
        q.aim_x = aim_x;
        q.aim_y = aim_y;
        quark::result<AbilityPlan> r =
            quark::block_on(player_ref_by_key(player).ask<AbilityPlan>(q));
        const AbilityPlan p = r.has_value() ? r.value() : AbilityPlan{};
        if (!p.ok) return false;
        const AbilityDef def = ability_def(p.ability);
        switch (def.kind) {
            case AbilityKind::kStrike: {
                AbilityStrike s{};
                s.x = p.x;
                s.y = p.y;
                s.facing = p.facing;
                s.shape = def.shape;
                s.radius = def.radius;
                s.damage = p.damage;
                s.stun_ticks = def.stun_ticks;
                s.element = p.element;
                s.skill = def.school;
                // Nova's flash is the CURRENT element's own effect (a fire bloom, an ice burst),
                // which is what makes a nova read as "the school I have selected"; the other strikes
                // carry their fixed flash from the table.
                s.fx = def.applies_element ? effect_of(p.element) : def.fx;
                s.player = player;
                fan_to_neighbours(p.map, p.x, p.y, s);
                break;
            }
            case AbilityKind::kVolley: {
                if (!in_map(p.x, p.y)) return false;
                // Aim toward the cursor, defaulting to the facing when it sits on the player, then
                // spread the arrows evenly across the fan angle.
                float dx = p.aim_x - p.x;
                float dy = p.aim_y - p.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                float base;
                if (len < 0.01f) {
                    base = std::atan2(facing_y(p.facing), facing_x(p.facing));
                } else {
                    base = std::atan2(dy, dx);
                }
                const float spread = static_cast<float>(def.spread_deg) * 3.14159265f / 180.0f;
                const int n = def.shots;
                for (int i = 0; i < n; ++i) {
                    const float t = (n <= 1) ? 0.0f
                                             : static_cast<float>(i) / static_cast<float>(n - 1) -
                                                   0.5f;  // -0.5 .. 0.5
                    const float ang = base + t * spread;
                    LaunchArrow a{};
                    a.x = p.x;
                    a.y = p.y;
                    a.vx = std::cos(ang) * kArrowSpeed;
                    a.vy = std::sin(ang) * kArrowSpeed;
                    a.damage = p.damage;
                    a.player = player;
                    chunk_ref_at(p.map, p.x, p.y).tell(a);
                }
                break;
            }
            case AbilityKind::kZone: {
                if (!in_map(p.x, p.y)) return false;
                SpawnZone z{};
                z.kind = def.zone_kind;
                z.x = p.x;
                z.y = p.y;
                z.radius = def.radius;
                z.ticks = def.zone_ticks;
                z.player = player;
                chunk_ref_at(p.map, p.x, p.y).tell(z);
                break;
            }
        }
        return true;
    }

    void plant(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty,
               CropKind k, std::int64_t now_ms) {
        if (!in_map(tx, ty)) return;
        chunk_ref(map, tx, ty).tell(PlantCrop{tx, ty, k, now_ms, player});
    }

    void harvest(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty) {
        if (!in_map(tx, ty)) return;
        chunk_ref(map, tx, ty).tell(HarvestAt{tx, ty, player});
    }

    // Base expansion: reclaim a tile as farmland. Costs a little wood so it is a real choice
    // against building with it.
    bool till(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty) {
        if (!in_map(tx, ty)) return false;
        quark::result<bool> paid = quark::block_on(
            player_ref_by_key(player).ask<bool>(SpendItems{ItemKind::kWood, kTillCost}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(TillGround{tx, ty, player});
        return true;
    }

    // Upgrade whatever building is on this tile. Same ask-then-tell ordering as build_at: the
    // trusted inventory decides affordability before the (possibly untrusted) chunk is told.
    bool upgrade(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty,
                 BuildKind k, std::uint8_t current_level) {
        if (!in_map(tx, ty) || current_level >= kMaxLevel) return false;
        const BuildCost c = upgrade_cost_of(k, current_level);
        quark::result<bool> paid =
            quark::block_on(player_ref_by_key(player).ask<bool>(SpendItems{c.kind, c.count}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(UpgradeBuilding{tx, ty, player});
        return true;
    }

    // Placement costs resources, so it is a two-step: ASK the trusted inventory to debit, and only
    // tell the (possibly untrusted) chunk to build if the debit succeeded. Doing it in this order
    // is what makes a compromised chunk host unable to mint free buildings.
    bool build_at(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty,
                  BuildKind k) {
        if (!in_map(tx, ty)) return false;
        const BuildCost c = cost_of(k);
        quark::result<bool> paid =
            quark::block_on(player_ref_by_key(player).ask<bool>(SpendItems{c.kind, c.count}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(PlaceBuilding{tx, ty, k, player});
        return true;
    }

    // Put creatures on the map at a point. This is the DIRECTOR's own message, exposed for tools:
    // the headless runner uses it to stage a fight it can assert on, and the client binds it to a
    // debug key. It deliberately does not bypass anything — the chunk validates the placement and
    // scales the creature for its ring exactly as it does for a raid.
    void spawn_wave_at(std::uint16_t tx, std::uint16_t ty, CreatureKind kind, std::uint16_t count,
                       std::uint32_t seed = 1, std::uint16_t map = kOverworld) {
        if (!in_map(tx, ty)) return;
        SpawnWave w{};
        w.count = count;
        w.seed = seed;
        w.kind = static_cast<std::uint8_t>(kind);
        w.tx = tx;
        w.ty = ty;
        w.radius = 2;
        chunk_ref(map, tx, ty).tell(w);
    }

    // Debug/tools: hand a player experience directly, so a staged scenario can reach the school
    // level an ability needs without grinding a fight for it. It rides the same GrantXp a kill uses
    // and is clamped by the same level cap in PlayerActor — it bypasses nothing but the grind.
    void grant_xp(std::uint64_t player, Skill skill, std::uint32_t amount) {
        player_ref_by_key(player).tell(GrantXp{skill, amount});
    }

    // Debug/tools: top a player's bars up. Amounts ADD and are clamped to the maxima by the trusted
    // actor, so passing the maxima refills from any state. Used by the headless runner to start each
    // staged ability fight from full, so the test measures the ability rather than the wildlife.
    void grant_vitals(std::uint64_t player, std::int16_t hp, std::int16_t mana,
                      std::int16_t stamina) {
        player_ref_by_key(player).tell(GrantVitals{hp, mana, stamina});
    }

    // The generated world, for anything that needs to know where things ARE rather than what a
    // chunk currently holds: the renderer (which buildings to draw), the map exporter, the tests.
    [[nodiscard]] const WorldLayout& layout() const noexcept { return *layout_; }

    [[nodiscard]] SnapshotBus& bus() noexcept { return bus_; }
    [[nodiscard]] const SnapshotBus& bus() const noexcept { return bus_; }
    [[nodiscard]] PlayerBus& players() noexcept { return players_; }
    [[nodiscard]] const PlayerBus& players() const noexcept { return players_; }
    [[nodiscard]] WorldStatus& status() noexcept { return status_; }
    [[nodiscard]] const WorldStatus& status() const noexcept { return status_; }
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

private:
    [[nodiscard]] AttackPlan plan(std::uint64_t player, AttackKind kind,
                                  Element element = Element::kNone) {
        quark::result<AttackPlan> r =
            quark::block_on(player_ref_by_key(player).ask<AttackPlan>(PlanAttack{kind, element}));
        return r.has_value() ? r.value() : AttackPlan{};
    }

    // A swing near a chunk border must reach across it, and a chunk only ever resolves hits against
    // creatures it owns — so the message goes to the 3x3 neighbourhood and each recipient filters.
    // Nothing can be hit twice because no two chunks own the same creature.
    //
    // The MAP is a parameter, not a constant. It used to be hard-coded to kOverworld, which quietly
    // meant a swing indoors fanned to the overworld chunks under the room and hit nothing in it — no
    // combat inside a building at all. The map comes from the trusted actor's plan, so a fight in a
    // dojo lands on the interior chunks that own the dojo's creatures.
    template <class M>
    void fan_to_neighbours(std::uint16_t map, float x, float y, const M& msg) {
        if (!in_map(x, y)) return;
        const ChunkCoord home = chunk_of(map, x, y);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = static_cast<int>(home.cx) + dx;
                const int cy = static_cast<int>(home.cy) + dy;
                if (cx < 0 || cy < 0 || cx >= kMapChunks || cy >= kMapChunks) continue;
                router_
                    ->get<ChunkActor>(chunk_key(ChunkCoord{map, static_cast<std::uint16_t>(cx),
                                                           static_cast<std::uint16_t>(cy)}))
                    .tell(msg);
            }
        }
    }

    [[nodiscard]] static constexpr float facing_x(Facing f) noexcept {
        return f == Facing::kLeft ? -1.0f : (f == Facing::kRight ? 1.0f : 0.0f);
    }
    [[nodiscard]] static constexpr float facing_y(Facing f) noexcept {
        return f == Facing::kUp ? -1.0f : (f == Facing::kDown ? 1.0f : 0.0f);
    }

    [[nodiscard]] quark::ActorRef<PlayerActor> player_ref(int slot) {
        return router_->get<PlayerActor>(player_key(slot));
    }

    [[nodiscard]] quark::ActorRef<PlayerActor> player_ref_by_key(std::uint64_t key) {
        return router_->get<PlayerActor>(key);
    }

    [[nodiscard]] quark::ActorRef<ChunkActor> chunk_ref(std::uint16_t map, std::uint16_t tx,
                                                        std::uint16_t ty) {
        return router_->get<ChunkActor>(
            chunk_key(chunk_of(map, static_cast<float>(tx), static_cast<float>(ty))));
    }

    [[nodiscard]] quark::ActorRef<ChunkActor> chunk_ref_at(std::uint16_t map, float x, float y) {
        return router_->get<ChunkActor>(chunk_key(chunk_of(map, x, y)));
    }

    // The whole roster is registered before `start()`, because `Engine::register_activation` is
    // cold-only (see player_actor.hpp). An unbound slot is inert — it ticks, ignores the tick, and
    // publishes an empty view that says `account == 0`.
    void build_players() {
        for (int slot = 0; slot < kMaxPlayers; ++slot) {
            auto p = std::make_unique<PlayerActor>();
            p->id = player_key(slot);
            p->slot = slot;
            p->map = kOverworld;
            p->bus = &players_;
            p->publish_now();
            auto act = std::make_unique<quark::Activation>(p.get(), PlayerActor::dispatch_table(),
                                                           pool_->sink());
            quark::register_actor<PlayerActor>(*engine_, player_key(slot), *act);
            players_actors_.push_back(std::move(p));
            player_acts_.push_back(std::move(act));
        }
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
                    // Null indoors, and said rather than left to `ready()` to catch: the flow field
                    // routes monsters to the nearest VILLAGE, and there is no village to walk to
                    // from inside somebody's front room. Only `flow_[kOverworld]` is ever built.
                    ch->flow = (map == kOverworld) ? &flow_[kOverworld] : nullptr;
                    // Fallback heading for a creature the flow field cannot route (an island, a
                    // pocket walled in by cliffs): the village nearest this chunk's own centre.
                    const int mid_x = cx * kChunkTiles + kChunkTiles / 2;
                    const int mid_y = cy * kChunkTiles + kChunkTiles / 2;
                    if (const Village* v = layout_->nearest_village(mid_x, mid_y)) {
                        ch->home_x = static_cast<float>(v->tx) + 0.5f;
                        ch->home_y = static_cast<float>(v->ty) + 0.5f;
                    }
                    ch->generate_terrain(kWorldSeed);
                    // Villages and roads are already in the terrain the line above cached — they
                    // are part of the world, not entities placed on top of it. Nothing is seeded
                    // here except wildlife: a new world starts with no player buildings anywhere.
                    //
                    // And no wildlife indoors. `seed_wildlife` places animals on walkable ground,
                    // and every room on the interior map is walkable ground — so without this every
                    // house on the overworld would have had a boar in it.
                    if (map == kOverworld) ch->seed_wildlife(kWorldSeed);
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
        director_->players = &players_;
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
    PlayerBus players_;
    WorldStatus status_;
    std::array<FlowField, kMapCount> flow_{};

    AccountStore accounts_;
    std::array<AccountId, kMaxPlayers> bound_{};

    std::vector<std::unique_ptr<PlayerActor>> players_actors_;
    std::vector<std::unique_ptr<quark::Activation>> player_acts_;

    std::vector<std::unique_ptr<ChunkActor>> chunks_;
    std::vector<std::unique_ptr<quark::Activation>> chunk_acts_;
    std::vector<ChunkCoord> chunk_coords_;

    std::unique_ptr<MapDirector> director_;
    std::unique_ptr<quark::Activation> director_act_;
    quark::ActorRef<MapDirector> director_ref_{};
};

}  // namespace mmo
