// World geometry, entity PODs and the deterministic RNG shared by every actor.
//
// Everything here is plain data with no engine dependency — the simulation actors (chunk_actor.hpp,
// player_actor.hpp, map_director.hpp) and the renderer both include this and nothing else in common.
// That is deliberate: the render seam carries only these types, so swapping raylib for another
// backend touches no simulation code.
//
// COORDINATES. There is exactly one spatial unit in the simulation: the **tile**, as a float, in
// map-global space (`0 .. kMapTiles`). Pixels exist only inside the renderer (`kTilePx`), and chunk
// membership is a pure function of a tile coordinate (`chunk_of`). Keeping one unit removes the
// whole class of "which space is this in?" bug that a client/server split usually invites.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mmo {

// --- Geometry ------------------------------------------------------------------------------------
// A chunk is the unit of ACTOR OWNERSHIP: one ChunkActor owns exactly one chunk and is the single
// writer of everything inside it. Sizing is a trade — smaller chunks mean more actors (more
// parallelism, more cross-chunk migration traffic), larger chunks mean fatter sequential handlers.
// 32x32 keeps a chunk's snapshot comfortably under a page while still giving 64 actors per map.
inline constexpr int kTilePx = 32;      // renderer-only: pixels per tile
inline constexpr int kChunkTiles = 32;  // tiles per chunk edge
inline constexpr int kMapChunks = 32;   // chunks per map edge
inline constexpr int kMapTiles = kChunkTiles * kMapChunks;  // 1024 tiles per overworld edge
inline constexpr int kChunksPerMap = kMapChunks * kMapChunks;  // 1024 chunk actors

// ONE seamless overworld, plus ONE map that holds every interior.
//
// The second map is not a second world. It is a grid of rooms, one per dwelling, and it exists
// because a door has to lead somewhere: `kMapTiles / kRoomPitch` squared is 4096 rooms against 443
// dwellings, so every house on the map gets its own and there is room for ten times as many. The
// alternative — a map per building — would be 443 maps of a million tiles each to hold what is
// really 443 rectangles of 70.
inline constexpr int kMapCount = 2;
inline constexpr int kChunkCount = kMapCount * kChunksPerMap;

inline constexpr std::uint16_t kOverworld = 0;
inline constexpr std::uint16_t kInterior = 1;

// How many players can be logged in at once. This is a SESSION-SLOT count, not an account limit:
// the account table is unbounded, and a slot is what an account is bound to when it logs in.
//
// It is fixed rather than grown on demand because `Engine::register_activation` is cold-only —
// actors are registered before `start()` and the registry is not safe to mutate afterwards. So the
// roster is pre-registered and login binds. See player_actor.hpp.
inline constexpr int kMaxPlayers = 8;
inline constexpr std::uint64_t kPlayerKeyBase = 0x1000;

[[nodiscard]] inline constexpr std::uint64_t player_key(int slot) noexcept {
    return kPlayerKeyBase + static_cast<std::uint64_t>(slot);
}

[[nodiscard]] inline constexpr int player_slot(std::uint64_t key) noexcept {
    return static_cast<int>(key - kPlayerKeyBase);
}

// The world seed lives here, not in world.hpp, because `terrain_of` is the thing that consumes it
// and the renderer needs it too (to know which ring a tile is in without asking an actor).
inline constexpr std::uint64_t kWorldSeed = 0x5EED'0BEEF'CAFEull;

// The middle of the map. It is the easiest ring and the reference point the rings radiate from —
// but it is NOT where a player starts any more. There used to be a hard-coded 13x13 tilled apron
// here and a hearth in the middle of it, which meant every world opened on the same farm. The
// player now wakes somewhere unremarkable and has to walk to a village (GAME.md §6b); `spawn_tile`
// in worldgen.hpp picks that spot.
inline constexpr int kHomeTx = kMapTiles / 2;
inline constexpr int kHomeTy = kMapTiles / 2;

// --- Terrain -------------------------------------------------------------------------------------
enum class Terrain : std::uint8_t {
    kGrass = 0,
    kDirt = 1,   // tilled — the only terrain a crop may be planted on
    kWater = 2,  // impassable
    kStone = 3,
    kSand = 4,
    kTree = 5,  // impassable, harvestable for wood
    kSnow = 6,   // walkable; crops freeze without a hearth nearby
    kMarsh = 7,  // walkable but slow; no foundations without stilts
    kAsh = 8,    // walkable, barren — nothing grows here at all
    // The two terrains world GENERATION writes (everything above is noise). A road and a village
    // square are the same packed earth; a structure's footprint is solid.
    kPath = 9,
    // The footprint of a whole building. Impassable, and the renderer draws PATH under it — the
    // building itself is one multi-tile sprite drawn in its own pass, exactly like a tree. This is
    // why the game needs no single-tile wall art: there is no such thing as half a house.
    kBuilding = 10,
    kCount = 11,
};

[[nodiscard]] inline constexpr bool is_walkable(Terrain t) noexcept {
    return t != Terrain::kWater && t != Terrain::kTree && t != Terrain::kBuilding;
}

// Can a crop be planted here once tilled? Ash never; nor a road or a floor you do not own.
[[nodiscard]] inline constexpr bool is_tillable(Terrain t) noexcept {
    return is_walkable(t) && t != Terrain::kAsh && t != Terrain::kStone && t != Terrain::kPath;
}

// --- Rings: difficulty radiates out from the centre (Valheim-style) --------------------------------
// One rule the player never has to be told: further from the middle is harder. It also removes the
// hardest world-generation constraint for free — stronghold density is a function of radius, so a
// stronghold can never smother a central village. See GAME.md §4.
enum class Ring : std::uint8_t {
    kMeadow = 0,    // the chill ring. Deliberately huge: a player who never leaves has a whole home
    kForest = 1,
    kWetland = 2,   // swamp on one side of the map, desert on the other
    kSnow = 3,
    kWasteland = 4,
    kCount = 5,
};

inline constexpr int kRingCount = static_cast<int>(Ring::kCount);

// Outer edge of each ring, as a fraction of the map's half-width, measured in CHEBYSHEV distance
// (square rings, not circles).
//
// Two things were wrong with the obvious version and the world-map exporter showed both at a glance:
//
//   * **Euclidean rings waste the corners.** A circle inscribed in the map leaves four corners that
//     are all the outermost, harshest ring. The first render was an island of content in a sea of
//     ash. Square rings use the whole map.
//   * **Equal radius steps are nowhere near equal areas.** Area grows as r-squared, so evenly spaced
//     edges gave Meadow 4.6% and Wasteland 44.6% — precisely backwards, since Meadow is the ring a
//     player who never wants to leave has to live in. Measured, not guessed.
//
// So the edges below are the square roots of the CUMULATIVE AREA each ring should own:
//   Meadow 26% · Forest 22% · Wetland 20% · Snow 18% · Wasteland 14%.
// Meadow is deliberately the biggest: it is the chill ring, and it should feel like a whole country.
inline constexpr float kRingEdge[kRingCount] = {0.5099f, 0.6928f, 0.8246f, 0.9274f, 1.01f};

