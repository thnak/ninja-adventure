# Dựng content/map/forest.tscn (Rừng Sương) từ bố cục viết tay, không sinh ngẫu nhiên.
# Chạy: godot --headless --path game --script ../tools/make_forest.gd
#
# Khu rộng 4x2 màn hình camera: tx -9..70, ty -5..16. Dữ liệu bên dưới chỉ gồm đường mòn (đường gấp khúc ở mức ô tile),
# các bãi trống và điểm nhấn; mọi ô còn lại là rừng rải bằng hàm băm cố định (cùng bố cục mỗi lần chạy).
# Va chạm của rừng là mặt nạ gộp, không theo từng cây. Bố cục và lý do: docs/design/layout-proposals.md.
extends SceneTree

const Kit = preload("res://system/mapgen/map_kit.gd")

const TX0 = -9
const TY0 = -5
const TX1 = 70
const TY1 = 16

# Đường mòn (toạ độ góc trên trái của cọ 2x2). Cắt ranh giới màn hình ty 5/6 theo chiều ngang, không chạy dọc theo nó.
const MAIN_A = [[-7, 3], [-2, 3], [2, 1], [6, 1], [9, 3], [13, 3], [16, 1], [19, 2], [21, 4], [22, 8]]
const LINK = [[24, 10], [28, 11]]
const MAIN_B = [[28, 11], [33, 12], [38, 10], [41, 7], [42, 3], [46, 1], [51, 2], [55, 4], [58, 2], [61, 0]]
const SPUR_E = [[55, 4], [57, 6]]

# Bãi trống (cỏ): [tâm, bán kính]
const GLADES = [
	[Vector2(-3, 3), Vector2(4.5, 2.2)],
	[Vector2(25.5, 9.5), Vector2(5.0, 3.0)],
	[Vector2(59.5, 8.5), Vector2(5.5, 3.2)],
	[Vector2(13.5, -2), Vector2(3.0, 1.6)],
	[Vector2(35.5, 11.5), Vector2(3.5, 2.0)],
]
const OPEN_RECTS = [Rect2i(16, -1, 2, 2), Rect2i(59, -1, 6, 3)]

# Điểm nhấn đặt tay: [khoá, tx, ty]
const LANDMARKS = [["big_pink", 7, -3], ["big_green", 17, 10], ["big_pink", 30, 6], ["big_green", 50, 8]]

var kit: Kit
var area := {}
var open := {}
var wood := {}   # ô thuộc khối rừng (không phải đường/bãi)


func _initialize() -> void:
	kit = Kit.new("forest")
	area = Kit.rect_cells(Rect2i(TX0, TY0, TX1 - TX0 + 1, TY1 - TY0 + 1))

	var dirt := Kit.path_cells(MAIN_A)
	dirt = Kit.union(dirt, Kit.path_cells(LINK))
	dirt = Kit.union(dirt, Kit.path_cells(MAIN_B))
	dirt = Kit.union(dirt, Kit.path_cells(SPUR_E))
	dirt = Kit.smooth_dirt(dirt)
	dirt = clip(dirt)
	var typed: Array[Vector2i] = []
	for c in dirt:
		typed.append(c)
	kit.paint_ground(TX0, TY0, TX1, TY1, typed)

	# Ô đi được: đường, bãi trống, sân cửa hang.
	open = dirt.duplicate()
	for g in GLADES:
		open = Kit.union(open, clip(Kit.ellipse_cells(g[0], g[1])))
	for r in OPEN_RECTS:
		open = Kit.union(open, clip(Kit.rect_cells(r)))

	# Cổng torii và cửa hang có va chạm riêng.
	var own := Kit.rect_cells(Rect2i(-9, 3, 3, 2))
	own = Kit.union(own, Kit.rect_cells(Rect2i(61, -3, 3, 3)))
	kit.reserve_cells(own)
	features()
	for c in area:
		if !open.has(c):
			wood[c] = true

	# Điểm nhấn trước, rồi khối rừng; chừa 1 ô quanh vùng đi được để thân cây không dính vào đường.
	for l in LANDMARKS:
		var t: Array = Kit.TREES[l[0]]
		if kit.fits(wood, l[1], l[2], t[3], t[4]):
			kit.plant(l[0], l[1], l[2])
		else:
			printerr("landmark ", l, " bị đường/bãi chiếm chỗ")
	kit.stamp(Kit.WALL, Kit.NATURE, 16, 5, 4, 3, 57, -4)
	kit.reserve(57, -4, 4, 3)
	kit.stamp(Kit.WALL, Kit.NATURE, 16, 8, 2, 2, 65, -1)
	kit.reserve(65, -1, 2, 2)

	plant_forest()
	kit.fill_small(wood, 5)
	kit.edge_decor(dirt, 11, 0.2)
	glade_decor()

	var blocked := blocked_mask(own)
	var rects := kit.collide_mask(blocked)
	print("collision rects from forest mask: ", rects)
	check_reachable(blocked)

	kit.borders(TX0, TY0, TX1, TY1)
	kit.add_env(Vector2(504, 104), Vector2(1280, 352))
	kit.save("res://content/map/forest.tscn")
	quit()


func clip(cells: Dictionary) -> Dictionary:
	var out := {}
	for c in cells:
		if area.has(c):
			out[c] = true
	return out


