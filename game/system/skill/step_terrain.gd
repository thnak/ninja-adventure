## Dựng địa hình tạm (tường băng, gai đá): chặn đường, có máu, tự mất sau vài giây.
class_name StepTerrain
extends SkillStep

@export var texture: Texture2D
@export var hframes := 1
@export var fps := 14.0
## Khung hình lúc trồi lên (đầu, cuối) rồi giữ khung cuối.
@export var grow_frames := Vector2i(0, 0)
## Khung hình lúc tan vỡ (đầu, cuối); (-1,-1) = không có, chỉ mờ dần.
@export var break_frames := Vector2i(-1, -1)
@export var count := 3
@export var spacing := 14.0
@export var forward := 22.0
@export var size := Vector2(14, 12)
@export var lifetime := 6.0
@export var max_life := 3.0
## Dịch hình để chân vật nằm trên ô va chạm.
@export var sprite_offset := Vector2(0, -8)
## Đòn gây sát thương lúc địa hình trồi lên (gai đá đâm trúng kẻ đứng đó).
@export var eruption: StepStrike
## Độ bền mỗi điểm máu khi đỡ đá tảng: đá cứng hơn băng (xem BoulderModel).
@export var strength_per_life := 120.0
## Số lớp xếp nối nhau theo hướng dùng và khoảng cách giữa các lớp.
@export var rows := 1
@export var row_gap := 20.0


func run(ctx: SkillContext) -> void:
	var side := ctx.dir.orthogonal()
	for row in rows:
		for i in count:
			var at := ctx.pos + ctx.dir * (forward + row * row_gap) + side * (i - (count - 1) / 2.0) * spacing
			SkillTerrain.spawn(ctx.parent, at, self)
			if eruption:
				eruption.strike(ctx.parent, at, ctx.dir, ctx.team)
