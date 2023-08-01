#pragma once

#include "common.hpp"

namespace dd {

struct job_promise : enable_memory_resource_support, return_block<void> {
  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  static constexpr std::suspend_never final_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<job_promise>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }
};

struct [[maybe_unused]] job {
  using promise_type = job_promise;
  using handle_type = std::coroutine_handle<promise_type>;
  constexpr job(handle_type) noexcept {
  }
};

}  // namespace dd