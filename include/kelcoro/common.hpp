#pragma once

#include <utility>
#include <type_traits>
#include <optional>
#include <coroutine>
#include <cassert>

#include "noexport/macro.hpp"
#include "executor_interface.hpp"

namespace dd {

constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;

struct not_movable {
  constexpr not_movable() noexcept = default;
  not_movable(not_movable&&) = delete;
  void operator=(not_movable&&) = delete;
};

// 'teaches' promise to return
template <typename T>
struct return_block {
  std::optional<T> storage = std::nullopt;

  template <typename U = T>
  constexpr void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
    storage.emplace(std::forward<U>(value));
  }
  constexpr T&& result() noexcept KELCORO_LIFETIMEBOUND {
    assert(storage.has_value());
    return std::move(*storage);
  }
};
template <typename T>
struct return_block<T&> {
  T* storage = nullptr;

  constexpr void return_value(T& value) noexcept {
    storage = std::addressof(value);
  }
  constexpr T& result() noexcept {
    assert(storage != nullptr);
    return *storage;
  }
};
template <>
struct return_block<void> {
  constexpr void return_void() const noexcept {
  }
  static void result() noexcept {
  }
};

// if transfers to nullptr, then behaves as suspend_never
struct [[nodiscard("co_await it!")]] transfer_control_to {
  std::coroutine_handle<> who_waits;

  bool await_ready() const noexcept {
    return !who_waits;
  }
  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
    return who_waits;  // symmetric transfer here
  }
  static constexpr void await_resume() noexcept {
  }
};

template <std::invocable<> F>
struct [[nodiscard("Dont forget to name it!")]] scope_exit {
  KELCORO_NO_UNIQUE_ADDRESS F todo;

  scope_exit(F todo) : todo(std::move(todo)) {
  }
  constexpr ~scope_exit() noexcept(std::is_nothrow_invocable_v<F&>) {
    todo();
  }
};

// destroys coroutine in which awaited for
struct KELCORO_CO_AWAIT_REQUIRED destroy_coro_t {
  static bool await_ready() noexcept {
    return false;
  }
  static void await_suspend(std::coroutine_handle<> handle) noexcept {
    handle.destroy();
  }
  static void await_resume() noexcept {
    KELCORO_UNREACHABLE;
  }
};

template <typename F>
struct KELCORO_CO_AWAIT_REQUIRED suspend_and_t {
  KELCORO_NO_UNIQUE_ADDRESS F fn;

  constexpr static bool await_ready() noexcept {
    return false;
  }
  template <typename P>
  constexpr auto await_suspend(std::coroutine_handle<P> handle) noexcept {
    return fn(handle);
  }
  constexpr static void await_resume() noexcept {
  }
};

template <typename F>
suspend_and_t(F&&) -> suspend_and_t<std::remove_cvref_t<F>>;

namespace this_coro {

struct KELCORO_CO_AWAIT_REQUIRED get_handle_t {
  template <typename PromiseType>
  struct awaiter {
    std::coroutine_handle<PromiseType> handle_;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    KELCORO_ASSUME_NOONE_SEES bool await_suspend(std::coroutine_handle<PromiseType> handle) noexcept {
      handle_ = handle;
      return false;
    }
    [[nodiscard]] std::coroutine_handle<PromiseType> await_resume() const noexcept {
      return handle_;
    }
  };

  awaiter<void> operator co_await() const noexcept {
    return awaiter<void>{};
  }
};

struct get_context_t {
  template <typename Ctx>
  struct awaiter {
    Ctx* ctx;

    static bool await_ready() noexcept {
      return false;
    }
    template <typename T>
    bool await_suspend(std::coroutine_handle<T> h) {
      ctx = std::addressof(h.promise().ctx);
      return false;
    }
    [[nodiscard]] Ctx& await_resume() const noexcept {
      return *ctx;
    }
  };
};

// provides access to inner handle of coroutine
constexpr inline get_handle_t handle = {};

constexpr inline destroy_coro_t destroy = {};

// co_awaiting on this function suspends coroutine and invokes 'fn' with coroutine handle.
// await suspend returns what 'fn' returns!
constexpr auto suspend_and(auto&& fn) {
  return suspend_and_t(std::forward<decltype(fn)>(fn));
}

struct [[nodiscard("co_await it!")]] destroy_and_transfer_control_to {
  std::coroutine_handle<> who_waits;

#if !KELCORO_AGGREGATE_PAREN_INIT
  destroy_and_transfer_control_to() = default;
  explicit destroy_and_transfer_control_to(std::coroutine_handle<> h) noexcept : who_waits(h) {
  }
#endif
  static bool await_ready() noexcept {
    return false;
  }
  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(std::coroutine_handle<> self) noexcept {
    // move it to stack memory to save from destruction
    auto w = who_waits;
    self.destroy();
    return w ? w : std::noop_coroutine();  // symmetric transfer here
  }
  static void await_resume() noexcept {
    KELCORO_UNREACHABLE;
  }
};

}  // namespace this_coro

template <typename T>
concept has_member_co_await = requires(T (*value)()) { value().operator co_await(); };

template <typename T>
concept has_global_co_await = requires(T (*value)()) { operator co_await(value()); };

template <typename T>
concept ambigious_co_await_lookup = has_global_co_await<T> && has_member_co_await<T>;

template <typename T>
concept co_awaiter = requires(T value) {
  { value.await_ready() } -> std::same_as<bool>;
  // cant check await_suspend here because:
  // case: value.await_suspend(coroutine_handle<>{}) - may be non convertible to concrete
  // T in signature of await_suspend case: value.await_suspend(nullptr) - may be template
  // signature, compilation error(cant deduct type) another case - signature with
  // requires, impossible to know how to call it
  value.await_resume();
};

template <typename T>
concept co_awaitable = has_member_co_await<T> || has_global_co_await<T> || co_awaiter<T>;

// imitating compiler behaviour for co_await expression mutation into awaiter(without await_transform)
template <co_awaitable T>
[[nodiscard]] constexpr decltype(auto) build_awaiter(T&& value) {
  static_assert(!ambigious_co_await_lookup<T>);
  if constexpr (co_awaiter<T&&>)  // first bcs can have operator co_await too
    return std::forward<T>(value);
  else if constexpr (has_global_co_await<T&&>)
    return operator co_await(std::forward<T>(value));
  else if constexpr (has_member_co_await<T&&>)
    return std::forward<T>(value).operator co_await();
}

}  // namespace dd
