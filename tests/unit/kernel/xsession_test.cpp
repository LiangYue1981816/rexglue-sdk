#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include <rex/kernel/xam/xsession.h>

using namespace rex::kernel::xam;

namespace {

constexpr size_t kDetailsHostUserIndexOffset = 0;
constexpr size_t kDetailsGameTypeOffset = 4;
constexpr size_t kDetailsGameModeOffset = 8;
constexpr size_t kDetailsFlagsOffset = 12;
constexpr size_t kDetailsMaxPublicOffset = 16;
constexpr size_t kDetailsMaxPrivateOffset = 20;
constexpr size_t kDetailsAvailablePublicOffset = 24;
constexpr size_t kDetailsAvailablePrivateOffset = 28;
constexpr size_t kDetailsActualCountOffset = 32;
constexpr size_t kDetailsReturnedCountOffset = 36;
constexpr size_t kDetailsStateOffset = 40;
constexpr size_t kDetailsPaddingOffset = 44;
constexpr size_t kDetailsNonceOffset = 48;
constexpr size_t kDetailsInfoOffset = 56;
constexpr size_t kDetailsArbitrationOffset = 116;
constexpr size_t kDetailsMembersPointerOffset = 124;
constexpr size_t kDetailsArbitrationSize = 8;
constexpr size_t kMemberXuidOffset = 0;
constexpr size_t kMemberUserIndexOffset = 8;
constexpr size_t kMemberFlagsOffset = 12;
constexpr uint32_t kGuestBaseAddress = 0x40001000;
constexpr uint8_t kFillByte = 0xEE;

uint32_t ReadBigEndian32(const std::vector<uint8_t>& buffer, size_t offset) {
  return (static_cast<uint32_t>(buffer[offset]) << 24) |
         (static_cast<uint32_t>(buffer[offset + 1]) << 16) |
         (static_cast<uint32_t>(buffer[offset + 2]) << 8) |
         static_cast<uint32_t>(buffer[offset + 3]);
}

uint64_t ReadBigEndian64(const std::vector<uint8_t>& buffer, size_t offset) {
  return (static_cast<uint64_t>(buffer[offset]) << 56) |
         (static_cast<uint64_t>(buffer[offset + 1]) << 48) |
         (static_cast<uint64_t>(buffer[offset + 2]) << 40) |
         (static_cast<uint64_t>(buffer[offset + 3]) << 32) |
         (static_cast<uint64_t>(buffer[offset + 4]) << 24) |
         (static_cast<uint64_t>(buffer[offset + 5]) << 16) |
         (static_cast<uint64_t>(buffer[offset + 6]) << 8) |
         static_cast<uint64_t>(buffer[offset + 7]);
}

XSessionRecord MakeDetailsSession() {
  XSessionRecord session;
  session.flags = 0x21;
  session.host_user_index = 0;
  session.max_public = 4;
  session.max_private = 0;
  session.state = XSessionState::kInGame;
  session.nonce = 0x0102030405060708ULL;
  for (size_t i = 0; i < session.info.size(); ++i) {
    session.info[i] = static_cast<uint8_t>(i);
  }
  session.members = {{0x1111, kXUserIndexNone, false}, {0x2222, kXUserIndexNone, true}};
  return session;
}

}  // namespace

TEST_CASE("xsession: AddSessionMembers skips a repeated XUID", "[kernel][xsession]") {
  XSessionRecord session;
  const std::array<XSessionMember, 2> joining = {
      {{0x1111, kXUserIndexNone, false}, {0x2222, kXUserIndexNone, false}}};
  AddSessionMembers(session, joining);
  REQUIRE(session.members.size() == 2);
  const std::array<XSessionMember, 1> again = {{{0x2222, kXUserIndexNone, false}}};
  AddSessionMembers(session, again);
  CHECK(session.members.size() == 2);
  CHECK(session.members[0].xuid == 0x1111);
  CHECK(session.members[1].xuid == 0x2222);
}

TEST_CASE("xsession: AddSessionMembers dedupes local in one call", "[kernel][xsession]") {
  XSessionRecord session;
  const std::array<XSessionMember, 2> joining = {{{0, 0, false}, {0, 0, false}}};
  AddSessionMembers(session, joining);
  CHECK(session.members.size() == 1);
  CHECK(session.members[0].xuid == 0);
  CHECK(session.members[0].user_index == 0);
}

