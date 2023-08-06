#pragma once

#include <type_traits>
#include <cstddef>
#include <optional>
#include <coroutine>
#include <cassert>
#include <thread>
#include <memory_resource>

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

#if defined(__GNUC__) || defined(__clang__)
#define KELCORO_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KELCORO_ALWAYS_INLINE __forceinline
#else
#define KELCORO_ALWAYS_INLINE inline
#endif

#if defined(__GNUC__) || defined(__clang__)
#define KELCORO_PURE __attribute__((pure))
#else
#define KELCORO_PURE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define KELCORO_ASSUME(condition) __builtin_assume(condition)
#elif defined(_MSC_VER)
#define KELCORO_ASSUME(condition) __assume(condition)
#else
#define KELCORO_ASSUME(condition)
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

namespace noexport {

// caches value (just because its C-non-inline-call...)
static std::pmr::memory_resource* const new_delete_resource = std::pmr::new_delete_resource();

// used to pass argument to promise operator new, say 'ty' to standard writers,
// i want separate coroutine logic from how it allocates frame
inline thread_local std::pmr::memory_resource* co_memory_resource = new_delete_resource;

}  // namespace noexport

// passes 'implicit' argument to all coroutines allocatinon on this thread until object dies
// usage:
//    foo_t local = dd::with_resource{r}, create_coro(), foo();
// OR
//    dd::with_resource{r}, [&] { ... }();
// OR
//    dd::with_resource name{r};
//    ... code with coroutines ...
struct [[nodiscard]] with_resource {
 private:
  std::pmr::memory_resource* old;

 public:
  explicit with_resource(std::pmr::memory_resource& m) noexcept
      : old(std::exchange(noexport::co_memory_resource, &m)) {
    KELCORO_ASSUME(old != nullptr);
  }
  with_resource(with_resource&&) = delete;
  void operator=(with_resource&&) = delete;

  ~with_resource() {
    noexport::co_memory_resource = old;
  }
};

// TODO
// // basically free list with customizable max blocks
// // must be good because coroutines have same sizes,
// // its easy to reuse memory for them
// struct co_memory_resource { ... };
// inheritor(coroutine promise) recieves allocation(std::pmr::memory_resource) support
// usage: see with_resource(R)
struct enable_memory_resource_support {
  static void* operator new(std::size_t frame_size) {
    auto* r = noexport::co_memory_resource;
    auto* p = (std::byte*)r->allocate(frame_size + sizeof(void*), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    std::byte* alloc_ptr = p + frame_size;
    std::memcpy(alloc_ptr, &r, sizeof(void*));
    return p;
  }

  static void operator delete(void* ptr, std::size_t frame_size) noexcept {
    auto* alloc_ptr = (std::byte*)ptr + frame_size;
    std::pmr::memory_resource* m;
    std::memcpy(&m, alloc_ptr, sizeof(void*));
    m->deallocate(ptr, frame_size + sizeof(void*), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
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
  static constexpr void await_resume() noexcept {
  }
};

// TODO normal scope guards
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

struct KELCORO_CO_AWAIT_REQUIRED get_handle_t {
 private:
  struct return_handle {
    std::coroutine_handle<> handle_;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      handle_ = handle;
      return false;
    }
    std::coroutine_handle<void> await_resume() const noexcept {
      return handle_;
    }
  };

 public:
  return_handle operator co_await() const noexcept {
    return return_handle{};
  }
};

namespace this_coro {

// provides access to inner handle of coroutine
constexpr inline get_handle_t handle = {};

// returns last resource which was setted on this thread by 'with_resource'
// guaranteed to be alive only in coroutine for which it was setted
inline std::pmr::memory_resource& current_memory_resource() noexcept {
  return *noexport::co_memory_resource;
}

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

struct always_done_coroutine_promise {
  static void* operator new(std::size_t frame_size) {
    assert(frame_size < 128 && "hack dont works(");
    // there are only one always done coroutine.
    // Its always without state, only compiler-inner things
    alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) static char hack[128];
    return hack;
  }

  static void operator delete(void* ptr, std::size_t frame_size) noexcept {
    // noop
  }
  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<always_done_coroutine_promise>::from_promise(*this);
  }
  [[noreturn]] static void unhandled_exception() noexcept {
    assert(false);  // must be unreachable
    std::abort();
  }
  static constexpr void return_void() noexcept {
  }
  static constexpr std::suspend_always final_suspend() noexcept {
    return {};
  }
};

using always_done_coroutine_handle = std::coroutine_handle<always_done_coroutine_promise>;

}  // namespace dd

namespace std {

template <typename... Args>
struct coroutine_traits<::dd::always_done_coroutine_handle, Args...> {
  using promise_type = ::dd::always_done_coroutine_promise;
};

}  // namespace std

namespace dd::noexport {

static inline const auto always_done_coro = []() -> always_done_coroutine_handle { co_return; }();

}  // namespace dd::noexport

namespace dd {
// TODO тип для обмена сообщениями между каналом/генератором и потребителем
// что то типа упаковки над указателем, желательно ещё НЕ назвать его socket
// мб connection<T> + send/recieve сообщений
// или просто send/reseive обёртки на lvalue ссылкой... Тоже неплохо, а в генераторе
// сделать например перегрузку yield
// важно то, что это не должно зависеть от возвращаемого типа генератора/канала!
// хм, recieve должен на вход генератор чтоли получать... И иметь оператор co_await для канала
// recieve(gen) co_await recieve(channel)

// returns handle for which
// .done() == true
// .destroy() is noop
// .resume() produces undefined behavior
inline always_done_coroutine_handle always_done_coroutine() noexcept {
  return noexport::always_done_coro;
}

template <typename R>
struct elements_of {
  [[no_unique_address]] R rng;

#if __cpp_aggregate_paren_init < 201902L
  // may be clang will never support aggregate () initialization...
  constexpr elements_of(std::type_identity_t<R> rng) noexcept : rng(static_cast<R&&>(rng)) {
  }
#endif
};
template <typename R>
elements_of(R&&) -> elements_of<R&&>;

KELCORO_ALWAYS_INLINE void assume(bool b) noexcept {
  assert(b);
  KELCORO_ASSUME(b);
}

KELCORO_ALWAYS_INLINE void assume_not_null(std::coroutine_handle<> h) noexcept {
  assume(h.address() != nullptr);
}

// tag for yielding from generator/channel by reference.
// This means, if 'value' will be changed by caller it will be
// observable from coroutine
// example:
// int i = 0;
// co_yield ref{i};
// ..here i may be != 0, if caller changed it
template <typename Yield>
struct by_ref {
  Yield& value;
};

template <typename Yield>
by_ref(Yield&) -> by_ref<Yield>;

}  // namespace dd