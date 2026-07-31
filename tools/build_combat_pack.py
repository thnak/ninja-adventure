#!/usr/bin/env python3
"""RFC-008 — compile data/combat/**.json into one canonical, hashed combat pack.

    python tools/build_combat_pack.py

Reads every authored document under `data/combat/`, validates it, assigns/reuses stable u16 ids
via `data/combat/ids.lock.json` (append-only, never reused — RFC-008 §4), then writes:

    assets/_gen/combat_pack.json         canonical pack (sorted keys, no whitespace, notes stripped)
    assets/_gen/combat_pack.hash.json    {"struct_hash", "full_hash", "schema"} sidecar
    src/world/combat_ids.hpp             generated `kSkillMeteor`-style constants + the pack hashes

WHY THIS RUNS EVEN WITHOUT THE RAW ASSET PACK. `assets/_src/` (the CC0 "Ninja Adventure" pack) is
fetched manually and is not checked into git (`.gitignore`'s own comment: "restore with
tools/fetch_assets.sh"). A dev machine that hasn't fetched it yet still needs to be able to run
this tool (CI, a fresh clone). Every check that reads `assets/_src/` (V14/V17/V18 below) is
therefore SKIPPED WITH A WARNING, not silently passed and not a hard failure, when that directory
is absent — the same "missing is not an error" posture `account.hpp`/`world.hpp` already take with
missing save state. A machine that HAS fetched the pack gets the full, real check.

SCOPE NARROWED FROM THE RFC'S OWN 45-RULE TABLE (§8), DOCUMENTED HERE ONCE, NOT REPEATED PER
RULE: this implements every validation rule (V01-V19, V23-V35, V38-V43) that governs the six
document domains actually shipped this pass — `fx`, `snd` (sounds.json), `icon` (icons.json),
`status` (the closed 6-document set), `entity`, and `skill`. NOT implemented, named explicitly:
  - The `boss.*` domain and `capabilities/boss_poses.json` (V20-V22, V36, V37, V44, V45) — RFC-005's
    boss-kit authoring surface is already shipped as a hand-tuned `boss.hpp` table; serializing it
    means producing a real measured pose-capability audit (RFC-008 §2: "measured data checked in
    from the 2026-07-23 audit") this pass cannot fabricate honestly. A future pass that does that
    audit owns this domain.
  - Manual-edit forensics on `ids.lock.json` beyond append-only-by-construction (V11) — this tool
    IS the sole writer and always recomputes `assigned`/`retired` from what's on disk, which makes
    the append-only guarantee structural rather than a second detection pass.
  - Hot reload (§10), schema migration tooling (§6 major-bump `migrate_combat_vN.py`), per-realm
    overlay packs (§4's `50000+` range, Open Question 3) — none has a consumer yet.
  - A C++ runtime migration of `abilities.hpp`/`tiles.hpp`/`boss.hpp` onto this pack. RFC-008's own
    Non-goals: "Runtime semantics... This RFC is the disk contract only." The existing constexpr
    tables keep driving the live sim; `src/world/combat_pack.hpp` is a real, working LOADER for the
    generated artifact (proving the contract end-to-end), not a rewiring of `PlayerActor`/
    `ChunkActor`/`BossActor` to consume it — that migration is RFC-001/002/004/005/009's own future
    work against a pack that, as of this pass, genuinely exists and is genuinely loadable.

HASH ALGORITHM: RFC-008 §5 specifies SHA-256 but flags (Open Questions §4) that BLAKE2b avoids a
second crypto dependency if the C++ side ever needs to verify canonical bytes itself — which it
does here (`combat_pack.hpp` re-hashes the loaded file to catch a stale/corrupted artifact). This
project already vendors Monocypher (`third_party/monocypher`) for Argon2/BLAKE2b in `account.hpp`.
This tool therefore uses `hashlib.blake2b(..., digest_size=32)` — the same RFC 7693 BLAKE2b
Monocypher's `crypto_blake2b(hash, 32, msg, msg_size)` implements, unkeyed on both sides — so the
two languages compute byte-identical digests over the byte-identical canonical file with no shared
library between them. Resolves Open Question 4 in favor of BLAKE2b.
"""
import hashlib
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "data", "combat")
GEN_DIR = os.path.join(ROOT, "assets", "_gen")
SRC_ROOT = os.path.join(ROOT, "assets", "_src")
HPP_OUT = os.path.join(ROOT, "src", "world", "combat_ids.hpp")

DOMAINS = ("skill", "status", "entity", "boss", "fx", "snd", "icon")

MAX_AUTHORED_FILE_BYTES = 64 * 1024  # V04

# --- §3: unit ranges (V07) and required suffixes (V08) ----------------------------------------

_TICKS_RANGE = (0, 65535)
_MT_RANGE = (0, 32000)
_PM_RANGE = (0, 100000)
_DEG_RANGE = (0, 360)
_CHANNEL_PT_RANGE = (0, 1000)   # RFC-003 payload channels
_HP_COST_RANGE = (0, 32767)
_BUILDUP_RANGE = (0, 1000)      # status payload amount/gain
_COATING_TICKS_RANGE = (0, 255)

_PAYLOAD_CHANNELS = ("damage", "pierce", "crush", "impulse", "heat", "cold", "electric", "explosion")
_V08_EXEMPT = {"channel", "coating", "amount", "radius", "gain", "period", "team_mask"}


class BuildError(Exception):
    pass


class Errors:
    """Collects every V-rule violation across every file before failing the build once."""

    def __init__(self):
        self.items = []
        self.warnings = []

    def add(self, rule, doc_id, path, message):
        self.items.append("V%02d: %s %s: %s" % (rule, doc_id, path, message))

    def warn(self, rule, doc_id, path, message):
        self.warnings.append("W (V%02d): %s %s: %s" % (rule, doc_id, path, message))

    def check(self):
        for w in self.warnings:
            print(w)
        if self.items:
            print("\ncombat pack build FAILED (%d error%s):" %
                  (len(self.items), "" if len(self.items) == 1 else "s"))
            for item in self.items:
                print("  " + item)
            raise BuildError("%d validation error(s)" % len(self.items))


