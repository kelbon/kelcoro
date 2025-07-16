#pragma once

#include <coroutine>
#include <cassert>
#include <exception>
#include <utility>
#include <cstddef>

#include <kelcoro/noexport/macro.hpp>

namespace dd {

struct gate_closed_exception : std::exception {};

// Note: all methods should be called from one thread
struct gate {
 private:
  size_t count = 0;
  std::coroutine_handle<> close_waiter = nullptr;

 public:
  // if gate is closed, returns false.
  // Otherwise returns true and caller must call 'leave' in future
  [[nodiscard]] bool try_enter() noexcept {
    if (close_waiter) [[unlikely]]
      return false;
    ++count;
    return true;
  }

  void enter() {
    if (!try_enter())
      throw gate_closed_exception{};
  }

  // must be invoked only after invoking 'try_enter'
  void leave() noexcept {
    assert(count != 0);
    --count;
    if (close_waiter && count == 0) [[unlikely]]
      close_waiter.resume();
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
  // this method has 2 proposes.
  // first - if caller want to not use 'close', instead looping while 'active_count() != 0'
  // and second - if canceling operations may produce more operations (and they will observe not closed gate)
  // in this case, caller firstly call 'request_close', then canceling operations and then awaiting .close()
  void request_close() noexcept {
    assert(!is_closed());
    close_waiter = std::noop_coroutine();
  }

  struct gate_close_awaiter {
    gate* g = nullptr;

    bool await_ready() noexcept {
      return g->count == 0;
    }
    void await_suspend(std::coroutine_handle<> waiter) noexcept {
      g->close_waiter = waiter;
    }
    void await_resume() noexcept {
      assert(g->count == 0);
      // avoid storing handle to dead coroutine
      // + guarantee, that after calling 'close' gate is closed, even if count == 0
      g->close_waiter = std::noop_coroutine();
    }
  };

  // must not be called twice.
  // postcondition: is_closed() == true.
  // reopen() will allow calling 'close' again
  KELCORO_CO_AWAIT_REQUIRED gate_close_awaiter close() noexcept {
    return gate_close_awaiter{this};
  }

  [[nodiscard]] bool is_closed() const noexcept {
    return close_waiter != nullptr;
  }

  // precondition: .close() was awaited or try_enter was not called
  void reopen() noexcept {
    assert(count == 0);
    close_waiter = nullptr;
  }

  gate() = default;

#ifndef NDEBUG
  ~gate() {
    assert(count == 0 && "gate closed with unfinished requests");
  }
  gate(gate&& other) noexcept {
    assert(other.count == 0);
    count = 0;
    close_waiter = std::exchange(other.close_waiter, nullptr);
  }
  gate& operator=(gate&& other) noexcept {
    assert(count == 0 && other.count == 0);
    std::swap(close_waiter, other.close_waiter);
    return *this;
  }
#else
  ~gate() = default;
  gate(gate&&) = default;
  gate& operator=(gate&&) = default;
#endif
};

}  // namespace dd
