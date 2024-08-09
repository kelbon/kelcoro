#pragma once

#include "common.hpp"
#include "async_task.hpp"
#include "memory_support.hpp"

/*
same as dd::task, but guaranteed to nothrow (terminate), slightly more effective
*/

namespace dd {

template <typename Result>
struct noexcept_task_promise : return_block<Result> {
  std::coroutine_handle<void> who_waits;

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<noexcept_task_promise<Result>>::from_promise(*this);
  }
  [[noreturn]] static void unhandled_exception() noexcept {
    std::terminate();
  }
  auto final_suspend() noexcept {
    // who_waits always setted because task not started or co_awaited
    return transfer_control_to{who_waits};
  }
};

// single value generator that returns a value with a co_return
template <typename Result>
struct noexcept_task : enable_resource_deduction {
  using result_type = Result;
  using promise_type = noexcept_task_promise<Result>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_;

 public:
  constexpr noexcept_task() noexcept = default;
  constexpr noexcept_task(handle_type handle) noexcept : handle_(handle) {
  }

  noexcept_task(noexcept_task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }
  noexcept_task& operator=(noexcept_task&& other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
  }

  ~noexcept_task() {
    if (handle_)
      handle_.destroy();
  }
  // returns true if task cant be co_awaited
  constexpr bool empty() const noexcept {
    return handle_ == nullptr || handle_.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }
  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(handle_, nullptr);
  }
#if defined(__GNUC__) || defined(__clang__)
  // postcondition: empty(), task result ignored
  // returns released task handle
  // if stop_at_end is false, then task will delete itself at end, otherwise handle.destroy() should be called
  handle_type start_and_detach(bool stop_at_end = false) {
    if (!handle_)
      return nullptr;
    handle_type h = std::exchange(handle_, nullptr);
    // task resumes itself at end and destroys itself or just stops with noop_coroutine
    h.promise().who_waits = stop_at_end ? std::noop_coroutine() : std::coroutine_handle<>(h);
    h.resume();
    return h;
  }
#endif
  // blocking
  result_type get() noexcept {
    assert(!empty());
    return [](noexcept_task t) -> async_task<result_type> { co_return co_await t; }(std::move(*this)).get();
  }

 private:
  struct remember_waiter_and_start_task_t {
    handle_type task_handle;

    static bool await_ready() noexcept {
      return false;
    }
    KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<void> await_suspend(
        std::coroutine_handle<void> handle) const noexcept {
      task_handle.promise().who_waits = handle;
      // symmetric transfer control to task
      return task_handle;
    }
    [[nodiscard]] std::add_rvalue_reference_t<result_type> await_resume() noexcept {
      return task_handle.promise().result();
    }
  };

 public:
  constexpr auto operator co_await() noexcept {
    assert(!empty());
    return remember_waiter_and_start_task_t{handle_};
  }
};

template <typename Ret, memory_resource R>
using noexcept_task_r = resourced<noexcept_task<Ret>, R>;

namespace pmr {

template <typename Ret>
using noexcept_task = ::dd::noexcept_task_r<Ret, polymorphic_resource>;

}

template <typename R>
struct operation_hash<std::coroutine_handle<noexcept_task_promise<R>>> {
  operation_hash_t operator()(std::coroutine_handle<noexcept_task_promise<R>> handle) noexcept {
    return operation_hash<std::coroutine_handle<>>()(handle.promise().who_waits);
  }
};

}  // namespace dd
