/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <cstring>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

REXCVAR_DEFINE_BOOL(
    xma_loop_diagnostics, false, "Audio/XMA",
    "Log low-volume generic XMA loop scheduler transitions");

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

namespace rex::audio {

using stream::BitStream;

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

bool XmaContext::Work() {
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (!current_frame_window_pending_) {
      return true;
    }
    Consume(&output_rb, &data);
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  if (remaining_subframe_blocks_in_output_buffer_ <=
      static_cast<int32_t>(data.output_buffer_padding)) {
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >
         static_cast<int32_t>(data.output_buffer_padding)) {
    const uint32_t previous_read_offset = data.input_buffer_read_offset;
    const uint8_t previous_current_buffer = data.current_buffer;
    const bool previous_buffer_0_valid = data.input_buffer_0_valid != 0;
    const bool previous_buffer_1_valid = data.input_buffer_1_valid != 0;
    const bool previous_window_pending = current_frame_window_pending_;
    const uint32_t previous_output_byte = current_frame_output_byte_;
    const uint32_t previous_partial_output_bytes =
        partial_output_block_bytes_;

    Decode(&data);
    const bool consumed = Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      break;
    }

    const bool decoder_progressed =
        consumed ||
        previous_read_offset != data.input_buffer_read_offset ||
        previous_current_buffer != data.current_buffer ||
        previous_buffer_0_valid != (data.input_buffer_0_valid != 0) ||
        previous_buffer_1_valid != (data.input_buffer_1_valid != 0) ||
        previous_window_pending != current_frame_window_pending_ ||
        previous_output_byte != current_frame_output_byte_ ||
        previous_partial_output_bytes != partial_output_block_bytes_;
    if (!decoder_progressed) {
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  if (output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetStreamState();
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  set_is_allocated(false);
  ResetStreamState();
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::ResetStreamState() {
  // Setup allocates the codec context before the title creates or initializes
  // any XMA stream. Clear may therefore arrive while FFmpeg has a codec
  // pointer but no AVCodecInternal yet; avcodec_flush_buffers requires an
  // opened context.
  const bool codec_open = av_context_ && avcodec_is_open(av_context_);
  if (codec_open) {
    avcodec_flush_buffers(av_context_);
  }
  current_frame_window_ = {};
  current_frame_output_byte_ = 0;
  current_frame_output_end_byte_ = 0;
  current_frame_window_pending_ = false;
  current_frame_physical_eof_ = false;
  logical_frame_.fill(0);
  partial_output_block_.fill(0);
  partial_output_block_bytes_ = 0;
  pending_frame_advance_ = {};
  logical_loop_cache_.Reset();
  logical_loop_cache_frame_advance_ = {};
  decoded_stream_state_.Reset();
  loop_scheduler_.Reset();
  loop_input_buffer_address_ = 0;
  loop_diagnostic_event_count_ = 0;
  decoder_needs_flush_ = false;
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

uint32_t XmaContext::GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count) {
  if (next_packet_index < current_input_packet_count) {
    return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()) +
           next_packet_index * kBytesPerPacket;
  }

  const uint8_t next_buffer_index = data->current_buffer ^ 1;
  if (!data->IsInputBufferValid(next_buffer_index)) {
    return nullptr;
  }

  const uint32_t next_buffer_address = data->GetInputBufferAddress(next_buffer_index);
  if (!next_buffer_address) {
    REXAPU_ERROR("XmaContext {}: Buffer marked valid but has null pointer!", id());
    return nullptr;
  }

  return memory()->TranslatePhysical(next_buffer_address);
}

uint32_t XmaContext::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count) {
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeinBits) {
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  return kBitsPerPacketHeader;
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

void XmaContext::WriteOutputPcm(memory::RingBuffer* output_rb,
                                const uint8_t* pcm, size_t byte_count) {
  assert_zero(byte_count % kOutputBytesPerBlock);
  output_rb->Write(pcm, byte_count);
}

bool XmaContext::Consume(memory::RingBuffer* output_rb, XMA_CONTEXT_DATA* data) {
  if (!current_frame_window_pending_) {
    return false;
  }

  const uint32_t effective_sdc =
      std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  const int32_t available_blocks =
      std::max(remaining_subframe_blocks_in_output_buffer_, 0);
  int32_t block_budget =
      std::min(static_cast<int32_t>(effective_sdc), available_blocks);

  const uint32_t remaining_frame_bytes =
      current_frame_output_end_byte_ - current_frame_output_byte_;
  const uint32_t pending_bytes =
      partial_output_block_bytes_ + remaining_frame_bytes;
  uint32_t blocks_needed_to_finish =
      pending_bytes / kOutputBytesPerBlock;
  if (current_frame_physical_eof_ &&
      pending_bytes % kOutputBytesPerBlock != 0) {
    ++blocks_needed_to_finish;
  }
  if (blocks_needed_to_finish <= static_cast<uint32_t>(block_budget) &&
      available_blocks <
          static_cast<int32_t>(blocks_needed_to_finish +
                               data->output_buffer_padding)) {
    block_budget =
        std::max(available_blocks -
                     static_cast<int32_t>(data->output_buffer_padding),
                 0);
  }

  bool progressed = false;
  const auto write_blocks = [&](const uint8_t* pcm, uint32_t block_count) {
    WriteOutputPcm(output_rb, pcm,
                   static_cast<size_t>(block_count) *
                       kOutputBytesPerBlock);
    remaining_subframe_blocks_in_output_buffer_ -= block_count;
    block_budget -= block_count;
  };

  if (partial_output_block_bytes_ == kOutputBytesPerBlock) {
    if (block_budget == 0) {
      return false;
    }
    write_blocks(partial_output_block_.data(), 1);
    partial_output_block_bytes_ = 0;
    progressed = true;
  }

  while (current_frame_output_byte_ < current_frame_output_end_byte_) {
    const uint32_t frame_bytes_left =
        current_frame_output_end_byte_ - current_frame_output_byte_;

    if (partial_output_block_bytes_ != 0) {
      const uint32_t copy_count =
          std::min(kOutputBytesPerBlock - partial_output_block_bytes_,
                   frame_bytes_left);
      std::memcpy(
          partial_output_block_.data() + partial_output_block_bytes_,
          logical_frame_.data() + current_frame_output_byte_, copy_count);
      partial_output_block_bytes_ += copy_count;
      current_frame_output_byte_ += copy_count;
      progressed = true;

      if (partial_output_block_bytes_ == kOutputBytesPerBlock) {
        if (block_budget == 0) {
          return true;
        }
        write_blocks(partial_output_block_.data(), 1);
        partial_output_block_bytes_ = 0;
      }
      continue;
    }

    const uint32_t full_blocks = frame_bytes_left / kOutputBytesPerBlock;
    if (full_blocks != 0 && block_budget != 0) {
      const uint32_t blocks_to_write =
          std::min(full_blocks, static_cast<uint32_t>(block_budget));
      write_blocks(logical_frame_.data() + current_frame_output_byte_,
                   blocks_to_write);
      current_frame_output_byte_ +=
          blocks_to_write * kOutputBytesPerBlock;
      progressed = true;
      continue;
    }

    if (frame_bytes_left < kOutputBytesPerBlock) {
      std::memcpy(partial_output_block_.data(),
                  logical_frame_.data() + current_frame_output_byte_,
                  frame_bytes_left);
      partial_output_block_bytes_ = frame_bytes_left;
      current_frame_output_byte_ = current_frame_output_end_byte_;
      progressed = true;
    }
    break;
  }

  if (current_frame_output_byte_ == current_frame_output_end_byte_) {
    if (current_frame_physical_eof_ &&
        partial_output_block_bytes_ != 0) {
      if (partial_output_block_bytes_ < kOutputBytesPerBlock) {
        std::fill(partial_output_block_.begin() +
                      partial_output_block_bytes_,
                  partial_output_block_.end(), 0);
        partial_output_block_bytes_ = kOutputBytesPerBlock;
        progressed = true;
      }
      if (block_budget == 0) {
        return progressed;
      }
      write_blocks(partial_output_block_.data(), 1);
      partial_output_block_bytes_ = 0;
    }

    remaining_subframe_blocks_in_output_buffer_ -=
        data->output_buffer_padding;
    CompleteFrameWindow(data);
    progressed = true;
  }
  return progressed;
}

void XmaContext::AdvanceInputAfterFrame(XMA_CONTEXT_DATA* data) {
  const PendingFrameAdvance advance = pending_frame_advance_;
  pending_frame_advance_ = {};
  if (!advance.valid) {
    return;
  }

  if (!advance.swap_input_buffer) {
    data->input_buffer_read_offset = advance.next_input_offset;
    return;
  }

  SwapInputBuffer(data);
  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  const uint8_t* next_input_buffer =
      memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
  const uint32_t next_input_offset =
      xma::GetPacketFrameOffset(next_input_buffer);
  if (next_input_offset > kMaxFrameSizeinBits) {
    SwapInputBuffer(data);
    return;
  }
  data->input_buffer_read_offset = next_input_offset;
}

void XmaContext::CompleteFrameWindow(XMA_CONTEXT_DATA* data) {
  const xma::LoopFrameConfig loop_config = {
      .loop_start_frame =
          std::max(kBitsPerPacketHeader, data->loop_start),
      .loop_end_frame = std::max(kBitsPerPacketHeader, data->loop_end),
      .loop_subframe_start =
          static_cast<uint8_t>(data->loop_subframe_start),
      .loop_subframe_end =
          static_cast<uint8_t>(data->loop_subframe_end),
      .loop_subframe_skip =
          static_cast<uint8_t>(data->loop_subframe_skip),
      .loop_count = static_cast<uint8_t>(data->loop_count),
  };
  const xma::LoopFrameCompletion completion =
      loop_scheduler_.CompleteFrame(loop_config);

  current_frame_window_ = {};
  current_frame_output_byte_ = 0;
  current_frame_output_end_byte_ = 0;
  current_frame_window_pending_ = false;
  current_frame_physical_eof_ = false;

  if (completion.decrement_loop_count && data->loop_count > 0 &&
      data->loop_count < xma::kInfiniteLoopCount) {
    --data->loop_count;
  }

  if (completion.rewind_to_loop_start) {
    data->input_buffer_read_offset = loop_config.loop_start_frame;
    pending_frame_advance_ = {};
  } else {
    AdvanceInputAfterFrame(data);
  }

  if (REXCVAR_GET(xma_loop_diagnostics) &&
      (completion.rewind_to_loop_start ||
       completion.decrement_loop_count)) {
    const uint32_t event_number = ++loop_diagnostic_event_count_;
    if (event_number <= 8 ||
        (event_number & (event_number - 1)) == 0) {
      REXAPU_INFO(
          "XmaContext {} loop event {}: rewind={} decrement={} "
          "iteration_active={} loop_count={} read_offset={}",
          id(), event_number, completion.rewind_to_loop_start,
          completion.decrement_loop_count,
          loop_scheduler_.loop_iteration_active(),
          static_cast<uint32_t>(data->loop_count),
          static_cast<uint32_t>(data->input_buffer_read_offset));
    }
  }
}

int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    decoder_needs_flush_ = false;
    return 1;
  }
  if (decoder_needs_flush_) {
    avcodec_flush_buffers(av_context_);
    decoder_needs_flush_ = false;
  }
  return 0;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

bool XmaContext::DecodeFinalOverlap(AVCodecContext* av_context,
                                    AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, nullptr);
  if (ret < 0) {
    return false;
  }
  decoder_needs_flush_ = true;

  ret = avcodec_receive_frame(av_context, av_frame);
  if (ret < 0) {
    return false;
  }
  return av_frame->nb_samples >=
         static_cast<int>(kSamplesPerSubframe);
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_window_pending_) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  const uint32_t current_input_buffer_address =
      data->GetCurrentInputBufferAddress();
  if (decoded_stream_state_.physical_eof_seen() &&
      !loop_scheduler_.loop_iteration_active()) {
    // A new valid input after a completed non-looping stream is a fresh
    // activation even if the guest reuses the same XMA context.
    decoded_stream_state_.Reset();
  }
  if (loop_input_buffer_address_ != 0 &&
      loop_input_buffer_address_ != current_input_buffer_address &&
      loop_scheduler_.loop_start_saved()) {
    // A loop rewind is relative to its original input block. Replacing that
    // block starts a new stream and makes its intro eligible again.
    loop_scheduler_.Reset();
    logical_loop_cache_.Reset();
    logical_loop_cache_frame_advance_ = {};
    loop_input_buffer_address_ = 0;
    loop_diagnostic_event_count_ = 0;
  }

