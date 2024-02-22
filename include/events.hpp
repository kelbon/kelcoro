#pragma once

#include <variant>

#include "job.hpp"
#include "nonowner_lockfree_stack.hpp"

namespace dd {

struct nullstruct {};

// PRECONDITIONS :
// if coroutine subscribes(by co_await event<Name>), then:
// coroutine will be suspended until event resumes it
// coroutine guaratees that its not .done()
// executor's method .execute() MUST NEVER throw exception after resuming executed task(if its not task's
// exception)(or checking .done is UB)
// Input must be default constructible, copy constructible and noexcept movable
// TODO with mutex and every event handled
template <typename Input>
struct event {
  using input_type = std::conditional_t<std::is_void_v<Input>, nullstruct, Input>;

  static_assert(std::is_nothrow_move_constructible_v<input_type> &&
                std::is_copy_constructible_v<input_type> && std::is_default_constructible_v<input_type>);

 private:
  // subscribes coroutine on event,
  // make it part of event-based stack of awaiters
  // accepts and returns required arguments for event
  struct awaiter_t {
    event* event_;
    std::coroutine_handle<void> handle;
    awaiter_t* next = nullptr;  // im a part of awaiters stack!
    KELCORO_NO_UNIQUE_ADDRESS input_type input;

    static bool await_ready() noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle_) noexcept {
      handle = handle_;
      event_->subscribers.push(this);
    }
    // resumed only after initializing input
    [[nodiscard]] input_type await_resume() noexcept {
      if constexpr (!std::is_void_v<input_type>)
        return std::move(input);
    }
  };

  // non-owning lockfree manager for distributed awaiters system.
  // awaiters always alive, unique, one-thread interact with manager
  // (but manager interact with any count of threads)

  nonowner_lockfree_stack<awaiter_t> subscribers;

 public:
  event() noexcept = default;
  event(event&&) = delete;
  void operator=(event&&) = delete;

  // event source functional

  // returns false if no one has been woken up
  // clang-format off
  template<executor Executor>
  requires(std::is_void_v<Input>)
  bool notify_all(Executor&& exe) {
    // clang-format on
    // must be carefull - awaiter ptrs are or their coroutine(which we want to execute / destroy)
    awaiter_t* top = subscribers.try_pop_all();
    if (top == nullptr)
      return false;
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
        subscribers.push_stack(top);
        throw;
      }
      top = next;
    }
    return true;
  }

  // copies input for all recievers(all coros returns to waiting if copy constructor throws)
  // returns false if no one has been woken up
  // clang-format off
  template <executor Executor>
  requires(!std::is_void_v<Input>)
  bool notify_all(Executor&& exe, input_type input) {
    // clang-format on
    awaiter_t* top = subscribers.try_pop_all();
    if (top == nullptr)
      return false;
    awaiter_t* next;
    while (top != nullptr) {
      next = top->next;  // copy from awaiter, which on frame and will die
      try {
        top->input = input;  // not in copy, input must be on coroutine before await_resume
        std::atomic_thread_fence(std::memory_order::release);
      } catch (...) {
        subscribers.push_stack(top);
        throw;
      }
      std::coroutine_handle<void> handle = top->handle;
      try {
        std::forward<Executor>(exe).execute(handle);
      } catch (...) {
        if (!handle.done())  // throw was not while resuming
          subscribers.push(top);
        top = next;
        subscribers.push_stack(top);
        throw;
      }
      top = next;
    }
    return true;
  }

  // subscribe for non coroutines

  // callback invoked once and then dissapears
  template <typename F>
  void set_callback(F&& f) {
    [](event& event_, std::remove_reference_t<F> f_) -> job {
      if constexpr (std::is_void_v<Input>) {
        (void)(co_await event_);
        f_();
      } else {
        auto&& input = co_await event_;
        f_(std::forward<decltype(input)>(input));
      }  // INVOKED HERE
    }(*this, std::forward<F>(f));
  }

  // subscribe, but only for coroutines
  [[nodiscard]] auto operator co_await() noexcept {
    return awaiter_t{this};
  }
};

// default interaction point between recipients and sources of events

