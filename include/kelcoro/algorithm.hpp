#pragma once

#include "kelcoro/noexport/expected.hpp"
#include "kelcoro/task.hpp"
#include "kelcoro/job.hpp"

namespace dd {

namespace noexport {

template <typename T, typename OwnerPromise, typename Ctx>
job job_for_when_all(task<T, Ctx>& child, std::coroutine_handle<OwnerPromise> owner,
                     expected<T, std::exception_ptr>& result, std::atomic<size_t>& count) {
  co_await child.wait_with_proxy_owner(owner);

  if (child.raw_handle().promise().has_exception()) [[unlikely]]
    result.data.template emplace<1>(child.raw_handle().promise().take_exception());
  else if constexpr (!std::is_void_v<T>)
    result.data.template emplace<0>(child.raw_handle().promise().result());
  else
    result.data.template emplace<0>();
  size_t i = count.fetch_sub(1, std::memory_order_acq_rel);
  if (i == 1)  // im last, now 'count' == 0
    co_await this_coro::destroy_and_transfer_control_to(owner);
};

template <typename... Ts>
struct when_any_state {
  std::mutex mtx;
  std::variant<std::monostate, expected<Ts, std::exception_ptr>...> result;
  size_t count_waiters = 0;
  std::coroutine_handle<> owner = nullptr;

  explicit when_any_state(size_t count_waiters, std::coroutine_handle<> owner) noexcept
      : count_waiters(count_waiters), owner(owner) {
  }

  // returns owner if called must resume it
  template <size_t I>
  [[nodiscard]] std::coroutine_handle<> set_exception(std::exception_ptr p) noexcept {
    std::lock_guard l(mtx);
    if (!owner)  // do not overwrite value, if set_result setted
      return nullptr;
    result.template emplace<I>(unexpected(std::move(p)));
    --count_waiters;
    // returns true only if all tasks ended with exception
    return count_waiters == 0 ? owner : nullptr;
  }
  // returns owner if caller must resume it
  template <size_t I, typename... Args>
  [[nodiscard]] std::coroutine_handle<> set_result(Args&&... args) {
    static_assert(I != 0);
    std::unique_lock l(mtx);
    if (!owner)
      return nullptr;
    try {
      result.template emplace<I>(std::forward<Args>(args)...);
    } catch (...) {
      l.unlock();  // prevent deadlock
      return set_exception<I>(std::current_exception());
    }
    return std::exchange(owner, nullptr);
  }
};

template <size_t I, typename T, typename Ctx, typename... Ts>
job job_for_when_any(task<T, Ctx> child, std::weak_ptr<when_any_state<Ts...>> state) {
  // stop at entry and give when_any do its preparations
  co_await std::suspend_always{};
  if (state.expired()) {
    // someone sets result while we was starting without awaiting
    co_return;
  }
  // without proxy owner, because real owner may be destroyed while this task is running
  co_await child.wait();
  std::shared_ptr state_s = state.lock();
  if (!state_s)  // no one waits
    co_return;
  auto& child_promise = child.raw_handle().promise();
  std::coroutine_handle<> owner;
  if (child_promise.has_exception()) [[unlikely]]
    owner = state_s->template set_exception<I>(child_promise.take_exception());
  else if constexpr (!std::is_void_v<T>)
    owner = state_s->template set_result<I>(child_promise.result());
  else
    owner = state_s->template set_result<I>();
  if (owner)
    co_await this_coro::destroy_and_transfer_control_to(owner);
}

template <typename T>
struct when_any_state_dyn {
  std::mutex mtx;
  std::variant<std::monostate, expected<T, std::exception_ptr>> result;
  size_t index = size_t(-1);
  size_t count_waiters = 0;
  std::coroutine_handle<> owner = nullptr;

  explicit when_any_state_dyn(size_t count_waiters, std::coroutine_handle<> owner) noexcept
      : count_waiters(count_waiters), owner(owner) {
  }

