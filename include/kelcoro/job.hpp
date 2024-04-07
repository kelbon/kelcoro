#pragma once

#include "memory_support.hpp"

namespace dd {

struct job_promise;

struct job : enable_resource_deduction {
  using promise_type = job_promise;
  using handle_type = std::coroutine_handle<promise_type>;

  handle_type handle = nullptr;
  constexpr job() noexcept = default;
  constexpr job(handle_type h) noexcept : handle(h) {
  }
};

template <memory_resource R>
using job_r = resourced<job, R>;

namespace pmr {

template <typename Ret>
using job = ::dd::job_r<polymorphic_resource>;

}

struct job_promise {
  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  static constexpr std::suspend_never final_suspend() noexcept {
    return {};
  }
  job get_return_object() noexcept {
    return job{std::coroutine_handle<job_promise>::from_promise(*this)};
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] static void unhandled_exception() noexcept {
    std::terminate();
  }
};

}  // namespace dd
