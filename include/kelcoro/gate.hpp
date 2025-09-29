#pragma once

#include <coroutine>
#include <cassert>
#include <exception>
#include <utility>
#include <cstddef>

#include <kelcoro/noexport/macro.hpp>
#include <kelcoro/road.hpp>

namespace dd {

struct gate_closed_exception : std::exception {};

// Note: all methods should be called from one thread
struct gate {
 private:
  size_t count = 0;
  awaiters_queue<task_node> close_waiters;
  bool close_requested = false;

  void request_close() noexcept {
    close_requested = true;
  }

 public:
  // if gate is closed, returns false.
  // Otherwise returns true and caller must call 'leave' in future
  [[nodiscard]] bool try_enter() noexcept {
    if (is_closed()) [[unlikely]]
      return false;
    ++count;
    return true;
  }

  void enter() {
    if (!try_enter())
      throw gate_closed_exception{};
  }

  // must be invoked only after invoking 'try_enter'
  // Note: may resume close awaiters.
  // Sometimes it makes sense to push them into task list instead of executing now
  // e.g. someone who call `leave` uses resources, which they will destroy
  void leave() noexcept {
    assert(count != 0);
    --count;
    if (is_closed() && count == 0) [[unlikely]]
      attach_list(this_thread_executor, close_waiters.pop_all());
  }

  struct holder {
    gate* g = nullptr;

    holder(gate* g) : g(g) {
      g->enter();
    }
    ~holder() {
      if (g)
        g->leave();
    }
    holder(holder&& other) noexcept : g(std::exchange(other.g, nullptr)) {
    }
    holder& operator=(holder&& other) noexcept {
      std::swap(other.g, g);
      return *this;
    }
  };

  [[nodiscard]] holder hold() KELCORO_LIFETIMEBOUND {
    return holder{this};
  }

  // now many successfull 'try_enter' calls not finished by 'leave' now
  [[nodiscard]] size_t active_count() const noexcept {
    return count;
  }

  struct gate_close_awaiter : task_node {
    gate* g = nullptr;

    gate_close_awaiter(gate* g) noexcept : g(g) {
    }

    bool await_ready() noexcept {
      return g->count == 0;
    }
    void await_suspend(std::coroutine_handle<> waiter) noexcept {
      this->task = waiter;
      g->close_waiters.push(this);
    }
    static void await_resume() noexcept {
    }
  };

  // postcondition: is_closed() == true && `active_count()` == 0
  // Note: is_closed() true even before co_await
  // Note: several coroutines may wait `close`. All close awaiters will be resumed.
  // reopen() will allow calling 'close' again
  KELCORO_CO_AWAIT_REQUIRED gate_close_awaiter close() noexcept {
    request_close();
    return gate_close_awaiter{this};
  }

  [[nodiscard]] bool is_closed() const noexcept {
    return close_requested;
  }

  // after `reopen` gate may be `entered` or `closed` again
  // precondition: .active_count() == 0 && no one waits for `close`
  void reopen() noexcept {
    assert(count == 0);
    assert(close_waiters.empty());
    close_requested = false;
  }

  gate() = default;

  ~gate() {
    assert(count == 0 && "gate closed with unfinished requests");
  }
  gate(gate&& other) noexcept
      : count(0),
        close_waiters(std::exchange(other.close_waiters, {})),
        close_requested(std::exchange(other.close_requested, false)) {
    assert(other.count == 0);
  }
  gate& operator=(gate&& other) noexcept {
    assert(count == 0 && other.count == 0);
    assert(close_waiters.empty() && other.close_waiters.empty());
    close_waiters = std::exchange(other.close_waiters, {}),
    close_requested = std::exchange(other.close_requested, false);
    return *this;
  }
};

}  // namespace dd
