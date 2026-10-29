# Đề xuất bố cục: làng, Rừng Sương, đạo quán, hang mỏ

Mục tiêu: nhiều cây hơn, lối đi uốn lượn, nội thất có đồ. Mọi toạ độ là **ô tile (tx, ty)** theo quy ước `map_kit.gd`
(gốc Tilemap (8,8), `kit.center(tx,ty)`), mọi toạ độ atlas là **(cột, hàng)** đã xem trên sheet có lưới.
Bố cục làng và rừng dưới đây đã được raster bằng một mô phỏng Python (cọ 2x2, kiểm tra chồng lấn, BFS đi được);
ASCII là kết quả mô phỏng, không vẽ tay.

## 0. Hiện trạng (ảnh chụp bằng `tools/shot.ps1`)

| Cảnh | Nhận xét |
|---|---|
| village | 2 đường ngang thẳng (ty 1..2 và 12..13), nhà xếp 2 hàng, quảng trường chữ nhật; giữa map là cỏ trống; cây chỉ là 1 vòng 2x2 lặp 4 kiểu ở mép, 7 cây lẻ. |
| forest | Rừng là lưới đều các cây 2x2 (đọc như "giấy dán tường"); đường mòn gấp khúc vuông góc; khoảng trống là hình chữ nhật phẳng. |
| dojo | Phòng 18x9 ô trống, 2 kệ, 2 giá kiếm, thảm 4x4; 60% sàn không có gì. |
| cave | 4 khối quặng + 6 pha lê rải đều, không có cấu trúc, không có lối đi. |

## 1. Tile dùng được (đã xem)

Nguồn trong `tileset_village.tres`: 0 = VillageAbandoned, 1 = Floor (terrain cỏ/đất), 2 = InteriorFloor,
4 = WallSimple, 6 = House, 7 = Nature, 8 = Element, 9 = ReliefDetail. Đề xuất thêm `const ABANDONED = 0`,
`const FLOOR_DETAIL = 2` (lớp FloorDetail, z -1, nằm dưới người) vào `map_kit.gd`.

| Nhóm | Nguồn | Atlas (cột,hàng) và cỡ |
|---|---|---|
| Cây 2x2 | 7 | (0,0) xanh tròn, (2,0) thông, (4,0) cây chết, (6,0) xanh rễ lộ, (14,0) anh đào, (16,0) xanh, (18,0) xanh gốc vàng, (6,8) cây non xanh |
| Cụm cây 4x3 | 7 | (0,2) thông, (4,2) xanh, (12,2) anh đào, (16,2) xanh, (20,2) xanh gốc vàng, (0,5) cây chết |
| Cụm rêu 4x3 / cây 2x2 rêu | 0 | (0,6) và (0,9) cụm 4x3; (4,6) và (4,9) cây 2x2 |
| Cây lớn 3x3 | 7 | (0,18) anh đào, (3,18) xanh, (9,18) thu cam (bỏ (6,18) tuyết) |
| Gốc cây | 7 | (0,8) 2x2 gốc lớn, (2,8) 2x2 gốc cam, (4,8) 1x1, (5,8) khúc gỗ 1x1 |
| Bụi 1x1 | 7 | hàng 10, cột 0..10 (đang dùng) |
| Hoa 1x1 | 7 | (0,11),(1,11) hướng dương, (2,11) cỏ ba lá, (3,11) hoa đỏ, (6,11) hoa trắng, (7,11) cỏ cao |
| Đá | 7 | (13,8) nâu 2x2, (16,8) xám 2x2, (15,9) nâu 1x1, (18,9) xám 1x1, (12,5) cụm nâu 4x3, (16,5) cụm xám 4x3 |
| Quặng / pha lê | 7 | quặng lớn 2x2: (7,14),(9,14),(7,16),(9,16); nút quặng 1x1: cột 4 (đầy), 5, 6 (cạn dần) × hàng 14..17; pha lê (0..3,14) |
| Hang | 9 | cửa lò xám 3x3 (0,7), nâu 3x3 (3,7); thang xám 1x3 (3,0), cam 1x3 (3,3); đá vụn (0,0),(0,2), rêu (0,1) |
| Hàng rào gỗ | 6 | rào ray: (10,7) đầu trái, (11,7) thân lặp. Đầu phải chưa rõ: cho rào kết thúc áp vào nhà/cây |
| Đạo quán | 6 | giá kiếm 2x1 (6,4); trụ đá kem 1x2 (7,17); tượng ngồi 1x2 (7,15); 7 băng rôn 1x2 (22..28,20); giỏ vũ khí 1x2 (14,18); vòm đá 4x3 (29,20) |
| Nội thất | 8 | kệ sách 2x2 (8,7), tủ 2x2 (10,7); chậu cây 1x2 (0,7); chum cao 1x2 (0,5); bàn dài 3x1 (2,10); **chiếu rơm 3x3 (13,4)**; bó rơm 1x2 (12,4) |
| Nước (**chưa có**) | thêm | `TilesetWater.png`: ao cỏ 3x3 đứng riêng (0,6); ao 4x4 có đảo cỏ (4,6); suối dọc 1x3 (3,6); suối ngang 3x1 (0,9) |

