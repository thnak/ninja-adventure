extends Interactable
class_name Door
## Cửa: mở/đóng, có thể khoá bằng vật phẩm. Hình vẽ tạm, chưa có sprite thật.


@export var key_item:StringName = &""
@export var locked := false

var opened := false

@onready var blocker:CollisionShape2D = $Blocker/Shape


func _ready() -> void:
	super._ready()


func restore_state() -> void:
	var saved = get_state()
	if saved != null:
		opened = saved.opened
		locked = saved.locked
	apply()


func interact(_player:Node2D) -> void:
	if locked:
		if key_item != &"" and GameState.item_count(key_item) > 0:
			locked = false
			say("Bạn mở khoá cửa.")
		else:
			say("Cửa bị khoá.")
			return
	else:
		opened = !opened
	set_state({"opened":opened,"locked":locked})
	apply()


func apply() -> void:
	blocker.set_deferred("disabled",opened)
	prompt = "Đóng" if opened else "Mở"
	queue_redraw()


func _draw() -> void:
	if opened:
		draw_rect(Rect2(-8,-8,16,16),Color("3b2a1a"),false)
	else:
		draw_rect(Rect2(-8,-8,16,16),Color("8a5a2b"))
		draw_rect(Rect2(-8,-8,16,16),Color("3b2a1a"),false)
		draw_circle(Vector2(4,0),1.5,Color("e8c25a") if !locked else Color("c0392b"))
