// Audio — a tiny wrapper over raylib's raudio.
//
// One-shots plus one looping track, plus (RFC-012) the minimal voice-management layer this
// project's 20-50-concurrent-player target actually needs: raylib's `PlaySound` has zero
// polyphony per `Sound` (a second play just restarts the same buffer's cursor, cutting the first
// off), so `play_world_cue` backs the eleven high-concurrency Sfx kinds with a small
// `LoadSoundAlias` pool per kind, priority-stealing and P0 ducking instead of the raw restart.
// Local-input cues (the seven verbs already gated on the player's own input) stay on plain `play`
// and never contend for a voice — see audio.hpp's own comment.
//
// The files live in `assets/audio/` and are COPIED there from the CC0 source packs by hand rather
// than referenced out of `assets/_src/` — `_src` is not committed, and the game must run from a
// clean checkout. The ten Sfx kinds RFC-012 adds are not yet copied in (the source pack isn't
// fetched in this environment) — they degrade to silence via the same missing-asset path every
// other Sfx already falls back on; copying real files is a follow-up asset pass (RFC-012 Open
// Question 3 explicitly leaves file selection out of the spec's own scope).
#pragma once

#include <cstdint>
#include <memory>

namespace mmo::ui {

enum class Sfx : std::uint8_t {
    kUiClick,
    kBuild,
    kHarvest,
    kHit,       // a blow landing — finally played, off the effects a fight publishes
    kSwing,     // a light melee swing
    kSwingHeavy,// a charged melee swing
    kCast,      // a spell going out
    kShoot,     // an arrow loosed
    kCombo,     // a combo detonating (the kBlast flash)
    kLevelUp,   // a skill went up a level
    // RFC-012 §4.6 — appended, never inserted, since Sfx is never persisted to disk.
    kTelegraphCommit,        // ordinary monster commits to a wind-up (Light/Heavy pitch-banded, §2.2)
    kTelegraphImminent,      // ordinary monster's last-3-tick warning
    kBossTelegraphCommit,    // boss commits — dedicated file, §2.3
    kBossTelegraphImminent,  // boss imminent warning — repeats once more at windup==1, §2.3
    kInterrupt,              // a wind-up fizzled (stun-cancelled) instead of landing, §2.4
    kImpactFire,
    kImpactIce,
    kImpactRock,
    kImpactThunder,
    kHitHeavy,               // WhirlCleave / CrushBlow's finisher, §3
    kCount,                  // 20
};

class Audio {
public:
    Audio();
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // No-ops when the device or the file failed to open, so a missing asset degrades to silence
    // rather than to a crash. `gain`/`pitch` scale the clip and `pan` follows raylib's own
    // convention (0=left, 0.5=center, 1=right) — the defaults reproduce today's flat playback, so
    // every existing zero-argument call site is unaffected (RFC-012 §4.6).
    void play(Sfx s, float gain = 1.0f, float pitch = 1.0f, float pan = 0.5f) const;

    // RFC-012 §4.3-§4.5 — the world-sourced cue path (telegraph commit/imminent/fizzle, impact
    // cues). Routes the 11 high-concurrency Sfx kinds through a small per-kind alias pool with
    // priority-based stealing (a busy pool claims the incoming event only by evicting a STRICTLY
    // lower-priority voice, never an equal/higher one) and ducks concurrently-sounding P2/P3
    // voices while a P0 event plays (§4.3 step 5). `prio`: 0=P0 (highest) .. 3=P3 (lowest), per
    // §4.4's table. Sfx kinds outside the pooled 11 (the seven local-input cues, kLevelUp,
    // kInterrupt — exempt by construction, §4.2/§4.6) fall back to the plain `play` above. Silently
    // drops the event (never queues) when every voice in its pool is busy at an equal-or-higher
    // priority — a stale cue a tick late is worse than the drop, per §4.3 step 3.
    void play_world_cue(Sfx s, std::uint8_t prio, float gain, float pitch, float pan) const;

    void start_music() const;
    // Must be called once a frame to keep the music stream fed and to decay P0 ducking windows
    // (§4.3 step 5); `dt_ms` may be 0 for callers that don't care about ducking timing.
    void update(float dt_ms = 0.0f) const;

    // Options-screen controls. `set_master_volume` scales EVERYTHING (raylib's SetMasterVolume), and
    // `set_music_enabled` mutes just the looping track by dropping its own volume to zero — so the
    // two are independent: silencing the music leaves the click and combat cues at the master level.
    // `v01` is clamped to 0..1. Both no-op without a device, like the rest of this class.
    void set_master_volume(float v01) const;
    void set_music_enabled(bool on) const;

    [[nodiscard]] bool ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmo::ui
