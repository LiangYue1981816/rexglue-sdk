/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <tuple>

#include <rex/cvar.h>

#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/system_link.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_INT32(net_traffic_log_sec, 0, "Network",
                     "Log the UDP bytes and packets sent to and received from each peer every this "
                     "many seconds (0 = off); the per-client bandwidth baseline for co-op");

namespace rex::system {
namespace {

thread_local uint32_t t_host_error = 0;

constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaEFault = 10014;
constexpr uint32_t kWsaEMsgSize = 10040;
constexpr uint32_t kGuestMsgPeek = 0x2;
constexpr int kMaxDroppedDatagramsPerCall = 64;
constexpr int kSockaddrInSize = 16;
constexpr uint32_t kGuestSolSocket = 0xFFFF;
constexpr uint32_t kGuestOptNoEncrypt1 = 0x5801;
constexpr uint32_t kGuestOptNoEncrypt2 = 0x5802;
constexpr uint16_t kAfInet = 2;
constexpr int kMinInetSockaddrLen = 8;
constexpr uint32_t kLingerValueSize = 4;
constexpr uint32_t kIntOptionSize = 4;
constexpr uint32_t kMaxSetsockoptRemapLogs = 50;
constexpr uint32_t kMaxDropLogs = 50;
constexpr uint32_t kDropLogInterval = 500;
constexpr size_t kBeaconLogCapacity = system_link::kBeaconHelloCapacity;

template <class T>
bool RememberOnce(std::vector<T>& seen, const T& value, size_t capacity) {
  if (std::find(seen.begin(), seen.end(), value) != seen.end()) {
    return false;
  }
  if (capacity == 0) {
    return false;
  }
  if (seen.size() >= capacity) {
    seen.erase(seen.begin());
  }
  seen.push_back(value);
  return true;
}

struct TrafficTotals {
  uint64_t bytes = 0;
  uint64_t packets = 0;
};

// (sent, ip, port) -> totals of the current window.
using TrafficKey = std::tuple<bool, uint32_t, uint16_t>;

struct TrafficWindow {
  std::mutex lock;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::map<TrafficKey, TrafficTotals> totals;
};

TrafficWindow& Traffic() {
  static TrafficWindow window;
  return window;
}

void LogTrafficWindow(const std::map<TrafficKey, TrafficTotals>& totals, double seconds) {
  for (const auto& [key, t] : totals) {
    const auto& [sent, ip, port] = key;
    REXLOG_INFO("[nettraffic] last {:.1f} s: {} {}:{} {} B in {} packets ({:.0f} B/s)", seconds,
                sent ? "sent to" : "received from", system_link::FormatIpv4(ip), port, t.bytes,
                t.packets, double(t.bytes) / seconds);
  }
}

void CountTraffic(bool sent, uint32_t ip, uint16_t port, int bytes) {
  const int32_t period = REXCVAR_GET(net_traffic_log_sec);
  if (period <= 0 || bytes <= 0) return;
  TrafficWindow& window = Traffic();
  std::lock_guard<std::mutex> hold(window.lock);
  TrafficTotals& t = window.totals[{sent, ip, port}];
  t.bytes += uint64_t(bytes);
  ++t.packets;
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - window.start).count();
  if (seconds < period) return;
  LogTrafficWindow(window.totals, seconds);
  window.totals.clear();
  window.start = now;
}

sockaddr_in MakeHostSockaddr(uint32_t ip, uint16_t port) {
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(ip);
  return addr;
}

#if REX_PLATFORM_WIN32
// Otherwise an ICMP port-unreachable turns into error 10054 on the next receive, which the game
// treats as a lost connection once the link reports active.
void DisableUdpConnectionReset(uint64_t handle) {
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(static_cast<SOCKET>(handle), SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0,
           &returned, nullptr, nullptr);
}
#endif

void LogBindRewrite(uint32_t requested_ip, uint32_t effective_ip, uint16_t port) {
  REXLOG_INFO("[netlink] bind {}:{} -> {}:{} (net_bind_ip)", system_link::FormatIpv4(requested_ip),
              port, system_link::FormatIpv4(effective_ip), port);
}

bool ShouldLogSetsockoptRemap() {
  static std::atomic<uint32_t> logged{0};
  return logged.fetch_add(1, std::memory_order_relaxed) < kMaxSetsockoptRemapLogs;
}

void LogSetsockoptRemap(uint32_t guest_level, uint32_t guest_opt, int host_level, int host_opt) {
  if (!system_link::TraceEnabled()) {
    return;
  }
  if (host_level == static_cast<int>(guest_level) && host_opt == static_cast<int>(guest_opt)) {
    return;
  }
  if (!ShouldLogSetsockoptRemap()) {
    return;
  }
  REXLOG_INFO("[netlink] setsockopt level {:#x} opt {:#x} -> host level {} opt {}", guest_level,
              guest_opt, host_level, host_opt);
}

bool ShouldLogDroppedDatagram() {
  static std::atomic<uint32_t> logged{0};
  uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
  return n < kMaxDropLogs || (n % kDropLogInterval == 0);
}

bool ShouldLogSendtoRemap() {
  static std::atomic<uint32_t> logged{0};
  uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
  return n < kMaxDropLogs || (n % kDropLogInterval == 0);
}

void LogSendtoRemap(uint16_t guest_port, uint16_t host_port) {
  if (guest_port == host_port) {
    return;
  }
  if (!system_link::TraceEnabled()) {
    return;
  }
  if (!ShouldLogSendtoRemap()) {
    return;
  }
  REXLOG_INFO("[netlink] sendto port {} -> {} (net_port_offset)", guest_port, host_port);
}

bool ShouldLogFanOutTrace() {
  static std::atomic<uint32_t> logged{0};
  uint32_t n = logged.fetch_add(1, std::memory_order_relaxed);
  return n < kMaxDropLogs || (n % kDropLogInterval == 0);
}

std::string FormatEndpointList(const std::vector<system_link::Endpoint>& endpoints) {
  std::string list;
  for (const auto& endpoint : endpoints) {
    if (!list.empty()) {
      list += ", ";
    }
    list += system_link::FormatEndpoint(endpoint);
  }
  return list;
}

bool IsPlainLanBroadcast(uint16_t port, const std::vector<uint32_t>& lan_ips,
                         const std::vector<system_link::Endpoint>& endpoints) {
  if (endpoints.size() != lan_ips.size()) {
    return false;
  }
  for (size_t i = 0; i < endpoints.size(); ++i) {
    if (endpoints[i] != system_link::Endpoint{lan_ips[i], port}) {
      return false;
    }
  }
  return true;
}

void LogBroadcastFanOut(uint16_t port, const std::vector<uint32_t>& lan_ips,
                        const std::vector<system_link::Endpoint>& endpoints) {
  if (!endpoints.empty() && !IsPlainLanBroadcast(port, lan_ips, endpoints)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      REXLOG_INFO("[netlink] broadcasts to port {} go out to [{}]", port,
                  FormatEndpointList(endpoints));
    }
  }
  if (!system_link::TraceEnabled()) {
    return;
  }
  if (!ShouldLogFanOutTrace()) {
    return;
  }
  REXLOG_INFO("[netlink] broadcasts to port {} go out to [{}]", port,
              FormatEndpointList(endpoints));
}

