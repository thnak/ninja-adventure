# Cỏ giẫm được: người chơi bước vào thì cỏ biến mất, bắn mảnh, rồi mọc lại; đứng trên cỏ thì không mọc lại.
# Chạy: tools\test_interact.ps1 -Test trample   (cần cửa sổ, không headless)
extends SceneTree

var failures := 0
var out_dir := "."


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


func run(world: World) -> void:
	await frames(60)   # chờ bộ chỉnh màu chuyển xong
	var grasses: Array[Trampleable] = []
	for n in world.map.find_children("Grass_*", "Area2D", true, false):
		grasses.append(n)
	check(grasses.size() > 20, "village has trampleable grass (%d)" % grasses.size())
	var g := grasses[0]
	g.regrow_sec = 0.5
	check(g.sprite.visible and !g.trampled, "grass starts standing")

	# Bước vào: cỏ biến mất và có hạt.
	world.player.global_position = g.global_position + Vector2(0, 20)
	for camera in get_nodes_in_group("camera"):
		camera.teleport_to(g.global_position)
		camera.set_world_position(g.global_position)
	await frames(20)
	world.player.global_position = g.global_position
	await frames(4)
	check(g.trampled and !g.sprite.visible, "stepping on it tramples it")
	var burst := g.find_children("*", "CPUParticles2D", false, false)
	check(burst.size() == 1, "a one-shot burst was spawned")
	for i in 4:
		await frames(3)
		await RenderingServer.frame_post_draw
		root.get_viewport().get_texture().get_image().save_png("%s/trample_burst%d.png" % [out_dir, i])

	# Đứng nguyên trên cỏ: không mọc lại dưới chân.
	await create_timer(1.2).timeout
	check(g.trampled and !g.sprite.visible, "stays flat while the player stands on it")

	# Bước ra: mọc lại; mảnh vỡ đã tự dọn.
	world.player.global_position = g.global_position + Vector2(80, 0)
	await create_timer(1.6).timeout
	check(!g.trampled and g.sprite.visible, "regrows once the player leaves")
	check(g.find_children("*", "CPUParticles2D", false, false).is_empty(), "burst node cleaned up")

	# NPC/vật khác đi qua không giẫm được: chỉ người chơi.
	check(!g.get_collision_mask_value(2), "only the player layer is monitored")

	print("failures=", failures)
	quit(1 if failures else 0)
