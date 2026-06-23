/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

REXCVAR_DEFINE_INT32(
    audio_maxqframes, 8, "Audio",
    "Max buffered audio frames (range 4-64). Lower reduces latency but may cause stuttering.");

REXCVAR_DEFINE_BOOL(
    audio_self_heal, true, "Audio",
    "Recover automatically if the audio worker stops receiving backend wake "
    "signals (which would otherwise drain the queue and silence ALL output "
    "permanently until restart) by refilling a starved client's queue on the "
    "worker's wait timeout.");
REXCVAR_DEFINE_BOOL(
    audio_diagnostics, false, "Audio",
    "Log audio worker health diagnostics (slow guest callbacks, self-heal "
    "recoveries) to help track down audio dropouts.");
REXCVAR_DEFINE_INT32(
    audio_watchdog_timeout_ms, 2000, "Audio",
    "If a guest audio callback runs (or the audio worker loop stalls) for this "
    "many ms, dump the worker thread's native stack -- catches a guest callback "
    "hung on a guest wait/lock that silences all audio with no other log output. "
    "0 disables the watchdog.");
REXCVAR_DEFINE_INT32(
    audio_callback_wait_timeout_ms, 200, "Audio",
    "Clamp infinite (NULL-timeout) guest waits issued from inside the audio "
    "render callback to this many ms. The callback can do a cross-thread "
    "rendezvous with the game's mixer thread; if that ever stalls, an unbounded "
    "wait wedges the audio worker and silences ALL output until restart. "
    "Clamping degrades a stalled rendezvous to a skipped frame. Only ever fires "
    "on the stall path (normal playback never waits here). 0 disables.");

namespace {

// A guest audio callback runs inline on the worker thread; while it runs, no
// frames are produced. Warn (throttled) when one is pathologically slow, which
// points at a callback blocked on a guest object as the cause of a dropout.
void ReportSlowCallback(size_t client, uint32_t callback, int64_t ms) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1) + 1;
  const bool power_of_two = (n & (n - 1)) == 0;
  if (n <= 8 || power_of_two || REXCVAR_GET(audio_diagnostics)) {
    REXAPU_WARN(
        "AudioWorker: guest callback {:08X} (client {}) took {} ms; no audio is "
        "produced while it runs (total slow callbacks: {}).",
        callback, client, ms, n);
  }
}

// Throttled report when the worker had to re-prime a starved client's queue to
// escape what would otherwise be permanent silence.
void ReportSelfHeal(size_t client, uint32_t refills) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1) + 1;
  const bool power_of_two = (n & (n - 1)) == 0;
  if (n <= 8 || power_of_two || REXCVAR_GET(audio_diagnostics)) {
    REXAPU_WARN(
        "AudioWorker: client {} audio queue starved (no backend wake signal); "
        "refilled {} frame(s) to recover playback (total recoveries: {}). Set "
        "audio_diagnostics=true for per-event detail.",
        client, refills, n);
  }
}

// Throttled report on every worker wait timeout. A timeout means the backend
// released no wake signal for 500ms (it consumed no frame), which only happens
// during a dropout -- so this line pinpoints the onset and classifies the mode
// (queue-starved vs. backend-stalled) with the deltas needed to confirm it.
void ReportWorkerTimeout(size_t client, const rex::audio::AudioDriverHealth& h, uint64_t d_callbacks,
                         uint64_t d_consumed, uint64_t d_silence, uint32_t refills,
                         const char* mode) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = count.fetch_add(1) + 1;
  const bool power_of_two = (n & (n - 1)) == 0;
  if (n <= 16 || power_of_two || REXCVAR_GET(audio_diagnostics)) {
    REXAPU_WARN(
        "AudioWorker: wait timeout [{}] client {} queued={} dCallbacks={} dConsumed={} "
        "dSilence={} refills={} (total timeouts: {}).",
        mode, client, h.queued, d_callbacks, d_consumed, d_silence, refills, n);
  }
}

}  // namespace

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

