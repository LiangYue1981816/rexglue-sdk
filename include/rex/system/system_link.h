#pragma once
// Every IPv4 address here is a host-order uint32_t (127.0.0.1 = 0x7F000001).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex::system::system_link {

constexpr uint32_t kLoopbackIp = 0x7F000001;
constexpr uint32_t kBroadcastIp = 0xFFFFFFFF;
// What the game saw before System Link existed in the port; still used while the link is off.
constexpr uint64_t kOfflineXuid = 0xB13EBABEBABEBABEull;
// 58410A1F refuses XUIDs with either of these bits set (see UserProfile).
constexpr uint64_t kXuidForbiddenBits = 0x00C0000000000000ull;
constexpr uint64_t kInstallXuidPrefix = 0xB13E000000000000ull;
constexpr uint64_t kInstallXuidRandomMask = 0x0000FFFFFFFFFFFFull;
constexpr uint32_t kEthernetLinkActive100MbpsFullDuplex = 0x0B;
constexpr size_t kMacSize = 6;
constexpr size_t kXnAddrSize = 36;
constexpr size_t kXnKidSize = 8;
constexpr size_t kXnKeySize = 16;
constexpr size_t kSessionInfoSize = kXnKidSize + kXnAddrSize + kXnKeySize;
constexpr const char* kInstallXuidFileName = "system_link_xuid.txt";

// Cross-play and internet play (patch 0011).
constexpr uint32_t kDefaultPortOffset = 40000;
constexpr uint32_t kPrivilegedPortLimit = 1024;
constexpr uint32_t kMaxPort = 65535;
constexpr int kDefaultLocalPrefixLength = 24;
constexpr int kHostPrefixLength = 32;
constexpr uint32_t kLoopbackNetwork = 0x7F000000;
constexpr int kLoopbackPrefixLength = 8;
constexpr size_t kXnAddrInaOffset = 0;
constexpr size_t kXnAddrInaOnlineOffset = 4;
constexpr size_t kXnAddrPortOnlineOffset = 8;
constexpr uint64_t kPeerResolveIntervalMs = 5ull * 60 * 1000;
// A broadcast is copied back only to senders heard this recently: the beacon answer follows its
// query within a few frames, and a longer window turns the host into a reflector.
constexpr uint64_t kReplyToSourceWindowMs = 1000;
constexpr size_t kMaxReplyCopiesPerBroadcast = 4;
constexpr uint32_t kReplyCopiesPerSecond = 2;
constexpr size_t kReplyBudgetCapacity = 64;
// UE3's LanAnnouncePort: the System Link beacon socket, the only one whose broadcasts are copied
// back to recent senders.
constexpr uint16_t kBeaconGuestPort = 14001;
constexpr size_t kMaxUdpDatagramSize = 65536;
constexpr size_t kBeaconHelloSize = 17;
constexpr uint8_t kBeaconHelloVersion = 1;
constexpr uint64_t kBeaconHelloWindowMs = 5000;
constexpr size_t kBeaconHelloCapacity = 32;
constexpr uint64_t kHeardFromWindowMs = 10ull * 60 * 1000;
constexpr size_t kRecentSourceCapacity = 32;
constexpr uint64_t kDuplicateWindowMs = 250;
constexpr size_t kDuplicateCapacity = 16;
constexpr size_t kFirstContactCapacity = 256;
// Not REX_NET_LOCAL_IP: that name sets the net_local_ip cvar and would outrank the config file.
constexpr const char* kLocalIpEnvironmentVariable = "REX_DETECTED_LOCAL_IP";

using MacAddress = std::array<uint8_t, kMacSize>;
using XnAddrBytes = std::array<uint8_t, kXnAddrSize>;

enum class XuidSource { kSetting, kOffline, kInstall };

struct Subnet {
  uint32_t ip = 0;
  int prefix_length = kDefaultLocalPrefixLength;
  bool operator==(const Subnet&) const = default;
};

struct HostPort {
  std::string host;
  uint16_t port = 0;  // 0: no port given
  bool operator==(const HostPort&) const = default;
};

struct Endpoint {
  uint32_t ip = 0;
  uint16_t port = 0;
  bool operator==(const Endpoint&) const = default;
};

std::array<uint8_t, kBeaconHelloSize> EncodeBeaconHello(uint64_t token);
std::optional<uint64_t> ParseBeaconHello(std::span<const uint8_t> datagram);

// Remembers the latest co-op hello from each source. Not thread-safe.
class BeaconHellos {
 public:
  BeaconHellos(uint64_t window_ms, size_t capacity);
  void Note(const Endpoint& source, uint64_t token, uint64_t now_ms);
  std::optional<uint64_t> TokenFrom(const Endpoint& source, uint64_t now_ms) const;

 private:
  struct Entry {
    Endpoint source;
    uint64_t token;
    uint64_t seen_ms;
  };
  uint64_t window_ms_;
  size_t capacity_;
  std::vector<Entry> entries_;
};

enum class BeaconVerdict { kPass, kHello, kNoHello, kMismatch };

