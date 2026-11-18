#pragma once
// File format of the Android driver pipeline cache (<title>.vkpc beside the shader storage): a
// header naming the device and driver, then the VkPipelineCache data. Pure, for host unit tests.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace rex::graphics::vulkan::pipeline_cache_file {

struct DeviceIdentity {
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint32_t driver_version = 0;
  uint8_t pipeline_cache_uuid[16] = {};
};

inline constexpr uint32_t kMagic = 0x43505647;  // "GVPC"
inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kHeaderSize = 56;
inline constexpr size_t kVulkanHeaderSize = 32;
inline constexpr uint32_t kVulkanHeaderVersionOne = 1;

enum class LoadStatus {
  kOk,
  kEmpty,
  kTooSmall,
  kBadMagic,
  kBadVersion,
  kOtherDevice,
  kBadSize,
  kBadHash,
  kBadVulkanHeader,
};

struct Unwrapped {
  LoadStatus status = LoadStatus::kEmpty;
  size_t data_offset = 0;
  size_t data_size = 0;
};

inline const char* LoadStatusName(LoadStatus status) {
  switch (status) {
    case LoadStatus::kOk:
      return "ok";
    case LoadStatus::kEmpty:
      return "empty";
    case LoadStatus::kTooSmall:
      return "too small";
    case LoadStatus::kBadMagic:
      return "bad magic";
    case LoadStatus::kBadVersion:
      return "bad version";
    case LoadStatus::kOtherDevice:
      return "other device or driver";
    case LoadStatus::kBadSize:
      return "bad size";
    case LoadStatus::kBadHash:
      return "bad hash";
    case LoadStatus::kBadVulkanHeader:
      return "bad Vulkan header";
  }
  return "unknown";
}

template <typename T>
inline T ReadLittleEndian(const uint8_t* source) {
  uint8_t bytes[sizeof(T)];
  std::memcpy(bytes, source, sizeof(bytes));
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= T(bytes[i]) << (i * 8);
  }
  return value;
}

template <typename T>
inline void WriteLittleEndian(uint8_t* destination, T value) {
  uint8_t bytes[sizeof(T)];
  for (size_t i = 0; i < sizeof(T); ++i) {
    bytes[i] = uint8_t(value >> (i * 8));
  }
  std::memcpy(destination, bytes, sizeof(bytes));
}

inline uint64_t HashData(const uint8_t* data, size_t size) { return XXH3_64bits(data, size); }

inline std::vector<uint8_t> Wrap(const DeviceIdentity& device, const uint8_t* data, size_t size) {
  std::vector<uint8_t> file(kHeaderSize + size);
  WriteLittleEndian(file.data(), kMagic);
  WriteLittleEndian(file.data() + 4, kVersion);
  WriteLittleEndian(file.data() + 8, device.vendor_id);
  WriteLittleEndian(file.data() + 12, device.device_id);
  WriteLittleEndian(file.data() + 16, device.driver_version);
  WriteLittleEndian(file.data() + 20, uint32_t(0));
  std::memcpy(file.data() + 24, device.pipeline_cache_uuid, 16);
  WriteLittleEndian(file.data() + 40, uint64_t(size));
  WriteLittleEndian(file.data() + 48, HashData(data, size));
  if (size) {
    std::memcpy(file.data() + kHeaderSize, data, size);
  }
  return file;
}

inline bool MatchesDevice(const uint8_t* header, const DeviceIdentity& device) {
  return ReadLittleEndian<uint32_t>(header + 8) == device.vendor_id &&
         ReadLittleEndian<uint32_t>(header + 12) == device.device_id &&
         ReadLittleEndian<uint32_t>(header + 16) == device.driver_version &&
         std::memcmp(header + 24, device.pipeline_cache_uuid, 16) == 0;
}

inline bool HasValidVulkanHeader(const uint8_t* data, size_t size,
                                const DeviceIdentity& device) {
  if (size < kVulkanHeaderSize) {
    return false;
  }
  const uint32_t header_size = ReadLittleEndian<uint32_t>(data);
  return header_size >= kVulkanHeaderSize && header_size <= size &&
         ReadLittleEndian<uint32_t>(data + 4) == kVulkanHeaderVersionOne &&
         ReadLittleEndian<uint32_t>(data + 8) == device.vendor_id &&
         ReadLittleEndian<uint32_t>(data + 12) == device.device_id &&
         std::memcmp(data + 16, device.pipeline_cache_uuid, 16) == 0;
}

inline Unwrapped Unwrap(const uint8_t* file, size_t file_size, const DeviceIdentity& device) {
  if (!file_size) {
    return {LoadStatus::kEmpty};
  }
  if (file_size < kHeaderSize) {
    return {LoadStatus::kTooSmall};
  }
  if (ReadLittleEndian<uint32_t>(file) != kMagic) {
    return {LoadStatus::kBadMagic};
  }
  if (ReadLittleEndian<uint32_t>(file + 4) != kVersion) {
    return {LoadStatus::kBadVersion};
  }
  if (!MatchesDevice(file, device)) {
    return {LoadStatus::kOtherDevice};
  }
  const size_t size = file_size - kHeaderSize;
  if (ReadLittleEndian<uint64_t>(file + 40) != size) {
    return {LoadStatus::kBadSize};
  }
  const uint8_t* data = file + kHeaderSize;
  if (ReadLittleEndian<uint64_t>(file + 48) != HashData(data, size)) {
    return {LoadStatus::kBadHash};
  }
  if (!HasValidVulkanHeader(data, size, device)) {
    return {LoadStatus::kBadVulkanHeader};
  }
  return {LoadStatus::kOk, kHeaderSize, size};
}

}  // namespace rex::graphics::vulkan::pipeline_cache_file
