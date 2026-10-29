## Thanh chakra và bốn ô kỹ năng đang chọn (phím J, K, L, U).
class_name SkillBar
extends Control

const KEYS := {&"attack": "J", &"use_tool": "K", &"cast": "L", &"ultimate": "U"}
const BAR := Vector2(54, 4)

var caster: SkillCaster


func bind(skill_caster: SkillCaster) -> void:
	caster = skill_caster
	caster.chakra_changed.connect(queue_redraw)
	caster.slots_changed.connect(queue_redraw)
	# caster.character chỉ có sau khi caster vào cây, nên lấy từ cha.
	(caster.get_parent() as Character).shield_changed.connect(queue_redraw)
	custom_minimum_size = Vector2(140, 50)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func _process(_delta: float) -> void:
	# Cập nhật thanh hồi chiêu.
	queue_redraw()


func _draw() -> void:
	if !caster:
		return
	draw_rect(Rect2(Vector2.ZERO, BAR + Vector2(2, 2)), Color(0, 0, 0, 0.6))
	draw_rect(Rect2(Vector2.ONE, Vector2(BAR.x * caster.chakra / caster.max_chakra, BAR.y)), Color(0.3, 0.6, 1.0))
	# Thanh khiên (vàng khi có thế thủ) nằm ngay dưới thanh chakra.
	var who := caster.character
	if who.shield > 0.0 and who.shield_max > 0.0:
		var tint := Color(1.0, 0.85, 0.4) if who.shield_guards else Color(0.45, 0.8, 1.0)
		draw_rect(Rect2(Vector2(0, 7), Vector2(BAR.x + 2, 3)), Color(0, 0, 0, 0.6))
		draw_rect(Rect2(Vector2(1, 8), Vector2(BAR.x * who.shield / who.shield_max, 1)), tint)
	var font := ThemeDB.fallback_font
	var y := 16.0
	for slot in KEYS:
		var skill: ResourceSkill = caster.current.get(slot)
		if !skill:
			continue
		var cd := caster.remaining(skill)
		var tint := Color.WHITE if cd <= 0.0 and caster.chakra >= skill.cost else Color(1, 1, 1, 0.45)
		draw_string(font, Vector2(1, y), "%s %s" % [KEYS[slot], skill.display_name], HORIZONTAL_ALIGNMENT_LEFT, -1, 8, tint)
		y += 9.0
