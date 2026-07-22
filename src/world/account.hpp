// Accounts — the identity a PlayerActor is keyed by.
//
// The whole login design is two sentences: the first node to start the world is the trusted leader
// and owns this table (ARCHITECTURE.md §2), and a name it has never seen before creates an account
// rather than being rejected. That is the Minecraft/Valheim shape — nobody registers, you just join
// — and it is the reason there is no account server, no OIDC and no certificate anywhere in this
// project.
//
// THE ONE THING NOT COMPROMISED ON: passwords are never stored, not even obfuscated. Argon2i via
// Monocypher, a fresh 16-byte salt per account. The threat being defended against is NOT an attacker
// breaking into a friend's game server — it is that **players reuse passwords**, and open-source
// games get their save files shared around. Leaking a plaintext password file would leak your
// friends' email accounts, and that is not a risk this project is entitled to take on their behalf.
//
// WHAT THIS IS NOT. It is not persistence — P5 owns that, and will key its saves by `AccountId`
// rather than by session slot for exactly this reason. The file format below is the minimum that
// survives a restart: a magic number, a version, and a flat array of fixed-size records. No
// migration path is promised and none is needed yet.
//
// Note also what is deliberately absent: any notion of a session token, permission or role. Those
// belong to `quark::Principal` and the `Authorizer` at P6, when there is a wire for them to travel
// over. Inventing them now would mean inventing them twice.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <random>
#include <string_view>
#include <vector>

extern "C" {
#include "monocypher.h"
}

namespace mmo {

inline constexpr int kMaxAccountName = 24;  // including the terminator
inline constexpr int kSaltBytes = 16;
inline constexpr int kHashBytes = 32;

// Argon2i parameters. 32 MiB and three passes is Monocypher's own recommendation for the
// interactive case, and it is the knob that decides how expensive a stolen account file is to
// attack offline. Raising it costs the player one login; lowering it costs them nothing they can
// see and costs an attacker everything — so it does not get lowered to make a test faster.
inline constexpr std::uint32_t kArgonBlocks = 32u * 1024u;  // in KiB blocks -> 32 MiB
inline constexpr std::uint32_t kArgonPasses = 3;

// 1-based. 0 means "no account" and is what a failed login returns.
using AccountId = std::uint32_t;
inline constexpr AccountId kNoAccount = 0;

struct AccountRecord {
    char name[kMaxAccountName] = {};
    std::uint8_t salt[kSaltBytes] = {};
    std::uint8_t hash[kHashBytes] = {};
};

enum class LoginOutcome : std::uint8_t {
    kCreated,       // the name was new — an account was made for it
    kAuthenticated,
    kWrongPassword,
    kBadName,       // empty, too long, or non-printable
    kFull,          // no free session slot (see kMaxPlayers)
};

[[nodiscard]] inline const char* describe(LoginOutcome o) noexcept {
    switch (o) {
        case LoginOutcome::kCreated: return "account created";
        case LoginOutcome::kAuthenticated: return "welcome back";
        case LoginOutcome::kWrongPassword: return "wrong password";
        case LoginOutcome::kBadName: return "unusable name";
        case LoginOutcome::kFull: return "world is full";
    }
    return "?";
}

class AccountStore {
public:
    // Names are compared case-sensitively and stored verbatim. Deliberately no normalisation: a
    // Unicode-aware casefold is a genuinely hard problem and getting it half right invites two
    // accounts that look identical and are not.
    [[nodiscard]] static bool name_ok(std::string_view name) noexcept {
        if (name.empty() || name.size() >= static_cast<std::size_t>(kMaxAccountName)) return false;
        for (const char c : name) {
            if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7E) {
                return false;  // printable ASCII, no spaces — it is a login, not a display name
            }
        }
        return true;
    }

    // Create-or-authenticate in one call. Returns kNoAccount on failure, and sets `out`.
    [[nodiscard]] AccountId login(std::string_view name, std::string_view password,
                                  LoginOutcome& out) {
        if (!name_ok(name)) {
            out = LoginOutcome::kBadName;
            return kNoAccount;
        }
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (name != records_[i].name) continue;
            std::uint8_t candidate[kHashBytes];
            derive(password, records_[i].salt, candidate);
            // Constant-time. A byte-by-byte compare leaks the length of the matching prefix through
            // timing, which turns an offline problem into an online one.
            const bool ok = crypto_verify32(candidate, records_[i].hash) == 0;
            crypto_wipe(candidate, sizeof candidate);
            out = ok ? LoginOutcome::kAuthenticated : LoginOutcome::kWrongPassword;
            return ok ? static_cast<AccountId>(i + 1) : kNoAccount;
        }

