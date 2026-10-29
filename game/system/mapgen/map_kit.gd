extends RefCounted
## Bộ dụng cụ dựng map từ script (dùng bởi tools/make_*.gd). Không dùng khi chạy game.
## Toạ độ ô (tx, ty): Tilemap đặt ở ORIGIN = (8, 8) nên ô phủ pixel [16*tx+8, 16*tx+24) x [16*ty+8, 16*ty+24).
## CameraGrid của pack có offset (24, 16): màn hình (cx, cy) phủ x [320cx-136, 320cx+184], y [176cy-72, 176cy+104].
## Nên một màn hình = tx -9..10, ty -5..5; làng 3x2 màn hình (cx -1..1, cy 0..1) = tx -29..30, ty -5..16.


const MAP = preload("res://system/map/map.tscn")
const SPAWN = preload("res://system/world/spawn_point.gd")
const PORTAL = preload("res://system/world/portal.gd")
const SIGN = preload("res://content/interactable/sign.tscn")
const CHEST = preload("res://content/interactable/chest.tscn")
const NPC = preload("res://content/character/npc.tscn")
const ENV_SHAPE = preload("res://system/environment/environment_shape.gd")
const ENV_NEUTRAL = preload("res://content/environment/neutral/environment.tres")
const TRAMPLE_GRASS = preload("res://content/destroyable/trampleable_grass.tscn")
const PUSH_CRATE = preload("res://content/destroyable/push_crate.tscn")
const TILESET = preload("res://content/map/tileset_village.tres")

const WALL = 0       # lớp có y-sort: công trình, cây, đồ vật
const WALL2 = 1
const FLOOR = 3
const FLOOR_PACK = 1 # nguồn TilesetFloor (có terrain cỏ/đất)
const INTERIOR_FLOOR = 2
const WALL_SIMPLE = 4
const HOUSE = 6
const NATURE = 7
const ELEMENT = 8
const RELIEF = 9
const GRASS = 0      # terrain cỏ
const DIRT = 1       # terrain đất
const ORIGIN = Vector2(8, 8)
const MARGIN = 3

var map:Node2D
var tilemap:TileMap
var colliders:StaticBody2D


func _init(scene_id:String) -> void:
	map = MAP.instantiate(PackedScene.GEN_EDIT_STATE_MAIN_INHERITED) as Node2D
	map.name = scene_id.to_pascal_case()
	map.set("scene_id", scene_id)
	tilemap = map.get_node("Tilemap")
	tilemap.tile_set = TILESET
	tilemap.position = ORIGIN
	colliders = StaticBody2D.new()
	colliders.name = "Colliders"
	colliders.collision_layer = 1
	add(map, colliders)


## Ghi scene, in kết quả, giải phóng.
func save(path:String) -> int:
	var packed := PackedScene.new()
	var err := packed.pack(map)
	if err == OK:
		err = ResourceSaver.save(packed, path)
	print(path, " err=", err)
	map.free()
	return err


# ---------- toạ độ ----------

## Tâm ô (tx, ty) trong toạ độ thế giới.
func center(tx:float, ty:float) -> Vector2:
	return Vector2(tx * 16 + ORIGIN.x + 8, ty * 16 + ORIGIN.y + 8)


## Góc trên trái của ô (tx, ty) trong toạ độ thế giới.
func corner(tx:float, ty:float) -> Vector2:
	return Vector2(tx * 16 + ORIGIN.x, ty * 16 + ORIGIN.y)


## Vị trí cục bộ trong Tilemap (nơi đặt vật cần y-sort) của một điểm thế giới.
func local(point:Vector2) -> Vector2:
	return point - ORIGIN


# ---------- ô ----------

## Vẽ khối ô [ax, ay, w, h] của nguồn `source` vào lớp `layer` tại ô (tx, ty); bỏ qua ô trong suốt.
func stamp(layer:int, source:int, ax:int, ay:int, w:int, h:int, tx:int, ty:int) -> void:
	var atlas := TILESET.get_source(source) as TileSetAtlasSource
	for j in h:
		for i in w:
			if atlas.has_tile(Vector2i(ax + i, ay + j)):
				tilemap.set_cell(layer, Vector2i(tx + i, ty + j), source, Vector2i(ax + i, ay + j))


