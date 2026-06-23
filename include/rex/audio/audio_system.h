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

#include <atomic>
#include <queue>
#include <thread>

#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::audio {

constexpr memory::fourcc_t kAudioSaveSignature = memory::make_fourcc("XAUD");

class AudioDriver;
class XmaDecoder;

class AudioSystem : public system::IAudioSystem {
 public:
  virtual ~AudioSystem();

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  XmaDecoder* xma_decoder() const { return xma_decoder_.get(); }

  virtual X_STATUS Setup(system::KernelState* kernel_state);
  virtual void Shutdown();

  X_STATUS RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index);
  void UnregisterClient(size_t index);
  void SubmitFrame(size_t index, uint32_t samples_ptr);

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  explicit AudioSystem(runtime::FunctionDispatcher* function_dispatcher);

  virtual void Initialize();

  void WorkerThreadMain();

  // Watchdog: the single worker thread runs guest audio callbacks inline, so a
  // guest callback that blocks forever (a guest wait/lock/spin that never
  // completes) wedges ALL audio with no log output -- the worker never returns
  // to its wait loop to time out. This host-side monitor detects that (a
  // callback in flight too long, or the worker loop not advancing while neither
  // in a callback nor paused) and dumps the worker's native stack so the
  // offending guest function can be identified and named.
  void WatchdogThreadMain();

  virtual X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                AudioDriver** out_driver) = 0;
  virtual void DestroyDriver(AudioDriver* driver) = 0;

  static constexpr size_t kMaximumQueuedFrames = 64;

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  std::unique_ptr<XmaDecoder> xma_decoder_;
  uint32_t queued_frames_;

  std::atomic<bool> worker_running_ = {false};
  system::object_ref<system::XHostThread> worker_thread_;

  // Watchdog state (see WatchdogThreadMain). All cross-thread, so atomic.
  std::thread watchdog_thread_;
  std::atomic<bool> watchdog_running_ = {false};
  std::atomic<void*> worker_native_handle_ = {nullptr};
  std::atomic<uint32_t> cb_active_callback_ = {0};  // guest addr of in-flight callback, 0 = none
  std::atomic<int64_t> cb_active_since_ms_ = {0};   // steady-clock ms when it started
  std::atomic<uint64_t> cb_generation_ = {0};       // bumped per dispatch (dedupes dumps)
  std::atomic<uint64_t> worker_heartbeat_ = {0};    // bumped each worker loop iteration

  rex::thread::global_critical_region global_critical_region_;
  static const size_t kMaximumClientCount = 8;
  struct {
    AudioDriver* driver;
    uint32_t callback;
    uint32_t callback_arg;
    uint32_t wrapped_callback_arg;
    bool in_use;
  } clients_[kMaximumClientCount];

  int FindFreeClient();

  std::unique_ptr<rex::thread::Semaphore> client_semaphores_[kMaximumClientCount];
  // Event is always there in case we have no clients.
  std::unique_ptr<rex::thread::Event> shutdown_event_;
  rex::thread::WaitHandle* wait_handles_[kMaximumClientCount + 1];

  std::atomic<bool> paused_ = {false};
  rex::thread::Fence pause_fence_;
  std::unique_ptr<rex::thread::Event> resume_event_;
};

}  // namespace rex::audio
