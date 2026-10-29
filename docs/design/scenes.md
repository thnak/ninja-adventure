# Kế hoạch scene

Trạng thái: **kế hoạch, chưa cài đặt.** Danh sách khu cụ thể sẽ chốt sau khi có `asset-inventory.md`.

## 1. Hiện trạng (project mẫu của tác giả pack)

- `main.gd` nạp title hoặc `world.tscn`. `world.gd` giữ một `Map` duy nhất (`generate_map`), cùng thời tiết, nhạc, chuyển cảnh, camera lưới.
- `Teleporter` chỉ nối hai điểm **cùng scene** (trỏ node trực tiếp). Không nhảy được sang scene khác.
- `Destroyable` (crate, grass, pot) là vật thể duy nhất có tương tác, và chỉ là đập vỡ.
- Môi trường (mưa, tuyết, nhạc, màu) theo vùng `EnvironmentArea` trong map.

Thiếu: nhiều scene, điểm kết nối giữa scene, tương tác ngoài đập vỡ, trạng thái giữ lại khi đổi scene.

## 2. Nguyên tắc

1. **Một khu = một scene `.tscn`** dựng tay, nằm trong `game/content/map/<tên>/`.
2. Scene **không biết scene khác nằm ở đâu**. Nó chỉ khai báo cổng (`Portal`) kèm id. Cách các khu nối nhau nằm ở một file dữ liệu duy nhất (xem 4).
3. Vật thể tương tác là scene nhỏ tái dùng, đặt vào map như prefab, cấu hình bằng `@export` hoặc Resource.
4. Trạng thái thế giới (rương đã mở, cửa đã mở, quái đã hạ) lưu theo `scene_id + object_id`, không nằm trong scene.

## 3. Cấu trúc của một scene khu

```
Map (Node2D, script Map)           # extends system/map/map.gd, có scene_id
├─ Ground, Decor, Walls            # TileMapLayer (nền, trang trí, vật cản)
├─ YSort                           # vật thể + nhân vật xếp theo y
│  ├─ Interactables/               # các vật thể tương tác (mục 5)
│  └─ Actors/                      # NPC, quái
├─ Portals/                        # các cổng sang scene khác (mục 4)
├─ SpawnPoints/                    # Marker2D: nơi người chơi xuất hiện, tên = id cổng vào
└─ EnvironmentArea                 # thời tiết, nhạc, màu (đã có sẵn)
```

Mỗi `Map` có `scene_id` (ví dụ `village`) và thông tin `EnvironmentResource` mặc định.

## 4. Điểm kết nối giữa scene

**`Portal` (Area2D)**: thay thế dùng chéo scene cho `Teleporter`.

| Thuộc tính | Ý nghĩa |
|---|---|
| `portal_id` | id trong scene này (ví dụ `east_gate`) |
| `target_scene` | `scene_id` đích (ví dụ `forest_path`), **không** phải đường dẫn |
| `target_spawn` | tên SpawnPoint trong scene đích |
| `direction` | hướng người chơi nhìn khi đến |
| `transition` | kiểu chuyển cảnh (fade...) |
| `requires` | điều kiện mở (tuỳ chọn): chìa khoá, cờ trạng thái, thời gian |

**Luồng:** người chơi chạm Portal → `World.travel(scene_id, spawn)` → fade out → giải phóng map cũ → nạp map mới theo bảng đăng ký → đặt người chơi tại SpawnPoint → fade in → áp dụng môi trường của khu mới.

**Bảng đăng ký** `game/content/world_graph.json`: `scene_id → đường dẫn .tscn`, vùng, điểm bắt đầu. Các cạnh nằm ở chính các `Portal` trong scene (đích là `target_scene` + `target_spawn`); `tools/check_world.ps1` kiểm tra chúng khớp với bảng và có cổng ngược.
Lý do dùng id thay vì đường dẫn: đổi tên hoặc di chuyển file không làm hỏng cổng, và kiểm tra tính hợp lệ được tự động.

**Kiểm tra tự động (script chạy headless):**
- Mọi `Portal` có đích tồn tại và có SpawnPoint tương ứng.
- Mọi cổng hai chiều đều có cổng ngược.
- Mọi scene trong đồ thị đều tới được từ khu bắt đầu (không có khu mồ côi).
- Không có hai SpawnPoint trùng tên trong một scene.

