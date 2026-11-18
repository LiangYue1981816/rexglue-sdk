/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/xsession.h>
#include <rex/logging.h>
#include <rex/system/system_link.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

REXCVAR_DECLARE(bool, system_link_trace);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

// M0 diagnostics for System Link (gated by system_link_trace, default off).
// Rate limiter: log the first 50 calls per call site, then every 500th.
static bool NetLinkTraceAllow(std::atomic<uint64_t>* counter) {
  uint64_t n = counter->fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= 50 || (n % 500) == 0;
}

namespace {

namespace system_link = rex::system::system_link;
using SessionInfoBytes = std::array<uint8_t, system_link::kSessionInfoSize>;

constexpr uint32_t kMaxSessionUsersPerCall = 64;
constexpr uint32_t kXuidSize = 8;
constexpr uint32_t kDwordSize = 4;

uint32_t LoadGuestU32(memory::Memory* guest_memory, uint32_t address) {
  return memory::load_and_swap<uint32_t>(guest_memory->TranslateVirtual(address));
}

uint64_t LoadGuestU64(memory::Memory* guest_memory, uint32_t address) {
  return memory::load_and_swap<uint64_t>(guest_memory->TranslateVirtual(address));
}

SessionInfoBytes MakeHostSessionInfo(uint64_t host_xuid) {
  SessionInfoBytes info{};
  auto xnkid = std::span(info).first<system_link::kXnKidSize>();
  system_link::FillRandom(xnkid);
  system_link::MarkSystemLinkXnKid(xnkid);
  auto xnaddr = system_link::TitleXnAddr(host_xuid);
  std::copy(xnaddr.begin(), xnaddr.end(), info.begin() + system_link::kXnKidSize);
  system_link::FillRandom(std::span(info).last<system_link::kXnKeySize>());
  return info;
}

SessionInfoBytes ReadSessionInfo(memory::Memory* guest_memory, uint32_t info_ptr) {
  SessionInfoBytes info{};
  if (info_ptr) {
    std::memcpy(info.data(), guest_memory->TranslateVirtual(info_ptr), info.size());
  }
  return info;
}

void WriteHostSessionInfo(memory::Memory* guest_memory, uint32_t info_ptr, uint32_t nonce_ptr,
                          const SessionInfoBytes& info, uint64_t nonce) {
  if (info_ptr) {
    std::memcpy(guest_memory->TranslateVirtual(info_ptr), info.data(), info.size());
  }
  if (nonce_ptr) {
    memory::store_and_swap<uint64_t>(guest_memory->TranslateVirtual(nonce_ptr), nonce);
  }
}

// First eight bytes of a value as a big-endian number, for the XGI trace lines.
uint64_t LeadingBytes(std::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (uint8_t byte : bytes.first(std::min<size_t>(bytes.size(), 8))) {
    value = (value << 8) | byte;
  }
  return value;
}

// XUSER_STATS_SPEC is dwViewId, dwNumColumnIds and 64 column ids (WORD).
constexpr uint32_t kStatsSpecSize = 136;
// XUSER_STATS_READ_RESULTS {dwNumViews, pViews} and XUSER_STATS_VIEW {dwViewId,
// dwTotalViewRows, dwNumRows, pRows}.
constexpr uint32_t kStatsResultsHeaderSize = 8;
constexpr uint32_t kStatsViewSize = 16;

// No stats on System Link: one empty view per requested spec. Titles read pViews[i] for every
// spec they asked for, so null views crash them (Gears of War 2 Horde reading high scores).
void WriteEmptyStatsResults(memory::Memory* guest_memory, uint32_t specs_count, uint32_t specs_ptr,
                            uint32_t results_size, uint32_t results_ptr) {
  if (!results_ptr || results_size < kStatsResultsHeaderSize) {
    return;
  }
  const uint32_t views =
      std::min(specs_count, (results_size - kStatsResultsHeaderSize) / kStatsViewSize);
  const uint32_t views_ptr = results_ptr + kStatsResultsHeaderSize;
  uint8_t* results = guest_memory->TranslateVirtual(results_ptr);
  memory::store_and_swap<uint32_t>(results + 0, views);
  memory::store_and_swap<uint32_t>(results + 4, views ? views_ptr : 0);
  for (uint32_t i = 0; i < views; ++i) {
    const uint32_t view_id =
        specs_ptr ? LoadGuestU32(guest_memory, specs_ptr + i * kStatsSpecSize) : 0;
    uint8_t* view = guest_memory->TranslateVirtual(views_ptr + i * kStatsViewSize);
    memory::store_and_swap<uint32_t>(view + 0, view_id);
    memory::store_and_swap<uint32_t>(view + 4, 0);
    memory::store_and_swap<uint32_t>(view + 8, 0);
    memory::store_and_swap<uint32_t>(view + 12, 0);
  }
}

uint64_t LocalUserXuid(KernelState* kernel_state, uint32_t user_index) {
  const auto* user_profile = kernel_state->user_profile(user_index);
  return user_profile ? user_profile->network_xuid() : 0;
}

std::vector<XSessionMember> ReadSessionUsers(memory::Memory* guest_memory,
                                             KernelState* kernel_state, uint32_t count,
                                             uint32_t xuid_array, uint32_t index_array,
                                             uint32_t private_array) {
  std::vector<XSessionMember> users;
  for (uint32_t i = 0; i < std::min(count, kMaxSessionUsersPerCall); ++i) {
    XSessionMember user;
    if (xuid_array) {
      user.xuid = LoadGuestU64(guest_memory, xuid_array + i * kXuidSize);
    } else if (index_array) {
      user.user_index = LoadGuestU32(guest_memory, index_array + i * kDwordSize);
      user.xuid = LocalUserXuid(kernel_state, user.user_index);
    }
    if (private_array) {
      user.private_slot = LoadGuestU32(guest_memory, private_array + i * kDwordSize) != 0;
    }
    users.push_back(user);
  }
  return users;
}

std::vector<uint64_t> XuidsOf(const std::vector<XSessionMember>& users) {
  std::vector<uint64_t> xuids;
  for (const auto& user : users) {
    xuids.push_back(user.xuid);
  }
  return xuids;
}

std::vector<uint32_t> UserIndicesOf(const std::vector<XSessionMember>& users) {
  std::vector<uint32_t> indices;
  for (const auto& user : users) {
    indices.push_back(user.user_index);
  }
  return indices;
}

void SetSessionState(uint32_t object, XSessionState state) {
  SessionRegistry().Update(object, [state](XSessionRecord& session) { session.state = state; });
}

X_HRESULT CreateSession(memory::Memory* guest_memory, KernelState* kernel_state,
                        const uint8_t* buffer, uint32_t buffer_length) {
  uint32_t object = memory::load_and_swap<uint32_t>(buffer + 0);
  uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
  uint32_t max_public = memory::load_and_swap<uint32_t>(buffer + 8);
  uint32_t max_private = memory::load_and_swap<uint32_t>(buffer + 12);
  uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 16);
  uint32_t info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
  uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
  bool host = (flags & kXSessionCreateHost) != 0;