  // returns owner if called must resume it
  [[nodiscard]] std::coroutine_handle<> set_exception(size_t I, std::exception_ptr p) noexcept {
    std::lock_guard l(mtx);
    if (!owner)  // do not overwrite value, if set_result setted
      return nullptr;
    result.template emplace<1>(unexpected(std::move(p)));
    index = I;
    --count_waiters;
    // returns true only if all tasks ended with exception
    return count_waiters == 0 ? owner : nullptr;
  }
  // returns owner if caller must resume it
  template <typename... Args>
  [[nodiscard]] std::coroutine_handle<> set_result(size_t I, Args&&... args) {
    std::unique_lock l(mtx);
    if (!owner)
      return nullptr;
    try {
      result.template emplace<1>(std::forward<Args>(args)...);
    } catch (...) {
      l.unlock();  // prevent deadlock
      return set_exception(I, std::current_exception());
    }
    index = I;
    return std::exchange(owner, nullptr);
  }
};

template <typename T, typename Ctx>
job job_for_when_any_dyn(task<T, Ctx> child, std::weak_ptr<when_any_state_dyn<T>> state, size_t I) {
  // stop at entry and give when_any do its preparations
  co_await std::suspend_always{};
  if (state.expired()) {
    // someone sets result while we was starting without awaiting
    co_return;
  }
  // without proxy owner, because real owner may be destroyed while this task is running
  co_await child.wait();
  std::shared_ptr state_s = state.lock();
  if (!state_s)  // no one waits
    co_return;
  auto& child_promise = child.raw_handle().promise();
  std::coroutine_handle<> owner;
  if (child_promise.has_exception()) [[unlikely]]
    owner = state_s->set_exception(I, child_promise.take_exception());
  else if constexpr (!std::is_void_v<T>)
    owner = state_s->set_result(I, child_promise.result());
  else
    owner = state_s->set_result(I);
  if (owner)
    co_await this_coro::destroy_and_transfer_control_to(owner);
}

}  // namespace noexport

// all tasks contexts will be attached to one owner
// precondition: all tasks not .empty()
template <typename... Ts, typename Ctx>
auto when_all(KELCORO_ELIDABLE_ARG task<Ts, Ctx>... tasks)
    -> task<std::tuple<expected<Ts, std::exception_ptr>...>, Ctx> {
  assert((tasks && ...));
  if constexpr (sizeof...(tasks) == 0)
    co_return {};
  std::atomic<size_t> count = sizeof...(tasks);
  std::tuple<expected<Ts, std::exception_ptr>...> results;

  co_await this_coro::suspend_and([&](auto when_all_handle) {
    auto starter = [&](auto&... vals) {
      (noexport::job_for_when_all(tasks, when_all_handle, vals, count), ...);
    };
    std::apply(starter, results);
  });
  co_return results;
}

// precondition: all tasks not .empty()
template <typename T, typename Ctx>
auto when_all(std::vector<task<T, Ctx>> tasks) -> task<std::vector<expected<T, std::exception_ptr>>, Ctx> {
  assert(std::ranges::find_if(tasks, [](auto& t) { return t.empty(); }) == tasks.end());
  if (tasks.empty())
    co_return {};
  std::atomic<size_t> count = tasks.size();
  std::vector<expected<T, std::exception_ptr>> results(tasks.size());

  co_await this_coro::suspend_and([&](auto when_all_handle) {
    size_t i = 0;
    for (auto& task : tasks)
      noexport::job_for_when_all(task, when_all_handle, results[i++], count);
  });
  co_return results;
}

template <typename T, typename... Ts>
struct first_type {
  using type = T;
};
// precondition: all tasks not .empty()
// returns first not failed or last exception if all failed
// result is never monostate (.index() > 0)
template <typename... Ts, typename... Ctx>
auto when_any(task<Ts, Ctx>... tasks)
    -> task<std::variant<std::monostate, expected<Ts, std::exception_ptr>...>,
            typename first_type<Ctx...>::type> {
  assert((tasks && ...));
  static_assert(sizeof...(tasks) != 0);

  std::shared_ptr state =
      std::make_shared<noexport::when_any_state<Ts...>>(sizeof...(tasks), co_await this_coro::handle);

  co_await this_coro::suspend_and([&](std::coroutine_handle<>) {
    [&]<size_t... Is>(std::index_sequence<Is...>) {
      // guard for case when someone destroys 'when_any' while we are starting
      // (return result and ends coroutine)
      std::weak_ptr guard = state;
      // to stack for case when one of them throws/returns without await and destroys 'when_any' task
      // + 1 bcs of monostate in results
      job jobs[] = {noexport::job_for_when_any<Is + 1>(std::move(tasks), guard)...};
      // must not throw
      for (job& j : jobs)
        j.handle.resume();
    }(std::index_sequence_for<Ts...>{});
  });

  co_return std::move(state->result);
}

template <typename T>
struct when_any_dyn_result {
  expected<T, std::exception_ptr> value;
  size_t indx = size_t(-1);

