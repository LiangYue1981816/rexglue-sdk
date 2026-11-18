#include <rex/system/system_link.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex::system::system_link {
namespace {

constexpr uint32_t kAnyIp = 0;
constexpr int kErrnoSuccess = 0;
constexpr uint64_t kZeroXuid = 0;
constexpr uint64_t kInstallXuidFallbackLow = 1;
constexpr uint32_t kOctetMask = 0xFF;
constexpr int kBitsPerByte = 8;
constexpr int kOctet0Shift = 3 * kBitsPerByte;
constexpr int kOctet1Shift = 2 * kBitsPerByte;
constexpr int kOctet2Shift = 1 * kBitsPerByte;
constexpr int kOctet3Shift = 0;
constexpr int kHalfWordBits = 4 * kBitsPerByte;
constexpr size_t kIpv4FieldCount = 4;
constexpr size_t kIpv4MaxDigits = 3;
constexpr uint32_t kIpv4MaxFieldValue = 255;
constexpr uint32_t kDecimalBase = 10;
constexpr size_t kXuidHexWidth = 16;
constexpr size_t kHexPrefixLength = 2;
constexpr int kNibbleBits = 4;
constexpr uint64_t kNibbleMask = 0xF;
constexpr uint8_t kHexLetterOffset = 10;
constexpr char kUpperHexDigits[] = "0123456789ABCDEF";
constexpr char kLowerHexDigits[] = "0123456789abcdef";
constexpr uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kSplitMixMul1 = 0xBF58476D1CE4E5B9ull;
constexpr uint64_t kSplitMixMul2 = 0x94D049BB133111EBull;
constexpr int kSplitMixShift1 = 30;
constexpr int kSplitMixShift2 = 27;
constexpr int kSplitMixShift3 = 31;
constexpr uint8_t kLocallyAdministeredMac = 0x02;
constexpr size_t kIpByteCount = 4;
constexpr size_t kXnAddrPadAfterIp = 6;
constexpr size_t kXnAddrIpOffset = 0;
constexpr size_t kXnAddrMacOffset = kIpByteCount + kXnAddrPadAfterIp;
constexpr uint8_t kXnKidLowNibbleMask = 0x0F;
constexpr std::array<uint8_t, 8> kBeaconHelloMagic{'G', 'O', 'W', '2',
                                                    'H', 'E', 'L', 'O'};

constexpr uint32_t kWsaSuccess = 0;
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaEInProgress = 10036;
constexpr uint32_t kWsaEAlready = 10037;
constexpr uint32_t kWsaENotSock = 10038;
constexpr uint32_t kWsaEMsgSize = 10040;
constexpr uint32_t kWsaENoProtoOpt = 10042;
constexpr uint32_t kWsaEOpNotSupp = 10045;
constexpr uint32_t kWsaEAfNoSupport = 10047;
constexpr uint32_t kWsaEAddrInUse = 10048;
constexpr uint32_t kWsaEAddrNotAvail = 10049;
constexpr uint32_t kWsaENetDown = 10050;
constexpr uint32_t kWsaENetUnreach = 10051;
constexpr uint32_t kWsaEConnAborted = 10053;
constexpr uint32_t kWsaEConnReset = 10054;
constexpr uint32_t kWsaENoBufs = 10055;
constexpr uint32_t kWsaEIsConn = 10056;
constexpr uint32_t kWsaENotConn = 10057;
constexpr uint32_t kWsaETimedOut = 10060;
constexpr uint32_t kWsaEHostUnreach = 10065;
constexpr uint32_t kWsaEAcces = 10013;
constexpr uint32_t kWsaEFault = 10014;
constexpr uint32_t kWsaEInval = 10022;
constexpr uint32_t kWsaEMfile = 10024;

struct ErrnoWsaPair {
  int errno_value;
  uint32_t wsa_value;
};

constexpr ErrnoWsaPair kErrnoToWsaTable[] = {
    {EAGAIN, kWsaEWouldBlock},
    {EWOULDBLOCK, kWsaEWouldBlock},
    {EINPROGRESS, kWsaEInProgress},
    {EALREADY, kWsaEAlready},
    {ENOTSOCK, kWsaENotSock},
    {EBADF, kWsaENotSock},
    {EMSGSIZE, kWsaEMsgSize},
    {ENOPROTOOPT, kWsaENoProtoOpt},
    {EOPNOTSUPP, kWsaEOpNotSupp},
    {EAFNOSUPPORT, kWsaEAfNoSupport},
    {EADDRINUSE, kWsaEAddrInUse},
    {EADDRNOTAVAIL, kWsaEAddrNotAvail},
    {ENETDOWN, kWsaENetDown},
    {ENETUNREACH, kWsaENetUnreach},
    {ECONNABORTED, kWsaEConnAborted},
    {ECONNRESET, kWsaEConnReset},
    // A UDP port-unreachable is ECONNREFUSED on POSIX and 10054 on Windows.
    {ECONNREFUSED, kWsaEConnReset},
    {ENOBUFS, kWsaENoBufs},
    {EISCONN, kWsaEIsConn},
    {ENOTCONN, kWsaENotConn},
    {ETIMEDOUT, kWsaETimedOut},
    {EHOSTUNREACH, kWsaEHostUnreach},
    {EACCES, kWsaEAcces},
    {EFAULT, kWsaEFault},
    {EINVAL, kWsaEInval},
    {EMFILE, kWsaEMfile},
};

bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string_view TrimAsciiWhitespace(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && IsAsciiWhitespace(text[begin])) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && IsAsciiWhitespace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool IsIpv4ListDelimiter(char c) {
  return c == ',' || c == ';' || c == ' ' || c == '\t';
}

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

uint8_t HexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint8_t>(c - 'a' + kHexLetterOffset);
  }
  return static_cast<uint8_t>(c - 'A' + kHexLetterOffset);
}

std::optional<uint32_t> ParseIpv4Field(std::string_view token) {
  if (token.empty() || token.size() > kIpv4MaxDigits) {
    return std::nullopt;
  }
  uint32_t value = 0;
  for (char c : token) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * kDecimalBase + static_cast<uint32_t>(c - '0');
  }
  if (value > kIpv4MaxFieldValue) {
    return std::nullopt;
  }
  return value;
}

