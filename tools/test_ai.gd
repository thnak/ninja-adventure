# AI kẻ địch: canh gác -> phát hiện -> truy đuổi -> ra chiêu theo tầm; cung thủ giữ khoảng cách; pháp sư dựng khiên khi yếu;
# boss chia pha theo máu (tuyệt kỹ chỉ mở từ pha 2, chuyển pha thì dựng khiên), tận dụng đá môi trường, mất dấu thì quay về
# và hồi đầy máu; bị choáng thì đứng im; chết thì dừng và biến mất.
# Chạy: tools\test_interact.ps1 -Test ai   (cần cửa sổ, không headless)
extends SceneTree

const WIDE = preload("res://content/rock/rock_black_wide.png")

var failures := 0
var out_dir := "."
var spawned: Array[Node] = []


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_dir = args[0]
	seed(7)
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
	root.get_viewport().get_texture().get_image().save_png("%s/ai_%s.png" % [out_dir, name])


func clear() -> void:
	for n in spawned:
		if is_instance_valid(n):
			n.free()
	spawned.clear()


func spawn(world: World, at: Vector2, kind: String) -> Character:
	var e := Enemies.spawn(world.player.get_parent(), at, kind)
	spawned.append(e)
	return e


func brain_of(e: Character) -> Brain:
	return e.get_node("Brain")


func caster_of(e: Character) -> SkillCaster:
	for n in e.get_children():
		if n is SkillCaster:
			return n
	return null


## Ghi lại các kỹ năng một kẻ địch đã dùng.
func record(e: Character) -> Array:
	var log: Array = []
	caster_of(e).used.connect(func(s): log.append(s.id))
	return log


func hit(who: Character, amount: int, lethal := false) -> void:
	var d := ResourceDamage.new()
	d.amount = amount
	d.lethal = lethal
	who.hitbox.take_damage(d, who.global_position + Vector2(-20, 0))


