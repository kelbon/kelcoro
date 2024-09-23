#pragma once

#include "common.hpp"
#include "kelcoro/async_task.hpp"
#include "memory_support.hpp"

namespace dd {

template <typename Result>
struct task_promise : return_block<Result> {
  std::coroutine_handle<void> who_waits;
  std::exception_ptr exception = nullptr;

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<task_promise<Result>>::from_promise(*this);
  }
  void unhandled_exception() noexcept {
    exception = std::current_exception();
  }
  auto final_suspend() noexcept {
    return transfer_control_to{who_waits};
  }
};

// single value generator that returns a value with a co_return
template <typename Result>
struct [[nodiscard]] task : enable_resource_deduction {
  using result_type = Result;
  using promise_type = task_promise<Result>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_;

 public:
  constexpr task() noexcept = default;
  constexpr task(handle_type handle) noexcept : handle_(handle) {
  }

  task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }
  task& operator=(task&& other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
  }

  ~task() {
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
  [[nodiscard]] handle_type raw_handle() const noexcept {
    return handle_;
  }

  // postcondition: empty(), task result ignored
  // returns released task handle
  // if stop_at_end is false, then task will delete itself at end, otherwise handle.destroy() should be called
  handle_type start_and_detach(bool stop_at_end = false) {
    if (!handle_)
      return nullptr;
    handle_type h = std::exchange(handle_, nullptr);
    // task resumes itself at end and destroys itself or just stops with noop_coroutine
    h.promise().who_waits = stop_at_end ? std::noop_coroutine() : std::coroutine_handle<>(nullptr);
    h.resume();
    return h;
  }

  // blocking
  result_type get() {
    assert(!empty());
    return [](task t) -> async_task<result_type> { co_return co_await t; }(std::move(*this)).get();
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
    [[nodiscard]] std::add_rvalue_reference_t<result_type> await_resume() {
      auto& promise = task_handle.promise();
      if (promise.exception) [[unlikely]]
        std::rethrow_exception(promise.exception);
      return promise.result();
    }
  };

 public:
  constexpr auto operator co_await() noexcept {
    assert(!empty());
    return remember_waiter_and_start_task_t{handle_};
  }
};

template <typename Ret, memory_resource R>
using task_r = resourced<task<Ret>, R>;

namespace pmr {

template <typename Ret>
using task = ::dd::task_r<Ret, polymorphic_resource>;

}

template <typename R>
struct operation_hash<std::coroutine_handle<task_promise<R>>> {
  operation_hash_t operator()(std::coroutine_handle<task_promise<R>> handle) noexcept {
    return operation_hash<std::coroutine_handle<>>()(handle.promise().who_waits);
  }
};

}  // namespace dd