TEST_CASE("xsession: AddSessionMembers keeps locals without index", "[kernel][xsession]") {
  XSessionRecord session;
  const std::array<XSessionMember, 2> joining = {
      {{0, kXUserIndexNone, false}, {0, kXUserIndexNone, false}}};
  AddSessionMembers(session, joining);
  CHECK(session.members.size() == 2);
}

TEST_CASE("xsession: RemoveSessionMembersByXuid keeps order", "[kernel][xsession]") {
  XSessionRecord session;
  session.members = {{0x1111, kXUserIndexNone, false},
                     {0x2222, kXUserIndexNone, false},
                     {0x3333, kXUserIndexNone, false},
                     {0, 0, false}};
  const std::array<uint64_t, 1> xuids = {0x2222};
  RemoveSessionMembersByXuid(session, xuids);
  REQUIRE(session.members.size() == 3);
  CHECK(session.members[0].xuid == 0x1111);
  CHECK(session.members[1].xuid == 0x3333);
  CHECK(session.members[2].xuid == 0);
  const std::array<uint64_t, 1> zero = {0};
  RemoveSessionMembersByXuid(session, zero);
  CHECK(session.members.size() == 3);
}

TEST_CASE("xsession: RemoveSessionMembersByUserIndex keeps order", "[kernel][xsession]") {
  XSessionRecord session;
  session.members = {{0, 0, false}, {0, 1, false}, {0, 2, false}, {0x1111, kXUserIndexNone, false}};
  const std::array<uint32_t, 1> indices = {1};
  RemoveSessionMembersByUserIndex(session, indices);
  REQUIRE(session.members.size() == 3);
  CHECK(session.members[0].user_index == 0);
  CHECK(session.members[1].user_index == 2);
  CHECK(session.members[2].xuid == 0x1111);
  const std::array<uint32_t, 1> none = {kXUserIndexNone};
  RemoveSessionMembersByUserIndex(session, none);
  CHECK(session.members.size() == 3);
}

TEST_CASE("xsession: CountSessionMembers counts by slot", "[kernel][xsession]") {
  XSessionRecord session;
  session.members = {{0x1111, kXUserIndexNone, false},
                     {0x2222, kXUserIndexNone, true},
                     {0x3333, kXUserIndexNone, true}};
  CHECK(CountSessionMembers(session, false) == 1);
  CHECK(CountSessionMembers(session, true) == 2);
}

TEST_CASE("xsession: AvailableSessionSlots clamps at zero", "[kernel][xsession]") {
  CHECK(AvailableSessionSlots(4, 1) == 3);
  CHECK(AvailableSessionSlots(4, 4) == 0);
  CHECK(AvailableSessionSlots(2, 5) == 0);
}

TEST_CASE("xsession: WriteLocalDetails writes fields big-endian", "[kernel][xsession]") {
  const XSessionRecord session = MakeDetailsSession();
  std::vector<uint8_t> buffer(640, kFillByte);
  const uint32_t returned = WriteLocalDetails(session, buffer, kGuestBaseAddress);
  CHECK(returned == 2);
  CHECK(ReadBigEndian32(buffer, kDetailsHostUserIndexOffset) == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsGameTypeOffset) == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsGameModeOffset) == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsFlagsOffset) == 0x21);
  CHECK(ReadBigEndian32(buffer, kDetailsMaxPublicOffset) == 4);
  CHECK(ReadBigEndian32(buffer, kDetailsMaxPrivateOffset) == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsAvailablePublicOffset) == 3);
  CHECK(ReadBigEndian32(buffer, kDetailsAvailablePrivateOffset) == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsActualCountOffset) == 2);
  CHECK(ReadBigEndian32(buffer, kDetailsReturnedCountOffset) == 2);
  CHECK(ReadBigEndian32(buffer, kDetailsStateOffset) == 2);
  CHECK(ReadBigEndian32(buffer, kDetailsPaddingOffset) == 0);
  CHECK(ReadBigEndian64(buffer, kDetailsNonceOffset) == 0x0102030405060708ULL);
  for (size_t i = 0; i < session.info.size(); ++i) {
    CHECK(buffer[kDetailsInfoOffset + i] == static_cast<uint8_t>(i));
  }
  for (size_t i = 0; i < kDetailsArbitrationSize; ++i) {
    CHECK(buffer[kDetailsArbitrationOffset + i] == 0);
  }
  CHECK(ReadBigEndian32(buffer, kDetailsMembersPointerOffset) == 0x40001080);
  const size_t first = kXSessionLocalDetailsSize;
  CHECK(ReadBigEndian64(buffer, first + kMemberXuidOffset) == 0x1111);
  CHECK(ReadBigEndian32(buffer, first + kMemberUserIndexOffset) == kXUserIndexNone);
  CHECK(ReadBigEndian32(buffer, first + kMemberFlagsOffset) == 0);
  const size_t second = kXSessionLocalDetailsSize + kXSessionMemberSize;
  CHECK(ReadBigEndian64(buffer, second + kMemberXuidOffset) == 0x2222);
  CHECK(ReadBigEndian32(buffer, second + kMemberUserIndexOffset) == kXUserIndexNone);
  CHECK(ReadBigEndian32(buffer, second + kMemberFlagsOffset) == kXSessionMemberPrivateSlot);
  const size_t tail = kXSessionLocalDetailsSize + 2 * kXSessionMemberSize;
  for (size_t i = tail; i < buffer.size(); ++i) {
    CHECK(buffer[i] == kFillByte);
  }
}

