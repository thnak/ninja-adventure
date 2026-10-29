# Sinh các khu thử content/map/test_{a,b,c}.tscn từ system/map/map.tscn.
# Chạy: godot --headless --path game --script ../tools/make_test_maps.gd
extends SceneTree

const MAP = preload("res://system/map/map.tscn")
const PORTAL = preload("res://system/world/portal.gd")
const SPAWN = preload("res://system/world/spawn_point.gd")
const CHEST = preload("res://content/interactable/chest.tscn")
const SIGN = preload("res://content/interactable/sign.tscn")
const DOOR = preload("res://content/interactable/door.tscn")
const ENV_SHAPE = preload("res://system/environment/environment_shape.gd")
const ENV_NEUTRAL = preload("res://content/environment/neutral/environment.tres")

const GRASS_SOURCE = 1
const GRASS_ATLAS = Vector2i(0, 12)
const FLOOR_LAYER = 3

# portals: [đích, vị trí cổng, tên spawn ở khu đích]; spawns: [tên, vị trí]
const AREAS = {
	"test_a": {
		"title": "Khu A", "tint": Color.WHITE,
		"portals": [["test_b", Vector2(136, 0), "from_a"]],
		"spawns": [["start", Vector2(0, 0)], ["from_b", Vector2(112, 0)]],
	},
	"test_b": {
		"title": "Khu B", "tint": Color(0.7, 0.85, 1.0),
		"portals": [["test_a", Vector2(-136, 0), "from_b"], ["test_c", Vector2(136, 0), "from_b"]],
		"spawns": [["from_a", Vector2(-112, 0)], ["from_c", Vector2(112, 0)]],
	},
	"test_c": {
		"title": "Hang C", "tint": Color(0.75, 0.6, 0.9),
		"portals": [["test_b", Vector2(-136, 0), "from_c"]],
		"spawns": [["from_b", Vector2(-112, 0)]],
	},
}


func _initialize() -> void:
	for id in AREAS:
		build(id, AREAS[id])
	quit()


func build(id: String, spec: Dictionary) -> void:
	var root := MAP.instantiate(PackedScene.GEN_EDIT_STATE_MAIN_INHERITED) as Node2D
	root.name = id.to_pascal_case()
	root.set("scene_id", id)
	var tilemap: TileMap = root.get_node("Tilemap")
	tilemap.modulate = spec.tint
	for x in range(-11, 11):
		for y in range(-7, 7):
			tilemap.set_cell(FLOOR_LAYER, Vector2i(x, y), GRASS_SOURCE, GRASS_ATLAS)

	var walls := StaticBody2D.new()
	walls.name = "Walls"
	walls.collision_layer = 1
	add(root, walls)
	for rect in [Rect2(-176, -96, 16, 192), Rect2(160, -96, 16, 192), Rect2(-176, -104, 352, 16), Rect2(-176, 88, 352, 16)]:
		var shape := CollisionShape2D.new()
		shape.shape = RectangleShape2D.new()
		shape.shape.size = rect.size
		shape.position = rect.position + rect.size / 2.0
		add(walls, shape)

	# Phủ cả khu bằng một môi trường: nếu không World giữ màu nền mặc định (nâu) của pack.
	var env_shape := ENV_SHAPE.new() as CollisionShape2D
	env_shape.shape = RectangleShape2D.new()
	env_shape.shape.size = Vector2(352, 208)
	env_shape.resource_environment = ENV_NEUTRAL
	add(root.get_node("EnvironmentArea"), env_shape)

	var label := Label.new()
	label.text = spec.title
	label.position = Vector2(-20, -60)
	add(root, label)

	if id == "test_a":
		place(root, CHEST, "Chest1", Vector2(-80, -30), {"item_id": &"gold_key", "amount": 1})
		place(root, SIGN, "Sign1", Vector2(-40, -30), {"text": "Cửa phía đông dẫn sang Khu B.\nRương này giữ chìa khoá cửa."})
		place(root, DOOR, "Door1", Vector2(40, -30), {"key_item": &"gold_key", "locked": true})

	for p in spec.portals:
		var portal := PORTAL.new() as Area2D
		portal.name = "Portal_" + p[0]
		portal.set("portal_id", p[0])
		portal.set("target_scene", p[0])
		portal.set("target_spawn", p[2])
		portal.position = p[1]
		add(root, portal)
		var pshape := CollisionShape2D.new()
		pshape.shape = RectangleShape2D.new()
		pshape.shape.size = Vector2(16, 64)
		add(portal, pshape)

	for s in spec.spawns:
		var spawn := SPAWN.new() as Marker2D
		spawn.name = s[0]
		spawn.position = s[1]
		add(root, spawn)

	var packed := PackedScene.new()
	var err := packed.pack(root)
	var path := "res://content/map/%s.tscn" % id
	if err == OK:
		err = ResourceSaver.save(packed, path)
	print(path, " err=", err)
	root.free()


func place(root: Node, scene: PackedScene, node_name: String, pos: Vector2, props: Dictionary) -> void:
	var node := scene.instantiate() as Node2D
	node.name = node_name
	node.position = pos
	for key in props:
		node.set(key, props[key])
	add(root, node)


func add(parent: Node, child: Node) -> void:
	parent.add_child(child)
	child.owner = parent.owner if parent.owner else parent
