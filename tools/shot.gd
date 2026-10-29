# Chụp ảnh một scene ra PNG để kiểm tra bằng mắt. Chạy qua tools/shot.ps1.
# Tham số sau "--": <scene res://...> <out.png> [số frame chờ=90] [cx cy zoom]
# Có cx cy zoom thì thêm Camera2D nhìn vào (cx, cy) với độ phóng `zoom` (để xem cả map lớn).
extends SceneTree

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("usage: -- <scene> <out.png> [frames] [cx cy zoom]")
		quit(2)
		return
	var packed := load(args[0]) as PackedScene
	if packed == null:
		push_error("cannot load scene: " + args[0])
		quit(3)
		return
	root.add_child(packed.instantiate())
	if args.size() >= 6:
		var camera := Camera2D.new()
		camera.position = Vector2(float(args[3]), float(args[4]))
		camera.zoom = Vector2.ONE * float(args[5])
		root.add_child(camera)
	var frames := int(args[2]) if args.size() > 2 else 90
	_capture(args[1], frames)

func _capture(out_path: String, frames: int) -> void:
	for i in frames:
		await process_frame
	await RenderingServer.frame_post_draw
	var img := root.get_viewport().get_texture().get_image()
	var err := img.save_png(out_path)
	print("saved ", out_path, " ", img.get_size(), " err=", err)
	quit(0 if err == OK else 4)
