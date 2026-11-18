#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <rex/graphics/vulkan/pipeline_cache_file.h>

namespace pc = rex::graphics::vulkan::pipeline_cache_file;

namespace {

pc::DeviceIdentity TestDevice() {
  pc::DeviceIdentity device;
  device.vendor_id = 0x12345678;
  device.device_id = 0x90ABCDEF;
  device.driver_version = 0x11223344;
  for (size_t i = 0; i < 16; ++i) {
    device.pipeline_cache_uuid[i] = uint8_t(i + 1);
  }
  return device;
}

void SetWord(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = uint8_t(value >> (i * 8));
  }
}

std::vector<uint8_t> DriverData(const pc::DeviceIdentity& device) {
  std::vector<uint8_t> data(40, 0xA5);
  SetWord(data, 0, uint32_t(pc::kVulkanHeaderSize));
  SetWord(data, 4, pc::kVulkanHeaderVersionOne);
  SetWord(data, 8, device.vendor_id);
  SetWord(data, 12, device.device_id);
  std::memcpy(data.data() + 16, device.pipeline_cache_uuid, 16);
  return data;
}

void CheckStatus(const std::vector<uint8_t>& file, const pc::DeviceIdentity& device,
                 pc::LoadStatus expected) {
  const pc::Unwrapped result = pc::Unwrap(file.data(), file.size(), device);
  CHECK(result.status == expected);
  CHECK(result.data_offset == 0);
  CHECK(result.data_size == 0);
}

void CheckLittleEndian(const std::vector<uint8_t>& file, size_t offset, uint64_t value,
                       size_t size) {
  for (size_t i = 0; i < size; ++i) {
    CHECK(file[offset + i] == uint8_t(value >> (i * 8)));
  }
}

}  // namespace

TEST_CASE("pipeline cache files round trip driver data at the documented offsets",
          "[vulkan][pipeline-cache]") {
  const auto device = TestDevice();
  const auto data = DriverData(device);
  const auto file = pc::Wrap(device, data.data(), data.size());
  const auto result = pc::Unwrap(file.data(), file.size(), device);
  REQUIRE(result.status == pc::LoadStatus::kOk);
  CHECK(result.data_offset == pc::kHeaderSize);
  CHECK(result.data_size == data.size());
  CHECK(file.size() == pc::kHeaderSize + data.size());
  CHECK(std::memcmp(file.data() + result.data_offset, data.data(), data.size()) == 0);
  CheckLittleEndian(file, 0, pc::kMagic, 4);
  CheckLittleEndian(file, 4, pc::kVersion, 4);
  CheckLittleEndian(file, 8, device.vendor_id, 4);
  CheckLittleEndian(file, 12, device.device_id, 4);
  CheckLittleEndian(file, 16, device.driver_version, 4);
  CheckLittleEndian(file, 20, 0, 4);
  CHECK(std::memcmp(file.data() + 24, device.pipeline_cache_uuid, 16) == 0);
  CheckLittleEndian(file, 40, data.size(), 8);
  CheckLittleEndian(file, 48, XXH3_64bits(data.data(), data.size()), 8);
  CHECK(pc::HashData(data.data(), data.size()) == XXH3_64bits(data.data(), data.size()));
}

TEST_CASE("pipeline cache files reject missing or unsupported outer headers",
          "[vulkan][pipeline-cache]") {
  const auto device = TestDevice();
  const auto data = DriverData(device);
  auto file = pc::Wrap(device, data.data(), data.size());
  SECTION("empty file") {
    CheckStatus({}, device, pc::LoadStatus::kEmpty);
  }
  SECTION("short header") {
    CheckStatus(std::vector<uint8_t>(55), device, pc::LoadStatus::kTooSmall);
  }
  SECTION("wrong magic") {
    SetWord(file, 0, 0);
    CheckStatus(file, device, pc::LoadStatus::kBadMagic);
  }
  SECTION("unsupported version") {
    SetWord(file, 4, 2);
    CheckStatus(file, device, pc::LoadStatus::kBadVersion);
  }
}

