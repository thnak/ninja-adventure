extends Area2D
class_name Trampleable
## Cỏ/bụi bị giẫm: người chơi bước vào thì bắn mảnh vỡ rồi biến mất, sau `regrow_sec` giây mọc lại.
## Nhẹ hơn Destroyable: không có hitbox, không có hạt thường trực (mỗi lần giẫm tạo một CPUParticles2D dùng một lần).


const PLAYER_LAYER = 5
const BURST_FRAMES = 6

@export var burst_texture:Texture2D
@export var regrow_sec := 12.0

var trampled := false

@onready var sprite: Sprite2D = $Sprite


func _ready() -> void:
	collision_layer = 0
	collision_mask = 0
	set_collision_mask_value(PLAYER_LAYER,true)
	monitorable = false
	body_entered.connect(on_body_entered)


func on_body_entered(body:Node2D) -> void:
	if trampled or !body.is_in_group("player"):
		return
	trample()


func trample() -> void:
	trampled = true
	sprite.visible = false
	burst()
	get_tree().create_timer(regrow_sec,false).timeout.connect(regrow)


func regrow() -> void:
	# Người chơi còn đứng trên chỗ này thì chờ thêm: không để cỏ mọc dưới chân.
	for body in get_overlapping_bodies():
		if body.is_in_group("player"):
			get_tree().create_timer(1.0,false).timeout.connect(regrow)
			return
	trampled = false
	sprite.visible = true
	sprite.scale = Vector2(1,0.2)
	create_tween().tween_property(sprite,"scale",Vector2.ONE,0.25).set_trans(Tween.TRANS_BACK).set_ease(Tween.EASE_OUT)


func burst() -> void:
	if burst_texture == null:
		return
	var p := CPUParticles2D.new()
	var material := CanvasItemMaterial.new()
	material.particles_animation = true
	material.particles_anim_h_frames = BURST_FRAMES
	material.particles_anim_v_frames = 1
	material.particles_anim_loop = false
	p.material = material
	p.texture = burst_texture
	p.position = Vector2(0,-3)
	p.one_shot = true
	p.explosiveness = 0.9
	p.amount = 6
	p.lifetime = 0.6
	p.direction = Vector2(0,-1)
	p.spread = 70.0
	p.gravity = Vector2(0,160)
	p.initial_velocity_min = 25.0
	p.initial_velocity_max = 55.0
	p.anim_speed_min = 1.5
	p.anim_speed_max = 1.5
	p.emitting = true
	add_child(p)
	p.finished.connect(p.queue_free)
