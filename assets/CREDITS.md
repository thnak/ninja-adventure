# Art credits

All source art is **CC0 1.0 (public domain)**. No attribution is legally required; this file records
provenance anyway, and the Ninja Adventure authors ask (but do not require) for a link.

Licences were verified by reading the `LICENSE.txt` shipped inside each pack — not by trusting an
itch.io tag. For an open-source project this check is not optional: on itch.io "free" routinely
means *free for personal use*, which is not the same thing.

| Pack | Used for | Link |
|---|---|---|
| **Ninja Adventure** (Pixel-Boy & AAA) | **characters (95, 4-direction animated), monsters (66), bosses (20), farm animals (27), FX, items, UI, audio** | <https://pixel-boy.itch.io/ninja-adventure-asset-pack> |
| Roguelike/RPG pack | crop growth stages (the last Kenney art in the world) | <https://kenney.nl/assets/roguelike-rpg-pack> |
| Roguelike Caves & Dungeons | dungeon and mine interiors | <https://kenney.nl/assets/roguelike-caves-dungeons> |
| UI Pack RPG Expansion | menu panels, buttons | <https://kenney.nl/assets/ui-pack-rpg-expansion> |
| Fantasy UI Borders | menu frames | <https://kenney.nl/assets/fantasy-ui-borders> |
| Game Icons | item and skill icons (425) | <https://kenney.nl/assets/game-icons> |
| Particle Pack | spell and impact effects (193) | <https://kenney.nl/assets/particle-pack> |
| RPG Audio | hits, footsteps, UI sounds (52) | <https://kenney.nl/assets/rpg-audio> |
| Music Jingles | stingers and fanfares (86) | <https://kenney.nl/assets/music-jingles> |

The last six are for the full game (see [GAME.md](../GAME.md)) and are **not yet packed into
`atlas.png`** — UI needs its own atlas with different cell sizes, and audio is loaded as files.

## Style consistency — resolved in P1

As of the P0 commit the game mixed two art styles: **characters and monsters were Ninja Adventure,
but terrain, buildings and crops were still Kenney.** It showed — the world read as one game and the
things walking around in it as another.

**The first world is now entirely Ninja Adventure.** Terrain (all eleven types), trees, roads,
houses, tents, ruins, the hearth and the crop plot all come from the one pack. The only Kenney
sprites left in `atlas.png` are the five crop growth stages, and those are queued behind
`TilesetField.png` in P4 when crops get their own system.

### How the buildings finally migrated: by changing the design, not the manifest

The blocker was real and it was not a pick. Ninja Adventure has **no single-tile wall and no
single-tile tower** — `TilesetHouse` (759 tiles) and `TilesetTowers` (144) are every one of them an
`object_part` slice of something bigger, and the fence is two tiles tall. A first attempt at the
fence took only its top row and rendered a thin stick, worse than the Kenney tile it replaced.

Painting a perimeter one tile at a time simply cannot be drawn with this art. So the design changed
(GAME.md §6b): **the player no longer builds walls at all.** Buildings are whole structures, placed
whole, and the ones that exist are the ones world generation puts down. `BuildKind` went from five
values to two — hearth and crop plot, the only two things that genuinely are one tile.

That deleted more code than it added, including an entire cross-chunk pre-pass in the renderer: a
per-tile wall had to know its neighbours in the chunk next door so its style did not change at the
boundary, and a structure that is one sprite has no neighbours to ask.

### Multi-tile crops: look before you trust

Trees are 2 tiles wide and 3 tall, and houses are 3-4 wide by 3 tall. Four separate rendering bugs
in this project came from picking a rectangle without seeing it:

| Bug | What was actually there |
|---|---|
| A field of tower roofs instead of trees | `TilesetTowers (0,0)` is a roof |
| Trees "cut with the wrong margin" | the right sheet, sliced to one 16x16 = the left half of a canopy |
| Ground rendering as a flat background wash | the scan looked for the LOWEST-variance tiles, which selects solid fills by construction |
| A market stall used as a campfire | `TilesetElement (0,4)` is an awning, picked off a thumbnail |

A wrong rectangle is worse than a wrong tile, because it is only wrong at its **edges** — you get a
house with a strip of its neighbour's roof attached, which no per-tile contact sheet will show you.
So `tools/verify_structures.py` crops every candidate exactly as the packer would, at 6x, on grass,
with a tile grid over it. Everything in `BIG_MANIFEST` went through it. The campfire above is the
one that did not, and it is the one that was wrong.

`assets/tile_index.json` (5225 tiles, with `role`, `self_contained` and a human description written
after looking) is the other half of the answer: it is how a replacement gets found without
eyeballing five hundred tiles a sheet.

### Which sheet covers what

| Need | Ninja Adventure tileset |
|---|---|
| ground, grass, dirt, roads | `TilesetFloor.png` (22x26 tiles) |
| trees, bushes, flora | `TilesetNature.png` (24x21) |
| water, shore | `TilesetWater.png` (28x17) |
| desert ring | `TilesetDesert.png` (20x12) |
| farm plots and crops | `TilesetField.png` (5x15) |
| village houses | `TilesetHouse.png` (33x23) |
| stronghold tents, fire pit | `tileset_camp.png` (23x9) |
| ruins (wasteland villages) | `TilesetVillageAbandoned.png` (20x12) |
| cliffs, elevation | `TilesetRelief.png` (20x12) |
| dungeon interiors | `TilesetDungeon.png` (12x4) |
| drifting leaves, rain, snow | `FX/Particle/` |
| fire / ice / earth / shock spells, the blast a combo makes | `FX/Elemental/` |
| a sword arc | `FX/Attack/Cut/` |
| the arrow in flight | `Items/Projectile/Arrow.png` |
| wildlife and the outer-ring monster | `Actor/Animal/WildBoar`, `Actor/Animal/DogBlack`, `Actor/Monster/{Bear,Racoon,Skull}` |
| the Character screen's portrait | `Actor/Character/NinjaGreen/Faceset.png` |

