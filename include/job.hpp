#pragma once

#include "common.hpp"

namespace dd {

template <typename Alloc = std::allocator<std::byte>>
struct job_promise : memory_block<Alloc>, return_block<void> {
  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  static constexpr std::suspend_never final_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<job_promise<Alloc>>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }
  auto await_transform(get_handle_t) const noexcept {
    return return_handle_t<job_promise>{};
  }
  template <typename T>
  decltype(auto) await_transform(T&& v) const noexcept {
    return build_awaiter(std::forward<T>(v));
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }
};

template <typename Alloc>
struct [[maybe_unused]] job_mm {
  using promise_type = job_promise<Alloc>;
  using handle_type = std::coroutine_handle<promise_type>;
  // TODO bugreport почему не конвертится к coroutine_handle<> неявно
  constexpr job_mm(handle_type) noexcept {
  }
};

using job = job_mm<std::allocator<std::byte>>;

}  // namespace dd