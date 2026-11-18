/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>
#if REX_PLATFORM_ANDROID && REX_ARCH_ARM64
#include <cstdlib>  // std::abort (Android fiber start trampoline)
#endif

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

#if REX_PLATFORM_ANDROID && REX_ARCH_ARM64
// Bionic provides no ucontext user-context functions, so fibers switch with a
// small hand-rolled AArch64 context switch: only callee-saved registers
// (x19-x28, fp, lr, d8-d15) plus sp are preserved, matching the AAPCS64
// contract, and x0_ carries the Fiber* into freshly created fibers.

__attribute__((naked)) static void FiberSwitchImpl(AndroidFiberContext* from,
                                                   AndroidFiberContext* to) {
  __asm__ volatile(
      // Save callee-saved state into *from (x0).
      "stp x19, x20, [x0, #8]\n"
      "stp x21, x22, [x0, #24]\n"
      "stp x23, x24, [x0, #40]\n"
      "stp x25, x26, [x0, #56]\n"
      "stp x27, x28, [x0, #72]\n"
      "stp x29, x30, [x0, #88]\n"
      "mov x2, sp\n"
      "str x2, [x0, #104]\n"
      "stp d8, d9, [x0, #112]\n"
      "stp d10, d11, [x0, #128]\n"
      "stp d12, d13, [x0, #144]\n"
      "stp d14, d15, [x0, #160]\n"
      // Restore callee-saved state from *to (x1).
      "ldp x19, x20, [x1, #8]\n"
      "ldp x21, x22, [x1, #24]\n"
      "ldp x23, x24, [x1, #40]\n"
      "ldp x25, x26, [x1, #56]\n"
      "ldp x27, x28, [x1, #72]\n"
      "ldp d8, d9, [x1, #112]\n"
      "ldp d10, d11, [x1, #128]\n"
      "ldp d12, d13, [x1, #144]\n"
      "ldp d14, d15, [x1, #160]\n"
      "ldr x2, [x1, #104]\n"
      "mov sp, x2\n"
      "ldp x29, x30, [x1, #88]\n"
      "ldr x0, [x1]\n"
      "ret\n");
}

// Entry point for freshly created fibers: x0 holds the Fiber*. Runs entry_
// once; returning aborts since there is no caller to resume.
/*static*/ void Fiber::StartTrampoline(Fiber* f) {
  f->entry_(f->arg_);
  assert(false && "Fiber entry returned");
  std::abort();
}

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  if (f->stack_.size() < 64) {
    delete f;
    return nullptr;
  }
  // 16-byte aligned stack top, per AAPCS64.
  const uintptr_t top =
      (reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size())) & ~uintptr_t(15);
  f->context_.x0_ = reinterpret_cast<uint64_t>(f);
  f->context_.fp_ = 0;
  f->context_.lr_ = reinterpret_cast<uint64_t>(&Fiber::StartTrampoline);
  f->context_.sp_ = top;
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // Unused on Android (StartTrampoline is entered directly), kept so the
  // member declaration stays shared across POSIX backends.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  FiberSwitchImpl(&from->context_, &target->context_);
}

#else

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

#endif  // REX_PLATFORM_ANDROID && REX_ARCH_ARM64

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
