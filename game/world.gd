extends Node2D
class_name World


const PLAYER_SCENE = preload("res://content/character/ninja_blue/ninja_blue.tscn")
const PORTAL_COOLDOWN_MS = 400

@export var stating_weapon:ResourceWeapon
## Tắt trong bài kiểm tra để không đọc/ghi save thật.
@export var use_save := true
@export var autosave_interval := 180.0
@export var autosave_retry := 5.0

var map:Map
var player:Character
var travelling := false
var arrived_at_ms := 0
var play_time := 0.0

@onready var rain: GPUParticles2D = %Rain
@onready var snow: GPUParticles2D = %Snow
@onready var cloud: GPUParticles2D = %Cloud
@onready var leaf: GPUParticles2D = %Leaf
@onready var raylight: GPUParticles2D = %Raylight
@onready var fog: TextureRect = %Fog

@onready var transition: ColorRect = %Transition
@onready var music: AudioStreamPlayer = %Music
@onready var color_correction: ColorRect = %ColorCorrection
@onready var camera_grid: CameraGrid = %CameraGrid
@onready var pivot: Node2D = %Pivot
@onready var player_ui: Control = %PlayerUi


func _ready():
	add_to_group("world")
	camera_grid.animation_finished.connect(on_camera_animation_finished)
	player = PLAYER_SCENE.instantiate()
	player.add_child(PlayerAssignor.new())
	player.add_child(Interactor.new())
	var caster := SkillCaster.new()
	player.add_child(caster)
	var skill_bar := SkillBar.new()
	player_ui.add_child(skill_bar)
	skill_bar.position = Vector2(3, 22)
	skill_bar.bind(caster)
	add_child(Dialog.new())
	add_child(MapScreen.new())
	if !(use_save and load_saved()):
		if !load_area(WorldGraph.start_scene(),WorldGraph.start_spawn()):
			push_error("World: cannot load start area")
	if use_save:
		var autosave := Autosave.new()
		autosave.interval_sec = autosave_interval
		autosave.retry_sec = autosave_retry
		add_child(autosave)


func _process(delta:float) -> void:
	play_time += delta


## Nạp save nếu có và khu đã lưu còn tồn tại. Trả về false thì World bắt đầu từ đầu.
func load_saved() -> bool:
	var data := SaveGame.read()
	if data.is_empty() or !WorldGraph.has_area(StringName(data.player.get("scene",""))):
		return false
	SaveGame.apply(data)
	play_time = data.get("play_time",0.0)
	var pos:Array = data.player.pos
	var facing:Array = data.player.get("facing",[0,1])
	return load_area(StringName(data.player.scene),&"",Vector2(pos[0],pos[1]),Vector2(facing[0],facing[1]))


func on_camera_animation_finished():
	pivot.position = camera_grid.global_position


## Bắt đầu đổi khu nếu được. Trả về false khi chưa thể (đang đổi khu hoặc vừa mới đến): người gọi nên thử lại.
## Quá trình đổi khu chạy nền (fade tối, thay map, đặt người chơi tại SpawnPoint, fade sáng).
func travel(scene_id:StringName,spawn_id:StringName) -> bool:
	if travelling or Time.get_ticks_msec() - arrived_at_ms < PORTAL_COOLDOWN_MS:
		return false
	if !WorldGraph.has_area(scene_id):
		push_error("World.travel: unknown area '%s'" % scene_id)
		return true
	travelling = true
	run_travel(scene_id,spawn_id)
	return true


func run_travel(scene_id:StringName,spawn_id:StringName) -> void:
	player.move_vector = Vector2.ZERO
	player.process_mode = Node.PROCESS_MODE_DISABLED
	var fx:Transition = get_tree().get_first_node_in_group("transition")
	if fx:
		await fx.play(Transition.Type.FADE_IN)
	load_area(scene_id,spawn_id)
	player.process_mode = Node.PROCESS_MODE_INHERIT
	if fx:
		await fx.play(Transition.Type.FADE_OUT)
	travelling = false


## Thay map hiện tại bằng khu `scene_id`. Chỉ một map nằm trong bộ nhớ tại một thời điểm.
func load_area(scene_id:StringName,spawn_id:StringName,at_position := Vector2.INF,facing := Vector2.ZERO) -> bool:
	var packed := load(WorldGraph.scene_path(scene_id)) as PackedScene
	if packed == null:
		push_error("World.load_area: cannot load '%s'" % scene_id)
		return false
	if player.get_parent():
		player.get_parent().remove_child(player)
	if map:
		map.queue_free()
	map = packed.instantiate()
	add_child(map)
	FireField.of(map)
	map.environment_area.environment_changed.connect(apply_environment)
	var spawn := map.find_spawn(spawn_id)
	if spawn == null and at_position == Vector2.INF:
		push_error("World.load_area: '%s' has no spawn '%s'" % [scene_id,spawn_id])
	map.actors_root().add_child(player)
	if at_position != Vector2.INF:
		player.global_position = at_position
		if facing != Vector2.ZERO:
			player.sprite.direction = facing
	elif spawn:
		player.global_position = spawn.global_position
		player.sprite.direction = spawn.facing
	for camera in get_tree().get_nodes_in_group("camera"):
		camera.teleport_to(player.global_position)
		camera.set_world_position(player.global_position)
	GameState.visit(scene_id)
	arrived_at_ms = Time.get_ticks_msec()
	return true


func apply_environment(resource_environment:ResourceEnvironment):
	# METEO
	if !resource_environment:
		return
	rain.emitting = ResourceEnvironment.Meteo.RAIN in resource_environment.meteo_list
	snow.emitting = ResourceEnvironment.Meteo.SNOW in resource_environment.meteo_list
	cloud.emitting = ResourceEnvironment.Meteo.CLOUD in resource_environment.meteo_list
	leaf.emitting = ResourceEnvironment.Meteo.LEAF in resource_environment.meteo_list
	fog.active = ResourceEnvironment.Meteo.FOG in resource_environment.meteo_list
	raylight.emitting = ResourceEnvironment.Meteo.RAY in resource_environment.meteo_list
	# MUSIC
	if resource_environment.music:
		music.change_music(resource_environment.music)
	else:
		music.stop_music()
	
	# GRADIENT
	color_correction.gradient = resource_environment.color_gradient


func play_transition(type:Transition.Type):
	transition