**Cần thêm sheet** (chỉ cho ao, tuỳ chọn): chép `assets/_src/ninja/Backgrounds/Tilesets/TilesetWater.png` thành
`game/content/map/tileset_water.png`, thêm `10: "res://content/map/tileset_water.png"` vào `SOURCES` của
`tools/build_village_tileset.gd`, chạy lại. Mọi thứ khác đã có trong tileset.

## 2. Hàm trợ giúp đề xuất cho `map_kit.gd`

```gdscript
## Ô đất của đường gấp khúc: cọ w x w đặt (góc trên trái) ở mỗi bước Bresenham, bước 4-liên thông
## (đổi x rồi mới đổi y), nên đoạn chéo thành bậc thang mỗi hàng 3 ô, không bao giờ 1 ô.
func path_cells(points: Array, w := 2) -> Dictionary            # Vector2i -> true
func ellipse_cells(c: Vector2, r: Vector2) -> Dictionary         # ô có tâm nằm trong elip
## Làm mượt: lấp ô cỏ có >= 3 láng giềng đất; bỏ mỏm đất có <= 1 láng giềng (trừ khi fill_only).
func smooth_dirt(d: Dictionary, passes := 2, fill_only := false) -> Dictionary
## Băm xác định: thêm/bớt một cây không làm xê dịch cây khác (khác RandomNumberGenerator theo thứ tự gọi).
static func hash01(x: int, y: int, seed: int) -> float
## Bảng chiếm chỗ dùng chung (nhà, cây, đá, vật tương tác).
var occupied := {}
func reserve(tx: int, ty: int, w: int, h: int) -> void
func is_free(tx: int, ty: int, w: int, h: int, avoid: Dictionary) -> bool
## Đặt một loại cây theo bảng TREES (bên dưới): stamp + reserve + (tuỳ) collide_base.
func plant(kind: StringName, tx: int, ty: int, collide := true) -> void
## Rải cây trên lưới bước `step`, hàng lẻ lệch 1 ô (so le), mỗi điểm giữ với xác suất `chance`,
## chọn loại theo trọng số; không vừa thì thử 2x2; trả về danh sách đã đặt.
func scatter(area: Dictionary, avoid: Dictionary, seed: int, kinds: Array, step := 2, chance := 1.0, collide := true) -> Array
## Bụi/hoa 1x1 ở ô cỏ kề trên/dưới đường (lớp FloorDetail cho hoa, Wall2 cho bụi), không va chạm.
func edge_decor(dirt: Dictionary, avoid: Dictionary, seed: int, chance := 0.12) -> void
## Va chạm từ mặt nạ ô bị chặn: gộp đoạn ngang mỗi hàng, rồi gộp dọc các đoạn trùng [x0,x1] ở hàng liền kề.
func collide_mask(blocked: Dictionary) -> int                   # trả số hình chữ nhật
```

Bảng `TREES` (khoá → nguồn, atlas, cỡ, va chạm):

| Khoá | Nguồn/atlas | Cỡ | Va chạm |
|---|---|---|---|
| `t_green` `t_pine` `t_green2` `t_gold` `t_pink` `t_young` | 7: (0,0) (2,0) (16,0) (18,0) (14,0) (6,8) | 2x2 | `collide_base(tx,ty,2,2)` (như hiện tại) |
| `t_dead`, `t_moss` | 7:(4,0); 0:(4,6) | 2x2 | như trên; chỉ dùng trong rừng |
| `c_pine` `c_green` `c_green2` `c_pink` `c_moss` | 7:(0,2) (4,2) (16,2) (12,2); 0:(0,6) | 4x3 | `collide_base(tx,ty,4,3, 6.0)` |
| `big_green` `big_pink` `big_autumn` | 7:(3,18) (0,18) (9,18) | 3x3 | `collide_base(tx,ty,3,3, 14.0)` (thân hẹp) |

