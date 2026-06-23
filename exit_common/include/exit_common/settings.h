// exit_common - shared, game-agnostic ReXGlue glue
//
// Tiny INI-style settings read at startup from `<app>.ini`, located in the
// working directory (the same folder the SDK uses for `<app>.toml` and
// `logs/`). This is the game-neutral core lifted from the SCIV reference's
// sciv_settings.* -- parameterized by the app name so EXIT 1 and EXIT 2 (and
// any future title in this repo) share one implementation.
//
// Design contract:
//   * Running with no INI is fine -- a default INI is written on first run.
//   * A missing file, missing section, or missing key all fall back to the
//     compiled-in defaults (the `Config` member initializers below).
//   * The format is forgiving: `[Section]` headers, `Key = Value` pairs,
//     `//`, `;` and `#` line comments, blank lines, and case-insensitive
//     section/key names.
//
// Kept deliberately minimal: only settings whose underlying SDK support exists
// in the clean baseline are exposed. Naming follows the MSXX/SCIV lesson
// (`*_settings.*`, never `*_config.*`) to avoid colliding with the generated
// codegen config names.

#pragma once

#include <filesystem>
#include <string>

namespace exit_common {

// Parsed settings, pre-populated with the defaults used when a value is absent.
struct Config {
  // [System]
  //
  // Portable = True : keep this install self-contained -- the SDK's writable
  //                   data (user_data_root, the shader cache, and the runtime's
  //                   `<app>.toml`) lives in the working directory `./` instead
  //                   of the per-user platform folders (default). Combined with
  //                   game_data_root defaulting to `./`, the whole thing can be
  //                   dropped on a USB stick and run in place.
  // Portable = False: use the SDK's platform defaults (e.g. %USERPROFILE% on
  //                   Windows) for that writable data.
  //
  // Note `<app>.ini` itself always lives in `./` -- it is the bootstrap file
  // that carries this flag, so its location can't depend on it. Applied in
  // OnConfigurePaths (which runs before the runtime's paths are locked in).
  bool portable = true;

  // [Game]
  //
  // TrialMode = True : the game runs in trial/demo mode -- only the content the
  //                    demo unlocks is accessible (default). This is what the
  //                    XBLA build does when the marketplace has not granted a
  //                    full license.
  // TrialMode = False: full game; every mode/level is unlocked.
  //
  // Implemented by driving the XBLA license the game reads via
  // XamContentGetLicenseMask: False sets license bit 0 (purchased), True clears
  // it (trial). Applied to the `license_mask` cvar in OnPostInitLogging.
  bool trial_mode = false;
};

// Loads `<app_name>.ini` from the working directory, writing a commented default
// file if none exists. Logs what it resolved. Safe to call repeatedly (it loads
// once and caches); the first call fixes the app name and INI path. Returns a
// reference to the process-wide config.
const Config& LoadConfig(const std::string& app_name);

// Returns the config resolved by LoadConfig() (defaults if never loaded).
const Config& config();

// Re-writes `<app_name>.ini` from the live config, so any keys absent from an
// older INI get populated (and comments refreshed) while keeping the values the
// user already set. Written atomically (temp file + rename). Intended to be
// called once on shutdown. Logs the outcome; never throws.
void SaveConfig(const std::string& app_name);

// Resolved path of the INI file (working directory / "<app_name>.ini").
std::filesystem::path ConfigPath(const std::string& app_name);

}  // namespace exit_common
