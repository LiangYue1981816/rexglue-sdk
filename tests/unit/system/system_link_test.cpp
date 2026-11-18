#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/system/system_link.h>

using namespace rex::system::system_link;

namespace {
const std::vector<uint32_t> kLan{0xC0A80AFFu /* 192.168.10.255 */};
}  // namespace

TEST_CASE("system_link: ParseIpv4 parses valid addresses and rejects malformed ones",
          "[system][system_link]") {
  CHECK(ParseIpv4("127.0.0.1") == 0x7F000001u);
  CHECK(ParseIpv4(" 10.0.0.255 ") == 0x0A0000FFu);
  CHECK(ParseIpv4("001.002.003.004") == 0x01020304u);
  CHECK(ParseIpv4("0.0.0.0") == 0u);
  CHECK(!ParseIpv4("256.1.1.1").has_value());
  CHECK(!ParseIpv4("1.2.3").has_value());
  CHECK(!ParseIpv4("1.2.3.4.5").has_value());
  CHECK(!ParseIpv4("1..2.3").has_value());
  CHECK(!ParseIpv4("a.b.c.d").has_value());
  CHECK(!ParseIpv4("1.2.3.4x").has_value());
  CHECK(!ParseIpv4("").has_value());
}

TEST_CASE("system_link: ParseIpv4List splits and filters addresses", "[system][system_link]") {
  std::vector<uint32_t> expected{0x7F000001u, 0x7F000002u, 0x7F000003u};
  CHECK(ParseIpv4List("127.0.0.1, 127.0.0.2;bad,,127.0.0.3") == expected);
  CHECK(ParseIpv4List("") == std::vector<uint32_t>{});
  std::vector<uint32_t> dupes{0x7F000001u, 0x7F000001u};
  CHECK(ParseIpv4List("127.0.0.1\t127.0.0.1") == dupes);
  CHECK(ParseIpv4List("bad;also-bad") == std::vector<uint32_t>{});
}

TEST_CASE("system_link: FormatIpv4 formats dotted quads", "[system][system_link]") {
  CHECK(FormatIpv4(0x7F000002u) == "127.0.0.2");
  CHECK(FormatIpv4(0u) == "0.0.0.0");
  CHECK(FormatIpv4(0x0A0000FFu) == "10.0.0.255");
}

TEST_CASE("system_link: ParseXuid parses usable XUIDs", "[system][system_link]") {
  CHECK(ParseXuid("B13EBABEBABEBABE") == 0xB13EBABEBABEBABEull);
  CHECK(ParseXuid("0xe000000000000001") == 0xE000000000000001ull);
  CHECK(ParseXuid(" 1 ") == 1ull);
  CHECK(ParseXuid("0X1") == 1ull);
  CHECK(!ParseXuid("00C0000000000000").has_value());
  CHECK(!ParseXuid("0040000000000000").has_value());
  CHECK(!ParseXuid("0").has_value());
  CHECK(!ParseXuid("12345678901234567").has_value());
  CHECK(!ParseXuid("xyz").has_value());
  CHECK(!ParseXuid("0x").has_value());
  CHECK(!ParseXuid("").has_value());
}

TEST_CASE("system_link: IsUsableXuid validates XUIDs", "[system][system_link]") {
  CHECK(!IsUsableXuid(0ull));
  CHECK(IsUsableXuid(1ull));
  CHECK(!IsUsableXuid(0x00C0000000000000ull));
  CHECK(!IsUsableXuid(0x0040000000000000ull));
  CHECK(IsUsableXuid(0xB13EBABEBABEBABEull));
  CHECK(IsUsableXuid(0xE000000000000001ull));
}

TEST_CASE("system_link: MakeInstallXuid builds install XUIDs", "[system][system_link]") {
  CHECK(MakeInstallXuid(0ull) == 0xB13E000000000001ull);
  CHECK(MakeInstallXuid(0xFFFFFFFFFFFFFFFFull) == 0xB13EFFFFFFFFFFFFull);
  CHECK(IsUsableXuid(MakeInstallXuid(0ull)));
  CHECK(IsUsableXuid(MakeInstallXuid(0xFFFFFFFFFFFFFFFFull)));
  CHECK(IsUsableXuid(MakeInstallXuid(0x123456789ABCDEF0ull)));
}

TEST_CASE("system_link: FormatXuid formats XUIDs", "[system][system_link]") {
  CHECK(FormatXuid(0xABull) == "00000000000000AB");
  CHECK(FormatXuid(0ull) == "0000000000000000");
  CHECK(ParseXuid(FormatXuid(1ull)) == 1ull);
  CHECK(ParseXuid(FormatXuid(0xB13EBABEBABEBABEull)) == 0xB13EBABEBABEBABEull);
  CHECK(ParseXuid(FormatXuid(0xE000000000000001ull)) == 0xE000000000000001ull);
  CHECK(ParseXuid(FormatXuid(0xB13E000000000001ull)) == 0xB13E000000000001ull);
}

TEST_CASE("system_link: FormatCoopHash formats lowercase zero-padded hashes",
          "[system][system_link]") {
  CHECK(FormatCoopHash(0x0) == "0000000000000000");
  CHECK(FormatCoopHash(0x937c35f80cb274b7) == "937c35f80cb274b7");
}

TEST_CASE("system_link: ResolveLocalIp selects local address", "[system][system_link]") {
  CHECK(ResolveLocalIp(0x0A000001u, 0x0A000002u, std::nullopt) == 0x0A000001u);
  CHECK(ResolveLocalIp(0x0A000001u, std::nullopt, std::nullopt) == 0x0A000001u);
  CHECK(ResolveLocalIp(std::nullopt, 0x0A000002u, std::nullopt) == 0x0A000002u);
  CHECK(ResolveLocalIp(std::nullopt, std::nullopt, std::nullopt) == kLoopbackIp);
  CHECK(ResolveLocalIp(std::nullopt, std::nullopt, 0xC0A80114u) == 0xC0A80114u);
  CHECK(ResolveLocalIp(0x0A000001u, std::nullopt, 0xC0A80114u) == 0x0A000001u);
  CHECK(ResolveLocalIp(std::nullopt, 0x0A000002u, 0xC0A80114u) == 0x0A000002u);
  CHECK(ResolveLocalIp(std::nullopt, std::nullopt, 0x7F000001u) == kLoopbackIp);
  CHECK(ResolveLocalIp(std::nullopt, std::nullopt, 0u) == kLoopbackIp);
  CHECK(ResolveLocalIp(std::nullopt, std::nullopt, kBroadcastIp) == kLoopbackIp);
  CHECK(ResolveLocalIp(std::nullopt, std::nullopt, 0xE0000001u) == kLoopbackIp);
}

TEST_CASE("system_link: IsLanAddress accepts unicast LAN addresses",
          "[system][system_link]") {
  CHECK(!IsLanAddress(0u));
  CHECK(!IsLanAddress(kBroadcastIp));
  CHECK(!IsLanAddress(0x7F000001u));
  CHECK(!IsLanAddress(0x7F000002u));
  CHECK(!IsLanAddress(0xE0000001u));
  CHECK(IsLanAddress(0xC0A80114u));
  CHECK(IsLanAddress(0x0A000005u));
  CHECK(IsLanAddress(0xAC100001u));
  CHECK(IsLanAddress(0xA9FE0304u));
}