func fill(layer:int, source:int, atlas:Vector2i, x0:int, y0:int, x1:int, y1:int) -> void:
	for x in range(x0, x1 + 1):
		for y in range(y0, y1 + 1):
			tilemap.set_cell(layer, Vector2i(x, y), source, atlas)


## Nền cỏ rồi đường đất (terrain tự chọn ô viền). `dirt` là các ô đất.
func paint_ground(x0:int, y0:int, x1:int, y1:int, dirt:Array[Vector2i]) -> void:
	# Nền cỏ nở ra MARGIN ô quanh khu: ô viền terrain ở rìa không để lộ khoảng trống dưới cây ngoài cùng.
	var grass:Array[Vector2i] = []
	for x in range(x0 - MARGIN, x1 + 1 + MARGIN):
		for y in range(y0 - MARGIN, y1 + 1 + MARGIN):
			grass.append(Vector2i(x, y))
	tilemap.set_cells_terrain_connect(FLOOR, grass, 0, GRASS)
	if !dirt.is_empty():
		tilemap.set_cells_terrain_connect(FLOOR, dirt, 0, DIRT)


## Khung tường 5x5 của TilesetWallSimple (bộ màu bắt đầu ở (bx, by)) quanh hình chữ nhật ô [x0..x1] x [y0..y1].
## `gap` là danh sách ô (ở hàng dưới cùng) để trống làm lối ra.
func wall_ring(bx:int, by:int, x0:int, y0:int, x1:int, y1:int, gap:Array[int] = []) -> void:
	for x in range(x0, x1 + 1):
		var top := Vector2i(bx + (0 if x == x0 else 4 if x == x1 else 2), by)
		var bottom := Vector2i(bx + (0 if x == x0 else 4 if x == x1 else 2), by + 4)
		tilemap.set_cell(WALL, Vector2i(x, y0), WALL_SIMPLE, top)
		if !gap.has(x):
			tilemap.set_cell(WALL, Vector2i(x, y1), WALL_SIMPLE, bottom)
	for y in range(y0 + 1, y1):
		tilemap.set_cell(WALL, Vector2i(x0, y), WALL_SIMPLE, Vector2i(bx, by + 2))
		tilemap.set_cell(WALL, Vector2i(x1, y), WALL_SIMPLE, Vector2i(bx + 4, by + 2))


# ---------- đường, bãi trống, cây rải ----------

const ABANDONED = 0
const FLOOR_DETAIL = 2

## Cây: khoá -> [nguồn, cột atlas, hàng atlas, rộng, cao]
const TREES = {
	"t_green": [7, 0, 0, 2, 2], "t_pine": [7, 2, 0, 2, 2], "t_green2": [7, 16, 0, 2, 2], "t_gold": [7, 18, 0, 2, 2],
	"t_pink": [7, 14, 0, 2, 2], "t_young": [7, 6, 8, 2, 2], "t_dead": [7, 4, 0, 2, 2], "t_moss": [0, 4, 6, 2, 2],
	"c_pine": [7, 0, 2, 4, 3], "c_green": [7, 4, 2, 4, 3], "c_green2": [7, 16, 2, 4, 3], "c_pink": [7, 12, 2, 4, 3],
	"c_moss": [0, 0, 6, 4, 3], "c_dead": [7, 0, 5, 4, 3],
	"big_green": [7, 3, 18, 3, 3], "big_pink": [7, 0, 18, 3, 3], "big_autumn": [7, 9, 18, 3, 3],
}

## Ô đã bị chiếm (nhà, cây, đá, vật): Vector2i -> true.
var occupied := {}


## Băm xác định theo (x, y, seed) -> [0, 1). Thêm/bớt một cây không làm dịch các cây khác.
static func hash01(x:int, y:int, seed_value:int) -> float:
	var h := (x * 73856093) ^ (y * 19349663) ^ (seed_value * 83492791)
	h = (h ^ (h >> 13)) * 1274126177
	h = h ^ (h >> 16)
	return float(h & 0xFFFF) / 65536.0