  const xma::LoopFrameConfig loop_config = {
      .loop_start_frame =
          std::max(kBitsPerPacketHeader, data->loop_start),
      .loop_end_frame =
          std::max(kBitsPerPacketHeader, data->loop_end),
      .loop_subframe_start =
          static_cast<uint8_t>(data->loop_subframe_start),
      .loop_subframe_end =
          static_cast<uint8_t>(data->loop_subframe_end),
      .loop_subframe_skip =
          static_cast<uint8_t>(data->loop_subframe_skip),
      .loop_count = static_cast<uint8_t>(data->loop_count),
  };
  const bool is_same_frame_loop =
      data->loop_count != 0 &&
      loop_config.loop_start_frame == data->input_buffer_read_offset &&
      loop_config.loop_end_frame == data->input_buffer_read_offset;
  if (is_same_frame_loop && loop_scheduler_.loop_iteration_active() &&
      logical_loop_cache_.valid()) {
    const uint8_t repeated_begin_subframe =
        std::min(loop_config.loop_subframe_skip,
                 xma::kSubframesPerFrame);
    const uint8_t repeated_end_subframe =
        std::max<uint8_t>(
            repeated_begin_subframe,
            loop_config.loop_subframe_end == 0
                ? xma::kSubframesPerFrame
                : std::min(loop_config.loop_subframe_end,
                           xma::kSubframesPerFrame));
    const xma::LogicalLoopCacheKey cache_key = {
        .input_buffer_address = current_input_buffer_address,
        .frame_offset = data->input_buffer_read_offset,
        .begin_subframe = repeated_begin_subframe,
        .end_subframe = repeated_end_subframe,
        .sample_rate = static_cast<uint8_t>(data->sample_rate),
        .is_stereo = bool(data->is_stereo),
    };
    if (logical_loop_cache_.Matches(cache_key)) {
      const xma::LoopFrameSchedule schedule =
          loop_scheduler_.ScheduleFrame(data->input_buffer_read_offset,
                                        loop_config);
      logical_frame_.fill(0);
      std::memcpy(logical_frame_.data(), logical_loop_cache_.data(),
                  logical_loop_cache_.byte_count());
      current_frame_window_ = schedule.window;
      current_frame_output_byte_ = 0;
      current_frame_output_end_byte_ =
          static_cast<uint32_t>(logical_loop_cache_.byte_count());
      current_frame_window_pending_ = true;
      // Preserve partial output between cached traversals, but pad the final
      // partial block when a finite loop reaches its physical stream end.
      current_frame_physical_eof_ =
          data->loop_count == 1;
      pending_frame_advance_ = logical_loop_cache_frame_advance_;
      return;
    }

    logical_loop_cache_.Reset();
    logical_loop_cache_frame_advance_ = {};
  }
  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXAPU_ERROR("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
                 static_cast<uint32_t>(data->input_buffer_read_offset));
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    uint32_t next_input_offset =
        GetNextPacketReadOffset(current_input_buffer, packet_index + 1, current_input_packet_count);
    if (next_input_offset == kBitsPerPacketHeader) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
  }

  BitStream stream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(static_cast<uint32_t>(stream.BitsRemaining()),
                                                      packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    REXAPU_ERROR("XmaContext {}: There are no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
    }
  }

  PendingFrameAdvance frame_advance = {};
  frame_advance.valid = true;
  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    frame_advance.next_input_offset =
        (packet_index * kBitsPerPacket) + next_frame_offset;
  } else {
    frame_advance.next_input_offset =
        GetNextPacketReadOffset(current_input_buffer, next_packet_index,
                                current_input_packet_count);
    frame_advance.swap_input_buffer =
        frame_advance.next_input_offset == kBitsPerPacketHeader;
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  PreparePacket(packet_info.current_frame_size_, padding_start);
  if (DecodePacket(av_context_, av_packet_, av_frame_)) {
    const int decoded_frame_samples = av_frame_->nb_samples;
    const xma::LoopFrameSchedule schedule =
        loop_scheduler_.ScheduleFrame(data->input_buffer_read_offset,
                                      loop_config);

    uint32_t start_skip_samples = 0;
    uint32_t end_skip_samples = 0;
    if (const AVFrameSideData* skip_data =
            av_frame_get_side_data(av_frame_, AV_FRAME_DATA_SKIP_SAMPLES);
        skip_data && skip_data->size >= 8) {
      const auto read_le_u32 = [](const uint8_t* bytes) {
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8) |
               (static_cast<uint32_t>(bytes[2]) << 16) |
               (static_cast<uint32_t>(bytes[3]) << 24);
      };
      start_skip_samples = read_le_u32(skip_data->data);
      end_skip_samples = read_le_u32(skip_data->data + 4);
    }

    ConvertSamples(
        reinterpret_cast<const uint8_t**>(&av_frame_->data),
        bool(data->is_stereo), decoded_frame_samples, raw_frame_.data());

    const uint8_t next_buffer_index = data->current_buffer ^ 1;
    const bool available_input_ends =
        frame_advance.swap_input_buffer &&
        !data->IsInputBufferValid(next_buffer_index);
    // An empty second input buffer can also mean temporary starvation for a
    // streamed source. Treat it as physical EOF only when frame metadata or
    // same-frame loop policy proves that the decoder tail is required.
    const bool physical_eof =
        available_input_ends &&
        (end_skip_samples != 0 || is_same_frame_loop ||
         start_skip_samples >=
             static_cast<uint32_t>(decoded_frame_samples));
    bool final_overlap_decoded = false;
    uint32_t final_overlap_samples = 0;
    if (physical_eof && DecodeFinalOverlap(av_context_, av_frame_)) {
      final_overlap_decoded = true;
      final_overlap_samples =
          static_cast<uint32_t>(av_frame_->nb_samples);
      ConvertSamples(
          reinterpret_cast<const uint8_t**>(&av_frame_->data),
          bool(data->is_stereo), final_overlap_samples,
          raw_frame_.data() +
              (static_cast<size_t>(decoded_frame_samples) *
               kBytesPerSample << data->is_stereo));
    }

    xma::DecodedSampleWindow requested_window = {
        .begin_sample =
            static_cast<uint32_t>(schedule.window.begin_subframe) *
            kSamplesPerSubframe,
        .end_sample =
            static_cast<uint32_t>(schedule.window.end_subframe) *
            kSamplesPerSubframe,
    };
    if (final_overlap_decoded) {
      if (is_same_frame_loop) {
        // Encoded loop subframes precede their decoded IMDCT overlap by one
        // overlap window. Shift both boundaries into the drained PCM.
        requested_window.begin_sample += final_overlap_samples;
        requested_window.end_sample += final_overlap_samples;
      } else if (schedule.window.end_subframe ==
                 xma::kSubframesPerFrame) {
        // A non-looping full-frame request includes the final physical
        // overlap before end padding is removed.
        requested_window.end_sample += final_overlap_samples;
      }
    }
    xma::DecodedStreamState loop_cache_stream_state =
        decoded_stream_state_;
    const xma::DecodedSampleSelection selected_samples =
        decoded_stream_state_.SelectFrameWindows(
            requested_window,
            static_cast<uint32_t>(decoded_frame_samples),
            final_overlap_samples, start_skip_samples, end_skip_samples,
            physical_eof);
    const uint32_t bytes_per_sample_frame =
        kBytesPerSample << data->is_stereo;
    logical_frame_.fill(0);
    size_t logical_byte_count = 0;
    for (uint8_t window_index = 0;
         window_index < selected_samples.window_count; ++window_index) {
      const xma::DecodedSampleWindow& window =
          selected_samples.windows[window_index];
      const size_t source_byte =
          static_cast<size_t>(window.begin_sample) *
          bytes_per_sample_frame;
      const size_t window_byte_count =
          static_cast<size_t>(window.sample_count()) *
          bytes_per_sample_frame;
      std::memcpy(logical_frame_.data() + logical_byte_count,
                  raw_frame_.data() + source_byte, window_byte_count);
      logical_byte_count += window_byte_count;
    }

    if (is_same_frame_loop && final_overlap_decoded) {
      const uint8_t repeated_begin_subframe =
          std::min(loop_config.loop_subframe_skip,
                   xma::kSubframesPerFrame);
      const uint8_t repeated_end_subframe =
          std::max<uint8_t>(
              repeated_begin_subframe,
              loop_config.loop_subframe_end == 0
                  ? xma::kSubframesPerFrame
                  : std::min(loop_config.loop_subframe_end,
                             xma::kSubframesPerFrame));
      const xma::DecodedSampleWindow repeated_requested_window = {
          .begin_sample =
              static_cast<uint32_t>(repeated_begin_subframe) *
                  kSamplesPerSubframe +
              final_overlap_samples,
          .end_sample =
              static_cast<uint32_t>(repeated_end_subframe) *
                  kSamplesPerSubframe +
              final_overlap_samples,
      };
      const xma::DecodedSampleSelection repeated_selection =
          loop_cache_stream_state.SelectFrameWindows(
              repeated_requested_window,
              static_cast<uint32_t>(decoded_frame_samples),
              final_overlap_samples, start_skip_samples,
              end_skip_samples, physical_eof);
      std::array<uint8_t,
                 kBytesPerFrameWithFinalOverlapChannel * 2>
          repeated_pcm = {};
      size_t repeated_byte_count = 0;
      for (uint8_t window_index = 0;
           window_index < repeated_selection.window_count;
           ++window_index) {
        const xma::DecodedSampleWindow& window =
            repeated_selection.windows[window_index];
        const size_t source_byte =
            static_cast<size_t>(window.begin_sample) *
            bytes_per_sample_frame;
        const size_t window_byte_count =
            static_cast<size_t>(window.sample_count()) *
            bytes_per_sample_frame;
        std::memcpy(repeated_pcm.data() + repeated_byte_count,
                    raw_frame_.data() + source_byte,
                    window_byte_count);
        repeated_byte_count += window_byte_count;
      }

      const xma::LogicalLoopCacheKey cache_key = {
          .input_buffer_address = current_input_buffer_address,
          .frame_offset = data->input_buffer_read_offset,
          .begin_subframe = repeated_begin_subframe,
          .end_subframe = repeated_end_subframe,
          .sample_rate = static_cast<uint8_t>(data->sample_rate),
          .is_stereo = bool(data->is_stereo),
      };
      logical_loop_cache_frame_advance_ = {};
      if (logical_loop_cache_.Store(
              cache_key, repeated_pcm.data(), repeated_byte_count)) {
        logical_loop_cache_frame_advance_ = frame_advance;
      }
    }

    if (data->loop_count != 0 &&
        data->input_buffer_read_offset == loop_config.loop_start_frame) {
      loop_input_buffer_address_ = current_input_buffer_address;
    }
    current_frame_window_ = schedule.window;
    current_frame_output_byte_ = 0;
    current_frame_output_end_byte_ =
        static_cast<uint32_t>(logical_byte_count);
    current_frame_window_pending_ = true;
    current_frame_physical_eof_ = physical_eof;
    pending_frame_advance_ = frame_advance;
  } else {
    // FFmpeg may consume a physical frame without returning PCM while priming
    // decoder state. Advance normally, but do not create a logical PCM window.
    pending_frame_advance_ = frame_advance;
    AdvanceInputAfterFrame(data);
  }
}

void XmaContext::ConvertSamples(const uint8_t** samples, bool is_two_channel,
                                uint32_t sample_count,
                                uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  assert_zero(sample_count % 8);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < sample_count; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < sample_count; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < sample_count; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
