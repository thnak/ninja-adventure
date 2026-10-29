# Dựng content/map/forest.tscn (Rừng Sương) từ bố cục viết tay, không sinh ngẫu nhiên.
# Chạy: godot --headless --path game --script ../tools/make_forest.gd
#
# Bản đồ chia thành lưới 40x11 "ô lớn" 2x2 ô tile. Mỗi ô lớn là một trong: '#' rừng rậm (cây), '.' đất trống, '=' đường mòn.
# Bố cục dưới đây chỉ là dữ liệu: đường mòn (các điểm nối), các khoảng trống và ngách; mọi thứ còn lại là rừng.
# Khu rộng 4x2 màn hình camera: tx -9..70, ty -5..16.
extends SceneTree

const Kit = preload("res://system/mapgen/map_kit.gd")

const CELLS_X = 40
const CELLS_Y = 11
const TX0 = -9
const TY0 = -5

# Đường mòn: các điểm (cột, hàng) của ô lớn, nối thẳng theo hàng/cột từ điểm này sang điểm kế.
# Hàng ô lớn 5 (ty 5..6) nằm đúng ranh giới màn hình camera nên đường tránh đi dọc hàng đó.
const PATH = [[0, 4], [7, 4], [7, 3], [13, 3], [13, 6], [19, 6], [19, 4], [25, 4], [25, 7], [31, 7], [31, 6], [35, 6], [35, 2]]
const NOOK_PATH = [[10, 3], [10, 2]]          # nhánh lên ngách bí mật
# Khoảng trống: [cột đầu, hàng đầu, cột cuối, hàng cuối]
const CLEARINGS = [[1, 3, 4, 7], [15, 2, 22, 8], [31, 2, 38, 8], [9, 1, 12, 2]]
# Ô lớn dành cho cửa hang (không trồng cây).
const RESERVED = [[35, 1], [36, 1], [35, 2], [36, 2]]

const SMALL_TREES = [[0, 0], [2, 0], [14, 0], [16, 0]]

var grid := []
var kit: Kit


func _initialize() -> void:
	for y in CELLS_Y:
		grid.append("#".repeat(CELLS_X).split(""))
	for c in CLEARINGS:
		fill_cells(c, ".")
	carve(PATH)
	carve(NOOK_PATH)
	for r in RESERVED:
		grid[r[1]][r[0]] = "."

	kit = Kit.new("forest")
	paint()
	plant()
	decorate()
	features()
	kit.borders(TX0, TY0, TX0 + CELLS_X * 2 - 1, TY0 + CELLS_Y * 2 - 1)
	kit.add_env(Vector2(504, 104), Vector2(1280, 352))
	kit.save("res://content/map/forest.tscn")
	quit()


func fill_cells(rect: Array, ch: String) -> void:
	for y in range(rect[1], rect[3] + 1):
		for x in range(rect[0], rect[2] + 1):
			grid[y][x] = ch


func carve(points: Array) -> void:
	for i in points.size() - 1:
		var a: Array = points[i]
		var b: Array = points[i + 1]
		fill_cells([mini(a[0], b[0]), mini(a[1], b[1]), maxi(a[0], b[0]), maxi(a[1], b[1])], "=")


## Tile (tx, ty) góc trên trái của ô lớn (cx, cy).
func cell_tile(cx: int, cy: int) -> Vector2i:
	return Vector2i(TX0 + cx * 2, TY0 + cy * 2)


func is_open(cx: int, cy: int) -> bool:
	return grid[cy][cx] != "#"


func paint() -> void:
	var dirt: Array[Vector2i] = []
	for cy in CELLS_Y:
		for cx in CELLS_X:
			if grid[cy][cx] == "=":
				var t := cell_tile(cx, cy)
				for i in 4:
					dirt.append(t + Vector2i(i % 2, i / 2))
	kit.paint_ground(TX0, TY0, TX0 + CELLS_X * 2 - 1, TY0 + CELLS_Y * 2 - 1, dirt)


## Cây dày trên mọi ô '#'; va chạm gộp theo đoạn liên tiếp trên cùng hàng.
func plant() -> void:
	for cy in CELLS_Y:
		var run_start := -1
		for cx in CELLS_X + 1:
			var tree: bool = cx < CELLS_X and grid[cy][cx] == "#"
			if tree:
				var t := cell_tile(cx, cy)
				var pick: Array = SMALL_TREES[(cx * 7 + cy * 13 + (cx / 5)) % SMALL_TREES.size()]
				kit.stamp(Kit.WALL, Kit.NATURE, pick[0], pick[1], 2, 2, t.x, t.y)
				if run_start < 0:
					run_start = cx
			elif run_start >= 0:
				var t0 := cell_tile(run_start, cy)
				var width := (cx - run_start) * 32.0
				kit.collide_rect(Rect2(kit.corner(t0.x, t0.y + 1) + Vector2(4, 6), Vector2(width - 8, 8)))
				run_start = -1


## Bụi cỏ và hoa rải trên đất trống theo chỗ cố định (không ngẫu nhiên).
func decorate() -> void:
	for cy in CELLS_Y:
		for cx in CELLS_X:
			if grid[cy][cx] != ".":
				continue
			var t := cell_tile(cx, cy)
			var h := (cx * 31 + cy * 17) % 9
			if h == 0:
				kit.tilemap.set_cell(Kit.WALL2, t + Vector2i(1, 0), Kit.NATURE, Vector2i((cx + cy) % 10, 10))
			elif h == 4:
				kit.tilemap.set_cell(Kit.WALL2, t + Vector2i(0, 1), Kit.NATURE, Vector2i((cx * 3) % 4, 11))


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

	# Ngách bí mật (trên) và khoảng trống giữa rừng.
	kit.add_chest("Chest_nook", kit.center(12, -2), &"herb", 2)
	kit.add_sign("Sign_nook", kit.center(11, -1), "Ai đó từng nghỉ chân ở đây.\nGhi bên dưới: 'Đừng báo cho trưởng làng'.", true)
	kit.add_chest("Chest_glade", kit.center(28, 5), &"coin", 15)

	# Người trong rừng.
	kit.add_npc("Girl", "child", kit.center(-1, 2), "Cô bé", "Tớ đuổi theo con bướm mà lạc tới tận đây. Chú đừng mách mẹ nhé!", 30.0)
	kit.add_npc("Hunter", "hunter", kit.center(24, 5), "Thợ săn", "Ở khoảng trống này nghe rõ tiếng gõ từ hang mỏ.\nTôi chưa dám vào sâu.", 20.0)
	kit.add_npc("OldMan", "oldman", kit.center(58, 7), "Ông lão", "Cửa hang ở ngay phía trên kia.\nNgày xưa ở đó khai thác đá cho cả làng.", 10.0)