namespace rex::audio {

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)), static_cast<uint32_t>(4)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  // Safety net: Shutdown() normally stops the watchdog, but never leave a
  // joinable std::thread (it would call std::terminate on destruction).
  watchdog_running_ = false;
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();

  // Start the audio worker watchdog. It needs the worker's real OS thread
  // handle (a pseudo-handle can't be suspended from another thread), which is
  // valid now that Create() has returned.
  if (auto* t = worker_thread_->thread()) {
    worker_native_handle_.store(t->native_handle(), std::memory_order_relaxed);
  }
  watchdog_running_ = true;
  watchdog_thread_ = std::thread([this]() { WatchdogThreadMain(); });

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // Runs one client guest callback (which mixes and submits a single audio
  // frame). Timed: the callback runs inline on this thread, so a slow or
  // blocked one stalls all audio production -- surface that in the log.
  auto execute_client = [&](size_t index, uint32_t callback, uint32_t arg) {
    SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
    uint64_t args[] = {arg};
    const auto start = std::chrono::steady_clock::now();
    // Arm the watchdog: publish the start time and generation first, then the
    // callback address last (release) as the "in flight" signal, so the
    // watchdog never pairs a fresh callback with a stale start time.
    cb_active_since_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  start.time_since_epoch())
                                  .count(),
                              std::memory_order_relaxed);
    cb_generation_.fetch_add(1, std::memory_order_relaxed);
    cb_active_callback_.store(callback, std::memory_order_release);

    // Bound any infinite guest wait the callback makes (it can do a cross-thread
    // rendezvous with the game's mixer thread), so a stalled rendezvous degrades
    // to a skipped frame instead of wedging this worker -- and all audio -- forever.
    worker_thread_->set_bounded_infinite_wait_ms(
        static_cast<uint32_t>(std::max(0, REXCVAR_GET(audio_callback_wait_timeout_ms))));
    function_dispatcher_->Execute(worker_thread_->thread_state(), callback, args,
                                  rex::countof(args));
    worker_thread_->set_bounded_infinite_wait_ms(0);

    cb_active_callback_.store(0, std::memory_order_release);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (ms >= 250) {
      ReportSlowCallback(index, callback, ms);
    }
  };

  // Per-client backend-health snapshot from the previous wait timeout, used to
  // compute deltas and apply hysteresis before recovering a stalled backend.
  struct ClientHealthPrev {
    uint64_t callbacks = 0;
    uint64_t consumed = 0;
    uint64_t silence = 0;
    uint32_t stall_ticks = 0;
  };
  std::array<ClientHealthPrev, kMaximumClientCount> health_prev{};

  // Recovery path. In normal operation the backend (SDL) releases a client
  // semaphore every few ms as it consumes queued frames, so the wait below
  // essentially never times out. A full 500 ms timeout therefore means the
  // backend went quiet (no frame consumed) -- i.e. a dropout. Two distinct
  // modes, told apart by queue depth:
  //   * queue near-empty  -> worker-side starvation: the queue drained and the
  //     backend now plays silence (releasing no semaphore), so nothing will wake
  //     this worker again. Refilling re-primes the backend back into its normal
  //     signal-driven flow.
  //   * queue near-full   -> backend stall: the backend's own callback stopped
  //     firing (wedged/migrated device) while audio is still queued. Refilling
  //     can't help (the queue isn't starved); the stream itself must be
  //     recovered. Done after brief hysteresis to ignore one-off hiccups.
  // Bounded by the queue target so a genuinely dead device can't make us spin or
  // grow memory without limit.
  auto on_timeout = [&]() {
    struct ActiveClient {
      size_t index;
      uint32_t callback;
      uint32_t arg;
      AudioDriver* driver;
    };
    std::array<ActiveClient, kMaximumClientCount> active;
    size_t active_count = 0;
    {
      auto global_lock = global_critical_region_.Acquire();
      for (size_t i = 0; i < kMaximumClientCount; ++i) {
        if (clients_[i].in_use && clients_[i].callback && clients_[i].driver) {
          active[active_count++] = {i, clients_[i].callback, clients_[i].wrapped_callback_arg,
                                    clients_[i].driver};
        }
      }
    }

    const size_t target = std::max<size_t>(1, queued_frames_ / 2);
    const bool self_heal_on = REXCVAR_GET(audio_self_heal);
    for (size_t a = 0; a < active_count && worker_running_; ++a) {
      const size_t idx = active[a].index;
      AudioDriver* driver = active[a].driver;
      const AudioDriverHealth h = driver->GetHealth();
      ClientHealthPrev& p = health_prev[idx];
      const uint64_t d_callbacks = h.backend_callbacks - p.callbacks;
      const uint64_t d_consumed = h.frames_consumed - p.consumed;
      const uint64_t d_silence = h.silence_fills - p.silence;

      uint32_t refills = 0;
      const char* mode;
      if (h.queued < target) {
        // Worker-side starvation: refill to re-prime the backend.
        mode = "queue-starved";
        while (worker_running_ && self_heal_on && refills < queued_frames_ &&
               driver->GetQueuedFrameCount() < target) {
          execute_client(idx, active[a].callback, active[a].arg);
          ++refills;
        }
        p.stall_ticks = 0;
        if (refills > 0) {
          ReportSelfHeal(idx, refills);
        }
      } else if (d_callbacks == 0 && d_consumed == 0) {
        // Backend stall: queue still holds audio but the callback stopped firing.
        mode = "backend-stalled";
        p.stall_ticks++;
        if (self_heal_on && p.stall_ticks >= 2 && driver->RecoverStalledBackend()) {
          p.stall_ticks = 0;
        }
      } else {
        // Backend still active but the wait happened to time out -- transient.
        mode = "transient";
        p.stall_ticks = 0;
      }

      ReportWorkerTimeout(idx, h, d_callbacks, d_consumed, d_silence, refills, mode);
      p.callbacks = h.backend_callbacks;
      p.consumed = h.frames_consumed;
      p.silence = h.silence_fills;
    }
  };

  // Main run loop.
  while (worker_running_) {
    // Heartbeat for the watchdog: a healthy worker advances this at least every
    // 500ms (the WaitAny timeout), so a frozen value while not paused and not in
    // a callback means the loop itself is wedged (e.g. blocked on a lock).
    worker_heartbeat_.fetch_add(1, std::memory_order_relaxed);
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    auto result = rex::thread::WaitAny(wait_handles_, rex::countof(wait_handles_), true,
                                       std::chrono::milliseconds(500));
    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      on_timeout();
      continue;
    }

    if (result.first == thread::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        thread::Wait(resume_event_.get(), false);
      }

      continue;
    }

    if (result.first == rex::thread::WaitResult::kSuccess) {
      auto index = result.second;

      auto global_lock = global_critical_region_.Acquire();
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      global_lock.unlock();

      if (client_callback) {
        execute_client(index, client_callback, client_callback_arg);
      } else {
        REXAPU_DEBUG("AudioWorker: semaphore signaled for client {} but callback is 0", index);
      }
    }

    if (!worker_running_) {
      break;
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

void AudioSystem::WatchdogThreadMain() {
  rex::thread::set_current_thread_name("Audio Watchdog");

  constexpr auto kPoll = std::chrono::milliseconds(200);
  const auto now_ms = []() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };

  uint64_t last_dumped_gen = UINT64_MAX;  // callback episode already reported
  uint64_t last_heartbeat = 0;
  int64_t heartbeat_changed_ms = now_ms();
  bool heartbeat_dumped = false;

  while (watchdog_running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(kPoll);
    if (!watchdog_running_.load(std::memory_order_relaxed)) {
      break;
    }

    const int threshold = REXCVAR_GET(audio_watchdog_timeout_ms);
    void* handle = worker_native_handle_.load(std::memory_order_relaxed);
    if (threshold <= 0 || !handle || paused_.load(std::memory_order_relaxed)) {
      // Disabled, not yet started, or legitimately parked: reset stall tracking
      // so a resume doesn't immediately look like a hang.
      last_heartbeat = worker_heartbeat_.load(std::memory_order_relaxed);
      heartbeat_changed_ms = now_ms();
      heartbeat_dumped = false;
      continue;
    }

    const int64_t now = now_ms();

    // (1) Stuck inside a guest callback: the worker dispatched a callback that
    // has not returned. This is the wake-chain-killing case -- no SubmitFrame,
    // queue drains, SDL plays silence forever, and nothing else logs.
    const uint32_t cb = cb_active_callback_.load(std::memory_order_acquire);
    if (cb != 0) {
      const uint64_t gen = cb_generation_.load(std::memory_order_relaxed);
      const int64_t since = cb_active_since_ms_.load(std::memory_order_relaxed);
      if (now - since >= threshold && gen != last_dumped_gen) {
        REXAPU_WARN(
            "AudioWorker watchdog: guest callback {:08X} has not returned after {} ms -- "
            "audio is wedged (no frames produced, queue will drain to permanent silence). "
            "Dumping worker thread stack to identify the blocking guest call:",
            cb, now - since);
        rex::debug::DumpThreadBacktrace(handle, "audio worker (stuck in guest callback)");
        last_dumped_gen = gen;
      }
      // A callback in flight legitimately freezes the heartbeat; don't also flag
      // it as a loop stall.
      last_heartbeat = worker_heartbeat_.load(std::memory_order_relaxed);
      heartbeat_changed_ms = now;
      heartbeat_dumped = false;
      continue;
    }

    // (2) Worker loop not progressing while not in a callback and not paused --
    // e.g. blocked acquiring a lock, or a wait that never returns.
    const uint64_t hb = worker_heartbeat_.load(std::memory_order_relaxed);
    if (hb != last_heartbeat) {
      last_heartbeat = hb;
      heartbeat_changed_ms = now;
      heartbeat_dumped = false;
    } else if (!heartbeat_dumped && now - heartbeat_changed_ms >= threshold) {
      REXAPU_WARN(
          "AudioWorker watchdog: worker loop made no progress for {} ms (not in a callback, "
          "not paused) -- audio worker appears wedged. Dumping worker thread stack:",
          now - heartbeat_changed_ms);
      rex::debug::DumpThreadBacktrace(handle, "audio worker (loop stalled)");
      heartbeat_dumped = true;  // report once per stall episode
    }
  }
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  // Stop the watchdog first: it suspends/resumes the worker thread, so it must
  // not be running when we terminate that thread below or invalidate its handle.
  watchdog_running_ = false;
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }
  worker_native_handle_.store(nullptr, std::memory_order_relaxed);

  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked
    // on guest objects (e.g. KeWaitForMultipleObjects).
    // Terminate the thread to break the deadlock.
    worker_thread_->Terminate(0);
    worker_thread_.reset();
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXAPU_DEBUG("AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXAPU_DEBUG("AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::thread::WaitResult wait_result;
  do {
    wait_result = rex::thread::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::thread::WaitResult::kSuccess);
  assert_true(wait_result == rex::thread::WaitResult::kTimeout);
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      REXAPU_ERROR(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio
