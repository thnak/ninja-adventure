#include "ui/screens.hpp"

#include "render/atlas_slots.hpp"
#include "render/ui_sprites.hpp"
#include "world/player_actor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "raylib.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

namespace mmo::ui {
namespace {

constexpr int kPanelW = 340;
constexpr int kBtnH = 42;
constexpr int kGap = 10;

// The four schools, shared by the character sheet, the HUD chips, the XP motes and the level-up
// banner so a school reads the SAME colour everywhere it appears — the one place progression is
// shown twice (glanceable on the HUD, in full on the sheet) it must not be two different reds. The
// families echo the vitals: melee warm like health, ranged green like stamina, magic blue like mana,
// craft the gold the sheet already trims its panels with. Order is `Skill` (Melee, Ranged, Magic,
// Craft). The letters avoid Magic colliding with Melee on M — G, as maGic, is the readout's own word.
static const Color kSchoolColor[kSkillCount] = {
    Color{226, 130, 120, 255}, Color{150, 210, 140, 255},
    Color{150, 170, 230, 255}, Color{214, 176, 106, 255}};
static const char* const kSchoolLetter[kSkillCount] = {"M", "R", "G", "C"};
static const char* const kSchoolUpper[kSkillCount] = {"MELEE", "RANGED", "MAGIC", "CRAFT"};

// The abilities' display names, indexed by `AbilityId`. The table in abilities.hpp carries every
// number the three tiers agree on but no name — a name is a UI concern, so it lives here, the one
// place that shows it. Kept in enum order so `ability_def`'s index addresses it directly.
static const char* const kAbilityName[kAbilityCount] = {
    "Whirl Cleave", "Crush Blow", "Fan Volley", "Smoke Bomb", "Elemental Nova", "Rain Call"};

// A shade dimmer, for the inactive chips: the same colour, pulled toward the background so the
// active school is the only one at full strength.
[[nodiscard]] Color dim(Color c, float k) {
    return Color{static_cast<unsigned char>(static_cast<float>(c.r) * k),
                 static_cast<unsigned char>(static_cast<float>(c.g) * k),
                 static_cast<unsigned char>(static_cast<float>(c.b) * k), c.a};
}

// The index of the highest school — the ACTIVE one, the school `equipped_ability` draws the loadout
// from. Ties break low (Melee first), matching that function so the chip lit here is the school whose
// abilities are in the two slots.
[[nodiscard]] int active_school(const PlayerView& player) {
    int best = 0;
    for (int i = 1; i < kSkillCount; ++i)
        if (player.skill_level[i] > player.skill_level[best]) best = i;
    return best;
}

// The next ability unlock for a school at its current level, read from the ability table so the
// hint tracks a retune of the unlock levels rather than a hardcoded 2/6. Returns the level, or 0 if
// the school has nothing left to unlock (both taken, or Craft, which has no abilities).
[[nodiscard]] int next_unlock_level(int school, std::uint8_t level) {
    int best = 0;
    for (int a = 0; a < kAbilityCount; ++a) {
        const AbilityDef d = ability_def(static_cast<AbilityId>(a));
        if (static_cast<int>(d.school) != school || d.unlock_level <= level) continue;
        if (best == 0 || d.unlock_level < best) best = d.unlock_level;
    }
    return best;
}

// Centred column of buttons; returns the index clicked, or -1.
int button_column(const char* const* labels, int count, int top) {
    const int x = (GetScreenWidth() - kPanelW) / 2;
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        const Rectangle r{static_cast<float>(x), static_cast<float>(top + i * (kBtnH + kGap)),
                          static_cast<float>(kPanelW), static_cast<float>(kBtnH)};
        if (GuiButton(r, labels[i])) clicked = i;
    }
    return clicked;
}

// Two side-by-side segments filling the panel width; the selected one reads lit. Returns the index
// clicked this frame, or -1. Deliberately the same flat-rectangle-plus-text idiom as `button_column`
// and `text_field` — a segmented choice is a new job, not a reason to reach for a new widget
// vocabulary. `second_selected` is which of the two is currently chosen.
int segment_pair(int x, int y, const char* a, const char* b, bool second_selected) {
    const int seg_w = (kPanelW - kGap) / 2;
    const Vector2 m = GetMousePosition();
    int clicked = -1;
    for (int i = 0; i < 2; ++i) {
        const Rectangle r{static_cast<float>(x + i * (seg_w + kGap)), static_cast<float>(y),
                          static_cast<float>(seg_w), 40.0f};
        const bool selected = (i == 1) == second_selected;
        if (CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) clicked = i;
        DrawRectangleRec(r, selected ? Color{51, 56, 74, 255} : Color{26, 29, 38, 255});
        DrawRectangleLinesEx(r, 2.0f,
                             selected ? Color{214, 176, 106, 255} : Color{92, 96, 108, 255});
        const char* label = i == 0 ? a : b;
        const int tw = MeasureText(label, 18);
        DrawText(label, static_cast<int>(r.x) + (seg_w - tw) / 2, static_cast<int>(r.y) + 11, 18,
                 selected ? Color{240, 226, 190, 255} : Color{150, 158, 168, 255});
    }
    return clicked;
}

// A flat slider in the same idiom as `button_column` and `segment_pair`: a dark track, a lit fill up
// to the value, a gold knob. Click or drag anywhere on the track to set it; the caller adds arrow
// keys. `value` is 0..100 and is written in place. Deliberately not a raygui `GuiSlider` for the same
// reason `text_field` is hand-rolled — one widget vocabulary, not half of two.
void slider(int x, int y, int w, int& value) {
    constexpr int h = 22;
    const Rectangle track{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                          static_cast<float>(h)};
    const Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, track) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const int v = static_cast<int>(std::lround((m.x - static_cast<float>(x)) /
                                                   static_cast<float>(w) * 100.0f));
        value = std::clamp(v, 0, 100);
    }
    const int fill = w * value / 100;
    DrawRectangleRec(track, Color{26, 29, 38, 255});
    DrawRectangle(x, y, fill, h, Color{51, 56, 74, 255});
    DrawRectangleLinesEx(track, 2.0f, Color{92, 96, 108, 255});
    DrawRectangle(x + fill - 3, y - 2, 6, h + 4, Color{214, 176, 106, 255});
}