TEST_CASE("system_link: ChooseXuidSource keeps the offline XUID while the link is off",
          "[system][system_link]") {
  CHECK(ChooseXuidSource(true, true) == XuidSource::kSetting);
  CHECK(ChooseXuidSource(true, false) == XuidSource::kSetting);
  CHECK(ChooseXuidSource(false, true) == XuidSource::kInstall);
  CHECK(ChooseXuidSource(false, false) == XuidSource::kOffline);
  CHECK(IsUsableXuid(kOfflineXuid));
}

TEST_CASE("system_link: SplitMix64 mixes state", "[system][system_link]") {
  uint64_t state = 0;
  uint64_t output = SplitMix64(state);
  CHECK(output == 0xE220A8397B1DCDAFull);
  CHECK(state == 0x9E3779B97F4A7C15ull);
}

TEST_CASE("system_link: MakeInstanceMac derives MAC addresses", "[system][system_link]") {
  MacAddress first = MakeInstanceMac(1ull, 0x7F000001u);
  CHECK(first[0] == 0x02);
  CHECK(MakeInstanceMac(1ull, 0x7F000001u) == first);
  CHECK(MakeInstanceMac(1ull, 0x7F000002u) != first);
  CHECK(MakeInstanceMac(2ull, 0x7F000001u) != first);
}

