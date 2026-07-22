# Ninja Adventure — lộ trình

Thiết kế: [GAME.md](GAME.md) · Kỹ thuật: [ARCHITECTURE.md](ARCHITECTURE.md)

> **Đọc [GAME.md §0](GAME.md) và [ARCHITECTURE.md §0](ARCHITECTURE.md) trước.** GAME §0 là triết lý
> (chill mặc định, thử thách tuỳ chọn) — hàng rào để đánh giá mọi tính năng mới. ARCH §0 là 8 chỗ
> thiết kế sai đã sửa, trong đó `BuildKind::kCore`, điều kiện thua và nguồn spawn phải gỡ khỏi code.

---

## Ba nguyên tắc sắp xếp

**1. Mỗi giai đoạn kết thúc bằng một game chơi được.** Không có giai đoạn nào mà sản phẩm cuối là
"một hệ thống chưa nối vào đâu". Đây là hàng rào chính chống lại rủi ro phạm vi.

**2. Dựng hình dạng multiplayer từ đầu, kể cả khi chưa có người chơi thứ hai.** `PlayerActor` hiện
là singleton `key = 1`. Nếu xây chiến đấu, túi đồ và chế tạo lên trên một singleton rồi mới thêm
multiplayer ở giai đoạn 5, sẽ phải viết lại cả ba. Đổi thành *keyed theo tài khoản* ngay ở P1 tốn
gần như không gì; đổi ở P5 tốn hàng tuần.

Áp dụng tương tự cho: *interest set* (client chỉ nhận chunk quanh mình — hiện renderer đọc toàn bộ
`SnapshotBus`), và mọi verb người chơi phải mang `player_id` ngay từ bây giờ.

**3. Việc rủi ro nhất đặt sau cùng, nhưng đường lui phải chuẩn bị trước.** RL để cuối. Thiết kế
game đã có phương án thay thế (bảng hành vi theo "thế hệ") để nếu RL thất bại thì bạn vẫn có game.

**4. Gỡ thiết kế sai trước khi xây lên trên nó.** Core / điều kiện thua / trại spawn phải bỏ ở P0–P2,
không phải "dọn sau" — mỗi tính năng xây đè lên chúng làm việc gỡ đắt gấp đôi.

---

## P0 — Dọn nền và vỏ giao diện — **XONG**

Chuyển từ "demo" sang "game", và dựng cái vỏ mà mọi hệ thống sau sẽ cắm vào.

| | Trạng thái |
|---|---|
| Giấy phép MIT (code) + ghi rõ art là CC0 riêng | ✅ |
| Gỡ `BuildKind::kCore`, `core_hp`, điều kiện thua | ✅ thay bằng `kHearth` — lò sưởi, mất thì xây lại |
| Nhân vật + quái Ninja Adventure, 4 hướng × 4 frame | ✅ `facing` là state của sim, không phải suy đoán ở renderer |
| raygui + menu (Chính / Tạm dừng / Nhật ký / Tuỳ chọn) | ✅ theme tối |
| HUD rút gọn: máu, hotbar, đồng hồ ngày | ✅ hướng dẫn chuyển vào Nhật ký |
| Chỉ số kỹ thuật → overlay `F3` | ✅ |
| Âm thanh (`raudio`) | ✅ tự im lặng khi không có sound device (**chưa nghe thử được trên máy build headless**) |

Còn nợ lại sang sau: gán lại phím (màn hình Tuỳ chọn mới chỉ là khung), và atlas UI riêng từ Kenney
UI Pack — raygui vẽ được menu bằng hình học nên chưa cần, để khi làm túi đồ/chế tạo (P4).

---

## Về câu hỏi "P1 hay P2 trước" — câu trả lời là **tách P2 ra làm đôi**

Bạn hỏi nên làm chiến đấu trước hay thế giới trước. Sau khi chốt 1024×1024 thì cả hai đáp án gốc đều
dở, vì một lý do cụ thể:

