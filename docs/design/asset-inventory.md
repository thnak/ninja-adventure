# Kiểm kê asset và "những thứ build được"

> **Lưu ý:** tài liệu này viết khi thiết kế cũ (nông trại, GAME.md) còn hiệu lực. Các mục về trồng trọt, nông trại, phòng thủ làng nay **không còn là yêu cầu**; chỉ dùng phần kiểm kê asset và gợi ý ghép asset.

Phạm vi: `assets/_src/` (mọi pack), `assets/audio/`, `game/` (project Godot mẫu của tác giả pack).
Mục tiêu: không bỏ sót nhóm asset nào; mỗi nhóm gắn với ít nhất một thứ build được, hoặc ghi lý do bỏ.

**Cột "Xem"**, cho biết mức kiểm chứng:
- **X**: đã mở ảnh thật (upscale, có lưới) và nhìn.
- **P**: chỉ xem ảnh preview hoặc AllPreview của pack.
- **T**: suy ra từ tên file hoặc kích thước, chưa nhìn.

Contact sheet đã render nằm trong `asset-sheets/` (monsters, animals, bosses, items_particles,
emote_skill_icons, animated_vehicles, nature_ore_nodes, tileset_house_grid, tileset_element_grid).

**Giấy phép.** Mọi pack đều là **CC0**. Đã đọc `LICENSE.txt`/`License.txt` của từng pack: Ninja
Adventure (Pixel-Boy & AAA) và 9 pack Kenney. Không bắt buộc ghi công, nhưng nên giữ link.
Lưu ý: `assets/CREDITS.md` viết cho kiến trúc cũ (atlas, world gen), nên cần viết lại.

---

## 1. Kiểm kê

### 1a. Ninja Adventure (`assets/_src/ninja/`), pack chính, một phong cách thống nhất

