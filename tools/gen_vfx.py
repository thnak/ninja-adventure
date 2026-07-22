#!/usr/bin/env python3
"""Generate VFX sprite sheets from math, in the Ninja Adventure house style.

Why this is possible at all: the pack's own VFX are not painted art. Every effect sheet in
`assets/_src/ninja/FX` decomposes into exactly two things —

  1. a **binary mask** (alpha is only ever 0 or 255; there is zero anti-aliasing), and
  2. a **3-5 step ramp** pulled from one row of `Palette.png`.

    FX/Particle/Fire.png       3 colours: e46d3a ff9554 ffe18d      (palette row 2 + row 0)
    FX/Slash/SpriteSheetArc    3 colours: 79b8ce 8feff1 ffffff      (palette row 4 + white)
    FX/Magic/Circle/White      1 colour:  ffffff

A shape you can write as a formula plus a ramp you look up is a thing a script can emit. That is
why this generator targets VFX and *not* characters: a 4-direction walk cycle carries anatomy and
intent that no closed-form field describes, but an expanding shockwave is `abs(r - r(t)) < w(t)`.

House rules this file enforces, because breaking any one of them reads as "not from this pack":

  * **Binary alpha.** Coverage is supersampled then thresholded at 0.5. Never blended.
  * **Palette lock.** Every emitted colour is snapped to a real `Palette.png` entry.
  * **Few bands.** Shading is quantised to the ramp length, so gradients stay posterised.

Per ARCHITECTURE.md this is build-time tooling; the runtime stays C++ and never calls Python.

    tools/gen_vfx.py                          # writes sheets + preview.png to assets/_gen/vfx
    tools/gen_vfx.py --out /tmp/x --scale 6   # somewhere else, bigger preview
"""
import argparse
import math
import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
PALETTE_PNG = ROOT / "assets" / "_src" / "ninja" / "Palette.png"

SS = 3  # supersamples per pixel axis; only decides mask coverage, never blends colour


# ---------------------------------------------------------------------------------------------
# palette
# ---------------------------------------------------------------------------------------------
def load_palette():
    """The 90-cell Palette.png grid, minus the 000000 cells that pad short rows."""
    img = Image.open(PALETTE_PNG).convert("RGB")
    out = []
    for y in range(img.height):
        for x in range(img.width):
            c = img.getpixel((x, y))
            if c != (0, 0, 0):
                out.append(c)
    out.append((255, 255, 255))
    return out


PALETTE = load_palette()


def snap(c):
    """Nearest palette entry. Guards hand-typed ramps from drifting off-pack."""
    return min(PALETTE, key=lambda p: sum((a - b) ** 2 for a, b in zip(p, c)))


def ramp(*hexes):
    """Dim -> bright. Index 0 is the outer/cool edge, the last entry is the hot core."""
    return [snap((int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))) for h in hexes]


RAMPS = {
    "fire":   ramp("e46d3a", "ff9554", "ffe18d", "ffffff"),
    "ice":    ramp("548789", "79b8ce", "8feff1", "ffffff"),
    "arcane": ramp("543c52", "8f3e56", "a5608b", "d3a2c0"),
    "smoke":  ramp("4e484a", "5f7160", "8d977f", "abc2bc"),
    "nature": ramp("56864c", "74a334", "a8a129", "adbc3a"),
    "holy":   ramp("ffad5d", "ffcb8d", "ffe18d", "ffffff"),
}


# ---------------------------------------------------------------------------------------------
# deterministic noise — no `random`, so a rebuild is byte-identical
# ---------------------------------------------------------------------------------------------
def hash01(i, j=0, k=0):
    h = (i * 374761393 + j * 668265263 + k * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFFFF) / 0xFFFFFF


def smoothstep(a):
    return a * a * (3 - 2 * a)


def value_noise(x, y):
    xi, yi = math.floor(x), math.floor(y)
    xf, yf = smoothstep(x - xi), smoothstep(y - yi)
    n00, n10 = hash01(xi, yi), hash01(xi + 1, yi)
    n01, n11 = hash01(xi, yi + 1), hash01(xi + 1, yi + 1)
    return (n00 * (1 - xf) + n10 * xf) * (1 - yf) + (n01 * (1 - xf) + n11 * xf) * yf


def ease_out(t, p=2.0):
    return 1 - (1 - t) ** p


# ---------------------------------------------------------------------------------------------
# fields — each returns None for transparent, else heat in [0,1] (1 = hottest ramp entry), or
# (layer, heat) to select a second ramp. Two ramps is what a meteor needs: grey rock, fire trail.
#
# u,v have the origin at the sprite centre with +v up, scaled by the LONGER axis so a pixel stays
# square. On a 12x16 sprite that means v spans [-1,1] and u only [-0.75,0.75] — circles stay round.
# t is 0..1 across the loop.
# ---------------------------------------------------------------------------------------------
def field_shockwave(u, v, t):
    """Expanding ring. Radius eases out, wall thins as it goes — the classic impact pop."""
    r = math.hypot(u, v)
    radius = 0.15 + 0.85 * ease_out(t, 2.2)
    width = 0.30 * (1 - t) ** 1.4 + 0.05
    d = abs(r - radius)
    if d > width:
        return None
    return (1 - d / width) * (1 - 0.55 * t)


