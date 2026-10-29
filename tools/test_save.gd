# Kiểm tra tự lưu: chỉ lưu khi an toàn, nội dung đúng, khởi động lại khôi phục đúng, file hỏng rơi về .bak.
# Chạy: tools\test_interact.ps1 -Test save   (dùng thư mục tạm, không đụng save thật)
extends SceneTree

var failures := 0
var out_dir := "."


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_dir = args[0]
	WorldGraph.use("res://tests/world_graph_test.json")
	SaveGame.path = out_dir + "/test_save.json"
	SaveGame.delete()
	GameState.reset()
	run()


func check(cond: bool, what: String) -> void:
	print(("PASS " if cond else "FAIL "), what)
	if !cond:
		failures += 1


func frames(n: int) -> void:
	for i in n:
		await physics_frame


func wait(sec: float) -> void:
	await create_timer(sec, true).timeout


func new_world() -> World:
	var world: World = (load("res://world.tscn") as PackedScene).instantiate()
	world.autosave_interval = 1.0
	world.autosave_retry = 0.3
	root.add_child(world)
	return world


func run() -> void:
	var world := new_world()
	await frames(30)
	check(world.map.scene_id == &"test_a", "no save yet: starts in test_a")
	check(!SaveGame.has_save(), "no save file on a fresh start")

	# Hộp thoại mở làm game tạm dừng: không được lưu.
	var chest := world.map.get_node("Chest1") as Chest
	chest.interact(world.player)
	var dialog := get_first_node_in_group("dialog") as Dialog
	check(dialog.is_open() and paused, "dialog open pauses the game")
	await wait(1.6)
	check(!SaveGame.has_save(), "autosave waits while the dialog is open")

	dialog.close()
	await wait(1.0)
	check(SaveGame.has_save(), "autosave writes once it is safe")
	var data := SaveGame.read()
	check(int(data.version) == SaveGame.VERSION, "save has a version")
	check(data.player.scene == "test_a", "save records the current area")
	check(int(data.state.items.get("gold_key", 0)) == 1, "save records the item")
	check(data.state.objects.get("test_a/Chest1", false) == true, "save records the chest state")
	check(data.state.visited.has("test_a"), "save records visited areas")

	# Sang khu B, đứng ở một vị trí riêng, chờ lưu lần nữa.
	for node in get_nodes_in_group("portal"):
		if world.map.is_ancestor_of(node) and node.target_scene == &"test_b":
			world.player.global_position = node.global_position
	await frames(90)
	check(world.map.scene_id == &"test_b", "travelled to test_b")
	world.player.global_position = Vector2(20, 10)
	await wait(1.8)
	data = SaveGame.read()
	check(data.player.scene == "test_b" and absf(data.player.pos[0] - 20.0) < 2.0 and absf(data.player.pos[1] - 10.0) < 2.0, "second save has new area and position")
	check(FileAccess.file_exists(SaveGame.backup_path()), "previous save is kept as .bak")

	# Khởi động lại: world mới đọc save.
	root.remove_child(world)
	world.free()
	GameState.reset()
	var world2 := new_world()
	await frames(30)
	check(world2.map.scene_id == &"test_b", "restart resumes in the saved area")
	check(world2.player.global_position.distance_to(Vector2(20, 10)) < 2.0, "restart resumes at the saved position")
	check(GameState.item_count(&"gold_key") == 1, "restart restores the inventory")
	check(GameState.has_visited(&"test_a") and GameState.has_visited(&"test_b"), "restart restores visited areas")
	check(GameState.object_state("test_a/Chest1", false) == true, "restart restores object state")

	# File chính hỏng: rơi về .bak.
	root.remove_child(world2)
	world2.free()
	var broken := FileAccess.open(SaveGame.path, FileAccess.WRITE)
	broken.store_string("{ this is not json")
	broken.close()
	check(!SaveGame.read().is_empty(), "corrupt main save falls back to the .bak copy")

	SaveGame.delete()
	print("failures=", failures)
	quit(1 if failures else 0)
