/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>
#include <cstring>
#include <span>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/system_link.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

#include <cerrno>
#endif

REXCVAR_DEFINE_BOOL(system_link_trace, false, "Network",
                    "Verbose System Link diagnostics: trace XGI session, XNet and socket calls");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace system_link = rex::system::system_link;

constexpr u32 kWsaENotSock = 0x2736;
constexpr u32 kWsaEFault = 0x271E;
constexpr u32 kGuestSockaddrInSize = 16;

// M0 diagnostics for System Link (gated by system_link_trace, default off).
// All helpers below are read-only: they never write guest buffers/registers.

// Rate limiter: log the first 50 calls per call site, then every 500th, so
// packet loops (sendto/recvfrom) cannot flood the log.
static bool NetLinkTraceAllow(std::atomic<uint64_t>* counter) {
  uint64_t n = counter->fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= 50 || (n % 500) == 0;
}

// Split a guest AF_INET sockaddr (big-endian fields) into host-order parts.
// Null-safe; yields 0.0.0.0:0 for a null pointer.
static void NetLinkSplitAddrIn(ppc_ptr_t<XSOCKADDR_IN> addr, uint32_t* ip_host_out,
                               uint16_t* port_host_out) {
  if (!addr) {
    *ip_host_out = 0;
    *port_host_out = 0;
    return;
  }
  *ip_host_out = addr->sin_addr;
  *port_host_out = addr->sin_port;
}

