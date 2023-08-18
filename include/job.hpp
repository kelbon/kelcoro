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

struct [[maybe_unused]] job : enable_resource_deduction {
  using promise_type = job_promise;
  using handle_type = std::coroutine_handle<promise_type>;
  constexpr job(handle_type) noexcept {
  }
};

template <memory_resource R>
using job_r = resourced<job, R>;

namespace pmr {

template <typename Ret>
using job = ::dd::job_r<polymorphic_resource>;

}

}  // namespace dd