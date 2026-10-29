# Dựng các kỹ năng mẫu vào game/content/skill/*.tres. Chạy: godot --headless --path game --script ../tools/make_skills.gd
# Muốn thêm kỹ năng: ghép bước (StepStrike / StepProjectile / StepTerrain) ở đây rồi chạy lại.
extends SceneTree

const FX = "res://content/skill/fx/"
const SFX = "res://content/skill/sfx/"


func _initialize() -> void:
	save(skill("slash", "Chém", ResourceSkill.Kind.MELEE, 0.0, 0.3, 0.22, SpriteCharacter.Anim.ATTACK, "slash",
		[strike("slash.png", 4, 16.0, 15.0, 14.0, 2, 60, 1.5, true, 1, 2)]))
	save(skill("spin", "Xoáy", ResourceSkill.Kind.MELEE, 0.0, 1.2, 0.3, SpriteCharacter.Anim.ATTACK, "slash",
		[strike("spin.png", 4, 14.0, 0.0, 24.0, 2, 100, 1.0, false, 1, 2)]))
	save(skill("shuriken", "Phi tiêu", ResourceSkill.Kind.THROW, 0.0, 0.35, 0.2, SpriteCharacter.Anim.ATTACK, "throw",
		[shuriken_bolt()]))
	save(skill("kunai_fan", "Kunai quạt", ResourceSkill.Kind.THROW, 0.0, 0.8, 0.25, SpriteCharacter.Anim.ATTACK, "throw",
		[kunai_bolt()]))
	var boom := strike("explosion.png", 9, 16.0, 0.0, 22.0, 3, 130, 1.0, false, 2, 5)
	boom.start_frame = 2
	boom.ignites = true
	boom.shockwave = true
	boom.debris = true
	boom.damage.launch_sec = 0.5
	boom.damage.launch_height = 10.0
	boom.hit_from = 3
	boom.hit_to = 5
	var fire := bolt("Fireball.png", 4, 150.0, 190.0, 2, 1, 0.0, 0, false, 0.0)
	fire.impact = boom
	fire.hit_radius = 5.0
	fire.trail = Color(1.0, 0.55, 0.15, 0.7)
	fire.embers = Color(1.0, 0.75, 0.25, 1.0)
	save(skill("fireball", "Hỏa cầu", ResourceSkill.Kind.CAST, 20.0, 0.6, 0.35, SpriteCharacter.Anim.ABILITY, "cast", [fire]))
	var wall := StepTerrain.new()
	wall.texture = load(FX + "ice.png")
	wall.hframes = 10
	wall.grow_frames = Vector2i(3, 6)
	wall.break_frames = Vector2i(7, 9)
	wall.strength_per_life = 90.0
	save(skill("ice_wall", "Tường băng", ResourceSkill.Kind.CAST, 25.0, 1.5, 0.4, SpriteCharacter.Anim.ABILITY, "cast", [wall]))
	var bolt_strike := strike("thunder.png", 8, 12.0, 56.0, 14.0, 3, 80, 1.6, false, 2, 5)
	bolt_strike.delay = 0.15
	save(skill("thunder", "Sấm sét", ResourceSkill.Kind.CAST, 30.0, 1.4, 0.45, SpriteCharacter.Anim.ABILITY, "cast", [bolt_strike]))
	# --- Địa hình ---
	var spike_hit := strike("", 3, 12.0, 0.0, 17.0, 2, 90, 1.0, false, 0, 2)
	var spikes := StepTerrain.new()
	spikes.texture = load(FX + "rockspike.png")
	spikes.hframes = 10
	spikes.grow_frames = Vector2i(0, 3)
	spikes.break_frames = Vector2i(8, 9)
	spikes.count = 3
	spikes.spacing = 36.0
	spikes.forward = 26.0
	spikes.size = Vector2(28, 12)
	spikes.max_life = 4.0
	spikes.lifetime = 7.0
	spikes.sprite_offset = Vector2(0, -16)
	spikes.eruption = spike_hit
	spikes.strength_per_life = 160.0
	save(skill("rock_spikes", "Gai đá", ResourceSkill.Kind.CAST, 30.0, 2.0, 0.4, SpriteCharacter.Anim.ABILITY, "cast", [spikes]))

	# --- Khống chế ---
	var prison := strike("rockspike.png", 10, 9.0, 44.0, 24.0, 1, 0, 1.2, false, 0, 2)
	prison.damage.status = ResourceDamage.Status.ROOT
	prison.damage.status_sec = 3.0
	save(skill("earth_prison", "Địa lao", ResourceSkill.Kind.CAST, 25.0, 2.0, 0.4, SpriteCharacter.Anim.ABILITY, "cast", [prison]))
	var nova := strike("ice_burst.png", 9, 12.0, 40.0, 28.0, 1, 0, 1.2, false, 1, 3)
	nova.damage.status = ResourceDamage.Status.FREEZE
	nova.damage.status_sec = 3.0
	nova.extinguishes = true
	save(skill("frost_nova", "Đóng băng", ResourceSkill.Kind.CAST, 25.0, 2.0, 0.4, SpriteCharacter.Anim.ABILITY, "cast", [nova]))
	var pillar := strike("water_pillar.png", 9, 12.0, 40.0, 16.0, 1, 0, 1.0, false, 1, 2)
	pillar.damage.status = ResourceDamage.Status.STUN
	pillar.damage.status_sec = 2.0
	pillar.extinguishes = true
	save(skill("water_pillar", "Thủy trụ", ResourceSkill.Kind.CAST, 25.0, 2.0, 0.4, SpriteCharacter.Anim.ABILITY, "cast", [pillar]))
	var tide := strike("tide.png", 10, 14.0, 40.0, 30.0, 1, 50, 1.0, false, 1, 3)
	tide.damage.status = ResourceDamage.Status.SLOW
	tide.damage.status_sec = 4.0
	tide.extinguishes = true
	save(skill("tide", "Thủy triều", ResourceSkill.Kind.CAST, 20.0, 1.5, 0.35, SpriteCharacter.Anim.ABILITY, "cast", [tide]))

	# --- Tuyệt kỹ diện rộng (dùng được cho boss: chỉ cần gọi try_use) ---
	var blade := []
	for i in 3:
		var spin := strike("spin.png", 4, 14.0, 0.0, 36.0, 3, 150, 2.0, false, 1, 2)
		spin.delay = 0.3 * i
		blade.append(spin)
	save(skill("blade_storm", "Bão kiếm", ResourceSkill.Kind.ULTIMATE, 60.0, 12.0, 1.0, SpriteCharacter.Anim.ATTACK, "slash", blade))
	var meteor := strike("explosion.png", 9, 14.0, 0.0, 26.0, 4, 170, 1.5, false, 3, 5)
	meteor.start_frame = 2
	meteor.windup = 0.9
	meteor.ignites = true
	meteor.shockwave = true
	meteor.smoke = true
	meteor.debris = true
	meteor.damage.launch_sec = 0.6
	meteor.damage.launch_height = 16.0
	var rain := StepScatter.new()
	rain.inner = meteor
	rain.count = 10
	rain.radius = 85.0
	rain.span = 2.5
	rain.center_forward = 40.0
	save(skill("meteor_rain", "Mưa thiên thạch", ResourceSkill.Kind.ULTIMATE, 80.0, 20.0, 0.8, SpriteCharacter.Anim.ABILITY, "cast", [rain]))
	var bolt_hit := strike("thunder.png", 8, 12.0, 0.0, 18.0, 3, 90, 1.8, false, 2, 5)
	bolt_hit.windup = 0.5
	bolt_hit.damage.status = ResourceDamage.Status.STUN
	bolt_hit.damage.status_sec = 1.0
	var storm := StepScatter.new()
	storm.inner = bolt_hit
	storm.count = 12
	storm.radius = 90.0
	storm.span = 3.0
	storm.center_forward = 30.0
	save(skill("thunder_storm", "Bão sấm", ResourceSkill.Kind.ULTIMATE, 80.0, 20.0, 0.8, SpriteCharacter.Anim.ABILITY, "cast", [storm]))
	var quake := strike("explosion.png", 9, 10.0, 0.0, 60.0, 3, 180, 3.2, false, 3, 5)
	quake.start_frame = 2
	quake.windup = 0.6
	quake.shockwave = true
	quake.smoke = true
	quake.debris = true
	quake.damage.launch_sec = 0.8
	quake.damage.launch_height = 26.0
	quake.damage.status = ResourceDamage.Status.STUN
	quake.damage.status_sec = 1.5
	save(skill("quake", "Địa chấn", ResourceSkill.Kind.ULTIMATE, 70.0, 20.0, 0.9, SpriteCharacter.Anim.ABILITY, "cast", [quake]))
	make_variants()
	make_boss_skills()
	make_shield_skills()
	quit()