bool IsFirstContact(uint64_t socket_key, const system_link::Endpoint& peer) {
  static std::mutex mutex;
  static system_link::FirstContacts contacts{system_link::kFirstContactCapacity};
  std::lock_guard<std::mutex> lock(mutex);
  return contacts.IsFirst(socket_key, peer);
}

uint64_t ReceiveContactKey(uint64_t handle) { return handle * 2; }

uint64_t SendContactKey(uint64_t handle) { return handle * 2 + 1; }

uint16_t LocalHostPort(uint64_t handle) {
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  if (getsockname(handle, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return 0;
  }
  return ntohs(addr.sin_port);
}

void LogFirstReceive(uint64_t handle, const system_link::Endpoint& source, int len) {
  if (!system_link::TraceEnabled() || !IsFirstContact(ReceiveContactKey(handle), source)) {
    return;
  }
  REXLOG_INFO("[netlink] first datagram from {} on local port {} ({} bytes)",
              system_link::FormatEndpoint(source), LocalHostPort(handle), len);
}

void LogFirstSend(uint64_t handle, const system_link::Endpoint& destination, int len) {
  if (!system_link::TraceEnabled() || !IsFirstContact(SendContactKey(handle), destination)) {
    return;
  }
  REXLOG_INFO("[netlink] first datagram to {} from local port {} ({} bytes)",
              system_link::FormatEndpoint(destination), LocalHostPort(handle), len);
}

void FillGuestSockaddrIn(N_XSOCKADDR_IN* out, const sockaddr_in& host_addr) {
  out->sin_family = kAfInet;
  out->sin_addr = ntohl(host_addr.sin_addr.s_addr);
  out->sin_port =
      system_link::HostToGuestPort(ntohs(host_addr.sin_port), system_link::PortOffset());
  std::memset(out->x_sin_zero, 0, sizeof(out->x_sin_zero));
}

std::vector<uint8_t>& HostDatagramScratch() {
  thread_local std::vector<uint8_t> scratch(system_link::kMaxUdpDatagramSize);
  return scratch;
}

void WriteGuestSource(N_XSOCKADDR_IN* from, uint32_t* from_len, const sockaddr_in& nfrom,
                      socklen_t nfromlen) {
  if (from) {
    FillGuestSockaddrIn(from, nfrom);
  }
  if (from_len) {
    *from_len = nfromlen;
  }
}

bool CopyDatagramToGuest(const uint8_t* data, int size, uint8_t* buf, uint32_t buf_len) {
  uint32_t length = static_cast<uint32_t>(size);
  uint32_t count = std::min(length, buf_len);
  if (count > 0) {
    std::memcpy(buf, data, count);
  }
  return length <= buf_len;
}

uint16_t ReadInetSockaddrPort(const N_XSOCKADDR& addr) {
  uint8_t hi = static_cast<uint8_t>(addr.sa_data[0]);
  uint8_t lo = static_cast<uint8_t>(addr.sa_data[1]);
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

void WriteInetSockaddrPort(N_XSOCKADDR* addr, uint16_t port) {
  addr->sa_data[0] = static_cast<char>((port >> 8) & 0xFF);
  addr->sa_data[1] = static_cast<char>(port & 0xFF);
}

uint16_t ReadGenericSockaddrPort(const sockaddr& addr) {
  uint8_t hi = static_cast<uint8_t>(addr.sa_data[0]);
  uint8_t lo = static_cast<uint8_t>(addr.sa_data[1]);
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

void WriteGenericSockaddrPort(sockaddr* addr, uint16_t port) {
  addr->sa_data[0] = static_cast<char>((port >> 8) & 0xFF);
  addr->sa_data[1] = static_cast<char>(port & 0xFF);
}

enum class IoctlOutcome {
  kOk,
  kNullArg,
  kHostError,
};

IoctlOutcome ApplyGuestNonBlocking(uint64_t handle, uint8_t* arg_ptr) {
  if (arg_ptr == nullptr) {
    return IoctlOutcome::kNullArg;
  }
  uint32_t enabled =
      system_link::ReadBigEndian32(std::span<const uint8_t, 4>(arg_ptr, 4));
#if REX_PLATFORM_WIN32
  u_long mode = enabled != 0 ? 1 : 0;
  if (ioctlsocket(handle, FIONBIO, &mode) != 0) {
    return IoctlOutcome::kHostError;
  }
#else
  int fd = static_cast<int>(handle);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return IoctlOutcome::kHostError;
  }
  if (enabled != 0) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }
  if (fcntl(fd, F_SETFL, flags) != 0) {
    return IoctlOutcome::kHostError;
  }
#endif
  return IoctlOutcome::kOk;
}

IoctlOutcome QueryGuestAvailableBytes(uint64_t handle, uint8_t* arg_ptr) {
  if (arg_ptr == nullptr) {
    return IoctlOutcome::kNullArg;
  }
#if REX_PLATFORM_WIN32
  u_long count = 0;
  if (ioctlsocket(handle, FIONREAD, &count) != 0) {
    return IoctlOutcome::kHostError;
  }
  system_link::WriteBigEndian32(std::span<uint8_t, 4>(arg_ptr, 4),
                                static_cast<uint32_t>(count));
#else
  int count = 0;
  if (ioctl(static_cast<int>(handle), FIONREAD, &count) != 0) {
    return IoctlOutcome::kHostError;
  }
  system_link::WriteBigEndian32(std::span<uint8_t, 4>(arg_ptr, 4),
                                static_cast<uint32_t>(count));
#endif
  return IoctlOutcome::kOk;
}

int PassthroughGuestIoctl(uint64_t handle, uint32_t cmd, uint8_t* arg_ptr) {
  return rex::net::socket_ioctl(handle, cmd, arg_ptr);
}

}  // namespace

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

