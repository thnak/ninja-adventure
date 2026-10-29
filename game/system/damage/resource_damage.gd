@icon("../damage/icon_resource_damage.png")
extends Resource
class_name ResourceDamage


## Khống chế kèm theo đòn: trói (không đi được), làm chậm, choáng và đóng băng (không đi, không ra chiêu).
enum Status {NONE, ROOT, SLOW, STUN, FREEZE}

@export var amount := 1
@export var push_force := 0
@export var status := Status.NONE
@export var status_sec := 0.0
## Hất tung: thời gian bay (giây) và độ cao (pixel); mục tiêu bay theo hướng đẩy rồi rơi và trượt thêm một đoạn.
@export var launch_sec := 0.0
@export var launch_height := 0.0
## Đòn chí mạng (bị đè, bị nghiền): chết ngay, không qua đỡ đòn hay lá chắn.
@export var lethal := false
