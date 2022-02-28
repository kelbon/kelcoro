
module;
#include <cassert>
#include <coroutine>
export module kel.coro : main;

// export import <coroutine>; MSVC BUG(breaks compilation)

import<thread>;
import<chrono>;
import<system_error>;
import<memory>;
import<future>;

import kel.traits;

export namespace kel {

template <typename>
struct signature;

template <typename R, typename... Types>
struct signature<R(Types...)> {
  using result_type = R;
  using parameter_list = type_list<Types...>;
};

}  // namespace kel

namespace noexport {

#define NEED_CO_AWAIT [[nodiscard("forget co_await?")]]

// for this_coro::invoked_in template
template <typename... Types>
struct find_signature {
 private:
  template <size_t Number, typename First, typename... Others>
  static consteval auto finder(kel::type_list<First, Others...>,
                               std::integral_constant<size_t, Number>) noexcept {
    if constexpr (kel::is_instance_of_v<kel::signature, First>) {
      struct index_and_type_t : std::integral_constant<size_t, Number> {
        using type = First;
      };
      return index_and_type_t{};
    } else {
      return finder(kel::type_list<Others...>{}, std::integral_constant<size_t, Number + 1>{});
    }
  }
  template <size_t N>
  static consteval auto finder(kel::type_list<>, std::integral_constant<size_t, N>) {
    static_assert(kel::always_false<std::integral_constant<size_t, N>>(),
                  "usage example: auto& [x, y] = co_await this_coro::invoked_in(foo, fooarg1, fooarg2"
                  "signature<int(error_code, size_t)>, fooarg3, fooarg4");
  }
  using result_t = decltype(finder(kel::type_list<Types...>{}, std::integral_constant<size_t, 0>{}));

 public:
  using type = typename result_t::type;             // signature itself
  static constexpr size_t value = result_t::value;  // signature's number in type list
};

template <typename... Types>
consteval auto add_universal_ref_for_all(kel::type_list<Types...>) noexcept {
  return kel::type_list<Types&&...>{};
}

struct NEED_CO_AWAIT want_handle_t {};
struct NEED_CO_AWAIT want_promise_t {};
struct NEED_CO_AWAIT want_stop_token_t {};

struct two_way_bound {
 private:
  std::atomic_flag flag;

 public:
  two_way_bound() = default;
  two_way_bound(two_way_bound&&) = delete;
  void operator=(two_way_bound&&) = delete;

  bool try_inform() noexcept {
    bool informed = !flag.test_and_set(std::memory_order::relaxed);
    if (informed)
      flag.notify_one();
    return informed;
  }
  void wait() noexcept {
    flag.wait(false, std::memory_order::relaxed);
  }
};

// awaiters which return handle/promise& into coroutine(co await this_coro::handle/promise)

template <typename Promise>
struct handle_carrier_t {
 private:
  using handle_type = std::coroutine_handle<Promise>;
  handle_type result;

 public:
  constexpr bool await_ready() const noexcept {
    return false;
  }
  constexpr bool await_suspend(handle_type handle) noexcept {
    result = handle;
    return false;
  }
  constexpr handle_type await_resume() const noexcept {
    return result;
  }
};
template <typename Promise>
struct promise_carrier_t {
 private:
  Promise* result_ptr;

 public:
  constexpr bool await_ready() const noexcept {
    return false;
  }
  constexpr bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    result_ptr = &handle.promise();
    return false;
  }
  constexpr Promise& await_resume() const noexcept {
    return *result_ptr;
  }
};

template <typename Promise>
constexpr inline auto promise_carrier = promise_carrier_t<Promise>{};
template <typename Promise>
constexpr inline auto handle_carrier = handle_carrier_t<Promise>{};

// TODO - relocate this into cancellable_promise::final_suspend when MSVC BUG will be fixed
template <typename CancellablePromise>
struct set_done_and_wait_or_destroy {
  two_way_bound& flag;
  bool await_ready() const noexcept {
    return false;
  }
  void await_suspend(std::coroutine_handle<CancellablePromise> handle) const noexcept {
    if (!flag.try_inform())
      handle.destroy();
  }
  void await_resume() const noexcept {
  }
};
// returns how much add to size for size + result == k * align where k natural
template <size_t align>
constexpr size_t padding_length_before_resource(size_t size) noexcept {
  return (align - (size % align)) % align;
}

// TEMPLATE scope_exit

template <std::invocable F>
struct [[nodiscard("Dont forget to name it!")]] scope_exit {
 private:
  F todo;