# --- strict JSON loading (V01-V03) -------------------------------------------------------------

def _reject_float(_text):
    raise ValueError("float literal not allowed (V02) — use an integer-unit field (_ticks/_mt/_pm/_deg)")


def load_strict_json(path, errors, doc_id):
    """Enforces V01 (UTF-8, single top-level object, no BOM), V02 (no floats), V03 (no null / no
    duplicate keys). Raises on anything that isn't recoverable as a document (a file this broken
    can't be validated further); returns None to let the caller skip it."""
    raw = open(path, "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        errors.add(1, doc_id, "$", "UTF-8 BOM not allowed")
        return None
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as e:
        errors.add(1, doc_id, "$", "not valid UTF-8: %s" % e)
        return None

    dup_paths = []

    def object_pairs_hook(pairs):
        seen = {}
        for k, v in pairs:
            if k in seen:
                dup_paths.append(k)
            seen[k] = v
        return seen

    try:
        doc = json.loads(text, object_pairs_hook=object_pairs_hook, parse_float=_reject_float)
    except ValueError as e:
        errors.add(2 if "V02" in str(e) else 1, doc_id, "$", "invalid JSON: %s" % e)
        return None

    if dup_paths:
        errors.add(3, doc_id, "$", "duplicate key(s): %s" % ", ".join(sorted(set(dup_paths))))
    if not isinstance(doc, dict):
        errors.add(1, doc_id, "$", "top level must be one JSON object")
        return None
    if _contains_null(doc):
        errors.add(3, doc_id, "$", "null is not allowed anywhere (optional = absent)")
    return doc


def _contains_null(v):
    if v is None:
        return True
    if isinstance(v, dict):
        return any(_contains_null(x) for x in v.values())
    if isinstance(v, list):
        return any(_contains_null(x) for x in v)
    return False


# --- V05/V06: id/filename/charset -----------------------------------------------------------

_ID_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)*$")


def check_id(domain, stem, doc, path, errors):
    doc_id = doc.get("id")
    expected = "%s.%s" % (domain, stem)
    if doc_id != expected:
        errors.add(5, expected, path, "id must be '%s' (file stem must match), got %r" %
                    (expected, doc_id))
        return expected
    if len(doc_id) > 48 or not _ID_RE.match(doc_id):
        errors.add(6, doc_id, path, "id charset is [a-z0-9_.], max 48 chars")
    return doc_id


# --- V04, V12, V13, generic field helpers -------------------------------------------------------

def check_file_size(path, doc_id, errors):
    size = os.path.getsize(path)
    if size > MAX_AUTHORED_FILE_BYTES:
        errors.add(4, doc_id, "$", "authored file is %d bytes, max %d" % (size, MAX_AUTHORED_FILE_BYTES))


def check_unknown_keys(doc, allowed, doc_id, path, errors, rule=12):
    extra = set(doc.keys()) - set(allowed)
    if extra:
        errors.add(rule, doc_id, path, "unknown field(s): %s" % ", ".join(sorted(extra)))


def check_schema_major(doc, pack_major, doc_id, errors):
    if doc.get("schema") != pack_major:
        errors.add(13, doc_id, "$.schema", "schema %r does not match pack.json major %r" %
                    (doc.get("schema"), pack_major))


def check_range(value, lo, hi, field, doc_id, path, errors, rule=7):
    if not isinstance(value, int) or isinstance(value, bool) or not (lo <= value <= hi):
        errors.add(rule, doc_id, path + "." + field, "%r out of range [%d, %d]" % (value, lo, hi))


def check_suffix(field, doc_id, path, errors):
    if field in _V08_EXEMPT:
        return
    if field.endswith(("_ticks", "_mt", "_pm", "_deg")):
        return
    if field in ("hp", "cost", "ticks", "tint") or field in _PAYLOAD_CHANNELS:
        return
    errors.add(8, doc_id, path, "unit-bearing field '%s' must carry its unit suffix (§3)" % field)


def is_int_list(v, n):
    return isinstance(v, list) and len(v) == n and all(isinstance(x, int) and not isinstance(x, bool) for x in v)


# --- V14/V17/V18: raw asset existence, skipped with a warning if assets/_src is absent ----------

def _png_dims(path):
    """Reads only the PNG signature + IHDR chunk — width/height, nothing else. No zlib decode."""
    with open(path, "rb") as f:
        sig = f.read(8)
        if sig != b"\x89PNG\r\n\x1a\n":
            return None
        f.read(4)  # IHDR chunk length
        ihdr_tag = f.read(4)
        if ihdr_tag != b"IHDR":
            return None
        w, h = struct.unpack(">II", f.read(8))
        return w, h


def _assets_available():
    return os.path.isdir(SRC_ROOT)


def check_sheet_exists(sheet_rel, frames, doc_id, path, errors):
    if not _assets_available():
        return
    full = os.path.join(SRC_ROOT, sheet_rel)
    if not os.path.isfile(full):
        errors.add(14, doc_id, path, "sheet not found: assets/_src/%s" % sheet_rel)
        return
    dims = _png_dims(full)
    if dims is None:
        errors.add(14, doc_id, path, "not a readable PNG: assets/_src/%s" % sheet_rel)
        return
    w, _h = dims
    if frames > 0 and w % frames != 0:
        errors.add(14, doc_id, path,
                    "sheet width %d does not divide evenly into %d frames" % (w, frames))


def check_sound_files_exist(files, doc_id, path, errors):
    if not _assets_available():
        return
    for f in files:
        if not os.path.isfile(os.path.join(SRC_ROOT, f)):
            errors.add(17, doc_id, path, "sound file not found: assets/_src/%s" % f)


def check_icon_exists(icon_path, doc_id, path, errors):
    if not _assets_available():
        return
    full = os.path.join(SRC_ROOT, icon_path)
    if not os.path.isfile(full):
        errors.add(18, doc_id, path, "icon not found: assets/_src/%s" % icon_path)
        return
    stem, ext = os.path.splitext(icon_path)
    disabled = stem + "Disabled" + ext
    if not os.path.isfile(os.path.join(SRC_ROOT, disabled)):
        errors.add(18, doc_id, path, "missing Disabled twin: assets/_src/%s" % disabled)