TEST_CASE("system_link: BuildXnAddr lays out XNADDR bytes", "[system][system_link]") {
  MacAddress mac{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  XnAddrBytes out = BuildXnAddr(0x7F000001u, mac);
  CHECK(out.size() == kXnAddrSize);
  CHECK(out[0] == 0x7F);
  CHECK(out[1] == 0x00);
  CHECK(out[2] == 0x00);
  CHECK(out[3] == 0x01);
  for (size_t i = 4; i < 10; ++i) {
    CHECK(out[i] == 0x00);
  }
  CHECK(out[10] == 0x02);
  CHECK(out[11] == 0x11);
  CHECK(out[12] == 0x22);
  CHECK(out[13] == 0x33);
  CHECK(out[14] == 0x44);
  CHECK(out[15] == 0x55);
  for (size_t i = 16; i < kXnAddrSize; ++i) {
    CHECK(out[i] == 0x00);
  }
}

TEST_CASE("system_link: MarkSystemLinkXnKid masks XNKID", "[system][system_link]") {
  std::array<uint8_t, kXnKidSize> kid{0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  MarkSystemLinkXnKid(kid);
  std::array<uint8_t, kXnKidSize> expected{0x0F, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  CHECK(kid == expected);
}

TEST_CASE("system_link: RewriteBindAddress rewrites bind address", "[system][system_link]") {
  CHECK(RewriteBindAddress(0u, 0x0A000001u) == 0x0A000001u);
  CHECK(RewriteBindAddress(0u, 0u) == 0u);
  CHECK(RewriteBindAddress(0x0A000002u, 0x0A000001u) == 0x0A000002u);
  CHECK(RewriteBindAddress(0x0A000002u, 0u) == 0x0A000002u);
}

TEST_CASE("system_link: SendDestinations expands broadcast targets", "[system][system_link]") {
  std::vector<uint32_t> single{0x7F000002u};
  CHECK(SendDestinations(0x7F000001u, single, 0x7F000003u) == std::vector<uint32_t>{0x7F000001u});
  std::vector<uint32_t> empty{};
  CHECK(SendDestinations(kBroadcastIp, empty, 0x7F000001u) == std::vector<uint32_t>{kBroadcastIp});
  std::vector<uint32_t> targets{0x7F000001u, 0x7F000002u, 0x7F000003u,
                                0x7F000002u, 0u,          kBroadcastIp};
  std::vector<uint32_t> expanded{0x7F000001u, 0x7F000003u};
  CHECK(SendDestinations(kBroadcastIp, targets, 0x7F000002u) == expanded);
  std::vector<uint32_t> self_only{0x7F000002u};
  CHECK(SendDestinations(kBroadcastIp, self_only, 0x7F000002u) == std::vector<uint32_t>{});
}

TEST_CASE("system_link: WsaErrorFromErrno maps errnos", "[system][system_link]") {
  CHECK(WsaErrorFromErrno(0) == 0u);
#ifdef EAGAIN
  CHECK(WsaErrorFromErrno(EAGAIN) == 10035u);
#endif
#ifdef EWOULDBLOCK
  CHECK(WsaErrorFromErrno(EWOULDBLOCK) == 10035u);
#endif
#ifdef EINPROGRESS
  CHECK(WsaErrorFromErrno(EINPROGRESS) == 10036u);
#endif
#ifdef EALREADY
  CHECK(WsaErrorFromErrno(EALREADY) == 10037u);
#endif
#ifdef ENOTSOCK
  CHECK(WsaErrorFromErrno(ENOTSOCK) == 10038u);
#endif
#ifdef EBADF
  CHECK(WsaErrorFromErrno(EBADF) == 10038u);
#endif
#ifdef EMSGSIZE
  CHECK(WsaErrorFromErrno(EMSGSIZE) == 10040u);
#endif
#ifdef ENOPROTOOPT
  CHECK(WsaErrorFromErrno(ENOPROTOOPT) == 10042u);
#endif
#ifdef EOPNOTSUPP
  CHECK(WsaErrorFromErrno(EOPNOTSUPP) == 10045u);
#endif
#ifdef EAFNOSUPPORT
  CHECK(WsaErrorFromErrno(EAFNOSUPPORT) == 10047u);
#endif
#ifdef EADDRINUSE
  CHECK(WsaErrorFromErrno(EADDRINUSE) == 10048u);
#endif
#ifdef EADDRNOTAVAIL
  CHECK(WsaErrorFromErrno(EADDRNOTAVAIL) == 10049u);
#endif
#ifdef ENETDOWN
  CHECK(WsaErrorFromErrno(ENETDOWN) == 10050u);
#endif
#ifdef ENETUNREACH
  CHECK(WsaErrorFromErrno(ENETUNREACH) == 10051u);
#endif
#ifdef ECONNABORTED
  CHECK(WsaErrorFromErrno(ECONNABORTED) == 10053u);
#endif
#ifdef ECONNRESET
  CHECK(WsaErrorFromErrno(ECONNRESET) == 10054u);
#endif
#ifdef ECONNREFUSED
  CHECK(WsaErrorFromErrno(ECONNREFUSED) == 10054u);
#endif
#ifdef ENOBUFS
  CHECK(WsaErrorFromErrno(ENOBUFS) == 10055u);
#endif
#ifdef EISCONN
  CHECK(WsaErrorFromErrno(EISCONN) == 10056u);
#endif
#ifdef ENOTCONN
  CHECK(WsaErrorFromErrno(ENOTCONN) == 10057u);
#endif
#ifdef ETIMEDOUT
  CHECK(WsaErrorFromErrno(ETIMEDOUT) == 10060u);
#endif
#ifdef EHOSTUNREACH
  CHECK(WsaErrorFromErrno(EHOSTUNREACH) == 10065u);
#endif
#ifdef EACCES
  CHECK(WsaErrorFromErrno(EACCES) == 10013u);
#endif
#ifdef EFAULT
  CHECK(WsaErrorFromErrno(EFAULT) == 10014u);
#endif
#ifdef EINVAL
  CHECK(WsaErrorFromErrno(EINVAL) == 10022u);
#endif
#ifdef EMFILE
  CHECK(WsaErrorFromErrno(EMFILE) == 10024u);
#endif
  CHECK(WsaErrorFromErrno(999999) == 10022u);
}

TEST_CASE("system_link: GuestToHostPort maps guest ports by offset", "[system][system_link]") {
  CHECK(GuestToHostPort(1000, 0) == 1000);
  CHECK(GuestToHostPort(0, 40000) == 0);
  CHECK(GuestToHostPort(14001, 40000) == 14001);
  CHECK(GuestToHostPort(1024, 40000) == 1024);
  CHECK(GuestToHostPort(1000, 65000) == 1000);

  CHECK(GuestToHostPort(1000, 40000) == 41000);
  CHECK(GuestToHostPort(1, 40000) == 40001);
  CHECK(GuestToHostPort(1023, 40000) == 41023);
}

TEST_CASE("system_link: HostToGuestPort reverses the port offset", "[system][system_link]") {
  CHECK(HostToGuestPort(41000, 0) == 41000);

  CHECK(HostToGuestPort(41000, 40000) == 1000);
  CHECK(HostToGuestPort(40001, 40000) == 1);
  CHECK(HostToGuestPort(41023, 40000) == 1023);

  CHECK(HostToGuestPort(40000, 40000) == 40000);
  CHECK(HostToGuestPort(41024, 40000) == 41024);
  CHECK(HostToGuestPort(14001, 40000) == 14001);
}

TEST_CASE("system_link: HostToGuestPort round-trips GuestToHostPort for privileged ports",
          "[system][system_link]") {
  for (uint32_t p = 1; p < 1024; ++p) {
    uint16_t guest = static_cast<uint16_t>(p);
    CHECK(HostToGuestPort(GuestToHostPort(guest, 40000), 40000) == guest);
  }
}

TEST_CASE("system_link: IsUsablePortOffset validates port offsets", "[system][system_link]") {
  CHECK(IsUsablePortOffset(0));
  CHECK(IsUsablePortOffset(1024));
  CHECK(IsUsablePortOffset(40000));
  CHECK(IsUsablePortOffset(64512));

  CHECK(!IsUsablePortOffset(1));
  CHECK(!IsUsablePortOffset(1023));
  CHECK(!IsUsablePortOffset(64513));
  CHECK(!IsUsablePortOffset(70000));
}

TEST_CASE("system_link: WsaErrorFromConnectErrno maps connect errnos", "[system][system_link]") {
#ifdef EINPROGRESS
  CHECK(WsaErrorFromConnectErrno(EINPROGRESS) == 10035u);
#endif
#ifdef ECONNREFUSED
  CHECK(WsaErrorFromConnectErrno(ECONNREFUSED) == 10054u);
#endif
#ifdef EAGAIN
  CHECK(WsaErrorFromConnectErrno(EAGAIN) == 10035u);
#endif
}

TEST_CASE("system_link: WsaErrorFromErrno maps the documented subset", "[system][system_link]") {
  CHECK(WsaErrorFromErrno(0) == 0u);
#ifdef EAGAIN
  CHECK(WsaErrorFromErrno(EAGAIN) == 10035u);
#endif
#ifdef EMSGSIZE
  CHECK(WsaErrorFromErrno(EMSGSIZE) == 10040u);
#endif
#ifdef ENETUNREACH
  CHECK(WsaErrorFromErrno(ENETUNREACH) == 10051u);
#endif
#ifdef ECONNRESET
  CHECK(WsaErrorFromErrno(ECONNRESET) == 10054u);
#endif
#ifdef ECONNREFUSED
  CHECK(WsaErrorFromErrno(ECONNREFUSED) == 10054u);
#endif
#ifdef EHOSTUNREACH
  CHECK(WsaErrorFromErrno(EHOSTUNREACH) == 10065u);
#endif
#ifdef EADDRINUSE
  CHECK(WsaErrorFromErrno(EADDRINUSE) == 10048u);
#endif
#ifdef EACCES
  CHECK(WsaErrorFromErrno(EACCES) == 10013u);
#endif
}

TEST_CASE("system_link: SplitListTokens splits on delimiters", "[system][system_link]") {
  std::vector<std::string_view> expected{"a.com:1", "1.2.3.4", "b", "c"};
  CHECK(SplitListTokens("a.com:1, 1.2.3.4;; b\tc") == expected);
  CHECK(SplitListTokens("") == std::vector<std::string_view>{});
}

TEST_CASE("system_link: ParseIpv4Cidr parses addresses with optional prefix",
          "[system][system_link]") {
  Subnet defaulted{0xC0A80A0Au /* 192.168.10.10 */, 24};
  CHECK(ParseIpv4Cidr("192.168.10.10", 24) == defaulted);

  Subnet slash8{0x0A000001u /* 10.0.0.1 */, 8};
  CHECK(ParseIpv4Cidr(" 10.0.0.1/8 ", 24) == slash8);

  Subnet host{0x01020304u /* 1.2.3.4 */, 32};
  CHECK(ParseIpv4Cidr("1.2.3.4/32", 24) == host);

  Subnet any{0x01020304u /* 1.2.3.4 */, 0};
  CHECK(ParseIpv4Cidr("1.2.3.4/0", 24) == any);

  CHECK(!ParseIpv4Cidr("1.2.3.4/33", 24).has_value());
  CHECK(!ParseIpv4Cidr("1.2.3.4/", 24).has_value());
  CHECK(!ParseIpv4Cidr("1.2.3.4/x", 24).has_value());
  CHECK(!ParseIpv4Cidr("1.2.3/24", 24).has_value());
  CHECK(!ParseIpv4Cidr("1.2.3.4/24/1", 24).has_value());
  CHECK(!ParseIpv4Cidr("", 24).has_value());
}

TEST_CASE("system_link: FormatSubnet formats subnets", "[system][system_link]") {
  Subnet subnet{0xC0A80A0Au /* 192.168.10.10 */, 24};

  CHECK(FormatSubnet(subnet) == "192.168.10.10/24");
}

TEST_CASE("system_link: FormatEndpoint formats endpoints", "[system][system_link]") {
  Endpoint endpoint{0x01020304u /* 1.2.3.4 */, 41000};

  CHECK(FormatEndpoint(endpoint) == "1.2.3.4:41000");
}

TEST_CASE("system_link: PrefixMask builds prefix masks", "[system][system_link]") {
  CHECK(PrefixMask(0) == 0u);
  CHECK(PrefixMask(8) == 0xFF000000u);
  CHECK(PrefixMask(24) == 0xFFFFFF00u);
  CHECK(PrefixMask(32) == 0xFFFFFFFFu);
  CHECK(PrefixMask(-1) == 0u);
  CHECK(PrefixMask(40) == 0xFFFFFFFFu);
}

TEST_CASE("system_link: IsInSubnet tests subnet membership", "[system][system_link]") {
  Subnet subnet{0xC0A80A0Au /* 192.168.10.10 */, 24};

  CHECK(IsInSubnet(0xC0A80A72u /* 192.168.10.114 */, subnet));
  CHECK(!IsInSubnet(0xC0A80B01u /* 192.168.11.1 */, subnet));

  Subnet any{0x01020304u /* 1.2.3.4 */, 0};
  CHECK(IsInSubnet(0x08080808u /* 8.8.8.8 */, any));
  CHECK(IsInSubnet(0u, any));
}

TEST_CASE("system_link: IsLoopbackIp detects loopback addresses", "[system][system_link]") {
  CHECK(IsLoopbackIp(0x7F000001u));
  CHECK(IsLoopbackIp(0x7F000002u));
  CHECK(!IsLoopbackIp(0x80000001u /* 128.0.0.1 */));
}

TEST_CASE("system_link: LanBroadcastIp selects the broadcast address", "[system][system_link]") {
  Subnet lan{0xC0A80A72u /* 192.168.10.114 */, 24};
  CHECK(LanBroadcastIp(lan, true) == 0xC0A80AFFu /* 192.168.10.255 */);
  CHECK(LanBroadcastIp(lan, false) == 0xFFFFFFFFu);

  Subnet loopback{0x7F000001u, 8};
  CHECK(LanBroadcastIp(loopback, true) == 0xFFFFFFFFu);

  Subnet narrow31{0xC0A80A72u /* 192.168.10.114 */, 31};
  CHECK(LanBroadcastIp(narrow31, true) == 0xFFFFFFFFu);

  Subnet host32{0xC0A80A72u /* 192.168.10.114 */, 32};
  CHECK(LanBroadcastIp(host32, true) == 0xFFFFFFFFu);

  Subnet zero{0u, 24};
  CHECK(LanBroadcastIp(zero, true) == 0xFFFFFFFFu);

  Subnet ten{0x0A010203u /* 10.1.2.3 */, 8};
  CHECK(LanBroadcastIp(ten, true) == 0x0AFFFFFFu /* 10.255.255.255 */);
}

TEST_CASE("system_link: ResolveLocalSubnet prefers configured sources in order",
          "[system][system_link]") {
  Subnet configured{0x01020304u /* 1.2.3.4 */, 16};
  Subnet environment{0x05060708u /* 5.6.7.8 */, 8};
  uint32_t bind_ip = 0x09090909u;      // 9.9.9.9
  uint32_t detected_ip = 0x0A000001u;  // 10.0.0.1
  Subnet from_bind{0x09090909u /* 9.9.9.9 */, 24};
  Subnet from_detected{0x0A000001u /* 10.0.0.1 */, 24};
  Subnet fallback{0x7F000001u, 8};

  CHECK(ResolveLocalSubnet(configured, environment, bind_ip, detected_ip) == configured);
  CHECK(ResolveLocalSubnet(std::nullopt, environment, bind_ip, detected_ip) == environment);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, bind_ip, detected_ip) == from_bind);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt, detected_ip) ==
        from_detected);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt, std::nullopt) == fallback);
}

