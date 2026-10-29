class_name ResourceSkill
extends Resource

## Nhóm quyết định phím: MELEE = attack, THROW = use_tool, CAST = cast, ULTIMATE = ultimate (tuyệt kỹ diện rộng).
enum Kind {MELEE, THROW, CAST, ULTIMATE}

@export var id: StringName
@export var display_name := ""
@export var kind := Kind.MELEE
@export var cost := 0.0
@export var cooldown := 0.4
## Thời gian nhân vật đứng yên làm động tác.
@export var lock_sec := 0.25
@export var anim := SpriteCharacter.Anim.ATTACK
@export var sound: AudioStream
@export var steps: Array[SkillStep] = []
## Gợi ý cho AI: ATTACK = đánh khi mục tiêu trong tầm; DEFEND = khiên, tường chắn (dùng khi nguy cấp);
## SPECIAL = cần điều kiện riêng (đá để đẩy, đường lăn đá).
enum AiRole {ATTACK, DEFEND, SPECIAL}
@export var ai_role := AiRole.ATTACK
## Tầm dùng hiệu quả (pixel) tính từ người dùng tới mục tiêu; -1 = lấy mặc định theo nhóm.
@export var ai_min := -1.0
@export var ai_max := -1.0


func ai_range() -> Vector2:
	if ai_max >= 0.0:
		return Vector2(maxf(ai_min, 0.0), ai_max)
	match kind:
		Kind.MELEE:
			return Vector2(0.0, 34.0)
		Kind.THROW:
			return Vector2(40.0, 170.0)
		Kind.CAST:
			return Vector2(30.0, 150.0)
		_:
			return Vector2(0.0, 180.0)
