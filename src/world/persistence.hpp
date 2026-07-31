// RFC-016 — Persistence & Save-File Format.
//
// SCOPE OF THIS PASS, narrowed against the RFC's own text and documented here rather than
// scattered:
//
// 1. BACKEND: `quark::FileStore`, not `quark::adapters::SqliteStore`. The RFC's own §1 names
//    SqliteStore the default but rules FileStore "an explicitly supported fallback... a
//    build-configuration choice, not a schema choice — either backend implements the identical
//    Store concept." This project's CMakeLists never sets `QUARK_WITH_SQLITE` (grepped: zero
//    hits), and adding a new hard build dependency is out of scope for this pass — matching the
//    project's own "0 đồng, không cần gì thêm" discipline (GAME.md/ARCHITECTURE.md). FileStore is
//    std-only and already crash-durable (fdatasync-framed WAL, quark/core/file_store.hpp).
//    Swapping to SqliteStore later is a `Store` template-parameter change only; nothing below is
//    written against FileStore-specific behavior.
//
// 2. "RELATIONAL TABLES" -> DESCRIBED SNAPSHOT STRUCTS. `FileStore`/`SqliteStore` both sit behind
//    the same per-ActorId blob-store `Store` concept (quark/core/persistence.hpp) — there is no
//    join surface to design against, so §4.1's `players`/`player_items` two-table schema becomes
//    ONE `PlayerProgression` struct with a fixed-size item/skill array folded in (kItemKinds and
//    kSkillCount are both small, fixed, compile-time constants — a child table buys nothing a
//    vector field does not already give). Quest state (§"Quest state" table) is not built: no
//    `QuestInstance` exists anywhere in this codebase (grepped, zero hits) — RFC-020 is unbuilt,
//    so there is nothing concrete to schema against, the same reasoning RFC-015 used to defer
//    wire encoding for `ChunkView` fields no RFC had specified yet.
//
// 3. `WorldPersistenceActor` (RFC §6.2) is NOT a real `quark::Actor<..., Require<Trusted>>` here.
//    `instance_manager.hpp` already establishes and documents the exact same divergence for
//    `InstanceManager`, for the exact same reason: `World` already IS this codebase's single-
//    process leader, and every mutating call site (`World::plant`/`till`/`upgrade`/`build_at`)
//    already runs as plain, synchronous C++ before this RFC. `WorldOverlayStore::record()` below
//    is called directly from those same call sites — which makes the Sync commit MORE literal
//    than the RFC's own actor-message design (it completes before the `World` method returns, no
//    second actor hop to reason about), not less.
//
// 4. `manifest.json` (RFC §2.1) is written as a flat binary record (magic + version + fields),
//    mirroring `account.hpp`'s own established convention. No JSON library exists anywhere in
//    this codebase (grepped) and adding one is out of scope for this pass — same information,
//    different physical encoding, exactly the kind of divergence RFC-015 already documented for
//    translating an RFC's literal shape onto what this codebase's dependencies actually support.
//
// 5. DEFERRED, NAMED (matching the RFC's own Non-goals precedent): RL checkpoint storage/retention
//    (§8) — no RL checkpoint file I/O exists anywhere in `src/` today (grepped: no "gen_0",
//    "meta.json", "weights_hash"), so there is no concrete path to redirect, the same "nothing to
//    schema against" reasoning as quest state. Multi-world create/load/delete MENU UI (§2.2) — no
//    such menu exists in this project; the real, callable mechanism (`World::open_save`) is built
//    and exercised directly, matching the precedent every instance-lifecycle verb in
//    `world.hpp` already set ("no trigger detector exists yet — these are the real, callable
//    verbs a future trigger would invoke"). Live (leader-running) save export (§9 Open Question
//    3) — the always-correct "export while stopped" baseline needs no code here (it is just
//    "copy the directory"; nothing this pass builds needs a running-export code path).
//    `kWorldSeed` becoming a genuine runtime, per-world value (§2.1, the RFC's own Open Question
//    2) — `manifest.dat` records whatever seed was used, but `World::build()` still reads the
//    single compile-time `kWorldSeed` constant at every call site, unchanged; named, not solved,
//    the same "two ways to close this gap, neither chosen here" shape RFC-014 §3.2 already used.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "quark/core/event_log.hpp"
#include "quark/core/file_store.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/snapshot.hpp"

