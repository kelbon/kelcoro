#pragma once

#include <coroutine>
#include <iterator>
#include <memory>
#include <utility>
#include <ranges>

#include "common.hpp"

#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif

namespace dd {

template <typename>
struct elements_of_t;

// TODO think about size
template <typename Yield>
struct generator_promise {
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
    static constexpr void await_resume() noexcept {}
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
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(std::is_nothrow_copy_constructible_v<Yield>)
    requires (!std::is_const_v<Yield>)
  {
    return hold_value_until_resume{Yield(clvalue)};
  }
  // attaches leaf-generator
  auto yield_value(elements_of_t<Yield>&&) noexcept;

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
    assert(handle != nullptr); // invariant
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

  template<typename>
  friend struct attach_leaf;
 public:
  static_assert(!std::is_reference_v<Yield>);

  using value_type = Yield;
  using promise_type = generator_promise<Yield>;
  constexpr generator() noexcept = default;
  // precondition : you dont use this constructor, its only for compiler
  constexpr generator(handle_type handle) noexcept : handle(handle) {
    assert(handle.address() != nullptr && "only compiler must use this constructor");
    const void* p = handle.address();
    [[assume(p != nullptr)]];
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
};

// implementation detail
template<typename Yield>
struct attach_leaf {
  generator<Yield>& leaf;

  bool await_ready() const noexcept {
    return leaf.empty();
  }
  void await_suspend(std::coroutine_handle<generator_promise<Yield>> owner) const noexcept {
    auto& leaf_p = leaf.handle.promise();
    generator_promise<Yield>* root = owner.promise().root;
    leaf_p.root = root;
    leaf_p.owner = owner;
    root->current_worker = leaf.handle;
    root->current_result = leaf_p.current_result;  // first value was created by leaf
  }
  static constexpr void await_resume() noexcept {}
};

template <typename Yield>
auto generator_promise<Yield>::yield_value(elements_of_t<Yield>&& e) noexcept {
  return attach_leaf<Yield>{e.g};
}

template <typename Yield>
struct elements_of_t {
  generator<Yield> g;
};
template <typename T>
elements_of_t(generator<T>) -> elements_of_t<T>;

template <std::ranges::range R>
[[nodiscard]] auto elements_of(R&& r [[clang::lifetimebound]]) {
  // takes reference to 'r' and its not a error, because it must be used
  // in yield expression co_yield dd::elements_of(rng{}); - 'rng' outlives created generator
  auto make_gen = [](R&& r) -> generator<std::ranges::range_value_t<R>> { // hmm, reference_t?..
    for (auto&& x : r)
      co_yield x;
  };
  return elements_of_t{make_gen(std::forward<R>(r))};
}

template <typename Yield>
[[nodiscard("co yield it")]] constexpr elements_of_t<Yield> elements_of(generator<Yield> g) noexcept {
  return elements_of_t{std::move(g)};
}

}  // namespace dd

#if __clang__
#pragma clang diagnostic pop
#endif
