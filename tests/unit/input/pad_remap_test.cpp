#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

#include <rex/input/pad_remap.h>

namespace pr = rex::input::pad_remap;

TEST_CASE("identity remapping leaves every bit unchanged", "[pad_remap]") {
  auto identity = pr::IdentitySources();
  CHECK(pr::Remap(0, identity) == 0);
  CHECK(pr::Remap(0xFFFF, identity) == 0xFFFF);
  for (uint16_t bit = 1; bit != 0; bit <<= 1) {
    CHECK(pr::Remap(bit, identity) == bit);
  }
}

TEST_CASE("swapping A and B sources swaps the bits", "[pad_remap]") {
  std::array<uint16_t, 14> sources = pr::IdentitySources();
  sources[0] = 0x2000;  // A driven by B
  sources[1] = 0x1000;  // B driven by A
  CHECK(pr::Remap(0x1000, sources) == 0x2000);  // physical A -> game B
  CHECK(pr::Remap(0x2000, sources) == 0x1000);  // physical B -> game A
  CHECK(pr::Remap(0x3000, sources) == 0x3000);  // both -> both
}

TEST_CASE("one physical drives two game buttons", "[pad_remap]") {
  std::array<uint16_t, 14> sources = pr::IdentitySources();
  sources[0] = 0x1000;  // A -> A (same)
  sources[2] = 0x1000;  // X -> A (physical A also drives X)
  CHECK(pr::Remap(0x1000, sources) == (0x1000 | 0x4000));
}

TEST_CASE("none bit is zero and drops the mapped button", "[pad_remap]") {
  std::array<uint16_t, 14> sources = pr::IdentitySources();
  sources[0] = 0;  // none for A
  CHECK(pr::BitForName("none") == 0);
  CHECK(pr::BitForName("bogus") == 0);
  CHECK(pr::Remap(0x1000, sources) == 0);
}

TEST_CASE("Guide bit passes through unchanged", "[pad_remap]") {
  auto identity = pr::IdentitySources();
  CHECK(pr::Remap(0x0400, identity) == 0x0400);
}

TEST_CASE("BitForName covers all 14 names and unknown returns 0", "[pad_remap]") {
  CHECK(pr::BitForName("a") == 0x1000);
  CHECK(pr::BitForName("b") == 0x2000);
  CHECK(pr::BitForName("x") == 0x4000);
  CHECK(pr::BitForName("y") == 0x8000);
  CHECK(pr::BitForName("lb") == 0x0100);
  CHECK(pr::BitForName("rb") == 0x0200);
  CHECK(pr::BitForName("back") == 0x0020);
  CHECK(pr::BitForName("start") == 0x0010);
  CHECK(pr::BitForName("ls") == 0x0040);
  CHECK(pr::BitForName("rs") == 0x0080);
  CHECK(pr::BitForName("dpad_up") == 0x0001);
  CHECK(pr::BitForName("dpad_down") == 0x0002);
  CHECK(pr::BitForName("dpad_left") == 0x0004);
  CHECK(pr::BitForName("dpad_right") == 0x0008);
  CHECK(pr::BitForName("bogus") == 0);
}