## Biến thể đổi màu từ tờ hình gốc (ảnh sinh bằng tools/recolor_fx.py): cùng khung hoạt hình, khác màu và khác hiệu quả.
func make_variants() -> void:
	var ice_hit := strike("", 3, 12.0, 0.0, 17.0, 2, 60, 1.0, false, 0, 2)
	ice_hit.damage.status = ResourceDamage.Status.FREEZE
	ice_hit.damage.status_sec = 1.5
	var ice := StepTerrain.new()
	ice.texture = load(FX + "ice_spike_sheet.png")
	ice.hframes = 10
	ice.grow_frames = Vector2i(0, 3)
	ice.break_frames = Vector2i(8, 9)
	ice.count = 3
	ice.spacing = 36.0
	ice.forward = 26.0
	ice.size = Vector2(28, 12)
	ice.max_life = 3.0
	ice.lifetime = 8.0
	ice.sprite_offset = Vector2(0, -16)
	ice.eruption = ice_hit
	ice.strength_per_life = 90.0
	save(skill("ice_spikes", "Măng băng", ResourceSkill.Kind.CAST, 30.0, 2.0, 0.4, SpriteCharacter.Anim.ABILITY, "cast", [ice]))

	var frost_boom := strike("frost_explosion.png", 9, 16.0, 0.0, 22.0, 2, 90, 1.0, false, 3, 5)
	frost_boom.start_frame = 2
	frost_boom.extinguishes = true
	frost_boom.shockwave = true
	frost_boom.smoke = true
	frost_boom.fx_tint = Color(0.6, 0.85, 1.0)
	frost_boom.damage.status = ResourceDamage.Status.FREEZE
	frost_boom.damage.status_sec = 1.2
	var frost := bolt("FrostBall.png", 4, 150.0, 190.0, 1, 1, 0.0, 0, false, 0.0)
	frost.impact = frost_boom
	frost.hit_radius = 5.0
	frost.trail = Color(0.5, 0.85, 1.0, 0.7)
	frost.embers = Color(0.7, 0.95, 1.0, 1.0)
	save(skill("frost_ball", "Cầu băng", ResourceSkill.Kind.CAST, 22.0, 0.7, 0.35, SpriteCharacter.Anim.ABILITY, "cast", [frost]))

	var poison_boom := strike("poison_explosion.png", 9, 14.0, 0.0, 26.0, 2, 50, 1.2, false, 3, 6)
	poison_boom.start_frame = 2
	poison_boom.shockwave = true
	poison_boom.smoke = true
	poison_boom.fx_tint = Color(0.6, 1.0, 0.5)
	poison_boom.damage.status = ResourceDamage.Status.SLOW
	poison_boom.damage.status_sec = 5.0
	var poison := bolt("PoisonBall.png", 4, 120.0, 180.0, 1, 1, 0.0, 0, false, 0.0)
	poison.impact = poison_boom
	poison.hit_radius = 5.0
	poison.trail = Color(0.4, 1.0, 0.3, 0.7)
	poison.embers = Color(0.6, 1.0, 0.3, 1.0)
	save(skill("poison_ball", "Cầu độc", ResourceSkill.Kind.CAST, 22.0, 0.7, 0.35, SpriteCharacter.Anim.ABILITY, "cast", [poison]))

	var violet := strike("violet_thunder.png", 8, 12.0, 56.0, 15.0, 4, 100, 1.8, false, 2, 5)
	violet.windup = 0.3
	violet.damage.status = ResourceDamage.Status.STUN
	violet.damage.status_sec = 1.5
	save(skill("violet_thunder", "Tử lôi", ResourceSkill.Kind.CAST, 35.0, 1.8, 0.5, SpriteCharacter.Anim.ABILITY, "cast", [violet]))

	var ice_star := bolt("IceShuriken.png", 2, 230.0, 180.0, 1, 1, 0.0, 0, false, 720.0)
	ice_star.trail = Color(0.6, 0.9, 1.0, 0.5)
	ice_star.damage.status = ResourceDamage.Status.SLOW
	ice_star.damage.status_sec = 2.5
	save(skill("ice_shuriken", "Phi tiêu băng", ResourceSkill.Kind.THROW, 0.0, 0.4, 0.2, SpriteCharacter.Anim.ATTACK, "throw", [ice_star]))

	var spark := strike("explosion.png", 9, 18.0, 0.0, 12.0, 1, 40, 0.6, false, 3, 5)
	spark.start_frame = 2
	spark.ignites = true
	var hot := bolt("FireKunai.png", 1, 280.0, 150.0, 1, 3, 18.0, 0, true, 0.0)
	hot.impact = spark
	hot.trail = Color(1.0, 0.5, 0.15, 0.6)
	hot.embers = Color(1.0, 0.7, 0.2, 1.0)
	save(skill("fire_kunai", "Kunai lửa", ResourceSkill.Kind.THROW, 0.0, 1.0, 0.25, SpriteCharacter.Anim.ATTACK, "throw", [hot]))

	var flake := strike("frost_explosion.png", 9, 14.0, 0.0, 26.0, 3, 100, 1.5, false, 3, 5)
	flake.start_frame = 2
	flake.windup = 0.9
	flake.extinguishes = true
	flake.shockwave = true
	flake.smoke = true
	flake.fx_tint = Color(0.6, 0.85, 1.0)
	flake.damage.status = ResourceDamage.Status.FREEZE
	flake.damage.status_sec = 1.2
	var snow := StepScatter.new()
	snow.inner = flake
	snow.count = 12
	snow.radius = 90.0
	snow.span = 2.8
	snow.center_forward = 40.0
	save(skill("blizzard", "Bão tuyết", ResourceSkill.Kind.ULTIMATE, 80.0, 20.0, 0.8, SpriteCharacter.Anim.ABILITY, "cast", [snow]))


