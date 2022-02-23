
module;
#include <cassert>
export module kel.coro;

import<atomic>;
import<coroutine>;
import<variant>;
import<optional>;

import kel.traits;

export import : main;

export namespace kel {
#define NEED_CO_AWAIT [[nodiscard("forget co_await?")]]

// customization point object, specialize it if you want to transfer arguments into event

template <typename>
struct event_traits {
  // what event excepts from sender and returns from co_await to coro
  using input_type = nullstruct;
};

template <typename T>
using event_input_t = event_traits<T>::input_type;

// PRECONDITIONS :
// if coroutine subscribes(by co_await event<Name>), then
// coroutine will be suspended until event_t resume it(possibly througth event pool)
// coroutine guaratees that its not .done()
// coroutine guarantes that its correct delete itself(or some owner delete it) if exception throws while
// handle.resume()
// executor's method .execute() MUST NEVER throw exception after resuming executed task(if its not task's
// exception)(or checking .done is UB)
// STRONG EXCEPTION GUARANTEE(except for notify all, coroutines which can be called are called)
template <typename NamedTag>
struct event_t {
 public:
  using input_type = event_input_t<NamedTag>;

  static_assert(std::is_nothrow_move_constructible_v<input_type>);

 private:
  template <executor, typename>
  friend struct event_pool;

  // subscribes coroutine on event,
  // make it part of event-based stack of awaiters
  // accepts and returns required arguments for event
  struct awaiter_t {
    event_t* my_event;
    std::coroutine_handle<void> handle;
    awaiter_t* next = nullptr;  // im a part of awaiters stack!
    [[no_unqiue_address]] input_type input;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle_) noexcept {
      handle = handle_;
      my_event->subscribers.push(this);
    }
    [[nodiscard]] auto await_resume() noexcept {
      if constexpr (is_nullstruct_v<input_type>)
        return;
      else  // must be setted by informer of event by event<Name>.notify_one/all(Input...)
        return input;
    }
  };

  // non-owning lockfree manager for distributed awaiters system.
  // awaiters always alive, unique, one-thread interact with manager
  // (but manager interact with any count of threads)

  struct stack_manager {
   private:
    using enum std::memory_order;

    std::atomic<awaiter_t*> top{nullptr};

   public:
    // may be may be using multithread_category = lock_free_tag;

    // надо возвращать указатель на ноду...
    void push(awaiter_t* value_ptr) noexcept {
      // any other push works with another value_ptr
      assert(value_ptr != nullptr);
      value_ptr->next = top.load(relaxed);

      // after this loop this->top == value_ptr, value_ptr->next == previous value of this->top
      while (!top.compare_exchange_weak(value_ptr->next, value_ptr, acq_rel, acquire)) {
      }
    }

    // returns top of the stack
    [[nodiscard]] awaiter_t* pop_all() {
      return top.exchange(nullptr, acq_rel);
    }

    // not need to release any resources, its not mine!
    // if any, then coroutine will be not resumed, its memory leak
    ~stack_manager() {
      assert(top.load(relaxed) == nullptr);
    }
  };

  stack_manager subscribers{};

 public:
  event_t() noexcept = default;
  event_t(event_t&&) = delete;
  void operator=(event_t&&) = delete;

  // event source functional

  // clang-format off
  template<executor Executor>
  requires(is_nullstruct_v<input_type>)
  void notify_all(Executor&& exe) {
    // clang-format on
    // must be carefull - awaiter ptrs are or their coroutine(which we want to execute / destroy)
    awaiter_t* top = subscribers.pop_all();
    awaiter_t* next;
    while (top != nullptr) {
      next = top->next;
      std::coroutine_handle<void> handle = top->handle;
      try {
        std::forward<Executor>(exe).execute(handle);
      } catch (...) {
        if (!handle.done())  // throws before handle.resume()
          subscribers.push(top);
        top = next;
        while (top != nullptr) {
          subscribers.push(top);
          top = top->next;
        }
        throw;
      }
      top = next;
    }
  }