        AccountRecord rec{};
        std::memcpy(rec.name, name.data(), name.size());
        fill_salt(rec.salt);
        derive(password, rec.salt, rec.hash);
        records_.push_back(rec);
        dirty_ = true;
        out = LoginOutcome::kCreated;
        return static_cast<AccountId>(records_.size());
    }

    [[nodiscard]] const char* name_of(AccountId id) const noexcept {
        return (id == kNoAccount || id > records_.size()) ? "" : records_[id - 1].name;
    }

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    // --- the file --------------------------------------------------------------------------------
    // Written whole, never appended to: the table is tiny and a partial append is a corrupt table.
    // A real atomic-rename save belongs to P5 along with everything else that has to survive a crash.
    bool save(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (f == nullptr) return false;
        const std::uint32_t header[2] = {kMagic, kVersion};
        const auto count = static_cast<std::uint32_t>(records_.size());
        bool ok = std::fwrite(header, sizeof header, 1, f) == 1 &&
                  std::fwrite(&count, sizeof count, 1, f) == 1;
        if (ok && count != 0) {
            ok = std::fwrite(records_.data(), sizeof(AccountRecord), count, f) == count;
        }
        std::fclose(f);
        return ok;
    }

    bool load(const char* path) {
        std::FILE* f = std::fopen(path, "rb");
        if (f == nullptr) return false;
        std::uint32_t header[2] = {};
        std::uint32_t count = 0;
        bool ok = std::fread(header, sizeof header, 1, f) == 1 && header[0] == kMagic &&
                  header[1] == kVersion && std::fread(&count, sizeof count, 1, f) == 1 &&
                  count <= kMaxRecords;
        if (ok) {
            records_.assign(count, AccountRecord{});
            ok = count == 0 || std::fread(records_.data(), sizeof(AccountRecord), count, f) == count;
        }
        std::fclose(f);
        if (!ok) records_.clear();
        dirty_ = false;
        return ok;
    }

private:
    static constexpr std::uint32_t kMagic = 0x4E'41'43'43;  // "NACC"
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint32_t kMaxRecords = 4096;  // a sanity bound on a corrupt header

    static void derive(std::string_view password, const std::uint8_t salt[kSaltBytes],
                       std::uint8_t out[kHashBytes]) {
        // Argon2 is memory-hard by design, so the work area is the point: 32 MiB, allocated for the
        // duration of one login and then wiped. It is NOT a static buffer — two nodes logging in
        // concurrently must not share it.
        auto work = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(kArgonBlocks) * 1024u);
        crypto_argon2_config cfg{};
        cfg.algorithm = CRYPTO_ARGON2_I;
        cfg.nb_blocks = kArgonBlocks;
        cfg.nb_passes = kArgonPasses;
        cfg.nb_lanes = 1;
        crypto_argon2_inputs in{};
        in.pass = reinterpret_cast<const std::uint8_t*>(password.data());
        in.pass_size = static_cast<std::uint32_t>(password.size());
        in.salt = salt;
        in.salt_size = kSaltBytes;
        crypto_argon2(out, kHashBytes, work.get(), cfg, in, crypto_argon2_no_extras);
        crypto_wipe(work.get(), static_cast<std::size_t>(kArgonBlocks) * 1024u);
    }

    // A salt does not have to be secret, only unique. `random_device` is a real CSPRNG on every
    // toolchain this project builds with — but it is famously deterministic on one that it does
    // not (MinGW), and a constant salt would silently make every account's hash comparable. So the
    // clock and an allocation address are mixed in as a floor: worst case the salt is merely
    // unpredictable-enough-to-be-unique rather than cryptographically random.
    static void fill_salt(std::uint8_t out[kSaltBytes]) {
        std::random_device rd;
        std::uint64_t mix = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        mix ^= reinterpret_cast<std::uintptr_t>(&out[0]) * 0x9E37'79B9'7F4A'7C15ull;
        for (int i = 0; i < kSaltBytes; i += 4) {
            const std::uint32_t r = rd() ^ static_cast<std::uint32_t>(mix >> (8 * (i % 5)));
            std::memcpy(out + i, &r, 4);
            mix = mix * 6364136223846793005ull + 1442695040888963407ull;
        }
    }

    std::vector<AccountRecord> records_;
    bool dirty_ = false;
};

}  // namespace mmo
