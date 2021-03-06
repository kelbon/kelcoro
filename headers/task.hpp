#pragma once

#include "common.hpp"

namespace dd {

template <typename Result, typename Alloc>
struct task_promise : memory_block<Alloc>, return_block<Result> {
  std::coroutine_handle<void> who_waits;
  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<task_promise<Result, Alloc>>::from_promise(*this);
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }
  auto final_suspend() noexcept {
    // who_waits always setted because task not started or co_awaited
    return transfer_control_to{who_waits};
  }
};

// single value generator that returns a value with a co_return. Can be sent on executor(have operator())
template <typename Result, typename Alloc = std::allocator<std::byte>>
struct task {
  using result_type = Result;
  using promise_type = task_promise<Result, Alloc>;
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
  constexpr auto operator co_await() noexcept {
    struct remember_waiter_and_start_task_t {
      handle_type task_handle;

      bool await_ready() const noexcept {
        assert(task_handle != nullptr && !task_handle.done());
        return false;
      }
      std::coroutine_handle<void> await_suspend(std::coroutine_handle<void> handle) const noexcept {
        task_handle.promise().who_waits = handle;
        // symmetric transfer control to task
        return task_handle;
      }
      [[nodiscard]] result_type await_resume() {
        if constexpr (!std::is_void_v<result_type>)
          return task_handle.promise().result();
      }
    };
    return remember_waiter_and_start_task_t{handle_};
  }
};

}  // namespace dd