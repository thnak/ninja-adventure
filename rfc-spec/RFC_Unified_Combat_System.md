# RFC: Unified Combat & Physics System

> Status: Overview / umbrella document. Detailed specs live in RFC-001 … RFC-010.

## Goal
Build a unified combat system for Player, Boss, and AI/RL, where every skill is a
composition of shared modules instead of bespoke logic.

## 1. Ability Pipeline
`Cast -> Channel -> Release -> Travel -> Impact -> Persist -> Expire`

- Cast/Channel (wind-up, charging) produces a telegraph.
- Damage is dealt only at Impact, or on collision/explosion.
- Persist creates hazards, terrain changes, and entities.
- Every phase can be counter-played (interrupted, destroyed, dodged, …).

## 2. Telegraph-first
Skill = Pose + FX + Gameplay Effect.
No dedicated cast animation is needed if pose + FX communicate the intent clearly.

## 3. Combat Entity
Every spike, ice pillar, smoke cloud, boulder, … is a CombatEntity with:
- HP
- Lifetime
- Collision
- Aura/Status
- Destroyable
- Team
- Tags

Examples:
- Spike: Root + Destroyable
- Ice Pillar: Wall + Destroyable
- Smoke: Blind + Lifetime
- Totem: Casts Lightning

## 4. Battlefield Control
Skills don't just deal damage — they reshape the battlefield:
- Block paths
- Slow zones
- Vision blockers
- Water pools
- Rubble
- Cracked ground

## 5. Terrain Evolution
Example — Meteor:
Cast -> Levitate -> Fall -> Impact -> Earthquake -> Crater/Rubble.

## 6. Physical Rules
An attack carries:
- Damage
- Impulse
- Heat
- Cold
- Electric
- Explosion
- Pierce
- Crush

Terrain carries:
- Friction
- Grip
- Conductivity
- Stability

Examples:
- Mud: reduces knockback, increases force-transfer damage.
- Ice: increases knockback, reduces direct damage.
- Water: conducts electricity.
- Rubble: slows movement.

## 7. Interaction Rules
No hard-coding per skill. Interactions emerge from rules.

Examples:
- Water + Lightning = electricity spreads.
- Ice + Shockwave = slide farther.
- Mud + Shockwave = less push but more pain.
- Fire + Plant = fire spreads.

## 8. Material System
Every entity has a Material:
- Flesh
- Stone
- Spirit
- Metal
- Wood
- Plant
- Water
- Slime

Spirit:
- immune to Physical Impact
- vulnerable to Arcane/Holy.

## 9. Effect Accumulation
No absolute immunities.

Every effect has:
- Power
- Build-up
- Decay

Freeze ladder:
Cold -> Slow -> Heavy Slow -> Freeze.

Large bosses accumulate more slowly, but are never immune.

## 10. Scale
Scale tiers:
Tiny / Small / Medium / Large / Giant / Titan

Scale automatically adjusts:
- Knockback
- Freeze build-up
- Stun build-up
- Root
- Slow

## 11. Mass & Physics
Mass matters:
Impulse / Mass = Knockback.

A big boss is hard to shove, but still receives the force.

## 12. Destructible Counterplay
The player can:
- destroy a Spike
- break an Ice Wall
- shoot a Meteor out of the air
- shatter a Boulder before Impact

## 13. Skill Composition
Skill = Visual + Spawn + Motion + Impact + After Effect + Status.

Examples:
Rock + Sky + Fall + Explosion = Meteor.
Rock + Ground + Rise + Root = Spike.

## 14. Asset Reuse
Use tint/filter/FX layering:
- Red-tinted rock + trail = Meteor
- Purple rock = Cursed Rock
- Frost-covered rock = Ice Boulder

Reduce/reorder animation frames to create new skills from existing sheets.

## 15. Visual Filters
No dedicated asset needed for every state:

- Freeze: blue filter + frost overlay.
- Burn: red/orange emissive + heat distortion.
- Shock: electricity arcs across the sprite.
- Poison: blue-purple tint.

## 16. Battlefield States
Example — Earthquake:
- Light camera shake.
- Projectiles drift off course.
- Accuracy reduced.
- Telegraphs tremble.
- Terrain changes.

## 17. RL-friendly
The observation state includes:
- Boss ability
- Channel stage
- Terrain
- Hazards
- Entities
- Statuses
- Cooldowns

RL learns patterns, not individual bosses.

## Core Design
Combat is modeled as the interaction between:
- Ability
- Combat Entity
- Terrain
- Physics
- Materials
- Battlefield States

Adding a new boss or skill is primarily data configuration — no new systems required.