void dim_background() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 170});
}

void title(const char* text, int y) {
    const int w = MeasureText(text, 40);
    DrawText(text, (GetScreenWidth() - w) / 2, y, 40, Color{240, 226, 190, 255});
}

void subtitle(const char* text, int y) {
    const int w = MeasureText(text, 16);
    DrawText(text, (GetScreenWidth() - w) / 2, y, 16, Color{160, 170, 180, 255});
}

// A text field, hand-rolled rather than raygui's.
//
// Two reasons, and the second is the real one. raygui's `GuiTextBox` has no password mode, so the
// field that most needs one would have had to be special-cased anyway — and a half-raygui,
// half-manual pair of boxes on the same form behaves inconsistently under focus. So both are the
// same twenty lines. Masking here means the plaintext never reaches the screen; it still reaches
// `AccountStore::login`, which is where it is turned into an Argon2 hash and forgotten.
void text_field(const Rectangle& r, char* buf, int cap, bool& editing, bool mask,
                const char* placeholder) {
    const Vector2 m = GetMousePosition();
    const bool over = CheckCollisionPointRec(m, r);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) editing = over;

    if (editing) {
        int c = GetCharPressed();
        while (c > 0) {
            const auto len = static_cast<int>(std::strlen(buf));
            if (c >= 32 && c < 127 && len < cap - 1) {
                buf[len] = static_cast<char>(c);
                buf[len + 1] = '\0';
            }
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            const auto len = static_cast<int>(std::strlen(buf));
            if (len > 0) buf[len - 1] = '\0';
        }
    }

    DrawRectangleRec(r, Color{26, 29, 38, 255});
    DrawRectangleLinesEx(r, 2.0f,
                         editing ? Color{214, 176, 106, 255} : Color{92, 96, 108, 255});
    const auto len = static_cast<int>(std::strlen(buf));
    char shown[kPassMax + 1];
    if (mask) {
        const int n = len < kPassMax ? len : kPassMax;
        for (int i = 0; i < n; ++i) shown[i] = '*';
        shown[n] = '\0';
    }
    const char* text = len == 0 ? placeholder : (mask ? shown : buf);
    DrawText(text, static_cast<int>(r.x) + 10, static_cast<int>(r.y) + 10, 20,
             len == 0 ? Color{110, 114, 124, 255} : Color{224, 228, 234, 255});
    // A caret, so an empty focused box does not look broken.
    if (editing && (static_cast<int>(GetTime() * 2.0) & 1)) {
        const int tw = MeasureText(len == 0 ? "" : text, 20);
        DrawRectangle(static_cast<int>(r.x) + 11 + tw, static_cast<int>(r.y) + 9, 2, 22,
                      Color{224, 228, 234, 255});
    }
}

// --- in-game HUD -----------------------------------------------------------------------------
// Deliberately small. Three bars, the day clock, and the hotbar — nothing else. Anything a player
// does not need every second belongs in a screen they open, not permanently over the world.

void bar(int x, int y, int w, int h, float frac, Color fill, const char* label) {
    DrawRectangle(x - 2, y - 2, w + 4, h + 4, Color{0, 0, 0, 150});
    DrawRectangle(x, y, static_cast<int>(static_cast<float>(w) * (frac < 0.0f ? 0.0f : frac)), h,
                  fill);
    if (label != nullptr) DrawText(label, x + 6, y + (h - 14) / 2, 14, RAYWHITE);
}

