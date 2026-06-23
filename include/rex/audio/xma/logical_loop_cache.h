/**
 ******************************************************************************
 * XMA logical loop PCM cache                                                  *
 ******************************************************************************
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rex::audio::xma {

struct LogicalLoopCacheKey {
  uint32_t input_buffer_address = 0;
  uint32_t frame_offset = 0;
  uint8_t begin_subframe = 0;
  uint8_t end_subframe = 0;
  uint8_t sample_rate = 0;
  bool is_stereo = false;

  bool operator==(const LogicalLoopCacheKey&) const = default;
};

// Retains a decoded logical loop window so repeated traversals do not require
// a physical decoder drain, flush, and re-decode cycle.
template <size_t Capacity>
class LogicalLoopCache {
 public:
  bool Store(const LogicalLoopCacheKey& key, const uint8_t* pcm,
             size_t byte_count) {
    if (!pcm || byte_count == 0 || byte_count > Capacity) {
      Reset();
      return false;
    }

    key_ = key;
    std::memcpy(pcm_.data(), pcm, byte_count);
    byte_count_ = byte_count;
    valid_ = true;
    return true;
  }

  void Reset() {
    key_ = {};
    byte_count_ = 0;
    valid_ = false;
  }

  bool Matches(const LogicalLoopCacheKey& key) const {
    return valid_ && key_ == key;
  }

  bool valid() const { return valid_; }
  const uint8_t* data() const { return pcm_.data(); }
  size_t byte_count() const { return byte_count_; }

 private:
  std::array<uint8_t, Capacity> pcm_ = {};
  LogicalLoopCacheKey key_;
  size_t byte_count_ = 0;
  bool valid_ = false;
};

}  // namespace rex::audio::xma
