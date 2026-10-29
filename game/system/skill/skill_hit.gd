## Cách chung để một đòn gây sát thương lên Hitbox, theo luật đội giống DamageArea.
class_name SkillHit
extends RefCounted


static func apply(hitbox: Hitbox, damage: ResourceDamage, team: ResourceDamageTeam, from: Vector2) -> bool:
	if hitbox.has_meta("boulder"):
		hitbox.get_meta("boulder").blast(damage, team, from)
		return true
	if hitbox.team and team:
		if hitbox.team == team or hitbox.team.is_ally(team):
			return false
	hitbox.take_damage(damage, from)
	return true
