#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/env.h>
#include <rex/system/system_link.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(net_link_active, false, "Network",
                    "Report an active 100 Mbps Ethernet link to the game, which turns System Link "
                    "(LAN play, campaign co-op) on. false: an unplugged cable, as on a 360 without "
                    "one; the game opens no LAN session and sees the fixed offline XUID")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(
    net_beacon_gate, true, "Network",
    "With gow2_coop_players 3+ the game sets a co-op hash; System Link then lists and answers "
    "only peers that announce the same hash. false turns the check off");
REXCVAR_DEFINE_UINT64(
    net_coop_hash, 0, "Network",
    "Set by the game when gow2_coop_players is 3 or more: the co-op hash every System Link peer "
    "must share (0: no gate). Not meant to be set by hand");
REXCVAR_DEFINE_STRING(net_local_ip, "", "Network",
                      "IPv4 address (a.b.c.d or a.b.c.d/prefix, prefix 24 by default) this "
                      "instance advertises to System Link peers (its XNADDR) and treats as its "
                      "local network. Empty: REX_DETECTED_LOCAL_IP (the Android app sets it), then "
                      "net_bind_ip, then the address of the adapter that holds the default route, "
                      "then 127.0.0.1")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(net_bind_ip, "", "Network",
                      "Bind the game's sockets to this IPv4 address instead of 0.0.0.0, so several "
                      "instances can share one PC (127.0.0.2, 127.0.0.3, ...). Empty: off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    net_broadcast_targets, "", "Network",
    "Comma-separated IPv4 addresses. Each broadcast the game sends goes out as one "
    "unicast copy per address, skipping this instance's own. Needed with "
    "net_bind_ip. Empty: broadcasts go out unchanged")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(net_port_offset, 40000, "Network",
                      "Guest ports below 1024 use port + this offset on the host (the game's UDP "
                      "1000 becomes 41000), so Android can bind them. Every peer needs the same "
                      "value. 0: off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    net_peers, "", "Network",
    "Comma-separated host[:port] list (IPv4 addresses or DNS names such as "
    "gow2.example.com). Every broadcast the game sends also goes to each peer as a unicast "
    "copy: System Link over the internet. Names are resolved at start and every 5 minutes")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    net_public_address, "", "Network",
    "host[:port] (IPv4 or DNS name) this instance advertises as its online address "
    "(XNADDR inaOnline/wPortOnline). Peers outside the local network connect to it. Set "
    "net_allow_peers too: without it only the local network and net_peers may reach the game. "
    "Empty: none")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    net_allow_peers, "", "Network",
    "Comma-separated IPv4 addresses, a.b.c.d/n ranges or DNS names that may send datagrams to "
    "the game; loopback and the local network always may. Empty: with net_peers or "
    "net_public_address set, only the net_peers and the local network; else everyone")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT64(net_random_seed, 0, "Network",
                      "Non-zero: XNetRandom, session keys and nonces come from a deterministic "
                      "stream with this seed (tests). 0: real randomness")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(
    user_xuid, "", "Kernel",
    "XUID the game sees for the signed-in player, 1-16 hex digits with bits 54-55 "
    "clear. Empty: with net_link_active, a random XUID made once per install and kept "
    "in system_link_xuid.txt in the user data folder; without it, the fixed offline "
    "XUID B13EBABEBABEBABE")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(user_name, "User", "Kernel",
                      "Gamertag the game shows for the signed-in player (up to 15 characters)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(local_users, 1, "Kernel",
                      "Signed-in local players (split-screen). Players 2-4 get their own profile, "
                      "gamertag and XUID; each needs its own controller.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .range(1, 4);

REXCVAR_DECLARE(bool, system_link_trace);

namespace rex::system::system_link {
namespace {

constexpr size_t kMaxGamertagLength = 15;
constexpr const char* kDefaultGamertag = "User";
constexpr size_t kRandomWordSize = sizeof(uint64_t);
std::atomic<bool> g_beacon_gate_on_logged{false};
std::atomic<bool> g_beacon_gate_off_logged{false};
// Any address off the local networks makes the routing table pick the default-route adapter;
// connect() on UDP sends nothing. 192.0.2.1 is TEST-NET-1 (RFC 5737).
constexpr uint32_t kRouteProbeIp = 0xC0000201;
constexpr uint16_t kRouteProbePort = 9;
constexpr size_t kHeardFromCapacity = 64;
#if REX_PLATFORM_WIN32
constexpr bool kPreferDirectedBroadcast = false;
#else
// Android sends 255.255.255.255 out of the default network, which can be mobile data.
constexpr bool kPreferDirectedBroadcast = true;
#endif

struct PeerSettings {
  bool peers_set = false;
  bool allow_set = false;
  bool public_set = false;
  std::vector<HostPort> peers;
  std::vector<std::string> allow_entries;
  std::optional<HostPort> public_host;
  std::string peers_text;
  std::string allow_text;
  std::string public_text;
};

// The settings are read once, when the kernel starts: the network functions run on game threads
// while the settings panel can rewrite the cvars, and the advertised address must not move.
struct NetworkSettings {
  bool link_active = false;
  std::optional<uint32_t> bind_ip;
  Subnet local_subnet{kLoopbackIp, kLoopbackPrefixLength};
  const char* local_origin = "";
  std::vector<uint32_t> broadcast_targets;
  std::vector<uint32_t> lan_broadcast_ips;
  uint32_t port_offset = kDefaultPortOffset;
  uint64_t random_seed = 0;
  PeerSettings peers;
};

std::optional<uint32_t> ParseIpSetting(std::string_view setting_name, const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  auto ip = ParseIpv4(value);
  if (!ip) {
    REXLOG_WARN("{} '{}' is not an IPv4 address; ignored", setting_name, value);
  }
  return ip;
}

std::optional<uint32_t> ReadBindIp() {
  auto ip = ParseIpSetting("net_bind_ip", REXCVAR_GET(net_bind_ip));
  if (ip && *ip == 0) {
    return std::nullopt;
  }
  return ip;
}

std::vector<uint32_t> ReadBroadcastTargets() {
  std::string value = REXCVAR_GET(net_broadcast_targets);
  auto targets = ParseIpv4List(value);
  if (targets.empty() && !value.empty()) {
    REXLOG_WARN("net_broadcast_targets '{}' holds no IPv4 address; ignored", value);
  }
  return targets;
}

std::optional<Subnet> ParseSubnetSetting(std::string_view setting_name, std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  auto subnet = ParseIpv4Cidr(value, kDefaultLocalPrefixLength);
  if (!subnet) {
    REXLOG_WARN("{} '{}' is not an IPv4 address or a.b.c.d/n; ignored", setting_name, value);
  }
  return subnet;
}

std::optional<Subnet> ReadEnvironmentSubnet() {
  std::optional<std::string> value = rex::platform::env::get(kLocalIpEnvironmentVariable);
  if (!value) {
    return std::nullopt;
  }
  return ParseSubnetSetting(kLocalIpEnvironmentVariable, *value);
}

uint32_t ReadPortOffset() {
  uint32_t offset = REXCVAR_GET(net_port_offset);
  if (!IsUsablePortOffset(offset)) {
    REXLOG_WARN("net_port_offset {} is not 0 or 1024-64512; using {}", offset,
                kDefaultPortOffset);
    return kDefaultPortOffset;
  }
  return offset;
}

void WarnAboutUnusableAllowEntries(const std::vector<std::string>& entries) {
  for (const std::string& entry : entries) {
    if (!ParseIpv4Cidr(entry, kHostPrefixLength) && !ParseHostPort(entry)) {
      REXLOG_WARN("net_allow_peers entry '{}' is not an address, range or host name; ignored",
                  entry);
    }
  }
}

std::optional<HostPort> ReadPublicHost(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  auto host = ParseHostPort(text);
  if (!host) {
    REXLOG_WARN("net_public_address '{}' is not host[:port]; ignored", text);
  }
  return host;
}

PeerSettings ReadPeerSettings() {
  PeerSettings p;
  p.peers_text = REXCVAR_GET(net_peers);
  p.allow_text = REXCVAR_GET(net_allow_peers);
  p.public_text = REXCVAR_GET(net_public_address);
  p.peers_set = !SplitListTokens(p.peers_text).empty();
  p.allow_set = !SplitListTokens(p.allow_text).empty();
  p.public_set = !p.public_text.empty();
  p.peers = ParseHostPortList(p.peers_text);
  for (std::string_view token : SplitListTokens(p.allow_text)) {
    p.allow_entries.emplace_back(token);
  }
  WarnAboutUnusableAllowEntries(p.allow_entries);
  p.public_host = ReadPublicHost(p.public_text);
  return p;
}

const char* LocalSubnetOrigin(std::optional<Subnet> configured, std::optional<Subnet> environment,
                              std::optional<uint32_t> bind_ip, const Subnet& local_subnet) {
  if (configured) {
    return "net_local_ip";
  }
  if (environment) {
    return kLocalIpEnvironmentVariable;
  }
  if (bind_ip) {
    return "net_bind_ip";
  }
  return local_subnet.ip == kLoopbackIp ? "no network adapter found" : "default-route adapter";
}

std::optional<uint32_t> DetectedIpWhenNeeded(bool link_active, bool address_given) {
  if (!link_active || address_given) {
    return std::nullopt;
  }
  return DetectRoutedIpv4();
}

void ReadLocalSubnet(NetworkSettings& s) {
  auto configured = ParseSubnetSetting("net_local_ip", REXCVAR_GET(net_local_ip));
  auto environment = ReadEnvironmentSubnet();
  bool address_given = configured || environment || s.bind_ip;
  auto detected = DetectedIpWhenNeeded(s.link_active, address_given);
  s.local_subnet = ResolveLocalSubnet(configured, environment, s.bind_ip, detected);
  s.local_origin = LocalSubnetOrigin(configured, environment, s.bind_ip, s.local_subnet);
}

std::string FormatIpList(const std::vector<uint32_t>& ips) {
  std::string list;
  for (uint32_t ip : ips) {
    list += (list.empty() ? "" : ", ") + FormatIpv4(ip);
  }
  return list.empty() ? "nothing" : list;
}

void WarnIfPeersCannotReach(const NetworkSettings& s) {
  if (s.local_subnet.ip != kLoopbackIp || !s.broadcast_targets.empty() || s.peers.peers_set) {
    return;
  }
  REXLOG_WARN(
      "[netlink] System Link advertises 127.0.0.1: players on other PCs cannot join. Set "
      "net_local_ip to this PC's LAN address");
}

void WarnIfPublicWithoutAllowList(const NetworkSettings& s) {
  if (!s.peers.public_set || s.peers.allow_set) {
    return;
  }
  REXLOG_WARN(
      "[netlink] net_public_address is set but net_allow_peers is empty: only the local network "
      "and net_peers may send to the game. List your friends' addresses or DNS names in "
      "net_allow_peers");
}

void LogInternetSettings(const PeerSettings& p) {
  if (!p.peers_set && !p.allow_set && !p.public_set) {
    return;
  }
  REXLOG_INFO("[netlink] net_peers '{}', net_public_address '{}', net_allow_peers '{}'",
              p.peers_text, p.public_text, p.allow_text);
}

void LogNetworkSettings(const NetworkSettings& s) {
  if (!s.link_active) {
    REXLOG_INFO("[netlink] link reported down (net_link_active=false): no System Link");
    return;
  }
  REXLOG_INFO(
      "[netlink] link active: local {} ({}), bind {}, {} broadcast target(s), LAN broadcast "
      "{}, port offset {}",
      FormatSubnet(s.local_subnet), s.local_origin,
      s.bind_ip ? FormatIpv4(*s.bind_ip) : std::string("0.0.0.0"), s.broadcast_targets.size(),
      FormatIpList(s.lan_broadcast_ips), s.port_offset);
  LogInternetSettings(s.peers);
  WarnIfPeersCannotReach(s);
  WarnIfPublicWithoutAllowList(s);
}

NetworkSettings ReadNetworkSettings() {
  NetworkSettings s;
  s.link_active = REXCVAR_GET(net_link_active);
  s.bind_ip = ReadBindIp();
  ReadLocalSubnet(s);
  s.broadcast_targets = ReadBroadcastTargets();
  s.lan_broadcast_ips = LanBroadcastIps(s.local_subnet, kPreferDirectedBroadcast);
  s.port_offset = ReadPortOffset();
  s.random_seed = REXCVAR_GET(net_random_seed);
  s.peers = ReadPeerSettings();
  LogNetworkSettings(s);
  return s;
}

const NetworkSettings& Settings() {
  static const NetworkSettings settings = ReadNetworkSettings();
  return settings;
}

std::optional<uint64_t> ConfiguredXuid() {
  std::string value = REXCVAR_GET(user_xuid);
  if (value.empty()) {
    return std::nullopt;
  }
  auto xuid = ParseXuid(value);
  if (!xuid) {
    REXLOG_ERROR(
        "user_xuid '{}' is not a usable XUID (1-16 hex digits, bits 54-55 clear); ignored",
        value);
  }
  return xuid;
}

std::filesystem::path InstallXuidPath(const std::filesystem::path& user_data_root) {
  return user_data_root / kInstallXuidFileName;
}

std::optional<uint64_t> ReadInstallXuid(const std::filesystem::path& user_data_root) {
  if (user_data_root.empty()) {
    return std::nullopt;
  }
  std::ifstream file(InstallXuidPath(user_data_root));
  std::string text;
  if (!file || !(file >> text)) {
    return std::nullopt;
  }
  auto xuid = ParseXuid(text);
  if (!xuid) {
    REXLOG_WARN("{} does not hold a usable XUID; making a new one",
                InstallXuidPath(user_data_root).string());
  }
  return xuid;
}

void WriteInstallXuid(const std::filesystem::path& user_data_root, uint64_t xuid) {
  if (user_data_root.empty()) {
    REXLOG_WARN("No user data folder: the System Link XUID {} lasts this run only",
                FormatXuid(xuid));
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(user_data_root, error);
  std::ofstream file(InstallXuidPath(user_data_root), std::ios::trunc);
  file << FormatXuid(xuid) << '\n';
  if (!file) {
    REXLOG_WARN("Could not write {}: the System Link XUID {} lasts this run only",
                InstallXuidPath(user_data_root).string(), FormatXuid(xuid));
  }
}

uint64_t InstallXuid(const std::filesystem::path& user_data_root) {
  if (auto stored = ReadInstallXuid(user_data_root)) {
    return *stored;
  }
  uint64_t created = MakeInstallXuid(RandomNonZero64());
  WriteInstallXuid(user_data_root, created);
  return created;
}

void CopyWord(std::span<uint8_t> out, size_t offset, uint64_t word) {
  size_t count = std::min(kRandomWordSize, out.size() - offset);
  std::memcpy(out.data() + offset, &word, count);
}

void FillFromSeededStream(std::span<uint8_t> out, uint64_t seed) {
  static std::mutex mutex;
  static uint64_t stream_state = seed;
  std::lock_guard lock(mutex);
  for (size_t offset = 0; offset < out.size(); offset += kRandomWordSize) {
    CopyWord(out, offset, SplitMix64(stream_state));
  }
}

void FillFromRandomDevice(std::span<uint8_t> out) {
  static std::mutex mutex;
  static std::random_device device;
  std::lock_guard lock(mutex);
  for (size_t offset = 0; offset < out.size(); offset += kRandomWordSize) {
    uint64_t word = (uint64_t(device()) << 32) | device();
    CopyWord(out, offset, word);
  }
}

#if REX_PLATFORM_WIN32
using ProbeSocket = SOCKET;
constexpr ProbeSocket kNoProbeSocket = INVALID_SOCKET;
void CloseProbeSocket(ProbeSocket s) {
  closesocket(s);
}

void EnsureWinsockStarted() {
  // The resolver thread may run before the game calls WSAStartup; Winsock counts the extra start.
  static std::once_flag flag;
  std::call_once(flag, []() {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}
#else
using ProbeSocket = int;
constexpr ProbeSocket kNoProbeSocket = -1;
void CloseProbeSocket(ProbeSocket s) {
  close(s);
}

void EnsureWinsockStarted() {}
#endif

std::optional<uint32_t> SourceIpTowards(uint32_t destination_ip, uint16_t port) {
  ProbeSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoProbeSocket) {
    return std::nullopt;
  }
  sockaddr_in destination = {};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(port);
  destination.sin_addr.s_addr = htonl(destination_ip);
  sockaddr_in source = {};
  socklen_t source_size = sizeof(source);
  bool routed =
      connect(s, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0 &&
      getsockname(s, reinterpret_cast<sockaddr*>(&source), &source_size) == 0;
  CloseProbeSocket(s);
  if (!routed) {
    return std::nullopt;
  }
  return ntohl(source.sin_addr.s_addr);
}

std::vector<uint32_t> LookupHost(const std::string& host) {
  EnsureWinsockStarted();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* results = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0) {
    return {};
  }
  std::vector<uint32_t> ips;
  for (addrinfo* node = results; node != nullptr; node = node->ai_next) {
    if (node->ai_family != AF_INET || node->ai_addr == nullptr) {
      continue;
    }
    uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(node->ai_addr)->sin_addr.s_addr);
    if (std::find(ips.begin(), ips.end(), ip) == ips.end()) {
      ips.push_back(ip);
    }
  }
  freeaddrinfo(results);
  return ips;
}

struct ResolvedPeers {
  std::vector<Endpoint> peers;
  std::vector<Subnet> allowed;
  std::optional<Endpoint> public_address;
};

struct ResolverState {
  std::mutex mutex;
  NameTable names;
  std::shared_ptr<const ResolvedPeers> current = std::make_shared<const ResolvedPeers>();
};

ResolverState& Resolver() {
  // The detached resolver thread may outlive static destruction.
  static ResolverState* state = new ResolverState();
  return *state;
}

std::vector<uint32_t> PeerIps(const std::vector<Endpoint>& peers) {
  std::vector<uint32_t> ips;
  ips.reserve(peers.size());
  for (const Endpoint& peer : peers) {
    ips.push_back(peer.ip);
  }
  return ips;
}

ResolvedPeers BuildResolvedPeers(const NetworkSettings& s, const NameTable& names) {
  const PeerSettings& p = s.peers;
  ResolvedPeers resolved;
  resolved.peers = PeerEndpointsFrom(p.peers, names);
  AllowPolicyInput policy;
  policy.allow_list_set = p.allow_set;
  policy.allow_list = AllowListFrom(p.allow_entries, names);
  policy.peers_set = p.peers_set;
  policy.peer_ips = PeerIps(resolved.peers);
  policy.public_address_set = p.public_set;
  policy.local_subnet = s.local_subnet;
  resolved.allowed = EffectiveAllowList(policy);
  resolved.public_address = PublicEndpointFrom(p.public_host, names);
  return resolved;
}

void PublishNames(NameTable names) {
  auto resolved = std::make_shared<const ResolvedPeers>(BuildResolvedPeers(Settings(), names));
  std::lock_guard lock(Resolver().mutex);
  Resolver().names = std::move(names);
  Resolver().current = std::move(resolved);
}

NameTable CurrentNames() {
  std::lock_guard lock(Resolver().mutex);
  return Resolver().names;
}

bool Contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

void LogUnchangedNames(const NameRefresh& refresh) {
  for (const auto& [name, ips] : refresh.table) {
    if (!Contains(refresh.changed, name) && !Contains(refresh.stale, name)) {
      REXLOG_INFO("[netlink] resolved {} -> {} (unchanged)", name, FormatIpList(ips));
    }
  }
}

void LogNameRefresh(const NameRefresh& refresh) {
  for (const std::string& name : refresh.changed) {
    REXLOG_INFO("[netlink] resolved {} -> {}", name, FormatIpList(refresh.table.at(name)));
  }
  for (const std::string& name : refresh.stale) {
    REXLOG_WARN("[netlink] cannot resolve {}; keeping {} until it resolves again", name,
                FormatIpList(refresh.table.at(name)));
  }
  for (const std::string& name : refresh.missing) {
    REXLOG_WARN("[netlink] cannot resolve {}", name);
  }
  if (TraceEnabled()) {
    LogUnchangedNames(refresh);
  }
}

void ResolverLoop(std::vector<std::string> names) {
  for (;;) {
    NameRefresh refresh = RefreshNames(CurrentNames(), names, LookupHost);
    LogNameRefresh(refresh);
    PublishNames(std::move(refresh.table));
    std::this_thread::sleep_for(std::chrono::milliseconds(kPeerResolveIntervalMs));
  }
}

std::shared_ptr<const ResolvedPeers> CurrentPeers() {
  StartPeerResolver();
  std::lock_guard lock(Resolver().mutex);
  return Resolver().current;
}

struct HeardFromTable {
  std::mutex mutex;
  RecentSources sources{kHeardFromWindowMs, kHeardFromCapacity};
};

HeardFromTable& HeardFrom() {
  static HeardFromTable table;
  return table;
}

void AppendUniqueIp(std::vector<uint32_t>& ips, uint32_t ip) {
  if (ip == 0 || std::find(ips.begin(), ips.end(), ip) != ips.end()) {
    return;
  }
  ips.push_back(ip);
}

}  // namespace

std::optional<uint32_t> DetectRoutedIpv4() {
#if REX_PLATFORM_WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return std::nullopt;
  }
#endif
  auto ip = SourceIpTowards(kRouteProbeIp, kRouteProbePort);
#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
  if (ip && !IsLanAddress(*ip)) {
    return std::nullopt;
  }
  return ip;
}

bool LinkActive() {
  return Settings().link_active;
}

uint32_t EthernetLinkStatus() {
  return LinkActive() ? kEthernetLinkActive100MbpsFullDuplex : 0;
}

std::optional<uint32_t> BindIp() {
  return Settings().bind_ip;
}

uint32_t LocalIp() {
  return Settings().local_subnet.ip;
}

Subnet LocalSubnet() {
  return Settings().local_subnet;
}

uint32_t SelfIp() {
  return BindIp().value_or(LocalIp());
}

std::vector<uint32_t> SelfIps() {
  std::vector<uint32_t> ips;
  AppendUniqueIp(ips, SelfIp());
  AppendUniqueIp(ips, LocalIp());
  if (auto public_address = PublicAddress()) {
    AppendUniqueIp(ips, public_address->ip);
  }
  return ips;
}

std::vector<uint32_t> BroadcastTargets() {
  return Settings().broadcast_targets;
}

std::vector<uint32_t> LanBroadcastIps() {
  return Settings().lan_broadcast_ips;
}

uint32_t PortOffset() {
  return Settings().port_offset;
}

std::vector<Endpoint> PeerEndpoints() {
  return CurrentPeers()->peers;
}

std::vector<Subnet> AllowedSources() {
  return CurrentPeers()->allowed;
}

std::optional<Endpoint> PublicAddress() {
  return CurrentPeers()->public_address;
}

void StartPeerResolver() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    // Literal addresses apply at once; a name allows nothing until its first lookup.
    PublishNames({});
    const PeerSettings& p = Settings().peers;
    std::vector<std::string> names = NamesToResolve(p.peers, p.allow_entries, p.public_host);
    if (!Settings().link_active || names.empty()) {
      return;
    }
    std::thread(ResolverLoop, std::move(names)).detach();
  });
}

