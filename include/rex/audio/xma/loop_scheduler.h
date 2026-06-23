/**
 ******************************************************************************
 * XMA decoded-frame loop window scheduler                                    *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::audio::xma {

constexpr uint8_t kSubframesPerFrame = 4;
constexpr uint8_t kInfiniteLoopCount = 255;

struct DecodedFrameWindow {
  uint8_t begin_subframe = 0;
  uint8_t end_subframe = kSubframesPerFrame;

  uint8_t begin_output_block(bool is_stereo) const {
    return begin_subframe << static_cast<uint8_t>(is_stereo);
  }
  uint8_t end_output_block(bool is_stereo) const {
    return end_subframe << static_cast<uint8_t>(is_stereo);
  }
};

struct LoopFrameConfig {
  uint32_t loop_start_frame = 0;
  uint32_t loop_end_frame = 0;
  uint8_t loop_subframe_start = 0;
  uint8_t loop_subframe_end = 0;
  uint8_t loop_subframe_skip = 0;
  uint8_t loop_count = 0;
};

struct LoopFrameSchedule {
  DecodedFrameWindow window;
  bool hold_input_at_frame = false;
};

struct LoopFrameCompletion {
  bool rewind_to_loop_start = false;
  bool decrement_loop_count = false;
};

// Keeps XACT loop policy separate from physical XMA frame decoding. A frame is
// scheduled once, may be emitted over multiple decoder kicks, and is completed
// only after its selected PCM window has been fully consumed.
class LoopFrameScheduler {
 public:
  LoopFrameSchedule ScheduleFrame(uint32_t frame_offset, const LoopFrameConfig& config) {
    frame_pending_ = true;
    pending_loop_end_ = false;

    LoopFrameSchedule result;
    if (config.loop_count == 0) {
      return result;
    }

    const bool is_loop_start = frame_offset == config.loop_start_frame;
    const bool is_loop_end = frame_offset == config.loop_end_frame;
    if (is_loop_start) {
      loop_start_saved_ = true;
    }

    if (loop_iteration_active_ && is_loop_start) {
      // loop_subframe_skip is the guest-visible decoder boundary used by the
      // existing XMA context implementation. loop_subframe_start is retained
      // in the config because it is distinct XMA metadata.
      result.window.begin_subframe = std::min(config.loop_subframe_skip, kSubframesPerFrame);
    }
    if (is_loop_end) {
      // The field is encoded modulo one frame: zero is the exclusive boundary
      // after subframe 3, not the beginning of the frame.
      result.window.end_subframe = config.loop_subframe_end == 0
                                       ? kSubframesPerFrame
                                       : std::min(config.loop_subframe_end, kSubframesPerFrame);
      result.window.end_subframe =
          std::max(result.window.end_subframe, result.window.begin_subframe);
    }

    pending_loop_end_ = is_loop_end && loop_start_saved_ && config.loop_count != 0;
    result.hold_input_at_frame = pending_loop_end_;
    return result;
  }

  LoopFrameCompletion CompleteFrame(const LoopFrameConfig& config) {
    LoopFrameCompletion result;
    if (!frame_pending_) {
      return result;
    }

    frame_pending_ = false;
    if (!pending_loop_end_) {
      return result;
    }
    pending_loop_end_ = false;

    // The title may cancel a loop while the selected PCM window is pending.
    if (config.loop_count == 0) {
      loop_iteration_active_ = false;
      return result;
    }

    if (!loop_iteration_active_) {
      // The initial traversal reaches the loop end without consuming a loop.
      // The rewind makes the following traversal the first loop iteration.
      loop_iteration_active_ = true;
      result.rewind_to_loop_start = true;
      return result;
    }

    uint8_t remaining_loop_count = config.loop_count;
    if (remaining_loop_count != kInfiniteLoopCount) {
      result.decrement_loop_count = true;
      --remaining_loop_count;
    }

    if (remaining_loop_count != 0) {
      result.rewind_to_loop_start = true;
    } else {
      loop_iteration_active_ = false;
    }
    return result;
  }

  void Reset() {
    loop_start_saved_ = false;
    loop_iteration_active_ = false;
    frame_pending_ = false;
    pending_loop_end_ = false;
  }

  bool loop_start_saved() const { return loop_start_saved_; }
  bool loop_iteration_active() const { return loop_iteration_active_; }
  bool frame_pending() const { return frame_pending_; }

 private:
  bool loop_start_saved_ = false;
  bool loop_iteration_active_ = false;
  bool frame_pending_ = false;
  bool pending_loop_end_ = false;
};

}  // namespace rex::audio::xma
