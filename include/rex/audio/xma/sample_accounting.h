/**
 ******************************************************************************
 * XMA physical-to-logical sample accounting                                  *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace rex::audio::xma {

struct DecodedSampleWindow {
  uint32_t begin_sample = 0;
  uint32_t end_sample = 0;

  uint32_t sample_count() const {
    return end_sample > begin_sample ? end_sample - begin_sample : 0;
  }

  bool empty() const { return begin_sample >= end_sample; }
};

struct DecodedSampleSelection {
  std::array<DecodedSampleWindow, 2> windows = {};
  uint8_t window_count = 0;

  uint32_t sample_count() const {
    uint32_t result = 0;
    for (uint8_t i = 0; i < window_count; ++i) {
      result += windows[i].sample_count();
    }
    return result;
  }

  bool empty() const { return window_count == 0; }
};

// Applies physical XMA start/end skip metadata to decoded PCM without owning
// XACT loop policy. Start delay is consumed once per stream activation, while
// end padding is removed from the last regular frame before its final IMDCT
// overlap is appended.
class DecodedStreamState {
 public:
  DecodedSampleSelection SelectFrameWindows(
      DecodedSampleWindow requested_window, uint32_t frame_sample_count,
      uint32_t final_overlap_samples, uint32_t start_skip_samples,
      uint32_t end_skip_samples, bool physical_eof) {
    if (!start_skip_captured_ && start_skip_samples != 0) {
      start_skip_remaining_ = start_skip_samples;
      start_skip_captured_ = true;
    }

    const uint32_t frame_start_skip =
        std::min(start_skip_remaining_, frame_sample_count);
    start_skip_remaining_ -= frame_start_skip;

    uint32_t frame_valid_end = frame_sample_count;
    if (physical_eof) {
      physical_eof_seen_ = true;
      frame_valid_end -= std::min(end_skip_samples, frame_valid_end);
    }

    const uint32_t decoded_sample_count =
        frame_sample_count + final_overlap_samples;
    requested_window.begin_sample =
        std::min(requested_window.begin_sample, decoded_sample_count);
    requested_window.end_sample =
        std::min(requested_window.end_sample, decoded_sample_count);

    DecodedSampleSelection selection;
    const auto append_intersection =
        [&](DecodedSampleWindow valid_window) {
          DecodedSampleWindow selected = {
              .begin_sample = std::max(requested_window.begin_sample,
                                       valid_window.begin_sample),
              .end_sample = std::min(requested_window.end_sample,
                                     valid_window.end_sample),
          };
          if (!selected.empty()) {
            selection.windows[selection.window_count++] = selected;
          }
        };

    append_intersection({
        .begin_sample = frame_start_skip,
        .end_sample = frame_valid_end,
    });

    const uint32_t overlap_start_skip =
        std::min(start_skip_remaining_, final_overlap_samples);
    start_skip_remaining_ -= overlap_start_skip;
    append_intersection({
        .begin_sample = frame_sample_count + overlap_start_skip,
        .end_sample = decoded_sample_count,
    });
    return selection;
  }

  DecodedSampleSelection SelectFrameWindows(
      DecodedSampleWindow requested_window, uint32_t frame_sample_count,
      uint32_t start_skip_samples, uint32_t end_skip_samples,
      bool physical_eof) {
    return SelectFrameWindows(requested_window, frame_sample_count, 0,
                              start_skip_samples, end_skip_samples,
                              physical_eof);
  }

  void Reset() {
    start_skip_remaining_ = 0;
    start_skip_captured_ = false;
    physical_eof_seen_ = false;
  }

  uint32_t start_skip_remaining() const { return start_skip_remaining_; }
  bool physical_eof_seen() const { return physical_eof_seen_; }

 private:
  uint32_t start_skip_remaining_ = 0;
  bool start_skip_captured_ = false;
  bool physical_eof_seen_ = false;
};

inline DecodedSampleWindow GetSelectionBounds(
    const DecodedSampleSelection& selection) {
  if (selection.empty()) {
    return {};
  }
  return {
      .begin_sample = selection.windows.front().begin_sample,
      .end_sample =
          selection.windows[selection.window_count - 1].end_sample,
  };
}

}  // namespace rex::audio::xma
