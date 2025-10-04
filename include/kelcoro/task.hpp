#pragma once

#include "common.hpp"
#include "kelcoro/async_task.hpp"
#include "memory_support.hpp"

namespace dd {

namespace this_coro {

constexpr inline get_context_t context = get_context_t{};

// just tag, coroutine knows how to return it
struct KELCORO_CO_AWAIT_REQUIRED get_return_place_t {
  explicit get_return_place_t() = default;

  template <typename Ptr>
  struct awaiter {
    Ptr place;

    static bool await_ready() noexcept {
      return false;
    }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
      place = std::addressof(handle.promise().return_place());
      return false;
    }
    decltype(auto) await_resume() noexcept {
      return *place;
    }
  };
};

constexpr inline get_return_place_t return_place = get_return_place_t{};

}  // namespace this_coro

// default task Ctx, also example of what context may do
struct null_context {
  // invoked when task is scheduled to execute, 'owner' is handle of coroutine, which awaits task
  // never invoked with nullptrs
  // note: one task may have only one owner, but one owner can have many child tasks (when_all/when_any)
  template <typename OwnerPromise, typename P>
  static void on_owner_setted(std::coroutine_handle<OwnerPromise>, std::coroutine_handle<P>) noexcept {
  }
  // may be invoked even without 'on_owner_setted' before, if task is detached
  template <typename P>
  static void on_start(std::coroutine_handle<P>) noexcept {
  }
  // invoked when task ended with/without exception
  template <typename P>
  static void on_end(std::coroutine_handle<P>) noexcept {
  }

  // invoked when no one waits for task and exception thrown
  // precondition: std::current_exception() != nullptr
  // Note: if exception thrown memory leak possible (no one will destroy handle)
  // this function MUST NOT destroy handle (UB) and handle must not be destroyed while exception alive
  // Note: passed coroutine handle will became invalid after this call
  template <typename P>
  static void on_ignored_exception(std::coroutine_handle<P>) noexcept {
  }
};

template <typename Result, typename Ctx>
struct task_promise : return_block<Result> {
  KELCORO_NO_UNIQUE_ADDRESS Ctx ctx;
  std::coroutine_handle<> who_waits;
  std::exception_ptr exception = nullptr;

  auto await_transform(this_coro::get_context_t) noexcept {
    return this_coro::get_context_t::awaiter<Ctx>{};
  }
  auto await_transform(this_coro::get_handle_t) noexcept {
    return this_coro::get_handle_t::awaiter<task_promise>{};
  }
  auto await_transform(this_coro::get_return_place_t) noexcept {
    return this_coro::get_return_place_t::awaiter<decltype(std::addressof(this->return_place()))>{};
  }
  KELCORO_DEFAULT_AWAIT_TRANSFORM;

  auto self_handle() noexcept {
    return std::coroutine_handle<task_promise>::from_promise(*this);
  }
  // precondition: not running && .done
  std::add_rvalue_reference_t<Result> result_or_rethrow() {
    if (exception) [[unlikely]]
      std::rethrow_exception(exception);
    return this->result();
  }

  static constexpr std::suspend_always initial_suspend() noexcept {
    return {};
  }
  auto get_return_object() noexcept {
    return self_handle();
  }
  void unhandled_exception() noexcept(noexcept(ctx.on_ignored_exception(self_handle()))) {
    // if no one waits, throw exception to last .resume caller
    if (who_waits == nullptr) [[unlikely]]
      ctx.on_ignored_exception(self_handle());
    exception = std::current_exception();
  }
  auto final_suspend() noexcept {
    ctx.on_end(self_handle());
    return transfer_control_to{who_waits};
  }
};

// single value generator that returns a value with a co_return
template <typename Result, typename Ctx = null_context>
struct KELCORO_ELIDE_CTX [[nodiscard]] task : enable_resource_deduction {
  using result_type = Result;
  using promise_type = task_promise<Result, Ctx>;
  using handle_type = std::coroutine_handle<promise_type>;
  using context_type = Ctx;

 private:
  handle_type handle_;

 public:
  constexpr task() noexcept = default;
  // precoondition: one handle owned only by 1 task
  constexpr task(handle_type handle) noexcept : handle_(handle) {
  }

  task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {
  }
  task& operator=(task&& other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
  }

  ~task() {
    if (handle_)
      handle_.destroy();
  }
  // returns true if task cant be co_awaited
  constexpr bool empty() const noexcept {
    return handle_ == nullptr;
  }
  constexpr explicit operator bool() const noexcept {
    return !empty();
  }
  [[nodiscard]] handle_type release() noexcept {
    return std::exchange(handle_, nullptr);
  }
  [[nodiscard]] handle_type raw_handle() const noexcept {
    return handle_;
  }

  context_type* get_context() const noexcept {
    return handle_ ? std::addressof(handle_.promise().ctx) : (context_type*)nullptr;
  }

