#pragma once

#include "nonowner_lockfree_stack.hpp"
#include "executor_interface.hpp"
#include "common.hpp"

namespace dd {

// same as std::latch, but for coroutines
// when completed, starts all waiters on passed executor
// may be destroyed right after `wait` / `arrive_and_wait` returned
struct latch {
 private:
  nonowner_lockfree_stack<task_node> stack;
  any_executor_ref exe;
  std::atomic_ptrdiff_t counter;

  struct wait_awaiter {
    latch& l;
    task_node node;

    bool await_ready() const noexcept {
      return l.ready();
    }
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      node.task = handle;
      // prevent using latch after push
      return l.stack.try_push(&node);
    }
    static void await_resume() noexcept {
    }
  };

  // same as wait if n == 0
  struct arrive_and_wait_awaiter {
    latch& l;
    ptrdiff_t n;
    task_node node;

    bool await_ready() const noexcept {
      // n > 0 => 100% not completed, count_down not yet done
      return n == 0 && l.ready();
    }
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      node.task = handle;
      if (n == 0)  // same as `wait`
        return l.stack.try_push(&node);
      l.stack.push(&node);
      l.count_down(n);
      return true;
    }
    static void await_resume() noexcept {
    }
  };

 public:
  // precondition: count >= 0 and <= max, task will be executed on 'e'
  constexpr explicit latch(ptrdiff_t count, any_executor_ref e) noexcept
      : stack(count == 0), exe(e), counter(count) {
    assert(count >= 0 && count <= max());
  }
  latch(latch&&) = delete;
  void operator=(latch&&) = delete;

  static constexpr ptrdiff_t max() noexcept {
    return std::numeric_limits<ptrdiff_t>::max();
  }

  // decrements the internal counter by n without blocking the caller
  // precondition: n >= 0 && n <= internal counter
  void count_down(std::ptrdiff_t n = 1) noexcept {
    assert(n >= 0 && n <= counter.load(std::memory_order::acquire));
    ptrdiff_t c = counter.fetch_sub(n, std::memory_order::acq_rel);
    assert(c >= n && "precondition violated");
    if (c == n) [[unlikely]]
      wakeup_all();
  }

  // returns true if the internal counter has reached zero and waiters are woken
  // never blocks, 'try_wait' in std::latch
  [[nodiscard]] bool ready() const noexcept {
    return stack.closed();
  }

  // suspends the calling coroutine until the internal counter reaches ​0​.
  // If it is zero already, returns immediately
  KELCORO_CO_AWAIT_REQUIRED co_awaiter auto wait() noexcept {
    return wait_awaiter{*this};
  }

  // precondition: n >= 0 && n <= internal counter
  // logical equivalent to count_down(n); wait() (but atomicaly, really count down + wait is rata race)
  KELCORO_CO_AWAIT_REQUIRED co_awaiter auto arrive_and_wait(std::ptrdiff_t n = 1) noexcept {
    assert(n >= 0 && n <= counter.load(std::memory_order::acquire));
    return arrive_and_wait_awaiter{*this, n};
  }

  any_executor_ref get_executor() const noexcept {
    return exe;
  }

 private:
  void wakeup_all() noexcept {
    // do not use *this after possible destroy
    any_executor_ref e = exe;
    attach_list(e, stack.close());
  }
};

}  // namespace dd
