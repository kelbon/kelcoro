#pragma once

#include <utility>
#include <type_traits>
#include <cstddef>
#include <optional>
#include <coroutine>
#include <cassert>
#include <thread>
#include <memory_resource>

#include "operation_hash.hpp"

#define KELCORO_CO_AWAIT_REQUIRED [[nodiscard("forget co_await?")]]

#if defined(__GNUC__) || defined(__clang__)
#define KELCORO_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define KELCORO_UNREACHABLE __assume(false)
#else
#define KELCORO_UNREACHABLE assert(false)
#endif

#if !defined(NDEBUG)
#define KELCORO_ASSUME(expr) assert(expr)
#elif defined(_MSC_VER)
#define KELCORO_ASSUME(expr) __assume((expr))
#elif defined(__clang__)
#define KELCORO_ASSUME(expr) __builtin_assume((expr))
#elif defined(__GNUC__)
#define KELCORO_ASSUME(expr) \
  if (!(expr))               \
  __builtin_unreachable()
#else
#define KELCORO_ASSUME(expr) (void)(expr)
#endif

// for some implementation reasons clang adds noinline on 'await_suspend'
// https://github.com/llvm/llvm-project/issues/64945
// As workaround to not affect performance I explicitly mark 'await_suspend' as always inline
// if no one can observe changes on coroutine frame after 'await_suspend' start until its end(including
// returning)
#ifdef __clang__
#define KELCORO_ASSUME_NOONE_SEES  // disabled, dont want to provoke clang bugs [[gnu::always_inline]]
#else
#define KELCORO_ASSUME_NOONE_SEES
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define KELCORO_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif defined(__clang__)
#define KELCORO_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define KELCORO_LIFETIMEBOUND
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define KELCORO_PURE
#else
#define KELCORO_PURE [[gnu::pure]]
#endif

#ifdef _MSC_VER
#define KELCORO_MSVC_EBO __declspec(empty_bases)
#else
#define KELCORO_MSVC_EBO
#endif

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(no_unique_address)
#define KELCORO_NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define KELCORO_NO_UNIQUE_ADDRESS
#endif
#else
#define KELCORO_NO_UNIQUE_ADDRESS
#endif

namespace dd {

struct not_movable {
  constexpr not_movable() noexcept = default;
  not_movable(not_movable&&) = delete;
  void operator=(not_movable&&) = delete;
};

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

// concept of type which can be returned from function or yielded from generator
// that is - not function, not array, not cv-qualified (its has no )
// additionally reference is not yieldable(std::ref exists...)
template <typename T>
concept yieldable = std::same_as<std::decay_t<T>, T> && (!std::is_void_v<T>);

template <typename T>
concept memory_resource = (!std::is_reference_v<T>)&&requires(T value, size_t sz, size_t align, void* ptr) {
  { value.allocate(sz, align) } -> std::convertible_to<void*>;
  { value.deallocate(ptr, sz, align) } noexcept -> std::same_as<void>;
  requires std::is_nothrow_move_constructible_v<T>;
  requires alignof(T) <= alignof(std::max_align_t);
  requires !(std::is_empty_v<T> && !std::default_initializable<T>);
};

namespace noexport {

template <typename T>
concept coro = requires { typename T::promise_type; };

}

namespace pmr {

struct polymorphic_resource {
 private:
  std::pmr::memory_resource* passed;

  static auto& default_resource() {
    // never null
    static std::atomic<std::pmr::memory_resource*> r = std::pmr::new_delete_resource();
    return r;
  }
  static auto& passed_resource() {
    thread_local constinit std::pmr::memory_resource* r = nullptr;
    return r;
  }
  friend std::pmr::memory_resource& get_default_resource() noexcept;
  friend std::pmr::memory_resource& set_default_resource(std::pmr::memory_resource&) noexcept;
  friend void pass_resource(std::pmr::memory_resource&) noexcept;