// The two ability slots, drawn to the right of the element/build hotbar. Everything a slot needs it
// reads from the published PlayerView (which ability, its cooldown) and derives the rest from the
// shared ability table plus the vitals already in the view — no `ask`, no client-side ability state.
void draw_ability_slots(const PlayerView& player, int x0, int y, int slot_px) {
    static const char* kKeys[kAbilitySlots] = {"F", "G"};
    for (int i = 0; i < kAbilitySlots; ++i) {
        const int x = x0 + i * (slot_px + 6);
        const AbilityId id = player.ability[i];
        const AbilityDef def = ability_def(id);
        const bool locked = player.skill_level[static_cast<int>(def.school)] < def.unlock_level;
        const std::int16_t have =
            def.cost_kind == AbilityCost::kStamina ? player.stamina : player.mana;
        const bool poor = have < def.cost;
        const std::uint16_t cd = player.ability_cd[i];
        const bool cooling = cd > 0;
        const bool disabled = locked || poor || cooling;

        DrawRectangle(x, y, slot_px, slot_px, Color{0, 0, 0, 150});
        // The greyed twin whenever the slot cannot fire, for any of the three reasons.
        draw_ui_icon(static_cast<int>(id), disabled, static_cast<float>(x + 4),
                     static_cast<float>(y + 4), static_cast<float>(slot_px - 8));

        // Cooldown wipe: a dark panel over the top of the icon whose height is the fraction of the
        // cooldown still to run, so it empties downward and the icon fills back in as it readies.
        // Flat rectangles on purpose — the same idiom as the bars and the hotbar, not a new one.
        if (cooling && def.cooldown > 0) {
            const float frac = static_cast<float>(cd) / static_cast<float>(def.cooldown);
            const int ch = static_cast<int>(frac * static_cast<float>(slot_px));
            DrawRectangle(x, y, slot_px, ch, Color{10, 12, 18, 170});
        }

        DrawRectangleLines(x, y, slot_px, slot_px,
                           disabled ? Color{92, 96, 108, 255} : Color{240, 214, 130, 255});
        // Keybind, top-left; and the unlock level, bottom, while the school is still too low for it.
        DrawText(kKeys[i], x + 4, y + 2, 12, Color{210, 214, 222, 255});
        if (locked) {
            const char* need = TextFormat("Lv%d", def.unlock_level);
            DrawText(need, x + (slot_px - MeasureText(need, 10)) / 2, y + slot_px - 12, 10,
                     Color{200, 150, 140, 255});
        }
    }
}

// The four school levels as one compact row above the vitals — the playtest's "progression is
// invisible in play" answered where the player already looks. A chip is a letter and a number in the
// school's colour; the active school (the one whose abilities are equipped) reads at full strength
// while the rest sit dimmed. Glanceable state, not a panel: the full breakdown is still C.
void draw_skill_chips(const PlayerView& player, int x, int y) {
    const int active = active_school(player);
    constexpr int kChipW = 40;
    constexpr int kChipH = 20;
    constexpr int kChipGap = 4;
    for (int i = 0; i < kSkillCount; ++i) {
        const int cx = x + i * (kChipW + kChipGap);
        const bool on = i == active;
        const Color tint = on ? kSchoolColor[i] : dim(kSchoolColor[i], 0.55f);
        DrawRectangle(cx, y, kChipW, kChipH, Color{0, 0, 0, static_cast<unsigned char>(on ? 175 : 120)});
        DrawRectangleLines(cx, y, kChipW, kChipH, tint);
        DrawText(kSchoolLetter[i], cx + 5, y + 4, 14, tint);
        const char* lv = TextFormat("%u", player.skill_level[i]);
        DrawText(lv, cx + kChipW - MeasureText(lv, 14) - 5, y + 4, 14,
                 on ? Color{240, 240, 244, 255} : Color{158, 162, 170, 255});
    }
}

// The XP motes and the level-up banner, both driven off the fixed pool the shell carries. Drawn last
// in the HUD so they sit over the world. The local player is the camera's target, so "above the
// player" is a fixed point above screen centre whatever the zoom — no world-to-screen projection to
// thread through, and no reason for the shell to learn the camera.
void draw_hud_feedback(HudFeedback& fx, int w, int h) {
    constexpr float kBannerSecs = 2.0f;
    const float dt = GetFrameTime();

    const int px = w / 2;
    const int py = h / 2 - 44;
    for (HudFeedback::XpMote& t : fx.motes) {
        if (!t.live) continue;
        t.age += dt;
        if (t.age >= 1.0f) {
            t.live = false;
            continue;
        }
        const float a = 1.0f - t.age;  // linear fade
        const int ty = py - static_cast<int>(t.age * 34.0f);  // drifts up ~34px over its life
        const int tx = px + static_cast<int>(t.dx);
        const char* s = TextFormat("+%u", t.amount);
        DrawText(s, tx + 1, ty + 1, 18, Color{0, 0, 0, static_cast<unsigned char>(a * 150.0f)});
        Color c = kSchoolColor[t.school];
        c.a = static_cast<unsigned char>(a * 255.0f);
        DrawText(s, tx, ty, 18, c);
    }

    if (fx.banner_age < kBannerSecs) {
        fx.banner_age += dt;
        // Hold, then fade the last third so it leaves rather than blinks out.
        const float a = fx.banner_age > kBannerSecs * 0.66f
                            ? (kBannerSecs - fx.banner_age) / (kBannerSecs * 0.34f)
                            : 1.0f;
        const auto alpha = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        const int by = h / 2 - 120;
        Color tint = kSchoolColor[fx.banner_school];
        tint.a = alpha;
        const int tw = MeasureText(fx.banner_top, 48);
        DrawText(fx.banner_top, (w - tw) / 2 + 2, by + 2, 48,
                 Color{0, 0, 0, static_cast<unsigned char>(a * 150.0f)});
        DrawText(fx.banner_top, (w - tw) / 2, by, 48, tint);
        if (fx.banner_bottom[0] != '\0') {
            const int bw = MeasureText(fx.banner_bottom, 22);
            DrawText(fx.banner_bottom, (w - bw) / 2, by + 56, 22, Color{240, 226, 190, alpha});
        }
    }
}

