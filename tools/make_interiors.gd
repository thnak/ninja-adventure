# Dựng các khu nội thất một màn hình: content/map/dojo.tscn, cave.tscn.
# Chạy: godot --headless --path game --script ../tools/make_interiors.gd
# Một màn hình = tx -9..10, ty -5..5; tường khung ở rìa, lối ra là khe 2 ô ở giữa hàng dưới (tx 0..1).
extends SceneTree

const Kit = preload("res://system/mapgen/map_kit.gd")

const X0 = -9
const X1 = 10
const Y0 = -5
const Y1 = 5
const GAP: Array[int] = [0, 1]


func _initialize() -> void:
	build_dojo()
	build_cave()
	quit()


## Khung tường + sàn + va chạm + lối ra dẫn về `exit_scene`/`exit_spawn` + điểm xuất hiện `spawn_name`.
func room(id: String, wall: Vector2i, floor_tile: Vector2i, exit_scene: String, exit_spawn: String, spawn_name: String) -> Kit:
	var k := Kit.new(id)
	k.fill(Kit.FLOOR, Kit.INTERIOR_FLOOR, floor_tile, X0, Y0, X1, Y1)
	k.wall_ring(wall.x, wall.y, X0, Y0, X1, Y1, GAP)
	# Va chạm của tường: trên, trái, phải, dưới (trừ khe).
	k.collide_rect(Rect2(k.corner(X0, Y0), Vector2(20 * 16, 16)))
	k.collide_rect(Rect2(k.corner(X0, Y0 + 1), Vector2(16, 9 * 16)))
	k.collide_rect(Rect2(k.corner(X1, Y0 + 1), Vector2(16, 9 * 16)))
	k.collide_rect(Rect2(k.corner(X0, Y1), Vector2(9 * 16, 16)))
	k.collide_rect(Rect2(k.corner(2, Y1), Vector2(9 * 16, 16)))
	k.collide_rect(Rect2(k.corner(X0, Y1 + 1), Vector2(20 * 16, 16)))   # chặn phía sau khe
	k.add_env(k.corner(X0, Y0) + Vector2(160, 88), Vector2(320, 176))
	k.add_portal(exit_scene, exit_spawn, Rect2(k.corner(0, Y1), Vector2(32, 16)))
	k.add_spawn(spawn_name, k.center(0, Y1 - 2) + Vector2(8, 0), Vector2.UP)
	return k


func build_dojo() -> void:
	var k := room("dojo", Vector2i(5, 0), Vector2i(12, 1), "village", "from_dojo", "from_village")
	# Thảm 4x4 giữa phòng.
	k.stamp(Kit.FLOOR, Kit.INTERIOR_FLOOR, 15, 0, 4, 4, -1, -2)
	# Giá kiếm dựa tường phía bắc, kệ sách hai bên.
	k.stamp(Kit.WALL2, Kit.HOUSE, 6, 4, 2, 1, -4, -4)
	k.stamp(Kit.WALL2, Kit.HOUSE, 6, 4, 2, 1, 3, -4)
	k.stamp(Kit.WALL2, Kit.ELEMENT, 8, 7, 2, 2, -8, -4)
	k.collide_block(-8, -4, 2, 2)
	k.stamp(Kit.WALL2, Kit.ELEMENT, 10, 7, 2, 2, 8, -4)
	k.collide_block(8, -4, 2, 2)
	k.collide_block(-4, -4, 2, 1)
	k.collide_block(3, -4, 2, 1)
	# Trụ đá và băng rôn quanh sư phụ, chiếu rơm tập luyện, chậu cây, giỏ vũ khí, bàn trà, chum.
	for x in [-2, 2]:
		k.stamp(Kit.WALL, Kit.HOUSE, 7, 17, 1, 2, x, -4)
		k.collide_block(x, -3, 1, 1)
	for bx in [[22, -6], [26, 6]]:
		k.stamp(Kit.WALL2, Kit.HOUSE, bx[0], 20, 1, 2, bx[1], -4)
		k.collide_block(bx[1], -3, 1, 1)
	k.stamp(Kit.FLOOR_DETAIL, Kit.ELEMENT, 13, 4, 3, 3, -7, 0)
	k.stamp(Kit.FLOOR_DETAIL, Kit.ELEMENT, 13, 4, 3, 3, 4, -1)
	for x in [-8, 9]:
		k.stamp(Kit.WALL, Kit.ELEMENT, 0, 7, 1, 2, x, -1)
		k.collide_block(x, 0, 1, 1)
	k.stamp(Kit.WALL, Kit.HOUSE, 14, 18, 1, 2, -8, 2)
	k.collide_block(-8, 3, 1, 1)
	k.stamp(Kit.WALL, Kit.ELEMENT, 2, 10, 3, 1, 5, 3)
	k.collide_block(5, 3, 3, 1)
	k.stamp(Kit.WALL, Kit.ELEMENT, 0, 5, 1, 2, 9, 3)
	k.collide_block(9, 4, 1, 1)
	k.add_sign("Sign_rules", k.center(0, -4) + Vector2(8, 0), "NỘI QUY ĐẠO QUÁN\n1. Cúi chào trước khi bước vào.\n2. Không rút kiếm trong phòng.\n3. Trà nguội thì tự pha lại.", false, "Đọc")
	k.add_chest("Chest_dojo", k.center(-7, 3), &"scroll", 1)
	k.add_npc("Master", "master", k.center(0, -3) + Vector2(8, 0), "Sư phụ",
		"Ngươi đến đúng lúc. Đạo quán đang thiếu người pha trà.\nNếu muốn học kiếm, hãy đi một vòng quanh làng rồi quay lại.")
	k.add_npc("Monk1", "monk", k.center(-6, 1), "Đệ tử", "Sư phụ nghiêm khắc lắm, nhưng trà sư phụ pha thì ngon nhất làng.", 12.0)
	k.add_npc("Monk2", "monk2", k.center(5, 0), "Đệ tử", "Tôi đã quét sân ba lần hôm nay. Lá cứ rơi hoài.", 12.0)
	k.save("res://content/map/dojo.tscn")