Quy tắc chung: **chạy `smooth_dirt` trên đường chính và quảng trường TRƯỚC khi nối lối cửa nhà**, sau đó
chỉ chạy lại với `fill_only = true` (nếu không, lối cửa 2x1 bị coi là mỏm và bị xoá; đã gặp trong mô phỏng).
Mọi lối/đường rộng **2 ô**; không đoạn nào chạy dọc ty 5/6 hay tx ±10/11, 30/31, 50/51 (chỉ được cắt ngang).
Sau khi dựng, script nên tự BFS từ điểm xuất hiện đến mọi cổng/cửa và `push_error` nếu không tới được.

## 3. Làng (village, 3x2 màn hình, tx -29..30, ty -5..16)

### 3a. Bố cục (kết quả mô phỏng)

```
     987654321098765432109876543210123456789012345678901234567890   (chữ số hàng đơn vị của tx, từ -29)
  -5 ........tttttt......tt......tttttttttttt......tttt........tt
  -4 ........tttttt......tt......tttttttttttt......tttt..HHHH..tt
  -3 ..HHHH.........HHHH....HHHH...............HHH.......HHHH....
  -2 ..HHHH..HHH....HHHH....HHHH...HHHH........HHH..HHH..HDHH....
  -1 ..HDHH..HHH....HHDH....HDHH...HHHH........HDH..HHH...==.....
   0 ...==...HDH......==.....==....HDHH.........==..HDH...==NS...
   1 tt.========......=========N=...==.....f...======N=..f====GPG
   2 tt.====N====..===================.....===================GPG
   3 tttttt==============AAA.tt==================..============..
   4 tttttt....======b...AAA.tt..N===========tt..tttttt======.f..
   5 .tt......tttttttt...AAAtt...===.f..b.AAAtt..ttttttKKK===.tt.
   6 .tt......tttttttt......ttf..===......AAA.....tttt.KKK.===tt.
   7 tt######.................======S==.f.AAA.....tttt.KKK.===.tt
   8 tt.HHHH...######WWWW...=====@=======.....HHH...........==.tt
   9 ...HHHHKKK.HHHH.WWWW..==========N====b...HHH.....HHH...==...
  10 ...HDHHKKK.HHHH.WWWW..================...HDH.....HHH...==...
  11 tt..==.KKK.HDHH.WWWW==============X=====..==N....HDH..===.tt
  12 tt..======..==...==N====tt..........===========...==.====.tt
  13 ..====================..tt............==================....
  14 ..====..===========..........................==========.....
  15 tttttttttttttttttttttttttttttttttttttttttttttttttttttttttttt
  16 tttttttttttttttttttttttttttttttttttttttttttttttttttttttttttt
```
`.` cỏ, `=` đất, `H` nhà, `D` cửa, `G/P` torii/vùng cổng, `t` vùng cây 2x2, `A` anh đào lớn 3x3, `K` cây xanh lớn 3x3,
`W` ao 4x4, `#` rào, `N` NPC, `S` biển, `X` rương, `@` start, `f/b` hoa/bụi. Các `.` còn lại quanh nhà là cố ý (sân).

### 3b. Nhà (dời nhẹ để phá hàng thẳng; **giữ nguyên** bà Lan và đạo quán vì test)

| Nhà | Hiện tại | Đề xuất | Lý do |
|---|---|---|---|
| orange bà Lan | (-27,-3) | giữ | `test_village` đo tường/cửa ở (-24,0), (-26,0) |
| shop | (-20,-2) | (-21,-2) | giãn khoảng với quán trọ |
| inn | (-13,-3) | (-14,-3) | |
| dojo | (-6,-3) | giữ | cửa (-5,-1) = cổng đạo quán, `test_journey` đi tới (-64,0) |
| orange2 ông Tám | (2,-2) | (1,-2) | |
| clay lò gốm | (10,-3) | (13,-3) | lối cửa cũ ở tx 10..11 nằm **đúng ranh giới màn hình** |
| cabin | (17,-2) | (18,-2) | |
| red trưởng làng | (24,-4) | (23,-4) | chừa chỗ Noble/biển ở (26..27,0) |
| red kho thóc | (-25,8) | (-26,8) | |
| orange2 chú Bảy | (-17,8) | (-18,9) | lệch hàng |
| clay lò rèn | (9,8) | (12,8) | lối cửa cũ ở tx 10..11 (ranh giới) |
| cabin bỏ trống | (18,8) | (20,9) | lệch hàng |

Tên biển cửa `Door_<loại>_<tx>` đổi theo tx mới; biển không lưu trạng thái nên chỉ cần tên vẫn duy nhất.

### 3c. Đường (cọ 2x2, toạ độ góc trên trái)

