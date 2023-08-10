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

template <typename>
struct generator;

// TODO usage .begin as output iterator hmm что то типа .out хммм

template <typename Yield>
struct generator_promise : enable_memory_resource_support {
  static_assert(!std::is_reference_v<Yield>);
  using handle_type = std::coroutine_handle<generator_promise>;

 private:
  friend generator<Yield>;
  // invariant: root != nullptr
  generator_promise* root = this;
  handle_type current_worker = self_handle();
  union {
    Yield** _current_result_ptr;  // setted only in root
    handle_type _owner;           // setted only in leafs
  };

  handle_type owner() const noexcept {
    assert(root != this);
    return _owner;
  }
  void set_result(Yield* v) const noexcept {
    *root->_current_result_ptr = v;
  }

 public:
  constexpr generator_promise() noexcept {
  }

  generator_promise(generator_promise&&) = delete;
  void operator=(generator_promise&&) = delete;

  [[gnu::pure]] handle_type self_handle() noexcept {
    return handle_type::from_promise(*this);
  }
  generator<Yield> get_return_object() noexcept {
    return generator(self_handle());
  }
  // there are no correct things which you can do with co_await
  // in generator
  void await_transform(auto&&) = delete;
  auto await_transform(get_handle_t) noexcept {
    return this_coro::handle.operator co_await();
  }

 private:
  // TODO reuse in channel
  struct hold_value_until_resume {
    Yield value;

    static constexpr bool await_ready() noexcept {
      return false;
    }
    void await_suspend(handle_type handle) noexcept {
      handle.promise().set_result(std::addressof(value));
    }
    static constexpr void await_resume() noexcept {
    }
  };

  struct attach_leaf {
    generator<Yield> leaf;

    bool await_ready() const noexcept {
      return leaf.empty();
    }

    std::coroutine_handle<> await_suspend(handle_type owner) const noexcept {
      assert(owner != leaf.top);
      generator_promise& leaf_p = leaf.top.promise();
      generator_promise& root_p = *owner.promise().root;
      leaf_p.current_worker.promise().root = &root_p;
      leaf_p._owner = owner;
      root_p.current_worker = leaf_p.current_worker;
      return leaf_p.current_worker;
    }
    static constexpr void await_resume() noexcept {
    }
  };

 public:
  std::suspend_always yield_value(Yield&& rvalue) noexcept {
    set_result(std::addressof(rvalue));
    return {};
  }
  hold_value_until_resume yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return hold_value_until_resume{Yield(clvalue)};
  }
  std::suspend_always yield_value(terminator_t) noexcept {
    set_result(nullptr);
    return {};
  }
  std::suspend_always yield_value(by_ref<Yield> r) noexcept {
    set_result(std::addressof(r.value));
    return {};
  }
  // attaches leaf-generator
  // TODO reuse somehow in channel, same logic, create X, pass to attach leaf
  template <typename R>
  attach_leaf yield_value(elements_of<R> e) noexcept {
    if constexpr (std::is_same_v<std::remove_cvref_t<R>, generator<Yield>>) {
      return attach_leaf{std::move(e.rng)};
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
      return attach_leaf{make_gen(e.rng)};
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
    return transfer_control_to{std::noop_coroutine()};
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] [[gnu::cold]] void unhandled_exception() {
    if (root != this)
      (void)final_suspend();  // skip this leaf
    set_result(nullptr);
    throw;
  }

  // interface for iterator, used only on top-level generator
  void produce_next() {
    assume(root == this);
    assume(!self_handle().done());
    current_worker.resume();
  }
};

// no default ctor, because its input iterator
template <typename Yield>
struct generator_iterator {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  generator<Yield>* self;

 public:
  constexpr explicit generator_iterator(generator<Yield>& g) noexcept : self(std::addressof(g)) {
  }

  using iterator_category = std::input_iterator_tag;
  using value_type = Yield;
  using reference = std::conditional_t<std::is_const_v<Yield>, Yield&, Yield&&>;
  using difference_type = ptrdiff_t;

  // return true if they are attached to same 'channel' object
  constexpr bool equivalent(const generator_iterator& other) const noexcept {
    return self == other.self;
  }

  constexpr bool operator==(std::default_sentinel_t) const noexcept {
    return self->current_result == nullptr;
  }
  // * returns rvalue ref
  constexpr reference operator*() const noexcept {
    assert(*this != std::default_sentinel);
    return static_cast<reference>(*self->current_result);
  }
  // * after invoking references to value from operator* are invalidated
  generator_iterator& operator++() KELCORO_LIFETIMEBOUND {
    assert(!self->empty());
    self->top.promise().produce_next();
    return *this;
  }
  void operator++(int) {
    ++(*this);
  }
};

// * produces first value when .begin called
// * recursive (co_yield dd::elements_of(rng))
// * default constructed generator is an empty range
// notes:
//  * generator ignores fact, that 'destroy' may throw exception from destructor of object in coroutine, it
//  will lead to std::terminate
//  * if exception was throwed from recursivelly co_yielded generator, then this leaf just skipped and caller
//  can continue iterating after catch(requires new .begin call)
template <typename Yield>
struct generator {
  using promise_type = generator_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;
  using iterator = generator_iterator<Yield>;

 private:
  friend generator_iterator<Yield>;
  friend generator_promise<Yield>;

  Yield* current_result = nullptr;
  coroutine_handle<promise_type> top = always_done_coroutine();

  // precondition: 'handle' != nullptr, handle does not have other owners
  // used from promise::gen_return_object
  constexpr explicit generator(handle_type top) noexcept : top(top) {
  }

 public:
  // postcondition: empty(), 'for' loop produces 0 values
  constexpr generator() noexcept = default;

  // postcondition: other.empty()
  constexpr generator(generator&& other) noexcept {
    swap(other);
  }
  constexpr generator& operator=(generator&& other) noexcept {
    swap(other);
    return *this;
  }

  constexpr void swap(generator& other) noexcept {
    std::swap(current_result, other.current_result);
    std::swap(top, other.top);
  }
  friend constexpr void swap(generator& a, generator& b) noexcept {
    a.swap(b);
  }

  constexpr void reset(handle_type handle) noexcept {
    clear();
    if (handle)
      top = handle;
  }

  // postcondition: .empty()
  // its caller responsibility to correctly destroy handle
  [[nodiscard]] constexpr handle_type release() noexcept {
    if (empty())
      return nullptr;
    return std::exchange(top, always_done_coroutine()).get();
  }
  // postcondition: .empty()
  constexpr void clear() noexcept {
    std::exchange(top, always_done_coroutine()).destroy();
  }
  constexpr ~generator() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return top.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  // * if .empty(), then begin() == end()
  // * produces next value(often first)
  iterator begin() KELCORO_LIFETIMEBOUND {
    if (!empty()) [[likely]] {
      top.promise()._current_result_ptr = &current_result;
      top.promise().produce_next();
    }
    return iterator{*this};
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }
};

}  // namespace dd

#if __clang__
#pragma clang diagnostic pop
#endif
