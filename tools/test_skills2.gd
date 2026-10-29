# Địa hình mới, khống chế (trói/chậm/choáng/băng), đẩy lùi, tuyệt kỹ diện rộng và việc boss dùng chung hệ kỹ năng.
# Chạy: tools\test_interact.ps1 -Test skills2   (cần cửa sổ, không headless)
extends SceneTree

const CRATE = preload("res://content/destroyable/crate.tscn")
const NINJA = preload("res://content/character/ninja_blue/ninja_blue.tscn")
const S = ResourceDamage.Status

var failures := 0
var out_dir := "."
var spawned: Array[Node] = []


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_dir = args[0]
	GameState.reset()
	var world: World = (load("res://world.tscn") as PackedScene).instantiate()
	world.use_save = false
	root.add_child(world)
	run(world)


func check(cond: bool, what: String) -> void:
	print(("PASS " if cond else "FAIL "), what)
	if !cond:
		failures += 1


func frames(n: int) -> void:
	for i in n:
		await physics_frame


func shot(name: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_viewport().get_texture().get_image().save_png("%s/skill2_%s.png" % [out_dir, name])


func clear() -> void:
	for n in spawned:
		if is_instance_valid(n):
			n.free()
	spawned.clear()


func crate(world: World, at: Vector2) -> Destroyable:
	var c: Destroyable = CRATE.instantiate()
	world.player.get_parent().add_child(c)
	c.global_position = at
	c.life.max_life = 20.0
	c.life.life = 20.0
	spawned.append(c)
	return c


func dummy(world: World, at: Vector2) -> Character:
	var t: Character = NINJA.instantiate()
	t.team = load("res://content/team/enemy_team.tres")
	world.player.get_parent().add_child(t)
	t.global_position = at
	spawned.append(t)
	return t


func use(caster: SkillCaster, skill_id: String) -> bool:
	for s in caster.known:
		if s.id == StringName(skill_id):
			var slot: StringName = SkillCaster.SLOTS.find_key(s.kind)
			caster.current[slot] = s
			caster.ready_at.clear()
			caster.chakra = caster.max_chakra
			caster.character.action_left = 0.0
			return caster.try_use(slot)
	return false


func effects(world: World) -> Array:
	return world.map.find_children("*", "", true, false).filter(func(n): return n is SkillEffect)


## Quãng đường nhân vật đi được trong n khung khi cố đi sang phải.
func walk_distance(t: Character, n: int) -> float:
	var start := t.global_position.x
	t.move_vector = Vector2.RIGHT
	await frames(n)
	t.move_vector = Vector2.ZERO
	await frames(20)
	return t.global_position.x - start


func run(world: World) -> void:
	await frames(60)
	# NPC đi lang thang có thể đứng đúng chỗ địa hình mọc lên; dời đi cho kết quả ổn định.
	for npc in get_nodes_in_group("npc"):
		npc.wander_radius = 0.0
		npc.global_position += Vector2(0, 600)
	var player := world.player
	var caster: SkillCaster
	for n in player.get_children():
		if n is SkillCaster:
			caster = n
	check(caster.known.size() == 28, "28 skills known (%d)" % caster.known.size())
	check(caster.current.has(&"ultimate") and caster.current[&"ultimate"].id == &"blade_storm", "the ultimate slot starts on blade storm")
	var base := player.global_position
	player.sprite.direction = Vector2.RIGHT

	# Đẩy lùi: cầu lửa nổ ngay trên mục tiêu, mục tiêu bị hất ra xa tâm vụ nổ.
	var target := dummy(world, base + Vector2(70, 0))
	var x0 := target.global_position.x
	use(caster, "fireball")
	await frames(10)
	var bolt: Projectile
	for n in world.map.find_children("*", "", true, false):
		if n is Projectile:
			bolt = n
	check(bolt != null and bolt.find_children("*", "CPUParticles2D", false, false).size() == 1, "the fireball trails embers")
	check(get_nodes_in_group("skill_ghost").size() >= 3, "the fireball leaves fading afterimages (%d)" % get_nodes_in_group("skill_ghost").size())
	await shot("fireball_trail")
	# Vụ nổ hất tung: bay lên cao, không ra chiêu được, rơi xuống rồi trượt thêm.
	var peak := 0.0
	var air_frames := 0
	var landed_x := NAN
	var was_air := false
	for i in 90:
		await physics_frame
		peak = minf(peak, target.sprite.position.y)
		if target.air_left > 0.0:
			was_air = true
			air_frames += 1
			if air_frames == 6:
				check(!target.can_act() and target.is_immobile(), "an airborne character cannot act")
				await shot("launch_air")
		elif was_air and is_nan(landed_x):
			landed_x = target.global_position.x
			check(target.sprite.position.y == 0.0, "the sprite is back on the ground after landing")
	check(peak < -6.0, "the blast throws the character into the air (peak %.1f px)" % peak)
	check(air_frames > 15, "airtime lasts a while (%d frames)" % air_frames)
	check(target.global_position.x > landed_x + 2.0, "the character keeps sliding after landing (%.1f px)" % (target.global_position.x - landed_x))
	var pushed := target.global_position.x - x0
	check(pushed > 20.0, "the fireball blast pushes a character away (%.1f px)" % pushed)
	await shot("knockback")
	clear()
	await frames(30)

	# Đóng băng: không đi, không ra chiêu, có màu; hết giờ thì đi lại.
	target = dummy(world, base + Vector2(40, 0))
	use(caster, "frost_nova")
	await frames(24)
	check(target.has_status(S.FREEZE) and !target.can_act(), "frost nova freezes: cannot act")
	check(target.sprite.self_modulate != Color.WHITE, "a frozen character is tinted")
	await shot("freeze")
	var frozen_move := await walk_distance(target, 20)
	check(absf(frozen_move) < 1.0, "a frozen character cannot walk (%.1f px)" % frozen_move)
	target.status_left[S.FREEZE] = 0.05
	await frames(10)
	check(!target.has_status(S.FREEZE) and target.sprite.self_modulate == Color.WHITE, "the freeze wears off and the tint clears")
	var free_move := await walk_distance(target, 20)
	check(free_move > 8.0, "a freed character walks again (%.1f px)" % free_move)
	clear()
	await frames(30)

	# Trói: không đi được nhưng vẫn ra chiêu được.
	target = dummy(world, base + Vector2(44, 0))
	use(caster, "earth_prison")
	await frames(30)
	check(target.has_status(S.ROOT) and target.can_act(), "earth prison roots but still allows acting")
	check(absf(await walk_distance(target, 20)) < 1.0, "a rooted character cannot walk")
	await shot("root")
	clear()
	await frames(40)

	# Làm chậm: đi chậm hơn hẳn lúc bình thường.
	target = dummy(world, base + Vector2(40, 0))
	var normal := await walk_distance(target, 30)
	target.global_position = base + Vector2(40, 0)
	await frames(10)
	use(caster, "tide")
	await frames(20)
	check(target.has_status(S.SLOW), "tide slows")
	var slowed := await walk_distance(target, 30)
	check(slowed < normal * 0.7, "a slowed character covers less ground (%.1f vs %.1f px)" % [slowed, normal])
	await shot("slow")
	clear()
	await frames(30)

	# Choáng: không ra chiêu được.
	target = dummy(world, base + Vector2(40, 0))
	use(caster, "water_pillar")
	await frames(24)
	check(target.has_status(S.STUN) and !target.can_act() and target.is_immobile(), "water pillar stuns")
	await shot("stun")
	clear()
	await frames(60)

	check(player.global_position.distance_to(base) < 3.0, "the player is still where it started before the %s (%s vs %s)" % ["rock spikes", player.global_position, base])
	# Gai đá: địa hình chặn đường, gây sát thương khi trồi lên, có thể phá.
	var spot := crate(world, base + Vector2(26, -4))
	use(caster, "rock_spikes")
	await frames(30)
	var terrain := world.map.find_children("*", "", true, false).filter(func(n): return n is SkillTerrain)
	check(terrain.size() >= 2, "rock spikes raise blocking terrain (%d)" % terrain.size())
	check(spot.life.life < 20.0, "the eruption hurts what stood there (%s)" % spot.life.life)
	await shot("rock_spikes")
	for t in terrain:
		t.step.lifetime = 0.2
	await frames(80)
	clear()

	check(player.global_position.distance_to(base) < 3.0, "the player is still where it started before the %s (%s vs %s)" % ["meteor rain", player.global_position, base])
	# Mưa thiên thạch: 10 vụ nổ lần lượt, mỗi vụ có vòng cảnh báo trước.
	var field: Array[Destroyable] = []
	for gx in range(-3, 4):
		for gy in range(-3, 4):
			field.append(crate(world, base + Vector2(40, 0) + Vector2(gx, gy) * 24.0))
	use(caster, "meteor_rain")
	var seen := {}
	var saw_telegraph := false
	var shot_telegraph := false
	for i in 260:
		await physics_frame
		for e in effects(world):
			seen[e.get_instance_id()] = true
			if e.age < e.step.windup and !e.sprite.visible:
				saw_telegraph = true
				if !shot_telegraph and seen.size() >= 3:
					shot_telegraph = true
					await shot("meteor_telegraph")
		if i == 120:
			await shot("meteor_mid")
	check(seen.size() == 10, "meteor rain drops 10 blasts (%d)" % seen.size())
	check(saw_telegraph, "each blast shows a warning ring before it lands")
	var hurt := field.filter(func(c): return is_instance_valid(c) and c.life.life < 20.0).size()
	check(hurt >= 2, "the blasts hurt several targets (%d)" % hurt)
	clear()
	# Hộp bị hất văng có thể lấn vào người chơi và đẩy người chơi ra; đặt lại cho các phần sau.
	player.global_position = base
	await frames(30)

	check(player.global_position.distance_to(base) < 3.0, "the player is still where it started before the %s (%s vs %s)" % ["thunder storm", player.global_position, base])
	# Bão sấm: 12 tia, gây choáng.
	use(caster, "thunder_storm")
	seen.clear()
	for i in 260:
		await physics_frame
		for e in effects(world):
			seen[e.get_instance_id()] = true
		if i == 100:
			await shot("storm")
	check(seen.size() == 12, "thunder storm drops 12 strikes (%d)" % seen.size())
	await frames(60)

	check(player.global_position.distance_to(base) < 3.0, "the player is still where it started before the %s (%s vs %s)" % ["quake", player.global_position, base])
	# Địa chấn: vùng lớn quanh người dùng, choáng và hất tung mọi thứ gần.
	var near_t := dummy(world, base + Vector2(34, 0))
	var near_x := near_t.global_position.x
	var other := crate(world, base + Vector2(-30, 0))
	use(caster, "quake")
	await frames(20)
	await shot("quake_telegraph")
	await frames(30)
	await shot("quake_peak")
	await frames(30)
	check(near_t.has_status(S.STUN), "the quake stuns what is near")
	check(near_t.global_position.x - near_x > 10.0, "the quake knocks it back (%.1f px)" % (near_t.global_position.x - near_x))
	check(other.life.life < 20.0, "the quake also reaches the other side")
	await shot("quake")
	clear()
	await frames(40)

	# Boss: kẻ địch dùng chung hệ kỹ năng; không đọc phím, đánh trúng người chơi và gây choáng.
	var boss := dummy(world, base + Vector2(30, 0))
	boss.sprite.direction = Vector2.LEFT
	var boss_caster := SkillCaster.new()
	boss.add_child(boss_caster)
	await frames(2)
	var player_hits := [0]
	player.hitbox.damage_received.connect(func(_d, _p): player_hits[0] += 1)
	boss_caster.current[&"ultimate"] = boss_caster.known.filter(func(s): return s.id == &"quake")[0]
	check(boss_caster.try_use(&"ultimate"), "the boss casts the quake")
	await frames(80)
	await shot("boss_quake")
	check(player_hits[0] >= 1, "the boss quake hits the player (%d)" % player_hits[0])
	check(player.has_status(S.STUN), "the player is stunned by the boss")
	check(!boss.has_status(S.STUN), "the boss is not hurt by its own quake")
	player.status_left.clear()
	player.update_status_tint()

	print("failures=", failures)
	quit(1 if failures else 0)