// Split a generic guest sockaddr when it carries AF_INET (family + port/addr
// in the first 6 sa_data bytes). Yields 0.0.0.0:0 otherwise.
static void NetLinkSplitAddr(ppc_ptr_t<XSOCKADDR> addr, uint32_t addr_len, uint32_t* ip_host_out,
                             uint16_t* port_host_out) {
  *ip_host_out = 0;
  *port_host_out = 0;
  if (!addr || addr_len < 8 || addr->address_family != AF_INET) {
    return;
  }
  uint16_t port_be;
  uint32_t ip_be;
  std::memcpy(&port_be, addr->sa_data, sizeof(port_be));
  std::memcpy(&ip_be, addr->sa_data + 2, sizeof(ip_be));
  *port_host_out = rex::byte_swap(port_be);
  *ip_host_out = rex::byte_swap(ip_be);
}

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetStartup caller={} params={:#010x} ret=0", caller,
                params ? params.guest_address() : 0);
  }

  system_link::StartPeerResolver();
  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  if (buffer_ptr) {
    system_link::FillRandom(std::span(static_cast<uint8_t*>(buffer_ptr), length));
  }
  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] WSAStartup caller={} version={:#06x} ret={}", caller, version, ret);
  }

  system_link::StartPeerResolver();
  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  if (overlapped_ptr) {
    // auto evt = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  auto ev = object_ref<XEvent>(new XEvent(REX_KERNEL_STATE()));
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

#if REX_PLATFORM_ANDROID
// Out-of-line definitions for the status constants above. They are declared
// static const (not constexpr), so any odr-use — e.g. binding one to the
// const& parameter of the logging layer, as NetDll_XNetGetTitleXnAddr_entry
// does — requires a definition at namespace scope. The desktop toolchains
// happen to fold the value uses they see, but bionic/Clang keeps the
// reference, hence the undefined symbol at link time. Android-only so desktop
// builds are unchanged.
const uint32_t XnAddrStatus::XNET_GET_XNADDR_PENDING;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_NONE;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_ETHERNET;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_STATIC;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_DHCP;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_PPPOE;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_GATEWAY;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_DNS;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_ONLINE;
const uint32_t XnAddrStatus::XNET_GET_XNADDR_TROUBLESHOOT;
#endif  // REX_PLATFORM_ANDROID

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  static_assert(sizeof(XNADDR) == system_link::kXnAddrSize);
  // RakNet's 360 version (58411436) derives "random" numbers from abEnet and fails on a zero MAC,
  // so the MAC is a non-zero, locally administered address unique per instance.
  auto xnaddr = system_link::TitleXnAddr(REX_KERNEL_STATE()->user_profile()->network_xuid());
  std::memcpy(static_cast<XNADDR*>(addr_ptr), xnaddr.data(), xnaddr.size());

  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    uint32_t trace_ip = rex::byte_swap(static_cast<uint32_t>(addr_ptr->ina.s_addr));
    uint32_t trace_online_ip =
        rex::byte_swap(static_cast<uint32_t>(addr_ptr->inaOnline.s_addr));
    uint16_t trace_online_port = addr_ptr->wPortOnline;
    REXLOG_INFO(
        "[netlink] XNetGetTitleXnAddr caller={} ret={:#x} ina={}.{}.{}.{} inaOnline={} "
        "portOnline={} mac={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
        caller, XnAddrStatus::XNET_GET_XNADDR_STATIC, (trace_ip >> 24) & 0xFF,
        (trace_ip >> 16) & 0xFF, (trace_ip >> 8) & 0xFF, trace_ip & 0xFF,
        system_link::FormatIpv4(trace_online_ip), trace_online_port, addr_ptr->abEnet[0],
        addr_ptr->abEnet[1], addr_ptr->abEnet[2], addr_ptr->abEnet[3], addr_ptr->abEnet[4],
        addr_ptr->abEnet[5]);
  }

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
// PeerInAddr picks the reachable address for this peer (LAN vs online).
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  constexpr u32 kWsaEInval = 0x2726;
  u32 result = kWsaEInval;
  uint32_t ina = 0;
  uint32_t ina_online = 0;
  uint32_t target = 0;
  if (xn_addr && in_addr) {
    ina = rex::byte_swap(static_cast<uint32_t>(xn_addr->ina.s_addr));
    ina_online = rex::byte_swap(static_cast<uint32_t>(xn_addr->inaOnline.s_addr));
    target = system_link::PeerInAddr(ina, ina_online, system_link::LocalSubnet(),
                                     system_link::HeardFromRecently(ina));
    uint32_t target_raw = rex::byte_swap(target);
    std::memcpy(static_cast<uint8_t*>(in_addr), &target_raw, sizeof(target_raw));
    result = 0;
  }
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetXnAddrToInAddr caller={} ina={} inaOnline={} -> {} ret={:#x}", caller,
                system_link::FormatIpv4(ina), system_link::FormatIpv4(ina_online),
                system_link::FormatIpv4(target), result);
  }
  return result;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, mapped_void in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    uint32_t trace_ip = 0;
    if (in_addr) {
      uint32_t raw;
      std::memcpy(&raw, static_cast<uint8_t*>(in_addr), sizeof(raw));
      trace_ip = rex::byte_swap(raw);
    }
    REXLOG_INFO("[netlink] XNetInAddrToXnAddr caller={} in_addr={}.{}.{}.{} ret=1", caller,
                (trace_ip >> 24) & 0xFF, (trace_ip >> 16) & 0xFF, (trace_ip >> 8) & 0xFF,
                trace_ip & 0xFF);
  }
  return 1;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  uint32_t status = system_link::EthernetLinkStatus();
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetGetEthernetLinkStatus caller={} ret={:#x}", caller, status);
  }
  return status;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetRegisterKey caller={} ret=0", caller);
  }
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void xnkid) {
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetUnregisterKey caller={} ret=0", caller);
  }
  return 0;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) {
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetUnregisterInAddr caller={} ret=0", caller);
  }
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] XNetQosListen caller={} data_size={} flags={:#010x} ret={:#x}", caller,
                data_size, flags, X_ERROR_FUNCTION_FAILED);
  }
  return X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  auto socket = object_ref<XSocket>(new XSocket(REX_KERNEL_STATE()));
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->ReleaseHandle();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    static std::atomic<uint64_t> netlink_trace_fail_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_fail_count)) {
      REXLOG_INFO("[netlink] socket af={} type={} proto={} ret=-1 error={:#x}", af, type, protocol,
                  error);
    }
    return -1;
  }

  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] socket af={} type={} proto={} ret={:#010x}", af, type, protocol,
                socket->handle());
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    XThread::SetLastError(XSocket::LastHostError());
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    static std::atomic<uint64_t> netlink_trace_bad_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_bad_count)) {
      REXLOG_INFO("[netlink] setsockopt s={:#010x} ret=-1 (bad handle)", socket_handle);
    }
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (XFAILED(status)) {
    XThread::SetLastError(XSocket::LastHostError());
  }
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    uint32_t trace_value = 0;
    if (optval_ptr && optlen >= 4) {
      uint32_t raw;
      std::memcpy(&raw, static_cast<uint8_t*>(optval_ptr), sizeof(raw));
      trace_value = rex::byte_swap(raw);  // guest integers are big-endian
    }
    REXLOG_INFO("[netlink] setsockopt s={:#010x} level={:#x} opt={:#x} optlen={} value={:#x} ret={}",
                socket_handle, level, optname, optlen, trace_value,
                XSUCCEEDED(status) ? 0 : -1);
  }
  return XSUCCEEDED(status) ? 0 : -1;
}

