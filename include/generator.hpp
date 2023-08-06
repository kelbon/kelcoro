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
// TODO generator must be consumer
template <typename>
struct generator;

// TODO usage .begin as output iterator hmm что то типа .out хммм

template <typename Yield>
struct generator_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<generator_promise>;

  // invariant: root != nullptr
  generator_promise* root = this;
  // invariant: never nullptr, initialized in first co_yield
  Yield* current_result = nullptr;
  std::coroutine_handle<> current_worker = get_return_object();
  // nullptr means top-level
  handle_type owner = nullptr;

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
    std::coroutine_handle<> leaf;

    bool await_ready() const noexcept {
      assume_not_null(leaf);
      return leaf.done();
    }

    std::coroutine_handle<> await_suspend(handle_type owner) const noexcept {
      generator_promise& leaf_p = handle_type::from_address(leaf.address()).promise();
      generator_promise& root = *owner.promise().root;
      leaf_p.root = &root;
      leaf_p.owner = owner;
      root.current_worker = leaf_p.current_worker;
      return root.current_worker;
    }
    static constexpr void await_resume() noexcept {
    }
    ~attach_leaf() {
      leaf.destroy();
    }
  };

 public:
  std::suspend_always yield_value(Yield&& rvalue) noexcept {
    root->current_result = std::addressof(rvalue);
    return {};
  }
  std::suspend_always yield_value(by_ref<Yield> r) noexcept {
    root->current_result = std::addressof(r.value);
    return {};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return hold_value_until_resume{Yield(clvalue)};
  }
  // attaches leaf-generator
  template <typename R>
  attach_leaf yield_value(elements_of<R> e) noexcept {
    if constexpr (std::is_same_v<std::remove_cvref_t<R>, generator<Yield>>) {
      return attach_leaf{e.rng.release()};
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
      auto h = make_gen(e.rng).release();
      assume_not_null(h);
      return attach_leaf{h};
    }
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    root->current_worker = owner;
    root->current_result = nullptr;
    if (owner) {
      owner.promise().root = root;
      return transfer_control_to{owner};
    }
    return transfer_control_to{std::noop_coroutine()};
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] static void unhandled_exception() {
    throw;
  }

  // interface for iterator, used only on top-level generator
  void produce_next() {
    assume(root == this);
    assume(!get_return_object().done());
    current_worker.resume();
  }
};

template <typename Yield>
struct generator_iterator {
 private:
  using handle_type = std::coroutine_handle<generator_promise<Yield>>;
  // invariant: != nullptr
  std::coroutine_handle<> _handle = always_done_coroutine();

  handle_type handle() const noexcept {
    return handle_type::from_address(_handle.address());
  }

 public:
  generator_iterator() noexcept = default;
  // precondition: h != nullptr
  generator_iterator(std::coroutine_handle<> h) noexcept : _handle(h) {
    assume_not_null(h);
  }

  using iterator_category = std::input_iterator_tag;
  using value_type = Yield;
  // rvalue ref, but never produces const T&&
  using reference = std::conditional_t<std::is_const_v<Yield>, Yield&, Yield&&>;
  using difference_type = ptrdiff_t;

  bool operator==(std::default_sentinel_t) const noexcept {
    return _handle.done();
  }
  // * returns rvalue ref
  reference operator*() const noexcept {
    assert(*this != std::default_sentinel);
    return static_cast<reference>(*handle().promise().current_result);
  }
  // * after invoking references to value from operator* are invalidated
  generator_iterator& operator++() KELCORO_LIFETIMEBOUND {
    assert(*this != std::default_sentinel);
    handle().promise().produce_next();
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
  // invariant: != nullptr
  std::coroutine_handle<> handle = always_done_coroutine();

 public:
  // postcondition: empty(), 'for' loop produces 0 values
  constexpr generator() noexcept = default;
  // precondition: 'handle' != nullptr
  constexpr generator(handle_type handle) noexcept : handle(handle) {
    assume_not_null(handle);
  }
  // postcondition: other.empty()
  constexpr generator(generator&& other) noexcept : handle(other.release()) {
  }
  constexpr generator& operator=(generator&& other) noexcept {
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
  constexpr ~generator() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return handle.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  // * if .empty(), then begin() == end()
  // produces next value(often first)
  iterator begin() KELCORO_LIFETIMEBOUND {
    iterator result(handle);
    if (!empty()) [[likely]]
      ++result;
    return result;
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  Yield* next() noexcept {
    if (empty())
      return nullptr;
    auto& p = handle.promise();
    p.produce_next();
    return p.current_result;
  }
};

}  // namespace dd

#if __clang__
#pragma clang diagnostic pop
#endif
