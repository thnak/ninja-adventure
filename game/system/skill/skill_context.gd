## Dữ liệu một lần dùng kỹ năng: ai dùng, từ đâu, hướng nào. Các bước (SkillStep) chỉ đọc cái này.
class_name SkillContext
extends RefCounted

var caster: Node2D
var team: ResourceDamageTeam
var pos: Vector2
var dir: Vector2
## Nút chứa vật sinh ra (cùng cha với người dùng để sắp xếp theo y).
var parent: Node
