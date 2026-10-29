extends Area2D
class_name Portal
## Cổng sang khu khác. Khu không biết file của khu đích, chỉ biết `scene_id` và tên SpawnPoint.
## Nếu người chơi bước vào lúc chưa đổi khu được (vừa mới đến, đang đổi khu), cổng tự thử lại cho đến khi được
## hoặc người chơi bước ra.


const MASK_PLAYER = 5

@export var portal_id:StringName
@export var target_scene:StringName
@export var target_spawn:StringName

var waiting:Node2D


func _init() -> void:
	add_to_group("portal")


func _ready() -> void:
	collision_layer = 0
	collision_mask = 0
	set_collision_mask_value(MASK_PLAYER,true)
	body_entered.connect(on_body_entered)
	body_exited.connect(on_body_exited)
	set_physics_process(false)


func on_body_entered(body:Node2D) -> void:
	if !body.is_in_group("player"):
		return
	waiting = body
	if try_travel():
		return
	set_physics_process(true)


func on_body_exited(body:Node2D) -> void:
	if body == waiting:
		waiting = null
		set_physics_process(false)


func _physics_process(_delta:float) -> void:
	if waiting and try_travel():
		waiting = null
		set_physics_process(false)


func try_travel() -> bool:
	var world:World = get_tree().get_first_node_in_group("world")
	return world != null and world.travel(target_scene,target_spawn)
