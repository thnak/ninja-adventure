# Biến thể đổi màu: măng băng, cầu băng, cầu độc, tử lôi, phi tiêu băng, kunai lửa, bão tuyết.
# Chạy: tools\test_interact.ps1 -Test skills3   (cần cửa sổ, không headless)
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
	root.get_viewport().get_texture().get_image().save_png("%s/skill3_%s.png" % [out_dir, name])


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


func nodes_of(world: World, script: Script) -> Array:
	return world.map.find_children("*", "", true, false).filter(func(n): return n.get_script() == script)


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
	check(caster.known.size() == 26, "26 skills known (%d)" % caster.known.size())
	var base := player.global_position
	player.sprite.direction = Vector2.RIGHT

	# Măng băng: bức tường băng chặn đường, đóng băng kẻ đứng tại chỗ trồi lên.
	var victim := dummy(world, base + Vector2(26, 0))
	use(caster, "ice_spikes")
	await frames(24)
	check(victim.has_status(S.FREEZE), "ice spikes freeze whoever stood where they erupt")
	check(nodes_of(world, SkillTerrain).size() >= 2, "ice spikes stay as blocking terrain (%d)" % nodes_of(world, SkillTerrain).size())
	await shot("ice_spikes")
	for t in nodes_of(world, SkillTerrain):
		t.step.lifetime = 0.2
	await frames(80)
	clear()

	# Cầu băng: bay có đuôi xanh, nổ đóng băng.
	victim = dummy(world, base + Vector2(70, 0))
	use(caster, "frost_ball")
	await frames(14)
	await shot("frost_ball_flight")
	await frames(26)
	await shot("frost_ball_hit")
	check(victim.has_status(S.FREEZE), "the frost ball freezes what it hits")
	clear()
	await frames(40)

	# Cầu độc: làm chậm lâu.
	victim = dummy(world, base + Vector2(70, 0))
	use(caster, "poison_ball")
	await frames(14)
	await shot("poison_ball_flight")
	await frames(26)
	await shot("poison_ball_hit")
	check(victim.has_status(S.SLOW) and victim.status_left[S.SLOW] > 3.0, "the poison ball slows for a long time")
	clear()
	await frames(40)

	# Tử lôi: báo trước rồi giáng xuống gây choáng.
	victim = dummy(world, base + Vector2(56, 0))
	use(caster, "violet_thunder")
	await frames(8)
	var warning: Array = world.map.find_children("*", "", true, false).filter(func(n): return n is SkillEffect and n.age < n.step.windup)
	check(!warning.is_empty(), "the violet thunder warns before it strikes")
	await frames(30)
	await shot("violet_thunder")
	check(victim.has_status(S.STUN), "the violet thunder stuns")
	clear()
	await frames(40)

	# Phi tiêu băng và kunai lửa.
	victim = dummy(world, base + Vector2(90, 0))
	use(caster, "ice_shuriken")
	await frames(40)
	check(victim.has_status(S.SLOW), "the ice shuriken slows its target")
	clear()
	await frames(30)
	var target := crate(world, base + Vector2(90, -4))
	use(caster, "fire_kunai")
	await frames(2)
	check(nodes_of(world, Projectile).size() == 3, "fire kunai throws 3")
	await frames(6)
	await shot("fire_kunai")
	await frames(40)
	check(target.life.life < 20.0, "a fire kunai hits and sparks (%s)" % target.life.life)
	clear()
	await frames(30)

	# Bão tuyết: 12 vụ nổ, đóng băng ít nhất một mục tiêu.
	var group: Array[Character] = []
	for gx in range(-2, 3):
		for gy in range(-1, 2):
			group.append(dummy(world, base + Vector2(40, 0) + Vector2(gx, gy) * 30.0))
	use(caster, "blizzard")
	var seen := {}
	var frozen := false
	for i in 270:
		await physics_frame
		for e in world.map.find_children("*", "", true, false):
			if e is SkillEffect:
				seen[e.get_instance_id()] = true
		for g in group:
			if g.has_status(S.FREEZE):
				frozen = true
		if i == 130:
			await shot("blizzard")
	check(seen.size() == 12, "the blizzard drops 12 blasts (%d)" % seen.size())
	check(frozen, "the blizzard freezes someone")

	print("failures=", failures)
	quit(1 if failures else 0)