BeaconVerdict JudgeBeaconDatagram(uint16_t socket_guest_port, uint64_t own_token,
                                  std::span<const uint8_t> datagram, const Endpoint& source,
                                  BeaconHellos& hellos, uint64_t now_ms);

uint64_t BeaconGateToken();

enum class GuestSocketOption {
  kUnknown,
  kReuseAddr,
  kKeepAlive,
  kBroadcast,
  kOobInline,
  kLinger,
  kSendBuffer,
  kReceiveBuffer,
  kError,
  kType,
};

enum class GuestSocketIoctl {
  kUnknown,
  kNonBlocking,
  kBytesReadable,
};

struct LingerOption {
  uint16_t on = 0;
  uint16_t seconds = 0;
  bool operator==(const LingerOption&) const = default;
};

// Sources a socket heard from recently, oldest evicted first. Not thread-safe.
class RecentSources {
 public:
  RecentSources(uint64_t window_ms, size_t capacity);
  void Note(const Endpoint& source, uint64_t now_ms);
  std::vector<Endpoint> Active(uint64_t now_ms) const;
  std::vector<Endpoint> Latest(uint64_t now_ms, size_t max_count) const;
  bool HeardFrom(uint32_t ip, uint64_t now_ms) const;

 private:
  struct Entry {
    Endpoint source;
    uint64_t seen_ms;
  };
  uint64_t window_ms_;
  size_t capacity_;
  std::vector<Entry> entries_;
};

// Per-source token bucket for the unicast copies of a broadcast sent back to recent senders.
// Not thread-safe.
class ReplyBudget {
 public:
  ReplyBudget(uint32_t copies_per_second, size_t capacity);
  bool TryTake(const Endpoint& source, uint64_t now_ms);

 private:
  struct Entry {
    Endpoint source;
    uint32_t tokens;
    uint64_t refilled_ms;
  };
  void EvictOldestIfFull();
  void Refill(Entry& entry, uint64_t now_ms) const;
  uint32_t copies_per_second_;
  size_t capacity_;
  std::vector<Entry> entries_;
};

// Drops a datagram identical to one from the same source a moment ago: a host's beacon reply
// arrives both as its broadcast and as a unicast copy. Not thread-safe.
class DuplicateFilter {
 public:
  DuplicateFilter(uint64_t window_ms, size_t capacity);
  bool IsDuplicate(const Endpoint& source, std::span<const uint8_t> datagram, uint64_t now_ms);

 private:
  struct Entry {
    Endpoint source;
    uint64_t hash;
    size_t size;
    uint64_t seen_ms;
  };
  uint64_t window_ms_;
  size_t capacity_;
  std::vector<Entry> entries_;
};

// Remembers which (socket, peer) pairs were already traced, so the first datagram each way
// is always logged once the rate-limited trace budget is spent. Not thread-safe.
class FirstContacts {
 public:
  explicit FirstContacts(size_t capacity);
  bool IsFirst(uint64_t socket_key, const Endpoint& peer);

 private:
  struct Entry {
    uint64_t socket_key;
    Endpoint peer;
  };
  size_t capacity_;
  std::vector<Entry> entries_;
};

// Host name -> its last good IPv4 addresses.
using NameTable = std::map<std::string, std::vector<uint32_t>>;
using HostLookup = std::function<std::vector<uint32_t>(const std::string& host)>;

struct NameRefresh {
  NameTable table;
  std::vector<std::string> changed;  // resolved to a different, non-empty list
  std::vector<std::string> stale;    // lookup failed; previous addresses kept
  std::vector<std::string> missing;  // lookup failed and nothing to keep
};

struct AllowPolicyInput {
  bool allow_list_set = false;       // net_allow_peers is not empty
  std::vector<Subnet> allow_list;    // its ranges plus the addresses of its names
  bool peers_set = false;            // net_peers is not empty
  std::vector<uint32_t> peer_ips;    // the addresses of net_peers
  bool public_address_set = false;   // net_public_address is not empty
  Subnet local_subnet;
};

std::optional<uint32_t> ParseIpv4(std::string_view text);
std::vector<uint32_t> ParseIpv4List(std::string_view text);
std::string FormatIpv4(uint32_t ip);
std::optional<uint64_t> ParseXuid(std::string_view text);
bool IsUsableXuid(uint64_t xuid);
uint64_t MakeInstallXuid(uint64_t random_bits);
std::string FormatXuid(uint64_t xuid);
std::string FormatCoopHash(uint64_t hash);
bool IsLanAddress(uint32_t ip);
uint32_t ResolveLocalIp(std::optional<uint32_t> local_ip, std::optional<uint32_t> bind_ip,
                        std::optional<uint32_t> detected_ip);
XuidSource ChooseXuidSource(bool xuid_setting_usable, bool link_active);
uint64_t SplitMix64(uint64_t& state);
MacAddress MakeInstanceMac(uint64_t xuid, uint32_t local_ip);
XnAddrBytes BuildXnAddr(uint32_t local_ip, const MacAddress& mac, uint32_t online_ip = 0,
                        uint16_t online_port = 0);
