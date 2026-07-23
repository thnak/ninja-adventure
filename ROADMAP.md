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
| **Worldgen đặt làng / cứ điểm / đường** | ✅ 51 làng có tường, 22 cứ điểm, 443 nhà + 4.406 mảnh tường, `src/world/worldgen.hpp` + `village.hpp` |
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

## Xen giữa: R0–R4 — cách thế giới được ráp lại trên màn hình

Không phải một phase trong P0–P9. Đây là việc dựng hình, làm sau P2 vì bốn ảnh GIF demo của chính
tác giả bộ art cho thấy thế giới của chúng ta **cứng** hơn hẳn — cùng một bộ art, khác cách ráp.
Chi tiết đầy đủ ở [RENDER_SPEC.md](RENDER_SPEC.md).

| | Việc | Kết quả đo được |
|---|---|---|
| R0 | Hai hình chữ nhật cây bị cắt đôi (2×3 → 4×3) | 58% và 60% viền bị cắt → **0%** |
| R1 | Hoạ tiết nền: mọi ô → ~24%, theo cụm | `density` 0.150 → **0.096** (dải của pack: 0.084–0.104) |
| R2 | Địa hình quyết định theo **đỉnh góc**, một bộ viền cho mỗi địa hình | `shoreline_lock` 0.98 → **0.25** ở đầm lầy |
| R3 | Y-sort: sinh vật, cây, nhà, người chơi vào **một danh sách, sắp một lần** | người chơi đi sau tán cây và trước gốc cây, không cần trường hợp đặc biệt |
| R4 | Dùng bộ **47-mask** của chính tác giả cho nước, đất, tuyết | `shoreline_lock` rừng 0.162 → **0.143**, đầm 0.249 → **0.171**; bờ nước có bờ đất và vành bọt |

**Một điều phải nói rõ: con số mà RENDER_SPEC.md được viết quanh nó là sai.** `grid_lock` tách hai
tập ảnh hoàn hảo (0.061 vs 0.127) nhưng nó đo **tỉ lệ phóng của bản ghi màn hình**, không đo cách
dựng hình: bốn GIF demo là ảnh phóng nearest-neighbour đúng 4 lần, nên màu chỉ có thể đổi ở một cột
trong bốn — và hai cột mà `grid_lock` lấy mẫu không nằm trong lớp pha đó. Không có lượng công việc
tile nào kéo ảnh của chúng ta xuống 0.06 được. Xem RENDER_SPEC.md §0.2; `shoreline_lock` là phép đo
thay thế, có tự dò pha.

**R4 làm lộ một con số nữa tôi báo sai.** Tôi từng ghi pack chỉ có **3** bộ viền hoàn chỉnh. Thực
tế có **13**, và project Godot của chính tác giả (`assets/_src/ninja/GodotProject.zip` — đúng project
đã quay bốn cái GIF) ghi rõ toạ độ từng bộ. Scanner của tôi đếm thiếu vì nó dò layout **16 mask 4
góc**, trong khi pack dùng **BITMASK_3X3_MINIMAL của Godot 3: 47 ô, khối 11×5**. Project đó cũng xác
nhận độc lập chuyện phóng 4×: `window/size` là 320×176, `test_width` là 1280×704.

Nhưng kết luận cũ vẫn đứng: **không có bộ chuyển cát↔cỏ.** Dựng lại bản đồ làng của tác giả từ file
scene cho thấy ông ấy ghép cứng cát vào cỏ rồi rắc vật thể lên che. Nên 4 địa hình dùng art của pack,
7 địa hình vẫn tự sinh.

## Xen giữa: R5 — làng có phố

Dựng lại `Village.tscn` của tác giả cho thấy khoảng cách với làng của ta **không nằm ở mật độ trang
trí** — `density` của ta là 0.095, dải của pack là 0.084–0.104, đã nằm trong dải. Nó nằm ở **bố cục**:
`worldgen.hpp` đặt nhà bằng rejection sampling trong một vành vuông quanh quảng trường, tức là rắc
ngẫu nhiên, nên không có gì thẳng hàng với gì cả.

Bản lề là một tính chất `try_place` vốn đã có: nó lát hàng ô ngay dưới mặt tiền, vì **mọi sprite nhà
trong pack này đều quay mặt xuống nam**. Đặt nhà sao cho hàng thềm của nó *chính là* mặt đường thì
cửa tự mở ra phố — không cần thêm phép kiểm tra nào có thể lệch khỏi art về sau.

