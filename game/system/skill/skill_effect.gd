class_name SkillEffect
extends Area2D

const SHOCKWAVE := preload("res://content/skill/fx/shockwave.png")
const SMOKE := preload("res://content/skill/fx/smoke.png")
const ROCKS := preload("res://content/particle/particle_rock.png")

var step: StepStrike
var team: ResourceDamageTeam
var elapsed := 0.0
var hit := {}
var sprite: Sprite2D
var age := 0.0


static func spawn(parent: Node, at: Vector2, dir: Vector2, strike_step: StepStrike, strike_team: ResourceDamageTeam) -> SkillEffect:
	var fx := SkillEffect.new()
	fx.step = strike_step
	fx.team = strike_team
	fx.global_position = at
	if strike_step.rotate_with_dir:
		fx.rotation = dir.angle()
	parent.add_child(fx)
	fx.global_position = at
	return fx


func _ready() -> void:
	collision_layer = 0
	collision_mask = 0
	set_collision_mask_value(Hitbox.HITBOX_LAYER, true)
	monitorable = false
	var shape := CollisionShape2D.new()
	var circle := CircleShape2D.new()
	circle.radius = step.radius
	shape.shape = circle
	add_child(shape)
	sprite = Sprite2D.new()
	sprite.texture = step.texture
	sprite.visible = step.windup <= 0.0
	sprite.hframes = step.hframes
	sprite.scale = Vector2.ONE * step.fx_scale
	add_child(sprite)
	if step.windup <= 0.0:
		begin()


func _physics_process(delta: float) -> void:
	if age < step.windup:
		age += delta
		queue_redraw()
		if age >= step.windup:
			sprite.visible = step.texture != null
			queue_redraw()
			begin()
		return
	elapsed += delta
	var frame := step.start_frame + int(elapsed * step.fps)
	if frame >= step.hframes:
		queue_free()
		return
	sprite.frame = frame
	var last := step.hit_to if step.hit_to >= 0 else step.hframes - 1
	if frame < step.hit_from or frame > last:
		return
	for area in get_overlapping_areas():
		if hit.has(area) or !area is Hitbox:
			continue
		hit[area] = true
		SkillHit.apply(area, step.damage, team, global_position)


## Lúc đòn thực sự nổ ra: âm thanh và các lớp hiệu ứng phụ.
func begin() -> void:
	if step.sound:
		SkillAudio.play(self, step.sound)
	var host := get_parent()
	if step.ignites or step.extinguishes:
		var field := FireField.of(self)
		if field and step.ignites:
			field.ignite(global_position, step.radius, team)
		if field and step.extinguishes:
			field.extinguish(global_position, step.radius * 1.3)
	if step.shockwave:
		OneShotFx.spawn(host, global_position, SHOCKWAVE, 4, 9.0, step.radius / 11.0, 1, step.fx_tint * Color(1, 1, 1, 0.9))
	if step.smoke:
		for i in 4:
			var at := global_position + Vector2.from_angle(i * TAU / 4.0 + 0.6) * step.radius * 0.55
			OneShotFx.spawn(host, at, SMOKE, 6, 7.0, step.radius / 26.0, 0, step.fx_tint * Color(1, 1, 1, 0.75))
	if step.debris:
		spawn_debris(host)


## Mảnh đá bay lên rồi rơi (trọng lực kéo xuống) rải quanh tâm.
func spawn_debris(host: Node) -> void:
	var rocks := CPUParticles2D.new()
	var canvas_material := CanvasItemMaterial.new()
	canvas_material.particles_animation = true
	canvas_material.particles_anim_h_frames = 5
	canvas_material.particles_anim_v_frames = 1
	rocks.material = canvas_material
	rocks.texture = ROCKS
	rocks.one_shot = true
	rocks.explosiveness = 1.0
	rocks.amount = int(6 + step.radius / 5.0)
	rocks.lifetime = 0.9
	rocks.emission_shape = CPUParticles2D.EMISSION_SHAPE_SPHERE
	rocks.emission_sphere_radius = step.radius * 0.5
	rocks.direction = Vector2.UP
	rocks.spread = 80.0
	rocks.gravity = Vector2(0, 340)
	rocks.initial_velocity_min = 70.0
	rocks.initial_velocity_max = 150.0
	rocks.angular_velocity_min = -240.0
	rocks.angular_velocity_max = 240.0
	rocks.anim_offset_max = 1.0
	rocks.finished.connect(rocks.queue_free)
	host.add_child(rocks)
	rocks.global_position = global_position
	rocks.emitting = true


## Vòng cảnh báo: viền cố định là vùng sẽ bị đánh, phần tô đầy dần theo thời gian chờ.
func _draw() -> void:
	if age >= step.windup:
		return
	var fill := clampf(age / step.windup, 0.0, 1.0)
	draw_circle(Vector2.ZERO, step.radius * fill, Color(1.0, 0.15, 0.1, 0.28))
	draw_arc(Vector2.ZERO, step.radius, 0.0, TAU, 32, Color(1.0, 0.25, 0.15, 0.9), 1.0)
