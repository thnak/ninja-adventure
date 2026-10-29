## Đá tảng: trượt/lăn theo vận tốc, ma sát làm chậm dần, bị địa hình "khấu trừ" năng lượng (xem BoulderModel).
## Từ tốc độ CRUSH_SPEED trở lên, ai chạm vào (khác phe người đẩy) đều chết ngay, không có đỡ đòn hay lá chắn.
##
## Hai dạng:
##  - viên tròn (mặc định, dùng cho boss lăn đá): hình tròn, xoay theo quãng đường lăn.
##  - tấm đá (đặt `slab_texture`): đá có sẵn trong môi trường, hộp va chạm chữ nhật, trượt thẳng như viên gạch, không xoay.
## Đá môi trường nằm yên cho tới khi bị vụ nổ đẩy hoặc bị boss `shove` (StepShove).
class_name Boulder
extends CharacterBody2D

signal crushed(target)
signal stopped

const ROCK := preload("res://content/skill/fx/rockgray.png")
const SHADOW := preload("res://content/character/Shadow.png")
const MAX_SPEED := 260.0
## Khối lượng của tấm đá = diện tích ảnh / hệ số này (tấm 65x48 nặng khoảng 130).
const SLAB_MASS_DIVISOR := 24.0

@export var mass := 100.0
@export var drag := 60.0
@export var radius := 12.0
## Có ảnh thì là tấm đá: trượt không xoay, hộp va chạm theo đáy tấm đá.
@export var slab_texture: Texture2D

## Phe đang đẩy đá: đá không giết đồng minh của phe này. Đổi sang phe vừa dội mạnh vào nó.
var owner_team: ResourceDamageTeam
var crushed_targets: Array[Node] = []
var sprite: Sprite2D
var hitbox: Hitbox
var crush_area: Area2D
var was_moving := false
var rolls := true
## Kích thước đáy chạm đất (dùng cho vệt báo trước và hộp va chạm).
var footprint := Vector2.ZERO


static func spawn(parent: Node, at: Vector2, velocity_start: Vector2, team: ResourceDamageTeam, weight := 100.0, friction := 60.0) -> Boulder:
	var b := Boulder.new()
	b.mass = weight
	b.drag = friction
	b.owner_team = team
	parent.add_child(b)
	b.global_position = at
	b.velocity = velocity_start
	return b


## Tấm đá có sẵn dạng ảnh (đá môi trường), nằm yên.
static func spawn_slab(parent: Node, at: Vector2, texture: Texture2D, team: ResourceDamageTeam = null) -> Boulder:
	var b := Boulder.new()
	b.slab_texture = texture
	b.owner_team = team
	parent.add_child(b)
	b.global_position = at
	return b


func _ready() -> void:
	add_to_group("boulder")
	motion_mode = CharacterBody2D.MOTION_MODE_FLOATING
	collision_layer = 1
	collision_mask = 1
	var body := CollisionShape2D.new()
	var shadow := Sprite2D.new()
	shadow.texture = SHADOW
	sprite = Sprite2D.new()
	var contact: Shape2D
	if slab_texture:
		rolls = false
		var size := slab_texture.get_size()
		# Đáy chạm đất: gần hết bề rộng, độ sâu bằng khoảng 40% chiều cao (tối đa 22).
		footprint = Vector2(size.x - 8.0, clampf(size.y * 0.4, 10.0, 22.0))
		if mass == 100.0:
			mass = size.x * size.y / SLAB_MASS_DIVISOR
		var rect := RectangleShape2D.new()
		rect.size = footprint
		contact = rect
		sprite.texture = slab_texture
		sprite.offset = Vector2(0, -size.y / 2.0 + footprint.y / 2.0)
		shadow.scale = Vector2(size.x / 22.0, 1.4)
	else:
		footprint = Vector2(radius * 2.0, radius * 2.0)
		var disc := CircleShape2D.new()
		disc.radius = radius
		contact = disc
		sprite.texture = ROCK
		sprite.hframes = 5
		sprite.frame = 0
		sprite.scale = Vector2(2.0, 2.0)
		sprite.offset = Vector2(0, -4)
		shadow.scale = Vector2(1.6, 1.6)
	body.shape = contact
	add_child(body)
	add_child(shadow)
	add_child(sprite)
	# Hitbox không có đội để vụ nổ của mọi phe đều đẩy được; SkillHit nhận ra nhờ meta "boulder".
	hitbox = Hitbox.new()
	hitbox.set_meta("boulder", self)
	hitbox.add_child(grown_shape(footprint, 4.0))
	add_child(hitbox)
	crush_area = Area2D.new()
	crush_area.collision_layer = 0
	crush_area.collision_mask = 0
	crush_area.set_collision_mask_value(Hitbox.HITBOX_LAYER, true)
	crush_area.monitorable = false
	crush_area.add_child(grown_shape(footprint, 6.0))
	add_child(crush_area)