```gdscript
const MAIN    = [[-26,1],[-25,1],[-22,2],[-18,3],[-14,2],[-10,1],[-6,1],[-2,2],[2,3],[6,3],
                 [10,2],[14,1],[18,2],[22,3],[25,2],[28,1]]          # phủ ty 1..4, sóng biên độ 2 hàng, chu kỳ ~8 ô
const LINK    = [[-1,3],[-1,5],[0,6],[0,7]]                           # xuống quảng trường, cắt ty 5/6 vuông góc
const SOUTH_W = [[-27,13],[-24,12],[-20,13],[-15,13],[-11,12],[-7,10]] # vào quảng trường từ tây nam
const SOUTH_E = [[6,10],[9,12],[13,12],[17,13],[21,13],[25,12],[26,10],[26,7],[25,4],[24,3]]  # vòng đông nối lại đường chính
const PLAZA   = ellipse_cells(Vector2(0.5, 9.5), Vector2(7.5, 2.6))  # tx -7..8, ty 7..11, góc bo tròn
```
Cách vẽ: `dirt = smooth(MAIN ∪ LINK ∪ SOUTH_W ∪ SOUTH_E ∪ PLAZA)`; rồi lối cửa (như `dirt_cells()` hiện tại:
2 ô từ ô dưới cửa xuống tới khi gặp đất); rồi `smooth(fill_only)`; cắt bỏ ô ngoài tx -28..29;
`paint_ground(X0,Y0,X1,Y1, dirt)`. Kết quả: đường chính uốn giữa ty 1..4 (không chạm ty 5), vòng đông đi xuống
ở tx 24..27 (cắt ranh giới ty 5/6 thẳng đứng, đúng quy tắc), không đoạn dọc nào nằm ở tx -10/-9 hay 10/11.
Mô phỏng: BFS từ start (-1,8) tới cửa đạo quán, cửa bà Lan, cổng torii, (0,13), (-24,0) đều tới được.

### 3d. Cây theo vùng

| Vùng | Ô | Cách đặt | Ước lượng |
|---|---|---|---|
| Vành bắc | ty -5..-4 ngoài chân nhà | `scatter(step 2, chance 1.0)`, 2x2 trộn `t_green,t_pine,t_green2,t_gold` | ~20 |
| Vành nam | ty 15..16 | như trên, thêm 1/7 `c_green` khi vừa | 30 |
| Vành tây/đông | tx -29..-28, 29..30; lớp hai tx -27..-26 và 27..28 ở ty 4..9 | như trên | ~14 |
| Vườn giữa hai đường | `Rect2i(-27,3,16,5)`, `(-11,3,8,4)`, `(7,4,16,4)`, `(14,6,6,3)` | `scatter(step 2, chance 0.85, seed 31+i)`, `t_*` + `c_green` trọng số 6:1 | ~16 |
| Quanh nhà | 1 ô quanh chân nhà + 2 hàng trước cửa | **cấm** (avoid) | 0 |
| Điểm nhấn | `big_pink` (-9,3) và (8,5); `big_green` (-22,9) và (21,5) | `plant()` tay | 4 |
| Viền đường | ô cỏ kề trên/dưới đất | `edge_decor(chance 0.12)`: 60% hoa (FloorDetail), 40% bụi (Wall2) | ~10 |

`avoid` = đất ∪ (chân nhà nở 1 ô) ∪ (ô 4x2 trước mỗi cửa) ∪ (mọi NPC/biển/rương/spawn nở 1 ô) ∪ `Rect2i(26,0,5,4)`
(torii) ∪ `Rect2i(-27,-1,4,2)` và `Rect2i(-1,12,3,3)` (hai ô test). Gốc cây cũ (-10,4) bỏ (vướng anh đào lớn).

### 3e. Điểm nhấn và vật

- **Ao có đảo** (cần TilesetWater): stamp 10:(4,6) 4x4 vào lớp Floor tại (-13,8); va chạm `collide_block(-13,8,4,4)`.
  Không thêm sheet thì thay bằng `big_green` (-12,8) + đá 7:(13,8) ở (-10,10).
- **Rào sân thóc**: 6:(10,7) ở (-27,7), 6:(11,7) ở (-26..-22,7); rào sau nhà chú Bảy 6:(10,7) (-19,8), (11,7) (-18..-14,8).
  Va chạm 1 dải mỗi hàng rào: `collide_rect(Rect2(corner(x0,y)+Vector2(0,8), Vector2(w*16,6)))`.
- **Bó rơm** 8:(12,4) 1x2 ở (-21,9) cạnh kho thóc (va chạm hàng dưới).
- Bình vỡ: giữ Pot1 (-24,0), Pot2 (1,3); dời Pot3 → (22,0), Pot4 → (-21,8) (chỗ cũ là cây lớn), Pot5 → (15,10).
- NPC giữ chỗ, trừ OldMan (14,12) → (15,11) (trước lò rèn mới). Chest_plaza (5,11), thùng (-5..-4,10..11), biển (2,7) giữ.