void XSocket::CaptureHostError() {
#if REX_PLATFORM_WIN32
  t_host_error = static_cast<uint32_t>(WSAGetLastError());
#else
  t_host_error = system_link::WsaErrorFromErrno(errno);
#endif
}

void XSocket::SetHostError(uint32_t wsa_error) {
  t_host_error = wsa_error;
}

uint32_t XSocket::LastHostError() {
  return t_host_error;
}

bool XSocket::NativeSocketOption(uint32_t guest_level, uint32_t guest_optname, int* native_level,
                                int* native_optname) {
  using system_link::GuestSocketOption;
  switch (system_link::ClassifyGuestSocketOption(guest_level, guest_optname)) {
    case GuestSocketOption::kReuseAddr:
      *native_level = SOL_SOCKET;
      *native_optname = SO_REUSEADDR;
      return true;
    case GuestSocketOption::kKeepAlive:
      *native_level = SOL_SOCKET;
      *native_optname = SO_KEEPALIVE;
      return true;
    case GuestSocketOption::kBroadcast:
      *native_level = SOL_SOCKET;
      *native_optname = SO_BROADCAST;
      return true;
    case GuestSocketOption::kOobInline:
      *native_level = SOL_SOCKET;
      *native_optname = SO_OOBINLINE;
      return true;
    case GuestSocketOption::kLinger:
      *native_level = SOL_SOCKET;
      *native_optname = SO_LINGER;
      return true;
    case GuestSocketOption::kSendBuffer:
      *native_level = SOL_SOCKET;
      *native_optname = SO_SNDBUF;
      return true;
    case GuestSocketOption::kReceiveBuffer:
      *native_level = SOL_SOCKET;
      *native_optname = SO_RCVBUF;
      return true;
    case GuestSocketOption::kError:
      *native_level = SOL_SOCKET;
      *native_optname = SO_ERROR;
      return true;
    case GuestSocketOption::kType:
      *native_level = SOL_SOCKET;
      *native_optname = SO_TYPE;
      return true;
    case GuestSocketOption::kUnknown:
      *native_level = static_cast<int>(guest_level);
      *native_optname = static_cast<int>(guest_optname);
      return false;
  }
  *native_level = static_cast<int>(guest_level);
  *native_optname = static_cast<int>(guest_optname);
  return false;
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }
#if REX_PLATFORM_WIN32
  if (type == Type::X_SOCK_DGRAM) {
    DisableUdpConnectionReset(native_handle_);
  }
