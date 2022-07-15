#pragma once

#include <atomic>

#include "common.hpp"

namespace dd {

enum class state : uint8_t { not_ready, almost_ready, ready, consumer_dead };

template <typename Result, typename Alloc>
struct async_task_promise : memory_block<Alloc>, return_block<Result> {
  std::atomic<state> task_state;

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<async_task_promise<Result, Alloc>>::from_promise(*this);
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }
  auto final_suspend() noexcept {
    const auto state_before = task_state.exchange(state::almost_ready, std::memory_order::acq_rel);

    struct destroy_if_consumer_dead_t {
      bool is_consumer_dead;
      std::atomic<state>& task_state;

      bool await_ready() const noexcept {
        return is_consumer_dead;
      }
      void await_suspend(std::coroutine_handle<void> handle) const noexcept {
        task_state.exchange(state::ready, std::memory_order::acq_rel);
        task_state.notify_one();
      }
      void await_resume() const noexcept {
      }
    };
    return destroy_if_consumer_dead_t{state_before == state::consumer_dead, task_state};
  }
};

template <typename Result, typename Alloc = std::allocator<std::byte>>
struct async_task {
 public:
  using promise_type = async_task_promise<Result, Alloc>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_;

 public:
  constexpr async_task(handle_type handle) : handle_(handle) {
    assert(handle_ != nullptr);
  }

  async_task(async_task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }
  void operator=(async_task&&) = delete;

  // postcondition - coro stops with value
  void wait() const noexcept {
    if (!handle_)
      return;
    auto& cur_state = handle_.promise().task_state;
    state now = cur_state.load(std::memory_order::acquire);
    while (now != state::ready) {
      cur_state.wait(now, std::memory_order::acquire);
      now = cur_state.load(std::memory_order::acquire);
    }
  }
  // postcondition - handle_ == nullptr
  Result get() && requires(!std::is_void_v<Result>) {
    assert(handle_ != nullptr); // must never happens, second get
    wait();
    scope_exit clear([&] { handle_ = nullptr; });

    auto result = *std::move(handle_.promise().storage);
    // result always exist, its setted or std::terminate called on exception.
    handle_.destroy();
    return result;
  }
  
  // return true if value was already getted, otherwise false
  bool empty() const noexcept {
    return handle_ == nullptr;
  }

  ~async_task() {
    if (!handle_)
      return;
    const auto state_before =
        handle_.promise().task_state.exchange(state::consumer_dead, std::memory_order::acq_rel);
    switch (state_before) {
      case state::almost_ready:
        handle_.promise().task_state.wait(state::almost_ready, std::memory_order::acquire);
        [[fallthrough]];
      case state::ready:
        handle_.destroy();
        break;
    }
    // otherwise frame destroys itself because consumer is dead
  }
};

}  // namespace dd