class_name GameState
## Trạng thái thế giới theo khoá "scene_id/object_id", túi đồ và các khu đã đến. Chưa lưu ra đĩa (xem docs/design/scenes.md mục 11).


static var objects := {}
static var items := {}
static var visited := {}


static func object_state(key:String, default = null):
	return objects.get(key, default)


static func set_object_state(key:String, value) -> void:
	objects[key] = value


static func add_item(item_id:StringName, amount := 1) -> void:
	items[item_id] = items.get(item_id, 0) + amount


static func item_count(item_id:StringName) -> int:
	return items.get(item_id, 0)


static func visit(scene_id:StringName) -> void:
	visited[scene_id] = true


static func has_visited(scene_id:StringName) -> bool:
	return visited.has(scene_id)


## Khu đã đến, hoặc kề với một khu đã đến (hiện trên bản đồ dưới dạng "?").
static func is_known(scene_id:StringName) -> bool:
	if has_visited(scene_id):
		return true
	for n in WorldGraph.neighbors(scene_id):
		if has_visited(n):
			return true
	return false


static func reset() -> void:
	objects.clear()
	items.clear()
	visited.clear()