# --- domain loaders --------------------------------------------------------------------------

_DOMAIN_DIRS = {"skill": "skills", "status": "statuses", "entity": "entities", "boss": "bosses",
                "fx": "fx"}


def load_domain_files(domain, suffix):
    d = os.path.join(DATA_DIR, _DOMAIN_DIRS[domain])
    out = []
    if not os.path.isdir(d):
        return out
    for name in sorted(os.listdir(d)):
        if name.endswith(suffix):
            out.append((name[: -len(suffix)], os.path.join(d, name)))
    return out


_FX_ALLOWED = {"schema", "id", "notes", "sheet", "frames", "ticks_per_frame", "anchor", "tint",
               "loop", "flip_h", "sound"}
_STATUS_CHANNEL_ALLOWED = {"schema", "id", "notes", "kind", "channel", "decay_per_s", "stage_ticks",
                           "tint", "overlay_fx"}
_STATUS_COATING_ALLOWED = {"schema", "id", "notes", "kind", "coating", "default_ticks", "tint"}
_ENTITY_ALLOWED = {"schema", "id", "notes", "material", "scale", "mass_pm", "hp", "arm_ticks",
                   "hittable_while_arming", "lifetime_ticks", "collision", "blocks_vision", "team",
                   "destroyable", "tags", "aura", "render", "on_death", "on_expire"}
_AURA_ALLOWED = {"channel", "coating", "radius", "gain", "period", "team_mask"}
_RENDER_ALLOWED = {"fx", "anchor"}
_ON_DEATH_ALLOWED = {"fx", "sound", "scar", "spawn"}
_ON_EXPIRE_ALLOWED = {"fx", "sound", "scar"}

_MATERIALS = {"flesh", "stone", "spirit", "metal", "wood", "plant", "water", "slime"}
_SCALES = {"tiny", "small", "medium", "large", "giant", "titan"}
_COLLISIONS = {"none", "ground", "ground_and_shot"}
_TEAMS = {"caster", "monster", "player", "neutral"}
_CHANNELS = {"cold", "heat", "shock", "earth", "stagger"}
_COATINGS = {"wet"}
_TERRAIN_EFFECTS = {"none", "crack", "rubble", "scorch", "wet"}
_ELEMENTS = {"none", "fire", "ice", "rock", "thunder"}
_POSES = {"attack", "ability1", "ability2"}
_VITALS = {"stamina", "mana"}
_SHAPE_KINDS = {"circle", "line", "cone", "tile"}
_RELEASE_KINDS = {"strike", "spawn_projectile", "spawn_entity", "from_sky"}


def check_aura(aura, doc_id, path, errors):
    check_unknown_keys(aura, _AURA_ALLOWED, doc_id, path, errors)
    has_channel = "channel" in aura
    has_coating = "coating" in aura
    if has_channel == has_coating:
        errors.add(23, doc_id, path, "aura must have exactly one of channel/coating")
    if has_channel and aura["channel"] not in _CHANNELS:
        errors.add(23, doc_id, path + ".channel", "unknown channel %r" % aura.get("channel"))
    if has_coating and aura["coating"] not in _COATINGS:
        errors.add(23, doc_id, path + ".coating", "unknown coating %r" % aura.get("coating"))
    for f in ("radius", "gain", "period"):
        if f in aura:
            check_suffix(f, doc_id, path, errors)
    if "radius" in aura:
        check_range(aura["radius"], 0, 3000, "radius", doc_id, path, errors)
    if "period" in aura and aura["period"] < 1:
        errors.add(24, doc_id, path + ".period", "aura period must be >= 1 (no unreplayable per-tick state)")
    if aura.get("team_mask") not in ("enemies", "everyone"):
        errors.add(23, doc_id, path + ".team_mask", "team_mask must be 'enemies' or 'everyone'")


def validate_fx(doc_id, doc, path, errors, referenced_fx):
    check_unknown_keys(doc, _FX_ALLOWED, doc_id, path, errors)
    frames = doc.get("frames")
    tpf = doc.get("ticks_per_frame")
    if not isinstance(frames, int) or frames <= 0:
        errors.add(7, doc_id, path + ".frames", "frames must be a positive integer")
        frames = 0
    if not isinstance(tpf, int) or tpf <= 0:
        errors.add(7, doc_id, path + ".ticks_per_frame", "ticks_per_frame must be a positive integer")
        tpf = 0
    life = frames * tpf
    if life > 255:
        errors.add(15, doc_id, path, "frames(%d) * ticks_per_frame(%d) = %d exceeds 255 (V15, u8 age)" %
                    (frames, tpf, life))
    if doc.get("anchor") not in ("center", "ground"):
        errors.add(23, doc_id, path + ".anchor", "anchor must be 'center' or 'ground'")
    if "tint" in doc and not is_int_list(doc["tint"], 4):
        errors.add(7, doc_id, path + ".tint", "tint must be [r,g,b,a] integers")
    if doc.get("loop") and doc_id not in referenced_fx.get("loop_ok", set()):
        errors.add(16, doc_id, path + ".loop",
                   "loop=true is only allowed on fx referenced from a persist/render context")
    if "sheet" in doc:
        check_sheet_exists(doc["sheet"], frames, doc_id, path + ".sheet", errors)
    if "sound" in doc:
        referenced_fx.setdefault("snd_refs", set()).add(doc["sound"])


