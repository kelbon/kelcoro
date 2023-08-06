#pragma once

#include "common.hpp"

namespace dd {

// logic and behavior is very similar to generator, but its async(may suspend before co_yield)

// TODO создать generator просто одним ифом в channel, это же одно и то же. Различие в обработке исключений,
// запрете co_await и наличии итератора(т.к. существует гарантия, что синхронно)

template <typename>
struct channel;

struct nothing_t {
  explicit nothing_t() = default;
};
// may be yielded from channel to produce nullptr on caller side without ending an channel.
// for example you can produce 'null terminated' sequences of values with this
// or create an empty but never done channel
constexpr inline nothing_t nothing{};

template <typename Yield>
struct channel_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<channel_promise>;

  struct consumer_t {
   private:
    friend channel_promise;
    friend channel<Yield>;

    Yield* current_result = nullptr;
    std::coroutine_handle<> handle;
    std::coroutine_handle<> _top;  // maybe always done coro or handle_type

    constexpr consumer_t(std::coroutine_handle<> top) noexcept : _top(top) {
      assume_not_null(top);
    }
    handle_type top() const noexcept {
      return handle_type::from_address(_top.address());
    }

   public:
    bool await_ready() const noexcept {
      return _top.done();
    }
    std::coroutine_handle<void> await_suspend(std::coroutine_handle<void> consumer) noexcept {
      handle = consumer;
      auto& p = top().promise();
      p.consumer = this;
      return p.current_worker;
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
    std::coroutine_handle<> leaf;

    bool await_ready() const noexcept {
      assume_not_null(leaf);
      return leaf.done();
    }

    std::coroutine_handle<> await_suspend(handle_type owner) const noexcept {
      channel_promise& leaf_p = handle_type::from_address(leaf.address()).promise();
      consumer_t& consumer = *owner.promise().consumer;
      leaf_p.consumer = &consumer;
      leaf_p.owner = owner;
      consumer.top().promise().current_worker = leaf_p.current_worker;
      return leaf_p.current_worker;
    }
    constexpr void await_resume() const {
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
  transfer_control_to yield_value(nothing_t) noexcept {
    consumer->current_result = nullptr;
    return transfer_control_to{consumer->handle};
  }
  transfer_control_to yield_value(by_ref<Yield> r) noexcept {
    consumer->current_result = std::addressof(r.value);
    return transfer_control_to{consumer->handle};
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
      return attach_leaf{e.rng.release()};
    } else {
      auto make_channel = [](auto& r) -> channel<Yield> {
        auto next = r.next();
        while (auto* x = co_await next)
          co_yield Yield(*x);
      };
      return attach_leaf{make_channel(e.rng).release()};
    }
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    consumer->top().promise().current_worker = owner;
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
  [[noreturn]] static void unhandled_exception() noexcept {
    std::terminate();
  }

  bool done() noexcept {
    return get_return_object().done();
  }
};

// co_await on empty channel produces nullptr
template <typename Yield>
struct channel {
  using promise_type = channel_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;

 private:
  // invariant: != nullptr
  std::coroutine_handle<> handle = always_done_coroutine();

 public:
  // postcondition: empty()
  constexpr channel() noexcept = default;
  // precondition: 'handle' != nullptr
  constexpr channel(handle_type handle) noexcept : handle(handle) {
    assume_not_null(handle);
  }

  constexpr channel(channel&& other) noexcept : handle(other.release()) {
  }
  constexpr channel& operator=(channel&& other) noexcept {
    std::swap(handle, other.handle);
    return *this;
  }

  // postcondition: .empty()
  // after this method its caller responsibility to correctly destroy 'handle'
  [[nodiscard]] constexpr std::coroutine_handle<> release() noexcept {
    return std::exchange(handle, always_done_coroutine());
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
    return handle.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  // usage:
  //  while(auto* x = co_await channel.next()) TODO doc
  [[nodiscard]] auto next() & noexcept KELCORO_LIFETIMEBOUND {
    assume_not_null(handle);
    return typename promise_type::consumer_t(handle);
  }
};

}  // namespace dd
