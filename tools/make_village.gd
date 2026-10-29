# Dựng content/map/village.tscn từ bố cục viết tay bên dưới (không sinh ngẫu nhiên).
# Chạy: godot --headless --path game --script ../tools/make_village.gd
# Cần tileset_village.tres (tools/build_village_tileset.gd) và hàm trợ giúp ở system/mapgen/map_kit.gd.
#
# Làng 3x2 màn hình camera: tx -29..30, ty -5..16 = x [-456, 504], y [-72, 280] (test_village kiểm tra khít lưới camera).
extends SceneTree

const Kit = preload("res://system/mapgen/map_kit.gd")

const X0 = -29
const X1 = 30
const Y0 = -5
const Y1 = 16

# Công trình trong TilesetHouse: [cột, hàng, rộng, cao, cột cửa (tương đối)]
const BUILDINGS = {
	"orange": [0, 0, 4, 3, 1],
	"dojo": [4, 0, 4, 3, 1],
	"orange2": [8, 0, 4, 3, 1],
	"red": [12, 0, 4, 3, 1],
	"shop": [16, 0, 3, 3, 1],
	"inn": [19, 0, 4, 3, 2],
	"clay": [23, 0, 3, 3, 1],
	"cabin": [26, 0, 3, 3, 1],
}

# Các nhà: [loại, ô x trái, ô y trên, chữ ở biển cửa]. Hàng trên (cửa ra đường chính ty=1..2), hàng dưới (cửa ra đường phía nam ty=12..13).
# Đường nằm giữa một màn hình camera (không đặt đường lên ranh giới màn hình ty 5/6, vì camera sẽ giật khi đi dọc đường).
# "portal:<khu>" thay biển bằng cổng vào khu nội thất.
const HOUSES = [
	["orange", -27, -3, "Nhà bà Lan.\nCửa khoá, chắc bà đi chợ rồi."],
	["shop", -20, -2, "Cửa hàng tạp hoá.\nĐang nghỉ bán."],
	["inn", -13, -3, "Quán trọ Sương Mai.\nHôm nay hết phòng."],
	["dojo", -6, -3, "portal:dojo"],
	["orange2", 2, -2, "Nhà ông Tám.\nCó tiếng ngáy bên trong."],
	["clay", 10, -3, "Lò gốm.\nBếp còn ấm nhưng không thấy ai."],
	["cabin", 17, -2, "Nhà gỗ của người săn thú.\nTreo một tấm da hươu trước cửa."],
	["red", 24, -4, "Nhà trưởng làng.\nCửa đóng, treo biển 'đi vắng'."],
	["red", -25, 8, "Kho thóc của làng.\nKhoá bằng ba lớp xích."],
	["orange2", -17, 8, "Nhà chú Bảy.\nSân phơi đầy cá khô."],
	["clay", 9, 8, "Lò rèn nhỏ.\nBễ lò đã nguội từ lâu."],
	["cabin", 18, 8, "Nhà gỗ bỏ trống.\nMạng nhện giăng đầy cửa sổ."],
]

# Cây 2x2 trong TilesetNature: [cột, hàng]
const TREES = [[0, 0], [2, 0], [14, 0], [16, 0]]

var kit: Kit


func _initialize() -> void:
	kit = Kit.new("village")
	kit.paint_ground(X0, Y0, X1, Y1, dirt_cells())
	build_houses()
	build_gates()
	plant_trees()
	scatter_props()
	people()
	kit.borders(X0, Y0, X1, Y1)
	kit.add_env(Vector2(24, 104), Vector2(960, 352))
	kit.add_spawn("start", kit.center(-1, 8))
	kit.save("res://content/map/village.tscn")
	quit()


func door_tile(h: Array) -> Vector2i:
	var b: Array = BUILDINGS[h[0]]
	return Vector2i(h[1] + b[4], h[2] + b[3] - 1)


# ---------- nền ----------

func dirt_cells() -> Array[Vector2i]:
	var dirt: Array[Vector2i] = []
	# Hai đường ngang: chính (ty 1..2) và phía nam (ty 12..13).
	for x in range(X0 + 1, X1):
		for y in [1, 2, 12, 13]:
			dirt.append(Vector2i(x, y))
	# Lối nhỏ từ cửa mỗi nhà xuống đường gần nhất (2 ô rộng).
	for h in HOUSES:
		var d := door_tile(h)
		var y_end := 0 if d.y < 5 else 11
		for y in range(d.y + 1, y_end + 1):
			dirt.append(Vector2i(d.x, y))
			dirt.append(Vector2i(d.x + 1, y))
	# Lối từ đường chính xuống quảng trường, và quảng trường giữa làng nối hai đường.
	for y in range(3, 7):
		dirt.append(Vector2i(-1, y))
		dirt.append(Vector2i(0, y))
	for x in range(-6, 7):
		for y in range(7, 12):
			dirt.append(Vector2i(x, y))
	return dirt


# ---------- công trình ----------

