## Phát tiếng một lần rồi tự xoá.
class_name SkillAudio
extends RefCounted


static func play(host: Node, stream: AudioStream) -> void:
	if !stream or !host.is_inside_tree():
		return
	var player := AudioStreamPlayer.new()
	player.stream = stream
	player.finished.connect(player.queue_free)
	host.get_tree().root.add_child(player)
	player.play()