## 4. Rừng Sương (forest, 4x2 màn hình, tx -9..70, ty -5..16)

Bỏ lưới "ô lớn" 2x2: rừng = mọi ô không thuộc (đường ∪ bãi trống); đường là đường gấp khúc ở mức ô tile.

### 4a. Bố cục (kết quả mô phỏng)

```
     98765432101234567890123456789012345678901234567890123456789012345678901234567890   (từ tx -9)
  -5 ttCCCCttttttttttttttbbbbttttttttCCCCCCCCttttttCCCCttttCCCCttttttttbbbbtttttttttt
  -4 ttCCCCttttttttttttttb,,,ttttttttCCCCCCCCttttttCCCCttttCCCCttttttttRRRRtttttttttt
  -3 bbCCCCbttCCCCttbAAAb,,,,,ttttttbCCCCCCCCbttttbCCCCbttbCCCCbCCCCttbRRRRMMMttttttb
  -2 bbbbbbbttCCCCttbAAAb,,,X,ttttttbbbbbbbbbbttttbbbbbbttbbbbbbCCCCttbRRRRMMMttttttb
  -1 ttttttttbCCCCbttAAAbbS,,b,,bttttttttttttttttttttttttttttttbCCCCbtttt,,MPM,rrtttt
   0 ttttttttbbbbbbttbbbbbbbbb,,bttttttttttttttttttttttttttttttbbbbbbtttt,,==,,rrtttt
   1 bbb,,,S,,bb=======bbbttbb====bbttttttttttttCCCCttttttbb=====bbbbbttb===@,,bttttb
   2 bb,,,,,,N===========btt========ttttttttttttCCCCtttttt===========btt=====bbbttttb
   3 GPG=@========btt================ttttttttttbCCCCbttb======b============CCCCCCCCtt
   4 GPG========bbbttbb=======bbbb===ttttttttttbbbbbbttb====bbbbbbb=======bCCCCCCCCtt
   5 bttttttCCCCttttCCCCttttttttttb==bbbttttbbbbttttttb===ttttCCCCttb====,,CCCCCCCCbb
   6 bttttttCCCCttttCCCCttttttttttb===b,ttttAAAbttttttb===ttttCCCCttb,===,,,,,bbbbbbb
   7 ttCCCCbCCCCbttbCCCCbCCCCtttttt===,,,WWWAAAttttttbb==ttttbCCCCrr,,,=N,,,,,,ttCCCC
   8 ttCCCCbbbbbbttbbbbbbCCCCtttttt,==,,,WWWAAAttttttb===ttttbbbKKKr,,,,,,,,,,,ttCCCC
   9 bbCCCCbttCCCCttttttbCCCCbbbbb,X==N,,WWW,bbbb,bbb====bCCCCttKKKb,,,,,,,,,,,bbCCCC
  10 bbbbbbbttCCCCttttttbbbbbbbKKKb,,,=====,bb,,,,,=====bbCCCCttKKKbb,,,,,,,,,bbbbbbb
  11 ttttttttbCCCCbttttttttttttKKKbb,,=========,,======ttbCCCCbttttttttb,,,CCCCCCCCtt
  12 ttttttttbbbbbbttttttttttttKKKbbbbb,b============bbttbbbbbbttttttttbbbbCCCCCCCCtt
  13 bttttttttCCCCttCCCCttCCCCttttttttttttttb======bttCCCCCCCCttttttttttttbCCCCCCCCbb
  14 bttttttttCCCCttCCCCttCCCCttttttttttttttbbbbbbbbttCCCCCCCCttttttttttttbbbbbbbbbbb
  15 ttttttttbCCCCbbCCCCbbCCCCbttttttttttttttttttttttbCCCCCCCCbtttttttttttttttttttttt
  16 ttttttttbbbbbbbbbbbbbbbbbbttttttttttttttttttttttbbbbbbbbbbtttttttttttttttttttttt
```
`,` cỏ trống (bãi), `=` đường đất, `t` cây 2x2, `C` cụm 4x3, `A/K` cây lớn 3x3, `b` ô lấp (bụi/gốc/đá 1x1),
`R/r` cụm đá 4x3 / đá 2x2, `M` cửa hang, `W` ao 3x3. Mô phỏng: 204 cây (25 cụm 4x3), mọi điểm đích đi tới được.

### 4b. Đường và bãi trống

