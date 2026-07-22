# Ninja Adventure — thiết kế game

> Repo: `https://github.com/thnak/ninja-adventure.git`

**Một câu:** một ninja giải nghệ về quê trồng trọt — và thế giới vẫn ở đó nếu bạn muốn ra ngoài.

---

## 0. Triết lý: chill là mặc định, thử thách là tuỳ chọn

Đây là nguyên tắc quan trọng nhất và mọi thứ khác phải phục vụ nó.

> **Một buổi chơi mà bạn chỉ tưới cây, cho gà ăn, sửa lại hàng rào — phải là một buổi chơi trọn vẹn.**
> Không có thanh đếm ngược nào chạy sau lưng bạn.

Bản kế hoạch trước tôi viết theo hướng "thế giới đang thua, làng mạc suy tàn nếu bạn không cứu".
**Đó là áp lực môi trường thường trực, và nó mâu thuẫn thẳng với tông chill.** Tôi bỏ hướng đó.

### Nhưng vẫn giữ được móc câu "quái tự luyện"

Hai điều tưởng như xung khắc — *chill* và *quái ngày càng mạnh* — hoà giải được bằng một thay đổi
nhỏ về **ai đi tìm ai**:

| | Kế hoạch cũ (sai tông) | Bây giờ |
|---|---|---|
| Quái luyện xong thì | **kéo đến đập nhà bạn** | ở yên trong hầm ngục của chúng |
| Bạn không làm gì thì | làng tụt bậc, thế giới co lại | không sao cả — thế giới vẫn thế |
| Độ khó tăng khi | thời gian trôi | **bạn chọn đi tìm nó** |

Quái luyện tập vẫn diễn ra, vẫn hiển thị "Thế hệ 47". Nhưng nó là **đường cong độ khó tự mọc**, chờ
bạn tới lấy — không phải cái đồng hồ hẹn giờ.

Người chơi chill bỏ qua và không mất gì. Người chơi muốn khó thì đi tìm cái hầm ngục đã luyện ba
tuần liền. **Độ khó do người chơi lái, chứ không phải do lịch.**

Đêm vẫn có quái lảng vảng quanh nhà, nhưng ở mức hàng rào gỗ chặn được. Chúng làm cho ban đêm có
việc để làm, không phải để bạn sợ.

---

## 1. Thế giới và câu chuyện

**Một ninja nghỉ hưu.** Đủ rồi những đêm trèo tường, đủ rồi mấy vụ tranh chấp của người khác. Anh ta
về mảnh đất thừa kế ở Thung Lũng Sương — nhà cũ dột, ruộng bỏ hoang, và một con gà không biết từ đâu
ra.

Thế là bắt đầu: dựng lại mái, vỡ đất, gieo hạt. Hàng xóm là mấy cái làng nhỏ đủ thân thiện. Ban đêm
có vài con quái đi lạc qua — kỹ năng cũ vẫn còn dùng được.

**Và có những cái cổng.** Rải rác trong thung lũng là các cổng dịch chuyển cũ, không ai biết ai dựng.
Bước qua là sang một cõi khác. Có cõi để **thử thách** — hầm ngục, thí luyện, quái đã luyện tập hàng
tuần. Có cõi chỉ để **đi chơi** — đảo trên mây, suối nước nóng, một vùng đất linh hồn nơi thời gian
trôi chậm. Ninja giải nghệ hoàn toàn có quyền đi du lịch.

**Không có kết cục bắt buộc.** Không trùm cuối phải giết, không thế giới phải cứu. Bạn có một mảnh
đất, vài người hàng xóm, và bao nhiêu cánh cổng tuỳ bạn có muốn mở hay không.

### Vì sao đặt tên theo bộ asset

`Ninja Adventure` là tên bộ art (CC0, Pixel-Boy & AAA) và giờ là tên game. Hợp lý: nó đặt tông đúng
(pixel art dễ thương, không u ám), và nó **thành thật về nguồn gốc** — với một dự án mã nguồn mở thì
đó là điểm cộng.

Ninja chỉ là *điểm xuất phát*, không phải cái khung trói buộc. Vì có instance, mỗi cõi có thể dùng
bộ asset khác hẳn — và câu chuyện đủ chỗ cho tất cả.

### Ba trục tài nguyên

| | Nguồn | Dùng cho |
|---|---|---|
| **Đất** (nông trại) | trồng trọt, chăn nuôi | thức ăn → hồi máu, buff, giao thương |
| **Đá** (khu mỏ) | quặng, đá, gỗ | xây dựng, vũ khí, giáp |
| **Tinh chất** (các cõi) | chỉ có ở cõi thử thách | phép thuật, nâng cấp bậc cao, mở cổng mới |

Ba trục ép người chơi *muốn* làm cả ba — nhưng không ép về thời gian. Bạn có thể cày cấy ba tuần rồi
mới mở cổng đầu tiên.

---

## 2. Vòng lặp

```
   NGÀY                       ĐÊM                        KHI BẠN MUỐN
   ├ trồng / thu hoạch        ├ vài con quái đi lạc      ├ mở cổng vào cõi nghỉ (câu cá, suối
   ├ chăn nuôi                ├ hàng rào gỗ chặn được    │   nước nóng, đảo mây) — không đánh nhau
   ├ khai mỏ / chặt gỗ        ├ hoặc ra tay cho vui      ├ hoặc cõi thử thách (hầm ngục, boss)
   ├ chế tạo / nâng cấp       └ không có gì phải sợ      ├ dời nhà ra vòng ngoài
   └ ghé làng, giao thương                               └ giúp làng lên bậc
        └────────── MÙA đổi mỗi ~7 ngày trong game ──────────┘
```

**Nhịp.** 1 ngày game ≈ 20 phút thực (14 phút ngày / 6 phút đêm — đêm ngắn hơn bản trước, vì đêm là
gia vị chứ không phải bài kiểm tra). 1 mùa = 7 ngày. 1 năm = 4 mùa ≈ 9 giờ chơi.

**Cột "khi bạn muốn" là cột quan trọng nhất.** Nó không có lịch, không có nhắc nhở, không có phần
thưởng đăng nhập hàng ngày. Ngày và đêm cứ thế trôi; cổng thì luôn ở đó.

---

## 3. Cấu trúc thế giới — một mặt đất liền mạch, nhiều cõi sau cổng

Câu hỏi "một scene với chunk, hay nhiều bản đồ nối bằng cổng?" là quyết định cấu trúc lớn nhất còn
lại. Đáp án đúng là **cả hai, nhưng cho hai loại không gian khác nhau**.

### Quyết định