| | |
|---|---|
| Phố | chạy đông-tây, bước 5 ô = 3 ô nhà + 2 ô lòng đường; cắt đúng bằng dãy nhà nó phục vụ |
| Dãy nhà | mọc **ra hai bên từ ngã tư**, mỗi bên tối đa 2 căn, rồi mở phố mới |
| Trục | một đường bắc-nam xuyên quảng trường, nối mọi phố với mạng đường ngoài |
| Sảnh | tier ≥ 3, quay mặt ra **quảng trường** — thứ đáng nhìn khi vừa tới nơi |
| Quảng trường | thu từ `3 + tier` xuống `2 + tier/2`; ở tier 5 nó từng rộng 17 ô, hơn cả chiều cao màn hình |

Tier giờ đọc được từ hình dạng: tier 1 một dãy phố, tier 3 hai, tier 5 sáu. Số công trình 522 → **519**
(quảng trường nhỏ hơn làm vài chỗ đặt hụt). Windows/MSVC ra đúng từng con số của Linux — `grass
362748`, `path 22169`, `buildg 5259` — nên tính tất định của worldgen còn nguyên.

Còn nợ: **ash/stone vẫn là nền lát đá trong nhà** — bộ CC0 này không có đất cháy hay đá lộ thiên
nào; đó là art phải tự vẽ, để lại cho P9.

## Xen giữa: R6 — làng có tường, và `village.hpp`

Pack **có sẵn hàng rào cọc**, và đó là thứ dự án này đã bỏ sót suốt: `TilesetHouse` chứa cột gỗ 3×5,
tường ván 3×3, đúng tường đó khoét vòm, và cọc rào 1×2. Chính bốn mảnh này dựng nên hàng rào trong
`Village.tscn` của tác giả (lớp House, tile id 39/40/41 xếp chồng = cột, 42 = tường, 43 = vòm,
44/45/46 = cọc). Không phải đoán — đọc ra từ file scene của ông ấy.

Toàn bộ phần "một cái làng gồm những gì" chuyển sang **`src/world/village.hpp`**. `worldgen.hpp` chỉ
còn trả lời *ở đâu*: khoảng cách, vòng, đường nối. Hai câu hỏi đó không cùng cỡ — một cái quét cả bản
đồ, một cái soi xem cột nào của sprite cổng là cái lỗ.

| | |
|---|---|
| Tường bắc/nam | ô 3 tile: cột (chẵn) — tường (lẻ) — **cổng vòm ở chính giữa**; số ô luôn lẻ nên hai đầu đều là cột |
| Tường đông/tây | **cột gỗ xếp chồng** — tường ván vẽ nhìn ngang sẽ thành tường đổ |
| Cổng đông/tây | ô cột giữa nhường chỗ: cọc rào 2 hàng trên, 2 hàng dưới, chừa **đúng 1 ô** ở giữa |
| Bốn cổng | `gates_of(tâm, tier)` là hàm thuần — nên **đường được đào tới cổng** chứ không tới tâm |
| Rào cọc | lối vào bốn cổng, và **vườn rau** trong tường: rào một mặt, đất `kDirt` — đúng thứ P3 cần |
| Dọn cây | 1/12 cây trong tường ở lại |

**Đường phải nhắm vào cổng.** Đường được đào *trước* khi làng dựng, nên nếu nó nhắm vào tâm thì tường
sẽ đè lên chính nó và làng có một con đường cụt húc vào hàng rào. `gates_of` thuần chính là để giải
việc đó.

### Ba lỗi mà chỉ đo mới thấy

| lỗi | cách phát hiện |
|---|---|
| **Cọc rào cao 2 ô** nên đặt lệch 1 ô là nó phủ luôn mặt đường | probe: ô tiếp cận cổng có `terrain = kBuilding` |
| **Vòm cổng bị bịt hàng trên** — hàng trên của sprite là mũ tường, vẽ đặc | flood-fill: **23/51 làng** có cổng nhìn thấy mà không đi qua được |
| **Cột 3×5 gặp một ô nước là rụng cả cột**, để lại lỗ 15 ô | quét cả 51 làng: chỗ hở rộng nhất 17 ô |

