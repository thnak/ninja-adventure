# Máu, khiên và cái chết của người chơi: khiên hấp thụ trước máu rồi vỡ/hết giờ, thế thủ chặn đẩy lùi,
# đòn chí mạng và đá nghiền xuyên khiên, hết máu thì chết, màn hình chết cho hồi sinh về điểm xuất phát.
# Chạy: tools\test_interact.ps1 -Test vitals   (cần cửa sổ, không headless)
extends SceneTree

var failures := 0
var out_dir := "."


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_dir = args[0]
	GameState.reset()
	var world: World = (load("res://world.tscn") as PackedScene).instantiate()
	world.use_save = false
	root.add_child(world)
	run(world)


func check(cond: bool, what: String) -> void:
	print(("PASS " if cond else "FAIL "), what)
	if !cond:
		failures += 1


func frames(n: int) -> void:
	for i in n:
		await physics_frame


func shot(name: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_viewport().get_texture().get_image().save_png("%s/vitals_%s.png" % [out_dir, name])


func hit(who: Character, amount: int, push := 0, lethal := false) -> void:
	var d := ResourceDamage.new()
	d.amount = amount
	d.push_force = push
	d.lethal = lethal
	who.hitbox.take_damage(d, who.global_position + Vector2(-20, 0))


func use(caster: SkillCaster, skill_id: String) -> bool:
	for s in caster.known:
		if s.id == StringName(skill_id):
			var slot: StringName = SkillCaster.SLOTS.find_key(s.kind)
			caster.current[slot] = s
			caster.ready_at.clear()
			caster.chakra = caster.max_chakra
			caster.character.action_left = 0.0
			return caster.try_use(slot)
	return false


func run(world: World) -> void:
	await frames(60)
	for npc in get_nodes_in_group("npc"):
		npc.wander_radius = 0.0
		npc.global_position += Vector2(0, 600)
	var player := world.player
	var caster: SkillCaster
	for n in player.get_children():
		if n is SkillCaster:
			caster = n
	check(caster.known.size() == 28, "28 skills known (%d)" % caster.known.size())
	var heart_bar: Control = world.player_ui.receptacle_bar
	check(player.life.max_life == 12.0 and player.life.life == 12.0, "the player starts with 12 HP")
	check(heart_bar.max_life == 12 and heart_bar.life == 12, "the heart bar shows the full 12")

	# Sát thương trừ máu và cập nhật thanh tim.
	var hurts: Array = []
	player.hurt.connect(func(a): hurts.append(a))
	hit(player, 3)
	check(player.life.life == 9.0 and hurts == [3.0], "3 damage takes 3 HP (%s, %s)" % [player.life.life, hurts])
	check(heart_bar.life == 9, "the heart bar follows (%d)" % heart_bar.life)

	# Khiên mỏng: hấp thụ trước, phần dư mới trừ máu, vỡ thì hào quang mất.
	check(use(caster, "chakra_ward"), "the player casts the chakra ward")
	await frames(5)
	check(player.shield == 5.0 and !player.guard, "the ward gives 5 shield points, no guard (%s)" % player.shield)
	check(player.get_node_or_null("ShieldAura") != null, "a glowing aura surrounds the caster")
	await shot("ward")
	await frames(40)
	hit(player, 2)
	await frames(4)
	await shot("ward_block")
	check(player.shield == 3.0 and player.life.life == 9.0 and hurts.size() == 1, "the shield soaks 2 and HP is untouched (shield %s, hp %s)" % [player.shield, player.life.life])
	hit(player, 5)
	check(player.shield == 0.0 and player.life.life == 7.0, "the overflow of a 5-hit past 3 shield costs 2 HP (hp %s)" % player.life.life)
	await frames(3)
	await shot("ward_break")
	check(player.get_node_or_null("ShieldAura") == null or player.get_node("ShieldAura").is_queued_for_deletion(), "a broken shield drops its aura")

	# Khiên hết giờ thì tự mất.
	player.add_shield(4.0, 0.3)
	await frames(30)
	check(player.shield == 0.0, "an expired shield disappears")

	# Kim chung: có thế thủ nên không bị đẩy lùi, nhưng vẫn hấp thụ như khiên thường.
	check(use(caster, "golden_bell"), "the player casts the golden bell")
	await frames(5)
	check(player.shield == 12.0 and player.guard and player.shield_guards, "the bell gives 12 points and a guard")
	player.velocity = Vector2.ZERO
	hit(player, 4, 200)
	check(player.velocity.length() < 1.0 and player.shield == 8.0, "a guarded hit neither pushes nor costs HP (v=%.0f, shield %s)" % [player.velocity.length(), player.shield])
	await shot("bell")
	hit(player, 2, 0, true)
	check(player.dead, "a lethal blow ignores the shield")
	check(player.shield == 8.0 or player.shield == 0.0, "the shield was not spent on a lethal blow")
	check(player.life.life == 7.0, "HP was not touched by a lethal blow")
	check(world.death_screen.showing, "the death screen opens")
	check(player.get_node_or_null("ShieldAura") == null or player.get_node("ShieldAura").is_queued_for_deletion(), "a dead body drops its shield")
	await frames(90)
	await shot("death")
	hit(player, 3)
	check(player.life.life == 7.0, "a dead player cannot be hurt any more")

	# Hồi sinh: máu đầy, hết khiên, về điểm xuất phát.
	await frames(80)
	var pressed := InputEventAction.new()
	pressed.action = &"interact"
	pressed.pressed = true
	Input.parse_input_event(pressed)
	await frames(5)
	check(!player.dead and player.life.life == 12.0 and player.shield == 0.0, "respawn restores the player (hp %s)" % player.life.life)
	check(!world.death_screen.showing and heart_bar.life == 12, "the death screen closes and the hearts refill")
	check(player.sprite.scale == Vector2.ONE and player.collision_layer == 1, "the body is back to normal")

	# Chết vì hết máu (không phải chí mạng): nguyên nhân "slain".
	var causes: Array = []
	player.died.connect(func(c): causes.append(c))
	hit(player, 12)
	check(player.dead and causes == [&"slain"], "running out of HP is death by being slain: %s" % [causes])

	# Đá nghiền xuyên cả khiên dày.
	world.respawn()
	await frames(5)
	check(!player.dead, "the player is up again")
	check(use(caster, "golden_bell"), "the bell is cast again")
	await frames(5)
	var rock := Boulder.spawn(player.get_parent(), player.global_position + Vector2(-40, 0), Vector2(120, 0), load("res://content/team/enemy_team.tres"))
	player.sprite.direction = Vector2.RIGHT
	await frames(60)
	check(player.dead and player.life.life > 0.0, "a rolling boulder kills through a full shield without touching HP (hp %s)" % player.life.life)
	if is_instance_valid(rock):
		rock.free()

	print("failures=", failures)
	quit(1 if failures else 0)
