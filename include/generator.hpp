#pragma once

#include <iterator>
#include <memory>
#include <utility>

#include "common.hpp"

#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif

namespace dd {

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

template <typename Yield>
struct generator_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  // invariant: root != nullptr
  generator_promise* root = this;
  // invariant: never nullptr, initialized when generator created
  Yield* current_result;
  std::coroutine_handle<> current_worker = get_return_object();
  std::coroutine_handle<> owner = std::noop_coroutine();

  generator_promise() = default;

  generator_promise(generator_promise&&) = delete;
  void operator=(generator_promise&&) = delete;

  auto get_return_object() noexcept {
    return std::coroutine_handle<generator_promise>::from_promise(*this);
  }

 private:
  struct hold_value_until_resume {
    Yield value;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<generator_promise> handle) noexcept {
      handle.promise().root->current_result = std::addressof(value);
    }
    static constexpr void await_resume() noexcept {
    }
  };

  struct attach_leaf {
    // precondition: leaf != nullptr
    std::coroutine_handle<generator_promise> leaf;

    bool await_ready() const noexcept {
      [[assume(leaf != nullptr)]];
      return leaf.done();
    }
    void await_suspend(std::coroutine_handle<generator_promise> owner) const noexcept {
      generator_promise& leaf_p = leaf.promise();
      generator_promise& root = *owner.promise().root;
      leaf_p.root = &root;
      leaf_p.owner = owner;
      root.current_worker = leaf;
      root.current_result = leaf_p.current_result;  // first value was created by leaf
    }
    static constexpr void await_resume() noexcept {
    }
    ~attach_leaf() {
      // make sure compiler, that its destroyed here (end of yield expression)
      leaf.destroy();
    }
  };

 public:
  std::suspend_always yield_value(Yield& lvalue) noexcept {
    root->current_result = std::addressof(lvalue);
    return {};
  }
  std::suspend_always yield_value(Yield&& lvalue) noexcept {
    root->current_result = std::addressof(lvalue);
    return {};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>)
    requires(!std::is_const_v<Yield>)
  {
    return hold_value_until_resume{Yield(clvalue)};
  }
  // attaches leaf-generator
  template <typename R>
  attach_leaf yield_value(elements_of<R>) noexcept;

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    root->current_worker = owner;
    return transfer_control_to{owner};  // noop coro if done
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] static void unhandled_exception() {
    throw;
  }

  // interface for iterator, used only on top-level generator

  bool done() noexcept {
    return get_return_object().done();
  }
  void produce_next() {
    assert(root == this && !done());
    current_worker.resume();
  }
};

template <typename Yield>
struct generator_iterator {
 private:
  // but.. why?
  // Its required for guarantee that default constructed generator is an empty range
  // so i store erased handle when iterator is default constructed and 'handle' otherwise
  // P.S. i know about formal UB here
  union {
    // invariants: always != nullptr, if !erased_handle.done(), then 'handle' stored
    std::coroutine_handle<> erased_handle;
    std::coroutine_handle<generator_promise<Yield>> handle;
  };

 public:
  generator_iterator() noexcept : erased_handle(always_done_coroutine()) {
  }
  // precondition: h != nullptr
  generator_iterator(std::coroutine_handle<generator_promise<Yield>> h) noexcept : handle(h) {
    assert(h != nullptr);
  }

  using iterator_category = std::input_iterator_tag;
  using value_type = Yield;
  using difference_type = ptrdiff_t;

  bool operator==(std::default_sentinel_t) const noexcept {
    assert(erased_handle != nullptr);  // invariant
    return erased_handle.done();
  }
  // * may be invoked > 1 times,
  // * change of value by reference may be observed from generator if lvalue was yielded
  value_type& operator*() const noexcept {
    assert(!handle.done());
    return *handle.promise().current_result;
  }
  generator_iterator& operator++() {
    assert(!handle.done());
    handle.promise().produce_next();
    return *this;
  }
  void operator++(int) {
    ++(*this);
  }
};

template <typename Yield>
struct generator {
  using iterator = generator_iterator<Yield>;
  using handle_type = std::coroutine_handle<generator_promise<Yield>>;

 private:
  handle_type handle = nullptr;

 public:
  using value_type = Yield;
  using promise_type = generator_promise<Yield>;

  constexpr generator() noexcept = default;
  // precondition : you dont use this constructor, its only for compiler
  constexpr generator(handle_type handle) noexcept : handle(handle) {
    assert(handle.address() != nullptr && "only compiler must use this constructor");
    [[assume(handle != nullptr)]];
  }
  constexpr generator(generator&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
  }
  constexpr generator& operator=(generator&& other) noexcept {
    std::swap(handle, other.handle);
    return *this;
  }

  constexpr ~generator() {
    if (handle)
      handle.destroy();
  }

  iterator begin() const noexcept [[clang::lifetimebound]] {
    if (!handle) [[unlikely]]
      return iterator{};
    return iterator(handle);
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  bool empty() const noexcept {
    return !handle || handle.done();
  }
  // postcondition: handle == nullptr
  // after this method its caller responsibility to correctly destroy 'handle'
  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(handle, nullptr);
  }
};

template <typename Yield>
template <typename R>
typename generator_promise<Yield>::attach_leaf generator_promise<Yield>::yield_value(
    elements_of<R> e) noexcept {
  if constexpr (std::is_same_v<std::remove_cvref_t<R>, generator<Yield>>) {
    std::coroutine_handle h = e.rng.release();
    // handle case when default constructed generator yielded,
    // (it must always behave as empty range)
    if (!h) [[unlikely]]
      return attach_leaf{[]() -> generator<Yield> { co_return; }().release()};
    return attach_leaf{h};
  } else {
    auto make_gen = [](auto& r) -> generator<Yield> {
      for (auto&& x : r) {
        using val_t = std::remove_reference_t<decltype(x)>;
        if constexpr (std::is_same_v<val_t, Yield>)
          co_yield x;
        else
          co_yield Yield(x);
      }
    };
    return attach_leaf{make_gen(e.rng).release()};
  }
}

}  // namespace dd

#if __clang__
#pragma clang diagnostic pop
#endif