## Ô đất của đường gấp khúc qua các điểm [x, y]: cọ w x w (góc trên trái) ở mỗi bước, bước đi 4-liên thông.
static func path_cells(points:Array, w := 2) -> Dictionary:
	var out := {}
	for i in points.size() - 1:
		var x:int = points[i][0]
		var y:int = points[i][1]
		var x1:int = points[i + 1][0]
		var y1:int = points[i + 1][1]
		var dx := absi(x1 - x)
		var dy := absi(y1 - y)
		var sx := 1 if x1 > x else -1
		var sy := 1 if y1 > y else -1
		var mx := 0
		var my := 0
		for s in dx + dy + 1:
			for j in w:
				for k in w:
					out[Vector2i(x + k, y + j)] = true
			if s == dx + dy:
				break
			# Một bước 4-liên thông: đi trục đang tụt lại so với tỉ lệ đường thẳng.
			if my >= dy or (mx < dx and (mx + 0.5) / dx <= (my + 0.5) / dy):
				x += sx
				mx += 1
			else:
				y += sy
				my += 1
	return out


## Ô có tâm nằm trong elip (tâm c, bán kính r tính theo ô; tâm ô = (tx + 0.5, ty + 0.5)).
static func ellipse_cells(c:Vector2, r:Vector2) -> Dictionary:
	var out := {}
	for y in range(floori(c.y - r.y) - 1, ceili(c.y + r.y) + 1):
		for x in range(floori(c.x - r.x) - 1, ceili(c.x + r.x) + 1):
			var d := Vector2((x + 0.5 - c.x) / r.x, (y + 0.5 - c.y) / r.y)
			if d.length_squared() <= 1.0:
				out[Vector2i(x, y)] = true
	return out


static func rect_cells(r:Rect2i) -> Dictionary:
	var out := {}
	for y in range(r.position.y, r.end.y):
		for x in range(r.position.x, r.end.x):
			out[Vector2i(x, y)] = true
	return out


static func union(a:Dictionary, b:Dictionary) -> Dictionary:
	var out := a.duplicate()
	out.merge(b)
	return out


const NEIGH4 = [Vector2i(1, 0), Vector2i(-1, 0), Vector2i(0, 1), Vector2i(0, -1)]


## Làm mượt đất: lấp ô cỏ kẹp giữa đất; bỏ mỏm đất chỉ có <= 1 láng giềng (trừ khi fill_only).
## Terrain khớp góc/cạnh nên lỗ 1 ô hay mỏm 1 ô cho ô viền xấu.
static func smooth_dirt(d:Dictionary, passes := 2, fill_only := false) -> Dictionary:
	var cur := d.duplicate()
	for p in passes:
		var next := cur.duplicate()
		var seen := {}
		for c in cur:
			for n in NEIGH4:
				seen[c + n] = true
			seen[c] = true
		for c in seen:
			var count := 0
			for n in NEIGH4:
				if cur.has(c + n):
					count += 1
			if !cur.has(c):
				var sandwich: bool = (cur.has(c + Vector2i(1, 0)) and cur.has(c + Vector2i(-1, 0))) or (cur.has(c + Vector2i(0, 1)) and cur.has(c + Vector2i(0, -1)))
				if count >= 3 or sandwich:
					next[c] = true
			elif !fill_only and count <= 1:
				next.erase(c)
		cur = next
	return cur


func reserve(tx:int, ty:int, w:int, h:int) -> void:
	for j in h:
		for i in w:
			occupied[Vector2i(tx + i, ty + j)] = true


func reserve_cells(cells:Dictionary) -> void:
	for c in cells:
		occupied[c] = true


## Nở tập ô ra `n` ô (cả chéo).
static func grow_cells(cells:Dictionary, n := 1) -> Dictionary:
	var out := cells.duplicate()
	for c in cells:
		for j in range(-n, n + 1):
			for i in range(-n, n + 1):
				out[c + Vector2i(i, j)] = true
	return out


## `area` = các ô được phép; trả true nếu khối w x h toàn ô được phép và chưa bị chiếm.
func fits(area:Dictionary, tx:int, ty:int, w:int, h:int) -> bool:
	for j in h:
		for i in w:
			var c := Vector2i(tx + i, ty + j)
			if !area.has(c) or occupied.has(c):
				return false
	return true


