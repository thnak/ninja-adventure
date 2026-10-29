## Màn hình khi người chơi ngã xuống: tối dần, hiện nguyên nhân, bấm nút tương tác để hồi sinh ở điểm xuất phát.
class_name DeathScreen
extends CanvasLayer

signal respawn_requested

const CAUSES := {
	&"crushed": "Bị đè bẹp",
	&"lethal": "Trúng đòn chí mạng",
	&"slain": "Hết sức chống đỡ",
}
## Chờ một chút trước khi cho hồi sinh để người chơi kịp nhìn cảnh vừa xảy ra.
const WAIT_SEC := 1.2

var veil: ColorRect
var title: Label
var hint: Label
var shown_for := 0.0
var showing := false


func _ready() -> void:
	layer = 50
	process_mode = Node.PROCESS_MODE_ALWAYS
	veil = ColorRect.new()
	veil.set_anchors_preset(Control.PRESET_FULL_RECT)
	veil.color = Color(0.05, 0.0, 0.0, 0.0)
	veil.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(veil)
	var column := VBoxContainer.new()
	column.set_anchors_preset(Control.PRESET_FULL_RECT)
	column.alignment = BoxContainer.ALIGNMENT_CENTER
	column.add_theme_constant_override("separation", 10)
	column.mouse_filter = Control.MOUSE_FILTER_IGNORE
	veil.add_child(column)
	title = label(16)
	hint = label(9)
	column.add_child(title)
	column.add_child(hint)
	visible = false


func label(size: int) -> Label:
	var l := Label.new()
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_outline_color", Color.BLACK)
	l.add_theme_constant_override("outline_size", 4)
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	l.modulate.a = 0.0
	return l


func open(cause: StringName) -> void:
	title.text = "Bạn đã ngã xuống\n%s" % CAUSES.get(cause, "")
	hint.text = "Nhấn nút tương tác để hồi sinh"
	shown_for = 0.0
	showing = true
	visible = true


func close() -> void:
	showing = false
	visible = false
	veil.color.a = 0.0


func _process(delta: float) -> void:
	if !showing:
		return
	shown_for += delta
	veil.color.a = minf(0.65, shown_for * 0.8)
	title.modulate.a = clampf(shown_for * 1.5, 0.0, 1.0)
	hint.modulate.a = clampf((shown_for - WAIT_SEC) * 2.0, 0.0, 1.0)


func _unhandled_input(event: InputEvent) -> void:
	if showing and shown_for >= WAIT_SEC and event.is_action_pressed(&"interact"):
		get_viewport().set_input_as_handled()
		respawn_requested.emit()