constexpr uint64_t kNoPortOffset = 0;
constexpr uint16_t kEmptyPort = 0;
constexpr uint64_t kInclusiveRangeAdjustment = 1;
constexpr uint64_t kUsableOffsetSpan =
    static_cast<uint64_t>(kPrivilegedPortLimit) - kInclusiveRangeAdjustment;
constexpr uint64_t kMaxUsablePortOffset = static_cast<uint64_t>(kMaxPort) - kUsableOffsetSpan;
constexpr size_t kZeroCapacity = 0;

constexpr size_t kBe32Byte0 = 0;
constexpr size_t kBe32Byte1 = 1;
constexpr size_t kBe32Byte2 = 2;
constexpr size_t kBe32Byte3 = 3;
constexpr size_t kPortHighByteIndex = 0;
constexpr size_t kPortLowByteIndex = 1;
constexpr int kPortHighByteShift = kBitsPerByte;
constexpr int kPortLowByteShift = 0;

uint32_t LoadBigEndian32(const uint8_t* src) {
  uint32_t high = (static_cast<uint32_t>(src[kBe32Byte0]) << kOctet0Shift) |
                  (static_cast<uint32_t>(src[kBe32Byte1]) << kOctet1Shift);
  uint32_t low = (static_cast<uint32_t>(src[kBe32Byte2]) << kOctet2Shift) |
                 (static_cast<uint32_t>(src[kBe32Byte3]) << kOctet3Shift);
  return high | low;
}

void StoreBigEndian32(uint8_t* dest, uint32_t value) {
  dest[kBe32Byte0] = static_cast<uint8_t>((value >> kOctet0Shift) & kOctetMask);
  dest[kBe32Byte1] = static_cast<uint8_t>((value >> kOctet1Shift) & kOctetMask);
  dest[kBe32Byte2] = static_cast<uint8_t>((value >> kOctet2Shift) & kOctetMask);
  dest[kBe32Byte3] = static_cast<uint8_t>((value >> kOctet3Shift) & kOctetMask);
}

void StoreBigEndian16(uint8_t* dest, uint16_t value) {
  dest[kPortHighByteIndex] =
      static_cast<uint8_t>((value >> kPortHighByteShift) & kOctetMask);
  dest[kPortLowByteIndex] = static_cast<uint8_t>((value >> kPortLowByteShift) & kOctetMask);
}

uint16_t ReadBigEndian16(uint8_t high, uint8_t low) {
  return static_cast<uint16_t>((static_cast<uint16_t>(high) << kBitsPerByte) | low);
}

constexpr char kCidrSeparator = '/';
constexpr size_t kCidrMinPrefixDigits = 1;
constexpr size_t kCidrMaxPrefixDigits = 2;
constexpr int kCidrMinPrefixValue = 0;
constexpr int kCidrMaxPrefixValue = 32;

bool IsDecimalDigit(char c) {
  return c >= '0' && c <= '9';
}

std::optional<int> ParseCidrPrefixLength(std::string_view text) {
  if (text.size() < kCidrMinPrefixDigits || text.size() > kCidrMaxPrefixDigits) {
    return std::nullopt;
  }
  for (char c : text) {
    if (!IsDecimalDigit(c)) {
      return std::nullopt;
    }
  }
  int value = 0;
  for (char c : text) {
    value = value * static_cast<int>(kDecimalBase) + (c - '0');
  }
  if (value < kCidrMinPrefixValue || value > kCidrMaxPrefixValue) {
    return std::nullopt;
  }
  return value;
}

constexpr int kEmptyPrefixLength = 0;
constexpr int kIpv4BitWidth = 32;
constexpr uint32_t kEmptyPrefixMask = 0;
constexpr uint32_t kFullPrefixMask = 0xFFFFFFFF;
constexpr int kMinDirectedPrefixLength = 1;
constexpr int kMaxDirectedPrefixLength = 30;

bool HasDirectedBroadcastPrefix(const Subnet& subnet) {
  return subnet.prefix_length >= kMinDirectedPrefixLength &&
         subnet.prefix_length <= kMaxDirectedPrefixLength;
}

bool CanUseDirectedBroadcast(const Subnet& subnet) {
  if (subnet.ip == kAnyIp) {
    return false;
  }
  if (IsLoopbackIp(subnet.ip)) {
    return false;
  }
  return HasDirectedBroadcastPrefix(subnet);
}

constexpr char kHostPortSeparator = ':';
constexpr size_t kMinPortDigits = 1;
constexpr size_t kMaxPortDigits = 5;
constexpr uint32_t kMinPortValue = 1;

bool IsHostNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '.' || c == '-';
}

bool IsValidHostName(std::string_view host) {
  if (host.empty()) {
    return false;
  }
  for (char c : host) {
    if (!IsHostNameChar(c)) {
      return false;
    }
  }
  return true;
}

std::optional<uint16_t> ParsePortNumber(std::string_view text) {
  if (text.size() < kMinPortDigits || text.size() > kMaxPortDigits) {
    return std::nullopt;
  }
  for (char c : text) {
    if (!IsDecimalDigit(c)) {
      return std::nullopt;
    }
  }
  uint32_t value = 0;
  for (char c : text) {
    value = value * kDecimalBase + static_cast<uint32_t>(c - '0');
  }
  if (value < kMinPortValue || value > kMaxPort) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(value);
}

bool ContainsIp(std::span<const uint32_t> ips, uint32_t ip) {
  return std::find(ips.begin(), ips.end(), ip) != ips.end();
}

bool IsExcludedFanOutIp(uint32_t ip, std::span<const uint32_t> self_ips) {
  if (ip == kAnyIp) {
    return true;
  }
  if (ip == kBroadcastIp) {
    return true;
  }
  return ContainsIp(self_ips, ip);
}

void AppendUniqueEndpoint(std::vector<Endpoint>& out, const Endpoint& endpoint) {
  if (std::find(out.begin(), out.end(), endpoint) == out.end()) {
    out.push_back(endpoint);
  }
}

uint16_t EffectivePeerPort(const Endpoint& peer, uint16_t host_port) {
  return peer.port != kEmptyPort ? peer.port : host_port;
}