## Đặt một cây theo khoá trong TREES: vẽ, giữ chỗ, và (tuỳ chọn) va chạm gốc.
func plant(kind:String, tx:int, ty:int, collide := false) -> void:
	var t:Array = TREES[kind]
	stamp(WALL, t[0], t[1], t[2], t[3], t[4], tx, ty)
	reserve(tx, ty, t[3], t[4])
	if collide:
		collide_base(tx, ty, t[3], t[4], 14.0 if t[3] == 3 else 6.0)


## Rải cây trên lưới bước 2 (neo ở `anchor`, hàng lẻ không lệch nên khe luôn >= 2 ô): mỗi điểm giữ với xác suất `chance`,
## thử cụm lớn (xác suất `big_chance`) rồi cây nhỏ. Trả số cây đặt.
func scatter(area:Dictionary, anchor:Vector2i, seed_value:int, small:Array, big:Array, big_chance := 0.0, chance := 1.0, collide := false) -> int:
	var minx := 1 << 30
	var maxx := -(1 << 30)
	var miny := 1 << 30
	var maxy := -(1 << 30)
	for c in area:
		minx = mini(minx, c.x)
		maxx = maxi(maxx, c.x)
		miny = mini(miny, c.y)
		maxy = maxi(maxy, c.y)
	var n := 0
	var y := anchor.y + floori((miny - anchor.y) / 2.0) * 2
	while y <= maxy:
		var x := anchor.x + floori((minx - anchor.x) / 2.0) * 2
		while x <= maxx:
			var h := hash01(x, y, seed_value)
			if h < chance:
				var h2 := hash01(x, y, seed_value + 101)
				var h3 := hash01(x, y, seed_value + 202)
				var done := false
				if !big.is_empty() and h2 < big_chance:
					var kind:String = big[int(h3 * big.size()) % big.size()]
					var t:Array = TREES[kind]
					if fits(area, x, y, t[3], t[4]):
						plant(kind, x, y, collide)
						n += 1
						done = true
				if !done and !small.is_empty():
					var kind2:String = small[int(h3 * small.size()) % small.size()]
					var t2:Array = TREES[kind2]
					if fits(area, x, y, t2[3], t2[4]):
						plant(kind2, x, y, collide)
						n += 1
			x += 2
		y += 2
	return n


## Lấp ô trống còn lại trong `area` bằng bụi / gốc / khúc gỗ / đá nhỏ (lớp WALL2, không va chạm riêng).
func fill_small(area:Dictionary, seed_value:int) -> void:
	for c in area:
		if occupied.has(c):
			continue
		var h := hash01(c.x, c.y, seed_value)
		var atlas:Vector2i
		if h < 0.78:
			atlas = Vector2i(int(h * 100.0) % 11, 10)
		elif h < 0.85:
			atlas = Vector2i(4, 8)
		elif h < 0.9:
			atlas = Vector2i(5, 8)
		else:
			atlas = Vector2i(15, 9) if h < 0.95 else Vector2i(18, 9)
		tilemap.set_cell(WALL2, c, NATURE, atlas)


## Đổi một phần cụm cỏ (ô WALL2 hàng 10 của tileset thiên nhiên) trong `cells` thành vật thể giẫm được, mọc lại sau khi giẫm.
## Trả số cụm đã đổi. Chỉ nên gọi cho vùng người chơi đi tới được.
func trample_grass(cells:Dictionary, seed_value:int, share := 0.5) -> int:
	var count := 0
	for c in cells:
		if tilemap.get_cell_source_id(WALL2, c) != NATURE or tilemap.get_cell_atlas_coords(WALL2, c).y != 10:
			continue
		if hash01(c.x, c.y, seed_value) >= share:
			continue
		tilemap.erase_cell(WALL2, c)
		add_scene(TRAMPLE_GRASS, "Grass_%d_%d" % [c.x, c.y], center(c.x, c.y))
		count += 1
	return count


## Hoa (lớp FLOOR_DETAIL) và bụi (WALL2) ở ô cỏ kề trên/dưới/hai bên ô đất; không va chạm.
func edge_decor(dirt:Dictionary, seed_value:int, chance := 0.12) -> void:
	for d in dirt:
		for n in NEIGH4:
			var c:Vector2i = d + n
			if dirt.has(c) or occupied.has(c):
				continue
			var h := hash01(c.x, c.y, seed_value)
			if h >= chance:
				continue
			occupied[c] = true
			if h < chance * 0.6:
				tilemap.set_cell(FLOOR_DETAIL, c, NATURE, [Vector2i(0, 11), Vector2i(3, 11), Vector2i(6, 11), Vector2i(7, 11)][int(h * 1000.0) % 4])
			else:
				tilemap.set_cell(WALL2, c, NATURE, Vector2i(int(h * 1000.0) % 11, 10))