> **Mặt đất: MỘT bản đồ liền mạch.** Toàn bộ thế giới ngoài trời — đồng cỏ, rừng, sa mạc, đầm lầy,
> núi tuyết, cứ điểm, cửa mỏ — nằm trên một lưới chunk duy nhất, không có màn hình tải, không có
> cổng.
>
> **Sau cổng: NHIỀU CÕI riêng, mỗi nhóm một bản sao (instance).** Hầm ngục, hầm mỏ, và cả những cõi
> hoàn toàn khác — mỗi cõi có thể dùng **bộ asset riêng**, luật riêng, tông riêng.

### Vì sao mặt đất phải liền mạch

Trụ cột thiết kế là "xây ở bất kỳ đâu, miễn thích nghi được với môi trường". Nếu mặt đất bị chia
thành 3 vùng nối bằng cổng thì:

- "Bất kỳ đâu" biến thành "một trong ba nơi" — quyết định chọn chỗ dựng nhà mất hết ý nghĩa.
- Ranh giới quần xã trở thành *ranh giới bản đồ* thay vì một dải chuyển tiếp bạn đi bộ qua được.
  Cảm giác "đi từ đồng cỏ vào sa mạc rồi thấy cây trồng héo" biến mất.
- Căn cứ của bạn và mỏ bạn khai thác nằm ở hai bản đồ khác nhau → hậu cần thành phiền toái vô cớ.

Liền mạch cũng chính là thứ kiến trúc chunk-actor sinh ra để làm. Chia bản đồ để "giảm tải" là giải
một vấn đề mà chunk đã giải rồi.

### Vì sao hầm ngục phải là bản đồ riêng

Không phải để giảm tải, mà vì **luật chơi khác hẳn**:

| | Mặt đất | Hầm ngục / mỏ |
|---|---|---|
| Bản sao | một, chung cho cả server | **một bản riêng mỗi nhóm** |
| Thời tiết, mùa | có | không (trong lòng đất) |
| Xây dựng | tự do | cấm |
| Chết | hồi sinh tại nhà | bị đẩy ra ngoài, mất đồ mang theo |
| Tileset | ngoài trời | hang động |

**Instance là lý do quyết định.** Nội dung PvE nhóm cần mỗi nhóm có hầm ngục của riêng mình — không
thể có chuyện nhóm A dọn sạch quái rồi nhóm B vào thấy trống. Điều đó **bất khả thi trong một thế
giới chung liền mạch** và là chuyện tầm thường với bản đồ riêng.

Đấu trường huấn luyện RL ([§10](GAME.md)) cũng cần một không gian đóng, có biên rõ — đúng là một bản
đồ riêng.

### Cổng và các cõi

Cổng là **công trình cố định trên mặt đất**. Đi vào → cấp phát một instance → chuyển người chơi.
Không có cổng nào nối hai vùng *mặt đất* với nhau.

Điểm mấu chốt: **cõi không chỉ là hầm ngục.** Chúng chia hai loại, phục vụ hai tâm trạng khác nhau —
và đây chính là chỗ tông "chill" được bảo vệ:

| Loại cõi | Ví dụ | Có gì | Phần thưởng |
|---|---|---|---|
| **Cõi nghỉ** | đảo trên mây, suối nước nóng, vùng đất linh hồn, hồ câu cá | **không chiến đấu**, hái lượm, câu cá, ngắm cảnh | vật liệu hiếm, đồ trang trí, công thức nấu ăn |
| **Cõi thử thách** | hầm ngục, thí luyện, hang sâu | quái đã luyện RL, boss | Tinh chất, đá khảm, trang bị bậc cao |

Một buổi chơi "hôm nay không muốn đánh nhau" vẫn có chỗ đi: mở cổng vào cõi câu cá, ngồi câu, mang
về ít cá hiếm. **Đó là nội dung thật, không phải phần thưởng an ủi.**

### Mỗi cõi một bộ asset — biến hạn chế thành ưu điểm

Vì cõi là bản đồ tách biệt, nó **không bị ràng buộc bởi style của mặt đất**. Một cõi có thể dùng
Kenney, một cõi dùng Ninja Adventure, một cõi dùng pack khác tải sau.

Với một dự án không có ngân sách art, đây là đòn bẩy lớn nhất có được: **thêm một cõi mới = thêm một
asset pack CC0 + một bộ luật**, không cần vẽ gì. Và vì mỗi cõi là một thế giới riêng, style khác nhau
không hề chỏi — nó *là* lý do người chơi muốn đi.

Hệ quả kỹ thuật: mỗi cõi cần **atlas riêng**, nạp khi vào và giải phóng khi ra. Bộ đóng gói
(`tools/build_atlas.py`) đã sẵn sàng cho việc này — chỉ cần nhiều manifest thay vì một.

### Kích thước: 1024×1024

Chốt **1024×1024 ô = 32×32 = 1024 chunk actor**. Bản đồ rộng cho cảm giác hoang vu và chỗ để mọi
người dựng nhà cách xa nhau — hợp với tông chill.

Nhưng "to hơn thì tốt hơn" chỉ đúng nếu ba thứ sau **đi kèm**, chứ không phải hoãn lại:

| Con số | 512×512 | **1024×1024** |
|---|---|---|
| Tổng số ô | 262 K | **1,05 triệu** |
| Chunk actor | 256 | **1024** |
| Đi bộ hết chiều ngang | 85 giây | **2 phút 51** (chéo góc: ~4 phút) |
| Bộ nhớ flow field | 512 KB | **2 MB** |
| Dựng lại flow field (BFS) | ~4 ms | **~25 ms** |
| Trên mỗi người chơi (50 người) | 5.200 ô | **21.000 ô** |

**Ba thứ bắt buộc đi kèm:**

1. **Phương tiện di chuyển, ngay từ đầu.** Ở 512² thì ngựa là tiện nghi; ở 1024² thì đi bộ 3 phút
   một chiều là **không chấp nhận được**. Ngựa (hoặc lướt kiểu ninja) + điểm dịch chuyển ở làng phải
   nằm trong giai đoạn dựng thế giới, không để dành.

2. **Mật độ nội dung phải nhân theo.** Diện tích gấp 4 → cần khoảng **40–60 làng** thay vì 15, cùng
   tỉ lệ cho cứ điểm, mỏ, cổng. Một bản đồ 1024² với mật độ của 512² không phải "hoang vu thơ mộng",
   nó là **trống rỗng**. Đây là khác biệt giữa Valheim và một bãi đất.

3. **Chunk phải ngủ được (LOD mô phỏng).** 1024 chunk tick 10 Hz là 10.240 message/giây — Quark chịu
   được thoải mái, chi phí tick rỗng chỉ khoảng 0,3% CPU. Vấn đề không phải cái tick, mà là **quái
   vẫn đi lại trong những chunk không ai nhìn thấy**. Chunk không có người chơi gần phải hạ xuống
   1 Hz hoặc ngủ hẳn. Xem [ARCHITECTURE.md §4](ARCHITECTURE.md).

