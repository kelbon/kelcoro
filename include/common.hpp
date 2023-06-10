#pragma once

#include <type_traits>
#include <cstddef>
#include <optional>
#include <coroutine>
#include <cassert>
#include <thread>

#define KELCORO_CO_AWAIT_REQUIRED [[nodiscard("forget co_await?")]]
#ifdef __clang__
#define KELCORO_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define KELCORO_LIFETIMEBOUND
#endif
#if defined(__GNUC__) || defined(__clang__)
#define KELCORO_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define KELCORO_UNREACHABLE __assume(false)
#else
#define KELCORO_UNREACHABLE (void)0
#endif

// TODO undef

namespace dd {

// CONCEPTS about co_await operator

template <typename T>
concept has_member_co_await = requires(T (*value)()) { value().operator co_await(); };

template <typename T>
concept has_global_co_await = requires(T (*value)()) { operator co_await(value()); };

template <typename T>
concept ambigious_co_await_lookup = has_global_co_await<T> && has_member_co_await<T>;

// CONCEPT co_awaiter

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

// CONCEPT co_awaitable

template <typename T>
concept co_awaitable = has_member_co_await<T> || has_global_co_await<T> || co_awaiter<T>;

// Just 'teaches' promises of every coroutine how to allocate memory

// TODO support trailing allocator convention
template <typename Alloc>
struct memory_block {
  // leading allocator convention
  template <typename... Args>
  static void* operator new(std::size_t frame_size, std::allocator_arg_t, Alloc resource, Args&&...) {
    // check for default here, because need to create it by default in operator delete
    if constexpr (std::is_empty_v<Alloc> && std::default_initializable<Alloc>) {
      return resource.allocate(frame_size);
    } else {
      // Fuck alignment!(really)
      std::byte* frame_ptr = reinterpret_cast<std::byte*>(resource.allocate(frame_size + sizeof(Alloc)));
      std::construct_at(reinterpret_cast<Alloc*>(frame_ptr + frame_size), std::move(resource));
      return frame_ptr;
    }
  }
  static void* operator new(std::size_t frame_size)
    requires(std::default_initializable<Alloc>)
  {
    return operator new(frame_size, std::allocator_arg, Alloc{});
  }

  static void operator delete(void* ptr, std::size_t frame_size) noexcept {
    auto* p = reinterpret_cast<std::byte*>(ptr);
    if constexpr (std::is_empty_v<Alloc> && std::default_initializable<Alloc>) {
      Alloc{}.deallocate(p, frame_size);
    } else {  // Fuck aligment again
      auto* resource_on_frame = reinterpret_cast<Alloc*>(p + frame_size);
      // move it from frame, because its in memory which it will deallocate
      auto resource = std::move(*resource_on_frame);
      std::destroy_at(resource_on_frame);
      resource.deallocate(p, frame_size + sizeof(Alloc));
    }
  }
};

// 'teaches' promise to return

template <typename T>
struct return_block {
  using result_type = T;
  // possibly can be replaced with some buffer
  std::optional<result_type> storage = std::nullopt;

  constexpr void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    storage.emplace(std::move(value));
  }
  constexpr T result() noexcept(noexcept(*std::move(storage))) {
    return *std::move(storage);
  }
};
template <>
struct return_block<void> {
  using result_type = void;
  constexpr void return_void() const noexcept {
  }
};

struct [[nodiscard("co_await it!")]] transfer_control_to {
  std::coroutine_handle<void> who_waits;

  bool await_ready() const noexcept {
    assert(who_waits != nullptr);
    return false;
  }
  std::coroutine_handle<void> await_suspend(std::coroutine_handle<void>) noexcept {
    return who_waits;  // symmetric transfer here
  }
  void await_resume() const noexcept {
  }
};

template <std::invocable<> F>
struct [[nodiscard("Dont forget to name it!")]] scope_exit {
  [[no_unique_address]] F todo;

  scope_exit(F todo) : todo(std::move(todo)) {
  }
  constexpr ~scope_exit() noexcept(std::is_nothrow_invocable_v<F&>) {
    todo();
  }
};

template <typename T>
concept executor = requires(T& value) { value.execute([] {}); };

// DEFAULT EXECUTORS

struct noop_executor {
  template <std::invocable F>
  void execute(F&&) const noexcept {
  }
};

struct this_thread_executor {
  template <std::invocable F>
  void execute(F&& f) const noexcept(std::is_nothrow_invocable_v<F&&>) {
    (void)std::forward<F>(f)();
  }
};

struct new_thread_executor {
  template <std::invocable F>
  void execute(F&& f) const {
    std::thread([foo = std::forward<F>(f)]() mutable { (void)std::forward<F>(foo)(); }).detach();
  }
};

template <executor T>
struct KELCORO_CO_AWAIT_REQUIRED jump_on {
 private:
  using stored_type = std::conditional_t<(std::is_empty_v<T> && std::is_default_constructible_v<T>), T,
                                         std::add_pointer_t<T>>;
  [[no_unique_address]] stored_type exe_;

  constexpr void store(T& e) {
    if constexpr (std::is_pointer_v<stored_type>)
      exe_ = std::addressof(e);
  }

 public:
  constexpr jump_on(T& e KELCORO_LIFETIMEBOUND) noexcept {
    store(e);
  }
  constexpr jump_on(T&& e KELCORO_LIFETIMEBOUND) noexcept {
    store(e);
  }
  constexpr bool await_ready() const noexcept {
    return false;
  }
  constexpr void await_suspend(std::coroutine_handle<void> handle) const {
    if constexpr (std::is_pointer_v<stored_type>)
      exe_->execute(handle);
    else
      exe_.execute(handle);
  }
  constexpr void await_resume() const noexcept {
  }
};

template <typename Promise>
struct return_handle_t {
 private:
  std::coroutine_handle<Promise> handle_;

  return_handle_t(return_handle_t&&) = delete;

 public:
  return_handle_t() = default;

  static bool await_ready() noexcept {
    return false;
  }
  bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    handle_ = handle;
    return false;
  }
  std::coroutine_handle<void> await_resume() const noexcept {
    return handle_;
  }
};

struct KELCORO_CO_AWAIT_REQUIRED get_handle_t {};

namespace this_coro {

// provides access to inner handle of coroutine
constexpr inline get_handle_t handle = {};
// TODO? context?
}  // namespace this_coro

// imitating compiler behaviour for co_await expression mutation into awaiter(without await_transform)
// very usefull if you have await_transform and for all other types you need default behavior
template <co_awaitable T>
constexpr decltype(auto) build_awaiter(T&& value) {
  static_assert(!ambigious_co_await_lookup<T>);
  if constexpr (co_awaiter<T&&>)  // first bcs can have operator co_await too
    return std::forward<T>(value);
  else if constexpr (has_global_co_await<T&&>)
    return operator co_await(std::forward<T>(value));
  else if constexpr (has_member_co_await<T&&>)
    return std::forward<T>(value).operator co_await();
}

}  // namespace dd