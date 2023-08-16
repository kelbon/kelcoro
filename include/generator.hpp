#pragma once

#include <iterator>
#include <memory>
#include <utility>

#include "common.hpp"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

namespace dd {

template <yieldable Yield>
struct generator_promise : not_movable {
  using handle_type = std::coroutine_handle<generator_promise>;

 private:
  friend generator<Yield>;
  friend generator_iterator<Yield>;
  friend noexport::attach_leaf<generator<Yield>>;
  friend noexport::hold_value_until_resume<Yield>;

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
  [[gnu::pure]] handle_type self_handle() noexcept {
    return handle_type::from_promise(*this);
  }

 public:
  constexpr generator_promise() noexcept {
  }

  generator<Yield> get_return_object() noexcept {
    return generator<Yield>(self_handle());
  }
  // there are no correct things which you can do with co_await in generator
  void await_transform(auto&&) = delete;
  auto await_transform(get_handle_t) noexcept {
    return this_coro::handle.operator co_await();
  }
  std::suspend_always yield_value(Yield&& rvalue) noexcept {
    set_result(std::addressof(rvalue));
    return {};
  }
  noexport::hold_value_until_resume<Yield> yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>) {
    return noexport::hold_value_until_resume<Yield>{Yield(clvalue)};
  }
  std::suspend_always yield_value(by_ref<Yield> r) noexcept {
    set_result(std::addressof(r.value));
    return {};
  }
  template <typename R>
  noexport::attach_leaf<generator<Yield>> yield_value(elements_of<R> e) noexcept {
    return noexport::create_and_attach_leaf<Yield, generator>(std::move(e));
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
  [[noreturn]] void unhandled_exception() {
    if (root != this)
      (void)final_suspend();  // skip this leaf
    set_result(nullptr);
    throw;
  }
};

// no default ctor, because its input iterator
// behaves also as generator_view (has its own begin/end)
template <yieldable Yield>
struct generator_iterator {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  generator<Yield>* self;

 public:
  constexpr explicit generator_iterator(generator<Yield>& g) noexcept : self(std::addressof(g)) {
  }

  using iterator_category = std::input_iterator_tag;
  using value_type = Yield;
  using reference = Yield&&;
  using difference_type = ptrdiff_t;

  // return true if they are attached to same 'channel' object
  constexpr bool equivalent(const generator_iterator& other) const noexcept {
    return self == other.self;
  }
  generator<Yield>& owner() const noexcept {
    return *self;
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
  generator_iterator& operator++() [[clang::lifetimebound]] {
    assert(!self->empty());
    self->top.promise().current_worker.resume();
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
//
// about R - see 'with_resource'
template <yieldable Yield, memory_resource R /* = select_from_signature*/>
struct generator : enable_resource_support<R> {
  using promise_type = generator_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = Yield;
  using iterator = generator_iterator<Yield>;

 private:
  friend generator_iterator<Yield>;
  friend generator_promise<Yield>;
  friend noexport::attach_leaf<generator>;

  // invariant: == nullptr when top.done()
  Yield* current_result = nullptr;
  handle_type top = nullptr;

  // precondition: 'handle' != nullptr, handle does not have other owners
  // used from promise::gen_return_object
  constexpr explicit generator(handle_type top) noexcept : top(top) {
  }

 public:
  // postcondition: empty(), 'for' loop produces 0 values
  constexpr generator() noexcept = default;

  // postconditions:
  // * other.empty()
  // * iterators to 'other' == end()
  constexpr generator(generator&& other) noexcept {
    swap(other);
  }
  constexpr generator& operator=(generator&& other) noexcept {
    swap(other);
    return *this;
  }

  // iterators to 'other' and 'this' are swapped too
  constexpr void swap(generator& other) noexcept {
    std::swap(current_result, other.current_result);
    std::swap(top, other.top);
  }
  friend constexpr void swap(generator& a, generator& b) noexcept {
    a.swap(b);
  }

  constexpr void reset(handle_type handle) noexcept {
    clear();
    top = handle;
  }

  // postcondition: .empty()
  // its caller responsibility to correctly destroy handle
  [[nodiscard]] constexpr handle_type release() noexcept {
    return std::exchange(top, nullptr);
  }
  // postcondition: .empty()
  constexpr void clear() noexcept {
    if (top)
      std::exchange(top, nullptr).destroy();
  }
  constexpr ~generator() {
    clear();
  }

  // observers

  constexpr bool empty() const noexcept {
    return !top || top.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  bool operator==(const generator& other) const noexcept {
    if (empty())
      return other.empty();
    return this == &other;  // invariant: coro handle has only one owner
  }

  // * if .empty(), then begin() == end()
  // * produces next value(often first)
  // iterator invalidated only when generator dies
  iterator begin() & [[clang::lifetimebound]] {
    if (!empty()) [[likely]] {
      top.promise()._current_result_ptr = &current_result;
      top.promise().current_worker.resume();
    }
    return iterator{*this};
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }
};

namespace pmr {

template <yieldable Y>
using generator = ::dd::generator<Y, polymorphic_resource>;

}

}  // namespace dd

#ifdef __clang__
#pragma clang diagnostic pop
#endif
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
