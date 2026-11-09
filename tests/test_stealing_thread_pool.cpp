#include "kelcoro/job.hpp"
#include "kelcoro/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#define error_if(...)                                       \
  if ((__VA_ARGS__)) {                                      \
    std::cout << "ERROR ON LINE " << __LINE__ << std::endl; \
    std::exit(__LINE__);                                    \
  }

static_assert(dd::executor<dd::stealing_thread_pool>);

// false if timed out
static bool wait_until(auto pred) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

static size_t worker_index(dd::stealing_thread_pool& pool, std::thread::id id) {
  auto workers = pool.workers_range();
  for (size_t i = 0; i < workers.size(); ++i)
    if (workers[i].get_id() == id)
      return i;
  std::exit(__LINE__);
}

// one worker is blocked, N tasks are in its queue, worker blocked + woken_offset is woken by a task in its
// own queue. Returns count of tasks from the blocked queue executed while it is blocked
static int stolen_while_blocked(size_t threads, size_t steal_attempts, size_t woken_offset) {
  enum { N = 100 };
  std::atomic_bool release = false;
  std::atomic<std::thread::id> blocked_id;
  std::atomic_int done = 0;
  std::atomic_bool waker_done = false;
  std::atomic_int stolen = 0;
  {
    dd::stealing_thread_pool pool(threads, steal_attempts);
    pool.schedule(
        [&] {
          blocked_id = std::this_thread::get_id();
          while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },
        0);
    error_if(!wait_until([&] { return blocked_id.load() != std::thread::id(); }));
    // other workers went to sleep after their first look into queues
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    size_t blocked = worker_index(pool, blocked_id);
    for (int i = 0; i < N; ++i) {
      pool.schedule(
          [&] {
            if (std::this_thread::get_id() != blocked_id.load())
              ++stolen;
            ++done;
          },
          blocked);
    }
    pool.schedule([&] { waker_done = true; }, (blocked + woken_offset) % threads);
    error_if(!wait_until([&] { return waker_done.load(); }));
    // with stealing all are done while the owner is blocked, without - only after release
    if (steal_attempts > 0)
      error_if(!wait_until([&] { return done == N; }));
    release = true;
    error_if(!wait_until([&] { return done == N; }));
  }
  return stolen;
}

// tasks from many threads, coroutines return to the pool many times
static void many_producers() {
  enum { PRODUCERS = 4, TASKS = 10'000, JUMPS = 10 };
  std::atomic_int executed = 0;
  {
    dd::stealing_thread_pool pool(4, 3);
    auto task = [](dd::stealing_thread_pool& pool, std::atomic_int& executed) -> dd::job {
      for (int i = 0; i < JUMPS; ++i)
        if (!co_await dd::jump_on(pool))
          co_return;
      ++executed;
    };
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
      producers.emplace_back([&] {
        for (int i = 0; i < TASKS; ++i)
          task(pool, executed);
      });
    for (auto& t : producers)
      t.join();
    error_if(!wait_until([&] { return executed == PRODUCERS * TASKS; }));
  }
  error_if(executed != PRODUCERS * TASKS);
}

// deadpills may be stolen: every worker stops, every task is executed or cancelled
static void stop_while_stealing() {
  for (int iteration = 0; iteration < 300; ++iteration) {
    std::atomic_int finished = 0;
    enum { COROS = 50 };
    {
      dd::stealing_thread_pool pool(4, 3);
      auto coro = [](dd::stealing_thread_pool& pool, std::atomic_int& finished, int i) -> dd::job {
        while (co_await dd::jump_on(pool))
          if (i == 0)
            pool.request_stop();
        ++finished;
      };
      for (int i = 0; i < COROS; ++i)
        coro(pool, finished, i);
      std::move(pool).wait_stop();
    }
    error_if(finished != COROS);
  }
  // destructor without request_stop
  for (int iteration = 0; iteration < 300; ++iteration) {
    std::atomic_int finished = 0;
    enum { COROS = 50 };
    {
      dd::stealing_thread_pool pool(4, 3);
      auto coro = [](dd::stealing_thread_pool& pool, std::atomic_int& finished) -> dd::job {
        while (co_await dd::jump_on(pool))
          ;
        ++finished;
      };
      for (int i = 0; i < COROS; ++i)
        coro(pool, finished);
    }
    error_if(finished != COROS);
  }
}

int main() {
  // a woken worker takes tasks of the blocked one
  error_if(stolen_while_blocked(2, 1, 1) != 100);
  // no stealing
  error_if(stolen_while_blocked(2, 0, 1) != 0);
  // 3 attempts look into all other queues: the blocked one is found wherever it is
  for (size_t woken = 1; woken < 4; ++woken)
    error_if(stolen_while_blocked(4, 3, woken) != 100);
  many_producers();
  stop_while_stealing();
  return 0;
}
