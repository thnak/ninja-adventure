# Hành trình thật qua đồ thị thật: làng -> đạo quán -> làng -> rừng -> hang -> rừng -> làng, nói chuyện NPC, bản đồ lớn.
# Chạy: tools\test_interact.ps1 -Test journey   (cần cửa sổ, không headless)
extends SceneTree

var failures := 0
var out_dir := "."


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


func press(action: StringName) -> void:
	var ev := InputEventAction.new()
	ev.action = action
	ev.pressed = true
	Input.parse_input_event(ev)
	await frames(3)
	ev = InputEventAction.new()
	ev.action = action
	ev.pressed = false
	Input.parse_input_event(ev)
	await frames(3)


func shot(name: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_viewport().get_texture().get_image().save_png("%s/%s.png" % [out_dir, name])


## Đặt người chơi vào vùng kích hoạt của cổng rồi chờ chuyển cảnh xong.
func go(world: World, point: Vector2) -> void:
	var before := world.map.scene_id
	world.player.global_position = point
	# Chờ khu đổi (tối đa ~10 giây thật), rồi thêm chút thời gian cho fade và camera.
	var deadline := Time.get_ticks_msec() + 10000
	while world.map.scene_id == before and Time.get_ticks_msec() < deadline:
		await physics_frame
	await frames(60)


func near(world: World, point: Vector2, tolerance := 3.0) -> bool:
	return world.player.global_position.distance_to(point) <= tolerance


func run(world: World) -> void:
	await frames(2)
	check(world.map.scene_id == &"village", "start: village")

	# Làng -> đạo quán qua cửa nhà be. Bước vào ngay lúc vừa khởi động (cổng đang chờ hết thời gian chờ) vẫn phải chạy.
	await go(world, Vector2(-64, 0))
	check(world.map.scene_id == &"dojo", "dojo door leads to the dojo")
	check(near(world, Vector2(24, 64)), "arrive inside the dojo at %s" % world.player.global_position)
	await shot("j1_dojo")

	# Nói chuyện với sư phụ.
	world.player.global_position = Vector2(24, -14)
	await frames(10)
	await press(&"interact")
	var dialog := get_first_node_in_group("dialog") as Dialog
	check(dialog.is_open() and dialog.label.text.contains("Sư phụ"), "master speaks: %s" % dialog.label.text.replace("\n", " / ").left(60))
	await shot("j2_master_talk")
	await press(&"interact")
	check(!dialog.is_open(), "dialog closes")

	# Đạo quán -> làng qua khe ở hàng dưới.
	await go(world, Vector2(24, 96))
	check(world.map.scene_id == &"village" and near(world, Vector2(-64, 24)), "exit dojo to the village door at %s" % world.player.global_position)

	# NPC đi lang thang nhưng không rời bán kính.
	var child := world.map.get_node("Tilemap/Child") as Npc
	var home := child.home
	var farthest := 0.0
	var moved := 0.0
	for i in 600:
		await physics_frame
		farthest = maxf(farthest, child.global_position.distance_to(home))
		moved = maxf(moved, child.global_position.distance_to(home))
	check(moved > 2.0, "the child wanders (moved %.1f px)" % moved)
	check(farthest <= child.wander_radius + 20.0, "the child stays near home (%.1f <= %.1f)" % [farthest, child.wander_radius + 20.0])

	# Làng -> rừng qua cổng torii phía đông.
	await go(world, Vector2(480, 40))
	check(world.map.scene_id == &"forest" and near(world, Vector2(-64, 72)), "east gate leads to the forest at %s" % world.player.global_position)
	await shot("j3_forest")

	# Rừng -> hang qua cửa mỏ.
	await go(world, Vector2(1008, 0))
	check(world.map.scene_id == &"cave" and near(world, Vector2(24, 64)), "cave mouth leads into the cave at %s" % world.player.global_position)
	await shot("j4_cave")

	# Hang -> rừng -> làng.
	await go(world, Vector2(24, 96))
	check(world.map.scene_id == &"forest" and near(world, Vector2(1008, 32)), "cave exit returns to the forest at %s" % world.player.global_position)
	await go(world, Vector2(-112, 72))
	check(world.map.scene_id == &"village" and near(world, Vector2(432, 40)), "west gate returns to the village at %s" % world.player.global_position)

	# Bản đồ lớn sau chuyến đi: cả 3 vùng, vùng hang là vùng đặc biệt.
	var map_screen := get_first_node_in_group("map_screen") as MapScreen
	await press(&"open_map")
	check(map_screen.items() == [&"valley", &"forest", &"cave"], "world map lists the three visited regions")
	await shot("j5_world_map")
	await press(&"interact")
	check(map_screen.region_id == &"valley" and map_screen.items() == [&"village", &"dojo"], "valley region lists village and dojo")
	await shot("j6_valley_map")
	await press(&"ui_cancel")
	await press(&"ui_cancel")
	check(!map_screen.is_open() and !paused, "map closes")

	print("failures=", failures)
	quit(1 if failures else 0)
