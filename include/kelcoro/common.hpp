#pragma once

#include <utility>
#include <type_traits>
#include <coroutine>
#include <cassert>

#include "noexport/macro.hpp"
#include "executor_interface.hpp"

namespace dd::noexport {

enum struct retkind_e : uint8_t { EMPTY, VAL, EX };

template <typename T>
struct retblock_storage {
  alignas(std::max(alignof(T),
                   alignof(std::exception_ptr))) char data[std::max(sizeof(T), sizeof(std::exception_ptr))];

  retblock_storage() = default;
  retblock_storage(retblock_storage&&) = delete;
  void operator=(retblock_storage&&) = delete;

  T* as_value() noexcept {
    return (T*)+data;
  }
  std::exception_ptr* as_ex() noexcept {
    return (std::exception_ptr*)+data;
  }
  const T* as_value() const noexcept {
    return (const T*)+data;
  }
  const std::exception_ptr* as_ex() const noexcept {
    return (const std::exception_ptr*)+data;
  }
};

}  // namespace dd::noexport

namespace dd {

constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;

struct not_movable {
  constexpr not_movable() noexcept = default;
  not_movable(not_movable&&) = delete;
  void operator=(not_movable&&) = delete;
};

struct rvo_tag_t {
  // gcc 12 workaround for co_return {}
  struct do_not_break_construction {
    explicit do_not_break_construction() = default;
  };
  explicit constexpr rvo_tag_t(do_not_break_construction) noexcept {};
};

// Optimization for returning objects in dd::task
//
// Example of potential memory duplication for BigT:
// There are two BigT:
//   * in coroutine frame (local variable)
//   * in coroutine promise for co_return
//
// dd::task<BigT> foo() {
//   BigT t;
//   fill(&t);
//   co_return t;
// }
//
// Optimized version (only one BigT in coroutine promise):
//
// dd::task<BigT> foo() {
//   BigT& t = co_await dd::this_coro::return_place;
//   fill(&t);
//   co_return dd::rvo;
// }
//
constexpr inline const rvo_tag_t rvo = rvo_tag_t{rvo_tag_t::do_not_break_construction{}};

// 'teaches' promise to return
// Note - promise must implement exception logic itself
template <typename T>
struct return_block {
 private:
  noexport::retblock_storage<T> data;
  noexport::retkind_e kind = noexport::retkind_e::EMPTY;

 public:
  return_block() = default;

  ~return_block() {
    switch (kind) {
      case noexport::retkind_e::VAL:
        std::destroy_at(data.as_value());
        break;
      case noexport::retkind_e::EX:
        std::destroy_at(data.as_ex());
        break;
      case noexport::retkind_e::EMPTY:
        break;
    }
  }

  bool has_exception() const noexcept {
    return kind == noexport::retkind_e::EX;
  }
  void set_exception(std::exception_ptr&& e) noexcept {
    assert(kind != noexport::retkind_e::EX);
    assert(e != nullptr);
    if (kind == noexport::retkind_e::VAL)
      std::destroy_at(data.as_value());
    std::construct_at(data.as_ex(), std::move(e));
    kind = noexport::retkind_e::EX;
  }
  // precondition: has_exception()
  std::exception_ptr take_exception() noexcept {
    assert(has_exception());
    std::exception_ptr p = std::move(*data.as_ex());
    std::destroy_at(data.as_ex());
    kind = noexport::retkind_e::EMPTY;
    return p;
  }
  void unhandled_exception() = delete;  // force promise implement it

  template <typename U = T>
  constexpr void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
    assert(kind == noexport::retkind_e::EMPTY);
    std::construct_at(data.as_value(), std::forward<U>(value));
    kind = noexport::retkind_e::VAL;
  }

  constexpr void return_value(rvo_tag_t) noexcept {
    assert(kind != noexport::retkind_e::EMPTY);
  }

  constexpr T&& result() noexcept KELCORO_LIFETIMEBOUND {
    assert(kind == noexport::retkind_e::VAL);
    return std::move(*data.as_value());
  }
  // args for case when T is not default contructible
  // must be used with co_return dd::rvo
  template <typename... Args>
  constexpr T& return_place(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
    assert(kind == noexport::retkind_e::EMPTY);
    T* p = std::construct_at(data.as_value(), std::forward<Args>(args)...);
    kind = noexport::retkind_e::VAL;
    return *p;
  }
};

template <typename T>
struct return_block<T&> {
 private:
  T* storage = nullptr;
  std::exception_ptr ex;

 public:
  return_block() = default;

  ~return_block() = default;

  bool has_exception() const noexcept {
    return ex != nullptr;
  }
  void set_exception(std::exception_ptr&& e) noexcept {
    ex = std::move(e);
  }
  // precondition: has_exception()
  std::exception_ptr take_exception() noexcept {
    assert(has_exception());  // same preconditon as in other specializations
    return std::move(ex);
  }
  void unhandled_exception() = delete;  // force promise implement it

  constexpr void return_value(T& value) noexcept {
    assert(storage == nullptr);
    storage = std::addressof(value);
  }

  constexpr void return_value(rvo_tag_t) noexcept {
    assert(storage != nullptr);
  }

  constexpr T& result() noexcept {
    assert(storage != nullptr);
    return *storage;
  }
  constexpr T*& return_place(T* p = nullptr) {
    return storage = p;
  }
};

template <>
struct return_block<void> {
 private:
  std::exception_ptr ex;

 public:
  return_block() = default;

  ~return_block() = default;

  bool has_exception() const noexcept {
    return ex != nullptr;
  }
  void set_exception(std::exception_ptr&& e) noexcept {
    ex = std::move(e);
  }
  // precondition: has_exception()
  std::exception_ptr take_exception() noexcept {
    assert(has_exception());  // same preconditon as in other specializations
    return std::move(ex);
  }
  void unhandled_exception() = delete;  // force promise implement it

  constexpr void return_void() const noexcept {
  }
  static void result() noexcept {
  }
  static void return_place() noexcept {
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
  explicit get_handle_t() = default;

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
  explicit get_context_t() = default;

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
constexpr inline get_handle_t handle = get_handle_t{};

constexpr inline destroy_coro_t destroy = destroy_coro_t{};

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

  // ASAN produces false positive here (understandable)
#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
  [[gnu::no_sanitize_address]]
  #else
  KELCORO_ASSUME_NOONE_SEES
  #endif
#else
  KELCORO_ASSUME_NOONE_SEES
#endif
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> self) noexcept {
    // move it to stack memory to save from destruction
    auto w = who_waits;
    self.destroy();
    return w ? w : std::noop_coroutine();  // symmetric transfer here
  }
  [[noreturn]] static void await_resume() noexcept {
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

namespace noexport {

template <typename T>
consteval auto do_await_result() {
  static_assert(!ambigious_co_await_lookup<T>);
  if constexpr (has_global_co_await<T>) {
    return std::type_identity<
        decltype(std::declval<decltype(operator co_await(std::declval<T>()))>().await_resume())>{};
  } else if constexpr (has_member_co_await<T>) {
    return std::type_identity<
        decltype(std::declval<decltype(std::declval<T>().operator co_await())>().await_resume())>{};
  } else {
    // co_awaiter
    return std::type_identity<decltype(std::declval<decltype(std::declval<T>())>().await_resume())>{};
  }
}

}  // namespace noexport

// Note: ignores await_transform!
template <co_awaitable T>
using await_result_t = typename decltype(noexport::do_await_result<T>())::type;

}  // namespace dd