TEST_CASE("system_link: ParseHostPort parses hosts with optional ports", "[system][system_link]") {
  HostPort bare{"gow2.suruworks.com", 0};
  CHECK(ParseHostPort("gow2.suruworks.com") == bare);

  HostPort with_port{"1.2.3.4", 14001};
  CHECK(ParseHostPort("1.2.3.4:14001") == with_port);

  HostPort trimmed{"host-1", 65535};
  CHECK(ParseHostPort(" host-1:65535 ") == trimmed);
}

TEST_CASE("system_link: ParseHostPort rejects malformed input", "[system][system_link]") {
  CHECK(!ParseHostPort("").has_value());
  CHECK(!ParseHostPort(":1000").has_value());
  CHECK(!ParseHostPort("host:").has_value());
  CHECK(!ParseHostPort("host:0").has_value());
  CHECK(!ParseHostPort("host:65536").has_value());
  CHECK(!ParseHostPort("host:123456").has_value());
  CHECK(!ParseHostPort("host:12a").has_value());
  CHECK(!ParseHostPort("a:b:c").has_value());
  CHECK(!ParseHostPort("bad host").has_value());
  CHECK(!ParseHostPort("[::1]:1000").has_value());
  CHECK(!ParseHostPort("host/24").has_value());
}

TEST_CASE("system_link: ParseHostPortList parses token lists", "[system][system_link]") {
  std::vector<HostPort> expected{{"gow2.suruworks.com", 0}, {"1.2.3.4", 24001}, {"bad", 0}};

  CHECK(ParseHostPortList("gow2.suruworks.com, 1.2.3.4:24001;bad host:x") == expected);
}

TEST_CASE("system_link: IsAllowedSource filters by allow list", "[system][system_link]") {
  std::vector<Subnet> empty{};
  CHECK(IsAllowedSource(0x08080808u /* 8.8.8.8 */, empty));

  std::vector<Subnet> allowed{{0xC0A80A00u /* 192.168.10.0 */, 24},
                              {0x01020304u /* 1.2.3.4 */, 32}};
  CHECK(IsAllowedSource(0xC0A80A72u /* 192.168.10.114 */, allowed));
  CHECK(IsAllowedSource(0x01020304u /* 1.2.3.4 */, allowed));
  CHECK(!IsAllowedSource(0x01020305u /* 1.2.3.5 */, allowed));
  CHECK(IsAllowedSource(0x7F000001u, allowed));

  std::vector<Subnet> fail_closed{{0u, 32}};
  CHECK(!IsAllowedSource(0x08080808u /* 8.8.8.8 */, fail_closed));
  CHECK(IsAllowedSource(0x7F000001u, fail_closed));
}

TEST_CASE("system_link: DefaultAllowList combines local subnet and peers", "[system][system_link]") {
  std::vector<uint32_t> peer_ips{0x01020304u /* 1.2.3.4 */, 0u, 0x01020304u /* 1.2.3.4 */,
                                 0x05060708u /* 5.6.7.8 */};
  Subnet local{0xC0A80A0Au /* 192.168.10.10 */, 24};
  std::vector<Subnet> expected{{0xC0A80A0Au /* 192.168.10.10 */, 24},
                               {0x01020304u /* 1.2.3.4 */, 32},
                               {0x05060708u /* 5.6.7.8 */, 32}};

  CHECK(DefaultAllowList(peer_ips, local) == expected);
}

TEST_CASE("system_link: BroadcastFanOut sends to LAN broadcast by default",
          "[system][system_link]") {
  std::vector<uint32_t> no_targets{};
  std::vector<Endpoint> no_peers{};
  std::vector<Endpoint> no_sources{};
  std::vector<uint32_t> no_self{};
  std::vector<Endpoint> expected{{0xC0A80AFFu /* 192.168.10.255 */, 14001}};

  CHECK(BroadcastFanOut(14001, kLan, no_targets, no_peers,
                        no_sources, no_self) == expected);
}

TEST_CASE("system_link: BroadcastFanOut uses broadcast targets minus self",
          "[system][system_link]") {
  std::vector<uint32_t> targets{0x7F000001u, 0x7F000002u, 0x7F000003u};
  std::vector<uint32_t> self{0x7F000002u};
  std::vector<Endpoint> no_peers{};
  std::vector<Endpoint> no_sources{};
  std::vector<Endpoint> expected{{0x7F000001u, 14001}, {0x7F000003u, 14001}};

  CHECK(BroadcastFanOut(14001, kLan, targets, no_peers,
                        no_sources, self) == expected);
}

