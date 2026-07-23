// PlayerActor — one player's authoritative state: identity, position, vitals, skills, inventory.
//
// TRUST TIER A. This actor carries `Require<Trusted>` in its placement policy, so once the world is
// distributed it can only ever be placed on a node that advertises the `trusted` capability flag.
// Player machines will not advertise it. That is the whole anti-cheat posture for high-value state,
// and it is expressed as a *template parameter* — a node without the flag is not merely discouraged
// from hosting an inventory, it is ineligible, and the check happens in placement resolution rather
// than in game code that could be forgotten.
//
// The inventory is why this matters: a `SpendItems` ask is a check-and-debit. Because the actor is
// `Sequential`, that pair is atomic without a lock — no two concurrent build requests can both pass
// the affordability check against the same wood. Moving this actor onto a player's machine would
// hand that player the authority to answer "yes, I could afford it" to themselves.
//
// COMBAT USES EXACTLY THE SAME SHAPE. `PlanAttack` is check-and-debit against stamina or mana, and
// it answers with the damage AND the position the swing happens at. The client is never asked where
// it is or how hard it hits; it asks permission and is told. One ask per swing is affordable
// precisely because a swing is a discrete event — the thing that could not be an ask is a creature
// reading the player's position every tick, which is why that goes the other way as a beacon.
//
// ONE ACTOR PER SESSION SLOT, KEYED. `PlayerActor` used to be a singleton at key 1. It is now keyed
// by `player_key(slot)` and every verb in the game carries that key, because the alternative was to
// write combat, inventory and crafting against a singleton and rewrite all three at P6 (ROADMAP
// principle 2). The roster is fixed at bring-up rather than grown on demand, and that is not a
// design preference: `Engine::register_activation` is documented cold-only — "safe, single-threaded
// before start()" — so an actor cannot appear while the world is running. A login therefore BINDS
// an account to a pre-registered slot. Real servers have connection slots for the same reason.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/capabilities.hpp"
#include "quark/core/placement_policies.hpp"

#include "world/protocol.hpp"
#include "world/snapshot.hpp"
#include "world/tiles.hpp"

