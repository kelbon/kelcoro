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

  if (child.raw_handle().promise().exception) [[unlikely]]
    result.data.template emplace<1>(child.raw_handle().promise().exception);
  else {
    if constexpr (!std::is_void_v<T>) {
      result.data.template emplace<0>(child.raw_handle().promise().result());
    } else {
      result.data.template emplace<0>();
    }
  }
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
    std::lock_guard l(mtx);
    if (!owner)
      return nullptr;
    result.template emplace<I>(std::forward<Args>(args)...);
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
  if (child_promise.exception) [[unlikely]]
    owner = state_s->template set_exception<I>(child_promise.exception);
  else {
    if constexpr (!std::is_void_v<T>) {
      owner = state_s->template set_result<I>(child_promise.result());
    } else {
      owner = state_s->template set_result<I>();
    }
  }
  if (owner)
    co_await this_coro::destroy_and_transfer_control_to(owner);
}

}  // namespace noexport

// all tasks contexts will be attached to one owner
// precondition: all tasks not .empty()
template <typename... Ts, typename Ctx>
auto when_all(task<Ts, Ctx>... tasks) -> task<std::tuple<expected<Ts, std::exception_ptr>...>, Ctx> {
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
      // guard for case when someone destroys 'when_all' while we are starting
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

// TODO when any dynamic count, fail-fast policy with returning ANY first result?
// TODO different contexts when_all Tuple contextes + attach one to one...

}  // namespace dd