  // A joiner passes the host's XSESSION_INFO from the search result in this buffer.
  SessionInfoBytes info = ReadSessionInfo(guest_memory, info_ptr);
  uint64_t nonce = 0;
  if (host) {
    info = MakeHostSessionInfo(kernel_state->user_profile()->network_xuid());
    nonce = system_link::RandomNonZero64();
    WriteHostSessionInfo(guest_memory, info_ptr, nonce_ptr, info, nonce);
  }
  bool known = SessionRegistry().Update(object, [&](XSessionRecord& session) {
    session.flags = flags;
    session.creator_user_index = user_index;
    session.host_user_index = host ? user_index : kXUserIndexNone;
    session.max_public = max_public;
    session.max_private = max_private;
    session.state = XSessionState::kLobby;
    session.nonce = nonce;
    session.info = info;
    session.members.clear();
  });

  REXKRNL_DEBUG("XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {:08X})", object,
                flags, max_public, max_private, user_index, info_ptr, nonce_ptr);
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    uint32_t host_ip = memory::load_and_swap<uint32_t>(info.data() + system_link::kXnKidSize);
    REXLOG_INFO(
        "[netlink] XGISessionCreate buf_len={} session={:#010x} flags={:#010x} slots_pub={} "
        "slots_priv={} user={:#010x} info_ptr={:#010x} nonce_ptr={:#010x} host={} known={} "
        "xnkid={:016X} host_ip={} nonce={:016X} ret={:#010x}",
        buffer_length, object, flags, max_public, max_private, user_index, info_ptr, nonce_ptr,
        host, known, memory::load_and_swap<uint64_t>(info.data()), system_link::FormatIpv4(host_ip),
        nonce, (uint32_t)X_E_SUCCESS);
  }
  return X_E_SUCCESS;
}

