#pragma once

#include <iterator>
#include <memory>

#include "common.hpp"
#include "noexport/generators_common.hpp"

#ifdef __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wunknown-attributes"
#endif
#ifdef __GNUC__
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wattributes"
#endif

namespace dd {

// generator with this type is optimized
struct nothing_t {
  nothing_t() = default;
  constexpr nothing_t(std::nullptr_t) noexcept {
  }
};

template <typename Yield>
using pointer_to_yielded_t =
    std::conditional_t<std::is_same_v<nothing_t, Yield>, nothing_t, std::add_pointer_t<Yield>>;

template <typename CRTP, yieldable Yield>
struct yield_block {
  std::suspend_always yield_value(Yield&& rvalue) noexcept
    requires(!std::is_reference_v<Yield> && choose_me_if_ambiguous<Yield>)
  {
    static_cast<CRTP&>(*this).set_result(std::addressof(rvalue));
    return {};
  }
  std::suspend_always yield_value(Yield& lvalue) noexcept
    requires(std::is_reference_v<Yield>)
  {
    return yield_value(by_ref{lvalue});
  }
  noexport::hold_value_until_resume<Yield> yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>)
    requires(!std::is_reference_v<Yield>)
  {
    return noexport::hold_value_until_resume<Yield>{Yield(clvalue)};
  }
  template <typename U>
  std::suspend_always yield_value(by_ref<U> r) noexcept {
    static_cast<CRTP&>(*this).set_result(std::addressof(r.value));
    return {};
  }
};

template <typename CRTP>
struct yield_block<CRTP, nothing_t> {
  static std::suspend_always yield_value(nothing_t = {}) {
    return {};
  }
};

template <yieldable Yield>
struct generator_promise : not_movable, yield_block<generator_promise<Yield>, Yield> {
  using handle_type = std::coroutine_handle<generator_promise>;

  // invariant: root != nullptr
  generator_promise* root = this;
  handle_type current_worker = self_handle();
  union {
    generator<Yield>* _consumer;  // setted only in root
    handle_type _owner;           // setted only in leafs
  };

  handle_type owner() const noexcept {
    KELCORO_ASSUME(root != this);
    return _owner;
  }
  void set_result(pointer_to_yielded_t<Yield> p) const noexcept {
    if constexpr (!std::is_same_v<nothing_t, Yield>) {
      root->_consumer->current_result = p;
    }
  }
  KELCORO_PURE handle_type self_handle() noexcept {
    return handle_type::from_promise(*this);
  }
  void skip_this_leaf() const noexcept {
    KELCORO_ASSUME(root != this);
    generator_promise& owner_p = _owner.promise();
    KELCORO_ASSUME(&owner_p != this);
    owner_p.root = root;
    root->current_worker = _owner;
  }

 public:
  constexpr generator_promise() noexcept {
  }

  generator<Yield> get_return_object() noexcept {
    return generator<Yield>(self_handle());
  }
  KELCORO_DEFAULT_AWAIT_TRANSFORM;
  auto await_transform(this_coro::get_handle_t) noexcept {
    return this_coro::get_handle_t::awaiter<generator_promise>{};
  }

  using yield_block<generator_promise, Yield>::yield_value;

  template <typename R>
  noexport::attach_leaf<generator<Yield>> yield_value(elements_of<R> e) noexcept {
    return noexport::create_and_attach_leaf<Yield, generator>(std::move(e));
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }

  struct final_awaiter {
    const generator_promise& p;
    static constexpr bool await_ready() noexcept {
      return false;
    }
    static constexpr void await_resume() noexcept {
    }
    KELCORO_ASSUME_NOONE_SEES constexpr std::coroutine_handle<> await_suspend(
        std::coroutine_handle<>) const noexcept {
      if (p.root != &p) {
        p.skip_this_leaf();
        return p.owner();
      }
      p.set_result(nullptr);
      return std::noop_coroutine();
    }
  };

  final_awaiter final_suspend() const noexcept {
    return final_awaiter{*this};
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] void unhandled_exception() {
    if (root != this)
      skip_this_leaf();
    set_result(nullptr);
    throw;
  }
};

// no default ctor, because its input iterator
template <yieldable Yield>
struct generator_iterator {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  generator<Yield>* self;

 public:
  // do not resumes 'g'
  constexpr explicit generator_iterator(generator<Yield>& g KELCORO_LIFETIMEBOUND) noexcept
      : self(std::addressof(g)) {
  }

  using iterator_category = std::input_iterator_tag;
  using value_type = std::decay_t<Yield>;
  using reference = std::conditional_t<std::is_same_v<nothing_t, Yield>, Yield, Yield&&>;
  using difference_type = ptrdiff_t;

  // return true if they are attached to same 'generator' object
  constexpr bool equivalent(const generator_iterator& other) const noexcept {
    return self == other.self;
  }
  generator<Yield>& owner() const noexcept {
    return *self;
  }