TEST_CASE("pipeline cache files bind the driver data to the device and driver",
          "[vulkan][pipeline-cache]") {
  auto device = TestDevice();
  const auto data = DriverData(device);
  const auto file = pc::Wrap(device, data.data(), data.size());
  SECTION("other vendor") { ++device.vendor_id; }
  SECTION("other device") { ++device.device_id; }
  SECTION("other driver") { ++device.driver_version; }
  SECTION("other UUID") { ++device.pipeline_cache_uuid[7]; }
  CheckStatus(file, device, pc::LoadStatus::kOtherDevice);
}

TEST_CASE("pipeline cache files reject changed lengths and contents", "[vulkan][pipeline-cache]") {
  const auto device = TestDevice();
  const auto data = DriverData(device);
  auto file = pc::Wrap(device, data.data(), data.size());
  SECTION("appended byte") {
    file.push_back(0);
    CheckStatus(file, device, pc::LoadStatus::kBadSize);
  }
  SECTION("changed driver byte") {
    file.back() ^= 1;
    CheckStatus(file, device, pc::LoadStatus::kBadHash);
  }
}

TEST_CASE("pipeline cache files validate the wrapped Vulkan header", "[vulkan][pipeline-cache]") {
  const auto device = TestDevice();
  auto data = DriverData(device);
  SECTION("unsupported header version") { SetWord(data, 4, 2); }
  SECTION("other UUID") { data[23] ^= 1; }
  SECTION("short data") { data.resize(16); }
  SECTION("header larger than the data") { SetWord(data, 0, 64); }
  SECTION("header smaller than the Vulkan header") { SetWord(data, 0, 31); }
  SECTION("other vendor") { SetWord(data, 8, device.vendor_id + 1); }
  SECTION("other device") { SetWord(data, 12, device.device_id + 1); }
  const auto file = pc::Wrap(device, data.data(), data.size());
  CheckStatus(file, device, pc::LoadStatus::kBadVulkanHeader);
}

TEST_CASE("pipeline cache files report the first failed check", "[vulkan][pipeline-cache]") {
  const auto device = TestDevice();
  auto data = DriverData(device);
  SetWord(data, 4, 2);
  auto file = pc::Wrap(device, data.data(), data.size());
  file.back() ^= 1;
  CheckStatus(file, device, pc::LoadStatus::kBadHash);
  file.push_back(0);
  CheckStatus(file, device, pc::LoadStatus::kBadSize);
  file[24] ^= 1;
  CheckStatus(file, device, pc::LoadStatus::kOtherDevice);
  SetWord(file, 4, 2);
  CheckStatus(file, device, pc::LoadStatus::kBadVersion);
  SetWord(file, 0, 0);
  CheckStatus(file, device, pc::LoadStatus::kBadMagic);
  file.resize(55);
  CheckStatus(file, device, pc::LoadStatus::kTooSmall);
  file.clear();
  CheckStatus(file, device, pc::LoadStatus::kEmpty);
}

TEST_CASE("pipeline cache status names are distinct and non-empty", "[vulkan][pipeline-cache]") {
  const std::vector<pc::LoadStatus> statuses = {
      pc::LoadStatus::kOk,           pc::LoadStatus::kEmpty,   pc::LoadStatus::kTooSmall,
      pc::LoadStatus::kBadMagic,     pc::LoadStatus::kBadVersion,
      pc::LoadStatus::kOtherDevice,  pc::LoadStatus::kBadSize, pc::LoadStatus::kBadHash,
      pc::LoadStatus::kBadVulkanHeader};
  const std::vector<std::string_view> expected = {
      "ok",         "empty",    "too small", "bad magic", "bad version",
      "other device or driver", "bad size", "bad hash", "bad Vulkan header"};
  for (size_t i = 0; i < statuses.size(); ++i) {
    const std::string_view name = pc::LoadStatusName(statuses[i]);
    CHECK_FALSE(name.empty());
    CHECK(name == expected[i]);
    for (size_t j = 0; j < i; ++j) {
      CHECK(name != pc::LoadStatusName(statuses[j]));
    }
  }
}
