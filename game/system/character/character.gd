@icon("../character/icon_character.png")
extends CharacterBody2D
class_name Character


enum State{IDLE}


signal teleported
signal died(cause)
signal hurt(amount)
signal shield_changed
## Khiên vừa hấp thụ một đòn đánh tới từ vị trí `from` (để vẽ vòng cung chắn đúng hướng).
signal shield_absorbed(from)

@export_category("vitals")
@export var max_hp := 12.0

@export_category("physics")
@export var speed = 100.0
@export var acceleration = 1000.0
@export var deceleration = 800.0

@export var actor_scene:PackedScene:
	set(v):
		actor_scene = v
		if !is_inside_tree():
			await ready
		if actor:
			actor.queue_free()
		# Nhân vật không dùng actor_scene (ví dụ Npc) để trống: không có gì để tạo.
		actor = actor_scene.instantiate() if actor_scene else null

@export var team:ResourceDamageTeam:
	set(v):
		team = v


var actor:ActorSprite
var move_vector := Vector2.ZERO:
	set(v):
		move_vector = v
		if move_vector.length() and action_left <= 0.0:
			sprite.direction = move_vector.normalized()
var state:State
var just_teleport := false
## Đang làm động tác (đánh, ném, niệm): đứng yên và không đổi hướng cho tới khi hết giờ.
var action_left := 0.0
var action_anim := SpriteCharacter.Anim.ATTACK
## Trạng thái khống chế đang chịu: loại -> giây còn lại.
var status_left := {}
var dead := false
## Lá chắn: chặn mọi đòn thường (trạng thái, đẩy lùi). Không chặn được đòn chí mạng hay bị đè.
var guard := false
var life := ResourceLife.new()
## Khiên: điểm hấp thụ còn lại và số giây khiên còn tồn tại. Hấp thụ sát thương trước khi trừ máu;
## không đỡ được đòn chí mạng hay bị đè (die() bỏ qua cả khiên lẫn máu).
var shield := 0.0
var shield_max := 0.0
var shield_left := 0.0
## Khiên có kèm thế thủ (guard) hay không: khi hết khiên thì thế thủ cũng hết.
var shield_guards := false

const STATUS_TINT := {
	ResourceDamage.Status.ROOT: Color(0.85, 0.65, 0.45),
	ResourceDamage.Status.SLOW: Color(0.6, 0.8, 1.0),
	ResourceDamage.Status.STUN: Color(1.0, 1.0, 0.45),
	ResourceDamage.Status.FREEZE: Color(0.55, 0.9, 1.4),
}
const SLOW_FACTOR := 0.4
const DUST := preload("res://content/skill/fx/smoke.png")
const LAND_SLIDE_SEC := 0.35
const LAND_SLIDE_DECEL := 150.0
## Bị hất tung: thời gian bay còn lại, tổng, độ cao và thời gian trượt sau khi rơi.
var air_left := 0.0
var air_total := 0.0
var air_height := 0.0
var slide_left := 0.0

@onready var sprite: SpriteCharacter = $Sprite
@onready var hitbox: Hitbox = $Hitbox


func _ready() -> void:
	life.max_life = max_hp
	life.life = max_hp
	# Hitbox dựng sẵn trong character.tscn có đội riêng; đồng bộ để đòn của chính mình không trúng mình.
	if team:
		hitbox.team = team
	hitbox.damage_received.connect(on_hit)


func _physics_process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if dead:
		sprite.anim = SpriteCharacter.Anim.DEAD
		return
	tick_status(delta)
	tick_shield(delta)
	match state:
		State.IDLE:
			if air_left > 0.0:
				tick_air(delta)
			elif slide_left > 0.0:
				slide_left -= delta
				sprite.anim = SpriteCharacter.Anim.IDLE
				velocity = velocity.move_toward(Vector2.ZERO,LAND_SLIDE_DECEL*delta)
			elif is_immobile():
				sprite.anim = SpriteCharacter.Anim.IDLE
				velocity = velocity.move_toward(Vector2.ZERO,deceleration*delta*4.0)
			elif action_left > 0.0:
				action_left -= delta
				sprite.anim = action_anim
				velocity = velocity.move_toward(Vector2.ZERO,deceleration*delta)
			elif move_vector.length():
				sprite.anim = SpriteCharacter.Anim.MOVING
				velocity = velocity.move_toward(move_vector*speed*(SLOW_FACTOR if has_status(ResourceDamage.Status.SLOW) else 1.0),acceleration*delta)
			else:
				sprite.anim = SpriteCharacter.Anim.IDLE
				velocity = velocity.move_toward(Vector2.ZERO,deceleration*delta)
	move_and_slide()
	if move_vector.length():
		for i in get_slide_collision_count():
			var block := get_slide_collision(i).get_collider() as PushBlock
			if block and move_vector.normalized().dot(-get_slide_collision(i).get_normal()) > 0.7:
				block.nudge(-get_slide_collision(i).get_normal(), delta)


