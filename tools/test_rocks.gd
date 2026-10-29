# Đá đen môi trường: boss chọn tảng nặng nhất trong tầm với, đẩy trượt thẳng như viên gạch (không xoay), nghiền chết ngay;
# tảng càng nặng càng cần nhiều lớp địa hình để chặn; vụ nổ cũng đẩy được. Rừng có sẵn đá đen đặt tay.
# Chạy: tools\test_interact.ps1 -Test rocks   (cần cửa sổ, không headless)
extends SceneTree

const NINJA = preload("res://content/character/ninja_blue/ninja_blue.tscn")
const WIDE = preload("res://content/rock/rock_black_wide.png")
const TALL = preload("res://content/rock/rock_black_tall.png")

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
	root.get_viewport().get_texture().get_image().save_png("%s/rocks_%s.png" % [out_dir, name])


func clear() -> void:
	for n in spawned:
		if is_instance_valid(n):
			n.free()
	spawned.clear()


func dummy(world: World, at: Vector2, team_path: String) -> Character:
	var t: Character = NINJA.instantiate()
	t.team = load(team_path)
	world.player.get_parent().add_child(t)
	t.global_position = at
	spawned.append(t)
	return t


func slab(world: World, at: Vector2, texture: Texture2D) -> Boulder:
	var b := Boulder.spawn_slab(world.player.get_parent(), at, texture)
	spawned.append(b)
	return b


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


func terrain(world: World) -> Array:
	return world.map.find_children("*", "", true, false).filter(func(n): return n is SkillTerrain)