// M1: getsockopt for the common SOL_SOCKET integer options (SO_BROADCAST,
// SO_RCVBUF, SO_SNDBUF, SO_ERROR, SO_TYPE), forwarded to the host socket like
// setsockopt above. Integer values are big-endian on the guest, so the host
// result is byte-swapped on the way back.
u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, mapped_u32 optlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    static std::atomic<uint64_t> netlink_trace_bad_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_bad_count)) {
      REXLOG_INFO("[netlink] getsockopt s={:#010x} ret=-1 (bad handle)", socket_handle);
    }
    return -1;
  }
  if (!optval_ptr || !optlen_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  int host_level = static_cast<int>(level);
  int host_optname = static_cast<int>(optname);
  XSocket::NativeSocketOption(level, optname, &host_level, &host_optname);

  // *optlen_ptr is big-endian on the guest; MappedPtr converts on access.
  int host_len = static_cast<int>(optlen_ptr.value());
  uint8_t host_buf[128];
  if (host_len < 0) {
    host_len = 0;
  }
  if (host_len > static_cast<int>(sizeof(host_buf))) {
    host_len = static_cast<int>(sizeof(host_buf));
  }

#if REX_PLATFORM_WIN32
  int ret = ::getsockopt(static_cast<SOCKET>(socket->native_handle()), host_level, host_optname,
                         reinterpret_cast<char*>(host_buf), &host_len);
#else
  socklen_t optlen_native = static_cast<socklen_t>(host_len);
  int ret = ::getsockopt(static_cast<int>(socket->native_handle()), host_level, host_optname,
                         reinterpret_cast<char*>(host_buf), &optlen_native);
  host_len = static_cast<int>(optlen_native);
#endif

  uint32_t trace_value = 0;
  bool trace_have_value = false;
  u32 result;
  if (ret != 0) {
#if REX_PLATFORM_WIN32
    XThread::SetLastError(WSAGetLastError());
#else
    XThread::SetLastError(system_link::WsaErrorFromErrno(errno));
#endif
    result = static_cast<u32>(-1);
  } else {
    bool is_int_opt = (level == 0xFFFF) && (optname == 0x0020 || optname == 0x1001 ||
                                            optname == 0x1002 || optname == 0x1007 ||
                                            optname == 0x1008);
    if (is_int_opt && host_len == 4) {
      uint32_t host_value;
      std::memcpy(&host_value, host_buf, sizeof(host_value));
      memory::store_and_swap<uint32_t>(static_cast<uint8_t*>(optval_ptr), host_value);
      trace_value = host_value;
      trace_have_value = true;
    } else {
      std::memcpy(static_cast<uint8_t*>(optval_ptr), host_buf, host_len);
    }
    *optlen_ptr = static_cast<uint32_t>(host_len);
    result = 0;
  }

  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO(
        "[netlink] getsockopt s={:#010x} level={:#x} opt={:#x} optlen={} value={} ret={}",
        socket_handle, level, optname, static_cast<uint32_t>(host_len),
        trace_have_value ? trace_value : 0, static_cast<int32_t>(result));
  }
  return result;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(XSocket::LastHostError());
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    static std::atomic<uint64_t> netlink_trace_bad_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_bad_count)) {
      REXLOG_INFO("[netlink] bind s={:#010x} ret=-1 (bad handle)", socket_handle);
    }
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  int trace_ret;
  if (XFAILED(status)) {
    XThread::SetLastError(XSocket::LastHostError());
    trace_ret = -1;
  } else {
    trace_ret = 0;
  }
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    uint32_t trace_ip;
    uint16_t trace_port;
    NetLinkSplitAddrIn(name, &trace_ip, &trace_port);
    REXLOG_INFO("[netlink] bind s={:#010x} addr={}.{}.{}.{}:{} namelen={} ret={}", socket_handle,
                (trace_ip >> 24) & 0xFF, (trace_ip >> 16) & 0xFF, (trace_ip >> 8) & 0xFF,
                trace_ip & 0xFF, trace_port, namelen, trace_ret);
  }

  return trace_ret;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    static std::atomic<uint64_t> netlink_trace_bad_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_bad_count)) {
      REXLOG_INFO("[netlink] connect s={:#010x} ret=-1 (bad handle)", socket_handle);
    }
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  int trace_ret;
  if (XFAILED(status)) {
    XThread::SetLastError(XSocket::LastHostError());
    trace_ret = -1;
  } else {
    trace_ret = 0;
  }
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    uint32_t trace_ip;
    uint16_t trace_port;
    NetLinkSplitAddr(name, namelen, &trace_ip, &trace_port);
    REXLOG_INFO("[netlink] connect s={:#010x} addr={}.{}.{}.{}:{} namelen={} ret={}",
                socket_handle, (trace_ip >> 24) & 0xFF, (trace_ip >> 16) & 0xFF,
                (trace_ip >> 8) & 0xFF, trace_ip & 0xFF, trace_port, namelen, trace_ret);
  }

  return trace_ret;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(XSocket::LastHostError());
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    XThread::SetLastError(XSocket::LastHostError());
    return -1;
  }
}