Nếu không làm cả ba, 1024² sẽ tệ hơn 512² chứ không tốt hơn.

### Mở rộng về sau

Nếu sau này muốn thêm "lục địa thứ hai", nó là **một bản đồ mặt đất mới** nối bằng bến tàu — cộng
thêm, không phá vỡ gì. `MapId` đã tồn tại sẵn. Nhưng **đừng bắt đầu bằng nhiều vùng**: nó chia nhỏ
cộng đồng 20–50 người và không đổi lại được gì.

---

## 4. Vùng đất: độ khó toả ra từ tâm

### Kiểu Valheim: càng xa tâm càng khó

Bạn đề xuất mô hình Valheim và tôi thấy đó là lựa chọn đúng, vì ba lý do:

1. **Bản đồ tự giải thích.** Người chơi hiểu ngay "đi xa hơn = khó hơn" mà không cần một dòng hướng
   dẫn nào. Không cần biển báo cấp độ.
2. **Nó biến "xây bất kỳ đâu" thành một cung tiến trình.** Bạn bắt đầu ở tâm, và mỗi lần dời nhà ra
   xa hơn là một cột mốc bạn tự đặt cho mình. Đây là thứ mà quần xã rải ngẫu nhiên **không** cho được.
3. **Nó khiến việc sinh thế giới dễ hơn nhiều** — xem [ARCHITECTURE.md §8](ARCHITECTURE.md). Ràng
   buộc khó nhất trong bản kế hoạch trước (đặt cứ điểm sao cho không bóp nghẹt làng) gần như tự tan.

Với mặt đất 1024×1024, tâm ở (512,512), bán kính tối đa ~512 ô:

| Vòng | Bán kính | Quần xã | Cản trở | Cứ điểm |
|---|---|---|---|---|
| 0 | 0–120 | **Đồng cỏ** | không | không có |
| 1 | 120–220 | **Rừng** | phát quang; quái ẩn nấp | thưa, yếu |
| 2 | 220–320 | **Đầm lầy** / **Sa mạc** | móng cọc, độc / tưới nước | vừa |
| 3 | 320–420 | **Núi tuyết** | lò sưởi, giày đinh | dày |
| 4 | 420+ | **Đất cằn** | không trồng được | dày, mạnh nhất |

Vòng 0 rộng 120 ô là **có chủ ý**: đó là vùng chill, và nó phải đủ lớn để một người chơi không bao
giờ muốn ra ngoài vẫn có cả một vùng quê để sống.

Ranh giới **không phải hình tròn hoàn hảo** — trộn thêm noise để bờ vòng lượn sóng, có mũi nhô ra và
vịnh ăn vào. Nếu không thì bản đồ trông như cái bia bắn cung.

### Xây tự do — nhưng phải thích nghi

Mỗi vòng cần một công nghệ để định cư được. Đây là thứ biến khoảng cách thành **tiến trình**, chứ
không chỉ là quãng đường đi bộ:

| Quần xã | Cản trở | Cách thích nghi |
|---|---|---|
| Đồng cỏ | không | vùng khởi đầu |
| Rừng | phát quang; quái ẩn nấp | rìu tốt hơn; đèn |
| Sa mạc | cây trồng chết khô | Giếng + kênh dẫn |
| Đầm lầy | không đặt được móng, độc | Cọc móng; bộ lọc |
| Núi tuyết | cây chết cóng, đi chậm | Lò sưởi; giày đinh |
| Đất cằn | không trồng được gì | chỉ khai mỏ; thanh tẩy bằng Tinh chất |

**Đền bù cho rủi ro**: vòng ngoài có quặng tốt hơn, thảo dược hiếm hơn, cây trồng giá trị cao hơn.
Người chơi chill ở lại vòng 0–1 và chơi được trọn vẹn; người chơi muốn hơn thì dời nhà ra ngoài.

### Không xây được

| Vùng | Luật | Vì sao tồn tại |
|---|---|---|
| **Cứ điểm** | cấm xây bán kính 12 ô | nguồn của các đợt; có thể tấn công để giảm nhịp spawn; đóng vĩnh viễn bằng Tinh chất |
| **Hầm ngục** | cấm xây toàn bộ | nơi quái luyện tập — nội dung PvE nhóm |
| **Khu mỏ** | cấm xây; đặt được máy khai thác | tài nguyên tầng cao, có quái canh |

Cấm xây ở đây **không phải luật tùy tiện** — nó ngăn người chơi bịt kín cửa spawn, thứ sẽ giết chết
toàn bộ vòng lặp.

---

## 5. Sinh vật: phe và thái độ

Không phải mọi thứ di chuyển đều muốn giết bạn. Đây là thứ biến "bản đồ có quái" thành "thế giới có
hệ sinh thái".

### Phe, không phải cờ nhị phân

Dùng **phe** (faction) thay vì cờ "thù địch/thân thiện", vì phe cho phép **chúng đánh lẫn nhau** —
và đó là thứ làm thế giới sống động gần như miễn phí.

| Phe | Ví dụ |
|---|---|
| `Wild` | hươu, thỏ, gà rừng, lợn rừng, sói, nhện lớn |
| `Monster` | slime, ma, quái từ cứ điểm và hầm ngục |
| `Player` | người chơi và công trình |
| `Villager` | thương nhân lang thang, NPC nhiệm vụ |

Ma trận thái độ giữa các phe:

| | Wild | Monster | Player | Villager |
|---|---|---|---|---|
| **Wild** | trung lập | **thù địch** | trung lập | trung lập |
| **Monster** | **thù địch** | đồng minh | **thù địch** | **thù địch** |
| **Player** | trung lập | **thù địch** | — | thân thiện |

Hệ quả trực tiếp: **một đợt quái đi qua rừng sẽ giết hươu.** Người chơi nhìn thấy hậu quả của việc
không dọn cứ điểm — không cần một dòng chữ nào giải thích.

### Ba thái độ với người chơi

| Thái độ | Hành vi | Sinh vật |
|---|---|---|
| **Thù địch** | tấn công ngay khi thấy | quái cứ điểm, quái hầm ngục |
| **Trung lập** | mặc kệ cho đến khi bị chọc | lợn rừng, sói, nhện lớn, gấu |
| **Hiền** | không bao giờ tấn công, bỏ chạy khi bị đánh | hươu, thỏ, gà, cừu, bò |

**Điểm mấu chốt: thái độ là *trạng thái*, không phải *loại*.** Một con lợn rừng trung lập trở thành
thù địch-với-bạn khi bị chọc, rồi nguội lại. Nếu nó là thuộc tính cố định của loài thì không có gì
để chơi.

### Cái gì khiến sinh vật trung lập nổi giận