// --- Factions --------------------------------------------------------------------------------
// A faction, not a hostile/friendly bit — because a faction lets creatures fight EACH OTHER, and
// that is what turns "the map has monsters on it" into "the world has an ecosystem" almost for free
// (GAME.md §5). A raid crossing a forest kills the deer in it, and the player sees the cost of
// leaving a stronghold standing without one line of text explaining it.
enum class Faction : std::uint8_t {
    kWild = 0,     // boar, wolf, bear, hare, chicken
    kMonster = 1,  // slimes, spirits, everything a stronghold spits out
    kPlayer = 2,   // the player and anything they build
    kVillager = 3,
    kCount = 4,
};

inline constexpr int kFactionCount = static_cast<int>(Faction::kCount);

// How `a` regards `b` by default. Note the asymmetry that makes wildlife work: Wild is neutral
// toward Player, but Monster is hostile toward everyone including Wild.
enum class Stance : std::uint8_t { kNeutral = 0, kHostile = 1, kFriendly = 2 };

[[nodiscard]] inline constexpr Stance stance_between(Faction a, Faction b) noexcept {
    if (a == Faction::kMonster) return b == Faction::kMonster ? Stance::kFriendly : Stance::kHostile;
    if (b == Faction::kMonster) return Stance::kHostile;  // everyone hates monsters back
    if (a == Faction::kPlayer && b == Faction::kVillager) return Stance::kFriendly;
    if (a == Faction::kVillager && b == Faction::kPlayer) return Stance::kFriendly;
    return Stance::kNeutral;
}

// How a creature regards the PLAYER specifically, and the load-bearing sentence of the whole
// system: **this is state, not a species trait.** A neutral boar that has been shot is hostile to
// you for a while and then cools off. If it were a fixed property of the species there would be
// nothing here to play with — you would simply learn which sprites to avoid.
enum class Disposition : std::uint8_t {
    kHostile = 0,  // attacks on sight
    kNeutral = 1,  // ignores you until provoked
    kTimid = 2,    // never attacks; flees when hit
};

// How long a provoked creature stays angry, and the memory that makes it feel motivated: each time
// you provoke the same creature it stays angry longer. Harass an animal enough and it becomes a
// real enemy — a small detail, but it is what makes the behaviour look like it has a reason.
inline constexpr std::uint16_t kAngerTicks = 200;      // 20 s at 10 Hz
inline constexpr std::uint16_t kAngerPerGrudge = 100;  // +10 s each time it is provoked again
inline constexpr std::uint8_t kMaxGrudge = 4;

// --- Elements and status ------------------------------------------------------------------------
// Magic SETS a status; a physical blow DETONATES it (GAME.md §7). That is the combo system in one
// sentence, and it is why exactly one status is tracked per creature rather than a bitmask: every
// combo in the design is (one status) x (one kind of blow), so a set of them would be state nobody
// reads. Applying a second status replaces the first, which is also what makes chaining a decision
// — you cannot stack every school onto one target and swing once.
enum class Element : std::uint8_t {
    kNone = 0,
    kFire = 1,
    kIce = 2,
    kEarth = 3,
    kShock = 4,
    kCount = 5,
};

inline constexpr int kElementCount = static_cast<int>(Element::kCount);

enum class Status : std::uint8_t {
    kNone = 0,
    kFrozen = 1,   // cannot move at all
    kBurning = 2,  // damage over time
    kWet = 3,      // slower, and the conductor for Shock
    kMuddy = 4,    // much slower
    kShocked = 5,  // damage over time, and staggers
    kCount = 6,
};

[[nodiscard]] inline constexpr Status status_of(Element e) noexcept {
    switch (e) {
        case Element::kFire: return Status::kBurning;
        case Element::kIce: return Status::kFrozen;
        case Element::kEarth: return Status::kMuddy;
        case Element::kShock: return Status::kShocked;
        case Element::kNone:
        case Element::kCount: break;
    }
    return Status::kNone;
}

// How long a freshly applied status lasts, in ticks. Frozen is the shortest because it is the
// strongest: it is a full stop, and the Shatter combo has to be a window you aim for rather than a
// state you park a creature in.
[[nodiscard]] inline constexpr std::uint8_t status_ticks_of(Status s) noexcept {
    switch (s) {
        case Status::kFrozen: return 25;
        case Status::kBurning: return 50;
        case Status::kWet: return 80;
        case Status::kMuddy: return 60;
        case Status::kShocked: return 30;
        case Status::kNone:
        case Status::kCount: break;
    }
    return 0;
}

// Movement multiplier a status imposes.
[[nodiscard]] inline constexpr float status_speed_scale(Status s) noexcept {
    switch (s) {
        case Status::kFrozen: return 0.0f;
        case Status::kMuddy: return 0.45f;
        case Status::kWet: return 0.85f;
        case Status::kShocked: return 0.7f;
        case Status::kBurning: return 1.15f;  // it panics — burning things run
        case Status::kNone:
        case Status::kCount: break;
    }
    return 1.0f;
}

// The combos. `heavy` distinguishes a charged melee blow from a light one; `by_projectile` an arrow
// from a hand weapon. Returned as a damage multiplier plus a flag for the side effect the chunk has
// to act on, because a multiplier alone cannot express "splash two tiles" or "give the caster mana".
enum class Combo : std::uint8_t {
    kNone = 0,
    kShatter = 1,   // Frozen + heavy melee: x2.5, ignores armour
    kBlast = 2,     // Burning + arrow: splash damage 2 tiles
    kConduct = 3,   // Wet + Shock: chains to every wet enemy nearby
    kCrush = 4,     // Muddy + heavy melee: stun
    kArc = 5,       // Shocked + melee: returns mana to the striker
};

[[nodiscard]] inline constexpr Combo combo_of(Status s, bool heavy, bool by_projectile,
                                              Element by_element) noexcept {
    if (by_element == Element::kShock && s == Status::kWet) return Combo::kConduct;
    if (by_element != Element::kNone) return Combo::kNone;  // other spells do not detonate
    if (s == Status::kFrozen && heavy && !by_projectile) return Combo::kShatter;
    if (s == Status::kBurning && by_projectile) return Combo::kBlast;
    if (s == Status::kMuddy && heavy && !by_projectile) return Combo::kCrush;
    if (s == Status::kShocked && !by_projectile) return Combo::kArc;
    return Combo::kNone;
}

[[nodiscard]] inline constexpr float combo_damage_scale(Combo c) noexcept {
    switch (c) {
        case Combo::kShatter: return 2.5f;
        case Combo::kBlast: return 1.6f;
        case Combo::kConduct: return 1.4f;
        case Combo::kCrush: return 1.3f;
        case Combo::kArc: return 1.1f;
        case Combo::kNone: break;
    }
    return 1.0f;
}

[[nodiscard]] inline const char* describe(Combo c) noexcept {
    switch (c) {
        case Combo::kShatter: return "SHATTER";
        case Combo::kBlast: return "BLAST";
        case Combo::kConduct: return "CONDUCT";
        case Combo::kCrush: return "CRUSH";
        case Combo::kArc: return "ARC";
        case Combo::kNone: break;
    }
    return "";
}

