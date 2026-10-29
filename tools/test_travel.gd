# Chạy world.tscn, đưa người chơi vào cổng A->B rồi B->A, kiểm tra đổi khu và chụp ảnh.
# Chạy (cần cửa sổ, không headless): godot --rendering-method gl_compatibility --path game --script ../tools/test_travel.gd -- <thư mục ảnh>
extends SceneTree

var failures := 0
var out_dir := "."


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_dir = args[0]
	WorldGraph.use("res://tests/world_graph_test.json")
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
	root.get_viewport().get_texture().get_image().save_png("%s/%s.png" % [out_dir, name])


func portal_in(world: World) -> Portal:
	for node in get_nodes_in_group("portal"):
		if world.map.is_ancestor_of(node):
			return node
	return null


func run(world: World) -> void:
	await frames(30)
	check(world.map != null and world.map.scene_id == &"test_a", "starts in test_a")
	check(world.player.is_in_group("player"), "player is assigned")
	await shot("1_test_a_start")

	var p := portal_in(world)
	check(p != null, "test_a has a portal")
	world.player.global_position = p.global_position
	await frames(90)
	check(world.map.scene_id == &"test_b", "portal A -> B changes map")
	var spawn := world.map.find_spawn(&"from_a")
	check(world.player.global_position.distance_to(spawn.global_position) < 8.0, "player placed at spawn from_a")
	check(world.player.get_parent() == world.map.actors_root(), "player lives in the new map")
	await shot("2_test_b_arrived")

	await frames(40)
	check(world.map.scene_id == &"test_b", "no bounce back after arriving next to a portal")

	p = portal_in(world)
	world.player.global_position = p.global_position
	await frames(90)
	check(world.map.scene_id == &"test_a", "portal B -> A changes map")
	await shot("3_test_a_back")

	var maps := 0
	for child in world.get_children():
		if child is Map:
			maps += 1
	check(maps == 1, "exactly one map in memory (%d)" % maps)
	print("failures=", failures)
	quit(1 if failures else 0)
