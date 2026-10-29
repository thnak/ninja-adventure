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
	var grass:Array[Vector2i] = []
	for x in range(x0, x1 + 1):
		for y in range(y0, y1 + 1):
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


func add_env(center_point:Vector2, size:Vector2) -> void:
	var shape := ENV_SHAPE.new() as CollisionShape2D
	shape.shape = RectangleShape2D.new()
	shape.shape.size = size
	shape.position = center_point
	shape.resource_environment = ENV_NEUTRAL
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