## Chết ngay (bị đè, đòn chí mạng): không đi qua bất kỳ cơ chế đỡ đòn nào.
func die(cause:StringName = &""):
	if dead:
		return
	dead = true
	velocity = Vector2.ZERO
	action_left = 0.0
	air_left = 0.0
	collision_layer = 0
	hitbox.disable()
	break_shield()
	sprite.position.y = 0.0
	sprite.anim = SpriteCharacter.Anim.DEAD
	if cause == &"crushed":
		sprite.scale = Vector2(1.25, 0.3)
		sprite.offset = Vector2(0, 2)
	died.emit(cause)


func has_status(kind:ResourceDamage.Status) -> bool:
	return status_left.get(kind, 0.0) > 0.0


## Không đi được: trói, choáng, đóng băng.
func is_immobile() -> bool:
	return has_status(ResourceDamage.Status.ROOT) or !can_act()


## Choáng và đóng băng không ra chiêu được; trói thì vẫn ra chiêu.
func can_act() -> bool:
	return !dead and air_left <= 0.0 and !has_status(ResourceDamage.Status.STUN) and !has_status(ResourceDamage.Status.FREEZE)


func apply_status(kind:ResourceDamage.Status,seconds:float):
	if kind == ResourceDamage.Status.NONE or seconds <= 0.0:
		return
	status_left[kind] = maxf(status_left.get(kind, 0.0), seconds)
	update_status_tint()


func tick_status(delta:float):
	if status_left.is_empty():
		return
	for kind in status_left.keys():
		status_left[kind] -= delta
		if status_left[kind] <= 0.0:
			status_left.erase(kind)
	update_status_tint()


func update_status_tint():
	var tint := Color.WHITE
	for kind in status_left:
		tint *= STATUS_TINT[kind]
	sprite.self_modulate = tint


## Dựng khiên: thay khiên cũ nếu khiên mới dày hơn hoặc lâu hơn.
func add_shield(points:float,seconds:float,guarding := false):
	if dead or points <= 0.0 or seconds <= 0.0:
		return
	shield = maxf(shield, points)
	shield_max = maxf(shield_max, shield)
	shield_left = maxf(shield_left, seconds)
	if guarding:
		shield_guards = true
		guard = true
	shield_changed.emit()


func break_shield():
	var had := shield > 0.0 or shield_left > 0.0
	shield = 0.0
	shield_max = 0.0
	shield_left = 0.0
	if shield_guards:
		shield_guards = false
		guard = false
	if had:
		shield_changed.emit()


func tick_shield(delta:float):
	if shield_left <= 0.0:
		return
	shield_left -= delta
	if shield_left <= 0.0:
		break_shield()


## Hồi sinh với đầy máu, không khiên, không khống chế.
func revive():
	if !dead:
		return
	dead = false
	collision_layer = 1
	hitbox.monitorable = true
	sprite.scale = Vector2.ONE
	sprite.offset = Vector2.ZERO
	status_left.clear()
	update_status_tint()
	break_shield()
	life.life = max_hp


func on_hit(damage:ResourceDamage,at_pos):
	if dead:
		return
	if damage.lethal:
		die(&"lethal")
		return
	var dealt := float(damage.amount)
	if shield > 0.0:
		var soaked := minf(shield, dealt)
		shield_absorbed.emit(at_pos)
		shield -= soaked
		dealt -= soaked
		if shield <= 0.0:
			break_shield()
		else:
			shield_changed.emit()
	if dealt > 0.0:
		life.damage(dealt)
		hurt.emit(dealt)
		if !life.is_alive():
			die(&"slain")
			return
	if guard:
		return
	apply_status(damage.status, damage.status_sec)
	if damage.launch_sec > 0.0:
		launch(at_pos, damage.push_force, damage.launch_sec, damage.launch_height)
	elif damage.push_force:
		push(at_pos, damage.push_force)


## Hất lên không: bay theo hướng từ tâm đòn ra, không đi hay ra chiêu được cho tới khi rơi xuống.
func launch(from:Vector2,force:int,seconds:float,height:float):
	var away := from.direction_to(global_position)
	if away == Vector2.ZERO:
		away = Vector2.DOWN
	velocity = away*force*0.5
	air_total = seconds
	air_left = seconds
	air_height = height
	slide_left = 0.0
	action_left = 0.0


func tick_air(delta:float):
	air_left -= delta
	var t := 1.0 - clampf(air_left/air_total,0.0,1.0)
	sprite.position.y = -air_height*4.0*t*(1.0-t)
	sprite.anim = SpriteCharacter.Anim.JUMP
	if air_left <= 0.0:
		air_left = 0.0
		sprite.position.y = 0.0
		slide_left = LAND_SLIDE_SEC
		OneShotFx.spawn(get_parent(), global_position, DUST, 6, 14.0, 0.7)


func push(from:Vector2,force:int):
	velocity += from.direction_to(global_position)*force*2.0


func act(anim:SpriteCharacter.Anim,seconds:float):
	action_anim = anim
	action_left = seconds


func teleport(target_teleporter:Teleporter,offset_position:Vector2):
	if just_teleport:
		return
	global_position = target_teleporter.global_position+offset_position+(target_teleporter.direction*Vector2(25,25))
	just_teleport = true
	for camera in get_tree().get_nodes_in_group("camera"):
		camera.teleport_to(global_position)
	await get_tree().create_timer(0.1,false).timeout
	just_teleport = false
	teleported.emit()