func build_houses() -> void:
	for h in HOUSES:
		var b: Array = BUILDINGS[h[0]]
		var tx: int = h[1]
		var ty: int = h[2]
		kit.stamp(Kit.WALL, Kit.HOUSE, b[0], b[1], b[2], b[3], tx, ty)
		kit.collide_block(tx, ty, b[2], b[3], b[4])
		var d := door_tile(h)
		if String(h[3]).begins_with("portal:"):
			var target := String(h[3]).substr(7)
			kit.add_portal(target, "from_village", Rect2(kit.corner(d.x, d.y), Vector2(16, 16)))
			kit.add_spawn("from_" + target, kit.center(d.x, d.y + 1) + Vector2(0, 8))
		else:
			kit.add_sign("Door_%s_%d" % [h[0], tx], kit.center(d.x, d.y), h[3], false, "Xem")
		# Biển DOJO treo trên cửa đạo quán (lớp phủ lên hàng mái dưới).
		if h[0] == "dojo":
			kit.stamp(Kit.WALL2, Kit.HOUSE, 4, 4, 2, 1, tx + 1, ty + 1)
		# Hai bụi hai bên lối vào.
		kit.tilemap.set_cell(Kit.WALL2, Vector2i(d.x - 1, d.y + 1), Kit.NATURE, Vector2i((d.x * 3) % 10, 10))
		kit.tilemap.set_cell(Kit.WALL2, Vector2i(d.x + 2, d.y + 1), Kit.NATURE, Vector2i((d.x * 7) % 10, 10))


func build_gates() -> void:
	# Cổng torii cuối đường chính, mở ra Rừng Sương. Hai cột chặn, giữa là cổng.
	kit.stamp(Kit.WALL, Kit.HOUSE, 0, 5, 3, 2, 28, 1)
	kit.collide_rect(Rect2(kit.corner(28, 2) + Vector2(11, 0), Vector2(6, 16)))
	kit.collide_rect(Rect2(kit.corner(28, 2) + Vector2(32, 0), Vector2(6, 16)))
	kit.add_portal("forest", "from_village", Rect2(kit.corner(29, 1), Vector2(16, 32)))
	kit.add_spawn("from_forest", kit.center(26, 1) + Vector2(0, 8), Vector2.LEFT)
	kit.add_sign("Sign_east", kit.center(27, 0), "Đường ra Rừng Sương.\nTrời tối thì đừng đi một mình.")


# ---------- cây ----------

func plant_trees() -> void:
	var n := 0
	for x in range(X0, X1, 2):
		place_tree(x, Y0, n)
		place_tree(x, Y1 - 1, n + 1)
		n += 2
	for y in range(Y0 + 2, Y1 - 1, 2):
		# Chừa lối hai đường ngang (ty 0..3 và 11..14).
		if (y >= 0 and y <= 3) or (y >= 11 and y <= 14):
			continue
		place_tree(X0, y, n)
		place_tree(X1 - 1, y, n + 1)
		n += 2
	# Vài cây lẻ giữa các nhà ở phía nam, và gốc cây ở lối vào quảng trường.
	for p in [[-12, 8], [-10, 9], [6, 8], [14, 9], [23, 9], [-29, 9], [26, 9]]:
		place_tree(p[0], p[1], n)
		n += 1
	kit.stamp(Kit.WALL, Kit.NATURE, 0, 8, 2, 2, -10, 4)


func place_tree(tx: int, ty: int, variant: int) -> void:
	var t: Array = TREES[(variant * 5 + tx) % TREES.size()]
	kit.stamp(Kit.WALL, Kit.NATURE, t[0], t[1], 2, 2, tx, ty)
	kit.collide_base(tx, ty, 2, 2)


# ---------- vật dụng ----------

func scatter_props() -> void:
	kit.add_sign("Sign_notice", kit.center(2, 7), "LÀNG THUNG LŨNG\nĐạo quán ở phía bắc, đường ra Rừng Sương ở phía đông.\nBảng này do trưởng làng dựng.")
	kit.add_chest("Chest_plaza", kit.center(5, 11), &"coin", 20)
	var crates := [Vector2i(-5, 10), Vector2i(-4, 10), Vector2i(-5, 11)]
	for i in crates.size():
		kit.add_scene(load("res://content/destroyable/crate.tscn"), "Crate%d" % (i + 1), kit.center(crates[i].x, crates[i].y))
	var pots := [Vector2i(-24, 0), Vector2i(1, 3), Vector2i(21, 0), Vector2i(-21, 10), Vector2i(12, 10)]
	for i in pots.size():
		kit.add_scene(load("res://content/destroyable/pot.tscn"), "Pot%d" % (i + 1), kit.center(pots[i].x, pots[i].y))


func people() -> void:
	kit.add_npc("Villager1", "villager", kit.center(3, 9), "Dân làng", "Nghe nói trưởng làng sắp mở hội mùa thu.\nChỉ mong năm nay có nhiều bánh.", 40.0)
	kit.add_npc("OldWoman", "oldwoman", kit.center(-22, 2), "Bà Lan", "Cháu có thấy con mèo của bà không?\nNó hay trốn trong rừng phía đông.", 12.0)
	kit.add_npc("Child", "child", kit.center(-1, 4), "Bé Na", "Đố chú bắt được cháu đấy!", 56.0)
	kit.add_npc("Monk", "monk", kit.center(-3, 1), "Tiểu sư", "Đạo quán ngay đây thôi. Sư phụ đang ở trong, cứ vào đi.", 14.0)
	kit.add_npc("Hunter", "hunter", kit.center(19, 1), "Thợ săn", "Phía đông có hang mỏ cũ.\nTôi từng thấy ánh sáng lạ chiếu ra từ đó.", 14.0)
	kit.add_npc("Noble", "noble", kit.center(26, 0), "Phú ông", "Làng này yên bình quá, yên bình đến mức hơi nhàm.", 10.0)
	kit.add_npc("Woman", "woman", kit.center(-10, 12), "Chị bán rau", "Hôm nay không họp chợ nên tôi chẳng bán được gì.", 30.0)
	kit.add_npc("OldMan", "oldman", kit.center(14, 12), "Ông lão", "Hồi trẻ ta cũng từng đi qua cổng torii kia.\nBây giờ chỉ thích ngồi ngắm người qua lại.", 20.0)
