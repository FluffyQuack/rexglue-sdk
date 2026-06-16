// metalslugxx - ReXGlue Recompiled Project
//
// Tiny INI-style settings read at startup from `metalslugxx.ini`, located next to
// the executable (the same folder the SDK uses for `<name>.toml` and `logs/`).
//
// Design contract:
//   * Running with no INI is fine -- a default INI is written on first run.
//   * A missing file, missing section, or missing key all fall back to the
//     compiled-in defaults (the `Config` member initializers below).
//   * The format is forgiving: `[Section]` headers, `Key = Value` pairs,
//     `//`, `;` and `#` line comments, blank lines, and case-insensitive
//     section/key names.

#pragma once

#include <filesystem>
#include <string>

namespace metalslugxx {

// Parsed settings, pre-populated with the defaults used when a value is absent.
struct Config {
  // [System]
  //
  // Sleep = True : guest frame-pacing waits use a plain OS sleep (default;
  //                the proven-good behavior after the timeBeginPeriod(1) fix).
  // Sleep = False: waits sleep off the bulk of the interval then spin-wait the
  //                final ~1.5 ms so they land exactly on the deadline instead of
  //                overshooting the OS timer granularity (tighter pacing, a
  //                little more CPU). Wired to rex::thread::SetPreciseTimedWait.
  bool sleep = true;

  // Portable = True : keep this install self-contained -- the SDK's writable
  //                   data (user_data_root, the shader cache, and the runtime's
  //                   `<name>.toml`) lives in the working directory `./` instead
  //                   of the per-user platform folders (default). Combined with
  //                   game_data_root defaulting to `./`, the whole thing can be
  //                   dropped on a USB stick and run in place.
  // Portable = False: use the SDK's platform defaults (e.g. %USERPROFILE% on
  //                   Windows) for that writable data.
  //
  // Note `metalslugxx.ini` itself always lives in `./` -- it is the bootstrap
  // file that carries this flag, so its location can't depend on it. Applied in
  // OnConfigurePaths (which runs before the runtime's paths are locked in).
  bool portable = true;

  // [Game]
  //
  // TrialMode = True : the game runs in trial/demo mode -- only the content the
  //                    demo unlocks is accessible. This is what the XBLA build
  //                    does when the marketplace has not granted a full license.
  // TrialMode = False: full game; every mode/level is unlocked (default).
  //
  // Implemented by driving the XBLA license the game reads via
  // XamContentGetLicenseMask: False sets license bit 0 (purchased), True clears
  // it (trial). Applied to the `license_mask` cvar in OnPostInitLogging.
  bool trial_mode = false;

  // SkipLogos = True : skip the boot logo sequence (XBLA "Arcade" logo, the
  //                    US-region ESRB logo, and the SNK Playmore "Vendor" logo)
  //                    and go straight to the title screen.
  // SkipLogos = False: play the logos as on hardware (default).
  //
  // Implemented as the project's first guest-code patch: a [[midasm_hook]] at
  // 0x825715E8 (inside the boot-logo scene sub_82571500 = ms_boot_logo_sequence)
  // jumps over all three logo-display loops to the scene teardown at 0x82571700
  // when this is True. See metalslugxx_config.toml and metalslugxx_hooks.cpp.
  bool skip_logos = true;

  // UnlockLeona = True : the playable character Leona is unlocked (default).
  // UnlockLeona = False: Leona stays locked, as on a console without the add-on.
  //
  // On Xbox 360 Leona was a marketplace add-on: a DLC package whose 5-byte
  // `0.id` file held the text "leona". The game mounts that package as `DLC0`,
  // reads `DLC0:\0.id`, and unlocks her on a match. When True we synthesize that
  // package on disk (a `0.id` containing "leona") in the SDK content root so the
  // game's normal enumerate/mount/read path finds it -- no guest-code patch
  // needed. See metalslugxx_dlc.h. Applied in OnPostInitLogging.
  bool unlock_leona = true;

  // [Graphics]
  //
  // GameUpscaleFilter controls the texture filter used for every texture the game
  // samples -- both the pixel-art sprite/background draws and the tiled
  // (EDRAM-resolved) render-target upscale that builds the in-game playfield --
  // so the whole image can be forced crisp or smooth in one switch. Menus are
  // pixel art too and are affected as well.
  //   "default": keep the game's own filtering (stock; the native frame is
  //              point-doubled then bilinear-scaled, as on hardware).
  //   "linear" : force bilinear everywhere (smoothest).
  //   "point"  : force nearest everywhere, anisotropy off (crisp pixel-art look).
  //
  // Applied to the SDK `game_upscale_filter` cvar in OnPostInitLogging. Stored
  // lowercased; an unrecognized value falls back to "default".
  std::string game_upscale_filter = "default";

  // SampleTexelBias shifts the source sample coordinate of every 2D texture
  // fetch by this many texels. It is a DIAGNOSTIC knob for the in-game upscale
  // "shuffling": the playfield is built by sampling EDRAM-resolved render
  // targets through a multi-pass bilinear upscale, and a half-texel tap
  // misalignment makes that bilinear degenerate toward nearest (scrambling 1-px
  // HUD glyphs). Sweep this (e.g. -0.5..0.5) against a hardware capture to find
  // the phase that un-scrambles it. 0 = stock (default; no change). Currently
  // global -- it also shifts menu/background samples -- so it is for finding the
  // correction, not yet a shippable default. Drives the SDK
  // `game_sample_texel_bias` cvar in OnPostInitLogging.
  double sample_texel_bias = 0.0;
  // True only if the INI explicitly contained [Graphics] SampleTexelBias. When
  // false, OnPostInitLogging leaves the SDK cvar untouched so a command-line
  // `--game_sample_texel_bias=...` (parsed earlier, during cvar::Init) survives
  // instead of being clobbered back to the default.
  bool sample_texel_bias_set = false;

