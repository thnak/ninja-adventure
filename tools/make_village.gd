# Dựng content/map/village.tscn từ bố cục viết tay bên dưới (cây rải bằng hàm băm cố định, không ngẫu nhiên).
# Chạy: godot --headless --path game --script ../tools/make_village.gd
# Cần tileset_village.tres (tools/build_village_tileset.gd) và hàm trợ giúp ở system/mapgen/map_kit.gd.
# Bố cục và lý do: docs/design/layout-proposals.md.
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

# Các nhà: [loại, ô x trái, ô y trên, chữ ở biển cửa]. Nhà bà Lan và đạo quán giữ nguyên chỗ (test đo vị trí).
# Không đặt lối cửa lên ranh giới màn hình (tx -10/-9, 10/11, 30/31; ty 5/6): camera sẽ giật khi đi dọc lối.
# "portal:<khu>" thay biển bằng cổng vào khu nội thất.
const HOUSES = [
	["orange", -27, -3, "Nhà bà Lan.\nCửa khoá, chắc bà đi chợ rồi."],
	["shop", -21, -2, "Cửa hàng tạp hoá.\nĐang nghỉ bán."],
	["inn", -14, -3, "Quán trọ Sương Mai.\nHôm nay hết phòng."],
	["dojo", -6, -3, "portal:dojo"],
	["orange2", 1, -2, "Nhà ông Tám.\nCó tiếng ngáy bên trong."],
	["clay", 13, -3, "Lò gốm.\nBếp còn ấm nhưng không thấy ai."],
	["cabin", 18, -2, "Nhà gỗ của người săn thú.\nTreo một tấm da hươu trước cửa."],
	["red", 23, -4, "Nhà trưởng làng.\nCửa đóng, treo biển 'đi vắng'."],
	["red", -26, 8, "Kho thóc của làng.\nKhoá bằng ba lớp xích."],
	["orange2", -18, 9, "Nhà chú Bảy.\nSân phơi đầy cá khô."],
	["clay", 12, 8, "Lò rèn nhỏ.\nBễ lò đã nguội từ lâu."],
	["cabin", 20, 9, "Nhà gỗ bỏ trống.\nMạng nhện giăng đầy cửa sổ."],
]

# Đường (góc trên trái của cọ 2x2): chính uốn giữa ty 1..4, lối xuống quảng trường, hai đường nam.
const MAIN = [[-26, 1], [-25, 1], [-22, 2], [-18, 3], [-14, 2], [-10, 1], [-6, 1], [-2, 2], [2, 3], [6, 3],
	[10, 2], [14, 1], [18, 2], [22, 3], [25, 2], [28, 1]]
const LINK = [[-1, 3], [-1, 5], [0, 6], [0, 7]]
const SOUTH_W = [[-27, 13], [-24, 12], [-20, 13], [-15, 13], [-11, 12], [-7, 10]]
const SOUTH_E = [[6, 10], [9, 12], [13, 12], [17, 13], [21, 13], [25, 12], [26, 10], [26, 7], [25, 4], [24, 3]]

# Điểm nhấn đặt tay: [khoá, tx, ty]
const LANDMARKS = [["big_pink", -9, 3], ["big_pink", 8, 5], ["big_green", -22, 9], ["big_green", 21, 5], ["big_green", -12, 8]]

# Vườn cây giữa hai đường: [rect, seed]
const GARDENS = [[Rect2i(-27, 3, 16, 5), 31], [Rect2i(-11, 3, 8, 4), 32], [Rect2i(7, 4, 16, 4), 33], [Rect2i(14, 6, 6, 3), 34], [Rect2i(-27, 14, 58, 1), 35]]

const SMALL = ["t_green", "t_pine", "t_green2", "t_gold", "t_pink"]
const BIG = ["c_green", "c_green2"]

var kit: Kit
var area := {}
var dirt := {}
var solid_extra := {}   # vật đặt tay chặn đường đi (rào, bó rơm...)