## Rừng chia theo vùng dọc đường: gần làng (tx <= 10), giữa, gần hang (tx >= 51).
func plant_forest() -> void:
	var zones := [
		[-9, 10, ["t_green", "t_green", "t_gold", "t_green2"], ["c_green", "c_green2"]],
		[11, 50, ["t_green", "t_pine", "t_pine", "t_green2", "t_moss", "t_young"], ["c_pine", "c_green", "c_moss", "c_pine"]],
		[51, 70, ["t_pine", "t_dead", "t_green2", "t_pink", "t_dead"], ["c_pine", "c_dead", "c_pine"]],
	]
	for z in zones:
		var zone_area := {}
		for c in area:
			if c.x >= z[0] and c.x <= z[1] and !open.has(c):
				zone_area[c] = true
		kit.scatter(zone_area, Vector2i(TX0, TY0), 7, z[2], z[3], 0.3)


## Bụi lẻ trong bãi trống (xác định theo hàm băm), tránh ô có vật đứng.
func glade_decor() -> void:
	var keep := [Vector2i(-1, 2), Vector2i(-3, 2), Vector2i(24, 9), Vector2i(21, 9), Vector2i(58, 7), Vector2i(14, -2), Vector2i(12, -1)]
	for c in open:
		if kit.occupied.has(c) and !open.has(c):
			continue
		var near_keep := false
		for k in keep:
			if absi(c.x - k.x) <= 1 and absi(c.y - k.y) <= 1:
				near_keep = true
		if near_keep or Kit.hash01(c.x, c.y, 19) > 0.06:
			continue
		if kit.tilemap.get_cell_source_id(Kit.FLOOR_DETAIL, c) != -1 or kit.tilemap.get_cell_source_id(Kit.WALL2, c) != -1:
			continue
		kit.tilemap.set_cell(Kit.WALL2, c, Kit.NATURE, Vector2i(int(Kit.hash01(c.x, c.y, 3) * 11.0) % 11, 10))


## Ô bị chặn: mọi ô không đi được, trừ ô có va chạm riêng và ô liền dưới một ô đi được (người chơi lấn 1 hàng vào tán cây).
func blocked_mask(own: Dictionary) -> Dictionary:
	var blocked := {}
	for c in area:
		if open.has(c) or own.has(c):
			continue
		if open.has(c + Vector2i(0, -1)):
			continue
		blocked[c] = true
	return blocked


func check_reachable(blocked: Dictionary) -> void:
	var walk := {}
	for c in area:
		if !blocked.has(c):
			walk[c] = true
	var seen := Kit.reachable(walk, Vector2i(-5, 3))
	var targets := {"cave door": Vector2i(62, -1), "cave spawn": Vector2i(62, 1), "nook chest": Vector2i(14, -2), "nook sign": Vector2i(12, -1),
		"hunter": Vector2i(24, 9), "glade chest": Vector2i(21, 9), "old man": Vector2i(58, 7), "girl": Vector2i(-1, 2), "torii": Vector2i(-8, 3)}
	for k in targets:
		if !seen.has(targets[k]):
			printerr("KHÔNG TỚI ĐƯỢC: ", k, " ", targets[k])


func features() -> void:
	# Cổng torii phía tây, nối về làng. Vùng kích hoạt ở ô giữa của cổng.
	kit.stamp(Kit.WALL, Kit.HOUSE, 0, 5, 3, 2, -9, 3)
	kit.collide_rect(Rect2(kit.corner(-9, 4) + Vector2(11, 0), Vector2(6, 16)))
	kit.collide_rect(Rect2(kit.corner(-9, 4) + Vector2(32, 0), Vector2(6, 16)))
	kit.add_portal("village", "from_forest", Rect2(kit.corner(-8, 3), Vector2(16, 32)))
	kit.add_spawn("from_village", kit.center(-5, 3) + Vector2(0, 8), Vector2.RIGHT)
	kit.add_sign("Sign_trail", kit.center(-3, 2), "RỪNG SƯƠNG\nTheo đường mòn đi về phía đông là tới hang mỏ.\nĐừng rời khỏi đường nếu không muốn lạc.")

	# Cửa hang phía đông bắc.
	kit.stamp(Kit.WALL, Kit.RELIEF, 0, 7, 3, 3, 61, -3)
	kit.collide_block(61, -3, 3, 3, 1)
	kit.add_portal("cave", "from_forest", Rect2(kit.corner(62, -1), Vector2(16, 16)))
	kit.add_spawn("from_cave", kit.center(62, 1), Vector2.DOWN)

	# Ngách bí mật (không có đường đất, vào qua khe cỏ) và khoảng trống giữa rừng.
	kit.add_chest("Chest_nook", kit.center(14, -2), &"herb", 2)
	kit.add_sign("Sign_nook", kit.center(12, -1), "Ai đó từng nghỉ chân ở đây.\nGhi bên dưới: 'Đừng báo cho trưởng làng'.", true)
	kit.add_chest("Chest_glade", kit.center(21, 9), &"coin", 15)

	# Người trong rừng.
	kit.add_npc("Girl", "child", kit.center(-1, 2), "Cô bé", "Tớ đuổi theo con bướm mà lạc tới tận đây. Chú đừng mách mẹ nhé!", 30.0)
	kit.add_npc("Hunter", "hunter", kit.center(24, 9), "Thợ săn", "Ở khoảng trống này nghe rõ tiếng gõ từ hang mỏ.\nTôi chưa dám vào sâu.", 20.0)
	kit.add_npc("OldMan", "oldman", kit.center(58, 7), "Ông lão", "Cửa hang ở ngay phía trên kia.\nNgày xưa ở đó khai thác đá cho cả làng.", 10.0)
