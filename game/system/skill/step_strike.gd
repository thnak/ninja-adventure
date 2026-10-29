## Đòn trong một vùng tròn có hiệu ứng hoạt hình: nhát chém, xoáy, vụ nổ, tia sét.
class_name StepStrike
extends SkillStep

@export var texture: Texture2D
@export var hframes := 4
@export var fps := 14.0
## Bỏ qua các khung đầu của tờ hình (vụ nổ có 2 khung "đạn" không dùng).
@export var start_frame := 0
## Khoảng cách từ người dùng tới tâm đòn theo hướng nhìn.
@export var forward := 14.0
@export var radius := 14.0
@export var damage: ResourceDamage
@export var fx_scale := 1.0
## Xoay hình theo hướng dùng (chém); tắt cho hình đối xứng như vụ nổ.
@export var rotate_with_dir := true
## Khung hình đầu/cuối có gây sát thương (-1 = tới hết).
@export var hit_from := 0
@export var hit_to := -1
## Thời gian báo trước: vẽ vòng đỏ cảnh báo vùng đánh rồi mới nổ (dùng cho đòn mạnh, boss).
@export var windup := 0.0
## Vụ nổ lớn: thêm vòng sóng xung kích, khói và mảnh vụn bị hất lên rồi rơi xuống.
@export var shockwave := false
@export var smoke := false
@export var debris := false
## Màu của vòng sóng và khói (băng: xanh, độc: lục).
@export var fx_tint := Color.WHITE
## Bén lửa lên nền và cây trong vùng đánh (cháy lan, để lại dấu cháy xém) / dập lửa trong vùng.
@export var ignites := false
@export var extinguishes := false
@export var sound: AudioStream


func run(ctx: SkillContext) -> void:
	strike(ctx.parent, ctx.pos + ctx.dir * forward, ctx.dir, ctx.team)


func strike(parent: Node, at: Vector2, dir: Vector2, team: ResourceDamageTeam) -> void:
	SkillEffect.spawn(parent, at, dir, self, team)
