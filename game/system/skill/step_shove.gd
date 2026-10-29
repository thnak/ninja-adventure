## Boss tận dụng đá có sẵn trong môi trường: chọn tảng nặng nhất đang nằm yên trong tầm với, báo trước đường trượt
## rồi đẩy nó đi thẳng theo hướng nhìn. Tấm đá trượt như viên gạch (không xoay); năng lượng tỉ lệ khối lượng nên
## tảng càng lớn càng cần nhiều lớp địa hình để chặn.
class_name StepShove
extends SkillStep

@export var reach := 130.0
@export var speed := 170.0
@export var windup := 0.7
@export var lane_length := 200.0


func run(ctx: SkillContext) -> void:
	var rock := pick(ctx)
	if !rock:
		return
	var lane := Line2D.new()
	lane.width = maxf(rock.footprint.y, 16.0)
	lane.default_color = Color(1.0, 0.15, 0.1, 0.22)
	lane.add_point(rock.global_position)
	lane.add_point(rock.global_position + ctx.dir * lane_length)
	lane.z_index = -1
	ctx.parent.add_child(lane)
	lane.global_position = Vector2.ZERO
	await ctx.caster.get_tree().create_timer(windup, false).timeout
	if is_instance_valid(lane):
		lane.queue_free()
	if is_instance_valid(ctx.caster) and is_instance_valid(rock) and rock.speed() < 5.0:
		rock.shove(ctx.dir * speed, ctx.team)


## Tảng nặng nhất đang nằm yên trong tầm với.
func pick(ctx: SkillContext) -> Boulder:
	var best: Boulder
	for node in ctx.caster.get_tree().get_nodes_in_group("boulder"):
		var rock := node as Boulder
		if !rock or rock.speed() > 5.0 or rock.global_position.distance_to(ctx.pos) > reach:
			continue
		if !best or rock.mass > best.mass:
			best = rock
	return best
