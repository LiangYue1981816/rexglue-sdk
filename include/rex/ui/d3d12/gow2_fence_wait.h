#pragma once
#include <cstdint>
namespace rex::ui::d3d12::fence_wait {
enum class Status { kReady, kDeviceRemoved, kSignalFailed, kEventFailed, kWaitFailed, kIncomplete };
struct Result {
  Status status;
  uint64_t completed;
  uint32_t error = 0;
  explicit operator bool() const { return status == Status::kReady; }
};
// The removed-device sentinel must never become a reclamation watermark.
template <typename Read, typename Arm, typename Wait>
Result Await(uint64_t target, Read read, Arm arm, Wait wait) {
  uint64_t completed = read();
  if (completed == UINT64_MAX)
    return {Status::kDeviceRemoved, completed};
  if (completed < target) {
    const int32_t hr = int32_t(arm(target));
    if (hr < 0)
      return {Status::kEventFailed, completed, uint32_t(hr)};
    const uint32_t outcome = uint32_t(wait());
    if (outcome != 0)
      return {Status::kWaitFailed, completed, outcome};
    completed = read();
    if (completed == UINT64_MAX)
      return {Status::kDeviceRemoved, completed};
  }
  if (completed < target)
    return {Status::kIncomplete, completed};
  return {Status::kReady, completed};
}
template <typename Signal, typename Read, typename Arm, typename Wait>
Result SignalAndAwait(uint64_t target, Signal signal, Read read, Arm arm, Wait wait) {
  const int32_t hr = int32_t(signal(target));
  if (hr < 0)
    return {Status::kSignalFailed, 0, uint32_t(hr)};
  return Await(target, read, arm, wait);
}
}  // namespace rex::ui::d3d12::fence_wait