TEST_CASE("system_link: BroadcastFanOut appends peers after the LAN entry",
          "[system][system_link]") {
  std::vector<uint32_t> no_targets{};
  std::vector<Endpoint> peers{{0x01020304u /* 1.2.3.4 */, 0},
                              {0x05060708u /* 5.6.7.8 */, 24001}};
  std::vector<Endpoint> no_sources{};
  std::vector<uint32_t> no_self{};
  std::vector<Endpoint> expected{{0xC0A80AFFu /* 192.168.10.255 */, 14001},
                                 {0x01020304u /* 1.2.3.4 */, 14001},
                                 {0x05060708u /* 5.6.7.8 */, 24001}};

  CHECK(BroadcastFanOut(14001, kLan, no_targets, peers,
                        no_sources, no_self) == expected);
}

TEST_CASE("system_link: BroadcastFanOut appends recent sources and skips port zero",
          "[system][system_link]") {
  std::vector<uint32_t> no_targets{};
  std::vector<Endpoint> no_peers{};
  std::vector<Endpoint> sources{{0x09090909u /* 9.9.9.9 */, 51234},
                                {0x09090909u /* 9.9.9.9 */, 0}};
  std::vector<uint32_t> no_self{};
  std::vector<Endpoint> expected{{0xC0A80AFFu /* 192.168.10.255 */, 14001},
                                 {0x09090909u /* 9.9.9.9 */, 51234}};

  CHECK(BroadcastFanOut(14001, kLan, no_targets, no_peers,
                        sources, no_self) == expected);
}

TEST_CASE("system_link: BroadcastFanOut skips self and repeated endpoints in order",
          "[system][system_link]") {
  std::vector<uint32_t> no_targets{};
  std::vector<Endpoint> peers{{0x01020304u /* 1.2.3.4 */, 14001},
                              {0x05060708u /* 5.6.7.8 */, 24001}};
  std::vector<Endpoint> sources{{0x05060708u /* 5.6.7.8 */, 24001},
                                {0x09090909u /* 9.9.9.9 */, 1111}};
  std::vector<uint32_t> self{0x01020304u /* 1.2.3.4 */};
  std::vector<Endpoint> expected{{0xC0A80AFFu /* 192.168.10.255 */, 14001},
                                 {0x05060708u /* 5.6.7.8 */, 24001},
                                 {0x09090909u /* 9.9.9.9 */, 1111}};

  CHECK(BroadcastFanOut(14001, kLan, no_targets, peers,
                        sources, self) == expected);
}

TEST_CASE("system_link: PeerInAddr selects the reachable peer address", "[system][system_link]") {
  Subnet local{0xC0A80A0Au /* 192.168.10.10 */, 24};

  CHECK(PeerInAddr(0xC0A80A72u /* 192.168.10.114 */, 0u, local, false) ==
        0xC0A80A72u /* 192.168.10.114 */);
  CHECK(PeerInAddr(0xC0A80A72u /* 192.168.10.114 */, 0x01020304u /* 1.2.3.4 */, local,
                   true) == 0xC0A80A72u /* 192.168.10.114 */);
  CHECK(PeerInAddr(0xC0A80A05u /* 192.168.10.5 */, 0x01020304u /* 1.2.3.4 */, local,
                   false) == 0x01020304u /* 1.2.3.4 */);
  CHECK(PeerInAddr(0x0A000005u /* 10.0.0.5 */, 0x01020304u /* 1.2.3.4 */, local, true) ==
        0x01020304u /* 1.2.3.4 */);
  CHECK(PeerInAddr(0x0A000005u /* 10.0.0.5 */, 0x01020304u /* 1.2.3.4 */, local, false) ==
        0x01020304u /* 1.2.3.4 */);
  CHECK(PeerInAddr(0x0A000005u /* 10.0.0.5 */, 0u, local, false) ==
        0x0A000005u /* 10.0.0.5 */);
  CHECK(PeerInAddr(0x7F000003u, 0x01020304u /* 1.2.3.4 */, local, false) == 0x7F000003u);
}

TEST_CASE("system_link: ClassifyGuestSocketOption classifies socket options",
          "[system][system_link]") {
  CHECK(ClassifyGuestSocketOption(0xFFFF, 4) == GuestSocketOption::kReuseAddr);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x0008) == GuestSocketOption::kKeepAlive);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x20) == GuestSocketOption::kBroadcast);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x0100) == GuestSocketOption::kOobInline);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x80) == GuestSocketOption::kLinger);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x1001) == GuestSocketOption::kSendBuffer);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x1002) == GuestSocketOption::kReceiveBuffer);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x1007) == GuestSocketOption::kError);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x1008) == GuestSocketOption::kType);
  CHECK(ClassifyGuestSocketOption(0xFFFF, 0x5801) == GuestSocketOption::kUnknown);
  CHECK(ClassifyGuestSocketOption(6, 1) == GuestSocketOption::kUnknown);
}

TEST_CASE("system_link: ClassifyGuestIoctl classifies ioctls", "[system][system_link]") {
  CHECK(ClassifyGuestIoctl(0x8004667E) == GuestSocketIoctl::kNonBlocking);
  CHECK(ClassifyGuestIoctl(0x4004667F) == GuestSocketIoctl::kBytesReadable);
  CHECK(ClassifyGuestIoctl(0x1234) == GuestSocketIoctl::kUnknown);
}

TEST_CASE("system_link: ReadGuestLinger reads linger options", "[system][system_link]") {
  std::array<uint8_t, 4> bytes{0x00, 0x01, 0x00, 0x1E};
  LingerOption expected{1, 30};

  CHECK(ReadGuestLinger(bytes) == expected);
}

TEST_CASE("system_link: ReadBigEndian32 reads big-endian values", "[system][system_link]") {
  std::array<uint8_t, 4> bytes{0x00, 0x02, 0x00, 0x00};

  CHECK(ReadBigEndian32(bytes) == 0x20000u);
}

TEST_CASE("system_link: WriteBigEndian32 writes big-endian values", "[system][system_link]") {
  std::array<uint8_t, 4> out{};
  std::array<uint8_t, 4> expected{1, 2, 3, 4};

  WriteBigEndian32(out, 0x01020304u);

  CHECK(out == expected);
}

TEST_CASE("system_link: Fnv1a64 hashes bytes", "[system][system_link]") {
  CHECK(Fnv1a64(std::span<const uint8_t>{}) == 0xCBF29CE484222325ull);

  std::array<uint8_t, 1> a{static_cast<uint8_t>('a')};
  CHECK(Fnv1a64(a) == 0xAF63DC4C8601EC8Cull);
}

