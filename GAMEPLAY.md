# Gameplay

**Superseded.** This file described the demo-era farm/wave loop. The game design now lives in:

- [GAME.md](GAME.md) — world, story, and every gameplay system
- [ARCHITECTURE.md](ARCHITECTURE.md) — technical plan, and §0 lists the design errors being corrected
- [ROADMAP.md](ROADMAP.md) — phased plan

What is described here is still what the *current build* does; it is not what the game is becoming.

## What the current build does

192 chunk actors over 3 maps of 256x256 tiles. Day/night cycle, escalating night waves from five
fixed spawn camps, flow-field pathing, solid buildings with upgrades, crop growth on wall-clock
time, free-form tilling. Run `taskset -c 0-3 ./build/mmo_sim 1200`.

Several things it does are **known-wrong against the new design** and are scheduled for removal —
see the tech-debt table in [ROADMAP.md](ROADMAP.md):

| In the build | Why it is wrong now |
|---|---|
| `BuildKind::kCore`, `core_hp`, core-falls loss condition | there is no single point to defend once you build anywhere |
| Five fixed spawn camps | replaced by strongholds placed by world generation, denser further from the map centre |
| 3 fixed maps (`kMapCount`) | one seamless overworld + instanced realms behind portals |
| 256x256 overworld | too small for 20-50 players; 512x512 planned |