namespace mmo {

using quark::Ask;
using quark::HasFlag;
using quark::Protocol;
using quark::Require;

// The capability a node must advertise to be allowed to host tier-A state.
using Trusted = HasFlag<"trusted">;

struct PlayerActor : quark::Actor<PlayerActor, quark::Sequential, quark::Priority<0>,
                                  quark::Placement<quark::HashById, Require<Trusted>>> {
    using protocol =
        Protocol<Tick, MoveIntent, Teleport, GrantItems, HurtPlayer, GrantVitals, GrantXp,
                 SetRespawn, BindAccount, SetMounted, Ask<SpendItems, bool>, Ask<GetPlayer, PlayerView>,
                 Ask<PlanAttack, AttackPlan>, Ask<UseAbility, AbilityPlan>>;

    // Set once at bring-up, before the engine starts.
    std::uint64_t id = 0;
    std::uint16_t map = 0;
    PlayerBus* bus = nullptr;  // where this actor publishes its view (see snapshot.hpp)
    int slot = 0;

    // ================================ handlers ====================================================

    // The world clock, fanned by the director exactly as it is to a chunk. Everything here is time
    // passing rather than something happening: regeneration, status decay, the respawn countdown.
    void handle(const Tick& t) noexcept {
        tick_ = t.tick;
        world_ms_ = t.world_ms;
        if (account_ == 0) return;  // unbound slot: inert, and not published

        // Ability cooldowns. This is the first per-slot timer the sim has ever held — the basic
        // verbs are rationed by stamina alone, but an ability's whole shape is that it is powerful
        // AND scarce, and scarcity is a cooldown. Ticked while dead too, so it is not a free reset.
        for (std::uint16_t& cd : ability_cd_) {
            if (cd > 0) --cd;
        }

        if (dead_ticks_ > 0) {
            if (--dead_ticks_ == 0) respawn();
            publish();
            return;
        }

        stamina_ = std::min<std::int16_t>(kPlayerMaxStamina,
                                          static_cast<std::int16_t>(stamina_ + kStaminaRegen));
        mana_ = std::min<std::int16_t>(kPlayerMaxMana, static_cast<std::int16_t>(mana_ + kManaRegen));

        // Health comes back only once nothing has hit you for a while. That single condition is
        // what makes a fight a decision rather than an attrition sum: you can always leave, and
        // leaving is how you heal.
        if (world_ms_ - last_hurt_ms_ > kCombatCooldownMs && hp_ < kPlayerMaxHp) {
            if (world_ms_ - last_regen_ms_ >= kHealthRegenMs) {
                last_regen_ms_ = world_ms_;
                ++hp_;
            }
        }
        publish();
    }

    void handle(const BindAccount& b) noexcept {
        account_ = b.account;
        x_ = static_cast<float>(b.spawn_tx) + 0.5f;
        y_ = static_cast<float>(b.spawn_ty) + 0.5f;
        respawn_tx_ = b.spawn_tx;
        respawn_ty_ = b.spawn_ty;
        hp_ = kPlayerMaxHp;
        mana_ = kPlayerMaxMana;
        stamina_ = kPlayerMaxStamina;
        items_[static_cast<int>(ItemKind::kWood)] = b.wood;
        items_[static_cast<int>(ItemKind::kStone)] = b.stone;
        items_[static_cast<int>(ItemKind::kSeed)] = b.seed;
        publish();
    }

    void handle(const MoveIntent& m) noexcept {
        // This used to clamp to the map and nothing else, on the argument that terrain is
        // chunk-owned state and a tier-A actor should hold none of it. The argument was about
        // CHUNK state and it still holds — but `terrain_of` is not chunk state. It is a free
        // function over the seed and the published overlay, the same one the flow field calls
        // thousands of times per rebuild, and it costs a hash and a branch.
        //
        // What changed is that something now depends on the answer. A palisade the player walks
        // straight through is a painting of a palisade, and a doorway is only a doorway if the wall
        // beside it is not one. So the player is stopped here, where the authority is.
        //
        // The axes are resolved SEPARATELY, which is the difference between sliding along a wall
        // and sticking to it. Testing the diagonal as one move means a player walking into a wall
        // at any angle stops dead rather than sliding along it, and with a village made of
        // rectangles that is most of the time.
        if (account_ == 0 || dead_ticks_ > 0) return;
        const float nx = std::clamp(x_ + m.dx, 0.0f, static_cast<float>(kMapTiles) - 1.0f);
        const float ny = std::clamp(y_ + m.dy, 0.0f, static_cast<float>(kMapTiles) - 1.0f);
        if (passable(nx, y_)) x_ = nx;
        if (passable(x_, ny)) y_ = ny;
        if (m.dx != 0.0f || m.dy != 0.0f) {
            facing_ = facing_of(m.dx, m.dy);
            ++steps_;  // drives the walk animation; the renderer never sees raw positions over time
        }
        step_through_doors();
        publish();
    }

    // Somewhere else, right now. The door check still runs — arriving on a doorway by teleport is
    // the same arrival as walking onto one — but `last_tile_` is stamped first when the caller has
    // already decided the destination map, so a teleport into a room does not bounce straight back
    // out of it.
    void handle(const Teleport& t) noexcept {
        if (account_ == 0) return;
        map = t.map;
        x_ = std::clamp(t.x, 0.0f, static_cast<float>(kMapTiles) - 1.0f);
        y_ = std::clamp(t.y, 0.0f, static_cast<float>(kMapTiles) - 1.0f);
        step_through_doors();
        publish();
    }

    void handle(const GrantItems& g) noexcept {
        const int k = static_cast<int>(g.kind);
        if (k >= 0 && k < kItemKinds) items_[k] += g.count;
        publish();
    }

    void handle(const HurtPlayer& h) noexcept {
        if (account_ == 0 || dead_ticks_ > 0 || h.amount <= 0) return;
        hp_ = static_cast<std::int16_t>(std::max(0, hp_ - h.amount));
        last_hurt_ms_ = world_ms_;
        if (hp_ == 0) {
            dead_ticks_ = kRespawnTicks;
            ++deaths_;
            mounted_ = false;
        }
        publish();
    }

    void handle(const GrantVitals& g) noexcept {
        hp_ = std::clamp<std::int16_t>(static_cast<std::int16_t>(hp_ + g.hp), 0, kPlayerMaxHp);
        mana_ = std::clamp<std::int16_t>(static_cast<std::int16_t>(mana_ + g.mana), 0, kPlayerMaxMana);
        stamina_ = std::clamp<std::int16_t>(static_cast<std::int16_t>(stamina_ + g.stamina), 0,
                                            kPlayerMaxStamina);
        publish();
    }

    // You level what you use. The cap is enforced HERE rather than at the point of spending,
    // because it is a property of the character and this actor is the only writer of one.
    void handle(const GrantXp& g) noexcept {
        const int s = static_cast<int>(g.skill);
        if (s < 0 || s >= kSkillCount || g.amount == 0) return;
        xp_[s] += g.amount;
        while (level_[s] < kMaxSkillLevel && total_levels() < kSkillPointCap &&
               xp_[s] >= xp_for_level(level_[s])) {
            xp_[s] -= xp_for_level(level_[s]);
            ++level_[s];
        }
        publish();
    }

    void handle(const SetRespawn& r) noexcept {
        respawn_tx_ = r.tx;
        respawn_ty_ = r.ty;
    }

    void handle(const SetMounted& m) noexcept {
        if (dead_ticks_ > 0) return;
        mounted_ = m.mounted;
        publish();
    }

    // Check-and-debit in one sequential handler: either the caller gets `true` AND the items are
    // gone, or it gets `false` AND nothing changed. No partial state is observable.
    void handle(const Ask<SpendItems, bool>& m) noexcept {
        const SpendItems& s = m.query;
        const int k = static_cast<int>(s.kind);
        if (k < 0 || k >= kItemKinds || items_[k] < s.count) {
            m.respond(false);
            return;
        }
        items_[k] -= s.count;
        publish();
        m.respond(true);
    }

    // "May I swing, and how hard?" — the combat counterpart of SpendItems, and the reason the
    // client cannot decide its own damage. Note that the answer carries the position and facing
    // too: the caller does not get to say where the swing happened either.
    void handle(const Ask<PlanAttack, AttackPlan>& m) noexcept {
        AttackPlan p{};
        p.x = x_;
        p.y = y_;
        p.map = map;
        p.facing = facing_;
        if (account_ == 0 || dead_ticks_ > 0 || mounted_) {
            m.respond(p);  // ok stays false — you cannot fight from the saddle
            return;
        }
        switch (m.query.kind) {
            case AttackKind::kLight:
                if (stamina_ < kSwingStamina) break;
                stamina_ = static_cast<std::int16_t>(stamina_ - kSwingStamina);
                p.ok = true;
                p.damage = scaled(kBaseMeleeDamage, Skill::kMelee);
                p.reach = kMeleeReach;
                break;
            case AttackKind::kHeavy:
                if (stamina_ < kHeavyStamina) break;
                stamina_ = static_cast<std::int16_t>(stamina_ - kHeavyStamina);
                p.ok = true;
                p.damage = static_cast<std::int16_t>(scaled(kBaseMeleeDamage, Skill::kMelee) * 2);
                p.reach = kHeavyReach;
                break;
            case AttackKind::kShoot:
                if (stamina_ < kShootStamina) break;
                stamina_ = static_cast<std::int16_t>(stamina_ - kShootStamina);
                p.ok = true;
                p.damage = scaled(kBaseRangedDamage, Skill::kRanged);
                p.reach = 0.0f;
                break;
            case AttackKind::kCast:
                if (mana_ < kSpellMana || m.query.element == Element::kNone) break;
                mana_ = static_cast<std::int16_t>(mana_ - kSpellMana);
                p.ok = true;
                p.damage = scaled(kBaseSpellDamage, Skill::kMagic);
                p.reach = kSpellRadius;
                p.element = m.query.element;
                break;
        }
        if (p.ok) publish();
        m.respond(p);
    }

    // "May I use slot A/B, and how does it land?" The trusted counterpart of every check the client
    // must not make for itself: is the school high enough to have this ability at all, is it off
    // cooldown, can it be paid for. Same check-and-debit atomicity as PlanAttack and SpendItems —
    // either the caller gets `ok` AND the vital is spent AND the cooldown is set, or it gets a reason
    // and nothing moved.
    void handle(const Ask<UseAbility, AbilityPlan>& m) noexcept {
        AbilityPlan p{};
        p.x = x_;
        p.y = y_;
        p.map = map;
        p.facing = facing_;
        p.aim_x = m.query.aim_x;
        p.aim_y = m.query.aim_y;
        if (account_ == 0 || dead_ticks_ > 0 || mounted_) {
            p.reason = AbilityReject::kUnavailable;  // you cannot use an ability dead or from the saddle
            m.respond(p);
            return;
        }
        const int slot = (m.query.slot < kAbilitySlots) ? static_cast<int>(m.query.slot) : 0;
        const AbilityId id = equipped_ability(level_, slot);
        p.ability = id;
        if (id == AbilityId::kCount) {
            p.reason = AbilityReject::kLocked;  // no fighting school has reached level 5 yet
            m.respond(p);
            return;
        }
        const AbilityDef def = ability_def(id);
        if (level_[static_cast<int>(def.school)] < def.unlock_level) {
            p.reason = AbilityReject::kLocked;
            m.respond(p);
            return;
        }
        if (ability_cd_[static_cast<int>(id)] > 0) {
            p.reason = AbilityReject::kCooldown;
            m.respond(p);
            return;
        }
        if (def.cost_kind == AbilityCost::kStamina) {
            if (stamina_ < def.cost) {
                p.reason = AbilityReject::kResource;
                m.respond(p);
                return;
            }
            stamina_ = static_cast<std::int16_t>(stamina_ - def.cost);
        } else {
            if (mana_ < def.cost) {
                p.reason = AbilityReject::kResource;
                m.respond(p);
                return;
            }
            mana_ = static_cast<std::int16_t>(mana_ - def.cost);
        }
        ability_cd_[static_cast<int>(id)] = def.cooldown;
        p.ok = true;
        p.reason = AbilityReject::kOk;
        // The damage the chunk will apply, scaled here so the untrusted side never computes how hard
        // the player hits — exactly as PlanAttack does. Zones carry no direct damage.
        if (def.kind == AbilityKind::kStrike) {
            const std::int16_t base =
                (def.school == Skill::kMelee) ? kBaseMeleeDamage : kBaseSpellDamage;
            p.damage = static_cast<std::int16_t>(static_cast<float>(scaled(base, def.school)) *
                                                 def.damage_scale);
        } else if (def.kind == AbilityKind::kVolley) {
            p.damage = scaled(kBaseRangedDamage, Skill::kRanged);
        }
        // Nova imprints the caster's currently-selected element; every other ability ignores it.
        p.element = def.applies_element ? m.query.element : Element::kNone;
        publish();
        m.respond(p);
    }

    void handle(const Ask<GetPlayer, PlayerView>& m) noexcept { m.respond(view()); }

    // ================================ bring-up ====================================================

    [[nodiscard]] PlayerView view() const noexcept {
        PlayerView v{};
        v.id = id;
        v.account = account_;
        v.map = map;
        v.x = x_;
        v.y = y_;
        v.hp = hp_;
        v.max_hp = kPlayerMaxHp;
        v.mana = mana_;
        v.stamina = stamina_;
        v.facing = facing_;
        v.steps = steps_;
        v.dead_ticks = dead_ticks_;
        v.deaths = deaths_;
        v.mounted = mounted_;
        v.respawn_tx = respawn_tx_;
        v.respawn_ty = respawn_ty_;
        for (int i = 0; i < kItemKinds; ++i) v.items[i] = items_[i];
        for (int i = 0; i < kSkillCount; ++i) {
            v.skill_level[i] = level_[i];
            v.skill_xp[i] = xp_[i];
            v.skill_next[i] = xp_for_level(level_[i]);
        }
        // The fixed loadout, resolved from levels, plus each slot's remaining cooldown — everything
        // the HUD draws a slot from without asking. A slot whose school is still too low reports its
        // intended ability (so the greyed icon is the right one) and zero cooldown.
        for (int s = 0; s < kAbilitySlots; ++s) {
            const AbilityId id = equipped_ability(level_, s);
            v.ability[s] = id;
            v.ability_cd[s] = (id == AbilityId::kCount) ? 0 : ability_cd_[static_cast<int>(id)];
        }
        return v;
    }

    // Publish once at bring-up so an unbound slot has a view to read rather than a null.
    void publish_now() noexcept { publish(); }

private:
    // May the player's CENTRE stand on this point? One tile, not a box: the sprite is a tile wide,
    // and a box test with the same footprint cannot pass through a one-tile doorway without either
    // a smaller box (which then clips walls) or a special case for doors (which is the same bug
    // written twice).
    [[nodiscard]] bool passable(float px, float py) const noexcept {
        return is_walkable(terrain_of(kWorldSeed, map, static_cast<int>(px), static_cast<int>(py)));
    }

    // Doors. Fired on ARRIVAL — the tile has to change — for a reason that has nothing to do with
    // efficiency: a portal that fires while you are standing on it sends you through, and the tile
    // you land on is a portal back, so you flicker between two maps at ten hertz forever. Coming
    // out onto the doorstep rather than onto the doorway already breaks that loop, and this is the
    // belt to its braces.
    void step_through_doors() noexcept {
        const auto tx = static_cast<int>(x_);
        const auto ty = static_cast<int>(y_);
        const std::uint32_t here = tile_key(tx, ty);
        if (here == last_tile_) return;
        last_tile_ = here;
        const Portal p = portal_at(map, tx, ty);
        if (!p.valid) return;
        map = p.map;
        x_ = static_cast<float>(p.tx) + 0.5f;
        y_ = static_cast<float>(p.ty) + 0.5f;
        last_tile_ = tile_key(p.tx, p.ty);
        // Facing is set deliberately rather than left alone: you walk INTO a door going up and out
        // of one going down, so the sprite would otherwise arrive with its back to the room.
        facing_ = (p.map == kInterior) ? Facing::kUp : Facing::kDown;
    }

    [[nodiscard]] std::uint16_t total_levels() const noexcept {
        std::uint16_t n = 0;
        for (int i = 0; i < kSkillCount; ++i) n = static_cast<std::uint16_t>(n + level_[i]);
        return n;
    }

    [[nodiscard]] std::int16_t scaled(std::int16_t base, Skill s) const noexcept {
        return static_cast<std::int16_t>(static_cast<float>(base) *
                                         skill_scale(level_[static_cast<int>(s)]));
    }

    // Death is cheap on purpose. You wake at your hearth with nothing taken from you — no gear
    // dropped, no XP lost, no corpse run. This game's default is chill (GAME.md §0), and a death
    // penalty is the most reliable way to turn exploring into hoarding: players who fear losing a
    // backpack stop going anywhere with it. What death costs is the walk back, and out past the
    // second ring that is quite enough.
    void respawn() noexcept {
        x_ = static_cast<float>(respawn_tx_) + 0.5f;
        y_ = static_cast<float>(respawn_ty_) + 0.5f;
        hp_ = kPlayerMaxHp;
        mana_ = kPlayerMaxMana;
        stamina_ = kPlayerMaxStamina;
        last_hurt_ms_ = world_ms_;
    }

    // The tier-A → tier-A channel. Written by this actor only, read by the director (to fan
    // beacons) and by the renderer (to draw). It is the same lossy published-snapshot contract as
    // `SnapshotBus`, and it is legitimate for the same reason: the director and every PlayerActor
    // both carry `Require<Trusted>`, so they are co-located on the leader by construction. What
    // crosses to an untrusted chunk is a real message (`PlayerBeacon`), never this pointer.
    void publish() noexcept {
        if (bus == nullptr) return;
        bus->publish(slot, std::make_shared<const PlayerView>(view()));
    }

    std::uint32_t account_ = 0;  // 0 = this slot is not logged in
    float x_ = 0.0f;
    float y_ = 0.0f;
    std::int16_t hp_ = kPlayerMaxHp;
    std::int16_t mana_ = kPlayerMaxMana;
    std::int16_t stamina_ = kPlayerMaxStamina;
    Facing facing_ = Facing::kDown;
    std::uint32_t steps_ = 0;
    std::uint32_t last_tile_ = 0xFFFF'FFFFu;  // the tile a door was last tested against
    std::uint16_t dead_ticks_ = 0;
    std::uint32_t deaths_ = 0;
    bool mounted_ = false;
    std::uint16_t respawn_tx_ = 0;
    std::uint16_t respawn_ty_ = 0;
    std::int64_t last_hurt_ms_ = -kCombatCooldownMs;
    std::int64_t last_regen_ms_ = 0;
    std::int64_t world_ms_ = 0;
    std::uint64_t tick_ = 0;
    std::int32_t items_[kItemKinds] = {};
    std::uint8_t level_[kSkillCount] = {};
    std::uint32_t xp_[kSkillCount] = {};
    // Per-ability cooldown, in ticks, keyed by AbilityId. Keyed by the ABILITY rather than the slot
    // so the timer belongs to the move and survives a future loadout-picker unchanged; with the
    // fixed F1a loadout each slot maps to a distinct ability, so this reads identically to per-slot.
    std::uint16_t ability_cd_[kAbilityCount] = {};
};

// Build costs, consulted before a PlaceBuilding is issued.
struct BuildCost {
    ItemKind kind;
    std::int32_t count;
};

[[nodiscard]] inline constexpr BuildCost cost_of(BuildKind k) noexcept {
    switch (k) {
        case BuildKind::kPlot: return {ItemKind::kWood, 2};
        case BuildKind::kHearth: return {ItemKind::kStone, 20};
        case BuildKind::kCount: break;
    }
    return {ItemKind::kWood, 1};
}

// Upgrading level L -> L+1 costs the build price scaled by the level reached: cheap to reinforce a
// plot, expensive to max a hearth.
[[nodiscard]] inline constexpr BuildCost upgrade_cost_of(BuildKind k, std::uint8_t level) noexcept {
    const BuildCost base = cost_of(k);
    return BuildCost{base.kind, base.count * 2 * (level < 1 ? 1 : level)};
}

inline constexpr std::int32_t kTillCost = 1;  // wood, per tile reclaimed

}  // namespace mmo