X_HRESULT JoinSession(memory::Memory* guest_memory, KernelState* kernel_state,
                      const uint8_t* buffer, uint32_t buffer_length) {
  uint32_t object = memory::load_and_swap<uint32_t>(buffer + 0);
  uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
  uint32_t xuid_array = memory::load_and_swap<uint32_t>(buffer + 8);
  uint32_t index_array = memory::load_and_swap<uint32_t>(buffer + 12);
  uint32_t private_array = memory::load_and_swap<uint32_t>(buffer + 16);
  bool is_local = xuid_array == 0;

  auto users = ReadSessionUsers(guest_memory, kernel_state, user_count, xuid_array, index_array,
                                private_array);
  size_t member_count = 0;
  SessionRegistry().Update(object, [&](XSessionRecord& session) {
    AddSessionMembers(session, users);
    member_count = session.members.size();
  });

  REXKRNL_DEBUG("{}({:08X}, {}, {:08X}, {:08X}, {:08X})",
                is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", object, user_count,
                xuid_array, index_array, private_array);
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO(
        "[netlink] {} buf_len={} session={:#010x} users={} xuid_array={:#010x} "
        "index_array={:#010x} priv_array={:#010x} first_xuid={:016X} members={} ret={:#010x}",
        is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", buffer_length, object,
        user_count, xuid_array, index_array, private_array, users.empty() ? 0 : users.front().xuid,
        member_count, (uint32_t)X_E_SUCCESS);
  }
  return X_E_SUCCESS;
}

X_HRESULT LeaveSession(memory::Memory* guest_memory, KernelState* kernel_state,
                       const uint8_t* buffer, uint32_t buffer_length) {
  uint32_t object = memory::load_and_swap<uint32_t>(buffer + 0);
  uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
  uint32_t xuid_array = memory::load_and_swap<uint32_t>(buffer + 8);
  uint32_t index_array = memory::load_and_swap<uint32_t>(buffer + 12);
  bool is_local = xuid_array == 0;

  auto users = ReadSessionUsers(guest_memory, kernel_state, user_count, xuid_array, index_array, 0);
  size_t member_count = 0;
  SessionRegistry().Update(object, [&](XSessionRecord& session) {
    if (is_local) {
      RemoveSessionMembersByUserIndex(session, UserIndicesOf(users));
    } else {
      RemoveSessionMembersByXuid(session, XuidsOf(users));
    }
    member_count = session.members.size();
  });

  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO(
        "[netlink] {} buf_len={} session={:#010x} users={} xuid_array={:#010x} "
        "index_array={:#010x} members={} ret={:#010x}",
        is_local ? "XGISessionLeaveLocal" : "XGISessionLeaveRemote", buffer_length, object,
        user_count, xuid_array, index_array, member_count, (uint32_t)X_E_SUCCESS);
  }
  return X_E_SUCCESS;
}