## Thành đá nhiều lớp (cách chặn đá tảng) và chiêu lăn đá của boss.
func make_boss_skills() -> void:
	var bastion := StepTerrain.new()
	bastion.texture = load(FX + "rockspike.png")
	bastion.hframes = 10
	bastion.grow_frames = Vector2i(0, 3)
	bastion.break_frames = Vector2i(8, 9)
	bastion.count = 3
	bastion.spacing = 36.0
	bastion.rows = 3
	bastion.row_gap = 28.0
	bastion.forward = 30.0
	bastion.size = Vector2(24, 12)
	bastion.max_life = 4.0
	bastion.lifetime = 12.0
	bastion.strength_per_life = 160.0
	bastion.sprite_offset = Vector2(0, -16)
	save(skill("rock_bastion", "Thành đá", ResourceSkill.Kind.CAST, 50.0, 4.0, 0.5, SpriteCharacter.Anim.ABILITY, "cast", [bastion]))
	save(skill("shove_rock", "Đẩy đá", ResourceSkill.Kind.ULTIMATE, 0.0, 6.0, 0.7, SpriteCharacter.Anim.ATTACK, "slash", [StepShove.new()]))
	save(skill("roll_boulder", "Lăn đá", ResourceSkill.Kind.ULTIMATE, 0.0, 8.0, 0.8, SpriteCharacter.Anim.ABILITY, "cast", [StepBoulder.new()]))