## Hình va chạm cùng dạng với thân đá nhưng nở thêm `margin` mỗi phía.
func grown_shape(size: Vector2, margin: float) -> CollisionShape2D:
	var holder := CollisionShape2D.new()
	if slab_texture:
		var rect := RectangleShape2D.new()
		rect.size = size + Vector2.ONE * margin
		holder.shape = rect
	else:
		var disc := CircleShape2D.new()
		disc.radius = size.x / 2.0 + margin / 2.0
		holder.shape = disc
	return holder


func speed() -> float:
	return velocity.length()


## Boss (hoặc ai đó) đẩy đá đi theo vận tốc `push`, đứng về phe `team`.
func shove(push: Vector2, team: ResourceDamageTeam) -> void:
	velocity = push.limit_length(MAX_SPEED)
	owner_team = team


func _physics_process(delta: float) -> void:
	var current := speed()
	if current <= 0.5:
		velocity = Vector2.ZERO
		if was_moving:
			was_moving = false
			stopped.emit()
		return
	was_moving = true
	var collision := move_and_collide(velocity * delta)
	if collision:
		on_collide(collision)
	current = maxf(0.0, speed() - drag * delta)
	velocity = velocity.normalized() * current if current > 0.0 else Vector2.ZERO
	if rolls:
		sprite.rotation += current * delta / radius * signf(velocity.x if absf(velocity.x) > 0.01 else velocity.y)
	if current >= BoulderModel.CRUSH_SPEED:
		crush()


## Va vào vật cản: khấu trừ năng lượng theo độ bền. Tường và vật không có độ bền chặn đứng đá.
func on_collide(collision: KinematicCollision2D) -> void:
	var other := collision.get_collider()
	if !other or !other.has_method("strength"):
		velocity = Vector2.ZERO
		return
	var result := BoulderModel.hit(BoulderModel.energy(mass, speed()), other.strength())
	if result.broken:
		other.shatter()
		velocity = velocity.normalized() * BoulderModel.speed_for(mass, result.energy)
	else:
		other.take_load(result.absorbed)
		velocity = Vector2.ZERO


## Vụ nổ đẩy đá: Δv cộng theo vectơ. Nếu dội đủ mạnh thì đá đổi phe, quay lại nghiền kẻ đã đẩy ra.
func blast(damage: ResourceDamage, team: ResourceDamageTeam, from: Vector2) -> void:
	var away := from.direction_to(global_position)
	if away == Vector2.ZERO:
		away = Vector2.DOWN
	var dv := BoulderModel.blast_delta_v(mass, damage.push_force, global_position.distance_to(from))
	var before := speed()
	velocity = (velocity + away * dv).limit_length(MAX_SPEED)
	if team and dv >= maxf(10.0, before * 0.5):
		owner_team = team


func crush() -> void:
	for area in crush_area.get_overlapping_areas():
		if !area is Hitbox or area.has_meta("boulder"):
			continue
		if owner_team and area.team and (area.team == owner_team or area.team.is_ally(owner_team)):
			continue
		var victim := area.get_parent()
		if crushed_targets.has(victim):
			continue
		if victim is Character:
			victim.die(&"crushed")
		elif victim is Destroyable:
			victim.life.kill()
		else:
			continue
		crushed_targets.append(victim)
		crushed.emit(victim)