 public:
  scope_exit(F todo) noexcept(std::is_nothrow_move_constructible_v<F>) : todo(std::move(todo)) {
  }
  scope_exit(scope_exit &&) = delete;
  void operator=(scope_exit&&) = delete;

  ~scope_exit() noexcept(std::is_nothrow_invocable_v<F&>) {
    todo();
  }
};

}  // namespace noexport
using namespace noexport;

export namespace kel {

// imitating compiler behaviour for co_await expression mutation into awaiter(without await_transform)
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

// handle owning
// possible to do unique/shared coroutine handle<Promise> on this base
struct co_handle_deleter {
  void operator()(void* ptr) noexcept {
    std::coroutine_handle<void>::from_address(ptr).destroy();
  }
};

// TAG TYPES

struct input_and_output_iterator_tag : std::input_iterator_tag, std::output_iterator_tag {};

namespace this_coro {

constexpr inline auto handle = want_handle_t{};
constexpr inline auto promise = want_promise_t{};
constexpr inline auto stop_token = want_stop_token_t{};

}  // namespace this_coro

// generator knows how to await_transform it into its value!
template <typename... Types>
struct NEED_CO_AWAIT yield {
  // save arguments from yield, materialize it if rvalue
  std::tuple<std::conditional_t<std::is_rvalue_reference_v<Types&&>, Types, Types&>...> saved_args;
  template <typename... Ts>
  yield(Ts&&... args) : saved_args{std::forward<Ts>(args)...} {
  }
};

template <typename... Types>
yield(Types&&...) -> yield<Types&&...>;

// just triggers await_transform in coroutine promise, usefull for cancellable_coroutines
struct quit_if_requested_t {};
constexpr inline auto quit_if_requested = quit_if_requested_t{};

// TEMPLATE co_awaiter jump_on

template <typename T>
concept executor = requires(T& value) {
  value.execute([] {});
};

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

// for jumping on
constexpr inline auto another_thread = new_thread_executor{};

template <executor T>
struct NEED_CO_AWAIT jump_on {
  [[no_unique_address]] T my_exe;  // can be reference too
  constexpr bool await_ready() const noexcept {
    return false;
  }
  template <typename P>
  constexpr void await_suspend(std::coroutine_handle<P> handle) const {
    my_exe.execute([handle] { handle.resume(); });
  }
  constexpr void await_resume() const noexcept {
  }
};

export template <typename T>
jump_on(T&&) -> jump_on<T&&>;

// basic primitive for symmetric transfer between coroutines
struct NEED_CO_AWAIT transfer_control_to {
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

// TEMPLATE CLASS invoked_in FOR SPECIAL co_awaitable OBJECTS
namespace this_coro {

template <typename F, typename... Args>
struct NEED_CO_AWAIT invoked_in {
 private:
  using cb_signature = std::remove_cvref_t<typename find_signature<Args...>::type>;
  static constexpr size_t signature_nb = find_signature<Args...>::value;
  using cb_args_tuple = insert_type_list_t<std::tuple, typename cb_signature::parameter_list>;
  using cb_result_type = typename cb_signature::result_type;
  using cb_result_storage = typename decltype([] {
    if constexpr (std::is_void_v<cb_result_type>)
      return std::type_identity<nullstruct>{};
    else
      return std::type_identity<std::optional<cb_result_type>>{};
  }())::type;

  using result_args_tuple = std::conditional_t<
      std::is_void_v<cb_result_type>, cb_args_tuple,
      insert_type_list_t<std::tuple, merge_type_lists_t<cb_args_tuple, type_list<cb_result_storage&>>>>;

  F f;
  std::tuple<Args&&...> input_args;
  // input arguments can be no default constructible(for example references), but i need memory for them now
  std::optional<result_args_tuple> output_args;

  struct awaiter_t {
    invoked_in& my_call;

    constexpr bool await_ready() noexcept {
      return false;
    }

