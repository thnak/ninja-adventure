## Đẩy một tảng đá lăn thẳng về phía trước (chiêu của boss). Có vệt đỏ báo trước đường lăn.
class_name StepBoulder
extends SkillStep

@export var mass := 100.0
@export var speed := 190.0
@export var drag := 60.0
@export var forward := 30.0
## Thời gian báo trước trước khi đá bị đẩy đi.
@export var windup := 0.8
@export var lane_length := 220.0
@export var lane_width := 26.0


func run(ctx: SkillContext) -> void:
	var start := ctx.pos + ctx.dir * forward
	var lane := Line2D.new()
	lane.width = lane_width
	lane.default_color = Color(1.0, 0.15, 0.1, 0.22)
	lane.add_point(start)
	lane.add_point(start + ctx.dir * lane_length)
	lane.z_index = -1
	ctx.parent.add_child(lane)
	lane.global_position = Vector2.ZERO
	await ctx.caster.get_tree().create_timer(windup, false).timeout
	if is_instance_valid(lane):
		lane.queue_free()
	if is_instance_valid(ctx.caster) and is_instance_valid(ctx.parent) and ctx.parent.is_inside_tree():
		Boulder.spawn(ctx.parent, start, ctx.dir * speed, ctx.team, mass, drag)
