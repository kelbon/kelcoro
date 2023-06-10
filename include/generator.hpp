#pragma once

#include <coroutine>
#include <iterator>
#include <memory>
#include <utility>

#include "common.hpp"

namespace dd {

struct input_and_output_iterator_tag : std::input_iterator_tag, std::output_iterator_tag {};

template <typename Yield, typename Alloc>
struct generator_promise : memory_block<Alloc> {
  Yield* current_result = nullptr;

  // value type required, so no allocator traits(support memory resources too!)
  static_assert(std::is_same_v<std::byte, typename Alloc::value_type>);

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  static constexpr std::suspend_always final_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<generator_promise>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }

  auto await_transform(get_handle_t) const noexcept {
    return return_handle_t<generator_promise>{};
  }
  template <typename T>
  decltype(auto) await_transform(T&& v) const noexcept {
    return build_awaiter(std::forward<T>(v));
  }
  // yield things
 private:
  struct save_value_before_resume_t {
    Yield saved_value;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<generator_promise> handle) noexcept {
      handle.promise().current_result = std::addressof(saved_value);
    }
    void await_resume() const noexcept {
    }
  };

 public:
  // lvalue
  std::suspend_always yield_value(Yield& lvalue) noexcept {
    current_result = std::addressof(lvalue);
    return {};
  }
  // rvalue or some type which is convertible to Yield
  template <typename U>
  auto yield_value(U&& value) noexcept(std::is_nothrow_constructible_v<Yield, U&&>) {
    return save_value_before_resume_t{Yield(std::forward<U>(value))};
  }

  [[noreturn]] void unhandled_exception() const {
    throw;
  }
};

// synchronous producer
template <typename Yield, typename Alloc = std::allocator<std::byte>>
struct generator {
 public:
  using value_type = Yield;
  using promise_type = generator_promise<Yield, Alloc>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_;

 public:
  constexpr generator() noexcept = default;
  constexpr generator(handle_type handle) : handle_(handle) {
  }
  constexpr generator(generator&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }
  constexpr generator& operator=(generator&& other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
  }

  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(handle_, nullptr);
  }

  ~generator() {
    if (handle_)
      handle_.destroy();
  }

  struct iterator {
    handle_type owner;

    using iterator_category = input_and_output_iterator_tag;
    using value_type = Yield;
    using difference_type = ptrdiff_t;  // requirement of concept input_iterator

    bool operator==(std::default_sentinel_t) const noexcept {
      return owner.done();
    }
    value_type& operator*() const noexcept {
      return *owner.promise().current_result;
    }
    iterator& operator++() {
      owner.resume();
      return *this;
    }
    // postfix version impossible and logically incorrect for input iterator,
    // but it is required for concept of input iterator
    iterator operator++(int) {
      return ++(*this);
    }
  };

  iterator begin() {
    assert(!handle_.done());
    handle_.resume();
    return iterator{handle_};
  }

  static std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  // no range-for-loop access
  bool has_next() const noexcept {
    return !handle_.done();
  }
  [[nodiscard]] value_type& next() noexcept {
    assert(has_next());
    handle_.promise.resume();
    return *handle_.promise().current_result;
  }
};

template <std::forward_iterator It, std::sentinel_for<It> Sent>
generator<typename std::iterator_traits<It>::value_type> circular_view(It it, Sent sent) {
  while (true) {
    for (auto it_ = it; it_ != sent; ++it_)
      co_yield *it_;
  }
}
// clang-format off
  template <std::ranges::forward_range Rng>
  requires(std::ranges::borrowed_range<Rng>)
  generator<std::ranges::range_value_t<Rng>> circular_view(Rng&& r) {
  // clang-format on
  const auto it = std::ranges::begin(r);
  const auto sent = std::ranges::end(r);
  while (true) {
    for (auto it_ = it; it_ != sent; ++it_)
      co_yield *it_;
  }
}
template <typename First, std::same_as<First>... Ts>
generator<First> fixed_circular_view(First& f, Ts&... args) {
  while (true) {
    co_yield f;
    (co_yield args, ...);
  }
}
// TODO ? consumer

}  // namespace dd