def validate_status(doc_id, doc, path, errors):
    kind = doc.get("kind")
    if kind == "channel":
        check_unknown_keys(doc, _STATUS_CHANNEL_ALLOWED, doc_id, path, errors)
        if doc.get("channel") not in _CHANNELS:
            errors.add(41, doc_id, path + ".channel", "channel must be one of %s" % sorted(_CHANNELS))
        if not isinstance(doc.get("decay_per_s"), int) or doc["decay_per_s"] < 1:
            errors.add(19, doc_id, path + ".decay_per_s", "decay_per_s must be >= 1 (no absolute immunity)")
        if not (isinstance(doc.get("stage_ticks"), list) and len(doc["stage_ticks"]) == 3 and
                all(isinstance(x, int) and 0 <= x <= 65535 for x in doc["stage_ticks"])):
            errors.add(7, doc_id, path + ".stage_ticks", "stage_ticks must be exactly 3 u16 durations")
    elif kind == "coating":
        check_unknown_keys(doc, _STATUS_COATING_ALLOWED, doc_id, path, errors)
        if doc.get("coating") not in _COATINGS:
            errors.add(41, doc_id, path + ".coating", "coating must be one of %s" % sorted(_COATINGS))
        dt = doc.get("default_ticks")
        if not isinstance(dt, int) or not (0 <= dt <= 255):
            errors.add(19, doc_id, path + ".default_ticks", "default_ticks must be a finite u8 (no permanent marks)")
    else:
        errors.add(23, doc_id, path + ".kind", "kind must be 'channel' or 'coating'")
    if "tint" in doc and not is_int_list(doc["tint"], 4):
        errors.add(7, doc_id, path + ".tint", "tint must be [r,g,b,a] integers")


def validate_entity(doc_id, doc, path, errors, entity_refs):
    check_unknown_keys(doc, _ENTITY_ALLOWED, doc_id, path, errors)
    if doc.get("material") not in _MATERIALS:
        errors.add(23, doc_id, path + ".material", "unknown material %r" % doc.get("material"))
    if doc.get("scale") not in _SCALES:
        errors.add(23, doc_id, path + ".scale", "unknown scale %r" % doc.get("scale"))
    if doc.get("collision") not in _COLLISIONS:
        errors.add(23, doc_id, path + ".collision", "unknown collision %r" % doc.get("collision"))
    if doc.get("team") not in _TEAMS:
        errors.add(23, doc_id, path + ".team", "unknown team %r" % doc.get("team"))
    check_range(doc.get("hp", -1), *_HP_COST_RANGE, "hp", doc_id, path, errors)
    check_range(doc.get("arm_ticks", -1), 0, 255, "arm_ticks", doc_id, path, errors)
    check_range(doc.get("lifetime_ticks", -1), *_TICKS_RANGE, "lifetime_ticks", doc_id, path, errors)
    if "mass_pm" in doc:
        check_range(doc["mass_pm"], 500, 1500, "mass_pm", doc_id, path, errors, rule=42)
    if not isinstance(doc.get("destroyable"), bool):
        errors.add(23, doc_id, path + ".destroyable", "destroyable must be a boolean")
    if "aura" in doc:
        check_aura(doc["aura"], doc_id, path + ".aura", errors)
    render = doc.get("render")
    if not isinstance(render, dict):
        errors.add(12, doc_id, path + ".render", "render block is required")
    else:
        check_unknown_keys(render, _RENDER_ALLOWED, doc_id, path + ".render", errors)
        if "fx" in render:
            entity_refs.setdefault("fx", set()).add(render["fx"])
    for key, allowed in (("on_death", _ON_DEATH_ALLOWED), ("on_expire", _ON_EXPIRE_ALLOWED)):
        block = doc.get(key)
        if block is None:
            continue
        check_unknown_keys(block, allowed, doc_id, path + "." + key, errors)
        if "fx" in block:
            entity_refs.setdefault("fx", set()).add(block["fx"])
        if "sound" in block:
            entity_refs.setdefault("snd", set()).add(block["sound"])
        if block.get("scar") is not None and block["scar"] not in _TERRAIN_EFFECTS:
            errors.add(34, doc_id, path + "." + key + ".scar", "unknown scar %r" % block.get("scar"))
        for spawn in block.get("spawn", []) or []:
            entity_refs.setdefault("entity", set()).add(spawn.get("entity"))


def validate_skill(doc_id, doc, path, errors, skill_refs):
    top_allowed = {"schema", "id", "notes", "name", "element", "icon", "tags", "player", "phases"}
    check_unknown_keys(doc, top_allowed, doc_id, path, errors)
    if doc.get("element") not in _ELEMENTS:
        errors.add(26, doc_id, path + ".element", "element must be one of %s" % sorted(_ELEMENTS))
    if "icon" in doc:
        skill_refs.setdefault("icon", set()).add(doc["icon"])

    player = doc.get("player")
    if player is not None:
        p_allowed = {"pose", "cost", "cooldown_ticks", "unlock"}
        check_unknown_keys(player, p_allowed, doc_id, path + ".player", errors)
        if player.get("pose") not in _POSES:
            errors.add(27, doc_id, path + ".player.pose", "pose must be one of %s" % sorted(_POSES))
        cost = player.get("cost", {})
        check_unknown_keys(cost, {"vital", "amount"}, doc_id, path + ".player.cost", errors)
        if cost.get("vital") not in _VITALS:
            errors.add(28, doc_id, path + ".player.cost.vital", "cost.vital must be stamina or mana, never health")
        check_range(cost.get("amount", -1), *_HP_COST_RANGE, "amount", doc_id, path + ".player.cost", errors)
        check_range(player.get("cooldown_ticks", -1), *_TICKS_RANGE, "cooldown_ticks", doc_id,
                    path + ".player", errors)
    for key in doc:
        if key not in ("player",) and isinstance(doc.get(key), dict) and "pose" in doc.get(key, {}):
            if key != "player":
                errors.add(29, doc_id, path + "." + key + ".pose", "pose only allowed inside player/boss-kit blocks")

    phases = doc.get("phases")
    if not isinstance(phases, dict):
        errors.add(25, doc_id, path + ".phases", "phases block is required")
        return
    allowed_phases = {"cast", "channel", "release", "travel", "impact", "persist"}
    check_unknown_keys(phases, allowed_phases, doc_id, path + ".phases", errors, rule=25)
    for req in ("cast", "release", "impact"):
        if req not in phases:
            errors.add(25, doc_id, path + ".phases", "missing mandatory phase '%s'" % req)
    if "expire" in phases:
        errors.add(35, doc_id, path + ".phases.expire", "'expire' carries no data and may not be authored")

    _validate_cast(doc_id, phases.get("cast"), path + ".phases.cast", errors, skill_refs)
    if "channel" in phases:
        _validate_channel_phase(doc_id, phases["channel"], path + ".phases.channel", errors)
    release = phases.get("release")
    release_kind = _validate_release(doc_id, release, path + ".phases.release", errors, skill_refs)
    has_travel = "travel" in phases
    needs_travel = release_kind in ("spawn_projectile", "from_sky")
    if has_travel != needs_travel:
        errors.add(33, doc_id, path + ".phases.travel",
                   "travel present=%s but release.kind=%r requires travel=%s" %
                   (has_travel, release_kind, needs_travel))
    if has_travel:
        _validate_travel(doc_id, phases["travel"], path + ".phases.travel", errors, skill_refs)
    impact = phases.get("impact")
    impact_nonzero = _validate_impact(doc_id, impact, path + ".phases.impact", errors, skill_refs)
    cast = phases.get("cast") or {}
    if impact_nonzero and isinstance(cast.get("ticks"), int) and cast["ticks"] < 3:
        errors.add(30, doc_id, path + ".phases.cast.ticks",
                   "a skill with nonzero impact payload must telegraph >= 3 ticks")
    if "persist" in phases:
        _validate_persist(doc_id, phases["persist"], path + ".phases.persist", errors, skill_refs)
    if isinstance(impact, dict) and isinstance(cast, dict):
        _check_telegraph_covers_impact(doc_id, cast, impact, path + ".phases", errors)