TEST_CASE("system_link: BuildXnAddr lays out online address fields", "[system][system_link]") {
  MacAddress mac{0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

  XnAddrBytes out =
      BuildXnAddr(0xC0A80A72u /* 192.168.10.114 */, mac, 0x01020304u /* 1.2.3.4 */, 41000);
  CHECK(out[0] == 0xC0);
  CHECK(out[1] == 0xA8);
  CHECK(out[2] == 0x0A);
  CHECK(out[3] == 0x72);
  CHECK(out[4] == 0x01);
  CHECK(out[5] == 0x02);
  CHECK(out[6] == 0x03);
  CHECK(out[7] == 0x04);
  CHECK(out[8] == 0xA0);
  CHECK(out[9] == 0x28);
  CHECK(out[10] == 0x02);
  CHECK(out[11] == 0xAA);
  CHECK(out[12] == 0xBB);
  CHECK(out[13] == 0xCC);
  CHECK(out[14] == 0xDD);
  CHECK(out[15] == 0xEE);
  for (size_t i = 16; i < kXnAddrSize; ++i) {
    CHECK(out[i] == 0x00);
  }

  XnAddrBytes plain = BuildXnAddr(0xC0A80A72u /* 192.168.10.114 */, mac);
  for (size_t i = 4; i < 10; ++i) {
    CHECK(plain[i] == 0x00);
  }
}

TEST_CASE("system_link: beacon hello encodes and parses the versioned big-endian token",
          "[system][system_link]") {
  constexpr uint64_t token = 0x0123456789ABCDEFull;
  const std::array<uint8_t, kBeaconHelloSize> expected{
      'G', 'O', 'W', '2', 'H', 'E', 'L', 'O', 1, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD,
      0xEF};
  auto hello = EncodeBeaconHello(token);

  CHECK(hello == expected);
  CHECK(ParseBeaconHello(hello) == token);
}

TEST_CASE("system_link: beacon hello parser rejects malformed datagrams", "[system][system_link]") {
  auto hello = EncodeBeaconHello(0x0123456789ABCDEFull);
  std::array<uint8_t, kBeaconHelloSize - 1> short_hello{};
  std::array<uint8_t, kBeaconHelloSize + 1> long_hello{};

  CHECK_FALSE(ParseBeaconHello(short_hello).has_value());
  CHECK_FALSE(ParseBeaconHello(long_hello).has_value());
  hello[0] = 'X';
  CHECK_FALSE(ParseBeaconHello(hello).has_value());
  hello = EncodeBeaconHello(0x0123456789ABCDEFull);
  hello[8] = 2;
  CHECK_FALSE(ParseBeaconHello(hello).has_value());
}

TEST_CASE("system_link: BeaconHellos tracks source tokens, expiry and capacity",
          "[system][system_link]") {
  Endpoint a{0x01020304u, 14001};
  Endpoint other_port{0x01020304u, 14002};
  Endpoint c{0x05060708u, 14001};
  BeaconHellos hellos(5000, 2);

  hellos.Note(a, 0x1111, 100);
  CHECK(hellos.TokenFrom(a, 5100) == 0x1111);
  CHECK_FALSE(hellos.TokenFrom(a, 5101).has_value());
  CHECK_FALSE(hellos.TokenFrom(other_port, 200).has_value());

  hellos.Note(a, 0xAAAA, 200);
  hellos.Note(other_port, 0xBBBB, 300);
  CHECK(hellos.TokenFrom(a, 300) == 0xAAAA);
  CHECK(hellos.TokenFrom(other_port, 300) == 0xBBBB);
  hellos.Note(c, 0xCCCC, 400);
  CHECK_FALSE(hellos.TokenFrom(a, 400).has_value());
  CHECK(hellos.TokenFrom(other_port, 400) == 0xBBBB);
  CHECK(hellos.TokenFrom(c, 400) == 0xCCCC);
}

TEST_CASE("system_link: JudgeBeaconDatagram gates only announced beacon sources",
          "[system][system_link]") {
  Endpoint source{0x01020304u, 14001};
  BeaconHellos hellos(kBeaconHelloWindowMs, kBeaconHelloCapacity);
  std::array<uint8_t, 16> search{};
  auto matching_hello = EncodeBeaconHello(0x1111);
  auto other_hello = EncodeBeaconHello(0x2222);

  CHECK(JudgeBeaconDatagram(1000, 0x1111, matching_hello, source, hellos, 100) ==
        BeaconVerdict::kPass);
  CHECK_FALSE(hellos.TokenFrom(source, 100).has_value());
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, search, source, hellos, 200) ==
        BeaconVerdict::kNoHello);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0, search, source, hellos, 200) ==
        BeaconVerdict::kPass);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, matching_hello, source, hellos, 300) ==
        BeaconVerdict::kHello);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, search, source, hellos, 301) ==
        BeaconVerdict::kPass);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, other_hello, source, hellos, 400) ==
        BeaconVerdict::kHello);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, search, source, hellos, 401) ==
        BeaconVerdict::kMismatch);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, matching_hello, source, hellos, 6000) ==
        BeaconVerdict::kHello);
  CHECK(JudgeBeaconDatagram(kBeaconGuestPort, 0x1111, search, source, hellos, 11001) ==
        BeaconVerdict::kNoHello);
}

TEST_CASE("system_link: RecentSources tracks active sources with eviction and expiry",
          "[system][system_link]") {
  Endpoint a{0x01020304u /* 1.2.3.4 */, 1111};
  Endpoint b{0x05060708u /* 5.6.7.8 */, 2222};
  Endpoint c{0x09090909u /* 9.9.9.9 */, 3333};
  RecentSources sources(10000, 2);

  sources.Note(a, 0);
  sources.Note(b, 100);
  std::vector<Endpoint> first_two{a, b};
  CHECK(sources.Active(200) == first_two);

  sources.Note(a, 300);
  std::vector<Endpoint> refreshed = sources.Active(300);
  CHECK(refreshed.size() == 2u);
  size_t count_a = 0;
  for (const Endpoint& endpoint : refreshed) {
    if (endpoint == a) {
      ++count_a;
    }
  }
  CHECK(count_a == 1u);

  sources.Note(c, 400);
  std::vector<Endpoint> evicted{a, c};
  CHECK(sources.Active(400) == evicted);
  CHECK(sources.Active(10300) == evicted);
  std::vector<Endpoint> only_c{c};
  CHECK(sources.Active(10301) == only_c);
  CHECK(sources.Active(10401) == std::vector<Endpoint>{});

  CHECK(sources.HeardFrom(0x01020304u /* 1.2.3.4 */, 500));
  CHECK(!sources.HeardFrom(0x0A000001u /* 10.0.0.1 */, 500));
  CHECK(!sources.HeardFrom(0x01020304u /* 1.2.3.4 */, 20000));
}

TEST_CASE("system_link: DuplicateFilter drops recent repeats from the same source",
          "[system][system_link]") {
  Endpoint source{0x01020304u /* 1.2.3.4 */, 5000};
  Endpoint other{0x05060708u /* 5.6.7.8 */, 5000};
  std::array<uint8_t, 3> payload{1, 2, 3};
  std::array<uint8_t, 3> different{4, 5, 6};
  DuplicateFilter filter(250, 4);

  CHECK(!filter.IsDuplicate(source, payload, 0));
  CHECK(filter.IsDuplicate(source, payload, 100));
  CHECK(!filter.IsDuplicate(other, payload, 100));
  CHECK(!filter.IsDuplicate(source, different, 100));
  CHECK(!filter.IsDuplicate(source, payload, 400));
}

TEST_CASE("system_link: FirstContacts reports each socket and peer pair once",
          "[system][system_link]") {
  Endpoint peer{0x0A000001u /* 10.0.0.1 */, 14001};
  Endpoint other_port{0x0A000001u /* 10.0.0.1 */, 41000};
  FirstContacts contacts(8);

  CHECK(contacts.IsFirst(1, peer));
  CHECK(!contacts.IsFirst(1, peer));
  CHECK(contacts.IsFirst(2, peer));
  CHECK(contacts.IsFirst(1, other_port));
  CHECK(!contacts.IsFirst(2, peer));
}

