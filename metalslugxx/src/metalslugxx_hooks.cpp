// metalslugxx - ReXGlue Recompiled Project
//
// Mid-ASM hooks and function-level fixups for places the static lift could not
// fully resolve (the GoldenEye `ge_hooks.cpp` analogue), plus deliberate game
// patches. See include/rex/hook.h for the authoring API.

#include <rex/hook.h>

#include "metalslugxx_settings.h"

// ---------------------------------------------------------------------------
// [Game] SkipLogos — skip the boot logo sequence.
//
// sub_82571500 (ms_boot_logo_sequence) is the opening scene step that, after
// some setup, plays three logos back to back, each as "load the texture, then a
// `while (tick() == 996 /*busy*/)` fade-in/hold/fade-out wait loop":
//   * XBLA "Arcade" logo  (ms_load_arcade_logo  0x8256D288, tick 0x8256F420)
//   * ESRB logo, US only  (ms_load_esrb_logo    0x8256D438, tick 0x8256F700)
//   * SNK Playmore logo   (ms_load_vendor_logo  0x8256D490, tick 0x8256FA20)
// then runs teardown/transition code (0x82571700..) that sets up the next scene.
//
// We do NOT jump over the whole display region: each logo's load and its first
// tick initialize draw state that the later loading-screen/title transition
// reuses (jumping straight to 0x82571700 crashed with a null-deref in the
// "Loading…" render). Instead, three [[midasm_hook]]s (metalslugxx_config.toml)
// sit on the loop-back `beq` after each tick; when this returns true the
// recompiler jumps just past that loop, so each logo runs its load + exactly one
// tick (alpha≈0 at t≈0, so it's effectively invisible, and all per-logo init
// runs) and then bails out of the multi-second fade wait. When false the logos
// play exactly as on hardware (the hook is a cheap bool read per frame).
//
// The codegen emits `extern bool msxx_skip_boot_logos();` in the generated TU,
// so this must be a global-scope (non-namespaced, non-extern-"C") definition
// whose name matches the hooks' `name`.
bool msxx_skip_boot_logos() {
  return metalslugxx::config().skip_logos;
}

// ---------------------------------------------------------------------------
// [Game] Integer playfield scale.
//
// ms_present_playfield_upscale builds the final quad for the 640x480
// intermediate playfield texture. Stock code letterboxes the original 320x240
// content to 864x648 inside the 1280x720 frame (2.7x from the original pixel
// buffer). When the user requests point filtering, force the quad to a centered
// 960x720 rectangle so the combined 320x240 -> screen scale is exactly 3.0x.
void msxx_force_integer_playfield_scale(PPCRegister& left, PPCRegister& top,
                                        PPCRegister& right, PPCRegister& bottom) {
  if (metalslugxx::config().game_upscale_filter != "point") {
    return;
  }

  left.f64 = 160.0;
  top.f64 = 0.0;
  right.f64 = 1120.0;
  bottom.f64 = 720.0;
}
