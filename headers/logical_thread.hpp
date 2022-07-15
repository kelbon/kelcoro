#pragma once

#include <system_error>
#include <optional>

#include "common.hpp"

namespace dd {

struct two_way_bound {
 private:
  std::atomic_bool flag = false;

 public:
  two_way_bound() = default;
  two_way_bound(two_way_bound&&) = delete;
  void operator=(two_way_bound&&) = delete;

  bool try_inform() noexcept {
    bool informed = !flag.exchange(true, std::memory_order::acq_rel);
    if (informed)
      flag.notify_one();
    return informed;
  }
  void wait() noexcept {
    flag.wait(false, std::memory_order::acquire);
  }
  void reset() noexcept {
    flag.store(false, std::memory_order::release);
  }
};

// TODO - нужно resetable stop source / token, без шаренных и прочего, чтобы продолжать корутину
// заодно решу давнюю проблему. Стейт то на корутине уже выделен. Память больше выделять не нужно будет

struct request_stop_token_t {};

constexpr inline request_stop_token_t request_stop_token = {};

// there are may be only one stop_token. If coroutine accepts stop_token and then args,
// then stop_token in first argument will be filled when coroutine created
struct stop_token {
 private:
  std::atomic_bool* state_;
  template <typename>
  friend struct logical_thread_promise;
  // TODO friend empty_stop_token_t/request_stop_token или чет такое
  constexpr explicit stop_token(std::atomic_bool* state) noexcept : state_{state} {
  }
  // non trivially copyable, so cant be created by bit_cast
  stop_token(const stop_token&);
  void operator=(const stop_token&);

 public:
  // used when logical_thread created and its first argument is stop_token
     // TODO bugreport consteval breaks all here
  /*consteval*/ stop_token(request_stop_token_t) noexcept : state_(nullptr) {
  }
  stop_token(stop_token&& other) noexcept  {
    state_ = std::exchange(other.state_, nullptr);
  }
  void operator=(stop_token&&) = delete;
  // request is always possible, because only way to get token is pass empty_stop_token_t into coroutine
  bool stop_requested() const noexcept {
    assert(state_ != nullptr);
    return state_->load(std::memory_order::acquire);
  }
};


struct stop_now_t {};

namespace this_coro {
// by co_await on this type logical_thread indicates, that it is stoped as requested(or before request)
constexpr inline stop_now_t stop_now = {};

}  // namespace this_coro

//The initialization and destruction of each parameter copy occurs in the context of the called coroutine
//Initializations of parameter copies are sequenced before the call to the coroutine promise
//        constructor and indeterminately sequenced with respect to each other
//The lifetime of parameter copies ends immediately after the lifetime of the coroutine promise object ends

    // TODO хочу ресет сделать, чтобы можно было остановить и продолжить!(из остановки мб токен продолжения
// получать такой чтобы получить упорядоченность и не более одного всегда)
template<typename Alloc>
struct logical_thread_promise : memory_block<Alloc> {
  std::atomic_bool stop_requested_ = false;
  two_way_bound stopped;
  // token надо передавать в аргумент корутине, тогда он будет всегда один
  // НУЖНО ПЕРЕХВАТИТЬ АРГУМЕНТЫ С ПУСТЫМ СТОП ТОКЕНОМ! (на конструкторе промиса(выделения памяти может не быть))
  // вопрос изменятся ли аргументы или потом копии.
  // и нужно специальную константу приводящуюся к пустому токену, мб.
  //Хм.. Если там копируется, то можно сделать так, чтобы при копировании наоборот происходило хД

  // accepts copied to coroutine frame arguments if first arg is a stop_token, fills its for using in coroutine only
  // this and nomovability of stop_token guarantees, that token is only one for each coroutine
  logical_thread_promise() noexcept = default; // TODO bugreport видимо он игнорирует когда нет дефолта и делает хуету
  logical_thread_promise(stop_token& to_fill, auto&&...) noexcept {
    to_fill.state_ = &stop_requested_; // TODO check working
  }
  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() {
    return std::coroutine_handle<logical_thread_promise>::from_promise(*this);
  }
  static constexpr void return_void() noexcept {
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }

 private:
  // informs owner that is coro is stopped, destroys coro frame if owner is dead
  struct set_stopped_and_wait_or_destroy {
    two_way_bound& link;
    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<void> handle) const noexcept {
      if (!link.try_inform())
        handle.destroy();
    }
    void await_resume() const noexcept {
    }
  };

