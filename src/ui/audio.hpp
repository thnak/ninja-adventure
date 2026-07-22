// Audio — a tiny wrapper over raylib's raudio.
//
// Scope is deliberately small: a handful of one-shots plus one looping track. Anything more (mixing
// buses, positional falloff, dynamic music) is not what makes a game feel alive at this stage; a
// click when you press a button and a thud when something is hit is.
//
// The files live in `assets/audio/` and are COPIED there from the CC0 source packs by hand rather
// than referenced out of `assets/_src/` — `_src` is not committed, and the game must run from a
// clean checkout.
#pragma once

#include <cstdint>
#include <memory>

namespace mmo::ui {

enum class Sfx : std::uint8_t {
    kUiClick,
    kBuild,
    kHarvest,
    kHit,
    kCount,
};

class Audio {
public:
    Audio();
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // No-ops when the device or the file failed to open, so a missing asset degrades to silence
    // rather than to a crash.
    void play(Sfx s) const;
    void start_music() const;
    void update() const;  // must be called once a frame to keep the music stream fed

    [[nodiscard]] bool ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmo::ui
