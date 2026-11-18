/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <condition_variable>
#include <forward_list>
#include <mutex>
#include <stop_token>
#include <thread>

#include <rex/assert.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

// The dispatch thread sleeps until the earliest due time or until a timer is queued. It used to
// spin on a lock-free ring buffer, which kept it waking about 1000 times a second (19 yields and a
// 1 ms sleep, over and over) even with no timer due.
class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : dispatch_thread_([this](std::stop_token stop_token) { TimerThreadMain(stop_token); }) {}

  ~TimerQueue() {
    {
      // Under the lock, so the dispatch thread cannot check the stop token and then miss the wake.
      std::lock_guard<std::mutex> lock(mutex_);
      dispatch_thread_.request_stop();
    }
    queued_cond_.notify_one();
    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    set_current_thread_name("rex::thread::TimerQueue");

    while (!stop_token.stop_requested()) {
      MergeQueuedItems(WaitForQueuedItems(stop_token));
      RunDueItems();
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      queued_.push_front(std::move(wait_item));
    }
    queued_cond_.notify_one();

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  using WaitList = std::forward_list<std::shared_ptr<WaitItem>>;

  static bool DueEarlier(const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
    return left->due_ < right->due_;
  }

  // Returns the timers queued since the last call, once there are any, the earliest one in
  // wait_queue_ is due or a stop is requested.
  WaitList WaitForQueuedItems(const std::stop_token& stop_token) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto has_work = [&] { return !queued_.empty() || stop_token.stop_requested(); };
    if (wait_queue_.empty()) {
      queued_cond_.wait(lock, has_work);
    } else {
      queued_cond_.wait_until(lock, wait_queue_.front()->due_, has_work);
    }
    WaitList items;
    items.swap(queued_);
    return items;
  }

  void MergeQueuedItems(WaitList items) {
    items.sort(DueEarlier);
    wait_queue_.merge(items, DueEarlier);
  }

  // Invokes the callbacks that are due and reschedules the recurring ones.
  void RunDueItems() {
    WaitList wait_items;
    while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
      auto wait_item = std::move(wait_queue_.front());
      wait_queue_.pop_front();

      // Ensure that it isn't disarmed
      auto state = WaitItem::State::kIdle;
      if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                    std::memory_order_acq_rel)) {
        assert_not_null(wait_item->callback_);
        wait_item->callback_(wait_item->userdata_);

        if (wait_item->interval_ != clock::duration::zero() &&
            wait_item->state_.load(std::memory_order_acquire) !=
                WaitItem::State::kInCallbackSelfDisarmed) {
          // Item is recurring and didn't self-disarm during callback:
          wait_item->due_ += wait_item->interval_;
          wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
          wait_item->state_.notify_all();
          wait_items.push_front(std::move(wait_item));
        } else {
          wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
          wait_item->state_.notify_all();
        }
      } else {
        // Specifically, kInCallback is illegal here
        assert_true(WaitItem::State::kDisarmed == state);
      }
    }
    MergeQueuedItems(std::move(wait_items));
  }

  std::mutex mutex_;
  std::condition_variable queued_cond_;
  WaitList queued_;  // Protected by mutex_

  // This is a _sorted_ (ascending due_) list of active timers, owned by the dispatch thread
  WaitList wait_queue_;
  // Last, so it starts after the members it uses and is joined before they are destroyed.
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