    template <typename P>
    constexpr void await_suspend(std::coroutine_handle<P> handle) {
      [ this, handle ]<size_t... Is1, size_t... Is2, typename... CallBackArgs>(
          value_list<size_t, Is1...>, value_list<size_t, Is2...>, type_list<CallBackArgs...>)
          ->decltype(auto) {
        return std::invoke(
            my_call.f,                             // what to call
            std::get<Is1>(my_call.input_args)...,  // input arguments before callback signature
            // callback(builded from signature) to call on executor
            [ this, handle ]<typename... CbArgs>(
                CbArgs && ... cb_args) {  // <typename... CbArgs and CbArgs is workaround (modules bug), was
                                          // CallBackArgs and static_cast<CallBackArgs&&> (where forward now)
              if constexpr (std::is_void_v<cb_result_type>) {
                my_call.output_args.emplace(
                    std::forward<CbArgs>(cb_args)...);  // remembering callback arguments
                handle.resume();                        // resuming coroutine in callback
              } else {
                cb_result_storage result;
                // remembering callback arguments
                my_call.output_args.emplace(std::forward<CbArgs>(cb_args)..., result);
                handle.resume();
                return *result;  // next time coro will be suspended i ll return value to callback's caller
              }
            },
            std::get<Is2>(my_call.input_args)...  // input arguments after callback signature
        );
      }
      (  // INVOKE HERE
          make_value_list<size_t,
                          signature_nb>{},  // value list - helper for arguments before callback signature
          make_value_list<size_t, sizeof...(Args) - signature_nb - 1,
                          signature_nb + 1>{},     // same for arguments after signature
          typename cb_signature::parameter_list{}  // type list of callback arguments
      );
    }

    constexpr result_args_tuple await_resume() noexcept(
        std::is_nothrow_move_constructible_v<result_args_tuple>) {
      return *std::move(my_call.output_args);
    }
  };

 public:
  constexpr invoked_in(F&& f, Args&&... args)
      : f(std::forward<F>(f)), input_args(static_cast<Args&&>(args)...) {
  }

  constexpr auto operator co_await() && noexcept {
    return awaiter_t(*this);
  }
};

// deduction guide for simulating function deduct pattern
export template <typename... Types>
invoked_in(Types&&...) -> invoked_in<Types...>;

}  // namespace this_coro
// TEMPLATE STRUCT base_promise

template <typename T>
struct return_block {
  using result_type = T;

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

// default promise traits
template <typename T>
struct promise_traits {
  using memory_resource_type = std::allocator<std::byte>;
};
// if your type has template arguments + all of them are types + you did not specify promise_traits<YourType>
// then memory resource type is a last template argument
// clang-format off
template <typename T>
requires(requires(T) { typename extract_type_list_t<T>; })
struct promise_traits<T> {
  // clang-format on
 private:
  using list = extract_type_list_t<T>;

 public:
  using memory_resource_type = list::template get<list::size - 1>;
};

// CRTP == Curiously recurring template pattern
template <typename InitialSuspendAwaiter, typename FinalSuspendAwaiter, typename Result, typename CRTP>
struct base_promise : base_promise<InitialSuspendAwaiter, FinalSuspendAwaiter, Result, void> {
 private:
  using MemoryResource = promise_traits<CRTP>::memory_resource_type;
  static_assert(one_of<decltype(std::declval<MemoryResource>().allocate(1)), std::byte*, void*>);

 public:
  using memory_resource_type = MemoryResource;
  using handle_type = std::coroutine_handle<CRTP>;

  [[noreturn]] void unhandled_exception() noexcept {
    // its seams as default for any coroutine, bcs coroutines for async code mostly,
    // async_coro, logical_thread, job and task want to do that and only synchronic generator wants to rethrow
    std::terminate();  // behave as std::j/thread
  }
  auto await_transform(want_handle_t) noexcept {
    return handle_carrier<CRTP>;
  }
  auto await_transform(want_promise_t) noexcept {
    return promise_carrier<CRTP>;
  }
  template <typename T>
  decltype(auto) await_transform(T&& value) {
    return build_awaiter(std::forward<T>(value));
  }
  handle_type get_return_object() {
    return handle_type::from_promise(*static_cast<CRTP*>(this));
  }

  static void* operator new(std::size_t frame_size) requires(
      std::is_default_constructible_v<MemoryResource>) {
    return operator new (frame_size, std::allocator_arg, MemoryResource{});
  }

  // leading allocator convention
  template <typename... Args>
  static void* operator new(std::size_t frame_size, std::allocator_arg_t, MemoryResource resource,
                            Args&&...) {
    if constexpr (std::is_empty_v<MemoryResource> && std::is_default_constructible_v<MemoryResource>) {
      // let MemoryResource always default constructible if empty(stateless),
      // for supporting memory resources and allocators too
      return resource.allocate(frame_size);
    } else {  // just allocate more |frame bytes|padding|memory resource|
      const auto padding_len = padding_length_before_resource<alignof(MemoryResource)>(frame_size);
      void* frame_ptr = resource.allocate(frame_size + padding_len + sizeof(MemoryResource));
      new (reinterpret_cast<std::byte*>(frame_ptr) + frame_size + padding_len)
          MemoryResource(std::move(resource));
      return frame_ptr;
    }
  }