func build_cave() -> void:
	var k := room("cave", Vector2i(0, 6), Vector2i(12, 12), "forest", "from_cave", "from_forest")
	# Quặng và pha lê: nhóm lớn 2x2 và pha lê lẻ.
	k.stamp(Kit.WALL, Kit.NATURE, 7, 14, 2, 2, -7, -3)
	k.collide_block(-7, -3, 2, 2)
	k.stamp(Kit.WALL, Kit.NATURE, 9, 14, 2, 2, 7, -3)
	k.collide_block(7, -3, 2, 2)
	k.stamp(Kit.WALL, Kit.NATURE, 7, 16, 2, 2, 6, 1)
	k.collide_block(6, 1, 2, 2)
	for crystal in [[0, 14, -3, -1], [1, 14, 4, -3], [2, 14, -1, 2], [3, 14, 2, 0], [0, 14, 8, 3], [2, 14, -8, 3]]:
		k.stamp(Kit.WALL, Kit.NATURE, crystal[0], crystal[1], 1, 1, crystal[2], crystal[3])
	# Gờ đá chắn ngang giữa phòng (ty 0..1) biến phòng thành lối chữ S; cửa lò cũ, thang, đá vụn.
	k.stamp(Kit.WALL, Kit.NATURE, 16, 5, 4, 3, -8, 0)
	k.stamp(Kit.WALL, Kit.NATURE, 16, 8, 2, 2, -4, 0)
	k.stamp(Kit.WALL, Kit.NATURE, 13, 8, 2, 2, -2, 0)
	k.stamp(Kit.WALL, Kit.NATURE, 16, 8, 2, 2, 0, 0)
	k.stamp(Kit.WALL, Kit.NATURE, 13, 8, 2, 2, 2, 0)
	k.collide_rect(Rect2(k.corner(-8, 0), Vector2(12 * 16, 2 * 16)))
	k.collide_rect(Rect2(k.corner(-8, 2), Vector2(4 * 16, 16)))
	k.stamp(Kit.WALL, Kit.RELIEF, 0, 7, 3, 3, -6, -4)
	k.collide_block(-6, -4, 3, 3)
	k.stamp(Kit.WALL2, Kit.RELIEF, 3, 0, 1, 3, 7, -4)
	k.collide_block(7, -4, 1, 3)
	for d in [[0, 0, -5, 4], [0, 2, 6, 3], [0, 1, -3, -1], [0, 0, 4, -2]]:
		k.stamp(Kit.FLOOR_DETAIL, Kit.RELIEF, d[0], d[1], 1, 1, d[2], d[3])
	k.add_sign("Sign_mine", k.center(-2, 3), "HANG MỎ SƯƠNG\nCẩn thận đá rơi. Quặng đã khai thác thì để lại phần cho người sau.", true)
	k.add_chest("Chest_cave", k.center(0, -3) + Vector2(8, 0), &"iron_ore", 3)
	k.add_npc("Miner", "hunter", k.center(5, 1), "Thợ mỏ", "Hang này sâu hơn tôi tưởng. Tôi đứng ngoài này cho an toàn.", 18.0)
	k.save("res://content/map/cave.tscn")
