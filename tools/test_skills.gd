# Hệ kỹ năng: chém/xoáy, phi tiêu, hỏa cầu nổ lan, tường băng (chặn, vỡ, hết hạn), chakra và hồi chiêu.
# Chạy: tools\test_interact.ps1 -Test skills   (cần cửa sổ, không headless)
extends SceneTree

const CRATE = preload("res://content/destroyable/crate.tscn")

var failures := 0
var out_dir := "."
var crates: Array[Destroyable] = []


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
	root.get_viewport().get_texture().get_image().save_png("%s/skill_%s.png" % [out_dir, name])


## Hộp mục tiêu nhiều máu để đếm sát thương.
func crate(world: World, at: Vector2, hp := 10.0) -> Destroyable:
	var c: Destroyable = CRATE.instantiate()
	world.player.get_parent().add_child(c)
	c.global_position = at
	c.life.max_life = hp
	c.life.life = hp
	crates.append(c)
	return c


func clear_crates() -> void:
	for c in crates:
		if is_instance_valid(c):
			c.free()
	crates.clear()


func use(caster: SkillCaster, skill_id: String) -> bool:
	for s in caster.known:
		if s.id == StringName(skill_id):
			caster.current[SkillCaster.SLOTS.find_key(s.kind)] = s
			caster.ready_at.clear()
			caster.chakra = caster.max_chakra
			world_player(caster).action_left = 0.0
			return caster.try_use(SkillCaster.SLOTS.find_key(s.kind))
	return false


func world_player(caster: SkillCaster) -> Character:
	return caster.character


