## Lửa trong một khu: bén theo ô (16 px), cháy lan sang ô có nhiên liệu, và để lại dấu vết lâu dài.
## Nhiên liệu là cây (cháy lâu), bụi cỏ (nhanh), nền trống (chỉ cháy chốc lát vì "dầu" từ vụ nổ).
## Ô đã cháy được ghi vào GameState (khoá "<khu>/burnt") nên rời khu rồi quay lại, hoặc nạp save, vẫn thấy cảnh cháy xém.
class_name FireField
extends Node2D

const FIRE := preload("res://content/skill/fx/fire.png")
const BAMBOO := preload("res://content/skill/fx/bamboo.png")
const NATURE := 7
const LAYER_WALL := 0
const LAYER_WALL2 := 1
const GRASS_ROW := 10
const FUEL_TREE := 6.0
const FUEL_GRASS := 2.0
const BARE_SEC := 2.2
const NEIGHBOURS := [Vector2i.LEFT, Vector2i.RIGHT, Vector2i.UP, Vector2i.DOWN]

static var scorch_texture: ImageTexture
static var char_texture: ImageTexture

var map: Map
var tilemap: TileMap
var burning := {}
## ô -> loại đã cháy ("tree", "grass_tile", "grass_node", "ground").
var burnt := {}
var grass_nodes := {}
var rng := RandomNumberGenerator.new()


static func of(node: Node) -> FireField:
	var current := node
	while current and !current is Map:
		current = current.get_parent()
	if !current:
		return null
	var existing := current.get_node_or_null("FireField")
	if existing:
		return existing
	var field := FireField.new()
	field.name = "FireField"
	current.add_child(field)
	return field


func _ready() -> void:
	map = get_parent()
	tilemap = map.actors_root()
	rng.seed = 91
	for child in tilemap.get_children():
		if child is Trampleable:
			grass_nodes[cell_of(child.global_position)] = child
	for entry in GameState.object_state(state_key(), []):
		restore(Vector2i(int(entry[0]), int(entry[1])), String(entry[2]))


func state_key() -> String:
	return "%s/burnt" % map.scene_id


func cell_of(world_pos: Vector2) -> Vector2i:
	return tilemap.local_to_map(tilemap.to_local(world_pos))


func center_of(cell: Vector2i) -> Vector2:
	return tilemap.to_global(tilemap.map_to_local(cell))


## Loại và lượng nhiên liệu ở một ô; kind rỗng nghĩa là nền trống.
func fuel_at(cell: Vector2i) -> Dictionary:
	if grass_nodes.has(cell) and is_instance_valid(grass_nodes[cell]):
		return {"kind": "grass_node", "fuel": FUEL_GRASS}
	for layer in [LAYER_WALL2, LAYER_WALL]:
		if tilemap.get_cell_source_id(layer, cell) == NATURE:
			if tilemap.get_cell_atlas_coords(layer, cell).y == GRASS_ROW:
				return {"kind": "grass_tile", "fuel": FUEL_GRASS}
			return {"kind": "tree", "fuel": FUEL_TREE}
	return {"kind": "ground", "fuel": BARE_SEC}


func can_spread_to(cell: Vector2i) -> bool:
	return !burning.has(cell) and !burnt.has(cell) and fuel_at(cell).kind != "ground"


## Bén lửa mọi ô trong vòng tròn (ô nền trống chỉ bén ở lõi để khỏi rải lửa khắp nơi).
func ignite(world_pos: Vector2, radius: float, team: ResourceDamageTeam) -> void:
	var centre := cell_of(world_pos)
	var span := ceili(radius / 16.0) + 1
	for dx in range(-span, span + 1):
		for dy in range(-span, span + 1):
			var cell := centre + Vector2i(dx, dy)
			var dist := center_of(cell).distance_to(world_pos)
			if dist > radius:
				continue
			if fuel_at(cell).kind == "ground" and dist > radius * 0.6:
				continue
			ignite_cell(cell, team)


func ignite_cell(cell: Vector2i, team: ResourceDamageTeam) -> void:
	if burning.has(cell) or burnt.has(cell):
		return
	var info := fuel_at(cell)
	var fire := FireCell.new()
	fire.field = self
	fire.cell = cell
	fire.kind = info.kind
	fire.fuel_left = info.fuel
	fire.team = team
	burning[cell] = fire
	tilemap.add_child(fire)
	fire.global_position = center_of(cell)


