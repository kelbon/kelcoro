#pragma once

#include "common.hpp"

namespace dd {

// logic and behavior is very similar to generator, but its async(may suspend before co_yield)

template <typename>
struct channel;
template <typename>
struct channel_iterator;

template <typename Yield>
struct channel_promise : enable_memory_resource_support, not_movable {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<channel_promise>;

 private:
  friend channel<Yield>;
  friend channel_iterator<Yield>;

  // invariant: root != nullptr
  channel_promise* root = this;
  handle_type current_worker = self_handle();
  // invariant: never nullptr, stores owner for leafs and consumer for top-level channel
  union {
    channel<Yield>* _consumer;  // setted only in root
    handle_type _owner;         // setted only in leafs
  };

  handle_type owner() const noexcept {
    assert(root != this);
    return _owner;
  }
  channel<Yield>* consumer() const noexcept {
    return root->_consumer;
  }
  std::coroutine_handle<>& consumer_handle() const noexcept {
    return consumer()->handle;
  }
  void set_result(Yield* v) const noexcept {
    consumer()->current_result = v;
  }
  std::exception_ptr& exception() const noexcept {
    return consumer()->exception;
  }
  void set_exception(std::exception_ptr e) const noexcept {
    exception() = e;
  }

 public:
  constexpr channel_promise() noexcept {
  }

  [[gnu::pure]] handle_type self_handle() noexcept {
    return handle_type::from_promise(*this);
  }
  channel<Yield> get_return_object() noexcept {
    return channel(self_handle());
  }

 private:
  // TODO reuse in generator
  struct hold_value_until_resume {
    Yield value;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    std::coroutine_handle<> await_suspend(handle_type handle) noexcept {
      handle.promise().set_result(std::addressof(value));
      return handle.promise().consumer_handle();
    }
    static constexpr void await_resume() noexcept {
    }
  };

  struct attach_leaf {
    channel<Yield> leaf;

    bool await_ready() const noexcept {
      return leaf.empty();
    }

    std::coroutine_handle<> await_suspend(handle_type owner) const noexcept {
      assert(owner != leaf.top);
      channel_promise& leaf_p = leaf.top.promise();
      channel_promise& root_p = *owner.promise().root;
      leaf_p.current_worker.promise().root = &root_p;
      leaf_p._owner = owner;
      root_p.current_worker = leaf_p.current_worker;
      return leaf_p.current_worker;
    }
    static constexpr void await_resume() noexcept {
    }
  };

 public:
  transfer_control_to yield_value(Yield&& rvalue) noexcept {
    set_result(std::addressof(rvalue));
    return transfer_control_to{consumer_handle()};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return hold_value_until_resume{Yield(clvalue)};
  }
  transfer_control_to yield_value(terminator_t) noexcept {
    set_result(nullptr);
    return transfer_control_to{consumer_handle()};
  }
  transfer_control_to yield_value(by_ref<Yield> r) noexcept {
    set_result(std::addressof(r.value));
    return transfer_control_to{consumer_handle()};
  }

  template <typename X>
  attach_leaf yield_value(elements_of<X> e) noexcept {
    using rng_t = std::remove_cvref_t<X>;
    // TODO for all other ranges too
    if constexpr (!std::is_same_v<rng_t, channel<Yield>>)
      static_assert(![] {});
    if constexpr (std::is_same_v<typename rng_t::value_type, Yield>) {
      return attach_leaf{std::move(e.rng)};
    } else {
      auto make_channel = [](auto& r) -> channel<Yield> {
        auto next = r.next();
        while (auto* x = co_await next)
          co_yield Yield(std::move(*x));
      };
      return attach_leaf{make_channel(e.rng)};
    }
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    if (root != this) {
      root->current_worker = owner();
      root->current_worker.promise().root = root;
      return transfer_control_to{owner()};
    }
    set_result(nullptr);
    return transfer_control_to{consumer_handle()};
  }
  static constexpr void return_void() noexcept {
  }
  [[gnu::cold]] void unhandled_exception() {
    // case when already was exception, its not handled yet and next generated
    if (exception() != nullptr) [[unlikely]]
      std::terminate();
    if (root == this) {
      set_result(nullptr);
      throw;  // after it top is .done() and iteration is over
    }
    (void)final_suspend();  // up owner(we are done)
    // consumer sees nullptr and stop iterating,
    // if consumer catches/ignores exception and calls .begin again, he will observe elements from owner,
    // effectifelly we will skip failed leaf
    set_exception(std::current_exception());  // notify root->consumer about exception
    _consumer = root->_consumer;              // 'final_suspend' will 'set_result' through root
    root = this;                              // force final suspend return into consumer
    // here 'final suspend' sets result to 0 and returns to consumer
  }
};

// its pseudo iterator, requires co_awaits etc
template <typename Yield>
struct channel_iterator : not_movable {
 private:
  channel<Yield>& chan;
  friend channel<Yield>;

  constexpr explicit channel_iterator(channel<Yield>& c) noexcept : chan(c) {
  }