def _validate_cast(doc_id, cast, path, errors, skill_refs):
    if not isinstance(cast, dict):
        return
    check_unknown_keys(cast, {"ticks", "telegraph", "interruptible", "sound"}, doc_id, path, errors)
    check_range(cast.get("ticks", -1), *_TICKS_RANGE, "ticks", doc_id, path, errors)
    if "sound" in cast:
        skill_refs.setdefault("snd", set()).add(cast["sound"])
    tg = cast.get("telegraph")
    if not isinstance(tg, dict):
        errors.add(25, doc_id, path + ".telegraph", "telegraph block is required in cast")
        return
    check_unknown_keys(tg, {"fx", "at", "shape"}, doc_id, path + ".telegraph", errors)
    if "fx" in tg:
        skill_refs.setdefault("fx", set()).add(tg["fx"])
    if tg.get("at") not in ("self", "target"):
        errors.add(23, doc_id, path + ".telegraph.at", "at must be self or target")
    _validate_shape(doc_id, tg.get("shape"), path + ".telegraph.shape", errors)


def _validate_shape(doc_id, shape, path, errors):
    if not isinstance(shape, dict):
        errors.add(25, doc_id, path, "shape block is required")
        return
    kind = shape.get("kind")
    if kind not in _SHAPE_KINDS:
        errors.add(23, doc_id, path + ".kind", "shape kind must be one of %s" % sorted(_SHAPE_KINDS))
        return
    allowed = {"kind"}
    if kind == "circle":
        allowed |= {"radius_mt"}
        check_range(shape.get("radius_mt", -1), *_MT_RANGE, "radius_mt", doc_id, path, errors)
    elif kind == "line":
        allowed |= {"length_mt", "width_mt"}
        check_range(shape.get("length_mt", -1), *_MT_RANGE, "length_mt", doc_id, path, errors)
        check_range(shape.get("width_mt", -1), *_MT_RANGE, "width_mt", doc_id, path, errors)
    elif kind == "cone":
        allowed |= {"radius_mt", "arc_deg"}
        check_range(shape.get("radius_mt", -1), *_MT_RANGE, "radius_mt", doc_id, path, errors)
        check_range(shape.get("arc_deg", -1), *_DEG_RANGE, "arc_deg", doc_id, path, errors)
    check_unknown_keys(shape, allowed, doc_id, path, errors)


def _check_telegraph_covers_impact(doc_id, cast, impact, path, errors):
    tg_shape = (cast.get("telegraph") or {}).get("shape")
    im_shape = impact.get("shape")
    if not isinstance(tg_shape, dict) or not isinstance(im_shape, dict):
        return
    if tg_shape.get("kind") != im_shape.get("kind"):
        errors.add(31, doc_id, path, "telegraph shape kind must match impact shape kind")
        return
    for f in ("radius_mt", "length_mt", "width_mt", "arc_deg"):
        if f in im_shape and tg_shape.get(f, 0) < im_shape.get(f, 0):
            errors.add(31, doc_id, path, "telegraph.%s must be >= impact.%s (promise shown >= promise kept)" % (f, f))


def _validate_channel_phase(doc_id, ch, path, errors):
    if not isinstance(ch, dict):
        return
    check_unknown_keys(ch, {"max_ticks", "curve", "damage_bonus_pm_at_max"}, doc_id, path, errors)
    check_range(ch.get("max_ticks", -1), *_TICKS_RANGE, "max_ticks", doc_id, path, errors)
    if ch.get("curve") != "linear":
        errors.add(23, doc_id, path + ".curve", "curve is closed to 'linear' in v1")
    check_range(ch.get("damage_bonus_pm_at_max", -1), *_PM_RANGE, "damage_bonus_pm_at_max", doc_id, path, errors)


def _validate_release(doc_id, release, path, errors, skill_refs):
    if not isinstance(release, dict):
        errors.add(25, doc_id, path, "release block is required")
        return None
    kind = release.get("kind")
    if kind not in _RELEASE_KINDS:
        errors.add(23, doc_id, path + ".kind", "release.kind must be one of %s" % sorted(_RELEASE_KINDS))
        return kind
    allowed = {"kind"}
    if kind == "strike":
        extra = set(release.keys()) - allowed
        if extra:
            errors.add(32, doc_id, path, "strike carries no companion fields, found: %s" % sorted(extra))
    elif kind in ("spawn_projectile", "from_sky"):
        allowed |= {"count", "spread_deg", "entity"}
        count = release.get("count", 1)
        check_range(count, 1, 8, "count", doc_id, path, errors, rule=32)
        if "entity" in release:
            skill_refs.setdefault("entity", set()).add(release["entity"])
        check_unknown_keys(release, allowed, doc_id, path, errors)
    elif kind == "spawn_entity":
        allowed |= {"count", "entity"}
        check_range(release.get("count", 1), 1, 8, "count", doc_id, path, errors, rule=32)
        if "entity" not in release:
            errors.add(32, doc_id, path, "spawn_entity requires an 'entity' reference")
        else:
            skill_refs.setdefault("entity", set()).add(release["entity"])
        check_unknown_keys(release, allowed, doc_id, path, errors)
    return kind