## Khiên cá nhân: hấp thụ sát thương rồi vỡ. Kim chung còn thêm thế thủ (không bị đẩy lùi hay khống chế).
func make_shield_skills() -> void:
	var ward := StepShield.new()
	ward.points = 5.0
	ward.seconds = 8.0
	save(skill("chakra_ward", "Hộ thể", ResourceSkill.Kind.CAST, 25.0, 6.0, 0.35, SpriteCharacter.Anim.ABILITY, "cast", [ward]))
	var bell := StepShield.new()
	bell.points = 12.0
	bell.seconds = 10.0
	bell.guarding = true
	bell.tint = Color(1.0, 0.85, 0.4)
	save(skill("golden_bell", "Kim chung", ResourceSkill.Kind.ULTIMATE, 60.0, 18.0, 0.5, SpriteCharacter.Anim.ABILITY, "cast", [bell]))


func skill(id: String, display: String, kind: ResourceSkill.Kind, cost: float, cooldown: float, lock: float, anim: SpriteCharacter.Anim, sound: String, steps: Array) -> ResourceSkill:
	var s := ResourceSkill.new()
	s.id = StringName(id)
	s.display_name = display
	s.kind = kind
	s.cost = cost
	s.cooldown = cooldown
	s.lock_sec = lock
	s.anim = anim
	s.sound = load(SFX + sound + ".wav")
	s.steps.assign(steps)
	return s