  // LowresTiledBias is the fix for the in-game ~2px up-left offset. It shifts the
  // sample coord by this many texels, but ONLY for the bottom-of-chain low-res
  // tiled render target (the ~320x240 playfield) -- unlike SampleTexelBias (every
  // tiled fetch, which cancels through the upscale chain), this is applied at a
  // single non-cancelling pass so the shift survives the ~4x upscale (-0.5 texel
  // ~= 2px down-right). -0.5 is the confirmed value (verified by eye 2026-06-14);
  // 0 disables. This is only an override of the SDK cvar's own -0.5 default; the
  // value here mirrors it so a freshly-generated INI documents the active setting.
  // Drives the SDK `game_lowres_tiled_bias` cvar in OnPostInitLogging.
  double lowres_tiled_bias = -0.5;
  // True only if the INI explicitly contained [Graphics] LowresTiledBias, so a
  // command-line `--game_lowres_tiled_bias=...` is not clobbered back to default.
  bool lowres_tiled_bias_set = false;

  // [Keyboard1] / [Keyboard2]
  //
  // Basic keyboard-as-controller support. [Keyboard1] drives player 1's emulated
  // Xbox 360 pad and [Keyboard2] a second player's, both alongside any real
  // controller and both reading the same physical keyboard -- so for local co-op
  // their bindings must not overlap (player 2 defaults to the numpad). Each
  // binding is a key NAME (the same vocabulary the in-game settings use), NOT an
  // SDL keycode: on Windows the game window is a native Win32 window and key
  // presses arrive as Win32 virtual keys, so there is no SDL keycode at the
  // binding layer. Accepted names include the letters A-Z, the digits 0-9,
  // F1-F24, and: Up, Down, Left, Right, Return, Escape, Space, Tab, Backspace,
  // Delete, Insert, Home, End, PageUp, PageDown, Shift, Control, Alt,
  // Numpad0-Numpad9, NumpadEnter, NumpadPlus, NumpadMinus, NumpadStar,
  // NumpadSlash. An unknown name disables that one binding (and is logged).
  // [Keyboard1] drives the SDK `keyboard_mode` / `kb_*` cvars and [Keyboard2] the
  // `keyboard2_mode` / `kb2_*` cvars in OnPostInitLogging.
  //
  // Player 1 default layout (D-pad on the arrows, face buttons on WASD):
  //   Up/Down/Left/Right -> D-pad      W -> Y    A -> X
  //   Return -> Start                  S -> A    D -> B
  //   Backspace -> Back/Select
  bool keyboard_enabled = true;
  std::string kb_dpad_up = "Up";
  std::string kb_dpad_down = "Down";
  std::string kb_dpad_left = "Left";
  std::string kb_dpad_right = "Right";
  std::string kb_button_x = "A";
  std::string kb_button_a = "S";
  std::string kb_button_b = "D";
  std::string kb_button_y = "W";
  std::string kb_lshoulder = "Q";
  std::string kb_rshoulder = "E";
  std::string kb_ltrigger = "1";
  std::string kb_rtrigger = "3";
  std::string kb_start = "Return";
  std::string kb_back = "Backspace";

  // Player 2. Disabled by default; defaults to the numpad so it does not collide
  // with player 1's keys when both share one keyboard.
  //   Numpad8/2/4/6 -> D-pad           Numpad9 -> Y   Numpad7 -> X
  //   NumpadEnter -> Start             Numpad1 -> A   Numpad3 -> B
  //   Numpad0 -> Back/Select
  bool keyboard2_enabled = false;
  std::string kb2_dpad_up = "I";
  std::string kb2_dpad_down = "K";
  std::string kb2_dpad_left = "J";
  std::string kb2_dpad_right = "L";
  std::string kb2_button_x = "F";
  std::string kb2_button_a = "G";
  std::string kb2_button_b = "H";
  std::string kb2_button_y = "T";
  std::string kb2_lshoulder = "R";
  std::string kb2_rshoulder = "Y";
  std::string kb2_ltrigger = "4";
  std::string kb2_rtrigger = "6";
  std::string kb2_start = "P";
  std::string kb2_back = "O";
};

// Loads `metalslugxx.ini` from the executable folder, writing a commented
// default file if none exists. Logs what it resolved. Safe to call once at
// startup (after logging is up). Returns a reference to the process-wide config.
const Config& LoadConfig();

// Returns the config resolved by LoadConfig() (defaults if never loaded).
const Config& config();

// Re-writes `metalslugxx.ini` from the live config, so any keys absent from an
// older INI get populated (and comments refreshed) while keeping the values the
// user already set. Written atomically (temp file + rename). Intended to be
// called once on shutdown. Logs the outcome; never throws.
void SaveConfig();

// Resolved path of the INI file (executable folder / "metalslugxx.ini").
std::filesystem::path ConfigPath();

}  // namespace metalslugxx
