class_name SkillTerrain
extends StaticBody2D

var step: StepTerrain
var life: ResourceLife
var sprite: Sprite2D
var hitbox: Hitbox
var ending := false
var age := 0.0
var frame_clock := 0.0
var frame_range := Vector2i.ZERO


## Trả null nếu chỗ đó đã có tường hoặc người đứng (khỏi nhốt người chơi).
static func spawn(parent: Node, at: Vector2, built: StepTerrain) -> SkillTerrain:
	var query := PhysicsShapeQueryParameters2D.new()
	var probe := RectangleShape2D.new()
	probe.size = built.size
	query.shape = probe
	query.transform = Transform2D(0.0, at)
	query.collision_mask = 3
	if !parent.get_world_2d().direct_space_state.intersect_shape(query, 1).is_empty():
		return null
	var t := SkillTerrain.new()
	t.step = built
	parent.add_child(t)
	t.global_position = at
	return t


func _ready() -> void:
	collision_layer = 1
	collision_mask = 0
	var shape := CollisionShape2D.new()
	var rect := RectangleShape2D.new()
	rect.size = step.size
	shape.shape = rect
	add_child(shape)
	sprite = Sprite2D.new()
	sprite.texture = step.texture
	sprite.hframes = step.hframes
	sprite.offset = step.sprite_offset
	add_child(sprite)
	life = ResourceLife.new()
	life.max_life = step.max_life
	life.life = step.max_life
	life.killed.connect(break_up)
	# Hitbox không có đội: mọi bên đều đánh vỡ được.
	hitbox = Hitbox.new()
	var hit_shape := CollisionShape2D.new()
	hit_shape.shape = rect
	hitbox.add_child(hit_shape)
	add_child(hitbox)
	hitbox.damage_received.connect(on_damage)
	play(step.grow_frames)


func play(frames: Vector2i) -> void:
	frame_range = frames
	frame_clock = 0.0
	sprite.frame = frames.x


func _physics_process(delta: float) -> void:
	age += delta
	frame_clock += delta * step.fps
	var frame := frame_range.x + int(frame_clock)
	if frame > frame_range.y:
		if ending:
			queue_free()
			return
		frame = frame_range.y
	sprite.frame = frame
	if !ending and age >= step.lifetime:
		break_up()


## Độ bền đỡ đá tảng còn lại (giảm theo máu còn lại).
func strength() -> float:
	return step.strength_per_life * life.life


## Đá bị chặn lại: địa hình chịu phần năng lượng đó, mất máu tương ứng.
func take_load(energy: float) -> void:
	life.damage(energy / step.strength_per_life)
	sprite.modulate = Color(3, 3, 3)
	create_tween().tween_property(sprite, "modulate", Color.WHITE, 0.12)


## Đá phá vỡ địa hình.
func shatter() -> void:
	life.kill()


func on_damage(damage: ResourceDamage, _at) -> void:
	life.damage(damage.amount)
	sprite.modulate = Color(3, 3, 3)
	create_tween().tween_property(sprite, "modulate", Color.WHITE, 0.12)


## Hết hạn hoặc bị phá: tắt va chạm ngay, chạy hoạt hình vỡ rồi biến mất.
func break_up() -> void:
	if ending:
		return
	ending = true
	collision_layer = 0
	hitbox.disable()
	if step.break_frames.x >= 0:
		play(step.break_frames)
	else:
		var tween := create_tween()
		tween.tween_property(sprite, "modulate:a", 0.0, 0.3)
		tween.tween_callback(queue_free)
