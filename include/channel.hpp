#pragma once

#include "common.hpp"

namespace dd {

// logic and behavior is very similar to generator, but its async(may suspend before co_yield)

template <typename>
struct channel;

struct nothing_t {
  explicit nothing_t() = default;
};
// may be yielded from channel to produce nullptr on caller side without ending an channel.
// for example you can produce 'null terminated' sequences of values with this
// or create an empty but never done channel
constexpr inline nothing_t nothing{};
// TODO реально было лучше, нужно откатится до состояния с консумером и обработать исключения тоже,
// возможно через ifdef типа если отключены искючения, то пошло оно нахуй(можно узнать как проверить что
// исключения отрублены)
template <typename Yield>
struct channel_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<channel_promise>;
  // TODO remove(make it iterator)
  struct consumer_t {
   private:
    friend channel_promise;
    friend channel<Yield>;

    std::coroutine_handle<> _top;  // may be always done coro or handle_type

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
      auto& root = top().promise();
      root.owner = consumer;
      return root.current_worker;
    }
    [[nodiscard]] Yield* await_resume() const noexcept {
      if (_top.done())
        return nullptr;  // TODO hmm
      return top().promise().current_result;
    }
  };
  // invariant: root != nullptr
  channel_promise* root = this;
  Yield* current_result = nullptr;
  handle_type current_worker = get_return_object();
  // invariant: never nullptr, stores owner for leafs and consumer for top-level channel
  std::coroutine_handle<> owner;

  handle_type owner_() const noexcept {
    assert(root != this);
    return handle_type::from_address(owner.address());
  }

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
    std::coroutine_handle<> await_suspend(handle_type handle) noexcept {
      channel_promise& root = *handle.promise().root;
      root.current_result = std::addressof(value);
      return root.owner;
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
      channel_promise& root = *owner.promise().root;
      leaf_p.current_worker.promise().root = &root;
      leaf_p.owner = owner;
      root.current_worker = leaf_p.current_worker;
      return leaf_p.current_worker;
    }
    constexpr void await_resume() const {
    }
    ~attach_leaf() {
      leaf.destroy();
    }
  };

 public:
  transfer_control_to yield_value(Yield&& rvalue) noexcept {
    root->current_result = std::addressof(rvalue);
    return transfer_control_to{root->owner};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return hold_value_until_resume{Yield(clvalue)};
  }
  transfer_control_to yield_value(nothing_t) noexcept {
    root->current_result = nullptr;
    return transfer_control_to{root->owner};
  }
  transfer_control_to yield_value(by_ref<Yield> r) noexcept {
    root->current_result = std::addressof(r.value);
    return transfer_control_to{root->owner};
  }
  // attaches leaf channel
  // postcondition: e.rng.empty()
  template <typename X>
  attach_leaf yield_value(elements_of<X> e) noexcept {
    using rng_t = std::remove_cvref_t<X>;
    if constexpr (!std::is_same_v<rng_t, channel<Yield>>)
      static_assert(![] {});
    if constexpr (std::is_same_v<typename rng_t::value_type, Yield>) {
      return attach_leaf{e.rng.release()};
    } else {
      auto make_channel = [](auto& r) -> channel<Yield> {
        auto next = r.next();
        while (auto* x = co_await next)
          co_yield Yield(std::move(*x));
      };
      return attach_leaf{make_channel(e.rng).release()};
    }
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    if (root != this) {
      root->current_worker = owner_();
      root->current_worker.promise().root = root;
    }
    return transfer_control_to{owner};
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] static void unhandled_exception() noexcept {
    std::terminate();
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
  // precondition: 'handle' != nullptr, handle does not have other owners
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

  // co_await on result will return pointer to
  // next value(or nullptr if dd::nothing yielded or .empty())
  // usage:
  //  while(auto* x = co_await channel.next())
  [[nodiscard]] auto next() & noexcept KELCORO_LIFETIMEBOUND {
    assume_not_null(handle);
    return typename promise_type::consumer_t(handle);
  }
};

}  // namespace dd
