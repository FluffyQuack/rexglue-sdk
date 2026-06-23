// exit_common - shared, game-agnostic ReXGlue glue
//
// See exit_common/timing.h for rationale.

#include "exit_common/timing.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod

#include <rex/logging/api.h>
#include <rex/logging/macros.h>

#pragma comment(lib, "winmm.lib")

namespace exit_common {

void RaiseTimerResolution() {
  // Guest timed waits (~16.6 ms per frame) lowered onto Win32 Sleep/
  // WaitForSingleObjectEx round up to the default ~15.6 ms scheduler quantum,
  // which can overshoot a one-frame wait to ~31 ms and slow the game. Requesting
  // 1 ms resolution tightens them to land within ~1 ms of the requested interval.
  MMRESULT r = timeBeginPeriod(1);
  if (r != TIMERR_NOERROR) {
    REXLOG_WARN("timeBeginPeriod(1) failed (code {}); in-game pacing may run slow",
                static_cast<unsigned>(r));
  } else {
    REXLOG_INFO("timer resolution raised to 1 ms (timeBeginPeriod) for frame pacing");
  }
  // Intentionally never timeEndPeriod: we want 1 ms resolution for the whole
  // process lifetime; the OS restores the default on process exit.
}

}  // namespace exit_common

#else  // !_WIN32

namespace exit_common {
void RaiseTimerResolution() {}
}  // namespace exit_common

#endif