void draw_hud(const WorldStatus& status, const PlayerView& player, bool build_mode,
              int selected_slot, HudFeedback& fx) {
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();

    // Three bars, bottom-left, in the order they are spent: health is what the world takes,
    // stamina is what swinging costs, mana is what casting costs.
    const auto frac = [](std::int16_t v, std::int16_t max) {
        return max <= 0 ? 0.0f : static_cast<float>(v) / static_cast<float>(max);
    };
    bar(18, h - 74, 200, 20, frac(player.hp, kPlayerMaxHp), Color{196, 72, 72, 255},
        TextFormat("%d", player.hp));
    bar(18, h - 48, 140, 12, frac(player.stamina, kPlayerMaxStamina), Color{120, 190, 96, 255},
        nullptr);
    bar(18, h - 30, 140, 12, frac(player.mana, kPlayerMaxMana), Color{96, 140, 220, 255}, nullptr);

    // The skill chips ride just above the health bar — near the vitals, in the corner the eye already
    // returns to for its bars, so a school ticking up is seen without opening a screen.
    draw_skill_chips(player, 18, h - 98);

    // Day clock, top-right. `world_ms` is the world's own clock, not this client's.
    const std::int64_t ms = status.world_ms.load(std::memory_order_relaxed);
    const bool night = status.night.load(std::memory_order_relaxed);
    const int day = static_cast<int>(ms / kCycleMs) + 1;
    const char* phase = night ? "Night" : "Day";
    const char* label = TextFormat("%s  -  day %d", phase, day);
    const int lw = MeasureText(label, 20);
    DrawRectangle(w - lw - 32, 12, lw + 20, 30, Color{0, 0, 0, 150});
    DrawText(label, w - lw - 22, 18, 20,
             night ? Color{178, 190, 240, 255} : Color{255, 236, 176, 255});

    // Hotbar, bottom-centre. It shows four schools when you are fighting and two buildings when you
    // are building, because those are two different bars and pretending otherwise would mean six
    // slots of which four are always dead.
    static const char* kSchools[4] = {"Fire", "Ice", "Earth", "Shock"};
    static const Color kSchoolTint[4] = {Color{255, 150, 90, 255}, Color{140, 210, 255, 255},
                                         Color{180, 150, 110, 255}, Color{255, 245, 130, 255}};
    static const char* kBuilds[2] = {"Hearth", "Plot"};
    const int slots = build_mode ? 2 : 4;
    constexpr int kSlotPx = 44;
    // The element/build slots plus, in combat, the two ability slots — kept as one centred group so
    // the whole bar reads together and stays put when you switch modes. The ability slots are hidden
    // in build mode: there is nothing to fire while placing a hearth.
    const int ability_extent = build_mode ? 0 : (12 + 2 * kSlotPx + 6);
    const int total = slots * kSlotPx + (slots - 1) * 6 + ability_extent;
    const int hx = (w - total) / 2;
    for (int i = 0; i < slots; ++i) {
        const int x = hx + i * (kSlotPx + 6);
        DrawRectangle(x, h - 60, kSlotPx, kSlotPx, Color{0, 0, 0, 150});
        DrawRectangleLines(x, h - 60, kSlotPx, kSlotPx,
                           i == selected_slot ? Color{240, 214, 130, 255}
                                              : Color{110, 110, 118, 255});
        if (!build_mode) {
            DrawRectangle(x + 10, h - 44, 24, 24, kSchoolTint[i]);
        }
        DrawText(TextFormat("%d", i + 1), x + 4, h - 58, 12, Color{170, 170, 176, 255});
        const char* name = build_mode ? kBuilds[i] : kSchools[i];
        DrawText(name, x + (kSlotPx - MeasureText(name, 10)) / 2, h - 12, 10,
                 Color{170, 174, 182, 255});
    }
    if (!build_mode) {
        const int ax0 = hx + slots * (kSlotPx + 6) + 12;
        draw_ability_slots(player, ax0, h - 60, kSlotPx);
    }
    // Above the slots rather than beside them: at the narrowest supported window the hotbar reaches
    // most of the way across and a label to its left lands on top of slot one.
    {
        const char* mode = build_mode ? "BUILD  (B)" : "FIGHT  (B)";
        DrawText(mode, (w - MeasureText(mode, 16)) / 2, h - 92, 16,
                 build_mode ? Color{240, 214, 130, 255} : Color{212, 172, 172, 255});
    }

    // Death. A countdown, in the middle, with nothing else competing for attention.
    if (player.dead_ticks > 0) {
        const char* down = "You went down.";
        DrawText(down, (w - MeasureText(down, 44)) / 2, h / 2 - 60, 44,
                 Color{244, 214, 200, 255});
        const char* back =
            TextFormat("Waking at your hearth in %.0f...",
                       static_cast<double>(player.dead_ticks) / kTicksPerSecond + 0.9);
        DrawText(back, (w - MeasureText(back, 20)) / 2, h / 2, 20, Color{210, 180, 175, 255});
    }

    // The client-side feedback (XP motes, level-up banner) draws last so it lands over everything the
    // HUD put down.
    draw_hud_feedback(fx, w, h);
}

