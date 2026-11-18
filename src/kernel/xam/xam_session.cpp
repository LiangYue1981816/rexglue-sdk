#include <rex/kernel/xam/xsession.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace rex::kernel::xam {
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

void StoreBigEndian32(std::span<uint8_t> out, size_t offset, uint32_t value) {
  out[offset + 0] = static_cast<uint8_t>(value >> 24);
  out[offset + 1] = static_cast<uint8_t>(value >> 16);
  out[offset + 2] = static_cast<uint8_t>(value >> 8);
  out[offset + 3] = static_cast<uint8_t>(value);
}

void StoreBigEndian64(std::span<uint8_t> out, size_t offset, uint64_t value) {
  out[offset + 0] = static_cast<uint8_t>(value >> 56);
  out[offset + 1] = static_cast<uint8_t>(value >> 48);
  out[offset + 2] = static_cast<uint8_t>(value >> 40);
  out[offset + 3] = static_cast<uint8_t>(value >> 32);
  out[offset + 4] = static_cast<uint8_t>(value >> 24);
  out[offset + 5] = static_cast<uint8_t>(value >> 16);
  out[offset + 6] = static_cast<uint8_t>(value >> 8);
  out[offset + 7] = static_cast<uint8_t>(value);
}

uint64_t ReadBigEndianBits(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (uint8_t byte : bytes) {
    value = (value << 8) | byte;
  }
  return value;
}

bool IsDuplicateMember(const XSessionMember& existing, const XSessionMember& joining) {
  if (joining.xuid != 0) {
    return existing.xuid == joining.xuid;
  }
  if (joining.user_index == kXUserIndexNone) {
    return false;
  }
  return existing.xuid == 0 && existing.user_index == joining.user_index;
}

bool ContainsXuid(std::span<const uint64_t> xuids, uint64_t xuid) {
  return std::find(xuids.begin(), xuids.end(), xuid) != xuids.end();
}

bool ContainsUserIndex(std::span<const uint32_t> indices, uint32_t user_index) {
  return std::find(indices.begin(), indices.end(), user_index) != indices.end();
}

}  // namespace

void AddSessionMembers(XSessionRecord& session, std::span<const XSessionMember> joining) {
  for (const XSessionMember& member : joining) {
    auto is_same = [&member](const XSessionMember& existing) {
      return IsDuplicateMember(existing, member);
    };
    if (std::none_of(session.members.begin(), session.members.end(), is_same)) {
      session.members.push_back(member);
    }
  }
}

void RemoveSessionMembersByXuid(XSessionRecord& session, std::span<const uint64_t> xuids) {
  auto matches = [&xuids](const XSessionMember& member) {
    return member.xuid != 0 && ContainsXuid(xuids, member.xuid);
  };
  session.members.erase(std::remove_if(session.members.begin(), session.members.end(), matches),
                        session.members.end());
}

void RemoveSessionMembersByUserIndex(XSessionRecord& session,
                                     std::span<const uint32_t> user_indices) {
  auto matches = [&user_indices](const XSessionMember& member) {
    return member.user_index != kXUserIndexNone &&
           ContainsUserIndex(user_indices, member.user_index);
  };
  session.members.erase(std::remove_if(session.members.begin(), session.members.end(), matches),
                        session.members.end());
}

uint32_t CountSessionMembers(const XSessionRecord& session, bool private_slot) {
  uint32_t count = 0;
  for (const XSessionMember& member : session.members) {
    if (member.private_slot == private_slot) {
      ++count;
    }
  }
  return count;
}

uint32_t AvailableSessionSlots(uint32_t max_slots, uint32_t used_slots) {
  return max_slots > used_slots ? max_slots - used_slots : 0;
}

uint32_t RequiredLocalDetailsSize(const XSessionRecord& session) {
  return kXSessionLocalDetailsSize +
         static_cast<uint32_t>(session.members.size()) * kXSessionMemberSize;
}

uint32_t WriteLocalDetails(const XSessionRecord& session, std::span<uint8_t> out,
                           uint32_t out_guest_address) {
  if (out.size() < kXSessionLocalDetailsSize) {
    return 0;
  }
  std::memset(out.data(), 0, kXSessionLocalDetailsSize);
  const size_t capacity = (out.size() - kXSessionLocalDetailsSize) / kXSessionMemberSize;
  const size_t returned = std::min(session.members.size(), capacity);
  StoreBigEndian32(out, kDetailsHostUserIndexOffset, session.host_user_index);
  StoreBigEndian32(out, kDetailsGameTypeOffset, session.game_type);
  StoreBigEndian32(out, kDetailsGameModeOffset, session.game_mode);
  StoreBigEndian32(out, kDetailsFlagsOffset, session.flags);
  StoreBigEndian32(out, kDetailsMaxPublicOffset, session.max_public);
  StoreBigEndian32(out, kDetailsMaxPrivateOffset, session.max_private);
  StoreBigEndian32(out, kDetailsAvailablePublicOffset,
                   AvailableSessionSlots(session.max_public, CountSessionMembers(session, false)));
  StoreBigEndian32(out, kDetailsAvailablePrivateOffset,
                   AvailableSessionSlots(session.max_private, CountSessionMembers(session, true)));
  StoreBigEndian32(out, kDetailsActualCountOffset, static_cast<uint32_t>(session.members.size()));
  StoreBigEndian32(out, kDetailsReturnedCountOffset, static_cast<uint32_t>(returned));
  StoreBigEndian32(out, kDetailsStateOffset, static_cast<uint32_t>(session.state));
  StoreBigEndian32(out, kDetailsPaddingOffset, 0);
  StoreBigEndian64(out, kDetailsNonceOffset, session.nonce);
  std::memcpy(out.data() + kDetailsInfoOffset, session.info.data(), session.info.size());
  std::memset(out.data() + kDetailsArbitrationOffset, 0, kDetailsArbitrationSize);
  if (returned == 0) {
    StoreBigEndian32(out, kDetailsMembersPointerOffset, 0);
    return 0;
  }
  StoreBigEndian32(out, kDetailsMembersPointerOffset,
                   out_guest_address + kXSessionLocalDetailsSize);
  for (size_t i = 0; i < returned; ++i) {
    const XSessionMember& member = session.members[i];
    const size_t base = kXSessionLocalDetailsSize + i * kXSessionMemberSize;
    StoreBigEndian64(out, base + kMemberXuidOffset, member.xuid);
    StoreBigEndian32(out, base + kMemberUserIndexOffset, member.user_index);
    StoreBigEndian32(out, base + kMemberFlagsOffset,
                     member.private_slot ? kXSessionMemberPrivateSlot : 0);
  }
  return static_cast<uint32_t>(returned);
}