```gdscript
const MAIN_A = [[-7,3],[-2,3],[2,1],[6,1],[9,3],[13,3],[16,1],[19,2],[21,4],[22,8]]  # cổng -> Bãi Sương
const LINK   = [[24,10],[28,11]]
const MAIN_B = [[28,11],[33,12],[38,10],[41,7],[42,3],[46,1],[51,2],[55,4],[58,2],[61,0]]  # -> sân cửa hang
const SPUR_E = [[55,4],[57,6]]                                    # nhánh xuống Bãi Đá
# Bãi trống (cỏ, không đất): elip (tâm, bán kính) hoặc chữ nhật
const GLADES = [
	[Vector2(-3,3),    Vector2(4.5,2.2)],   # bãi cổng: Girl, biển
	[Vector2(25.5,9.5),Vector2(5.0,3.0)],   # Bãi Sương: Hunter, rương, ao
	[Vector2(59.5,8.5),Vector2(5.5,3.2)],   # Bãi Đá: OldMan
	[Vector2(13.5,-2), Vector2(3.0,1.6)],   # ngách bí mật (tx 11..16, ty -4..-1)
	[Vector2(35.5,11.5),Vector2(3.5,2.0)],  # chỗ phình giữa đường (nghỉ chân)
]
const OPEN_RECTS = [Rect2i(16,-1,2,2), Rect2i(59,-1,6,3)]  # khe cỏ vào ngách; sân trước cửa hang
```
Đường uốn lên/xuống qua cả hai hàng màn hình (cắt ty 5/6 ở tx 21..23, 41..43, 55..58: đều cắt ngang, không
chạy dọc). Ngách bí mật **không có đất**: lối vào là khe cỏ 2 ô (16..17,-1..0) giữa hai bụi cây, nhìn từ đường như
một chỗ thưa. Hết cảnh gấp khúc vuông góc: mọi đoạn là chéo 1:2..1:4.

### 4c. Cây và va chạm

1. Điểm nhấn (đặt trước): `big_pink` (7,-3) (canh ngách), `big_green` (17,10), `big_pink` (30,6) (bên ao),
   `big_green` (50,8); cụm đá xám 7:(16,5) 4x3 ở (57,-4) cạnh cửa hang, đá xám 7:(16,8) 2x2 ở (65,-1).
2. Khối rừng: `scatter(mass, avoid = đường ∪ bãi ∪ POI, seed 7, step 2, chance 1.0)`, loại theo trọng số
   7 `t_*` : 2 `c_*`. Theo vùng: tx -9..10 (gần làng) dùng `t_green,t_gold,c_green,c_green2`; tx 11..50 thêm
   `t_pine,c_pine,c_moss,t_moss`; tx 51..70 (gần hang) thêm `t_dead`, `c_pine`, 1/6 `t_pink`. Không dùng cây tuyết.
3. Ô còn trống trong khối rừng (dải 1 hàng dưới cụm 4x3, ~330 ô): lấp bằng `hash01` → 60% bụi hàng 10, 20% gốc
   (4,8), 10% khúc gỗ (5,8), 10% đá (15,9)/(18,9); lớp Wall2, không va chạm riêng.
4. Viền bãi trống: `edge_decor` với hoa (0,11),(3,11),(6,11) dày hơn (chance 0.2); thêm vài bụi lẻ trong bãi (hash, 1/12 ô).
5. **Va chạm theo mặt nạ** (không theo từng cây): `blocked = ô không thuộc (đường ∪ bãi)` trừ các ô có ô ngay phía
   trên là ô đi được (cho người chơi lấn 1 hàng vào tán cây phía nam, y-sort che đúng). `collide_mask(blocked)`
   → mô phỏng ra 73 đoạn ngang, ~52 hình sau khi gộp dọc. Cộng torii 2, cửa hang 3, ao 1, đá 2, cây lớn 4, biên 4
   ≈ **68 hình** (hiện tại ~55). Cây điểm nhấn trong bãi trống vẫn dùng `collide_base`.

### 4d. Vật, NPC, cổng

| Thứ | Hiện tại | Đề xuất |
|---|---|---|
| Torii + Portal_village + from_village | (-9,3), (-8,3..4), (-5,3)+8 | giữ (test_journey) |
| Cửa hang + Portal_cave + from_cave | (61,-3), (62,-1), (62,1) | giữ; đường kết thúc ở (61..62, 0..1) |
| Sign_trail | (-3,2) | giữ (trong bãi cổng) |
| Girl | (-1,2) r30 | giữ |
| Chest_nook / Sign_nook | (12,-2) / (11,-1) | (14,-2) / (12,-1) (cùng tên) |
| Hunter | (24,5) | (24,9) r20, trong Bãi Sương |
| Chest_glade | (28,5) | (21,9) |
| OldMan | (58,7) | giữ (Bãi Đá) |
| Ao (tuỳ chọn, TilesetWater) | – | 10:(0,6) 3x3 ở (27,7), va chạm `collide_block(27,7,3,3)` |

