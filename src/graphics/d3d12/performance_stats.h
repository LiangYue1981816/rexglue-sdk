#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, d3d12_stats_log);

namespace rex::graphics::d3d12::stats {

enum class Counter : size_t {
  kFrames,
  kDraws,
  kDrawsPipelinePending,
  kShadersSynchronous,
  kShadersDeferred,
  kPipelines,
  kGuestUploads,
  kGuestUploadBytes,
  kMemexportFast,
  kMemexportFull,
  kMemexportWaitUs,
  kTextureLoads,
  kTextureAllocations,
  kQueueWaits,
  kQueueWaitUs,
  kCount,
};

inline std::array<std::atomic<uint64_t>, size_t(Counter::kCount)> counters{};

inline void Record(Counter counter, uint64_t amount = 1) {
  if (REXCVAR_GET(d3d12_stats_log)) {
    counters[size_t(counter)].fetch_add(amount, std::memory_order_relaxed);
  }
}

template <typename Operation>
auto MeasureWait(Counter counter, Operation&& operation) {
  if (!REXCVAR_GET(d3d12_stats_log)) {
    return std::forward<Operation>(operation)();
  }
  const auto start = std::chrono::steady_clock::now();
  auto result = std::forward<Operation>(operation)();
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  counters[size_t(counter)].fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  if (counter == Counter::kQueueWaitUs) {
    counters[size_t(Counter::kQueueWaits)].fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

using Snapshot = std::array<uint64_t, size_t(Counter::kCount)>;

inline Snapshot TakeSnapshot() {
  Snapshot values;
  for (size_t i = 0; i < values.size(); ++i) {
    values[i] = counters[i].exchange(0, std::memory_order_relaxed);
  }
  return values;
}

inline void LogSnapshot(const Snapshot& values, double seconds) {
  const auto value = [&values](Counter counter) { return values[size_t(counter)]; };
  REXGPU_INFO(
      "D3D12STATS seconds={:.3f} frames={} draws={} draws_pipeline_pending={} "
      "shaders_sync={} shaders_deferred={} pipelines={} guest_uploads={} guest_upload_bytes={} "
      "memexport_fast={} memexport_full={} memexport_wait_us={} texture_loads={} "
      "texture_allocations={} queue_waits={} queue_wait_us={}",
      seconds, value(Counter::kFrames), value(Counter::kDraws),
      value(Counter::kDrawsPipelinePending), value(Counter::kShadersSynchronous),
      value(Counter::kShadersDeferred), value(Counter::kPipelines), value(Counter::kGuestUploads),
      value(Counter::kGuestUploadBytes), value(Counter::kMemexportFast),
      value(Counter::kMemexportFull), value(Counter::kMemexportWaitUs),
      value(Counter::kTextureLoads), value(Counter::kTextureAllocations),
      value(Counter::kQueueWaits), value(Counter::kQueueWaitUs));
}

inline void OnSwap() {
  enum class WindowState { kInitial, kActive, kDisabled };
  static WindowState state = WindowState::kInitial;
  static std::chrono::steady_clock::time_point window_start;
  if (!REXCVAR_GET(d3d12_stats_log)) {
    if (state == WindowState::kActive) {
      TakeSnapshot();
    }
    state = WindowState::kDisabled;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (state != WindowState::kActive) {
    window_start = now;
    state = WindowState::kActive;
  }
  counters[size_t(Counter::kFrames)].fetch_add(1, std::memory_order_relaxed);
  if (now - window_start < std::chrono::seconds(5)) {
    return;
  }
  LogSnapshot(TakeSnapshot(), std::chrono::duration<double>(now - window_start).count());
  window_start = now;
}

}  // namespace rex::graphics::d3d12::stats