TEST_CASE("xsession: WriteLocalDetails with header-only buffer", "[kernel][xsession]") {
  const XSessionRecord session = MakeDetailsSession();
  std::vector<uint8_t> buffer(kXSessionLocalDetailsSize, kFillByte);
  const uint32_t returned = WriteLocalDetails(session, buffer, kGuestBaseAddress);
  CHECK(returned == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsActualCountOffset) == 2);
  CHECK(ReadBigEndian32(buffer, kDetailsReturnedCountOffset) == 0);
  CHECK(ReadBigEndian32(buffer, kDetailsMembersPointerOffset) == 0);
}

TEST_CASE("xsession: WriteLocalDetails with small buffer writes nothing", "[kernel][xsession]") {
  const XSessionRecord session = MakeDetailsSession();
  std::vector<uint8_t> buffer(100, kFillByte);
  const uint32_t returned = WriteLocalDetails(session, buffer, kGuestBaseAddress);
  CHECK(returned == 0);
  for (size_t i = 0; i < buffer.size(); ++i) {
    CHECK(buffer[i] == kFillByte);
  }
}

TEST_CASE("xsession: XSessionRegistry assigns handles and snapshots", "[kernel][xsession]") {
  XSessionRegistry registry;
  const uint32_t first = registry.Create(0x1000);
  const uint32_t second = registry.Create(0x2000);
  CHECK(first == 0xCAFE0004);
  CHECK(second == 0xCAFE0008);
  CHECK(registry.ObjectForHandle(first) == 0x1000);
  CHECK(registry.ObjectForHandle(second) == 0x2000);
  CHECK(registry.ObjectForHandle(0xDEAD) == 0);
  CHECK_FALSE(registry.IsObject(0));
  CHECK(registry.IsObject(0x1000));
  CHECK_FALSE(registry.Update(0x9999, [](XSessionRecord& record) { record.flags = 1; }));
  CHECK(registry.Update(0x1000, [](XSessionRecord& record) { record.flags = 0x21; }));
  const std::optional<XSessionRecord> snapshot = registry.Snapshot(0x1000);
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->flags == 0x21);
  CHECK(snapshot->handle == first);
  CHECK_FALSE(registry.Snapshot(0x9999).has_value());
}

TEST_CASE("xsession: XUserValueStore stores contexts per user", "[kernel][xsession]") {
  XUserValueStore store;
  CHECK_FALSE(store.GetContext(0, kXContextGameType).has_value());
  store.SetContext(0, kXContextGameType, 0x21);
  const std::optional<uint32_t> stored = store.GetContext(0, kXContextGameType);
  REQUIRE(stored.has_value());
  CHECK(*stored == 0x21);
  CHECK_FALSE(store.GetContext(1, kXContextGameType).has_value());
  store.SetContext(0, kXContextGameType, 0x22);
  CHECK(*store.GetContext(0, kXContextGameType) == 0x22);
}