// --- Entities ------------------------------------------------------------------------------------
// ONE creature type for everything alive that is not a player — monsters, wildlife, and later the
// villagers. Splitting them into separate entity types would double every loop in ChunkActor for a
// difference (`faction` + `disposition`) that is two bytes. See GAME.md §5, technical consequence 3.
enum class CreatureKind : std::uint8_t {
    kSlime = 0,
    kSpider = 1,
    kGhost = 2,
    kSkull = 3,  // the outer rings' monster: slow, and it hits like a cart
    // Wildlife. These do NOT use the flow field — they wander around a home tile, which is what
    // makes them cheap enough to have a lot of (GAME.md §5, consequence 1).
    kBoar = 4,
    kWolf = 5,
    kBear = 6,
    kHare = 7,
    kChicken = 8,
    // The scripted BOSS (F3): a Giant Red Samurai that lives in a dojo interior room. It is ONE
    // creature type like everything else alive — so every player verb (melee/heavy/arrows/spells/
    // abilities, combos, status, stun) damages and affects it through the exact same loops and the
    // same strike() with no change. Its scripted policy, charge dash, leash and respawn state that a
    // plain Creature has no room for live in a parallel per-room BossState (see chunk_actor.hpp);
    // step_creatures SKIPS it and a dedicated step_bosses drives it. Kept LAST so every index above
    // is unchanged and nothing that rolls a random kind (raids, wildlife) can ever produce it.
    kBoss = 9,
    kCount = 10,
};

inline constexpr int kCreatureKinds = static_cast<int>(CreatureKind::kCount);

enum class CropKind : std::uint8_t { kWheat = 0, kCarrot = 1, kPumpkin = 2 };
// What a PLAYER puts down, one tile at a time. Deliberately short.
//
// It used to also carry kWall, kTurret and kFence, and removing them is a design decision, not a
// cut. Ninja Adventure has no single-tile wall or tower: TilesetHouse's 759 tiles and
// TilesetTowers' 144 are every one of them a SLICE of something bigger, and the fence is two tiles
// tall. Painting a perimeter tile by tile cannot be drawn with this art at all — the earlier
// attempt rendered a wall as the top-left corner of a nine-slice, repeated.
//
// So building became "place a whole structure" (GAME.md §6b), and the whole structures that exist
// today are the ones world generation places: houses, tents, ruins. What is left here is what
// genuinely IS one tile.
enum class BuildKind : std::uint8_t {
    // The player's home fire. It marks where you live and where you respawn, and in the snow rings
    // it is what keeps crops alive. It is NOT a thing whose destruction ends anything — losing it
    // costs you a rebuild and a respawn point, nothing more. (It replaced a `kCore` that the whole
    // world had to defend; see ARCHITECTURE.md §0 S2 for why that was the wrong shape once you can
    // build anywhere and villages exist.)
    kHearth = 0,
    kPlot = 1,
    kCount = 2,
};

// Everything except a crop plot is solid: a mob cannot walk through it and must break it instead.
// This is what makes a perimeter mean anything — before it existed, walls were decorative and mobs
// walked straight past them to the core.
[[nodiscard]] inline constexpr bool blocks_movement(BuildKind k) noexcept {
    return k != BuildKind::kPlot;
}

// Buildings upgrade in place. Level is 1-based; kMaxLevel is the cap.
inline constexpr std::uint8_t kMaxLevel = 3;
enum class ItemKind : std::uint8_t { kWood = 0, kStone = 1, kSeed = 2, kProduce = 3, kCount = 4 };

inline constexpr int kItemKinds = static_cast<int>(ItemKind::kCount);

struct CreatureStats {
    std::int16_t max_hp;
    float speed;          // tiles per second
    std::int16_t damage;  // per attack
    Faction faction;
    Disposition disposition;  // the STARTING disposition; a creature's own may change
    float aggro;              // tiles: how far it notices a player it is willing to fight
    float reach;              // tiles: how close it must be to land a blow
    std::uint16_t xp;         // awarded to whoever kills it
    float territory;          // tiles wandered from home; 0 = uses the flow field instead
    // Ticks a creature freezes, telegraphing, before a blow lands (F2). There is no attack frame
    // in this walk-only art, so the wind-up IS the attack read: the creature plants its feet, a
    // puff goes up, and the player has this long to leave. Bigger and heavier reads slower — the
    // three buckets are small/timid = 4, mid = 6, big = 8, so the thing that can kill you also
    // gives you the most warning. Cadence (kStrikeCooldown) is untouched; this is the beat BEFORE
    // the hit, not a change to how often one comes.
    std::uint8_t windup;
};

[[nodiscard]] inline constexpr CreatureStats stats_of(CreatureKind k) noexcept {
    using F = Faction;
    using D = Disposition;
    switch (k) {
        //                 hp  speed  dmg  faction      disposition  aggro reach  xp  territory windup
        case CreatureKind::kSlime:   return {30, 1.2f,  4, F::kMonster, D::kHostile,  7.0f, 1.0f, 12, 0.0f, 4};
        case CreatureKind::kSpider:  return {45, 2.6f,  7, F::kMonster, D::kHostile,  9.0f, 1.0f, 21, 0.0f, 4};
        case CreatureKind::kGhost:   return {80, 1.8f, 12, F::kMonster, D::kHostile, 10.0f, 1.2f, 36, 0.0f, 6};
        case CreatureKind::kSkull:   return {140, 1.1f, 22, F::kMonster, D::kHostile, 8.0f, 1.2f, 66, 0.0f, 8};
        // Wildlife. The neutral ones hit hard on purpose: a boar you chose to fight should be a
        // real decision, and a bear should be a mistake you only make once.
        case CreatureKind::kBoar:    return {70, 2.2f, 14, F::kWild, D::kNeutral, 3.5f, 1.0f, 27, 14.0f, 6};
        case CreatureKind::kWolf:    return {60, 3.0f, 11, F::kWild, D::kNeutral, 5.0f, 1.0f, 30, 20.0f, 6};
        case CreatureKind::kBear:    return {180, 1.9f, 28, F::kWild, D::kNeutral, 4.0f, 1.3f, 78, 16.0f, 8};
        case CreatureKind::kHare:    return {14, 3.4f,  0, F::kWild, D::kTimid,   6.0f, 0.0f,  6, 10.0f, 4};
        case CreatureKind::kChicken: return {10, 2.4f,  0, F::kWild, D::kTimid,   5.0f, 0.0f,  3,  8.0f, 4};
        // The dojo BOSS (F3). hp/damage here are the FLAT design numbers; spawn_boss uses them
        // verbatim rather than through make_creature's ring scaling, so a boss is 700 HP wherever its
        // room lands on the interior map. `windup` 10 is the biggest telegraph in the game for a
        // normal attack (skull is 8); the 14-tick charge wind-up is a boss-only constant in boss.hpp.
        // `xp` 400 is the design reward, though strike() special-cases the boss to grant it flat.
        case CreatureKind::kBoss:    return {700, 2.5f, 20, F::kMonster, D::kHostile, 12.0f, 2.6f, 400, 0.0f, 10};
        case CreatureKind::kCount: break;
    }
    return {30, 1.2f, 4, F::kMonster, D::kHostile, 7.0f, 1.0f, 4, 0.0f, 4};
}

// Difficulty by RING, applied when a creature is created rather than when it is hit — so a slime
// that wandered inward from the wasteland stays a wasteland slime, and the player can tell.
//
// This is the one balance knob that could only be tuned once the map was real (ROADMAP P2): the
// numbers below are per-ring multipliers on HP and damage, not on speed. Speed is left alone
// deliberately — a faster creature is not harder so much as unfair, because the player's own speed
// is fixed and there is no answer to something that simply outruns you.
[[nodiscard]] inline constexpr float ring_hp_scale(Ring r) noexcept {
    switch (r) {
        case Ring::kMeadow: return 1.0f;
        case Ring::kForest: return 1.5f;
        case Ring::kWetland: return 2.3f;
        case Ring::kSnow: return 3.4f;
        case Ring::kWasteland: return 5.0f;
        case Ring::kCount: break;
    }
    return 1.0f;
}

