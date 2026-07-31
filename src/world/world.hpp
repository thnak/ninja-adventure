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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
#include "world/instance_manager.hpp"
#include "world/liveness.hpp"
#include "world/map_director.hpp"
#include "world/map_system.hpp"
#include "world/persistence.hpp"
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

        // RFC-014 §3.2: the shared, world-lifetime resources every ChunkActor needs — set once, cold,
        // before ANY chunk (eager or lazily broker-constructed) can exist. See chunk_actor.hpp's own
        // header note on why this is a process-wide pointer rather than a `wire()`/`ResourceScope` pass.
        detail::g_shared_router = router_.get();
        detail::g_shared_bus = &bus_;
        detail::g_shared_status = &status_;

        // One multi-source BFS, before any actor exists: distance to the nearest VILLAGE from every
        // tile. Read-only from here on — see flow_field.hpp for why handing every chunk a pointer
        // to it does not reintroduce shared mutable state.
        std::vector<std::pair<int, int>> targets;
        targets.reserve(layout_->villages().size());
        for (const Village& v : layout_->villages()) targets.emplace_back(v.tx, v.ty);
        flow_[0].build(kWorldSeed, kOverworld, targets);

        build_players();
        build_chunks();
        build_bosses();
        build_director();

        // RFC-014 §3.2: declares the TYPE as lazily-activatable for the instanced band. Cold, once,
        // no instance constructed by this call — the persistent band's 2048 chunks stay exactly as
        // eagerly `register_actor`'d as they were before this line existed (verified against the
        // real engine source: `router.get<A>()`'s existing fast path is untouched by declaring a
        // type lazy; the lazy table is only consulted on a miss, which never happens for an
        // already-registered persistent-band chunk).
        (void)engine_->declare_lazy<ChunkActor>(nullptr, pool_->sink());

        instance_manager_.router = router_.get();
        instance_manager_.bus = &bus_;
        instance_manager_.world_seed = kWorldSeed;
        instance_manager_.director_ref = router_->get<MapDirector>(kDirectorKey);
    }

    void start() { engine_->start(); }

    void stop() { engine_->stop(); }

    // --- accounts ---------------------------------------------------------------------------------

    // Load an existing account table, if there is one. Missing file is not an error: the first run
    // of a new world has no accounts, and the first name typed into it creates one.
    void load_accounts(const char* path) { (void)accounts_.load(path); }
    bool save_accounts(const char* path) const { return accounts_.save(path); }
    [[nodiscard]] const AccountStore& accounts() const noexcept { return accounts_; }

    // --- RFC-016: persistence ------------------------------------------------------------------
    //
    // Opens (creating if missing) `<saves_root>/<world_name>/` — §2's directory layout: an
    // unchanged accounts.dat (§3, relocated only), a progression/ FileStore root (§4), and an
    // overlay/ FileStore root (§6). MUST be called after build() so the just-constructed (empty)
    // ChunkActors already exist for recover_overlay() to fold recovered state into, before
    // start() — the same cold, synchronous bring-up window build_bosses() already writes into.
    bool open_save(const std::string& saves_root, const std::string& world_name) {
        namespace fs = std::filesystem;
        save_root_ = saves_root + "/" + world_name;
        std::error_code ec;
        fs::create_directories(save_root_, ec);
        fs::create_directories(save_root_ + "/progression", ec);
        fs::create_directories(save_root_ + "/overlay", ec);

        const std::string manifest_path = save_root_ + "/manifest.dat";
        WorldManifest m{};
        if (!load_world_manifest(manifest_path.c_str(), m)) {
            m.world_seed = kWorldSeed;
            m.created_at_unix = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            std::snprintf(m.world_name, sizeof m.world_name, "%s", world_name.c_str());
            (void)save_world_manifest(manifest_path.c_str(), m);
        }

        accounts_.load((save_root_ + "/accounts.dat").c_str());
        progression_store_.open(save_root_ + "/progression");
        overlay_store_.open(save_root_ + "/overlay");
        recover_overlay();
        return true;
    }

    // The "lúc thoát" half of §6.3's "periodic + on-exit" pair — unconditional, ignoring both
    // cadence timers. Call once on clean shutdown.
    void save_world_now() {
        if (save_root_.empty()) return;
        (void)accounts_.save((save_root_ + "/accounts.dat").c_str());
        checkpoint_progression();
    }

    // §6.3: two independent cadences, self-rate-limited exactly like InstanceManager::sweep_idle
    // (instance_manager.hpp) — cheap to call every tick; the caller does not need to track timing.
    void run_periodic_persistence(std::int64_t world_ms) {
        if (save_root_.empty()) return;
        if (last_progression_checkpoint_ms_ < 0 ||
            world_ms - last_progression_checkpoint_ms_ >= kProgressionCheckpointIntervalMs) {
            checkpoint_progression();
            last_progression_checkpoint_ms_ = world_ms;
        }
        if (last_overlay_compact_ms_ < 0 ||
            world_ms - last_overlay_compact_ms_ >= kOverlaySnapshotIntervalMs) {
            compact_overlay();
            last_overlay_compact_ms_ = world_ms;
        }
    }

    // §4.2: hp/mana/stamina/x/y (and, by this pass's simplification, everything else in the row)
    // checkpointed on the periodic cadence for every currently bound session slot.
    void checkpoint_progression() {
        if (save_root_.empty()) return;
        for (int slot = 0; slot < kMaxPlayers; ++slot) {
            if (bound_[slot] == kNoAccount) continue;
            const PlayerView v = player_view(slot);
            if (!v.live()) continue;
            progression_store_.save(bound_[slot], progression_of(v));
        }
    }

    // §6.3 point 2: folds each touched persistent-band chunk's durable log into a compaction
    // snapshot, bounding replay length on a future recovery. Reads the live published ChunkView
    // rather than reaching into ChunkActor directly — the same lossy-but-sufficient channel the
    // renderer already reads (snapshot.hpp).
    void compact_overlay() {
        for (const ChunkCoord& c : chunk_coords_) {
            if (c.map >= kPersistentBandEnd) continue;  // §9: instanced band is out of scope
            if (overlay_store_.touched().find(chunk_key(c)) == overlay_store_.touched().end()) {
                continue;
            }
            ChunkViewPtr v = bus_.load(c);
            if (!v) continue;
            ChunkOverlaySnapshot snap;
            snap.crops = v->crops;
            snap.buildings = v->buildings;
            for (int ly = 0; ly < kChunkTiles; ++ly) {
                for (int lx = 0; lx < kChunkTiles; ++lx) {
                    const std::size_t li = static_cast<std::size_t>(ly * kChunkTiles + lx);
                    if (v->terrain[li] != static_cast<std::uint8_t>(Terrain::kDirt)) continue;
                    const int gx = c.cx * kChunkTiles + lx;
                    const int gy = c.cy * kChunkTiles + ly;
                    if (terrain_of(kWorldSeed, c.map, gx, gy) == Terrain::kDirt) continue;  // natural
                    snap.tilled.push_back(
                        TilledTile{static_cast<std::uint16_t>(gx), static_cast<std::uint16_t>(gy)});
                }
            }
            overlay_store_.compact(c, snap, overlay_store_.last_committed_seq(c));
        }
    }

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

        // RFC-014 §6 Reconnect: if this slot's last published position (untouched by an earlier
        // Unbind — see player_actor.hpp's handler) sits inside a still-open, non-closing
        // InstanceSession, resume in place instead of respawning fresh. An unbound slot that was
        // never inside an instance (the ordinary case — `map` defaults to kOverworld, never
        // `map_id_instanced`) always falls through to the fresh-spawn path below, unchanged.
        const PlayerView last = player_view(slot);
        if (map_id_instanced(last.map)) {
            InstanceSession* resume = instance_manager_.find_session(last.map);
            if (resume != nullptr && !instance_session_closing(resume->state)) {
                player_ref(slot).tell(Rebind{id});
                instance_manager_.enter(last.map, id);
                return slot;
            }
        }

        // RFC-016 §4/§7: a returning account gets its persisted progression back instead of the
        // fixed starter pack. NOTE what this does not solve: within one still-running process, a
        // non-instanced disconnect+reconnect already falls through to this same branch today
        // (Unbind's own header comment names the gap — only the instanced-resume case above is
        // wired) and always resets from SOME source; before this RFC that source was unconditionally
        // the starter pack, which this branch now improves to "the last checkpoint" without
        // attempting to fix same-process reconnect continuity itself (RFC-014/015's reconnect-
        // detector territory, still not wired end-to-end — out of scope here).
        if (std::optional<PlayerProgression> saved = progression_store_.load(id)) {
            apply_recovery_defaults(*saved);  // §5: hp<=0 at last checkpoint loads as respawned
            player_ref(slot).tell(restore_message(id, *saved));
            return slot;
        }

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

    // --- RFC-014: instance lifecycle, exposed for tests/tools ---------------------------------------
    // No portal-step trigger detector exists yet (that is client/input-layer territory, not this
    // RFC's — see instance_manager.hpp's own header note); these are the real, callable verbs a
    // future trigger would invoke, exercised directly today the same way `teleport_player` already is.

    [[nodiscard]] InstanceManager& instances() noexcept { return instance_manager_; }

    // RFC-022 §2.3 resolve() + RFC-014 §3 allocate_new(), composed into one call: resolve the portal
    // against currently-live sessions, allocate a fresh instance if the decision was
    // kNeedsAllocation, then physically move the player there. Returns false if allocation was
    // refused (kMaxConcurrentInstances) or the player key does not resolve.
    bool use_portal(std::uint64_t player, const PortalDef& portal, GroupId group, AccountId account,
                    MapDescriptor instanced_descriptor = {}) {
        const std::vector<MapSession>& live = instance_manager_.live_sessions_for_resolve();
        const ResolveResult r = resolve_portal(portal, live, group);
        MapId target = 0;
        float tx = portal.fixed_to_x + 0.5f;
        float ty = portal.fixed_to_y + 0.5f;
        MapId return_map = 0;
        std::uint16_t return_x = 0, return_y = 0;
        if (r.outcome == ResolveOutcome::kFound) {
            target = r.session.map_id;
            return_map = r.session.return_map;
            return_x = r.session.return_x;
            return_y = r.session.return_y;
            if (map_id_instanced(target)) instance_manager_.enter(target, account);
        } else {
            const MapSession* s = instance_manager_.allocate_new(portal, group, instanced_descriptor,
                                                                  account);
            if (s == nullptr) return false;
            target = s->map_id;
            tx = 1.5f;  // an instance's own (0,0) chunk centre — no authored spawn point exists yet
            ty = 1.5f;
            return_map = s->return_map;
            return_x = s->return_x;
            return_y = s->return_y;
        }
        // RFC-013 §6.2: cache the session's return point on the player's own actor BEFORE the
        // Teleport that lands them there — so a death on the very first tick after arrival still has
        // a real ejection destination rather than reading the all-zero "unset" default.
        if (map_id_instanced(target)) {
            player_ref_by_key(player).tell(
                SetInstanceReturn{static_cast<std::uint16_t>(return_map), return_x, return_y});
        }
        player_ref_by_key(player).tell(Teleport{target, tx, ty});
        return true;
    }

    // RFC-014 §6 deliberate exit: leaves `present`, never `members` — see instance_manager.hpp.
    // RFC-013 §6.8: also the bookkeeping call for a death-triggered ejection — "bookkept identically
    // to Deliberate Exit, not Disconnect," per that RFC's own ruling, so no second method is added
    // for it. Like a `kReturnPortal` crossing, nothing detects the moment to call this automatically
    // yet (no portal-trigger or death-observer wiring exists — both are equally caller-invoked today,
    // exercised directly by tests/tools, same as every other instance-lifecycle verb on this class).
    void leave_instance(MapId map, AccountId account) { instance_manager_.leave_present(map, account); }

    // RFC-014 §6 disconnect (data-level only — see Unbind's own header note).
    void disconnect_player(std::uint64_t player, MapId map, AccountId account) {
        instance_manager_.leave_present(map, account);
        player_ref_by_key(player).tell(Unbind{});
    }

    // RFC-014 §3.5: call on a coarse cadence, not once per simulation tick.
    void sweep_instances(std::int64_t world_ms) { instance_manager_.sweep_idle(world_ms); }

    [[nodiscard]] std::uint64_t key_of(int slot) const noexcept { return player_key(slot); }
    [[nodiscard]] AccountId account_of(int slot) const noexcept { return bound_[slot]; }

    // RFC-024 §3.5: input to the host-side courtesy notice ("N other players are connected —
    // closing now will disconnect them"). Counts every slot bound since this process started,
    // including the caller's own (`client_main.cpp` subtracts its own slot) — NOT "currently online"
    // in any stronger sense: `disconnect_player()` is data-level only (RFC-014's own scoping,
    // `disconnect_player`'s comment above) and never frees `bound_`, so an account that disconnected
    // and never reconnected still counts here. That is the one real limitation this number carries;
    // building true online/offline presence tracking is a separate, unrequested feature, not this
    // RFC's job.
    [[nodiscard]] int connected_player_count() const noexcept {
        int n = 0;
        for (int slot = 0; slot < kMaxPlayers; ++slot) {
            if (bound_[slot] != kNoAccount) ++n;
        }
        return n;
    }

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
        // RFC-001 Section 4: a payload only exists to dispatch once the head has reached Release
        // in this very call (phase == kIdle — see AbilityPlan's comment). Every shipped ability has
        // cast_ticks == 0 so this is always true today; a future cast-time ability would instead
        // still be sitting in kCast/kChannel here, with nothing yet to fan out.
        if (p.phase != AbilityPhase::kIdle) return true;
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
                s.stagger_power = def.stagger_power;
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
                // RFC-004: spawns a CombatEntity (kSmokeCloud/kWaterPool) instead of a Zone. Fanned to
                // the 3x3 neighbourhood, the same as a swing (F2): a spawn centred near a border
                // reaches into its neighbours, and each recipient keeps the part that overlaps it.
                SpawnEntity se{};
                se.kind = def.spawn_entity_kind;
                se.x = p.x;
                se.y = p.y;
                se.team = Faction::kPlayer;
                se.owner = player;
                se.radius_override = def.radius;
                fan_to_neighbours(p.map, p.x, p.y, se);
                break;
            }
        }
        return true;
    }

    void plant(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty,
               CropKind k, std::int64_t now_ms) {
        if (!in_map(tx, ty)) return;
        chunk_ref(map, tx, ty).tell(PlantCrop{tx, ty, k, now_ms, player});
        record_overlay(map, tx, ty,
                        ChunkMutationEvent{OverlayEventKind::kPlantCrop, tx, ty,
                                           static_cast<std::uint8_t>(k), now_ms, player});
    }

    void harvest(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty) {
        if (!in_map(tx, ty)) return;
        chunk_ref(map, tx, ty).tell(HarvestAt{tx, ty, player});
        record_overlay(map, tx, ty,
                        ChunkMutationEvent{OverlayEventKind::kHarvestAt, tx, ty, 0, 0, player});
    }

    // Base expansion: reclaim a tile as farmland. Costs a little wood so it is a real choice
    // against building with it.
    bool till(std::uint64_t player, std::uint16_t map, std::uint16_t tx, std::uint16_t ty) {
        if (!in_map(tx, ty)) return false;
        quark::result<bool> paid = quark::block_on(
            player_ref_by_key(player).ask<bool>(SpendItems{ItemKind::kWood, kTillCost}));
        if (!paid.has_value() || !paid.value()) return false;
        chunk_ref(map, tx, ty).tell(TillGround{tx, ty, player});
        record_overlay(map, tx, ty,
                        ChunkMutationEvent{OverlayEventKind::kTillGround, tx, ty, 0, 0, player});
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
        record_overlay(map, tx, ty,
                        ChunkMutationEvent{OverlayEventKind::kUpgradeBuilding, tx, ty, 0, 0, player});
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
        record_overlay(map, tx, ty,
                        ChunkMutationEvent{OverlayEventKind::kPlaceBuilding, tx, ty,
                                           static_cast<std::uint8_t>(k), 0, player});
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

    // Debug/tools: one creature on an EXACT tile (radius 0), for a staged scenario that needs a
    // single attacker at a known distance rather than a scattered wave — the F2 telegraph checks.
    void spawn_one_at(std::uint16_t tx, std::uint16_t ty, CreatureKind kind,
                      std::uint16_t map = kOverworld) {
        if (!in_map(tx, ty)) return;
        SpawnWave w{};
        w.count = 1;
        w.seed = 1;
        w.kind = static_cast<std::uint8_t>(kind);
        w.tx = tx;
        w.ty = ty;
        w.radius = 0;
        chunk_ref(map, tx, ty).tell(w);
    }

    // Debug/tools: drop a CombatEntity directly, bypassing the ability gate the same way
    // `spawn_wave_at` bypasses the director. Fanned to the neighbourhood exactly as the ability path
    // does for non-blocking kinds, so a staged scenario can prove the F2 seam fix (a spawn on a chunk
    // border reaching both sides) or exercise a blocking/always-hot/totem archetype no shipped
    // ability produces yet, without first levelling a caster or authoring boss content.
    void spawn_entity_at(EntityKind kind, float x, float y, float radius_override = 0.0f,
                         bool boss_room = false, std::uint16_t map = kOverworld,
                         Faction team = Faction::kPlayer, std::uint64_t owner = 0) {
        if (!in_map(x, y)) return;
        SpawnEntity e{};
        e.kind = kind;
        e.x = x;
        e.y = y;
        e.team = team;
        e.owner = owner;
        e.radius_override = radius_override;
        e.boss_room = boss_room;
        fan_to_neighbours(map, x, y, e);
    }

    // Debug/tools: hand a player experience directly, so a staged scenario can reach the school
    // level an ability needs without grinding a fight for it. It rides the same GrantXp a kill uses
    // and is clamped by the same level cap in PlayerActor — it bypasses nothing but the grind.
    void grant_xp(std::uint64_t player, Skill skill, std::uint32_t amount) {
        player_ref_by_key(player).tell(GrantXp{skill, amount});
    }

    // RFC-019 §5.6: respec at the player's own Hearth. No portal-step/UI trigger exists yet (that is
    // RFC-011's Combat HUD territory) — this is the real, callable verb a future "respec" action
    // would invoke, exercised directly here the same way `grant_xp` above already is. PlayerActor
    // itself enforces the Hearth-proximity and Overworld-map preconditions; this is a thin
    // passthrough, not a second copy of that logic.
    void respec_skill(std::uint64_t player, Skill from, Skill to) {
        player_ref_by_key(player).tell(RespecSkill{from, to});
    }

    // RFC-019 §5.7: debug/tools — hand a player Essence directly against one branch's Tier IV gate,
    // so a staged scenario can clear a capstone without RFC-018's (proposed) challenge-realm reward
    // path existing yet. Rides the same GrantEssence a future reward table would send.
    void grant_essence(std::uint64_t player, Skill skill, std::uint8_t amount = 1) {
        player_ref_by_key(player).tell(GrantEssence{skill, amount});
    }

    void use_waypoint(std::uint64_t player, std::uint16_t village_index, ItemKind pay_kind) {
        player_ref_by_key(player).tell(UseWaypoint{village_index, pay_kind});
    }

    // RFC-021 §5.2/§4.3: read a player's discovery state — the per-village visited/usable bitsets
    // (`DiscoveryView`) and a single fog-cell query, both exposed for tests/tools/a future Map
    // screen rather than the per-tick `PlayerView` bus (§5.4's own explicit cadence separation).
    [[nodiscard]] DiscoveryView discovery_of(std::uint64_t player) {
        quark::result<DiscoveryView> r =
            quark::block_on(player_ref_by_key(player).ask<DiscoveryView>(GetDiscovery{}));
        return r.has_value() ? r.value() : DiscoveryView{};
    }

    [[nodiscard]] bool fog_revealed(std::uint64_t player, std::uint16_t tx, std::uint16_t ty) {
        quark::result<bool> r =
            quark::block_on(player_ref_by_key(player).ask<bool>(IsFogRevealed{tx, ty}));
        return r.has_value() && r.value();
    }

    // Debug/tools: top a player's bars up. Amounts ADD and are clamped to the maxima by the trusted
    // actor, so passing the maxima refills from any state. Used by the headless runner to start each
    // staged ability fight from full, so the test measures the ability rather than the wildlife.
    void grant_vitals(std::uint64_t player, std::int16_t hp, std::int16_t mana,
                      std::int16_t stamina) {
        player_ref_by_key(player).tell(GrantVitals{hp, mana, stamina});
    }

    // Debug/tools: `GrantVitals`'s symmetric opposite, exercising the exact `HurtPlayer` message a
    // creature's own strike sends (chunk_actor.hpp), without needing a staged fight to land it. Lets
    // RFC-013's death/ejection contract be tested directly against a chosen amount and source.
    void hurt_player(std::uint64_t player, std::int16_t amount, std::uint32_t source = 0) {
        player_ref_by_key(player).tell(HurtPlayer{amount, source});
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

    // RFC-016 §6.2/§9: the durable counterpart of `chunk_ref(...).tell(...)` — instanced-band maps
    // are out of scope for durability by RFC-014's own default policy, so only persistent-band
    // mutations are staged/committed.
    void record_overlay(std::uint16_t map, std::uint16_t tx, std::uint16_t ty,
                        const ChunkMutationEvent& e) {
        if (map >= kPersistentBandEnd) return;
        overlay_store_.record(chunk_of(map, static_cast<float>(tx), static_cast<float>(ty)), e);
    }

    // RFC-016 §6.4: called once by open_save(), right after build_chunks() has constructed every
    // (empty) ChunkActor and before start() — the same cold, synchronous bring-up window
    // build_bosses() already writes into directly.
    void recover_overlay() {
        for (const ChunkCoord& c : chunk_coords_) {
            if (c.map >= kPersistentBandEnd) continue;
            std::optional<ChunkOverlaySnapshot> snap = overlay_store_.recover(c);
            if (!snap.has_value()) continue;
            ChunkActor& ch = *chunks_[static_cast<std::size_t>(chunk_index(c))];
            ch.apply_recovered_overlay(*snap);
            ch.publish_now();
        }
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

    // Plant a dojo boss in every interior room a tier>=3 village's DOJO door leads into (F3). Each
    // room lives inside one interior chunk (a room block is 16 tiles, well within a 32-tile chunk),
    // and `dojo_rooms` is derived from the layout alone — so this needs nothing from the simulation
    // and, like everything else here, runs once before the first tick. Re-publish so the boss is in
    // the very first view (the build loop published these chunks empty a moment ago).
    void build_bosses() {
        for (std::uint32_t room : layout_->dojo_rooms()) {
            const int bx = room_block_x(static_cast<int>(room));
            const int by = room_block_y(static_cast<int>(room));
            const ChunkCoord cc = chunk_of(kInterior, static_cast<float>(bx + kRoomX0),
                                           static_cast<float>(by + kRoomY0));
            ChunkActor& ch = *chunks_[static_cast<std::size_t>(chunk_index(cc))];
            ch.add_boss(room);
            ch.publish_now();
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

    InstanceManager instance_manager_;

    // RFC-016: unopened (save_root_.empty()) until open_save() is called — every persistence
    // method above is a no-op until then, matching load_accounts()'s own "missing file is not an
    // error" tone for a headless run that never calls open_save() at all (sim_main.cpp's default).
    std::string save_root_;
    PlayerProgressionStore progression_store_;
    WorldOverlayStore overlay_store_;
    std::int64_t last_progression_checkpoint_ms_ = -1;
    std::int64_t last_overlay_compact_ms_ = -1;
};

}  // namespace mmo
