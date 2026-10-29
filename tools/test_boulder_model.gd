# Kiểm tra mô hình toán của đá tảng bằng số, không cần cửa sổ.
# Chạy: tools\godot.ps1 --script ../tools/test_boulder_model.gd
extends SceneTree

var failures := 0


func check(cond: bool, what: String) -> void:
	print(("PASS " if cond else "FAIL "), what)
	if !cond:
		failures += 1


func near(a: float, b: float, tol := 0.01) -> bool:
	return absf(a - b) <= tol


func _initialize() -> void:
	# Động năng và đổi ngược.
	var e := BoulderModel.energy(100.0, 190.0)
	check(near(e, 1805.0), "E(m=100, v=190) = 1805 (%s)" % e)
	check(near(BoulderModel.speed_for(100.0, e), 190.0), "speed_for inverts energy")
	check(near(BoulderModel.energy(100.0, 0.0), 0.0), "a still boulder has no energy")

	# Một vật cản yếu bị phá, đá giữ phần năng lượng dư.
	var r := BoulderModel.hit(1805.0, 640.0)
	check(r.broken and near(r.energy, 1165.0), "a rock layer (S=640) breaks and leaves 1165")
	# Vật cản mạnh hơn năng lượng thì trụ lại và chặn đá.
	r = BoulderModel.hit(525.0, 640.0)
	check(!r.broken and near(r.absorbed, 525.0), "a layer stronger than the remaining energy holds")
	# Hai bên bằng nhau: vật cản trụ (E ≤ S).
	check(!BoulderModel.hit(640.0, 640.0).broken, "equal energy and strength: the layer holds")

	# Nhiều lớp măng đá: cần ceil(E/S) lớp.
	check(BoulderModel.layers_to_stop(1805.0, 640.0) == 3, "3 rock layers are needed against E=1805")
	check(BoulderModel.layers_to_stop(1805.0, 270.0) == 7, "7 ice layers are needed against E=1805")
	var rock := [640.0, 640.0, 640.0]
	var out := BoulderModel.chain(1805.0, rock)
	check(out.broken == 2 and out.stopped_at == 2 and near(out.energy, 0.0), "3 rock layers: breaks 2, stops at the 3rd")
	out = BoulderModel.chain(1805.0, [640.0])
	check(out.broken == 1 and out.stopped_at == -1 and near(out.energy, 1165.0), "1 rock layer is not enough")
	out = BoulderModel.chain(1805.0, [640.0, 640.0])
	check(out.stopped_at == -1 and near(out.energy, 525.0), "2 rock layers leave 525 and the boulder still rolls")
	out = BoulderModel.chain(1805.0, [270.0, 270.0, 270.0])
	check(out.stopped_at == -1, "3 ice layers cannot stop it")
	# Lớp hỗn hợp: tổng độ bền quyết định.
	out = BoulderModel.chain(1805.0, [270.0, 640.0, 270.0, 640.0])
	check(out.stopped_at == 3 or out.stopped_at == -1, "mixed layers resolve in order")
	check(out.broken == 3 and out.stopped_at == 3, "mixed 270+640+270 break, the second 640 holds")

	# Năng lượng còn lại quy ra vận tốc còn lại.
	var left: float = BoulderModel.chain(1805.0, [640.0, 640.0]).energy
	check(near(BoulderModel.speed_for(100.0, left), sqrt(2.0 * 525.0 * 1000.0 / 100.0)), "leftover energy maps back to a lower speed")
	check(BoulderModel.speed_for(100.0, left) < 190.0, "the boulder slows after breaking layers")

	# Quãng dừng do ma sát.
	check(near(BoulderModel.stopping_distance(190.0, 60.0), 300.83, 0.1), "stopping distance v²/2·drag")

	# Vụ nổ: gần tâm đẩy mạnh, xa tâm yếu, ngoài tầm vẫn có sàn.
	check(near(BoulderModel.blast_falloff(0.0), 1.0), "a blast at the centre has full falloff")
	check(near(BoulderModel.blast_falloff(24.0), 0.5), "halfway to the reach it is 0.5")
	check(near(BoulderModel.blast_falloff(500.0), BoulderModel.BLAST_MIN_FALLOFF), "far away the falloff floors out")
	var dv := BoulderModel.blast_delta_v(100.0, 130.0, 0.0)
	check(near(dv, 78.0), "one fireball blast gives dv=78 to a 100-mass boulder (%s)" % dv)
	check(dv < 100.0, "a single blast cannot get a heavy boulder moving fast")
	check(near(BoulderModel.blast_delta_v(100.0, 130.0, 24.0), 39.0), "the same blast 24 px away gives half")
	check(near(BoulderModel.blast_delta_v(4.0, 130.0, 0.0), 1950.0), "a light crate is flung far (dv=1950)")
	# Phối hợp: 3 vụ nổ đủ chặn viên đá đang lăn 190 và đẩy ngược lại.
	check(190.0 - 2.0 * dv > 0.0 and 190.0 - 3.0 * dv < 0.0, "3 combined blasts reverse a 190 px/s boulder, 2 do not")
	check(3.0 * dv > BoulderModel.CRUSH_SPEED * 5.0, "3 blasts push a still boulder past crush speed")

	print("failures=", failures)
	quit(1 if failures else 0)