 public:
  auto final_suspend() noexcept {
    return set_stopped_and_wait_or_destroy{stopped};
  }
  // co_await this_coro::stop_now support 
  auto await_transform(stop_now_t) noexcept {
    return set_stopped_and_wait_or_destroy(stopped);
  }
  template<typename T>
  decltype(auto) await_transform(T&& v) const noexcept {
    return build_awaiter(std::forward<T>(v));
  }
};

// shared owning of coroutine handle between coroutine object and coroutine frame.
// Frame always dies with a coroutine object, except it was detached(then it deletes itself after co_return)
template<typename Alloc>
struct logical_thread_mm {
  using promise_type = logical_thread_promise<Alloc>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle_;

 public:
  // ctor/owning

  logical_thread_mm() noexcept = default;
  logical_thread_mm(handle_type handle) : handle_(handle) {
  }

  logical_thread_mm(logical_thread_mm&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {
  }

  logical_thread_mm& operator=(logical_thread_mm&& other) noexcept {
    try_cancel_and_join();
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }

  void swap(logical_thread_mm& other) noexcept {
    std::swap(handle_, other.handle_);
  }
  friend void swap(logical_thread_mm& left, logical_thread_mm& right) noexcept {
    left.swap(right);
  }

  ~logical_thread_mm() {
    try_cancel_and_join();
  }

  // thread-like interface

  [[nodiscard]] bool joinable() const noexcept {
    return handle_ != nullptr;
  }

  // continue makes owner logical_thread joinable again
  struct continue_token {
   private:
    handle_type handle_;
    logical_thread_mm* owner_;

    template <typename>
    friend struct logical_thread_mm;

   public:
    // this and getting token only from .join of logical_thread guarantees,
    // that continue_token lifetime < logical_thread lifetime
    continue_token(continue_token&&) = delete;
    void operator=(continue_token&&) = delete;
    continue_token(handle_type h, logical_thread_mm* owner) noexcept : handle_(h), owner_(owner) {
      assert(owner != nullptr && h != nullptr);
    }

    // can be noexcept, because terminate on unhandled exception
    template <executor T>
    void continue_on(T&& exe) && {
      assert(handle_ != nullptr);
      std::forward<T>(exe).execute(handle_);
      owner_->handle_ = std::exchange(handle_, nullptr);
    }
    void continue_here() && noexcept {
      std::move(*this).continue_on(this_thread_executor{});
    }
    // non trivially copyable, so bit_cast not working
    ~continue_token() {
      if (handle_)
        handle_.destroy();
    }
  };
  // postcondition : !joinable()
  // returns token if coroutine is not done and just stopped and ready for continue
  // Continue makes *this joinable again
  // continue must be called before logical_thread dies(or never called)
  // TODO check в цикле как работает, можно ли вставить в опшнл новый токен из join
  std::optional<continue_token> join() noexcept {
    assert(joinable());
    // created here for nrvo
    handle_.promise().stopped.wait();
    if (handle_.done()) {
      // coroutine stops as co_return
      // here im sure im not detached from coro, so its not destroy itself
      handle_.destroy();
      handle_ = nullptr;
      return std::nullopt;
    } else {
      // coroutine stops as co_await dd::this_coro::stop_now
      handle_.promise().stopped.reset();
      handle_.promise().stop_requested_.store(false, std::memory_order::relaxed);
      return std::optional<continue_token>{std::in_place, std::exchange(handle_, nullptr), this};
    }
  }

  void detach() noexcept {
    assert(joinable());
    if (!handle_.promise().stopped.try_inform()) [[unlikely]]
      handle_.destroy();
    handle_ = nullptr;
  }

  // stopping

  bool stop_possible() const noexcept {
    return handle_ != nullptr;
  }
  bool request_stop() noexcept {
    if (!stop_possible())
      return false;
    handle_.promise().stop_requested_.store(true, std::memory_order::release);
    return true;
  }

 private:
  void try_cancel_and_join() noexcept {
    if (joinable()) {
      request_stop();
      join();
    }
  }
};

using logical_thread = logical_thread_mm<std::allocator<std::byte>>;

// TEMPLATE FUNCTION stop

template <typename T>
concept stopable = requires(T& value) {
  value.request_stop();
  value.join();
};
// effectively stops every cancellable
// (all.request stop(), then all.join(), faster then just request_stop() + join() for all)
// only for lvalues
void stop(stopable auto&... args) {
  ((args.request_stop()), ...);
  ((args.join()), ...);
}
template <std::ranges::borrowed_range T>
requires stopable<std::ranges::range_value_t<T>>
void stop(T&& rng) {
  for (auto& value : rng)
    value.request_stop();
  for (auto& value : rng)
    value.join();
}

}  // namespace dd