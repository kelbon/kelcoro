#include "kelcoro/async_task.hpp"
#include "kelcoro/task.hpp"
#include "kelcoro/thread_pool.hpp"
#include "kelcoro/latch.hpp"

#include <latch>
#include <iostream>

static_assert(dd::executor<dd::any_executor_ref> && dd::executor<dd::strand> &&
              dd::executor<dd::thread_pool> && dd::executor<dd::worker>);

#define error_if(Cond) error_count += static_cast<bool>((Cond));
#define TEST(NAME) size_t test_##NAME(size_t error_count = 0)

TEST(latch) {
  auto run_task = [](dd::thread_pool& p, std::atomic_int& i, dd::latch& start,
                     dd::latch& done) -> dd::task<void> {
    if (!co_await dd::jump_on(p))
      co_return;
    ++i;
    start.count_down();
  };
  dd::thread_pool pool;
  std::atomic_int counter = 0;
  enum { task_count = 1000 };
  dd::latch start(task_count, pool);
  dd::latch done(task_count, pool);
  auto create_test = [&]() -> dd::async_task<void> {
    co_await run_task(pool, counter, start, done);
    co_await start.wait();
    error_if(counter != task_count);
    co_await done.arrive_and_wait();
    --counter;
  };
  std::vector<dd::async_task<void>> test_tasks;
  for (int i = 0; i < task_count; ++i) {
    test_tasks.emplace_back(create_test());
  }
  for (auto& t : test_tasks)
    t.wait();
  error_if(counter != 0);
  return error_count;
}

enum {
  COUNT = 150,
  MUSTBE_EXECUTED = 1'000'000,
};
dd::job foo(dd::thread_pool& p, dd::latch& start, std::atomic_int& i, std::latch& l) {
  co_await start.arrive_and_wait();
  while (true) {
    auto x = i.fetch_add(1, std::memory_order::acq_rel);
    p.schedule([&] { ++i; });
    if (x >= MUSTBE_EXECUTED)
      break;
    if (!co_await dd::jump_on(p))
      co_return;
  }
  l.count_down();
}
TEST(thread_pool) {
  std::atomic_int i = 0;
  std::latch l(COUNT);
  dd::thread_pool p(16);
  dd::latch start(COUNT, p);

  for (int ind = 0; ind < COUNT; ++ind) {
    foo(p, start, i, l);
    p.schedule([&] { ++i; });
    p.schedule([&] { ++i; });
    p.schedule([&] { ++i; });
    p.schedule([&] { ++i; });
  }
  l.wait();
  if (i.load() < MUSTBE_EXECUTED)
    return -1;
// use global varialbe where stored metrics from thread pool
// (to see metrics only after full stop)
#ifdef KELCORO_ENABLE_THREADPOOL_MONITORING
  for (auto& m : dd::monitorings) {
    error_if(m.finished + m.cancelled != m.pushed);
    m.print(std::cout);
    std::cout << std::endl;
  }
#endif
  return error_count;
}

int main() {
  size_t ec = 0;
  ec += test_latch();
  ec += test_thread_pool();
  return ec;
}