  // precondition: empty() || not started yet
  // postcondition: empty(), task result ignored (exception too)
  // returns released task handle
  // if 'stop_at_end' is false, then task will delete itself at end,
  // otherwise handle.destroy() should be called
  handle_type start_and_detach(bool stop_at_end = false) {
    if (!handle_)
      return nullptr;
    handle_type h = detach();
    if (h.done()) {
      if (!stop_at_end) {
        h.destroy();
        return nullptr;
      }
      return h;
    }
    // task resumes itself at end and destroys itself or just stops with noop_coroutine
    h.promise().who_waits = stop_at_end ? std::noop_coroutine() : std::coroutine_handle<>(nullptr);
    h.promise().ctx.on_start(h);
    h.resume();
    return h;
  }

  // same as release() + prepare to start
  // example: thread_pool.schedule(mytask().detach())
  // postcondiion: empty()
  // precondition: not started yet
  [[nodiscard("must be resumed or destroyed")]] handle_type detach() noexcept {
    handle_type h = release();
    if (h)
      h.promise().who_waits = nullptr;
    return h;
  }

  // blocking (if not done yet)
  // Note: .get() on not ready task which is not scheduled to another thread will lead to deadlock
  // precondition: !task.empty() && task not started yet or already done (after .wait for example)
  result_type get() {
    assert(!empty());
    if (handle_.done()) {
      // fast path without creating a coroutine
      return handle_.promise().result_or_rethrow();
    }
    return [](task t) -> async_task<result_type> { co_return co_await t; }(std::move(*this)).get();
  }

 private:
  template <typename OwnerPromise>
  static void prepare_task_to_start(handle_type task_handle, std::coroutine_handle<OwnerPromise> owner) {
    auto& promise = task_handle.promise();
    promise.who_waits = owner;
    promise.ctx.on_owner_setted(owner, task_handle);
    promise.ctx.on_start(task_handle);
  }

  // remembers handle of coroutine where co_await happen and starts task
  struct starter {
    handle_type task_handle;

    explicit starter(handle_type t) noexcept : task_handle(t) {
    }
    bool await_ready() noexcept {
      // for case when awaited after start or second co_await
      return task_handle.done();
    }
    template <typename OwnerHandle>
    KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
        std::coroutine_handle<OwnerHandle> owner) const noexcept {
      prepare_task_to_start(task_handle, owner);
      return task_handle;
    }
  };

  struct wait_and_get_ref_awaiter : starter {
    using starter::starter;

    using starter::await_ready;
    using starter::await_suspend;
    [[nodiscard]] std::add_rvalue_reference_t<result_type> await_resume() {
      return this->task_handle.promise().result_or_rethrow();
    }
  };

  struct wait_and_get_value_awaiter : starter {
    using starter::starter;

    using starter::await_ready;
    using starter::await_suspend;
    [[nodiscard]] result_type await_resume() {
      return this->task_handle.promise().result_or_rethrow();
    }
  };

  struct wait_awaiter : starter {
    using starter::starter;

    using starter::await_ready;
    using starter::await_suspend;
    static void await_resume() noexcept {
    }
  };

  template <typename OwnerPromise>
  struct wait_with_proxy_owner_awaiter : wait_awaiter {
    std::coroutine_handle<OwnerPromise> proxy_owner;

    wait_with_proxy_owner_awaiter(handle_type task_h,
                                  std::coroutine_handle<OwnerPromise> proxy_owner) noexcept
        : wait_awaiter(task_h), proxy_owner(proxy_owner) {
    }

    KELCORO_ASSUME_NOONE_SEES std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> owner) const noexcept {
      prepare_task_to_start(this->task_handle, proxy_owner);
      this->task_handle.promise().who_waits = owner;
      // symmetric transfer control to task
      return this->task_handle;
    }
  };

 public:
  // precondition: !empty()
  // await_resume returns reference (result_type&& or void for void)
  constexpr auto operator co_await() & noexcept {
    assert(!empty());
    return wait_and_get_ref_awaiter{handle_};
  }

  // precondition: !empty()
  // await_resume returns value  (task is &&)
  constexpr auto operator co_await() && noexcept {
    assert(!empty());
    return wait_and_get_value_awaiter{handle_};
  }

  // precondition: !empty()
  // postcondition: .get() will return without blocking
  auto wait() noexcept {
    assert(!empty());
    return wait_awaiter(handle_);
  }

  // same as .wait, but uses context of another coroutine, see when_all for example
  // postcondition: .get() will return without blocking
  template <typename OwnerPromise>
  auto wait_with_proxy_owner(std::coroutine_handle<OwnerPromise> proxy_owner) noexcept {
    assert(!empty());
    return wait_with_proxy_owner_awaiter<OwnerPromise>(handle_, proxy_owner);
  }
};

template <typename Ret, memory_resource R, typename Ctx = null_context>
using task_r = resourced<task<Ret, Ctx>, R>;

namespace pmr {

template <typename Ret, typename Ctx = null_context>
using task = ::dd::task_r<Ret, polymorphic_resource, Ctx>;

}

template <typename R, typename Ctx>
struct operation_hash<std::coroutine_handle<task_promise<R, Ctx>>> {
  operation_hash_t operator()(std::coroutine_handle<task_promise<R, Ctx>> handle) noexcept {
    return operation_hash<std::coroutine_handle<>>()(handle.promise().who_waits);
  }
};

}  // namespace dd