void AppendLanEndpoints(std::vector<Endpoint>& out, uint16_t host_port,
                        std::span<const uint32_t> lan_broadcast_ips,
                        std::span<const uint32_t> broadcast_targets,
                        std::span<const uint32_t> self_ips) {
  if (broadcast_targets.empty()) {
    for (uint32_t ip : lan_broadcast_ips) {
      AppendUniqueEndpoint(out, Endpoint{ip, host_port});
    }
    return;
  }
  for (uint32_t target : broadcast_targets) {
    if (IsExcludedFanOutIp(target, self_ips)) {
      continue;
    }
    AppendUniqueEndpoint(out, Endpoint{target, host_port});
  }
}

void AppendPeerEndpoints(std::vector<Endpoint>& out, uint16_t host_port,
                         std::span<const Endpoint> peers,
                         std::span<const uint32_t> self_ips) {
  for (const Endpoint& peer : peers) {
    if (IsExcludedFanOutIp(peer.ip, self_ips)) {
      continue;
    }
    AppendUniqueEndpoint(out, Endpoint{peer.ip, EffectivePeerPort(peer, host_port)});
  }
}

void AppendRecentEndpoints(std::vector<Endpoint>& out,
                           std::span<const Endpoint> recent_sources,
                           std::span<const uint32_t> self_ips) {
  for (const Endpoint& source : recent_sources) {
    if (source.port == kEmptyPort) {
      continue;
    }
    if (IsExcludedFanOutIp(source.ip, self_ips)) {
      continue;
    }
    AppendUniqueEndpoint(out, source);
  }
}

bool IsLocalPeerAddress(uint32_t peer_ina, uint32_t peer_ina_online,
                        const Subnet& local_subnet, bool heard_from_ina) {
  if (IsLoopbackIp(peer_ina)) {
    return true;
  }
  if (!IsInSubnet(peer_ina, local_subnet)) {
    return false;
  }
  return peer_ina_online == kAnyIp || heard_from_ina;
}

void AppendUniqueSubnet(std::vector<Subnet>& out, const Subnet& subnet) {
  if (std::find(out.begin(), out.end(), subnet) == out.end()) {
    out.push_back(subnet);
  }
}

std::vector<Subnet> LocalSubnetPlus(const Subnet& local_subnet, std::span<const Subnet> extra) {
  std::vector<Subnet> result{local_subnet};
  for (const Subnet& subnet : extra) {
    AppendUniqueSubnet(result, subnet);
  }
  return result;
}

void AppendIfHostName(std::vector<std::string>& names, const std::string& host) {
  if (host.empty() || ParseIpv4(host).has_value()) {
    return;
  }
  names.push_back(host);
}

std::string AllowEntryHost(const std::string& entry) {
  if (ParseIpv4Cidr(entry, kHostPrefixLength).has_value()) {
    return {};
  }
  std::optional<HostPort> host_port = ParseHostPort(entry);
  return host_port.has_value() ? host_port->host : std::string();
}

std::vector<uint32_t> KnownAddresses(const NameTable& names, const std::string& name) {
  auto found = names.find(name);
  if (found == names.end()) {
    return {};
  }
  return found->second;
}

void RecordLookup(NameRefresh& refresh, const std::string& name, std::vector<uint32_t> fresh,
                  std::vector<uint32_t> previous) {
  if (!fresh.empty()) {
    if (fresh != previous) {
      refresh.changed.push_back(name);
    }
    refresh.table[name] = std::move(fresh);
    return;
  }
  if (!previous.empty()) {
    refresh.stale.push_back(name);
    refresh.table[name] = std::move(previous);
    return;
  }
  refresh.missing.push_back(name);
}

std::vector<Subnet> AllowEntrySubnets(const std::string& entry, const NameTable& names) {
  if (std::optional<Subnet> range = ParseIpv4Cidr(entry, kHostPrefixLength); range.has_value()) {
    return {*range};
  }
  std::optional<HostPort> host_port = ParseHostPort(entry);
  if (!host_port.has_value()) {
    return {};
  }
  std::vector<Subnet> result;
  for (uint32_t ip : HostAddresses(host_port->host, names)) {
    result.push_back(Subnet{ip, kHostPrefixLength});
  }
  return result;
}

constexpr uint64_t kMillisecondsPerSecond = 1000;

constexpr uint32_t kGuestSolSocket = 0xFFFF;
constexpr uint32_t kGuestSoReuseAddr = 0x0004;
constexpr uint32_t kGuestSoKeepAlive = 0x0008;
constexpr uint32_t kGuestSoBroadcast = 0x0020;
constexpr uint32_t kGuestSoOobInline = 0x0100;
constexpr uint32_t kGuestSoLinger = 0x0080;
constexpr uint32_t kGuestSoSndBuf = 0x1001;
constexpr uint32_t kGuestSoRcvBuf = 0x1002;
constexpr uint32_t kGuestSoError = 0x1007;
constexpr uint32_t kGuestSoType = 0x1008;

struct GuestSocketOptionEntry {
  uint32_t optname;
  GuestSocketOption option;
};

constexpr GuestSocketOptionEntry kGuestSocketOptionTable[] = {
    {kGuestSoReuseAddr, GuestSocketOption::kReuseAddr},
    {kGuestSoKeepAlive, GuestSocketOption::kKeepAlive},
    {kGuestSoBroadcast, GuestSocketOption::kBroadcast},
    {kGuestSoOobInline, GuestSocketOption::kOobInline},
    {kGuestSoLinger, GuestSocketOption::kLinger},
    {kGuestSoSndBuf, GuestSocketOption::kSendBuffer},
    {kGuestSoRcvBuf, GuestSocketOption::kReceiveBuffer},
    {kGuestSoError, GuestSocketOption::kError},
    {kGuestSoType, GuestSocketOption::kType},
};

constexpr uint32_t kGuestFionBio = 0x8004667E;
constexpr uint32_t kGuestFionRead = 0x4004667F;

constexpr size_t kLingerOnHighIndex = 0;
constexpr size_t kLingerOnLowIndex = 1;
constexpr size_t kLingerSecondsHighIndex = 2;
constexpr size_t kLingerSecondsLowIndex = 3;

constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

}  // namespace

std::optional<uint32_t> ParseIpv4(std::string_view text) {
  std::string_view trimmed = TrimAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  std::array<uint32_t, kIpv4FieldCount> octets{};
  size_t pos = 0;
  for (size_t field = 0; field < kIpv4FieldCount; ++field) {
    size_t dot = trimmed.find('.', pos);
    std::string_view token;
    if (field + 1 == kIpv4FieldCount) {
      if (dot != std::string_view::npos) {
        return std::nullopt;
      }
      token = trimmed.substr(pos);
    } else {
      if (dot == std::string_view::npos) {
        return std::nullopt;
      }
      token = trimmed.substr(pos, dot - pos);
      pos = dot + 1;
    }
    std::optional<uint32_t> value = ParseIpv4Field(token);
    if (!value.has_value()) {
      return std::nullopt;
    }
    octets[field] = *value;
  }
  uint32_t high = (octets[0] << kOctet0Shift) | (octets[1] << kOctet1Shift);
  uint32_t low = (octets[2] << kOctet2Shift) | (octets[3] << kOctet3Shift);
  return high | low;
}

std::vector<uint32_t> ParseIpv4List(std::string_view text) {
  std::vector<uint32_t> result;
  size_t token_start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    bool is_end = i == text.size();
    bool is_delim = !is_end && IsIpv4ListDelimiter(text[i]);
    if (!is_end && !is_delim) {
      continue;
    }
    if (i > token_start) {
      std::string_view token = text.substr(token_start, i - token_start);
      if (std::optional<uint32_t> ip = ParseIpv4(token); ip.has_value()) {
        result.push_back(*ip);
      }
    }
    token_start = i + 1;
  }
  return result;
}

std::string FormatIpv4(uint32_t ip) {
  uint32_t a = (ip >> kOctet0Shift) & kOctetMask;
  uint32_t b = (ip >> kOctet1Shift) & kOctetMask;
  uint32_t c = (ip >> kOctet2Shift) & kOctetMask;
  uint32_t d = (ip >> kOctet3Shift) & kOctetMask;
  std::string result = std::to_string(a);
  result += '.';
  result += std::to_string(b);
  result += '.';
  result += std::to_string(c);
  result += '.';
  result += std::to_string(d);
  return result;
}

std::optional<uint64_t> ParseXuid(std::string_view text) {
  std::string_view trimmed = TrimAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (trimmed.size() >= kHexPrefixLength && trimmed[0] == '0' &&
      (trimmed[1] == 'x' || trimmed[1] == 'X')) {
    trimmed = trimmed.substr(kHexPrefixLength);
    if (trimmed.empty()) {
      return std::nullopt;
    }
  }
  if (trimmed.size() > kXuidHexWidth) {
    return std::nullopt;
  }
  for (char c : trimmed) {
    if (!IsHexDigit(c)) {
      return std::nullopt;
    }
  }
  uint64_t value = 0;
  for (char c : trimmed) {
    value = (value << kNibbleBits) | HexDigitValue(c);
  }
  if (!IsUsableXuid(value)) {
    return std::nullopt;
  }
  return value;
}

bool IsUsableXuid(uint64_t xuid) {
  return xuid != kZeroXuid && (xuid & kXuidForbiddenBits) == 0;
}

uint64_t MakeInstallXuid(uint64_t random_bits) {
  uint64_t low = random_bits & kInstallXuidRandomMask;
  if (low == kZeroXuid) {
    low = kInstallXuidFallbackLow;
  }
  return kInstallXuidPrefix | low;
}

std::string FormatXuid(uint64_t xuid) {
  std::string result(kXuidHexWidth, '0');
  for (size_t i = 0; i < kXuidHexWidth; ++i) {
    size_t shift = (kXuidHexWidth - 1 - i) * static_cast<size_t>(kNibbleBits);
    uint8_t nibble = static_cast<uint8_t>((xuid >> shift) & kNibbleMask);
    result[i] = kUpperHexDigits[nibble];
  }
  return result;
}

std::string FormatCoopHash(uint64_t hash) {
  std::string result(kXuidHexWidth, '0');
  for (size_t i = 0; i < kXuidHexWidth; ++i) {
    size_t shift = (kXuidHexWidth - 1 - i) * static_cast<size_t>(kNibbleBits);
    uint8_t nibble = static_cast<uint8_t>((hash >> shift) & kNibbleMask);
    result[i] = kLowerHexDigits[nibble];
  }
  return result;
}

bool IsLanAddress(uint32_t ip) {
  constexpr uint32_t kLoopbackNet = 0x7F000000;
  constexpr uint32_t kLoopbackMask = 0xFF000000;
  constexpr uint32_t kMulticastNet = 0xE0000000;
  constexpr uint32_t kMulticastMask = 0xF0000000;
  return ip != kAnyIp && ip != kBroadcastIp && (ip & kLoopbackMask) != kLoopbackNet &&
         (ip & kMulticastMask) != kMulticastNet;
}

uint32_t ResolveLocalIp(std::optional<uint32_t> local_ip, std::optional<uint32_t> bind_ip,
                        std::optional<uint32_t> detected_ip) {
  if (local_ip.has_value()) {
    return *local_ip;
  }
  if (bind_ip.has_value()) {
    return *bind_ip;
  }
  if (detected_ip.has_value() && IsLanAddress(*detected_ip)) {
    return *detected_ip;
  }
  return kLoopbackIp;
}

XuidSource ChooseXuidSource(bool xuid_setting_usable, bool link_active) {
  if (xuid_setting_usable) {
    return XuidSource::kSetting;
  }
  return link_active ? XuidSource::kInstall : XuidSource::kOffline;
}

uint64_t SplitMix64(uint64_t& state) {
  state += kSplitMixIncrement;
  uint64_t z = state;
  z = (z ^ (z >> kSplitMixShift1)) * kSplitMixMul1;
  z = (z ^ (z >> kSplitMixShift2)) * kSplitMixMul2;
  return z ^ (z >> kSplitMixShift3);
}

