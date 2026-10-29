extends CanvasLayer
class_name MapScreen
## Bản đồ lớn hai cấp. Phím `open_map` mở/đóng. Cấp thế giới: các vùng; chọn vùng rồi `interact` để xem các khu.
## Vùng/khu chỉ hiện khi đã đến hoặc kề một khu đã đến (khu chưa đến hiện dưới dạng "?"). Vị trí lấy từ world_graph.json.


enum Level {WORLD, REGION}

const COLOR_BG = Color(0.05, 0.07, 0.1, 0.97)
const COLOR_REGION = Color("7fb069")
const COLOR_SPECIAL = Color("c77dff")
const COLOR_UNKNOWN = Color("6b7280")
const COLOR_LINK = Color("9aa5b1")
const COLOR_SELECT = Color("ffd166")
const COLOR_HERE = Color("ef476f")
const NODE_RADIUS = 34.0

var canvas:Control
var level := Level.WORLD
var region_id:StringName
var selected := 0


func _ready() -> void:
	layer = 30
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS
	add_to_group("map_screen")
	canvas = Control.new()
	canvas.set_anchors_preset(Control.PRESET_FULL_RECT)
	canvas.mouse_filter = Control.MOUSE_FILTER_IGNORE
	canvas.draw.connect(draw_map)
	add_child(canvas)


func is_open() -> bool:
	return visible


func current_area() -> StringName:
	var world := get_tree().get_first_node_in_group("world") as World
	return world.map.scene_id if world and world.map else &""


func open() -> void:
	level = Level.WORLD
	region_id = &""
	selected = max(0, items().find(WorldGraph.region_of(current_area())))
	visible = true
	get_tree().paused = true
	canvas.queue_redraw()


func close() -> void:
	visible = false
	get_tree().paused = false


## Mục đang hiển thị ở cấp hiện tại: vùng (cấp thế giới) hoặc khu (cấp vùng).
func items() -> Array:
	var result := []
	if level == Level.WORLD:
		for r in WorldGraph.region_ids():
			for a in WorldGraph.areas_in(StringName(r)):
				if GameState.is_known(a):
					result.append(StringName(r))
					break
	else:
		for a in WorldGraph.areas_in(region_id):
			if GameState.is_known(a):
				result.append(a)
	return result


func region_visited(r:StringName) -> bool:
	for a in WorldGraph.areas_in(r):
		if GameState.has_visited(a):
			return true
	return false


func back() -> void:
	if level == Level.REGION:
		var r := region_id
		level = Level.WORLD
		region_id = &""
		selected = max(0, items().find(r))
		canvas.queue_redraw()
	else:
		close()


func enter() -> void:
	if level != Level.WORLD or items().is_empty():
		return
	region_id = items()[selected]
	level = Level.REGION
	selected = max(0, items().find(current_area()))
	canvas.queue_redraw()


func move(step:int) -> void:
	var count := items().size()
	if count > 0:
		selected = posmod(selected + step, count)
		canvas.queue_redraw()


func _unhandled_input(event:InputEvent) -> void:
	if event.is_action_pressed(&"open_map"):
		if visible:
			back() if level == Level.REGION else close()
		elif !get_tree().paused:
			open()
		else:
			return
		get_viewport().set_input_as_handled()
	elif visible:
		if event.is_action_pressed(&"ui_cancel"):
			back()
		elif event.is_action_pressed(&"ui_right") or event.is_action_pressed(&"ui_down"):
			move(1)
		elif event.is_action_pressed(&"ui_left") or event.is_action_pressed(&"ui_up"):
			move(-1)
		elif event.is_action_pressed(&"interact") or event.is_action_pressed(&"ui_accept"):
			enter()
		else:
			return
		get_viewport().set_input_as_handled()


func draw_map() -> void:
	var size := canvas.size
	canvas.draw_rect(Rect2(Vector2.ZERO, size), COLOR_BG)
	if level == Level.WORLD:
		draw_world_level()
	else:
		draw_region_level()
	var hint := "←→ chọn    [E] xem vùng    [M] / Esc đóng" if level == Level.WORLD else "←→ chọn khu    Esc quay lại"
	text(hint, Vector2(size.x / 2, size.y - 36), 22, Color.WHITE, 900)


