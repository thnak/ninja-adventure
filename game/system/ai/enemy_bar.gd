## Thanh máu nhỏ trên đầu kẻ địch: chỉ hiện khi đã bị thương, kèm vạch khiên và vạch pha của boss.
class_name EnemyBar
extends Node2D

var who: Character
var width := 22.0
var brain: Brain


func _ready() -> void:
	who = get_parent()
	position = Vector2(0, -30)
	z_index = 5
	who.life.life_changed.connect(queue_redraw)
	who.shield_changed.connect(queue_redraw)


func _draw() -> void:
	if who.dead or who.life.life >= who.life.max_life and who.shield <= 0.0:
		return
	var half := width / 2.0
	draw_rect(Rect2(-half - 1.0, -1.0, width + 2.0, 4.0), Color(0, 0, 0, 0.7))
	var ratio := who.life.life / maxf(who.life.max_life, 1.0)
	draw_rect(Rect2(-half, 0.0, width * ratio, 2.0), Color(0.9, 0.25, 0.2))
	if brain:
		for threshold in brain.phase_hp:
			draw_rect(Rect2(-half + width * threshold, -1.0, 1.0, 4.0), Color(1, 1, 1, 0.8))
	if who.shield > 0.0 and who.shield_max > 0.0:
		var tint := Color(1.0, 0.85, 0.4) if who.shield_guards else Color(0.45, 0.8, 1.0)
		draw_rect(Rect2(-half, -3.0, width * who.shield / who.shield_max, 1.0), tint)