> **Cân bằng chiến đấu là hàm của thế giới nó diễn ra trong đó** — quãng đường di chuyển, mật độ
> quái, độ khó theo vòng. Nếu xây chiến đấu trên bản đồ 256×256 với 5 trại spawn và không có làng,
> rồi chuyển sang 1024×1024 có vòng đồng tâm và 50 làng, **bạn phải tune lại từ đầu.** Tune combat
> hai lần là lãng phí đắt nhất trong cả lộ trình.

Nhưng làm trọn vẹn P2 trước cũng sai: nó kết thúc bằng *một thế giới đẹp mà bạn không đánh nhau
được*. Đó không phải "game chơi được", vi phạm nguyên tắc 1.

**Lời giải: chia thế giới thành bộ khung và hệ thống.**

| | Nội dung | Vì sao ở đây |
|---|---|---|
| **P1 khung** | địa hình, vòng, worldgen đặt làng/cứ điểm/cổng, đường đi, di chuyển | Là *sân khấu*. Combat cần biết sân khấu thật rộng bao nhiêu |
| **P2 combat** | nhân vật, kỹ năng, phe, combo | Tune một lần, trên bản đồ thật |
| **P3 hệ thống** | làng lên bậc, địa giới, luật thích nghi quần xã | Không ảnh hưởng cân bằng combat → làm sau an toàn |

Ở P1, làng chỉ là **nhà cửa và người đứng đó** — chưa lên bậc, chưa quân đội, chưa nhiệm vụ. Đủ để
bản đồ có hồn và để combat biết mình đang cân bằng cho thế giới nào.

---

## P1 — Bộ khung thế giới — **XONG**

Dựng sân khấu thật, trước khi tune bất cứ thứ gì lên trên nó.

| | Trạng thái |
|---|---|
| **Công cụ xuất bản đồ** `mmo_worldmap` — PNG full-size, màu phẳng, thống kê vòng/địa hình | ✅ làm đầu tiên: không nhìn được bản đồ thì không tune được worldgen |
| 1024×1024 ô = **1024 chunk actor** | ✅ 700 tick chạy trong **1,9 giây** |
| 5 vòng đồng tâm + 3 địa hình mới (tuyết, đầm lầy, tro) | ✅ |
| Gỡ `kMapCount = 3` → một mặt đất `kOverworld` | ✅ |
| Thống nhất art: **địa hình + cây** sang Ninja Adventure | ✅ 9 loại địa hình, cây 2 mảnh mọc trên nền đúng vòng |
| **Thế giới đầu tiên 100% Ninja Adventure** | ✅ không còn ô Kenney nào trong world |
| Ảnh từng quần xã (`--ring N`) | ✅ `docs/biomes/` |
| **Worldgen đặt làng / cứ điểm / đường** | ✅ 51 làng, 27 cứ điểm, 522 công trình, `src/world/worldgen.hpp` |
| **Gỡ nông trại khởi đầu + tường/tháp/rào đặt-từng-ô** | ✅ `BuildKind` còn 2 giá trị |
| **Lớp không khí**: lá bay / mưa / tuyết theo vòng | ✅ 0 state, 0 message |
| **BFS đa nguồn** (tới làng gần nhất) | ✅ một lượt quét, không phải một field mỗi làng |
| **LOD chunk** | ✅ chunk rỗng chỉ publish 32 tick một lần |
| Đột kích ngẫu nhiên từ cứ điểm thay cho đợt tấn công theo lịch | ✅ 10%/cứ điểm/đêm |
| `PathfieldActor` debounce | ⬜ đẩy sang P3 — chưa có gì làm field phải dựng lại |
| Phương tiện di chuyển | ⬜ đẩy sang P2 — đi cùng với chiến đấu và thể lực |
| Autotile bờ biển / ranh giới quần xã (1113 ô `transition_edge` chưa dùng) | ⬜ đẩy sang P9 — thẩm mỹ, không chặn gì |
| Mỏ / cổng | ⬜ đẩy sang P4 — cả hai cần hạ tầng instance |

**Xong khi:** ✅ sinh một thế giới mới, đi từ tâm ra rìa và **thấy được** độ khó tăng dần
(22 → 13 → 5 → 9 → 2 làng, và cứ điểm dày lên ở rìa); làng nằm ở chỗ hợp lý; không vùng nào bị
cứ điểm bóp nghẹt.