## 5. Đạo quán (dojo, 1 màn hình)

```
     98765432101234567890   (tx -9..10)
  -5 ####################
  -4 #BBf.kkl.SSlkk.f.BB#
  -3 #BBf...l.NNl...f.BB#
  -2 #.......cccc.......#
  -1 #p......cccc.hhh..p#
   0 #phhh...cccc.hNh..p#
   1 #.hNh...cccc.hhh...#
   2 #whhh..............#
   3 #wX......@....TTT.j#
   4 #.................j#
   5 #########PP#########
```

| Ký hiệu | Vật | Nguồn:atlas, cỡ | Ô | Lớp / va chạm |
|---|---|---|---|---|
| B | kệ sách, tủ (giữ) | 8:(8,7), 8:(10,7) 2x2 | (-8,-4), (8,-4) | Wall2, `collide_block` |
| k | giá kiếm (giữ) | 6:(6,4) 2x1 | (-4,-4), (3,-4) | Wall2, `collide_block` |
| l | trụ đá kem hai bên sư phụ | 6:(7,17) 1x2 | (-2,-4), (2,-4) | Wall, `collide_block(x,-3,1,1)` |
| f | băng rôn | 6:(22,20), 6:(26,20) 1x2 | (-6,-4), (6,-4) | Wall2, `collide_block(x,-3,1,1)` |
| c | thảm (giữ) | 2:(15,0) 4x4 | (-1,-2) | Floor |
| h | chiếu rơm tập luyện | 8:(13,4) 3x3 | (-7,0), (4,-1) | FloorDetail, không va chạm |
| p | chậu cây | 8:(0,7) 1x2 | (-8,-1), (9,-1) | Wall, `collide_block(x,0,1,1)` |
| w | giỏ vũ khí | 6:(14,18) 1x2 | (-8,2) | Wall, `collide_block(-8,3,1,1)` |
| T | bàn trà | 8:(2,10) 3x1 | (5,3) | Wall, `collide_block(5,3,3,1)` |
| j | chum cao | 8:(0,5) 1x2 | (9,3) | Wall, `collide_block(9,4,1,1)` |

NPC: Master giữ (0,-3)+8 (test nói chuyện từ (24,-14)); Monk1 (-5,1) → (-6,1) r12, Monk2 (6,1) → (5,0) r12
(đứng trên chiếu). Sign_rules, Chest_dojo (-7,3), spawn, cổng: giữ. Thêm ~9 va chạm. Mô phỏng: từ spawn tới trước
sư phụ, rương, bàn trà đều đi được. Nếu muốn ấm trà trên bàn: 8:(1,10) trông như ấm chén nhưng cần xem lại cho chắc.

## 6. Hang mỏ (cave, 1 màn hình)

Ý tưởng: một **gờ đá** chắn ngang giữa phòng (ty 0..1) biến phòng trống thành lối chữ S: vào từ cửa dưới →
sang đông → lên → quay tây tới rương; góc tây nam thành hốc nhỏ có biển và quặng.

```
     98765432101234567890   (tx -9..10)
  -5 ####################
  -4 #OOMMM..o...o.o.L..#
  -3 #OOMMM...XX.....L..#
  -2 #..MMM.......,..L.o#
  -1 #.....,............#
   0 #RRRRrrOOrrOO.....,#
   1 #RRRRrrOOrrOO.N....#
   2 #RRRR............OO#
   3 #o.....S.@.....,.OO#
   4 #.o.,.o............#
   5 #########PP#########
```

| Ký hiệu | Vật | Nguồn:atlas, cỡ | Ô |
|---|---|---|---|
| M | cửa lò cũ (trang trí; sau này có thể làm cổng "hang sâu") | 9:(0,7) 3x3 | (-6,-4) |
| L | thang xám dựa vách | 9:(3,0) 1x3 | (7,-4) |
| R | cụm đá xám (đầu gờ) | 7:(16,5) 4x3 | (-8,0) |
| r | đá xám, đá nâu 2x2 | 7:(16,8), 7:(13,8) | (-4,0), (0,0) |
| O | quặng lớn 2x2 | 7:(7,16) (-2,0); 7:(9,16) (2,0); 7:(7,14) (-8,-4); 7:(9,14) (8,2) | |
| o | nút quặng / pha lê 1x1 | 7: cột 4..6 × hàng 14..17 (trộn trạng thái), 7:(0..3,14) | (-8,3),(-7,4),(-3,4),(5,-4),(9,-2),(3,-4),(-1,-4) |
| , | đá vụn, rêu (FloorDetail, không va chạm) | 9:(0,0),(0,2),(0,1),(0,5) | (-5,4),(6,3),(-3,-1),(4,-2),(9,0) |

