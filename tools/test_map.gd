# Kiểm tra bản đồ lớn: khám phá dần, hai cấp, chọn vùng, đóng/mở, và chụp ảnh từng bước.
# Chạy: tools\test_interact.ps1 -Test map   (cần cửa sổ, không headless)
extends SceneTree

var failures := 0
var out_dir := "."


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_dir = args[0]
	WorldGraph.use("res://tests/world_graph_test.json")
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


func through_portal(world: World, target: StringName) -> void:
	for node in get_nodes_in_group("portal"):
		if world.map.is_ancestor_of(node) and node.target_scene == target:
			world.player.global_position = node.global_position
	await frames(90)


func run(world: World) -> void:
	await frames(30)
	var map_screen := get_first_node_in_group("map_screen") as MapScreen
	check(GameState.has_visited(&"test_a") and !GameState.has_visited(&"test_b"), "starting area is marked visited")

	await press(&"open_map")
	check(map_screen.is_open() and paused, "open_map opens the map and pauses")
	check(map_screen.items() == [&"test"], "only the region containing known areas is shown")
	await shot("m1_world_start")

	await press(&"interact")
	check(map_screen.level == MapScreen.Level.REGION and map_screen.region_id == &"test", "interact enters the region")
	check(map_screen.items() == [&"test_a", &"test_b"], "region shows visited area and its unvisited neighbour")
	await shot("m2_region_start")

	await press(&"ui_cancel")
	check(map_screen.level == MapScreen.Level.WORLD and map_screen.is_open(), "Esc goes back to the world level")
	await press(&"ui_cancel")
	check(!map_screen.is_open() and !paused, "Esc at world level closes the map and unpauses")

	await through_portal(world, &"test_b")
	check(world.map.scene_id == &"test_b" and GameState.has_visited(&"test_b"), "entering test_b marks it visited")
	await press(&"open_map")
	check(map_screen.items().size() == 2, "special region becomes known after reaching its neighbour")
	await press(&"ui_right")
	check(map_screen.items()[map_screen.selected] == &"cave", "arrow key moves selection to the special region")
	await shot("m3_world_two_regions")

	await press(&"interact")
	check(map_screen.region_id == &"cave" and map_screen.items() == [&"test_c"], "special region lists its own area")
	await shot("m4_region_cave")
	await press(&"open_map")
	check(map_screen.level == MapScreen.Level.WORLD, "open_map goes back one level")
	await press(&"open_map")
	check(!map_screen.is_open() and !paused, "open_map closes from the world level")

	print("failures=", failures)
	quit(1 if failures else 0)
