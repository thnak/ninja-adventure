extends Area2D
class_name Interactable
## Vật thể người chơi tương tác được khi đứng gần. Lớp con ghi đè `interact()`.
## Trạng thái lưu qua GameState theo `object_key()` (= scene_id/tên node), nên tên node phải duy nhất trong khu.


## Hành động (InputMap) kích hoạt vật này: interact, pickup hoặc use_tool.
@export var action:StringName = &"interact"
## Chỉ với action use_tool: loại công cụ cần cầm (axe, pickaxe...).
@export var required_tool:StringName = &""
## Chữ hiện cạnh phím gợi ý, ví dụ "Mở".
@export var prompt:String = ""

var hint:Label


func _init() -> void:
	add_to_group("interactable")


func _ready() -> void:
	collision_layer = 0
	collision_mask = 0
	set_collision_mask_value(5,true)
	body_entered.connect(on_body_entered)
	body_exited.connect(on_body_exited)
	hint = Label.new()
	hint.visible = false
	hint.z_index = 20
	hint.position = Vector2(-12,-30)
	hint.add_theme_font_size_override("font_size",8)
	add_child(hint)
	restore_state()


func on_body_entered(body:Node2D) -> void:
	var interactor := body.get_node_or_null("Interactor") as Interactor
	if interactor:
		interactor.add(self)


func on_body_exited(body:Node2D) -> void:
	var interactor := body.get_node_or_null("Interactor") as Interactor
	if interactor:
		interactor.remove(self)


func object_key() -> String:
	var node := get_parent()
	while node and !(node is Map):
		node = node.get_parent()
	return "%s/%s" % [node.scene_id if node else &"?", name]


func get_state(default = null):
	return GameState.object_state(object_key(), default)


func set_state(value) -> void:
	GameState.set_object_state(object_key(), value)


func show_hint(shown:bool) -> void:
	hint.visible = shown
	if shown:
		var events := InputMap.action_get_events(action)
		var key:String = "?"
		for event in events:
			if event is InputEventKey:
				key = event.as_text_physical_keycode()
				break
		hint.text = "[%s] %s" % [key, prompt]


## Đọc lại trạng thái đã lưu khi khu được nạp. Lớp con ghi đè.
func restore_state() -> void:
	pass


func can_interact(_player:Node2D) -> bool:
	return true


## Lớp con ghi đè.
func interact(_player:Node2D) -> void:
	pass


func say(text:String) -> void:
	var dialog := get_tree().get_first_node_in_group("dialog") as Dialog
	if dialog:
		dialog.show_text(text)
