#include "ui/audio.hpp"

#include <array>
#include <string>

#include "raylib.h"

namespace mmo::ui {
namespace {

// Same search order as the atlas loader, so running from the repo root or from build/ both work.
[[nodiscard]] std::string find_asset(const char* rel) {
    for (const char* prefix : {"assets/audio/", "../assets/audio/", "../../assets/audio/"}) {
        std::string path = std::string(prefix) + rel;
        if (FileExists(path.c_str())) return path;
    }
    return {};
}

constexpr const char* kSfxFiles[static_cast<int>(Sfx::kCount)] = {
    "ui_click.wav",
    "build.wav",
    "harvest.wav",
    "hit.wav",
    "swing.wav",
    "swing_heavy.wav",
    "cast.wav",
    "shoot.wav",
    "combo.wav",
    "levelup.wav",
    // RFC-012 §4.6 — not yet copied into assets/audio/ (the licensed source pack isn't fetched in
    // this environment, and file selection is explicitly an asset pass, not a spec decision — RFC
    // Open Question 3). Missing ⇒ `loaded[i]=false` ⇒ every play call below is a silent no-op,
    // exactly like any other Sfx whose file is absent.
    "telegraph_commit.wav",
    "telegraph_imminent.wav",
    "boss_telegraph_commit.wav",
    "boss_telegraph_imminent.wav",
    "interrupt.wav",
    "impact_fire.wav",
    "impact_ice.wav",
    "impact_rock.wav",
    "impact_thunder.wav",
    "hit_heavy.wav",
};

// The looping track's own level when it is on. Kept as a named constant so the ctor and the music
// toggle agree on what "on" sounds like.
constexpr float kMusicVol = 0.35f;

// --- RFC-012 §4.3 voice management --------------------------------------------------------------

// The eleven high-concurrency world-sourced kinds, each backed by its own small alias pool. This
// is a documented simplification of §4.3 step 4's "one shared pool" for the four impact-element
// cues: raylib's `LoadSoundAlias` ties an alias to a single source `Sound` (one file), so a pool
// that plays any of four different clips needs one physical alias array per file regardless; here
// each element gets its own `kAliasesPerCue`-sized pool rather than four elements sharing one
// four-voice budget. Behaviourally near-identical (still bounded, still steals/ducks) — the RFC's
// own reasoning for "one pool" is only that the four are mutually exclusive per event, not an
// explicit combined-budget requirement.
constexpr Sfx kPooledKinds[] = {
    Sfx::kHit,       Sfx::kCombo,     Sfx::kHitHeavy,
    Sfx::kTelegraphCommit,     Sfx::kTelegraphImminent,
    Sfx::kBossTelegraphCommit, Sfx::kBossTelegraphImminent,
    Sfx::kImpactFire, Sfx::kImpactIce, Sfx::kImpactRock, Sfx::kImpactThunder,
};
constexpr int kPooledCount = static_cast<int>(sizeof(kPooledKinds) / sizeof(kPooledKinds[0]));
constexpr int kAliasesPerCue = 4;  // (tunable, §4.3 step 4)
constexpr float kDuckFactor = 0.5f;  // (tunable, §4.3 step 5)
constexpr float kDuckMs = 300.0f;    // (tunable, §4.3 step 5)

[[nodiscard]] int pool_index_of(Sfx s) noexcept {
    for (int i = 0; i < kPooledCount; ++i) {
        if (kPooledKinds[i] == s) return i;
    }
    return -1;
}

struct Voice {
    Sound alias{};
    bool valid = false;       // alias successfully created (device up and the base file loaded)
    std::uint8_t prio = 3;    // priority of whatever is currently sounding on it, for steal/duck
    float gain = 1.0f;        // its un-ducked target gain, so a duck window can restore exactly
    bool ducked = false;
    float duck_ms_left = 0.0f;
};

struct Pool {
    std::array<Voice, kAliasesPerCue> voices{};
};

}  // namespace

struct Audio::Impl {
    bool device = false;
    std::array<Sound, static_cast<std::size_t>(Sfx::kCount)> sfx{};
    std::array<bool, static_cast<std::size_t>(Sfx::kCount)> loaded{};
    Music music{};
    bool music_loaded = false;
    std::array<Pool, static_cast<std::size_t>(kPooledCount)> pools{};
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {
    InitAudioDevice();
    impl_->device = IsAudioDeviceReady();
    if (!impl_->device) {
        // Headless CI and machines with no sound card land here. Everything below degrades to a
        // no-op rather than failing the run — the screenshot mode must still work.
        TraceLog(LOG_WARNING, "AUDIO: no device; running silent");
        return;
    }
    for (int i = 0; i < static_cast<int>(Sfx::kCount); ++i) {
        const std::string path = find_asset(kSfxFiles[i]);
        if (path.empty()) {
            TraceLog(LOG_WARNING, "AUDIO: missing %s", kSfxFiles[i]);
            continue;
        }
        impl_->sfx[static_cast<std::size_t>(i)] = LoadSound(path.c_str());
        impl_->loaded[static_cast<std::size_t>(i)] = true;
    }
    for (int p = 0; p < kPooledCount; ++p) {
        const auto si = static_cast<std::size_t>(kPooledKinds[p]);
        if (!impl_->loaded[si]) continue;
        for (Voice& v : impl_->pools[static_cast<std::size_t>(p)].voices) {
            v.alias = LoadSoundAlias(impl_->sfx[si]);
            v.valid = true;
        }
    }
    const std::string theme = find_asset("theme_day.ogg");
    if (!theme.empty()) {
        impl_->music = LoadMusicStream(theme.c_str());
        impl_->music.looping = true;
        impl_->music_loaded = true;
        SetMusicVolume(impl_->music, kMusicVol);
    }
    TraceLog(LOG_INFO, "AUDIO: ready");
}

Audio::~Audio() {
    if (!impl_->device) return;
    for (Pool& pool : impl_->pools) {
        for (Voice& v : pool.voices) {
            if (v.valid) UnloadSoundAlias(v.alias);
        }
    }
    for (int i = 0; i < static_cast<int>(Sfx::kCount); ++i) {
        if (impl_->loaded[static_cast<std::size_t>(i)]) {
            UnloadSound(impl_->sfx[static_cast<std::size_t>(i)]);
        }
    }
    if (impl_->music_loaded) UnloadMusicStream(impl_->music);
    CloseAudioDevice();
}

bool Audio::ready() const { return impl_->device; }

void Audio::play(Sfx s, float gain, float pitch, float pan) const {
    const auto i = static_cast<std::size_t>(s);
    if (!impl_->device || i >= impl_->loaded.size() || !impl_->loaded[i]) return;
    SetSoundVolume(impl_->sfx[i], gain);
    SetSoundPitch(impl_->sfx[i], pitch);
    SetSoundPan(impl_->sfx[i], pan);
    PlaySound(impl_->sfx[i]);
}

void Audio::play_world_cue(Sfx s, std::uint8_t prio, float gain, float pitch, float pan) const {
    if (!impl_->device) return;
    const int pi = pool_index_of(s);
    if (pi < 0) {
        // Not one of the eleven pooled kinds (e.g. kInterrupt, exempt per §4.2) — a plain,
        // unmanaged play is exactly what §4.6 says these kinds keep doing.
        play(s, gain, pitch, pan);
        return;
    }
    const auto si = static_cast<std::size_t>(s);
    if (si >= impl_->loaded.size() || !impl_->loaded[si]) return;
    Pool& pool = impl_->pools[static_cast<std::size_t>(pi)];

    int free_voice = -1;
    int steal_voice = -1;
    std::uint8_t steal_prio = 0;  // the worst (highest-numbered) priority among busy voices
    for (int i = 0; i < kAliasesPerCue; ++i) {
        Voice& v = pool.voices[static_cast<std::size_t>(i)];
        if (!v.valid) continue;
        if (!IsSoundPlaying(v.alias)) {
            free_voice = i;
            break;
        }
        if (steal_voice < 0 || v.prio > steal_prio) {
            steal_voice = i;
            steal_prio = v.prio;
        }
    }
    int target = free_voice;
    // §4.3 step 4: steal only the lowest-priority currently-sounding alias, and only if the
    // incoming event strictly outranks it — a steal that would lose information is refused, and
    // the new event is dropped instead (never queued).
    if (target < 0 && steal_voice >= 0 && prio < steal_prio) target = steal_voice;
    if (target < 0) return;

    Voice& v = pool.voices[static_cast<std::size_t>(target)];
    if (IsSoundPlaying(v.alias)) StopSound(v.alias);
    SetSoundVolume(v.alias, gain);
    SetSoundPitch(v.alias, pitch);
    SetSoundPan(v.alias, pan);
    PlaySound(v.alias);
    v.prio = prio;
    v.gain = gain;
    v.ducked = false;
    v.duck_ms_left = 0.0f;

    // §4.3 step 5: while a P0 event sounds, every OTHER concurrently-sounding P2/P3 voice across
    // every pool gets ducked — a threat aimed at the local player must never be masked by ambient
    // noise from someone else's fight.
    if (prio == 0) {
        for (Pool& p : impl_->pools) {
            for (Voice& other : p.voices) {
                if (!other.valid || &other == &v) continue;
                if (!IsSoundPlaying(other.alias)) continue;
                if (other.prio < 2) continue;  // only P2/P3 get ducked
                if (!other.ducked) SetSoundVolume(other.alias, other.gain * kDuckFactor);
                other.ducked = true;
                other.duck_ms_left = kDuckMs;
            }
        }
    }
}

void Audio::start_music() const {
    if (!impl_->device || !impl_->music_loaded) return;
    PlayMusicStream(impl_->music);
}

void Audio::update(float dt_ms) const {
    if (!impl_->device) return;
    if (impl_->music_loaded) UpdateMusicStream(impl_->music);
    for (Pool& pool : impl_->pools) {
        for (Voice& v : pool.voices) {
            if (!v.valid || !v.ducked) continue;
            v.duck_ms_left -= dt_ms;
            if (v.duck_ms_left <= 0.0f || !IsSoundPlaying(v.alias)) {
                if (IsSoundPlaying(v.alias)) SetSoundVolume(v.alias, v.gain);
                v.ducked = false;
            }
        }
    }
}

void Audio::set_master_volume(float v01) const {
    if (!impl_->device) return;
    const float v = v01 < 0.0f ? 0.0f : (v01 > 1.0f ? 1.0f : v01);
    SetMasterVolume(v);
}

void Audio::set_music_enabled(bool on) const {
    if (!impl_->device || !impl_->music_loaded) return;
    // Muting by volume rather than pausing keeps the stream fed and running, so turning it back on is
    // instant and in time with the world clock rather than restarting the track from its head.
    SetMusicVolume(impl_->music, on ? kMusicVol : 0.0f);
}

}  // namespace mmo::ui