 public:
  polymorphic_resource() noexcept : passed(std::exchange(passed_resource(), nullptr)) {
    if (!passed)
      passed = std::pmr::get_default_resource();
    assert(passed != nullptr);
  }
  void* allocate(size_t sz, size_t align) {
    return passed->allocate(sz, align);
  }
  void deallocate(void* p, std::size_t sz, std::size_t align) noexcept {
    passed->deallocate(p, sz, align);
  }
};

inline std::pmr::memory_resource& get_default_resource() noexcept {
  return *polymorphic_resource::default_resource().load(std::memory_order_acquire);
}

inline std::pmr::memory_resource& set_default_resource(std::pmr::memory_resource& r) noexcept {
  return *polymorphic_resource::default_resource().exchange(&r, std::memory_order_acq_rel);
}
inline void pass_resource(std::pmr::memory_resource& m) noexcept {
  polymorphic_resource::passed_resource() = &m;
}

}  // namespace pmr

consteval size_t coroframe_align() {
  // Question: what will be if coroutine local contains alignas(64) int i; ?
  // Answer: (quote from std::generator paper)
  //  "Let BAlloc be allocator_traits<A>::template rebind_alloc<U> where U denotes an unspecified type
  // whose size and alignment are both _STDCPP_DEFAULT_NEW_ALIGNMENT__"
  return __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

// TODO free list with customizable max blocks
//  must be good because coroutines have same sizes,
//  its easy to reuse memory for them
// TODO info about current channel/generator call, is_top, handle, iteration from top to bot/reverse, swap two
//  generators in chain
//  struct co_memory_resource { ... };

// when passed into coroutine coro will allocate/deallocate memory using this resource
template <memory_resource R>
struct with_resource {
  KELCORO_NO_UNIQUE_ADDRESS R resource;
};
template <typename X>
with_resource(X&&) -> with_resource<std::remove_cvref_t<X>>;

namespace noexport {

template <typename... Args>
struct find_resource_tag : std::type_identity<void> {};
template <typename R, typename... Args>
struct find_resource_tag<with_resource<R>, Args...> : std::type_identity<R> {};
template <typename Head, typename... Tail>
struct find_resource_tag<Head, Tail...> : find_resource_tag<Tail...> {};

}  // namespace noexport

// searches for 'with_resource' in Types and returns first finded or void if no such
template <typename... Types>
using find_resource_tag_t = typename noexport::find_resource_tag<std::remove_cvref_t<Types>...>::type;

namespace noexport {

template <typename... Types>
consteval bool contains_1_resource_tag() {
  return 1 == (!std::is_void_v<find_resource_tag_t<Types>> + ... + 0);
}

constexpr auto only_for_resource(auto&& foo, auto&&... args) {
  static_assert(contains_1_resource_tag<decltype(args)...>());
  auto try_one = [&](auto& x) {
    if constexpr (contains_1_resource_tag<decltype(x)>())
      foo(x.resource);
  };
  (try_one(args), ...);
}

template <size_t RequiredPadding>
constexpr size_t padding_len(size_t sz) noexcept {
  enum { P = RequiredPadding };
  static_assert(P != 0);
  return ((P - sz) % P) % P;
}

}  // namespace noexport

// inheritor(coroutine promise) may be allocated with 'R'
// using 'with_resource' tag or default constructed 'R'
template <memory_resource R>
struct overload_new_delete {
 private:
  enum { frame_align = std::max(coroframe_align(), alignof(R)) };
  static void* do_allocate(size_t frame_sz, R& r) {
    if constexpr (std::is_empty_v<R>)
      return (void*)r.allocate(frame_sz, coroframe_align());
    else {
      const size_t padding = noexport::padding_len<frame_align>(frame_sz);
      const size_t bytes_count = frame_sz + padding + sizeof(R);
      std::byte* p = (std::byte*)r.allocate(bytes_count, frame_align);
      std::byte* resource_place = p + frame_sz + padding;
      std::construct_at((R*)resource_place, std::move(r));
      return p;
    }
  }

 public:
  static void* operator new(size_t frame_sz)
    requires(std::default_initializable<R>)
  {
    R r{};
    return do_allocate(frame_sz, r);
  }
  template <typename... Args>
    requires(std::same_as<find_resource_tag_t<Args...>, R> && noexport::contains_1_resource_tag<Args...>())
  static void* operator new(std::size_t frame_sz, Args&&... args) {
    void* p;
    noexport::only_for_resource([&](auto& resource) { p = do_allocate(frame_sz, resource); }, args...);
    return p;
  }
  static void operator delete(void* ptr, std::size_t frame_sz) noexcept {
    if constexpr (std::is_empty_v<R>) {
      R r{};
      r.deallocate(ptr, frame_sz, coroframe_align());
    } else {
      const size_t padding = noexport::padding_len<frame_align>(frame_sz);
      const size_t bytes_count = frame_sz + padding + sizeof(R);
      R* onframe_resource = (R*)((std::byte*)ptr + frame_sz + padding);
      assert((((uintptr_t)onframe_resource % alignof(R)) == 0));
      R r = std::move(*onframe_resource);
      std::destroy_at(onframe_resource);
      r.deallocate(ptr, bytes_count, frame_align);
    }
  }
};

struct enable_resource_deduction {};

// creates type of coroutine which may be allocated with resource 'R'
// disables resource deduction
// typical usage: aliases like generator_r/channel_r/etc
// for not duplicating code and not changing signature with default constructible resources
// see dd::pmr::generator as example
template <typename Coro, memory_resource R>
struct KELCORO_MSVC_EBO resourced : Coro {
  using resource_type = R;

  using Coro::Coro;
  using Coro::operator=;

  constexpr resourced(auto&&... args)
    requires(std::constructible_from<Coro, decltype(args)...>)
      : Coro(std::forward<decltype(args)>(args)...) {
  }

  constexpr Coro& decay() & noexcept {
    return *this;
  }
  constexpr Coro&& decay() && noexcept {
    return std::move(*this);
  }
  constexpr const Coro& decay() const& noexcept {
    return *this;
  }
  constexpr const Coro&& decay() const&& noexcept {
    return std::move(*this);
  }
};

template <typename Promise, memory_resource R>
struct KELCORO_MSVC_EBO resourced_promise : Promise, overload_new_delete<R> {
  using Promise::Promise;
  using Promise::operator=;

  constexpr resourced_promise(auto&&... args)
    requires(std::constructible_from<Promise, decltype(args)...>)
      : Promise(std::forward<decltype(args)>(args)...) {
  }

  using overload_new_delete<R>::operator new;
  using overload_new_delete<R>::operator delete;

  // assume sizeof and alignof of *this is equal with 'Promise'
  // its formal UB, but its used in reference implementation,
  // standard wording goes wrong
};

template <typename Promise, memory_resource R>
struct operation_hash<std::coroutine_handle<resourced_promise<Promise, R>>> {
  size_t operator()(std::coroutine_handle<resourced_promise<Promise, R>> h) const {
    return operation_hash<std::coroutine_handle<Promise>>()(
        // assume addresses are same (dirty hack for supporting allocators)
        std::coroutine_handle<Promise>::from_address(h.address()));
  }
};

// 'teaches' promise to return
template <typename T>
struct return_block {
  std::optional<T> storage = std::nullopt;

  constexpr void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    storage.emplace(std::move(value));
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

struct [[nodiscard("co_await it!")]] transfer_control_to {
  std::coroutine_handle<> who_waits;

  bool await_ready() const noexcept {
    assert(who_waits != nullptr);
    return false;
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

template <typename T>
concept executor = requires(T& value) { value.execute([] {}); };

// DEFAULT EXECUTORS

struct noop_executor {
  template <std::invocable F>
  static void execute(F&&) noexcept {
  }
};

struct this_thread_executor {
  template <std::invocable F>
  static void execute(F&& f) noexcept(std::is_nothrow_invocable_v<F&&>) {
    (void)std::forward<F>(f)();
  }
};

struct new_thread_executor {
  template <std::invocable F>
  static void execute(F&& f) {
    std::thread([foo = std::forward<F>(f)]() mutable { (void)std::forward<F>(foo)(); }).detach();
  }
};

template <executor E>
struct KELCORO_CO_AWAIT_REQUIRED jump_on {
  KELCORO_NO_UNIQUE_ADDRESS
  std::conditional_t<std::is_empty_v<E>, E, E&> e;

  static_assert(std::is_same_v<std::decay_t<E>, E>);

#if __cpp_aggregate_paren_init < 201902L
  constexpr jump_on(std::type_identity_t<E> e) noexcept : e(static_cast<E&&>(e)) {
  }
#endif
  static constexpr bool await_ready() noexcept {
    return false;
  }
  constexpr void await_suspend(std::coroutine_handle<> handle) const {
    e.execute(handle);
  }
  static constexpr void await_resume() noexcept {
  }
};
template <typename E>
jump_on(E&&) -> jump_on<std::remove_cvref_t<E>>;

struct KELCORO_CO_AWAIT_REQUIRED get_handle_t {
 private:
  struct return_handle {
    std::coroutine_handle<> handle_;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    KELCORO_ASSUME_NOONE_SEES bool await_suspend(std::coroutine_handle<> handle) noexcept {
      handle_ = handle;
      return false;
    }
    std::coroutine_handle<> await_resume() const noexcept {
      return handle_;
    }
  };

 public:
  return_handle operator co_await() const noexcept {
    return return_handle{};
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

struct KELCORO_CO_AWAIT_REQUIRED op_hash_t {
  operation_hash_t hash;

  static constexpr bool await_ready() noexcept {
    return false;
  }
  template <typename P>
  constexpr bool await_suspend(std::coroutine_handle<P> handle) noexcept {
    calculate_operation_hash(handle);
    return false;
  }
  constexpr operation_hash_t await_resume() noexcept {
    return hash;
  }
};

namespace this_coro {

// provides access to inner handle of coroutine
constexpr inline get_handle_t handle = {};

constexpr inline destroy_coro_t destroy = {};

constexpr inline op_hash_t operation_hash = {};

// co_awaiting on this function suspends coroutine and invokes 'fn' with coroutine handle.
// await suspend returns what 'fn' returns!
constexpr auto suspend_and(auto&& fn) {
  return suspend_and_t(std::forward<decltype(fn)>(fn));
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

template <typename R>
struct elements_of {
  KELCORO_NO_UNIQUE_ADDRESS R rng;

#if __cpp_aggregate_paren_init < 201902L
  // may be clang will never support aggregate () initialization...
  constexpr elements_of(std::type_identity_t<R> rng) noexcept : rng(static_cast<R&&>(rng)) {
  }
#endif
};
template <typename R>
elements_of(R&&) -> elements_of<R&&>;

// tag for yielding from generator/channel by reference.
// This means, if 'value' will be changed by caller it will be
// observable from coroutine
// example:
// int i = 0;
// co_yield ref{i};
// -- at this point 'i' may be != 0, if caller changed it
template <typename Yield>
struct by_ref {
  Yield& value;
};

template <typename Yield>
by_ref(Yield&) -> by_ref<Yield>;

template <yieldable>
struct generator_promise;
template <yieldable>
struct channel_promise;
template <yieldable>
struct generator_iterator;
template <yieldable>
struct channel_iterator;
template <yieldable>
struct generator;
template <yieldable>
struct channel;

}  // namespace dd

namespace dd::noexport {

template <typename Leaf>
struct attach_leaf {
  Leaf leaf;

  bool await_ready() const noexcept {
    return leaf.empty();
  }

  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
      typename Leaf::handle_type owner) const noexcept {
    assert(owner != leaf.top);
    auto& leaf_p = leaf.top.promise();
    auto& root_p = *owner.promise().root;
    leaf_p.current_worker.promise().root = &root_p;
    leaf_p._owner = owner;
    root_p.current_worker = leaf_p.current_worker;
    return leaf_p.current_worker;
  }
  // support yielding generators with different resource
  template <memory_resource R>
  KELCORO_ASSUME_NOONE_SEES auto await_suspend(
      std::coroutine_handle<resourced_promise<typename Leaf::promise_type, R>> handle) {
    return await_suspend(handle.promise().self_handle());
  }
  static constexpr void await_resume() noexcept {
  }
};

template <yieldable Y, typename Generator>
Generator to_generator(auto&& rng) {
  // 'rng' captured by ref because used as part of 'co_yield' expression
  if constexpr (!std::ranges::borrowed_range<decltype(rng)> &&
                std::is_same_v<std::ranges::range_rvalue_reference_t<decltype(rng)>, Y&&>) {
    using std::begin;
    using std::end;
    auto&& b = begin(rng);
    auto&& e = end(rng);
    for (; b != e; ++b)
      co_yield std::ranges::iter_move(b);
  } else {
    for (auto&& x : rng)
      co_yield static_cast<Y&&>(std::forward<decltype(x)>(x));
  }
}
template <typename, yieldable, template <typename> typename>
struct extract_from;

template <typename Rng, yieldable Y>
struct extract_from<Rng, Y, generator> {
  static generator<Y> do_(generator<Y>* g) {
    return std::move(*g);
  }
  template <memory_resource R>
  static generator<Y> do_(resourced<generator<Y>, R>* g) {
    return do_(&g->decay());
  }
  static generator<Y> do_(auto* r) {
    return to_generator<Y, generator<Y>>(static_cast<Rng&&>(*r));
  }
};
template <typename Rng, yieldable Y>
struct extract_from<Rng, Y, channel> {
  static channel<Y> do_(channel<Y>* g) {
    return std::move(*g);
  }
  template <memory_resource R>
  static channel<Y> do_(resourced<channel<Y>, R>* g) {
    return do_(&g->decay());
  }
  template <yieldable OtherY>
  static channel<Y> do_(channel<OtherY>* g) {
    auto& c = *g;
    // note: (void)(co_await) (++b)) only because gcc has bug, its not required
    for (auto b = co_await c.begin(); b != c.end(); (void)(co_await (++b)))
      co_yield static_cast<Y&&>(*b);
  }
  template <yieldable OtherY, memory_resource R>
  static channel<Y> do_(resourced<channel<OtherY>, R>* g) {
    return do_(&g->decay());
  }
  static channel<Y> do_(auto* r) {
    return to_generator<Y, channel<Y>>(static_cast<Rng&&>(*r));
  }
};

template <yieldable Y, template <typename> typename G, typename Rng>
attach_leaf<G<Y>> create_and_attach_leaf(elements_of<Rng>&& e) {
  return attach_leaf<G<Y>>{extract_from<Rng, Y, G>::do_(std::addressof(e.rng))};
}

template <typename Yield>
struct hold_value_until_resume {
  Yield value;

  static constexpr bool await_ready() noexcept {
    return false;
  }
  KELCORO_ASSUME_NOONE_SEES void await_suspend(
      std::coroutine_handle<generator_promise<Yield>> handle) noexcept {
    handle.promise().set_result(std::addressof(value));
  }
  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
      std::coroutine_handle<channel_promise<Yield>> handle) noexcept {
    handle.promise().set_result(std::addressof(value));
    return handle.promise().consumer_handle();
  }
  template <memory_resource R>
  KELCORO_ASSUME_NOONE_SEES void await_suspend(
      std::coroutine_handle<resourced_promise<generator_promise<Yield>, R>> handle) noexcept {
    // decay handle
    return await_suspend(handle.promise().self_handle());
  }
  template <memory_resource R>
  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
      std::coroutine_handle<resourced_promise<channel_promise<Yield>, R>> handle) noexcept {
    // decay handle
    return await_suspend(handle.promise().self_handle());
  }
  static constexpr void await_resume() noexcept {
  }
};

}  // namespace dd::noexport

namespace std {

template <::dd::noexport::coro Coro, ::dd::memory_resource R, typename... Args>
struct coroutine_traits<::dd::resourced<Coro, R>, Args...> {
  using promise_type = ::dd::resourced_promise<typename Coro::promise_type, R>;
};

template <::dd::noexport::coro Coro, typename... Args>
  requires derived_from<Coro, ::dd::enable_resource_deduction>
struct coroutine_traits<Coro, Args...> {
 private:
  template <typename Promise>
  static auto deduct_promise() {
    if constexpr (::dd::noexport::contains_1_resource_tag<Args...>())
      return std::type_identity<::dd::resourced_promise<Promise, ::dd::find_resource_tag_t<Args...>>>{};
    else
      return std::type_identity<Promise>{};
  }

 public:
  // if Args contain exactly ONE(1) 'with_resource<X>' it is used, otherwise Coro::promise_type selected
  using promise_type = typename decltype(deduct_promise<typename Coro::promise_type>())::type;
};

}  // namespace std
