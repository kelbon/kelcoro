#include "kelcoro/async_task.hpp"
#include "kelcoro/task.hpp"
#include "kelcoro/thread_pool.hpp"
#include "kelcoro/latch.hpp"

#include <latch>
#include <iostream>
#include <string>

static_assert(dd::executor<dd::any_executor_ref> && dd::executor<dd::strand> &&
              dd::executor<dd::thread_pool> && dd::executor<dd::worker>);

#define error_if(Cond) error_count += static_cast<bool>((Cond));
#define TEST(NAME) inline size_t test_##NAME(size_t error_count = 0)

bool pin_thread_to_cpu_core(std::thread&, int core_nb) noexcept;
bool set_thread_name(std::thread&, const char* name) noexcept;

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
  std::span workers = p.workers_range();
  for (int i = 0; i < workers.size(); ++i) {
    (void)pin_thread_to_cpu_core(workers[i].get_thread(), i);
    std::string name = "number " + std::to_string(i);
    (void)set_thread_name(workers[i].get_thread(), name.c_str());
  }
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

TEST(latch_waiters) {
  dd::thread_pool pool;

  std::vector<dd::async_task<void>> tasks;
  dd::latch test_latch(1000, pool);
  auto make_producer = [&]() -> dd::async_task<void> {
    (void)co_await jump_on(test_latch.get_executor());
    if (std::rand() % 2)
      test_latch.count_down();
    else
      co_await test_latch.arrive_and_wait();
    co_return;
  };

  auto make_waiter = [&]() -> dd::async_task<void> {
    (void)co_await dd::jump_on(pool);
    co_await test_latch.wait();
    co_return;
  };
  for (int i = 0; i < 1000; ++i) {
    tasks.push_back(make_waiter());
    tasks.push_back(make_producer());
  }
  for (auto& t : tasks)
    t.wait();
  return error_count;
}

int main() {
  size_t ec = 0;
  ec += test_latch();
  ec += test_thread_pool();
  ec += test_latch_waiters();
  return ec;
}
#ifdef _WIN32
#include <windows.h>
#elif defined(__unix__)
#include <pthread.h>
#else
#endif

bool pin_thread_to_cpu_core(std::thread& t, int core_nb) noexcept {
  if (core_nb < 0 || core_nb >= CHAR_BIT * sizeof(void*))
    return false;
#ifdef _WIN32
  HANDLE handle = t.native_handle();
  DWORD_PTR mask = 1ull << core_nb;
  return SetThreadAffinityMask(handle, mask);
#elif defined(__unix__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_nb, &cpuset);
  pthread_t handle = t.native_handle();
  return pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset) == 0;
#else
  // nothing, not pinned
  return false;
#endif
}

bool set_thread_name(std::thread& t, const char* name) noexcept {
  if (!name)
    return false;
#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0A00
  HANDLE handle = t.native_handle();
  int size_needed = MultiByteToWideChar(CP_UTF8, 0, name, strlen(name), NULL, 0);
  std::wstring nm(size_needed, 0);
  MultiByteToWideChar(CP_UTF8, 0, name, strlen(name), &nm[0], size_needed);
  HRESULT r = SetThreadDescription(handle, nm.c_str());
  return !(FAILED(r));
#elif defined(__unix__)
  return pthread_setname_np(pthread_self(), name) == 0;
#else
  // nothing, name not setted
  return false;
#endif
}
