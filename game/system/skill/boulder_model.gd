## Mô hình toán cho đá tảng: động năng bị vật cản "khấu trừ" dần, xung của vụ nổ cộng dồn.
##
##   E = m·v² / (2·K)                      động năng (K = ENERGY_SCALE chỉ để số gọn)
##   Gặp vật cản độ bền S:
##       E > S  -> vật cản vỡ, đá còn E − S và tiếp tục lăn
##       E ≤ S  -> vật cản trụ lại (chịu E), đá dừng hẳn
##   Cần ceil(E / S) lớp cùng độ bền S để chặn một viên đá có năng lượng E.
##   Vụ nổ cách tâm đá d, lực đẩy F:  Δv = F · J · f(d) / m,   f(d) = clamp(1 − d/R, fmin, 1)
##   Nhiều vụ nổ cộng Δv theo vectơ nên đẩy được vật nặng, hoặc triệt tiêu / đảo chiều vận tốc đang lăn.
##   Chạy chậm dần đều theo ma sát: quãng dừng = v² / (2·drag).
class_name BoulderModel
extends RefCounted

const ENERGY_SCALE := 1000.0
## Hệ số đổi push_force của đòn thành xung lượng.
const BLAST_IMPULSE := 60.0
## Tầm ảnh hưởng của vụ nổ lên đá và hệ số sàn ở rìa.
const BLAST_REACH := 48.0
const BLAST_MIN_FALLOFF := 0.25
## Từ tốc độ này trở lên đá nghiền nát mọi thứ nó chạm.
const CRUSH_SPEED := 30.0


static func energy(mass: float, speed: float) -> float:
	return mass * speed * speed / (2.0 * ENERGY_SCALE)


static func speed_for(mass: float, e: float) -> float:
	return sqrt(maxf(e, 0.0) * 2.0 * ENERGY_SCALE / mass)


## Va vào một vật cản độ bền s: năng lượng còn lại và vật cản có vỡ không.
static func hit(e: float, s: float) -> Dictionary:
	if e > s:
		return {"energy": e - s, "broken": true}
	return {"energy": 0.0, "broken": false, "absorbed": e}


## Đá năng lượng e đi qua lần lượt các lớp độ bền `strengths`.
## Trả về số lớp bị phá, năng lượng còn lại và chỉ số lớp chặn được đá (-1 nếu đá xuyên hết).
static func chain(e: float, strengths: Array) -> Dictionary:
	var broken := 0
	for i in strengths.size():
		var r := hit(e, strengths[i])
		if !r.broken:
			return {"broken": broken, "energy": 0.0, "stopped_at": i}
		e = r.energy
		broken += 1
	return {"broken": broken, "energy": e, "stopped_at": -1}


static func layers_to_stop(e: float, s: float) -> int:
	return ceili(e / s)


static func stopping_distance(speed: float, drag: float) -> float:
	return speed * speed / (2.0 * drag)


static func blast_falloff(dist: float) -> float:
	return clampf(1.0 - dist / BLAST_REACH, BLAST_MIN_FALLOFF, 1.0)


static func blast_delta_v(mass: float, push_force: float, dist: float) -> float:
	return push_force * BLAST_IMPULSE * blast_falloff(dist) / mass
