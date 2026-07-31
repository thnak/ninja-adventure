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
                 RespecSkill, GrantEssence, SetRespawn, BindAccount, Unbind, Rebind, SetMounted,
                 SetInstanceReturn, RestoreProgression, Ask<SpendItems, bool>,
                 Ask<GetPlayer, PlayerView>, Ask<PlanAttack, AttackPlan>,
                 Ask<UseAbility, AbilityPlan>>;

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

        // RFC-001 Section 2 (T2/T3/T4): while a head is mid-Cast/Channel, its clock is the caster's
        // own tick — this is what makes head phases progress "on the caster's ticks" per Section 4's
        // LOD rule (a player's actor always ticks, so a Cast never stalls). No shipped ability
        // reaches this branch (every `cast_ticks == 0` collapses inside the `Ask<UseAbility>` call
        // that starts it), but it is the same `advance_head()` that call uses, exercised here for
        // whatever future content actually holds a Cast open across a tick boundary.
        if (head_.phase == AbilityPhase::kCast || head_.phase == AbilityPhase::kChannel) {
            const AbilityDef def = ability_def(static_cast<AbilityId>(head_.ability));
            advance_head(head_, def.cast_ticks, /*has_channel=*/false, /*channel_max_ticks=*/0);
            if (head_.phase == AbilityPhase::kRelease) {
                // T5/T6: cooldown starts, head clears. Dispatching the resolved payload to chunks
                // has no consumer yet when Release is reached HERE rather than inside the admission
                // call — `World` only drains a payload from the `AbilityPlan` it gets back from that
                // one ask (see `world.hpp::use_ability`'s `phase == kIdle` gate). Wiring a second,
                // asynchronous hand-off is deliberately left for whichever RFC ships the first
                // ability with `cast_ticks > 0`, since nothing today can exercise it end-to-end.
                ready_at_tick_[head_.ability] = tick_ + def.cooldown;
                head_ = AbilityHead{};
            }
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

    // RFC-014 §6: a connection dropped with no kReturnPortal use. Reverts the session slot to
    // unbound WITHOUT touching position — the opposite of BindAccount's unconditional respawn/
    // refill, and the whole point: a disconnected player's `x_`/`y_`/`map` are left exactly where
    // they were, so a same-account reconnect (World::login()'s resume branch) can put them straight
    // back rather than at the overworld spawn with reset resources. No client-facing disconnect
    // DETECTOR exists yet (RFC-015's territory) — this handler is the data-level mechanism a future
    // one would call, exercised directly by tests/tools today.
    void handle(const Unbind&) noexcept {
        account_ = 0;
        publish();
    }

    // RFC-014 §6 Reconnect: re-arms the slot for `b.account` without touching `x_`/`y_`/`map`/vitals
    // — the player materializes exactly where they logged out, inside the still-live session.
    void handle(const Rebind& b) noexcept {
        account_ = b.account;
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
        // RFC-001 Section 5 (T12, voluntary cancel): moving during a Cast cancels it when the
        // ability says so (`move_cancels`, true for every shipped ability today). Unreached in
        // practice while every ability collapses Cast->Release inside its own admission call, but a
        // real cast-time ability would be rooted right up until this fires.
        if (head_.phase == AbilityPhase::kCast && (m.dx != 0.0f || m.dy != 0.0f)) {
            const AbilityDef def = ability_def(static_cast<AbilityId>(head_.ability));
            if (def.move_cancels) interrupt_head();
        }
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
            // RFC-001 Section 5 item 1 — death is the highest-priority interrupt and the simplest:
            // no refund bookkeeping, no stagger. The caster is gone and respawns with a clean slate
            // regardless (see `respawn()`), so there is nothing for `apply_interrupt` to compute.
            head_ = AbilityHead{};
            dead_ticks_ = kRespawnTicks;
            ++deaths_;
            mounted_ = false;
            // RFC-013 §6.1: decided once, at the moment of death, off `PlayerActor`'s own `map`
            // field — no cross-actor lookup on the damage-resolution path. Consumed and cleared by
            // respawn() at the end of the dead_ticks_ countdown.
            pending_eject_ = map_id_instanced(map);
        } else if (head_.phase == AbilityPhase::kCast &&
                   static_cast<float>(h.amount) >= kCastPoiseFrac * static_cast<float>(kPlayerMaxHp)) {
            // Section 5 item 3 — poise-break: a single hit hard enough breaks a Cast outright.
            interrupt_head();
        } else if (head_.phase == AbilityPhase::kChannel &&
                   static_cast<float>(h.amount) >=
                       kChannelPoiseFrac * static_cast<float>(kPlayerMaxHp)) {
            // Channel is squishier by design — it is the player-facing answer to "the boss is
            // charging something big: hit it hard now." Unreached today (no shipped ability
            // channels) but real: proven by ability_pipeline.hpp's own interrupt tests.
            interrupt_head();
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
    //
    // RFC-019 §5.1: a branch that can no longer convert — fully maxed, or blocked by the global
    // 34-point cap — drops further grants instead of banking them forever. Left unbounded, that
    // overflow is exactly what §5.6's respec formula could otherwise launder into a free refund
    // (farm a maxed branch, normally a no-op, then respec it for points it never legitimately
    // earned). The one sanctioned exception is the Essence gate (§5.7) below, which lets banked XP
    // sit at or above threshold ON PURPOSE while it waits — that state is reached via `convert_xp`,
    // never via this early-out.
    void handle(const GrantXp& g) noexcept {
        const int s = static_cast<int>(g.skill);
        if (s < 0 || s >= kSkillCount || g.amount == 0) return;
        if (level_[s] >= kMaxSkillLevel) return;      // fully maxed: drop, not bank
        if (total_levels() >= kSkillPointCap) return;  // blocked by the global cap: drop, not bank
        xp_[s] += g.amount;
        convert_xp(s);
        publish();
    }

    // RFC-019 §5.6: respec, at the player's own Hearth. One atomic action — no intermediate
    // spendable balance, no separate currency. `from`'s committed value (xp_to_reach of its level)
    // is priced using ONLY the level actually reached; any banked, uncommitted xp_[from] is
    // discarded outright, which is what keeps GrantXp's overflow-closure above meaningful (farming a
    // blocked branch for banked overflow gains nothing here either). 75% of the committed value is
    // granted into `to` through the same convert_xp path any other grant uses — capped by `to`'s own
    // ceiling and the global cap, any portion that doesn't fit simply forfeit. Resetting `from` also
    // clears its Essence-gate progress: the levels that progress unlocked are gone, so re-earning
    // them later re-pays the gate, the same as any other player who has never reached it.
    void handle(const RespecSkill& r) noexcept {
        if (account_ == 0 || dead_ticks_ > 0) return;
        const int from = static_cast<int>(r.from);
        const int to = static_cast<int>(r.to);
        if (from < 0 || from >= kSkillCount || to < 0 || to >= kSkillCount) return;
        if (level_[from] == 0) return;  // nothing invested, nothing to reset
        // The Hearth this player set (SetRespawn, chunk_actor.hpp) is always on the overworld —
        // respawn()'s own fresh-spawn branch assumes exactly that. "At your own Hearth" is checked
        // against that same point, so no new building or cross-actor query is needed.
        if (map != kOverworld) return;
        const float ddx = x_ - (static_cast<float>(respawn_tx_) + 0.5f);
        const float ddy = y_ - (static_cast<float>(respawn_ty_) + 0.5f);
        if (ddx * ddx + ddy * ddy > kRespecRadiusTiles * kRespecRadiusTiles) return;

        const std::uint32_t committed = xp_to_reach(level_[from]);
        level_[from] = 0;
        xp_[from] = 0;
        essence_paid_[from] = 0;
        xp_[to] += static_cast<std::uint32_t>(static_cast<std::uint64_t>(committed) *
                                              kRespecRefundPm / 1000u);
        convert_xp(to);
        publish();
    }

    // RFC-019 §5.7: one unit of Essence spent against `skill`'s Tier IV gate. Re-attempts the
    // level-up loop immediately in case XP was already banked past the threshold, waiting on
    // exactly this.
    void handle(const GrantEssence& g) noexcept {
        const int s = static_cast<int>(g.skill);
        if (s < 0 || s >= kSkillCount) return;
        essence_paid_[s] = static_cast<std::uint8_t>(
            std::min<int>(kEssenceGateTotal, essence_paid_[s] + g.amount));
        convert_xp(s);
        publish();
    }

    void handle(const SetRespawn& r) noexcept {
        respawn_tx_ = r.tx;
        respawn_ty_ = r.ty;
    }

    // RFC-013 §6.2: caches a MapSession's return coordinates so death-time ejection never needs a
    // cross-actor query. A zero-default (never sent, or sent with an all-zero triple) is treated by
    // respawn() as "no sane position to resume at" and falls back to the bound hearth point (§7).
    void handle(const SetInstanceReturn& r) noexcept {
        instance_return_map_ = r.map;
        instance_return_x_ = r.x;
        instance_return_y_ = r.y;
    }

    // RFC-016 §4/§7: BindAccount's counterpart for a returning account — World::login() sends this
    // instead of BindAccount whenever a saved PlayerProgression exists for the account. Restores
    // every persisted field directly rather than the fixed starter pack; §5's already-respawned
    // ruling for a checkpoint with hp<=0 is applied by the caller (world.hpp) before this arrives,
    // so `r.hp` here is never <= 0 in practice.
    void handle(const RestoreProgression& r) noexcept {
        account_ = r.account;
        map = r.map;
        x_ = r.x;
        y_ = r.y;
        hp_ = r.hp;
        mana_ = r.mana;
        stamina_ = r.stamina;
        deaths_ = r.deaths;
        respawn_tx_ = r.respawn_tx;
        respawn_ty_ = r.respawn_ty;
        instance_return_map_ = r.return_map;
        instance_return_x_ = r.return_x;
        instance_return_y_ = r.return_y;
        for (int i = 0; i < kItemKinds; ++i) items_[i] = r.items[i];
        for (int i = 0; i < kSkillCount; ++i) {
            level_[i] = r.level[i];
            xp_[i] = r.xp[i];
            essence_paid_[i] = r.essence_paid[i];
        }
        publish();
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
        // RFC-001 Invariant I1 — "the basic attack is also gated by I1: you cannot swing mid-cast."
        // Unreached today (every ability collapses Cast->Release before any other message can see
        // `head_` mid-flight) but real: a future cast-time ability roots the caster right up until
        // Release, exactly like a Channel roots it (Section 5's "no" column).
        if (head_.phase != AbilityPhase::kIdle) {
            m.respond(p);  // ok stays false
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
        // Record the tick of a granted swing so the renderer can play the attack animation for this
        // player — including a remote one — off published state alone. Only melee reads as a body
        // swing; a shot or a cast has its own effect and leaves the body idle.
        if (p.ok && (m.query.kind == AttackKind::kLight || m.query.kind == AttackKind::kHeavy)) {
            last_swing_tick_ = tick_;
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
        // RFC-001 Section 5 item 4 — pressing the SAME slot again while its own ability is mid-Cast
        // is a voluntary cancel, not a busy rejection (a different slot while busy still gets
        // kBusy, below, per Invariant I1). Unreached by shipped content (cast_ticks == 0 always
        // resolves before a second ask can arrive) but the right behavior once one channels or
        // winds up.
        if (head_.phase != AbilityPhase::kIdle && static_cast<AbilityId>(head_.ability) == id) {
            interrupt_head();
            p.reason = AbilityReject::kBusy;
            m.respond(p);
            return;
        }
        const bool locked = level_[static_cast<int>(def.school)] < def.unlock_level;
        const bool on_cooldown = ready_at_tick_[static_cast<int>(id)] > tick_;
        const bool lacks_resource = (def.cost_kind == AbilityCost::kStamina) ? stamina_ < def.cost
                                                                             : mana_ < def.cost;
        const bool staggered = tick_ < staggered_until_tick_;
        // kEntity is the only targeting model that can fail admission in v1 (RFC-001 Section 3),
        // and no shipped ability uses it (Open Question Q2) — always valid for now.
        const AbilityReject reason =
            reject_of(/*unavailable=*/false, locked, on_cooldown, lacks_resource, head_.phase,
                     staggered, /*bad_target=*/false);
        if (reason != AbilityReject::kOk) {
            p.reason = reason;
            m.respond(p);
            return;
        }
        if (def.cost_kind == AbilityCost::kStamina) {
            stamina_ = static_cast<std::int16_t>(stamina_ - def.cost);
        } else {
            mana_ = static_cast<std::int16_t>(mana_ - def.cost);
        }
        // T1 — seat the head. Aim/direction resolve per Section 7's targeting models; kSelf always
        // aims at the caster (every striking/zone ability today), kDirection freezes the cursor
        // vector world.hpp's own dispatch already computes from `aim_x/aim_y` — this copy is the
        // state machine's own bookkeeping (I3 "aim freezes"), not a second source of truth for the
        // dispatch math below, which is untouched from before this RFC.
        const float dir_x = (facing_ == Facing::kLeft) ? -1.0f : (facing_ == Facing::kRight ? 1.0f : 0.0f);
        const float dir_y = (facing_ == Facing::kUp) ? -1.0f : (facing_ == Facing::kDown ? 1.0f : 0.0f);
        try_start_cast(head_, static_cast<std::uint16_t>(id), x_, y_, dir_x, dir_y);
        advance_head(head_, def.cast_ticks, /*has_channel=*/false, /*channel_max_ticks=*/0);
        if (head_.phase == AbilityPhase::kRelease) {
            // T5/T6 — same-call collapse: every shipped ability (`cast_ticks == 0`) reaches this
            // branch immediately, so behavior here is byte-identical to the pre-RFC-001 code.
            ready_at_tick_[static_cast<int>(id)] = tick_ + def.cooldown;
            head_ = AbilityHead{};
            p.ok = true;
            p.phase = AbilityPhase::kIdle;
            p.reason = AbilityReject::kOk;
            // The damage the chunk will apply, scaled here so the untrusted side never computes how
            // hard the player hits — exactly as PlanAttack does. Zones carry no direct damage.
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
        } else {
            // Still winding up (unreached by shipped content). The activation was ACCEPTED — cost
            // is debited, the head is seated — but nothing has resolved yet (I3: aim freezes only
            // at Release), so `damage`/`element` stay at their zero defaults and `world.hpp` must
            // not dispatch a chunk message for this response (see AbilityPlan's `phase` comment).
            p.ok = true;
            p.phase = head_.phase;
            p.reason = AbilityReject::kOk;
        }
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
        v.last_swing_tick = last_swing_tick_;
        v.dead_ticks = dead_ticks_;
        v.deaths = deaths_;
        v.mounted = mounted_;
        v.respawn_tx = respawn_tx_;
        v.respawn_ty = respawn_ty_;
        v.return_map = instance_return_map_;
        v.return_x = instance_return_x_;
        v.return_y = instance_return_y_;
        for (int i = 0; i < kItemKinds; ++i) v.items[i] = items_[i];
        for (int i = 0; i < kSkillCount; ++i) {
            v.skill_level[i] = level_[i];
            v.skill_xp[i] = xp_[i];
            v.skill_next[i] = xp_for_level(level_[i]);
            v.essence_paid[i] = essence_paid_[i];
        }
        // The fixed loadout, resolved from levels, plus each slot's remaining cooldown — everything
        // the HUD draws a slot from without asking. A slot whose school is still too low reports its
        // intended ability (so the greyed icon is the right one) and zero cooldown.
        for (int s = 0; s < kAbilitySlots; ++s) {
            const AbilityId id = equipped_ability(level_, s);
            v.ability[s] = id;
            const std::uint64_t ready_at = (id == AbilityId::kCount)
                                               ? 0
                                               : ready_at_tick_[static_cast<int>(id)];
            // Same "ticks remaining" wire shape as before RFC-001 — only the internal storage
            // changed, from a decrementing counter to an absolute tick (Section 8's I4 discipline).
            v.ability_cd[s] =
                (ready_at > tick_) ? static_cast<std::uint16_t>(ready_at - tick_) : 0;
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

    // The one place xp_[s] ever converts to level_[s], shared by GrantXp and RespecSkill's refund
    // so both obey the same ceiling, the same global cap, and the same Essence-gate precondition
    // (RFC-019 §5.7): a level-up landing at 18/19/20 does not commit until essence_paid_[s] covers
    // it. Banked XP past the threshold simply waits when it doesn't — the loop stops, xp_[s] stays
    // at or above `xp_for_level(level_[s])`, and nothing is lost.
    void convert_xp(int s) noexcept {
        while (level_[s] < kMaxSkillLevel && total_levels() < kSkillPointCap &&
               xp_[s] >= xp_for_level(level_[s])) {
            const auto next_level = static_cast<std::uint8_t>(level_[s] + 1);
            if (next_level >= kEssenceGateStartLevel) {
                const std::uint8_t essence_needed =
                    static_cast<std::uint8_t>(next_level - (kEssenceGateStartLevel - 1));
                if (essence_paid_[s] < essence_needed) break;
            }
            xp_[s] -= xp_for_level(level_[s]);
            ++level_[s];
        }
    }

    [[nodiscard]] std::int16_t scaled(std::int16_t base, Skill s) const noexcept {
        return static_cast<std::int16_t>(static_cast<float>(base) *
                                         skill_scale(level_[static_cast<int>(s)]));
    }

    // RFC-001 Section 5 (T12): tear down whatever `head_` is holding — poise-break or a voluntary
    // cancel, never death (that path is simpler and handled inline in `handle(HurtPlayer)`). Refund
    // and cooldown follow `apply_interrupt`'s Cast-vs-Channel split; the `kStaggerTicks` recovery is
    // uniform across every T12 exit this function is called for.
    void interrupt_head() noexcept {
        const InterruptResult r = apply_interrupt(head_.phase);
        const auto id = static_cast<AbilityId>(head_.ability);
        const AbilityDef def = ability_def(id);
        if (r.refund_base) {
            if (def.cost_kind == AbilityCost::kStamina) {
                stamina_ = std::clamp<std::int16_t>(static_cast<std::int16_t>(stamina_ + def.cost), 0,
                                                    kPlayerMaxStamina);
            } else {
                mana_ = std::clamp<std::int16_t>(static_cast<std::int16_t>(mana_ + def.cost), 0,
                                                 kPlayerMaxMana);
            }
        }
        // r.refund_channel_drain has nothing to refund yet — no shipped ability has a channel block,
        // so nothing is ever drained per-tick to give back.
        if (r.charge_half_cooldown) {
            ready_at_tick_[static_cast<int>(id)] = tick_ + (def.cooldown + 1) / 2;  // ceil(cooldown/2)
        }
        staggered_until_tick_ = tick_ + r.stagger_ticks;
        head_ = AbilityHead{};
    }

    // Death is cheap on purpose. You wake at your hearth with nothing taken from you — no gear
    // dropped, no XP lost, no corpse run. This game's default is chill (GAME.md §0), and a death
    // penalty is the most reliable way to turn exploring into hoarding: players who fear losing a
    // backpack stop going anywhere with it. What death costs is the walk back, and out past the
    // second ring that is quite enough.
    // RFC-013 §3.6/§6.5: forks on `pending_eject_`, decided once at the moment of death. Persistent-
    // band death (the `else` branch) is byte-identical to the pre-RFC-013 function. Instanced-band
    // death clears the full carried-item array and relocates to the session's return point — or, if
    // that point was never wired (still the all-zero default), falls back to the bound hearth exactly
    // as §7's general "no sane position to resume at" rule prescribes. Neither path touches xp_[]/
    // level_[] — see §6.6: skills and unlocked abilities are never a death cost.
    //
    // DIVERGENCE from §6.5's literal listing: the RFC's own pseudocode never assigns `map`, deferring
    // to "a Teleport... issued by the same caller" — but respawn() has no external caller (it fires
    // from inside this actor's own Tick handler, at dead_ticks_ == 0), so there is no second message
    // to send. Setting `map` directly here, the same field Teleport's own handler writes, is the
    // minimal fix that makes ejection actually relocate the player rather than leaving them stranded
    // on a MapId their instance's teardown sweep is about to garbage-collect out from under them.
    void respawn() noexcept {
        if (pending_eject_) {
            for (int i = 0; i < kItemKinds; ++i) items_[i] = 0;
            const bool have_return =
                instance_return_map_ != 0 || instance_return_x_ != 0 || instance_return_y_ != 0;
            if (have_return) {
                map = instance_return_map_;
                x_ = static_cast<float>(instance_return_x_) + 0.5f;
                y_ = static_cast<float>(instance_return_y_) + 0.5f;
            } else {
                map = kOverworld;
                x_ = static_cast<float>(respawn_tx_) + 0.5f;
                y_ = static_cast<float>(respawn_ty_) + 0.5f;
            }
            pending_eject_ = false;
        } else {
            x_ = static_cast<float>(respawn_tx_) + 0.5f;
            y_ = static_cast<float>(respawn_ty_) + 0.5f;
        }
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
    std::uint64_t last_swing_tick_ = 0;  // tick of the last granted melee swing; drives the anim
    std::uint16_t dead_ticks_ = 0;
    std::uint32_t deaths_ = 0;
    bool mounted_ = false;
    std::uint16_t respawn_tx_ = 0;
    std::uint16_t respawn_ty_ = 0;
    // RFC-013 §6.1/§6.2: decided once at the moment of death (`pending_eject_`), consumed once by
    // respawn(). `instance_return_*` mirrors respawn_tx_/ty_'s own pattern — populated once, at entry
    // to an instanced MapSession, by SetInstanceReturn (World::use_portal()) — and defaults to the
    // all-zero triple respawn() reads as "unset" (§6.2's guard), never a genuine MapId-0/(0,0) target.
    bool pending_eject_ = false;
    std::uint16_t instance_return_map_ = 0;
    std::uint16_t instance_return_x_ = 0;
    std::uint16_t instance_return_y_ = 0;
    std::int64_t last_hurt_ms_ = -kCombatCooldownMs;
    std::int64_t last_regen_ms_ = 0;
    std::int64_t world_ms_ = 0;
    std::uint64_t tick_ = 0;
    std::int32_t items_[kItemKinds] = {};
    std::uint8_t level_[kSkillCount] = {};
    std::uint32_t xp_[kSkillCount] = {};
    // RFC-019 §5.7: Essence units spent against each branch's Tier IV gate (0..kEssenceGateTotal).
    std::uint8_t essence_paid_[kSkillCount] = {};
    // Per-ability cooldown, keyed by AbilityId (not by slot, so the timer belongs to the move and
    // survives a future loadout-picker unchanged — with the fixed F1a loadout each slot maps to a
    // distinct ability, so this reads identically to per-slot). RFC-001 Section 8 stores cooldowns
    // as an ABSOLUTE tick rather than a decrementing counter — the same I4 discipline
    // `chunk_actor.hpp`'s beacon lease already uses — so `view()` below still reports "ticks
    // remaining" (its wire shape is unchanged) but the stored value survives being read at any tick,
    // not just ticked down one at a time.
    std::uint64_t ready_at_tick_[kAbilityCount] = {};
    // RFC-001's head instance (Section 9): at most one in flight at a time (Invariant I1). Every
    // shipped ability has `cast_ticks == 0`, so in practice this is always kIdle again by the time
    // any other handler observes it — Cast collapses straight to Release inside the very
    // `Ask<UseAbility>` call that started it (see the handler below). It is real, ticked, and
    // interruptible infrastructure regardless: RFC-005/008 need a caster that actually holds a Cast
    // open across ticks once authored content gives it more than zero of them.
    AbilityHead head_{};
    std::uint64_t staggered_until_tick_ = 0;  // RFC-001 Section 5 — post-T12 recovery window
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