TEST_CASE("xsession: XUserValueStore keeps property bytes per user", "[kernel][xsession]") {
  constexpr uint32_t kPropertyId = 0x40000001;
  XUserValueStore store;
  CHECK_FALSE(store.GetProperty(0, kPropertyId).has_value());
  const std::vector<uint8_t> bytes = {0x00, 0x00, 0x01, 0x02};
  store.SetProperty(0, kPropertyId, bytes);
  const std::optional<std::vector<uint8_t>> stored = store.GetProperty(0, kPropertyId);
  REQUIRE(stored.has_value());
  CHECK(*stored == bytes);
  CHECK_FALSE(store.GetProperty(1, kPropertyId).has_value());
}

TEST_CASE("xsession: DecodeTypedUserValue reads big-endian by type", "[kernel][xsession]") {
  const std::vector<uint8_t> int32_bytes = {0xFF, 0xFF, 0xFF, 0xFE};
  const auto as_int32 = DecodeTypedUserValue(XUserDataType::kInt32, int32_bytes);
  REQUIRE(as_int32.has_value());
  CHECK(std::get<int32_t>(*as_int32) == -2);

  const std::vector<uint8_t> int64_bytes = {0, 0, 0, 0, 0, 0, 0, 0x2A};
  const auto as_int64 = DecodeTypedUserValue(XUserDataType::kInt64, int64_bytes);
  REQUIRE(as_int64.has_value());
  CHECK(std::get<int64_t>(*as_int64) == 42);

  const std::vector<uint8_t> float_bytes = {0x3F, 0x80, 0x00, 0x00};
  const auto as_float = DecodeTypedUserValue(XUserDataType::kFloat, float_bytes);
  REQUIRE(as_float.has_value());
  CHECK(std::get<float>(*as_float) == 1.0f);

  const std::vector<uint8_t> double_bytes = {0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2D, 0x18};
  const auto as_double = DecodeTypedUserValue(XUserDataType::kDouble, double_bytes);
  REQUIRE(as_double.has_value());
  CHECK(std::get<double>(*as_double) == 3.141592653589793);

  CHECK_FALSE(DecodeTypedUserValue(XUserDataType::kInt32, int64_bytes).has_value());
  CHECK_FALSE(DecodeTypedUserValue(XUserDataType::kWString, int32_bytes).has_value());
}

TEST_CASE("xsession: creator game type and mode reach details", "[kernel][xsession]") {
  XUserValueStore store;
  store.SetContext(0, kXContextGameType, 0x21);
  store.SetContext(0, kXContextGameMode, 0x03);
  store.SetContext(1, kXContextGameMode, 0x07);
  XSessionRecord session = MakeDetailsSession();
  session.creator_user_index = 0;
  ApplyCreatorContexts(session, store);
  std::vector<uint8_t> buffer(640, kFillByte);
  WriteLocalDetails(session, buffer, kGuestBaseAddress);
  CHECK(ReadBigEndian32(buffer, kDetailsGameTypeOffset) == 0x21);
  CHECK(ReadBigEndian32(buffer, kDetailsGameModeOffset) == 0x03);
}

TEST_CASE("xsession: unset creator contexts leave game type and mode at zero",
          "[kernel][xsession]") {
  XUserValueStore store;
  store.SetContext(1, kXContextGameType, 0x21);
  XSessionRecord session = MakeDetailsSession();
  session.creator_user_index = 0;
  ApplyCreatorContexts(session, store);
  CHECK(session.game_type == 0);
  CHECK(session.game_mode == 0);
}

TEST_CASE("xsession: RequiredLocalDetailsSize covers every member", "[kernel][xsession]") {
  const XSessionRecord session = MakeDetailsSession();
  const uint32_t required = RequiredLocalDetailsSize(session);
  CHECK(required == kXSessionLocalDetailsSize + 2 * kXSessionMemberSize);
  std::vector<uint8_t> exact(required, kFillByte);
  CHECK(WriteLocalDetails(session, exact, kGuestBaseAddress) == 2);
  std::vector<uint8_t> one_short(required - 1, kFillByte);
  CHECK(WriteLocalDetails(session, one_short, kGuestBaseAddress) == 1);
}
