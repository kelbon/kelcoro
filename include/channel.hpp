#pragma once

#include "common.hpp"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
namespace dd {

// behavior very similar to generator, but channel may suspend before co_yield

template <yieldable Yield>
struct channel_promise : not_movable {
  using handle_type = std::coroutine_handle<channel_promise>;

 private:
  friend channel<Yield>;
  friend channel_iterator<Yield>;
  friend noexport::attach_leaf<channel<Yield>>;
  friend noexport::hold_value_until_resume<Yield>;

  // invariant: root != nullptr
  channel_promise* root = this;
  handle_type current_worker = self_handle();
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
  KELCORO_PURE handle_type self_handle() noexcept {
    return handle_type::from_promise(*this);
  }

 public:
  constexpr channel_promise() noexcept {
  }

  channel<Yield> get_return_object() noexcept {
    return channel<Yield>(self_handle());
  }

  transfer_control_to yield_value(Yield&& rvalue) noexcept {
    set_result(std::addressof(rvalue));
    return transfer_control_to{consumer_handle()};
  }

  noexport::hold_value_until_resume<Yield> yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return noexport::hold_value_until_resume<Yield>{Yield(clvalue)};
  }
  transfer_control_to yield_value(by_ref<Yield> r) noexcept {
    set_result(std::addressof(r.value));
    return transfer_control_to{consumer_handle()};
  }
  template <typename X>
  noexport::attach_leaf<channel<Yield>> yield_value(elements_of<X> e) noexcept {
    return noexport::create_and_attach_leaf<Yield, channel>(std::move(e));
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
  void unhandled_exception() {
    // case when already was exception, its not handled yet and next generated
    if (exception() != nullptr) [[unlikely]]
      std::terminate();
    if (root != this)
      (void)final_suspend();  // up owner(we are done)
    // consumer sees nullptr and stop iterating,
    // if consumer catches/ignores exception and calls .begin again, he will observe elements from owner,
    // effectifelly we will skip failed leaf
    set_exception(std::current_exception());  // notify root->consumer about exception
    _consumer = root->_consumer;              // 'final_suspend' will 'set_result' through root->consumer
    root = this;                              // force final suspend return into consumer
    // here 'final suspend' sets result to 0 and returns to consumer
  }
};

// its pseudo iterator, requires co_awaits etc
template <yieldable Yield>
struct channel_iterator : not_movable {
 private:
  channel<Yield>& chan;
  friend channel<Yield>;

  constexpr explicit channel_iterator(channel<Yield>& c) noexcept : chan(c) {
  }

 public:
  using reference = Yield&&;

  // return true if they are attached to same 'channel' object
  constexpr bool equivalent(const channel_iterator& other) const noexcept {
    return std::addressof(chan) == std::addressof(other.chan);
  }
  channel<Yield>& owner() const noexcept {
    return chan;
  }

  constexpr bool operator==(std::default_sentinel_t) const noexcept {
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
//
// about R - see 'dd::with_resource'
template <yieldable Yield>
struct channel : enable_resource_deduction {
  using promise_type = channel_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;

 private:
  friend channel_promise<Yield>;
  friend channel_iterator<Yield>;
  friend noexport::attach_leaf<channel<Yield>>;

  // invariant: == nullptr when top.done()
  // Its important for exception handling and better == end(not call .done())
  // initialized when first value created(on in final suspend)
  Yield* current_result = nullptr;
  std::coroutine_handle<> handle = nullptr;  // coro in which i exist(setted in co_await on .begin)
  handle_type top = nullptr;                 // current top level channel
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

  // postconditions:
  // * other.empty()
  // * iterators to 'other' == end()
  constexpr channel(channel&& other) noexcept {
    swap(other);
  }
  constexpr channel& operator=(channel&& other) noexcept {
    swap(other);
    return *this;
  }

  // iterators to 'other' and 'this' are swapped too
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
    top = handle;
  }
  // postcondition: .empty()
  // after this method its caller responsibility to correctly destroy 'handle'
  [[nodiscard]] constexpr handle_type release() noexcept {
    return std::exchange(top, nullptr);
  }
  // postcondition: .empty()
  constexpr void clear() noexcept {
    if (top) {
      top.destroy();
      top = nullptr;
    }
  }
  ~channel() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return !top || top.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  // returns exception which happens while iterating (or nullptr)
  // postcondition: exception marked as handled (next call to 'take_exception' will return nullptr)
  [[nodiscard]] std::exception_ptr take_exception() noexcept {
    return std::exchange(exception, nullptr);
  }

  bool operator==(const channel& other) const noexcept {
    if (empty())
      return other.empty();
    return this == &other;  // invariant: coro handle has only one owner
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
  KELCORO_CO_AWAIT_REQUIRED starter begin() & noexcept KELCORO_LIFETIMEBOUND {
    return starter{*this};
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }
};

template <yieldable Y, memory_resource R>
using channel_r = resourced<channel<Y>, R>;

namespace pmr {

template <yieldable Y>
using channel = ::dd::channel_r<Y, polymorphic_resource>;

}

// usage example:
//  co_foreach(std::string s, mychannel) use(s);
// OR
//  co_foreach(YieldType&& x, mychannel) { ..use(std::move(x)).. };
#define co_foreach(VARDECL, ... /*CHANNEL, may be expression produces channel*/)      \
  if (auto&& dd_channel_ = __VA_ARGS__; true)                                         \
    for (auto dd_b_ = co_await dd_channel_.begin(); dd_b_ != ::std::default_sentinel; \
         (void)(co_await (++dd_b_)))                                                  \
      if (VARDECL = *dd_b_; true)
// note: (void)(co_await) (++dd_b)) only because gcc has bug, its not required
}  // namespace dd

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