[[nodiscard]] inline constexpr float ring_damage_scale(Ring r) noexcept {
    switch (r) {
        case Ring::kMeadow: return 1.0f;
        case Ring::kForest: return 1.35f;
        case Ring::kWetland: return 1.8f;
        case Ring::kSnow: return 2.4f;
        case Ring::kWasteland: return 3.2f;
        case Ring::kCount: break;
    }
    return 1.0f;
}

// Which monster a stronghold in this ring sends. The outer rings do not merely field tougher
// slimes; they field different things, so the ring reads as a different place rather than the same
// place with bigger numbers.
[[nodiscard]] inline constexpr CreatureKind raid_kind_of(Ring r, std::uint32_t roll) noexcept {
    switch (r) {
        case Ring::kMeadow: return CreatureKind::kSlime;
        case Ring::kForest: return (roll % 2 == 0) ? CreatureKind::kSlime : CreatureKind::kSpider;
        case Ring::kWetland: return (roll % 2 == 0) ? CreatureKind::kSpider : CreatureKind::kGhost;
        case Ring::kSnow: return (roll % 3 == 0) ? CreatureKind::kSkull : CreatureKind::kGhost;
        case Ring::kWasteland: return (roll % 3 == 0) ? CreatureKind::kGhost : CreatureKind::kSkull;
        case Ring::kCount: break;
    }
    return CreatureKind::kSlime;
}

// Which animals live in this ring, and how thick on the ground. Wildlife is ambient: it is placed
// once at bring-up per chunk, not spawned in waves, because it is scenery that fights back rather
// than a threat the director schedules.
[[nodiscard]] inline constexpr CreatureKind wildlife_kind_of(Ring r, std::uint32_t roll) noexcept {
    switch (r) {
        case Ring::kMeadow:
            return (roll % 4 == 0) ? CreatureKind::kBoar
                                   : (roll % 2 == 0 ? CreatureKind::kHare : CreatureKind::kChicken);
        case Ring::kForest:
            return (roll % 3 == 0) ? CreatureKind::kWolf
                                   : (roll % 3 == 1 ? CreatureKind::kBoar : CreatureKind::kHare);
        case Ring::kWetland:
            return (roll % 2 == 0) ? CreatureKind::kBoar : CreatureKind::kWolf;
        case Ring::kSnow:
            return (roll % 3 == 0) ? CreatureKind::kBear : CreatureKind::kWolf;
        case Ring::kWasteland: return CreatureKind::kBear;
        case Ring::kCount: break;
    }
    return CreatureKind::kHare;
}

// Which way a creature is facing. The order matches the COLUMN order of the Ninja Adventure walk
// sheets so the renderer can pass it straight through as an atlas column — see atlas_slots.hpp.
enum class Facing : std::uint8_t { kDown = 0, kUp = 1, kLeft = 2, kRight = 3 };

// Facing from a movement vector. Whichever axis dominates wins, which is what reads correctly for
// a 4-direction sprite set moving diagonally.
[[nodiscard]] inline Facing facing_of(float dx, float dy) noexcept {
    if (std::abs(dx) > std::abs(dy)) return dx < 0.0f ? Facing::kLeft : Facing::kRight;
    return dy < 0.0f ? Facing::kUp : Facing::kDown;
}

// A creature lives in map-global tile space. Its owning chunk is derived, never stored — so
// migration is "recompute the owner and forward", with no field to forget to update.
struct Creature {
    std::uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::int16_t hp = 0;
    std::int16_t max_hp = 0;  // already ring-scaled, so a health bar means something
    std::int16_t damage = 0;  // likewise
    CreatureKind kind = CreatureKind::kSlime;
    std::uint8_t attack_cd = 0;  // ticks until it may strike again
    Facing facing = Facing::kDown;

    // --- disposition, which is STATE ---------------------------------------------------------
    Disposition disposition = Disposition::kHostile;
    std::uint16_t anger_ticks = 0;  // >0: hostile to `target` regardless of `disposition`
    std::uint8_t grudge = 0;        // times provoked; each one makes the next anger last longer
    std::uint64_t target = 0;       // the player key it is angry at, 0 = nobody

    // --- elemental state ------------------------------------------------------------------------
    Status status = Status::kNone;
    std::uint8_t status_ticks = 0;
    std::uint8_t stun_ticks = 0;

    // --- the telegraphed attack, which is STATE the renderer reads (F2) -------------------------
    // A creature in reach COMMITS to a swing instead of hitting instantly: it freezes here for its
    // species' wind-up (counting DOWN), and only when this hits zero does the blow resolve — landing
    // if the player is still in reach, whiffing at the aimed-at spot if they left. The freeze is the
    // dodge window. `windup > 0` is published so the sprite can shake and tint through the telegraph
    // — a walk-only pack has no attack frame, so this counter is the ONLY thing that says "incoming".
    std::uint8_t windup = 0;
    std::uint64_t windup_target = 0;  // the player key it committed to swing at
    float windup_x = 0.0f;            // where that player stood at commit — the spot a whiff slashes
    float windup_y = 0.0f;

    // --- wildlife wandering ---------------------------------------------------------------------
    // Home is where an animal was born; it strays no further than its species' territory. Monsters
    // leave this at zero and follow the flow field instead.
    std::uint16_t home_tx = 0;
    std::uint16_t home_ty = 0;
    std::uint8_t wander_cd = 0;
    std::int8_t wander_dx = 0;
    std::int8_t wander_dy = 0;

    // Which pose the renderer draws for a kBoss creature (F3): a value of `BossPose` (boss.hpp),
    // written by step_bosses and published in the ChunkView so the renderer picks the right Samurai
    // sprite (idle/walk/attack/charge) — the walk-only telegraph read (windup) is carried by `windup`
    // as for any creature. Zero (kIdle) for every non-boss creature, which never reads it.
    std::uint8_t boss_pose = 0;
};

// An arrow or a bolt in flight, owned by the chunk it is currently over.
//
// WHY A PROJECTILE IS CHUNK STATE and not something the shooter owns: it has to be able to hit
// creatures the shooter cannot see, and the actor that knows what is standing on a tile is the one
// that owns the tile. Making it chunk state means it migrates exactly like a creature does, through
// the same hand-off, and hit resolution is a local loop over the local creature list — no
// cross-actor read on the hot path. The alternative (the player actor owning its arrows) would have
// to ask every chunk along the flight path what is in it, every tick.
struct Projectile {
    std::uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;  // tiles per second
    float vy = 0.0f;
    std::int16_t damage = 0;
    std::uint8_t life = 0;  // ticks remaining
    Element element = Element::kNone;
    std::uint64_t owner = 0;  // the player key to credit a kill to
};

inline constexpr float kArrowSpeed = 18.0f;   // tiles per second
inline constexpr std::uint8_t kArrowLife = 12;  // ticks — about 20 tiles of range

