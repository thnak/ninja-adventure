## Rải nhiều đòn xuống các điểm ngẫu nhiên trong một vòng, nối tiếp nhau theo thời gian
## (mưa thiên thạch, bão sấm). Mỗi đòn con tự có vòng cảnh báo nếu đặt windup.
class_name StepScatter
extends SkillStep

@export var inner: StepStrike
@export var count := 8
@export var radius := 80.0
## Tổng thời gian trải đều các đòn.
@export var span := 2.0
## Dịch tâm vòng ra phía trước người dùng.
@export var center_forward := 0.0


func run(ctx: SkillContext) -> void:
	var rng := RandomNumberGenerator.new()
	var centre := ctx.pos + ctx.dir * center_forward
	for i in count:
		var at := centre + Vector2.from_angle(rng.randf() * TAU) * radius * sqrt(rng.randf())
		var wait := span * i / maxf(count, 1.0)
		if wait <= 0.0:
			inner.strike(ctx.parent, at, ctx.dir, ctx.team)
		else:
			strike_later(ctx, at, wait)


func strike_later(ctx: SkillContext, at: Vector2, wait: float) -> void:
	await ctx.caster.get_tree().create_timer(wait, false).timeout
	if is_instance_valid(ctx.caster) and is_instance_valid(ctx.parent) and ctx.parent.is_inside_tree():
		inner.strike(ctx.parent, at, ctx.dir, ctx.team)