Cả ba đã sửa: cọc lùi thêm một ô, vòm khoét **cả ba hàng** (người chơi khuất sau mũ tường đúng một ô,
y hệt khi bước vào cửa nhà), và cột rụng thì hạ xuống tường, hạ tiếp xuống cọc. Sau đó thêm một lượt
**đường vượt nước** dọc hai trục — cùng thứ `carve_road` vẫn làm giữa các làng — vì `pave` từ chối
nước, nên một cái ao nằm trên trục là một cái cổng dẫn ra hồ.

Kết quả đo lại: **51/51 làng, cả bốn cổng đều đi được từ quảng trường; chỗ hở rộng nhất còn 8 ô.**
Windows/MSVC ra đúng từng con số của Linux — `grass 347579`, `path 24185`, `buildg 32421`, 51 làng /
22 cứ điểm / 4849 công trình, cùng điểm spawn.

## Xen giữa: R7 — cửa dẫn vào nhà

Mỗi ngôi nhà đều có cửa, và giờ cửa dẫn đi đâu đó thật.

**Vị trí ô cửa là ĐO chứ không đoán.** Quét hàng ô dưới cùng của cả 15 sprite công trình tìm dải
pixel gần-đen: cửa sổ 16px tối nhất rơi vào **cột 1** ở cả 15 cái — nhà 3 ô rộng, nhà 4 ô rộng, lều
tuyết, tàn tích, lều trại. Một hằng số, `kDoorDx`. Trước đó không chỗ nào trong dự án biết cửa ở đâu:
`size_of` trả về một hình chữ nhật, mà hình chữ nhật thì không có mặt trước.

| | |
|---|---|
| Map nội thất | **một** map phụ, lưới 64×64 phòng, mỗi phòng một khối 16×16 ô — 4096 chỗ cho 443 nhà |
| Địa hình phòng | thuần số học (`interior_tile`), **không** overlay — nên mọi node tự tính được, y như terrain |
| Sprite phòng | ghép lúc đóng gói từ nine-slice của chính pack (`compose_room`); ruột **trong suốt** để nền sàn vẫn đi qua đường terrain thường |
| Bảng cửa | mảng ~440 phần tử **sắp xếp theo ô**, tìm nhị phân; publish y như overlay |
| Ra ngoài | về ô **thềm** (dưới cửa một ô), không phải về ô cửa |

### Điều kiện cần: người chơi phải va được vào tường

Trước hôm nay **người chơi đi xuyên qua mọi thứ** — nước, cây, nhà. `MoveIntent` chỉ kẹp vào biên bản
đồ. Lý do cũ vẫn đúng cho *chunk state*, nhưng `terrain_of` không phải chunk state: nó là hàm tự do
trên seed + overlay, đúng cái flow field gọi hàng nghìn lần mỗi lần dựng lại.

Cái đã đổi là **giờ có thứ phụ thuộc vào câu trả lời**. Hàng rào mà đi xuyên qua được thì chỉ là bức
tranh hàng rào; và ô cửa chỉ là ô cửa khi bức tường bên cạnh nó không phải.

Hai trục được giải **riêng**, đó là khác biệt giữa *trượt dọc tường* và *dính vào tường* — với một cái
làng toàn hình chữ nhật thì đó là phần lớn thời gian.

Kèm theo: `Teleport` tách khỏi `MoveIntent`. Một bước đi bị kiểm tra từng ô, còn dịch chuyển 200 ô chỉ
kiểm tra ô đích — hai việc khác nhau, hai verb khác nhau, và `Teleport` không nối được với input.

### Vòng lặp vô hạn suýt xảy ra

Ra cửa mà rơi đúng vào ô cửa thì ô đó lại là cổng dẫn vào — nhảy qua nhảy lại 10 lần một giây, mãi
mãi. Chặn hai lớp: ra về ô **thềm**, và cổng chỉ kích hoạt khi **ô thay đổi** (`last_tile_`).

### Cái giá

`kMapCount` 1 → 2, tức **2048 chunk actor** thay vì 1024. `mmo_sim 600` từ 5,4s lên 8,3s. Đó là giá
thật và ghi ra đây chứ không giấu; đổi lại là kiến trúc không phải có ngoại lệ nào cho map thứ hai.

Windows/MSVC ra đúng từng con số của Linux, và `mmo_sim` (gồm cả hai phép thử mới: tường chặn được
người, cửa dẫn vào rồi ra đúng ô thềm) **OK** trên cả hai.

## Xen giữa: R8 — nội dung là parcel của chính tác giả (2026-07-23)

