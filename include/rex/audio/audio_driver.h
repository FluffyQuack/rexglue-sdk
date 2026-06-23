/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/kernel.h>
#include <rex/memory.h>

namespace rex::audio {

// Health snapshot of a backend's playback pipeline, sampled by the audio worker
// to distinguish dropout modes (worker-side starvation vs. backend stall vs.
// frames flowing but silent). All counters are monotonic since driver creation.
struct AudioDriverHealth {
  uint64_t backend_callbacks = 0;  // times the backend asked us for audio
  uint64_t frames_consumed = 0;    // real queued frames the backend played
  uint64_t silence_fills = 0;      // silence buffers emitted (queue was empty)
  size_t queued = 0;               // frames currently queued, not yet consumed
};

class AudioDriver {
 public:
  explicit AudioDriver(memory::Memory* memory);
  virtual ~AudioDriver();

  virtual void SubmitFrame(uint32_t samples_ptr) = 0;

  // Number of decoded frames currently queued for playback but not yet
  // consumed by the backend. The audio worker uses this to detect (and recover
  // from) queue starvation. Drivers that don't track a queue return 0.
  virtual size_t GetQueuedFrameCount() const { return 0; }

  // Pipeline health for diagnostics/recovery. Default: nothing tracked.
  virtual AudioDriverHealth GetHealth() const { return {}; }

  // Attempt to recover a backend whose callback has stopped consuming frames
  // (e.g. a wedged/migrated audio device). Returns true if a recovery action
  // was taken. Default: no-op (driver has no recoverable backend).
  virtual bool RecoverStalledBackend() { return false; }

 protected:
  inline uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return memory_->TranslatePhysical(guest_address);
  }

  memory::Memory* memory_ = nullptr;
};

}  // namespace rex::audio