| Kích hoạt | Ghi chú |
|---|---|
| Bị bạn đánh | hiển nhiên |
| Bạn bước vào **không gian riêng** | bán kính nhỏ hơn tầm phát hiện của quái thù địch |
| Bạn đánh một con cùng bầy | **bầy đàn nổi giận theo** — sói đi thành đàn 4–6 con, đánh một con là đánh cả đàn |
| Bạn lấy đồ trong lãnh thổ nó | tổ ong, hang, ổ trứng |

**Nguội lại:** sau ~20 giây ngoài tầm và không bị đánh, quay về trung lập — nhưng **có trí nhớ**:
bị chọc lần hai thì nổi giận nhanh hơn và lâu hơn. Con vật cứ bị bạn quấy rầy mãi sẽ thành kẻ thù
thật sự. Chi tiết nhỏ, nhưng nó làm hành vi có vẻ *có lý do*.

### Vai trò từng loại trong vòng lặp

- **Hiền** → thịt, da, lông. Và là **nguồn vật nuôi**: bắt về, thuần hoá, nhân giống ở nông trại
  (gà → trứng, bò → sữa, cừu → len). Đây là trụ cột nông trại mà thiết kế đang thiếu.
- **Trung lập** → đánh đổi rủi ro/phần thưởng. Rơi đồ tốt, nhưng bạn *chọn* có gây sự hay không. Và
  chúng **giúp bạn**: một đàn sói ở gần sẽ ăn thịt quái đi ngang qua.
- **Thù địch** → mối đe doạ của đêm và của hầm ngục.

### Hệ quả kỹ thuật (quan trọng)

1. **Chỉ sinh vật thù địch dùng flow field.** Chúng đi tới công trình gần nhất. Sinh vật hoang dã
   **lang thang trong lãnh thổ nhà** của nó (`home_x/home_y` + bán kính) và không cần tìm đường
   toàn cục — nên chúng **rẻ hơn nhiều**, điều này cho phép có nhiều động vật mà không tốn kém.
2. **Chỉ phe `Monster` mới huấn luyện RL.** Động vật hoang dã dùng hành vi viết tay. Không nói rõ
   điều này thì phạm vi RL phình ra mất kiểm soát.
3. **Một struct `Creature` duy nhất**, thêm `faction`, `disposition`, `home`, `territory_radius`,
   `threat_target`. Không tách thành nhiều loại entity — chunk actor giữ nguyên độ đơn giản.

---

## 6. Làng mạc — hàng xóm, không phải đồng hồ đếm ngược

Thế giới cũ chỉ có *đất trống + cứ điểm + nông trại bạn*. Không ai khác sống ở đó — với một game
nhiều người chơi thì đó là thế giới chết. Làng mạc sửa điều đó.

