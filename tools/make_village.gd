# Dựng content/map/village.tscn từ bố cục viết tay bên dưới (không sinh ngẫu nhiên).
# Chạy: godot --headless --path game --script ../tools/make_village.gd
# Cần tileset_village.tres (tools/build_village_tileset.gd) và content/map/*.tscn của map.tscn.
#
# Toạ độ: ô (tx, ty); Tilemap đặt ở ORIGIN nên ô (tx, ty) phủ pixel [16*tx+OX, 16*tx+OX+16) x [16*ty+OY, 16*ty+OY+16).
# CameraGrid của pack có offset (24, 16): tâm màn hình (cx, cy) ở (320*cx+24, 176*cy+16). ORIGIN = (24, 24) căn làng
# (3x2 màn hình, tx -29..30, ty -5..16) khít với lưới camera: x [-456, 504], y [-72, 280].
extends SceneTree

const MAP = preload("res://system/map/map.tscn")
const TILESET = preload("res://content/map/tileset_village.tres")
const SPAWN = preload("res://system/world/spawn_point.gd")
const CHEST = preload("res://content/interactable/chest.tscn")
const SIGN = preload("res://content/interactable/sign.tscn")
const CRATE = preload("res://content/destroyable/crate.tscn")
const POT = preload("res://content/destroyable/pot.tscn")
const ENV_SHAPE = preload("res://system/environment/environment_shape.gd")
const ENV_NEUTRAL = preload("res://content/environment/neutral/environment.tres")

const WALL = 0       # lớp có y-sort: công trình, cây
const WALL2 = 1
const FLOOR = 3
const HOUSE = 6      # nguồn TilesetHouse
const NATURE = 7     # nguồn TilesetNature
const GRASS = 0      # terrain cỏ
const DIRT = 1       # terrain đất

const OX = 24
const OY = 24
const ORIGIN = Vector2(OX, OY)

const X0 = -29
const X1 = 30
const Y0 = -5
const Y1 = 16

# Công trình trong TilesetHouse: [tên, cột, hàng, rộng, cao, cột cửa (tương đối)]
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

# Hàng nhà phía bắc (cao 3, ô cuối là ty=-1): [loại, ô x trái, biển ở cửa]
const ROW = [
	["orange", -27, "Nhà bà Lan.\nCửa khoá, chắc bà đi chợ rồi."],
	["shop", -20, "Cửa hàng tạp hoá.\nĐang nghỉ bán."],
	["inn", -13, "Quán trọ Sương Mai.\nHôm nay hết phòng."],
	["dojo", -6, "ĐẠO QUÁN THUNG LŨNG\nChưa mở cửa nhận đệ tử."],
	["orange2", 2, "Nhà ông Tám.\nCó tiếng ngáy bên trong."],
	["clay", 10, "Lò gốm.\nBếp còn ấm nhưng không thấy ai."],
	["cabin", 17, "Nhà gỗ của người săn thú."],
	["red", 24, "Nhà trưởng làng.\nCửa đóng, treo biển 'đi vắng'."],
]

# Cây 2x2 trong TilesetNature: [cột, hàng]
const TREES = [[0, 0], [2, 0], [14, 0], [16, 0]]
const CLUSTER = [16, 2, 4, 3]   # cụm cây lime 4x3

var tilemap: TileMap
var village: Node2D
var colliders: StaticBody2D


func _initialize() -> void:
	village = MAP.instantiate(PackedScene.GEN_EDIT_STATE_MAIN_INHERITED) as Node2D
	village.name = "Village"
	village.set("scene_id", "village")
	tilemap = village.get_node("Tilemap")
	tilemap.tile_set = TILESET
	tilemap.position = ORIGIN

	colliders = StaticBody2D.new()
	colliders.name = "Colliders"
	colliders.collision_layer = 1
	add(village, colliders)

	paint_ground()
	build_houses()
	build_east_gate()
	plant_trees()
	scatter_props()
	add_borders()

	var shape := ENV_SHAPE.new() as CollisionShape2D
	shape.shape = RectangleShape2D.new()
	shape.shape.size = Vector2(960, 352)
	shape.position = Vector2(24, 104)
	shape.resource_environment = ENV_NEUTRAL
	add(village.get_node("EnvironmentArea"), shape)

	var spawn := SPAWN.new() as Marker2D
	spawn.name = "start"
	spawn.position = tile_center(-1, 8)
	add(village, spawn)

	var packed := PackedScene.new()
	var err := packed.pack(village)
	if err == OK:
		err = ResourceSaver.save(packed, "res://content/map/village.tscn")
	print("village.tscn err=", err)
	village.free()
	quit()