## Dập lửa trong vòng tròn (nước, băng); ô chưa cháy hết thì còn nguyên nhiên liệu.
func extinguish(world_pos: Vector2, radius: float) -> void:
	for cell in burning.keys():
		if is_instance_valid(burning[cell]) and burning[cell].global_position.distance_to(world_pos) <= radius:
			burning[cell].put_out()
			burning.erase(cell)


## Một ô cháy hết nhiên liệu: để lại dấu vết lâu dài và lưu vào trạng thái thế giới.
func burn_out(cell: Vector2i, kind: String) -> void:
	burning.erase(cell)
	burnt[cell] = kind
	apply_scar(cell, kind, true)
	var saved: Array = GameState.object_state(state_key(), [])
	saved.append([cell.x, cell.y, kind])
	GameState.set_object_state(state_key(), saved)


func restore(cell: Vector2i, kind: String) -> void:
	burnt[cell] = kind
	apply_scar(cell, kind, false)


func apply_scar(cell: Vector2i, kind: String, fresh: bool) -> void:
	var at := center_of(cell)
	var scorch := Sprite2D.new()
	scorch.texture = get_scorch_texture()
	scorch.z_index = -1
	scorch.add_to_group("scorch")
	tilemap.add_child(scorch)
	scorch.global_position = at + Vector2(0, 2)
	match kind:
		"grass_tile":
			tilemap.erase_cell(LAYER_WALL2, cell)
			tilemap.erase_cell(LAYER_WALL, cell)
		"grass_node":
			if grass_nodes.has(cell) and is_instance_valid(grass_nodes[cell]):
				grass_nodes[cell].queue_free()
		"tree":
			var charred := Sprite2D.new()
			charred.texture = get_char_texture()
			charred.add_to_group("charred")
			tilemap.add_child(charred)
			charred.global_position = at + Vector2(0, 0.2)
			if fresh:
				spawn_debris(at)


## Mảnh tre và cành vụn bắn ra khi cây cháy sập.
func spawn_debris(at: Vector2) -> void:
	var shards := CPUParticles2D.new()
	var canvas_material := CanvasItemMaterial.new()
	canvas_material.particles_animation = true
	canvas_material.particles_anim_h_frames = 6
	canvas_material.particles_anim_v_frames = 1
	shards.material = canvas_material
	shards.texture = BAMBOO
	shards.one_shot = true
	shards.explosiveness = 1.0
	shards.amount = 6
	shards.lifetime = 0.8
	shards.direction = Vector2.UP
	shards.spread = 70.0
	shards.gravity = Vector2(0, 260)
	shards.initial_velocity_min = 40.0
	shards.initial_velocity_max = 90.0
	shards.anim_offset_max = 1.0
	shards.finished.connect(shards.queue_free)
	tilemap.add_child(shards)
	shards.global_position = at
	shards.emitting = true


static func get_scorch_texture() -> ImageTexture:
	if scorch_texture:
		return scorch_texture
	var img := Image.create(16, 12, false, Image.FORMAT_RGBA8)
	for y in 12:
		for x in 16:
			var dx := (x - 7.5) / 8.0
			var dy := (y - 5.5) / 6.0
			var alpha := clampf(1.0 - (dx * dx + dy * dy), 0.0, 1.0)
			img.set_pixel(x, y, Color(0.07, 0.05, 0.04, alpha * 0.7))
	scorch_texture = ImageTexture.create_from_image(img)
	return scorch_texture


static func get_char_texture() -> ImageTexture:
	if char_texture:
		return char_texture
	# Muội than loang lổ thay vì khối đặc: điểm ảnh nào tối thì tuỳ ngẫu nhiên cố định.
	var img := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	for y in 16:
		for x in 16:
			var noise := fposmod(sin(x * 12.9898 + y * 78.233) * 43758.5453, 1.0)
			var alpha := 0.0
			if noise > 0.22:
				alpha = 0.3 + 0.3 * noise
			img.set_pixel(x, y, Color(0.06, 0.04, 0.03, alpha))
	char_texture = ImageTexture.create_from_image(img)
	return char_texture
