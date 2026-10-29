# Map

Map **dựng tay**: bố cục viết thành dữ liệu trong một script `tools/make_*.gd`, chạy một lần để sinh scene `.tscn`
(TileMapLayer + tileset của pack). Không dùng world generator; chạy lại script cho ra đúng cùng một map.
Sau đó có thể mở scene trong Godot để chỉnh tay tiếp, nhưng lần chạy lại script sẽ ghi đè phần chỉnh tay.

## Quy trình dựng một khu

1. **Tileset:** `tools/build_village_tileset.gd` tạo `content/map/tileset_village.tres` = tileset của pack + `TilesetHouse` (nguồn 6)
   + `TilesetNature` (nguồn 7). Cần ô nào mới thì thêm PNG vào `SOURCES` của script này.
2. **Chọn ô:** cắt và phóng to sheet kèm nhãn toạ độ để đọc `(cột, hàng, rộng, cao)` của từng công trình/cây
   (xem `docs/design/asset-sheets/` cho các sheet đã chụp). Đừng đoán toạ độ.
3. **Bố cục:** sửa dữ liệu ở đầu `tools/make_village.gd` (hàng nhà, đường, cây, vật dụng), chạy lại, xem ảnh.
4. **Kiểm tra:** `tools\check_world.ps1` (đồ thị cổng) và `tools\test_interact.ps1 -Test village` (đi thử trong làng).
5. **Xem bằng mắt:** `tools\shot.ps1 -Scene res://content/map/village.tscn -Camera "24,104,1.3"` (toàn cảnh).

## Những điểm dễ sai (đã gặp)

- **Lưới camera lệch (24, 16).** `CameraGrid` của pack có `offset = (24, 16)`: tâm màn hình `(cx, cy)` ở `(320*cx+24, 176*cy+16)`.
  Một khu nhiều màn hình phải căn theo lưới này, nếu không đáy/biên lộ ra nền xám. `make_village.gd` dùng `ORIGIN = (24, 24)`.
- **Mỗi khu phải có `EnvironmentShape` phủ cả khu.** Nếu không `World` giữ màu nền mặc định (nâu) của pack.
- **Va chạm do script thêm theo công trình**, không nằm trong tile: khối nhà trừ ô cửa, gốc cây ở hàng ô cuối.
- **Công trình và cây vẽ vào lớp `Wall` (có y-sort)** để người chơi đi vòng sau/trước đúng; vật tương tác đặt trong node `Tilemap`.
- Tên node vật tương tác phải **duy nhất trong khu** (là khoá lưu trạng thái); `check_world` bắt trùng tên.
- Toạ độ trong `.tscn` của node con của `Tilemap` là toạ độ cục bộ (trừ `ORIGIN`).