X_HRESULT GetSessionDetails(memory::Memory* guest_memory, const uint8_t* buffer,
                            uint32_t buffer_length) {
  uint32_t object = memory::load_and_swap<uint32_t>(buffer + 0);
  uint32_t size_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
  uint32_t details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);

  auto session = SessionRegistry().Snapshot(object);
  uint32_t details_size = size_ptr ? LoadGuestU32(guest_memory, size_ptr) : 0;
  uint32_t required = 0;
  uint32_t returned = 0;
  X_HRESULT result = X_E_SUCCESS;
  if (session) {
    ApplyCreatorContexts(*session, UserValueStore());
    required = RequiredLocalDetailsSize(*session);
    // The real API fails a short buffer with ERROR_INSUFFICIENT_BUFFER; campaign co-op works with
    // writing what fits, so that stays until a title is seen to need the failure.
    if (details_ptr) {
      auto out = std::span(guest_memory->TranslateVirtual(details_ptr), details_size);
      returned = WriteLocalDetails(*session, out, details_ptr);
    }
  }

  REXKRNL_DEBUG("XSessionGetDetails({:08X}, {:08X}, {:08X})", object, size_ptr, details_ptr);
  if (REXCVAR_GET(system_link_trace)) {
    REXLOG_INFO(
        "XGI XSessionGetDetails buf_len={} obj={:#010x} details_size={} details_ptr={:#010x} "
        "known={} required={} game_type={:#010x} game_mode={:#010x} members_returned={} "
        "ret={:#010x}",
        buffer_length, object, details_size, details_ptr, session.has_value(), required,
        session ? session->game_type : 0, session ? session->game_mode : 0, returned,
        (uint32_t)result);
  }
  return result;
}

}  // namespace

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  {
    // Generic trace for XGI messages without a detailed case below: any other
    // 0x000B00xx message the dispatcher sees. Read-only: no guest writes.
    static std::atomic<uint64_t> netlink_trace_count{0};
    bool detailed = message == 0x000B0010 || message == 0x000B0012 || message == 0x000B0013 ||
                    message == 0x000B0014 || message == 0x000B0015 || message == 0x000B0016 ||
                    message == 0x000B001C || message == 0x000B001D;
    if (!detailed && REXCVAR_GET(system_link_trace) &&
        NetLinkTraceAllow(&netlink_trace_count)) {
      REXLOG_INFO("[netlink] XGI msg={:#010x} buf_ptr={:#010x} buf_len={}", message, buffer_ptr,
                  buffer_length);
    }
  }
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      UserValueStore().SetContext(user_index, context_id, context_value);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO("XGI SetContext user={} id={:#010x} value={:#010x} ret={:#010x}", user_index,
                    context_id, context_value, (uint32_t)X_E_SUCCESS);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      std::vector<uint8_t> value;
      X_HRESULT result = X_E_SUCCESS;
      if (value_size > kMaxUserPropertySize || (value_size && !value_ptr)) {
        result = X_E_INVALIDARG;
      } else {
        value.resize(value_size);
        if (value_size) {
          std::memcpy(value.data(), memory_->TranslateVirtual(value_ptr), value_size);
        }
        UserValueStore().SetProperty(user_index, property_id, value);
      }
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO("XGI SetProperty user={} id={:#010x} size={} value_head={:016X} ret={:#010x}",
                    user_index, property_id, value_size, LeadingBytes(value), (uint32_t)result);
      }
      return result;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      constexpr uint32_t kEntryIdOffset = 4;
      constexpr uint32_t kEntryStride = 8;
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXKRNL_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                       achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        auto* base = memory_->TranslateVirtual(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = memory::load_and_swap<uint32_t>(base + i * kEntryStride + kEntryIdOffset);
          REXKRNL_INFO("XGIUserWriteAchievements: id={} ({})", id, i);
          kernel_state_->UnlockAchievement(id);
        }
      } else {
        REXKRNL_INFO("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      return CreateSession(memory_, kernel_state_, buffer, buffer_length);
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      SetSessionState(obj_ptr, XSessionState::kDeleted);

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      return JoinSession(memory_, kernel_state_, buffer, buffer_length);
    }
    case 0x000B0013: {
      assert_true(!buffer_length || buffer_length == 20);
      return LeaveSession(memory_, kernel_state_, buffer, buffer_length);
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      SetSessionState(obj_ptr, XSessionState::kInGame);

      static std::atomic<uint64_t> netlink_trace_count{0};
      if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
        REXLOG_INFO("[netlink] XSessionStart buf_len={} obj={:#010x} flags={:#010x} nonce={:016X} ret={:#010x}",
                    buffer_length, obj_ptr, flags, session_nonce, (uint32_t)X_STATUS_SUCCESS);
      }

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);
      SetSessionState(obj_ptr, XSessionState::kLobby);

      static std::atomic<uint64_t> netlink_trace_count{0};
      if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
        REXLOG_INFO("[netlink] XSessionEnd buf_len={} obj={:#010x} flags={:#010x} nonce={:016X} ret={:#010x}",
                    buffer_length, obj_ptr, flags, session_nonce, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionSearch buf_len={} proc={} user={} max_results={} props={} ctx={} "
            "props_ptr={:#010x} ctx_ptr={:#010x} results_size={} results_ptr={:#010x} ret={:#010x}",
            buffer_length, proc_index, user_index, num_results, num_props, num_ctx, props_ptr,
            ctx_ptr, results_buffer_size, search_results_ptr, (uint32_t)X_E_SUCCESS);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t maxPrivateSlots = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                    maxPublicSlots, maxPrivateSlots);
      SessionRegistry().Update(obj_ptr, [&](XSessionRecord& session) {
        session.flags = flags;
        session.max_public = maxPublicSlots;
        session.max_private = maxPrivateSlots;
      });

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      // session_search
      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      //
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);

      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionSearchEx buf_len={} proc={} user={} max_results={} props={} ctx={} "
            "props_ptr={:#010x} ctx_ptr={:#010x} results_size={} results_ptr={:#010x} users={} "
            "ret={:#010x}",
            buffer_length, proc_index, user_index, num_results, num_props, num_ctx, props_ptr,
            ctx_ptr, results_buffer_size, search_results_ptr, num_users, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);
      return GetSessionDetails(memory_, buffer, buffer_length);
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                    session_info_ptr, user_index, reserved1, reserved2, reserved3);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionMigrateHost buf_len={} obj={:#010x} info_ptr={:#010x} user={} "
            "ret={:#010x}",
            buffer_length, obj_ptr, session_info_ptr, user_index, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionArbitrationRegister buf_len={} obj={:#010x} flags={:#010x} nonce={:016X} "
            "duration={} results_size={} results_ptr={:#010x} ret={:#010x}",
            buffer_length, obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
            results_ptr, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionSearchByID buf_len={} user={} ids={} ids_ptr={:#010x} results_size={} "
            "results_ptr={:#010x} ret={:#010x}",
            buffer_length, user_index, num_session_ids, session_ids_ptr, results_buffer_size,
            search_results_ptr, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionModifySkill buf_len={} obj={:#010x} count={} xuids_ptr={:#010x} "
            "ret={:#010x}",
            buffer_length, obj_ptr, array_count, xuid_array_ptr, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XUserReadStats buf_len={} title={:#010x} xuids={} xuids_ptr={:#010x} specs={} "
            "specs_ptr={:#010x} results_size={} results_ptr={:#010x} ret={:#010x}",
            buffer_length, title_id, xuids_count, xuids_ptr, specs_count, specs_ptr, results_size,
            results_ptr, (uint32_t)X_E_SUCCESS);
      }

      WriteEmptyStatsResults(memory_, specs_count, specs_ptr, results_size, results_ptr);
      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionWriteStats buf_len={} obj={:#010x} xuid={:016X} views={} "
            "views_ptr={:#010x} ret={:#010x}",
            buffer_length, obj_ptr, xuid, num_views, views_ptr, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionFlushStats buf_len={} obj={:#010x} xuid={:016X} views={} "
            "views_ptr={:#010x} ret={:#010x}",
            buffer_length, obj_ptr, xuid, num_views, views_ptr, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      std::optional<uint32_t> stored =
          context ? UserValueStore().GetContext(user_index, context_id) : std::nullopt;
      uint32_t value = stored.value_or(0);
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      X_HRESULT result = stored ? X_E_SUCCESS : X_E_FAIL;
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO("XGI GetContext user={} id={:#010x} value={:#010x} ret={:#010x}", user_index,
                    context_id, value, (uint32_t)result);
      }
      return result;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionSearchByIds buf_len={} user={} ids={} ids_ptr={:#010x} results_size={} "
            "results_ptr={:#010x} ret={:#010x}",
            buffer_length, user_index, num_session_ids, session_ids_ptr, results_buffer_size,
            search_results_ptr, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);
      if (REXCVAR_GET(system_link_trace)) {
        REXLOG_INFO(
            "XGI XSessionSearchWeighted buf_len={} proc={} user={} max_results={} "
            "weighted_props={} weighted_ctx={} props={} ctx={} results_size={} "
            "results_ptr={:#010x} users={} weighted={} ret={:#010x}",
            buffer_length, proc_index, user_index, num_results, num_weighted_properties,
            num_weighted_contexts, num_props, num_ctx, results_buffer_size, search_results_ptr,
            num_users, weighted_search, (uint32_t)X_E_SUCCESS);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
