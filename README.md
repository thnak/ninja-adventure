# Ninja Adventure

Game 2D top-down bằng **Godot 4** (GDScript), dùng asset pack *Ninja Adventure* (CC0, pixel-boy).
Mục tiêu: chạy nhẹ, không GC pause, map **dựng tay** thay vì sinh ngẫu nhiên.

## Cấu trúc

| Thư mục | Nội dung |
|---|---|
| `game/` | Project Godot (mở `game/project.godot`). Khởi đầu từ project mẫu của tác giả pack. |
| `assets/` | `CREDITS.md`, `audio/`. `_src/` là pack gốc (bị gitignore, tải thủ công). |
| `docs/design/` | Thiết kế game hiện hành. |
| `docs/maps/` | Quy ước dựng map bằng TileMap. |

## Chạy

```
godot --path game
```

Godot 4.7.2 cài bằng `winget install GodotEngine.GodotEngine`.

## Bản cũ

Bản C++ (server MMO + client tự viết + world generator) đã bị bỏ trên `main`.
Lấy lại từ lịch sử git: `git log --oneline -- src/` rồi `git checkout <commit> -- src/`.
Tài liệu thiết kế cũ (GAME, RFC 001-024...) cũng đã bỏ, vẫn xem được trong lịch sử git. Các thay đổi chưa commit lúc dọn dẹp (atlas, prefabs, `style_objects.py`) đã bị xoá hẳn.