func _initialize() -> void:
	kit = Kit.new("village")
	area = Kit.rect_cells(Rect2i(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1))
	build_ground()
	build_houses()
	build_gates()
	fences_and_props()
	plant_trees()
	scatter_props()
	people()
	check_reachable()
	kit.edge_decor(dirt, 21, 0.16)
	print("trampleable grass: ", kit.trample_grass(area, 31, 0.5))
	kit.borders(X0, Y0, X1, Y1)
	kit.add_env(Vector2(24, 104), Vector2(960, 352), load("res://content/environment/leafy/environment.tres"))
	kit.add_spawn("start", kit.center(-1, 8))
	kit.save("res://content/map/village.tscn")
	quit()


func door_tile(h: Array) -> Vector2i:
	var b: Array = BUILDINGS[h[0]]
	return Vector2i(h[1] + b[4], h[2] + b[3] - 1)


# ---------- nền ----------

func build_ground() -> void:
	var d := Kit.path_cells(MAIN)
	for pts in [LINK, SOUTH_W, SOUTH_E]:
		d = Kit.union(d, Kit.path_cells(pts))
	d = Kit.union(d, Kit.ellipse_cells(Vector2(0.5, 9.5), Vector2(7.5, 2.6)))
	# Làm mượt TRƯỚC khi nối lối cửa (lối 2x1 sẽ bị coi là mỏm), sau đó chỉ lấp lỗ.
	d = Kit.smooth_dirt(d)
	# Lối nhỏ từ cửa mỗi nhà (2 ô rộng) cho tới khi gặp đất.
	for h in HOUSES:
		var door := door_tile(h)
		var step := 1
		var y := door.y + 1
		var cells := []
		var found := false
		while y <= Y1 - 2:
			if d.has(Vector2i(door.x, y)) and d.has(Vector2i(door.x + 1, y)):
				found = true
				break
			cells.append(Vector2i(door.x, y))
			cells.append(Vector2i(door.x + 1, y))
			y += step
		if found:
			for c in cells:
				d[c] = true
		else:
			printerr("lối cửa ", h[0], " không tới được đường")
	d = Kit.smooth_dirt(d, 2, true)
	# Chừa ô ngoài cùng cho vành cây.
	for c in d.keys():
		if c.x <= X0 or c.x >= X1 or c.y < Y0 or c.y > Y1 - 2:
			d.erase(c)
	dirt = d
	var typed: Array[Vector2i] = []
	for c in dirt:
		typed.append(c)
	kit.paint_ground(X0, Y0, X1, Y1, typed)


# ---------- công trình ----------

func build_houses() -> void:
	for h in HOUSES:
		var b: Array = BUILDINGS[h[0]]
		var tx: int = h[1]
		var ty: int = h[2]
		kit.stamp(Kit.WALL, Kit.HOUSE, b[0], b[1], b[2], b[3], tx, ty)
		kit.collide_block(tx, ty, b[2], b[3], b[4])
		kit.reserve(tx, ty, b[2], b[3])
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
		# Hai bụi hai bên lối vào (bỏ nếu rơi vào đường).
		for pair in [[d.x - 1, 3], [d.x + 2, 7]]:
			var c := Vector2i(pair[0], d.y + 1)
			if !dirt.has(c):
				kit.tilemap.set_cell(Kit.WALL2, c, Kit.NATURE, Vector2i((d.x * pair[1]) % 10, 10))
				kit.occupied[c] = true


func build_gates() -> void:
	# Cổng torii cuối đường chính, mở ra Rừng Sương. Hai cột chặn, giữa là cổng.
	kit.stamp(Kit.WALL, Kit.HOUSE, 0, 5, 3, 2, 28, 1)
	kit.reserve(28, 1, 3, 2)
	kit.collide_rect(Rect2(kit.corner(28, 2) + Vector2(11, 0), Vector2(6, 16)))
	kit.collide_rect(Rect2(kit.corner(28, 2) + Vector2(32, 0), Vector2(6, 16)))
	kit.add_portal("forest", "from_village", Rect2(kit.corner(29, 1), Vector2(16, 32)))
	kit.add_spawn("from_forest", kit.center(26, 1) + Vector2(0, 8), Vector2.LEFT)
	kit.add_sign("Sign_east", kit.center(27, 0), "Đường ra Rừng Sương.\nTrời tối thì đừng đi một mình.")


# ---------- rào, bó rơm ----------