# ---------- nền ----------

func paint_ground() -> void:
	var grass: Array[Vector2i] = []
	for x in range(X0, X1 + 1):
		for y in range(Y0, Y1 + 1):
			grass.append(Vector2i(x, y))
	tilemap.set_cells_terrain_connect(FLOOR, grass, 0, GRASS)
	# Đường chính ngang (2 ô) và các lối nhỏ từ cửa từng nhà xuống đường chính.
	var dirt: Array[Vector2i] = []
	for x in range(X0 + 1, 29):
		dirt.append(Vector2i(x, 5))
		dirt.append(Vector2i(x, 6))
	for entry in ROW:
		var door_x := door_tile_x(entry)
		for y in range(0, 5):
			dirt.append(Vector2i(door_x, y))
			dirt.append(Vector2i(door_x + 1, y))
	# Quảng trường phía nam.
	for x in range(-6, 7):
		for y in range(7, 12):
			dirt.append(Vector2i(x, y))
	tilemap.set_cells_terrain_connect(FLOOR, dirt, 0, DIRT)


func door_tile_x(entry: Array) -> int:
	return entry[1] + BUILDINGS[entry[0]][4]


# ---------- công trình ----------

func build_houses() -> void:
	for entry in ROW:
		var b: Array = BUILDINGS[entry[0]]
		var tx: int = entry[1]
		var ty := -3
		stamp(WALL, HOUSE, b[0], b[1], b[2], b[3], tx, ty)
		collide_building(tx, ty, b[2], b[3], b[4])
		var sign := SIGN.instantiate() as Node2D
		sign.name = "Door_" + String(entry[0])
		sign.set("text", entry[2])
		sign.set("show_post", false)
		sign.set("prompt", "Xem")
		sign.position = tile_center(tx + b[4], ty + b[3] - 1) - ORIGIN
		add(tilemap, sign)
	# Biển DOJO treo ngay trên cửa đạo quán (lớp phủ lên hàng mái dưới).
	for entry in ROW:
		if entry[0] == "dojo":
			stamp(WALL2, HOUSE, 4, 4, 2, 1, entry[1] + 1, -2)


func build_east_gate() -> void:
	# Cổng torii cuối đường chính. Hai cột chặn, giữa để trống.
	stamp(WALL, HOUSE, 0, 5, 3, 2, 26, 5)
	collide_rect(Rect2(26 * 16 + OX + 11, 6 * 16 + OY, 6, 16))
	collide_rect(Rect2(26 * 16 + OX + 32, 6 * 16 + OY, 6, 16))
	var board := SIGN.instantiate() as Node2D
	board.name = "Sign_east"
	board.set("text", "Đường ra ngoài làng.\nPhía trước còn chưa mở.")
	board.position = tile_center(25, 4) - ORIGIN
	add(tilemap, board)


# ---------- cây ----------

func plant_trees() -> void:
	var n := 0
	# Hàng cây phía bắc, sát mép.
	for x in range(X0, X1, 2):
		place_tree(x, Y0, n)
		n += 1
	# Hàng cây phía nam.
	for x in range(X0, X1, 2):
		place_tree(x, Y1 - 1, n)
		n += 1
	# Hai bên, chừa lối đường chính (ty 4..7).
	for y in range(Y0 + 2, Y1 - 1, 2):
		if y >= 3 and y <= 7:
			continue
		place_tree(X0, y, n)
		place_tree(X1 - 1, y, n + 1)
		n += 2
	# Hai cụm lớn ở nam.
	stamp(WALL, NATURE, CLUSTER[0], CLUSTER[1], CLUSTER[2], CLUSTER[3], -22, 9)
	collide_rect(Rect2(-22 * 16 + OX + 4, 11 * 16 + OY + 4, 56, 10))
	stamp(WALL, NATURE, CLUSTER[0], CLUSTER[1], CLUSTER[2], CLUSTER[3], 16, 9)
	collide_rect(Rect2(16 * 16 + OX + 4, 11 * 16 + OY + 4, 56, 10))
	# Gốc cây và bụi trang trí.
	stamp(WALL, NATURE, 0, 8, 2, 2, -10, 9)
	for entry in ROW:
		var door_x := door_tile_x(entry)
		tilemap.set_cell(WALL2, Vector2i(door_x - 1, 0), NATURE, Vector2i((door_x * 3) % 10, 10))
		tilemap.set_cell(WALL2, Vector2i(door_x + 2, 0), NATURE, Vector2i((door_x * 7) % 10, 10))