func damage(amount: int, push: int) -> ResourceDamage:
	var d := ResourceDamage.new()
	d.amount = amount
	d.push_force = push
	return d


func strike(tex: String, frames: int, fps: float, forward: float, radius: float, amount: int, push: int, fx_scale: float, rotate: bool, from_frame: int, to_frame: int) -> StepStrike:
	var s := StepStrike.new()
	s.texture = load(FX + tex) if !tex.is_empty() else null
	s.hframes = frames
	s.fps = fps
	s.forward = forward
	s.radius = radius
	s.damage = damage(amount, push)
	s.fx_scale = fx_scale
	s.rotate_with_dir = rotate
	s.hit_from = from_frame
	s.hit_to = to_frame
	return s


func bolt(tex: String, frames: int, speed: float, travel: float, amount: int, count: int, spread: float, pierce: int, face: bool, spin: float) -> StepProjectile:
	var s := StepProjectile.new()
	s.texture = load(FX + tex)
	s.hframes = frames
	s.speed = speed
	s.travel = travel
	s.damage = damage(amount, 40)
	s.count = count
	s.spread_deg = spread
	s.pierce = pierce
	s.face_dir = face
	s.spin_deg = spin
	return s


func shuriken_bolt() -> StepProjectile:
	var s := bolt("Shuriken.png", 2, 230.0, 180.0, 1, 1, 0.0, 0, false, 720.0)
	s.trail = Color(0.8, 0.85, 0.95, 0.4)
	return s


func kunai_bolt() -> StepProjectile:
	var s := bolt("Kunai.png", 1, 280.0, 150.0, 1, 3, 18.0, 0, true, 0.0)
	s.trail = Color(1, 1, 1, 0.45)
	return s


# Gợi ý cho AI theo id: [vai trò, tầm tối thiểu, tầm tối đa]. Kỹ năng không có ở đây dùng mặc định theo nhóm.
const AI_HINTS := {
	"ice_wall": [ResourceSkill.AiRole.DEFEND, 30.0, 150.0],
	"rock_bastion": [ResourceSkill.AiRole.DEFEND, 40.0, 220.0],
	"chakra_ward": [ResourceSkill.AiRole.DEFEND, 0.0, 400.0],
	"golden_bell": [ResourceSkill.AiRole.DEFEND, 0.0, 400.0],
	"rock_spikes": [ResourceSkill.AiRole.ATTACK, 30.0, 110.0],
	"earth_prison": [ResourceSkill.AiRole.ATTACK, 20.0, 100.0],
	"ice_spikes": [ResourceSkill.AiRole.ATTACK, 30.0, 110.0],
	"quake": [ResourceSkill.AiRole.ATTACK, 0.0, 70.0],
	"spin": [ResourceSkill.AiRole.ATTACK, 0.0, 28.0],
	"roll_boulder": [ResourceSkill.AiRole.SPECIAL, 90.0, 260.0],
	"shove_rock": [ResourceSkill.AiRole.SPECIAL, 20.0, 220.0],
}


func save(s: ResourceSkill) -> void:
	if AI_HINTS.has(String(s.id)):
		var hint: Array = AI_HINTS[String(s.id)]
		s.ai_role = hint[0]
		s.ai_min = hint[1]
		s.ai_max = hint[2]
	var err := ResourceSaver.save(s, "res://content/skill/%s.tres" % s.id)
	print(s.id, " -> ", error_string(err))
