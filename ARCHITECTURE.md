# Ninja Adventure — kiến trúc kỹ thuật

> **Thay đổi định hướng (2026-07-22).** Trước đây tài liệu này mở đầu bằng "mục tiêu số 1 là trình
> diễn năng lực QuarkCpp". **Không còn nữa.** Mục tiêu là làm ra một game. QuarkCpp vẫn là engine —
> nó vẫn là lựa chọn đúng cho một thế giới phân tán, bền vững — nhưng từ giờ mọi quyết định thiết kế
> được cân theo *game*, không theo *demo*.
>
> Cụ thể những gì đổi: kích thước thế giới do gameplay quyết định chứ không do "cho đẹp biểu đồ";
> mục tiêu "5.000 entity trong video" bị bỏ; các chỉ số kỹ thuật (CHUNK MIGRATIONS…) rời khỏi HUD
> vào màn hình debug (F3).

Thiết kế game: [GAME.md](GAME.md) · Lộ trình: [ROADMAP.md](ROADMAP.md)

Engine: `/home/nvthanh/works/QuarkCpp` (C++23, header-first, 153 tests, cluster TCP + SWIM, PAL
Linux + Windows, verify sạch dưới ASan/UBSan/TSan).

---

## 0. Những chỗ tôi thiết kế sai — và đang sửa

Bạn yêu cầu chỉ ra và sửa thiết kế sai. Đây là danh sách thật, không phải hình thức. Phần lớn phát
sinh vì tôi thiết kế cho một game thương mại có kinh tế thật, trong khi thứ bạn đang làm là **game
đầu tay, mã nguồn mở, chơi cùng bạn bè** — hai bài toán rất khác nhau.

### S1. Kiến trúc tin cậy bị làm quá

Tôi đã đề xuất OIDC, mô hình đe doạ chi tiết, và cả lập luận "nhân bản làm tăng bề mặt gian lận".
Tất cả **đúng** cho một MMO thương mại. Với dự án này chúng là chi phí không đổi lại được gì — kẻ
tấn công là bạn bè bạn.

**Sửa:** node đầu tiên là leader và giữ sự thật. Xác thực bằng tên + mật khẩu. Bỏ OIDC.
`Require<Trusted>` **giữ lại**, nhưng đổi ý nghĩa: nó là cơ chế **đúng đắn** (đúng một chủ sở hữu
cho state có thẩm quyền — tránh hai node cùng ghi), không phải bức tường chống gian lận.

### S2. `BuildKind::kCore` là khái niệm sai

"Một Core bạn phải bảo vệ" là thiết kế tower-defense một người chơi. Nó mâu thuẫn trực tiếp với xây
tự do và với làng mạc: khi bạn có thể xây bất kỳ đâu và thế giới có sẵn các khu định cư, **không tồn
tại một điểm duy nhất để bảo vệ**.

**Sửa:** bỏ Core như một loại công trình đặc biệt. Nhà bạn là thứ bạn xây. Làng có trung tâm riêng.

### S3. Điều kiện thua sai

`core_hp = 0 → game over` không hợp một thế giới bền vững nhiều người chơi.

**Sửa:** không có điều kiện thua đơn lẻ. Mất căn cứ là một mất mát phải xây lại, không phải kết thúc.
Thế giới thua dần theo cách khác: **làng mạc suy tàn**.

### S4. Nguồn spawn sai

"5 trại spawn cố định" là lời giải cho vấn đề "quái đến từ 360°" — vấn đề đó có thật, lời giải đó
đúng lúc bấy giờ. Nhưng khi thế giới có làng mạc, nguồn đúng là **cứ điểm**: chúng vừa phát ra quái,
vừa **tấn công được**, và phải được đặt *tương quan với làng* để làng còn sống nổi.

**Sửa:** `kSpawnCamps` / `camp_tile()` → hệ thống cứ điểm, do bộ sinh thế giới đặt.

### S5. Mục tiêu của flow field thiếu một nửa

Quái hiện chỉ nhắm công trình của người chơi. Nhưng **làng cũng phải bị tấn công** — đó chính là cơ
chế làm làng suy tàn, và là thứ khiến thế giới có vẻ đang thua chứ không đứng yên chờ bạn.

