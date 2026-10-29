extends CanvasLayer
class_name Dialog
## Hộp thoại đơn giản ở đáy màn hình. Hiện chữ thì tạm dừng game; bấm `interact` để đóng.


var panel:PanelContainer
var label:Label


func _ready() -> void:
	layer = 20
	process_mode = Node.PROCESS_MODE_ALWAYS
	add_to_group("dialog")
	panel = PanelContainer.new()
	panel.visible = false
	panel.anchor_left = 0.05
	panel.anchor_right = 0.95
	panel.anchor_top = 0.72
	panel.anchor_bottom = 0.95
	label = Label.new()
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.add_theme_font_size_override("font_size",28)
	panel.add_child(label)
	add_child(panel)


func is_open() -> bool:
	return panel.visible


func show_text(text:String) -> void:
	label.text = text
	panel.visible = true
	get_tree().paused = true


func close() -> void:
	panel.visible = false
	get_tree().paused = false


func _unhandled_input(event:InputEvent) -> void:
	if panel.visible and event.is_action_pressed(&"interact"):
		close()
		get_viewport().set_input_as_handled()
