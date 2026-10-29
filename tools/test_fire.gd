# Lửa: bén vào nền/cây/cỏ, cháy lan, để lại dấu cháy xém, nước dập được, trạng thái giữ lại khi rời khu và trong save.
# Chạy: tools\test_interact.ps1 -Test fire   (cần cửa sổ, không headless)
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


func shot(name: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_viewport().get_texture().get_image().save_png("%s/fire_%s.png" % [out_dir, name])


func look_at(world: World, at: Vector2) -> void:
	world.player.global_position = at + Vector2(0, -24)
	for camera in get_nodes_in_group("camera"):
		camera.teleport_to(at)
		camera.set_world_position(at)
	await frames(10)


## Ô NATURE gần người chơi theo điều kiện, tránh các ô trong nhà.
func find_cells(field: FireField, want_grass: bool, count: int, near: Vector2) -> Array[Vector2i]:
	var found: Array[Vector2i] = []
	for layer in [FireField.LAYER_WALL2, FireField.LAYER_WALL]:
		for cell in field.tilemap.get_used_cells(layer):
			if field.tilemap.get_cell_source_id(layer, cell) != FireField.NATURE:
				continue
			var is_grass: bool = field.tilemap.get_cell_atlas_coords(layer, cell).y == FireField.GRASS_ROW
			if is_grass != want_grass or found.has(cell):
				continue
			if field.center_of(cell).distance_to(near) < 400.0:
				found.append(cell)
	found.sort_custom(func(a, b): return field.center_of(a).distance_to(near) < field.center_of(b).distance_to(near))
	return found.slice(0, count)


func run(world: World) -> void:
	await frames(60)
	for npc in get_nodes_in_group("npc"):
		npc.wander_radius = 0.0
		npc.global_position += Vector2(0, 600)
	var player := world.player
	var field := FireField.of(world.map)
	check(field != null and field.get_parent() == world.map, "every loaded map gets a fire field")
	var team: ResourceDamageTeam = player.team

	# Cỏ: bén lửa, cháy hết rồi biến mất và để lại vết xém.
	var grass := find_cells(field, true, 1, player.global_position)
	check(!grass.is_empty(), "the village has grass tufts to burn")
	var g := grass[0]
	var scorch_before := get_nodes_in_group("scorch").size()
	await look_at(world, field.center_of(g))
	field.ignite_cell(g, team)
	await frames(20)
	check(field.burning.has(g) and get_nodes_in_group("fire").size() >= 1, "igniting a grass cell starts a fire")
	await shot("grass_burning")
	await frames(190)
	check(field.burnt.get(g, "") == "grass_tile", "the grass burns out (%s)" % field.burnt.get(g, ""))
	check(field.tilemap.get_cell_source_id(FireField.LAYER_WALL2, g) == -1, "the burnt grass tile is gone")
	check(get_nodes_in_group("scorch").size() > scorch_before, "a scorch mark stays on the ground")
	await shot("grass_burnt")

	# Cây: cháy lâu hơn, lan sang ô kề, để lại lớp cháy đen.
	var trees := find_cells(field, false, 40, player.global_position)
	check(!trees.is_empty(), "the village has trees to burn")
	var heart := trees[0]
	for c in trees:
		var fuel_neighbours := 0
		for d in FireField.NEIGHBOURS:
			if field.fuel_at(c + d).kind == "tree":
				fuel_neighbours += 1
		if fuel_neighbours == 4:
			heart = c
			break
	await look_at(world, field.center_of(heart))
	field.ignite_cell(heart, team)
	await frames(30)
	var big_fire: FireCell = field.burning.get(heart)
	check(big_fire != null and big_fire.fuel_left > 4.0, "a tree burns longer than grass (%.1f s left)" % (big_fire.fuel_left if big_fire else -1.0))
	await frames(150)
	var spread := 0
	for d in FireField.NEIGHBOURS:
		if field.burning.has(heart + d) or field.burnt.has(heart + d):
			spread += 1
	check(spread >= 1, "the fire spreads to neighbouring trees (%d)" % spread)
	await shot("tree_burning")
	await frames(520)
	check(field.burnt.get(heart, "") == "tree", "the tree cell burns out")
	check(get_nodes_in_group("charred").size() >= 1, "burnt trees are charred")
	await shot("tree_charred")

	# Kỹ năng thật: vụ nổ của hỏa cầu bén lửa; nước dập lửa.
	var more := find_cells(field, true, 8, player.global_position)
	more = more.filter(func(c): return !field.burnt.has(c) and !field.burning.has(c))
	var fresh_cell: Vector2i = more[0]
	var fireball: ResourceSkill = load("res://content/skill/fireball.tres")
	var projectile: StepProjectile = fireball.steps[0]; var boom: StepStrike = projectile.impact
	check(boom.ignites, "the fireball explosion is flagged to ignite")
	boom.strike(player.get_parent(), field.center_of(fresh_cell), Vector2.RIGHT, team)
	await frames(30)
	check(field.burning.has(fresh_cell) or field.burnt.has(fresh_cell), "a fireball blast sets the ground alight")
	var tide: ResourceSkill = load("res://content/skill/tide.tres")
	tide.steps[0].strike(player.get_parent(), field.center_of(fresh_cell), Vector2.RIGHT, team)
	await frames(30)
	check(!field.burning.has(fresh_cell), "water puts the fire out")
	check(!field.burnt.has(fresh_cell) or field.tilemap.get_cell_source_id(FireField.LAYER_WALL2, fresh_cell) == -1, "a doused cell keeps its fuel unless it had already burnt")

	# Lửa làm hỏng đồ vật: hộp trong lửa mất máu.
	var crate: Destroyable = (load("res://content/destroyable/crate.tscn") as PackedScene).instantiate()
	player.get_parent().add_child(crate)
	crate.life.max_life = 10.0
	crate.life.life = 10.0
	var bare := Vector2i(0, 9)
	crate.global_position = field.center_of(bare)
	field.ignite_cell(bare, team)
	await frames(90)
	check(crate.life.life < 10.0, "standing in fire hurts (%s)" % crate.life.life)
	crate.free()

	# Giữ trạng thái: rời khu rồi quay lại vẫn thấy cảnh cháy xém.
	var saved: Array = GameState.object_state("village/burnt", [])
	check(saved.size() >= 3 and saved[0] is Array and saved[0].size() == 3, "burn state is stored in the world state (%d cells)" % saved.size())
	var round_trip = JSON.parse_string(JSON.stringify(GameState.objects))
	check(round_trip != null and round_trip["village/burnt"].size() == saved.size(), "the burn state survives a JSON save round trip")
	var burnt_count := field.burnt.size()
	world.load_area(&"forest", &"", Vector2(0, 40))
	await frames(20)
	check(world.map.scene_id == &"forest" and FireField.of(world.map).burnt.is_empty(), "another area has its own, clean fire field")
	world.load_area(&"village", &"", Vector2(0, 144))
	await frames(20)
	var back := FireField.of(world.map)
	check(back != field and back.burnt.size() == burnt_count, "the burnt cells come back after returning (%d of %d)" % [back.burnt.size(), burnt_count])
	check(back.tilemap.get_cell_source_id(FireField.LAYER_WALL2, g) == -1, "the burnt grass stays gone")
	check(get_nodes_in_group("scorch").size() >= burnt_count, "the scorch marks are redrawn")
	await look_at(world, back.center_of(heart))
	await shot("restored")

	print("failures=", failures)
	quit(1 if failures else 0)