  // copies input for all recievers(all coros returns to waiting if copy constructor throws)
  // clang-format off
  template <executor Executor>
  requires(std::is_copy_constructible_v<input_type> && !is_nullstruct_v<input_type>)
  void notify_all(Executor&& exe, input_type input) {
    // clang-format on
    awaiter_t* top = subscribers.pop_all();
    awaiter_t* next;
    while (top != nullptr) {
      next = top->next;  // copy from awaiter, which on frame and will die
      try {
        top->input = input;  // not in copy, input must be on coroutine before await_resume
        std::atomic_thread_fence(std::memory_order::release);
      } catch (...) {
        while (top != nullptr) {
          subscribers.push(top);
          top = top->next;
        }
        throw;
      }
      std::coroutine_handle<void> handle = top->handle;
      try {
        std::forward<Executor>(exe).execute(handle);
      } catch (...) {
        if (!handle.done())  // throw was not while resuming
          subscribers.push(top);
        top = next;
        while (top != nullptr) {
          subscribers.push(top);
          top = top->next;
        }
        throw;
      }
      top = next;
    }
  }

  // subscribe for not coroutines

  template <typename Alloc = std::allocator<std::byte>, typename F>
  void set_callback(F f, Alloc alloc = Alloc{}) {
    [](event_t& event_, F f_, Alloc) -> job_mm<Alloc> {
      if constexpr (is_nullstruct_v<input_type>) {
        co_await event_;
        f_();
      } else {
        auto&& input = co_await event_;
        f_(std::forward<decltype(input)>(input));
      }
    }(*this, std::move(f), std::move(alloc));
  }

  // subscribe, but only for coroutines

  [[nodiscard]] auto operator co_await() noexcept {
    return awaiter_t{.my_event = this};
  }
};

// default interaction point between recipients and sources of events

template <typename Name>
inline constinit event_t<Name> event{};

struct default_selector {
  template <typename Event>
  auto& operator()(std::type_identity<Event>) const noexcept {
    return ::kel::event<Event>;
  }
};

template <typename... EventTags, typename Selector = default_selector>
NEED_CO_AWAIT constexpr auto when_all(Selector selector = {}) {
  static_assert(sizeof...(EventTags) < 256);
  static_assert((noexcept(selector(std::type_identity<EventTags>{})) && ...));
  static_assert((std::is_lvalue_reference_v<decltype(selector(std::type_identity<EventTags>{}))> && ...));
  // compilation error if no unique types in pack
  struct _ : std::type_identity<EventTags>... {};

  // may be empty if empty input types, but not on MSVC(EBO bug)
  using possible_storage_t = std::tuple<event_input_t<EventTags>...>;
  using storage_t = std::conditional_t<std::is_default_constructible_v<possible_storage_t>,
                                       possible_storage_t, std::optional<possible_storage_t>>;
  struct subscribe_all_last_resumes_main_t {
    uint8_t count;
    [[no_unique_address]] Selector my_selector;
    [[no_unique_address]] storage_t input;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle) {
      const auto create_cb_for = [&]<typename E, size_t EventNumber>(
                                     std::type_identity<E>, std::integral_constant<size_t, EventNumber>) {
        if constexpr (is_nullstruct_v<event_input_t<E>>) {
          return [count = std::atomic_ref(this->count), handle] {
            if (count.fetch_add(1, std::memory_order::relaxed) == sizeof...(EventTags) - 1)
              handle.resume();
          };
        } else {
          return [this, handle](event_input_t<E> input_) {
            std::get<EventNumber>(this->input) = std::move(input_);
            auto count_ = std::atomic_ref(this->count);
            if (count_.fetch_add(1, std::memory_order::relaxed) == sizeof...(EventTags) - 1)
              handle.resume();
          };
        }
      };
      [&]<size_t... Is>(std::index_sequence<Is...>) {
        ((my_selector(std::type_identity<EventTags>{})
              .set_callback(
                  create_cb_for(std::type_identity<EventTags>{}, std::integral_constant<size_t, Is>{}))),
         ...);
      }  // INVOKED HERE
      (std::index_sequence_for<EventTags...>{});
    }

    [[nodiscard]] auto await_resume() noexcept(std::is_nothrow_move_constructible_v<decltype(input)>) {
      if constexpr ((is_nullstruct_v<event_input_t<EventTags>> + ...) != 0) {
        if constexpr (std::is_same_v<possible_storage_t, decltype(input)>)
          return std::move(input);  // not optional
        else
          return *std::move(input);
      }
      // else void result
    }
  };
  return subscribe_all_last_resumes_main_t{0, std::move(selector)};
}

