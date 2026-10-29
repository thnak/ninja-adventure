## Bộ não của kẻ địch và boss: máy trạng thái + chấm điểm kỹ năng.
##
## Trạng thái: IDLE (canh gác) -> CHASE (truy đuổi, chọn chiêu) -> ACT (hồi phục sau chiêu) -> RETURN (mất dấu, về chỗ cũ)
## và FLEE (bỏ chạy khi máu thấp). Brain chỉ điều khiển `move_vector`, hướng nhìn và gọi SkillCaster.try_use(),
## nên chakra, hồi chiêu, choáng, đóng băng... áp dụng cho AI y hệt người chơi.
##
## Chọn chiêu: mỗi chiêu trong `loadout` được chấm điểm theo khoảng cách tới mục tiêu so với tầm hiệu quả của chiêu,
## chakra dự trữ, tình huống (mục tiêu đang có khiên thì ưu tiên tuyệt kỹ), cộng nhiễu nhẹ để không lặp máy móc.
## Chiêu phòng thủ (khiên, tường) chỉ dùng khi máu thấp hoặc vừa chuyển pha; chiêu đặc biệt cần điều kiện (có đá để đẩy).
## Boss có nhiều pha theo % máu (`phase_hp`): pha càng cao càng nghĩ nhanh, tuyệt kỹ chỉ mở từ `ultimate_phase`.
class_name Brain
extends Node

signal state_changed(state)
signal phase_changed(phase)

enum State {IDLE, CHASE, ACT, RETURN, FLEE}

## Id các kỹ năng được dùng; để trống = dùng mọi kỹ năng đã biết.
@export var loadout: PackedStringArray = []
@export var sight := 140.0
## Đi xa chỗ canh quá mức này thì bỏ cuộc quay về.
@export var leash := 320.0
## Khoảng cách muốn đứng cách mục tiêu khi không ra chiêu.
@export var engage_distance := 26.0
## Đứng xa tầm đánh: lùi lại khi mục tiêu áp sát (cung thủ, pháp sư).
@export var keep_distance := false
@export var think_sec := 0.35
## Máu dưới tỉ lệ này thì bỏ chạy (0 = không bao giờ).
@export var flee_ratio := 0.0
## Ngưỡng máu (giảm dần) chia các pha, ví dụ [0.66, 0.33].
@export var phase_hp: Array[float] = []
@export var ultimate_phase := 1
@export var heal_on_reset := true
## Mất dấu rồi quay về thì tự hồi đầy máu (boss reset).
@export var wander_radius := 0.0
@export var corpse_sec := 3.0

const SCORE_FLOOR := 0.15
const RETURN_TIMEOUT := 12.0
const RECENT_KEPT := 3
const REPEAT_PENALTY := 0.3

var character: Character
var caster: SkillCaster
var state := State.IDLE
var target: Character
var home := Vector2.INF
var phase := 0
## Vừa chuyển pha: chiêu phòng thủ được ưu tiên một lần.
var panic := false
var skills: Array[ResourceSkill] = []
var think_left := 0.0
var scan_left := 0.0
var recover_left := 0.0
var return_left := 0.0
var wander_left := 0.0
var corpse_left := 0.0
## Vài chiêu vừa dùng gần đây: chiêu lặp lại bị trừ điểm để AI đa dạng hơn.
var recent: Array[StringName] = []


func _ready() -> void:
	character = get_parent()
	corpse_left = corpse_sec
	character.hurt.connect(on_hurt)


## Lần đầu chạy: nhận chỗ canh và danh sách chiêu (caster vào cây trước Brain nên đã biết kỹ năng).
func setup() -> bool:
	if caster == null:
		for n in character.get_children():
			if n is SkillCaster:
				caster = n
	if caster == null or caster.character == null:
		return false
	home = character.global_position
	skills.clear()
	for skill in caster.known:
		if loadout.is_empty() or loadout.has(String(skill.id)):
			skills.append(skill)
	return true


func hp_ratio() -> float:
	return character.life.life / maxf(character.life.max_life, 1.0)


func enter(next: State) -> void:
	if state == next:
		return
	state = next
	character.move_vector = Vector2.ZERO
	if next == State.RETURN:
		return_left = RETURN_TIMEOUT
		target = null
	state_changed.emit(next)


func on_hurt(_amount) -> void:
	# Bị đánh từ xa cũng quay lại tìm kẻ đánh.
	if state == State.IDLE or state == State.RETURN:
		var near := nearest_player()
		if near:
			target = near
			enter(State.CHASE)


func players() -> Array[Character]:
	var out: Array[Character] = []
	for node in get_tree().get_nodes_in_group("player"):
		var p := node as Character
		if p and !p.dead:
			out.append(p)
	return out


func nearest_player() -> Character:
	var best: Character
	var best_d := INF
	for p in players():
		var d := p.global_position.distance_to(character.global_position)
		if d < best_d:
			best = p
			best_d = d
	return best