> **Đính chính so với bản trước.** Tôi từng thiết kế làng **tụt bậc nếu người chơi không cứu**, để
> tạo áp lực toàn cục. Đó là áp lực môi trường thường trực và nó **phá tông chill** ([§0](#0-triết-lý-chill-là-mặc-định-thử-thách-là-tuỳ-chọn)).
> Bỏ. Làng **lên bậc khi được giúp, và chững lại khi không** — chúng không thối rữa sau lưng bạn.

### Năm bậc

| Bậc | Tên | Dân số | Quân sự | Cung cấp cho người chơi |
|---|---|---|---|---|
| 0 | **Trại lẻ** | 2–5 | không | trú tạm, mua bán vặt |
| 1 | **Xóm** | 5–15 | 1–2 dân binh | dùng nhờ trạm chế tạo cơ bản |
| 2 | **Làng** | 15–40 | 3–6 vệ binh | chợ, nhiệm vụ, **điểm hồi sinh** |
| 3 | **Thị trấn** | 40–100 | 8–15 vệ binh + **tường** | kho gửi đồ, đủ trạm chế tạo |
| 4 | **Thành lũy** | 100+ | **quân đội thường trực 20+** | thương nhân bậc cao, **xuất quân đánh cứ điểm** |

Bậc của làng khi thế giới sinh ra phụ thuộc **vòng** ([§4](#4-vùng-đất-độ-khó-toả-ra-từ-tâm)): thị
trấn và thành lũy nằm gần tâm; càng ra ngoài càng chỉ còn trại lẻ và xóm liều lĩnh.

### Bậc đi lên — chậm, và do bạn

**Lên** khi: bạn giao thương đều · bạn giúp phòng thủ · có đường nối tới làng khác · một cứ điểm gần
đó bị đóng.

**Chững** khi bạn không làm gì. Không tụt.

**Tụt** chỉ trong một trường hợp: bạn **chủ động chọc** một cứ điểm gần đó và để nó phản đòn. Tức là
hậu quả của một hành động bạn cố ý làm, không phải hình phạt cho việc nghỉ ngơi.

Điều này giữ được phần thưởng ("làng quê mình lớn lên vì mình") mà bỏ đi cây gậy ("làng chết vì mình
nghỉ một tuần"). Với game nhiều người chơi, cây gậy còn tệ hơn nữa — nó phạt người bận rộn.

### Đột kích — ngẫu nhiên, không theo lịch

Làng thỉnh thoảng bị quái đột kích. **Không có lịch.** Mỗi *tháng* trong game, mỗi làng gieo một
xác suất — khoảng **30%** ở mức cơ sở.

Đây là quyết định đúng tông, và lý do đáng nói rõ: **một cuộc đột kích có lịch chính là một cái đồng
hồ đếm ngược** — đúng thứ [§0](#0-triết-lý-chill-là-mặc-định-thử-thách-là-tuỳ-chọn) cấm. Ngẫu nhiên
thì khác hẳn: nó là *thời tiết*, không phải *deadline*.

**Nhịp thực tế.** 1 ngày game ≈ 20 phút thật; 1 tháng game ≈ 2,3 ngày ≈ 47 phút thật. Với 30% mỗi
tháng, một làng cụ thể bị đột kích trung bình mỗi ~7 giờ chơi. Nhưng bản đồ có **40–60 làng**, nên:

> **Hiếm với quê bạn, nhưng thường xuyên ở đâu đó trên bản đồ.**

Đó là tính chất rất đáng giá và nó rơi ra miễn phí: nhà bạn hầu như luôn yên, trong khi **luôn có
một cuộc đột kích bạn *có thể* đến giúp** nếu muốn. Nội dung tuỳ chọn tự sinh ra, không cần hệ thống
nhiệm vụ nào.

### Xác suất dịch chuyển được — đây là chỗ người chơi có quyền

Không phải 30% cứng. Nó là tích của mấy hệ số:

| Hệ số | Tăng rủi ro | Giảm rủi ro |
|---|---|---|
| **Vòng** | làng ở vòng ngoài | làng ở vòng trong |
| **Cứ điểm gần đó** | để nó luyện tập lâu ngày | bạn có đột kích nó |
| **Phòng thủ** | — | bậc làng cao, có tường, công trình bạn xây giúp |
| **Đã lâu không bị** | dồn dần lên | vừa bị xong thì hạ về đáy |

Hệ số cuối là **chống xui**: xác suất tăng dần theo thời gian kể từ lần đột kích trước, rồi reset.
Nếu không có nó, ngẫu nhiên thuần sẽ cho ra cả hai kiểu tệ — bị ba lần liên tiếp, hoặc im ắng cả
năm. Cái nào cũng làm hỏng cảm giác về nhịp.

Kết quả: **bạn không ngăn được đột kích, nhưng dịch được xác suất.** Không có nghĩa vụ nào, mà vẫn
có quyền tác động — đúng công thức chill.

### Báo trước, và làng tự chống được

Hai luật khiến việc ngẫu nhiên trở nên an toàn về mặt thiết kế:

1. **Báo trước một ngày.** Một người do thám chạy về làng, tin đồn lan ra. Bạn **chọn** có mặt hay
   không. Đó là lời mời, không phải cú phục kích — một cuộc đột kích ập xuống lúc bạn đang tưới rau
   thì chỉ gây bực.

2. **Làng bậc ≥2 tự chống được.** Vệ binh (đã luyện RL) đánh trận đó dù bạn có đến hay không. Bạn
   đến thì trận đấu **tốt hơn**, không phải *chỉ khi đó mới có kết quả*. Đây cũng là lúc hệ thống RL
   trở nên hữu hình nhất: bạn thấy đám vệ binh mình từng xem tập luyện, giờ đánh thật.

### Mất gì khi thua

**Không tụt bậc.** Công trình hư hại, hàng hoá mất, việc lên bậc chững lại vài tuần trong khi làng
sửa chữa. Bạn có thể giúp sửa cho nhanh.

Lý do không cho tụt bậc: tụt bậc vì một cú gieo xúc xắc mà bạn tình cờ đang offline là **hình phạt
cho việc có cuộc sống riêng**. Đó chính là kiểu thiết kế mà toàn bộ phần tông chill đang tránh.

**Một ngoại lệ duy nhất:** nếu bạn *chủ động* chọc một cứ điểm và nó phản đòn, cuộc đột kích đó **có
thể** làm tụt bậc. Hậu quả của một hành động bạn cố ý làm — hoàn toàn khác với xui xẻo.

### Quân sự và RL — chỉ chiến binh

Bạn nói đúng khi giới hạn RL cho chiến binh. Nhưng có một điều chỉnh kỹ thuật bắt buộc:

> **Một chính sách (policy) cho mỗi *nguyên mẫu*, không phải cho mỗi cá thể.**

Nếu mỗi vệ binh có mạng riêng: 15 làng × 5 chiến binh = 75 mạng phải huấn luyện. Không kham nổi, và
**học kém hơn** — mỗi mạng chỉ nhận 1/75 lượng dữ liệu.

Thay vào đó khoảng **4–6 nguyên mẫu dùng chung**: `VệBinhThương`, `VệBinhCung`, `LínhThường`,
`ĐộiTrưởng`. Mọi vệ binh cầm thương trong thế giới **dùng chung một bộ não**, khác nhau ở chỉ số và
trang bị. Tổng cả quái lẫn người chỉ khoảng **10–15 policy**.

Nông dân, thương nhân, trẻ con: **cây hành vi viết tay**, không RL.

### Hai võ đường

Bạn muốn mỗi phe có sân tập riêng, và tôi đồng ý — nó tốt hơn ý tưởng một-đấu-trường-chung của tôi,
vì hai lý do tôi đã bỏ qua:

1. **Người chơi nhìn thấy được.** Ghé làng, thấy vệ binh đang tập với nhau ở sân sau. Vào hầm ngục,
   thấy quái đang đấu tập. Tính năng RL trở nên *hữu hình* thay vì một con số trong menu.
2. **Mỗi phe cần luyện thứ khác nhau.** Vệ binh luyện đội hình và phối hợp; quái luyện áp sát và bao
   vây. Trộn chung từ đầu thì cả hai đều học nửa vời.

```
   VÕ ĐƯỜNG (trong làng)              GIẢNG ĐƯỜNG (trong hầm ngục)
   vệ binh tự đấu, luyện đội hình     quái tự đấu, luyện áp sát
              └──────── GIAO LƯU định kỳ ────────┘
              đấu với checkpoint ĐÓNG BĂNG của phe kia
```

**Giao lưu dùng checkpoint đóng băng của đối phương**, không phải cả hai cùng học một lúc. Nếu hai
bên cùng thay đổi trong lúc đấu, chúng đuổi nhau và không bên nào hội tụ — đây là kỹ thuật chuẩn
(league training) và cũng là cách duy nhất để việc giao lưu thật sự có ích.

Móc vào gameplay: **đột kích hầm ngục làm gián đoạn buổi tập**, lùi checkpoint phe quái vài thế hệ.
Việc "vào đánh lấy đồ" có hệ quả cơ học đo được — và vì hoàn toàn tuỳ chọn, nó không phá tông chill.

### Chọn điểm xuất phát

Lần đăng nhập đầu, người chơi **chọn một làng hoặc thị trấn ở vòng trong** làm quê. Việc này cho:

- **Hướng dẫn tự nhiên**: quê bạn là nơi giao nhiệm vụ đầu, chỗ hồi sinh, chỗ bán đồ đầu tiên.
- **Nhóm bạn tự nhiên**: bạn bè chọn cùng làng là ở cạnh nhau, không cần hệ thống guild.
- **Tính cách vùng miền**: làng chài, làng mỏ, làng nông — mỗi nơi một nghề, một giọng.
- **Lý do quan tâm**: quê bạn lớn lên nhờ bạn. Gắn bó cảm xúc gần như miễn phí.

Hệ quả thiết kế: **làng phải là thực thể bền vững có danh tính** — tên riêng, lịch sử, được lưu —
không phải nhiễu sinh ngẫu nhiên mỗi lần chạy.

---

## 6b. Khởi đầu: tay trắng giữa đồng không

> **Quyết định 2026-07-22.** Người chơi **không** bắt đầu với một nông trại dựng sẵn. Họ tỉnh dậy ở
> giữa đồng không mông quạnh và phải **đi tìm ngôi làng gần nhất**.

Quyết định này đến từ một bế tắc rất cụ thể về art, và nó giải bế tắc đó một cách sạch sẽ.

### Bế tắc

Công trình người chơi tự xây (tường, tháp, rào) cần sprite **vừa đúng một ô**, vì người chơi đặt
từng ô một. Nhưng Ninja Adventure **không có ô tường hay tháp đơn lẻ nào** — `TilesetHouse` (759 ô)
và `TilesetTowers` (144 ô) toàn là *mảnh* của công trình lớn hơn. Rào cũng cao 2 ô.

Ta đã thử ép và kết quả xấu hơn thứ nó thay thế (xem `assets/CREDITS.md`).

### Lời giải: công trình đến từ làng, không từ ô lẻ

Nếu người chơi không bắt đầu bằng việc xây, thì **không cần sprite một ô**:

- **Làng** là công trình **nhiều ô, đặt nguyên khối** bởi bộ sinh thế giới — đúng thứ bộ art này
  dựng sẵn để làm. Nhà, tường thành, tháp canh, quầy hàng, hàng rào quanh làng.
- Người chơi **học nghề xây từ làng**, và những gì họ dựng về sau cũng là **công trình nhiều ô**
  đặt nguyên khối (một cái lều, một cái chòi, một đoạn rào 3 mảnh) — chứ không phải tô từng ô như
  vẽ pixel.

Đổi từ "đặt từng ô" sang "đặt nguyên công trình" **vừa hợp art vừa hợp cảm giác**: bạn dựng một
*cái lán*, không phải tô 9 ô tường.

### Hệ quả

| | |
|---|---|
| **Bỏ nông trại khởi đầu** | Gỡ luôn mảng đất 13×13 đang bị nhét cứng trong `terrain_of`. Người chơi rơi xuống đất tự nhiên |
| **Bỏ tường/tháp/rào đặt-từng-ô** | Chúng là thứ đang chặn đường, và giờ không cần nữa |
| **Làng thành thiết yếu, không phải trang trí** | Không tới được làng thì không có gì để làm. Đây là lý do worldgen đặt làng phải lên trước mọi thứ khác |
| **Mở đầu game có hình dạng rõ** | Tỉnh dậy → nhìn quanh → đi. Không menu, không hướng dẫn, không thanh nhiệm vụ |

Cũng hợp tông chill: bạn bắt đầu bằng một chuyến đi bộ, không phải bằng một danh sách việc.

---

## 6c. Không khí: lá bay, mưa, gió

Ví dụ của bộ art có **lá bay ngang màn hình**, và đó là thứ rẻ nhất để một thế giới trông như đang
sống thay vì đang chờ.

| Lớp | Nội dung | Nguồn |
|---|---|---|
| **Hạt môi trường** | lá bay theo gió, bụi ở sa mạc, tuyết rơi, đom đóm ban đêm | `FX/` (105 file) |
| **Thời tiết** | mưa, bão, sương mù — do MapDirector phát, xem [§9](GAME.md) | `FX/` + overlay |
| **Ô động** | gợn nước, cỏ lay, cờ bay, cối xay quay | `Backgrounds/Animated/` |

Hai điểm kỹ thuật đáng nói:

1. **Hạt là thuần trang trí — không đi qua mô phỏng.** Chúng sống trong renderer, sinh từ
   `(seed, vị trí camera, thời gian)`. Không actor nào biết chúng tồn tại, nên chúng không tốn gì
   về kiến trúc và không bao giờ cần đồng bộ giữa các node.
2. **Ô động là hàm của thời gian thế giới**, không phải của frame — nên gợn nước ở hai máy khác nhau
   vẫn cùng pha, và ảnh chụp vẫn tái lập được.

Đây là thứ trả lại nhiều nhất trên mỗi dòng code trong toàn bộ danh sách còn lại.

---

## 7. Nhân vật và kỹ năng

### Không có class cứng

Người chơi lên **kỹ năng theo việc mình làm** (dùng cung → tăng cung). Không khoá class, nhưng có
**giới hạn tổng điểm kỹ năng**, nên vẫn phải chuyên môn hoá. Hợp MMO: người chơi bù trừ cho nhau.

### Bốn nhánh

| Nhánh | Ví dụ |
|---|---|
| **Cận chiến** | Chém quét, Đỡ đòn, Húc |
| **Tầm xa** | Bắn tỉa, Mưa tên, Bẫy |
| **Phép** | Hoả / Băng / Thổ / Lôi |
| **Nghề** | Canh tác, Khai mỏ, Chế tạo, Nấu ăn |

### Kết hợp vật lý + phép — cơ chế cốt lõi

Phép **đặt trạng thái**, đòn vật lý **kích nổ trạng thái**. Đây là cách làm "vật lý và phép kết hợp"
trở nên cụ thể thay vì chỉ là hai thanh máu riêng:

| Trạng thái (phép) | Đòn vật lý | Kết quả |
|---|---|---|
| **Đóng băng** (Băng) | cận chiến nặng | *Vỡ vụn* — sát thương ×2.5, bỏ qua giáp |
| **Bỏng** (Hoả) | tên | *Nổ* — sát thương lan 2 ô |
| **Ướt** (Băng yếu/mưa) | Lôi | *Dẫn điện* — lan sang mọi kẻ địch ướt gần đó |
| **Dính bùn** (Thổ) | húc | *Nghiền* — choáng 2 giây |
| **Nhiễm điện** (Lôi) | cận chiến | *Phóng hồ quang* — hồi mana |

Hệ quả hay: **thời tiết cũng đặt trạng thái**. Trời mưa → mọi thứ *Ướt* → build Lôi mạnh lên trong
mưa. Mùa đông → hồ đóng băng → vừa mở lối đi mới cho quái, vừa cho phép *Vỡ vụn* dễ hơn.

---

## 8. Chế tạo, tài nguyên, trang bị

**Bậc vật liệu.** Đồng → Sắt → Thép → Bí ngân (Mythril). Mỗi bậc cần một tầng mỏ sâu hơn, mà tầng
sâu hơn có quái mạnh hơn — nên tiến trình chế tạo bị gắn với tiến trình chiến đấu.

**Trạm chế tạo.** Bàn thợ mộc → Lò rèn → Bàn phù phép (cần Tinh chất) → Bàn luyện kim.

**Trang bị** có **độ bền** và **ổ khảm** (socket). Đá khảm rơi từ hầm ngục, cho hiệu ứng gắn với hệ
combo ở trên (ví dụ: đá Hoả cho vũ khí cận chiến tự đặt *Bỏng*, mở combo tự thân).

---

## 9. Mùa và thời tiết

| Mùa | Cây trồng | Thời tiết | Ảnh hưởng chiến đấu |
|---|---|---|---|
| Xuân | mọc nhanh +25% | mưa nhiều | Ướt thường xuyên → build Lôi mạnh |
| Hạ | cần tưới | hạn, bão | Hoả mạnh; cháy lan trên cỏ khô |
| Thu | thu hoạch +50% | gió | tầm xa lệch; đêm dài dần |
| Đông | cần sưởi | tuyết, băng | **hồ đóng băng → lối tấn công mới**; Băng mạnh |

**Thời tiết** đổi trong ngày: quang, mưa, bão, sương mù, tuyết. Sương mù giảm tầm tháp — điều đó
biến thời tiết thành yếu tố chiến thuật thật, không chỉ là hiệu ứng hình ảnh.

---

## 10. RL — quái và vệ binh cùng tự luyện

Đây là tính năng đặc trưng. Cần nói thẳng vài điều trước.

### Đính chính thuật ngữ

Bạn mô tả "ML tự học không giám sát". **Học không giám sát** (unsupervised) là tìm cấu trúc trong dữ
liệu không nhãn — phân cụm, giảm chiều. Đó không phải thứ bạn đang mô tả.

Thứ bạn mô tả — quái đánh nhau để giỏi hơn *ở việc tấn công* — là **học tăng cường bằng tự đấu**
(self-play reinforcement learning). Phân biệt này quan trọng vì nó quyết định cái gì làm được: RL có
tín hiệu thưởng rõ ràng (gây được bao nhiêu sát thương, sống được bao lâu), còn unsupervised thì
không có gì để tối ưu.

### Đã có sẵn phần khó nhất

`/home/nvthanh/works/windows-machine-self-learn` (RLDrive) **đã có một DQN hoàn chỉnh bằng C++20**:

| Dùng lại được | Ghi chú |
|---|---|
| `core/network/MlpCpu` | mạng dense, backward đã được kiểm chứng bằng gradient-check |
| `core/rl/{DqnAgent,DqnTrainer,ReplayBuffer,ActionSpace,Experience}` | vòng huấn luyện đầy đủ |
| `core/io/NetworkCheckpoint` | lưu/nạp trọng số dạng JSON |
| `core/rl/ITrainerBackend` | seam để cắm backend GPU sau |

`core/` là **2377 dòng C++20 thuần, build sạch trên Linux** (chỉ `gpu/` mới gated WIN32). Phần
*không* dùng lại được là `core/env/` (mô hình xe hơi) — ta viết `MonsterEnvironment` thay vào. Đó
đúng là chỗ seam đã được thiết kế sẵn: `DqnAgent` không biết gì về môi trường.

**Đây là tiết kiệm lớn nhất trong toàn bộ kế hoạch này.**

### Thiết kế: hai võ đường + giao lưu

```
TrainingActor (Require<Trusted>, Priority<2> — nhường đường cho gameplay)
  │
  ├─ VÕ ĐƯỜNG LÀNG              ─ vệ binh tự đấu, policy theo nguyên mẫu
  ├─ GIẢNG ĐƯỜNG HẦM NGỤC       ─ quái tự đấu, policy theo loài × bậc
  └─ GIAO LƯU (định kỳ)         ─ mỗi bên đấu với checkpoint ĐÓNG BĂNG của bên kia
        │
        └─► NetworkCheckpoint (thế hệ +1) → công bố {id, generation, hash}
                         │
                         ▼
      quái/vệ binh sinh ra nạp checkpoint tương ứng → hành vi = suy luận từ mạng
```

**Vì sao checkpoint đóng băng khi giao lưu:** nếu cả hai bên cùng học trong lúc đấu, chúng đuổi nhau
và không bên nào hội tụ. Đóng băng một bên là kỹ thuật chuẩn (league training) — và là điều kiện để
việc giao lưu thật sự có ích thay vì chỉ tốn CPU.

### Bốn ràng buộc bắt buộc

1. **Huấn luyện không nằm trên đường tới hạn.** `Priority<2>` + ngân sách drain giới hạn, có thể tạm
   dừng. Nếu training tranh CPU với mô phỏng thế giới thì game giật — và không ai đánh đổi framerate
   lấy "quái thông minh hơn".

2. **Chỉ một node huấn luyện mỗi hầm ngục.** Mạng nơ-ron là số thực; hai node huấn luyện song song
   sẽ ra hai kết quả khác nhau. Kết quả phát tán dưới dạng **checkpoint bất biến + hash**, không
   phải tính toán lặp lại. Đây cũng là lý do `Require<Trusted>`: một node người chơi có thể huấn
   luyện ra quái *cố tình yếu* để cày dễ.

3. **Phải có trần độ khó.** Đường cong kỹ năng bão hoà. Một hệ RL không chặn trên sẽ tạo ra game
   không thể thắng — đó không phải độ khó, đó là hỏng.

4. **Người chơi phải nhìn thấy được.** Hiển thị "Giảng Đường Hoả — Thế hệ 47". Nhưng quan trọng hơn:
   **cho xem tận mắt.** Ghé làng thấy vệ binh tập ở sân sau; vào hầm ngục thấy quái đấu tập. Một con
   số trong menu thì dễ bỏ qua; một sân tập đang hoạt động thì không.

5. **Quái đã luyện KHÔNG tự tìm đến nhà bạn.** Chúng ở trong hầm ngục của chúng. Đây là điều giữ cho
   tính năng này không phá tông chill ([§0](#0-triết-lý-chill-là-mặc-định-thử-thách-là-tuỳ-chọn)) —
   nó là đường cong độ khó chờ bạn tới lấy, không phải đồng hồ hẹn giờ.

### Giảm rủi ro

Nếu RL không đạt chất lượng mong muốn, **vẫn giữ được vòng lặp chiến lược**: thay `DqnAgent` bằng
một bảng hành vi có tham số tăng dần theo "thế hệ". Người chơi thấy y hệt. Nói cách khác — thiết kế
game **không cược vào việc RL hoạt động tốt**, và đó là điều bắt buộc với một tính năng nghiên cứu.

---

## 11. Multiplayer

- **Server bền vững**, không phải phòng chơi: thế giới chạy tiếp khi bạn offline. Cây vẫn lớn, đêm
  vẫn đến, căn cứ vẫn có thể bị phá.
- **Quy mô**: 20–50 người chơi đồng thời mỗi thế giới. Không đuổi theo con số 10⁴ — nó không phục vụ
  game này.
- **Sở hữu**: mỗi người chơi có **địa giới** (claim) quanh căn cứ. Trong địa giới chỉ chủ và người
  được cấp quyền mới xây/phá được.
- **Hợp tác**: hầm ngục thiết kế cho nhóm 2–4. Đợt đêm quy mô lớn khuyến khích phòng thủ chung.
- **PvP**: mặc định **tắt**. Latency 50–150 ms của cluster máy gia đình không hợp PvP hành động, và
  PvP mâu thuẫn với tiền đề "cùng nhau giành lại đất". Có thể thêm đấu trường tự nguyện sau.

Chi tiết kỹ thuật (đăng nhập, lưu trữ, tin cậy): [ARCHITECTURE.md](ARCHITECTURE.md).

---

## 12. Giao diện

Bỏ HUD overlay hiện tại. Số liệu như "CHUNK MIGRATIONS" là chỉ số kỹ thuật, chuyển vào màn hình
debug (F3).

| Màn hình | Nội dung |
|---|---|
| **Menu chính** | Tiếp tục / Chọn thế giới / Đăng nhập / Tuỳ chọn / Thoát |
| **Nhân vật** (`C`) | chỉ số, kỹ năng, trang bị |
| **Túi đồ** (`I`) | vật phẩm, độ bền, ổ khảm |
| **Chế tạo** (`B`) | công thức theo trạm, lọc theo nguyên liệu đang có |
| **Bản đồ** (`M`) | địa giới, cứ điểm + thế hệ, hầm ngục, mỏ, căn cứ đồng đội |
| **Nhật ký** (`J`) | nhiệm vụ, hướng dẫn, bách khoa |
| **Tuỳ chọn** | phím tắt, âm thanh, hiển thị |

HUD trong game chỉ giữ: máu/mana/thể lực, thanh kỹ năng, đồng hồ ngày/mùa, cảnh báo đợt tấn công.

**Công cụ:** [raygui](https://github.com/raysan5/raygui) (header-only, MIT, cùng tác giả raylib) +
Kenney *UI Pack RPG Expansion* / *Fantasy UI Borders* / *Game Icons* (đã tải).

---

## 13. Tài sản — và câu trả lời cho ngân sách nhân vật

### Quyết định: **0 đồng.**

Bạn hỏi nên chi bao nhiêu cho nhân vật. Câu trả lời là **đừng chi gì lúc này**, và lý do quan trọng
hơn con số:

> Với một game đầu tay, rủi ro không phải là *trông xấu* — rủi ro là *không hoàn thành*. Chi tiền
> mua art trước khi game vui là sai lầm kinh điển của dự án đầu tiên. Và bạn không thể đặt hàng tốt
> khi chưa biết mình cần đúng những hoạt ảnh nào.

Và hoá ra không cần chi thật: **Ninja Adventure Asset Pack** (Pixel-Boy & AAA) lấp đúng khoảng trống
lớn nhất. Giấy phép đã được **xác minh trực tiếp trong file `LICENSE.txt` của pack** — CC0 1.0
Universal, đầy đủ nguyên văn, cho phép thương mại, không bắt buộc ghi công.

| Nội dung | Số lượng |
|---|---|
| Nhân vật (`Actor/Character`) | **95** — có `Walk.png` 64×64 = **4 hướng × 4 frame**, cộng Idle/Attack/Dead/Jump |
| Quái (`Actor/Monster`) | 66 |
| Boss | 20 |
| Động vật (`Actor/Animal`) | 27 — **bò, gà, chó, mèo, cừu**: đúng thứ chăn nuôi cần |
| Hiệu ứng (`FX`) | 105 |
| Vật phẩm | 142 |
| UI | 361 |
| Âm thanh + nhạc | 188 |
| Chân dung (`Faceset.png`) | mỗi nhân vật một cái — dùng cho hộp thoại |

**Và nó là 16×16** — khớp chính xác lưới hiện tại. Không phải scale, không lệch tỉ lệ với tile
Kenney.

### Khi nào thì nên chi tiền

| Thời điểm | Chi | Vì sao |
|---|---|---|
| Bây giờ → P3 | **0 đ** | Ninja Adventure + Kenney đủ dùng |
| Sau P4 (thế giới đã bền vững, game đã vui) | ~$30–60 | mua thêm pack itch.io *nếu* thiếu một thứ cụ thể |
| Chỉ khi chuẩn bị phát hành | $300–600 | đặt vẽ riêng — lúc đó bạn biết chính xác cần gì, nên đặt hàng rẻ và trúng hơn |

**Cảnh báo giấy phép** (quan trọng với dự án mã nguồn mở): trên itch.io chữ "free" thường có nghĩa
*miễn phí cho dùng cá nhân*, **không** phải cho thương mại. Luôn mở file LICENSE trong pack và đọc,
đừng tin thẻ tag. LPC/Universal-LPC là kho nhân vật lớn nhưng **CC-BY-SA** — lây nhiễm sang tác phẩm
phái sinh; hợp với mã nguồn mở nhưng phải biết mình đang chấp nhận gì. Với CC0 thì không có ràng buộc
nào cả.

### Toàn bộ kho hiện có

| Pack | Dùng cho | Giấy phép |
|---|---|---|
| **Ninja Adventure** | **nhân vật, quái, boss, vật nuôi, FX, vật phẩm, UI, âm thanh** | CC0 (đã đọc LICENSE.txt) |
| Roguelike/RPG (Kenney) | địa hình, cây, cây trồng | CC0 |
| Tiny Town (Kenney) | công trình, hàng rào | CC0 |
| Tiny Dungeon (Kenney) | quái/người chơi tạm thời | CC0 |
| Roguelike Caves & Dungeons | hầm ngục, mỏ | CC0 |
| UI Pack RPG + Fantasy UI Borders | menu | CC0 |
| Game Icons (425) | biểu tượng vật phẩm/kỹ năng | CC0 |
| Particle Pack (193) | hiệu ứng phép | CC0 |
| RPG Audio + Music Jingles | âm thanh | CC0 |

**Không còn khoảng trống asset nào chặn đường.** Việc còn lại chỉ là chọn ra tập con và đóng gói —
`tools/build_atlas.py` đã làm sẵn.

---

## 14. Điều tôi lo nhất

**Phạm vi.** Đây là hơn 10 hệ thống lớn. Làm song song cả 10 thì không cái nào xong. Lộ trình trong
[ROADMAP.md](ROADMAP.md) sắp xếp sao cho **mỗi giai đoạn kết thúc bằng một game chơi được**, và
tính năng rủi ro nhất (ML) được đặt sau khi vòng lặp cốt lõi đã vững — để nếu nó thất bại thì bạn
vẫn có một game.

Hai rủi ro tôi từng lo nay đã hết:

- ~~Thiếu nhân vật có hoạt ảnh 4 hướng~~ → Ninja Adventure CC0, 16×16, đã xác minh giấy phép.
- ~~Cần VPS làm node tin cậy~~ → node đầu tiên là leader; không cần hạ tầng gì.

Rủi ro còn lại xếp theo mức độ:

1. **Phạm vi.** Vẫn là số một, và không có cách nào loại bỏ — chỉ có cách quản lý bằng thứ tự.
2. **Sinh thế giới** ([ARCHITECTURE.md §8](ARCHITECTURE.md)). Hệ thống tôi đã bỏ sót hoàn toàn ở
   bản kế hoạch trước. Đặt làng/cứ điểm/mỏ/đường sao cho mạch lạc khó hơn sinh địa hình nhiều.
3. **RL cho ra hành vi nhạt.** Agent có thể học được cách "đứng yên và đánh" rồi dừng ở đó. Đường
   lui đã chuẩn bị: bảng hành vi theo thế hệ, người chơi không phân biệt được.