func run(world: World) -> void:
	await frames(60)
	for npc in get_nodes_in_group("npc"):
		npc.wander_radius = 0.0
		npc.global_position += Vector2(0, 600)
	var player := world.player
	player.life.max_life = 999.0
	player.life.life = 999.0
	var base := player.global_position
	player.sprite.direction = Vector2.RIGHT

	# --- Lính cận chiến ---
	var grunt := spawn(world, base + Vector2(260, 0), "grunt")
	var gb := brain_of(grunt)
	var grunt_log := record(grunt)
	await frames(60)
	check(gb.state == Brain.State.IDLE and grunt.global_position.distance_to(base + Vector2(260, 0)) < 2.0, "a guard ignores a player outside its sight range")
	grunt.global_position = base + Vector2(110, 0)
	gb.home = grunt.global_position
	await frames(40)
	check(gb.state == Brain.State.CHASE or gb.state == Brain.State.ACT, "a player inside sight range is noticed (%s)" % Brain.State.keys()[gb.state])
	await shot("grunt_chase")
	await frames(150)
	check(grunt.global_position.distance_to(player.global_position) < 40.0, "the grunt closes in (%.0f px)" % grunt.global_position.distance_to(player.global_position))
	check(grunt_log.has(&"slash") and player.life.life < 999.0, "it slashes once in range (%s, hp %s)" % [grunt_log, player.life.life])
	check(!grunt_log.any(func(id): return id != &"slash"), "it only uses what is in its loadout")
	await shot("grunt_fight")

	# Bị đánh từ xa thì quay lại tìm kẻ đánh.
	clear()
	var far := spawn(world, base + Vector2(330, 0), "grunt")
	await frames(20)
	check(brain_of(far).state == Brain.State.IDLE, "out of sight it stays idle")
	hit(far, 1)
	await frames(5)
	check(brain_of(far).state == Brain.State.CHASE, "being hurt from afar wakes it up and it turns on the player")
	clear()

	# --- Cung thủ ---
	player.life.life = 999.0
	var archer := spawn(world, base + Vector2(120, 0), "archer")
	var archer_log := record(archer)
	await frames(180)
	var gap := archer.global_position.distance_to(player.global_position)
	check(gap > 70.0, "the archer keeps its distance (%.0f px)" % gap)
	check(archer_log.any(func(id): return id == &"shuriken" or id == &"kunai_fan"), "it throws from range (%s)" % [archer_log])
	check(player.life.life < 999.0, "the throws land (hp %s)" % player.life.life)
	await shot("archer")
	archer.global_position = player.global_position + Vector2(36, 0)
	await frames(100)
	var backed := archer.global_position.distance_to(player.global_position)
	check(backed > 62.0, "pressed up close it backs away (%.0f px)" % backed)
	clear()

	# --- Pháp sư: dựng khiên khi yếu ---
	var mage := spawn(world, base + Vector2(100, 0), "mage")
	var mage_log := record(mage)
	mage.life.life = 3.0
	await frames(200)
	check(mage_log.has(&"chakra_ward") and mage.shield > 0.0 or mage_log.has(&"chakra_ward"), "a wounded mage raises its ward (%s)" % [mage_log])
	await shot("mage_ward")
	clear()
	var healthy := spawn(world, base + Vector2(100, 0), "mage")
	var healthy_log := record(healthy)
	await frames(240)
	check(!healthy_log.has(&"chakra_ward"), "a healthy mage does not waste its ward (%s)" % [healthy_log])
	clear()

	# --- Choáng thì đứng im ---
	var stunned := spawn(world, base + Vector2(100, 0), "grunt")
	var stunned_log := record(stunned)
	await frames(20)
	stunned.apply_status(ResourceDamage.Status.FREEZE, 1.5)
	var frozen_at := stunned.global_position
	await frames(60)
	check(stunned_log.is_empty() and stunned.global_position.distance_to(frozen_at) < 2.0, "a frozen enemy neither moves nor casts")
	clear()

	# --- Chết thì dừng và biến mất ---
	var doomed := spawn(world, base + Vector2(100, 0), "grunt")
	brain_of(doomed).corpse_left = 0.4
	await frames(10)
	hit(doomed, 1, true)
	await frames(5)
	check(doomed.dead, "a lethal blow kills an enemy")
	await frames(40)
	check(!is_instance_valid(doomed) or doomed.is_queued_for_deletion(), "the corpse is removed")
	clear()

	# --- Boss: các pha ---
	player.life.life = 999.0
	player.global_position = base
	player.velocity = Vector2.ZERO
	var boss := spawn(world, base + Vector2(150, 0), "boss")
	var bb := brain_of(boss)
	var boss_log := record(boss)
	var phases: Array = []
	bb.phase_changed.connect(func(p): phases.append(p))
	await frames(10)
	check(boss.scale.x > 1.2 and bb.phase_hp == [0.66, 0.33], "the boss is bigger and has three phases")
	await frames(60)
	boss.life.life = 46.0
	await frames(3)
	await shot("boss_bar")
	boss.life.life = 60.0
	await frames(400)
	var ult_ids := [&"roll_boulder", &"shove_rock", &"meteor_rain", &"thunder_storm", &"quake"]
	check(!boss_log.any(func(id): return ult_ids.has(id)), "phase 1: no ultimates yet (%s)" % [boss_log])
	check(boss_log.size() >= 2 and bb.phase == 0, "phase 1 still fights (%d casts)" % boss_log.size())
	check(boss.shield == 0.0, "a healthy boss keeps its shield down")
	boss.life.life = 36.0
	await frames(5)
	check(phases == [1] and bb.phase == 1, "dropping under 66%% opens phase 2 (%s)" % [phases])
	await frames(300)
	check(boss.shield > 0.0 or boss_log.has(&"chakra_ward") or boss_log.has(&"golden_bell"), "changing phase makes it raise a shield (%s)" % [boss_log])
	await shot("boss_phase2")
	await frames(900)
	check(boss_log.any(func(id): return ult_ids.has(id)), "phase 2 unlocks the ultimates (%s)" % [boss_log])
	boss.life.life = 15.0
	await frames(5)
	check(phases == [1, 2], "under 33%% it enters phase 3 (%s)" % [phases])

	# Ở pha 2 boss có thể đã nghiền chết người chơi (đúng thiết kế); hồi sinh để thử tiếp.
	if player.dead:
		player.revive()
	player.global_position = base
	player.velocity = Vector2.ZERO
	# Đang về chỗ canh thì boss bỏ qua mọi mục tiêu; đợi nó về tới nơi rồi mới thấy người chơi.
	for i in 600:
		await frames(1)
		if bb.state == Brain.State.IDLE or bb.state == Brain.State.CHASE:
			break
	boss.life.life = 15.0
	await frames(30)
	check(bb.state == Brain.State.CHASE or bb.state == Brain.State.ACT, "the boss notices the revived player (%s)" % Brain.State.keys()[bb.state])

	# --- Mất dấu thì quay về và hồi máu ---
	var home := bb.home
	var visited: Array = []
	bb.state_changed.connect(func(st): visited.append(st))
	player.global_position = home + Vector2(700, 0)
	await frames(60)
	check(visited.has(Brain.State.RETURN), "it gives up when the player gets out of reach (%s)" % [visited.map(func(st): return Brain.State.keys()[st])])
	# Tường đá do chính boss dựng có thể chắn đường về; có hạn chờ riêng nên đợi tới khi nó về.
	for i in 900:
		await frames(1)
		if bb.state == Brain.State.IDLE:
			break
	check(bb.state == Brain.State.IDLE and boss.global_position.distance_to(home) < 12.0, "it walks back to its post")
	check(boss.life.life == boss.life.max_life, "and recovers its health")
	if player.dead:
		player.revive()
	player.global_position = base
	player.velocity = Vector2.ZERO
	clear()

	# --- Boss tận dụng đá ---
	var smart := spawn(world, base + Vector2(150, 0), "boss")
	var sb := brain_of(smart)
	sb.loadout = PackedStringArray(["shove_rock"])
	sb.phase_hp = []
	sb.ultimate_phase = 0
	sb.setup()
	var smart_log := record(smart)
	await frames(200)
	check(!smart_log.has(&"shove_rock"), "with no rock around the shove is never chosen")
	var rock := Boulder.spawn_slab(player.get_parent(), base + Vector2(95, 0), WIDE)
	spawned.append(rock)
	await frames(120)
	check(smart_log.has(&"shove_rock"), "with a rock between it and the target it shoves the rock")
	await frames(10)
	await shot("boss_shove")
	check(rock.speed() > 40.0 or rock.crushed_targets.size() > 0 or player.dead, "the rock goes sliding toward the player")
	clear()

	print("failures=", failures)
	quit(1 if failures else 0)