// --- Character sheet -------------------------------------------------------------------------
static const char* kSkillNames[kSkillCount] = {"Melee", "Ranged", "Magic", "Craft"};

void draw_character(const PlayerView& player) {
    dim_background();
    const int panel_w = 560;
    const int x = (GetScreenWidth() - panel_w) / 2;
    int y = 90;
    DrawRectangle(x - 24, y - 34, panel_w + 48, GetScreenHeight() - 150, Color{22, 24, 30, 240});
    DrawRectangleLines(x - 24, y - 34, panel_w + 48, GetScreenHeight() - 150,
                       Color{110, 96, 70, 255});

    // The portrait. Ninja Adventure ships one beside every actor; this is the only thing on the
    // screen that is art rather than a number, and it is what stops the sheet reading as a form.
    draw_ui_fx(static_cast<int>(Fx::kFacePlayer), 0, static_cast<float>(x + 48),
               static_cast<float>(y + 42), 96.0f);
    DrawRectangleLines(x, y - 6, 96, 96, Color{110, 96, 70, 255});

    DrawText("Character", x + 118, y - 4, 32, Color{240, 226, 190, 255});
    DrawText(TextFormat("deaths %u", player.deaths), x + 118, y + 36, 16,
             Color{160, 170, 180, 255});
    y += 112;

    DrawText(TextFormat("Health   %3d / %d", player.hp, player.max_hp), x, y, 18,
             Color{226, 150, 150, 255});
    DrawText(TextFormat("Stamina  %3d / %d", player.stamina, kPlayerMaxStamina), x + 250, y, 18,
             Color{160, 214, 150, 255});
    y += 26;
    DrawText(TextFormat("Mana     %3d / %d", player.mana, kPlayerMaxMana), x, y, 18,
             Color{150, 175, 226, 255});
    y += 44;

    // Skills. The cap is displayed next to the total on purpose: a limit the player only discovers
    // by hitting it is a bug report, not a design.
    int spent = 0;
    for (int i = 0; i < kSkillCount; ++i) spent += player.skill_level[i];
    DrawText("Skills", x, y, 20, Color{230, 206, 140, 255});
    const char* cap = TextFormat("%d / %d points  -  you level what you use", spent,
                                 static_cast<int>(kSkillPointCap));
    DrawText(cap, x + panel_w - MeasureText(cap, 15), y + 5, 15, Color{150, 158, 168, 255});
    y += 32;

    for (int i = 0; i < kSkillCount; ++i) {
        // The school's own colour, the same one the HUD chips, motes and banner use, so the sheet is
        // where a player first learns which red is Melee.
        DrawText(kSkillNames[i], x, y, 18, kSchoolColor[i]);
        DrawText(TextFormat("%2u", player.skill_level[i]), x + 92, y, 18,
                 Color{240, 226, 190, 255});
        const float f = player.skill_next[i] == 0
                            ? 1.0f
                            : static_cast<float>(player.skill_xp[i]) /
                                  static_cast<float>(player.skill_next[i]);
        bar(x + 128, y + 2, 236, 14, f, dim(kSchoolColor[i], 0.72f), nullptr);
        DrawText(TextFormat("%u/%u", player.skill_xp[i], player.skill_next[i]), x + 372, y + 1, 15,
                 Color{150, 158, 168, 255});
        // The next unlock, right-aligned as its own column: leveling a school buys a specific
        // ability, and this says which and when. Read from the ability table (via next_unlock_level)
        // so a retune of the unlock levels moves the hint with it rather than lying.
        const int nxt = next_unlock_level(i, player.skill_level[i]);
        if (nxt > 0) {
            const char* hint = TextFormat("ability at Lv%d", nxt);
            DrawText(hint, x + panel_w - MeasureText(hint, 14), y + 2, 14,
                     Color{150, 158, 168, 255});
        }
        y += 30;
    }
    y += 18;
    DrawText("Magic sets a status. A physical blow detonates it.", x, y, 17,
             Color{176, 190, 176, 255});
    y += 24;
    static const char* kCombos[] = {
        "Frozen  + heavy melee   ->  Shatter    x2.5",
        "Burning + arrow         ->  Blast      splash",
        "Wet     + shock         ->  Conduct    chains",
        "Muddy   + heavy melee   ->  Crush      stuns",
        "Shocked + melee         ->  Arc        returns mana",
    };
    for (const char* c : kCombos) {
        DrawText(c, x + 8, y, 16, Color{196, 200, 208, 255});
        y += 21;
    }
}

}  // namespace

