#include "thread_pool.hpp"
#include "latch.hpp"

#include <iostream>

static_assert(dd::co_executor<dd::thread_pool> && dd::co_executor<dd::strand> && dd::co_executor<dd::worker>);

// TODO test latch
std::atomic<int> i = 0;
enum {
  COUNT = 150,
  MUSTBE_EXECUTED = 100'000,
};
std::latch l(COUNT);
dd::thread_pool p(16);
dd::latch start(COUNT, p);
dd::job foo(dd::thread_pool& p) {
  co_await start.arrive_and_wait();
  while (true) {
    auto x = i.fetch_add(1, std::memory_order::acq_rel);
    if (x >= MUSTBE_EXECUTED)
      break;
    co_await p.transition();
  }
  l.count_down();
}
// TODO bench in release
int main() {
  for (int i = 0; i < COUNT; ++i) {
    foo(p);
  }
  l.wait();
  if (i.load() < MUSTBE_EXECUTED)
    return -1;
#ifdef KELCORO_ENABLE_THREADPOOL_MONITORING
  for (const dd::worker& w : p.workers_range()) {
    w.get_moniroting().print(std::cout);
    std::cout << '\n';
  }
#endif
  return 0;
}
