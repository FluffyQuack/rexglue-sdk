// metalslugxx - ReXGlue Recompiled Project
//
// Mid-ASM hooks and function-level fixups for places the static lift could not
// fully resolve (the GoldenEye `ge_hooks.cpp` analogue), plus deliberate game
// patches. See include/rex/hook.h for the authoring API.

#include <rex/hook.h>

#include "metalslugxx_config.h"

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
