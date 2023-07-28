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

template <typename Yield>
struct generator_promise_base {
  // invariant: root != nullptr
  generator_promise_base* root = this;
  // invariant: never nullptr, initialized when generator created
  Yield* current_result;
  std::coroutine_handle<> current_worker = get_return_object();
  std::coroutine_handle<> owner = std::noop_coroutine();

  generator_promise_base() = default;

  generator_promise_base(generator_promise_base&&) = delete;
  void operator=(generator_promise_base&&) = delete;

  auto get_return_object() noexcept {
    return std::coroutine_handle<generator_promise_base>::from_promise(*this);
  }
  std::suspend_always yield_value(Yield& lvalue) noexcept {
    root->current_result = std::addressof(lvalue);
    return {};
  }
  std::suspend_always yield_value(Yield&& rvalue) noexcept {
    root->current_result = std::addressof(rvalue);
    return {};
  }
  transfer_control_to yield_value(elements_of_t<Yield>&&);

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  transfer_control_to final_suspend() const noexcept {
    assert(root && !root->current_worker.done());
    root->current_worker = owner;// TODO in awaiter?
    // TODO почему то не считается .done()  после final suspend
    return transfer_control_to{owner};  // noop coro if done
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] void unhandled_exception() const {
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
// TODO
// template <typename Yield, typename Alloc>
// struct generator_promise : generator_promise_base<Yield>, memory_block<Alloc> {
//  static_assert(std::is_same_v<std::byte, typename Alloc::value_type>);
//};

template <typename Yield>
struct giterator {
 private:
  std::coroutine_handle<> handle;
  generator_promise_base<Yield>* promise = nullptr;

 public:
  giterator() noexcept;
  giterator(generator_promise_base<Yield>* p) noexcept;

  using iterator_category = std::input_iterator_tag;
  using value_type = Yield;
  using difference_type = ptrdiff_t;

  bool operator==(std::default_sentinel_t) const noexcept {
    return handle.done();
  }
  value_type& operator*() const noexcept {
    return *promise->current_result;
  }
  giterator& operator++() {
    assert(!promise->done());

    promise->produce_next();
    return *this;
  }
  // TODO void
  void operator++(int) {
    ++(*this);
  }
};

template <typename Yield>
struct generator {
 private:
  generator_promise_base<Yield>* promise = nullptr;

  template <typename Y>
  friend struct empty_generator_handle_t;
  template <typename Y>
  friend struct generator_promise_base;

 public:
  static_assert(!std::is_reference_v<Yield>);
  using value_type = Yield;
  // TODO coroutine traits specialization using promise_type = generator_promise_base<Yield>;
  using promise_type = generator_promise_base<Yield>;
  constexpr generator() noexcept = default;
  constexpr generator(std::coroutine_handle<generator_promise_base<Yield>> handle) noexcept
      : promise(std::addressof(handle.promise())) {
  }
  constexpr generator(generator&& other) noexcept : promise(std::exchange(other.promise, nullptr)) {
  }
  constexpr generator& operator=(generator&& other) noexcept {
    std::swap(promise, other.promise);
    return *this;
  }

  constexpr ~generator() {
    if (promise)
      promise->get_return_object().destroy();
  }

  giterator<Yield> begin() const noexcept {
    return giterator(promise);
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  bool empty() const noexcept {
    return !promise || promise->done();
  }
};

template <typename Yield>
struct empty_generator_handle_t {
  static auto value() noexcept {
    // TODO custom alloc
    static generator<Yield> g = []() -> generator<Yield> { co_return; }();

    return g.promise->get_return_object();
  }
};

template <typename Yield>
std::coroutine_handle<generator_promise_base<Yield>> empty_generator_handle() {
  return empty_generator_handle_t<Yield>::value();
}

template <typename Yield>
transfer_control_to generator_promise_base<Yield>::yield_value(elements_of_t<Yield>&& e) {
  if (e.g.empty()) {
    // continue execution while not yields a real value
    return transfer_control_to{get_return_object()};
  }
  auto& p = *e.g.promise;
  std::coroutine_handle h = e.g.promise->get_return_object();
  p.root = root;
  p.owner = get_return_object();
  root->current_worker = h;
  root->current_result = p.current_result;  // first value was created by e.g itself
  return transfer_control_to{std::noop_coroutine()};
}

template <typename Yield>
giterator<Yield>::giterator() noexcept : handle(empty_generator_handle<Yield>()) {
}
template <typename Yield>
giterator<Yield>::giterator(generator_promise_base<Yield>* p) noexcept
    : handle(p ? p->get_return_object() : empty_generator_handle<Yield>()), promise(p) {
}

template <typename Yield>
struct elements_of_t {
  generator<Yield> g;
};
template <typename T>
elements_of_t(generator<T>) -> elements_of_t<T>;

template <std::ranges::range R>
[[nodiscard]] auto elements_of(R&& r) {
  auto g = [&]() -> generator<std::ranges::range_value_t<R>> {
    for (auto&& x : r)
      co_yield x;
  }();

  return elements_of_t{g};
}
// TODO &&g
template <typename Yield>
[[nodiscard]] constexpr elements_of_t<Yield> elements_of(generator<Yield> g) noexcept {
  return elements_of_t{std::move(g)};
}

}  // namespace dd

#if __clang__
#pragma clang diagnostic pop
#endif