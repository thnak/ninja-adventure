extends Interactable
class_name Sign
## Biển chữ: bấm `interact` để đọc. Hình vẽ tạm, chưa có sprite thật.


@export_multiline var text:String = ""
## Tắt khi biển nằm trên một cửa/vật có sẵn hình (chỉ dùng làm điểm đọc chữ).
@export var show_post := true


func _ready() -> void:
	super._ready()
	if prompt.is_empty():
		prompt = "Đọc"


func interact(_player:Node2D) -> void:
	say(text)


func _draw() -> void:
	if !show_post:
		return
	draw_rect(Rect2(-1,-4,2,8),Color("6b4a2b"))
	draw_rect(Rect2(-7,-10,14,8),Color("c79a5b"))
	draw_rect(Rect2(-7,-10,14,8),Color("3b2a1a"),false)
