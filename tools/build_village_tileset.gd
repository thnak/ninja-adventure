# Tạo content/map/tileset_village.tres = tileset.tres của pack + TilesetHouse (nguồn 6) + TilesetNature (nguồn 7).
# Mỗi ô không trong suốt thành một tile (không có va chạm; va chạm do trình dựng map thêm theo công trình).
# Chạy: godot --headless --path game --script ../tools/build_village_tileset.gd
extends SceneTree

const SOURCES = {6: "res://content/map/tileset_house.png", 7: "res://content/map/tileset_nature.png"}


func _initialize() -> void:
	var tileset := (load("res://content/map/tileset.tres") as TileSet).duplicate(true) as TileSet
	for id in SOURCES:
		var texture := load(SOURCES[id]) as Texture2D
		var image := texture.get_image()
		var source := TileSetAtlasSource.new()
		source.texture = texture
		source.texture_region_size = Vector2i(16, 16)
		var count := 0
		for y in image.get_height() / 16:
			for x in image.get_width() / 16:
				if !image.get_region(Rect2i(x * 16, y * 16, 16, 16)).is_invisible():
					source.create_tile(Vector2i(x, y))
					count += 1
		tileset.add_source(source, id)
		print("source ", id, " ", SOURCES[id], " tiles=", count)
	var err := ResourceSaver.save(tileset, "res://content/map/tileset_village.tres")
	print("saved tileset_village.tres err=", err)
	quit()