func run(world: World) -> void:
	await frames(60)
	# NPC đi lang thang có thể đứng đúng chỗ địa hình mọc lên; dời đi cho kết quả ổn định.
	for npc in get_nodes_in_group("npc"):
		npc.wander_radius = 0.0
		npc.global_position += Vector2(0, 600)
	var player := world.player
	var caster: SkillCaster = player.find_child("SkillCaster", false, false) as SkillCaster
	if !caster:
		for n in player.get_children():
			if n is SkillCaster:
				caster = n
	check(caster != null and caster.known.size() == 26, "caster knows the 26 starter skills")
	check(caster.current[&"attack"].id == &"slash" and caster.current[&"use_tool"].id == &"shuriken" and caster.current[&"cast"].id == &"fireball", "default slots: slash / shuriken / fireball")
	var base := player.global_position
	player.sprite.direction = Vector2.RIGHT

	# Chém: hộp ngay trước mặt mất máu, hộp phía sau thì không.
	var front := crate(world, base + Vector2(18, -4))
	var behind := crate(world, base + Vector2(-18, -4))
	check(use(caster, "slash"), "slash can be used")
	check(player.action_left > 0.0, "the character is locked in the attack animation")
	await shot("slash_0")
	await frames(5)
	await shot("slash_mid")
	await frames(9)
	check(front.life.life < 10.0, "slash hurts what is in front (%s)" % front.life.life)
	check(behind.life.life == 10.0, "slash leaves what is behind alone")
	await shot("slash_1")
	await frames(30)

	clear_crates()
	# Xoáy: trúng cả hai phía.
	var left := crate(world, base + Vector2(-20, -4))
	var right := crate(world, base + Vector2(20, -4))
	use(caster, "spin")
	await frames(6)
	await shot("spin_early")
	await frames(14)
	check(left.life.life < 10.0 and right.life.life < 10.0, "spin hits both sides")
	await shot("spin")
	await frames(30)

	clear_crates()
	# Phi tiêu bay thẳng và trúng hộp xa hơn.
	var far := crate(world, base + Vector2(90, -4))
	use(caster, "shuriken")
	await frames(4)
	await shot("shuriken")
	await frames(40)
	check(far.life.life < 10.0, "shuriken reaches a target 90 px away")
	check(world.map.find_children("*", "", true, false).filter(func(n): return n is Projectile).is_empty(), "shuriken is gone after hitting")

	# Kunai quạt: ba viên.
	clear_crates()
	use(caster, "kunai_fan")
	await frames(2)
	var shots := world.map.find_children("*", "", true, false).filter(func(n): return n is Projectile)
	check(shots.size() == 3, "kunai fan throws 3 (%d)" % shots.size())
	await shot("kunai")
	await frames(60)

	clear_crates()
	# Hỏa cầu nổ lan sang hộp bên cạnh điểm trúng.
	var hit := crate(world, base + Vector2(100, -4))
	var splash := crate(world, base + Vector2(100, 14))
	use(caster, "fireball")
	await frames(8)
	await shot("fireball_flight")
	check(absf(caster.chakra - (caster.max_chakra - 20.0)) < 2.0, "fireball costs 20 chakra (%s)" % caster.chakra)
	await frames(30)
	await shot("fireball")
	await frames(30)
	check(hit.life.life < 10.0, "fireball hits its target")
	check(splash.life.life < 10.0, "the explosion also hurts the neighbour")
	await shot("explosion")
	await frames(30)

	# Sấm sét: giáng xuống trước mặt sau một nhịp trễ.
	clear_crates()
	var struck := crate(world, base + Vector2(56, -4))
	use(caster, "thunder")
	await frames(14)
	await shot("thunder")
	await frames(30)
	check(struck.life.life < 10.0, "thunder strikes the spot ahead (%s)" % struck.life.life)
	await frames(30)

	# Chakra không đủ / chưa hồi chiêu thì không dùng được.
	caster.chakra = 5.0
	caster.ready_at.clear()
	player.action_left = 0.0
	check(!caster.try_use(&"cast"), "not enough chakra blocks a cast")
	caster.chakra = 100.0
	check(caster.try_use(&"cast"), "enough chakra allows it")
	player.action_left = 0.0
	check(!caster.try_use(&"cast"), "cooldown blocks an immediate second cast")
	await frames(60)

	# Đổi kỹ năng niệm bằng skill_swap.
	caster.current[&"cast"] = caster.skills_of(ResourceSkill.Kind.CAST)[0]
	caster.last_slot = &"cast"
	caster.swap()
	check(caster.current[&"cast"].id == &"ice_wall", "swap moves the cast slot to the ice wall")

	clear_crates()
	# Tường băng: 3 khối chặn đường, đạn không xuyên, vỡ khi bị đánh, tự mất khi hết hạn.
	player.global_position = base
	player.sprite.direction = Vector2.RIGHT
	await frames(10)
	use(caster, "ice_wall")
	await frames(30)
	var walls := world.map.find_children("*", "", true, false).filter(func(n): return n is SkillTerrain)
	check(walls.size() == 3, "ice wall raises 3 blocks (%d)" % walls.size())
	await shot("ice_wall")
	var blocked: bool = player.test_move(player.global_transform, Vector2(30, 0))
	check(blocked, "the ice wall blocks movement")
	var behind_wall := crate(world, base + Vector2(60, -4))
	use(caster, "shuriken")
	await frames(60)
	check(behind_wall.life.life == 10.0, "a shuriken cannot pass through the ice wall")
	var centre: SkillTerrain = walls[1]
	var before_hits := centre.life.life
	use(caster, "slash")
	await frames(14)
	check(centre.life.life < before_hits, "a slash damages the wall (%s -> %s)" % [before_hits, centre.life.life])
	await shot("ice_wall_hit")
	await frames(20)
	use(caster, "slash")
	await frames(14)
	use(caster, "slash")
	await frames(20)
	check(!is_instance_valid(centre) or centre.ending, "the wall breaks when its life is gone")
	await shot("ice_wall_break")

	# Hết hạn: các khối còn lại tự tan, va chạm tắt.
	for w in walls:
		if is_instance_valid(w):
			w.step.lifetime = 0.5
	await create_timer(1.5).timeout
	var remaining := walls.filter(func(n): return is_instance_valid(n))
	check(remaining.is_empty(), "the remaining blocks expire (%d left)" % remaining.size())
	check(!player.test_move(player.global_transform, Vector2(30, 0)), "the way is open again")

	print("failures=", failures)
	quit(1 if failures else 0)
