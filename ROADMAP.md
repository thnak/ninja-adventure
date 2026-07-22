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

## P1 — Bộ khung thế giới — **đang làm**

Dựng sân khấu thật, trước khi tune bất cứ thứ gì lên trên nó.

| | Trạng thái |
|---|---|
| **Công cụ xuất bản đồ** `mmo_worldmap` — PNG full-size, màu phẳng, thống kê vòng/địa hình | ✅ làm đầu tiên: không nhìn được bản đồ thì không tune được worldgen |
| 1024×1024 ô = **1024 chunk actor** | ✅ 700 tick chạy trong **1,9 giây** |
| 5 vòng đồng tâm + 3 địa hình mới (tuyết, đầm lầy, tro) | ✅ |
| Gỡ `kMapCount = 3` → một mặt đất `kOverworld` | ✅ |
| Thống nhất art sang Ninja Adventure | ⬜ tileset đã khảo sát, xem `assets/CREDITS.md` |
| Worldgen đặt làng / cứ điểm / cổng / mỏ / đường | ⬜ |
| BFS đa nguồn + `PathfieldActor` debounce | ⬜ |
| LOD chunk (10 Hz / 1 Hz / ngủ) | ⬜ |
| Phương tiện di chuyển | ⬜ |

- **1024×1024 ô** (32×32 = 1024 chunk actor). Gỡ `kMapCount = 3`.
- **Vòng đồng tâm** ([GAME.md §4](GAME.md)): Đồng cỏ → Rừng → Đầm lầy/Sa mạc → Núi tuyết → Đất cằn,
  bờ vòng có noise.
- **Sinh thế giới** ([ARCHITECTURE.md §8](ARCHITECTURE.md)): đặt **40–60 làng** (mật độ phải nhân
  theo diện tích, nếu không bản đồ trống rỗng), cứ điểm (mật độ tăng ra ngoài), mỏ, cổng, đường.
  Bố cục được lưu và phát, không phải hàm thuần.
- **Cứ điểm thay trại spawn**: `StrongholdActor`; gỡ `kSpawnCamps`/`camp_tile()`. Tách `MapDirector`
  xuống chỉ còn đồng hồ thế giới.
- **BFS đa nguồn** + `PathfieldActor` debounce. Ở 1024² một lần BFS tốn ~25 ms → debounce là bắt
  buộc, không phải tối ưu hoá.
- **LOD mô phỏng**: chunk không có người chơi gần hạ xuống 1 Hz hoặc ngủ. Không làm thì quái chạy
  loanh quanh trong 1000 chunk không ai nhìn.
- **Di chuyển**: ngựa (hoặc lướt kiểu ninja) + điểm dịch chuyển ở làng. Ở 1024² đây là **thiết yếu**,
  không phải tiện nghi — đi bộ chéo bản đồ mất gần 4 phút.
- Làng ở mức "nhà cửa + NPC đứng đó". Chưa bậc, chưa quân đội.
- Truyền tham số `season` vào `terrain_of` ngay bây giờ — nó đụng flow field, làm một lần rẻ hơn hai.

**Xong khi:** sinh một thế giới mới, cưỡi ngựa từ tâm ra rìa và **thấy được** độ khó tăng dần qua
từng vòng; làng nằm ở chỗ hợp lý; không có vùng nào bị cứ điểm bóp nghẹt.

---

## P2 — Nhân vật và chiến đấu

Hiện người chơi chỉ đứng nhìn tháp bắn. Đây là giai đoạn biến nó thành một game — và giờ nó được
tune trên bản đồ thật.

- `PlayerActor` **keyed theo tài khoản**, nhiều instance (nguyên tắc 2).
- **Đăng nhập cơ bản**: tên + mật khẩu, hash bằng Argon2 (Monocypher — một file .c, public domain).
  Cần một tài khoản để `PlayerActor` key theo.
- Máu / mana / thể lực; chết và hồi sinh; quái tấn công người chơi.
- Cận chiến + tầm xa; đường đạn; thanh kỹ năng.
- Bốn hệ phép và **hệ trạng thái** (Đóng băng / Bỏng / Ướt / Dính bùn / Nhiễm điện).
- **Hệ phe và thái độ sinh vật** ([GAME.md §5](GAME.md)): `faction` + `disposition` là *trạng thái*
  chứ không phải thuộc tính loài; sinh vật trung lập nổi giận khi bị chọc rồi nguội lại; bầy đàn
  nổi giận theo. Động vật hoang dã lang thang quanh `home`, **không** dùng flow field.
- **Combo vật lý × phép** — cơ chế chữ ký, xem [GAME.md §7](GAME.md).
- Kinh nghiệm theo hành động, giới hạn tổng điểm kỹ năng.
- Màn hình Nhân vật (`C`), có chân dung (`Faceset.png` đi kèm mỗi nhân vật).
- **Cân bằng độ khó theo vòng** — đây là thứ chỉ làm được khi bản đồ đã thật.

**Xong khi:** một mình sống được ở vòng 1; combo Đóng băng → cận chiến nặng gây *Vỡ vụn* nhìn thấy
rõ; đi ngang đàn sói không gây sự thì chúng kệ bạn, bắn một con thì cả đàn xông tới; và ra vòng 3
thì **chết** — độ khó theo vòng có thật.

---

## P3 — Hệ thống thế giới

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
| Mặt đất 256×256; `kMapCount = 3` | **P1** |
| `kSpawnCamps`/`camp_tile()` → cứ điểm do worldgen đặt | P1 |
| `MapDirector` ôm quá nhiều trách nhiệm | P1 |
| Chunk tạo sẵn hết lúc bring-up, không ngủ được | P1 (LOD) |
| `PlayerActor` singleton | **P2** (nguyên tắc 2) |
| Tháp chỉ thấy quái cùng chunk | P3 |
| Công trình chỉ chặn đường trong chunk sở hữu | P3 |
| `MapId` là enum cố định | P4 (hạ tầng instance) |
| Chưa có persistence | P5 |
| Renderer đọc toàn bộ SnapshotBus, không có interest set | P6 (chuẩn bị ở P2) |
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

## Không còn gì chặn đường

Mọi quyết định đã chốt. Việc tiếp theo là **P0**.
