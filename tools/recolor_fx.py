"""Sinh bản đổi màu của các tờ hình kỹ năng để làm biến thể (măng đá -> băng, lửa -> băng/độc, sét -> tím).
Chạy: python tools/recolor_fx.py   (cần Pillow). Nguồn và đích đều ở game/content/skill/fx.

hue: dịch sắc độ (độ). colorize: ép sắc độ đích cả với điểm ảnh xám (kim loại). sat/val: nhân độ bão hòa / sáng.
"""
import colorsys
import os
import sys

from PIL import Image

FX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "game", "content", "skill", "fx")

# (nguồn, đích, kiểu, sắc độ, bão hòa, độ sáng)
JOBS = [
    ("rockspike.png", "ice_spike_sheet.png", "shift", 175, 0.55, 1.22),
    ("Fireball.png", "FrostBall.png", "shift", 175, 0.85, 1.1),
    ("explosion.png", "frost_explosion.png", "shift", 175, 0.7, 1.12),
    ("Fireball.png", "PoisonBall.png", "shift", 95, 1.0, 0.95),
    ("explosion.png", "poison_explosion.png", "shift", 95, 0.95, 0.95),
    ("thunder.png", "violet_thunder.png", "shift", 235, 0.9, 1.0),
    ("Shuriken.png", "IceShuriken.png", "colorize", 195, 0.6, 1.25),
    ("Kunai.png", "FireKunai.png", "colorize", 18, 0.9, 1.25),
]


def recolor(src: str, dst: str, mode: str, hue: float, sat: float, val: float) -> None:
    img = Image.open(os.path.join(FX, src)).convert("RGBA")
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if mode == "colorize":
                # Điểm ảnh xám/sáng nhận màu đích; viền tối giữ nguyên.
                if v > 0.25:
                    h = hue / 360
                    s = max(s, 0.5) * sat
            else:
                h = (h + hue / 360) % 1.0
                s = min(1.0, s * sat)
            v = min(1.0, v * val)
            nr, ng, nb = colorsys.hsv_to_rgb(h, s, v)
            px[x, y] = (round(nr * 255), round(ng * 255), round(nb * 255), a)
    img.save(os.path.join(FX, dst))
    print(src, "->", dst)


if __name__ == "__main__":
    for job in JOBS:
        recolor(*job)
    sys.exit(0)
