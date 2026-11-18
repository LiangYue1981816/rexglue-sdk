#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/render_target/native_surfaces.h>

namespace ns = rex::graphics::native_surfaces;

TEST_CASE("depth to depth transfers are kept", "[native_surfaces]") {
  CHECK(ns::KeepTransfer({true, 0, 0}, {true, 1, 0}));
  CHECK(ns::KeepTransfer({true, 1, 0}, {true, 0, 0}));
  CHECK(ns::KeepTransfer({true, 0, 2}, {true, 1, 0}));
  CHECK(ns::KeepTransfer({true, 1, 0}, {true, 0, 2}));
}

TEST_CASE("k_16_16 colour to depth transfers are kept", "[native_surfaces]") {
  CHECK(ns::KeepTransfer({false, 4, 0}, {true, 0, 0}));
  CHECK(ns::KeepTransfer({false, 4, 0}, {true, 1, 0}));
  CHECK_FALSE(ns::KeepTransfer({true, 0, 0}, {false, 4, 0}));
  CHECK_FALSE(ns::KeepTransfer({false, 4, 0}, {false, 6, 0}));
}

TEST_CASE("transfers involving colour are dropped", "[native_surfaces]") {
  CHECK_FALSE(ns::KeepTransfer({false, 3, 0}, {true, 0, 0}));
  CHECK_FALSE(ns::KeepTransfer({true, 1, 0}, {false, 6, 0}));
  CHECK_FALSE(ns::KeepTransfer({false, 3, 0}, {false, 6, 0}));
}

TEST_CASE("drop counters report rates and reset", "[native_surfaces]") {
  ns::DropCounters counters;
  CHECK(counters.empty());
  for (int i = 0; i < 24; ++i) {
    counters.Record({false, 3, 0}, {true, 0, 0}, 700);
  }
  for (int i = 0; i < 4; ++i) {
    counters.Record({true, 1, 0}, {false, 6, 0}, 100);
  }
  CHECK_FALSE(counters.empty());
  CHECK(counters.ReportAndReset(2.0) ==
        "C3x1->D0x1 12.0/s 8400 tiles/s; D1x1->C6x1 2.0/s 200 tiles/s");
  CHECK(counters.empty());
  CHECK(counters.ReportAndReset(1.0) == "");
}
