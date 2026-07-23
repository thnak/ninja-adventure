// A keyhole through which the UI shell can draw ONE atlas sprite.
//
// `screens.cpp` needs the player's portrait for the Character screen, and the portrait lives in the
// same packed atlas as everything else. The alternatives were both worse: loading a second copy of
// the atlas texture in the UI translation unit (3 MB of VRAM to draw a 38x38 face), or letting
// raylib types cross the shell's interface, which is the one thing `screens.hpp` exists to prevent.
//
// So the bridge — which already owns the texture — exposes one function taking plain numbers. The
// `Fx` index is passed as an `int` deliberately: the shell does not include `atlas_slots.hpp` and
// does not need to know what an atlas is.
#pragma once

namespace mmo {

// Draw FX sprite `fx` (an `Fx` enumerator) at frame `frame`, centred on (cx, cy) in SCREEN pixels,
// scaled so its longest side is `size`. A no-op if the atlas failed to load.
void draw_ui_fx(int fx, int frame, float cx, float cy, float size);

// Draw ability icon `icon` (an `Icon` enumerator), lit or greyed, filling a `size`x`size` box whose
// TOP-LEFT is (x, y) in SCREEN pixels. Used by the HUD's two ability slots. A no-op if the atlas
// failed to load. `icon` is an `int` for the same reason `draw_ui_fx` takes one: the shell must not
// need to include `atlas_slots.hpp`.
void draw_ui_icon(int icon, bool disabled, float x, float y, float size);

}  // namespace mmo