static void StoreGuestSockaddrIn(ppc_ptr_t<XSOCKADDR_IN> guest, const N_XSOCKADDR_IN& native) {
  guest->sin_family = native.sin_family;
  guest->sin_port = native.sin_port;
  guest->sin_addr = native.sin_addr;
  std::memset(guest->x_sin_zero, 0, sizeof(guest->x_sin_zero));
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 namelen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  uint32_t trace_ip = 0;
  uint16_t trace_port = 0;
  int trace_ret = -1;
  u32 result = static_cast<u32>(-1);
  if (!socket) {
    XThread::SetLastError(kWsaENotSock);
  } else if (!name || !namelen_ptr || *namelen_ptr < kGuestSockaddrInSize) {
    XThread::SetLastError(kWsaEFault);
  } else {
    N_XSOCKADDR_IN native_name;
    int native_len = static_cast<int>(kGuestSockaddrInSize);
    if (XFAILED(socket->GetSockName(&native_name, &native_len))) {
      XThread::SetLastError(XSocket::LastHostError());
    } else {
      StoreGuestSockaddrIn(name, native_name);
      *namelen_ptr = kGuestSockaddrInSize;
      trace_ip = native_name.sin_addr;
      trace_port = native_name.sin_port;
      trace_ret = 0;
      result = 0;
    }
  }
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] getsockname s={:#010x} addr={}:{} ret={}", socket_handle,
                system_link::FormatIpv4(trace_ip), trace_port, trace_ret);
  }
  return result;
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 namelen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  uint32_t trace_ip = 0;
  uint16_t trace_port = 0;
  int trace_ret = -1;
  u32 result = static_cast<u32>(-1);
  if (!socket) {
    XThread::SetLastError(kWsaENotSock);
  } else if (!name || !namelen_ptr || *namelen_ptr < kGuestSockaddrInSize) {
    XThread::SetLastError(kWsaEFault);
  } else {
    N_XSOCKADDR_IN native_name;
    int native_len = static_cast<int>(kGuestSockaddrInSize);
    if (XFAILED(socket->GetPeerName(&native_name, &native_len))) {
      XThread::SetLastError(XSocket::LastHostError());
    } else {
      StoreGuestSockaddrIn(name, native_name);
      *namelen_ptr = kGuestSockaddrInSize;
      trace_ip = native_name.sin_addr;
      trace_port = native_name.sin_port;
      trace_ret = 0;
      result = 0;
    }
  }
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_count)) {
    REXLOG_INFO("[netlink] getpeername s={:#010x} addr={}:{} ret={}", socket_handle,
                system_link::FormatIpv4(trace_ip), trace_port, trace_ret);
  }
  return result;
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        this->count = i;
        break;
      }
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }

  int MaxNativeHandle() const {
    int max_handle = -1;
    for (uint32_t i = 0; i < count; ++i) {
      int handle = static_cast<int>(sockets[i]->native_handle());
      if (handle > max_handle) {
        max_handle = handle;
      }
    }
    return max_handle;
  }
};

#if !REX_PLATFORM_WIN32
static int SelectHostNfds(const host_set& read_set, const host_set& write_set,
                          const host_set& except_set) {
  int host_nfds = read_set.MaxNativeHandle();
  int write_max = write_set.MaxNativeHandle();
  if (write_max > host_nfds) {
    host_nfds = write_max;
  }
  int except_max = except_set.MaxNativeHandle();
  if (except_max > host_nfds) {
    host_nfds = except_max;
  }
  return host_nfds < 0 ? 0 : host_nfds + 1;
}
#endif

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  // Snapshot the requested sets before the call below overwrites them (read-only).
  const bool trace_link = REXCVAR_GET(system_link_trace);
  uint32_t trace_rc = 0, trace_wc = 0, trace_ec = 0;
  bool trace_has_timeout = false;
  int32_t trace_tv_sec = 0, trace_tv_usec = 0;
  if (trace_link) {
    if (readfds) {
      trace_rc = readfds->fd_count;
    }
    if (writefds) {
      trace_wc = writefds->fd_count;
    }
    if (exceptfds) {
      trace_ec = exceptfds->fd_count;
    }
    if (timeout_ptr) {
      trace_has_timeout = true;
      trace_tv_sec = static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]);
      trace_tv_usec = static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1]);
    }
  }
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
#if REX_PLATFORM_WIN32
  int host_nfds = nfds;