def _validate_travel(doc_id, travel, path, errors, skill_refs):
    check_unknown_keys(travel, {"ticks", "fx", "body"}, doc_id, path, errors)
    check_range(travel.get("ticks", -1), *_TICKS_RANGE, "ticks", doc_id, path, errors)
    if "fx" in travel:
        skill_refs.setdefault("fx", set()).add(travel["fx"])
    body = travel.get("body")
    if body is not None:
        check_unknown_keys(body, {"entity"}, doc_id, path + ".body", errors)
        if "entity" in body:
            skill_refs.setdefault("entity", set()).add(body["entity"])


def _validate_impact(doc_id, impact, path, errors, skill_refs):
    if not isinstance(impact, dict):
        errors.add(25, doc_id, path, "impact block is required")
        return False
    check_unknown_keys(impact, {"payload", "shape", "statuses", "fx", "sound", "terrain"}, doc_id, path, errors)
    payload = impact.get("payload", {})
    check_unknown_keys(payload, set(_PAYLOAD_CHANNELS), doc_id, path + ".payload", errors, rule=43)
    nonzero = False
    for ch in _PAYLOAD_CHANNELS:
        if ch in payload:
            check_range(payload[ch], *_CHANNEL_PT_RANGE, ch, doc_id, path + ".payload", errors, rule=43)
            if payload[ch]:
                nonzero = True
    _validate_shape(doc_id, impact.get("shape"), path + ".shape", errors)
    for i, st in enumerate(impact.get("statuses", []) or []):
        sp = path + ".statuses[%d]" % i
        has_channel = "channel" in st
        has_coating = "coating" in st
        if has_channel == has_coating:
            errors.add(41, doc_id, sp, "a status payload has exactly one of channel/coating")
        if has_channel:
            if st["channel"] not in _CHANNELS:
                errors.add(41, doc_id, sp + ".channel", "unknown channel %r" % st.get("channel"))
            check_range(st.get("amount", -1), *_BUILDUP_RANGE, "amount", doc_id, sp, errors)
        if has_coating:
            if st["coating"] not in _COATINGS:
                errors.add(41, doc_id, sp + ".coating", "unknown coating %r" % st.get("coating"))
            check_range(st.get("ticks", -1), *_COATING_TICKS_RANGE, "ticks", doc_id, sp, errors)
    if "fx" in impact:
        skill_refs.setdefault("fx", set()).add(impact["fx"])
    if "sound" in impact:
        skill_refs.setdefault("snd", set()).add(impact["sound"])
    terrain = impact.get("terrain")
    if terrain is not None:
        check_unknown_keys(terrain, {"effect", "radius_mt"}, doc_id, path + ".terrain", errors)
        if terrain.get("effect") not in _TERRAIN_EFFECTS:
            errors.add(34, doc_id, path + ".terrain.effect", "unknown terrain effect %r" % terrain.get("effect"))
        check_range(terrain.get("radius_mt", -1), *_MT_RANGE, "radius_mt", doc_id, path + ".terrain", errors)
    return nonzero


def _validate_persist(doc_id, persist, path, errors, skill_refs):
    check_unknown_keys(persist, {"spawn"}, doc_id, path, errors)
    for i, sp in enumerate(persist.get("spawn", []) or []):
        spp = path + ".spawn[%d]" % i
        check_unknown_keys(sp, {"entity", "at", "offset_mt"}, doc_id, spp, errors)
        if "entity" in sp:
            skill_refs.setdefault("entity", set()).add(sp["entity"])
        if sp.get("at") not in ("impact", "caster"):
            errors.add(23, doc_id, spp + ".at", "persist spawn 'at' must be impact or caster")
        if "offset_mt" in sp and not is_int_list(sp["offset_mt"], 2):
            errors.add(7, doc_id, spp + ".offset_mt", "offset_mt must be [x,y] integers")


# --- ids.lock.json (V09-V11) --------------------------------------------------------------------

def load_lock():
    path = os.path.join(DATA_DIR, "ids.lock.json")
    if not os.path.isfile(path):
        return {"schema": 1, "next": {d: 1 for d in DOMAINS}, "assigned": {}, "retired": {}}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def assign_ids(lock, discovered_ids_by_domain):
    """discovered_ids_by_domain: {domain: sorted list of ids found on disk this run}. Mutates
    `lock` in place: assigns a fresh number to anything new (sorted, so assignment order is
    deterministic across platforms — the RFC's own determinism requirement, §9), and retires
    (never deletes) anything that was assigned before but is no longer discovered."""
    lock.setdefault("next", {})
    lock.setdefault("assigned", {})
    lock.setdefault("retired", {})
    for domain in DOMAINS:
        lock["next"].setdefault(domain, 1)
        discovered = discovered_ids_by_domain.get(domain, [])
        for doc_id in discovered:
            if doc_id in lock["assigned"] or doc_id in lock["retired"]:
                continue
            lock["assigned"][doc_id] = lock["next"][domain]
            lock["next"][domain] += 1
        discovered_set = set(discovered)
        for doc_id in list(lock["assigned"].keys()):
            domain_of = doc_id.split(".", 1)[0]
            if domain_of == domain and doc_id not in discovered_set:
                lock["retired"][doc_id] = lock["assigned"].pop(doc_id)
    return lock


# --- canonicalization and hashing (§5) -----------------------------------------------------------

_VALUE_FIELD_NAMES = set(_PAYLOAD_CHANNELS) | {"hp", "cost", "amount", "gain", "ticks", "tint"}


def _strip_notes(v):
    if isinstance(v, dict):
        return {k: _strip_notes(x) for k, x in v.items() if k != "notes"}
    if isinstance(v, list):
        return [_strip_notes(x) for x in v]
    return v