R0–R7 sửa **cách dựng hình**; R8 sửa **thứ được dựng**. Bốn GIF demo cho thấy khác biệt còn lại
không nằm ở tile hay ở autotile nữa mà ở **bố cục**: thế giới của ta vẫn là noise-scatter — cây và
nhà rắc theo hàm nhiễu — còn của tác giả là những cảnh **ghép tay**. Nhà ngồi ngay trên hàng rào,
chợ có xe kéo và luống củ cải, trại thông có lều và đống lửa.

**Quyết định: không vá noise-scatter cho ra vẻ có bố cục.** Đó là con đường tinh chỉnh vô tận —
thêm luật cụm, thêm luật khoảng cách — mà đích đến vẫn là đoán xem tác giả sẽ đặt gì. Bộ art CC0 này
đã ship sẵn thứ tốt hơn: `World/Maps/Village.tscn`, một cái làng chính tác giả ghép tay. Nên nội dung
set-piece của game **là composition của ông ấy**, không phải phỏng đoán procedural của ta về nó.

### Cái đã ship: một pipeline dữ liệu, chạy lúc build

| Bước | Việc |
|---|---|
| `tools/import_prefabs.py` | cắt các **parcel** hình chữ nhật ra khỏi `Village.tscn` — đọc scene, tileset và texture thẳng từ `GodotProject.zip`, dựng lại bản đồ đúng cách engine của tác giả dựng (bao gồm cái bẫy `cell_tile_origin` đã đo) |
| `tools/build_atlas.py` | đóng 12 parcel vào atlas và sinh `src/world/prefabs.hpp` — **art**: parcel trông thế nào |
| `src/world/prefab_stamp.hpp` | **luật thuần** `(def, variant)`: parcel chặn ở đâu, cụm tuỳ chọn nào sống, mirror ra sao. Số nguyên, chạy được cả ở sim lẫn renderer trên hai máy khác nhau mà vẫn khớp |

12 parcel cắt được: `camp_clearing`, `forest_cottage`, `snow_pond`, `south_orchard`, `market_yard`,
`street_houses`, `north_treeline_well`, `stairs_plaza`, và bốn cái **hoãn** (dưới).

### Landmark rắc theo vòng — `kPoiTable`

Bốn parcel được rắc như mốc landmark, mỗi vòng một loại, spacing là hàm thuần của seed nên tất định:

| parcel | vòng | đặt được | ràng buộc nền |
|---|---|---|---|
| `camp_clearing` | rừng | **28 trại** | nền khô bất kỳ; cell 32 / gap 56 (một màn hình) nên hai trại không đọc thành copy-paste |
| `forest_cottage` | rừng | **4 nhà** | hiếm vì *kiếm được*: chỉ lọt vào túi rừng mà không trại nào chiếm |
| `snow_pond` | tuyết | **12 ao** | CHỈ trên tuyết — ao là sàn băng đi được, stamp **không** ghi `kWater` nên trượt qua được; feather lên đá tan đọc thành hình cắt giấy |
| `south_orchard` | đồng cỏ | **11 vườn** | CHỈ trên cỏ — feather vào bãi cát sẽ hở lỗ tan giữa các cây |

`place_prefabs()` chạy **sau** làng và cứ điểm, vì một parcel từ chối mọi ô đã xây; nó là thứ cuối
đọc overlay trước khi index đóng băng.

### Parcel của làng, theo tier, có cửa thật

Làng không rắc landmark mà **lát parcel như đồ đạc** — `village.hpp` chọn theo tier:

| parcel | từ tier | chỗ |
|---|---|---|
| `market_yard` | 2 | vị trí xoay theo có sảnh hay không |
| `street_houses` | 3 | nam quảng trường, nơi làng đủ sâu |
| `north_treeline_well` | 3 | bắc quảng trường, trong hàng cây |
| `stairs_plaza` | — | quảng trường bậc thang |

Nhà trong parcel là sprite `TilesetHouse` thật, gắn cờ `has_door` lúc đóng gói; cửa nằm đúng **cột 1**
mà R7 đã đo cho mọi nhà, nên nhà parcel **vào được** mà không cần đo lại — builder chừa ô đó đi được và
worldgen phát một `Door` trỏ vào, y như nhà thường.

### Variant: một parcel, nhiều dáng

Để rừng không bị lát bằng một tấm ảnh, mỗi instance mang `variant` — hash của `(seed, anchor)` tính
một lần lúc đặt, và là **toàn bộ** state chung giữa sim và renderer:

