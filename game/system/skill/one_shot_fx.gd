## Hiệu ứng tờ hình chạy một lần rồi tự xoá (bụi, vòng sóng, khói).
class_name OneShotFx
extends Sprite2D

var fps := 12.0
var first := 0
var clock := 0.0


static func spawn(parent: Node, at: Vector2, sheet: Texture2D, frames: int, speed: float, fx_scale := 1.0, start_frame := 0, tint := Color.WHITE) -> OneShotFx:
	var fx := OneShotFx.new()
	fx.texture = sheet
	fx.hframes = frames
	fx.fps = speed
	fx.first = start_frame
	fx.frame = start_frame
	fx.scale = Vector2.ONE * fx_scale
	fx.modulate = tint
	parent.add_child(fx)
	fx.global_position = at
	return fx


func _process(delta: float) -> void:
	clock += delta
	var next := first + int(clock * fps)
	if next >= hframes:
		queue_free()
		return
	frame = next
