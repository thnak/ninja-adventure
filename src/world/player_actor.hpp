// PlayerActor — the player's authoritative state: position, health, inventory.
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
#pragma once

#include <algorithm>
#include <cstdint>

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

inline constexpr std::int16_t kPlayerMaxHp = 100;
inline constexpr float kPlayerSpeed = 6.0f;  // tiles per second

struct PlayerActor : quark::Actor<PlayerActor, quark::Sequential, quark::Priority<0>,
                                  quark::Placement<quark::HashById, Require<Trusted>>> {
    using protocol = Protocol<MoveIntent, GrantItems, HurtPlayer, Ask<SpendItems, bool>,
                              Ask<GetPlayer, PlayerView>>;

    // Set once at bring-up, before the engine starts.
    std::uint64_t id = 0;
    std::uint16_t map = 0;

    void handle(const MoveIntent& m) noexcept {
        // Movement is clamped to the map, not validated against terrain here: terrain is chunk-owned
        // state and this actor deliberately holds none of it. A tier-A actor that had to read the
        // world every step would be a bottleneck; instead the chunk rejects illegal *effects*
        // (planting on water, building on water), which is the property that actually matters.
        x_ = std::clamp(x_ + m.dx, 0.0f, static_cast<float>(kMapTiles) - 1.0f);
        y_ = std::clamp(y_ + m.dy, 0.0f, static_cast<float>(kMapTiles) - 1.0f);
    }

    void handle(const GrantItems& g) noexcept {
        const int k = static_cast<int>(g.kind);
        if (k >= 0 && k < kItemKinds) items_[k] += g.count;
    }

    void handle(const HurtPlayer& h) noexcept {
        hp_ = static_cast<std::int16_t>(std::max(0, hp_ - h.amount));
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
        m.respond(true);
    }

    void handle(const Ask<GetPlayer, PlayerView>& m) noexcept { m.respond(view()); }

    [[nodiscard]] PlayerView view() const noexcept {
        PlayerView v{};
        v.id = id;
        v.map = map;
        v.x = x_;
        v.y = y_;
        v.hp = hp_;
        for (int i = 0; i < kItemKinds; ++i) v.items[i] = items_[i];
        return v;
    }

    void set_position(float x, float y) noexcept {
        x_ = x;
        y_ = y;
    }

    void set_start_items(std::int32_t wood, std::int32_t stone, std::int32_t seed) noexcept {
        items_[static_cast<int>(ItemKind::kWood)] = wood;
        items_[static_cast<int>(ItemKind::kStone)] = stone;
        items_[static_cast<int>(ItemKind::kSeed)] = seed;
    }

private:
    float x_ = 0.0f;
    float y_ = 0.0f;
    std::int16_t hp_ = kPlayerMaxHp;
    std::int32_t items_[kItemKinds] = {};
};

// Build costs, consulted before a PlaceBuilding is issued.
struct BuildCost {
    ItemKind kind;
    std::int32_t count;
};

[[nodiscard]] inline constexpr BuildCost cost_of(BuildKind k) noexcept {
    switch (k) {
        case BuildKind::kFence: return {ItemKind::kWood, 2};
        case BuildKind::kWall: return {ItemKind::kWood, 5};
        case BuildKind::kTurret: return {ItemKind::kStone, 12};
        case BuildKind::kPlot: return {ItemKind::kWood, 2};
        case BuildKind::kCore: return {ItemKind::kStone, 0};
        case BuildKind::kCount: break;
    }
    return {ItemKind::kWood, 1};
}

// Upgrading level L -> L+1 costs the build price scaled by the level reached: cheap to reinforce a
// fence, expensive to max a turret.
[[nodiscard]] inline constexpr BuildCost upgrade_cost_of(BuildKind k, std::uint8_t level) noexcept {
    const BuildCost base = cost_of(k);
    return BuildCost{base.kind, base.count * 2 * (level < 1 ? 1 : level)};
}

inline constexpr std::int32_t kTillCost = 1;  // wood, per tile reclaimed

}  // namespace mmo