void hud_spawn_xp_mote(HudFeedback& fx, int school, std::uint32_t amount) {
    if (school < 0 || school >= kSkillCount || amount == 0) return;
    // A fixed pool: take a free slot, or the oldest live one so a burst never allocates and never
    // silently drops the newest tick in favour of a stale one.
    int pick = 0;
    float oldest = -1.0f;
    for (int i = 0; i < HudFeedback::kMaxMotes; ++i) {
        if (!fx.motes[i].live) {
            pick = i;
            break;
        }
        if (fx.motes[i].age > oldest) {
            oldest = fx.motes[i].age;
            pick = i;
        }
    }
    HudFeedback::XpMote& m = fx.motes[pick];
    m.live = true;
    m.age = 0.0f;
    m.school = school;
    m.amount = amount;
    // Scatter horizontally off the pool index so several motes in one frame fan out instead of
    // stacking into one fat number over the player's head.
    m.dx = static_cast<float>((pick % 5) - 2) * 12.0f;
}

void hud_spawn_level_up(HudFeedback& fx, int school, int level) {
    if (school < 0 || school >= kSkillCount) return;
    fx.banner_age = 0.0f;
    fx.banner_school = school;
    std::snprintf(fx.banner_top, sizeof fx.banner_top, "%s %d", kSchoolUpper[school], level);
    fx.banner_bottom[0] = '\0';
    // Did this level cross an ability's unlock? Read the unlock levels straight from the table — the
    // banner must never carry its own copy of the 2/6 the design tunes there.
    for (int a = 0; a < kAbilityCount; ++a) {
        const AbilityDef d = ability_def(static_cast<AbilityId>(a));
        if (static_cast<int>(d.school) != school || d.unlock_level != level) continue;
        // Which equipped slot it lands on: the lower-unlock of a school's two is F, the higher G.
        // Count same-school abilities gated below this one — 0 means F, 1 means G. This mirrors
        // `equipped_ability` without hardcoding the slot, so it stays right if the table is retuned.
        int slot = 0;
        for (int b = 0; b < kAbilityCount; ++b) {
            const AbilityDef e = ability_def(static_cast<AbilityId>(b));
            if (static_cast<int>(e.school) == school && e.unlock_level < level) ++slot;
        }
        std::snprintf(fx.banner_bottom, sizeof fx.banner_bottom, "New ability: %s [%c]",
                      kAbilityName[a], slot == 0 ? 'F' : 'G');
        break;
    }
}

bool handle_shell_keys(ShellState& st) {
    // The sign-in screen swallows everything: its text fields want the keyboard.
    if (st.screen == Screen::kLogin) return true;

    if (IsKeyPressed(KEY_F3)) {
        st.debug_overlay = !st.debug_overlay;
        return true;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        switch (st.screen) {
            case Screen::kPlaying: st.screen = Screen::kPaused; return true;
            case Screen::kPaused: st.screen = Screen::kPlaying; return true;
            case Screen::kJournal:
            case Screen::kCharacter:
            case Screen::kOptions: st.screen = Screen::kPlaying; return true;
            case Screen::kLogin:
            case Screen::kMainMenu: return false;
        }
    }
    if (IsKeyPressed(KEY_J) && st.screen == Screen::kPlaying) {
        st.screen = Screen::kJournal;
        return true;
    }
    if (IsKeyPressed(KEY_C)) {
        if (st.screen == Screen::kPlaying) {
            st.screen = Screen::kCharacter;
            return true;
        }
        if (st.screen == Screen::kCharacter) {
            st.screen = Screen::kPlaying;
            return true;
        }
    }
    // While a menu is open the world must not receive movement or attack clicks.
    return st.screen != Screen::kPlaying;
}

// raygui ships a light-grey default that fights a dark fantasy palette. Set once, on first draw.
void apply_theme() {
    static bool done = false;
    if (done) return;
    done = true;
    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);
    const auto hex = [](unsigned v) { return static_cast<int>(v); };
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, hex(0x14161cff));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, hex(0x6e6046ff));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, hex(0x232732ff));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, hex(0xcfd3d9ff));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, hex(0xd6b06aff));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, hex(0x33384aff));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, hex(0xf0e2beff));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, hex(0xf0d08aff));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, hex(0x3f4459ff));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, hex(0xfff3d0ff));
}

