class_name WorldGraph
## Đồ thị khu: vùng, khu (scene_id -> .tscn), liên kết, điểm bắt đầu. Dữ liệu ở content/world_graph.json.
## Cổng (Portal) nằm trong từng scene; tools/check_world.gd kiểm tra cổng khớp với đồ thị này.
## `map_pos` là toạ độ vẽ trên bản đồ lớn (không gian 1280x720), đặt tay.


## Đồ thị đang dùng. Bài kiểm tra đổi sang game/tests/world_graph_test.json qua `use()`.
static var path := "res://content/world_graph.json"

static var _data:Dictionary


static func use(graph_path:String) -> void:
	path = graph_path
	_data = {}


static func data() -> Dictionary:
	if _data.is_empty():
		var file := FileAccess.open(path,FileAccess.READ)
		if file == null:
			push_error("WorldGraph: cannot open " + path)
			return {}
		var parsed = JSON.parse_string(file.get_as_text())
		if !(parsed is Dictionary):
			push_error("WorldGraph: invalid JSON in " + path)
			return {}
		_data = parsed
	return _data


static func _area(scene_id:StringName) -> Dictionary:
	return data().get("areas",{}).get(String(scene_id),{})


static func _region(region_id:StringName) -> Dictionary:
	return data().get("regions",{}).get(String(region_id),{})


static func _vec(a) -> Vector2:
	return Vector2(a[0],a[1]) if a is Array and a.size() == 2 else Vector2.ZERO


static func start_scene() -> StringName:
	return StringName(data().get("start",{}).get("scene",""))


static func start_spawn() -> StringName:
	return StringName(data().get("start",{}).get("spawn","start"))


static func area_ids() -> Array:
	return data().get("areas",{}).keys()


static func has_area(scene_id:StringName) -> bool:
	return data().get("areas",{}).has(String(scene_id))


static func scene_path(scene_id:StringName) -> String:
	return _area(scene_id).get("path","")


static func region_of(scene_id:StringName) -> StringName:
	return StringName(_area(scene_id).get("region",""))


static func area_name(scene_id:StringName) -> String:
	return _area(scene_id).get("name",String(scene_id))


static func area_pos(scene_id:StringName) -> Vector2:
	return _vec(_area(scene_id).get("map_pos"))


static func region_ids() -> Array:
	return data().get("regions",{}).keys()


static func region_name(region_id:StringName) -> String:
	return _region(region_id).get("name",String(region_id))


static func region_pos(region_id:StringName) -> Vector2:
	return _vec(_region(region_id).get("map_pos"))


static func region_special(region_id:StringName) -> bool:
	return _region(region_id).get("special",false)


static func areas_in(region_id:StringName) -> Array[StringName]:
	var result:Array[StringName] = []
	for id in area_ids():
		if region_of(id) == region_id:
			result.append(StringName(id))
	return result


## Danh sách cặp [a, b] các khu nối nhau (hai chiều).
static func links() -> Array:
	return data().get("links",[])


static func neighbors(scene_id:StringName) -> Array[StringName]:
	var result:Array[StringName] = []
	for pair in links():
		if pair[0] == String(scene_id):
			result.append(StringName(pair[1]))
		elif pair[1] == String(scene_id):
			result.append(StringName(pair[0]))
	return result