func fences_and_props() -> void:
	# Rào sân kho thóc và sau nhà chú Bảy: đầu rào (10,7), thân (11,7) lặp.
	for f in [[-27, 7, 6], [-19, 8, 6]]:
		kit.stamp(Kit.WALL, Kit.HOUSE, 10, 7, 1, 1, f[0], f[1])
		for i in range(1, f[2]):
			kit.stamp(Kit.WALL, Kit.HOUSE, 11, 7, 1, 1, f[0] + i, f[1])
		kit.collide_rect(Rect2(kit.corner(f[0], f[1]) + Vector2(0, 8), Vector2(f[2] * 16, 6)))
		for i in f[2]:
			var c := Vector2i(f[0] + i, f[1])
			kit.occupied[c] = true
			solid_extra[c] = true
	# Bó rơm cạnh kho thóc.
	kit.stamp(Kit.WALL, Kit.ELEMENT, 12, 4, 1, 2, -21, 9)
	kit.collide_rect(Rect2(kit.corner(-21, 10) + Vector2(1, 4), Vector2(14, 10)))
	kit.reserve(-21, 9, 1, 2)
	# Hai thùng đẩy được ở rìa đông quảng trường.
	kit.add_push_crate("PushCrate1", 7, 10)
	kit.add_push_crate("PushCrate2", 8, 11)


# ---------- cây ----------

## Ô quanh vật/nhà/cửa/ô thử nghiệm mà cây không được chiếm.
func avoid_cells() -> Dictionary:
	var avoid := {}
	for h in HOUSES:
		var b: Array = BUILDINGS[h[0]]
		# Chừa 1 ô hai bên và phía trước nhà; phía sau nhà cây được mọc sát.
		avoid = Kit.union(avoid, Kit.rect_cells(Rect2i(h[1], h[2], b[2], b[3])))
		var d := door_tile(h)
		avoid = Kit.union(avoid, Kit.rect_cells(Rect2i(d.x - 1, d.y + 1, 4, 3)))
	for r in [Rect2i(26, 0, 5, 4), Rect2i(-27, -1, 4, 2), Rect2i(-1, 12, 3, 3)]:
		avoid = Kit.union(avoid, Kit.rect_cells(r))
	for p in poi_points():
		avoid = Kit.union(avoid, {p: true})
	return avoid


func poi_points() -> Array[Vector2i]:
	var pts: Array[Vector2i] = [Vector2i(-1, 8), Vector2i(2, 7), Vector2i(5, 11), Vector2i(27, 0), Vector2i(26, 1)]
	for c in [Vector2i(-5, 10), Vector2i(-4, 10), Vector2i(-5, 11), Vector2i(-24, 0), Vector2i(1, 3), Vector2i(22, 0), Vector2i(-21, 8), Vector2i(15, 10)]:
		pts.append(c)
	for c in [Vector2i(3, 9), Vector2i(-22, 2), Vector2i(-1, 4), Vector2i(-3, 1), Vector2i(19, 1), Vector2i(26, 0), Vector2i(-10, 12), Vector2i(16, 11)]:
		pts.append(c)
	return pts


func zone(rect: Rect2i, avoid: Dictionary) -> Dictionary:
	var out := {}
	for c in Kit.rect_cells(rect):
		if area.has(c) and !dirt.has(c) and !avoid.has(c):
			out[c] = true
	return out


