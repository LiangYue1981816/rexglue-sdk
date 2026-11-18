/**
 * @file        tests/unit/core/wait_test.cpp
 * @brief       Multi-handle and alertable waits: results, wakeups and timeouts
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/thread.h>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::SleepResult;
using rex::thread::Thread;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

namespace {

using Clock = std::chrono::steady_clock;

std::unique_ptr<Thread> StartThread(std::function<void()> body) {
  Thread::CreationParameters params;
  return Thread::Create(params, std::move(body));
}

void SignalLater(Event* event, std::chrono::milliseconds delay) {
  std::thread([event, delay] {
    std::this_thread::sleep_for(delay);
    event->Set();
  }).detach();
}

}  // namespace

TEST_CASE("WaitAny returns the index of the handle signaled while waiting", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  SignalLater(b.get(), 20ms);
  auto result = rex::thread::WaitAny(handles, 2, false, 5000ms);

  REQUIRE(result.first == WaitResult::kSuccess);
  REQUIRE(result.second == 1);
  // Auto-reset: the wait consumed the signal.
  REQUIRE(rex::thread::WaitAny(handles, 2, false, 0ms).first == WaitResult::kTimeout);
}

TEST_CASE("WaitAll waits for every handle and consumes each", "[thread][wait]") {
  auto a = Event::CreateAutoResetEvent(true);
  auto b = Event::CreateAutoResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  REQUIRE(rex::thread::WaitAll(handles, 2, false, 30ms) == WaitResult::kTimeout);
  SignalLater(b.get(), 20ms);
  REQUIRE(rex::thread::WaitAll(handles, 2, false, 5000ms) == WaitResult::kSuccess);
  REQUIRE(rex::thread::WaitAny(handles, 2, false, 0ms).first == WaitResult::kTimeout);
}

TEST_CASE("WaitAny times out after roughly the timeout", "[thread][wait]") {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  WaitHandle* handles[] = {a.get(), b.get()};

  auto start = Clock::now();
  auto result = rex::thread::WaitAny(handles, 2, false, 50ms);
  auto elapsed = Clock::now() - start;

  REQUIRE(result.first == WaitResult::kTimeout);
  REQUIRE(elapsed >= 45ms);
  REQUIRE(elapsed < 2000ms);
}

TEST_CASE("A semaphore release wakes WaitAny", "[thread][wait]") {
  auto event = Event::CreateManualResetEvent(false);
  auto semaphore = Semaphore::Create(0, 4);
  WaitHandle* handles[] = {event.get(), semaphore.get()};

  std::thread releaser([&semaphore] {
    std::this_thread::sleep_for(20ms);
    semaphore->Release(1, nullptr);
  });
  auto result = rex::thread::WaitAny(handles, 2, false, 5000ms);
  releaser.join();

  REQUIRE(result.first == WaitResult::kSuccess);
  REQUIRE(result.second == 1);
}

TEST_CASE("No signal is lost across many WaitAny round trips", "[thread][wait]") {
  constexpr int kRounds = 2000;
  auto ping = Event::CreateAutoResetEvent(false);
  auto pong = Event::CreateAutoResetEvent(false);
  auto idle = Event::CreateAutoResetEvent(false);
  std::atomic<int> answered{0};

  std::thread echo([&] {
    WaitHandle* handles[] = {idle.get(), ping.get()};
    for (int i = 0; i < kRounds; ++i) {
      if (rex::thread::WaitAny(handles, 2, false, 5000ms).second != 1) {
        return;
      }
      answered.fetch_add(1);
      pong->Set();
    }
  });
  for (int i = 0; i < kRounds; ++i) {
    ping->Set();
    if (rex::thread::Wait(pong.get(), false, 5000ms) != WaitResult::kSuccess) {
      break;
    }
  }
  echo.join();

  REQUIRE(answered.load() == kRounds);
}

TEST_CASE("A queued user callback ends an alertable WaitAny", "[thread][wait]") {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  auto started = Event::CreateManualResetEvent(false);
  std::atomic<bool> callback_ran{false};
  std::atomic<int> result{-1};
  std::atomic<int64_t> waited_ms{-1};

  auto waiter = StartThread([&] {
    WaitHandle* handles[] = {a.get(), b.get()};
    started->Set();
    auto start = Clock::now();
    auto r = rex::thread::WaitAny(handles, 2, true, 5000ms);
    waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    result = static_cast<int>(r.first);
  });
  REQUIRE(rex::thread::Wait(started.get(), false, 5000ms) == WaitResult::kSuccess);
  std::this_thread::sleep_for(30ms);
  waiter->QueueUserCallback([&callback_ran] { callback_ran = true; });
  REQUIRE(rex::thread::Wait(waiter.get(), false, 5000ms) == WaitResult::kSuccess);

  REQUIRE(callback_ran.load());
  REQUIRE(result.load() == static_cast<int>(WaitResult::kUserCallback));
  REQUIRE(waited_ms.load() < 2000);
}

TEST_CASE("A queued user callback ends an alertable sleep", "[thread][wait]") {
  auto started = Event::CreateManualResetEvent(false);
  std::atomic<bool> callback_ran{false};
  std::atomic<int> result{-1};

  auto sleeper = StartThread([&] {
    started->Set();
    result = static_cast<int>(rex::thread::AlertableSleep(5000ms));
  });
  REQUIRE(rex::thread::Wait(started.get(), false, 5000ms) == WaitResult::kSuccess);
  std::this_thread::sleep_for(30ms);
  sleeper->QueueUserCallback([&callback_ran] { callback_ran = true; });
  REQUIRE(rex::thread::Wait(sleeper.get(), false, 5000ms) == WaitResult::kSuccess);

  REQUIRE(callback_ran.load());
  REQUIRE(result.load() == static_cast<int>(SleepResult::kAlerted));
}

TEST_CASE("An alertable single wait still returns the signal", "[thread][wait]") {
  auto event = Event::CreateAutoResetEvent(false);
  SignalLater(event.get(), 20ms);
  REQUIRE(rex::thread::Wait(event.get(), true, 5000ms) == WaitResult::kSuccess);
  REQUIRE(rex::thread::Wait(event.get(), true, 0ms) == WaitResult::kTimeout);
}
