extends Node
class_name Interactor
## Gắn vào người chơi. Chọn vật tương tác gần nhất (ưu tiên theo hướng nhìn) và phát lệnh khi bấm phím.


const ACTIONS:Array[StringName] = [&"interact", &"pickup", &"use_tool"]
const FACING_BONUS = 6.0

var candidates:Array[Interactable] = []
var current_tool:StringName = &""
var best:Interactable


func _init() -> void:
	name = "Interactor"


func add(i:Interactable) -> void:
	if !candidates.has(i):
		candidates.append(i)


func remove(i:Interactable) -> void:
	candidates.erase(i)
	if best == i:
		i.show_hint(false)
		best = null


func _process(_delta:float) -> void:
	candidates = candidates.filter(func(c): return is_instance_valid(c))
	var chosen := pick(null)
	if chosen != best:
		if best and is_instance_valid(best):
			best.show_hint(false)
		best = chosen
		if best:
			best.show_hint(true)


## Vật gần nhất thoả `action` (null = mọi hành động), cộng thưởng nếu nằm đúng hướng nhìn.
func pick(action) -> Interactable:
	var player := get_parent() as Node2D
	var facing:Vector2 = player.sprite.direction if "sprite" in player else Vector2.DOWN
	var result:Interactable
	var best_score := INF
	for c in candidates:
		if action != null and c.action != action:
			continue
		if !c.can_interact(player):
			continue
		if c.action == &"use_tool" and c.required_tool != current_tool:
			continue
		var offset := c.global_position - player.global_position
		var score := offset.length() - FACING_BONUS * facing.dot(offset.normalized())
		if score < best_score:
			best_score = score
			result = c
	return result


func _unhandled_input(event:InputEvent) -> void:
	for action in ACTIONS:
		if event.is_action_pressed(action):
			var target := pick(action)
			if target:
				target.interact(get_parent())
				get_viewport().set_input_as_handled()
			return
