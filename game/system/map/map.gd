@icon("../map/icon_map.png")
extends Node2D
class_name Map

## Id của khu, trùng khoá trong content/world_graph.json.
@export var scene_id:StringName

@onready var screen_grid_ref: TextureRect = $ScreenGridRef
@onready var environment_area: EnvironmentArea = %EnvironmentArea


func _ready() -> void:
	screen_grid_ref.visible = false


## Node cha của nhân vật và vật thể (cần y-sort cùng tile).
func actors_root() -> Node2D:
	return $Tilemap


func find_spawn(spawn_id:StringName) -> SpawnPoint:
	for node in get_tree().get_nodes_in_group("spawn_point"):
		if is_ancestor_of(node) and node.name == spawn_id:
			return node
	return null