#else
  // The guest nfds is useless (UE3 passes 0), so derive it from the live sockets.
  int host_nfds = SelectHostNfds(host_readfds, host_writefds, host_exceptfds);
#endif
  int ret = select(host_nfds, readfds ? &native_readfds : nullptr,
                   writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (ret < 0) {
#if REX_PLATFORM_WIN32
    XThread::SetLastError(WSAGetLastError());
#else
    XThread::SetLastError(system_link::WsaErrorFromErrno(errno));
#endif
  }
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  static std::atomic<uint64_t> netlink_trace_count{0};
  if (trace_link && NetLinkTraceAllow(&netlink_trace_count)) {
    // No timeout pointer means block; shown as -1.000000.
    REXLOG_INFO(
        "[netlink] select nfds={} host_nfds={} read={} write={} except={} timeout={}.{:06d} ret={}",
        nfds, host_nfds, trace_rc, trace_wc, trace_ec, trace_has_timeout ? trace_tv_sec : -1,
        trace_has_timeout ? trace_tv_usec : 0, ret);
  }
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret < 0) {
    XThread::SetLastError(XSocket::LastHostError());
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    static std::atomic<uint64_t> netlink_trace_bad_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_bad_count)) {
      REXLOG_INFO("[netlink] recvfrom s={:#010x} ret=-1 (bad handle)", socket_handle);
    }
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (ret >= 0 && from_ptr) {
    StoreGuestSockaddrIn(from_ptr, native_from);
  }
  if (ret >= 0 && fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    XThread::SetLastError(XSocket::LastHostError());
  }

  // Would-block polls must not spend the trace budget of real datagrams.
  static std::atomic<uint64_t> netlink_trace_ok_count{0};
  static std::atomic<uint64_t> netlink_trace_fail_count{0};
  if (REXCVAR_GET(system_link_trace) &&
      NetLinkTraceAllow(ret >= 0 ? &netlink_trace_ok_count : &netlink_trace_fail_count)) {
    uint32_t trace_ip;
    uint16_t trace_port;
    NetLinkSplitAddrIn(from_ptr, &trace_ip, &trace_port);
    REXLOG_INFO(
        "[netlink] recvfrom s={:#010x} buf_len={} flags={:#x} from={}.{}.{}.{}:{} fromlen={} "
        "ret={}",
        socket_handle, buf_len, flags, (trace_ip >> 24) & 0xFF, (trace_ip >> 16) & 0xFF,
        (trace_ip >> 8) & 0xFF, trace_ip & 0xFF, trace_port,
        fromlen_ptr ? fromlen_ptr.value() : 0, ret);
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret < 0) {
    XThread::SetLastError(XSocket::LastHostError());
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    static std::atomic<uint64_t> netlink_trace_bad_count{0};
    if (REXCVAR_GET(system_link_trace) && NetLinkTraceAllow(&netlink_trace_bad_count)) {
      REXLOG_INFO("[netlink] sendto s={:#010x} ret=-1 (bad handle)", socket_handle);
    }
    return -1;
  }

  N_XSOCKADDR_IN native_to;
  if (to_ptr) {
    native_to = *to_ptr;
  }
  int ret = socket->SendTo(buf_ptr, buf_len, flags, to_ptr ? &native_to : nullptr, to_len);
  if (ret < 0) {
    XThread::SetLastError(XSocket::LastHostError());
  }
  // Failed sends get their own trace budget, like recvfrom.
  static std::atomic<uint64_t> netlink_trace_ok_count{0};
  static std::atomic<uint64_t> netlink_trace_fail_count{0};
  if (REXCVAR_GET(system_link_trace) &&
      NetLinkTraceAllow(ret >= 0 ? &netlink_trace_ok_count : &netlink_trace_fail_count)) {
    uint32_t trace_ip;
    uint16_t trace_port;
    NetLinkSplitAddrIn(to_ptr, &trace_ip, &trace_port);
    REXLOG_INFO("[netlink] sendto s={:#010x} buf_len={} flags={:#x} to={}.{}.{}.{}:{} to_len={} ret={}",
                socket_handle, buf_len, flags, (trace_ip >> 24) & 0xFF, (trace_ip >> 16) & 0xFF,
                (trace_ip >> 8) & 0xFF, trace_ip & 0xFF, trace_port, to_len, ret);
  }
  return ret;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