  // trailing allocator convention
  // clang-format off
  template <typename... Args>
  requires(std::is_same_v<MemoryResource&, last_t<Args...>&>)
  static void* operator new(std::size_t frame_size, Args&&... args) {
    return [&frame_size]<typename U, size_t... Is>(std::index_sequence<Is...>, U && tpl) {
      return operator new(frame_size, std::allocator_arg,
                          std::get<sizeof...(Args) - 1>(std::forward<U>(tpl)),  // last arg(memory resource)
                          std::get<Is>(std::forward<U>(tpl))...);               // other arguments
    }
    (std::make_index_sequence<sizeof...(Args) - 1>{},
     std::forward_as_tuple<Args...>(args...));  // INVOKE HERE
  }
  // clang-format on
  static void operator delete(void* ptr, std::size_t frame_size) noexcept {
    if constexpr (std::is_empty_v<MemoryResource> && std::is_default_constructible_v<MemoryResource>) {
      MemoryResource{}.deallocate(reinterpret_cast<std::byte*>(ptr), frame_size);
    } else {  // if move ctor of memory resource throws, then std::terminate called
      const size_t padding_len = padding_length_before_resource<alignof(MemoryResource)>(frame_size);
      auto& resource_on_frame =
          *reinterpret_cast<MemoryResource*>(reinterpret_cast<std::byte*>(ptr) + frame_size + padding_len);
      auto resource = std::move(resource_on_frame);
      resource_on_frame.~MemoryResource();
      resource.deallocate(reinterpret_cast<std::byte*>(ptr),
                          frame_size + padding_len + sizeof(MemoryResource));
    }
  }
};

template <typename InitialSuspendAwaiter, typename FinalSuspendAwaiter, typename Result>
struct base_promise<InitialSuspendAwaiter, FinalSuspendAwaiter, Result, void> : return_block<Result> {
  constexpr InitialSuspendAwaiter initial_suspend() const noexcept {
    return {};
  }
  constexpr FinalSuspendAwaiter final_suspend() const noexcept {
    return {};
  }
};

// TEMPLATE COROUTINE BASE coroutine

// just an unique owner of coroutine handle. Frame always dies with a coroutine object(Note: this is not
// thread safe)
template <typename Promise>
struct coroutine {
 public:
  using promise_type = Promise;
  using handle_type = promise_type::handle_type;

 protected:
  handle_type my_handle;

 public:
  constexpr coroutine() noexcept = default;
  constexpr coroutine(handle_type handle) : my_handle(handle) {
  }

  coroutine(coroutine&& other) noexcept : my_handle(std::exchange(other.my_handle, nullptr)) {
  }
  coroutine& operator=(coroutine&& other) noexcept {
    std::swap(my_handle, other.my_handle);
    return *this;
  }

  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(my_handle, nullptr);
  }

  ~coroutine() {
    if (my_handle) [[likely]]
      my_handle.destroy();
  }
};

// TEMPLATE COROUTINE TYPE cancellable_coroutine

template <typename CRTP>
struct cancellable_promise : base_promise<std::suspend_never, std::suspend_never, void, CRTP> {
 private:
  using base_t = base_promise<std::suspend_never, std::suspend_never, void, CRTP>;
  using handle_type = base_t::handle_type;

  // clang-format off
  template <typename Promise>
  requires(std::derived_from<Promise, cancellable_promise<Promise>>)
  friend struct cancellable_coroutine;
  template<typename,typename,typename,typename>
  friend struct base_promise;
  // clang-format on
  std::stop_token my_stop_token;
  two_way_bound done;

 public:
  auto final_suspend() noexcept {  // TODO - return to this scope after bugfix - workaroung MSVC bug( set done
                                   // and wait was in this scope and not template)
    return set_done_and_wait_or_destroy<CRTP>{done};
  }

  using base_t::await_transform;

  auto await_transform(want_stop_token_t) noexcept {
    struct return_stop_token_t : std::suspend_never {
      std::stop_token token;
      std::stop_token await_resume() noexcept {
        return std::move(token);
      }
    };
    return return_stop_token_t{{}, my_stop_token};
  }

