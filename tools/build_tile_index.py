#!/usr/bin/env python3
"""Build assets/tile_index.json — the searchable catalogue of every Ninja Adventure tile.

Three inputs are merged:

  1. tools/tile_metrics.py   pixel facts (opacity, self-containment, colour, texture measures)
  2. tools/tile_labels.py    human-authored region labels, written after looking at contact sheets
  3. assets/_src/ninja/Actor sprite-sheet folders, indexed at folder granularity

The point of the file is to stop tiles being chosen by guesswork. Two fields carry most of that
weight:

  self_contained   all four 1px borders are transparent, so the sprite fits inside this one tile.
                   False on a sprite tile means it is a slice of a bigger object — the trees here
                   are 2-4 tiles wide and 2-3 tall, and slicing one tile out of one gives you half
                   a canopy floating over grass.
  role             fill_plain vs fill_textured. fill_plain is a single solid colour with no pixel
                   detail; it renders as a flat background wash, not as art. fill_textured is a
                   full-tile ground fill that actually has specks/tufts/ripples in it. Use the
                   textured ones for ground.

    tools/build_tile_index.py [--metrics tools/_tile_metrics.json] [--out assets/tile_index.json]
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from tile_labels import RULES  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
ACTOR = ROOT / "assets" / "_src" / "ninja" / "Actor"
SCHEMA_VERSION = 1

# ---------------------------------------------------------------------------------------------
# numeric classification
# ---------------------------------------------------------------------------------------------
# A flat fill is literally one RGBA value. A textured fill is the dominant colour plus a light
# scattering of small specks — so: little of the tile is non-dominant AND no non-dominant blob is
# large. A transition edge fails both: roughly half the tile is the other terrain, in one lump.
PLAIN_MAX_COLORS = 1
TEXTURE_MAX_MINOR = 0.22
TEXTURE_MAX_BLOB = 18
# A tileable ground fill is low-contrast by nature; anything noisier than this is a picture.
TEXTURE_MAX_STDDEV = 35.0

COLOR_WORDS = [
    ((250, 250, 250), "white"), ((235, 232, 238), "off-white"), ((205, 205, 210), "pale grey"),
    ((150, 150, 155), "grey"), ((90, 90, 95), "dark grey"), ((30, 32, 34), "near-black"),
    ((255, 173, 93), "sand-orange"), ((255, 203, 141), "pale sand"), ((236, 213, 173), "beige"),
    ((247, 199, 118), "golden sand"), ((255, 202, 169), "rose-pink"), ((232, 150, 140), "salmon"),
    ((179, 149, 127), "taupe"), ((140, 100, 70), "brown"), ((100, 70, 50), "dark brown"),
    ((173, 188, 58), "lime-green"), ((116, 163, 52), "green"), ((70, 120, 55), "dark green"),
    ((91, 105, 87), "grey-green"), ((121, 184, 206), "water-blue"), ((124, 228, 239), "cyan"),
    ((190, 225, 235), "pale blue"), ((150, 90, 150), "purple"), ((200, 60, 70), "red"),
    ((250, 140, 60), "bright orange"), ((240, 220, 120), "straw-yellow"),
]


def color_word(rgb):
    r, g, b = rgb
    return min(COLOR_WORDS, key=lambda cw: (cw[0][0] - r) ** 2 + (cw[0][1] - g) ** 2
               + (cw[0][2] - b) ** 2)[1]


SIDE_DIFF = 26.0  # colour distance at which a border counts as "a different terrain"


def compass(t):
    """Which sides of the tile show something other than its middle.

    Measured, not guessed: each 1px border's mean colour is compared with the inner 8x8 mean. A
    border that differs is the side where the neighbouring terrain shows through, which is exactly
    what an autotile edge piece encodes. "c" means all four borders match the middle (a body
    tile); "ring" means all four differ (an isolated island tile).
    """
    ctr = t.get("_center")
    if ctr is None:
        return "c"
    out = ""
    for tag, side in zip("nswe", t["_sides"]):
        if side is None:
            out += tag
            continue
        d = sum((a - b) ** 2 for a, b in zip(side, ctr)) ** 0.5
        if d > SIDE_DIFF:
            out += tag
    if len(out) == 4:
        return "ring"
    return out or "c"


SIDE_WORD = {"n": "top", "s": "bottom", "w": "left", "e": "right"}


def edge_phrase(d):
    """Turn a compass code into a statement of what was actually measured."""
    if d == "ring":
        return "island piece - all four borders differ from the middle"
    if d == "c":
        return "body piece - all four borders match the middle"
    sides = " / ".join(SIDE_WORD[ch] for ch in d)
    return f"edge piece - its {sides} border differs from the middle"


def slug(*parts):
    # NB: `if p` would silently drop column/row 0 and collide names, so test for None/"" only.
    s = "_".join(str(p) for p in parts if p is not None and p != "")
    return re.sub(r"__+", "_", re.sub(r"[^a-z0-9_]+", "_", s.lower())).strip("_")


# ---------------------------------------------------------------------------------------------
# rule application
# ---------------------------------------------------------------------------------------------
def rule_for(sheet, col, row):
    """Last matching rule wins, so broad sheet-wide rules can be carved up by later ones."""
    hit = None
    for r in RULES.get(sheet, []):
        c0, c1 = r["cols"]
        r0, r1 = r["rows"]
        if c0 <= col <= c1 and r0 <= row <= r1:
            hit = r
    return hit


def label_tile(t):
    """Return (name, description, role) for one measured tile."""
    sheet, col, row = t["sheet"], t["col"], t["row"]
    if t["alpha_fraction"] == 0.0:
        return "empty", "empty tile, fully transparent", "unknown"

    rule = rule_for(sheet, col, row)
    opaque = t["opaque"]
    ncol = t["_ncolors"]
    flat = ncol <= PLAIN_MAX_COLORS
    lightly_textured = (t["_minor_frac"] <= TEXTURE_MAX_MINOR
                        and t["_max_blob"] <= TEXTURE_MAX_BLOB
                        and t["stddev"] <= TEXTURE_MAX_STDDEV)
    cw = color_word(t["_dom_rgb"])

    if rule is None:
        # No hand-written rule covers this cell. Say so honestly rather than inventing a name.
        if opaque and flat:
            return (slug(sheet.replace(".png", ""), "plain", cw),
                    f"plain flat {cw} fill, no pixel detail", "fill_plain")
        role = ("object_whole" if t["self_contained"] else
                ("fill_textured" if opaque and lightly_textured else
                 ("unknown" if not opaque else "unknown")))
        return (slug(sheet.replace(".png", ""), col, row),
                f"unclassified tile from {sheet} at column {col}, row {row}", role)

    kind = rule["kind"]

    # ---- ground fills -----------------------------------------------------------------------
    if kind == "fill":
        terrain = rule["terrain"]
        if flat:
            return (slug(terrain, "plain"),
                    f"plain flat {cw} fill, no pixel detail - {rule['desc']}", "fill_plain")
        if opaque:
            return (slug(terrain, "textured"),
                    f"{rule['desc']}, with visible {cw} pixel detail", "fill_textured")
        if t["self_contained"]:
            return slug(terrain, "detail"), rule["desc"], "decor"
        return slug(terrain, "edge", compass(t)), rule["desc"], \
            "transition_edge"

    # ---- autotile / transition sets ---------------------------------------------------------
    if kind == "edge":
        terrain = rule["terrain"]
        if opaque and flat:
            return (slug(terrain, "plain"),
                    f"plain flat {cw} fill, no pixel detail - interior of the "
                    f"{terrain.replace('_', ' ')} set", "fill_plain")
        if opaque and lightly_textured:
            return (slug(terrain, "textured"),
                    f"{rule['desc']} - full-tile fill with visible {cw} pixel detail",
                    "fill_textured")
        d = compass(t)
        if not opaque and t["self_contained"]:
            return slug(terrain, "isolated"), f"{rule['desc']}, isolated single-tile piece", "decor"
        return (slug(terrain, "edge", d),
                f"{rule['desc']}; {edge_phrase(d)}",
                "transition_edge")

    # ---- multi-tile objects -----------------------------------------------------------------
    if kind == "object":
        ow, oh = rule.get("ow", 1), rule.get("oh", 1)
        collab = rule.get("collab") or [str(i) for i in range(ow)]
        rowlab = rule.get("rowlab") or [str(i) for i in range(oh)]
        ci = (col - rule["cols"][0]) % ow
        ri = (row - rule["rows"][0]) % oh
        part = f"{rowlab[ri]}_{collab[ci]}" if ow > 1 else rowlab[ri]
        if ow == 1 and oh == 1:
            return slug(rule["name"]), rule["desc"], "object_whole"
        role = "object_whole" if t["self_contained"] else "object_part"
        return (slug(rule["name"], part),
                f"{rowlab[ri].replace('_', ' ')} / {collab[ci].replace('_', ' ')} slice of a "
                f"{ow}x{oh}-tile {rule['name'].replace('_', ' ')} - {rule['desc']}", role)

    # ---- single-tile decoration -------------------------------------------------------------
    if kind == "decor":
        role = "decor" if t["self_contained"] else ("fill_plain" if flat and opaque else "decor")
        if flat and opaque:
            return (slug(rule["name"], "plain"),
                    f"plain flat {cw} fill, no pixel detail - {rule['desc']}", "fill_plain")
        return slug(rule["name"], col, row), rule["desc"], role

    # ---- mixed prop areas -------------------------------------------------------------------
    if opaque and flat:
        return (slug(rule["name"], "plain"),
                f"plain flat {cw} fill, no pixel detail - {rule['desc']}", "fill_plain")
    if opaque and lightly_textured:
        return (slug(rule["name"], "textured"),
                f"{rule['desc']} - full-tile fill with visible {cw} pixel detail", "fill_textured")
    role = "object_whole" if t["self_contained"] else "object_part"
    return slug(rule["name"], col, row), rule["desc"], role


# ---------------------------------------------------------------------------------------------
# actor sheets — folder granularity
# ---------------------------------------------------------------------------------------------
CATEGORY_DESC = {
    "Animal": "farm and wild animal NPC",
    "Boss": "large multi-frame boss enemy",
    "Character": "16x16 humanoid character (player or NPC)",
    "CharacterAnimated": "32x32 fully-animated hero rig",
    "Monster": "small roaming monster enemy",
}


def split_words(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name).strip()


def actor_layout(fname, w, h):
    """Infer the frame grid of one actor sheet. Rules verified against the SeparateAnim files."""
    stem = fname[:-4]
    # 16x16 character SpriteSheet: 4 direction columns x 7 rows, checked pixel-for-pixel against
    # Character/*/SeparateAnim/*.png -- rows 0-3 walk (0 and 2 are the idle pose), 4 attack,
    # 5 jump, 6 is dead / item / special1 / special2 across the four columns.
    if stem.startswith("SpriteSheet") and (w, h) == (64, 112):
        return dict(frame=[16, 16], cols=4, rows=7,
                    cols_mean="facing down / up / left / right",
                    rows_mean="walk f0, walk f1, walk f2, walk f3, attack, jump, "
                              "(dead|item|special1|special2 by column)")
    if stem.startswith("SpriteSheet") and (w, h) == (64, 64) or (w, h) == (64, 64):
        return dict(frame=[16, 16], cols=4, rows=4,
                    cols_mean="facing down / up / left / right",
                    rows_mean="4 walk frames")
    if (w, h) == (64, 32):
        return dict(frame=[16, 16], cols=4, rows=2,
                    cols_mean="facing down / up / left / right", rows_mean="2 walk frames")
    if stem == "Faceset":
        return dict(frame=[w, h], cols=1, rows=1, cols_mean="single portrait",
                    rows_mean="dialogue portrait, not a map sprite")
    if stem.startswith("Faceset"):
        return dict(frame=[w, h], cols=1, rows=1, cols_mean="single portrait",
                    rows_mean="dialogue portrait recolour")
    if stem.startswith("SpriteSheet") or stem.startswith("SpriteShee"):
        # Animal sheets: always two frames side by side, frame width = sheet width / 2. Frame
        # size is NOT 16x16 here (hamsters are 15x15, lions 16x23, side views up to 30x16).
        return dict(frame=[w // 2, h], cols=2, rows=1,
                    cols_mean="2-frame idle/walk cycle",
                    rows_mean="single view (folders with a *Side.png give the side view)")
    if h and w % h == 0:
        return dict(frame=[h, h], cols=w // h, rows=1,
                    cols_mean=f"{w // h} animation frames left to right", rows_mean="single row")
    return dict(frame=[w, h], cols=1, rows=1, cols_mean="whole image", rows_mean="whole image")


def build_actors():
    out = []
    for d in sorted(p for p in ACTOR.rglob("*") if p.is_dir()):
        pngs = sorted(d.glob("*.png"))
        if not pngs:
            continue
        rel = d.relative_to(ACTOR)
        category = rel.parts[0]
        subject = split_words(rel.parts[-1]) if len(rel.parts) > 1 else split_words(rel.parts[0])
        sheets = []
        for p in pngs:
            im = Image.open(p)
            w, h = im.size
            sheets.append(dict(file=p.name, width=w, height=h, **actor_layout(p.name, w, h)))
        variants = sorted({re.sub(r"^(SpriteSheet|SpriteShee|Faceset)", "", s["file"][:-4]) or
                           "base" for s in sheets if not s["file"].startswith("Faceset")})
        out.append(dict(
            folder=str(d.relative_to(ROOT)),
            category=category,
            depicts=f"{subject} - {CATEGORY_DESC.get(category, 'actor')}",
            colour_variants=variants,
            sheets=sheets,
        ))
    return out


# ---------------------------------------------------------------------------------------------
# terrain suggestions
# ---------------------------------------------------------------------------------------------
# Hand-picked from the contact sheets, then verified against the computed role. Each need lists a
# primary and a runner-up plus the role the builder must find at that coordinate - terrain fills
# must come out fill_textured, shorelines must come out transition_edge. The build fails loudly if
# a pick has drifted, so this table can never silently rot into another flat-colour ground.
SUGGESTIONS = {
    "grass": ("fill_textured", [
        ("TilesetFloor.png", 1, 12,
         "lime grass with scattered blade tufts - the standard overworld grass, and the fix for "
         "the flat 0,12 currently wired into build_atlas.py"),
        ("TilesetFloor.png", 12, 12,
         "same tufted pattern in a deeper forest green, for woodland regions"),
    ]),
    "dirt": ("fill_textured", [
        ("TilesetFloor.png", 12, 19,
         "taupe earth with fine grain speckle; reads as packed soil rather than a brown wash"),
        ("TilesetFloor.png", 14, 19,
         "same taupe base with a sparser grain - use as the second dirt tile so runs of dirt do "
         "not visibly repeat"),
    ]),
    "sand": ("fill_textured", [
        ("TilesetFloor.png", 1, 5,
         "orange sand with wind-blown squiggle detail; matches the sand shoreline set in "
         "TilesetWater cols 0-9 rows 0-4"),
        ("tileset_camp.png", 16, 5,
         "paler golden dune sand carrying long wind ripples, from the camp mesa"),
    ]),
    "snow": ("fill_textured", [
        ("TilesetFloor.png", 1, 19,
         "white snow with faint crystal flecks - keeps snow reading as a surface, not as page "
         "background"),
        ("TilesetFloor.png", 3, 19,
         "same snow base with heavier flecking - alternate so large snowfields do not repeat"),
    ]),
    "marsh": ("fill_textured", [
        ("TilesetFloor.png", 12, 12,
         "deep green densely tufted grass - the pack has no dedicated marsh fill, and this is "
         "the wettest-looking ground in it; pair with the swamp shoreline at TilesetWater "
         "cols 13-23 rows 6-10"),
        ("TilesetFloor.png", 12, 19,
         "taupe grained earth, for the drier mud-flat half of a marsh"),
    ]),
    "ash": ("fill_textured", [
        ("TilesetInteriorFloor.png", 16, 13,
         "dark cobble in near-black mortar - actually grey and broken up, unlike the flat editor "
         "swatch TilesetLogic 6,0 that was previously used for ash"),
        ("TilesetInteriorFloor.png", 14, 15,
         "the darkest cobble variant of the same set, for burnt-out cores"),
    ]),
    "stone": ("fill_textured", [
        ("TilesetInteriorFloor.png", 5, 13,
         "sandy cobblestone with a clear stone-by-stone pattern that still reads at 16px"),
        ("TilesetInteriorFloor.png", 5, 1,
         "pale stone brick paving - use where stone should read as built rather than natural"),
    ]),
    "water": ("fill_textured", [
        ("TilesetWater.png", 11, 2,
         "open water with diagonal sparkle streaks; moves the eye, unlike the flat cyan at 11,0"),
        ("TilesetWater.png", 24, 2,
         "pale ice-water with the same streaks, for cold regions"),
    ]),
    "water_shore": ("transition_edge", [
        ("TilesetWater.png", 1, 6,
         "water-to-grass shoreline with the white foam rim; this cell is the top edge of the "
         "full 10x5 autotile at cols 0-9 rows 6-10 - take the whole block, not one tile"),
        ("TilesetWater.png", 1, 0,
         "water-to-sand shoreline, identical autotile shape at cols 0-9 rows 0-4"),
    ]),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--metrics", default=str(ROOT / "tools" / "_tile_metrics.json"))
    ap.add_argument("--out", default=str(ROOT / "assets" / "tile_index.json"))
    a = ap.parse_args()

    if not pathlib.Path(a.metrics).exists():
        subprocess.run([sys.executable, str(ROOT / "tools" / "tile_metrics.py")], check=True)
    m = json.loads(pathlib.Path(a.metrics).read_text())

    tiles, per_sheet = [], {}
    for t in m["tiles"]:
        name, desc, role = label_tile(t)
        tiles.append({
            "sheet": t["sheet"], "col": t["col"], "row": t["row"],
            "name": name, "description": desc, "role": role,
            "opaque": t["opaque"], "self_contained": t["self_contained"],
            "mean_rgb": t["mean_rgb"], "stddev": t["stddev"],
            "alpha_fraction": t["alpha_fraction"],
        })
        per_sheet.setdefault(t["sheet"], {"tiles": 0, "roles": {}})
        per_sheet[t["sheet"]]["tiles"] += 1
        per_sheet[t["sheet"]]["roles"][role] = per_sheet[t["sheet"]]["roles"].get(role, 0) + 1

    lut = {(t["sheet"], t["col"], t["row"]): t for t in tiles}
    suggestions, problems = {}, []
    for need, (want_role, picks) in SUGGESTIONS.items():
        entry = {"wanted_role": want_role}
        for slot, (sh, c, r, why) in zip(("pick", "runner_up"), picks):
            got = lut[(sh, c, r)]
            if got["role"] != want_role:
                problems.append(f"{need}.{slot} {sh} {c},{r} is {got['role']}, not {want_role}")
            entry[slot] = {"sheet": sh, "col": c, "row": r,
                           "name": got["name"], "role": got["role"], "reason": why}
        suggestions[need] = entry
    if problems:
        print("SUGGESTION CHECK FAILED:", *problems, sep="\n  ")

    role_totals = {}
    for t in tiles:
        role_totals[t["role"]] = role_totals.get(t["role"], 0) + 1

    doc = {
        "schema_version": SCHEMA_VERSION,
        "generated_by": "tools/build_tile_index.py",
        "tile_size": 16,
        "stride": 16,
        "note": "16x16 grid, no gutter. Sheets whose pixel height is not a multiple of 16 have "
                "their partial bottom row dropped; see sheets[].partial.",
        "roles": {
            "fill_plain": "one solid colour, zero pixel detail - renders as a flat background "
                          "wash, not as art",
            "fill_textured": "full-tile tileable ground fill that HAS visible detail "
                             "(specks, tufts, ripples, stones) - use these for ground",
            "transition_edge": "edge/corner piece where one terrain meets another; only correct "
                               "next to the matching fill",
            "object_part": "one slice of a sprite that spans several tiles - never usable alone",
            "object_whole": "a sprite that fits entirely inside its tile",
            "decor": "small self-contained detail to scatter over a fill",
            "unknown": "empty or unclassified",
        },
        "sheets": [dict(s, **per_sheet.get(s["sheet"], {})) for s in m["sheets"]],
        "totals": {"tiles": len(tiles), "sheets": len(m["sheets"]), "by_role": role_totals},
        "suggestions": suggestions,
        "actors": build_actors(),
        "tiles": tiles,
    }
    pathlib.Path(a.out).write_text(json.dumps(doc, separators=(",", ":")))
    print(f"wrote {a.out}  {len(tiles)} tiles, {len(doc['actors'])} actor folders")
    for k, v in sorted(role_totals.items(), key=lambda kv: -kv[1]):
        print(f"  {k:16s} {v}")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
