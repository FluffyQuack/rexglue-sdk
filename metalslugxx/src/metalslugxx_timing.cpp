// metalslugxx - ReXGlue Recompiled Project
//
// See metalslugxx_timing.h for rationale.

#include "metalslugxx_timing.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod

#include <rex/logging/api.h>
#include <rex/logging/macros.h>

#pragma comment(lib, "winmm.lib")

namespace metalslugxx {

void RaiseTimerResolution() {
  // The in-game loop throttles itself with guest timed waits (~16.6 ms) that the
  // SDK lowers onto Win32 Sleep/WaitForSingleObjectEx. At the default ~15.6 ms
  // scheduler quantum those round up to ~31 ms at random, halving the in-game
  // speed (present rate measured ~38 fps). Requesting 1 ms resolution tightens
  // them so the loop holds a clean 30 Hz sim / 60 fps present.
  MMRESULT r = timeBeginPeriod(1);
  if (r != TIMERR_NOERROR) {
    REXLOG_WARN("timeBeginPeriod(1) failed (code {}); in-game pacing may run slow",
                static_cast<unsigned>(r));
  }
  // Intentionally never timeEndPeriod: we want 1 ms resolution for the whole
  // process lifetime; the OS restores the default on process exit.
}

}  // namespace metalslugxx

#else  // !_WIN32

namespace metalslugxx {
void RaiseTimerResolution() {}
}  // namespace metalslugxx

#endif
