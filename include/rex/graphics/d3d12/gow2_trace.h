// Adapted from gears-judgement-recomp (BSD-3-Clause).
#pragma once
// Project-local frame trace shared by the D3D12 backend.
//   Create a file named "gow2_trace.now" in the working directory while the game runs: the next
//   few frames log every resolve (RESOLVE) and swap (SWAP) with their parameters, then it stops.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace rex::graphics::d3d12 {
inline std::atomic<uint32_t> gow2_trace_frame{0};
inline std::atomic<int> gow2_trace_remaining{0};

// Called once per swap. Returns true if this frame is being traced.
inline bool Gow2TraceBeginFrame() {
  uint32_t f = gow2_trace_frame.fetch_add(1);
  if (gow2_trace_remaining.load() <= 0 && (f % 30) == 0) {
    std::error_code ec;
    if (std::filesystem::exists("gow2_trace.now", ec)) {
      std::filesystem::remove("gow2_trace.now", ec);
      gow2_trace_remaining.store(3);
    }
  }
  return gow2_trace_remaining.load() > 0;
}
inline bool Gow2TraceActive() { return gow2_trace_remaining.load() > 0; }
inline void Gow2TraceEndFrame() {
  if (gow2_trace_remaining.load() > 0) gow2_trace_remaining.fetch_sub(1);
}
}  // namespace rex::graphics::d3d12