func draw_world_level() -> void:
	text("BẢN ĐỒ THẾ GIỚI", Vector2(canvas.size.x / 2, 70), 36, Color.WHITE, 900)
	var regions := items()
	var here := WorldGraph.region_of(current_area())
	# Liên kết giữa các vùng: suy ra từ liên kết giữa khu thuộc hai vùng khác nhau.
	for pair in WorldGraph.links():
		var ra := WorldGraph.region_of(StringName(pair[0]))
		var rb := WorldGraph.region_of(StringName(pair[1]))
		if ra != rb and regions.has(ra) and regions.has(rb):
			canvas.draw_line(WorldGraph.region_pos(ra), WorldGraph.region_pos(rb), COLOR_LINK, 4.0)
	for i in regions.size():
		var r:StringName = regions[i]
		var pos := WorldGraph.region_pos(r)
		var visited := region_visited(r)
		var color := COLOR_UNKNOWN if !visited else (COLOR_SPECIAL if WorldGraph.region_special(r) else COLOR_REGION)
		draw_node(pos, color, WorldGraph.region_special(r), i == selected)
		text(WorldGraph.region_name(r) if visited else "?", pos + Vector2(0, NODE_RADIUS + 34), 26, Color.WHITE, 300)
		if r == here:
			draw_here(pos)


func draw_region_level() -> void:
	text(WorldGraph.region_name(region_id).to_upper(), Vector2(canvas.size.x / 2, 70), 36, Color.WHITE, 900)
	var areas := items()
	var here := current_area()
	for pair in WorldGraph.links():
		var a := StringName(pair[0])
		var b := StringName(pair[1])
		var a_in := areas.has(a)
		var b_in := areas.has(b)
		if a_in and b_in:
			canvas.draw_line(WorldGraph.area_pos(a), WorldGraph.area_pos(b), COLOR_LINK, 4.0)
		elif a_in != b_in:
			# Lối ra ngoài vùng: đoạn ngắn kèm tên vùng bên kia.
			var inside := a if a_in else b
			var outside := b if a_in else a
			if GameState.is_known(outside):
				var p := WorldGraph.area_pos(inside)
				var dir := Vector2.RIGHT if WorldGraph.area_pos(outside).x >= p.x else Vector2.LEFT
				canvas.draw_line(p + dir * NODE_RADIUS, p + dir * 150, COLOR_LINK, 4.0)
				text(("→ " if dir.x > 0 else "← ") + WorldGraph.region_name(WorldGraph.region_of(outside)), p + dir * 150 + Vector2(0, 30), 20, COLOR_LINK, 260)
	for i in areas.size():
		var a:StringName = areas[i]
		var pos := WorldGraph.area_pos(a)
		var visited := GameState.has_visited(a)
		var color := COLOR_UNKNOWN if !visited else (COLOR_SPECIAL if WorldGraph.region_special(region_id) else COLOR_REGION)
		draw_node(pos, color, false, i == selected)
		text(WorldGraph.area_name(a) if visited else "?", pos + Vector2(0, NODE_RADIUS + 34), 26, Color.WHITE, 300)
		if a == here:
			draw_here(pos)


func draw_node(pos:Vector2, color:Color, special:bool, is_selected:bool) -> void:
	if special:
		var d := NODE_RADIUS * 1.25
		var points := PackedVector2Array([pos + Vector2(0, -d), pos + Vector2(d, 0), pos + Vector2(0, d), pos + Vector2(-d, 0)])
		canvas.draw_colored_polygon(points, color)
		if is_selected:
			canvas.draw_polyline(points + PackedVector2Array([points[0]]), COLOR_SELECT, 5.0)
	else:
		canvas.draw_circle(pos, NODE_RADIUS, color)
		if is_selected:
			canvas.draw_arc(pos, NODE_RADIUS + 6, 0, TAU, 48, COLOR_SELECT, 5.0)


func draw_here(pos:Vector2) -> void:
	var tip := pos + Vector2(0, -NODE_RADIUS - 14)
	canvas.draw_colored_polygon(PackedVector2Array([tip, tip + Vector2(-12, -20), tip + Vector2(12, -20)]), COLOR_HERE)
	text("Bạn ở đây", tip + Vector2(0, -30), 20, COLOR_HERE, 240)


## Vẽ chữ canh giữa quanh x của `pos` (pos.y là đường cơ sở).
func text(s:String, pos:Vector2, font_size:int, color:Color, width:float) -> void:
	canvas.draw_string(ThemeDB.fallback_font, Vector2(pos.x - width / 2.0, pos.y), s, HORIZONTAL_ALIGNMENT_CENTER, width, font_size, color)