MacAddress MakeInstanceMac(uint64_t xuid, uint32_t local_ip) {
  uint64_t ip_wide = static_cast<uint64_t>(local_ip);
  uint64_t state = xuid ^ (ip_wide << kHalfWordBits) ^ ip_wide;
  uint64_t hash = SplitMix64(state);
  MacAddress mac{};
  mac[0] = kLocallyAdministeredMac;
  mac[1] = static_cast<uint8_t>(hash >> kHalfWordBits);
  mac[2] = static_cast<uint8_t>(hash >> kOctet0Shift);
  mac[3] = static_cast<uint8_t>(hash >> kOctet1Shift);
  mac[4] = static_cast<uint8_t>(hash >> kOctet2Shift);
  mac[5] = static_cast<uint8_t>(hash >> kOctet3Shift);
  return mac;
}

XnAddrBytes BuildXnAddr(uint32_t local_ip, const MacAddress& mac, uint32_t online_ip,
                        uint16_t online_port) {
  XnAddrBytes out{};
  StoreBigEndian32(out.data() + kXnAddrIpOffset, local_ip);
  StoreBigEndian32(out.data() + kXnAddrInaOnlineOffset, online_ip);
  StoreBigEndian16(out.data() + kXnAddrPortOnlineOffset, online_port);
  for (size_t i = 0; i < kMacSize; ++i) {
    out[kXnAddrMacOffset + i] = mac[i];
  }
  return out;
}

void MarkSystemLinkXnKid(std::span<uint8_t, kXnKidSize> xnkid) {
  xnkid[0] &= kXnKidLowNibbleMask;
}

uint32_t RewriteBindAddress(uint32_t requested_ip, uint32_t bind_ip) {
  return (requested_ip == kAnyIp && bind_ip != kAnyIp) ? bind_ip : requested_ip;
}

std::vector<uint32_t> SendDestinations(uint32_t destination_ip,
                                       std::span<const uint32_t> broadcast_targets,
                                       uint32_t self_ip) {
  if (destination_ip != kBroadcastIp || broadcast_targets.empty()) {
    return std::vector<uint32_t>{destination_ip};
  }
  std::vector<uint32_t> result;
  result.reserve(broadcast_targets.size());
  for (uint32_t target : broadcast_targets) {
    if (target == self_ip || target == kAnyIp || target == kBroadcastIp) {
      continue;
    }
    if (std::find(result.begin(), result.end(), target) != result.end()) {
      continue;
    }
    result.push_back(target);
  }
  return result;
}

uint32_t WsaErrorFromErrno(int error) {
  if (error == kErrnoSuccess) {
    return kWsaSuccess;
  }
  for (const ErrnoWsaPair& entry : kErrnoToWsaTable) {
    if (entry.errno_value == error) {
      return entry.wsa_value;
    }
  }
  return kWsaEInval;
}

uint32_t WsaErrorFromConnectErrno(int error) {
  // Winsock reports a non-blocking connect as WSAEWOULDBLOCK.
  if (error == EINPROGRESS) {
    return kWsaEWouldBlock;
  }
  return WsaErrorFromErrno(error);
}

bool IsUsablePortOffset(uint64_t offset) {
  if (offset == kNoPortOffset) {
    return true;
  }
  return offset >= kPrivilegedPortLimit && offset <= kMaxUsablePortOffset;
}

uint16_t GuestToHostPort(uint16_t guest_port, uint32_t offset) {
  if (offset == kNoPortOffset) {
    return guest_port;
  }
  if (guest_port == kEmptyPort) {
    return guest_port;
  }
  if (static_cast<uint32_t>(guest_port) >= kPrivilegedPortLimit) {
    return guest_port;
  }
  uint64_t mapped = static_cast<uint64_t>(guest_port) + offset;
  if (mapped > kMaxPort) {
    return guest_port;
  }
  return static_cast<uint16_t>(mapped);
}

uint16_t HostToGuestPort(uint16_t host_port, uint32_t offset) {
  if (offset == kNoPortOffset) {
    return host_port;
  }
  uint32_t host = host_port;
  if (host <= offset) {
    return host_port;
  }
  uint64_t upper = static_cast<uint64_t>(offset) + kPrivilegedPortLimit;
  if (static_cast<uint64_t>(host) >= upper) {
    return host_port;
  }
  return static_cast<uint16_t>(host - offset);
}

std::vector<std::string_view> SplitListTokens(std::string_view text) {
  std::vector<std::string_view> result;
  size_t token_start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    bool is_end = i == text.size();
    bool is_delim = !is_end && IsIpv4ListDelimiter(text[i]);
    if (!is_end && !is_delim) {
      continue;
    }
    if (i > token_start) {
      result.push_back(text.substr(token_start, i - token_start));
    }
    token_start = i + 1;
  }
  return result;
}

