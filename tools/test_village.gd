# Kiểm tra làng chơi được: nhà chặn đường, lối cửa đi được, đọc biển cửa, biên giữ người chơi.
# Chạy: tools\test_interact.ps1 -Test village   (cần cửa sổ, không headless; dùng đồ thị thật)
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


func tile_center(tx: int, ty: int) -> Vector2:
	return Vector2(tx * 16 + 16, ty * 16 + 16)


func walk(world: World, action: StringName, n: int) -> void:
	Input.action_press(action)
	await frames(n)
	Input.action_release(action)
	await frames(5)


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


func run(world: World) -> void:
	await frames(30)
	check(world.map.scene_id == &"village", "game starts in the village")
	check(world.player.global_position.distance_to(tile_center(-1, 8)) < 2.0, "player starts at the plaza spawn")
	await shot("v2_start")

	# Làng phải khít lưới camera (màn hình (cx, cy) ở (320cx+24, 176cy+16)): 3x2 màn hình = x [-456,504], y [-72,280].
	var tilemap := world.map.get_node("Tilemap") as TileMap
	# Nền cỏ nở ra quanh khu (rìa 2 ô nhìn thấy) (để rìa không lộ khoảng trống); phần còn lại phải khít lưới.
	var used := tilemap.get_used_rect().grow(-2)
	var origin := tilemap.global_position
	var top_left := Vector2(used.position) * 16.0 + origin
	var bottom_right := Vector2(used.end) * 16.0 + origin
	check(top_left == Vector2(-456, -72) and bottom_right == Vector2(504, 280), "tiles exactly fill the camera grid: %s to %s" % [top_left, bottom_right])

	# Tường nhà bà Lan (x -27..-24, y -3..-1, cửa ở x=-26): đứng dưới phần tường bên phải cửa, đi lên.
	world.player.global_position = tile_center(-24, 0)
	await frames(5)
	await walk(world, &"move_up", 60)
	check(world.player.global_position.y > 6.0, "house wall blocks the player (y=%.1f)" % world.player.global_position.y)

	# Cửa: đi lên đến được ô cửa.
	world.player.global_position = tile_center(-26, 0)
	await frames(5)
	await walk(world, &"move_up", 60)
	check(world.player.global_position.y < 4.0, "the doorway is walkable (y=%.1f)" % world.player.global_position.y)

	# Đọc biển cửa.
	world.player.global_position = tile_center(-26, 0)
	await frames(10)
	var interactor := world.player.get_node("Interactor") as Interactor
	print("candidates=", interactor.candidates.map(func(c): return c.name), " best=", interactor.best.name if interactor.best else "none")
	await press(&"interact")
	var dialog := get_first_node_in_group("dialog") as Dialog
	print("open=", dialog.is_open(), " paused=", paused)
	check(dialog.is_open() and dialog.label.text.contains("bà Lan"), "door sign reads: %s" % dialog.label.text.replace("\n", " / "))
	await shot("v3_door_sign")
	await press(&"interact")

	# Thân cây chặn: đứng sát hàng cây phía nam, đi xuống.
	world.player.global_position = tile_center(0, 13)
	await frames(5)
	await walk(world, &"move_down", 90)
	check(world.player.global_position.y < 280.0, "bottom tree line stops the player (y=%.1f)" % world.player.global_position.y)

	# Biên: đi sang trái hết cỡ từ đường chính.
	world.player.global_position = tile_center(0, 1)
	await frames(5)
	await walk(world, &"move_left", 420)
	check(world.player.global_position.x >= -456.0, "left edge keeps the player in (x=%.1f)" % world.player.global_position.x)
	await shot("v4_west_end")

	print("failures=", failures)
	quit(1 if failures else 0)
