# Kiểm tra đồ thị khu: cổng có đích, có spawn, có cổng ngược; không có khu mồ côi.
# Chạy: tools\check_world.ps1   (thoát mã 0 nếu sạch, 1 nếu có lỗi)
extends SceneTree

# Chỉ để kiểm tra điểm xuất hiện không nằm trong vùng cổng: scene_id -> [{name, rect}] và scene_id -> {tên spawn: vị trí}.
var rects := {}
var spawn_points := {}


func _initialize() -> void:
	# Đồ thị chọn qua biến môi trường WORLD_GRAPH (PowerShell nuốt dấu "--" nên không truyền bằng đối số).
	var graph := OS.get_environment("WORLD_GRAPH")
	if !graph.is_empty():
		WorldGraph.use(graph)
	print("graph: ", WorldGraph.path)
	var errors: Array[String] = []
	var portals := {}   # scene_id -> [{id, target_scene, target_spawn}]
	var spawns := {}    # scene_id -> [tên]
	var ids := WorldGraph.area_ids()
	if ids.is_empty():
		errors.append("world_graph has no areas")

	for id in ids:
		var path := WorldGraph.scene_path(id)
		var packed := load(path) as PackedScene
		if packed == null:
			errors.append("%s: cannot load %s" % [id, path])
			continue
		var root := packed.instantiate()
		if String(root.get("scene_id")) != id:
			errors.append("%s: Map.scene_id is '%s'" % [id, root.get("scene_id")])
		if !WorldGraph.region_ids().has(String(WorldGraph.region_of(id))):
			errors.append("%s: unknown region '%s'" % [id, WorldGraph.region_of(id)])
		portals[id] = []
		spawns[id] = []
		rects[id] = []
		spawn_points[id] = {}
		_collect(root, id, portals, spawns, errors)
		root.free()

	# Điểm xuất hiện nằm trong (hoặc sát) vùng cổng sẽ đẩy người chơi ngược lại ngay sau khi đến (cổng tự thử lại).
	for id in rects:
		for sp_name in spawn_points[id]:
			for portal in rects[id]:
				if portal.rect.grow(10.0).has_point(spawn_points[id][sp_name]):
					errors.append("%s: spawn '%s' is inside/too close to portal '%s'" % [id, sp_name, portal.name])

	for id in portals:
		for p in portals[id]:
			var target: String = p.target_scene
			if !portals.has(target):
				errors.append("%s/%s: target scene '%s' not in world_graph" % [id, p.id, target])
				continue
			if !spawns[target].has(p.target_spawn):
				errors.append("%s/%s: scene '%s' has no spawn '%s'" % [id, p.id, target, p.target_spawn])
			var has_back := false
			for q in portals[target]:
				if q.target_scene == id:
					has_back = true
			if !has_back:
				errors.append("%s/%s: no portal back from '%s'" % [id, p.id, target])

	# Liên kết trong world_graph phải khớp với cổng thật; bản đồ vẽ theo liên kết.
	var linked := {}
	for pair in WorldGraph.links():
		linked[pair[0] + ">" + pair[1]] = true
		linked[pair[1] + ">" + pair[0]] = true
		for end in pair:
			if !portals.has(end):
				errors.append("link %s: unknown area '%s'" % [pair, end])
	for id in portals:
		for p in portals[id]:
			if !linked.has(id + ">" + p.target_scene):
				errors.append("%s/%s: no link %s-%s in world_graph.links" % [id, p.id, id, p.target_scene])
	for pair in WorldGraph.links():
		for e in [[pair[0], pair[1]], [pair[1], pair[0]]]:
			if portals.has(e[0]) and !portals[e[0]].any(func(q): return q.target_scene == e[1]):
				errors.append("link %s: '%s' has no portal to '%s'" % [pair, e[0], e[1]])
	var seen_pos := {}
	for id in ids:
		var key := "%s@%s" % [WorldGraph.region_of(id), WorldGraph.area_pos(id)]
		if seen_pos.has(key):
			errors.append("%s: map_pos %s duplicates %s" % [id, WorldGraph.area_pos(id), seen_pos[key]])
		seen_pos[key] = id
	for r in WorldGraph.region_ids():
		if WorldGraph.areas_in(StringName(r)).is_empty():
			errors.append("region '%s' has no areas" % r)

	var start := String(WorldGraph.start_scene())
	if !spawns.has(start) or !spawns[start].has(String(WorldGraph.start_spawn())):
		errors.append("start spawn '%s/%s' missing" % [start, WorldGraph.start_spawn()])
	var seen := {start: true}
	var queue := [start]
	while !queue.is_empty():
		var cur: String = queue.pop_front()
		for p in portals.get(cur, []):
			if !seen.has(p.target_scene):
				seen[p.target_scene] = true
				queue.append(p.target_scene)
	for id in ids:
		if !seen.has(id):
			errors.append("%s: unreachable from start" % id)

	for e in errors:
		printerr("ERROR ", e)
	print("check_world: %d areas, %d errors" % [ids.size(), errors.size()])
	quit(1 if errors.size() else 0)


func _collect(node: Node, id: String, portals: Dictionary, spawns: Dictionary, errors: Array[String]) -> void:
	if node.is_in_group("portal"):
		portals[id].append({"id": node.name, "target_scene": String(node.get("target_scene")), "target_spawn": String(node.get("target_spawn"))})
	if node.is_in_group("interactable"):
		var key := "interactable:" + String(node.name)
		if spawns[id].has(key):
			errors.append("%s: duplicate interactable name '%s' (state key would collide)" % [id, node.name])
		spawns[id].append(key)
	if node.is_in_group("portal"):
		for child in node.get_children():
			if child is CollisionShape2D and child.shape is RectangleShape2D:
				var center: Vector2 = node.position + child.position
				rects[id].append({"name": String(node.name), "rect": Rect2(center - child.shape.size / 2.0, child.shape.size)})
	if node.is_in_group("spawn_point"):
		spawn_points[id][String(node.name)] = node.position
		if spawns[id].has(String(node.name)):
			errors.append("%s: duplicate spawn '%s'" % [id, node.name])
		spawns[id].append(String(node.name))
	for child in node.get_children():
		_collect(child, id, portals, spawns, errors)
