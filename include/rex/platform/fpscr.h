/**
 * @file        platform/fpscr.h
 * @brief       Platform-specific FPSCR constants and intrinsics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>

#include <rex/types.h>

#include <simde/x86/sse.h>

// SSE3 constants are missing from simde
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

namespace rex::platform {

// The rounding field of each host's FP control register, mapped both ways to the guest's
// FPSCR[RN] (0 nearest, 1 toward zero, 2 toward +infinity, 3 toward -infinity). MXCSR.RC and
// FPCR.RMode order the two infinities differently, so each host has its own read-back table.
// Both are defined on every host, so a unit test on the PC checks the ARM one too.
struct FPSCRRoundingX64 {
  static constexpr size_t RoundShift = 13;
  static constexpr size_t RoundMaskVal = SIMDE_MM_ROUND_MASK;
  static constexpr size_t GuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                           SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};
  // MXCSR.RC: 0 nearest, 1 toward -infinity, 2 toward +infinity, 3 toward zero.
  static constexpr u32 HostToGuest[] = {0, 3, 2, 1};
};

struct FPSCRRoundingArm64 {
  // RMode
  static constexpr size_t RoundShift = 22;
  static constexpr size_t RoundMaskVal = size_t(3) << RoundShift;
  // Nearest, Zero, +Infinity, -Infinity
  static constexpr size_t GuestToHost[] = {size_t(0) << RoundShift, size_t(3) << RoundShift,
                                           size_t(1) << RoundShift, size_t(2) << RoundShift};
  // FPCR.RMode: 0 nearest, 1 toward +infinity, 2 toward -infinity, 3 toward zero.
  static constexpr u32 HostToGuest[] = {0, 2, 3, 1};
};

template <typename Rounding>
constexpr u32 GuestRoundingFromHost(u32 csr) noexcept {
  return Rounding::HostToGuest[(csr & Rounding::RoundMaskVal) >> Rounding::RoundShift];
}

template <typename Rounding>
constexpr bool RoundingTablesRoundTrip() noexcept {
  for (u32 mode = 0; mode < 4; ++mode) {
    if (GuestRoundingFromHost<Rounding>(u32(Rounding::GuestToHost[mode])) != mode) return false;
  }
  return true;
}

static_assert(RoundingTablesRoundTrip<FPSCRRoundingX64>());
static_assert(RoundingTablesRoundTrip<FPSCRRoundingArm64>());

// simde does not handle denormal flags, so we need to implement per-arch.
#if defined(__x86_64__) || defined(_M_X64)

struct FPSCRPlatform : FPSCRRoundingX64 {
  static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  // Exception mask bits (1 = exception masked/disabled)
  static constexpr u32 ExceptionMask = (1 << 7) |   // IM - Invalid operation
                                       (1 << 8) |   // DM - Denormal operand
                                       (1 << 9) |   // ZM - Zero divide
                                       (1 << 10) |  // OM - Overflow
                                       (1 << 11) |  // UM - Underflow
                                       (1 << 12);   // PM - Precision (Inexact)

  static inline u32 getcsr() noexcept { return simde_mm_getcsr(); }

  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr |= ExceptionMask;  // Set mask bits to disable exceptions
  }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

struct FPSCRPlatform : FPSCRRoundingArm64 {
  // FZ and FZ16
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
  // Exception enable bits (0 = exception disabled, ARM defaults to 0)
  static constexpr u32 ExceptionMask = (1 << 8) |   // IOE - Invalid Operation
                                       (1 << 9) |   // DZE - Division by Zero
                                       (1 << 10) |  // OFE - Overflow
                                       (1 << 11) |  // UFE - Underflow
                                       (1 << 12) |  // IXE - Inexact
                                       (1 << 15);   // IDE - Input Denormal

  static inline u32 getcsr() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }

  static inline void setcsr(u32 csr) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(csr)); }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr &= ~ExceptionMask;  // Clear enable bits to disable exceptions
  }
};

#else
#error "Missing implementation for FPSCR."
#endif

}  // namespace rex::platform