// M1 correctness: Gears of War 2 imports the stubs converted below, and
// REX_EXPORT_STUB leaves r3 untouched (an undefined return). These replacements
// behave like REX_STUB_RETURN (warn + fixed r3) and additionally emit a
// once-only [netlink] line when system_link_trace is on. Socket APIs return
// SOCKET_ERROR with WSAEOPNOTSUPP (0x273D, "operation not supported"); XNet APIs
// return X_ERROR_FUNCTION_FAILED. Stubs GOW2 does not import are left alone.
// The stub macros below are defined at global scope; the X_ERROR_* macros name X_RESULT.
using rex::X_RESULT;

#define NETLINK_STUB_RETURN(name, retval)                                           \
  extern "C" REX_FUNC(name) {                                                       \
    (void)base;                                                                     \
    REXKRNL_WARN("{} STUB - returning {:#x}", #name, static_cast<uint32_t>(retval)); \
    static bool netlink_stub_logged = false;                                        \
    if (!netlink_stub_logged && REXCVAR_GET(system_link_trace)) {                   \
      netlink_stub_logged = true;                                                   \
      REXLOG_INFO("[netlink] stub {} returning {:#x} (unimplemented)", #name,        \
                  static_cast<uint32_t>(retval));                                   \
    }                                                                               \
    ctx.r3.u64 = (retval);                                                          \
  }                                                                                 \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

#define NETLINK_STUB_WSA(name, retval)                                              \
  extern "C" REX_FUNC(name) {                                                       \
    (void)base;                                                                     \
    REXKRNL_WARN("{} STUB - returning {:#x}", #name, static_cast<uint32_t>(retval)); \
    static bool netlink_stub_logged = false;                                        \
    if (!netlink_stub_logged && REXCVAR_GET(system_link_trace)) {                   \
      netlink_stub_logged = true;                                                   \
      REXLOG_INFO("[netlink] stub {} returning {:#x} (unimplemented)", #name,        \
                  static_cast<uint32_t>(retval));                                   \
    }                                                                               \
    ::rex::system::XThread::SetLastError(0x273D); /* WSAEOPNOTSUPP */                \
    ctx.r3.u64 = (retval);                                                          \
  }                                                                                 \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

// M0 trace only: called out by the trace list but not imported by GOW2, so r3
// is deliberately left untouched (no behavior change); logs when tracing is on.
#define NETLINK_STUB_TRACE_ONLY(name)                                  \
  extern "C" REX_FUNC(name) {                                          \
    (void)base;                                                        \
    REXKRNL_WARN("{} STUB", #name);                                    \
    static std::atomic<uint64_t> netlink_trace_count{0};               \
    if (REXCVAR_GET(system_link_trace) &&                              \
        ::rex::kernel::xam::NetLinkTraceAllow(&netlink_trace_count)) { \
      REXLOG_INFO("[netlink] {} called (unimplemented stub)", #name);  \
    }                                                                  \
  }                                                                    \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
NETLINK_STUB_WSA(__imp__NetDll_WSACancelOverlappedIO, -1);
NETLINK_STUB_WSA(__imp__NetDll_WSAEventSelect, -1);
// BOOL API: FALSE (0) is failure; any nonzero value would mean success.
NETLINK_STUB_WSA(__imp__NetDll_WSAGetOverlappedResult, 0);
NETLINK_STUB_WSA(__imp__NetDll_WSARecv, -1);
NETLINK_STUB_WSA(__imp__NetDll_WSASend, -1);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetConnect);
NETLINK_STUB_TRACE_ONLY(__imp__NetDll_XNetCreateKey);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
NETLINK_STUB_TRACE_ONLY(__imp__NetDll_XNetGetConnectStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
NETLINK_STUB_RETURN(__imp__NetDll_XNetQosLookup, X_ERROR_FUNCTION_FAILED);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
NETLINK_STUB_RETURN(__imp__NetDll_XNetServerToInAddr, X_ERROR_FUNCTION_FAILED);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
