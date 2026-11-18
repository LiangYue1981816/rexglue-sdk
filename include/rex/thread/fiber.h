/**
 * @file        rex/thread/fiber.h
 * @brief       Host OS fiber primitive for cooperative context switching
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <cstddef>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#if REX_PLATFORM_MAC && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext APIs unless _XOPEN_SOURCE is defined
// before including <ucontext.h>.
#define _XOPEN_SOURCE 700
#endif
#if !REX_PLATFORM_ANDROID
// Bionic provides no ucontext.h user context functions (getcontext /
// makecontext / swapcontext); the Android context type below is used instead.
#include <ucontext.h>
#endif
#include <cstdint>
#include <vector>
#endif

#if REX_PLATFORM_ANDROID && REX_ARCH_ARM64
// Minimal AArch64 fiber context for Android: callee-saved registers plus the
// stack pointer. x0_ is restored into x0 on switch; fresh fibers use it to
// receive their Fiber* in the start trampoline.
struct AndroidFiberContext {
  uint64_t x0_ = 0;
  uint64_t x19_[10] = {};
  uint64_t fp_ = 0;
  uint64_t lr_ = 0;
  uint64_t sp_ = 0;
  // The low 64 bits of v8-v15 (d8-d15) are callee-saved too (AAPCS64).
  uint64_t d8_[8] = {};
};
static_assert(sizeof(AndroidFiberContext) == 22 * sizeof(uint64_t));
#endif

namespace rex::thread {

/// Host OS fiber primitive.
/// Each guest fiber gets one Fiber. Switching preserves the entire C++ call
/// stack, so mid-function resume works without any LR lookup.
struct Fiber {
  /// Convert the calling thread into a fiber.
  /// Must be called once on a thread before any SwitchTo.
  /// Returns a handle for the thread's current execution context.
  static Fiber* ConvertCurrentThread();

  /// Create a new fiber with its own host stack.
  /// entry(arg) is called when the fiber is first switched to.
  static Fiber* Create(size_t stack_size, void (*entry)(void*), void* arg);

  /// Suspend the current fiber and resume target.
  /// Returns when another fiber calls SwitchTo back to this one.
  static void SwitchTo(Fiber* target);

  /// Destroy this fiber. Must not be called while it is executing.
  void Destroy();

  /// Returns the fiber currently executing on this thread, or nullptr.
  static Fiber* Current() { return tls_current_; }

 private:
  static thread_local Fiber* tls_current_;

#if REX_PLATFORM_WIN32
  void* handle_ = nullptr;
  bool is_thread_fiber_ = false;
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#if REX_PLATFORM_ANDROID && REX_ARCH_ARM64
  AndroidFiberContext context_{};
#else
  ucontext_t context_{};
#endif
  std::vector<uint8_t> stack_;
  void (*entry_)(void*) = nullptr;
  void* arg_ = nullptr;
  bool is_thread_fiber_ = false;

  static void Trampoline();
#if REX_PLATFORM_ANDROID && REX_ARCH_ARM64
  // Android AArch64 entry point for freshly created fibers (bionic has no
  // makecontext, so the fiber is entered directly with the Fiber* in x0).
  // A member so it can reach entry_/arg_, which stay private.
  static void StartTrampoline(Fiber* f);
#endif
#endif
};

}  // namespace rex::thread
