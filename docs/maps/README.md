# Map

Map **dựng tay**: bố cục viết thành dữ liệu trong một script `tools/make_*.gd`, chạy một lần để sinh scene `.tscn`
(TileMapLayer + tileset của pack). Không dùng world generator; chạy lại script cho ra đúng cùng một map.
Có thể mở scene trong Godot để chỉnh tay tiếp, nhưng chạy lại script sẽ ghi đè phần chỉnh tay.

## Các khu hiện có

| Khu | Script | Ghi chú |
|---|---|---|
| `village` (làng) | `make_village.gd` | 3x2 màn hình, 12 nhà, hai đường ngang, quảng trường, 8 NPC, cổng torii ra rừng |
| `dojo` (đạo quán) | `make_interiors.gd` | 1 màn hình, tường cam, thảm, sư phụ và hai đệ tử |
| `forest` (Rừng Sương) | `make_forest.gd` | 4x2 màn hình, bố cục theo lưới "ô lớn" 2x2 (đường mòn, khoảng trống, ngách bí mật), cửa hang |
| `cave` (hang mỏ, vùng đặc biệt) | `make_interiors.gd` | 1 màn hình, quặng và pha lê, thợ mỏ |
| `test_a/b/c` | `make_test_maps.gd` | chỉ dùng cho bài kiểm tra (`game/tests/world_graph_test.json`) |

Dụng cụ dùng chung: `game/system/mapgen/map_kit.gd` (vẽ khối ô, tường khung, va chạm, đặt biển/rương/NPC/cổng/điểm xuất hiện).

## Quy trình dựng một khu

1. **Tileset:** `tools/build_village_tileset.gd` tạo `content/map/tileset_village.tres` = tileset của pack + `TilesetHouse` (6),
   `TilesetNature` (7), `TilesetElement` (8), `TilesetReliefDetail` (9). Cần sheet mới thì thêm vào `SOURCES` của script này.
2. **Chọn ô:** cắt và phóng to sheet kèm nhãn toạ độ để đọc `(cột, hàng, rộng, cao)` của từng công trình/cây. Đừng đoán toạ độ.
3. **Bố cục:** sửa dữ liệu ở đầu script `make_*.gd`, chạy lại, xem ảnh (`tools\shot.ps1 -Scene ... -Camera "cx,cy,zoom"`).
4. **Đồ thị:** thêm khu vào `content/world_graph.json` (vùng, tên, `map_pos`, `links`) và cổng/điểm xuất hiện trong scene.
5. **Kiểm tra:** `tools\check_world.ps1` (đồ thị cổng), rồi các bài `tools	est_interact.ps1 -Test <village|journey|...>`.

## Những điểm dễ sai (đã gặp)

- **Lưới camera lệch (24, 16).** `CameraGrid` của pack có `offset = (24, 16)`: màn hình `(cx, cy)` phủ x `[320cx-136, 320cx+184]`,
  y `[176cy-72, 176cy+104]`. Với gốc Tilemap `(8, 8)` (mặc định của `map.tscn`), một màn hình = `tx -9..10, ty -5..5`.
  Một khu nhiều màn hình phải khít lưới này; `test_village` kiểm tra bằng cách so `get_used_rect()` với lưới.
- **Không đặt đường/lối đi lên ranh giới màn hình** (`ty 5/6`, `tx -9/-10`...). Camera đổi ô khi người chơi vượt ranh giới:
  nếu đường nằm đúng đó, camera giật khi đi dọc đường và điểm xuất hiện bị đẩy lên mép màn hình. Đặt đường vào giữa một màn hình.
- **Mỗi khu phải có `EnvironmentShape` phủ cả khu.** Nếu không `World` giữ màu nền mặc định (nâu) của pack.
- **Va chạm do script thêm theo công trình**, không nằm trong tile: khối nhà trừ ô cửa, gốc cây ở hàng ô cuối.
- **Công trình và cây vẽ vào lớp `Wall` (có y-sort)**; vật tương tác và NPC đặt trong node `Tilemap` để y-sort.
- **Điểm xuất hiện không được nằm trong/sát vùng cổng**: cổng tự thử lại nên người chơi sẽ bị đẩy ngược. `check_world` bắt lỗi này.
- Tên node vật tương tác phải **duy nhất trong khu** (là khoá lưu trạng thái); `check_world` bắt trùng tên.
- Toạ độ của node con của `Tilemap` là toạ độ cục bộ (trừ gốc `ORIGIN`); `MapKit.local()` lo việc này.