Va chạm: gờ gộp thành 2 hình (`Rect` tx -8..3 ty 0..1, và tx -8..-5 ty 2), cửa lò 1, thang 1, quặng lớn rời 2,
nút quặng không va chạm hoặc `collide_base` nhỏ: **+6 đến +13** hình. Chest_cave (0,-3)+8, Sign_mine (-2,3),
spawn, cổng giữ; Miner (3,2) → (5,1) r12, đứng ở lối qua gờ ("Lối này, cẩn thận đầu"). Mô phỏng: rương, biển,
hốc tây nam, cửa lò, lối ra đều đi tới được.

## 7. Độ khó và số va chạm

| Cảnh | Việc | Độ khó | Va chạm hiện tại → đề xuất |
|---|---|---|---|
| map_kit | 9 hàm ở mục 2 + bảng TREES | vừa (~200 dòng) | – |
| forest | viết lại `_initialize/paint/plant`; dữ liệu mục 4 | vừa | ~55 → ~68 (gộp theo mặt nạ) |
| village | sửa dữ liệu HOUSES, `dirt_cells`, thay `plant_trees` bằng vùng | vừa | ~119 → ~80 (gộp vành cây theo hàng bằng `collide_mask`, cây rải trong làng giữ `collide_base`) |
| dojo | thêm ~10 lệnh stamp | dễ | 8 → ~17 |
| cave | thêm ~20 lệnh stamp | dễ | 10 → ~20 |

Số ô tile tăng (rừng ~1.700 ô Wall/Wall2) không đáng kể với TileMap; hình va chạm giữ dưới 100 mỗi khu.

## 8. Thứ tự triển khai khuyến nghị

1. `map_kit.gd`: `path_cells`, `ellipse_cells`, `smooth_dirt`, `hash01`, bảng chiếm chỗ, `plant`, `scatter`,
   `edge_decor`, `collide_mask`, hằng `ABANDONED`, `FLOOR_DETAIL`; thêm hàm BFS kiểm tra đi được.
2. **Rừng** (lợi nhiều nhất, ít ràng buộc test): chạy, chụp `-Camera "504,104,1.0"` và zoom từng màn hình
   (`"184,16,2"`, `"504,16,2"`...), `check_world`, `test_interact -Test journey`.
3. **Làng**: dời nhà, đường mới, cây theo vùng; `test_interact -Test village` và `journey`.
4. **Hang**, rồi **đạo quán** (chỉ thêm stamp).
5. Tuỳ chọn: thêm `TilesetWater` (mục 1) rồi ao làng (-13,8) và ao rừng (27,7).

## 9. Ba rủi ro lớn nhất

1. **Đường đất xấu do terrain**: terrain cỏ/đất là kiểu khớp góc+cạnh; một ô mỏm, một lỗ cỏ 1 ô, hay hai đoạn
   chỉ chạm góc sẽ ra ô viền lỗi. Phòng: cọ 2x2 bước 4-liên thông, `smooth_dirt` trước khi nối lối cửa rồi chỉ
   lấp (mô phỏng đã bắt được cả hai lỗi: lỗ cỏ ở (-5,11), (-21,1) và lối cửa quán trọ bị xoá). Luôn chụp ảnh zoom 2 kiểm tra.
2. **Cây che hoặc chặn thứ quan trọng**: tán 2 hàng của cây y-sort trước NPC/biển/rương/cửa, hoặc va chạm mặt nạ
   chặn mất ô trước cổng (torii có cột chặn ở hàng 2). Phòng: `avoid` nở 1 ô quanh mọi POI và 2 hàng trước cửa,
   BFS trong script dựng map, `check_world` + `test_journey` + `test_village` (giữ trống (-24,0), (-26,0), (0,13)).
3. **Số va chạm và cảm giác "vấp lá"**: rải cây tự do với `collide_base` từng cây sẽ đẩy rừng lên ~250 hình và tạo
   khe hẹp 1 ô kẹt người chơi. Phòng: rừng dùng `collide_mask` (~52 hình), vành cây làng gộp theo hàng, không để khe
   cỏ dưới 2 ô giữa hai cây (scatter lưới bước 2 đảm bảo; ô lẻ lấp bằng bụi). Phụ: cỏ viền ao của TilesetWater có
   thể lệch màu với cỏ TilesetFloor, xem ảnh trước khi giữ ao.
