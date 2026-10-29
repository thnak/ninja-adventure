## Bắn một hoặc nhiều đạn bay thẳng (phi tiêu, kunai, cầu lửa).
class_name StepProjectile
extends SkillStep

@export var texture: Texture2D
@export var hframes := 1
@export var fps := 12.0
@export var speed := 200.0
@export var travel := 160.0
@export var count := 1
## Góc giữa hai viên liền nhau (độ).
@export var spread_deg := 0.0
@export var damage: ResourceDamage
## Số mục tiêu có thể xuyên qua trước khi dừng.
@export var pierce := 0
@export var hit_radius := 4.0
@export var forward := 8.0
## Xoay hình theo hướng bay (kunai, mũi tên).
@export var face_dir := false
## Tốc độ tự xoay (độ/giây) cho phi tiêu.
@export var spin_deg := 0.0
## Đuôi hình bóng mờ nối theo sau (alpha 0 = không có) và đuôi tàn lửa.
@export var trail := Color(0, 0, 0, 0)
@export var embers := Color(0, 0, 0, 0)
## Vụ nổ khi trúng hoặc hết tầm (không bắt buộc).
@export var impact: StepStrike


func run(ctx: SkillContext) -> void:
	for i in count:
		var angle := deg_to_rad((i - (count - 1) / 2.0) * spread_deg)
		Projectile.spawn(ctx.parent, ctx.pos + ctx.dir * forward, ctx.dir.rotated(angle), self, ctx.team)
