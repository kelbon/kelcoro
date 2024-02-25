#include "async_task.hpp"
#include "task.hpp"
#include "thread_pool.hpp"
#include "latch.hpp"

#include <iostream>

static_assert(dd::co_executor<dd::thread_pool> && dd::co_executor<dd::strand> && dd::co_executor<dd::worker>);

#define error_if(Cond) error_count += static_cast<bool>((Cond));
#define TEST(NAME) size_t test_##NAME(size_t error_count = 0)

TEST(latch) {
  auto run_task = [](dd::thread_pool& p, std::atomic_int& i, dd::latch& start,
                     dd::latch& done) -> dd::task<void> {
    co_await dd::jump_on(p);
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
  MUSTBE_EXECUTED = 100'000,
};
dd::job foo(dd::thread_pool& p, dd::latch& start, std::atomic_int& i, std::latch& l) {
  co_await start.arrive_and_wait();
  while (true) {
    auto x = i.fetch_add(1, std::memory_order::acq_rel);
    p.execute([&] { ++i; });
    if (x >= MUSTBE_EXECUTED)
      break;
    co_await p.transition();
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
    p.execute([&] { ++i; });
    p.execute([&] { ++i; });
    p.execute([&] { ++i; });
    p.execute([&] { ++i; });
  }
  l.wait();
  if (i.load() < MUSTBE_EXECUTED)
    return -1;
#ifdef KELCORO_ENABLE_THREADPOOL_MONITORING
  for (const dd::worker& w : p.workers_range()) {
    auto& m = w.get_moniroting();
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
