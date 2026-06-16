// metalslugxx - ReXGlue Recompiled Project
//
// Last-chance crash diagnostics. The SDK's REX_FATAL traps (unregistered
// function, unresolved branch) log + flush before aborting, but a genuine
// native access violation inside recompiled guest code has no logger anywhere
// in the SDK -- the process just dies and the run log ends mid-frame with no
// trace (see Phase E Run 4). This installs a SetUnhandledExceptionFilter that,
// on the first unhandled fault, symbolizes the faulting RIP and walks the stack
// to recover the recompiled `sub_<guestaddr>` frames, writes them to the rex
// log (flushed) and stderr, then lets the process terminate.
//
// Project-side glue only -- no SDK fork. Windows-only (matches the build).

#pragma once

namespace metalslugxx {

// Installs the unhandled-exception filter and primes DbgHelp against the
// process PDB. Safe to call once, after logging is up (OnPostInitLogging).
void InstallCrashHandler();

}  // namespace metalslugxx