**Sửa:** nguồn của BFS đa nguồn = công trình người chơi **+ công trình của làng**.

### S6. Thiếu hẳn một hệ thống: sinh thế giới

Đây là lỗ hổng lớn nhất và tôi đã bỏ sót hoàn toàn. Sinh **địa hình** bằng noise thì có rồi. Nhưng
đặt **làng, cứ điểm, mỏ, đường** sao cho mạch lạc là một hệ thống riêng, và nó khó hơn địa hình:

- làng phải nằm ở chỗ sống được (gần nước, đất trồng được, không giữa sa mạc)
- cứ điểm không được quá gần làng, nếu không làng chết ngay từ ngày đầu
- mỏ phải nằm ở núi/đá
- đường nối làng với nhau — và đường là thứ khiến bản đồ *đọc được*

**Sửa:** thêm hẳn một hệ thống `WorldGen` vào P2. Xem §8.

### S7. `MapDirector` singleton sẽ không đủ

Với làng có mức phát triển riêng, quân đội riêng, và cứ điểm phát đợt theo nhịp riêng, một actor
điều phối toàn cục là nút thắt và là chỗ chứa quá nhiều trách nhiệm.

**Sửa:** `MapDirector` chỉ còn giữ **đồng hồ thế giới** (ngày/đêm, mùa, thời tiết). Mỗi làng một
`VillageActor`, mỗi cứ điểm một `StrongholdActor`, tự quyết định nhịp của mình.

### S8. `kMapCount = 3` đã chết

Đã nêu lần trước, nhắc lại vì nó vẫn còn trong code: một mặt đất liền mạch + instance động thay cho
ba bản đồ cố định.

---

## 1. Nền tảng giữ nguyên

Những quyết định này vẫn đúng khi mục tiêu đổi từ demo sang game, nên giữ nguyên:

| Quyết định | Vì sao vẫn đúng cho một game |
|---|---|
| **1 chunk 32×32 ô = 1 actor** | Thế giới bền vững, nhiều người chơi cần phân mảnh không gian. Actor cho single-writer miễn phí → không một mutex nào trong toàn bộ mô phỏng |
| **raylib + seam `IRenderBridge`** | App sở hữu main loop; renderer chỉ đọc snapshot, không bao giờ `ask` trong vòng vẽ |
| **Terrain là hàm thuần `(seed, map, x, y)`** | Node nào cũng tính được ô bất kỳ mà không hỏi ai — thứ khiến re-place chunk sau khi node chết trở nên rẻ |
| **Flow field thay vì steering tham** | Steer-and-slide kẹt vĩnh viễn trong vịnh nước — đã đo được, không phải phỏng đoán |
| **Phân tầng tin cậy bằng `Placement<...>` policy** | Máy người chơi là node *thù địch*. Giờ còn quan trọng hơn vì đã có tài khoản và tiến trình thật để bảo vệ |
| **Atlas sinh bằng Python offline** | Đổi art = sửa manifest, không đụng C++ |

---

## 2. Đăng nhập — node đầu tiên là leader

