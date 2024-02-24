#pragma once

#include "nonowner_lockfree_stack.hpp"

#include "thread_pool.hpp"

namespace dd {

// TODO
using any_executor_ref = thread_pool&;

// same as std::latch, but for coroutines
struct latch {
 private:
  alignas(hardware_destructive_interference_size) mutable nonowner_lockfree_stack<task_node> stack;
  alignas(hardware_destructive_interference_size) std::atomic_ptrdiff_t counter;
  any_executor_ref exe;

  struct wait_awaiter {
    const latch& l;
    task_node node;

    bool await_ready() const noexcept {
      return l.ready();
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept {
      node.task = handle;
      l.stack.push(&node);
    }
    static void await_resume() noexcept {
    }
  };
  // this prevents such situation:
  // - .count_down
  // - .wait
  // - checked if ready: no, then go to .push into stack
  // - someone else decrement counter and wakeup all
  // - one task leaks
  struct arrive_and_wait_awaiter {
    latch& l;
    ptrdiff_t n;
    task_node node;

    static bool await_ready() noexcept {
      // do not check here for preventing data race (counter == 0, but no task in queue)
      return false;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept {
      node.task = handle;
      l.stack.push(&node);
      // copy logic from count down but never resume
      assert(n >= 0 && n <= l.counter.load(std::memory_order::relaxed));
      ptrdiff_t c = l.counter.fetch_sub(n, std::memory_order::acq_rel) - n;
      if (c != 0) [[likely]] {
        assert(c >= 0 && "precondition violated");
        return;
      }
      l.wakeup_all();
    }
    static void await_resume() noexcept {
    }
  };

 public:
  // precondition: count >= 0 and <= max, task will be executed on 'e'
  constexpr explicit latch(ptrdiff_t count, any_executor_ref e) noexcept : counter(count), exe(e) {
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
    assert(n >= 0 && n <= counter.load(std::memory_order::relaxed));
    ptrdiff_t c = counter.fetch_sub(n, std::memory_order::acq_rel) - n;
    if (c != 0) [[likely]] {
      assert(c >= 0 && "precondition violated");
      return;
    }
    wakeup_all();
  }

  // returns true if the internal counter has reached zero
  // never blocks, 'try_wait' in std::latch
  [[nodiscard]] bool ready() const noexcept {
    return counter.load(std::memory_order::acquire) == 0;
  }

  // suspends the calling coroutine until the internal counter reaches ​0​.
  // If it is zero already, returns immediately
  KELCORO_CO_AWAIT_REQUIRED co_awaiter auto wait() const noexcept {
    return wait_awaiter(*this);
  }

  // precondition: n >= 0 && n <= internal counter
  // logical equivalent to count_down(n); wait() (but atomicaly, really count down + wait is rata race)
  KELCORO_CO_AWAIT_REQUIRED co_awaiter auto arrive_and_wait(std::ptrdiff_t n = 1) noexcept {
    assert(n >= 0 && n <= counter.load(std::memory_order::relaxed));
    return arrive_and_wait_awaiter(*this, n);
  }

 private:
  void wakeup_all() noexcept {
    task_node* top = stack.try_pop_all(std::memory_order::relaxed);
    while (top) {
      // save top to be not invalidated by .resume()
      task_node* next = top->next;
      exe.attach(top);
      top = next;
    }
  }
};

}  // namespace dd