### Ba thứ đo được rồi mới sửa, đáng ghi lại

| Triệu chứng | Nguyên nhân thật |
|---|---|
| 17 làng vùng tuyết / 5 làng vùng rừng | Lọc bằng địa hình **thay cho** mật độ theo vòng. Tuyết dễ xây, rừng khó — nên lọc theo địa hình thưởng đúng nơi phải vắng nhất |
| Bản đồ trông như bo mạch in | Noise của đường ở scale 48 biến thiên quá chậm → mỗi con đường thành hai đoạn vuông góc |
| Bếp lửa hoá thành quầy hàng | Chọn ô `TilesetElement (0,4)` theo thumbnail mà không soi. Đúng cái lỗi `tools/verify_structures.py` sinh ra để chặn — và nó lọt vì crop 2×1 không đi qua công cụ đó |

### Đẩy sang sau, và vì sao

- **`PathfieldActor` debounce** → P3. Một lần BFS ở 1024² tốn ~25 ms, nên debounce là bắt buộc —
  nhưng **chỉ khi có thứ làm field phải dựng lại**. Hôm nay làng là bất biến và công trình cố ý
  không ảnh hưởng flow field, nên field dựng đúng một lần lúc khởi động. Viết cơ chế debounce bây
  giờ là viết cho một vấn đề chưa tồn tại.
- **Phương tiện di chuyển** → P2. Đi chéo bản đồ mất gần 4 phút, nên ở 1024² đây là thiết yếu chứ
  không phải tiện nghi. Nhưng ngựa/lướt gắn với thể lực và tốc độ nhân vật, và cả hai được định
  nghĩa ở P2 — làm trước là tune tốc độ hai lần.
- **Autotile bờ biển / ranh giới quần xã** → P9. 1113 ô `transition_edge` vẫn chưa dùng, nên mọi
  ranh giới hiện là bậc thang cứng. Xấu, nhưng không chặn gì và không đụng vào thiết kế nào.
- **Mỏ và cổng** → P4. Cả hai cần hạ tầng instance (`MapId` là giá trị runtime), và P4 đã phải làm
  hạ tầng đó cho khu mỏ.