def field_burst(u, v, t):
    """Radial spark. 9 spokes of hashed length, flying out and shortening."""
    r = math.hypot(u, v)
    if r < 1e-4:
        return 1.0
    spokes = 9
    a = (math.atan2(v, u) / (2 * math.pi)) % 1.0
    idx = int(a * spokes)
    frac = abs(a * spokes - idx - 0.5) * 2  # 0 at spoke centre, 1 at the gap
    length = (0.55 + 0.45 * hash01(idx, 7)) * ease_out(t, 1.8)
    thick = 0.55 * (1 - t) + 0.12
    if frac > thick or r > length or r < length * 0.35 * t:
        return None
    return (1 - r / max(length, 1e-4)) * (1 - 0.4 * t) + 0.25


def field_slash(u, v, t):
    """Arc sweep. A polar wedge whose angular span opens then trails, thick in the middle."""
    r = math.hypot(u, v)
    a = math.atan2(v, u)
    start = math.radians(150)
    span = math.radians(170)
    head = start - span * ease_out(t, 1.6)
    tail = head + span * 0.55 * (1 - t * 0.4)
    aa = a
    while aa > start:
        aa -= 2 * math.pi
    while aa < start - 2 * math.pi:
        aa += 2 * math.pi
    if not (head <= aa <= tail):
        return None
    along = (aa - head) / max(tail - head, 1e-4)  # 0 at the leading edge
    radius = 0.72
    width = 0.26 * math.sin(math.pi * min(1.0, along ** 0.7)) * (1 - 0.35 * t) + 0.04
    d = abs(r - radius)
    if d > width:
        return None
    return (1 - d / width) * (1 - along * 0.5)


def field_magic_circle(u, v, t):
    """Rotating rune ring: outer hairline, inner hairline, 8 ticks between them."""
    r = math.hypot(u, v)
    a = math.atan2(v, u) + 2 * math.pi * t
    pulse = 0.94 + 0.06 * math.sin(2 * math.pi * t)
    for radius, w in ((0.88 * pulse, 0.055), (0.52 * pulse, 0.045)):
        if abs(r - radius) < w:
            return 0.9
    ticks = 8
    frac = abs(((a / (2 * math.pi) * ticks) % 1.0) - 0.5) * 2
    if 0.52 * pulse < r < 0.88 * pulse and frac > 0.75:
        return 0.55
    return None


def field_flame(u, v, t):
    """Looping flame. Teardrop envelope, noise nibbling the tip, hot at the base."""
    up = (v + 1) / 2  # 0 at the bottom of the sprite, 1 at the top
    sway = 0.18 * math.sin(2 * math.pi * (t + up * 0.8))
    half = 0.62 * (1 - up) ** 0.55 * (0.85 + 0.25 * math.sin(2 * math.pi * (t * 2 + up * 2)))
    if half <= 0:
        return None
    n = value_noise(u * 2.5 + 11, up * 3.0 - t * 3.0)
    half *= 0.75 + 0.5 * n
    d = abs(u - sway) / half
    if d > 1:
        return None
    return (1 - d * 0.7) * (1 - up * 0.75) + 0.1


def field_orb(u, v, t):
    """Projectile ball: hot core, cooler rim, breathing slightly so it never looks frozen."""
    breathe = 0.82 + 0.10 * math.sin(2 * math.pi * t)
    wob = 0.06 * math.sin(2 * math.pi * (t + 0.25))
    r = math.hypot(u - wob, v + wob * 0.5)
    if r > breathe:
        return None
    return 1 - (r / breathe) ** 1.5


def field_meteor(u, v, t):
    """Falling rock plus its own fire trail — layer 1 is stone, layer 0 is flame."""
    rock_y, rock_r = -0.62, 0.30
    dx, dy = u, v - rock_y
    r = math.hypot(dx, dy)
    bite = 0.82 + 0.18 * value_noise(math.atan2(dy, dx) * 1.6 + 3, t * 4)
    if r < rock_r * bite:
        lit = (dy - dx) / rock_r  # lit from the upper left, the direction it fell from
        return 1, max(0.0, min(1.0, 0.35 + 0.45 * lit + 0.2 * (1 - r / rock_r)))
    s = (v - rock_y) / 1.5  # 0 at the rock, 1 at the top of the sprite
    if not (0 <= s <= 1):
        return None
    flick = 0.7 + 0.5 * value_noise(u * 3 + 5, s * 4 - t * 4)
    half = 0.34 * (1 - s) ** 0.55 * flick
    if half <= 0 or abs(u) > half:
        return None
    return 0, (1 - abs(u) / half * 0.6) * (1 - s * 0.8) + 0.15


