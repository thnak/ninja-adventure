# Đá tảng của boss: nghiền chết ngay (kể cả có lá chắn), nhiều lớp địa hình khấu trừ năng lượng, vụ nổ phối hợp đẩy và đảo chiều đá.
# Chạy: tools\test_interact.ps1 -Test boulder   (cần cửa sổ, không headless)
extends SceneTree

const NINJA = preload("res://content/character/ninja_blue/ninja_blue.tscn")

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
	root.get_viewport().get_texture().get_image().save_png("%s/boulder_%s.png" % [out_dir, name])


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


func boulder(world: World, at: Vector2, v: Vector2, team: ResourceDamageTeam, friction := 60.0) -> Boulder:
	var b := Boulder.spawn(world.player.get_parent(), at, v, team, 100.0, friction)
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
	check(caster.known.size() == 26, "26 skills known (%d)" % caster.known.size())
	var base := player.global_position
	var enemy: ResourceDamageTeam = load("res://content/team/enemy_team.tres")
	var good: ResourceDamageTeam = player.team
	player.sprite.direction = Vector2.RIGHT

	# Đá yếu hơn ngưỡng nghiền thì không giết; nhanh hơn thì giết ngay.
	var slow_victim := dummy(world, base + Vector2(60, 0), "res://content/team/enemy_team.tres")
	var slow := boulder(world, base + Vector2(100, 0), Vector2(-18, 0), good, 0.0)
	await frames(100)
	check(!slow_victim.dead and slow.crushed_targets.is_empty(), "a boulder crawling below the crush speed kills nobody")
	slow.free()
	var fast := boulder(world, base + Vector2(100, 0), Vector2(-60, 0), good, 0.0)
	await frames(90)
	check(slow_victim.dead and fast.crushed_targets.has(slow_victim), "the same boulder at crush speed kills at once")
	clear()
	await frames(10)

	# Đá của boss lăn qua: kẻ bị đè chết dù có lá chắn, đồng minh của phe đẩy đá thì không sao.
	var boss := dummy(world, base + Vector2(170, 0), "res://content/team/enemy_team.tres")
	boss.sprite.direction = Vector2.LEFT
	var boss_caster := SkillCaster.new()
	boss.add_child(boss_caster)
	await frames(2)
	var shielded := dummy(world, base + Vector2(-10, 0), "res://content/team/player_team.tres")
	shielded.guard = true
	var ally := dummy(world, base + Vector2(120, 0), "res://content/team/enemy_team.tres")
	var causes: Array[StringName] = []
	shielded.died.connect(func(c): causes.append(c))
	player.global_position = base + Vector2(0, -80)
	check(use(boss_caster, "roll_boulder"), "the boss starts the boulder skill")
	await frames(20)
	var lanes := world.map.find_children("*", "Line2D", true, false)
	check(!lanes.is_empty(), "a red lane warns of the boulder's path before it rolls")
	await shot("lane_warning")
	await frames(60)
	var rolling: Boulder
	for n in world.map.find_children("*", "", true, false):
		if n is Boulder:
			rolling = n
	check(rolling != null and rolling.speed() > 100.0, "the boulder is rolling after the windup (%.0f px/s)" % (rolling.speed() if rolling else -1.0))
	check(world.map.find_children("*", "Line2D", true, false).is_empty(), "the warning lane disappears when it rolls")
	await frames(25)
	await shot("rolling")
	await frames(70)
	check(shielded.dead and causes == [&"crushed"], "being run over kills at once: %s" % [causes])
	check(shielded.guard, "even with the guard up (the shield did nothing)")
	check(shielded.sprite.scale.y < 0.5, "the crushed body is flattened")
	check(!ally.dead, "the boss's own side is spared by its boulder")
	await shot("crushed")
	clear()
	rolling = null
	for n in world.map.find_children("*", "", true, false):
		if is_instance_valid(n) and n is Boulder:
			n.free()
	await frames(10)

	# Khấu trừ: 3 lớp thành đá chặn đứng đá tảng, 2 lớp đầu vỡ, lớp cuối chịu lực.
	player.global_position = base
	player.sprite.direction = Vector2.RIGHT
	await frames(5)
	check(use(caster, "rock_bastion"), "the rock bastion can be cast")
	await frames(40)
	var wall := terrain(world)
	var centre_row := wall.filter(func(n): return absf(n.global_position.y - (base.y - 4.0)) < 2.0)
	check(centre_row.size() == 3 and wall.size() >= 7, "the bastion raises 3 layers across the boulder lane (%d blocks, %d in the lane)" % [wall.size(), centre_row.size()])
	await shot("bastion")
	var hit_boulder := boulder(world, base + Vector2(125, 0), Vector2(-190, 0), enemy)
	await frames(8)
	await shot("bastion_impact")
	await frames(160)
	var alive := terrain(world).filter(func(n): return !n.ending)
	var damaged := alive.filter(func(n): return n.life.life < n.step.max_life)
	check(hit_boulder.speed() == 0.0, "three rock layers stop the boulder")
	check(!player.dead and hit_boulder.crushed_targets.is_empty(), "the player behind the bastion is safe")
	check(alive.size() == wall.size() - 2 and damaged.size() == 1, "two blocks shattered, the last one took the load (%d alive, %d damaged)" % [alive.size(), damaged.size()])
	await shot("bastion_held")
	clear()
	for t in terrain(world):
		t.free()
	await frames(10)

	# Chỉ một lớp thì không đủ: đá phá vỡ và vẫn đủ đà nghiền kẻ phía sau.
	var victim := dummy(world, base + Vector2(-30, 0), "res://content/team/player_team.tres")
	player.global_position = base + Vector2(0, -90)
	var one := StepTerrain.new()
	one.texture = load("res://content/skill/fx/rockspike.png")
	one.hframes = 10
	one.size = Vector2(28, 12)
	one.max_life = 4.0
	one.strength_per_life = 160.0
	one.count = 1
	for i in 1:
		SkillTerrain.spawn(player.get_parent(), base + Vector2(40, 0), one)
	await frames(10)
	var single := boulder(world, base + Vector2(120, 0), Vector2(-190, 0), enemy)
	await frames(200)
	check(victim.dead, "one layer is not enough: the boulder breaks it and crushes what is behind")
	check(single.speed() == 0.0 or single.speed() < 190.0, "the boulder lost speed breaking the layer")
	clear()
	for t in terrain(world):
		t.free()
	await frames(10)

	# Vụ nổ phối hợp: 2 vụ chưa đảo chiều, 3 vụ đảo chiều và đổi phe; 5 vụ ném đá ngược về phía boss.
	var blast := ResourceDamage.new()
	blast.push_force = 130
	var b := boulder(world, base + Vector2(100, 0), Vector2(-190, 0), enemy, 0.0)
	for i in 2:
		b.blast(blast, good, b.global_position + Vector2(-5, 0))
	check(b.velocity.x < 0.0 and b.owner_team == good, "two blasts slow it but it still rolls toward the caster (vx=%.0f)" % b.velocity.x)
	b.blast(blast, good, b.global_position + Vector2(-5, 0))
	check(b.velocity.x > 0.0, "a third blast reverses it (vx=%.0f)" % b.velocity.x)
	check(b.owner_team == good, "once it is turned around, the boulder belongs to the side that pushed it")
	clear()
	var enemy_boss := dummy(world, base + Vector2(125, 0), "res://content/team/enemy_team.tres")
	var thrown := boulder(world, base + Vector2(85, 0), Vector2(-190, 0), enemy, 0.0)
	for i in 5:
		thrown.blast(blast, good, thrown.global_position + Vector2(-5, 0))
	await frames(120)
	check(enemy_boss.dead, "combined blasts send the boulder back and it crushes the boss")
	clear()
	await frames(10)

	# Vụ nổ thật: hỏa cầu đẩy đá đang nằm yên.
	player.global_position = base
	player.sprite.direction = Vector2.RIGHT
	await frames(5)
	var still := boulder(world, base + Vector2(60, 0), Vector2.ZERO, enemy)
	var x0 := still.global_position.x
	use(caster, "fireball")
	await frames(60)
	check(still.global_position.x > x0 + 10.0, "a real fireball shoves a resting boulder (%.1f px)" % (still.global_position.x - x0))
	clear()
	await frames(10)

	# Cuối cùng: lá chắn không cứu được người chơi khi bị đè.
	player.global_position = base
	player.guard = true
	var died_cause := [&""]
	player.died.connect(func(c): died_cause[0] = c)
	boulder(world, base + Vector2(120, 0), Vector2(-190, 0), enemy)
	await frames(120)
	check(player.dead and died_cause[0] == &"crushed", "the player dies when run over, shield or not")
	check(!player.can_act() and player.is_immobile(), "a dead player cannot act")
	await shot("player_crushed")

	print("failures=", failures)
	quit(1 if failures else 0)