| Nhóm | Số lượng | Kích thước / định dạng | Xem | Ghi chú |
|---|---|---|---|---|
| Actor/Character | 95 thư mục (89 sheet đầy đủ) | `SpriteSheet` 64×112 = ô 16×16, 4 cột (hướng) × 7 hàng (walk ×4, attack, jump, item/special); `Faceset` 38×38; `SeparateAnim/` | X (8 sheet) + P (AllPreview) | Child, OldWoman chỉ 64×32 (chỉ đi bộ). Gồm ninja ×17 màu, samurai, dân làng ×7, ông/bà lão, sư, pháp sư, skeleton, robot, quý tộc, công chúa, eskimo, người hang... |
| Actor/CharacterAnimated/NinjaGreen | 1 nhân vật | 256×544 = ô **32×32**, 8×17; tách lẻ: Idle, Walk, Attack, Jump, **Swim, Climb, Push, Roll, Pickup, Item**, Hit, Dead | X | **Nhân vật duy nhất có bộ động tác đầy đủ.** |
| CharacterAnimated/Weapon | 5 | 256×256 overlay khớp khung với NinjaGreen: Axe, Hammer, Katana, **Net**, Pickaxe | X (Hammer, Net) | Net là vợt bắt côn trùng/cá. |
| Actor/Monster | 66 | 64×64 = 16×16, 4 hướng × 4 khung + Faceset | X (cả 66) | Slime ×4, snake ×4, bat, spider, skull, mushroom, octopus, kappa, lantern, mole, owl, eye, spirit, flame, larva, T-rex, panda, racoon... Nhiều con là cặp palette swap sẵn. |
| Actor/Boss | 20 (10 mẫu × 2 màu) | dải ngang riêng theo hành động (Idle/Walk/Attack/Hit/Jump/Charge/Shoot/Trans), khung 40–96 px | X (Idle 10 con) | Dragon là bộ phận rời (Head, Body1/2, BodyEnd, Wing), ráp kiểu rắn. Samurai khổng lồ có Attack/Charge trái-phải 96 px. |
| Actor/Animal | 27 thư mục, ~60 sheet | dải 2 khung, ~16×16 (Lion 32×23), **một hướng** (lật để đổi hướng); một số có bản `Side` 3 khung | X (cả bộ) | Gà ×4 màu, bò, lợn ×3, ngựa ×2, lừa, chó ×6, mèo ×6, chuột hamster ×4, cá ×3, ếch, khỉ ×3, vẹt ×2, sư tử, linh cẩu, lợn rừng, gấu mèo. **Không có cừu, thỏ, hươu.** |
| Backgrounds/Tilesets | 19 sheet + 4 Interior | tất cả ô 16×16, **không gutter** (TilesetFloor cao 417 px, thừa 1 hàng pixel) | X (cả 23 sheet) | Chi tiết ở bảng 1b. |
| Backgrounds/Animated | 9 nhóm | Flag 8 màu (4 khung 16), Flower, Plant (4 khung), MillPropeller 64×64 ×4 khung ×2 màu, Watermill 34×36 ×3 ×2, Waterfall top/mid/bottom, Water Ripples (xanh/tím), QuickSand 32×32 ×8, Conveyor Belt L/M/R | X | |
| Backgrounds/Vehicles | 5 | Boat 80×32, Crane, FishNet/FishNetFull, Sail | X | |
| FX/Attack, Slash | 8 + 6 | dải khung 32 px; slash lớn tới 55 px | P | |
| FX/Elemental | 9 loại (12 sheet) | Explosion, Flam, Ice (+B, Flake), Plant (+B), Rock (+B), RockSpike, Thunder, Water, WaterPillar | P | |
| FX/Magic | 6 loại (14 sheet) | Aura, Boost, Circle ×4, Shield ×2, Spark, Spirit ×3 | P | |
| FX/Particle | 14 | dải nhỏ 7–16 px: Bamboo, Clouds, Fire, Grass, Leaf, LeafPink, Rain, RainOnFloor, Rock, RockGray, Snow, Spark, Vase, Wood | X | |
| FX/Projectile | 14 + Kunai/Shuriken lớn | Arrow, EnergyBall, Fireball, IceSpike, PlantSpike, CanonBall, Rock... | P | |
| FX/Smoke, Environment | 2 + 2 | Smoke, SmokeCircular; **Fog** 320×180, **Raylight** 216×102 | X (Env) | |
| Items/* | 115 PNG | icon 6–16 px, tĩnh. Food 24, Resource 15, Tool 8, Object 11, Potion 6, Scroll 7, Treasure 10, Projectile 7, Other 4, Action 2, Weapons 23 (Sprite + SpriteInHand) | X | Food có Seed1-3 (hạt), **SeedBig1-3 = cà rốt / củ cải trắng / củ dền**, SeedLarge(White). Tool: **Hoe, WateringCan, Sickle, Shovel**, Axe, Pickaxe, Hammer, Anvil. |
| Ui/Dialog | 8 | DialogBox, DialogBoxFaceset, ChoiceBox, FacesetBox, Yes/No, DialogInfo (bong bóng) | X | |
| Ui/Emote | 30 | bong bóng 16 px: !, ?, tim, tim vỡ, zzz, giận, vui... | X | |
| Ui/Skill Icon | 120 (60 + bản Disabled) | 24×24 có khung màu. Items&Weapon 12, **Job&Action 12 (Cook, Dish, Harvest, Interact, Mine, Plant, Potion, Punch, Repair, Sing, Talk, Water)**, **Meteo 4 (Sun, Moon, Rain, Snow)**, Spell 32 | X | |
| Ui/Receptacle | 6 + 11 + 16 | tim (3 bộ), thanh máu mini, bình chữ nhật/cầu (máu, mana, tím, trắng...) | X | |
| Ui/Theme | Theme Wood 43 + 12 theme WIP (mỗi cái 1 nine-patch) | nút, checkbox, slider, tab, panel 9-slice | X | `game/theme/theme_1` đã dựng sẵn từ Theme Wood. |
| Ui/Font | 3 | NormalFont.ttf, bitmap 8×8, 24×30 | X | |
| Ui/Input | 60 phím + 37 gamepad + 7 chuột | icon gợi ý nút | T (chỉ vài cái) | |
| Audio/Musics | 41 ogg (40 bài + 1 bản ngắn) | tên theo cảnh: Village ×2, Calm Village, Peaceful, Chill, Sunny, Good Time, Dream, Quiet, Clearing, Road, Boat, Aquatic, Cave, Dungeon, Crypt, Dark Forest, Dark Castle, Temple, Ruins, Manor, Fight ×2, Tension, Final Area, Melancholia, Lament, Sad, Curse, Mystical, Revelation, Ascension, Intro, Story, Credit, End ×2 | T | Chưa nghe. Tông nhạc suy từ tên. |
| Audio/Jingles | 15 wav | GameOver ×4, LevelUp ×3, Secret ×4, Success ×4 | T | |
| Audio/Sounds | 132 wav, 11 nhóm | Alert 6, **Ambient 9 (Rain ×3, River, Storm, Wave ×2, Wind ×2)**, Bonus 10, **Creature 5 (Bird, Dog, Duck, Wings ×2)**, Elemental 26, Hit 13, Jump 8, Magic 11, Menu 25, **Voice 10**, Whoosh 9 | T | |
| Misc | — | Palette.png 10×9 (bảng màu gốc), MusicCover, preview ×2, Example 1-4.gif | X | Palette.png dùng để kiểm màu khi tint. |
| `Godot Project V4.zip` | 2354 file | = `game/` (kèm `.git`, `.godot`) | T (unzip -l) | Trùng lặp. |
| `GodotProject.zip` | 401 file | project **Godot 3** cũ: Hud/DialogBox, Npc, Monster, Spike, MusicArea, Teleporter, Maps/Village + Interior | T (unzip -l) | Chỉ dùng làm tham khảo pattern (dialog, NPC, gai, vùng nhạc). |
| `assets/_src/Ninja Adventure - Asset Pack.zip` | 2674 file | = `ninja/` + `Example 5 (Old Version).png` | T | Trùng lặp. |

### 1b. Tileset Ninja Adventure, nội dung thật (đã xem với lưới)

| Sheet | Ô (cột×hàng) | Nội dung | Autotile? |
|---|---|---|---|
| TilesetFloor | 22×26 | nền cát cam, cát hồng, cỏ lime, cỏ xanh, đất nâu luống, tuyết, đất xám; mỗi loại có bộ viền 47-ô kiểu blob; cuối sheet là vũng nước/băng và **dung nham** | Có. `tileset.tres` mới khai báo terrain `grass` và `dirt`. |
| TilesetFloorB | 11×7 | mảng tuyết phủ chồng (overlay) + băng | Có (overlay) |
| TilesetFloorDetail | 16×5 | rác vụn, cành, đá nhỏ, **búi cỏ**, búi cỏ tuyết, hoa, xương, sọ, vỏ ốc | Không, rải tự do |
| TilesetField | 5×15 | 5 khối **nền cỏ nâng** (cam, lime, xanh, hồng, tuyết) có mép dưới | 9-slice thủ công |
| TilesetWater | 28×17 | nước có bờ cát / tuyết-băng / cỏ / **đầm lầy tím**, lá súng, đá giữa nước; **ván cầu/bến gỗ** (hàng 12-15) | Có, nhiều bộ viền |
| TilesetRelief | 20×12 | vách đá 3 màu (tuyết, rêu xám-xanh, cát nâu) + mặt cao nguyên + cột thác nước / thác cát | Có |
| TilesetReliefDetail | 12×12 | cửa mỏ, miệng hang (đá / cát), thang (xám, cam), đá rời | Không |
| TilesetHole | 11×5 | hố sâu có viền | Có |
| TilesetNature | 24×21 | cây 2×2 / 3×3 (xanh, thông, tuyết, hồng anh đào, khô, thu vàng), gốc cây, bụi, hoa (hướng dương, đỏ), tre, **cây non cam/xanh**, tảng đá lớn, khối pha lê 4 màu, **mỏ quặng có 3 trạng thái cạn dần** (4 màu), đá tuyết | Không, vật thể nhiều ô |
| TilesetHouse | 33×23 | nhà mái cam / be / gạch đỏ, cửa hàng, quán, **biển DOJO**, cổng torii, **hàng rào gỗ / đá / sắt**, lều cỏ, nhà hang, igloo ×3, **tượng Phật / ếch / thần giữ cửa** (bản vàng + bản rêu), cầu tre, sàn gỗ, giếng, rổ thức ăn, kệ, thang, nhà gỗ lớn kiểu kho thóc, **8 băng rôn huy hiệu**, vòm đá | Không, prefab ghép tay |
| TilesetElement | 16×15 | đồ nội thất: tủ, kệ sách, bàn, ghế, chậu, ấm, bao gạo, bù nhìn rơm (?), **rơm/cỏ khô** (có viền), tấm phên, giường nhỏ, tranh, rèm | Chỉ phần rơm |
| TilesetVillageAbandoned | 20×12 | phế tích phủ rêu, nhà gỗ đổ nát, cây đại thụ có nhà, cột biển, gốc gãy, tượng ếch rêu | Không |
| TilesetDesert | 20×12 | nhà mái vòm xanh, tường thành cát, cọ, hồ ốc đảo, tượng sư tử đá, xương | Không |
| TilesetTowers | 24×6 | 36 sprite 32×32 xếp thành nhóm 3 theo màu (gỗ, đá xám, cát, rêu, đỏ, pha lê) | Không. Nhiều khả năng là khung hoặc trạng thái của tháp; **chưa xác minh là animation**. |
| TilesetDungeon | 12×4 | đuốc (2 khung), công tắc sàn đỏ, thùng, gai, song sắt, **khối đẩy** (băng/xanh/cam), quả cầu công tắc 5 màu, nút bấm, **tháp pháo mắt** | Không |
| TilesetLogic | 8×10 | 8 màu × {ô màu, chìa, khoá, rương, ?, X, A, B, C, D} | Không |
| Pipes | 15×3 | ống 3 màu (cam, xám, xanh) | Ghép tay |
| tileset_bed | 14×12 | giường 4 màu × 3 hướng, đệm, nôi/rào gỗ, phiến đá rêu | Không |
| tileset_camp | 23×9 | lều ×3 (cái cuối rách), khúc gỗ, thùng, **lửa trại + nồi**, bếp đá, thảm cát lớn 8×8, cuộn dây, đèn, chai | Không |
| Interior/TilesetInterior | 16×20 | tường trong 4 màu: hồng nhạt, cam, **nâu, xanh rêu** | Có (viền) |
| Interior/TilesetInteriorFloor | 22×17 | sàn gạch hồng, cam, gỗ, đá xanh, sỏi cát, **sỏi tối** | Có |
| Interior/TilesetWallSimple | 10×11 | tường đơn 4 màu + bồn/ống sàn | Có |
| Interior/Elements | 9×3 | tường thùng gỗ, ghế đá, khối đồ | Không |

### 1c. Kenney (`_src/` ngoài ninja), CC0, **khác phong cách** (nét mảnh, bảng màu khác)

| Pack | Số lượng | Định dạng | Xem |
|---|---|---|---|
| roguelike (RPG pack) | ~1.700 ô | sheet 968×526, ô 16 + gutter 1 px; 2 file `.tmx` mẫu | X (cả sheet) |
| caves (Caves & Dungeons) | ~500 ô | sheet 16 + gutter 1 px: tường đá / gỗ, đường ray mỏ, quặng màu, pha lê, cửa song sắt | P |
| tinydungeon | 132 | ô 16×16 lẻ + tilemap + `.tmx`/`.tsx`: tường, cửa, rương, 20 nhân vật/quái, vũ khí, bình | P |
| tinytown | 132 | ô 16×16: cỏ, đường, cây (có màu thu), nhà mái, hàng rào, **tổ ong**, bia tập bắn, công cụ | P |
| particles | 81 trong suốt + 81 nền đen | PNG xám 512 px: khói, lửa, tia, vòng, sao, vết chém | P |
| icons (Game icons) | 105 × (trắng/đen × 1x/2x) | 50/100 px + vector | P |
| uiborders | 151 × 2 kiểu | khung 9-slice trắng 48/96 px, gạch chia | P |
| uirpg | 85 | panel, nút, thanh, mũi tên, bàn tay trỏ (phong cách nhẵn) | P |
| audio_music | 85 ogg | 5 bộ jingle × 17 (8-Bit, Hit, Pizzicato, Sax, Steel) | T |
| audio_rpg | 52 ogg | bước chân ×10, cửa, sách, vải, kim loại, xu, chặt, cót két, dao | T |

### 1d. Đã có sẵn trong repo

- **`game/`** (Godot 4.3, chạy được trên 4.7), các hệ đã có:
  - character + actor_sprite (đọc sheet 64×112), damage/hitbox/team
  - behavior follow / follow_path, camera_grid, teleporter
  - destroyable (crate / grass / pot + particle vỡ)
  - environment_area: fog, mưa, tuyết, lá, raylight, color_correction gradient theo vùng (autumn, swamp)
  - music crossfade, UI tim (receptacle), theme_1 (Wood)
- `tileset.tres` dùng 5 atlas (village_abandoned, floor, interior_floor, wall_simple, animated = hoa 4 khung) và 1 bộ scene (crate, grass, pot). Có terrain grass/dirt.
- `map_village.tscn`: NinjaBlue (người chơi), Pig + SamuraiGreen đi theo, SamuraiBlue đi tuần theo path, 2 teleporter, 5 vùng môi trường, nhạc `theme_dream`.
- **`assets/audio/`** (11 file): đều là bản sao đổi tên từ ninja. build=Menu/Cancel, cast=Magic1, combo=Explosion, harvest=Bonus, hit=Hit1, levelup=PowerUp1, shoot=Launch, swing=Slash, swing_heavy=Sword2, ui_click=Accept, theme_day=11-Clearing.

---

## 2. Có thể build

Độ khả thi: **D** = dễ, **V** = vừa, **K** = khó.

| Thứ build được | Asset dùng | Cách ghép sáng tạo | KT | Còn thiếu |
|---|---|---|---|---|
| **Người chơi (ninja giải nghệ)** | CharacterAnimated/NinjaGreen + 5 Weapon overlay | Dùng bộ 32 px duy nhất có Swim / Climb / Push / Roll / Pickup. Đổi màu áo bằng shader palette swap (Palette.png). Hammer = sửa hàng rào, Axe = chặt cây, Pickaxe = đào mỏ, Net = bắt bướm/cá. | V | Không có động tác cuốc / tưới. Thay bằng tư thế `Item` (giơ đồ) + icon Hoe/WateringCan + particle. |
| **Ruộng: cuốc, gieo, tưới, thu** | TilesetFloor (đất nâu luống), Items/Food Seed1-3 + SeedBig1-3, cây non trong TilesetNature, Animated/Plant, FloorDetail búi cỏ | Đất thường → đất luống (TilesetFloor đất nâu) → đất ẩm (cùng ô, `modulate` tối hơn). Các giai đoạn: hạt → cây non (Nature) → **SeedBig vẽ lên đất làm "củ nhú lên"** → thu thành icon. Plant 4 khung làm cây hoa. | V | **Không có sprite cây trồng nhiều giai đoạn trong pack Ninja**, đây là khoảng trống lớn nhất. Roguelike có vài mầm 4 màu nhưng lệch style. |
| Ruộng nâng / luống hoa | TilesetField (5 màu) | Dùng khối cỏ nâng làm luống, vườn hoa, sân dojo. Bản tuyết dùng mùa đông. | D | |
| **Gà, cho gà ăn, trứng** | Animal/Chicken ×4, Items/Food SeedLargeWhite, Nut, Creature/Bird.wav | Gà đi lang thang (2 khung, lật hướng). **SeedLargeWhite dùng làm trứng.** Thóc = Seed1. Emote tim khi được cho ăn. | D | Không có tổ gà (dùng rổ trong TilesetHouse hoặc rơm của TilesetElement). |
| Chăn nuôi khác | Cow (+Side), Pig ×3, Horse ×2, Donkey, Potion/MilkPot | Bò → MilkPot. Lợn đã có scene `pig.tscn`. Ngựa / lừa kéo xe (Vehicles). | D | Không có cừu, thỏ, hươu (GAME.md có nhắc). Hamster trắng có thể làm thỏ con. |
| **Hàng rào và sửa hàng rào** | TilesetHouse (rào gỗ / đá / sắt), Weapon/Hammer, Skill icon Repair, Particle/Wood | Rào hỏng = ô rào bỏ bớt một đoạn + Particle/Wood; sửa = vung Hammer overlay + Hit sound. | D | Không có sprite rào gãy. Dùng ô thiếu, hoặc tint tối. |
| Thú cưng | Dog ×6, Cat ×6, Hamster, Parrot, Animal Faceset | Đi theo người chơi (behavior_follow có sẵn), Faceset cho hội thoại. | D | |
| **Làng chính** | TilesetHouse, TilesetElement, TilesetFloor, TilesetNature, Animated Flag / Flower / Watermill / MillPropeller | Dựng tay theo mẫu Example 1/3 (đã xem): nhà mái cam, đường cát, rào cọc, cây. Cối xay gió (MillPropeller trên nhà gỗ lớn), cối xay nước bên sông. | D | |
| Nhà trong (nhà mình, quán, dojo) | Interior/* (tường hồng/cam), Element, tileset_bed | Nhà mình lúc đầu dột: dùng tường WallSimple + Particle/Rain trong nhà. Giường đổi màu là phần thưởng nâng cấp nhà. | D | |
| Dân làng (~15 NPC) | Character (Villager ×7, OldMan ×3, OldWoman, Woman, Child, Monk, Master, Noble, Princess, Inspector, Hunter, Eskimo...) + Faceset + Dialog | Lịch trình bằng behavior_follow_path. Mỗi NPC có faceset trong DialogBoxFaceset. Voice ×10 làm "giọng" bíp khi nói. | D | Chỉ có walk/attack/jump, không có ngồi/làm việc. Dùng emote thay. |
| **Quái hoá NPC hiền** | Monster: Kappa, Panda, Racoon, Mouse, Owl, Lantern, Spirit, Mushroom | Kappa bán cá ở hồ, Panda bán tre, Lantern làm đèn đường sống ban đêm, Spirit làm hồn tổ tiên dẫn đường. Không cần đánh. | D | |
| Nghề chợ / cửa hàng | TilesetHouse (quầy, rổ thức ăn), Items (Food, Resource, Treasure/Coin), Bonus/Coin sounds, Theme Wood | Quầy = Element + rổ hàng; UI mua bán dùng Theme Wood + Skill icon khung. | V | |
| Nấu ăn | tileset_camp (lửa trại + nồi), Items/Food 24 món, Skill icon Cook/Dish, Particle/Fire, Smoke | Công thức đơn giản: Fish + Onigiri → Sushi, Meat → Yakitori. Khói từ Smoke. | V | |
| Câu cá / bắt bướm | Weapon/Net, Animal/Fish ×3, Monster Butterfly ×2 (dùng làm sinh vật hiền), Vehicles FishNet/Full, Water Ripples, Ambient River | Butterfly / ButterflyBlue thả bay quanh hoa làm "côn trùng sưu tầm". Ripples báo chỗ có cá. Lưới đầy / rỗng hiện tiến độ. | V | Không có cần câu animation (chỉ có icon `Weapons/Fishing Rod`). Dùng Net. |
| Bến thuyền / ven biển | Vehicles Boat, Sail, Crane, TilesetWater ván bến, Ambient Wave | Thuyền là vật trang trí hoặc teleporter sang đảo. Cần cẩu ở bến. | D | |
| Đào mỏ | TilesetNature (quặng 4 màu, 3 trạng thái cạn), ReliefDetail (cửa mỏ, thang), Items Bar ×6 + Gem ×4, Particle/Rock, Pickaxe overlay | Mỗi lần đào chuyển sang trạng thái cạn hơn. Lò rèn: Anvil + Bar. | V | |
| Thu nhặt tài nguyên | Resource/Branch, Grass, Rock, Water, feather; destroyable (grass/pot/crate) | Cắt cỏ rơi Grass; đập bình rơi xu. Hệ destroyable đã có sẵn. | D | |
| **Khu rừng / đồng** | TilesetNature (cây xanh, thông), FloorDetail, Particle/Leaf, environment autumn | Mùa: đổi tile cây (xanh → thu vàng → tuyết → anh đào hồng) + gradient color_correction có sẵn. | V | Thay tile theo mùa cần script swap atlas. |
| **4 mùa và thời tiết** | Nature (4 biến thể cây), FloorB tuyết phủ, Particle Rain/Snow/Leaf/LeafPink, Fog, Raylight, Meteo icons, Ambient Rain/Storm/Wind | Xuân = LeafPink + cây anh đào; thu = Leaf + gradient autumn; đông = FloorB phủ + Snow. Icon Meteo hiện trên HUD. | V | |
| Ngày / đêm | color_correction gradient, Lantern (quái), đuốc TilesetDungeon, Meteo Sun/Moon, nhạc theo giờ | Ban đêm: gradient tối, Lantern / đuốc phát sáng (PointLight2D). | V | |
| Đầm lầy | TilesetWater (bờ tím), Water Ripples Purple, environment swamp có sẵn, Slime/Frog/Mollusc, nhạc Curse/Mystical | | D | |
| Sa mạc / ốc đảo (khu xa) | TilesetDesert, Floor cát, Relief cát, QuickSand (bẫy), Snake / Reptile / Lizard, Lion / Hyena | Cát lún 8 khung = vùng làm chậm. | V | |
| Núi tuyết | Relief tuyết, Floor tuyết, FloorB, igloo (House), Eskimo / NinjaEskimo, nước băng | Làng eskimo nhỏ = NPC phụ. | V | |
| Làng bỏ hoang / phế tích | TilesetVillageAbandoned, tượng rêu (House), nhạc Lost Village / Ruins | Khu nhiệm vụ "dọn rêu sửa làng" (chill). | D | |
| **Hầm ngục đầu tiên** | Interior tường nâu / xanh + sàn sỏi tối, TilesetDungeon (đuốc, gai, công tắc, khối đẩy, quả cầu, tháp pháo), TilesetHole, TilesetLogic (chìa/khoá màu), Push/Climb của NinjaGreen | Puzzle Zelda: khối đẩy lên công tắc, chìa màu mở khoá màu, hố + Roll. Tháp pháo mắt bắn EnergyBall. | V | Không có cửa dungeon riêng; dùng vòm đá House hoặc song sắt Dungeon. |
| Quái thường | 66 Monster | Palette swap sẵn (Slime ×4, Snake ×4) = cấp độ; tint thêm = biến thể "đã luyện" (GAME.md). Mỗi khu một họ. | D | Chỉ có walk 4 hướng; tấn công = lao tới + FX. |
| Boss | 20 Boss (10 cặp màu) | Bản thứ hai dùng làm "boss đã luyện" / hard mode. Rồng ráp đốt như rắn (khó). | V–K | Một số boss chỉ có Idle + Hit (DemonCyclop, GiantFlam, GiantSpirit). |
| Đánh nhau / chiêu | FX Slash / Attack / Elemental / Magic / Projectile, Items/Projectile, Scroll ×7, Spell icons, Whoosh / Hit / Magic sounds | Mỗi Scroll mở một nguyên tố (Fire / Ice / Plant / Rock / Thunder) khớp FX Elemental cùng tên. | V | |
| Phép tiện ích (chill) | FX Plant (mọc cây), Water / WaterPillar (tưới diện rộng), Magic/Boost, Spark | Kỹ năng ninja dùng vào việc đồng áng: "thuỷ độn" tưới 3×3 ô, "mộc độn" thúc cây lớn. | V | |
| HUD | Receptacle (tim, bình cầu), Skill icon, Meteo, Theme Wood, Font | Thể lực = bình cầu xanh lá; đồng hồ / mùa = icon Meteo. Player UI đã có tim. | D | Không có khung đồng hồ; ghép từ Sphere. |
| Hội thoại | Dialog (DialogBoxFaceset, ChoiceBox, Yes/No), Faceset (mọi actor), Emote, Voice | Emote trên đầu NPC khi có việc (!) hoặc đang ngủ (zzz). | D | |
| Túi đồ / chế tạo | Theme Wood (inventory_cell, tab), Items 115 icon, Skill icon Job | | V | |
| Gợi ý nút / tutorial | Ui/Input (Keyboard, Gamepad, Mouse), Items/Action Interact & Hit | Hiện nút E trên vật tương tác. | D | |
| Thư / nhiệm vụ | Items/Other Letter ×3, Stamp, Scroll, ScrollEmpty | Hộp thư trước nhà; nhiệm vụ nhẹ của hàng xóm. | D | |
| Kho báu / bí mật | Treasure (rương nhỏ/lớn, chìa vàng/bạc, cúp), Jingles Secret ×4 | | D | |
| Đồ trang trí đặt được | Element, House (tượng, băng rôn), Flag 8 màu, tileset_bed, camp | Cắm cờ màu đánh dấu ruộng; băng rôn huy hiệu cho dojo. | V | |
| Mini-game | Items/Object Dice ×4, Hourglass, PanFlute, Book; Kenney TinyTown bia tập bắn | Xúc xắc ở quán; PanFlute + Skill Sing = thổi sáo dụ thú. | V | |
| Dây chuyền / máy (vui) | Conveyor Belt, Pipes, Watermill | Máy xay bột chạy bằng cối nước; ống dẫn nước ra ruộng (tưới tự động cuối game). | K | |
| **Nhạc theo khu và giờ** | 41 Musics | Làng ngày: Village / Calm Village / Peaceful; chiều: Chill / Good Time; đêm: Quiet / Dream; rừng: Clearing / Road; biển: Aquatic / Boat; hang: The Cave; hầm: Dungeon / Crypt; boss: Fight / Tension; phế tích: Lost Village / Ruins. `music.gd` có crossfade sẵn. | D | Chưa nghe thử, tông suy từ tên. |
| Âm môi trường | Ambient 9, Creature 5, Kenney footstep ×10, cửa, cót két | Bước chân theo nền (Kenney); cửa nhà (doorOpen / doorClose). | D | Không có tiếng gà / bò / lợn. Bird + pitch shift tạm. |
| Jingle sự kiện | Ninja Jingles 15, Kenney jingles 85 | Thu hoạch = Pizzicato; ngủ sang ngày mới = Steel; thất bại nhẹ = Sax. | D | |

---

## 3. Phủ asset

| Nhóm asset | Dùng ở |
|---|---|
| NinjaGreen + Weapon overlay | người chơi, công cụ |
| Character (95) | NPC làng, khách, đối thủ dojo, kẻ cướp (Ninja màu tối), quái người (Skeleton, Demon) |
| Monster (66) | quái theo khu; nhóm hiền làm NPC / sinh vật (Butterfly, Kappa, Panda, Lantern, Spirit) |
| Boss (20) | boss hầm ngục + bản "đã luyện" |
| Animal (27) | nông trại, thú cưng, thú hoang theo khu |
| Facesets (~200) | hội thoại, bestiary, sổ thú cưng |
| TilesetFloor / FloorB / FloorDetail / Field | nền mọi khu, ruộng, mùa đông |
| Water / Relief / ReliefDetail / Hole | sông, hồ, đầm, vách, mỏ, hố hầm ngục |
| Nature | cây theo mùa, mỏ quặng, hoa, tre |
| House / Element / bed / camp | làng, nội thất, trại |
| VillageAbandoned / Desert | phế tích, sa mạc |
| Interior* | nhà trong + hầm ngục (màu nâu / xanh) |
| Dungeon / Logic | puzzle hầm ngục, chìa-khoá màu |
| Towers | trạm canh ở biên làng / tháp phòng thủ đột kích (GAME.md §6) |
| Pipes / Conveyor | tưới tự động, máy xay (cuối game) |
| Animated (Flag, Flower, Plant, Mill, Watermill, Waterfall, Ripples, QuickSand) | trang trí sống, cát lún |
| Vehicles | bến cá |
| FX (mọi nhóm) | chiến đấu, phép tiện ích, thời tiết, vỡ đồ |
| Items (mọi nhóm) | túi đồ, chế tạo, nấu ăn, chợ, nhiệm vụ |
| Ui (mọi nhóm) | HUD, menu, hội thoại, gợi ý nút |
| Audio ninja (Music / Jingle / Sounds) | toàn bộ âm thanh |
| Kenney audio_rpg | bước chân, cửa, sách, túi, xu |
| Kenney audio_music | jingle sự kiện nhỏ |
| Kenney particles | texture mềm cho GPUParticles2D / Light2D (ánh sáng đuốc, đom đóm, phép), tint theo màu Palette |
| Kenney uiborders | khung trắng tint màu cho thông báo / tooltip, nếu Theme Wood thiếu kiểu |
| Kenney icons | menu hệ thống (lưu, cài đặt, âm lượng, gamepad); không đặt vào thế giới game |
| `game/`, `GodotProject.zip` | nền code / tham khảo pattern |

### Chưa gán được, hoặc nên bỏ

| Asset | Đề xuất / lý do |
|---|---|
| Kenney **roguelike** (tile) | **Bỏ cho thế giới**: lệch style rõ (đã xem). Ngoại lệ có thể: vài **mầm cây 4 màu** làm giai đoạn cây trồng, nhưng phải vẽ lại / chỉnh palette; dùng tạm làm placeholder. |
| Kenney **caves** | Bỏ cho thế giới. Chỉ tham khảo layout ray mỏ. |
| Kenney **tinydungeon / tinytown** | Bỏ cho thế giới (style khác). Tinytown có **tổ ong, xe cút kít, bia tập bắn**, Ninja không có, nên chỉ làm placeholder rồi vẽ lại. |
| Kenney **uirpg** | Bỏ: style UI nhẵn, đã bị Theme Wood thay thế. |
| Ui/Theme/Wip (12 theme) | Giữ 1-2 (ThemeMap cho bản đồ, ThemeBamboo cho menu dojo); còn lại không cần. |
| Character robot ×3, Camouflage, GoldStatue, Statue | Robot lệch tông "làng quê". Dùng làm golem trong phế tích. Statue / GoldStatue làm **tượng sống** trong đền (đứng yên rồi động đậy). |
| Monster Grey Trex / TRex, Dragon (monster) | Khu xa hoặc hầm ngục khó. Không vào bản đầu. |
| Boss Squid, Tengu, Dragon | Ngoài bản đầu (cần biển / đền / bộ phận rời). |
| TilesetTowers | Chưa rõ ngữ nghĩa animation. Giữ cho hệ đột kích về sau. |
| Conveyor Belt | Khó khớp tông chill. Chỉ dùng nếu làm máy xay. |
| Ui/Input Gamepad bản nhấn | Chỉ khi hỗ trợ gamepad. |
| `assets/audio/` (11 file) | **Trùng lặp**: chỉ là bản sao đổi tên của ninja. Có thể xoá khi dọn (không xoá trong lần này). |
| 2 zip Godot + zip Asset Pack | Trùng lặp với thư mục đã giải nén. GodotProject (Godot 3) chỉ để tham khảo. |
| Example 1-4.gif, preview, MusicCover | Tham khảo style / bìa trang itch; không vào game. |

---

## 4. Ưu tiên cho bản chơi đầu (một làng, vài khu, một hầm ngục)

1. **Người chơi = NinjaGreen** (ô 32 px, nhưng thân nhân vật vẫn cỡ ~16 px như NPC; phần dư là chỗ cho lăn / chém) + overlay Hammer / Axe / Pickaxe / Net + tư thế Item cho cuốc / tưới. Hiện `game/` dùng NinjaBlue (sheet 64×112, ít động tác). Muốn ninja màu khác thì palette swap NinjaGreen.
2. **Nông trại nhỏ trước nhà**: ruộng (Floor đất nâu), hạt → mầm (Nature) → củ (SeedBig), tưới + Particle Water, gà ×4 màu + trứng (SeedLargeWhite), bò + MilkPot, hàng rào sửa bằng Hammer.
3. **Làng Thung Lũng Sương** dựng tay: nhà House, chợ, dojo (biển DOJO + torii), cối xay nước, 8–10 NPC với faceset + hội thoại + emote; quái hiền làm thương nhân (Kappa, Panda).
4. **Ngày / đêm + mùa nhẹ**: gradient color_correction, đổi tile cây theo mùa, Particle Rain / Snow / Leaf, Meteo icon trên HUD, nhạc theo giờ (Village → Chill → Quiet).
5. **Ba khu ngoài**: rừng (Clearing, quái slime / nấm, hái nhặt), bờ sông / hồ (câu cá bằng Net, bến thuyền, Kappa), phế tích rêu (VillageAbandoned, nhiệm vụ dọn dẹp).
6. **Một hầm ngục** (tường nâu / xanh, sỏi tối): khối đẩy + công tắc, chìa / khoá màu (Logic), gai, hố, tháp pháo mắt, quái Skull / Bat / Spider, boss **GiantSlime** (có Jump) hoặc **GiantRacoon**; bản màu 2 làm hard mode.
7. **HUD + hội thoại + túi đồ** từ Theme Wood, Receptacle, Dialog, Skill icon Job&Action.
8. **Âm thanh**: bộ SFX ninja + bước chân / cửa Kenney + jingle thu hoạch.

### Khoảng trống asset lớn nhất

| Khoảng trống | Mức độ | Cách lấp |
|---|---|---|
| **Cây trồng nhiều giai đoạn** (gieo → lớn → chín, theo loại) | Cao, trái tim của game | Ghép: mầm Nature + SeedBig + tint. Hoặc tự vẽ 3–4 khung / loại theo Palette.png. Roguelike làm placeholder. |
| Động tác nông trại (cuốc, tưới, cho ăn) | Trung bình | Tư thế Item / Pickup + icon công cụ + particle. Vẽ thêm overlay Hoe / WateringCan theo khung Weapon. |
| Đất ẩm / đất đã cuốc riêng | Thấp | Tint ô đất nâu. |
| Thú: cừu, thỏ, hươu; tổ gà, chuồng | Trung bình | Bỏ khỏi thiết kế bản đầu, hoặc vẽ thêm. Chuồng = rào + mái House. |
| NPC chỉ có walk / attack | Thấp | Emote + đứng yên theo lịch. |
| Âm thanh động vật trang trại | Thấp | Bird / Dog + pitch, hoặc nguồn CC0 khác. |
| Rào hỏng, nhà dột (trạng thái hư) | Thấp | Ô thiếu + tint + particle. |
| Chỉ một nhân vật có bộ động tác đầy đủ (bơi, leo, đẩy, lăn) | Trung bình | Người chơi luôn là NinjaGreen (+ palette swap). NPC không cần các động tác này. |