EFFECTS = [
    # name                field               w   h   frames  ramps
    ("shockwave",         field_shockwave,    32, 32, 8, ["ice"]),
    ("burst_spark",       field_burst,        24, 24, 6, ["holy"]),
    ("slash_arc",         field_slash,        32, 32, 6, ["ice"]),
    ("magic_circle",      field_magic_circle, 32, 32, 8, ["arcane"]),
    ("flame_loop",        field_flame,        12, 16, 8, ["fire"]),
    ("orb_projectile",    field_orb,          16, 16, 4, ["fire"]),
    ("meteor_fall",       field_meteor,       16, 28, 6, ["fire", "smoke"]),
    # same field, different ramp — one formula covers a whole element family
    ("shockwave_nature",  field_shockwave,    32, 32, 8, ["nature"]),
    ("flame_arcane",      field_flame,        12, 16, 8, ["arcane"]),
    ("orb_smoke",         field_orb,          16, 16, 4, ["smoke"]),
    ("meteor_ice",        field_meteor,       16, 28, 6, ["ice", "smoke"]),
]


def render_frame(field, w, h, t, ramps):
    """Supersample for coverage only; colour is a hard ramp index, so edges stay crisp."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    half = max(w, h) / 2  # scale by the longer axis so a pixel stays square
    for y in range(h):
        for x in range(w):
            heat, covered = {}, 0
            for sy in range(SS):
                for sx in range(SS):
                    u = (x + (sx + 0.5) / SS - w / 2) / half
                    v = (h / 2 - y - (sy + 0.5) / SS) / half
                    val = field(u, v, t)
                    if val is None:
                        continue
                    layer, hv = val if isinstance(val, tuple) else (0, val)
                    covered += 1
                    acc = heat.setdefault(layer, [0.0, 0])
                    acc[0] += max(0.0, min(1.0, hv))
                    acc[1] += 1
            if covered * 2 < SS * SS:  # threshold at 50% — binary alpha, no blending
                continue
            layer = max(heat, key=lambda k: heat[k][1])  # the layer most samples agreed on
            cols = ramps[min(layer, len(ramps) - 1)]
            band = min(len(cols) - 1, int(heat[layer][0] / heat[layer][1] * len(cols)))
            px[x, y] = cols[band] + (255,)
    return img


def build_sheet(field, w, h, frames, ramps):
    sheet = Image.new("RGBA", (w * frames, h), (0, 0, 0, 0))
    for i in range(frames):
        sheet.paste(render_frame(field, w, h, i / frames, ramps), (i * w, 0))
    return sheet


def checkerboard(w, h, cell=8):
    img = Image.new("RGBA", (w, h), (40, 42, 48, 255))
    d = ImageDraw.Draw(img)
    for y in range(0, h, cell):
        for x in range(0, w, cell):
            if (x // cell + y // cell) % 2:
                d.rectangle([x, y, x + cell - 1, y + cell - 1], fill=(52, 55, 62, 255))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "assets" / "_gen" / "vfx"))
    ap.add_argument("--scale", type=int, default=4, help="preview zoom (nearest-neighbour)")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    sheets = []
    for name, field, w, h, frames, ramp_names in EFFECTS:
        sheet = build_sheet(field, w, h, frames, [RAMPS[r] for r in ramp_names])
        sheet.save(out / f"{name}.png")
        pixels = list(sheet.get_flattened_data())
        colours = {c for c in pixels if c[3]}
        assert all(c[3] in (0, 255) for c in pixels), "alpha must stay binary"
        assert all(c[:3] in PALETTE for c in colours), "colours must stay on-palette"
        print(f"{name:18s} {sheet.width:3d}x{sheet.height:2d}  {frames} frames  "
              f"{len(colours)} colours  ramp={'+'.join(ramp_names)}")
        sheets.append((name, sheet))

    # contact sheet, so the result is judged by eye and not by the log line above
    s = args.scale
    pad, label_w = 6, 130
    width = label_w + max(sh.width for _, sh in sheets) * s + pad * 2
    height = sum(sh.height * s + pad for _, sh in sheets) + pad
    preview = checkerboard(width, height)
    d = ImageDraw.Draw(preview)
    y = pad
    for name, sh in sheets:
        big = sh.resize((sh.width * s, sh.height * s), Image.NEAREST)
        preview.paste(big, (label_w, y), big)
        d.text((6, y + big.height // 2 - 4), name, fill=(210, 214, 222, 255))
        y += big.height + pad
    preview.save(out / "preview.png")
    print(f"\n-> {out}/preview.png")


if __name__ == "__main__":
    main()
