extends CharacterBody2D
class_name PushBlock
## Khối đẩy được (thùng, đá): người chơi đi dồn vào nó một lúc thì nó trượt đúng một ô theo hướng đẩy.
## Không trượt nếu phía trước có tường, cây, người hoặc khối khác. Luôn dừng ở tâm ô nên không kẹt lệch ô.


const CELL = 16.0
const PUSH_HOLD_SEC = 0.25
const SLIDE_SEC = 0.18

var hold := 0.0
var hold_dir := Vector2.ZERO
var pushed_this_frame := false
var sliding := false


func _ready() -> void:
	collision_layer = 1
	collision_mask = 3
	motion_mode = CharacterBody2D.MOTION_MODE_FLOATING


func _physics_process(delta:float) -> void:
	# Không còn bị dồn trong khung này thì thôi tích luỹ.
	if !pushed_this_frame:
		hold = 0.0
		hold_dir = Vector2.ZERO
	pushed_this_frame = false


## Người chơi gọi mỗi khung khi đang đi dồn vào khối theo hướng `dir` (pháp tuyến va chạm, đã đảo dấu).
func nudge(dir:Vector2, delta:float) -> void:
	if sliding:
		return
	var axis := Vector2(signf(dir.x), 0.0) if absf(dir.x) > absf(dir.y) else Vector2(0.0, signf(dir.y))
	if axis != hold_dir:
		hold_dir = axis
		hold = 0.0
	pushed_this_frame = true
	hold += delta
	if hold >= PUSH_HOLD_SEC:
		hold = 0.0
		slide(axis)


func slide(axis:Vector2) -> void:
	if test_move(global_transform, axis * CELL):
		return
	sliding = true
	var target := (global_position + axis * CELL).snapped(Vector2(CELL, CELL) / 2.0)
	var tween := create_tween()
	tween.tween_property(self, "global_position", target, SLIDE_SEC)
	await tween.finished
	sliding = false