TEST_CASE("system_link: FirstContacts evicts the oldest pair at capacity",
          "[system][system_link]") {
  Endpoint a{0x0A000001u /* 10.0.0.1 */, 14001};
  Endpoint b{0x0A000002u /* 10.0.0.2 */, 14001};
  Endpoint c{0x0A000003u /* 10.0.0.3 */, 14001};
  FirstContacts contacts(2);

  CHECK(contacts.IsFirst(1, a));
  CHECK(contacts.IsFirst(1, b));
  CHECK(contacts.IsFirst(1, c));
  CHECK(contacts.IsFirst(1, a));
  CHECK(!contacts.IsFirst(1, c));
}

TEST_CASE("system_link: FirstContacts with capacity zero keeps the last pair",
          "[system][system_link]") {
  Endpoint a{0x0A000001u /* 10.0.0.1 */, 14001};
  FirstContacts contacts(0);

  CHECK(contacts.IsFirst(1, a));
  CHECK(!contacts.IsFirst(1, a));
}

TEST_CASE("system_link: LanBroadcastIps adds the directed broadcast on a real LAN",
          "[system][system_link]") {
  Subnet lan{0xC0A80A0Au /* 192.168.10.10 */, 24};
  std::vector<uint32_t> limited{0xFFFFFFFFu};
  std::vector<uint32_t> directed{0xC0A80AFFu /* 192.168.10.255 */};
  std::vector<uint32_t> both{0xC0A80AFFu /* 192.168.10.255 */, 0xFFFFFFFFu};

  CHECK(LanBroadcastIps(Subnet{0x7F000001u, 8}, false) == limited);
  CHECK(LanBroadcastIps(Subnet{0x7F000001u, 8}, true) == limited);
  CHECK(LanBroadcastIps(lan, true) == directed);
  CHECK(LanBroadcastIps(lan, false) == both);
  CHECK(LanBroadcastIps(Subnet{0xC0A80A0Au /* 192.168.10.10 */, 32}, false) == limited);
  CHECK(LanBroadcastIps(Subnet{0u, 24}, false) == limited);
}

TEST_CASE("system_link: BroadcastFanOut sends to every LAN broadcast address",
          "[system][system_link]") {
  std::vector<uint32_t> lan{0xC0A80AFFu /* 192.168.10.255 */, 0xFFFFFFFFu};
  std::vector<uint32_t> no_targets{};
  std::vector<uint32_t> targets{0x7F000002u};
  std::vector<Endpoint> none{};
  std::vector<uint32_t> no_self{};
  std::vector<Endpoint> expected_lan{{0xC0A80AFFu /* 192.168.10.255 */, 14001},
                                     {0xFFFFFFFFu, 14001}};
  std::vector<Endpoint> expected_targets{{0x7F000002u, 14001}};

  CHECK(BroadcastFanOut(14001, lan, no_targets, none, none, no_self) == expected_lan);
  CHECK(BroadcastFanOut(14001, lan, targets, none, none, no_self) == expected_targets);
}

TEST_CASE("system_link: ResolveLocalSubnet ignores a detected address that is not on a LAN",
          "[system][system_link]") {
  Subnet fallback{0x7F000001u, 8};

  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt, 0x7F000001u) == fallback);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt, 0u) == fallback);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt, 0xFFFFFFFFu) == fallback);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt, 0xE0000001u) == fallback);
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, std::nullopt,
                           0xC0A80A0Au /* 192.168.10.10 */) ==
        Subnet{0xC0A80A0Au /* 192.168.10.10 */, 24});
  CHECK(ResolveLocalSubnet(std::nullopt, std::nullopt, 0x7F000002u, std::nullopt) ==
        Subnet{0x7F000002u, 24});
}

TEST_CASE("system_link: EffectiveAllowList fails closed when the host is public",
          "[system][system_link]") {
  AllowPolicyInput input;
  input.public_address_set = true;
  input.local_subnet = Subnet{0xC0A80A0Au /* 192.168.10.10 */, 24};
  std::vector<Subnet> expected{input.local_subnet};

  std::vector<Subnet> allowed = EffectiveAllowList(input);

  CHECK(allowed == expected);
  CHECK(!IsAllowedSource(0x08080808u /* 8.8.8.8 */, allowed));
  CHECK(IsAllowedSource(0xC0A80A72u /* 192.168.10.114 */, allowed));
  CHECK(IsAllowedSource(0x7F000001u, allowed));
}

TEST_CASE("system_link: EffectiveAllowList never opens up while names are unresolved",
          "[system][system_link]") {
  Subnet local{0xC0A80A0Au /* 192.168.10.10 */, 24};
  std::vector<Subnet> only_local{local};
  AllowPolicyInput peers_only;
  peers_only.peers_set = true;
  peers_only.local_subnet = local;
  AllowPolicyInput allow_only;
  allow_only.allow_list_set = true;
  allow_only.local_subnet = local;

  CHECK(EffectiveAllowList(peers_only) == only_local);
  CHECK(EffectiveAllowList(allow_only) == only_local);
}

TEST_CASE("system_link: EffectiveAllowList adds the allow list after the local subnet",
          "[system][system_link]") {
  Subnet local{0xC0A80A0Au /* 192.168.10.10 */, 24};
  Subnet friend_ip{0xCB007107u /* 203.0.113.7 */, 32};
  Subnet friend_net{0xC6336400u /* 198.51.100.0 */, 24};
  AllowPolicyInput input;
  input.allow_list_set = true;
  input.allow_list = {friend_ip, friend_net, friend_ip};
  input.peers_set = true;
  input.peer_ips = {0x01020304u /* 1.2.3.4 */};
  input.local_subnet = local;
  std::vector<Subnet> expected{local, friend_ip, friend_net};

  std::vector<Subnet> allowed = EffectiveAllowList(input);

  CHECK(allowed == expected);
  CHECK(IsAllowedSource(0xCB007107u /* 203.0.113.7 */, allowed));
  CHECK(!IsAllowedSource(0xCB007108u /* 203.0.113.8 */, allowed));
  CHECK(!IsAllowedSource(0x01020304u /* 1.2.3.4 */, allowed));
}

TEST_CASE("system_link: EffectiveAllowList lets everyone in on a plain LAN",
          "[system][system_link]") {
  AllowPolicyInput input;
  input.local_subnet = Subnet{0xC0A80A0Au /* 192.168.10.10 */, 24};

  std::vector<Subnet> allowed = EffectiveAllowList(input);

  CHECK(allowed.empty());
  CHECK(IsAllowedSource(0x08080808u /* 8.8.8.8 */, allowed));
}

TEST_CASE("system_link: EffectiveAllowList with peers allows them and the local subnet",
          "[system][system_link]") {
  Subnet local{0xC0A80A0Au /* 192.168.10.10 */, 24};
  AllowPolicyInput input;
  input.peers_set = true;
  input.public_address_set = true;
  input.peer_ips = {0x01020304u /* 1.2.3.4 */};
  input.local_subnet = local;
  std::vector<Subnet> expected{local, {0x01020304u /* 1.2.3.4 */, 32}};

  CHECK(EffectiveAllowList(input) == expected);
}