  auto await_transform(quit_if_requested_t) noexcept {
    struct quit_if_requested_impl {
      const bool stop_requested;
      bool await_ready() noexcept {
        return !stop_requested;
      }
      void await_suspend(handle_type handle) noexcept {
        if (!handle.promise().done.try_inform())
          handle.destroy();
      }
      void await_resume() noexcept {
      }
    };
    return quit_if_requested_impl{my_stop_token.stop_requested()};
  }
};

// shared owning of coroutine handle between coroutine object and coroutine frame.
// Frame always dies with a coroutine object, except it was detached(then it deletes itself after co_return)
// clang-format off
template <typename Promise>
requires(std::derived_from<Promise, cancellable_promise<Promise>>)
struct cancellable_coroutine {
  // clang-format on

  using promise_type = Promise;
  using handle_type = std::coroutine_handle<Promise>;

 protected:
  handle_type my_handle;
  std::stop_source my_stop_source{std::nostopstate};

 public:
  // ctor/owning

  cancellable_coroutine() noexcept = default;
  cancellable_coroutine(handle_type handle) noexcept : my_handle(handle), my_stop_source() {
    handle.promise().my_stop_token = my_stop_source.get_token();  // share state with coroutine
  }

  cancellable_coroutine(cancellable_coroutine&& other) noexcept
      : my_handle(std::exchange(other.my_handle, nullptr)), my_stop_source(std::move(other.my_stop_source)) {
  }

  cancellable_coroutine& operator=(cancellable_coroutine&& other) noexcept {
    try_cancel_and_join();
    my_handle = std::exchange(other.my_handle, nullptr);
    my_stop_source = std::move(other.my_stop_source);
    return *this;
  }

  void swap(cancellable_coroutine& other) noexcept {
    std::swap(my_handle, other.my_handle);
    my_stop_source.swap(other.my_stop_source);
  }
  friend void swap(cancellable_coroutine& left, cancellable_coroutine& right) noexcept {
    left.swap(right);
  }

  ~cancellable_coroutine() {
    try_cancel_and_join();
  }

  // thread-like interface

  [[nodiscard]] bool joinable() const noexcept {
    return my_handle != nullptr;
  }
  void join() {
    if (!joinable())
      throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                              "kel::cancellable_coroutine: is not joinable");
    my_handle.promise().done.wait();
    // here im sure im not detached from coro, so its not destroy itself
    my_handle.destroy();
    my_handle = nullptr;
  }
  // returns stop_source for case when user want to stop detached coro from other place
  std::stop_source detach() {
    if (!joinable())
      throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                              "kel::cancellable_coroutine: was already detached/moved out or smth like");
    if (!my_handle.promise().done.try_inform()) [[unlikely]]
      my_handle.destroy();
    my_handle = nullptr;
    return my_stop_source;
  }
  [[nodiscard]] handle_type native_handle() const noexcept {
    return my_handle;
  }

  // cancelling

  bool request_stop() noexcept {
    return my_stop_source.request_stop();
  }
  [[nodiscard]] std::stop_token get_token() const noexcept {
    return my_stop_source.get_token();
  }
  [[nodiscard]] std::stop_source get_stop_source() noexcept {
    return my_stop_source;
  }

 private:
  void try_cancel_and_join() noexcept {
    if (joinable()) {
      request_stop();
      join();
    }
  }
};

// TEMPLATE FUNCTION stop

template <typename T>
concept stopable = requires(T& value) {
  value.request_stop();
  value.join();
};
// effectively stops every cancellable(request stop for all, then join, more effective then just dctor for
// all) only for lvalues
void stop(stopable auto&... args) {
  ((args.request_stop()), ...);
  ((args.join()), ...);
}
template <std::ranges::range T>
requires stopable<std::ranges::range_value_t<T>>
void stop(T& rng) {  // only for lvalue
  for (auto& value : rng)
    value.request_stop();
  for (auto& value : rng)
    value.join();
}

// TEMPLATE COROUTINE TYPE autonomic_coroutine

struct any_empty_autonomic_coroutine {
  template <typename T>
  constexpr operator T() const noexcept {
    static_assert(std::is_empty_v<T>);
    // deriver must shadow get_return_object with returning
    // std::coroutine_handle<Deriver>::from_promise(*this)
    // if handle is needed
    return T{};
  }
};
template <typename CRTP>
struct autonomic_promise : base_promise<std::suspend_never, std::suspend_never, void, CRTP> {
  constexpr any_empty_autonomic_coroutine get_return_object() const noexcept {
    return {};
  }
};

// empty class
template <typename Promise>
struct [[maybe_unused]] autonomic_coroutine {
  using promise_type = Promise;
  using handle_type = std::coroutine_handle<Promise>;
};