Quyết định của bạn (2026-07-22): **node đầu tiên khởi động là leader tin cậy**; đăng nhập chỉ là tên
+ mật khẩu. Điều này **đơn giản hoá kiến trúc rất nhiều** và tôi đã rút bỏ phần thừa (xem [§0](#0-những-chỗ-tôi-thiết-kế-sai--và-đang-sửa)).

Đây là mô hình Minecraft/Valheim: ai mở thế giới thì máy đó là chủ. Quen thuộc với người chơi, hợp
với mã nguồn mở, và không cần VPS.

```
Node #1 (leader — máy của người mở thế giới)
  ├ Bảng tài khoản (tên → hash mật khẩu)
  ├ PlayerActor cho mọi người chơi
  ├ VillageActor / StrongholdActor
  ├ Đồng hồ thế giới (MapDirector)
  └ Sổ cái bền vững  ◄── nguồn sự thật duy nhất

Node #2..N (máy bạn bè cùng chơi)
  └ ChunkActor: mô phỏng ô đất, quái, cây trồng, công trình
```

### Một điều duy nhất tôi xin không thoả hiệp: đừng lưu mật khẩu dạng thô

Không phải vì kẻ tấn công — mà vì **người chơi dùng lại mật khẩu**. Nếu file thế giới của bạn lộ ra
(mà với dự án mã nguồn mở thì save file rất hay được chia sẻ), bạn không chỉ làm lộ tài khoản game;
bạn làm lộ email của bạn mình.

Cái giá để làm đúng gần bằng không:

| | |
|---|---|
| Thư viện | [**Monocypher**](https://github.com/LoupVaillant/Monocypher) — **một file .c**, public domain, có sẵn Argon2 |
| Lưu | `argon2i(password, salt_ngẫu_nhiên)` + salt, mỗi tài khoản một salt |
| Không cần | OIDC, OAuth, máy chủ tài khoản, chứng chỉ |

Một file nguồn thả vào repo. Không có build system nào phải sửa.

### Thứ *có thể* để sau

- **Mã hoá đường truyền.** Chưa nối `SecureTransport` thì mật khẩu đi qua mạng ở dạng rõ. Với LAN /
  nhóm bạn thì chấp nhận được — nhưng phải **ghi rõ trong README**, đừng để người chơi tự đoán.
- **Chống gian lận.** Với nhóm bạn thì kick là đủ. `Require<Trusted>` vẫn giữ nhưng vì lý do đúng
  đắn kỹ thuật, không phải phòng thủ.

### Leader chết thì sao

Trung thực: **thế giới dừng.** Giống hệt khi chủ server Minecraft tắt máy — người chơi hiểu và chấp
nhận điều này.

Hai việc phải làm để nó không thành thảm hoạ:
1. **Save file phải xuất được và di chuyển được.** Người khác cầm file đó là mở lại được thế giới.
2. **Lưu định kỳ + lúc thoát.** Sập máy mất tối đa một khoảng snapshot.

Bầu leader tự động là chuyện của sau này, nếu có bao giờ cần.

---

## 3. Lưu trữ và tiến trình

Quyết định leader-là-nguồn-sự-thật làm phần này gọn hơn hẳn so với kế hoạch trước.

| Dữ liệu | Ở đâu | Mô hình |
|---|---|---|
| Tài khoản, túi đồ, kỹ năng, tiến trình | **leader** | SQLite (adapter Quark đã có) |
| Làng: mức phát triển, dân số, quân | **leader** | SQLite |
| Checkpoint RL | **leader** | file + hash |
| State thế giới: công trình, cây trồng, ô khai hoang | node sở hữu chunk | event-sourced + snapshot lên leader |
| Vị trí quái, chỉ số đợt | không lưu | trong bộ nhớ |

**Vẫn là phân tán ở chỗ quan trọng**: mô phỏng nặng (mọi chunk, mọi quái, tìm đường) chạy trên máy
người chơi. Chỉ *sự thật* mới tập trung — và với một game bạn bè, tập trung là ưu điểm chứ không
phải nhược điểm: một file save, một chỗ backup, một chỗ để sửa khi có gì hỏng.

Quark cho sẵn `Persistent<Snapshot|EventSourced>`, `EventLog`, seam `Store` và adapter SQLite. Không
cần nhân bản, không cần quorum — **toàn bộ nhóm việc khó nhất của kế hoạch cũ biến mất.**

---

## 4. Bản đồ liền mạch + instance

Quyết định thiết kế ở [GAME.md §3](GAME.md): **mặt đất là một bản đồ liền mạch; hầm ngục và mỏ là
bản đồ riêng, instance theo nhóm.** Hệ quả kỹ thuật:

### `MapId` phải trở thành giá trị runtime

Hiện `MapId` là enum ba giá trị cố định. Instance nghĩa là bản đồ được **cấp phát lúc chạy** và bị
huỷ khi nhóm rời đi.

May mắn: `chunk_key` đã dành sẵn chỗ.

```cpp
chunk_key = (map << 32) | (cy << 16) | cx      // map là 16 bit → 65.536 bản đồ đồng thời
```

16 bit là quá đủ. Cần đổi: `MapId` từ enum thành `std::uint16_t` có ý nghĩa, với dải phân vùng —
ví dụ `0..15` cho mặt đất bền vững, `16..65535` cấp phát cho instance.

### Vòng đời instance

Đây là điểm khác biệt lớn nhất so với hiện tại: **chunk actor hiện được tạo hết một lần lúc bring-up
và sống mãi.** Instance cần tạo và huỷ theo yêu cầu.

```
Người chơi chạm cổng
  └─► InstanceManager (Require<Trusted>) cấp map_id, spawn chunk actor cho instance đó
        └─► người chơi được chuyển; interest set đổi sang map mới
              └─► nhóm rời / hết giờ / trống > N phút
                    └─► lưu phần cần lưu, huỷ actor, trả lại map_id
```

Quark hỗ trợ được — `register_actor` / `Activation` không đòi hỏi mọi actor phải tồn tại từ đầu, và
`IdleTimeout` policy vốn sinh ra cho đúng kiểu vòng đời này. Nhưng `World::build()` hiện tại tạo
sẵn toàn bộ 192 chunk trong constructor; **phần đó phải viết lại thành cấp phát động** trước khi làm
hầm ngục.

Đây là lý do nên làm hạ tầng instance ở **P3 (khu mỏ)** chứ không đợi đến P7: mỏ cần nó trước, và
làm sớm thì hầm ngục chỉ là dùng lại.

### Kích thước mặt đất: 1024×1024

256×256 → **1024×1024 ô (32×32 = 1024 chunk actor)**. Xem [GAME.md §3](GAME.md) cho lập luận
gameplay và ba điều kiện đi kèm.

| | 256×256 (nay) | **1024×1024** |
|---|---|---|
| Chunk actor / bản đồ | 64 | **1024** |
| Bộ nhớ flow field | 128 KB | **2 MB** |
| BFS dựng lại field | ~1 ms | **~25 ms** |
| Tick fan-out mỗi giây (10 Hz) | 640 | **10.240** |
| Sinh địa hình lúc tạo thế giới | ~4 ms | **~100 ms** (một lần) |

Không con số nào là vấn đề cho Quark. Nhưng hai cái buộc phải xử lý:

**BFS 25 ms** → `PathfieldActor` debounce (§5) từ "tối ưu hoá nên có" thành **bắt buộc**. Dựng lại
field mỗi lần đặt một viên gạch là đứng hình 25 ms.

**LOD mô phỏng.** Tick rỗng của 1024 chunk chỉ tốn ~0,3% CPU — không đáng lo. Thứ đáng lo là **quái
vẫn đi lại, vẫn tìm đường, vẫn migrate qua biên trong 1000 chunk không ai nhìn thấy**. Cần ba mức:

| Mức | Điều kiện | Nhịp tick |
|---|---|---|
| Hoạt động | có người chơi trong bán kính 2 chunk | 10 Hz |
| Nền | có người chơi cùng bản đồ nhưng ở xa | 1 Hz |
| Ngủ | không ai gần trong N phút | 0 — `IdleTimeout` cho actor ngủ |

Cây trồng không mất gì khi chunk ngủ: giai đoạn cây được **suy ra từ thời gian đã trôi**, nên nó tự
bắt kịp khi chunk tỉnh lại. Đó là lợi ích trực tiếp của một quyết định đã ghi trong `chunk_actor.hpp`
từ trước — không phải may mắn.

---

## 5. Xây tự do phá vỡ flow field

Flow field hiện tại là **một BFS từ một Core duy nhất**. "Xây ở bất kỳ đâu" nghĩa là có N căn cứ, và
câu hỏi "quái đi đâu?" không còn một đáp án.

### Lời giải: BFS đa nguồn

Thay vì N field, dùng **một field mỗi bản đồ, BFS từ *tất cả* công trình người chơi cùng lúc** —
khởi tạo hàng đợi với mọi ô có công trình ở khoảng cách 0. Quái đi xuống dốc là tự động tới **công
trình gần nhất**. Vẫn O(số ô) một lần, **chi phí y hệt hiện tại**.

Một yêu cầu tưởng như đắt hoá ra không tốn thêm gì.

### Cái đắt là việc dựng lại

Field đổi mỗi khi người chơi xây/phá. BFS 65K ô mất vài ms — không thể chạy mỗi lần đặt một viên
gạch.

**Giải pháp:** một `PathfieldActor` mỗi bản đồ, `Priority<2>`, gom yêu cầu (debounce ~2 giây) rồi
công bố field mới dưới dạng `shared_ptr<const FlowField>` bất biến. Chunk đang dùng field cũ vẫn
chạy tiếp — quái đi lệch trong 2 giây thì không ai nhận ra.

Field trở thành **dữ liệu phái sinh có phiên bản**, không phải state chia sẻ. Cùng lập luận với
`flow_field.hpp` hiện tại, mở rộng thêm chiều thời gian.

### Chỉ sinh vật thù địch dùng field

Hệ phe ở [GAME.md §5](GAME.md) có một hệ quả hiệu năng đáng kể: **động vật hoang dã không tìm đường
toàn cục.** Chúng lang thang quanh `home` trong bán kính lãnh thổ. Chỉ phe `Monster` mới đi xuống
dốc theo field.

Nghĩa là có thể có rất nhiều động vật mà gần như không tốn gì — thế giới sống động không phải đánh
đổi bằng hiệu năng.

---

## 6. Mùa/thời tiết và tính thuần của terrain

`terrain_of` hiện **thuần theo thời gian**. Đó là thứ khiến mọi node đồng ý về địa hình mà không cần
trao đổi gì. Mùa và thời tiết đe doạ tính chất đó.

**Luật:** thời tiết **không được** đổi khả năng đi lại. Mưa, sương mù, bão chỉ đổi *hệ số* — tốc độ,
tốc độ lớn của cây, tầm tháp. Không đổi ô nào đi được.

**Một ngoại lệ có chủ ý: hồ đóng băng mùa đông.** Cái này *có* đổi khả năng đi lại, và là chi tiết
gameplay hay (mở lối tấn công mới vào giữa đông). Xử lý được vì **mùa là hàm thuần của thời gian
thế giới**:

```cpp
terrain_of(seed, map, x, y, season)   // season = f(world_time) — mọi node tính ra như nhau
```

Flow field dựng lại **4 lần mỗi năm game**, tại thời điểm mọi node biết trước. Xác định, không cần
đồng bộ.

Thời tiết *trong ngày* thì ngược lại — nó ngẫu nhiên và cần đồng thuận, nên **MapDirector (tin cậy)
quyết định rồi phát đi** như một phần của `Tick`. Không node nào tự nghĩ ra thời tiết.

---

## 7. Đặt việc huấn luyện RL ở đâu

Thiết kế gameplay ở [GAME.md §9](GAME.md). Về kỹ thuật:

- **Dùng lại `core/` của RLDrive** (`/home/nvthanh/works/windows-machine-self-learn`) — 2377 dòng
  C++20, build sạch trên Linux, backward đã gradient-check. Chỉ `gpu/` mới gated WIN32.
- Đưa vào bằng `add_subdirectory` giống cách QuarkCpp đang được dùng. **Không copy-paste** — dự án
  đó vẫn đang phát triển.
- Phải tự viết: `CombatEnvironment` thay cho `core/env/` (vốn là mô hình xe hơi). Seam
  `ITrainerBackend` / `DqnAgent` vốn đã không biết gì về môi trường — đúng chỗ để cắm vào.
- `TrainingActor`: `Require<Trusted>` + `Priority<2>` + `DrainBudget` nhỏ. Chạy trên leader.

### Một policy cho mỗi nguyên mẫu — không phải cho mỗi cá thể

Vệ binh của làng cũng học RL ([GAME.md §6](GAME.md)). Điều đó **bắt buộc** phải kèm một ràng buộc,
nếu không chi phí bùng nổ:

| | Nếu mỗi cá thể một mạng | Nếu mỗi nguyên mẫu một mạng |
|---|---|---|
| Số mạng phải huấn luyện | ~75 vệ binh + ~40 quái = **115** | **10–15** |
| Dữ liệu mỗi mạng nhận được | 1/115 | toàn bộ của nguyên mẫu đó |
| Chất lượng học | kém (thiếu dữ liệu) | tốt |

Cá thể khác nhau ở **chỉ số và trang bị**, không ở bộ não. Đây cũng là cách các game thật làm.

### Hai võ đường + giao lưu với đối thủ đóng băng

Bản trước tôi đề xuất **một** đấu trường chung cho hai phe. Theo góp ý của bạn, đổi thành **mỗi phe
một sân tập riêng, cộng một pha giao lưu định kỳ** — và tôi thấy hướng này đúng hơn:

- Mỗi phe cần luyện thứ khác nhau (vệ binh: đội hình; quái: áp sát, bao vây). Trộn chung từ đầu thì
  cả hai học nửa vời.
- Sân tập nằm trong thế giới → **người chơi xem được**, tính năng RL trở nên hữu hình.

```
VillageDojoActor      self-play, policy vệ binh
DungeonDojoActor      self-play, policy quái
SparringActor         định kỳ: mỗi bên đấu với checkpoint ĐÓNG BĂNG của bên kia
```

**Đóng băng là bắt buộc.** Nếu cả hai policy cùng cập nhật trong lúc đấu, chúng đuổi nhau và không
bên nào hội tụ (moving-target). Đóng băng một phía biến giao lưu thành một môi trường tĩnh để học —
đây là league training, kỹ thuật chuẩn.

Chi phí: hai sân thay vì một. Với policy theo nguyên mẫu (10–15 policy) thì vẫn nhỏ, và cả ba actor
đều `Priority<2>` nên chúng chỉ dùng CPU thừa.

Điểm móc gameplay: đột kích hầm ngục **lùi checkpoint phe quái vài thế hệ** — "vào đánh lấy đồ" có
hệ quả cơ học đo được.
- Kết quả phát tán dưới dạng **checkpoint bất biến + hash**, không phải tính toán lặp lại trên nhiều
  node — mạng nơ-ron là số thực, hai node huấn luyện song song sẽ phân kỳ.

---

## 8. Sinh thế giới — vùng đồng tâm kiểu Valheim

Đây là hệ thống tôi từng bỏ sót hoàn toàn ([§0 S6](#0-những-chỗ-tôi-thiết-kế-sai--và-đang-sửa)).
Đề xuất "tâm dễ, ngoài khó" của bạn **làm nó dễ hơn nhiều** so với phương án tôi định làm.

### Vì sao vùng đồng tâm là lời giải, không chỉ là phong cách

Phương án cũ của tôi là một bộ giải ràng buộc: đặt làng theo điểm phù hợp, rồi đặt cứ điểm với
khoảng cách tối thiểu tới làng, rồi kiểm tra và sinh lại nếu hỏng. Ràng buộc khó nhất là **cứ điểm
không được bóp nghẹt làng** — và nó khó vì hai thứ đó được đặt độc lập rồi mới đi hoà giải.

Vùng đồng tâm **loại bỏ ràng buộc đó ngay từ đầu**:

| | Ràng buộc rời rạc (cũ) | Vùng đồng tâm (mới) |
|---|---|---|
| Cứ điểm không bóp nghẹt làng | phải kiểm tra và sinh lại | **tự đúng** — mật độ cứ điểm là hàm của bán kính, làng lớn ở tâm nơi mật độ ~0 |
| Người mới không lạc vào chỗ chết | phải đặt riêng vùng khởi đầu | **tự đúng** — tâm là vòng dễ nhất |
| Bản đồ đọc được | cần đường sá làm xương sống | **tự đúng** — "xa hơn = khó hơn" là quy tắc duy nhất người chơi cần |

Một quyết định thiết kế xoá được cả một lớp lỗi. Đây là loại đơn giản hoá đáng lấy.

### Các lượt

```
1. bán kính     r = distance(ô, tâm) / bán_kính_max        → [0,1]
2. vòng         ring = f(r + noise*0.08)                    → noise để bờ vòng lượn sóng,
                                                              không thành cái bia bắn cung
3. địa hình     value noise, THAM SỐ theo vòng              → hồ nhiều ở đầm lầy, đá nhiều ở núi
4. làng         mật độ & bậc giảm theo r; bậc 3–4 chỉ ở vòng 0–1
5. cứ điểm      mật độ TĂNG theo r; vòng 0 không có
6. mỏ           trong vùng đá, bậc quặng tăng theo r
7. cổng         rải đều mọi vòng — cõi nghỉ ở gần, cõi thử thách ở xa
8. đường        A* nối các làng, chi phí cao khi gần cứ điểm
9. kiểm tra     mọi làng vòng-trong tới được nhau; ≥1 làng bậc ≥2 làm điểm xuất phát
```

Bước 9 vẫn giữ — nhưng giờ nó gần như luôn đạt ngay lần đầu, thay vì là cái van an toàn chính.

### Hệ quả kiến trúc (không đổi)

Vẫn đúng như đã nêu: **địa hình thuần theo (seed, x, y)** — mọi node tự tính, không đồng bộ. Nhưng
**bố cục** (làng/cứ điểm/mỏ/cổng/đường) là kết quả sinh một lần, **leader lưu và phát** cho node mới.

Hai loại dữ liệu khác nhau: một loại tính được, một loại phải nhớ.

### Các cõi thì sinh riêng

Mỗi cõi sau cổng có bộ sinh riêng, đơn giản hơn nhiều (không cần làng, không cần đường):
hầm ngục là phòng + hành lang; cõi nghỉ có thể **viết tay** — chúng nhỏ và được ghé nhiều lần, nên
bàn tay con người đáng giá hơn thuật toán ở đây.

---

## 9. Rủi ro

| Rủi ro | Mức | Giảm thiểu |
|---|---|---|
| **Phạm vi** — 10 hệ thống lớn cùng lúc | **Rất cao** | ROADMAP sắp xếp để mỗi giai đoạn kết thúc bằng game chơi được; ML đặt sau cùng |
| RL không cho hành vi thú vị | Cao | Thiết kế game không cược vào nó — thay bằng bảng hành vi theo "thế hệ", người chơi thấy như nhau |
| Leader chết → thế giới dừng | Trung bình | Mô hình Minecraft, người chơi hiểu. Save file phải xuất/di chuyển được |
| Sinh thế giới ra bố cục không chơi được | **Thấp** (đã hạ) | Vùng đồng tâm khiến ràng buộc khó nhất tự đúng (§8) |
| **Tông chill bị trôi về phía "game áp lực"** | Trung bình | §0 là hàng rào: mỗi tính năng mới phải trả lời "cái này có thêm đồng hồ đếm ngược nào không?" |
| Latency 50–150 ms giữa máy nhà | Trung bình | Thể loại đã chọn để chịu được; PvP tắt mặc định |
| ~~Thiếu nhân vật 4 hướng~~ | — | **Đã giải quyết**: Ninja Adventure CC0 16×16, giấy phép đã đọc trực tiếp |

---

## 10. Nợ kỹ thuật đã biết

Ghi lại để không quên khi mở rộng:

1. **Tháp chỉ thấy quái trong chunk của nó.** Bắn xuyên chunk = một chunk đọc state chunk khác. Cách
   đúng là chunk công bố *bản tóm tắt mối đe doạ* cho hàng xóm bằng message.
2. **Công trình chỉ chặn đường trong chunk sở hữu.** Tường nằm đúng trên biên chunk không chặn quái
   từ phía bên kia. Cùng cách sửa với (1).
3. **Một người chơi.** `PlayerActor` đang là singleton key=1. Multiplayer cần key theo tài khoản và
   một *interest set* không gian để client chỉ nhận chunk quanh mình.
4. **Chưa có persistence.** Chưa gắn `Persistent<...>` vào actor nào.
5. **Chưa có âm thanh.** raylib có `raudio` sẵn; asset đã tải (Ninja Adventure + RPG Audio + Jingles).
6. **`BuildKind::kCore` phải bỏ** (§0 S2) — cùng với `core_hp` trong `WorldStatus` và điều kiện thua.
7. **`kSpawnCamps` / `camp_tile()` phải thay bằng cứ điểm** do bộ sinh thế giới đặt (§0 S4).
8. **`MapDirector` phải tách** thành đồng hồ thế giới + `VillageActor` + `StrongholdActor` (§0 S7).
