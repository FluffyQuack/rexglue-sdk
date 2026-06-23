// exit2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp. The
// title-neutral glue (settings/INI, crash handler, timer fix, screenshot) lives
// in exit_common/ and is shared with EXIT 1; only path/keybind wiring and any
// address-bearing hooks are per-game here. EXIT 2 is the same engine as EXIT 1
// (shared dat/ tree, shared CRT), so this file is a near-verbatim sibling of
// exit1/src/exit1_app.h — the only differences are the class/app name; every
// override below is title-neutral (parameterized on GetName()) and was proven on
// EXIT 1, including the update:-device asset default (see OnConfigurePaths).

#pragma once

#include <filesystem>

#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

#include "exit_common/crash.h"
#include "exit_common/screenshot.h"
#include "exit_common/settings.h"
#include "exit_common/timing.h"

class Exit2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Exit2App>(new Exit2App(ctx, "exit2",
        PPCImageConfig));
  }

  // Install last-chance native-crash diagnostics as soon as the logger is up,
  // so an access violation in recompiled code yields a symbolized backtrace
  // (the recompiled `sub_<guestaddr>` frames) instead of a silent exit. Also
  // raise the host timer resolution to 1 ms so guest frame-pacing waits don't
  // overshoot the default ~15.6 ms quantum (see exit_common/timing.h). LoadConfig
  // is idempotent -- OnConfigurePaths ran first and already read the INI; this
  // just logs the resolved settings.
  void OnPostInitLogging() override {
    exit_common::InstallCrashHandler(GetName());
    exit_common::RaiseTimerResolution();
    const exit_common::Config& cfg = exit_common::LoadConfig(GetName());

    // Drive the XBLA trial/full state. The game asks "am I purchased?" via
    // XamContentGetLicenseMask, which the SDK answers from the `license_mask`
    // cvar (bit 0 = full license). TrialMode = False grants that bit so the full
    // game is unlocked; TrialMode = True leaves the mask at 0 (demo content
    // only). We control what the license API reports rather than touching guest
    // code. See exit_common/settings.h.
    rex::cvar::SetFlagByName("license_mask", cfg.trial_mode ? "0" : "1");
    REXLOG_INFO("license: TrialMode = {} -> license_mask = {}",
                cfg.trial_mode ? "True" : "False", cfg.trial_mode ? "0" : "1");
  }

  // Register project-specific keybinds. Called by the SDK right after the
  // built-in overlay binds (F3/F4/Backtick) are set up and the presenter is
  // live, so the presenter is reachable from the callback via runtime().
  //
  // F12 -> dump the guest-output framebuffer (pre present-scaling) to a PNG next
  // to the exe (via lodepng). General-purpose screenshot + a bring-up diagnostic
  // for comparing PC output against hardware captures: see
  // exit_common/screenshot.h.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* /*drawer*/) override {
    rex::ui::RegisterBind("bind_guest_capture", "F12", "Save guest-output screenshot (PNG)",
                          [this] { exit_common::DumpGuestOutput(runtime(), GetName()); });
  }

  // On quit, re-write exit2.ini from the live config so an INI from an older
  // build gets any newly-added keys populated (and comments refreshed), while
  // keeping whatever the user already set. See exit_common/settings.h.
  void OnShutdown() override { exit_common::SaveConfig(GetName()); }

  // Resolve the writable-data paths before the runtime locks them in. Three
  // project conventions (see exit_common/settings.h):
  //   1. If --game_data_root was not given, default it to the working directory
  //      (`./`) so the game runs against the data sitting next to it.
  //   2. EXIT loads its XEX from `game:` but reads all `dat/` assets from the
  //      `update:` device (this default was proven on EXIT 1, whose first asset
  //      open is `update:\dat\effect\game_button.gim`; EXIT 2 shares the engine
  //      and asset tree). With no `update:` mounted that open returns 0xC000000F
  //      and the title thread hangs before the first frame. The base `dat/` tree
  //      lives under the same dump root, and a separate title update can still
  //      override it via --update_data_root, so when the user did not pass one
  //      explicitly we default `update:` to the game root -- a single
  //      --game_data_root then boots the game.
  //   3. [System] Portable (default True) keeps the install self-contained: the
  //      SDK's user data, shader cache, and `<name>.toml` go in `./` instead of
  //      the per-user platform folders. This is the earliest hook that can read
  //      the INI (LoadConfig is idempotent; OnPostInitLogging reuses the result).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) return;  // can't resolve `./`; leave SDK defaults untouched

    if (paths.game_data_root.empty()) {
      paths.game_data_root = cwd;
      REXLOG_INFO("paths: --game_data_root not given; defaulting to working dir {}", cwd.string());
    }

    if (paths.update_data_root.empty()) {
      paths.update_data_root = paths.game_data_root;
      REXLOG_INFO("paths: --update_data_root not given; defaulting update: to game root {}",
                  paths.game_data_root.string());
    }

    if (exit_common::LoadConfig(GetName()).portable) {
      paths.user_data_root = cwd;
      paths.cache_root = cwd / "cache";
      paths.config_path = cwd / (GetName() + ".toml");
      REXLOG_INFO("paths: [System] Portable = True; writable data in working dir {}", cwd.string());
    }
  }

  // Override virtual hooks for customization:
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
};
