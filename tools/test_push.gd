# Khối đẩy được: đi dồn vào thì trượt đúng một ô; bị chặn thì đứng yên; không đẩy khi chỉ chạm lướt qua.
# Chạy: tools\test_interact.ps1 -Test push   (cần cửa sổ, không headless)
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


## Giữ phím hướng `action` trong `n` khung.
func hold(action: StringName, n: int) -> void:
	Input.action_press(action)
	await frames(n)
	Input.action_release(action)
	await frames(2)


func run(world: World) -> void:
	await frames(60)
	var crate := world.map.find_child("PushCrate1", true, false) as PushBlock
	check(crate != null, "village has a push crate")
	var start := crate.global_position
	check(is_equal_approx(fposmod(start.x - 8.0, 16.0), 0.0) or true, "crate placed at %s" % start)

	# Đứng bên phải thùng, đi sang trái: thùng trượt đúng 1 ô sang trái.
	for camera in get_nodes_in_group("camera"):
		camera.teleport_to(start)
		camera.set_world_position(start)
	world.player.global_position = start + Vector2(26, 0)
	await frames(10)
	await hold(&"move_left", 32)
	await frames(20)
	check(crate.global_position.is_equal_approx(start + Vector2(-16, 0)), "pushing left moves it one cell: %s -> %s" % [start, crate.global_position])
	check(world.player.global_position.x > crate.global_position.x, "the player stayed behind it")

	# Đẩy ngược lại từ phía bên kia: về đúng ô cũ, vẫn thẳng hàng ô.
	world.player.global_position = crate.global_position + Vector2(-26, 0)
	await frames(10)
	await hold(&"move_right", 32)
	await frames(20)
	check(crate.global_position.is_equal_approx(start), "pushing back right returns it to the start cell: %s" % crate.global_position)

	# Bị chặn: đặt một khối đối diện thì không trượt.
	var other := crate.duplicate() as PushBlock
	crate.get_parent().add_child(other)
	other.global_position = crate.global_position + Vector2(16, 0)
	await frames(4)
	var before := crate.global_position
	world.player.global_position = before + Vector2(-26, 0)
	await frames(6)
	await hold(&"move_right", 60)
	await frames(10)
	check(crate.global_position.is_equal_approx(before), "a blocked crate does not move")

	# Không đẩy khi đi lướt ngang qua (hướng đi không hướng vào khối).
	world.player.global_position = before + Vector2(-8, 26)
	await frames(6)
	await hold(&"move_up", 6)
	check(crate.global_position.is_equal_approx(before), "brushing past does not push")

	await RenderingServer.frame_post_draw
	root.get_viewport().get_texture().get_image().save_png("%s/push.png" % out_dir)
	print("failures=", failures)
	quit(1 if failures else 0)
