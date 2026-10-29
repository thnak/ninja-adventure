extends Marker2D
class_name SpawnPoint
## Nơi người chơi xuất hiện khi vào khu. Tên node = id (cổng dẫn tới đây dùng id này).


@export var facing := Vector2.DOWN


func _init() -> void:
	add_to_group("spawn_point")