// A flash where something happened: a sword arc, a spell landing, a blast going off.
//
// WHY THIS IS SIMULATION STATE AND NOT A CLIENT-SIDE FLOURISH. The client that swung the sword
// knows it swung, so it could draw its own arc for free. But then a fight is invisible to everyone
// except the person having it — the other players in the field see creatures losing health for no
// visible reason. Publishing effects as ordinary chunk state means combat is legible to whoever is
// watching, which is the entire difference between a multiplayer world and several single-player
// ones in the same coordinate system. The cost is a handful of 12-byte records per chunk per tick,
// all of them gone within a second.
enum class EffectKind : std::uint8_t {
    kSlash = 0,
    kFire = 1,
    kIce = 2,
    kEarth = 3,
    kShock = 4,
    kBlast = 5,  // a combo detonating
    // The ability layer's own flashes (F1a). They ride the SAME published-effect channel as the
    // basic verbs above rather than being drawn client-side, for exactly the reason in the struct
    // comment below: a WhirlCleave the other players cannot see is a fight they cannot read.
    kSlashHeavy = 6,  // WhirlCleave — the 360 arc, at the player
    kSlashCombo = 7,  // CrushBlow — the finishing strike, at the target
    kSmoke = 8,       // SmokeBomb — the puff where the zone is thrown
    kCount = 9,
};

[[nodiscard]] inline constexpr EffectKind effect_of(Element e) noexcept {
    switch (e) {
        case Element::kFire: return EffectKind::kFire;
        case Element::kIce: return EffectKind::kIce;
        case Element::kEarth: return EffectKind::kEarth;
        case Element::kShock: return EffectKind::kShock;
        case Element::kNone:
        case Element::kCount: break;
    }
    return EffectKind::kSlash;
}

// How many ticks an effect lives, PER KIND. A single constant (kEffectLife=6) truncated the long
// strips: the renderer maps age onto frame as `(age * frames) / life`, so six ticks played every
// effect in six steps — fine for a 4-frame slash, but Earth is 14 frames and got barely a third of
// them, and Ice ten. The frame counts here mirror the FX strips in tools/build_atlas.py (Slash 4,
// Fire 8, Ice 10, Earth 14, Shock 8, Blast 9). They live here as plain DATA rather than being read
// from the generated atlas, because tiles.hpp is the sim/renderer common ground and must not depend
// on the renderer-only header — the sim ages and evicts effects, the renderer picks the frame, and
// both read this one table.
//
// The lifetime is `frames * ticks_per_frame` with ticks_per_frame == 1, so at the 10 Hz tick rate
// every strip plays back at ~10 fps — one frame per tick — which is the rate the pixel-art was
// authored for, and it is exactly the mapping the renderer already used, only no longer clipped.
[[nodiscard]] inline constexpr std::uint8_t effect_life_of(EffectKind k) noexcept {
    switch (k) {
        case EffectKind::kSlash: return 4;
        case EffectKind::kFire: return 8;
        case EffectKind::kIce: return 10;
        case EffectKind::kEarth: return 14;
        case EffectKind::kShock: return 8;
        case EffectKind::kBlast: return 9;
        // The ability flashes, matching their FX strip frame counts in tools/build_atlas.py
        // (SlashHeavy 4, SlashCombo 4, Smoke 6) — same one-frame-per-tick playback as the rest.
        case EffectKind::kSlashHeavy: return 4;
        case EffectKind::kSlashCombo: return 4;
        case EffectKind::kSmoke: return 6;
        case EffectKind::kCount: break;
    }
    return 6;
}

// The longest any effect lives, so a caller sizing a buffer or a fade has one bound to reach for.
inline constexpr std::uint8_t kMaxEffectLife = 14;

struct Effect {
    float x = 0.0f;
    float y = 0.0f;
    EffectKind kind = EffectKind::kSlash;
    std::uint8_t age = 0;  // counts up; dropped at effect_life_of(kind)
};

// A ZONE: a lingering circle of effect a player drops on the ground, owned by the chunk it lands in
// and stepped down each tick until it expires. It is the ability layer's second kind of persistent
// state after the projectile — and, like the projectile, it is CHUNK state rather than the caster's,
// because what it does (wet a creature, blind a creature) is a fact about which creatures stand
// where, and the actor that knows that is the one that owns the tile.
//
// This is the minimal F1a shape: a zone belongs to exactly the chunk that owns its centre and only
// touches that chunk's own creatures. A radius that spills into a neighbour therefore under-covers
// at the seam — F2 owns the fan-out that fixes it. Kept deliberately small (kMaxZones per chunk) for
// the same reason effects are capped: a published view is copied, not referenced.
enum class ZoneKind : std::uint8_t {
    kWet = 0,           // RainCall — marks everything inside Status::kWet, feeding the Conduct chain
    kSmokeSuppress = 1, // SmokeBomb — creatures inside drop their target and cannot acquire prey
    kCount = 2,
};

struct Zone {
    ZoneKind kind = ZoneKind::kWet;
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    std::uint16_t ticks_left = 0;  // counts down; dropped at 0
};

inline constexpr std::size_t kMaxZones = 8;  // per chunk — a published view is copied, not referenced

struct Crop {
    std::uint16_t tx = 0;  // map-global tile
    std::uint16_t ty = 0;
    CropKind kind = CropKind::kWheat;
    std::uint8_t stage = 0;      // 0..kCropStages-1; kCropStages-1 == ripe
    std::int64_t planted_ms = 0;
    std::int64_t ripe_ms = 0;
};

inline constexpr std::uint8_t kCropStages = 4;

[[nodiscard]] inline constexpr std::int64_t grow_ms_of(CropKind k) noexcept {
    switch (k) {
        case CropKind::kWheat: return 20'000;
        case CropKind::kCarrot: return 35'000;
        case CropKind::kPumpkin: return 60'000;
    }
    return 20'000;
}

struct Building {
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    BuildKind kind = BuildKind::kHearth;
    std::int16_t hp = 0;
    std::uint8_t cooldown = 0;  // ticks until this building may act again
    std::uint8_t level = 1;
};

[[nodiscard]] inline constexpr std::int16_t base_hp_of(BuildKind k) noexcept {
    switch (k) {
        case BuildKind::kHearth: return 400;
        case BuildKind::kPlot: return 20;
        case BuildKind::kCount: break;
    }
    return 100;
}

// Level 1/2/3 -> x1 / x1.8 / x3. Upgrading is meant to beat rebuilding: three level-1 walls have
// 600 HP spread over three tiles, one level-3 wall has 600 HP on the tile that matters.
[[nodiscard]] inline constexpr std::int16_t max_hp_of(BuildKind k, std::uint8_t level = 1) noexcept {
    const int base = base_hp_of(k);
    const int scaled = (level >= 3) ? base * 3 : (level == 2 ? (base * 9) / 5 : base);
    return static_cast<std::int16_t>(scaled);
}

// --- Chunk addressing ----------------------------------------------------------------------------
struct ChunkCoord {
    std::uint16_t map = 0;
    std::uint16_t cx = 0;
    std::uint16_t cy = 0;

    friend constexpr bool operator==(ChunkCoord, ChunkCoord) = default;
};

// The actor key. One flat integer so `router.get<ChunkActor>(key)` addresses a chunk directly, and
// so the same key is a stable placement input once chunks are distributed across nodes.
[[nodiscard]] inline constexpr std::uint64_t chunk_key(ChunkCoord c) noexcept {
    return (static_cast<std::uint64_t>(c.map) << 32) | (static_cast<std::uint64_t>(c.cy) << 16) |
           static_cast<std::uint64_t>(c.cx);
}