#include "world/account.hpp"
#include "world/protocol.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"

namespace mmo {

// --- §2.1: the world directory's own manifest ------------------------------------------------

struct WorldManifest {
    char world_name[64] = {};
    std::uint64_t world_seed = 0;
    std::int64_t created_at_unix = 0;
};

namespace detail_persist {
inline constexpr std::uint32_t kManifestMagic = 0x4E'57'4C'44;  // "NWLD"
inline constexpr std::uint32_t kManifestVersion = 1;
}  // namespace detail_persist

[[nodiscard]] inline bool load_world_manifest(const char* path, WorldManifest& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::uint32_t header[2] = {};
    const bool ok = std::fread(header, sizeof header, 1, f) == 1 &&
                    header[0] == detail_persist::kManifestMagic &&
                    header[1] == detail_persist::kManifestVersion &&
                    std::fread(&out, sizeof out, 1, f) == 1;
    std::fclose(f);
    return ok;
}

[[nodiscard]] inline bool save_world_manifest(const char* path, const WorldManifest& m) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const std::uint32_t header[2] = {detail_persist::kManifestMagic, detail_persist::kManifestVersion};
    const bool ok = std::fwrite(header, sizeof header, 1, f) == 1 &&
                    std::fwrite(&m, sizeof m, 1, f) == 1;
    std::fclose(f);
    return ok;
}

// --- §4: player progression — Persistent<Snapshot, Batched> equivalent ------------------------
//
// One row per account, folded from `player_items` (§4.1) into fixed-size arrays: kItemKinds and
// kSkillCount are small compile-time constants, so a child table buys nothing here that a vector
// field does not already give under a per-ActorId blob store (see file header, point 2).
// `equipped_ability_0/1` (§4.1's reserved-but-NULL columns): RFC-011 now exists and is the first
// code to write them — added below as `loadout`, exactly as additive as this file originally
// predicted a reserved SQL column would have been (tag 17, never renumbering an existing tag).
struct PlayerProgression {
    std::uint16_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    std::int16_t mana = 0;
    std::int16_t stamina = 0;
    std::uint32_t deaths = 0;
    std::uint16_t respawn_tx = 0;
    std::uint16_t respawn_ty = 0;
    // §7: RFC-014's flagged return-location breadcrumb, resolved here — mirrors
    // PlayerActor::instance_return_map_/x_/y_ (RFC-013 §6.2) verbatim. `return_x`/`return_y`
    // are packed into one `uint32_t` (low 16 bits x, high 16 bits y) — the exact tx/ty-packing
    // idiom `Door::tile` already uses elsewhere in this codebase — to free one tagged-field
    // slot: `QUARK_SERIALIZE`'s underlying `QUARK_FOR_EACH` X-macro chain is authored only up
    // to 16 `(tag, member)` pairs (`quark/core/describe.hpp`), a real ceiling in the shared,
    // externally-owned QuarkCpp engine this project should not fork/patch around. RFC-011 §5.3's
    // new `loadout` field below needed the slot this packing frees, not a 17th pair.
    std::uint16_t return_map = 0;
    std::uint32_t return_xy = 0;
    std::vector<std::uint8_t> level;  // kSkillCount entries, Skill-enum order
    std::vector<std::uint32_t> xp;    // kSkillCount entries
    std::vector<std::int32_t> items;  // kItemKinds entries, ItemKind-enum order
    // RFC-019 §5.7: Essence units spent against each branch's Tier IV gate — without this, a
    // restart would silently re-lock levels 18-20 for a player who had already paid for them.
    std::vector<std::uint8_t> essence_paid;  // kSkillCount entries
    // RFC-011 §5.3: the raw manual-loadout-picker state — `kLoadoutAuto` or a raw `AbilityId`
    // value per slot, mirroring `PlayerView::loadout_raw` exactly (see that field's own comment
    // for why this is not simply the resolved `ability[]`).
    std::vector<std::uint8_t> loadout;  // kAbilitySlots entries
};
QUARK_SERIALIZE(PlayerProgression, (1, map), (2, x), (3, y), (4, hp), (5, mana), (6, stamina),
                (7, deaths), (8, respawn_tx), (9, respawn_ty), (10, return_map), (11, return_xy),
                (12, level), (13, xp), (14, items), (15, essence_paid), (16, loadout))