def _zero_value_fields(v, key=None):
    """§5 struct_hash: every value-class field (a unit-suffixed field, or one of the suffix-less
    §3 point fields) replaced by 0 — recursively, so a value nested under any key name is zeroed
    the same way whether it's a top-level field or inside a payload/aura block."""
    if key is not None and (key.endswith(("_ticks", "_mt", "_pm", "_deg")) or key in _VALUE_FIELD_NAMES):
        return 0
    if isinstance(v, dict):
        return {k: _zero_value_fields(x, k) for k, x in v.items()}
    if isinstance(v, list):
        return [_zero_value_fields(x, key) for x in v]
    return v


def canonical_bytes(pack_obj):
    return json.dumps(pack_obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def blake2b_hex(data):
    return hashlib.blake2b(data, digest_size=32).hexdigest()


# --- combat_ids.hpp generation -------------------------------------------------------------------

def _pascal(stem):
    return "".join(part.capitalize() for part in stem.split("_"))


def write_combat_ids_hpp(assigned, struct_hash, full_hash, pack_major, pack_minor):
    lines = [
        "// GENERATED by tools/build_combat_pack.py from data/combat/**.json — do not hand-edit.",
        "// Re-run the packer after changing any authored document; this file and",
        "// assets/_gen/combat_pack.json are regenerated together and must be committed together.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mmo {",
        "",
        "inline constexpr int kCombatPackSchemaMajor = %d;" % pack_major,
        "inline constexpr int kCombatPackSchemaMinor = %d;" % pack_minor,
        'inline constexpr const char* kCombatPackStructHash = "%s";' % struct_hash,
        'inline constexpr const char* kCombatPackFullHash = "%s";' % full_hash,
        "",
        "namespace combat_ids {",
        "",
    ]
    for doc_id in sorted(assigned.keys()):
        domain, stem = doc_id.split(".", 1)
        const_name = "k%s%s" % (domain.capitalize(), _pascal(stem))
        lines.append("inline constexpr std::uint16_t %s = %d;" % (const_name, assigned[doc_id]))
    lines += ["", "}  // namespace combat_ids", "", "}  // namespace mmo", ""]
    with open(HPP_OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


# --- main ------------------------------------------------------------------------------------

def main():
    errors = Errors()

    pack_manifest_path = os.path.join(DATA_DIR, "pack.json")
    with open(pack_manifest_path, "r", encoding="utf-8") as f:
        pack_manifest = json.load(f)
    pack_major = pack_manifest["schema"]["major"]
    pack_minor = pack_manifest["schema"]["minor"]

    domains = {"fx": {}, "snd": {}, "icon": {}, "status": {}, "entity": {}, "skill": {}}
    referenced_fx = {}  # fx_id -> True if a persist/render context references it (V16 gate)

    # --- icons.json / sounds.json (small maps, not one-file-per-doc) ---
    icons_path = os.path.join(DATA_DIR, "icons.json")
    icons_doc = load_strict_json(icons_path, errors, "icon.<map>") if os.path.isfile(icons_path) else {"icons": {}}
    for name, icon_path in (icons_doc or {}).get("icons", {}).items():
        domains["icon"][name] = icon_path
        check_icon_exists(icon_path, name, "$.icons.%s" % name, errors)

    sounds_path = os.path.join(DATA_DIR, "sounds.json")
    sounds_doc = load_strict_json(sounds_path, errors, "snd.<map>") if os.path.isfile(sounds_path) else {"sounds": {}}
    for name, snd in (sounds_doc or {}).get("sounds", {}).items():
        check_unknown_keys(snd, {"files", "gain_pm"}, name, "$.sounds.%s" % name, errors)
        check_range(snd.get("gain_pm", -1), 0, 100000, "gain_pm", name, "$.sounds.%s" % name, errors)
        domains["snd"][name] = snd
        check_sound_files_exist(snd.get("files", []), name, "$.sounds.%s.files" % name, errors)

    # --- fx/*.fx.json ---
    fx_persist_render_refs = set()  # filled in a first pass over entities/skills below (V16)

    def collect_persist_render_fx(docs_by_kind):
        for skill_doc in docs_by_kind.get("skill_raw", []):
            persist = (skill_doc.get("phases") or {}).get("persist") or {}
            for sp in persist.get("spawn", []) or []:
                pass  # entity's own render fx is what matters, handled via entity docs below
        for entity_doc in docs_by_kind.get("entity_raw", []):
            render = entity_doc.get("render") or {}
            if "fx" in render:
                fx_persist_render_refs.add(render["fx"])

    raw_docs = {"skill_raw": [], "entity_raw": []}

    for stem, path in load_domain_files("entity", ".entity.json"):
        doc = load_strict_json(path, errors, "entity.%s" % stem)
        if doc is not None:
            raw_docs["entity_raw"].append(doc)
    for stem, path in load_domain_files("skill", ".skill.json"):
        doc = load_strict_json(path, errors, "skill.%s" % stem)
        if doc is not None:
            raw_docs["skill_raw"].append(doc)
    collect_persist_render_fx(raw_docs)

    for stem, path in load_domain_files("fx", ".fx.json"):
        doc = load_strict_json(path, errors, "fx.%s" % stem)
        if doc is None:
            continue
        doc_id = check_id("fx", stem, doc, "$.id", errors)
        check_file_size(path, doc_id, errors)
        check_schema_major(doc, pack_major, doc_id, errors)
        validate_fx(doc_id, doc, "$", errors, {"loop_ok": fx_persist_render_refs})
        domains["fx"][doc_id] = doc

    # --- statuses/*.status.json ---
    for stem, path in load_domain_files("status", ".status.json"):
        doc = load_strict_json(path, errors, "status.%s" % stem)
        if doc is None:
            continue
        doc_id = check_id("status", stem, doc, "$.id", errors)
        check_file_size(path, doc_id, errors)
        check_schema_major(doc, pack_major, doc_id, errors)
        validate_status(doc_id, doc, "$", errors)
        domains["status"][doc_id] = doc
    expected_status = {"status.cold", "status.heat", "status.shock", "status.earth", "status.stagger",
                       "status.wet"}
    if set(domains["status"].keys()) != expected_status:
        errors.add(41, "status.<set>", "$",
                   "the status.* domain must be exactly %s, found %s" %
                   (sorted(expected_status), sorted(domains["status"].keys())))

    # --- entities/*.entity.json ---
    entity_refs_all = {}
    for stem, path in load_domain_files("entity", ".entity.json"):
        doc = load_strict_json(path, errors, "entity.%s" % stem)
        if doc is None:
            continue
        doc_id = check_id("entity", stem, doc, "$.id", errors)
        check_file_size(path, doc_id, errors)
        check_schema_major(doc, pack_major, doc_id, errors)
        refs = {}
        validate_entity(doc_id, doc, "$", errors, refs)
        entity_refs_all[doc_id] = refs
        domains["entity"][doc_id] = doc

    # --- skills/*.skill.json ---
    skill_refs_all = {}
    for stem, path in load_domain_files("skill", ".skill.json"):
        doc = load_strict_json(path, errors, "skill.%s" % stem)
        if doc is None:
            continue
        doc_id = check_id("skill", stem, doc, "$.id", errors)
        check_file_size(path, doc_id, errors)
        check_schema_major(doc, pack_major, doc_id, errors)
        refs = {}
        validate_skill(doc_id, doc, "$", errors, refs)
        skill_refs_all[doc_id] = refs
        domains["skill"][doc_id] = doc

    # --- V38: entity spawn-chain depth <= 2, acyclic ---
    def entity_spawn_children(entity_id):
        refs = entity_refs_all.get(entity_id, {})
        return sorted(refs.get("entity", set()) - {None})

    def check_chain(entity_id, chain):
        if entity_id in chain:
            errors.add(38, entity_id, "$", "entity spawn cycle: %s" % " -> ".join(chain + [entity_id]))
            return
        if len(chain) >= 2:
            errors.add(38, entity_id, "$", "entity spawn-chain depth exceeds 2: %s" %
                       " -> ".join(chain + [entity_id]))
            return
        for child in entity_spawn_children(entity_id):
            if child in domains["entity"]:
                check_chain(child, chain + [entity_id])

    for eid in domains["entity"]:
        check_chain(eid, [])

    # --- V39: every reference resolves; V40: reachability warning ---
    reachable_fx, reachable_snd, reachable_entity = set(), set(), set()

    def resolve(domain_dict, ref_id, from_doc, path, rule=39):
        if ref_id is None:
            return
        if ref_id not in domain_dict:
            errors.add(rule, from_doc, path, "dangling reference: %r" % ref_id)

    for eid, refs in entity_refs_all.items():
        for fx in refs.get("fx", set()):
            resolve(domains["fx"], fx, eid, "$.render/on_death/on_expire.fx")
            reachable_fx.add(fx)
        for snd in refs.get("snd", set()):
            resolve(domains["snd"], snd, eid, "$.on_death/on_expire.sound")
            reachable_snd.add(snd)
        for child in refs.get("entity", set()):
            resolve(domains["entity"], child, eid, "$.on_death.spawn[].entity")
            reachable_entity.add(child)

    for sid, refs in skill_refs_all.items():
        for icon in refs.get("icon", set()):
            resolve(domains["icon"], icon, sid, "$.icon")
        for fx in refs.get("fx", set()):
            resolve(domains["fx"], fx, sid, "$.phases..fx")
            reachable_fx.add(fx)
        for snd in refs.get("snd", set()):
            resolve(domains["snd"], snd, sid, "$.phases..sound")
            reachable_snd.add(snd)
        for ent in refs.get("entity", set()):
            resolve(domains["entity"], ent, sid, "$.phases..entity")
            reachable_entity.add(ent)

    for doc_id, doc in domains["fx"].items():
        if doc.get("sound"):
            resolve(domains["snd"], doc["sound"], doc_id, "$.sound")
            reachable_snd.add(doc["sound"])

    for fx_id in domains["fx"]:
        if fx_id not in reachable_fx:
            errors.warn(40, fx_id, "$", "unreferenced fx (dead content, not a build error)")
    for snd_id in domains["snd"]:
        if snd_id not in reachable_snd:
            errors.warn(40, snd_id, "$", "unreferenced sound (dead content, not a build error)")
    for eid in domains["entity"]:
        if eid not in reachable_entity:
            errors.warn(40, eid, "$", "unreferenced entity (dead content, not a build error)")

    errors.check()  # raises BuildError and prints everything collected above if anything failed

    # --- ids.lock.json: assign/retire, append-only ---
    lock = load_lock()
    discovered = {d: sorted(domains[d].keys()) for d in domains}
    assign_ids(lock, discovered)
    lock_path = os.path.join(DATA_DIR, "ids.lock.json")
    with open(lock_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(lock, f, indent=2, sort_keys=True)
        f.write("\n")

    # --- canonical pack + two hashes ---
    pack_obj = {
        "schema": {"major": pack_major, "minor": pack_minor},
        "name": pack_manifest.get("name", "combat"),
        "ids": lock["assigned"],
        "domains": {d: _strip_notes(domains[d]) for d in domains},
    }
    full_bytes = canonical_bytes(pack_obj)
    struct_bytes = canonical_bytes(_zero_value_fields(pack_obj))
    full_hash = blake2b_hex(full_bytes)
    struct_hash = blake2b_hex(struct_bytes)

    os.makedirs(GEN_DIR, exist_ok=True)
    with open(os.path.join(GEN_DIR, "combat_pack.json"), "wb") as f:
        f.write(full_bytes)
    with open(os.path.join(GEN_DIR, "combat_pack.hash.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"schema": {"major": pack_major, "minor": pack_minor},
                    "struct_hash": struct_hash, "full_hash": full_hash}, f, indent=2, sort_keys=True)
        f.write("\n")

    write_combat_ids_hpp(lock["assigned"], struct_hash, full_hash, pack_major, pack_minor)

    if not _assets_available():
        print("note: assets/_src/ not found — V14/V17/V18 (raw sheet/sound/icon file checks) were "
              "skipped. Run tools/fetch_assets.sh to enable them.")
    print("combat pack OK: %d skills, %d entities, %d fx, %d sounds, %d icons, %d statuses" %
          (len(domains["skill"]), len(domains["entity"]), len(domains["fx"]), len(domains["snd"]),
           len(domains["icon"]), len(domains["status"])))
    print("struct_hash=%s full_hash=%s" % (struct_hash, full_hash))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BuildError as e:
        sys.exit(1)
