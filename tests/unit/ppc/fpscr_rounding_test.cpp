// The guest's FPSCR rounding mode as mtfsf writes it and mffs reads it back, on x64 and arm64.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>

#include <rex/platform/fpscr.h>
#include <rex/ppc/context.h>

using rex::platform::FPSCRRoundingArm64;
using rex::platform::FPSCRRoundingX64;
using rex::platform::GuestRoundingFromHost;

namespace {

// One IEEE rounding direction as each register encodes it: MXCSR.RC (Intel SDM vol. 1, 10.2.3),
// FPCR.RMode (Arm ARM, FPCR) and the guest's FPSCR[RN] (PowerPC UISA, 4.2.2).
struct Direction {
  const char* name;
  uint32_t mxcsr_rc;
  uint32_t fpcr_rmode;
  uint32_t guest_rn;
};

constexpr Direction kDirections[] = {
    {"to nearest", 0, 0, rex::ppc::kRoundNearest},
    {"toward zero", 3, 3, rex::ppc::kRoundTowardZero},
    {"toward +infinity", 2, 1, rex::ppc::kRoundUp},
    {"toward -infinity", 1, 2, rex::ppc::kRoundDown},
};

// Bits outside the rounding field that a live control register carries (MXCSR: masks and
// FTZ/DAZ; FPCR: FZ, FZ16 and an exception enable).
constexpr uint32_t kOtherMxcsrBits = 0x9FC0;
constexpr uint32_t kOtherFpcrBits = (1u << 19) | (1u << 24) | (1u << 8);

template <typename Rounding>
uint32_t HostFieldOf(uint32_t guest_mode) {
  return uint32_t(Rounding::GuestToHost[guest_mode] >> Rounding::RoundShift);
}

// 1/3 in the guest's rounding mode, with the division kept between the mode switches.
float DivideInGuestMode(PPCFPSCRRegister& fpscr, uint32_t mode, float numerator) {
  fpscr.storeFromGuest(mode);
  std::atomic_signal_fence(std::memory_order_seq_cst);
  volatile float n = numerator;
  volatile float d = 3.0f;
  volatile float quotient = n / d;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  return quotient;
}

}  // namespace

TEST_CASE("mffs reads each host rounding direction as the mode the PC reads", "[ppc][fpscr]") {
  for (const Direction& d : kDirections) {
    INFO(d.name);
    CHECK(GuestRoundingFromHost<FPSCRRoundingX64>(d.mxcsr_rc << FPSCRRoundingX64::RoundShift) ==
          d.guest_rn);
    CHECK(GuestRoundingFromHost<FPSCRRoundingArm64>(d.fpcr_rmode << FPSCRRoundingArm64::RoundShift) ==
          d.guest_rn);
  }
}

TEST_CASE("mtfsf selects the host direction the guest mode names", "[ppc][fpscr]") {
  for (const Direction& d : kDirections) {
    INFO(d.name);
    CHECK(HostFieldOf<FPSCRRoundingX64>(d.guest_rn) == d.mxcsr_rc);
    CHECK(HostFieldOf<FPSCRRoundingArm64>(d.guest_rn) == d.fpcr_rmode);
  }
}

TEST_CASE("a guest mode written and read back is unchanged on both hosts", "[ppc][fpscr]") {
  for (uint32_t mode = 0; mode < 4; ++mode) {
    INFO("guest mode " << mode);
    const uint32_t mxcsr = kOtherMxcsrBits | uint32_t(FPSCRRoundingX64::GuestToHost[mode]);
    const uint32_t fpcr = kOtherFpcrBits | uint32_t(FPSCRRoundingArm64::GuestToHost[mode]);
    CHECK(GuestRoundingFromHost<FPSCRRoundingX64>(mxcsr) == mode);
    CHECK(GuestRoundingFromHost<FPSCRRoundingArm64>(fpcr) == mode);
  }
}

TEST_CASE("this host's FPSCR register reads back what the guest wrote", "[ppc][fpscr]") {
  PPCFPSCRRegister fpscr{};
  fpscr.InitHost();
  const uint32_t saved = fpscr.getcsr();
  uint32_t read_back[4] = {};
  for (uint32_t mode = 0; mode < 4; ++mode) {
    fpscr.storeFromGuest(mode);
    read_back[mode] = fpscr.loadFromHost();
  }
  fpscr.setcsr(saved);
  for (uint32_t mode = 0; mode < 4; ++mode) {
    INFO("guest mode " << mode);
    CHECK(read_back[mode] == mode);
  }
}

TEST_CASE("this host rounds in the direction the guest mode names", "[ppc][fpscr]") {
  PPCFPSCRRegister fpscr{};
  fpscr.InitHost();
  const uint32_t saved = fpscr.getcsr();
  const float up = DivideInGuestMode(fpscr, rex::ppc::kRoundUp, 1.0f);
  const float down = DivideInGuestMode(fpscr, rex::ppc::kRoundDown, 1.0f);
  const float nearest = DivideInGuestMode(fpscr, rex::ppc::kRoundNearest, 1.0f);
  const float negative_toward_zero = DivideInGuestMode(fpscr, rex::ppc::kRoundTowardZero, -1.0f);
  const float negative_down = DivideInGuestMode(fpscr, rex::ppc::kRoundDown, -1.0f);
  fpscr.setcsr(saved);
  CHECK(up > down);
  CHECK((nearest == up || nearest == down));
  CHECK(negative_toward_zero > negative_down);
}