- **Mirror** (bit 0) — lật ngang, chỉ khi parcel không có chữ đọc được (`mirrorable`); ~nửa số lật.
- **Cụm tuỳ chọn** — group 0 (sàn + tâm) luôn có; group 1.. là cụm prop/nhà độc lập, giữ ~75% (hai
  bit mỗi group), nên một parcel cho nhiều cách bày. Parcel mà thiếu một cụm sẽ thành bãi trống thì
  đặt `allow_group_drop=false` và bị ép giữ đủ trước khi lưu.
- **Feather mép** + **whitelist nền** — như bảng POI ở trên.

### Đo được: honest before/after (`tools/screen_metric.py`)

Thước đã hứa lúc thiết kế: chia màn hình thành ô 16px, mỗi ô lấy **tỉ lệ màu áp đảo**, xếp bucket
phẳng (`>0.85`) / có vân (`0.55–0.85`) / rậm (`<=0.55`). Baseline đo từ GIF của chính tác giả
(`Example 1.gif` frame 0, downscale 4× nearest về gốc, bỏ hàng HUD trên cùng): **phẳng 19 / vân 33 /
rậm 48**. Ảnh game 32px/tile nên đo ở `cell=32`.

| ảnh | phẳng | vân | rậm | |
|---|---|---|---|---|
| **Baseline tác giả** | 19 | 33 | **48** | đích |
| `shot_meadow` (đồng cỏ, hoang) | 33 | 53 | 14 | ~không đổi |
| `shot_forest` (rừng, hoang) | 19 | 59 | 22 | ~không đổi |
| `shot_village` (làng, TRƯỚC prefab) | 16 | 51 | 33 | |
| `village_market` (SAU, có `market_yard`) | 23 | 36 | **42** | tiến gần đích |
| `village_street` (SAU, có `street_houses`) | 22 | 38 | **40** | tiến gần đích |
| `village_square` (SAU, quảng trường mở) | 36 | 32 | 33 | quảng trường cố tình phẳng |

Đọc thẳng: **chỗ nào có parcel rậm trên màn hình, `rậm` nhảy lên 40–42, sát baseline 48** — xe kéo,
luống rau, dãy nhà của tác giả là detail thật. Quảng trường (`stairs_plaza`) đứng ở 33 vì nó **cố ý**
là sân mở. Còn đồng cỏ / rừng hoang gần như không đổi — đúng như dự đoán: chúng chưa có parcel nào,
và **detail-underlay** cho vùng hoang là việc tương lai (dưới).

### Reskin đang dang dở

Nghiên cứu (read-only) cho thấy `TilesetFloor` là hai nửa 11 cột song song cùng motif khác palette, nên
`green -> deep-forest` là dịch cột +11 pixel-exact; `green -> snow` phải khớp theo **tên lớp mask** vì
khối snow 47-mask có layout trong khác khối dirt. Đủ để một parcel mọc ra biến thể mùa — nhưng chưa
nối vào pipeline stamp, nên ghi là dang dở.

### Sổ nợ R8 — cố ý để ngoài bảng, mỗi cái một task

| nợ | vì sao chưa làm |
|---|---|
| `lake_islands` | art sàn chứa **nước hở**; stamp phải ghi overlay `kWater` để sim khớp với hình — việc thiết kế, không phải một dòng bảng |
| `waterfall_bridge` | thế giới **chưa có sông** để nó bắc qua |
| `fort_gate` / `fort_courtyard` | tích hợp vào **site cứ điểm**, không phải rắc tự do |
| `Interior.tscn` room prefab | phòng trong nhà vẫn ghép nine-slice; các phòng ghép tay của tác giả để sau |
| detail-underlay vùng hoang | `flat%` của đồng cỏ / rừng còn cao (33 / 19) — cần một lớp chi tiết dưới nền, chưa có |
| override tách cụm `camp_clearing` | trại tự gom về **0 cụm tuỳ chọn**, nên variant chỉ còn mirror; muốn nhiều dáng trại hơn cần override luật tách cụm |

## Không còn gì chặn đường

Mọi quyết định đã chốt. **P0, P1, P2 xong, và R0–R8 (dựng hình + làng có tường + cửa vào nhà +
prefab của tác giả) xong.** Việc tiếp theo là **P3 — hệ thống thế giới.**