func place_tree(tx: int, ty: int, variant: int) -> void:
	var t: Array = TREES[(variant * 5 + tx) % TREES.size()]
	stamp(WALL, NATURE, t[0], t[1], 2, 2, tx, ty)
	collide_rect(Rect2(tx * 16 + OX + 6, (ty + 1) * 16 + OY + 6, 20, 8))


# ---------- vật dụng ----------

func scatter_props() -> void:
	var notice := SIGN.instantiate() as Node2D
	notice.name = "Sign_notice"
	notice.set("text", "LÀNG THUNG LŨNG\nCon đường đông dẫn ra ngoài.\nBảng này do trưởng làng dựng.")
	notice.position = tile_center(2, 7) - ORIGIN
	add(tilemap, notice)

	var chest := CHEST.instantiate() as Node2D
	chest.name = "Chest_plaza"
	chest.set("item_id", &"coin")
	chest.set("amount", 20)
	chest.position = tile_center(5, 11) - ORIGIN
	add(tilemap, chest)

	var crates := [Vector2i(-5, 10), Vector2i(-4, 10), Vector2i(-5, 11)]
	for i in crates.size():
		var crate := CRATE.instantiate() as Node2D
		crate.name = "Crate%d" % (i + 1)
		crate.position = tile_center(crates[i].x, crates[i].y) - ORIGIN
		add(tilemap, crate)
	var pots := [Vector2i(-24, -0), Vector2i(1, 0), Vector2i(21, 0)]
	for i in pots.size():
		var pot := POT.instantiate() as Node2D
		pot.name = "Pot%d" % (i + 1)
		pot.position = tile_center(pots[i].x, pots[i].y) - ORIGIN
		add(tilemap, pot)


func add_borders() -> void:
	var left := X0 * 16 + OX
	var right := (X1 + 1) * 16 + OX
	var top := Y0 * 16 + OY
	var bottom := (Y1 + 1) * 16 + OY
	collide_rect(Rect2(left - 16, top - 16, right - left + 32, 16))
	collide_rect(Rect2(left - 16, bottom, right - left + 32, 16))
	collide_rect(Rect2(left - 16, top, 16, bottom - top))
	collide_rect(Rect2(right, top, 16, bottom - top))


# ---------- tiện ích ----------

## Vẽ khối ô [ax, ay, w, h] của nguồn `source` vào lớp `layer` tại ô (tx, ty); bỏ qua ô trong suốt.
func stamp(layer: int, source: int, ax: int, ay: int, w: int, h: int, tx: int, ty: int) -> void:
	var atlas := TILESET.get_source(source) as TileSetAtlasSource
	for j in h:
		for i in w:
			if atlas.has_tile(Vector2i(ax + i, ay + j)):
				tilemap.set_cell(layer, Vector2i(tx + i, ty + j), source, Vector2i(ax + i, ay + j))


## Va chạm của một công trình: cả khối trừ ô cửa ở hàng cuối.
func collide_building(tx: int, ty: int, w: int, h: int, door: int) -> void:
	var x0 := tx * 16 + OX
	var y0 := ty * 16 + OY
	collide_rect(Rect2(x0, y0, w * 16, (h - 1) * 16))
	var y_last := y0 + (h - 1) * 16
	if door > 0:
		collide_rect(Rect2(x0, y_last, door * 16, 16))
	if w - door - 1 > 0:
		collide_rect(Rect2(x0 + (door + 1) * 16, y_last, (w - door - 1) * 16, 16))


func collide_rect(rect: Rect2) -> void:
	var shape := CollisionShape2D.new()
	shape.shape = RectangleShape2D.new()
	shape.shape.size = rect.size
	shape.position = rect.position + rect.size / 2.0
	add(colliders, shape)


## Tâm ô (tx, ty) trong toạ độ thế giới.
func tile_center(tx: int, ty: int) -> Vector2:
	return Vector2(tx * 16 + OX + 8, ty * 16 + OY + 8)


func add(parent: Node, child: Node) -> void:
	parent.add_child(child)
	child.owner = village
