extends Character
class_name Npc
## Nhân vật không điều khiển: đứng yên hoặc đi lang thang quanh chỗ ở, nói chuyện khi người chơi bấm `interact`.
## Dùng scene content/character/npc.tscn; đổi `sheet` (bảng sprite 4x7 của pack) cho từng người.


@export var sheet:Texture2D
@export var display_name := ""
@export_multiline var dialogue := ""
## 0 = đứng yên. Lớn hơn 0: đi lang thang trong bán kính này (pixel) quanh vị trí đặt ban đầu.
@export var wander_radius := 0.0

var home:Vector2
var wander_timer := 0.0
var talk:NpcTalk


func _init() -> void:
	add_to_group("npc")


func _ready() -> void:
	if sheet:
		sprite.texture = sheet
	home = global_position
	wander_timer = randf_range(0.5, 2.0)
	talk = NpcTalk.new()
	talk.name = "Talk_" + String(name)
	talk.prompt = "Nói chuyện"
	var shape := CollisionShape2D.new()
	shape.shape = CircleShape2D.new()
	shape.shape.radius = 22.0
	talk.add_child(shape)
	add_child(talk)
	talk.text = ("%s\n%s" % [display_name, dialogue]) if !display_name.is_empty() else dialogue


func _physics_process(delta:float) -> void:
	wander(delta)
	super._physics_process(delta)


func wander(delta:float) -> void:
	if wander_radius <= 0.0:
		return
	wander_timer -= delta
	if wander_timer > 0.0:
		return
	if randf() < 0.45:
		move_vector = Vector2.ZERO
		wander_timer = randf_range(1.5, 3.5)
		return
	var dirs := [Vector2.UP, Vector2.DOWN, Vector2.LEFT, Vector2.RIGHT]
	var dir:Vector2 = dirs[randi() % dirs.size()]
	if global_position.distance_to(home) > wander_radius:
		dir = global_position.direction_to(home)
	move_vector = dir
	wander_timer = randf_range(0.5, 1.2)


func face(point:Vector2) -> void:
	move_vector = Vector2.ZERO
	var d := point - global_position
	sprite.direction = Vector2(signf(d.x), 0.0) if absf(d.x) > absf(d.y) else Vector2(0.0, signf(d.y))