#endif

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;
  }

  int ret = rex::net::socket_close(native_handle_);
  native_handle_ = -1;
  if (ret != 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == kGuestSolSocket && (optname == kGuestOptNoEncrypt1 || optname == kGuestOptNoEncrypt2)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  auto kind = system_link::ClassifyGuestSocketOption(level, optname);
  if (kind == system_link::GuestSocketOption::kLinger && optlen >= kLingerValueSize &&
      optval_ptr != nullptr) {
    return SetLingerOption(static_cast<const uint8_t*>(optval_ptr));
  }

  int native_level = 0;
  int native_optname = 0;
  NativeSocketOption(level, optname, &native_level, &native_optname);

  // Guest option values are big-endian; passed through raw, a 128 KiB SO_RCVBUF became 512 bytes.
  char* value = static_cast<char*>(optval_ptr);
  uint32_t host_value = 0;
  if (optlen == kIntOptionSize && optval_ptr != nullptr) {
    host_value = memory::load_and_swap<uint32_t>(optval_ptr);
    value = reinterpret_cast<char*>(&host_value);
  }
  LogSetsockoptRemap(level, optname, native_level, native_optname);
  int ret = setsockopt(native_handle_, native_level, native_optname, value, optlen);
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (kind == system_link::GuestSocketOption::kBroadcast) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetLingerOption(const uint8_t* guest_value) {
  if (guest_value == nullptr) {
    SetHostError(kWsaEFault);
    return X_STATUS_UNSUCCESSFUL;
  }
  system_link::LingerOption parsed =
      system_link::ReadGuestLinger(std::span<const uint8_t, 4>(guest_value, 4));
#if REX_PLATFORM_WIN32
  linger value{};
  value.l_onoff = static_cast<u_short>(parsed.on);
  value.l_linger = static_cast<u_short>(parsed.seconds);
#else
  linger value{};
  value.l_onoff = static_cast<int>(parsed.on);
  value.l_linger = static_cast<int>(parsed.seconds);
#endif
  int ret = setsockopt(native_handle_, SOL_SOCKET, SO_LINGER,
                       reinterpret_cast<const char*>(&value), sizeof(value));
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  switch (system_link::ClassifyGuestIoctl(cmd)) {
    case system_link::GuestSocketIoctl::kNonBlocking: {
      IoctlOutcome outcome = ApplyGuestNonBlocking(native_handle_, arg_ptr);
      if (outcome == IoctlOutcome::kNullArg) {
        SetHostError(kWsaEFault);
        return X_STATUS_UNSUCCESSFUL;
      }
      if (outcome == IoctlOutcome::kHostError) {
        CaptureHostError();
        return X_STATUS_UNSUCCESSFUL;
      }
      return X_STATUS_SUCCESS;
    }
    case system_link::GuestSocketIoctl::kBytesReadable: {
      IoctlOutcome outcome = QueryGuestAvailableBytes(native_handle_, arg_ptr);
      if (outcome == IoctlOutcome::kNullArg) {
        SetHostError(kWsaEFault);
        return X_STATUS_UNSUCCESSFUL;
      }
      if (outcome == IoctlOutcome::kHostError) {
        CaptureHostError();
        return X_STATUS_UNSUCCESSFUL;
      }
      return X_STATUS_SUCCESS;
    }
    case system_link::GuestSocketIoctl::kUnknown: {
      int ret = PassthroughGuestIoctl(native_handle_, cmd, arg_ptr);
      if (ret < 0) {
        CaptureHostError();
        return X_STATUS_UNSUCCESSFUL;
      }
      return X_STATUS_SUCCESS;
    }
  }
  int ret = PassthroughGuestIoctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  N_XSOCKADDR target = *name;
  if (target.address_family == AF_INET && name_len >= kMinInetSockaddrLen) {
    uint16_t guest_port = ReadInetSockaddrPort(target);
    uint16_t host_port = system_link::GuestToHostPort(guest_port, system_link::PortOffset());
    WriteInetSockaddrPort(&target, host_port);
    if (system_link::TraceEnabled() && host_port != guest_port) {
      REXLOG_INFO("[netlink] connect port {} -> {} (net_port_offset)", guest_port, host_port);
    }
  }
  int ret = connect(native_handle_, (sockaddr*)&target, name_len);
  if (ret < 0) {
#if REX_PLATFORM_WIN32
    CaptureHostError();
#else
    SetHostError(system_link::WsaErrorFromConnectErrno(errno));
#endif
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  uint32_t requested_ip = name->sin_addr;
  uint32_t effective_ip =
      system_link::RewriteBindAddress(requested_ip, system_link::BindIp().value_or(0));
  uint16_t guest_port = name->sin_port;
  uint16_t host_port = system_link::GuestToHostPort(guest_port, system_link::PortOffset());
  sockaddr_in addr = MakeHostSockaddr(effective_ip, host_port);
  addr.sin_family = name->sin_family;
  if (effective_ip != requested_ip) {
    LogBindRewrite(requested_ip, effective_ip, guest_port);
  }
  if (system_link::TraceEnabled() && host_port != guest_port) {
    REXLOG_INFO("[netlink] bind port {} -> {} (net_port_offset)", guest_port, host_port);
  }
  int ret = bind(native_handle_, reinterpret_cast<sockaddr*>(&addr),
                 std::min(name_len, static_cast<int>(sizeof(addr))));
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = guest_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr = {};
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == uintptr_t(-1)) {
    CaptureHostError();
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  if (n_sockaddr.sa_family == AF_INET) {
    uint16_t host_port = ReadGenericSockaddrPort(n_sockaddr);
    uint16_t guest_port = system_link::HostToGuestPort(host_port, system_link::PortOffset());
    WriteGenericSockaddrPort(&n_sockaddr, guest_port);
  }
  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  int ret = shutdown(native_handle_, how);
  if (ret < 0) {
    CaptureHostError();
  }
  return ret;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  int ret = recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
  if (ret < 0) {
    CaptureHostError();
  }
  return ret;
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  if (type_ == X_SOCK_DGRAM) {
    return RecvFromDatagram(buf, buf_len, flags, from, from_len);
  }
  return RecvFromStream(buf, buf_len, flags, from, from_len);
}

int XSocket::RecvFromDatagram(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                              N_XSOCKADDR_IN* from, uint32_t* from_len) {
  // The allow check runs before any byte reaches guest memory, and a datagram larger than the
  // guest buffer cannot fail the call before its source is known (WSAEMSGSIZE on Windows).
  std::vector<uint8_t>& scratch = HostDatagramScratch();
  sockaddr_in nfrom = {};
  for (int attempt = 0; attempt < kMaxDroppedDatagramsPerCall; ++attempt) {
    socklen_t nfromlen = sizeof(sockaddr_in);
    int ret = recvfrom(native_handle_, reinterpret_cast<char*>(scratch.data()),
                       static_cast<int>(scratch.size()), flags, (sockaddr*)&nfrom, &nfromlen);
    if (ret < 0) {
      CaptureHostError();
      return ret;
    }
    system_link::Endpoint source{ntohl(nfrom.sin_addr.s_addr), ntohs(nfrom.sin_port)};
    if (AcceptDatagram(source, scratch.data(), ret)) {
      CountTraffic(false, source.ip, source.port, ret);
      WriteGuestSource(from, from_len, nfrom, nfromlen);
      if (!CopyDatagramToGuest(scratch.data(), ret, buf, buf_len)) {
        // Winsock semantics: the truncated datagram is in the buffer and the call fails.
        SetHostError(kWsaEMsgSize);
        return -1;
      }
      return ret;
    }
    if ((flags & kGuestMsgPeek) != 0 && !DiscardPeekedDatagram(flags)) {
      return -1;
    }
  }
  SetHostError(kWsaEWouldBlock);
  return -1;
}

bool XSocket::DiscardPeekedDatagram(uint32_t flags) {
  std::vector<uint8_t>& discard = HostDatagramScratch();
  sockaddr_in nfrom = {};
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(discard.data()),
                     static_cast<int>(discard.size()), flags & ~kGuestMsgPeek, (sockaddr*)&nfrom,
                     &nfromlen);
  if (ret < 0) {
    CaptureHostError();
    return false;
  }
  return true;
}

int XSocket::RecvFromStream(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                            uint32_t* from_len) {
  // A stream's bytes are not datagrams: the allow list and the duplicate filter do not apply.
  sockaddr_in nfrom = {};
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (ret < 0) {
    CaptureHostError();
    return ret;
  }
  CountTraffic(false, ntohl(nfrom.sin_addr.s_addr), ntohs(nfrom.sin_port), ret);
  WriteGuestSource(from, from_len, nfrom, nfromlen);
  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  int ret = send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
  if (ret < 0) {
    CaptureHostError();
  }
  return ret;
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  if (!to) {
    int ret = sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags, nullptr, to_len);
    if (ret < 0) {
      CaptureHostError();
    }
    return ret;
  }
  uint32_t destination = to->sin_addr;
  uint16_t guest_port = to->sin_port;
  uint16_t host_port = system_link::GuestToHostPort(guest_port, system_link::PortOffset());
  LogSendtoRemap(guest_port, host_port);
  if (destination == system_link::kBroadcastIp) {
    return SendBroadcast(buf, buf_len, flags, host_port);
  }
  return SendCopy(buf, buf_len, flags, destination, host_port);
}

