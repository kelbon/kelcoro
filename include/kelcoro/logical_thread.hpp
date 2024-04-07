#pragma once

#include <utility>

#include "common.hpp"
#include "memory_support.hpp"

namespace dd {

struct two_way_bound {
 private:
  std::atomic_bool flag = false;

 public:
  two_way_bound() = default;
  two_way_bound(two_way_bound&&) = delete;
  void operator=(two_way_bound&&) = delete;

  bool try_inform() noexcept {
    bool informed = !flag.exchange(true, std::memory_order::acq_rel);
    if (informed)
      flag.notify_one();
    return informed;
  }
  void wait() noexcept {
    flag.wait(false, std::memory_order::acquire);
  }
  void reset() noexcept {
    flag.store(false, std::memory_order::release);
  }
};

// there are may be only one stop_token. If coroutine accepts stop_token and then args,
// then stop_token in first argument will be filled when coroutine created
struct KELCORO_CO_AWAIT_REQUIRED stop_token {
 private:
  std::atomic_bool* stop_state_;

  friend struct logical_thread_promise;

  constexpr explicit stop_token(std::atomic_bool* stop_state) : stop_state_(stop_state) {
  }

 public:
  stop_token(stop_token&&) = delete;
  void operator=(stop_token&&) = delete;
  // request is always possible, because only way to get token is pass empty_stop_token_t into coroutine
  bool stop_requested() const noexcept {
    return stop_state_->load(std::memory_order::acquire);
  }
};

struct get_stop_token_t {};

namespace this_coro {
// by co_awaiting on this logical thread accesses its own stop token
constexpr inline get_stop_token_t stop_token = {};

}  // namespace this_coro

struct logical_thread_promise {
  std::atomic_bool stop_requested_ = false;
  two_way_bound stopped;

  // accepts copied to coroutine frame arguments if first arg is a stop_token, fills its for using in
  // coroutine only this and nomovability of stop_token guarantees, that token is only one for each coroutine
  logical_thread_promise() noexcept = default;

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<logical_thread_promise>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }

 private:
  // informs owner that is coro is stopped, destroys coro frame if owner is dead
  struct set_stopped_and_wait_or_destroy {
    two_way_bound& link;
    static bool await_ready() noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle) const noexcept {
      if (!link.try_inform())
        handle.destroy();
    }
    static void await_resume() noexcept {
    }
  };
  struct return_stop_token {
    std::atomic_bool* stop_state;

    static bool await_ready() noexcept {
      return false;
    }
    KELCORO_ASSUME_NOONE_SEES bool await_suspend(
        std::coroutine_handle<logical_thread_promise> handle) noexcept {
      stop_state = &handle.promise().stop_requested_;
      return false;  // never suspend rly
    }
    [[nodiscard]] stop_token await_resume() noexcept {
      // do not require move, RVO, this guarantees that stop token always have a state
      return stop_token(stop_state);
    }
  };

 public:
  auto final_suspend() noexcept {
    return set_stopped_and_wait_or_destroy{stopped};
  }
  auto await_transform(get_stop_token_t) const noexcept {
    return return_stop_token{};
  }
  template <typename T>
  decltype(auto) await_transform(T&& v) const noexcept {
    return build_awaiter(std::forward<T>(v));
  }
};

// shared owning of coroutine handle between coroutine object and coroutine frame.
// Frame always dies with a coroutine object, except it was detached(then it deletes itself after co_return)
struct logical_thread : enable_resource_deduction {
  using promise_type = logical_thread_promise;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_;

 public:
  // ctor/owning

  logical_thread() noexcept = default;
  logical_thread(handle_type handle) noexcept : handle_(handle) {
  }

  logical_thread(logical_thread&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }

  logical_thread& operator=(logical_thread&& other) noexcept {
    try_cancel_and_join();
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }

  void swap(logical_thread& other) noexcept {
    std::swap(handle_, other.handle_);
  }
  friend void swap(logical_thread& left, logical_thread& right) noexcept {
    left.swap(right);
  }

  ~logical_thread() {
    try_cancel_and_join();
  }

  // thread-like interface

  [[nodiscard]] bool joinable() const noexcept {
    return handle_ != nullptr;
  }

  // postcondition : !joinable()
  void join() noexcept {
    assert(joinable());
    // created here for nrvo
    handle_.promise().stopped.wait();
    assert(handle_.done());
    handle_.destroy();
    handle_ = nullptr;
  }

  void detach() noexcept {
    assert(joinable());
    if (!handle_.promise().stopped.try_inform()) [[unlikely]]
      handle_.destroy();
    handle_ = nullptr;
  }

  // stopping

  bool stop_possible() const noexcept {
    return handle_ != nullptr;
  }
  bool request_stop() noexcept {
    if (!stop_possible())
      return false;
    handle_.promise().stop_requested_.store(true, std::memory_order::release);
    return true;
  }

 private:
  void try_cancel_and_join() noexcept {
    if (joinable()) {
      request_stop();
      join();
    }
  }
};

template <memory_resource R>
using logical_thread_r = resourced<logical_thread, R>;

namespace pmr {

template <typename Ret>
using logical_thread = ::dd::logical_thread_r<polymorphic_resource>;

}

// TEMPLATE FUNCTION stop

template <typename T>
concept stopable = requires(T& value) {
  value.request_stop();
  value.join();
};
// effectively stops every cancellable
// (all.request stop(), then all.join(), faster then just request_stop() + join() for all)
// only for lvalues
void stop(stopable auto&... args) {
  ((args.request_stop()), ...);
  ((args.join()), ...);
}
template <std::ranges::borrowed_range T>
  requires stopable<std::ranges::range_value_t<T>>
void stop(T&& rng) {
  for (auto& value : rng)
    value.request_stop();
  for (auto& value : rng)
    value.join();
}

}  // namespace dd