**Hai loại cổng:**
- **Ranh giới ngoài trời:** viền map nối sang khu kế bên (rìa đường, cổng làng).
- **Cửa vào trong:** nhà, hầm ngục, hang, có chuyển cảnh riêng.

## 5. Vật thể tương tác

Lớp cơ sở `Interactable` (Node2D/Area2D): phạm vi nhận biết người chơi, gợi ý nút (`[E]`), tín hiệu `interacted(player)`, `object_id` ổn định để lưu trạng thái.

| Vật thể | Hành vi | Trạng thái lưu |
|---|---|---|
| `Destroyable` (đã có) | đập vỡ, rơi vật phẩm | đã vỡ |
| `Chest` | mở, nhận vật phẩm | đã mở |
| `Door` | mở/đóng, có thể khoá | khoá/mở |
| `Sign` | hiện chữ | không |
| `NPC` | hội thoại, bán đồ, giao nhiệm vụ | tiến độ thoại |
| `Pickup` | nhặt vật phẩm trên đất | đã nhặt |
| `Switch` / `Lever` | bật tắt, điều khiển Door hoặc bẫy | bật/tắt |
| `Bed` / `Fireplace` | nghỉ, qua đêm, hồi sức | không |

Tương tác giữa vật thể: tín hiệu và `@export` trỏ node trong cùng scene (công tắc → cửa). Không nối chéo scene bằng node, chỉ bằng cờ trạng thái toàn cục.

## 6. Quản lý scene (code)

- `World` (đã có) thêm `travel(scene_id, spawn_id)` và giữ đúng **một** map đang nạp.
- Autoload `GameState`: cờ, vật phẩm, trạng thái theo `scene_id + object_id`, lưu ra file.
- Lớp tĩnh `WorldGraph` (`system/world/world_graph.gd`): đọc `world_graph.json`, trả đường dẫn khu. Không cần autoload.
- Nạp bằng `ResourceLoader.load_threaded_request` cho khu kế bên để chuyển cảnh không giật.
- Chỉ một khu nằm trong bộ nhớ, giải phóng khu cũ khi chuyển. Hợp với mục tiêu ít tài nguyên.

## 7. Thứ tự làm

1. **Lõi chạy được** (**xong**): `Portal`, `SpawnPoint`, `WorldGraph`, `World.travel`. Kiểm chứng bằng `tools/test_travel.gd` (hai khu thử `test_a`/`test_b`).
2. **`Interactable` + 3 vật mẫu** (**xong**): `Sign`, `Chest`, `Door`, cùng `Interactor`, `Dialog` và các nút mới trong InputMap. Kiểm chứng bằng `tools	est_interact.ps1`. Sprite cửa và biển còn là hình vẽ tạm.
3. **`GameState` + lưu trạng thái** (**xong**, gồm ghi ra đĩa qua tự lưu).
4. **Script kiểm tra đồ thị cổng** (**xong**: `tools/check_world.ps1`).
4b. **Bản đồ lớn** (**xong**: `MapScreen`, hai cấp thế giới/vùng, che khu chưa đến, `links` trong world_graph được `check_world` kiểm) và tách nút điều khiển (**xong**) (mục 9, 10). Chưa có đi nhanh.
4c. **Tự lưu theo timer** (**xong**: `SaveGame`, `Autosave`, kiểm chứng bằng `tools	est_interact.ps1 -Test save`; mục 11). Save ở `user://save.json`.
5. **Dựng khu thật** từ `map_village.tscn`: làng, nhà trong, đường ra ngoài, rồi các khu khác theo bảng asset.
   - **Đã dựng:** làng, đạo quán, Rừng Sương, hang mỏ (vùng đặc biệt), nối bằng cổng; có NPC (`Npc`, đi lang thang, nói chuyện). Xem `docs/maps/README.md`. Còn thiếu: nội thất các nhà khác, hàng rào, nhiệm vụ, nhạc theo khu.
6. Thêm `NPC`, `Switch` khi có khu cần đến.

## 8. Quyết định đã chốt

