"""Tách các tảng đá xám trong TilesetNature thành ảnh riêng, kèm bản "đá đen" (tối hơn, ngả lam) cho đá môi trường của boss.
Chạy: python tools/extract_rocks.py   (cần Pillow). Ghi vào game/content/rock/.
"""
import colorsys
import os

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "assets", "_src", "ninja", "Backgrounds", "Tilesets", "TilesetNature.png")
OUT = os.path.join(ROOT, "game", "content", "rock")

# Các tảng đá xám dính nhau trong tileset nên cắt theo toạ độ pixel (x0, y0, x1, y1), đọc từ ảnh phóng to.
CROPS = {
    "wide": (273, 225, 336, 273),    # tấm đá bè: "viên gạch" lớn nhất
    "tall": (336, 223, 368, 273),    # cột đá cao
    "round_a": (240, 225, 272, 257),
    "round_b": (240, 255, 272, 285),
    "pebble_a": (273, 271, 288, 288),
    "pebble_b": (288, 271, 304, 288),
    "pebble_c": (304, 271, 321, 288),
}

def keep_largest(tile: Image.Image) -> Image.Image:
    """Chỉ giữ mảng điểm ảnh liên thông lớn nhất: bỏ các mảnh viền dính từ tảng đá bên cạnh."""
    px = tile.load()
    seen = set()
    best = []
    for y in range(tile.height):
        for x in range(tile.width):
            if (x, y) in seen or px[x, y][3] == 0:
                continue
            comp = []
            stack = [(x, y)]
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                comp.append((cx, cy))
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < tile.width and 0 <= ny < tile.height and (nx, ny) not in seen and px[nx, ny][3] > 0:
                            seen.add((nx, ny))
                            stack.append((nx, ny))
            if len(comp) > len(best):
                best = comp
    keep = set(best)
    out = tile.copy()
    opx = out.load()
    for y in range(tile.height):
        for x in range(tile.width):
            if (x, y) not in keep:
                opx[x, y] = (0, 0, 0, 0)
    return out


def darken(tile: Image.Image) -> Image.Image:
    out = tile.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            # Đá đen: tối hơn hẳn, ngả lam nhẹ; viền vốn đã tối thì giữ gần như nguyên.
            nr, ng, nb = colorsys.hsv_to_rgb(0.62, min(1.0, s * 0.8 + 0.1), v * 0.45)
            px[x, y] = (round(nr * 255), round(ng * 255), round(nb * 255), a)
    return out


def main() -> None:
    img = Image.open(SRC).convert("RGBA")
    os.makedirs(OUT, exist_ok=True)
    for name, box in CROPS.items():
        tile = keep_largest(img.crop(box))
        if name == "tall":  # mẩu viền tối dính vào thân ở mép phải (hàng 31-33): tô lại bằng màu thân đá
            for y, x in ((31, 30), (32, 29), (32, 30), (33, 30)):
                tile.putpixel((x, y), tile.getpixel((x - 1, y - 2)))
        tile.save(os.path.join(OUT, f"rock_gray_{name}.png"))
        darken(tile).save(os.path.join(OUT, f"rock_black_{name}.png"))
        print(f"{name}: {tile.width}x{tile.height}")


if __name__ == "__main__":
    main()
