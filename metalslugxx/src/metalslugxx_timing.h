// metalslugxx - ReXGlue Recompiled Project
//
// Host timer-resolution fix for frame pacing.
//
// MSXX is a fixed-tick 60fps title whose main loop throttles via guest timed
// waits (KeWaitForSingleObject with a timeout / KeDelayExecutionThread), which
// the SDK lowers onto Win32 Sleep / WaitForSingleObjectEx with millisecond
// timeouts. Those honour the *system timer resolution*, which defaults to the
// scheduler quantum (~15.6 ms). A ~16.6 ms one-frame wait then overshoots to the
// next ~15.6 ms boundary, randomly landing at ~31 ms -> the game advances its
// simulation every 3-4 displayed frames instead of every 2 (observed jitter,
// half speed). Raising the resolution to 1 ms (timeBeginPeriod) tightens those
// waits so they land within ~1 ms of the requested interval.
//
// Project-side glue only -- no SDK fork. Windows-only (matches the build).

#pragma once

namespace metalslugxx {

// Request 1 ms system timer resolution and log the granularity actually
// achieved (so a run log confirms whether the fix took). Call once, after
// logging is up (OnPostInitLogging).
void RaiseTimerResolution();

}  // namespace metalslugxx
