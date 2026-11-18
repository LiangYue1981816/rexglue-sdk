#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <rex/system/system_link.h>

namespace rex::kernel::xam {

constexpr uint32_t kXSessionCreateHost = 0x00000001;
constexpr uint32_t kXUserIndexNone = 0x000000FE;
constexpr uint32_t kXSessionLocalDetailsSize = 128;
constexpr uint32_t kXSessionMemberSize = 16;
constexpr uint32_t kXSessionMemberPrivateSlot = 0x00000001;
constexpr uint32_t kFirstXSessionHandle = 0xCAFE0004;
constexpr uint32_t kXSessionHandleStep = 4;
// Context ids the session details report as game type and game mode.
constexpr uint32_t kXContextGameType = 0x0000800A;
constexpr uint32_t kXContextGameMode = 0x0000800B;
// Upper bound on a property value copied out of guest memory.
constexpr uint32_t kMaxUserPropertySize = 0x1000;

// XUSER_DATA_TYPE values (XDK xuser.h). Only used to decode stored property bytes.
enum class XUserDataType : uint8_t {
  kNotSet = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kWString = 4,
  kFloat = 5,
  kBinary = 6,
  kDateTime = 7,
};

using TypedUserValue = std::variant<int32_t, int64_t, double, float>;

// Decodes big-endian guest bytes as the given type. Empty when the type has no numeric
// form or the byte count does not match it.
std::optional<TypedUserValue> DecodeTypedUserValue(XUserDataType type,
                                                   std::span<const uint8_t> bytes);

enum class XSessionState : uint32_t {
  kLobby = 0,
  kRegistration = 1,
  kInGame = 2,
  kReporting = 3,
  kDeleted = 4,
};

struct XSessionMember {
  uint64_t xuid = 0;
  uint32_t user_index = kXUserIndexNone;
  bool private_slot = false;
};

struct XSessionRecord {
  uint32_t handle = 0;
  uint32_t object = 0;
  uint32_t flags = 0;
  uint32_t creator_user_index = kXUserIndexNone;
  uint32_t host_user_index = kXUserIndexNone;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  XSessionState state = XSessionState::kLobby;
  uint64_t nonce = 0;
  std::array<uint8_t, system::system_link::kSessionInfoSize> info{};
  std::vector<XSessionMember> members;
  // Filled from the creator's contexts when details are read (ApplyCreatorContexts).
  uint32_t game_type = 0;
  uint32_t game_mode = 0;
};

void AddSessionMembers(XSessionRecord& session, std::span<const XSessionMember> joining);
void RemoveSessionMembersByXuid(XSessionRecord& session, std::span<const uint64_t> xuids);
void RemoveSessionMembersByUserIndex(XSessionRecord& session,
                                     std::span<const uint32_t> user_indices);
uint32_t CountSessionMembers(const XSessionRecord& session, bool private_slot);
uint32_t AvailableSessionSlots(uint32_t max_slots, uint32_t used_slots);
uint32_t WriteLocalDetails(const XSessionRecord& session, std::span<uint8_t> out,
                           uint32_t out_guest_address);
// Bytes WriteLocalDetails needs for every current member.
uint32_t RequiredLocalDetailsSize(const XSessionRecord& session);

// Contexts and properties the game sets per local user (XUserSetContextEx / XUserSetPropertyEx).
// Keyed by (user_index, id); process lifetime. Property values are kept as the raw guest bytes.
class XUserValueStore {
 public:
  void SetContext(uint32_t user_index, uint32_t id, uint32_t value);
  std::optional<uint32_t> GetContext(uint32_t user_index, uint32_t id) const;
  void SetProperty(uint32_t user_index, uint32_t id, std::vector<uint8_t> bytes);
  std::optional<std::vector<uint8_t>> GetProperty(uint32_t user_index, uint32_t id) const;

 private:
  using Key = std::pair<uint32_t, uint32_t>;
  mutable std::mutex mutex_;
  std::map<Key, uint32_t> contexts_;
  std::map<Key, std::vector<uint8_t>> properties_;
};

XUserValueStore& UserValueStore();

// Copies the creator's game type and game mode contexts onto the session (unset ones stay 0).
void ApplyCreatorContexts(XSessionRecord& session, const XUserValueStore& store);

class XSessionRegistry {
 public:
  uint32_t Create(uint32_t object);
  uint32_t ObjectForHandle(uint32_t handle) const;
  bool IsObject(uint32_t object) const;
  bool Update(uint32_t object, const std::function<void(XSessionRecord&)>& edit);
  std::optional<XSessionRecord> Snapshot(uint32_t object) const;

 private:
  XSessionRecord* FindByObject(uint32_t object);
  const XSessionRecord* FindByObject(uint32_t object) const;

  mutable std::mutex mutex_;
  uint32_t next_handle_ = kFirstXSessionHandle;
  std::vector<XSessionRecord> sessions_;
};

XSessionRegistry& SessionRegistry();

}  // namespace rex::kernel::xam