// Dense index into a flat per-chunk array (the snapshot bus). Distinct from `chunk_key`, which is
// sparse-by-design because it doubles as a placement key.
[[nodiscard]] inline constexpr int chunk_index(ChunkCoord c) noexcept {
    return c.map * kChunksPerMap + c.cy * kMapChunks + c.cx;
}

[[nodiscard]] inline constexpr bool in_map(float tx, float ty) noexcept {
    return tx >= 0.0f && ty >= 0.0f && tx < static_cast<float>(kMapTiles) &&
           ty < static_cast<float>(kMapTiles);
}

// Which chunk owns this tile coordinate? Caller must have checked `in_map` first.
[[nodiscard]] inline constexpr ChunkCoord chunk_of(std::uint16_t map, float tx, float ty) noexcept {
    return ChunkCoord{map, static_cast<std::uint16_t>(static_cast<int>(tx) / kChunkTiles),
                      static_cast<std::uint16_t>(static_cast<int>(ty) / kChunkTiles)};
}

// Tile index local to a chunk, for the terrain array.
[[nodiscard]] inline constexpr int local_tile_index(int tx, int ty) noexcept {
    return (ty % kChunkTiles) * kChunkTiles + (tx % kChunkTiles);
}

// --- Deterministic RNG ---------------------------------------------------------------------------
// splitmix64. Every stochastic decision in the simulation draws from a seed derived from
// (chunk key, tick) — so a replay of the same tick sequence produces the same world, on any node.
// That property is what makes redundant execution across untrusted nodes checkable at all.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : s_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9E37'79B9'7F4A'7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, n). Modulo bias is irrelevant at these magnitudes.
    constexpr std::uint32_t below(std::uint32_t n) noexcept {
        return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n);
    }

    constexpr float unit() noexcept {  // [0,1)
        return static_cast<float>(next() >> 40) * (1.0f / 16'777'216.0f);
    }

private:
    std::uint64_t s_;
};

// --- Terrain generation --------------------------------------------------------------------------
// Terrain is a PURE FUNCTION of (world seed, map, global tile) — no neighbour lookups, no smoothing
// pass, no global state. Two consequences that matter more than the visual result:
//
//   * Any node can compute any tile without owning it. A chunk checking whether the tile a mob is
//     about to step onto is walkable does not have to ask the neighbouring chunk — which would be a
//     synchronous cross-actor (and eventually cross-machine) read on the movement hot path.
//   * A chunk re-placed after a node failure regenerates its terrain from its key alone; only the
//     mutable overlay (crops, buildings, mobs) has to be recovered from persistence.
//
// A chunk still caches its own 32x32 block, because the common case is a tile it owns.
// One lattice sample in [0,1). The whole noise field is built from this, so it is the only place
// randomness enters terrain.
[[nodiscard]] inline float lattice(std::uint64_t seed, int ix, int iy) noexcept {
    Rng r(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ix)) * 0x9E37'79B9ull) ^
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(iy)) * 0x85EB'CA6Bull));
    return r.unit();
}

// Smoothstep-interpolated value noise. `scale` is the lattice spacing in tiles: bigger means
// broader features. INTERPOLATION is the whole point — the previous version hashed each tile
// independently, which has no spatial correlation at all, so water/stone came out as isolated
// single-tile confetti instead of lakes and outcrops.
[[nodiscard]] inline float value_noise(std::uint64_t seed, float x, float y, float scale) noexcept {
    const float fx = x / scale;
    const float fy = y / scale;
    const float ffx = std::floor(fx);
    const float ffy = std::floor(fy);
    const int ix = static_cast<int>(ffx);
    const int iy = static_cast<int>(ffy);

    const float dx = fx - ffx;
    const float dy = fy - ffy;
    const float sx = dx * dx * (3.0f - 2.0f * dx);  // smoothstep: C1-continuous across lattice cells
    const float sy = dy * dy * (3.0f - 2.0f * dy);

    const float n00 = lattice(seed, ix, iy);
    const float n10 = lattice(seed, ix + 1, iy);
    const float n01 = lattice(seed, ix, iy + 1);
    const float n11 = lattice(seed, ix + 1, iy + 1);

    const float a = n00 + (n10 - n00) * sx;
    const float b = n01 + (n11 - n01) * sx;
    return a + (b - a) * sy;
}

