#pragma once

#include <concepts>

#include "noexport/macro.hpp"
#include "memory_support.hpp"

namespace dd {

// concept of type which can be returned from function or yielded from generator
// that is - not function, not array, not cv-qualified (its has no )
// additionally reference is not yieldable(std::ref exists...)
template <typename T>
concept yieldable = !std::is_void_v<T> && (std::same_as<std::decay_t<T>, T> || std::is_lvalue_reference_v<T>);

template <typename R>
struct elements_of {
  KELCORO_NO_UNIQUE_ADDRESS R rng;

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
by_ref(Yield&) -> by_ref<Yield&>;

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

}  // namespace dd

namespace dd::noexport {

template <typename Leaf>
struct attach_leaf {
  Leaf leaf;

  bool await_ready() const noexcept {
    return leaf.empty();
  }

  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
      typename Leaf::handle_type owner) const noexcept {
    assert(owner != leaf.top);
    auto& leaf_p = leaf.top.promise();
    auto& root_p = *owner.promise().root;
    leaf_p.current_worker.promise().root = &root_p;
    leaf_p._owner = owner;
    root_p.current_worker = leaf_p.current_worker;
    return leaf_p.current_worker;
  }
  // support yielding generators with different resource
  template <memory_resource R>
  KELCORO_ASSUME_NOONE_SEES auto await_suspend(
      std::coroutine_handle<resourced_promise<typename Leaf::promise_type, R>> handle) {
    return await_suspend(handle.promise().self_handle());
  }
  static constexpr void await_resume() noexcept {
  }
};

template <yieldable Y, typename Generator>
Generator to_generator(auto&& rng) {
  // 'rng' captured by ref because used as part of 'co_yield' expression
  if constexpr (!std::ranges::borrowed_range<decltype(rng)> &&
                std::is_same_v<std::ranges::range_rvalue_reference_t<decltype(rng)>, Y&&>) {
    using std::begin;
    using std::end;
    auto&& b = begin(rng);
    auto&& e = end(rng);
    for (; b != e; ++b)
      co_yield std::ranges::iter_move(b);
  } else {
    for (auto&& x : rng)
      co_yield static_cast<Y&&>(std::forward<decltype(x)>(x));
  }
}
template <typename, yieldable, template <typename> typename>
struct extract_from;

template <typename Rng, yieldable Y>
struct extract_from<Rng, Y, generator> {
  static generator<Y> do_(generator<Y>* g) {
    return std::move(*g);
  }
  template <memory_resource R>
  static generator<Y> do_(resourced<generator<Y>, R>* g) {
    return do_(&g->decay());
  }
  static generator<Y> do_(auto* r) {
    return to_generator<Y, generator<Y>>(static_cast<Rng&&>(*r));
  }
};
template <typename Rng, yieldable Y>
struct extract_from<Rng, Y, channel> {
  static channel<Y> do_(channel<Y>* g) {
    return std::move(*g);
  }
  template <memory_resource R>
  static channel<Y> do_(resourced<channel<Y>, R>* g) {
    return do_(&g->decay());
  }
  template <yieldable OtherY>
  static channel<Y> do_(channel<OtherY>* g) {
    auto& c = *g;
    // note: (void)(co_await) (++b)) only because gcc has bug, its not required
    for (auto b = co_await c.begin(); b != c.end(); (void)(co_await (++b)))
      co_yield static_cast<Y&&>(*b);
  }
  template <yieldable OtherY, memory_resource R>
  static channel<Y> do_(resourced<channel<OtherY>, R>* g) {
    return do_(&g->decay());
  }
  static channel<Y> do_(auto* r) {
    return to_generator<Y, channel<Y>>(static_cast<Rng&&>(*r));
  }
};

template <yieldable Y, template <typename> typename G, typename Rng>
attach_leaf<G<Y>> create_and_attach_leaf(elements_of<Rng>&& e) {
  return attach_leaf<G<Y>>{extract_from<Rng, Y, G>::do_(std::addressof(e.rng))};
}

template <typename Yield>
struct hold_value_until_resume {
  Yield value;

  static constexpr bool await_ready() noexcept {
    return false;
  }
  KELCORO_ASSUME_NOONE_SEES void await_suspend(
      std::coroutine_handle<generator_promise<Yield>> handle) noexcept {
    handle.promise().set_result(std::addressof(value));
  }
  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
      std::coroutine_handle<channel_promise<Yield>> handle) noexcept {
    handle.promise().set_result(std::addressof(value));
    return handle.promise().consumer_handle();
  }
  template <memory_resource R>
  KELCORO_ASSUME_NOONE_SEES void await_suspend(
      std::coroutine_handle<resourced_promise<generator_promise<Yield>, R>> handle) noexcept {
    // decay handle
    return await_suspend(handle.promise().self_handle());
  }
  template <memory_resource R>
  KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
      std::coroutine_handle<resourced_promise<channel_promise<Yield>, R>> handle) noexcept {
    // decay handle
    return await_suspend(handle.promise().self_handle());
  }
  static constexpr void await_resume() noexcept {
  }
};

}  // namespace dd::noexport