## Va chạm từ mặt nạ ô bị chặn: gộp ô liên tiếp theo hàng, rồi gộp dọc các đoạn trùng. Trả số hình chữ nhật.
func collide_mask(blocked:Dictionary) -> int:
	if blocked.is_empty():
		return 0
	var miny := 1 << 30
	var maxy := -(1 << 30)
	var rows := {}
	for c in blocked:
		miny = mini(miny, c.y)
		maxy = maxi(maxy, c.y)
		if !rows.has(c.y):
			rows[c.y] = []
		rows[c.y].append(c.x)
	var active := {}   # Vector2i(x0, x1) -> y bắt đầu
	var count := 0
	for y in range(miny, maxy + 2):
		var runs := {}
		if rows.has(y):
			var xs:Array = rows[y]
			xs.sort()
			var start:int = xs[0]
			var prev:int = xs[0]
			for i in range(1, xs.size() + 1):
				if i == xs.size() or xs[i] != prev + 1:
					runs[Vector2i(start, prev)] = true
					if i < xs.size():
						start = xs[i]
				if i < xs.size():
					prev = xs[i]
		for key in active.keys():
			if !runs.has(key):
				var y0:int = active[key]
				collide_rect(Rect2(corner(key.x, y0), Vector2((key.y - key.x + 1) * 16, (y - y0) * 16)))
				count += 1
				active.erase(key)
		for key in runs:
			if !active.has(key):
				active[key] = y
	return count


## BFS 4-liên thông trên các ô đi được từ `start`; trả tập ô tới được.
static func reachable(walkable:Dictionary, start:Vector2i) -> Dictionary:
	var seen := {start: true}
	var queue := [start]
	while !queue.is_empty():
		var c:Vector2i = queue.pop_back()
		for n in NEIGH4:
			var m:Vector2i = c + n
			if walkable.has(m) and !seen.has(m):
				seen[m] = true
				queue.append(m)
	return seen


# ---------- va chạm ----------

func collide_rect(rect:Rect2) -> void:
	var shape := CollisionShape2D.new()
	shape.shape = RectangleShape2D.new()
	shape.shape.size = rect.size
	shape.position = rect.position + rect.size / 2.0
	add(colliders, shape)


## Va chạm của một khối ô (tx, ty, w, h): cả khối trừ ô cửa (cột tương đối `door`, -1 = không có cửa) ở hàng cuối.
func collide_block(tx:int, ty:int, w:int, h:int, door := -1) -> void:
	var p := corner(tx, ty)
	if door < 0:
		collide_rect(Rect2(p, Vector2(w * 16, h * 16)))
		return
	collide_rect(Rect2(p, Vector2(w * 16, (h - 1) * 16)))
	var y_last := p.y + (h - 1) * 16
	if door > 0:
		collide_rect(Rect2(p.x, y_last, door * 16, 16))
	if w - door - 1 > 0:
		collide_rect(Rect2(p.x + (door + 1) * 16, y_last, (w - door - 1) * 16, 16))


## Va chạm gốc của một vật nằm ở hàng ô cuối của khối (tx, ty, w, h): dải thấp ở giữa.
func collide_base(tx:int, ty:int, w:int, h:int, inset := 6.0, depth := 8.0) -> void:
	var p := corner(tx, ty + h - 1)
	collide_rect(Rect2(p.x + inset, p.y + 6, w * 16 - inset * 2, depth))


## Tường chặn quanh hình chữ nhật ô [x0..x1] x [y0..y1] (rìa ngoài của các ô đó), có thể chừa lỗ theo `gaps`.
func borders(x0:int, y0:int, x1:int, y1:int, gaps:Array[Rect2] = []) -> void:
	var left := corner(x0, y0).x
	var top := corner(x0, y0).y
	var right := corner(x1 + 1, y1 + 1).x
	var bottom := corner(x1 + 1, y1 + 1).y
	for rect in [Rect2(left - 16, top - 16, right - left + 32, 16), Rect2(left - 16, bottom, right - left + 32, 16),
			Rect2(left - 16, top, 16, bottom - top), Rect2(right, top, 16, bottom - top)]:
		collide_rect(rect)


