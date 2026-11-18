/**
 * Internal to the Vulkan backend (not under include/: the game exe is built against those
 * headers, whose class layouts are frozen).
 *
 * Hooks for the vulkan_android_pass_timing diagnostic (PASSTIME lines, command_processor.cpp)
 * that the other Vulkan backend files call.
 */

#pragma once

#include <cstdint>

namespace rex::graphics::vulkan {

// What pushed a pending barrier. Must match kBarrierSourceNames in command_processor.cpp.
enum class BarrierSource : uint8_t {
  kUpload,
  kIndex,
  kTexture,
  kRenderTarget,
  kResolve,
  kOther,

  kCount,
};

// Defined in command_processor.cpp. Consumed (and reset to kOther) by the next
// VulkanCommandProcessor::PushBufferMemoryBarrier / PushImageMemoryBarrier.
extern BarrierSource pass_timing_barrier_source;

// Tags the barrier pushed right after this call. Only a store, so it's free when
// vulkan_android_pass_timing is off.
inline void SetBarrierSource(BarrierSource source) { pass_timing_barrier_source = source; }

// What recorded a compute dispatch. Must match the PASSTIME gaps/frame dispatch_by_source fields in
// command_processor.cpp.
enum class DispatchSource : uint8_t {
  kSwap,
  kResolve,
  kTextureLoad,
  kRtTransfer,
  kRtClear,
  kDepthStore,
  kNativeResolve,
  kOther,

  kCount,
};

// Defined in command_processor.cpp. Consumed (and reset to kOther) by the next
// PassTimingDispatchRecorded.
extern DispatchSource pass_timing_dispatch_source;

// Tags the dispatch recorded right after this call. Only a store, so it's free when
// vulkan_android_pass_timing is off.
inline void SetDispatchSource(DispatchSource source) { pass_timing_dispatch_source = source; }

// Defined in command_processor.cpp. Called right after each CmdVkDispatch: counts the dispatch
// for pass_timing_dispatch_source if the open submission is timed, and resets the source to
// kOther.
void PassTimingDispatchRecorded();

// Commands executed from a DeferredCommandBuffer that Vulkan only allows outside a render pass
// (so they are what fills the gaps between passes).
struct DeferredGapCounts {
  uint64_t copies = 0;           // vkCmdCopyBuffer.
  uint64_t image_copies = 0;     // vkCmdCopyBufferToImage, vkCmdCopyImage.
  uint64_t clears = 0;           // vkCmdClearColorImage.
  uint64_t dispatches = 0;       // vkCmdDispatch.
  uint64_t barrier_batches = 0;  // vkCmdPipelineBarrier.
};

// Defined in deferred_command_buffer.cpp. While counts is not null, DeferredCommandBuffer::Execute
// adds the commands it executes to it.
void SetDeferredGapCounts(DeferredGapCounts* counts);

}  // namespace rex::graphics::vulkan
