// exit1 - ReXGlue Recompiled Project

#include "generated/default/exit1_init.h"

#include "exit1_app.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>

// ===========================================================================
// Audio reconfig spin-barrier fix (permanent audio-cutout root cause)
// ===========================================================================
// EXIT 1 is the same audio engine as EXIT 2; this is the byte-identical barrier at
// EXIT1 address 0x8212C7E8 (EXIT2 was 0x8215B010). See exit2/src/main.cpp and
// EXIT_AUDIO_BARRIER_WIP.md for the full diagnosis. In short:
//
// The guest's Audio_RenderCoreSpinBarrier is a non-sense-reversing barrier: each
// expected CPU stamps check_in[cpu]=1 in a packed word, spins until the word equals
// the expected-CPU mask, and the last arriver zeroes the whole word and returns --
// then it is reused immediately. That is only safe in lock-step (Xenon's dedicated
// hardware threads). Under ReXGlue the two participants (MixerSide/WorkerSide) are
// host-OS preemptible and oversubscribed, so one laps the other: the laggard is stuck
// spinning in a round it already checked into and never re-stamps, so the word never
// again reaches 0 or the full mask -> both spin forever, the reconfig never finalizes,
// the render callback stays parked, and all audio dies. The reconfig is REAL (skipping
// it crashes the mixer), so we must let it COMPLETE; it just must not LAP.
//
// Fix: override the recompiled barrier (its generated definition is a weak alias, so
// this strong REX_HOOK_RAW wins at link) with a generation-counting barrier that keeps
// the exact rendezvous contract but BLOCKS instead of busy-waiting and counts arrivals
// per generation, so a fast thread cannot re-enter and lap the slow one (it blocks at
// the next round until its partner arrives). Lock-step by construction, like hardware.
namespace {

struct HostBarrier {
  std::mutex m;
  std::condition_variable cv;
  unsigned waiting = 0;   // participants currently parked in this generation
  uint64_t generation = 0;
};

std::mutex g_barriers_mutex;
// Keyed by the guest check-in-word address (render_obj+0x164 / +0x16C). Node-based,
// so a reference stays valid after other insertions; only ever a couple of entries.
std::unordered_map<uint32_t, HostBarrier> g_barriers;

HostBarrier& GetHostBarrier(uint32_t word_addr) {
  std::lock_guard<std::mutex> lk(g_barriers_mutex);
  return g_barriers[word_addr];
}

}  // namespace

// Strong override of the weak generated Audio_RenderCoreSpinBarrier (0x8212C7E8).
// r3 = render object, r4 = check-in-word base. Mirrors the guest's offsets:
//   +0x130 reconfig count (0 => barrier inert, early return -- the guest `beqlr`)
//   +0x134..+0x148 per-CPU expected slots (nonzero => that CPU participates)
REX_HOOK_RAW(Audio_RenderCoreSpinBarrier) {
  const uint32_t render_obj = ctx.r3.u32;
  const uint32_t word_addr = ctx.r4.u32;

  // Inert outside a reconfig -- the common case (every normal render frame), so keep
  // it to a single guest load before returning.
  if (REX_LOAD_U32(render_obj + 0x130) == 0) {
    return;
  }

  // How many CPUs are expected at this barrier (one nonzero slot per participant).
  unsigned target = 0;
  for (uint32_t off = 0x134; off <= 0x148; off += 4) {
    if (REX_LOAD_U32(render_obj + off) != 0) {
      ++target;
    }
  }
  if (target <= 1) {
    return;  // no one to rendezvous with
  }

  HostBarrier& b = GetHostBarrier(word_addr);
  std::unique_lock<std::mutex> lk(b.m);
  const uint64_t my_generation = b.generation;
  if (++b.waiting >= target) {
    // Last arriver: open the gate for this generation and reset for the next round.
    b.waiting = 0;
    ++b.generation;
    lk.unlock();
    b.cv.notify_all();
    return;
  }

  // Wait for the gate. The timeout is a safety net only (a vanished partner during
  // shutdown / a state change must not wedge us forever); a live reconfig releases in
  // microseconds and never reaches it. On timeout, undo our arrival so the count
  // stays balanced for subsequent rounds.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (b.generation == my_generation) {
    if (b.cv.wait_until(lk, deadline) == std::cv_status::timeout &&
        b.generation == my_generation) {
      if (b.waiting > 0) {
        --b.waiting;
      }
      return;
    }
  }
}

REX_DEFINE_APP(exit1, Exit1App::Create)