func _physics_process(delta: float) -> void:
	if character.dead:
		character.move_vector = Vector2.ZERO
		corpse_left -= delta
		if corpse_left <= 0.0:
			character.queue_free()
		return
	if home == Vector2.INF and !setup():
		return
	update_phase()
	scan_left -= delta
	if scan_left <= 0.0:
		scan_left = 0.25
		scan()
	match state:
		State.IDLE:
			wander(delta)
		State.CHASE:
			chase(delta)
		State.ACT:
			recover_left -= delta
			character.move_vector = Vector2.ZERO
			if recover_left <= 0.0:
				enter(State.CHASE if is_instance_valid(target) and !target.dead else State.RETURN)
		State.RETURN:
			go_home(delta)
		State.FLEE:
			flee(delta)


func update_phase() -> void:
	var next := 0
	for threshold in phase_hp:
		if hp_ratio() <= threshold:
			next += 1
	if next > phase:
		phase = next
		panic = true
		phase_changed.emit(phase)


## Nhìn quanh: canh gác thì phát hiện người chơi; đang đánh thì kiểm tra mục tiêu còn hợp lệ, hay mất dấu.
func scan() -> void:
	var near := nearest_player()
	match state:
		State.IDLE:
			if near and near.global_position.distance_to(character.global_position) <= sight:
				target = near
				enter(State.CHASE)
		State.CHASE, State.FLEE:
			if near and (!is_instance_valid(target) or target.dead or near.global_position.distance_to(character.global_position) + 24.0 < target.global_position.distance_to(character.global_position)):
				target = near
			if !is_instance_valid(target) or target.dead:
				enter(State.RETURN)
			elif target.global_position.distance_to(character.global_position) > sight * 1.8 or character.global_position.distance_to(home) > leash:
				enter(State.RETURN)


func chase(delta: float) -> void:
	if !is_instance_valid(target) or target.dead:
		enter(State.RETURN)
		return
	var to := target.global_position - character.global_position
	var d := to.length()
	if flee_ratio > 0.0 and hp_ratio() < flee_ratio:
		enter(State.FLEE)
		return
	think_left -= delta
	if think_left <= 0.0 and character.can_act() and character.action_left <= 0.0:
		think_left = think_sec * (1.0 - 0.2 * phase)
		var pick := choose(d, to)
		if pick:
			cast(pick, to)
			return
	if !character.can_act():
		character.move_vector = Vector2.ZERO
		return
	if d > engage_distance + 6.0:
		character.move_vector = to.normalized()
	elif keep_distance and d < engage_distance - 24.0:
		character.move_vector = -to.normalized()
	else:
		character.move_vector = Vector2.ZERO
		if character.action_left <= 0.0:
			character.sprite.direction = to.normalized()


func go_home(delta: float) -> void:
	return_left -= delta
	var to := home - character.global_position
	if to.length() < 8.0 or return_left <= 0.0:
		if return_left <= 0.0:
			character.global_position = home
		if heal_on_reset:
			character.life.life = character.life.max_life
		enter(State.IDLE)
		return
	character.move_vector = to.normalized()


func flee(delta: float) -> void:
	if !is_instance_valid(target) or target.dead:
		enter(State.RETURN)
		return
	var away := character.global_position - target.global_position
	# Thử dựng khiên trước khi chạy.
	think_left -= delta
	if think_left <= 0.0 and character.can_act() and character.action_left <= 0.0:
		think_left = think_sec
		for skill in skills:
			if skill.ai_role == ResourceSkill.AiRole.DEFEND and skills_shield(skill) and character.shield <= 0.0 and caster.can_use(skill):
				cast(skill, -away)
				return
	character.move_vector = away.normalized() if character.can_act() else Vector2.ZERO
	if away.length() > sight * 1.2:
		enter(State.RETURN)


func wander(delta: float) -> void:
	if wander_radius <= 0.0:
		character.move_vector = Vector2.ZERO
		return
	wander_left -= delta
	if wander_left > 0.0:
		return
	if randf() < 0.45:
		character.move_vector = Vector2.ZERO
		wander_left = randf_range(1.5, 3.5)
		return
	var dirs := [Vector2.UP, Vector2.DOWN, Vector2.LEFT, Vector2.RIGHT]
	var dir: Vector2 = dirs[randi() % dirs.size()]
	if character.global_position.distance_to(home) > wander_radius:
		dir = character.global_position.direction_to(home)
	character.move_vector = dir
	wander_left = randf_range(0.5, 1.2)


func cast(skill: ResourceSkill, to: Vector2) -> void:
	character.sprite.direction = to.normalized() if to != Vector2.ZERO else character.sprite.direction
	var slot: StringName = SkillCaster.SLOTS.find_key(skill.kind)
	caster.current[slot] = skill
	if !caster.try_use(slot):
		return
	if skill.ai_role == ResourceSkill.AiRole.DEFEND:
		panic = false
	recent.append(skill.id)
	if recent.size() > RECENT_KEPT:
		recent.pop_front()
	recover_left = skill.lock_sec + 0.25
	character.move_vector = Vector2.ZERO
	if state != State.FLEE:
		enter(State.ACT)


## Chiêu điểm cao nhất (trên ngưỡng), hoặc null nếu chưa nên ra chiêu.
func choose(d: float, to: Vector2) -> ResourceSkill:
	var best: ResourceSkill
	var best_score := SCORE_FLOOR
	for skill in skills:
		var s := score(skill, d, to)
		if s > best_score:
			best = skill
			best_score = s
	return best


## Tiết kiệm chakra: vừa chuyển pha mà chưa đủ chakra dựng khiên thì chỉ dùng chiêu miễn phí; đã mở tuyệt kỹ mà chưa đủ
## chakra thì cũng dành dụm (chiêu rẻ tiền không được ăn mất phần dành cho tuyệt kỹ).
func saving_chakra(skill: ResourceSkill) -> bool:
	if panic and character.shield <= 0.0:
		for other in skills:
			if other.ai_role == ResourceSkill.AiRole.DEFEND and skills_shield(other) and caster.chakra < other.cost:
				return true
	if skill.kind != ResourceSkill.Kind.ULTIMATE and phase >= ultimate_phase and ultimate_phase > 0:
		var cheapest := INF
		for other in skills:
			if other.kind == ResourceSkill.Kind.ULTIMATE and other.ai_role == ResourceSkill.AiRole.ATTACK and other.cost > 0.0 and caster.remaining(other) <= 0.0:
				cheapest = minf(cheapest, other.cost)
		return cheapest < INF and caster.chakra < cheapest
	return false


func skills_shield(skill: ResourceSkill) -> bool:
	return skill.steps.any(func(step): return step is StepShield)


func score(skill: ResourceSkill, d: float, to: Vector2) -> float:
	if !caster.can_use(skill):
		return -1.0
	if skill.kind == ResourceSkill.Kind.ULTIMATE and phase < ultimate_phase and skill.ai_role != ResourceSkill.AiRole.DEFEND:
		return -1.0
	var r := skill.ai_range()
	var noise := randf() * 0.25
	if skill.cost > 0.0 and skill.ai_role != ResourceSkill.AiRole.DEFEND and saving_chakra(skill):
		return -1.0
	match skill.ai_role:
		ResourceSkill.AiRole.DEFEND:
			return score_defend(skill, d, r, noise)
		ResourceSkill.AiRole.SPECIAL:
			return score_special(skill, d, r, to, noise)
	if d < r.x - 4.0 or d > r.y + 8.0:
		return -1.0
	var mid := (r.x + r.y) / 2.0
	var fit := 1.0 - clampf(absf(d - mid) / maxf(r.y - r.x, 1.0), 0.0, 1.0)
	var s := 0.5 + 0.5 * fit
	if skill.kind == ResourceSkill.Kind.ULTIMATE:
		s *= 1.3
	elif skill.kind == ResourceSkill.Kind.THROW:
		s *= 0.9
	if skill.kind != ResourceSkill.Kind.MELEE and caster.chakra - skill.cost < caster.max_chakra * 0.15:
		s -= 0.4
	s -= REPEAT_PENALTY * recent.count(skill.id)
	if is_instance_valid(target) and (target.shield > 0.0 or target.guard):
		if skill.kind == ResourceSkill.Kind.ULTIMATE:
			s += 0.3
		elif skill.kind == ResourceSkill.Kind.THROW:
			s -= 0.2
	return s + noise


func score_defend(skill: ResourceSkill, d: float, r: Vector2, noise: float) -> float:
	if skills_shield(skill):
		if character.shield > 0.0:
			return -1.0
		var urgency := (1.0 - hp_ratio()) * 1.2 + (0.25 if d <= 110.0 else 0.0) + (1.0 if panic else 0.0)
		return urgency + noise if urgency > 0.55 else -1.0
	# Tường chắn: dựng khi mục tiêu còn cách một đoạn và mình đã bị thương (hoặc vừa chuyển pha).
	if d < r.x or d > r.y or (hp_ratio() > 0.7 and !panic):
		return -1.0
	return 0.6 + noise


func score_special(skill: ResourceSkill, d: float, r: Vector2, to: Vector2, noise: float) -> float:
	if d < r.x or d > r.y:
		return -1.0
	for step in skill.steps:
		if step is StepShove:
			return 1.4 + noise if shove_is_useful(step as StepShove, to) else -1.0
	return 1.1 + noise


## Có tảng đá trong tầm với mà đường trượt (thẳng theo hướng nhìn) trúng mục tiêu hay không.
func shove_is_useful(step: StepShove, to: Vector2) -> bool:
	var ctx := SkillContext.new()
	ctx.caster = character
	ctx.team = character.team
	ctx.pos = character.global_position + Vector2(0, -4)
	ctx.dir = to.normalized()
	ctx.parent = character.get_parent()
	var rock := step.pick(ctx)
	if !rock or !is_instance_valid(target):
		return false
	var rel := target.global_position - rock.global_position
	var along := rel.dot(ctx.dir)
	var lateral := absf(rel.cross(ctx.dir))
	return along > 0.0 and along < step.lane_length and lateral < maxf(rock.footprint.y, 16.0) * 1.2