[[nodiscard]] inline PlayerProgression progression_of(const PlayerView& v) {
    PlayerProgression p;
    p.map = v.map;
    p.x = v.x;
    p.y = v.y;
    p.hp = v.hp;
    p.mana = v.mana;
    p.stamina = v.stamina;
    p.deaths = v.deaths;
    p.respawn_tx = v.respawn_tx;
    p.respawn_ty = v.respawn_ty;
    p.return_map = v.return_map;
    p.return_xy = static_cast<std::uint32_t>(v.return_x) |
                  (static_cast<std::uint32_t>(v.return_y) << 16);
    p.level.assign(v.skill_level, v.skill_level + kSkillCount);
    p.xp.assign(v.skill_xp, v.skill_xp + kSkillCount);
    p.items.assign(v.items, v.items + kItemKinds);
    p.essence_paid.assign(v.essence_paid, v.essence_paid + kSkillCount);
    p.loadout.assign(v.loadout_raw, v.loadout_raw + kAbilitySlots);
    return p;
}

[[nodiscard]] inline RestoreProgression restore_message(AccountId account,
                                                         const PlayerProgression& p) {
    RestoreProgression r{};
    r.account = account;
    r.map = p.map;
    r.x = p.x;
    r.y = p.y;
    r.hp = p.hp;
    r.mana = p.mana;
    r.stamina = p.stamina;
    r.deaths = p.deaths;
    r.respawn_tx = p.respawn_tx;
    r.respawn_ty = p.respawn_ty;
    r.return_map = p.return_map;
    r.return_x = static_cast<std::uint16_t>(p.return_xy & 0xFFFFu);
    r.return_y = static_cast<std::uint16_t>(p.return_xy >> 16);
    for (int i = 0; i < kItemKinds && i < static_cast<int>(p.items.size()); ++i) {
        r.items[i] = p.items[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < kSkillCount && i < static_cast<int>(p.level.size()); ++i) {
        r.level[i] = p.level[static_cast<std::size_t>(i)];
        r.xp[i] = (i < static_cast<int>(p.xp.size())) ? p.xp[static_cast<std::size_t>(i)] : 0;
        r.essence_paid[i] = (i < static_cast<int>(p.essence_paid.size()))
                                ? p.essence_paid[static_cast<std::size_t>(i)]
                                : 0;
    }
    for (int i = 0; i < kAbilitySlots; ++i) {
        // A save written before this field existed has an empty vector — kLoadoutAuto (unchanged
        // auto-pick behavior) is the only correct default, never a raw 0 (which would misread as
        // a manual WhirlCleave pick).
        r.loadout[i] = (i < static_cast<int>(p.loadout.size()))
                           ? p.loadout[static_cast<std::size_t>(i)]
                           : kLoadoutAuto;
    }
    return r;
}

// §5: "a recovering player whose last-checkpointed hp <= 0 is loaded as already-respawned" —
// mirrors PlayerActor::respawn() (player_actor.hpp) exactly, since dead_ticks_ itself is never
// persisted (§5's own ruling: resuming the exact remaining countdown buys nothing perceptible).
inline void apply_recovery_defaults(PlayerProgression& p) noexcept {
    if (p.hp > 0) return;
    if (map_id_instanced(p.map)) {
        const bool have_return = p.return_map != 0 || p.return_xy != 0;
        if (have_return) {
            p.map = p.return_map;
            p.x = static_cast<float>(p.return_xy & 0xFFFFu) + 0.5f;
            p.y = static_cast<float>(p.return_xy >> 16) + 0.5f;
        } else {
            p.map = kOverworld;
            p.x = static_cast<float>(p.respawn_tx) + 0.5f;
            p.y = static_cast<float>(p.respawn_ty) + 0.5f;
        }
    } else {
        p.x = static_cast<float>(p.respawn_tx) + 0.5f;
        p.y = static_cast<float>(p.respawn_ty) + 0.5f;
    }
    p.hp = kPlayerMaxHp;
    p.mana = kPlayerMaxMana;
    p.stamina = kPlayerMaxStamina;
    // Deliberately NOT clearing items: that only happens on a genuine in-session instanced-band
    // ejection (PlayerActor::respawn()'s pending_eject_ branch, RFC-013 §6.5). Recovering an
    // already-dead checkpoint on restart is not that event, and §5 rules on vitals/position only.
}

class PlayerProgressionStore {
public:
    void open(std::string root) { store_ = std::make_unique<quark::FileStore>(std::move(root)); }
    [[nodiscard]] bool is_open() const noexcept { return store_ != nullptr; }

    void save(AccountId account, const PlayerProgression& p) {
        if (store_ == nullptr || account == kNoAccount) return;
        const quark::ActorId id{kProgressionType, account};
        (void)quark::save_snapshot<PlayerProgression>(*store_, id, fence_of(id), /*through_seq=*/0,
                                                       p);
    }

    [[nodiscard]] std::optional<PlayerProgression> load(AccountId account) {
        if (store_ == nullptr || account == kNoAccount) return std::nullopt;
        const quark::ActorId id{kProgressionType, account};
        auto rec = quark::load_snapshot<PlayerProgression>(*store_, id);
        if (!rec.has_value() || !rec->has_value()) return std::nullopt;
        return (*rec)->state;
    }

private:
    [[nodiscard]] quark::FenceToken fence_of(quark::ActorId id) {
        auto it = fences_.find(id.key);
        if (it != fences_.end()) return it->second;
        const quark::FenceToken f = store_->acquire_fence(id);
        fences_.emplace(id.key, f);
        return f;
    }

    static constexpr quark::TypeKey kProgressionType{0x5000'0001};
    std::unique_ptr<quark::FileStore> store_;
    std::unordered_map<std::uint64_t, quark::FenceToken> fences_;
};

// --- §6: world overlay state — Persistent<EventSourced, Sync> equivalent -----------------------
//
// Reuses the shipped PlaceBuilding/UpgradeBuilding/PlantCrop/TillGround/HarvestAt *shapes*
// verbatim (§6.1) via one flat envelope struct rather than a tagged union of the five original
// message types — this engine's describe/wire codec (016) has no std::variant support to lean on,
// and a flat superset of fields tagged by `kind` is the same "already exactly a discrete,
// replayable world mutation" the RFC's own text describes, just POD-shaped instead of union-shaped.
enum class OverlayEventKind : std::uint8_t {
    kPlantCrop = 0,
    kPlaceBuilding = 1,
    kUpgradeBuilding = 2,
    kTillGround = 3,
    kHarvestAt = 4,
};

struct ChunkMutationEvent {
    OverlayEventKind kind = OverlayEventKind::kPlantCrop;
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    std::uint8_t sub_kind = 0;  // CropKind (kPlantCrop) or BuildKind (kPlaceBuilding); else unused
    std::int64_t now_ms = 0;    // kPlantCrop only
    std::uint64_t player = 0;
};
QUARK_SERIALIZE(ChunkMutationEvent, (1, kind), (2, tx), (3, ty), (4, sub_kind), (5, now_ms),
                (6, player))

// A tilled tile that has no struct of its own upstream (TillGround writes Terrain::kDirt directly
// into ChunkActor's terrain_[] cache, §6.1) — this is the durable fact worth logging/snapshotting.
struct TilledTile {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
};
QUARK_SERIALIZE(TilledTile, (1, tx), (2, ty))

QUARK_SERIALIZE(Crop, (1, tx), (2, ty), (3, kind), (4, stage), (5, planted_ms), (6, ripe_ms))
QUARK_SERIALIZE(Building, (1, tx), (2, ty), (3, kind), (4, hp), (5, cooldown), (6, level))

// The compaction-checkpoint / recovery-fold state (§6.3 point 2, §6.4). Building::hp is always
// reconstructed at max_hp_of(kind, level) by the fold below, never a damaged value — §6.6's
// ruling that combat damage in progress is deliberately not part of the durable event vocabulary,
// automatic here simply because no "damage a building" event is ever staged.
struct ChunkOverlaySnapshot {
    std::vector<Crop> crops;
    std::vector<Building> buildings;
    std::vector<TilledTile> tilled;
};
QUARK_SERIALIZE(ChunkOverlaySnapshot, (1, crops), (2, buildings), (3, tilled))

// The deterministic fold (§6.4) — literally the same logic chunk_actor.hpp's own
// handle(PlaceBuilding)/handle(TillGround)/etc. already contain, run once per recovered event
// instead of once per live message.
inline void apply(ChunkOverlaySnapshot& s, const ChunkMutationEvent& e) {
    switch (e.kind) {
        case OverlayEventKind::kPlantCrop: {
            std::erase_if(s.crops, [&](const Crop& c) { return c.tx == e.tx && c.ty == e.ty; });
            Crop c{};
            c.tx = e.tx;
            c.ty = e.ty;
            c.kind = static_cast<CropKind>(e.sub_kind);
            c.stage = 0;
            c.planted_ms = e.now_ms;
            c.ripe_ms = e.now_ms + grow_ms_of(c.kind);
            s.crops.push_back(c);
            break;
        }
        case OverlayEventKind::kHarvestAt: {
            std::erase_if(s.crops, [&](const Crop& c) { return c.tx == e.tx && c.ty == e.ty; });
            break;
        }
        case OverlayEventKind::kPlaceBuilding: {
            Building b{};
            b.tx = e.tx;
            b.ty = e.ty;
            b.kind = static_cast<BuildKind>(e.sub_kind);
            b.level = 1;
            b.hp = max_hp_of(b.kind, 1);
            s.buildings.push_back(b);
            break;
        }
        case OverlayEventKind::kUpgradeBuilding: {
            for (Building& b : s.buildings) {
                if (b.tx != e.tx || b.ty != e.ty) continue;
                if (b.level >= kMaxLevel) break;
                const std::int16_t before = max_hp_of(b.kind, b.level);
                ++b.level;
                const std::int16_t after = max_hp_of(b.kind, b.level);
                b.hp = static_cast<std::int16_t>(b.hp + (after - before));
                break;
            }
            break;
        }
        case OverlayEventKind::kTillGround: {
            bool already = false;
            for (const TilledTile& t : s.tilled) {
                if (t.tx == e.tx && t.ty == e.ty) {
                    already = true;
                    break;
                }
            }
            if (!already) s.tilled.push_back(TilledTile{e.tx, e.ty});
            break;
        }
    }
}

class WorldOverlayStore {
public:
    void open(std::string root) {
        store_ = std::make_unique<quark::FileStore>(root);
        touched_path_ = root + "/touched.dat";
        load_touched();
    }
    [[nodiscard]] bool is_open() const noexcept { return store_ != nullptr; }

    // §6.2: called directly from the same World method that already tells the chunk the mutating
    // message (see file header, point 3) — completes before that method returns, which is what
    // makes this a real Sync commit without a second actor hop.
    void record(ChunkCoord coord, const ChunkMutationEvent& e) {
        if (store_ == nullptr) return;
        const std::uint64_t key = chunk_key(coord);
        mark_touched(key);
        Entry& entry = entry_for(key);
        entry.log->stage(e);
        (void)entry.log->commit();
    }

    // §6.4: replay a persistent-band chunk's durable log (+ latest compaction snapshot, if any)
    // into a ChunkOverlaySnapshot. nullopt for a chunk that was never mutated — "an untouched
    // chunk needs no recovery" (§6.4) is what `touched_` exists to make true in PRACTICE, not
    // just in intent: without it, recovery would lazily open (and thereby create) an empty
    // FileStore entry for every persistent-band chunk in the world on every single bring-up.
    // Must be called before any record() for the same coord (true by construction: World calls
    // this once, for every chunk, immediately after build_chunks() and before the world starts
    // accepting player actions).
    [[nodiscard]] std::optional<ChunkOverlaySnapshot> recover(ChunkCoord coord) {
        if (store_ == nullptr) return std::nullopt;
        const std::uint64_t key = chunk_key(coord);
        if (touched_.find(key) == touched_.end()) return std::nullopt;
        const quark::ActorId id{kOverlayType, key};
        auto rec = quark::recover_event_sourced<ChunkOverlaySnapshot, ChunkMutationEvent>(
            *store_, id, ChunkOverlaySnapshot{}, &apply);
        if (!rec.has_value()) return std::nullopt;
        Entry& entry = entry_for_fresh(key, rec->last_seq + 1);
        (void)entry;
        return rec->state;
    }

    // §6.3 point 2: folds the durable log into a compaction checkpoint so replay length stays
    // bounded. `current` is this chunk's live state; `through_seq` must be the highest sequence
    // actually committed for this chunk (last_committed_seq()) — never ahead of it (the store
    // rejects an over-advanced through_seq, 012's own "never ahead of the appended tail" rule).
    void compact(ChunkCoord coord, const ChunkOverlaySnapshot& current, quark::SeqNo through_seq) {
        if (store_ == nullptr) return;
        const std::uint64_t key = chunk_key(coord);
        if (touched_.find(key) == touched_.end()) return;  // nothing durable to compact yet
        const quark::ActorId id{kOverlayType, key};
        (void)quark::save_snapshot<ChunkOverlaySnapshot>(*store_, id, fence_of(id), through_seq,
                                                          current);
    }

    [[nodiscard]] quark::SeqNo last_committed_seq(ChunkCoord coord) {
        if (store_ == nullptr) return 0;
        const std::uint64_t key = chunk_key(coord);
        if (touched_.find(key) == touched_.end()) return 0;
        return store_->last_seq(quark::ActorId{kOverlayType, key});
    }

    [[nodiscard]] const std::unordered_set<std::uint64_t>& touched() const noexcept {
        return touched_;
    }

private:
    struct Entry {
        quark::FenceToken fence{};
        std::unique_ptr<quark::EventLog<ChunkMutationEvent, quark::FileStore>> log;
    };

    [[nodiscard]] quark::FenceToken fence_of(quark::ActorId id) {
        auto it = fences_.find(id.key);
        if (it != fences_.end()) return it->second;
        const quark::FenceToken f = store_->acquire_fence(id);
        fences_.emplace(id.key, f);
        return f;
    }

    Entry& entry_for(std::uint64_t key) {
        auto it = entries_.find(key);
        if (it != entries_.end()) return *it->second;
        const quark::ActorId id{kOverlayType, key};
        return entry_for_fresh(key, store_->last_seq(id) + 1);
    }

    Entry& entry_for_fresh(std::uint64_t key, quark::SeqNo next_seq) {
        const quark::ActorId id{kOverlayType, key};
        auto e = std::make_unique<Entry>();
        e->fence = fence_of(id);
        e->log = std::make_unique<quark::EventLog<ChunkMutationEvent, quark::FileStore>>(
            *store_, id, e->fence, next_seq);
        auto [it, inserted] = entries_.insert_or_assign(key, std::move(e));
        (void)inserted;
        return *it->second;
    }

    void mark_touched(std::uint64_t key) {
        if (!touched_.insert(key).second) return;
        save_touched();
    }

    // A small flat file of chunk keys ever mutated — see recover()'s own comment for why this
    // exists. Mirrors account.hpp's own "flat array of fixed records" convention.
    void load_touched() {
        std::FILE* f = std::fopen(touched_path_.c_str(), "rb");
        if (f == nullptr) return;
        std::uint64_t key = 0;
        while (std::fread(&key, sizeof key, 1, f) == 1) touched_.insert(key);
        std::fclose(f);
    }
    void save_touched() const {
        std::FILE* f = std::fopen(touched_path_.c_str(), "wb");
        if (f == nullptr) return;
        for (std::uint64_t key : touched_) std::fwrite(&key, sizeof key, 1, f);
        std::fclose(f);
    }

    static constexpr quark::TypeKey kOverlayType{0x5000'0002};
    std::unique_ptr<quark::FileStore> store_;
    std::string touched_path_;
    std::unordered_set<std::uint64_t> touched_;
    std::unordered_map<std::uint64_t, quark::FenceToken> fences_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Entry>> entries_;
};

// §6.3: two independent cadences, in ticks (kTicksPerSecond=10, tiles.hpp) converted to ms by the
// caller — first-guess numbers computed against the stated ≤60s budget, not measured against a
// running leader under real load (the same caveat RFC-014 Open Question 6 already carries for its
// own first-guess timers).
inline constexpr std::int64_t kProgressionCheckpointIntervalMs = 30'000;  // 300 ticks — half the 60s bar
inline constexpr std::int64_t kOverlaySnapshotIntervalMs = 180'000;       // 1800 ticks — 3 min

}  // namespace mmo
