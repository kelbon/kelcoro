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
// TODO usage .begin as output iterator hmm что то типа .out хммм
// TODO sent(x) yield overload
template <typename Yield>
struct generator_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<generator_promise>;

  // invariant: root != nullptr
  generator_promise* root = this;
  // invariant: never nullptr, initialized in first co_yield
  Yield* current_result;
  std::coroutine_handle<> current_worker = get_return_object();
  std::coroutine_handle<> owner = std::noop_coroutine();

  generator_promise() = default;

  generator_promise(generator_promise&&) = delete;
  void operator=(generator_promise&&) = delete;

  handle_type get_return_object() noexcept {
    return handle_type::from_promise(*this);
  }
  // there are no correct things which you can do with co_await
  // in generator
  void await_transform(auto&&) = delete;
  auto await_trasform(get_handle_t) const noexcept {
    return this_coro::handle.operator co_await();
  }

 private:
  struct hold_value_until_resume {
    Yield value;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    void await_suspend(handle_type handle) noexcept {
      handle.promise().root->current_result = std::addressof(value);
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

   private:
    void set_root_for_each_leaf_subleaf(generator_promise& root) const noexcept {
      // attached leaf already has some leaves and it is 'root' for them.
      // may be reached ONLY if co_yield elements_of
      // is before producing first value

      // invariant: 'leaf' is reachable from 'lowest', they are in same 'stack'
      handle_type lowest = handle_type::from_address(leaf.promise().current_worker.address());
      while (leaf != lowest) {
        lowest.promise().root = &root;
        lowest = handle_type::from_address(lowest.promise().owner.address());
      }
    }

   public:
    void await_suspend(handle_type owner) const noexcept {
      generator_promise& leaf_p = leaf.promise();
      generator_promise& root = *owner.promise().root;
      // TODO сделать так чтобы это было только в final suspend?
      if (leaf_p.current_worker != leaf) [[unlikely]]
        set_root_for_each_leaf_subleaf(root);
      leaf_p.root = &root;
      leaf_p.owner = owner;
      root.current_worker = leaf_p.current_worker;
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
  std::suspend_always yield_value(Yield&& lvalue) noexcept {
    root->current_result = std::addressof(lvalue);
    return {};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    // this overload accepts Yield& too, because if i accept lvalue by ref and on
    // call site move out value it will be visible(move_iterator will possbly break code)
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
    // i dont have value here now, so i ask 'owner' to create it
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
  // rvalue ref, but never produces const T&&
  using reference = std::conditional_t<std::is_const_v<Yield>, Yield&, Yield&&>;
  using difference_type = ptrdiff_t;

  bool operator==(std::default_sentinel_t) const noexcept {
    assert(erased_handle != nullptr);  // invariant
    return erased_handle.done();
  }
  // * may be invoked > 1 times, but be carefull with moving out
  reference operator*() const noexcept {
    assert(!handle.done());
    // returns && because yield guarantees that generator will not observe changes
    // and i want effective for(std::string s : generator)
    return static_cast<reference>(*handle.promise().current_result);
  }
  // * after invoking references to value from operator* are invalidated
  generator_iterator& operator++() {
    assert(!handle.done());
    handle.promise().produce_next();
    return *this;
  }
  void operator++(int) {
    ++(*this);
  }
};

// * produces first value when created
// * recusrive (co_yield dd::elements_of(rng))
// * default constructed generator is an empty range
// * suspend which is not co_yield may produce undefined behavior,
//   this means co_await expression must never suspend generator
template <typename Yield>
struct generator {
  using promise_type = generator_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;
  using iterator = generator_iterator<Yield>;

 private:
  handle_type handle = nullptr;

 public:
  // postcondition: empty(), 'for' loop produces 0 values
  constexpr generator() noexcept = default;
  // precondition: 'handle' != nullptr
  constexpr generator(handle_type handle) noexcept : handle(handle) {
    assume_not_null(handle);
  }
  // postcondition: other.empty()
  constexpr generator(generator&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {
  }
  constexpr generator& operator=(generator&& other) noexcept {
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
  constexpr ~generator() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return !handle || handle.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }
  // TODO hmm, я повторяю одно и то же число в начале при реюзе
  // * if .empty(), then begin() == end()
  iterator begin() const noexcept [[clang::lifetimebound]] {
    if (!handle) [[unlikely]]
      return iterator{};
    return iterator(handle);
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
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
