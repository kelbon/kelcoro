#pragma once

#include "common.hpp"

namespace dd {

struct job_promise {
  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  static constexpr std::suspend_never final_suspend() noexcept {
    return {};
  }
  auto get_return_object() noexcept {
    return std::coroutine_handle<job_promise>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] static void unhandled_exception() noexcept {
    std::terminate();
  }
};

template <memory_resource R = select_from_signature>
struct [[maybe_unused]] job_r : enable_resource_support<R> {
  using promise_type = job_promise;
  using handle_type = std::coroutine_handle<promise_type>;
  constexpr job_r(handle_type) noexcept {
  }
};

using job = job_r<>;

}  // namespace dd