Action draw(ShellState& st, const WorldStatus& status, const PlayerView& player, bool build_mode,
            int selected_slot) {
    apply_theme();

    switch (st.screen) {
        case Screen::kLogin: {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{18, 20, 26, 255});
            title("Ninja Adventure", GetScreenHeight() / 2 - 260);
            subtitle("A name this world has not seen before becomes an account.",
                     GetScreenHeight() / 2 - 214);

            const int x = (GetScreenWidth() - kPanelW) / 2;
            static const Color kLabel{160, 170, 180, 255};

            // --- World: host vs join -----------------------------------------------------------
            // The connection choice comes FIRST because it decides which world the name/password
            // then sign into. Left/Right or a click picks a segment; see below for why join is
            // gated in this build.
            int y = GetScreenHeight() / 2 - 170;
            DrawText("World", x, y - 22, 16, kLabel);
            const int seg_clicked = segment_pair(x, y, "Host this world", "Join a world",
                                                  st.join_mode);
            if (seg_clicked == 0) st.join_mode = false;
            if (seg_clicked == 1) st.join_mode = true;
            // Arrow keys are free on this screen (the text fields have no cursor to move), so they
            // drive the segment — the flat-choice equivalent of Tab between fields.
            if (IsKeyPressed(KEY_LEFT)) st.join_mode = false;
            if (IsKeyPressed(KEY_RIGHT)) st.join_mode = true;
            y += 52;

            // The block's own explanatory line, in the screen's existing voice. The join line is
            // deliberately honest: the distributed router that JOIN needs is the P5 step (see
            // world.hpp's LocalRouter note and ARCHITECTURE §2), so this build cannot actually
            // reach a remote leader yet, and says so rather than pretending.
            subtitle(st.join_mode
                         ? "Joining over the network lands with the cluster router (P5). "
                           "This build hosts."
                         : "Your machine keeps this world. Friends join with your address.",
                     y);
            y += 34;

            // The address field exists only when joining — there is nothing to type when you are
            // the host. It leads the Tab cycle when present.
            if (st.join_mode) {
                DrawText("Address", x, y - 22, 16, kLabel);
                text_field(Rectangle{static_cast<float>(x), static_cast<float>(y),
                                     static_cast<float>(kPanelW), 40.0f},
                           st.join_addr, kJoinAddrMax, st.editing_addr, false,
                           "leader address - ip:port");
                y += 62;
            } else {
                st.editing_addr = false;  // an unseen field must not keep the keyboard
            }

            // --- Name / Password ---------------------------------------------------------------
            DrawText("Name", x, y - 22, 16, kLabel);
            text_field(Rectangle{static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(kPanelW), 40.0f},
                       st.name, kNameMax, st.editing_name, false, "who are you?");
            y += 62;
            DrawText("Password", x, y - 22, 16, kLabel);
            text_field(Rectangle{static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(kPanelW), 40.0f},
                       st.pass, kPassMax, st.editing_pass, true, "");
            y += 58;

            if (st.login_message != nullptr) {
                const int mw = MeasureText(st.login_message, 16);
                DrawText(st.login_message, (GetScreenWidth() - mw) / 2, y, 16,
                         Color{224, 150, 140, 255});
            }
            y += 26;

            // Tab cycles the visible fields, Enter submits — the two things anyone typing a login
            // will try without being told. When joining, the address leads: Address -> Name ->
            // Password. When hosting, there is no address, so it is just Name -> Password.
            if (IsKeyPressed(KEY_TAB)) {
                if (st.join_mode) {
                    if (!st.editing_addr && !st.editing_name && !st.editing_pass) {
                        st.editing_addr = true;  // nothing focused yet: land on the address
                    } else {
                        const bool a = st.editing_addr, n = st.editing_name;
                        st.editing_addr = st.editing_pass;  // Password -> Address
                        st.editing_name = a;                // Address -> Name
                        st.editing_pass = n;                // Name -> Password
                    }
                } else {
                    st.editing_pass = !st.editing_pass;
                    st.editing_name = !st.editing_pass;
                }
            }
            const bool submit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
            static const char* items[] = {"Sign in", "Quit"};
            const int clicked = button_column(items, 2, y);
            if (clicked == 1) return Action::kQuit;
            if (clicked == 0 || submit) return Action::kSignIn;

            subtitle("Passwords are hashed with Argon2 and never stored. See assets/CREDITS.md.",
                     GetScreenHeight() - 60);
            return Action::kNone;
        }

        case Screen::kPlaying:
            draw_hud(status, player, build_mode, selected_slot, st.hud);
            return Action::kNone;

        case Screen::kMainMenu: {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{18, 20, 26, 255});
            title("Ninja Adventure", GetScreenHeight() / 2 - 190);
            subtitle("You wake in open country. Somewhere out there, people.",
                     GetScreenHeight() / 2 - 138);
            static const char* items[] = {"Play", "Journal", "Options", "Quit"};
            switch (button_column(items, 4, GetScreenHeight() / 2 - 80)) {
                case 0: st.screen = Screen::kPlaying; return Action::kStartGame;
                case 1: st.screen = Screen::kJournal; break;
                case 2: st.screen = Screen::kOptions; break;
                case 3: return Action::kQuit;
                default: break;
            }
            return Action::kNone;
        }

        case Screen::kPaused: {
            draw_hud(status, player, build_mode, selected_slot, st.hud);
            dim_background();
            title("Paused", GetScreenHeight() / 2 - 170);
            subtitle("The world keeps going while you are here.", GetScreenHeight() / 2 - 120);
            static const char* items[] = {"Resume", "Character", "Journal", "Quit to menu"};
            switch (button_column(items, 4, GetScreenHeight() / 2 - 70)) {
                case 0: st.screen = Screen::kPlaying; return Action::kResume;
                case 1: st.screen = Screen::kCharacter; break;
                case 2: st.screen = Screen::kJournal; break;
                case 3: st.screen = Screen::kMainMenu; break;
                default: break;
            }
            return Action::kNone;
        }

        case Screen::kCharacter:
            draw_character(player);
            return Action::kNone;

        case Screen::kJournal: {
            dim_background();
            const int x = (GetScreenWidth() - 620) / 2;
            int y = 70;
            DrawRectangle(x - 24, y - 34, 668, GetScreenHeight() - 110, Color{22, 24, 30, 240});
            DrawRectangleLines(x - 24, y - 34, 668, GetScreenHeight() - 110,
                               Color{110, 96, 70, 255});
            DrawText("Journal", x, y, 32, Color{240, 226, 190, 255});
            y += 48;

            DrawText("Controls", x, y, 20, Color{230, 206, 140, 255});
            y += 28;
            static const char* rows[] = {
                "WASD / arrows      move",
                "B                  swap between fighting and building",
                "Left mouse         swing   (hold Shift: a heavy blow)",
                "Right mouse        cast the selected school at the cursor",
                "Q / Space          loose an arrow at the cursor",
                "1 - 4              fire / ice / earth / shock",
                "R                  mount up - faster, but you cannot fight",
                "E                  harvest a ripe crop",
                "T / U              till a tile / upgrade what is under the cursor",
                "C                  character sheet",
                "J                  this journal",
                "Esc                pause",
            };
            for (const char* r : rows) {
                DrawText(r, x + 8, y, 17, Color{206, 210, 216, 255});
                y += 22;
            }
            y += 10;
            DrawText("Getting started", x, y, 20, Color{230, 206, 140, 255});
            y += 28;
            static const char* tips[] = {
                "Nothing is counting down. Nothing is chasing you.",
                "You start with nothing. Walk until you find a village.",
                "Most animals want nothing to do with you. Crowd one and it will mind.",
                "Hit a wolf and the whole pack minds.",
                "Further from the middle of the world is harder. That is the only rule.",
            };
            for (const char* t : tips) {
                DrawText(t, x + 8, y, 17, Color{176, 190, 176, 255});
                y += 22;
            }

            const Rectangle back{static_cast<float>(x),
                                 static_cast<float>(GetScreenHeight() - 108), 160.0f,
                                 static_cast<float>(kBtnH)};
            if (GuiButton(back, "Back")) st.screen = Screen::kPlaying;
            return Action::kNone;
        }

        case Screen::kOptions: {
            dim_background();
            title("Options", GetScreenHeight() / 2 - 170);
            const int x = (GetScreenWidth() - kPanelW) / 2;
            static const Color kLabel{160, 170, 180, 255};
            int y = GetScreenHeight() / 2 - 90;

            // Master volume: click or drag the track, or nudge with Left/Right (the screen has no
            // text field, so the arrows are free — the same trick the login screen's segments use).
            // It applies live: `client_main` watches these two fields and calls into Audio, which the
            // shell is not allowed to see.
            DrawText("Master volume", x, y - 22, 16, kLabel);
            slider(x, y, kPanelW - 64, st.master_volume);
            if (IsKeyPressed(KEY_LEFT)) st.master_volume = std::max(0, st.master_volume - 5);
            if (IsKeyPressed(KEY_RIGHT)) st.master_volume = std::min(100, st.master_volume + 5);
            {
                const char* pct = TextFormat("%d%%", st.master_volume);
                DrawText(pct, x + kPanelW - MeasureText(pct, 18), y, 18, Color{240, 226, 190, 255});
            }
            y += 58;

            // Music on/off — the same two-segment choice the login screen hosts/joins with. `Off` is
            // the second segment, so it lights when the music is off.
            DrawText("Music", x, y - 22, 16, kLabel);
            const int music_click = segment_pair(x, y, "On", "Off", !st.music_on);
            if (music_click == 0) st.music_on = true;
            if (music_click == 1) st.music_on = false;
            y += 66;

            subtitle("Key rebinding is still owed - it lands with the input pass.", y);
            y += 30;

            static const char* items[] = {"Back"};
            if (button_column(items, 1, y) == 0) st.screen = Screen::kPaused;
            return Action::kNone;
        }
    }
    return Action::kNone;
}

