class_name SaveGame
## Lưu/nạp tiến độ (bản offline). JSON có `version`. Ghi qua file tạm rồi đổi tên, giữ một bản `.bak`
## để một lần ghi hỏng không mất save; `read()` tự rơi về `.bak` khi file chính hỏng.


const VERSION = 1

## Đường dẫn save. Bài kiểm tra đổi sang thư mục tạm để không đụng save thật.
static var path := "user://save.json"


static func backup_path() -> String:
	return path + ".bak"


static func temp_path() -> String:
	return path + ".tmp"


static func has_save() -> bool:
	return FileAccess.file_exists(path) or FileAccess.file_exists(backup_path())


static func snapshot(world:World) -> Dictionary:
	var items := {}
	for id in GameState.items:
		items[String(id)] = GameState.items[id]
	var visited := []
	for id in GameState.visited:
		visited.append(String(id))
	var pos := world.player.global_position
	var facing:Vector2 = world.player.sprite.direction
	return {
		"version": VERSION,
		"saved_at": Time.get_datetime_string_from_system(),
		"play_time": world.play_time,
		"player": {"scene": String(world.map.scene_id), "pos": [pos.x, pos.y], "facing": [facing.x, facing.y]},
		"state": {"objects": GameState.objects, "items": items, "visited": visited},
	}


static func write(world:World) -> bool:
	var text := JSON.stringify(snapshot(world), "\t")
	var file := FileAccess.open(temp_path(), FileAccess.WRITE)
	if file == null:
		push_error("SaveGame: cannot write " + temp_path())
		return false
	file.store_string(text)
	file.close()
	if FileAccess.get_file_as_string(temp_path()) != text:
		push_error("SaveGame: temp file verification failed")
		return false
	if FileAccess.file_exists(path):
		if FileAccess.file_exists(backup_path()):
			DirAccess.remove_absolute(backup_path())
		DirAccess.rename_absolute(path, backup_path())
	return DirAccess.rename_absolute(temp_path(), path) == OK


## Dữ liệu save hợp lệ đầu tiên (file chính, rồi `.bak`), hoặc {} nếu không có.
static func read() -> Dictionary:
	for candidate in [path, backup_path()]:
		if !FileAccess.file_exists(candidate):
			continue
		var parsed = JSON.parse_string(FileAccess.get_file_as_string(candidate))
		if valid(parsed):
			return parsed
		push_warning("SaveGame: %s is corrupt, trying the next copy" % candidate)
	return {}


static func valid(data) -> bool:
	return data is Dictionary \
		and int(data.get("version", 0)) >= 1 and int(data.get("version", 0)) <= VERSION \
		and data.get("player") is Dictionary and data.get("state") is Dictionary \
		and data.player.get("pos") is Array and data.player.pos.size() == 2


## Nạp phần trạng thái vào GameState (vị trí người chơi do World xử lý).
static func apply(data:Dictionary) -> void:
	GameState.reset()
	GameState.objects = data.state.get("objects", {})
	for id in data.state.get("items", {}):
		GameState.items[StringName(id)] = int(data.state.items[id])
	for id in data.state.get("visited", []):
		GameState.visit(StringName(id))


static func delete() -> void:
	for p in [path, backup_path(), temp_path()]:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(p)