// Two octaves: a broad shape plus a finer one, so coastlines and forest edges are not perfectly
// smooth blobs.
[[nodiscard]] inline float fbm(std::uint64_t seed, float x, float y, float scale) noexcept {
    return 0.68f * value_noise(seed, x, y, scale) +
           0.32f * value_noise(seed ^ 0xA5A5'5A5Aull, x, y, scale * 0.42f);
}

// Which ring a tile is in. Noise on the radius keeps the boundaries from reading as a dartboard —
// rings get headlands and bays, and you cross them without noticing a line.
[[nodiscard]] inline Ring ring_of(std::uint64_t world_seed, int gx, int gy) noexcept {
    const float half = static_cast<float>(kMapTiles) * 0.5f;
    const float dx = (static_cast<float>(gx) - half) / half;
    const float dy = (static_cast<float>(gy) - half) / half;
    // Chebyshev, so the rings are squares and the map's corners are used — see kRingEdge.
    float r = std::max(std::abs(dx), std::abs(dy));
    // Wobble the radius, not the thresholds — that way the whole boundary moves coherently instead
    // of dissolving into per-tile speckle.
    r += (fbm(world_seed ^ 0x5150'5150ull, static_cast<float>(gx), static_cast<float>(gy), 70.0f) -
          0.5f) * 0.16f;
    for (int i = 0; i < kRingCount; ++i) {
        if (r < kRingEdge[i]) return static_cast<Ring>(i);
    }
    return Ring::kWasteland;
}

// --- Interiors -----------------------------------------------------------------------------------
// A room on map `kInterior` is a rectangle of floor inside a one-tile wall, with a gap in the
// bottom wall you walk out through. It is laid out by ARITHMETIC and nothing else — the geometry
// below is the whole definition, there is no interior generator and no interior overlay — which is
// what keeps the second map as cheap as the first is expensive: any node can evaluate any interior
// tile from its coordinates, exactly like terrain.
//
// The room sprite is composed at pack time from the pack's own nine-slice (`compose_room` in
// tools/build_atlas.py) and its interior is transparent, so what is drawn inside a room is the
// ordinary terrain path over `kStone` — the interior masonry floor that reads as paving outdoors
// and is exactly right in here.
inline constexpr int kRoomPitch = 16;                          // one room per 16x16 tile block
inline constexpr int kRoomsPerRow = kMapTiles / kRoomPitch;    // 64
inline constexpr int kRoomX0 = 3;   // first FLOOR tile inside the block
inline constexpr int kRoomY0 = 2;
inline constexpr int kRoomW = 10;   // floor extent; the sprite is this + 2 in each axis
inline constexpr int kRoomH = 7;
inline constexpr int kRoomDoorX = kRoomX0 + kRoomW / 2 - 1;  // the gap in the bottom wall
inline constexpr int kRoomDoorY = kRoomY0 + kRoomH;

// Top-left tile of room `i`'s block.
[[nodiscard]] inline constexpr int room_block_x(int i) noexcept { return (i % kRoomsPerRow) * kRoomPitch; }
[[nodiscard]] inline constexpr int room_block_y(int i) noexcept { return (i / kRoomsPerRow) * kRoomPitch; }
[[nodiscard]] inline constexpr int room_index_at(int gx, int gy) noexcept {
    return (gy / kRoomPitch) * kRoomsPerRow + (gx / kRoomPitch);
}

// Where you stand when you come in: the floor tile directly above the doorway, so the first step
// back down is the way out and nothing else is.
[[nodiscard]] inline constexpr int room_entry_x(int i) noexcept { return room_block_x(i) + kRoomDoorX; }
[[nodiscard]] inline constexpr int room_entry_y(int i) noexcept { return room_block_y(i) + kRoomDoorY - 1; }

[[nodiscard]] inline constexpr Terrain interior_tile(int gx, int gy) noexcept {
    const int lx = gx % kRoomPitch;
    const int ly = gy % kRoomPitch;
    if (lx == kRoomDoorX && ly == kRoomDoorY) return Terrain::kStone;  // the doorway
    if (lx < kRoomX0 || lx >= kRoomX0 + kRoomW || ly < kRoomY0 || ly >= kRoomY0 + kRoomH) {
        return Terrain::kBuilding;  // wall, and everything between one room and the next
    }
    return Terrain::kStone;
}

// The land itself, before anyone built on it. Pure noise, no neighbour lookups, no global state —
// which is what lets world GENERATION call it while it is still deciding where the villages go.
[[nodiscard]] inline Terrain terrain_base(std::uint64_t world_seed, std::uint16_t map, int gx,
                                          int gy) noexcept {
    if (map == kInterior) return interior_tile(gx, gy);
    const std::uint64_t base = world_seed ^ (static_cast<std::uint64_t>(map) << 48);
    const auto x = static_cast<float>(gx);
    const auto y = static_cast<float>(gy);
    const Ring ring = ring_of(world_seed, gx, gy);

    // Three fields, shared by every ring; only the THRESHOLDS change per ring. Using the same noise
    // everywhere is what makes a biome boundary look like the same land changing rather than two
    // maps stitched together.
    const float wet = fbm(base ^ 0x1111'2222ull, x, y, 26.0f);   // low = water
    const float rock = fbm(base ^ 0x7777'8888ull, x, y, 34.0f);  // high = stone
    const float flora = fbm(base ^ 0x3333'4444ull, x, y, 11.0f);  // high = trees

    switch (ring) {
        case Ring::kMeadow:
            if (wet < 0.24f) return Terrain::kWater;   // small ponds
            if (wet < 0.28f) return Terrain::kSand;
            if (flora > 0.74f) return Terrain::kTree;  // sparse copses
            return Terrain::kGrass;

        case Ring::kForest:
            if (wet < 0.26f) return Terrain::kWater;
            if (wet < 0.30f) return Terrain::kSand;
            if (rock > 0.80f) return Terrain::kStone;
            if (flora > 0.63f) return Terrain::kTree;  // dense, but with gaps you can walk through
            return Terrain::kGrass;

        case Ring::kWetland: {
            // Swamp on one side, desert on the other. The split is NOISY, not a meridian — the
            // first version compared `gx > centre` and drew a dead-straight line down the middle of
            // the map, which the exporter made impossible to miss. Now the boundary wanders, and
            // the seam between the two is a real place you can walk along.
            const float lean = (x - static_cast<float>(kHomeTx)) / (static_cast<float>(kMapTiles) * 0.25f);
            const float wander = (fbm(base ^ 0x2B2B'3C3Cull, x, y, 90.0f) - 0.5f) * 3.0f;
            const bool desert = (lean + wander) > 0.0f;
            if (desert) {
                if (wet < 0.20f) return Terrain::kWater;  // rare oasis
                if (rock > 0.82f) return Terrain::kStone;
                if (flora > 0.86f) return Terrain::kTree;  // the odd palm
                return Terrain::kSand;
            }
            if (wet < 0.42f) return Terrain::kWater;  // lots of standing water
            if (flora > 0.66f) return Terrain::kTree;
            return Terrain::kMarsh;
        }

        case Ring::kSnow:
            if (wet < 0.22f) return Terrain::kWater;  // frozen over in winter (P7)
            if (rock > 0.66f) return Terrain::kStone;
            if (flora > 0.78f) return Terrain::kTree;
            return Terrain::kSnow;

        case Ring::kWasteland:
        case Ring::kCount: break;
    }
    if (wet < 0.18f) return Terrain::kWater;
    if (rock > 0.55f) return Terrain::kStone;
    return Terrain::kAsh;
}

// --- The generated overlay -------------------------------------------------------------------
// Roads, village squares and building footprints cannot be a pure function of (seed, x, y) the way
// terrain is, and the reason is not laziness: a village has to know where the OTHER villages are
// before it can pick a spot, and a road has to know both ends. So world generation runs once, up
// front, and publishes its result here as one byte per tile — `kNoOverlay` where it did not build.
//
// WHY A PROCESS-WIDE POINTER IS THE RIGHT SHAPE, and not a lapse into shared mutable state. It is
// the same argument flow_field.hpp makes, and it holds for the same three reasons:
//
//   * It is derived from the world seed alone, so every node computes a byte-identical array on
//     its own. Nothing is ever sent, and there is no coherence problem when chunks live on
//     different machines.
//   * It is written exactly once, before the engine starts, and is const from then on. There is no
//     interleaving to reason about because there is no second write.
//   * `terrain_of` has to stay a free function callable for ANY tile — that property is what lets a
//     chunk test the tile a mob is stepping onto without asking its neighbour. Threading a layout
//     handle through every caller would buy nothing and cost that.
//
// The alternative — each chunk owning its own patch of village — reintroduces exactly the
// cross-chunk read this design exists to avoid, at the border of every structure.
inline constexpr std::uint8_t kNoOverlay = 0xFF;

namespace detail {
inline const std::uint8_t* g_overlay = nullptr;  // kMapTiles * kMapTiles, or null before worldgen
}

// Called once by worldgen, before the engine starts. Passing null restores bare noise (used by
// tools that want to see the land without anything on it).
inline void publish_overlay(const std::uint8_t* tiles) noexcept { detail::g_overlay = tiles; }

// The world as it actually is: the land, plus whatever generation put on it.
[[nodiscard]] inline Terrain terrain_of(std::uint64_t world_seed, std::uint16_t map, int gx,
                                        int gy) noexcept {
    // The overlay is indexed by tile alone, so it belongs to ONE map. Interiors are pure arithmetic
    // and have no overlay; without this guard every room would be stamped with whatever the
    // overworld happens to have built at the same coordinates.
    if (map == kOverworld && detail::g_overlay != nullptr && gx >= 0 && gy >= 0 && gx < kMapTiles &&
        gy < kMapTiles) {
        const std::uint8_t o = detail::g_overlay[static_cast<std::size_t>(gy) * kMapTiles + gx];
        if (o != kNoOverlay) return static_cast<Terrain>(o);
    }
    return terrain_base(world_seed, map, gx, gy);
}

// --- Doors ---------------------------------------------------------------------------------------
// A door is a pair of tiles on two different maps, and it is published exactly like the overlay is
// and for the same three reasons: it is derived from the world seed alone so every node computes an
// identical copy, it is written once before the engine starts and const afterwards, and `portal_at`
// has to stay a free function callable for any tile — a player actor deciding whether the step it
// just took was through a doorway cannot be made to hold a layout handle.
//
// It is a SORTED ARRAY, not a second million-tile map. There are ~440 doors; a binary search is
// nine comparisons and the alternative is two megabytes to answer a question asked once per player
// per movement message.
struct Door {
    std::uint32_t tile;   // (ty << 16) | tx, on the OVERWORLD — the doorway under the sprite
    std::uint32_t room;   // room index on kInterior
};

[[nodiscard]] inline constexpr std::uint32_t tile_key(int tx, int ty) noexcept {
    return (static_cast<std::uint32_t>(ty) << 16) | static_cast<std::uint32_t>(tx);
}

namespace detail {
inline const Door* g_doors = nullptr;
inline int g_door_count = 0;
}  // namespace detail

// Called once by worldgen, before the engine starts. `doors` must be sorted by `tile`.
inline void publish_doors(const Door* doors, int count) noexcept {
    detail::g_doors = doors;
    detail::g_door_count = count;
}

[[nodiscard]] inline int door_count() noexcept { return detail::g_door_count; }

// Where standing on (map, tx, ty) takes you, or `valid == false` for the overwhelming majority of
// tiles that are not a doorway.
struct Portal {
    std::uint16_t map = 0;
    std::uint16_t tx = 0;
    std::uint16_t ty = 0;
    bool valid = false;
};

[[nodiscard]] inline Portal portal_at(std::uint16_t map, int tx, int ty) noexcept {
    if (detail::g_doors == nullptr) return {};
    if (map == kOverworld) {
        const std::uint32_t key = tile_key(tx, ty);
        int lo = 0;
        int hi = detail::g_door_count - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const std::uint32_t k = detail::g_doors[mid].tile;
            if (k == key) {
                const auto r = static_cast<int>(detail::g_doors[mid].room);
                return {kInterior, static_cast<std::uint16_t>(room_entry_x(r)),
                        static_cast<std::uint16_t>(room_entry_y(r)), true};
            }
            if (k < key) lo = mid + 1;
            else hi = mid - 1;
        }
        return {};
    }
    // Leaving. Only the doorway tile of a room that actually belongs to a door leads anywhere —
    // the other 3600-odd rooms in the grid exist as arithmetic and have nothing on the other side.
    if (tx % kRoomPitch != kRoomDoorX || ty % kRoomPitch != kRoomDoorY) return {};
    const int r = room_index_at(tx, ty);
    if (r < 0 || r >= detail::g_door_count) return {};
    // Out onto the DOORSTEP, the paved tile below the door, not the doorway itself. Landing back on
    // the door would put the player on a portal tile the instant they left through it.
    const std::uint32_t t = detail::g_doors[r].tile;
    return {kOverworld, static_cast<std::uint16_t>(t & 0xFFFFu),
            static_cast<std::uint16_t>((t >> 16) + 1), true};
}

// Which of four mirror orientations to draw a base terrain tile in. A single 16x16 grass tile
// repeated across a 256x256 map shows an obvious grid; mirroring per-tile breaks the repeat at
// zero cost (a negative source rect, no extra texture). Renderer-only — the simulation neither
// knows nor cares.
[[nodiscard]] inline int tile_variant(std::uint16_t map, int gx, int gy) noexcept {
    Rng r((static_cast<std::uint64_t>(map) << 40) ^
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 20) ^
          static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy)));
    return static_cast<int>(r.below(64));  // low 2 bits = mirroring, higher bits = variant choice
}

// (Spawn camps used to live here — five points on the map's rim, evenly spaced by arithmetic.
// World generation places STRONGHOLDS now: they sit where the land allows, their density rises with
// the ring, and they are the thing a raid actually comes out of. See worldgen.hpp.)

// --- The player -----------------------------------------------------------------------------
// Three bars, and each one gates a different verb, which is the only reason to have three: health
// is what the world takes from you, stamina is what swinging and shooting costs, mana is what
// casting costs. If two of them gated the same thing one of them would be decoration.
inline constexpr std::int16_t kPlayerMaxHp = 100;
inline constexpr std::int16_t kPlayerMaxMana = 60;
inline constexpr std::int16_t kPlayerMaxStamina = 100;
inline constexpr float kPlayerSpeed = 6.0f;   // tiles per second on foot
inline constexpr float kMountSpeed = 11.0f;   // on a mount — the 1024x1024 map's travel answer

// Regeneration per tick, at 10 Hz. Stamina comes back fast because it paces a fight; mana comes
// back slowly because it paces a day. Health barely comes back at all — that is what a hearth and
// food are for.
inline constexpr std::int16_t kStaminaRegen = 2;
inline constexpr std::int16_t kManaRegen = 1;
inline constexpr std::int64_t kHealthRegenMs = 3'000;  // 1 hp per 3 s, out of combat only
inline constexpr std::int64_t kCombatCooldownMs = 5'000;

inline constexpr std::int16_t kSwingStamina = 12;
inline constexpr std::int16_t kHeavyStamina = 28;
inline constexpr std::int16_t kShootStamina = 16;
inline constexpr std::int16_t kSpellMana = 14;

inline constexpr std::int16_t kBaseMeleeDamage = 14;
inline constexpr std::int16_t kBaseRangedDamage = 11;
inline constexpr std::int16_t kBaseSpellDamage = 16;
inline constexpr float kMeleeReach = 1.9f;
inline constexpr float kHeavyReach = 2.4f;
inline constexpr float kSpellRadius = 1.8f;

inline constexpr std::uint8_t kSwingCooldown = 3;  // ticks
inline constexpr std::uint8_t kHeavyCooldown = 8;
inline constexpr std::uint16_t kRespawnTicks = 30;

// --- Skills ----------------------------------------------------------------------------------
// No classes: you level what you use (GAME.md §7). The cap is what keeps that from collapsing into
// "everyone maxes everything by hour 40" — it forces a choice, which is what makes players in an
// MMO worth having around each other.
enum class Skill : std::uint8_t { kMelee = 0, kRanged = 1, kMagic = 2, kCraft = 3, kCount = 4 };

inline constexpr int kSkillCount = static_cast<int>(Skill::kCount);
inline constexpr std::uint8_t kMaxSkillLevel = 20;
inline constexpr std::uint16_t kSkillPointCap = 34;  // total levels across all four skills

// XP needed to go from `level` to `level+1`. Quadratic, so the first few come quickly and the last
// few are a project.
[[nodiscard]] inline constexpr std::uint32_t xp_for_level(std::uint8_t level) noexcept {
    const std::uint32_t l = level + 1u;
    return 40u * l * l;
}

// Every skill level is +6% on that skill's damage. Small enough that a level is not a gate, big
// enough that twenty of them is a different character.
[[nodiscard]] inline constexpr float skill_scale(std::uint8_t level) noexcept {
    return 1.0f + 0.06f * static_cast<float>(level);
}

// --- Simulation cadence --------------------------------------------------------------------------
inline constexpr int kTicksPerSecond = 10;
inline constexpr std::int64_t kTickMs = 1000 / kTicksPerSecond;
inline constexpr std::int64_t kDayMs = 45'000;    // daylight: farm and build
inline constexpr std::int64_t kNightMs = 30'000;  // night: waves attack the core
inline constexpr std::int64_t kCycleMs = kDayMs + kNightMs;

[[nodiscard]] inline constexpr bool is_night(std::int64_t world_ms) noexcept {
    return (world_ms % kCycleMs) >= kDayMs;
}

}  // namespace mmo