  constexpr bool operator==(std::default_sentinel_t) const noexcept {
    if constexpr (!std::is_same_v<nothing_t, Yield>) {
      return self->current_result == nullptr;
    } else {
      return self->top.done();
    }
  }
  constexpr reference operator*() const noexcept {
    KELCORO_ASSUME(*this != std::default_sentinel);
    if constexpr (!std::is_same_v<nothing_t, Yield>) {
      return static_cast<reference>(*self->current_result);
    } else {
      return reference{};
    }
  }
  constexpr std::add_pointer_t<reference> operator->() const noexcept
    requires(!std::is_same_v<nothing_t, Yield>)
  {
    auto&& ref = operator*();
    return std::addressof(ref);
  }

  // * after invoking references to value from operator* are invalidated
  generator_iterator& operator++() KELCORO_LIFETIMEBOUND {
    KELCORO_ASSUME(!self->empty());
    const auto* const self_before = self;
    const auto* const top_address_before = self->top.address();
    self->top.promise().current_worker.resume();
    KELCORO_ASSUME(self_before == self);
    const auto* const top_address_after = self->top.address();
    KELCORO_ASSUME(top_address_before == top_address_after);
    return *this;
  }
  void operator++(int) {
    ++(*this);
  }
  // converts to iterator which can be used as output iterator
  auto out() const&& noexcept;
};

template <yieldable Yield>
struct generator_output_iterator : generator_iterator<Yield> {
  using base_t = generator_iterator<Yield>;
  constexpr Yield& operator*() const noexcept {
    static_assert(!std::is_same_v<nothing_t, Yield>);
    static_assert(std::is_reference_v<decltype(base_t::operator*())>);
    Yield&& i = base_t::operator*();
    // avoid C++23 automove in return expression
    Yield& j = i;
    return j;
  }
  constexpr generator_output_iterator& operator++() {
    base_t::operator++();
    return *this;
  }
  constexpr generator_output_iterator& operator++(int) {
    base_t::operator++();
    return *this;
  }
};

template <yieldable Yield>
auto generator_iterator<Yield>::out() const&& noexcept {
  return generator_output_iterator<Yield>{*this};
}
// * produces first value when .begin called
// * recursive (co_yield dd::elements_of(rng))
// * default constructed generator is an empty range
// notes:
//  * generator ignores fact, that 'destroy' may throw exception from destructor of object in coroutine, it
//  will lead to std::terminate
//  * if exception was thrown from recursivelly co_yielded generator, then this leaf just skipped and caller
//  can continue iterating after catch(requires new .begin call)
//  * its caller responsibility to not use co_await with real suspend in generator
// (or you must know what you do)
template <yieldable Yield>
struct generator : enable_resource_deduction {
  using promise_type = generator_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = std::decay_t<Yield>;
  using iterator = generator_iterator<Yield>;

 private:
  friend generator_iterator<Yield>;
  friend generator_promise<Yield>;
  friend noexport::attach_leaf<generator>;

  // invariant: == nullptr when top.done()
  KELCORO_NO_UNIQUE_ADDRESS pointer_to_yielded_t<Yield> current_result = nullptr;
  handle_type top = nullptr;

  // precondition: 'handle' != nullptr, handle does not have other owners
  // used from promise::get_return_object
  constexpr explicit generator(handle_type top) noexcept : top(top) {
    prepare_to_start();
  }

  void prepare_to_start() noexcept {
    assert(raw_handle() != nullptr);
    if constexpr (!std::is_same_v<nothing_t, Yield>)
      top.promise()._consumer = this;
  }

 public:
  // postcondition: empty(), 'for' loop produces 0 values
  constexpr generator() noexcept = default;

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
    if (top)
      prepare_to_start();
    if (other.top)
      other.prepare_to_start();
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
  [[nodiscard]] constexpr handle_type raw_handle() const noexcept {
    return top;
  }

  // postcondition: .empty()
  constexpr void clear() noexcept {
    if (top) {
      top.destroy();
      top = nullptr;
    }
  }
  constexpr ~generator() {
    clear();
  }

  // observers

  KELCORO_PURE constexpr bool empty() const noexcept {
    return !top || top.done();
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }

  bool operator==(const generator& other) const noexcept {
    if (this == &other)  // invariant: coro handle has only one owner
      return true;
    return empty() && other.empty();
  }

  // * if .empty(), then begin() == end()
  // * produces next value(often first)
  // iterator invalidated only when generator dies
  iterator begin() KELCORO_LIFETIMEBOUND {
    iterator it(*this);
    if (!empty()) [[likely]]
      ++it;
    return it;
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  // precondition: !raw_handle() != nullptr &&  .begin() or .prepare_to_start() was called
  // if generator is not started, its 'before_begin' iterator, which may not be dereferenced
  iterator cur_iterator() noexcept {
    assert(raw_handle() != nullptr);
    return iterator(*this);
  }
};

template <yieldable Y, memory_resource R>
using generator_r = resourced<generator<Y>, R>;

namespace pmr {

template <yieldable Y>
using generator = ::dd::generator_r<Y, polymorphic_resource>;

}

}  // namespace dd

#ifdef __clang__
  #pragma clang diagnostic pop
#endif
#ifdef __GNUC__
  #pragma GCC diagnostic pop
#endif
