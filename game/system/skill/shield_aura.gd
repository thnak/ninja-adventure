## Vòng sáng của khiên quanh nhân vật: nhỏ dần theo phần khiên còn lại, nháy khi hấp thụ đòn, tự mất khi khiên hết.
class_name ShieldAura
extends Node2D

const FLASH_SEC := 0.15
const SHEET_BLUE := preload("res://content/skill/fx/shield_blue.png")
const SHEET_YELLOW := preload("res://content/skill/fx/shield_yellow.png")
const SHATTER := preload("res://content/skill/fx/aura_break.png")
## Tờ hình khiên: 6 khung, vòng cung hiện ra rồi tan; vòng cung quay lên phía trên khi không xoay.
const SHEET_FRAMES := 6
const SHATTER_FRAMES := 5

var who: Character
var tint := Color(0.45, 0.8, 1.0)
var sheet: Texture2D = SHEET_BLUE
var last := 0.0
var flash := 0.0


static func attach(target: Character, colour: Color, guarding := false) -> ShieldAura:
	var existing := target.get_node_or_null("ShieldAura") as ShieldAura
	if existing and !existing.is_queued_for_deletion():
		existing.tint = colour
		existing.sheet = SHEET_YELLOW if guarding else SHEET_BLUE
		existing.cast_arc(Vector2.ZERO)
		return existing
	var aura := ShieldAura.new()
	aura.name = "ShieldAura"
	aura.who = target
	aura.tint = colour
	aura.sheet = SHEET_YELLOW if guarding else SHEET_BLUE
	target.add_child(aura)
	return aura


func _ready() -> void:
	z_index = 1
	position = Vector2(0, -10)
	last = who.shield
	who.shield_changed.connect(on_changed)
	who.shield_absorbed.connect(cast_arc)
	cast_arc(Vector2.ZERO)


## Phát vòng cung khiên: từ hướng `from` đánh tới thì xoay về phía đó; (0,0) = bao quanh người lúc dựng khiên.
func cast_arc(from: Vector2) -> void:
	var at := global_position
	var turn := 0.0
	var size := 1.7
	if from != Vector2.ZERO:
		var dir := (from - who.global_position).normalized()
		at += dir * 9.0
		turn = dir.angle() + PI / 2.0
		size = 1.2
	var fx := OneShotFx.spawn(who.get_parent(), at, sheet, SHEET_FRAMES, 18.0, size)
	fx.rotation = turn
	fx.z_index = 2


func on_changed() -> void:
	if who.shield <= 0.0:
		# Vỡ hoặc hết giờ: mảnh vỡ bay ra.
		OneShotFx.spawn(who.get_parent(), global_position, SHATTER, SHATTER_FRAMES, 14.0, 1.2).z_index = 2
		queue_free()
		return
	if who.shield < last:
		flash = FLASH_SEC
	last = who.shield


func _process(delta: float) -> void:
	flash = maxf(0.0, flash - delta)
	queue_redraw()


func _draw() -> void:
	if !who or who.shield_max <= 0.0:
		return
	var fill := clampf(who.shield / who.shield_max, 0.0, 1.0)
	# Sắp hết giờ thì nhấp nháy.
	var fading := who.shield_left < 1.5 and int(who.shield_left * 8.0) % 2 == 0
	var alpha := (0.1 + 0.25 * fill) * (0.5 if fading else 1.0) + (0.35 if flash > 0.0 else 0.0)
	var radius := 11.0 + 6.0 * fill
	var squash := Vector2(1.0, 0.85)
	draw_set_transform(Vector2.ZERO, 0.0, squash)
	draw_circle(Vector2.ZERO, radius, Color(tint, alpha))
	draw_arc(Vector2.ZERO, radius, 0.0, TAU, 32, Color(tint, minf(1.0, alpha * 3.0)), 1.0)
