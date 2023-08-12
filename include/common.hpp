#pragma once

#include <type_traits>
#include <cstddef>
#include <optional>
#include <coroutine>
#include <cassert>
#include <thread>
#include <memory_resource>
// TODO перенести всякую муть в utility связанную с тредами, опшнлами и тд
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

namespace noexport {

// caches value (just because its C-non-inline-call...)
static std::pmr::memory_resource* const new_delete_resource = std::pmr::new_delete_resource();

// used to pass argument to promise operator new, say 'ty' to standard writers,
// i want separate coroutine logic from how it allocates frame
inline thread_local std::pmr::memory_resource* co_memory_resource = new_delete_resource;

}  // namespace noexport

// TODO only for next allocation + default resource
// passes 'implicit' argument to all coroutines allocatinon on this thread until object dies
// usage:
//    foo_t local = dd::with_resource{r}, create_coro(), foo();
// OR
//    dd::with_resource{r}, [&] { ... }();
// OR
//    dd::with_resource name{r};
//    ... code with coroutines ...
struct [[nodiscard]] with_resource : not_movable {
 private:
  std::pmr::memory_resource* old;

 public:
  explicit with_resource(std::pmr::memory_resource& m) noexcept
      : old(std::exchange(noexport::co_memory_resource, &m)) {
    KELCORO_ASSUME(old != nullptr);
  }

  ~with_resource() {
    noexport::co_memory_resource = old;
  }
};

