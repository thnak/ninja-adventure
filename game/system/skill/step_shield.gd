## Dựng khiên quanh người dùng: hấp thụ `points` sát thương trong `seconds` giây (xem Character.add_shield).
## `guarding` thêm thế thủ: khi khiên còn, đòn thường cũng không đẩy lùi hay khống chế được. Đòn chí mạng và đá nghiền vẫn xuyên qua.
class_name StepShield
extends SkillStep

@export var points := 6.0
@export var seconds := 8.0
@export var guarding := false
@export var tint := Color(0.45, 0.8, 1.0)


func run(ctx: SkillContext) -> void:
	var who := ctx.caster as Character
	if !who:
		return
	who.add_shield(points, seconds, guarding)
	ShieldAura.attach(who, tint, guarding)
