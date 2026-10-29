extends CanvasLayer
class_name Autosave
## Tự lưu theo timer. Chỉ lưu khi an toàn (không đang chuyển cảnh, không hội thoại/bản đồ làm tạm dừng game);
## nếu chưa an toàn thì thử lại sau `retry_sec`. Lưu thêm khi người chơi đóng cửa sổ game.


signal saved

@export var interval_sec := 180.0
@export var retry_sec := 5.0

var world:World
var timer:Timer
var indicator:Label


func _ready() -> void:
	layer = 25
	process_mode = Node.PROCESS_MODE_ALWAYS
	world = get_parent()
	timer = Timer.new()
	timer.one_shot = true
	timer.timeout.connect(on_timeout)
	add_child(timer)
	timer.start(interval_sec)
	indicator = Label.new()
	indicator.text = "Đang lưu…"
	indicator.add_theme_font_size_override("font_size",22)
	indicator.anchor_left = 1.0
	indicator.anchor_right = 1.0
	indicator.offset_left = -200
	indicator.offset_top = 14
	indicator.modulate.a = 0.0
	add_child(indicator)


func can_save() -> bool:
	return world.map != null and !world.travelling and !get_tree().paused


func on_timeout() -> void:
	if can_save():
		save_now()
		timer.start(interval_sec)
	else:
		timer.start(retry_sec)


func save_now() -> bool:
	var ok := SaveGame.write(world)
	if ok:
		saved.emit()
		var tween := create_tween()
		indicator.modulate.a = 1.0
		tween.tween_interval(0.8)
		tween.tween_property(indicator,"modulate:a",0.0,0.5)
	return ok


func _notification(what:int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST and world and world.map and !world.travelling:
		SaveGame.write(world)
