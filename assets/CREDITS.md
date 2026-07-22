# Art credits

All source art is **CC0 1.0 (public domain)**. No attribution is legally required; this file records
provenance anyway, and the Ninja Adventure authors ask (but do not require) for a link.

Licences were verified by reading the `LICENSE.txt` shipped inside each pack — not by trusting an
itch.io tag. For an open-source project this check is not optional: on itch.io "free" routinely
means *free for personal use*, which is not the same thing.

| Pack | Used for | Link |
|---|---|---|
| **Ninja Adventure** (Pixel-Boy & AAA) | **characters (95, 4-direction animated), monsters (66), bosses (20), farm animals (27), FX, items, UI, audio** | <https://pixel-boy.itch.io/ninja-adventure-asset-pack> |
| Roguelike/RPG pack | terrain (grass, dirt, water, stone, sand), tree, crop plants | <https://kenney.nl/assets/roguelike-rpg-pack> |
| Tiny Town | core, wall, turret, plot | <https://kenney.nl/assets/tiny-town> |
| Tiny Dungeon | slime, spider, ghost, player | <https://kenney.nl/assets/tiny-dungeon> |
| Roguelike Caves & Dungeons | dungeon and mine interiors | <https://kenney.nl/assets/roguelike-caves-dungeons> |
| UI Pack RPG Expansion | menu panels, buttons | <https://kenney.nl/assets/ui-pack-rpg-expansion> |
| Fantasy UI Borders | menu frames | <https://kenney.nl/assets/fantasy-ui-borders> |
| Game Icons | item and skill icons (425) | <https://kenney.nl/assets/game-icons> |
| Particle Pack | spell and impact effects (193) | <https://kenney.nl/assets/particle-pack> |
| RPG Audio | hits, footsteps, UI sounds (52) | <https://kenney.nl/assets/rpg-audio> |
| Music Jingles | stingers and fanfares (86) | <https://kenney.nl/assets/music-jingles> |

The last seven are for the full game (see [GAME.md](../GAME.md)) and are **not yet packed into
`atlas.png`** — UI needs its own atlas with different cell sizes, and audio is loaded as files.

## Style consistency — a known issue being fixed in P1

As of the P0 commit the game mixed two art styles: **characters and monsters were Ninja Adventure,
but terrain, buildings and crops were still Kenney.** It shows — the world reads as one game and the
things walking around in it as another.

This is not a licensing problem (both are CC0) but it is a quality one, and it is being fixed in P1
rather than left, because P1 rewrites the terrain system anyway (radial biome rings) and the tileset
has to be touched regardless.

**Done so far: terrain and trees.** All nine terrain types and both tree species now come from Ninja
Adventure. **Still Kenney: buildings** — wall, turret, fence, hearth, plot — and the crop sprites.

Fill tiles were located by *scanning* every sheet for low-variance (flat) tiles and matching mean
colour against the terrain palette, rather than by eyeballing five hundred tiles per sheet.

**Trees are 2 tiles wide and 3 tall in this pack.** Two separate mistakes came from not checking
that: first `TilesetTowers (0,0)` was picked as a tree and is actually a tower roof; then the right
sheet was used but sliced to a single 16x16, which takes the *left half* of a canopy and renders it
sitting on grass. Both look exactly like a bad crop, which is what they were. The packer now has a
`BIG_MANIFEST` for multi-tile sprites: the region is packed contiguously and drawn as one quad,
anchored bottom-centre so the trunk sits on its own tile and the canopy overhangs the tiles above.

Before adding a sprite, check whether it is self-contained in one tile — the check is whether all
four 1px borders are transparent.

**The buildings are still Kenney, and migrating them is not a one-line manifest swap.** Attempted
and reverted: Ninja Adventure's fence (`TilesetElement` 11-13,2) is a horizontal 3-piece run that is
**2 tiles tall** — rail on top, base below. Taking only the top tile renders a thin stick, which
looked worse than what it replaced. Same class of problem as the trees, and it needs the same
answer: a `BIG_MANIFEST` entry plus an autotile pass that knows about run ends, not just middles.

The wall and turret are in the same position. `TilesetHouse` (759 tiles) and `TilesetTowers` (144,
all `object_part` slices of 2x2 landmark towers) have no single-tile wall or turret at all — every
candidate is part of a larger structure. This is a real piece of work, not a pick.

Ninja Adventure ships a complete 16x16 tileset family that covers everything Kenney was doing here:

| Need | Ninja Adventure tileset |
|---|---|
| ground, grass, dirt, paths | `TilesetFloor.png` (22x26 tiles) |
| trees, bushes, flora | `TilesetNature.png` (24x21) |
| water, shore | `TilesetWater.png` (28x17) |
| desert ring | `TilesetDesert.png` (20x12) |
| farm plots and crops | `TilesetField.png` (5x15) |
| village buildings, walls | `TilesetHouse.png` (33x23) |
| towers / turrets | `TilesetTowers.png` (24x6) |
| cliffs, elevation | `TilesetRelief.png` (20x12) |
| dungeon interiors | `TilesetDungeon.png` (12x4) |
| monster camps | `tileset_camp.png` (23x9) |
| ruins | `TilesetVillageAbandoned.png` (20x12) |

The Kenney packs stay in `fetch_assets.sh` for now — some of them (Game Icons, Particle Pack, the
UI packs) cover things Ninja Adventure does not, and those are still wanted.

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

- **`atlas.png`** — the ~20 tiles this game actually uses, packed into one 144×54 texture.
  Committed, because it is the only art the build needs.
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
# Kenney roguelike sheets have a 1px gutter (--stride 17); the tiny_* tilemaps do not (--stride 16)
python3 tools/atlas_preview.py assets/_src/tinytown/Tilemap/tilemap_packed.png /tmp/tt.png \
    --rect 0 0 11 10 --stride 16 --scale 4 --cols-per-row 12
```

## Notes on the packing

- **Tiny Dungeon sprites carry an opaque dark silhouette** (`#3F2631`) — the dungeon floor showing
  through behind each creature. On grass it reads as a mud blob, so the packer keys that colour out.
  Both variants were composited over the grass tile before choosing.
- **The castle wall is a 3x3 nine-slice, not a standalone tile.** Using its top-left corner for
  every wall segment made a horizontal run repeat a left edge over and over. A one-tile-thick,
  free-form player wall cannot use a nine-slice at all, so the renderer autotiles instead: the
  top-middle tile (crenellations along the length, no side edges) drawn unrotated for a horizontal
  run and rotated 90 degrees for a vertical one, with plain brick for isolated tiles and junctions.
  The neighbour lookup is gathered across ALL visible chunks, so a wall does not change style where
  it crosses a chunk boundary.
- **The tall tree is two tiles** (canopy dome above canopy-base-plus-trunk); row 9 of the roguelike
  sheet holds a complete one-tile tree, but the two-tile version is what reads as a forest.
- **Every tile is extruded by 1px** of its own border colour. The camera zooms continuously, so a
  source rect can land on fractional texture coordinates; without the extruded border, sampling at
  a tile edge picks up the neighbour and shows seams.
