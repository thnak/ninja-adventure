extends Interactable
class_name Chest
## Rương: mở một lần, cho vật phẩm vào túi đồ. Trạng thái "đã mở" được lưu.


@export var item_id:StringName = &"coin"
@export var amount := 1

@onready var sprite:Sprite2D = $Sprite


func _ready() -> void:
	super._ready()
	if prompt.is_empty():
		prompt = "Mở"


func restore_state() -> void:
	sprite.frame = 1 if get_state(false) else 0


func can_interact(_player:Node2D) -> bool:
	return !get_state(false)


func interact(_player:Node2D) -> void:
	set_state(true)
	sprite.frame = 1
	GameState.add_item(item_id,amount)
	say("Nhận được: %s x%d" % [item_id,amount])
