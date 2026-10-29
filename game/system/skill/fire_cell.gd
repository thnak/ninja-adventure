## Lửa trên một ô: hoạt hình ngọn lửa, gây sát thương theo nhịp, lan sang ô kề có nhiên liệu.
class_name FireCell
extends Node2D

const DAMAGE_EVERY := 0.5
const SPREAD_EVERY := 0.6
const SPREAD_CHANCE := 0.3
const NEIGHBOURS := [Vector2i.LEFT, Vector2i.RIGHT, Vector2i.UP, Vector2i.DOWN]

static var fire_damage: ResourceDamage

var field: FireField
var cell: Vector2i
var kind := "ground"
var fuel_left := 2.0
var team: ResourceDamageTeam
var flames: Array[Sprite2D] = []
var area: Area2D
var age := 0.0
var damage_clock := 0.0
var spread_clock := 0.0


func _ready() -> void:
	if !fire_damage:
		fire_damage = ResourceDamage.new()
		fire_damage.amount = 1
	var big := kind == "tree"
	var spots := [Vector2(-4, 2), Vector2(4, 0), Vector2(0, -3)] if big else [Vector2(-3, 2), Vector2(3, 0)]
	for i in spots.size():
		var flame := Sprite2D.new()
		flame.texture = FireField.FIRE
		flame.hframes = 12
		flame.frame = 1
		flame.position = spots[i] + (Vector2(0, -4) if big else Vector2.ZERO)
		flame.scale = Vector2.ONE * (1.8 if big else 1.3)
		flame.set_meta("phase", float(i) * 0.37)
		add_child(flame)
		flames.append(flame)
	area = Area2D.new()
	area.collision_layer = 0
	area.collision_mask = 0
	area.set_collision_mask_value(Hitbox.HITBOX_LAYER, true)
	area.monitorable = false
	var shape := CollisionShape2D.new()
	var rect := RectangleShape2D.new()
	rect.size = Vector2(14, 14)
	shape.shape = rect
	area.add_child(shape)
	add_child(area)
	add_to_group("fire")


func _physics_process(delta: float) -> void:
	age += delta
	fuel_left -= delta
	if fuel_left <= 0.0:
		field.burn_out(cell, kind)
		queue_free()
		return
	for flame in flames:
		var phase: float = flame.get_meta("phase")
		if fuel_left < 0.45:
			flame.frame = 9 + mini(2, int((0.45 - fuel_left) / 0.15))
		else:
			flame.frame = 1 + int((age + phase) * 11.0) % 8
	damage_clock -= delta
	if damage_clock <= 0.0:
		damage_clock = DAMAGE_EVERY
		for hit in area.get_overlapping_areas():
			if hit is Hitbox and !hit.has_meta("boulder"):
				SkillHit.apply(hit, fire_damage, team, global_position)
	spread_clock -= delta
	if spread_clock <= 0.0:
		spread_clock = SPREAD_EVERY
		for dir in NEIGHBOURS:
			var next: Vector2i = cell + dir
			if field.can_spread_to(next) and field.rng.randf() < SPREAD_CHANCE:
				field.ignite_cell(next, team)


## Bị dập (nước, băng): tắt ngay, không để lại dấu cháy.
func put_out() -> void:
	remove_from_group("fire")
	queue_free()
