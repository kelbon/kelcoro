#pragma once

#include "common.hpp"

namespace dd {

// TODO запретить next на rvalue канале? И запретить && на rvalue next!
// тогда юзер всегда если не даун будет писать
// channel c = foo();
// auto next = c.next();
// while (auto* p = co_await next)
// но хотелось бы конечно приятнее как то + обработка исключений. Мб запихнуть под макрос
// logic and behavior is very similar to generator, but its async(may suspend before co_yield)
// ... HMM вообще говоря динамический goto вполне решает проблему... и ифа больше не будет

// TODO для таски и канала добавить возможность запустить их синхронно, но это явно unsafe.. Хотя это
// эквивалентно просто резуму хендла

// TODO exception into consumer?
template <typename>
struct channel;

template <typename Yield>
struct channel_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<channel_promise>;

  struct consumer_t {
   private:
    friend channel_promise;
    friend channel<Yield>;

    Yield* current_result;
    std::coroutine_handle<> handle;
    handle_type top;

    constexpr consumer_t(handle_type top) noexcept : current_result(nullptr), handle(nullptr), top(top) {
      assume_not_null(top);
    }

   public:
    bool await_ready() const noexcept {
      return top.done();
    }
    std::coroutine_handle<void> await_suspend(std::coroutine_handle<void> consumer) noexcept {
      handle = consumer;
      top.promise().consumer = this;
      return top.promise().current_worker;
    }
    [[nodiscard]] Yield* await_resume() const noexcept {
      return current_result;
    }
  };
  // invariant: if running, then consumer != nullptr, setted when channel co_awaited
  consumer_t* consumer;
  handle_type current_worker = get_return_object();
  // nullptr means top-level
  handle_type owner = nullptr;
  // stores unhandled exception, rethrows it
  // TODO прокидывается везде через attach leaf await resume, но в самом конце в потребителе
  // я не хочу ифы на каждое значение, тут вариант либо visit в каком то виде...(подключение к консумеру)
  // потребителя берущего функцию и применяющую ко всему из канала)
  // либо второй вариант - в ~next_t делать проверку и прокидывать исключение, но это конечно проблема
  // плюс ещё проблема, что исключение тогда может вылететь абы где, а не тогда когда логически оно вылетело
  // вариант №3, макрос async for each делающий ровно это, с проверкой после цикла и бросанием
  std::exception_ptr exception = nullptr;

  channel_promise() = default;

  channel_promise(channel_promise&&) = delete;
  void operator=(channel_promise&&) = delete;

  handle_type get_return_object() noexcept {
    return handle_type::from_promise(*this);
  }

 private:
  struct hold_value_until_resume {
    Yield value;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    std::coroutine_handle<> await_suspend(handle_type current_leaf) noexcept {
      consumer_t& c = *current_leaf.promise().consumer;
      c.current_result = std::addressof(value);
      return c.handle;
    }
    static constexpr void await_resume() noexcept {
    }
  };

  struct attach_leaf {
    // precondition: leaf != nullptr
    handle_type leaf;

    bool await_ready() const noexcept {
      assume_not_null(leaf);
      return leaf.done();
    }

   public:
    std::coroutine_handle<> await_suspend(handle_type owner) const noexcept {
      channel_promise& leaf_p = leaf.promise();
      consumer_t& consumer = *owner.promise().consumer;
      leaf_p.consumer = &consumer;
      leaf_p.owner = owner;
      consumer.top.promise().current_worker = leaf_p.current_worker;
      return leaf_p.current_worker;
    }
    constexpr void await_resume() const {
      if (leaf.promise().exception)
        std::rethrow_exception(leaf.promise().exception);
    }
    ~attach_leaf() {
      // make sure compiler, that its destroyed here (end of yield expression)
      leaf.destroy();
    }
  };

 public:
  transfer_control_to yield_value(Yield&& rvalue) noexcept {
    consumer->current_result = std::addressof(rvalue);
    return transfer_control_to{consumer->handle};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return hold_value_until_resume{Yield(clvalue)};
  }
  // attaches leaf channel
  // TODO think about generator-leaf
  // postcondition: e.rng.empty()
  template <typename X>
  attach_leaf yield_value(elements_of<X> e) noexcept {
    using rng_t = std::decay_t<X>;
    if constexpr (!std::is_same_v<rng_t, channel<Yield>>)
      static_assert(![] {});
    if constexpr (std::is_same_v<typename rng_t::value_type, Yield>) {
      handle_type h = e.rng.release();
      if (!h)
        h = []() -> channel<Yield> { co_return; }().release();
      assume_not_null(h);
      return attach_leaf{h};
    } else {
      auto make_channel = [](auto& r) -> channel<Yield> {
        auto next = r.next();
        while (auto* x = co_await next)
          co_yield Yield(*x);
      };
      return attach_leaf{make_channel(e.rng).release()};
    }
    // TODO else if generator/just range
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    consumer->top.promise().current_worker = owner;
    consumer->current_result = nullptr;  // may be im last
    // i dont have value here now, so i ask 'owner' to create it
    if (owner) {
      owner.promise().consumer = consumer;
      return transfer_control_to{owner};
    }
    return transfer_control_to{consumer->handle};
  }
  static constexpr void return_void() noexcept {
  }
  void unhandled_exception() noexcept {
    exception = std::current_exception();
    // TODO check fallback into final suspend
  }

  // interface for iterator, used only on top-level generator
  // TODO struct next_awaiter { ... }; and maybe .done() lifetimebound!!!

  bool done() noexcept {
    return get_return_object().done();
  }
};

// co_await on empty/ended with exception channel produces nullptr
template <typename Yield>
struct channel {
  using promise_type = channel_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;

 private:
  handle_type handle;

 public:
  // postcondition: empty()
  constexpr channel() noexcept = default;
  // precondition: 'handle' != nullptr
  constexpr channel(handle_type handle) noexcept : handle(handle) {
    assume_not_null(handle);
  }

  constexpr channel(channel&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
  }
  constexpr channel& operator=(channel&& other) noexcept {
    std::swap(handle, other.handle);
    return *this;
  }

  // postcondition: .empty()
  // after this method its caller responsibility to correctly destroy 'handle'
  [[nodiscard]] constexpr handle_type release() noexcept {
    return std::exchange(handle, nullptr);
  }
  // postcondition: .empty()
  constexpr void clear() noexcept {
    if (handle)
      release().destroy();
  }
  constexpr ~channel() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return !handle || handle.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  constexpr bool ended_with_exception() const noexcept {
    return handle && handle.promise().exception;
  }

  // TODO consume_all(foo) -> task<pair<decltype(foo), excepion_ptr>>, + move внутрь функции конечно же, если
  // не const
  // TODO перенести в promise + for(auto c = channel.consumer(); auto* v = co_await c;)
  // analogue for 'begin' of ranges
  // usage:
  //  while(auto* x = co_await channel.next())
  [[nodiscard]] auto next() & noexcept KELCORO_LIFETIMEBOUND {
    if (!handle) [[unlikely]]
      *this = []() -> channel<Yield> { co_return; }();
    assume_not_null(handle);
    return typename promise_type::consumer_t(handle);
  }
};

}  // namespace dd
