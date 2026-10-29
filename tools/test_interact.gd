# Kiểm tra tương tác qua phím bấm thật (InputMap): biển, rương, cửa khoá, và trạng thái giữ lại khi đổi khu.
# Chạy: tools\test_interact.ps1 (cần cửa sổ, không headless)
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


func stand_at(world: World, node: Node2D) -> void:
	world.player.global_position = node.global_position + Vector2(0, 18)
	await frames(10)


func run(world: World) -> void:
	await frames(30)
	var dialog := get_first_node_in_group("dialog") as Dialog
	var chest := world.map.get_node("Chest1") as Chest
	var sign := world.map.get_node("Sign1") as Sign
	var door := world.map.get_node("Door1") as Door
	var interactor := world.player.get_node("Interactor") as Interactor

	# Biển
	await stand_at(world, sign)
	check(interactor.best == sign, "sign is the best candidate when standing next to it")
	check(sign.hint.visible and sign.hint.text.begins_with("[E]"), "hint shows the bound key: '%s'" % sign.hint.text)
	await press(&"interact")
	check(dialog.is_open() and paused, "interact on sign opens dialog and pauses")
	check(dialog.label.text.contains("Khu B"), "dialog shows the sign text")
	await shot("i1_sign_dialog")
	await press(&"interact")
	check(!dialog.is_open() and !paused, "interact again closes dialog and unpauses")

	# Cửa khoá khi chưa có chìa
	await stand_at(world, door)
	await press(&"interact")
	check(dialog.is_open() and dialog.label.text.contains("khoá") and door.locked, "locked door refuses without key")
	await press(&"interact")

	# Rương
	await stand_at(world, chest)
	check(interactor.best == chest, "chest is the best candidate")
	await press(&"pickup")
	check(chest.get_state(false) == null or chest.get_state(false) == false, "pickup key does not open a chest (interact action)")
	await press(&"interact")
	check(chest.get_state(false) == true and chest.sprite.frame == 1, "chest opens")
	check(GameState.item_count(&"gold_key") == 1, "chest gives the key")
	await shot("i2_chest_open")
	await press(&"interact")
	await press(&"interact")
	check(GameState.item_count(&"gold_key") == 1, "opened chest gives nothing twice")

	# Cửa mở khoá rồi mở
	await stand_at(world, door)
	await press(&"interact")
	check(!door.locked and !door.opened, "key unlocks the door")
	await press(&"interact")
	await press(&"interact")
	check(door.opened and door.blocker.disabled, "door opens and stops blocking")

	# Giữ trạng thái khi đổi khu và quay lại
	var portal: Portal
	for node in get_nodes_in_group("portal"):
		if world.map.is_ancestor_of(node):
			portal = node
	world.player.global_position = portal.global_position
	await frames(90)
	check(world.map.scene_id == &"test_b", "travelled to test_b")
	world.player.global_position = (get_nodes_in_group("portal").filter(func(n): return world.map.is_ancestor_of(n))[0] as Node2D).global_position
	await frames(90)
	check(world.map.scene_id == &"test_a", "travelled back to test_a")
	var chest2 := world.map.get_node("Chest1") as Chest
	var door2 := world.map.get_node("Door1") as Door
	check(chest2.sprite.frame == 1 and !chest2.can_interact(world.player), "chest still open after returning")
	check(door2.opened and !door2.locked and door2.blocker.disabled, "door state restored after returning")
	await shot("i3_back_in_a")

	print("failures=", failures)
	quit(1 if failures else 0)
