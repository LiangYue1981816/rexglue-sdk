/**
 * @file        tests/unit/core/timer_queue_test.cpp
 * @brief       Timer queue: one-shot and recurring callbacks, disarming, and waking for a new timer
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/thread/timer_queue.h>

using namespace std::chrono_literals;
using Clock = rex::thread::TimerQueueWaitItem::clock;

namespace {

bool WaitForCount(const std::atomic<int>& count, int expected, Clock::duration limit) {
  auto deadline = Clock::now() + limit;
  while (count.load() < expected) {
    if (Clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

void Increment(void* userdata) {
  static_cast<std::atomic<int>*>(userdata)->fetch_add(1);
}

}  // namespace

TEST_CASE("One-shot timer fires once, not before its due time", "[thread][timer]") {
  std::atomic<int> count{0};
  auto start = Clock::now();
  auto item = rex::thread::QueueTimerOnce(&Increment, &count, start + 30ms);
  REQUIRE(WaitForCount(count, 1, 2s));
  CHECK(Clock::now() - start >= 30ms);
  std::this_thread::sleep_for(50ms);
  CHECK(count.load() == 1);
}

TEST_CASE("Recurring timer repeats until disarmed", "[thread][timer]") {
  std::atomic<int> count{0};
  auto item = rex::thread::QueueTimerRecurring(&Increment, &count, Clock::now(), 5ms);
  REQUIRE(WaitForCount(count, 4, 2s));
  item.lock()->Disarm();
  int after_disarm = count.load();
  std::this_thread::sleep_for(40ms);
  CHECK(count.load() == after_disarm);
}

TEST_CASE("A new near timer fires while a far one is pending", "[thread][timer]") {
  std::atomic<int> far_count{0};
  std::atomic<int> near_count{0};
  auto far_item = rex::thread::QueueTimerOnce(&Increment, &far_count, Clock::now() + 10s);
  std::this_thread::sleep_for(20ms);
  auto start = Clock::now();
  auto near_item = rex::thread::QueueTimerOnce(&Increment, &near_count, start + 10ms);
  REQUIRE(WaitForCount(near_count, 1, 2s));
  CHECK(Clock::now() - start < 1s);
  CHECK(far_count.load() == 0);
  far_item.lock()->Disarm();
}
