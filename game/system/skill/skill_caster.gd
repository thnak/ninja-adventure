## Gắn vào nhân vật: đọc phím, trừ chakra, hồi chiêu, rồi chạy các bước của kỹ năng.
class_name SkillCaster
extends Node

signal chakra_changed
signal slots_changed

const SLOTS := {&"attack": ResourceSkill.Kind.MELEE, &"use_tool": ResourceSkill.Kind.THROW, &"cast": ResourceSkill.Kind.CAST, &"ultimate": ResourceSkill.Kind.ULTIMATE}
const STARTING := ["slash", "spin", "shuriken", "kunai_fan", "fireball", "ice_wall", "thunder", "rock_spikes", "earth_prison", "frost_nova", "water_pillar", "tide", "blade_storm", "meteor_rain", "thunder_storm", "quake",
	"ice_spikes", "frost_ball", "poison_ball", "violet_thunder", "ice_shuriken", "fire_kunai", "blizzard", "rock_bastion", "roll_boulder", "shove_rock"]

@export var max_chakra := 100.0
@export var chakra_regen := 8.0

var chakra := 100.0
var known: Array[ResourceSkill] = []
var current := {}
var last_slot := &"cast"
var ready_at := {}
var clock := 0.0

var character: Character


func _ready() -> void:
	character = get_parent()
	chakra = max_chakra
	for id in STARTING:
		learn(load("res://content/skill/%s.tres" % id))


func learn(skill: ResourceSkill) -> void:
	if known.has(skill):
		return
	known.append(skill)
	for slot in SLOTS:
		if SLOTS[slot] == skill.kind and !current.has(slot):
			current[slot] = skill
	slots_changed.emit()


func skills_of(kind: ResourceSkill.Kind) -> Array[ResourceSkill]:
	return known.filter(func(s: ResourceSkill): return s.kind == kind)


## Đổi sang kỹ năng kế tiếp trong nhóm của phím vừa dùng.
func swap(slot := &"") -> void:
	if slot == &"":
		slot = last_slot
	var list := skills_of(SLOTS[slot])
	if list.size() < 2:
		return
	current[slot] = list[(list.find(current.get(slot)) + 1) % list.size()]
	slots_changed.emit()


func remaining(skill: ResourceSkill) -> float:
	return maxf(0.0, ready_at.get(skill, 0.0) - clock)


func can_use(skill: ResourceSkill) -> bool:
	return skill and remaining(skill) <= 0.0 and chakra >= skill.cost and character.action_left <= 0.0 and character.can_act()


func try_use(slot: StringName) -> bool:
	var skill: ResourceSkill = current.get(slot)
	if !can_use(skill):
		return false
	last_slot = slot
	chakra -= skill.cost
	chakra_changed.emit()
	ready_at[skill] = clock + skill.cooldown
	character.act(skill.anim, skill.lock_sec)
	SkillAudio.play(self, skill.sound)
	var ctx := SkillContext.new()
	ctx.caster = character
	ctx.team = character.team
	ctx.pos = character.global_position + Vector2(0, -4)
	ctx.dir = character.sprite.direction.normalized()
	ctx.parent = character.get_parent()
	for step in skill.steps:
		if step.delay > 0.0:
			run_later(step, ctx)
		else:
			step.run(ctx)
	return true


func run_later(step: SkillStep, ctx: SkillContext) -> void:
	await get_tree().create_timer(step.delay, false).timeout
	# Người dùng có thể đã sang khu khác trong lúc chờ.
	if is_instance_valid(ctx.parent) and ctx.parent.is_inside_tree():
		step.run(ctx)


func _physics_process(delta: float) -> void:
	clock += delta
	if chakra < max_chakra:
		chakra = minf(max_chakra, chakra + chakra_regen * delta)
		chakra_changed.emit()
	# Chỉ người chơi đọc phím; boss hoặc kẻ địch gọi try_use() từ bộ điều khiển của chúng.
	if !character.is_in_group("player"):
		return
	for slot in SLOTS:
		if Input.is_action_just_pressed(slot):
			try_use(slot)
	if Input.is_action_just_pressed(&"skill_swap"):
		swap()