// Question: what will be if coroutine local contains alignas(64) int i; ?
// Answer: (quote from std::generator paper)
//  "Let BAlloc be allocator_traits<A>::template rebind_alloc<U> where U denotes an unspecified type
// whose size and alignment are both _STDCPP_DEFAULT_NEW_ALIGNMENT__"
consteval size_t coroframe_align() {
  return __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

// when used as first argumemt in coroutine it will be allocated by this resource
// and resource will be stored until coroutine frame destroyed
template <typename R>
struct inplace_resource : not_movable {
  [[no_unique_address]] R r;
};
template <typename R>
inplace_resource(R&&) -> inplace_resource<std::remove_reference_t<R>>;

// TODO
// TODO inplace_resource<R> конструируемый в аргументах корутины и не используемый в ней явно, только для
// аллокации корутина должна иметь к нему доступ (через паблик интерфейс его)
// // basically free list with customizable max blocks
// // must be good because coroutines have same sizes,
// // its easy to reuse memory for them
// struct co_memory_resource { ... };
// inheritor(coroutine promise) recieves allocation(std::pmr::memory_resource) support
// usage: see with_resource(R)
struct enable_memory_resource_support {
  static void* operator new(std::size_t frame_size) {
    auto* r = noexport::co_memory_resource;
    auto* p = (std::byte*)r->allocate(frame_size + sizeof(void*), coroframe_align());
    std::byte* alloc_ptr = p + frame_size;
    std::memcpy(alloc_ptr, &r, sizeof(void*));
    return p;
  }
  // TODO with inplace resource static void operator new
  static void operator delete(void* ptr, std::size_t frame_size) noexcept {
    auto* alloc_ptr = (std::byte*)ptr + frame_size;
    std::pmr::memory_resource* m;
    std::memcpy(&m, alloc_ptr, sizeof(void*));
    m->deallocate(ptr, frame_size + sizeof(void*), coroframe_align());
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

struct always_done_coroutine_promise : not_movable {
  static void* operator new(std::size_t frame_size) {
    // worst part - i have  no guarantees about frame size, even when compiler exactly knows
    // how much it will allocoate (if he will)
    alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) static char bytes[50];
    if (frame_size <= 50)
      return bytes;
    // this memory can not be deallocated in dctor of global object,
    // because i have no guarantee, that this memory even will be allocated.
    // so its memory leak. Ok
    return new char[frame_size];
  }

  static void operator delete(void*, std::size_t) noexcept {
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

static inline const always_done_coroutine_handle always_done_coro{
    []() -> always_done_coroutine_handle { co_return; }()};

}  // namespace dd::noexport

namespace dd {

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

// TODO name handle union
// never nullptr, stores always_done_coroutine_handle or std::coroutine_handle<Promise>
// TODO attributes like pure/const/flatten/cold/hot etc
// реально, если забить на мсвц, то можно писать аттрибуты просто через gnu::x и их поймёт кланг тоже...
// TODO убрать макросы тогда нахрен
// TODO проверить комбинацию cold + flatten на unhandled_exception!
template <typename Promise>
struct coroutine_handle {
 private:
  // invariant _h != nullptr
  std::coroutine_handle<> _h = always_done_coroutine();

 public:
  coroutine_handle() = default;
  coroutine_handle(always_done_coroutine_handle h) noexcept : _h(h) {
    assert(_h != nullptr);
  }
  coroutine_handle(std::coroutine_handle<Promise> h) noexcept : _h(h) {
    assert(_h != nullptr);
  }
  coroutine_handle(coroutine_handle&) = default;
  coroutine_handle(coroutine_handle&&) = default;
  coroutine_handle& operator=(const coroutine_handle&) = default;
  coroutine_handle& operator=(coroutine_handle&&) = default;

  std::coroutine_handle<Promise> get() const noexcept {
    assert(_h != nullptr);
    return std::coroutine_handle<Promise>::from_address(_h.address());
  }
  // precondition: not always_done_coroutine stored
  Promise& promise() const noexcept {
    return std::coroutine_handle<Promise>::from_address(_h.address()).promise();
  }
  static coroutine_handle from_promise(Promise& p) {
    coroutine_handle h;
    h._h = std::coroutine_handle<Promise>::from_promise(p);
    return h;
  }
  // postcondition returned != nullptr
  constexpr void* address() const noexcept {
    void* p = _h.address();
    KELCORO_ASSUME(p != nullptr);
    return p;
  }
  static constexpr coroutine_handle from_address(void* addr) {
    coroutine_handle h;
    h._h = std::coroutine_handle<Promise>::from_address(addr);
    return h;
  }

  bool done() const noexcept {
    return _h.done();
  }
  void resume() const {
    assert(!done());
    _h.resume();
  }
  // can be safely used more then 1 time, noop if no coro attached
  void destroy() {
    _h.destroy();
  }
  operator std::coroutine_handle<>() const noexcept {
    return _h;
  }
};

// TODO info about current channel/generator call, is top, handles, iteration from top to bot, !SWAP!
// операция свапающая два хендла внутри одной цепочки...

// насчет мемори ресурса. Хм.. Может быть на 1 аллокацию сделать ровно, если установлено - берёт его
// если не установлено - берёт дефолтное(и set default всякое такое сверху)
// TODO? можно ещё сделать отдельный "канал" для отправки и получения чего-то, что не Yield типа...
// то есть я на принимающей стороне .send(X), на стороне канала делаю auto x = co_yield y;

}  // namespace dd

// TODO into one header with generator / channel
// TODO specializations like ranges::borrowed_range (TODO hmm)
namespace dd {

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

// accepts addressof(elements_of<X>.rng) and converts it into leaf-coroutine
template <typename Yield, template <typename> typename Generator>
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
    for (auto b = co_await c.begin(); b != c.end(); co_await ++c)
      co_yield static_cast<Yield>(*b);
  }
  template <typename U>
  static channel<Yield> do_extract(channel<U>&& c) {
    return do_extract(c);
  }

  // leaf is just a range

  static Generator<Yield> do_extract(auto&& rng) {
    if constexpr (!std::ranges::borrowed_range<decltype(rng)> &&
                  std::is_same_v<std::ranges::range_rvalue_reference_t<decltype(rng)>, Yield&&>) {
      auto&& b = std::ranges::begin(rng);
      auto&& e = std::ranges::end(rng);
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
