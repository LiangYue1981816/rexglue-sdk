#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/render_target/native_surfaces.h>

namespace ns = rex::graphics::native_surfaces;

TEST_CASE("depth to depth transfers are kept", "[native_surfaces]") {
  CHECK(ns::KeepTransfer({true, 0, 0}, {true, 1, 0}));
  CHECK(ns::KeepTransfer({true, 1, 0}, {true, 0, 0}));
  CHECK(ns::KeepTransfer({true, 0, 2}, {true, 1, 0}));
  CHECK(ns::KeepTransfer({true, 1, 0}, {true, 0, 2}));
}

TEST_CASE("depth and k_16_16 colour transfers are kept both ways", "[native_surfaces]") {
  CHECK(ns::KeepTransfer({false, 4, 0}, {true, 0, 0}));
  CHECK(ns::KeepTransfer({false, 4, 0}, {true, 1, 0}));
  CHECK(ns::KeepTransfer({true, 0, 0}, {false, 4, 0}));
  CHECK(ns::KeepTransfer({true, 1, 0}, {false, 4, 0}));
  CHECK_FALSE(ns::KeepTransfer({false, 4, 0}, {false, 6, 0}));
}

TEST_CASE("transfers involving colour are dropped", "[native_surfaces]") {
  CHECK_FALSE(ns::KeepTransfer({false, 3, 0}, {true, 0, 0}));
  CHECK_FALSE(ns::KeepTransfer({true, 1, 0}, {false, 6, 0}));
  CHECK_FALSE(ns::KeepTransfer({false, 3, 0}, {false, 6, 0}));
  CHECK_FALSE(ns::KeepTransfer({true, 0, 0}, {false, 3, 0}));
  CHECK_FALSE(ns::KeepTransfer({true, 0, 0}, {false, 0, 0}));
  CHECK_FALSE(ns::KeepTransfer({true, 0, 0}, {false, 7, 0}));
  CHECK_FALSE(ns::KeepTransfer({false, 6, 0}, {true, 0, 0}));
  CHECK_FALSE(ns::KeepTransfer({false, 4, 0}, {false, 4, 0}));
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

TEST_CASE("empty and default policies match KeepTransfer", "[native_surfaces]") {
  const ns::TransferPolicy empty = ns::ParseTransferPolicy("");
  const ns::TransferPolicy def = ns::ParseTransferPolicy("default");
  CHECK(empty.use_default);
  CHECK(empty.rules.empty());
  CHECK(def.use_default);
  CHECK(def.rules.empty());
  const std::pair<ns::SurfaceKind, ns::SurfaceKind> pairs[] = {
      {{true, 0, 0}, {true, 1, 0}},  {{true, 1, 0}, {true, 0, 0}},
      {{true, 0, 2}, {true, 1, 0}},  {{true, 1, 0}, {true, 0, 2}},
      {{false, 4, 0}, {true, 0, 0}}, {{false, 4, 0}, {true, 1, 0}},
      {{true, 0, 0}, {false, 4, 0}}, {{false, 4, 0}, {false, 6, 0}},
      {{false, 3, 0}, {true, 0, 0}}, {{true, 1, 0}, {false, 6, 0}},
      {{false, 3, 0}, {false, 6, 0}},
  };
  for (const auto& [source, dest] : pairs) {
    CHECK(ns::KeepTransferWithPolicy(empty, source, dest) == ns::KeepTransfer(source, dest));
    CHECK(ns::KeepTransferWithPolicy(def, source, dest) == ns::KeepTransfer(source, dest));
  }
}

TEST_CASE("all keeps everything and none drops everything", "[native_surfaces]") {
  const ns::TransferPolicy all = ns::ParseTransferPolicy("all");
  CHECK_FALSE(all.use_default);
  CHECK(ns::KeepTransferWithPolicy(all, {false, 3, 0}, {false, 6, 0}));
  CHECK(ns::KeepTransferWithPolicy(all, {true, 0, 0}, {false, 3, 0}));
  const ns::TransferPolicy none = ns::ParseTransferPolicy("none");
  CHECK_FALSE(none.use_default);
  CHECK(none.rules.empty());
  CHECK_FALSE(ns::KeepTransferWithPolicy(none, {true, 0, 0}, {true, 1, 0}));
}

TEST_CASE("colour to depth policy", "[native_surfaces]") {
  const ns::TransferPolicy policy = ns::ParseTransferPolicy("C>D");
  CHECK(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {true, 0, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {true, 0, 0}, {false, 3, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {false, 6, 0}));
}

TEST_CASE("format-specific policy", "[native_surfaces]") {
  const ns::TransferPolicy policy = ns::ParseTransferPolicy("C3>C0");
  CHECK(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {false, 0, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {false, 2, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {false, 2, 0}, {false, 0, 0}));
}

TEST_CASE("rule plus default policy", "[native_surfaces]") {
  const ns::TransferPolicy policy = ns::ParseTransferPolicy("D>C6, default");
  CHECK(policy.use_default);
  CHECK(ns::KeepTransferWithPolicy(policy, {true, 1, 0}, {false, 6, 0}));
  CHECK(ns::KeepTransferWithPolicy(policy, {true, 0, 0}, {true, 1, 0}));
  CHECK(ns::KeepTransferWithPolicy(policy, {false, 4, 0}, {true, 0, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {true, 1, 0}, {false, 3, 0}));
}

TEST_CASE("malformed policies fall back to default", "[native_surfaces]") {
  for (const char* text : {"C3>", "X>D", "C3>D0x", "C99>D"}) {
    const ns::TransferPolicy policy = ns::ParseTransferPolicy(text);
    CHECK(policy.use_default);
    CHECK(policy.rules.empty());
    CHECK(ns::KeepTransferWithPolicy(policy, {true, 0, 0}, {true, 1, 0}));
    CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {false, 6, 0}));
  }
}

TEST_CASE("policy parsing ignores whitespace and commas", "[native_surfaces]") {
  const ns::TransferPolicy policy = ns::ParseTransferPolicy("  C>C ,D>C  ");
  CHECK_FALSE(policy.use_default);
  CHECK(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {false, 6, 0}));
  CHECK(ns::KeepTransferWithPolicy(policy, {true, 0, 0}, {false, 3, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {false, 3, 0}, {true, 0, 0}));
  CHECK_FALSE(ns::KeepTransferWithPolicy(policy, {true, 0, 0}, {true, 1, 0}));
}