func run(world: World) -> void:
	await frames(60)
	for npc in get_nodes_in_group("npc"):
		npc.wander_radius = 0.0
		npc.global_position += Vector2(0, 600)
	var player := world.player
	var caster: SkillCaster
	for n in player.get_children():
		if n is SkillCaster:
			caster = n
	check(caster.known.size() == 28, "28 skills known (%d)" % caster.known.size())
	var base := player.global_position
	var enemy: ResourceDamageTeam = load("res://content/team/enemy_team.tres")
	player.sprite.direction = Vector2.RIGHT

	# Tấm đá: nặng theo diện tích, hộp va chạm chữ nhật, không xoay.
	var wide := slab(world, base + Vector2(-10, 0), WIDE)
	var tall := slab(world, base + Vector2(-10, -45), TALL)
	await frames(3)
	check(!wide.rolls and wide.footprint.x > wide.footprint.y, "an environment rock is a slab with a rectangular footprint (%s)" % wide.footprint)
	check(wide.mass > tall.mass and absf(wide.mass - 126.0) < 2.0, "bigger rock, bigger mass (%.0f vs %.0f)" % [wide.mass, tall.mass])
	check(wide.is_in_group("boulder"), "slabs register as boulders the boss can find")

	# Boss ở gần: chọn tảng nặng nhất, trượt thẳng, nghiền kẻ đứng trước.
	var boss := dummy(world, base + Vector2(-60, 0), "res://content/team/enemy_team.tres")
	boss.sprite.direction = Vector2.RIGHT
	var boss_caster := SkillCaster.new()
	boss.add_child(boss_caster)
	await frames(2)
	var victim := dummy(world, base + Vector2(60, 0), "res://content/team/player_team.tres")
	victim.guard = true
	var friend := dummy(world, base + Vector2(60, -45), "res://content/team/enemy_team.tres")
	# Đứng sau lưng boss (ngoài đường trượt) để máy quay vẫn thấy cảnh.
	player.global_position = base + Vector2(-110, 0)
	boss_caster.current[&"ultimate"] = boss_caster.known.filter(func(s): return s.id == &"shove_rock")[0]
	check(boss_caster.try_use(&"ultimate"), "the boss uses the shove skill")
	await frames(20)
	check(!world.map.find_children("*", "Line2D", true, false).is_empty(), "a lane shows where the rock will slide")
	await shot("lane")
	await frames(40)
	check(wide.speed() > 80.0 and tall.speed() == 0.0, "it shoves the heaviest rock in reach, not the lighter one (wide %.0f, tall %.0f px/s)" % [wide.speed(), tall.speed()])
	check(absf(wide.velocity.y) < 0.01 and wide.owner_team == boss.team, "the rock slides straight and belongs to the boss's side")
	await shot("sliding")
	await frames(25)
	await shot("crushing")
	await frames(45)
	check(wide.sprite.rotation == 0.0, "a slab never rotates")
	check(victim.dead and wide.crushed_targets.has(victim), "the sliding rock crushes the target even through a shield")
	check(!friend.dead and !boss.dead, "the boss's own side is safe")
	await shot("crushed")

	# Không có đá trong tầm với thì chiêu không làm gì.
	clear()
	boss = dummy(world, base + Vector2(-300, 0), "res://content/team/enemy_team.tres")
	boss.sprite.direction = Vector2.RIGHT
	boss_caster = SkillCaster.new()
	boss.add_child(boss_caster)
	await frames(2)
	var far_rock := slab(world, base + Vector2(120, 0), WIDE)
	boss_caster.current[&"ultimate"] = boss_caster.known.filter(func(s): return s.id == &"shove_rock")[0]
	boss_caster.try_use(&"ultimate")
	await frames(70)
	check(far_rock.speed() == 0.0 and world.map.find_children("*", "Line2D", true, false).is_empty(), "with no rock in reach the shove does nothing")
	clear()
	await frames(5)

	# Vật nặng cần nhiều lớp: tấm 126 kg năng lượng ~1878 xuyên 2 lớp, kẹt ở lớp 3.
	player.global_position = base
	player.sprite.direction = Vector2.RIGHT
	await frames(5)
	check(use(caster, "rock_bastion"), "the player raises a bastion")
	await frames(40)
	var heavy := slab(world, base + Vector2(135, 0), WIDE)
	heavy.shove(Vector2(-170, 0), enemy)
	await frames(10)
	await shot("heavy_hits_bastion")
	await frames(150)
	var left := terrain(world).filter(func(n): return !n.ending)
	check(heavy.speed() == 0.0 and !player.dead, "three rock layers still stop a 126-mass slab")
	check(heavy.crushed_targets.is_empty(), "nothing was crushed behind the bastion")
	clear()
	for t in terrain(world):
		t.free()
	await frames(10)

	# Vụ nổ đẩy tấm đá nặng: 1 vụ chỉ làm nó nhúc nhích, trượt thẳng không xoay.
	var dummy_blast := ResourceDamage.new()
	dummy_blast.push_force = 130
	var pushed := slab(world, base + Vector2(60, 0), WIDE)
	pushed.blast(dummy_blast, player.team, pushed.global_position + Vector2(-5, 0))
	check(pushed.velocity.x > 0.0 and pushed.speed() < 70.0, "one blast barely moves a heavy slab (%.0f px/s)" % pushed.speed())
	var light := slab(world, base + Vector2(60, -60), TALL)
	light.blast(dummy_blast, player.team, light.global_position + Vector2(-5, 0))
	check(light.speed() > pushed.speed() * 1.5, "the same blast moves a lighter slab much faster (%.0f vs %.0f)" % [light.speed(), pushed.speed()])
	await frames(40)
	check(pushed.sprite.rotation == 0.0 and light.sprite.rotation == 0.0, "pushed slabs slide without turning")
	# So với viên tròn: lăn thì có xoay.
	var ball := Boulder.spawn(player.get_parent(), base + Vector2(60, 40), Vector2(80, 0), enemy)
	spawned.append(ball)
	await frames(30)
	check(ball.sprite.rotation != 0.0, "a round boulder does turn as it rolls")
	clear()
	await frames(10)

	# Rừng có sẵn đá đen đặt tay.
	world.load_area(&"forest", &"", Vector2(760, 90))
	await frames(40)
	var rocks := get_nodes_in_group("boulder").filter(func(n): return n is Boulder and n.slab_texture != null)
	check(rocks.size() == 3, "the forest glade has 3 black rocks (%d)" % rocks.size())
	check(rocks.all(func(r): return r.speed() == 0.0), "they rest until someone moves them")
	for camera in get_nodes_in_group("camera"):
		camera.teleport_to(rocks[0].global_position)
		camera.set_world_position(rocks[0].global_position)
	await frames(10)
	await shot("forest_rocks")

	print("failures=", failures)
	quit(1 if failures else 0)