void NoteHeardFrom(const Endpoint& source) {
  HeardFromTable& table = HeardFrom();
  std::lock_guard lock(table.mutex);
  table.sources.Note(source, NowMs());
}

bool HeardFromRecently(uint32_t ip) {
  HeardFromTable& table = HeardFrom();
  std::lock_guard lock(table.mutex);
  return table.sources.HeardFrom(ip, NowMs());
}

uint64_t NowMs() {
  auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void LogBeaconGateOnOnce(uint64_t token) {
  if (!g_beacon_gate_on_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[netlink] beacon gate on: co-op hash {}", FormatCoopHash(token));
  }
}

void LogBeaconGateOffOnce() {
  if (!g_beacon_gate_off_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[netlink] beacon gate off (net_beacon_gate=false)");
  }
}

uint64_t BeaconGateToken() {
  const uint64_t token = REXCVAR_GET(net_coop_hash);
  if (token == 0) {
    return 0;
  }
  if (!REXCVAR_GET(net_beacon_gate)) {
    LogBeaconGateOffOnce();
    return 0;
  }
  LogBeaconGateOnOnce(token);
  return token;
}

bool TraceEnabled() {
  return REXCVAR_GET(system_link_trace);
}

std::string Gamertag() {
  std::string name = REXCVAR_GET(user_name);
  if (name.empty()) {
    return kDefaultGamertag;
  }
  return name.substr(0, kMaxGamertagLength);
}

uint64_t ResolveXuid(const std::filesystem::path& user_data_root) {
  auto configured = ConfiguredXuid();
  switch (ChooseXuidSource(configured.has_value(), LinkActive())) {
    case XuidSource::kSetting:
      return *configured;
    case XuidSource::kInstall:
      return InstallXuid(user_data_root);
    case XuidSource::kOffline:
      break;
  }
  return kOfflineXuid;
}

XnAddrBytes TitleXnAddr(uint64_t xuid) {
  uint32_t ip = LocalIp();
  std::optional<Endpoint> public_address = PublicAddress();
  uint32_t online_ip = public_address ? public_address->ip : 0;
  uint16_t online_port = public_address ? public_address->port : 0;
  return BuildXnAddr(ip, MakeInstanceMac(xuid, ip), online_ip, online_port);
}

void FillRandom(std::span<uint8_t> out) {
  uint64_t seed = Settings().random_seed;
  if (seed != 0) {
    FillFromSeededStream(out, seed);
  } else {
    FillFromRandomDevice(out);
  }
}

uint64_t RandomNonZero64() {
  uint64_t value = 0;
  while (value == 0) {
    FillRandom(std::span(reinterpret_cast<uint8_t*>(&value), sizeof(value)));
  }
  return value;
}

}  // namespace rex::system::system_link
