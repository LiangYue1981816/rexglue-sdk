#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xam/host_ui_route.h>

using namespace rex::kernel::xam;

TEST_CASE("host_ui_route: no command keeps the device selector headless", "[kernel][host_ui]") {
  CHECK_FALSE(RouteDeviceSelectorToHostUi(false, 0x200, 0x200));
  CHECK_FALSE(RouteDeviceSelectorToHostUi(false, 0, 0));
}

TEST_CASE("host_ui_route: matching flags route a command to the host UI", "[kernel][host_ui]") {
  CHECK(RouteDeviceSelectorToHostUi(true, 0x200, 0x200));
  CHECK(RouteDeviceSelectorToHostUi(true, 0x201, 0x200));
}

TEST_CASE("host_ui_route: missing flags keep the device selector headless", "[kernel][host_ui]") {
  CHECK_FALSE(RouteDeviceSelectorToHostUi(true, 0, 0x200));
  CHECK_FALSE(RouteDeviceSelectorToHostUi(true, 0x100, 0x200));
}

TEST_CASE("host_ui_route: zero mask routes every call with a command", "[kernel][host_ui]") {
  CHECK(RouteDeviceSelectorToHostUi(true, 0, 0));
  CHECK(RouteDeviceSelectorToHostUi(true, 0x100, 0));
}
