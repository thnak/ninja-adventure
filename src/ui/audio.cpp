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
};

}  // namespace

struct Audio::Impl {
    bool device = false;
    std::array<Sound, static_cast<std::size_t>(Sfx::kCount)> sfx{};
    std::array<bool, static_cast<std::size_t>(Sfx::kCount)> loaded{};
    Music music{};
    bool music_loaded = false;
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
    const std::string theme = find_asset("theme_day.ogg");
    if (!theme.empty()) {
        impl_->music = LoadMusicStream(theme.c_str());
        impl_->music.looping = true;
        impl_->music_loaded = true;
        SetMusicVolume(impl_->music, 0.35f);
    }
    TraceLog(LOG_INFO, "AUDIO: ready");
}

Audio::~Audio() {
    if (!impl_->device) return;
    for (int i = 0; i < static_cast<int>(Sfx::kCount); ++i) {
        if (impl_->loaded[static_cast<std::size_t>(i)]) {
            UnloadSound(impl_->sfx[static_cast<std::size_t>(i)]);
        }
    }
    if (impl_->music_loaded) UnloadMusicStream(impl_->music);
    CloseAudioDevice();
}

bool Audio::ready() const { return impl_->device; }

void Audio::play(Sfx s) const {
    const auto i = static_cast<std::size_t>(s);
    if (!impl_->device || i >= impl_->loaded.size() || !impl_->loaded[i]) return;
    PlaySound(impl_->sfx[i]);
}

void Audio::start_music() const {
    if (!impl_->device || !impl_->music_loaded) return;
    PlayMusicStream(impl_->music);
}

void Audio::update() const {
    if (!impl_->device || !impl_->music_loaded) return;
    UpdateMusicStream(impl_->music);
}

}  // namespace mmo::ui
