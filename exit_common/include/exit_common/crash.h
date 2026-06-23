// exit_common - shared, game-agnostic ReXGlue glue
//
// Last-chance crash diagnostics. The SDK's REX_FATAL traps (unregistered
// function, unresolved branch) log + flush before aborting, but a genuine
// native access violation inside recompiled guest code has no logger anywhere
// in the SDK -- the process just dies and the run log ends mid-frame with no
// trace. This installs a SetUnhandledExceptionFilter that, on the first
// unhandled fault, symbolizes the faulting RIP and walks the stack to recover
// the recompiled `sub_<guestaddr>` frames, writes them to the rex log (flushed)
// and stderr, then lets the process terminate.
//
// This is the game-neutral core lifted from the SCIV reference's sciv_crash.*;
// the SCIV file's TEMPORARY type-registry bring-up diagnostics (DumpTypeRegistry,
// ArmRegistryWriteWatch, the DR0 write-watchpoint, etc.) were SCIV-specific and
// are deliberately omitted -- per-game bring-up diagnostics belong in that game's
// own `<game>_hooks.cpp` / crash glue during its Phase 7, not in shared code.
//
// Project-side glue only -- no SDK fork. Windows-only (matches the build).

#pragma once

#include <string>

namespace exit_common {

// Installs the unhandled-exception filter and primes DbgHelp against the
// process PDB. `app_name` only tags the "installed" log line. Safe to call once,
// after logging is up (OnPostInitLogging).
void InstallCrashHandler(const std::string& app_name);

}  // namespace exit_common
