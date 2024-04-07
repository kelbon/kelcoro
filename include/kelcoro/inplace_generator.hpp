#pragma once

#include "noexport/generators_common.hpp"

namespace dd {

template <yieldable Yield>
struct inplace_generator_promise;

template <typename Yield>
struct hold_value_until_resume {
  Yield value;
  static constexpr bool await_ready() noexcept {
    return false;
  }
  KELCORO_ASSUME_NOONE_SEES void await_suspend(
      std::coroutine_handle<inplace_generator_promise<Yield>> handle) {
    handle.promise().set_result(std::addressof(value));
  }
  static constexpr void await_resume() noexcept {
  }
};

template <yieldable Yield>
struct inplace_generator_promise {
  using handle_type = std::coroutine_handle<inplace_generator_promise>;

  std::add_pointer_t<Yield> current_result;  // nitialzed by ++it

  void set_result(std::add_pointer_t<Yield> p) noexcept {
    current_result = p;
  }

  handle_type get_return_object() noexcept {
    return handle_type::from_promise(*this);
  }
  // there are no correct things which you can do with co_await in inplace_generator
  void await_transform(auto&&) = delete;

  std::suspend_always yield_value(Yield&& rvalue) noexcept
    requires(!std::is_reference_v<Yield> && choose_me_if_ambiguous<Yield>)
  {
    set_result(std::addressof(rvalue));
    return {};
  }
  std::suspend_always yield_value(Yield& lvalue) noexcept
    requires(std::is_reference_v<Yield>)
  {
    return yield_value(by_ref{lvalue});
  }

  hold_value_until_resume<Yield> yield_value(const Yield& clvalue) noexcept(
      std::is_nothrow_copy_constructible_v<Yield>)
    requires(!std::is_reference_v<Yield>)
  {
    return hold_value_until_resume<Yield>{Yield(clvalue)};
  }
  template <typename U>
  std::suspend_always yield_value(by_ref<U> r) noexcept {
    set_result(std::addressof(r.value));
    return {};
  }

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }

  std::suspend_always final_suspend() const noexcept {
    return {};
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] static void unhandled_exception() {
    throw;
  }
};

// no default ctor, because its input iterator
template <yieldable Yield>
struct inplace_generator_iterator {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  inplace_generator_promise<Yield>* self;

 public:
  // do not resumes 'g'
  constexpr explicit inplace_generator_iterator(
      inplace_generator_promise<Yield>& p KELCORO_LIFETIMEBOUND) noexcept
      : self(&p) {
  }

  using iterator_category = std::input_iterator_tag;
  using value_type = std::decay_t<Yield>;
  using reference = Yield&&;
  using difference_type = ptrdiff_t;

  constexpr bool operator==(std::default_sentinel_t) const noexcept {
    return std::coroutine_handle<inplace_generator_promise<Yield>>::from_promise(*self).done();
  }

  constexpr reference operator*() const noexcept {
    return static_cast<reference>(*(operator->()));
  }
  constexpr std::add_pointer_t<reference> operator->() const noexcept {
    KELCORO_ASSUME(*this != std::default_sentinel);
    return self->current_result;
  }

  // * after invoking references to value from operator* are invalidated
  inplace_generator_iterator& operator++() KELCORO_LIFETIMEBOUND {
    KELCORO_ASSUME(*this != std::default_sentinel);
    const auto* const self_before = self;
    std::coroutine_handle<inplace_generator_promise<Yield>>::from_promise(*self).resume();
    KELCORO_ASSUME(self_before == self);
    return *this;
  }
  void operator++(int) {
    ++(*this);
  }
};

template <yieldable Yield>
struct nonowner_inplace_generator_t {
  using promise_type = inplace_generator_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;

  handle_type handle;

  nonowner_inplace_generator_t(std::coroutine_handle<promise_type> handle) noexcept : handle(handle) {
  }
};

// optimized non recursive version of generator
// * calculates first value when generator created
// * does not support memory resources, because should never be allocated
template <yieldable Yield>
struct inplace_generator {
  using promise_type = inplace_generator_promise<Yield>;
  using handle_type = std::coroutine_handle<promise_type>;
  using value_type = std::decay_t<Yield>;
  using iterator = inplace_generator_iterator<Yield>;

 private:
  // invariant: != nullptr
  handle_type top;

  static nonowner_inplace_generator_t<Yield> empty_generator() {
    co_return;
  }

 public:
  // should be used only from get_return_object
  constexpr inplace_generator(handle_type handle) noexcept : top(handle) {
  }
  constexpr inplace_generator(inplace_generator&& other) noexcept : top(other.top) {
    // assume not allocates (noexcept)
    other.top = empty_generator().handle;
  }
  constexpr inplace_generator& operator=(inplace_generator&& other) noexcept {
    swap(other);
    return *this;
  }

  // iterators to 'other' and 'this' are swapped too
  constexpr void swap(inplace_generator& other) noexcept {
    std::swap(top, other.top);
  }
  friend constexpr void swap(inplace_generator& a, inplace_generator& b) noexcept {
    a.swap(b);
  }

  [[nodiscard]] bool empty() const noexcept {
    return top.done();
  }
  explicit operator bool() const noexcept {
    return !empty();
  }
  constexpr ~inplace_generator() {
    top.destroy();
  }

  [[nodiscard]] iterator begin() const noexcept {
    return iterator{top.promise()};
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }
};

}  // namespace dd
