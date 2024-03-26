#pragma once

#include "common.hpp"
#include "memory_support.hpp"

namespace dd {

template <typename Result>
struct task_promise : return_block<Result> {
  std::coroutine_handle<void> who_waits;
  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<task_promise<Result>>::from_promise(*this);
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }
  auto final_suspend() noexcept {
    // who_waits always setted because task not started or co_awaited
    return transfer_control_to{who_waits};
  }
};

// single value generator that returns a value with a co_return
template <typename Result>
struct task : enable_resource_deduction {
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
  bool empty() const noexcept {
    return handle_ == nullptr || handle_.done();
  }
  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(handle_, nullptr);
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
