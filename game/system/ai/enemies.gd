## Nhà máy tạo kẻ địch: dựng nhân vật phe địch, gắn SkillCaster + Brain theo từng loại.
## Loại mới = thêm một dòng vào KINDS (hình, máu, kỹ năng, tính cách).
class_name Enemies
extends RefCounted

const BODY := preload("res://content/character/ninja_blue/ninja_blue.tscn")
const TEAM := preload("res://content/team/enemy_team.tres")

const KINDS := {
	# Lính cận chiến: xông vào chém, mất dấu thì về.
	"grunt": {"sprite": "res://content/character/samurai_green/samurai_green.png", "hp": 8.0, "speed": 70.0,
		"loadout": ["slash"], "sight": 130.0, "engage": 22.0},
	# Cung thủ: đứng xa phóng phi tiêu, bị áp sát thì lùi.
	"archer": {"sprite": "res://content/character/samurai_blue/sprite.png", "hp": 6.0, "speed": 60.0,
		"loadout": ["shuriken", "kunai_fan"], "sight": 170.0, "engage": 110.0, "keep": true, "flee": 0.25},
	# Pháp sư: cầu lửa, tường băng, dựng khiên khi bị thương.
	"mage": {"sprite": "res://content/character/ninja_blue/sprite.png", "tint": Color(0.85, 0.6, 1.0), "hp": 8.0, "speed": 55.0,
		"loadout": ["fireball", "frost_ball", "ice_wall", "chakra_ward"], "sight": 180.0, "engage": 100.0, "keep": true},
	# Boss: ba pha, tuyệt kỹ mở từ pha 2, tận dụng đá môi trường.
	"boss": {"sprite": "res://content/character/samurai_blue/sprite.png", "tint": Color(1.0, 0.7, 0.7), "scale": 1.3, "hp": 60.0, "speed": 62.0,
		"loadout": ["slash", "spin", "fireball", "rock_spikes", "quake", "rock_bastion", "golden_bell", "chakra_ward",
			"roll_boulder", "shove_rock", "meteor_rain", "thunder_storm"],
		"chakra": 200.0, "regen": 20.0, "sight": 220.0, "leash": 420.0, "engage": 30.0, "phases": [0.66, 0.33], "ultimate_phase": 1, "bar": true},
}


static func spawn(parent: Node, at: Vector2, kind: String) -> Character:
	var spec: Dictionary = KINDS[kind]
	var body: Character = BODY.instantiate()
	body.team = TEAM
	body.max_hp = spec.hp
	body.speed = spec.speed
	parent.add_child(body)
	body.global_position = at
	body.sprite.texture = load(spec.sprite)
	body.sprite.modulate = spec.get("tint", Color.WHITE)
	body.scale = Vector2.ONE * spec.get("scale", 1.0)
	var caster := SkillCaster.new()
	caster.max_chakra = spec.get("chakra", 100.0)
	caster.chakra_regen = spec.get("regen", 8.0)
	body.add_child(caster)
	var brain := Brain.new()
	brain.name = "Brain"
	brain.loadout = PackedStringArray(spec.loadout)
	brain.sight = spec.get("sight", 140.0)
	brain.leash = spec.get("leash", 320.0)
	brain.engage_distance = spec.get("engage", 26.0)
	brain.keep_distance = spec.get("keep", false)
	brain.flee_ratio = spec.get("flee", 0.0)
	brain.ultimate_phase = spec.get("ultimate_phase", 1)
	var phases: Array[float] = []
	phases.assign(spec.get("phases", []))
	brain.phase_hp = phases
	body.add_child(brain)
	var bar := EnemyBar.new()
	bar.brain = brain
	bar.width = 34.0 if spec.get("bar", false) else 22.0
	body.add_child(bar)
	body.add_to_group("enemy")
	return body
