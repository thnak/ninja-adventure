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