template <typename Input, typename Tag = void>
inline constinit event<Input> event_v{};

template <typename... Inputs>
KELCORO_CO_AWAIT_REQUIRED constexpr auto when_all(event<Inputs>&... events) {
  static_assert(sizeof...(Inputs) < 256);

  // always default constructible and noexcept movable because of event_t guarantees
  using storage_t = std::tuple<typename event<Inputs>::input_type...>;
  struct subscribe_all_last_resumes_main_t {
    uint8_t count;
    std::tuple<event<Inputs>&...> events_;
    KELCORO_NO_UNIQUE_ADDRESS storage_t input;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle) {
      const auto create_cb_for = [&]<typename Input, size_t EventIndex>(
                                     std::type_identity<Input>, std::integral_constant<size_t, EventIndex>) {
        if constexpr (std::is_void_v<Input>) {
          return [this, handle] {
            // count_ declared here and not in [ ] of lambda because of MSVC BUG
            auto count_ = std::atomic_ref(this->count);
            if (count_.fetch_add(1, std::memory_order::acq_rel) == sizeof...(Inputs) - 1)
              handle.resume();
          };
        } else {
          return [this, handle](Input input_) {
            auto count_ = std::atomic_ref(this->count);
            std::get<EventIndex>(this->input) = std::move(input_);
            if (count_.fetch_add(1, std::memory_order::acq_rel) == sizeof...(Inputs) - 1)
              handle.resume();
          };
        }
      };

      [&]<size_t... Is>(std::index_sequence<Is...>) {
        (std::get<Is>(events_).set_callback(
             create_cb_for(std::type_identity<Inputs>{}, std::integral_constant<size_t, Is>{})),
         ...);
      }  // INVOKED HERE
      (std::index_sequence_for<Inputs...>{});
    }
    // noexcept because of event_ invariants
    [[nodiscard]] storage_t await_resume() noexcept {
      return std::move(input);
    }
  };
  return subscribe_all_last_resumes_main_t{0, std::tie(events...)};
}

template <typename... Inputs>
KELCORO_CO_AWAIT_REQUIRED constexpr auto when_any(event<Inputs>&... events) {
  static_assert(sizeof...(Inputs) < 256 && sizeof...(Inputs) >= 2);

  struct subscribe_all_first_resumes_main_t {
    // no monostate because default constructible always
    std::tuple<event<Inputs>&...> events_;
    KELCORO_NO_UNIQUE_ADDRESS std::variant<typename event<Inputs>::input_type...> input;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle) {
      // need to allocate, because after coro resuming awaiter will die(with flag in it)
      auto count_ptr = std::make_shared<std::atomic<uint8_t>>(0);
      const auto create_cb_for = [&]<typename Input, size_t EventIndex>(
                                     std::type_identity<Input>, std::integral_constant<size_t, EventIndex>) {
        if constexpr (std::is_void_v<Input>) {
          return [this, count_ptr, handle]() {
            auto value = count_ptr->fetch_add(1, std::memory_order::acq_rel);
            if (value == 0) {
              this->input.template emplace<EventIndex>(nullstruct{});
              handle.resume();
            }
          };
        } else {
          return [this, count_ptr, handle](Input input_) {
            auto value = count_ptr->fetch_add(1, std::memory_order::acq_rel);
            if (value == 0) {
              this->input.template emplace<EventIndex>(std::move(input_));
              // move ctor for input type always noexcept(static assert in 'event')
              handle.resume();
            }
          };
        }
      };
      [&]<size_t... Is>(std::index_sequence<Is...>) {
        (std::get<Is>(events_).set_callback(
             create_cb_for(std::type_identity<Inputs>{}, std::integral_constant<size_t, Is>{})),
         ...);
      }  // INVOKED HERE
      (std::index_sequence_for<Inputs...>{});
    }
    // always returns variant with information what happens(index) and input
    // always noexcept because of 'event' invariants
    [[nodiscard]] auto await_resume() noexcept {
      return std::move(input);
    }
  };
  return subscribe_all_first_resumes_main_t{{events...}};
}

}  // namespace dd