int XSocket::SendCopy(const uint8_t* buf, uint32_t buf_len, uint32_t flags, uint32_t ip,
                      uint16_t port) {
  sockaddr_in addr = MakeHostSockaddr(ip, port);
  uint64_t gate_token = system_link::BeaconGateToken();
  if (ShouldSendHelloBeforePayload(gate_token)) {
    SendBeaconHello(system_link::Endpoint{ip, port}, gate_token, flags, "co-op hello to");
  }
  int ret = sendto(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags,
                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (ret < 0) {
    CaptureHostError();
    return ret;
  }
  CountTraffic(true, ip, port, ret);
  LogFirstSend(native_handle_, system_link::Endpoint{ip, port}, ret);
  return ret;
}

std::vector<system_link::Endpoint> XSocket::TakeReplyCopyTargets(uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(peers_mutex_);
  auto latest = recent_sources_.Latest(now_ms, system_link::kMaxReplyCopiesPerBroadcast);
  return system_link::ReplyCopyTargets(bound_port_, latest, reply_budget_, now_ms);
}

int XSocket::SendBroadcast(const uint8_t* buf, uint32_t buf_len, uint32_t flags, uint16_t port) {
  std::vector<system_link::Endpoint> reply_to = TakeReplyCopyTargets(system_link::NowMs());
  auto lan_ips = system_link::LanBroadcastIps();
  auto targets = system_link::BroadcastTargets();
  auto peers = system_link::PeerEndpoints();
  auto self_ips = system_link::SelfIps();
  std::vector<system_link::Endpoint> endpoints =
      system_link::BroadcastFanOut(port, lan_ips, targets, peers, reply_to, self_ips);
  LogBroadcastFanOut(port, lan_ips, endpoints);
  // Every listed peer being this instance is a broadcast nobody else hears: still a success.
  int result = static_cast<int>(buf_len);
  bool any_sent = endpoints.empty();
  for (const auto& endpoint : endpoints) {
    int ret = SendCopy(buf, buf_len, flags, endpoint.ip, endpoint.port);
    if (ret >= 0) {
      any_sent = true;
      result = ret;
    }
  }
  return any_sent ? result : -1;
}

bool XSocket::ShouldSendHelloBeforePayload(uint64_t token) const {
  return bound_port_ == system_link::kBeaconGuestPort && token != 0;
}

void XSocket::SendBeaconHello(const system_link::Endpoint& destination, uint64_t token,
                              uint32_t flags, const char* what) {
  auto hello = system_link::EncodeBeaconHello(token);
  sockaddr_in addr = MakeHostSockaddr(destination.ip, destination.port);
  int ret = sendto(native_handle_, reinterpret_cast<const char*>(hello.data()),
                   static_cast<int>(hello.size()), flags, reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr));
  if (system_link::TraceEnabled()) {
    REXLOG_INFO("[netlink] {} {}: {}", what, system_link::FormatEndpoint(destination),
                ret < 0 ? "send failed" : "sent");
  }
}