 public:
  using reference = std::conditional_t<std::is_const_v<Yield>, Yield&, Yield&&>;

  // return true if they are attached to same 'channel' object
  constexpr bool equivalent(const channel_iterator& other) const noexcept {
    return std::addressof(chan) == std::addressof(other.chan);
  }
  constexpr bool operator==(std::default_sentinel_t) const noexcept {
    assert(!(chan.top.done() && chan.current_result != nullptr));
    return chan.current_result == nullptr;
  }
  // * returns rvalue ref
  reference operator*() const noexcept {
    assert(*this != std::default_sentinel);
    return static_cast<reference>(*chan.current_result);
  }
  // * after invoking references to value from operator* are invalidated
  KELCORO_CO_AWAIT_REQUIRED transfer_control_to operator++() noexcept {
    assert(*this != std::default_sentinel);
    return transfer_control_to{chan.top.promise().current_worker};
  }

  // on the end of loop, when current_value == nullptr reached
  // lifetime of this iterator ends and it checks for exception.
  // its for performance(no 'if' on each ++, only one for channel)
  ~channel_iterator() noexcept(false) {
    if (chan.exception) [[unlikely]]
      std::rethrow_exception(chan.take_exception());
  }
};

// co_await on empty channel produces nullptr
// for using see macro co_foreach(value, channel)
// or use manually
//   for(auto it = co_await chan.begin(); it != chan.end(); co_await ++it)
//       auto&& v = *it;
template <typename Yield>
struct channel {
  using promise_type = channel_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;

 private:
  friend channel_promise<Yield>;
  friend channel_iterator<Yield>;
  // invariant: == nullptr when generated last value.
  // Its important for exception handling and better == end(not call .done())
  // initialized when first value created(on in final suspend)
  Yield* current_result = nullptr;
  std::coroutine_handle<> handle = nullptr;  // coro in which i exist(setted in co_await on .begin)
  coroutine_handle<promise_type> top = always_done_coroutine();  // current top level channel
  // invariant: setted only once for one coroutine frame
  // if setted, then top may be not done yet
  std::exception_ptr exception = nullptr;

  // precondition: 'handle' != nullptr, handle does not have other owners
  // used from promise::get_return_object
  constexpr explicit channel(handle_type top) noexcept : top(top) {
  }

 public:
  // postcondition: empty()
  constexpr channel() noexcept = default;

  constexpr channel(channel&& other) noexcept {
    swap(other);
  }
  constexpr channel& operator=(channel&& other) noexcept {
    swap(other);
    return *this;
  }

  void swap(channel& other) noexcept {
    std::swap(current_result, other.current_result);
    std::swap(handle, other.handle);
    std::swap(top, other.top);
    std::swap(exception, other.exception);
  }
  friend void swap(channel& a, channel& b) noexcept {
    a.swap(b);
  }

  constexpr void reset(handle_type handle) noexcept {
    clear();
    if (handle)
      top = handle;
  }
  // postcondition: .empty()
  // after this method its caller responsibility to correctly destroy 'handle'
  [[nodiscard]] constexpr handle_type release() noexcept {
    if (empty())
      return nullptr;
    return std::exchange(top, always_done_coroutine()).get();
  }
  // postcondition: .empty()
  constexpr void clear() noexcept {
    std::exchange(top, always_done_coroutine()).destroy();
  }
  ~channel() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return top.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  // returns exception which happens while iterating (or 0)
  // postcondition: exception marked as handled
  [[nodiscard]] std::exception_ptr take_exception() noexcept {
    return std::exchange(exception, nullptr);
  }

 private:
  struct starter : not_movable {
    channel& self;

    constexpr explicit starter(channel& c) noexcept : self(c) {
    }
    bool await_ready() const noexcept {
      return self.empty();
    }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> consumer) noexcept {
      self.handle = consumer;
      self.top.promise()._consumer = &self;
      return self.top.promise().current_worker;
    }
    [[nodiscard]] channel_iterator<Yield> await_resume() const noexcept {
      return channel_iterator<Yield>{self};
    }
  };

 public:
  // * if .empty(), then co_await begin() == end()
  // produces next value(often first)
  // NOTE: 'co_await begin' invalidates previous iterators!
  KELCORO_CO_AWAIT_REQUIRED starter begin() & noexcept KELCORO_LIFETIMEBOUND {
    return starter{*this};
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }
};

// usage example:
//  co_foreach(std::string s, mychannel) use(s);
// OR
//  co_foreach(YieldType&& x, mychannel) { ..use(std::move(x)).. };
#define co_foreach(VARDECL, ... /*CHANNEL, may be expression produces channel*/)                        \
  if (auto&& dd_channel_ = __VA_ARGS__; true)                                                           \
    for (auto dd_b_ = co_await dd_channel_.begin(); dd_b_ != ::std::default_sentinel; co_await ++dd_b_) \
      if (VARDECL = *dd_b_; true)
}  // namespace dd