uint32_t XSessionRegistry::Create(uint32_t object) {
  std::lock_guard<std::mutex> lock(mutex_);
  XSessionRecord record;
  record.handle = next_handle_;
  record.object = object;
  sessions_.push_back(std::move(record));
  const uint32_t handle = next_handle_;
  next_handle_ += kXSessionHandleStep;
  return handle;
}

uint32_t XSessionRegistry::ObjectForHandle(uint32_t handle) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const XSessionRecord& session : sessions_) {
    if (session.handle == handle) {
      return session.object;
    }
  }
  return 0;
}

bool XSessionRegistry::IsObject(uint32_t object) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (object == 0) {
    return false;
  }
  return FindByObject(object) != nullptr;
}

bool XSessionRegistry::Update(uint32_t object, const std::function<void(XSessionRecord&)>& edit) {
  std::lock_guard<std::mutex> lock(mutex_);
  XSessionRecord* record = FindByObject(object);
  if (record == nullptr) {
    return false;
  }
  edit(*record);
  return true;
}

std::optional<XSessionRecord> XSessionRegistry::Snapshot(uint32_t object) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const XSessionRecord* record = FindByObject(object);
  if (record == nullptr) {
    return std::nullopt;
  }
  return *record;
}

XSessionRecord* XSessionRegistry::FindByObject(uint32_t object) {
  for (XSessionRecord& session : sessions_) {
    if (session.object == object) {
      return &session;
    }
  }
  return nullptr;
}

const XSessionRecord* XSessionRegistry::FindByObject(uint32_t object) const {
  for (const XSessionRecord& session : sessions_) {
    if (session.object == object) {
      return &session;
    }
  }
  return nullptr;
}

XSessionRegistry& SessionRegistry() {
  static XSessionRegistry instance;
  return instance;
}

std::optional<TypedUserValue> DecodeTypedUserValue(XUserDataType type,
                                                   std::span<const uint8_t> bytes) {
  switch (type) {
    case XUserDataType::kInt32:
      if (bytes.size() != sizeof(uint32_t)) {
        return std::nullopt;
      }
      return static_cast<int32_t>(static_cast<uint32_t>(ReadBigEndianBits(bytes)));
    case XUserDataType::kInt64:
      if (bytes.size() != sizeof(uint64_t)) {
        return std::nullopt;
      }
      return static_cast<int64_t>(ReadBigEndianBits(bytes));
    case XUserDataType::kFloat: {
      if (bytes.size() != sizeof(float)) {
        return std::nullopt;
      }
      const auto bits = static_cast<uint32_t>(ReadBigEndianBits(bytes));
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
    case XUserDataType::kDouble: {
      if (bytes.size() != sizeof(double)) {
        return std::nullopt;
      }
      const uint64_t bits = ReadBigEndianBits(bytes);
      double value;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
    default:
      return std::nullopt;
  }
}

void XUserValueStore::SetContext(uint32_t user_index, uint32_t id, uint32_t value) {
  std::lock_guard<std::mutex> lock(mutex_);
  contexts_[{user_index, id}] = value;
}

std::optional<uint32_t> XUserValueStore::GetContext(uint32_t user_index, uint32_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = contexts_.find({user_index, id});
  if (it == contexts_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void XUserValueStore::SetProperty(uint32_t user_index, uint32_t id, std::vector<uint8_t> bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  properties_[{user_index, id}] = std::move(bytes);
}

std::optional<std::vector<uint8_t>> XUserValueStore::GetProperty(uint32_t user_index,
                                                                 uint32_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = properties_.find({user_index, id});
  if (it == properties_.end()) {
    return std::nullopt;
  }
  return it->second;
}

XUserValueStore& UserValueStore() {
  static XUserValueStore instance;
  return instance;
}

void ApplyCreatorContexts(XSessionRecord& session, const XUserValueStore& store) {
  if (session.creator_user_index == kXUserIndexNone) {
    return;
  }
  session.game_type =
      store.GetContext(session.creator_user_index, kXContextGameType).value_or(0);
  session.game_mode =
      store.GetContext(session.creator_user_index, kXContextGameMode).value_or(0);
}

}  // namespace rex::kernel::xam