# ---------- vật thể ----------

func add(parent:Node, child:Node) -> void:
	parent.add_child(child)
	child.owner = map


func add_env(center_point:Vector2, size:Vector2, environment:ResourceEnvironment = ENV_NEUTRAL) -> void:
	var shape := ENV_SHAPE.new() as CollisionShape2D
	shape.shape = RectangleShape2D.new()
	shape.shape.size = size
	shape.position = center_point
	shape.resource_environment = environment
	add(map.get_node("EnvironmentArea"), shape)


func add_spawn(spawn_name:String, point:Vector2, facing := Vector2.DOWN) -> void:
	var spawn := SPAWN.new() as Marker2D
	spawn.name = spawn_name
	spawn.position = point
	spawn.set("facing", facing)
	add(map, spawn)


## Cổng sang khu `target` (vào điểm `target_spawn`). `rect` là vùng kích hoạt trong toạ độ thế giới.
func add_portal(target:String, target_spawn:String, rect:Rect2) -> void:
	var portal := PORTAL.new() as Area2D
	portal.name = "Portal_" + target
	portal.set("portal_id", target)
	portal.set("target_scene", target)
	portal.set("target_spawn", target_spawn)
	portal.position = rect.get_center()
	add(map, portal)
	var shape := CollisionShape2D.new()
	shape.shape = RectangleShape2D.new()
	shape.shape.size = rect.size
	add(portal, shape)


## Biển chữ đặt ở điểm thế giới `point` (trong Tilemap để y-sort). `post` = vẽ cọc biển.
func add_sign(sign_name:String, point:Vector2, text:String, post := true, prompt := "") -> void:
	var node := SIGN.instantiate() as Node2D
	node.name = sign_name
	node.position = local(point)
	node.set("text", text)
	node.set("show_post", post)
	if !prompt.is_empty():
		node.set("prompt", prompt)
	add(tilemap, node)


func add_chest(chest_name:String, point:Vector2, item_id:StringName, amount := 1) -> void:
	var node := CHEST.instantiate() as Node2D
	node.name = chest_name
	node.position = local(point)
	node.set("item_id", item_id)
	node.set("amount", amount)
	add(tilemap, node)


## Thùng đẩy được đặt ở tâm ô (tx, ty), giữ ô đó cho cây khỏi mọc đè.
func add_push_crate(crate_name:String, tx:int, ty:int) -> void:
	add_scene(PUSH_CRATE, crate_name, center(tx, ty))
	reserve(tx, ty, 1, 1)


## Đá môi trường (tảng đá đen/xám boss có thể đẩy): `kind` như "black_wide", "gray_tall" (res://content/rock/rock_<kind>.tscn).
## Chiếm `w` x `h` ô để các lớp khác không đè lên.
func add_rock(rock_name:String, kind:String, tx:int, ty:int, w := 2, h := 1) -> void:
	# Nút đá nằm ở giữa khối ô nó chiếm (tâm của hộp va chạm đáy).
	add_scene(load("res://content/rock/rock_%s.tscn" % kind), rock_name, center(tx, ty) + Vector2((w - 1) * 8.0, (h - 1) * 8.0))
	reserve(tx, ty, w, h)


func add_scene(scene:PackedScene, node_name:String, point:Vector2) -> Node2D:
	var node := scene.instantiate() as Node2D
	node.name = node_name
	node.position = local(point)
	add(tilemap, node)
	return node


## NPC dùng bảng sprite `sheet` (res://content/character/npc/<tên>.png).
func add_npc(npc_name:String, sheet:String, point:Vector2, display_name:String, dialogue:String, wander := 0.0, facing := Vector2.DOWN) -> void:
	var node := NPC.instantiate() as Node2D
	node.name = npc_name
	node.position = local(point)
	node.set("sheet", load("res://content/character/npc/%s.png" % sheet))
	node.set("display_name", display_name)
	node.set("dialogue", dialogue)
	node.set("wander_radius", wander)
	add(tilemap, node)