void draw_debug_overlay(const WorldStatus& status, const PlayerView& player, int drawn_chunks,
                        int drawn_creatures) {
    const int w = 340;
    DrawRectangle(GetScreenWidth() - w - 12, 52, w, 208, Color{0, 0, 0, 190});
    int y = 60;
    const auto line = [&](const char* text) {
        DrawText(text, GetScreenWidth() - w - 2, y, 16, Color{150, 220, 150, 255});
        y += 20;
    };
    line("-- debug (F3) ------------------");
    line(TextFormat("fps               %d", GetFPS()));
    line(TextFormat("world tick        %llu",
                    static_cast<unsigned long long>(status.tick.load(std::memory_order_relaxed))));
    line(TextFormat("chunk actors      %d drawn", drawn_chunks));
    line(TextFormat("creatures shown   %d", drawn_creatures));
    line(TextFormat("creatures killed  %u",
                    status.creatures_killed.load(std::memory_order_relaxed)));
    line(TextFormat("  by players      %u", status.player_kills.load(std::memory_order_relaxed)));
    line(TextFormat("player deaths     %u", status.player_deaths.load(std::memory_order_relaxed)));
    // The headline number of the engine work: every one of these was a creature or an arrow handed
    // from one actor to another, and will be a network frame once chunks live on separate machines.
    line(TextFormat("chunk migrations  %u", status.migrations.load(std::memory_order_relaxed)));
    line(TextFormat("player tile       %d, %d", static_cast<int>(player.x),
                    static_cast<int>(player.y)));
}

}  // namespace mmo::ui