// TEMPLATE COROUTINE TYPE generator

template <typename Yield, typename MemoryResource>
struct generator_promise
    : base_promise<std::suspend_always, std::suspend_always, void, generator_promise<Yield, MemoryResource>> {
  using yield_type = Yield;

 private:
  using base_t =
      base_promise<std::suspend_always, std::suspend_always, void, generator_promise<Yield, MemoryResource>>;

  template <typename, typename>
  friend struct generator;

  yield_type* current_result = nullptr;

  struct save_value_before_resume_t {
    yield_type saved_value;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<generator_promise> handle) noexcept {
      handle.promise().current_result = &saved_value;
    }
    void await_resume() const noexcept {
    }
  };

 public:
  // allow yielding

  std::suspend_always yield_value(yield_type& lvalue) noexcept {
    current_result = std::addressof(lvalue);
    return {};
  }
  template <typename U>
  auto yield_value(U&& value) noexcept(std::is_nothrow_constructible_v<yield_type, U&&>) {
    return save_value_before_resume_t{yield_type(std::forward<U>(value))};
  }

  [[noreturn]] void unhandled_exception() {
    throw;
  }

  // yield one lvalue ref, same behavior as co_yield
  auto await_transform(yield<yield_type&>&& saved) noexcept {
    return yield_value(std::get<0>(saved.saved_args));
  }

  using base_t::await_transform;

  template <typename... Types>
  auto await_transform(yield<Types...>&& storage) noexcept(
      noexcept(std::make_from_tuple<yield_type>(storage.saved_args))) {
    return save_value_before_resume_t(std::make_from_tuple<yield_type>(std::move(storage.saved_args)));
  }
  template <typename Executor>
  void await_transform(jump_on<Executor>) = delete;
  void await_transform(std::suspend_always) = delete;
};

// synchronous producer
template <typename Yield, typename MemoryResource = std::allocator<std::byte>>
struct generator : coroutine<generator_promise<Yield, MemoryResource>> {
 private:
  using base_t = coroutine<generator_promise<Yield, MemoryResource>>;

 public:
  using value_type = base_t::promise_type::yield_type;
  using handle_type = base_t::handle_type;

  using base_t::base_t;

  // range based loop support

  struct iterator {
    handle_type owner;

    using iterator_category = input_and_output_iterator_tag;
    using value_type = Yield;
    using difference_type = ptrdiff_t;  // requirement of concept input_iterator

    bool operator==(std::default_sentinel_t) const noexcept {
      return owner.done();
    }
    value_type& operator*() const noexcept {
      return *owner.promise().current_result;
    }
    iterator& operator++() {
      owner.resume();
      return *this;
    }
    // postfix version impossible and logically incorrect for input iterator,
    // but it is required for concept of input iterator
    iterator operator++(int) {
      return ++(*this);
    }
  };

  iterator begin() & {
    assert(!this->my_handle.done());
    this->my_handle.resume();
    return iterator{this->my_handle};
  }
  struct iterator_owner {
    handle_type owner;

    using iterator_category = input_and_output_iterator_tag;
    using value_type = Yield;
    using difference_type = ptrdiff_t;  // requirement of concept input_iterator

    iterator_owner(handle_type handle) noexcept : owner(handle) {
    }
    iterator_owner(iterator_owner&& other) noexcept : owner(std::exchange(other.owner, nullptr)) {
    }
    iterator_owner& operator=(iterator_owner&& other) noexcept {
      std::swap(owner, other.owner);
      return *this;
    }

    bool operator==(std::default_sentinel_t) const noexcept {
      return owner.done();
    }
    value_type& operator*() const noexcept {
      return *owner.promise().current_result;
    }
    iterator_owner& operator++() {
      owner.resume();
      return *this;
    }
    // postfix version impossible and logically incorrect for input iterator,
    // but it is required for concept of input iterator
    iterator_owner& operator++(int) {
      return ++(*this);
    }
    ~iterator_owner() {
      if (owner)
        owner.destroy();
    }
  };
  // for enabling borrowed range, generator object just a view for generator's frame
  iterator_owner begin() && {
    this->my_handle.resume();
    return iterator_owner{this->release()};
  }
  [[nodiscard]] static std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  // no range-for-loop access

  [[nodiscard]] value_type& next() noexcept {
    assert(has_next());
    this->my_handle.promise.resume();
    return *this->my_handle.promise().current_result;
  }
  [[nodiscard]] bool has_next() const noexcept {
    return !this->my_handle.done();
  }