1. **Chuyển cảnh:** từng khu rời, có màn fade. Không ghép thế giới mở.
2. **Điều hướng:** vì có rất nhiều khu nên cần bản đồ lớn (mục 9).
3. **Khu đặc biệt** (hầm ngục, đạo quán, hang...) cũng là một vùng, và bên trong cũng gồm các khu rời (mục 9).
4. **Nút tương tác:** tách riêng từng hành động (mục 10).
5. **Lưu game:** bản offline tự lưu theo timer (mục 11).
6. **Danh sách khu bản chơi đầu:** chọn từ mục 4 của `asset-inventory.md`, chốt riêng.

## 9. Vùng và bản đồ lớn

**Hai cấp:** `Region` (vùng) chứa nhiều `Area` (khu, mỗi khu là một scene). Khu đặc biệt là một `Region` có cờ `special = true`, có cổng vào từ khu thường và các khu con riêng bên trong (ví dụ hầm ngục nhiều tầng).

`world_graph` mở rộng:

| Dữ liệu | Nội dung |
|---|---|
| `regions` | `region_id`, tên, `special`, khu vào mặc định, vị trí và màu trên bản đồ lớn, nhạc/môi trường mặc định |
| `areas` | `scene_id`, `region_id`, đường dẫn `.tscn`, `map_pos` (vị trí vẽ trên bản đồ), `map_icon`, `hidden_until` (cờ lộ ra) |
| `edges` | cạnh cổng như mục 4, có cờ `one_way` và `requires` |

**Giao diện bản đồ** (phím `open_map`):
1. **Cấp thế giới:** vẽ các vùng và nối giữa chúng, đánh dấu vị trí người chơi và vùng chưa khám phá bị che.
2. **Cấp vùng:** chọn một vùng để xem các khu, đường nối, cổng, điểm đặc biệt.
3. Khu chưa đến chỉ hiện sau khi đã vào hoặc nhận được tin (cờ trong `GameState`). Đã đến thì hiện tên và đường nối.
4. Bản đồ vẽ bằng dữ liệu `map_pos` đặt tay, không chụp từ scene, nên nhẹ và đẹp tuỳ ý.
5. Tuỳ chọn: đi nhanh giữa các khu đã khám phá. Chưa chốt, vì ảnh hưởng nhịp chơi.

**Kiểm tra tự động thêm:** mọi `Area` thuộc đúng một `Region`, mọi `Region` có khu vào, khu đặc biệt chỉ có cổng ngoài qua đúng cổng vào đã khai báo, mọi `map_pos` không trùng nhau.

## 10. Nút điều khiển

Mỗi hành động một nút riêng (map hành động `InputMap`, hỗ trợ bàn phím và tay cầm):

| Hành động | Dùng cho |
|---|---|
| `interact` | nói chuyện, mở cửa/rương, đọc biển, ngủ, vào cổng thủ công |
| `pickup` | nhặt vật phẩm trên đất |
| `attack` | vũ khí đang cầm (hiện có hitbox) |
| `use_tool` | búa, rìu, cuốc chim, vợt... theo công cụ đang chọn |
| `open_map`, `open_inventory`, `pause` | giao diện |

Quy tắc: `Interactable` khai báo `action` mà nó phản hồi (`interact` hoặc `pickup`) và `Interactable` cần `use_tool` khai báo loại công cụ (`axe`, `pickaxe`...). Gợi ý nút hiển thị đúng nút. Nhiều vật cùng tầm thì chọn vật gần nhất theo hướng nhìn.

## 11. Tự lưu (offline)

- Timer tự lưu mỗi **N phút** (mặc định 3, chỉnh được trong cài đặt), chỉ lưu khi an toàn: không đang chuyển cảnh, không đang trong hội thoại.
- Dữ liệu lưu: `GameState` (cờ, trạng thái vật thể theo `scene_id + object_id`, khám phá bản đồ), vị trí người chơi (`scene_id` + toạ độ hoặc spawn), túi đồ, chỉ số, thời gian trong game.
- Ghi an toàn: ghi ra file tạm rồi đổi tên, giữ 2 bản xoay vòng để một lần ghi hỏng không mất save.
- Lưu thêm khi thoát game bình thường. Hiện biểu tượng nhỏ khi đang lưu.
- Định dạng JSON có trường `version` để sau này nâng cấp save cũ.