The Kenney packs stay in `fetch_assets.sh` — some of them (Game Icons, Particle Pack, the UI packs)
cover things Ninja Adventure does not, and those are still wanted for P4's inventory and crafting
screens.

### Choosing the P2 creatures: three plausible names ruled out by looking

Wildlife needed a pack animal, a big neutral bruiser and a small timid critter. The obvious picks by
NAME were all wrong, and a 6x contact sheet of the candidates side by side settled it in one glance:

| Wanted | Obvious pick | What it actually is |
|---|---|---|
| a wolf | `Animal/Hyena` | a **28×13** sheet — it does not fit the 16px grid at all |
| a wolf | `Monster/Beast` | a red demon; nothing about it reads as wildlife |
| a small critter | `Animal/Racoon` | a two-frame side view — `Monster/Racoon` is the same animal with all four directions |

The pack is also not consistent about sheet SHAPE: its four-direction actors are 4×4 grids (column =
facing, row = frame), but its small animals ship as a single row of two frames. Feeding `facing`
into a one-row sheet makes an animal snap between two poses as you walk around it. The renderer
therefore treats `rows == 1` as "these columns are frames, not facings" — one rule, derived from
what the packer already records, rather than a per-species flag.

## Password hashing

`third_party/monocypher/` is [Monocypher](https://github.com/LoupVaillant/Monocypher) 4.0.2,
vendored — **one `.c` file**, dual BSD-2 / CC0. It is what turns a password into an Argon2i hash in
`src/world/account.hpp`, and it was chosen over libsodium or a system Argon2 for exactly one reason:
nothing to install, no version to match, and no step a contributor on Windows can get wrong.

## Audio

`assets/audio/` holds the four sound effects and one music track the game actually uses, copied out
of the Ninja Adventure pack by hand. They are committed (2.1 MB) because `_src/` is not, and the
game has to run from a clean checkout.

| File | Source |
|---|---|
| `ui_click.wav` | `Audio/Sounds/Menu/Accept.wav` |
| `build.wav` | `Audio/Sounds/Menu/Accept2.wav` |
| `harvest.wav` | `Audio/Sounds/Bonus/Bonus.wav` |
| `hit.wav` | `Audio/Sounds/Hit & Impact/Hit1.wav` |
| `theme_day.ogg` | `Audio/Musics/11 - Clearing.ogg` |

## What is committed

- **`atlas.png`** — every sprite the game uses: 37 tiles, 12 animation sheets, 17 multi-tile
  structures and 12 off-grid strips (weather, spells, the arrow, the portrait), packed into one
  448×1834 texture. Committed, because it is the only art the build needs.
- **`tile_index.json`** — 5225 source tiles with role, geometry and a human description. Committed
  because regenerating it costs a full visual pass over 23 sheets.
- **`_src/`** — the raw packs. **Not committed** (`.gitignore`); ~2 MB of mostly-unused tiles.
  Run `tools/fetch_assets.sh` to restore them.

## Changing the art

Everything is driven by the `MANIFEST` in [`tools/build_atlas.py`](../tools/build_atlas.py):

```bash
tools/fetch_assets.sh          # download packs (skips what's present) + rebuild atlas
python3 tools/build_atlas.py   # rebuild atlas only, after editing the MANIFEST
```

The packer regenerates both `assets/atlas.png` and `src/render/atlas_slots.hpp`, so a sprite swap
is a one-line manifest edit — no C++ changes. To find a tile's `(col, row)` in a source sheet:

```bash
# Query the index for a role and a colour, rather than eyeballing 500 tiles a sheet
python3 -c "import json;[print(t) for t in json.load(open('assets/tile_index.json'))['tiles']
            if t['role']=='fill_textured' and 'snow' in t['name']]"

# Whole sheets, labelled col,row, for identifying by eye
python3 tools/contact_sheet.py assets/_src/ninja/Backgrounds/Tilesets/TilesetHouse.png /tmp/h

# MANDATORY before adding anything to BIG_MANIFEST: every candidate rectangle at 6x, on grass,
# with a tile grid over it. A footprint one column too wide is only wrong at its edge.
python3 tools/verify_structures.py /tmp/structures.png
```

## Notes on the packing

- **A road tile has to work in every ring it passes through.** The red-clay path set
  (`TilesetFloor` cols 3-9) was the first pick and looks right on grass; across a snowfield it looks
  like a wound. Only the neutral brown set (cols 14-20) works everywhere the road actually goes.
- **The interior cell is the only usable one in an autotile set.** Every other cell of a path set
  carries grass on one or more edges, so two of them side by side show a hard seam. The index names
  them: `_c` is the centre, everything else is a compass direction.
- **Particles are not on the tile grid** — a leaf is 12x7, a raindrop 8x8 — so they get their own
  packing pass with no extrusion. They are drawn at a fixed size in screen space and never sampled
  at a fractional coordinate, so they have no seam to bleed.
- **Every tile is extruded by 1px** of its own border colour. The camera zooms continuously, so a
  source rect can land on fractional texture coordinates; without the extruded border, sampling at
  a tile edge picks up the neighbour and shows seams.