  // for using generator as borrowed range
  // (std::ranges do not forward its arguments, so begin&& will not be called
  // on rvalue generator in Gen() | std::views*something*) (compilation error)
  [[nodiscard]] auto view() && {
    return std::ranges::subrange(std::move(*this).begin(), end());
  }
};

// TEMPLATE COROUTINE TYPE channel
// uses symmetric transfer(used only in another coroutine by co_await) generates values of type T and trasfers
// control to owner coroutine, transfer will only appear after co_yield, which means you can suspend before
// co_yield without value

template <typename Yield, typename MemoryResource>
struct channel_promise
    : base_promise<std::suspend_always, std::suspend_always, void, channel_promise<Yield, MemoryResource>> {
  using yield_type = Yield;

 private:
  using base_t =
      base_promise<std::suspend_always, std::suspend_always, void, channel_promise<Yield, MemoryResource>>;

  template <typename, typename>
  friend struct channel;

  yield_type* current_result = nullptr;
  // always setted(by co_await), may change
  std::coroutine_handle<void> current_owner;

  struct create_value_and_transfer_control_to : transfer_control_to {
    yield_type saved_value;

    std::coroutine_handle<void> await_suspend(std::coroutine_handle<channel_promise> handle) noexcept {
      handle.promise().current_result = &saved_value;
      return who_waits;
    }
  };

 public:
  auto final_suspend() noexcept {
    return transfer_control_to{current_owner};
  }
  // allow yielding

  auto yield_value(yield_type& lvalue) noexcept {
    current_result = std::addressof(lvalue);
    return transfer_control_to{current_owner};
  }

  template <typename T>
  auto yield_value(T&& value) noexcept(std::is_nothrow_constructible_v<yield_type, T&&>) {
    return create_value_and_transfer_control_to{{current_owner}, yield_type{std::forward<T>(value)}};
  }
  using base_t::await_transform;

  template <typename... Types>
  auto await_transform(yield<Types...>&& storage) noexcept(
      noexcept(std::make_from_tuple<yield_type>(storage.saved_args))) {
    return create_value_and_transfer_control_to(
        {current_owner}, std::make_from_tuple<yield_type>(std::move(storage.saved_args)));
  }
};

template <typename Yield, typename MemoryResource = std::allocator<std::byte>>
struct channel : coroutine<channel_promise<Yield, MemoryResource>> {
 private:
  using base_t = coroutine<channel_promise<Yield, MemoryResource>>;

 public:
  using value_type = base_t::promise_type::yield_type;
  using handle_type = base_t::handle_type;

  using base_t::base_t;

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
    return remember_owner_transfer_control_to{this->my_handle};
  }
};

// TODO - in C++26 may be constexpr(new in compile time possible in C++20, then why coroutines cant be
// constexpr?) Makes an consumator - (like generator, but consumes values instead of producing(see
// example-tests)) applies F to every value(which is must be not discarded and must be taken by reference)
template <typename T, std::invocable<T&&> F>
generator<T> make_consumator(F&& f) {
  T input{};  // must be default constructible
  using result_type = std::invoke_result_t<F, T&&>;
  if constexpr (std::is_same_v<result_type, bool>) {
    do {
      co_yield input;
    } while (f(std::move(input)));
  } else if constexpr (std::is_same_v<result_type, void>) {
    while (true) {
      co_yield input;
      f(std::move(input));
    }
  } else {
    static_assert(always_false<T>, "Result type must be void or bool(continue consumption or not)");
  }
}

// TEMPLATE COROUTINE TYPE task

template <typename Result, typename MemoryResource>
struct task_promise
    : base_promise<std::suspend_always, std::suspend_always, Result, task_promise<Result, MemoryResource>> {
  std::coroutine_handle<void> who_waits;

  auto final_suspend() noexcept {
    // who_waits always setted because task not started or co_awaited
    return transfer_control_to{who_waits};
  }
};

// single value generator that returns a value with a co_return. Can be sent on executor(have operator())
template <typename Result, typename MemoryResource = std::allocator<std::byte>>
struct task : coroutine<task_promise<Result, MemoryResource>> {
 private:
  using base_t = coroutine<task_promise<Result, MemoryResource>>;

 public:
  using result_type = Result;

  using base_t::base_t;  // allow ctor from handle