- **Tham số `season` cho `terrain_of`** → P7, cùng với mùa. Lý do cũ ("đụng flow field, làm một lần
  rẻ hơn hai") không còn đúng: flow field giờ nhắm vào làng, không vào địa hình mùa vụ.

---

## P2 — Nhân vật và chiến đấu — **XONG**

| | Trạng thái |
|---|---|
| `PlayerActor` **keyed theo tài khoản**, nhiều instance (nguyên tắc 2) | ✅ 8 slot phiên, đăng ký nguội lúc bring-up |
| **Đăng nhập**: tên + mật khẩu, Argon2 qua Monocypher | ✅ `src/world/account.hpp`, 32 MiB / 3 lượt, salt riêng mỗi tài khoản |
| Máu / mana / thể lực; chết và hồi sinh | ✅ ba thanh, mỗi thanh khoá một động từ khác nhau |
| Quái tấn công người chơi | ✅ qua **beacon**, không phải `ask` — xem dưới |
| Cận chiến + tầm xa; đường đạn | ✅ mũi tên là state của chunk, migrate y hệt sinh vật |
| Bốn hệ phép và hệ trạng thái | ✅ Đóng băng / Bỏng / Ướt / Dính bùn / Nhiễm điện |
| **Combo vật lý × phép** | ✅ đủ 5 dòng bảng [GAME.md §7](GAME.md) |
| **Phe và thái độ** | ✅ `disposition` là *trạng thái*; nổi giận, nguội lại, và **nhớ mặt** |
| Bầy đàn nổi giận theo | ✅ đánh một con sói là đánh cả đàn |
| Động vật hoang dã lang thang quanh `home`, không dùng flow field | ✅ ~620 con, gieo một lần từ khoá chunk, không hồi sinh |
| Kinh nghiệm theo hành động, giới hạn tổng điểm kỹ năng | ✅ 4 kỹ năng, trần 34 điểm |
| Màn hình Nhân vật (`C`), có chân dung | ✅ |
| **Cân bằng độ khó theo vòng** | ✅ HP ×1 → ×5, sát thương ×1 → ×3,2, và **loài khác nhau** theo vòng |
| Phương tiện di chuyển (đẩy từ P1 sang) | ✅ `R` — nhanh hơn, nhưng không đánh được |

**Xong khi:** ✅ `mmo_sim` dàn dựng một trận thật và khẳng định được từng mắt xích — beacon tới chunk,
quái đánh người chơi, người chơi vung kiếm, chunk xử sát thương, XP quay về; hai tài khoản cùng đứng
một ô là **hai thanh máu khác nhau**; chết rồi hồi sinh ở lò sưởi mà **không mất gì**.

### Ba thứ đo được rồi mới sửa, đáng ghi lại

| Triệu chứng | Nguyên nhân thật |
|---|---|
| "Quái không đánh người chơi" — máu vẫn 100 sau 25 tick | Không phải lỗi. Có **hai** tài khoản cùng đăng nhập, cùng đứng một ô spawn; quái chọn người gần nhất và nó chọn người kia. Chính xác là điều nguyên tắc 2 tồn tại để làm đúng |
| `mmo_probe` segfault | `int hist[6]` đánh chỉ số bằng `Terrain` — enum đã lên 11 giá trị từ **P1**. Ghi tràn stack, im lặng suốt một giai đoạn. Giờ mảng lấy kích thước từ `Terrain::kCount` |
| Cả thế giới hoá đỏ khi chết | Lớp phủ toàn màn hình alpha 130. Một lớp wash phủ kín mạnh hơn nhiều so với lúc đọc code, vì mắt không còn chỗ nào chưa nhuộm để so |

### Một quyết định kiến trúc đáng ghi: beacon, không phải `ask`

Sinh vật cần biết người chơi ở đâu **mỗi tick**. Hỏi `PlayerActor` (tier A, sau này ở máy khác) là
đặt một lượt đọc đồng bộ xuyên actor — rồi xuyên máy — vào đường nóng di chuyển của mọi con quái
trong thế giới. Nên chiều dữ liệu bị đảo lại: MapDirector đọc vị trí từ `PlayerBus` đã publish rồi
phát `PlayerBeacon` tới 5×5 chunk quanh mỗi người chơi, ba tick một lần.

Nó là **soft state có hạn dùng**: chunk quên một beacon không nghe lại sau 12 tick. Không cần message
"người chơi đã rời đi", mất một beacon thì tự lành, và một chunk vừa được đặt lại sau khi node chết
chỉ việc học lại danh sách ở nhịp sau. Đây là ARP, và đúng vì cùng một lý do ARP đúng.

Phần thưởng không định trước: **danh sách beacon chính là interest set.** Wildlife làm gần như không
chunk nào còn "rỗng", nên luật LOD cũ (chunk rỗng publish thưa) lẽ ra đã âm thầm hết tác dụng. Luật
đúng — và đáng lẽ luôn phải là — "chỉ publish đủ nhanh khi có người nhìn được", và `players_` chính
là vị từ đó, miễn phí. P6 sẽ dùng lại đúng danh sách này để quyết định stream chunk nào cho ai.

---

## P3 — Hệ thống thế giới — **tiếp theo**

Giờ mới xây các hệ thống *lên trên* bộ khung, khi cân bằng combat đã ổn định.

- **Làng lên bậc** ([GAME.md §6](GAME.md)): 5 bậc, `VillageActor`, lên khi được giúp, **chững khi
  không** — không tụt. **Chọn làng xuất phát lúc đăng nhập lần đầu.**
- **Địa giới** (claim) quanh căn cứ; xây tự do ngoài vùng cấm.
- **Luật thích nghi quần xã**: sa mạc cần tưới, tuyết cần sưởi, đầm lầy cần móng cọc.
- **Vùng cấm xây**: cứ điểm (bán kính 12 ô), cổng.
- Nhiệm vụ và giao thương với làng; giá theo bậc làng.
- Trả nợ kỹ thuật: message tóm tắt mối đe doạ giữa chunk hàng xóm (tháp bắn xuyên chunk, tường trên
  biên chunk chặn được).

**Xong khi:** dựng được căn cứ ở sa mạc và sống nhờ hệ thống tưới; giúp quê mình lên từ Xóm thành
Làng và thấy chợ mở ra.

---

## P4 — Kinh tế: tài nguyên, chế tạo, trang bị

- **Hạ tầng instance** ([ARCHITECTURE.md §4](ARCHITECTURE.md)): `MapId` thành giá trị runtime,
  `InstanceManager`, chunk actor cấp phát/huỷ động thay vì tạo sẵn hết trong `World::build()`.
  Làm ở đây chứ không đợi P8 — khu mỏ cần nó trước, và hầm ngục sau đó chỉ việc dùng lại.
- Khu mỏ nhiều tầng (mỗi tầng là một instance); bậc quặng Đồng → Sắt → Thép → Bí ngân.
- Trạm chế tạo; công thức; màn hình Chế tạo (`B`) lọc theo nguyên liệu đang có.
- Túi đồ (`I`), trang bị, độ bền, ổ khảm.
- **Thuần hoá và chăn nuôi**: bắt động vật hiền về nông trại, nhân giống (gà → trứng, bò → sữa,
  cừu → len) + nấu ăn cho buff. Sprite đã có sẵn trong Ninja Adventure (`Actor/Animal`, 27 loài).
- **Giao thương với làng**: giá phụ thuộc bậc làng và thứ làng đang thiếu.

**Xong khi:** đi hết được chuỗi từ quặng thô → thanh kim loại → vũ khí → khảm đá, và trang bị đó
tạo khác biệt đo được trong chiến đấu.

---

## P5 — Bền vững

- `Persistent<EventSourced>` cho state thế giới; snapshot định kỳ lên node tin cậy.
- `Persistent<Snapshot>` cho tiến trình người chơi trên node tin cậy.
- Nhiều thế giới; tạo/tải/xoá.
- Kịch bản khôi phục: giết node giữa chừng, chunk re-place, replay từ snapshot.

**Xong khi:** tắt máy giữa lúc chơi, mở lại, thế giới đúng như cũ (sai lệch ≤ 60 giây).

---

## P6 — Multiplayer

- Cluster nhiều process qua `TcpTransport`; leader = node đầu tiên khởi động.
- Token phiên → `quark::Principal`; `Authorizer` cho từng (principal, target, message); địa giới
  được cưỡng chế ở tầng này.
- `Require<Trusted>` cưỡng chế bằng capability view thật — **để đảm bảo đúng một chủ sở hữu**, không
  phải để chống gian lận.
- **Interest set**: client đăng ký chunk quanh mình, không nhận cả bản đồ.
- Đồng bộ nhiều người chơi, chat, danh sách bạn.
- `RelayTransport` cho NAT.

**Xong khi:** hai máy khác nhau, hai tài khoản, cùng một thế giới, cùng phòng thủ một đợt tấn công.

---

## P7 — Mùa và thời tiết

- Bốn mùa với cây trồng, độ dài ngày/đêm, hệ số chiến đấu riêng.
- Thời tiết do MapDirector phát: mưa/bão/sương mù/tuyết, kèm trạng thái (mưa → *Ướt*).
- **Hồ đóng băng mùa đông** — dựng lại flow field theo mùa, mở lối tấn công mới.
- Hiệu ứng hạt cho thời tiết.

**Xong khi:** phòng tuyến hiệu quả mùa hè bị xuyên thủng mùa đông vì quái đi qua mặt hồ đóng băng.

---

## P8 — Hầm ngục và quái tự học

- **Cõi nghỉ trước, cõi thử thách sau** — cõi câu cá / suối nước nóng dùng lại hạ tầng instance
  của P4 và **không cần chiến đấu**, nên nó là cách rẻ nhất để chứng minh hệ thống cõi hoạt động.
- Bản đồ hầm ngục, PvE nhóm, Tinh chất rơi ra. Mỗi cõi một atlas riêng, nạp/giải phóng khi vào/ra.
- Đóng cứ điểm bằng Tinh chất → thu hẹp vùng nguy hiểm.
- **Tích hợp RLDrive** (đã khảo sát — xem [ARCHITECTURE.md §7](ARCHITECTURE.md)): `add_subdirectory`,
  viết `CombatEnvironment`, `TrainingActor` trên leader. **Không dùng `DqnTrainer`** — tự viết vòng
  lặp 20 dòng. **Phải xử lý `kActionCount` gắn cứng = 15 trong `DqnAgent.cpp:25`**, nếu không thì
  segfault ngay khi không gian hành động nhỏ hơn.
- **Pre-train offline, commit file trọng số** (JSON, ~24 KB): "Thế hệ 0" phải là checkpoint đã
  huấn luyện, không phải mạng ngẫu nhiên — boss mới sinh mà hành xử ngẫu nhiên thì cả tính năng
  thành trò cười. Round-trip đã kiểm chứng bit-exact.
- **Policy theo nguyên mẫu, không theo cá thể** (10–15 policy, không phải 115) — bắt buộc, nếu không
  chi phí bùng nổ. Xem [ARCHITECTURE.md §7](ARCHITECTURE.md).
- **Hai võ đường**: `VillageDojoActor` + `DungeonDojoActor` tự đấu riêng, cộng `SparringActor` giao
  lưu định kỳ với **checkpoint đóng băng** của phe kia (bắt buộc — nếu cả hai cùng học thì không hội
  tụ). Sân tập nằm trong thế giới để người chơi **xem được**.
- Đột kích hầm ngục → lùi checkpoint phe quái. Hiển thị "Thế hệ N".
- **Trần độ khó** — bắt buộc, xem [GAME.md §9](GAME.md).

**Xong khi:** bỏ mặc một hầm ngục vài ngày rồi thấy đợt tấn công của nó khó lên **một cách đo được**
— và có thể tắt RL để quay về bảng hành vi mà người chơi không nhận ra khác biệt.

---

## P9 — Đánh bóng

Cân bằng, nhạc, hiệu ứng hạt, hướng dẫn, bách khoa, tối ưu, đóng gói phát hành.

---

## Nợ kỹ thuật, gắn vào giai đoạn nào

| Nợ | Trả ở |
|---|---|
| `BuildKind::kCore` + `core_hp` + điều kiện thua | **P0** |
| Mặt đất 256×256; `kMapCount = 3` | **P1 ✅** |
| `kSpawnCamps`/`camp_tile()` → cứ điểm do worldgen đặt | **P1 ✅** |
| Chunk tạo sẵn hết lúc bring-up, không ngủ được | **P1 ✅** chunk rỗng publish 32 tick/lần |
| `MapDirector` ôm quá nhiều trách nhiệm | P3 — giờ nó chỉ còn đồng hồ + tung xúc xắc đột kích, đủ nhỏ để đợi |
| `PlayerActor` singleton | **P2 ✅** 8 slot phiên, keyed |
| Tháp chỉ thấy quái cùng chunk | P3 |
| Công trình chỉ chặn đường trong chunk sở hữu | P3 |
| `MapId` là enum cố định | P4 (hạ tầng instance) |
| Chưa có persistence | P5 |
| Renderer đọc toàn bộ SnapshotBus, không có interest set | P6 — **nửa đầu đã có ở P2**: danh sách beacon đã là interest set phía server |
| Chưa có âm thanh | P0 |

---

## Quyết định — đã chốt

| | Quyết định | Ghi chú |
|---|---|---|
| Đăng nhập | **tên + mật khẩu**, không OIDC | hash bằng Argon2 (Monocypher, 1 file, public domain) — đây là chỗ duy nhất tôi xin không thoả hiệp, vì người chơi dùng lại mật khẩu |
| Node tin cậy | **node đầu tiên là leader** | không cần VPS; mô hình Minecraft/Valheim |
| Ngân sách nhân vật | **0 đồng** | Ninja Adventure CC0 16×16, 95 nhân vật 4 hướng, đã đọc LICENSE.txt |
| Tên game | **Ninja Adventure** | repo `github.com/thnak/ninja-adventure` |
| Tông | **chill mặc định, thử thách tuỳ chọn** | GAME.md §0 — bỏ hướng "thế giới suy tàn" |
| Bố cục thế giới | **vòng đồng tâm kiểu Valheim** | tâm dễ, ngoài khó |
| Kích thước mặt đất | **1024×1024** (1024 chunk actor) | bắt buộc kèm: phương tiện di chuyển, mật độ nội dung ×4, LOD chunk |
| Thứ tự | **khung thế giới → combat → hệ thống** | tune combat một lần, trên bản đồ thật |
| RL | quái **+ vệ binh làng**, chỉ chiến binh | policy theo nguyên mẫu, không theo cá thể |

## Nợ P2 đẩy sang sau, và vì sao

- **Slot phiên cố định (8)** → P6. `Engine::register_activation` là cold-only ("safe, single-threaded
  before start()"), nên một actor không thể xuất hiện khi thế giới đang chạy. Roster đăng ký sẵn và
  đăng nhập chỉ *gắn* tài khoản vào slot. Máy chủ thật cũng có connection slot vì đúng lý do này;
  bỏ giới hạn cần Quark cho phép spawn nóng, và đó là việc của P6.
- **Mật khẩu đi qua mạng dạng rõ** → P6, cùng `SecureTransport`. Chưa có mạng nên chưa có gì để
  mã hoá, nhưng phải ghi rõ trước khi có người chơi cùng nhau qua Internet.
- **Sát thương do va chạm giữa quái và quái** chỉ trong cùng chunk → P3, cùng message tóm tắt hàng xóm.
- **Loot của quái** → P4. Sinh vật hoang dã rơi thịt; quái chưa rơi gì, vì bảng loot là việc của P4
  và đặt một bảng tạm bây giờ là đặt nó hai lần.

## Xen giữa: R0–R3 — cách thế giới được ráp lại trên màn hình

Không phải một phase trong P0–P9. Đây là việc dựng hình, làm sau P2 vì bốn ảnh GIF demo của chính
tác giả bộ art cho thấy thế giới của chúng ta **cứng** hơn hẳn — cùng một bộ art, khác cách ráp.
Chi tiết đầy đủ ở [RENDER_SPEC.md](RENDER_SPEC.md).

| | Việc | Kết quả đo được |
|---|---|---|
| R0 | Hai hình chữ nhật cây bị cắt đôi (2×3 → 4×3) | 58% và 60% viền bị cắt → **0%** |
| R1 | Hoạ tiết nền: mọi ô → ~24%, theo cụm | `density` 0.150 → **0.096** (dải của pack: 0.084–0.104) |
| R2 | Địa hình quyết định theo **đỉnh góc**, một bộ viền cho mỗi địa hình | `shoreline_lock` 0.98 → **0.25** ở đầm lầy |
| R3 | Y-sort: sinh vật, cây, nhà, người chơi vào **một danh sách, sắp một lần** | người chơi đi sau tán cây và trước gốc cây, không cần trường hợp đặc biệt |

**Một điều phải nói rõ: con số mà RENDER_SPEC.md được viết quanh nó là sai.** `grid_lock` tách hai
tập ảnh hoàn hảo (0.061 vs 0.127) nhưng nó đo **tỉ lệ phóng của bản ghi màn hình**, không đo cách
dựng hình: bốn GIF demo là ảnh phóng nearest-neighbour đúng 4 lần, nên màu chỉ có thể đổi ở một cột
trong bốn — và hai cột mà `grid_lock` lấy mẫu không nằm trong lớp pha đó. Không có lượng công việc
tile nào kéo ảnh của chúng ta xuống 0.06 được. Xem RENDER_SPEC.md §0.2; `shoreline_lock` là phép đo
thay thế, có tự dò pha.

Còn nợ: **R4** — thay các bộ viền tự sinh bằng bộ vẽ tay của pack ở bốn cặp có nước (pack có sẵn,
cùng một hình học 5×5, đã đo). Và **ash/stone vẫn là nền lát đá trong nhà** — bộ CC0 này không có
đất cháy hay đá lộ thiên nào; đó là art phải tự vẽ, để lại cho P9.

## Không còn gì chặn đường

Mọi quyết định đã chốt. **P0, P1, P2 xong, và R0–R3 (dựng hình) xong.** Việc tiếp theo là
**P3 — hệ thống thế giới.**