void MarkSystemLinkXnKid(std::span<uint8_t, kXnKidSize> xnkid);
uint32_t RewriteBindAddress(uint32_t requested_ip, uint32_t bind_ip);
std::vector<uint32_t> SendDestinations(uint32_t destination_ip,
                                       std::span<const uint32_t> broadcast_targets,
                                       uint32_t self_ip);
uint32_t WsaErrorFromErrno(int error);
uint32_t WsaErrorFromConnectErrno(int error);

bool IsUsablePortOffset(uint64_t offset);
uint16_t GuestToHostPort(uint16_t guest_port, uint32_t offset);
uint16_t HostToGuestPort(uint16_t host_port, uint32_t offset);

std::vector<std::string_view> SplitListTokens(std::string_view text);
std::optional<Subnet> ParseIpv4Cidr(std::string_view text, int default_prefix_length);
std::string FormatSubnet(const Subnet& subnet);
uint32_t PrefixMask(int prefix_length);
bool IsInSubnet(uint32_t ip, const Subnet& subnet);
bool IsLoopbackIp(uint32_t ip);
uint32_t LanBroadcastIp(const Subnet& local_subnet, bool prefer_directed);
std::vector<uint32_t> LanBroadcastIps(const Subnet& local_subnet, bool prefer_directed);
Subnet ResolveLocalSubnet(std::optional<Subnet> configured, std::optional<Subnet> environment,
                          std::optional<uint32_t> bind_ip, std::optional<uint32_t> detected_ip);
std::optional<HostPort> ParseHostPort(std::string_view text);
std::vector<HostPort> ParseHostPortList(std::string_view text);
std::string FormatEndpoint(const Endpoint& endpoint);

bool IsAllowedSource(uint32_t source_ip, std::span<const Subnet> allowed);
std::vector<Subnet> DefaultAllowList(std::span<const uint32_t> peer_ips, const Subnet& local_subnet);
std::vector<Subnet> EffectiveAllowList(const AllowPolicyInput& input);
std::vector<Endpoint> BroadcastFanOut(uint16_t host_port,
                                      std::span<const uint32_t> lan_broadcast_ips,
                                      std::span<const uint32_t> broadcast_targets,
                                      std::span<const Endpoint> peers,
                                      std::span<const Endpoint> recent_sources,
                                      std::span<const uint32_t> self_ips);
uint32_t PeerInAddr(uint32_t peer_ina, uint32_t peer_ina_online, const Subnet& local_subnet,
                    bool heard_from_ina);
std::vector<Endpoint> ReplyCopyTargets(uint16_t socket_guest_port,
                                       std::span<const Endpoint> latest_sources,
                                       ReplyBudget& budget, uint64_t now_ms);

std::vector<std::string> NamesToResolve(std::span<const HostPort> peers,
                                        std::span<const std::string> allow_entries,
                                        const std::optional<HostPort>& public_host);
NameRefresh RefreshNames(const NameTable& previous, std::span<const std::string> names,
                         const HostLookup& lookup);
std::vector<uint32_t> HostAddresses(const std::string& host, const NameTable& names);
std::vector<Endpoint> PeerEndpointsFrom(std::span<const HostPort> peers, const NameTable& names);
std::vector<Subnet> AllowListFrom(std::span<const std::string> allow_entries,
                                  const NameTable& names);
std::optional<Endpoint> PublicEndpointFrom(const std::optional<HostPort>& public_host,
                                           const NameTable& names);

GuestSocketOption ClassifyGuestSocketOption(uint32_t level, uint32_t optname);
GuestSocketIoctl ClassifyGuestIoctl(uint32_t cmd);
LingerOption ReadGuestLinger(std::span<const uint8_t, 4> big_endian_value);
uint32_t ReadBigEndian32(std::span<const uint8_t, 4> bytes);
void WriteBigEndian32(std::span<uint8_t, 4> bytes, uint32_t value);
uint64_t Fnv1a64(std::span<const uint8_t> bytes);

bool LinkActive();
uint32_t EthernetLinkStatus();
std::optional<uint32_t> BindIp();
uint32_t LocalIp();
Subnet LocalSubnet();
uint32_t SelfIp();
std::vector<uint32_t> SelfIps();
std::vector<uint32_t> BroadcastTargets();
std::vector<uint32_t> LanBroadcastIps();
uint32_t PortOffset();
std::vector<Endpoint> PeerEndpoints();
std::vector<Subnet> AllowedSources();
std::optional<Endpoint> PublicAddress();
void StartPeerResolver();
void NoteHeardFrom(const Endpoint& source);
bool HeardFromRecently(uint32_t ip);
uint64_t NowMs();
bool TraceEnabled();
std::string Gamertag();
uint64_t ResolveXuid(const std::filesystem::path& user_data_root);
std::optional<uint32_t> DetectRoutedIpv4();
XnAddrBytes TitleXnAddr(uint64_t xuid);
void FillRandom(std::span<uint8_t> out);
uint64_t RandomNonZero64();

}  // namespace rex::system::system_link