  constexpr auto operator co_await() noexcept {
    struct remember_waiter_and_start_task_t {
      typename base_t::handle_type task_handle;

      bool await_ready() const noexcept {
        assert(task_handle != nullptr && !task_handle.done());
        return false;
      }
      void await_suspend(std::coroutine_handle<void> handle) const noexcept {
        task_handle.promise().who_waits = handle;
        // go to first resume(may be to the end). Resumes who_waits in after final suspend
        task_handle.resume();
      }
      [[nodiscard]] result_type await_resume() {
        if constexpr (!std::is_void_v<result_type>)
          return task_handle.promise().result();
      }
    };
    return remember_waiter_and_start_task_t{this->my_handle};
  }
};

// TEMPLATE COROUTINE TYPE async_task

// for async_task TODO move to noexport(now impossible because of MSVC BUG)
enum class state : uint8_t { not_ready, almost_ready, ready, consumer_dead };

template <typename Result, typename MemoryResource>
struct async_task_promise : base_promise<std::suspend_never, std::suspend_always, Result,
                                         async_task_promise<Result, MemoryResource>> {
 private:
  using base_t = base_promise<std::suspend_never, std::suspend_always, Result,
                              async_task_promise<Result, MemoryResource>>;

 public:
  std::atomic<state> task_state;

  auto final_suspend() noexcept {
    const auto state_before = task_state.exchange(state::almost_ready, std::memory_order::acq_rel);

    struct destroy_if_consumer_dead_t {
      bool is_consumer_dead;
      std::atomic<state>& task_state;

      bool await_ready() const noexcept {
        return is_consumer_dead;
      }
      void await_suspend(std::coroutine_handle<void> handle) const noexcept {
        task_state.exchange(state::ready, std::memory_order::acq_rel);
        task_state.notify_one();
      }
      void await_resume() const noexcept {
      }
    };
    return destroy_if_consumer_dead_t{state_before == state::consumer_dead, task_state};
  }
};

template <typename Result, typename MemoryResource = std::allocator<std::byte>>
struct async_task {
 public:
  using promise_type = async_task_promise<Result, MemoryResource>;
  using handle_type = promise_type::handle_type;

 protected:
  handle_type my_handle;

 public:
  constexpr async_task() noexcept = default;
  constexpr async_task(handle_type handle) : my_handle(handle) {
  }

  async_task(async_task&& other) noexcept : my_handle(std::exchange(other.my_handle, nullptr)) {
  }
  void operator=(async_task&&) = delete;

  // postcondition - coro stops with value or exception
  void wait() const noexcept {
    if (my_handle) {
      auto& cur_state = my_handle.promise().task_state;
      state now = cur_state.load(std::memory_order::acquire);
      while (now != state::ready) {
        cur_state.wait(now, std::memory_order::acquire);
        now = cur_state.load(std::memory_order::acquire);
      }
    }
  }
  // postcondition - my_handle == nullptr
  Result get() requires(!std::is_void_v<Result>) {
    if (my_handle == nullptr)
      throw std::future_error(std::future_errc::no_state);
    wait();
    scope_exit clear([&] { my_handle = nullptr; });

    auto result = *std::move(my_handle.promise().storage);
    // result always exist, its setted or std::terminate called on exception.
    my_handle.destroy();
    return result;
  }

  ~async_task() {
    if (my_handle == nullptr)
      return;
    const auto state_before =
        my_handle.promise().task_state.exchange(state::consumer_dead, std::memory_order::acq_rel);
    switch (state_before) {
      case state::almost_ready:
        my_handle.promise().task_state.wait(state::almost_ready, std::memory_order::acquire);
        [[fallthrough]];
      case state::ready:
        my_handle.destroy();
        break;
    }
    // else frame destroys itself because consumer is dead
  }
};

// behaviour similar to std::jthread
template <typename MemoryResource>
struct logical_thread_promise : cancellable_promise<logical_thread_promise<MemoryResource>> {};

template <typename MemoryResource>  // mm - memory management
using logical_thread_mm = cancellable_coroutine<logical_thread_promise<MemoryResource>>;
using logical_thread = logical_thread_mm<std::allocator<std::byte>>;

// behaviour of this type coroutine is like a detached logical_thread, but it is a lightweight in comparasion
// with logical_thread you can get handle from .release() method, but you have no any guarantee about frame
// lifetime
template <typename MemoryResource>
struct job_promise : autonomic_promise<job_promise<MemoryResource>> {};

template <typename MemoryResource>
using job_mm = autonomic_coroutine<job_promise<MemoryResource>>;
using job = job_mm<std::allocator<std::byte>>;

}  // namespace kel