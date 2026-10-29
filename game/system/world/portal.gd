extends Area2D
class_name Portal
## Cổng sang khu khác. Khu không biết file của khu đích, chỉ biết `scene_id` và tên SpawnPoint.


const MASK_PLAYER = 5

@export var portal_id:StringName
@export var target_scene:StringName
@export var target_spawn:StringName


func _init() -> void:
	add_to_group("portal")


func _ready() -> void:
	collision_layer = 0
	collision_mask = 0
	set_collision_mask_value(MASK_PLAYER,true)
	body_entered.connect(on_body_entered)


func on_body_entered(body:Node2D) -> void:
	if !body.is_in_group("player"):
		return
	var world:World = get_tree().get_first_node_in_group("world")
	if world:
		world.travel(target_scene,target_spawn)
