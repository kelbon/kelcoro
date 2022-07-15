#pragma once

#include "common.hpp"

namespace dd {

template <typename Yield, typename Alloc>
struct channel_promise : memory_block<Alloc> {
  using yield_type = Yield;

  yield_type* current_result = nullptr;
  // always setted(by co_await), may change
  std::coroutine_handle<void> current_owner;

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<channel_promise<Yield, Alloc>>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }
  auto final_suspend() noexcept {
    return transfer_control_to{current_owner};
  }

  struct create_value_and_transfer_control_to : transfer_control_to {
    yield_type saved_value;

    std::coroutine_handle<void> await_suspend(std::coroutine_handle<channel_promise> handle) noexcept {
      handle.promise().current_result = std::addressof(saved_value);
      return who_waits;
    }
  };

  // allow yielding

  auto yield_value(yield_type& lvalue) noexcept {
    current_result = std::addressof(lvalue);
    // Interesting fact - i dont really know is it needed? I already returns to
    // current owner because i just need to execute code and its my caller...
    // so return std::suspend_always{} here seems to be same...
    // Same logic works also for task<T>...
    return transfer_control_to{current_owner};
  }

  template <typename T>
  auto yield_value(T&& value) noexcept(std::is_nothrow_constructible_v<yield_type, T&&>) {
    return create_value_and_transfer_control_to{{current_owner}, yield_type{std::forward<T>(value)}};
  }
};

template <typename Yield, typename Alloc = std::allocator<std::byte>>
struct channel {
  using value_type = Yield;
  using promise_type = channel_promise<Yield, Alloc>;
  using handle_type = std::coroutine_handle<promise_type>;

 protected:
  handle_type handle_;

 public:
  constexpr channel() noexcept = default;
  constexpr channel(handle_type handle) : handle_(handle) {
  }

  channel(channel&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }
  channel& operator=(channel&& other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
  }

  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(handle_, nullptr);
  }
  // returns true if no value can be taken from channel
  bool empty() const noexcept {
    return handle_ == nullptr || handle_.done();
  }

  ~channel() {
    if (handle_) [[likely]]
      handle_.destroy();
  }
  auto operator co_await() noexcept {
    struct remember_owner_transfer_control_to {
      handle_type stream_handle;

      bool await_ready() const noexcept {
        assert(stream_handle != nullptr && !stream_handle.done());
        return false;
      }
      std::coroutine_handle<void> await_suspend(std::coroutine_handle<void> owner) noexcept {
        stream_handle.promise().current_owner = owner;
        return stream_handle;
      }
      [[nodiscard]] value_type* await_resume() const noexcept {
        if (!stream_handle.done()) [[likely]]
          return stream_handle.promise().current_result;
        else
          return nullptr;
      }
    };
    return remember_owner_transfer_control_to{handle_};
  }
};

}  // namespace dd