template <typename... EventTags, typename Selector = default_selector>
NEED_CO_AWAIT constexpr auto when_any(Selector selector = {}) {
  static_assert(sizeof...(EventTags) < 256 && sizeof...(EventTags) >= 2);
  static_assert((noexcept(selector(std::type_identity<EventTags>{})) && ...));
  static_assert((std::is_lvalue_reference_v<decltype(selector(std::type_identity<EventTags>{}))> && ...));
  // compilation error if no unique types in pack
  struct _ : std::type_identity<EventTags>... {};

  using std::variant;  // MSVC workaround! omg(

  struct subscribe_all_first_resumes_main_t {
    [[no_unique_address]] Selector my_selector;
    std::variant<std::monostate, event_input_t<EventTags>...> input;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle) {
      // need to allocate, because after coro resuming awaiter will die(with flag in it)
      auto count_ptr = std::make_shared<std::atomic<uint8_t>>(0);
      const auto create_cb_for = [&]<typename E, size_t EventNumber>(
                                     std::type_identity<E>, std::integral_constant<size_t, EventNumber>) {
        if constexpr (is_nullstruct_v<event_input_t<E>>) {
          return [this, count_ptr, handle]() {
            auto value = count_ptr->fetch_add(1, std::memory_order::relaxed);
            if (value == 0) {  // + 1 because of monostate in variant
              this->input.emplace<EventNumber + 1>(nullstruct{});
              handle.resume();
            }
          };
        } else {
          return [this, count_ptr, handle](event_input_t<E> event_input) {
            auto value = count_ptr->fetch_add(1, std::memory_order::relaxed);
            if (value == 0) {  // + 1 because of monostate in variant
              this->input.emplace<EventNumber + 1>(std::move(event_input));
              // move ctor for input type always noexcept(static assert in event_t)
              handle.resume();
            }
          };
        }
      };
      [&]<size_t... Is>(std::index_sequence<Is...>) {
        ((my_selector(std::type_identity<EventTags>{})
              .set_callback(
                  create_cb_for(std::type_identity<EventTags>{}, std::integral_constant<size_t, Is>{}))),
         ...);
      }  // INVOKED HERE
      (std::index_sequence_for<EventTags...>{});
    }
    // always returns variant with information what happens(index + 1 bcs of std::monostate) and input type(if
    // exist)
    [[nodiscard]] auto await_resume() noexcept(std::is_nothrow_move_constructible_v<decltype(input)>) {
      return std::move(input);
    }
  };
  return subscribe_all_first_resumes_main_t{std::move(selector)};
}

// Executor may be a lvalue reference too
// Selector used to select event by selector(std::type_identity<Event>{})->event_t&
// it can be used to event dispatching, logging, categorization ( requires ) etc
// PRECONDITIONS : associated event must live longer then event pool
template <executor Executor, typename Selector = default_selector>
struct event_pool {
 private:
  [[no_unique_address]] Executor my_exe{};
  [[no_unique_address]] Selector my_selector{};

 public:
  using executor_type = Executor;

  // constructing

  event_pool() noexcept(
      std::is_nothrow_default_constructible_v<Executor>&& std::is_nothrow_default_constructible_v<Selector>) {
  }

  explicit event_pool(Executor exe, Selector selector = {}) noexcept(
      std::is_nothrow_move_constructible_v<Executor>&& std::is_nothrow_move_constructible_v<Selector>)
      : my_exe(std::move(exe)), my_selector(std::move(selector)) {
  }

  // state observing

  // clang-format off
  Executor get_executor()
      noexcept(std::is_nothrow_copy_constructible_v<Executor>)
      requires(std::is_copy_constructible_v<Executor>)
  {
    return my_exe;
  }

  // main functional

  template <typename Event>
  requires(is_nullstruct_v<event_input_t<Event>>)
  void notify_all() {
      get_receiver<Event>().notify_all(my_exe);
  }
  template <typename Event>
  requires(!is_nullstruct_v<event_input_t<Event>>)
  void notify_all(event_input_t<Event> input) {
      get_receiver<Event>().notify_all(my_exe, std::move(input));
  }
  // clang-format on
 private:
  template <typename Event>
  auto& get_receiver() noexcept(noexcept(my_selector(std::type_identity<Event>{}))) {
    static_assert(std::is_lvalue_reference_v<decltype(my_selector(std::type_identity<Event>{}))>);
    return my_selector(std::type_identity<Event>{});
  }
};

}  // namespace kel