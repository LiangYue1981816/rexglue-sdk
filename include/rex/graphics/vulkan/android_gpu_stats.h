#pragma once
// Per-window counters of the Vulkan command processor's work on Android, logged as GPUSTATS lines
// (vulkan_android_stats_log). Only the command processor thread touches them.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
#define REX_ANDROID_GPU_STAT(statement) statement
#else
#define REX_ANDROID_GPU_STAT(statement)
#endif

namespace rex::graphics::vulkan {

struct AndroidGpuStats {
  int64_t window_start_us = 0;
  uint32_t swaps = 0;
  uint32_t swaps_skipped_pending = 0;
  uint32_t draws = 0;
  uint32_t draws_skipped_pending = 0;
  uint32_t draws_without_pixel_shader = 0;
  uint32_t draws_without_pixel_shader_color_bound = 0;
  // Draws without a pixel shader on the float24 depth buffer with no color target 0 bound: the
  // ones vulkan_android_depth_experiment bit 1024 cannot paint.
  uint32_t float24_depth_only_draws_without_color_0 = 0;
  uint32_t draws_geometry_shader = 0;
  uint32_t draws_depth_d24s8 = 0;
  uint32_t draws_depth_d24fs8 = 0;
  uint32_t draws_without_vertices = 0;
  uint32_t draws_user_clip_planes = 0;
  uint32_t color_draws_user_clip_planes = 0;
  uint32_t draws_vertex_kill = 0;
  uint32_t color_draws_vertex_kill = 0;
  uint32_t missing_texture_bindings = 0;
  // Texture descriptor sets (one per stage with textures or samplers) written, and kept because the
  // draw binds what the set written earlier in the frame holds (vulkan_reuse_texture_descriptors).
  uint32_t texture_sets_written = 0;
  uint32_t texture_sets_kept = 0;
  // Draws with a pixel shader and color writes, by the xenos::CompareFunction of their depth test;
  // the last slot counts those with the depth test off.
  std::array<uint32_t, 9> color_draws_by_depth_func{};
  uint32_t render_passes = 0;
  uint32_t submissions = 0;
  uint32_t queue_drains = 0;
  uint32_t occlusion_query_ends = 0;
  uint32_t occlusion_host_results = 0;
  uint32_t occlusion_zero_results = 0;
  uint64_t occlusion_samples = 0;
  uint32_t pipeline_setup_slow_calls = 0;
  int64_t pipeline_setup_us = 0;
  int64_t pipeline_setup_max_us = 0;
  // Why render passes ended: pending barriers, a forced end without barriers (uploads, copies),
  // or a switch to other render targets.
  uint32_t render_pass_ends_barrier = 0;
  uint32_t render_pass_ends_forced = 0;
  uint32_t render_pass_ends_switch = 0;
  uint32_t shared_memory_uploads = 0;
  uint64_t shared_memory_upload_bytes = 0;
  // Upload bytes by the draw step that requested them (the rest come from copies and resolves).
  uint64_t upload_bytes_index = 0;
  uint64_t upload_bytes_texture = 0;
  uint64_t upload_bytes_vertex = 0;
  uint64_t upload_bytes_memexport = 0;
  uint32_t memexport_draws = 0;
  uint32_t memexport_unknown_range_draws = 0;
  uint32_t cpu_invalidation_callbacks = 0;
  uint64_t cpu_invalidated_bytes = 0;
  uint64_t cpu_newly_invalidated_pages = 0;
  uint32_t texture_loads = 0;
  uint32_t texture_gpu_written = 0;
  uint32_t texture_zero_base_compressed = 0;
  uint32_t texture_zero_mips_compressed = 0;
  uint32_t texture_zero_other = 0;
  uint32_t texture_reloaded_after_zero = 0;
  uint32_t texture_invalidations = 0;
};

constexpr int64_t kAndroidGpuStatsWindowUs = 5'000'000;
constexpr int64_t kAndroidSlowPipelineSetupUs = 1'000;

inline int64_t AndroidGpuStatsNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void AddPipelineSetupTime(AndroidGpuStats& stats, int64_t start_us) {
  const int64_t elapsed_us = AndroidGpuStatsNowUs() - start_us;
  stats.pipeline_setup_us += elapsed_us;
  stats.pipeline_setup_max_us = std::max(stats.pipeline_setup_max_us, elapsed_us);
  if (elapsed_us >= kAndroidSlowPipelineSetupUs) {
    ++stats.pipeline_setup_slow_calls;
  }
}

}  // namespace rex::graphics::vulkan