void XSocket::LogBeaconHelloMismatchOnce(const system_link::Endpoint& source, uint64_t theirs,
                                         uint64_t ours) {
  if (!RememberOnce(beacon_mismatch_logs_, std::pair{source, theirs}, kBeaconLogCapacity)) {
    return;
  }
  REXLOG_INFO("[netlink] co-op hash from {} is {}, ours is {}: different build or co-op settings; "
              "its campaigns stay hidden",
              system_link::FormatEndpoint(source), system_link::FormatCoopHash(theirs),
              system_link::FormatCoopHash(ours));
}

void XSocket::LogBeaconDropOnce(const system_link::Endpoint& source,
                                system_link::BeaconVerdict verdict, uint64_t ours, uint64_t now) {
  if (verdict == system_link::BeaconVerdict::kNoHello) {
    if (!RememberOnce(beacon_drop_logs_, std::pair{source, uint64_t{0}}, kBeaconLogCapacity)) {
      return;
    }
    REXLOG_INFO("[netlink] beacon from {} dropped: no co-op hello (ours {})",
                system_link::FormatEndpoint(source), system_link::FormatCoopHash(ours));
    return;
  }
  uint64_t theirs = beacon_hellos_.TokenFrom(source, now).value();
  if (!RememberOnce(beacon_drop_logs_, std::pair{source, theirs}, kBeaconLogCapacity)) {
    return;
  }
  REXLOG_INFO("[netlink] beacon from {} dropped: co-op hash {} differs from ours {}",
              system_link::FormatEndpoint(source), system_link::FormatCoopHash(theirs),
              system_link::FormatCoopHash(ours));
}

