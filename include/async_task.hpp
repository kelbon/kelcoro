#pragma once

#include <atomic>

#include "common.hpp"

namespace dd {

namespace noexport {
enum struct state : uint8_t { not_ready, ready, consumer_dead = ready };
}

template <typename>
struct async_task;

template <typename Result>
struct async_task_promise : return_block<Result> {
  std::atomic<noexport::state> task_state = noexport::state::not_ready;

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  async_task<Result> get_return_object() {
    return async_task<Result>(std::coroutine_handle<async_task_promise<Result>>::from_promise(*this));
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }

 private:
  struct destroy_if_consumer_dead_t {
    std::atomic<noexport::state>& task_state;

    static bool await_ready() noexcept {
      return false;
    }
    bool await_suspend(std::coroutine_handle<void> handle) const noexcept {
      noexport::state before = task_state.exchange(noexport::state::ready, std::memory_order::acq_rel);
      if (before == noexport::state::consumer_dead)
        return false;
      task_state.notify_one();
      return true;
    }
    static void await_resume() noexcept {
    }
  };

 public:
  auto final_suspend() noexcept {
    return destroy_if_consumer_dead_t{task_state};
  }
};

// one producer, one consumer
template <typename Result>
struct async_task : enable_resource_deduction {
  using promise_type = async_task_promise<Result>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_ = nullptr;

  friend promise_type;
  constexpr explicit async_task(handle_type handle) noexcept : handle_(handle) {
  }

 public:
  constexpr async_task() noexcept = default;

  constexpr void swap(async_task& other) noexcept {
    std::swap(handle_, other.handle_);
  }
  friend constexpr void swap(async_task& a, async_task& b) noexcept {
    a.swap(b);
  }
  constexpr async_task(async_task&& other) noexcept {
    swap(other);
  }
  constexpr async_task& operator=(async_task&& other) noexcept {
    swap(other);
    return *this;
  }

  // postcondition: if !empty(), then coroutine suspended and value produced
  void wait() const noexcept {
    if (empty())
      return;
    auto& cur_state = handle_.promise().task_state;
    noexport::state now = cur_state.load(std::memory_order::acquire);
    while (now != noexport::state::ready) {
      cur_state.wait(now, std::memory_order::acquire);
      now = cur_state.load(std::memory_order::acquire);
    }
    return;
  }
  // returns true if 'get' is callable and will return immedially without wait
  bool ready() const noexcept {
    if (empty())
      return false;
    return noexport::state::ready == handle_.promise().task_state.load(std::memory_order::acquire);
  }
  // postcondition: empty()
  void detach() noexcept {
    if (empty())
      return;
    noexport::state state_before =
        handle_.promise().task_state.exchange(noexport::state::consumer_dead, std::memory_order::acq_rel);
    if (state_before == noexport::state::ready)
      handle_.destroy();
    handle_ = nullptr;
    // otherwise frame destroys itself because consumer is dead
  }

  // precondition: !empty()
  // must be invoked in one thread(one consumer)
  std::add_rvalue_reference_t<Result> get() && noexcept KELCORO_LIFETIMEBOUND {
    assert(!empty());
    wait();
    // result always exist, its setted or std::terminate called on exception.
    return handle_.promise().result();
  }

  // return true if call to 'get' will produce UB
  constexpr bool empty() const noexcept {
    return handle_ == nullptr;
  }

  ~async_task() {
    detach();
  }
};

template <typename Ret, memory_resource R>
using async_task_r = resourced<async_task<Ret>, R>;

namespace pmr {

template <typename Ret>
using async_task = ::dd::async_task_r<Ret, polymorphic_resource>;

}

}  // namespace dd