func plant_trees() -> void:
	var avoid := avoid_cells()
	# Điểm nhấn.
	for l in LANDMARKS:
		var t: Array = Kit.TREES[l[0]]
		var fit := true
		for j in t[4]:
			for i in t[3]:
				var c := Vector2i(l[1] + i, l[2] + j)
				if dirt.has(c) or avoid.has(c) or kit.occupied.has(c):
					fit = false
		if fit:
			kit.plant(l[0], l[1], l[2], true)
		else:
			print("bỏ điểm nhấn ", l)
	# Vành cây: bắc, nam, tây, đông, hai lớp bên ở giữa; va chạm gộp bằng mặt nạ.
	var before := kit.occupied.duplicate()
	var rim := {}
	for r in [Rect2i(X0, Y0, X1 - X0 + 1, 2), Rect2i(X0, Y1 - 1, X1 - X0 + 1, 2), Rect2i(X0, Y0 + 2, 2, Y1 - Y0 - 3), Rect2i(X1 - 1, Y0 + 2, 2, Y1 - Y0 - 3),
			Rect2i(-27, -3, 2, 17), Rect2i(27, 3, 2, 12)]:
		rim = Kit.union(rim, zone(r, avoid))
	kit.scatter(rim, Vector2i(X0, Y0), 3, SMALL, BIG, 0.12)
	var mask := {}
	for c in kit.occupied:
		if !before.has(c):
			mask[c] = true
	print("rim collision rects: ", kit.collide_mask(mask))
	# Vườn cây giữa làng: cây thưa, va chạm từng gốc.
	for g in GARDENS:
		kit.scatter(zone(g[0], avoid), Vector2i(X0, Y0), g[1], SMALL, BIG, 0.14, 0.85, true)
	# Phần còn lại của làng: rải thưa thành chòm quanh nhà.
	kit.scatter(zone(Rect2i(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1), avoid), Vector2i(X0, Y0), 41, SMALL, BIG, 0.2, 1.0, true)
	# Thêm các lượt lệch lưới để lấp khe còn sót: rừng rậm, nền chỉ còn lộ ở đường và sân nhà.
	for off in [Vector2i(1, 1), Vector2i(1, 0), Vector2i(0, 1)]:
		kit.scatter(zone(Rect2i(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1), avoid), Vector2i(X0, Y0) + off, 43 + off.x * 2 + off.y, SMALL, BIG, 0.15, 1.0, true)
	# Bụi, gốc, hoa phủ ô cỏ còn trống (một nửa số ô) cho nền bớt trơ.
	var ground := zone(Rect2i(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1), {})
	var sparse := {}
	for c in ground:
		if Kit.hash01(c.x, c.y, 77) < 0.45 and !avoid.has(c):
			sparse[c] = true
	kit.fill_small(sparse, 9)
	# Vành ngoài có thể còn ô lẻ: lấp bụi/hoa, không va chạm.
	var rest := {}
	for c in rim:
		rest[c] = true
	kit.fill_small(rest, 8)


# ---------- vật dụng ----------

func scatter_props() -> void:
	kit.add_sign("Sign_notice", kit.center(2, 7), "LÀNG THUNG LŨNG\nĐạo quán ở phía bắc, đường ra Rừng Sương ở phía đông.\nBảng này do trưởng làng dựng.")
	kit.add_chest("Chest_plaza", kit.center(5, 11), &"coin", 20)
	var crates := [Vector2i(-5, 10), Vector2i(-4, 10), Vector2i(-5, 11)]
	for i in crates.size():
		kit.add_scene(load("res://content/destroyable/crate.tscn"), "Crate%d" % (i + 1), kit.center(crates[i].x, crates[i].y))
	var pots := [Vector2i(-24, 0), Vector2i(1, 3), Vector2i(22, 0), Vector2i(-21, 8), Vector2i(15, 10)]
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
	kit.add_npc("OldMan", "oldman", kit.center(16, 11), "Ông lão", "Hồi trẻ ta cũng từng đi qua cổng torii kia.\nBây giờ chỉ thích ngồi ngắm người qua lại.", 20.0)


## Từ điểm xuất hiện phải tới được cửa mỗi nhà, cổng torii và NPC (ô đặc = ô đã chiếm, trừ ô cửa).
func check_reachable() -> void:
	var doors := {}
	for h in HOUSES:
		doors[door_tile(h)] = true
	var walk := {}
	for c in area:
		if !kit.occupied.has(c) or doors.has(c):
			walk[c] = true
	var seen := Kit.reachable(walk, Vector2i(-1, 8))
	var targets := {"torii": Vector2i(27, 1), "Chest_plaza": Vector2i(5, 11)}
	for h in HOUSES:
		targets["door " + String(h[0]) + str(h[1])] = door_tile(h) + Vector2i(0, 1)
	for p in poi_points():
		targets["poi " + str(p)] = p
	for k in targets:
		if !seen.has(targets[k]):
			printerr("KHÔNG TỚI ĐƯỢC: ", k, " ", targets[k])