XSocket::DatagramFate XSocket::JudgeAndRecord(const system_link::Endpoint& source,
                                               std::span<const uint8_t> datagram,
                                               uint64_t own_token, uint64_t now) {
  std::lock_guard<std::mutex> lock(peers_mutex_);
  system_link::BeaconVerdict verdict = system_link::JudgeBeaconDatagram(
      bound_port_, own_token, datagram, source, beacon_hellos_, now);
  if (verdict == system_link::BeaconVerdict::kHello) {
    auto theirs = system_link::ParseBeaconHello(datagram);
    if (own_token != 0 && theirs && *theirs != own_token) {
      LogBeaconHelloMismatchOnce(source, *theirs, own_token);
    }
    return DatagramFate::kDrop;
  }
  if (verdict == system_link::BeaconVerdict::kNoHello ||
      verdict == system_link::BeaconVerdict::kMismatch) {
    LogBeaconDropOnce(source, verdict, own_token, now);
    return hello_budget_.TryTake(source, now) ? DatagramFate::kDropAndAnswerHello
                                              : DatagramFate::kDrop;
  }
  if (duplicates_.IsDuplicate(source, datagram, now)) {
    if (system_link::TraceEnabled() && ShouldLogDroppedDatagram()) {
      REXLOG_INFO("[netlink] dropped a duplicate of {} bytes from {}", datagram.size(),
                  system_link::FormatEndpoint(source));
    }
    return DatagramFate::kDrop;
  }
  recent_sources_.Note(source, now);
  return DatagramFate::kAccept;
}

