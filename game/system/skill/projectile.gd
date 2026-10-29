class_name Projectile
extends Area2D

var step: StepProjectile
var team: ResourceDamageTeam
var dir := Vector2.RIGHT
var travelled := 0.0
var pierced := 0
var hit := {}
var sprite: Sprite2D
var done := false
var ghost_clock := 0.0


static func spawn(parent: Node, at: Vector2, direction: Vector2, shot: StepProjectile, shot_team: ResourceDamageTeam) -> Projectile:
	var p := Projectile.new()
	p.step = shot
	p.team = shot_team
	p.dir = direction
	parent.add_child(p)
	p.global_position = at
	return p


func _ready() -> void:
	collision_layer = 0
	collision_mask = 0
	set_collision_mask_value(1, true)
	set_collision_mask_value(Hitbox.HITBOX_LAYER, true)
	monitorable = false
	var shape := CollisionShape2D.new()
	var circle := CircleShape2D.new()
	circle.radius = step.hit_radius
	shape.shape = circle
	add_child(shape)
	sprite = Sprite2D.new()
	sprite.texture = step.texture
	sprite.hframes = step.hframes
	add_child(sprite)
	if step.face_dir:
		sprite.rotation = dir.angle()
	if step.embers.a > 0.0:
		add_embers()
	area_entered.connect(on_area)
	body_entered.connect(on_body)


func _physics_process(delta: float) -> void:
	if done:
		return
	var move := dir * step.speed * delta
	global_position += move
	travelled += move.length()
	if step.hframes > 1:
		sprite.frame = int(travelled / step.speed * step.fps) % step.hframes
	if step.trail.a > 0.0:
		ghost_clock -= delta
		if ghost_clock <= 0.0:
			ghost_clock = 0.025
			spawn_ghost()
	if step.spin_deg:
		sprite.rotation += deg_to_rad(step.spin_deg) * delta
	if travelled >= step.travel:
		finish()


func on_area(area: Area2D) -> void:
	if done or hit.has(area) or !area is Hitbox:
		return
	hit[area] = true
	if !SkillHit.apply(area, step.damage, team, global_position):
		return
	pierced += 1
	if pierced > step.pierce:
		finish()


func on_body(_body: Node2D) -> void:
	finish()


func finish() -> void:
	if done:
		return
	done = true
	if step.impact:
		# Đang trong lệnh gọi lại của vật lý nên phải trì hoãn việc tạo vùng đánh mới.
		step.impact.strike.call_deferred(get_parent(), global_position, dir, team)
	queue_free()


## Một bóng mờ tại vị trí hiện tại, nhỏ dần rồi tan: nối lại thành vệt đuôi.
func spawn_ghost() -> void:
	var ghost := Sprite2D.new()
	ghost.texture = sprite.texture
	ghost.hframes = sprite.hframes
	ghost.frame = sprite.frame
	ghost.rotation = sprite.rotation
	ghost.modulate = step.trail
	ghost.add_to_group("skill_ghost")
	get_parent().add_child(ghost)
	ghost.global_position = global_position
	var tween := ghost.create_tween().set_parallel(true)
	tween.tween_property(ghost, "scale", Vector2.ONE * 0.3, 0.25)
	tween.tween_property(ghost, "modulate:a", 0.0, 0.25)
	tween.chain().tween_callback(ghost.queue_free)


## Tàn lửa rơi lại phía sau (toạ độ thế giới nên để lại vệt khi đạn bay đi).
func add_embers() -> void:
	var sparks := CPUParticles2D.new()
	sparks.local_coords = false
	sparks.amount = 16
	sparks.lifetime = 0.45
	sparks.direction = -dir
	sparks.spread = 28.0
	sparks.gravity = Vector2.ZERO
	sparks.initial_velocity_min = 12.0
	sparks.initial_velocity_max = 36.0
	sparks.scale_amount_min = 1.0
	sparks.scale_amount_max = 2.2
	var fade := Gradient.new()
	fade.set_color(0, step.embers)
	fade.set_color(1, Color(step.embers.r, step.embers.g * 0.3, 0.0, 0.0))
	sparks.color_ramp = fade
	add_child(sparks)
