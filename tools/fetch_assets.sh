#!/usr/bin/env bash
# Download the CC0 source art packs into assets/_src/ and rebuild the atlas.
#
# The packs are NOT committed (see .gitignore): they are ~2 MB of mostly-unused tiles, and the only
# thing the game needs is assets/atlas.png, which IS committed. Run this only when changing which
# sprites the atlas uses.
#
# All three packs are CC0 (public domain) by Kenney — no attribution required, but see
# assets/CREDITS.md, which records it anyway.
set -euo pipefail

cd "$(dirname "$0")/.."
DEST="assets/_src"
mkdir -p "$DEST"

fetch() {
  local name="$1" url="$2" dir="$3"
  if [[ -d "$DEST/$dir" ]]; then
    echo "have  $name"
    return
  fi
  echo "fetch $name"
  curl -fsSL --max-time 180 -o "$DEST/$name.zip" "$url"
  unzip -oq "$DEST/$name.zip" -d "$DEST/$dir"
  rm -f "$DEST/$name.zip"
}

# Pinned URLs: Kenney's CDN paths embed a content hash, so these resolve to exact versions. If one
# 404s, the pack was re-released — find the new link on the linked page and update it here.
# https://kenney.nl/assets/roguelike-rpg-pack
fetch kenney_roguelike-rpg-pack \
  "https://kenney.nl/media/pages/assets/roguelike-rpg-pack/12c03cd78b-1677697420/kenney_roguelike-rpg-pack.zip" \
  roguelike
# https://kenney.nl/assets/tiny-town
fetch kenney_tiny-town \
  "https://kenney.nl/media/pages/assets/tiny-town/a415fbeb49-1735736916/kenney_tiny-town.zip" \
  tinytown
# https://kenney.nl/assets/tiny-dungeon
fetch kenney_tiny-dungeon \
  "https://kenney.nl/media/pages/assets/tiny-dungeon/f8422efb44-1674742415/kenney_tiny-dungeon.zip" \
  tinydungeon

# --- Packs for the full game (see GAME.md / ROADMAP.md) -----------------------
# https://kenney.nl/assets/roguelike-caves-dungeons   -- dungeon + mine tiles
fetch kenney_roguelike-caves-dungeons \
  "https://kenney.nl/media/pages/assets/roguelike-caves-dungeons/5195ceb8ca-1677694831/kenney_roguelike-caves-dungeons.zip" \
  caves
# https://kenney.nl/assets/ui-pack-rpg-expansion      -- menus, panels, buttons
fetch kenney_ui-pack-rpg-expansion \
  "https://kenney.nl/media/pages/assets/ui-pack-rpg-expansion/7ec4a46657-1677661824/kenney_ui-pack-rpg-expansion.zip" \
  uirpg
# https://kenney.nl/assets/fantasy-ui-borders         -- menu frames
fetch kenney_fantasy-ui-borders \
  "https://kenney.nl/media/pages/assets/fantasy-ui-borders/ab29cd0165-1701602367/kenney_fantasy-ui-borders.zip" \
  uiborders
# https://kenney.nl/assets/game-icons                 -- item / skill icons
fetch kenney_game-icons \
  "https://kenney.nl/media/pages/assets/game-icons/1ebf9c14af-1677661579/kenney_game-icons.zip" \
  icons
# https://kenney.nl/assets/particle-pack               -- spell / impact effects
fetch kenney_particle-pack \
  "https://kenney.nl/media/pages/assets/particle-pack/f8fe0f8cb8-1677578741/kenney_particle-pack.zip" \
  particles
# https://kenney.nl/assets/rpg-audio                   -- hits, footsteps, UI
fetch kenney_rpg-audio \
  "https://kenney.nl/media/pages/assets/rpg-audio/8e99002d76-1677590336/kenney_rpg-audio.zip" \
  audio_rpg
# https://kenney.nl/assets/music-jingles               -- stingers, fanfares
fetch kenney_music-jingles \
  "https://kenney.nl/media/pages/assets/music-jingles/f37e530b9e-1677590399/kenney_music-jingles.zip" \
  audio_music

echo
python3 tools/build_atlas.py