  size_t index() const noexcept {
    return indx;
  }
};
// precondition: all tasks not .empty()
// returns value and index of first not failed or last exception if all failed
// returns only index if T is void
// Note: unlike static `when_any` version, returns actual index, not variant with index + 1
template <typename T, typename Ctx>
auto when_any(std::vector<task<T, Ctx>> tasks)
    -> task<when_any_dyn_result<T>, typename first_type<Ctx>::type> {
  assert(!tasks.empty());
  std::shared_ptr state =
      std::make_shared<noexport::when_any_state_dyn<T>>(tasks.size(), co_await this_coro::handle);

  co_await this_coro::suspend_and([&](std::coroutine_handle<>) {
    // guard for case when someone destroys 'when_any' while we are starting
    // (return result and ends coroutine)
    std::weak_ptr guard = state;
    // to stack for case when one of them throws/returns without await and destroys 'when_any' task
    // + 1 bcs of monostate in results
    std::vector<job> jobs;
    size_t i = 0;
    for (auto& t : tasks)
      jobs.push_back(noexport::job_for_when_any_dyn(std::move(t), guard, i++));
    // must not throw
    for (job& j : jobs)
      j.handle.resume();
  });

  co_return when_any_dyn_result{std::move(std::get<1>(state->result)), state->index};
}

// binds arguments to a task so that they will be destroyed after the task is executed
// example:
//   unique_ptr<X> x = ...;
//   X* ptr = x.get();
//   with(mytask(ptr), std::move(x)).start_and_detach();
// precondition: !t.empty()
template <typename T, typename Ctx>
task<T, Ctx> with(KELCORO_ELIDABLE_ARG task<T, Ctx> t, auto...) {
  co_return co_await t;
}

// helper struct to return avaitable and just value at same time
// behaves as std::suspend_never, but returns `T` from co_await
//  example:
//   // jumps on executor and just returns 10 after it
//   dd::chain(dd::jump_on(e), dd::ignore_result, dd::avalue(10));
//  example:
//   dd::task<> foo(int arg);
//   // just passes `42` into `foo`
//   dd::chain(dd::avalue(42), foo);
template <typename T>
struct avalue {
  T value;

  static constexpr bool await_ready() noexcept {
    return true;
  }
  static constexpr void await_suspend(std::coroutine_handle<>) noexcept {
    KELCORO_UNREACHABLE;
  }
  constexpr T&& await_resume() noexcept KELCORO_LIFETIMEBOUND {
    return std::move(value);
  }
};

template <co_awaitable A, typename Ctx = null_context>
task<await_result_t<A>, Ctx> with(KELCORO_ELIDABLE_ARG A t, auto...) {
  co_return co_await t;
}

// helper for `chain`, ignores result of prev awaitable
//  example:
//   auto foo_without_args = [] { return make_task(); };
//   chain(mytask(), dd::ignore_result(), foo_without_args);
struct ignore_result_t {
  std::suspend_never operator()(auto&&...) const noexcept {
    return {};
  }
};

constexpr inline ignore_result_t ignore_result = {};

// `foo` should be invocable as foo(T{}) -> awaitable
// precondition: !t.empty() && foo returns smth avaitable
template <typename Ctx = null_context, co_awaitable A, std::invocable<await_result_t<A>> B>
  requires(!std::is_void_v<await_result_t<A>>)
auto chain(KELCORO_ELIDABLE_ARG A a, KELCORO_ELIDABLE_ARG B b)
    -> task<std::remove_cvref_t<await_result_t<std::invoke_result_t<B, await_result_t<A>>>>, Ctx> {
  co_return co_await b(co_await a);
}

// if `foo` is both invocable AND co_awaitable, compilation error (ambigious)
// B should be invocable without args
template <typename Ctx = null_context, co_awaitable A, std::invocable B>
  requires(std::is_void_v<await_result_t<A>>)
auto chain(KELCORO_ELIDABLE_ARG A a, KELCORO_ELIDABLE_ARG B b)
    -> task<std::remove_cvref_t<await_result_t<std::invoke_result_t<B>>>, Ctx> {
  co_await a;
  co_return co_await b();
}

// B should be co_awaitable
template <typename Ctx = null_context, co_awaitable A, co_awaitable B>
  requires(std::is_void_v<await_result_t<A>>)
auto chain(KELCORO_ELIDABLE_ARG A a, KELCORO_ELIDABLE_ARG B b)
    -> task<std::remove_cvref_t<await_result_t<B>>, Ctx> {
  co_await a;
  co_return co_await b;
}

template <typename Ctx = null_context, co_awaitable A, typename Head, typename Tail1, typename... Tail>
auto chain(KELCORO_ELIDABLE_ARG A t, KELCORO_ELIDABLE_ARG Head foo, KELCORO_ELIDABLE_ARG Tail1 tail1,
           KELCORO_ELIDABLE_ARG Tail... tail) {
  return chain<Ctx>(chain<Ctx>(std::move(t), std::move(foo)), std::move(tail1), std::move(tail)...);
}

}  // namespace dd
