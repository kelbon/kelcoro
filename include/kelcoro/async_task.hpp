#pragma once

#include <atomic>

#include "common.hpp"
#include "memory_support.hpp"

namespace dd {

template <typename>
struct async_task;

template <typename Result>
struct async_task_promise : return_block<Result> {
  std::atomic_bool ready = false;
  // only owner and coroutine itself are owners
  std::atomic_int8_t ref_count = 1;

  static constexpr std::suspend_never initial_suspend() noexcept {
    return {};
  }
  async_task<Result> get_return_object() {
    return async_task<Result>(std::coroutine_handle<async_task_promise<Result>>::from_promise(*this));
  }
  [[noreturn]] void unhandled_exception() const noexcept {
    std::terminate();
  }

 private:
  struct destroy_if_consumer_dead_t {
    static bool await_ready() noexcept {
      return false;
    }
    bool await_suspend(std::coroutine_handle<async_task_promise> handle) const noexcept {
      auto& p = handle.promise();
      p.ready.exchange(true, std::memory_order::acq_rel);
      p.ready.notify_one();
      // continue and destroy if ref count == 0
      bool im_last_owner = p.ref_count.fetch_sub(1, std::memory_order::acq_rel) == 1;
      return !im_last_owner;
    }
    static void await_resume() noexcept {
    }
  };

 public:
  auto final_suspend() noexcept {
    return destroy_if_consumer_dead_t{};
  }
};

// one producer, one consumer
template <typename Result>
struct async_task : enable_resource_deduction {
  using promise_type = async_task_promise<Result>;
  using handle_type = std::coroutine_handle<promise_type>;

 private:
  handle_type handle = nullptr;

  friend promise_type;
  constexpr explicit async_task(handle_type handle) noexcept : handle(handle) {
    handle.promise().ref_count.fetch_add(1, std::memory_order::acq_rel);
  }

 public:
  constexpr async_task() noexcept = default;

  constexpr void swap(async_task& other) noexcept {
    std::swap(handle, other.handle);
  }
  friend constexpr void swap(async_task& a, async_task& b) noexcept {
    a.swap(b);
  }
  constexpr async_task(async_task&& other) noexcept {
    swap(other);
  }
  constexpr async_task& operator=(async_task&& other) noexcept {
    swap(other);
    return *this;
  }

  // postcondition: if !empty(), then coroutine suspended and value produced
  void wait() const noexcept {
    if (!empty())
      handle.promise().ready.wait(false, std::memory_order::acquire);
  }
  // returns true if 'get' is callable and will return immedially without wait
  bool ready() const noexcept {
    if (empty())
      return false;
    return handle.promise().ready.load(std::memory_order::acquire);
  }
  // postcondition: empty()
  void detach() noexcept {
    if (empty())
      return;
    if (handle.promise().ref_count.fetch_sub(1, std::memory_order::acq_rel) == 1)
      handle.destroy();
    handle = nullptr;
  }

  // precondition: !empty()
  // must be invoked in one thread(one consumer)
  std::add_rvalue_reference_t<Result> get() && noexcept KELCORO_LIFETIMEBOUND {
    assert(!empty());
    wait();
    // result always exist, its setted or std::terminate called on exception.
    return handle.promise().result();
  }

  // return true if call to 'get' will produce UB
  constexpr bool empty() const noexcept {
    return handle == nullptr;
  }

  ~async_task() {
    detach();
  }
};

template <typename Ret, memory_resource R>
using async_task_r = resourced<async_task<Ret>, R>;

namespace pmr {

template <typename Ret>
using async_task = ::dd::async_task_r<Ret, polymorphic_resource>;

}

}  // namespace dd