std::optional<Subnet> ParseIpv4Cidr(std::string_view text, int default_prefix_length) {
  std::string_view trimmed = TrimAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  size_t slash = trimmed.find(kCidrSeparator);
  if (slash == std::string_view::npos) {
    std::optional<uint32_t> ip = ParseIpv4(trimmed);
    if (!ip.has_value()) {
      return std::nullopt;
    }
    return Subnet{*ip, default_prefix_length};
  }
  if (trimmed.find(kCidrSeparator, slash + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  std::optional<uint32_t> ip = ParseIpv4(trimmed.substr(0, slash));
  if (!ip.has_value()) {
    return std::nullopt;
  }
  std::optional<int> prefix = ParseCidrPrefixLength(trimmed.substr(slash + 1));
  if (!prefix.has_value()) {
    return std::nullopt;
  }
  return Subnet{*ip, *prefix};
}

std::string FormatSubnet(const Subnet& subnet) {
  std::string result = FormatIpv4(subnet.ip);
  result += kCidrSeparator;
  result += std::to_string(subnet.prefix_length);
  return result;
}

uint32_t PrefixMask(int prefix_length) {
  if (prefix_length <= kEmptyPrefixLength) {
    return kEmptyPrefixMask;
  }
  if (prefix_length >= kIpv4BitWidth) {
    return kFullPrefixMask;
  }
  return kFullPrefixMask << (kIpv4BitWidth - prefix_length);
}

bool IsInSubnet(uint32_t ip, const Subnet& subnet) {
  uint32_t mask = PrefixMask(subnet.prefix_length);
  return (ip & mask) == (subnet.ip & mask);
}

bool IsLoopbackIp(uint32_t ip) {
  return IsInSubnet(ip, Subnet{kLoopbackNetwork, kLoopbackPrefixLength});
}

uint32_t LanBroadcastIp(const Subnet& local_subnet, bool prefer_directed) {
  if (!prefer_directed) {
    return kBroadcastIp;
  }
  if (!CanUseDirectedBroadcast(local_subnet)) {
    return kBroadcastIp;
  }
  return local_subnet.ip | ~PrefixMask(local_subnet.prefix_length);
}

std::vector<uint32_t> LanBroadcastIps(const Subnet& local_subnet, bool prefer_directed) {
  if (!CanUseDirectedBroadcast(local_subnet)) {
    return {kBroadcastIp};
  }
  uint32_t directed = LanBroadcastIp(local_subnet, true);
  if (prefer_directed) {
    return {directed};
  }
  // Windows sends 255.255.255.255 out of one adapter only, which may not be the LAN one.
  return {directed, kBroadcastIp};
}

Subnet ResolveLocalSubnet(std::optional<Subnet> configured, std::optional<Subnet> environment,
                          std::optional<uint32_t> bind_ip,
                          std::optional<uint32_t> detected_ip) {
  if (configured.has_value()) {
    return *configured;
  }
  if (environment.has_value()) {
    return *environment;
  }
  uint32_t ip = ResolveLocalIp(std::nullopt, bind_ip, detected_ip);
  if (!bind_ip.has_value() && ip == kLoopbackIp) {
    return Subnet{kLoopbackIp, kLoopbackPrefixLength};
  }
  return Subnet{ip, kDefaultLocalPrefixLength};
}

std::optional<HostPort> ParseHostPort(std::string_view text) {
  std::string_view trimmed = TrimAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  size_t colon = trimmed.find(kHostPortSeparator);
  if (colon == std::string_view::npos) {
    if (!IsValidHostName(trimmed)) {
      return std::nullopt;
    }
    return HostPort{std::string(trimmed), kEmptyPort};
  }
  if (trimmed.find(kHostPortSeparator, colon + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view host = trimmed.substr(0, colon);
  if (!IsValidHostName(host)) {
    return std::nullopt;
  }
  std::optional<uint16_t> port = ParsePortNumber(trimmed.substr(colon + 1));
  if (!port.has_value()) {
    return std::nullopt;
  }
  return HostPort{std::string(host), *port};
}

std::vector<HostPort> ParseHostPortList(std::string_view text) {
  std::vector<HostPort> result;
  for (std::string_view token : SplitListTokens(text)) {
    if (std::optional<HostPort> parsed = ParseHostPort(token); parsed.has_value()) {
      result.push_back(*parsed);
    }
  }
  return result;
}

std::string FormatEndpoint(const Endpoint& endpoint) {
  std::string result = FormatIpv4(endpoint.ip);
  result += kHostPortSeparator;
  result += std::to_string(endpoint.port);
  return result;
}

bool IsAllowedSource(uint32_t source_ip, std::span<const Subnet> allowed) {
  if (allowed.empty()) {
    return true;
  }
  if (IsLoopbackIp(source_ip)) {
    return true;
  }
  for (const Subnet& subnet : allowed) {
    if (IsInSubnet(source_ip, subnet)) {
      return true;
    }
  }
  return false;
}

std::vector<Subnet> DefaultAllowList(std::span<const uint32_t> peer_ips,
                                     const Subnet& local_subnet) {
  std::vector<Subnet> result;
  result.push_back(local_subnet);
  for (uint32_t ip : peer_ips) {
    if (ip == kAnyIp) {
      continue;
    }
    Subnet entry{ip, kHostPrefixLength};
    if (std::find(result.begin(), result.end(), entry) != result.end()) {
      continue;
    }
    result.push_back(entry);
  }
  return result;
}

std::vector<Subnet> EffectiveAllowList(const AllowPolicyInput& input) {
  if (input.allow_list_set) {
    return LocalSubnetPlus(input.local_subnet, input.allow_list);
  }
  if (input.public_address_set || input.peers_set) {
    return DefaultAllowList(input.peer_ips, input.local_subnet);
  }
  return {};
}

std::vector<Endpoint> BroadcastFanOut(uint16_t host_port,
                                      std::span<const uint32_t> lan_broadcast_ips,
                                      std::span<const uint32_t> broadcast_targets,
                                      std::span<const Endpoint> peers,
                                      std::span<const Endpoint> recent_sources,
                                      std::span<const uint32_t> self_ips) {
  std::vector<Endpoint> result;
  AppendLanEndpoints(result, host_port, lan_broadcast_ips, broadcast_targets, self_ips);
  AppendPeerEndpoints(result, host_port, peers, self_ips);
  AppendRecentEndpoints(result, recent_sources, self_ips);
  return result;
}

uint32_t PeerInAddr(uint32_t peer_ina, uint32_t peer_ina_online, const Subnet& local_subnet,
                    bool heard_from_ina) {
  if (IsLocalPeerAddress(peer_ina, peer_ina_online, local_subnet, heard_from_ina)) {
    return peer_ina;
  }
  if (peer_ina_online != kAnyIp) {
    return peer_ina_online;
  }
  return peer_ina;
}

std::vector<Endpoint> ReplyCopyTargets(uint16_t socket_guest_port,
                                       std::span<const Endpoint> latest_sources,
                                       ReplyBudget& budget, uint64_t now_ms) {
  std::vector<Endpoint> result;
  if (socket_guest_port != kBeaconGuestPort) {
    return result;
  }
  for (const Endpoint& source : latest_sources) {
    if (source.port == kEmptyPort) {
      continue;
    }
    if (budget.TryTake(source, now_ms)) {
      result.push_back(source);
    }
  }
  return result;
}

std::vector<std::string> NamesToResolve(std::span<const HostPort> peers,
                                        std::span<const std::string> allow_entries,
                                        const std::optional<HostPort>& public_host) {
  std::vector<std::string> names;
  for (const HostPort& peer : peers) {
    AppendIfHostName(names, peer.host);
  }
  for (const std::string& entry : allow_entries) {
    AppendIfHostName(names, AllowEntryHost(entry));
  }
  if (public_host.has_value()) {
    AppendIfHostName(names, public_host->host);
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

NameRefresh RefreshNames(const NameTable& previous, std::span<const std::string> names,
                         const HostLookup& lookup) {
  NameRefresh refresh;
  for (const std::string& name : names) {
    RecordLookup(refresh, name, lookup(name), KnownAddresses(previous, name));
  }
  return refresh;
}

std::vector<uint32_t> HostAddresses(const std::string& host, const NameTable& names) {
  if (std::optional<uint32_t> ip = ParseIpv4(host); ip.has_value()) {
    return {*ip};
  }
  return KnownAddresses(names, host);
}

std::vector<Endpoint> PeerEndpointsFrom(std::span<const HostPort> peers, const NameTable& names) {
  std::vector<Endpoint> result;
  for (const HostPort& peer : peers) {
    for (uint32_t ip : HostAddresses(peer.host, names)) {
      AppendUniqueEndpoint(result, Endpoint{ip, peer.port});
    }
  }
  return result;
}

std::vector<Subnet> AllowListFrom(std::span<const std::string> allow_entries,
                                  const NameTable& names) {
  std::vector<Subnet> result;
  for (const std::string& entry : allow_entries) {
    for (const Subnet& subnet : AllowEntrySubnets(entry, names)) {
      AppendUniqueSubnet(result, subnet);
    }
  }
  return result;
}

std::optional<Endpoint> PublicEndpointFrom(const std::optional<HostPort>& public_host,
                                           const NameTable& names) {
  if (!public_host.has_value()) {
    return std::nullopt;
  }
  std::vector<uint32_t> ips = HostAddresses(public_host->host, names);
  if (ips.empty()) {
    return std::nullopt;
  }
  return Endpoint{ips.front(), public_host->port};
}

GuestSocketOption ClassifyGuestSocketOption(uint32_t level, uint32_t optname) {
  if (level != kGuestSolSocket) {
    return GuestSocketOption::kUnknown;
  }
  for (const GuestSocketOptionEntry& entry : kGuestSocketOptionTable) {
    if (entry.optname == optname) {
      return entry.option;
    }
  }
  return GuestSocketOption::kUnknown;
}

GuestSocketIoctl ClassifyGuestIoctl(uint32_t cmd) {
  if (cmd == kGuestFionBio) {
    return GuestSocketIoctl::kNonBlocking;
  }
  if (cmd == kGuestFionRead) {
    return GuestSocketIoctl::kBytesReadable;
  }
  return GuestSocketIoctl::kUnknown;
}

LingerOption ReadGuestLinger(std::span<const uint8_t, 4> big_endian_value) {
  uint16_t on = ReadBigEndian16(big_endian_value[kLingerOnHighIndex],
                                big_endian_value[kLingerOnLowIndex]);
  uint16_t seconds = ReadBigEndian16(big_endian_value[kLingerSecondsHighIndex],
                                     big_endian_value[kLingerSecondsLowIndex]);
  return LingerOption{on, seconds};
}

uint32_t ReadBigEndian32(std::span<const uint8_t, 4> bytes) {
  return LoadBigEndian32(bytes.data());
}

void WriteBigEndian32(std::span<uint8_t, 4> bytes, uint32_t value) {
  StoreBigEndian32(bytes.data(), value);
}

uint64_t Fnv1a64(std::span<const uint8_t> bytes) {
  uint64_t hash = kFnvOffsetBasis;
  for (uint8_t b : bytes) {
    hash ^= b;
    hash *= kFnvPrime;
  }
  return hash;
}

std::array<uint8_t, kBeaconHelloSize> EncodeBeaconHello(uint64_t token) {
  std::array<uint8_t, kBeaconHelloSize> datagram{};
  std::copy(kBeaconHelloMagic.begin(), kBeaconHelloMagic.end(), datagram.begin());
  datagram[kBeaconHelloMagic.size()] = kBeaconHelloVersion;
  for (size_t i = 0; i < sizeof(token); ++i) {
    size_t shift = (sizeof(token) - 1 - i) * kBitsPerByte;
    datagram[kBeaconHelloMagic.size() + 1 + i] = static_cast<uint8_t>(token >> shift);
  }
  return datagram;
}

std::optional<uint64_t> ParseBeaconHello(std::span<const uint8_t> datagram) {
  if (datagram.size() != kBeaconHelloSize ||
      !std::equal(kBeaconHelloMagic.begin(), kBeaconHelloMagic.end(), datagram.begin()) ||
      datagram[kBeaconHelloMagic.size()] != kBeaconHelloVersion) {
    return std::nullopt;
  }
  uint64_t token = 0;
  for (size_t i = 0; i < sizeof(token); ++i) {
    token = (token << kBitsPerByte) | datagram[kBeaconHelloMagic.size() + 1 + i];
  }
  return token;
}

BeaconHellos::BeaconHellos(uint64_t window_ms, size_t capacity)
    : window_ms_(window_ms), capacity_(capacity) {}

void BeaconHellos::Note(const Endpoint& source, uint64_t token, uint64_t now_ms) {
  for (Entry& entry : entries_) {
    if (entry.source == source) {
      entry.token = token;
      entry.seen_ms = now_ms;
      return;
    }
  }
  if (capacity_ == kZeroCapacity) {
    return;
  }
  if (entries_.size() >= capacity_) {
    auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                   [](const Entry& a, const Entry& b) {
                                     return a.seen_ms < b.seen_ms;
                                   });
    entries_.erase(oldest);
  }
  entries_.push_back(Entry{source, token, now_ms});
}

std::optional<uint64_t> BeaconHellos::TokenFrom(const Endpoint& source, uint64_t now_ms) const {
  for (const Entry& entry : entries_) {
    if (entry.source == source && entry.seen_ms <= now_ms &&
        now_ms - entry.seen_ms <= window_ms_) {
      return entry.token;
    }
  }
  return std::nullopt;
}

BeaconVerdict JudgeBeaconDatagram(uint16_t socket_guest_port, uint64_t own_token,
                                  std::span<const uint8_t> datagram, const Endpoint& source,
                                  BeaconHellos& hellos, uint64_t now_ms) {
  if (socket_guest_port != kBeaconGuestPort) {
    return BeaconVerdict::kPass;
  }
  if (std::optional<uint64_t> hello = ParseBeaconHello(datagram)) {
    hellos.Note(source, *hello, now_ms);
    return BeaconVerdict::kHello;
  }
  if (own_token == 0) {
    return BeaconVerdict::kPass;
  }
  std::optional<uint64_t> peer_token = hellos.TokenFrom(source, now_ms);
  if (!peer_token) {
    return BeaconVerdict::kNoHello;
  }
  return *peer_token == own_token ? BeaconVerdict::kPass : BeaconVerdict::kMismatch;
}

RecentSources::RecentSources(uint64_t window_ms, size_t capacity)
    : window_ms_(window_ms), capacity_(capacity) {}

void RecentSources::Note(const Endpoint& source, uint64_t now_ms) {
  for (Entry& entry : entries_) {
    if (entry.source == source) {
      entry.seen_ms = now_ms;
      return;
    }
  }
  if (capacity_ == kZeroCapacity) {
    return;
  }
  if (entries_.size() >= capacity_) {
    auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                   [](const Entry& a, const Entry& b) {
                                     return a.seen_ms < b.seen_ms;
                                   });
    entries_.erase(oldest);
  }
  entries_.push_back(Entry{source, now_ms});
}

std::vector<Endpoint> RecentSources::Active(uint64_t now_ms) const {
  std::vector<Endpoint> result;
  for (const Entry& entry : entries_) {
    if (entry.seen_ms > now_ms) {
      continue;
    }
    if (now_ms - entry.seen_ms > window_ms_) {
      continue;
    }
    result.push_back(entry.source);
  }
  return result;
}

std::vector<Endpoint> RecentSources::Latest(uint64_t now_ms, size_t max_count) const {
  std::vector<Entry> active;
  for (const Entry& entry : entries_) {
    if (entry.seen_ms <= now_ms && now_ms - entry.seen_ms <= window_ms_) {
      active.push_back(entry);
    }
  }
  std::stable_sort(active.begin(), active.end(),
                   [](const Entry& a, const Entry& b) { return a.seen_ms > b.seen_ms; });
  std::vector<Endpoint> result;
  for (size_t i = 0; i < active.size() && i < max_count; ++i) {
    result.push_back(active[i].source);
  }
  return result;
}

ReplyBudget::ReplyBudget(uint32_t copies_per_second, size_t capacity)
    : copies_per_second_(copies_per_second), capacity_(capacity) {}

bool ReplyBudget::TryTake(const Endpoint& source, uint64_t now_ms) {
  if (copies_per_second_ == 0 || capacity_ == kZeroCapacity) {
    return false;
  }
  auto found = std::find_if(entries_.begin(), entries_.end(),
                            [&source](const Entry& entry) { return entry.source == source; });
  if (found == entries_.end()) {
    EvictOldestIfFull();
    entries_.push_back(Entry{source, copies_per_second_, now_ms});
    found = entries_.end() - 1;
  } else {
    Refill(*found, now_ms);
  }
  if (found->tokens == 0) {
    return false;
  }
  --found->tokens;
  return true;
}

void ReplyBudget::EvictOldestIfFull() {
  if (entries_.size() < capacity_) {
    return;
  }
  auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                 [](const Entry& a, const Entry& b) {
                                   return a.refilled_ms < b.refilled_ms;
                                 });
  entries_.erase(oldest);
}

void ReplyBudget::Refill(Entry& entry, uint64_t now_ms) const {
  uint64_t interval_ms = kMillisecondsPerSecond / copies_per_second_;
  if (now_ms > entry.refilled_ms && interval_ms > 0) {
    uint64_t gained = (now_ms - entry.refilled_ms) / interval_ms;
    if (gained > 0) {
      uint64_t tokens = std::min<uint64_t>(copies_per_second_, entry.tokens + gained);
      entry.tokens = static_cast<uint32_t>(tokens);
      entry.refilled_ms += gained * interval_ms;
    }
  }
  if (entry.tokens == copies_per_second_) {
    // A full bucket banks nothing: the next refill counts from now.
    entry.refilled_ms = std::max(entry.refilled_ms, now_ms);
  }
}

bool RecentSources::HeardFrom(uint32_t ip, uint64_t now_ms) const {
  for (const Endpoint& source : Active(now_ms)) {
    if (source.ip == ip) {
      return true;
    }
  }
  return false;
}

DuplicateFilter::DuplicateFilter(uint64_t window_ms, size_t capacity)
    : window_ms_(window_ms), capacity_(capacity) {}

bool DuplicateFilter::IsDuplicate(const Endpoint& source, std::span<const uint8_t> datagram,
                                  uint64_t now_ms) {
  auto is_expired = [now_ms, window = window_ms_](const Entry& entry) {
    if (entry.seen_ms > now_ms) {
      return true;
    }
    return now_ms - entry.seen_ms > window;
  };
  std::erase_if(entries_, is_expired);
  uint64_t hash = Fnv1a64(datagram);
  size_t size = datagram.size();
  for (const Entry& entry : entries_) {
    if (entry.source == source && entry.size == size && entry.hash == hash) {
      return true;
    }
  }
  if (capacity_ == kZeroCapacity) {
    return false;
  }
  if (entries_.size() >= capacity_) {
    auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                   [](const Entry& a, const Entry& b) {
                                     return a.seen_ms < b.seen_ms;
                                   });
    entries_.erase(oldest);
  }
  entries_.push_back(Entry{source, hash, size, now_ms});
  return false;
}

FirstContacts::FirstContacts(size_t capacity) : capacity_(std::max<size_t>(capacity, 1)) {}

bool FirstContacts::IsFirst(uint64_t socket_key, const Endpoint& peer) {
  for (const Entry& entry : entries_) {
    if (entry.socket_key == socket_key && entry.peer == peer) {
      return false;
    }
  }
  if (entries_.size() >= capacity_) {
    entries_.erase(entries_.begin());
  }
  entries_.push_back(Entry{socket_key, peer});
  return true;
}

}  // namespace rex::system::system_link
