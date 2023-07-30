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
// TODO rename everything (_base / giterator)
template <typename>
struct elements_of_t;

template <typename Yield>
struct generator_promise {
  // invariant: root != nullptr
  generator_promise* root = this;
  // invariant: never nullptr, initialized when generator created
  // TODO hmm about const here
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

// TODO эта штука должна не зависеть от Yield
template <typename Yield>
struct giterator {
  // but.. why?
  // Its required for guarantee that default constructed generator is an empty range
  // so i store erased handle when iterator is default constructed and 'handle' otherwise
  // P.S. i know about formal UB here
  union {
    // invariants: always != nullptr, if !erased_handle.done(), then 'handle' stored
    std::coroutine_handle<> erased_handle = always_done_coroutine();
    std::coroutine_handle<generator_promise<Yield>> handle;
  };

  using iterator_category = std::input_iterator_tag;
  using value_type = Yield;
  using difference_type = ptrdiff_t;

  bool operator==(std::default_sentinel_t) const noexcept {
    assert(handle != nullptr); // invariant
    return erased_handle.done();
  }
  // TODO rvalue reference here...
  value_type& operator*() const noexcept {
    assert(!handle.done());
    return *handle.promise().current_result;
  }
  giterator& operator++() {
    assert(!handle.done());
    handle.promise().produce_next();
    return *this;
  }
  void operator++(int) {
    ++(*this);
  }
};
// TODO support для случая, когда yield string(...), ты же хочешь вымувать это значение наружу
// а вместо этого происходит копирование получается
// TODO нахуй эти референс дерьмо, реально и без них отлично всё будет работать, если поддержать вымув( с ними непонятно как)
// Хмм, TODO всегда мувать, но тогда для lvalue нужно будет копировать... Лучше сделать non const
// как у меня и есть, а для констант создавать их на месте(чекнуть что они не создаются сразу... через &&)
// TODO? iter_move / iter_swap?
// TODO!!!!!!!!! begin_to(out iterator returning pointers to uninitialized memorys) с RVO сразу (может быть output итератор...?)
// TODO reference type?
// можно чет типа emplace_yield... но куда..
// TODO promise must work for Reference
// TODO попробовать тупо аллокатор байтами сложить в конец, потом всё равно нужно будет его достать перед удалением памяти
// и можно избежать проблем с алигментом таким образом всех
// но для этого нужно потребовать trivially copyable аллокатор, но это не проблема, если он тупо указатель на ресурс
// НО! Можно просто и требовать собственно... Указатель на std::memory resource... Типа а почему нет?
// Либо stateless аллокатор, либо std::memory_resource* !
// + какой то тег в котором его нужно передавать, типа  TODO я хочу чтобы это было как то снаружи, чтобы не пихать
// в интерфейс корутины это
// ХММ, а что если сделать thread local std::memory_resource*... И передавать туда...
// А потом функцию dd::co_alloc(&resource, &co_fn)
// !!! TODO !!! вся логика будет ВСЕГДА thread_local_mr.allocate(c, align); store_ptr_after_frame; tread_local_mr = new_delete()
// а деаллокация достаёт поинтер за фреймом через memcpy и вызывает функцию.
// TODO надо попробовать каким то образом воспользоваться тем фактом, что аллокация всегда ровно одна, например сохранять
// не memory_resource*, а конкретный поинтер или чёт такое
template <typename Yield>
struct generator {
  using iterator = giterator<Yield>;
  using handle_type = std::coroutine_handle<generator_promise<Yield>>;
 private:
  handle_type handle = nullptr;

// TODO less friends
  template <typename>
  friend struct generator_promise;
  template<typename>
  friend struct elements_of_t;
  template<typename>
  friend struct attach_leaf;
 public:
  static_assert(!std::is_reference_v<Yield>);
  // TODO check ranges value_t?
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

  giterator<Yield> begin() const noexcept {
    if (!handle) [[unlikely]]
      return iterator{};
    return iterator{handle};
  }
  static constexpr std::default_sentinel_t end() noexcept {
    return std::default_sentinel;
  }

  bool empty() const noexcept {
    return !handle || handle.done();
  }
};

// TODO noexport
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

// TODO hmm, а важно ли тут что конкретно бросается из генератора...
template <typename Yield>
struct elements_of_t {
  generator<Yield> g;
};
template <typename T>
elements_of_t(generator<T>) -> elements_of_t<T>;

template <std::ranges::range R>
[[nodiscard]] auto elements_of(R&& r) {
  auto g = [&]() -> generator<std::ranges::range_value_t<R>> { // hmm, reference_t?..
    for (auto&& x : r)
      co_yield x;
  }();
  return elements_of_t{g};
}

template <typename Yield>
[[nodiscard]] constexpr elements_of_t<Yield> elements_of(generator<Yield> g) noexcept {
  return elements_of_t{std::move(g)};
}

}  // namespace dd

#if __clang__
#pragma clang diagnostic pop
#endif
