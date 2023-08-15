#pragma once

#include <cstring>  // memcpy
#include <utility>
#include <type_traits>
#include <cstddef>
#include <optional>
#include <coroutine>
#include <cassert>
#include <thread>
#include <memory_resource>
#include <concepts>

#define KELCORO_CO_AWAIT_REQUIRED [[nodiscard("forget co_await?")]]

#if defined(__GNUC__) || defined(__clang__)
#define KELCORO_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define KELCORO_UNREACHABLE __assume(false)
#else
#define KELCORO_UNREACHABLE (void)0
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
concept memory_resource = std::is_void_v<T> ||
                          (!std::is_reference_v<T>) &&
                              requires(T& value, size_t sz, size_t align, void* ptr) {
                                { value.allocate(sz, align) } -> std::convertible_to<void*>;
                                { value.deallocate(ptr, sz, align) } noexcept -> std::same_as<void>;
                                requires std::is_nothrow_move_constructible_v<T>;
                                requires alignof(T) <= alignof(std::max_align_t);
                                requires !(std::is_empty_v<T> && !std::default_initializable<T>);
                              };

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
  polymorphic_resource(std::pmr::memory_resource& m) noexcept
      : passed(std::exchange(passed_resource(), nullptr)) {
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
  [[no_unique_address]] R resource;
};
template <typename X>
with_resource(X&&) -> with_resource<std::remove_cvref_t<X>>;

template <typename>
struct is_resource_tag : std::false_type {};

template <memory_resource R>
struct is_resource_tag<with_resource<R>> : std::true_type {};

template <typename... Types>
consteval bool contains_1_resource_tag() {
  return (0 + ... + is_resource_tag<std::remove_cvref_t<Types>>::value) == 1;
}
constexpr auto only_for_resource(auto&& foo, auto&&... args) {
  auto try_one = [&](auto& x) {
    if constexpr (is_resource_tag<std::remove_cvref_t<decltype(x)>>::value)
      foo(x.resource);
  };
  (try_one(args), ...);
}

template <typename... Args>
struct find_resource_tag : std::type_identity<void> {};
template <typename R, typename... Args>
struct find_resource_tag<with_resource<R>, Args...> : std::type_identity<R> {};
template <typename Head, typename... Tail>
struct find_resource_tag<Head, Tail...> : find_resource_tag<Tail...> {};

// inheritor(coroutine promise) may be allocated with 'R'
// using 'with_resource' tag or default constructed 'R'
// TODO tests
template <memory_resource R>
struct enable_memory_resource_support {
  template <size_t RequiredPadding>
  static size_t padding_len(size_t sz) noexcept {
    enum { P = RequiredPadding };
    static_assert(P != 0);
    return ((P - sz) % P) % P;
  }

 private:
  static_assert(padding_len<16>(16) == 0);
  static_assert(padding_len<16>(0) == 0);
  static_assert(padding_len<16>(1) == 15);
  static_assert(padding_len<16>(8) == 8);