TEST_CASE("system_link: RecentSources::Latest lists the newest senders first",
          "[system][system_link]") {
  RecentSources sources(1000, 8);
  Endpoint a{0x0A000001u /* 10.0.0.1 */, 14001};
  Endpoint b{0x0A000002u /* 10.0.0.2 */, 14001};
  Endpoint c{0x0A000003u /* 10.0.0.3 */, 14001};
  sources.Note(a, 100);
  sources.Note(b, 1500);
  sources.Note(c, 1800);
  std::vector<Endpoint> newest_two{c, b};
  std::vector<Endpoint> newest_one{c};

  CHECK(sources.Latest(2000, 4) == newest_two);
  CHECK(sources.Latest(2000, 1) == newest_one);
  CHECK(sources.Latest(2000, 0).empty());
  CHECK(sources.Latest(3000, 4).empty());
}

TEST_CASE("system_link: ReplyBudget allows two copies per second per source",
          "[system][system_link]") {
  ReplyBudget budget(2, 64);
  Endpoint a{0x0A000001u /* 10.0.0.1 */, 14001};
  Endpoint b{0x0A000002u /* 10.0.0.2 */, 14001};

  CHECK(budget.TryTake(a, 1000));
  CHECK(budget.TryTake(a, 1000));
  CHECK(!budget.TryTake(a, 1000));
  CHECK(budget.TryTake(b, 1000));
  CHECK(!budget.TryTake(a, 1499));
  CHECK(budget.TryTake(a, 1500));
  CHECK(!budget.TryTake(a, 1500));
  CHECK(budget.TryTake(a, 2600));
  CHECK(budget.TryTake(a, 2600));
  CHECK(!budget.TryTake(a, 2600));
}

TEST_CASE("system_link: ReplyBudget limits and edge cases", "[system][system_link]") {
  Endpoint a{0x0A000001u /* 10.0.0.1 */, 14001};
  Endpoint b{0x0A000002u /* 10.0.0.2 */, 14001};
  ReplyBudget no_rate(0, 64);
  ReplyBudget no_room(2, 0);
  ReplyBudget one_slot(2, 1);
  ReplyBudget clock(2, 64);

  CHECK(!no_rate.TryTake(a, 1000));
  CHECK(!no_room.TryTake(a, 1000));

  CHECK(one_slot.TryTake(a, 1000));
  CHECK(one_slot.TryTake(a, 1000));
  CHECK(!one_slot.TryTake(a, 1000));
  CHECK(one_slot.TryTake(b, 1000));
  CHECK(one_slot.TryTake(a, 1000));

  CHECK(clock.TryTake(a, 1000));
  CHECK(clock.TryTake(a, 1000));
  CHECK(!clock.TryTake(a, 500));
}

TEST_CASE("system_link: ReplyCopyTargets copies only beacon broadcasts within budget",
          "[system][system_link]") {
  Endpoint a{0x0A000001u /* 10.0.0.1 */, 14001};
  Endpoint no_port{0x0A000002u /* 10.0.0.2 */, 0};
  Endpoint c{0x0A000003u /* 10.0.0.3 */, 50000};
  std::vector<Endpoint> latest{a, no_port, c};
  std::vector<Endpoint> expected{a, c};
  ReplyBudget budget(2, 64);

  CHECK(ReplyCopyTargets(1000, latest, budget, 1000).empty());
  CHECK(ReplyCopyTargets(14001, latest, budget, 1000) == expected);
  CHECK(ReplyCopyTargets(14001, latest, budget, 1000) == expected);
  CHECK(ReplyCopyTargets(14001, latest, budget, 1000).empty());
}

TEST_CASE("system_link: NamesToResolve collects host names only", "[system][system_link]") {
  std::vector<HostPort> peers{{"gow2.suruworks.com", 0}, {"1.2.3.4", 14001}, {"b.example", 0}};
  std::vector<std::string> allow{"10.0.0.0/8", "5.6.7.8", "a.example", "gow2.suruworks.com",
                                 "bad name!"};
  std::optional<HostPort> public_host = HostPort{"c.example", 14001};
  std::vector<std::string> expected{"a.example", "b.example", "c.example",
                                    "gow2.suruworks.com"};

  CHECK(NamesToResolve(peers, allow, public_host) == expected);
  CHECK(NamesToResolve({}, {}, HostPort{"9.9.9.9", 0}).empty());
}

TEST_CASE("system_link: RefreshNames keeps the last good addresses", "[system][system_link]") {
  std::map<std::string, std::vector<uint32_t>> answers{
      {"fresh.example", {0x01010101u /* 1.1.1.1 */}},
      {"same.example", {0x02020202u /* 2.2.2.2 */}},
  };
  int calls = 0;
  HostLookup lookup = [&answers, &calls](const std::string& host) {
    ++calls;
    auto found = answers.find(host);
    return found == answers.end() ? std::vector<uint32_t>{} : found->second;
  };
  NameTable previous{
      {"fresh.example", {0x09090909u /* 9.9.9.9 */}},
      {"same.example", {0x02020202u /* 2.2.2.2 */}},
      {"down.example", {0x03030303u /* 3.3.3.3 */}},
      {"dropped.example", {0x04040404u /* 4.4.4.4 */}},
  };
  std::vector<std::string> names{"down.example", "fresh.example", "never.example",
                                 "same.example"};
  NameTable expected_table{
      {"down.example", {0x03030303u /* 3.3.3.3 */}},
      {"fresh.example", {0x01010101u /* 1.1.1.1 */}},
      {"same.example", {0x02020202u /* 2.2.2.2 */}},
  };

  NameRefresh refresh = RefreshNames(previous, names, lookup);

  CHECK(calls == 4);
  CHECK(refresh.table == expected_table);
  CHECK(refresh.changed == std::vector<std::string>{"fresh.example"});
  CHECK(refresh.stale == std::vector<std::string>{"down.example"});
  CHECK(refresh.missing == std::vector<std::string>{"never.example"});
  CHECK(previous.count("dropped.example") == 1);
}

TEST_CASE("system_link: peer, allow and public lists come from the name table",
          "[system][system_link]") {
  NameTable names{{"friend.example", {0xCB007107u /* 203.0.113.7 */,
                                      0xCB007108u /* 203.0.113.8 */}}};
  std::vector<HostPort> peers{{"friend.example", 14001}, {"1.2.3.4", 0},
                              {"unknown.example", 0}};
  std::vector<std::string> allow{"10.0.0.0/8", "friend.example", "unknown.example", "bad name!",
                                 "10.0.0.0/8"};
  std::vector<Endpoint> expected_peers{{0xCB007107u /* 203.0.113.7 */, 14001},
                                       {0xCB007108u /* 203.0.113.8 */, 14001},
                                       {0x01020304u /* 1.2.3.4 */, 0}};
  std::vector<Subnet> expected_allow{{0x0A000000u /* 10.0.0.0 */, 8},
                                     {0xCB007107u /* 203.0.113.7 */, 32},
                                     {0xCB007108u /* 203.0.113.8 */, 32}};

  CHECK(HostAddresses("5.6.7.8", names) == std::vector<uint32_t>{0x05060708u});
  CHECK(HostAddresses("unknown.example", names).empty());
  CHECK(PeerEndpointsFrom(peers, names) == expected_peers);
  CHECK(AllowListFrom(allow, names) == expected_allow);
  CHECK(PublicEndpointFrom(HostPort{"friend.example", 14001}, names) ==
        Endpoint{0xCB007107u /* 203.0.113.7 */, 14001});
  CHECK(!PublicEndpointFrom(std::nullopt, names).has_value());
  CHECK(!PublicEndpointFrom(HostPort{"unknown.example", 0}, names).has_value());
}
