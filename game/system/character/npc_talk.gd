extends Interactable
class_name NpcTalk
## Vùng nói chuyện gắn vào Npc. Chỉ đọc chữ, không lưu trạng thái.


var text := ""


func interact(player:Node2D) -> void:
	var npc := get_parent() as Npc
	if npc:
		npc.face(player.global_position)
	say(text)