bool XSocket::AcceptDatagram(const system_link::Endpoint& source, const uint8_t* buf, int len) {
  auto allowed = system_link::AllowedSources();
  if (!system_link::IsAllowedSource(source.ip, allowed)) {
    if (system_link::TraceEnabled() && ShouldLogDroppedDatagram()) {
      REXLOG_INFO("[netlink] dropped {} bytes from {} (net_allow_peers)", len,
                  system_link::FormatEndpoint(source));
    }
    return false;
  }
  uint64_t now = system_link::NowMs();
  std::span<const uint8_t> datagram;
  if (buf != nullptr && len > 0) {
    datagram = std::span<const uint8_t>(buf, static_cast<size_t>(len));
  }
  uint64_t own_token = system_link::BeaconGateToken();
  DatagramFate fate = JudgeAndRecord(source, datagram, own_token, now);
  if (fate == DatagramFate::kDropAndAnswerHello) {
    SendBeaconHello(source, own_token, 0, "co-op hello reply to");
  }
  if (fate != DatagramFate::kAccept) {
    return false;
  }
  system_link::NoteHeardFrom(source);
  LogFirstReceive(native_handle_, source, len);
  return true;
}

X_STATUS XSocket::GetSockName(N_XSOCKADDR_IN* name, int* name_len) {
  if (name == nullptr || name_len == nullptr) {
    SetHostError(kWsaEFault);
    return X_STATUS_UNSUCCESSFUL;
  }
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  int ret = getsockname(native_handle_, reinterpret_cast<sockaddr*>(&addr), &len);
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }
  FillGuestSockaddrIn(name, addr);
  *name_len = kSockaddrInSize;
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetPeerName(N_XSOCKADDR_IN* name, int* name_len) {
  if (name == nullptr || name_len == nullptr) {
    SetHostError(kWsaEFault);
    return X_STATUS_UNSUCCESSFUL;
  }
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  int ret = getpeername(native_handle_, reinterpret_cast<sockaddr*>(&addr), &len);
  if (ret < 0) {
    CaptureHostError();
    return X_STATUS_UNSUCCESSFUL;
  }
  FillGuestSockaddrIn(name, addr);
  *name_len = kSockaddrInSize;
  return X_STATUS_SUCCESS;
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