  enum { frame_align = std::max(coroframe_align(), alignof(R)) };
  static void* do_allocate(size_t frame_sz, memory_resource auto& r) {
    if constexpr (std::is_empty_v<R>)
      return (void*)r->allocate(frame_sz, coroframe_align());
    else {
      const size_t padding = padding_len<frame_align>(frame_sz);
      const size_t bytes_count = frame_sz + padding + sizeof(R);
      std::byte* p = (std::byte*)r->allocate(bytes_count, frame_align);
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
    requires(contains_1_resource_tag<Args...>())
  static void* operator new(std::size_t frame_sz, Args&&... args) {
    void* p;
    only_for_resource([&](auto& resource) { p = do_allocate(frame_sz, resource); }, args...);
    return p;
  }
  static void operator delete(void* ptr, std::size_t frame_sz) noexcept {
    if constexpr (std::is_empty_v<R>) {
      R r{};
      r.deallocate(ptr, frame_sz, coroframe_align());
    } else {
      const size_t padding = padding_len<frame_align>(frame_sz);
      const size_t bytes_count = frame_sz + padding + sizeof(R);
      R* onframe_resource = (R*)((std::byte*)ptr + frame_sz + padding);
      R r = std::move(*onframe_resource);
      std::destroy_at(onframe_resource);
      r->deallocate(ptr, bytes_count, frame_align);
    }
  }
};
// TODO macro which enables support for memory resources...
// да, можно просто добавить тег по которому отсеивать специализации
// TODO and macro specialization of coroutine traits
// и видимо придётся это всё таки в шаблон генератора/всех остальных корутин пихать... И алиасы на дефолт

// 'teaches' promise to return
template <typename T>
struct return_block {
  std::optional<T> storage = std::nullopt;

  constexpr void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    storage.emplace(std::move(value));
  }
  constexpr T result() noexcept(noexcept(*std::move(storage))) {
    return *std::move(storage);
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

template <executor E>
struct KELCORO_CO_AWAIT_REQUIRED jump_on {
  [[no_unique_address]] E e;
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
jump_on(E&&) -> jump_on<E>;

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
  [[no_unique_address]] R rng;

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
template <yieldable, memory_resource = void>
struct generator;
template <yieldable, memory_resource = void>
struct channel;
template <typename>
struct elements_of;

}  // namespace dd

namespace dd::noexport {

template <typename Leaf>
struct attach_leaf {
  Leaf leaf;

  bool await_ready() const noexcept {
    return leaf.empty();
  }

  std::coroutine_handle<> await_suspend(typename Leaf::handle_type owner) const noexcept {
    assert(owner != leaf.top);
    auto& leaf_p = leaf.top.promise();
    auto& root_p = *owner.promise().root;
    leaf_p.current_worker.promise().root = &root_p;
    leaf_p._owner = owner;
    root_p.current_worker = leaf_p.current_worker;
    return leaf_p.current_worker;
  }
  static constexpr void await_resume() noexcept {
  }
};

// accepts elements_of<X> and converts it into leaf-coroutine
// TODO упростить ( с приведением к общему виду, чтобы все генераторы кастовались к void версии, игнорируя
// ресурсы)
template <yieldable Yield, template <typename, typename = void> typename Generator>
struct elements_extractor {
 private:
  static_assert(!std::is_reference_v<Yield> && !std::is_const_v<Yield>);
  // leaf type == owner type

  static Generator<Yield> do_extract(Generator<Yield>&& g) noexcept {
    return std::move(g);
  }
  static Generator<Yield> do_extract(Generator<Yield>& g) noexcept {
    return do_extract(std::move(g));
  }

  // leaf yields other type

  template <typename U>
  static generator<Yield> do_extract(generator<U>& g) {
    for (U&& x : g)
      co_yield static_cast<Yield&&>(std::move(x));
  }
  template <typename U>
  static generator<Yield> do_extract(generator<U>&& g) {
    return do_extract(g);
  }

  template <typename U>
  static channel<Yield> do_extract(channel<U>& c) {
    // note: (void)(co_await) (++b)) only because gcc has bug, its not required
    for (auto b = co_await c.begin(); b != c.end(); (void)(co_await (++b)))
      co_yield static_cast<Yield&&>(*b);
  }
  template <typename U>
  static channel<Yield> do_extract(channel<U>&& c) {
    return do_extract(c);
  }

  // leaf is just a range

  static Generator<Yield> do_extract(auto&& rng) {
    if constexpr (!std::ranges::borrowed_range<decltype(rng)> &&
                  std::is_same_v<std::ranges::range_rvalue_reference_t<decltype(rng)>, Yield&&>) {
      using std::begin;
      using std::end;
      auto&& b = begin(rng);
      auto&& e = end(rng);
      for (; b != e; ++b)
        co_yield std::ranges::iter_move(b);
    } else {
      for (auto&& x : rng)
        co_yield static_cast<Yield&&>(std::forward<decltype(x)>(x));
    }
  }

 public:
  template <typename X>
  static attach_leaf<Generator<Yield>> extract(elements_of<X>&& e) {
    // captures range by reference, because its all in yield expression in the coroutine
    return attach_leaf<Generator<Yield>>{do_extract(static_cast<X&&>(e.rng))};
  }
};

template <typename Yield>
struct hold_value_until_resume {
  Yield value;

  static constexpr bool await_ready() noexcept {
    return false;
  }
  void await_suspend(std::coroutine_handle<generator_promise<Yield>> handle) noexcept {
    handle.promise().set_result(std::addressof(value));
  }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<channel_promise<Yield>> handle) noexcept {
    handle.promise().set_result(std::addressof(value));
    return handle.promise().consumer_handle();
  }
  static constexpr void await_resume() noexcept {
  }
};

}  // namespace dd::noexport
