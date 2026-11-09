
#include <atomic>
#include <cstdlib>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kelcoro/async_task.hpp"
#include "kelcoro/task.hpp"
#include "kelcoro/thread_pool.hpp"
#include "kelcoro/latch.hpp"

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
  for (int i = 0; i < task_count; ++i)
    test_tasks.emplace_back(create_test());
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
    (void)pin_thread_to_cpu_core(workers[i], i);
    std::string name = "number " + std::to_string(i);
    (void)set_thread_name(workers[i], name.c_str());
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

// latch may be destroyed right after `wait` returned: count_down on another thread does not use latch after
// waking waiters
TEST(latch_destroyed_after_wait) {
  dd::thread_pool pool(4);
  std::vector<dd::async_task<void>> tasks;
  for (int i = 0; i < 2000; ++i) {
    dd::latch* l = new dd::latch(1, pool);
    auto waiter = [](dd::thread_pool& pool, dd::latch* l) -> dd::async_task<void> {
      (void)co_await dd::jump_on(pool);
      co_await l->wait();
      delete l;
    };
    tasks.push_back(waiter(pool, l));
    dd::schedule_to(pool, [l] { l->count_down(); });
  }
  for (auto& t : tasks)
    t.wait();
  return error_count;
}

TEST(request_stop) {
  auto tsize = dd::thread_pool::default_thread_count();
  thread_local std::minstd_rand rand(42);
  for (size_t i = 0; i <= 1000; i++) {
    dd::thread_pool pool;
    std::vector<dd::async_task<void>> tasks;
    std::atomic_int counter{0};
    std::vector<std::atomic_bool> cancelled(tsize);
    std::vector<std::atomic_int> request_stop_on(tsize);

    for (auto& i : request_stop_on)
      i.store(RAND_MAX);
    auto maker_task = [&](size_t idx) -> dd::async_task<void> {
      auto distr = std::uniform_int_distribution<size_t>(0, i);
      request_stop_on[idx] = distr(rand);
      while (co_await dd::jump_on(pool))
        if (counter.fetch_add(1) >= request_stop_on[idx])
          pool.request_stop();
      cancelled[idx] = true;
    };
    for (size_t i = 0; i < tsize; i++)
      tasks.push_back(maker_task(i));
    std::move(pool).wait_stop();
    for (size_t i = 0; i < tsize; i++) {
      error_if(!tasks[i].ready());
      error_if(!cancelled[i]);
    }
  }

  return error_count;
}

// counts allocations to check that memory is returned to the same resource
struct counting_resource : std::pmr::memory_resource {
  std::atomic_int allocated = 0;

  void* do_allocate(size_t bytes, size_t align) override {
    ++allocated;
    return std::pmr::new_delete_resource()->allocate(bytes, align);
  }
  void do_deallocate(void* p, size_t bytes, size_t align) override {
    --allocated;
    std::pmr::new_delete_resource()->deallocate(p, bytes, align);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
};

TEST(thread_pool_resource) {
  counting_resource r;
  {
    dd::thread_pool pool(2, dd::default_worker_job, r);
    error_if(&pool.get_resource() != &r);
  }
  error_if(r.allocated != 0);
  return error_count;
}

static_assert(dd::executor<dd::blocking_pool>);

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

// long task does not delay tasks pushed after it
TEST(blocking_pool_long_task) {
  std::atomic_bool release = false;
  std::atomic<std::thread::id> blocked_thread;
  std::atomic_int done = 0;
  std::atomic_int on_blocked_thread = 0;
  dd::blocking_pool pool(2);
  dd::schedule_to(pool, [&] {
    blocked_thread = std::this_thread::get_id();
    while (!release)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  error_if(!wait_until([&] { return blocked_thread.load() != std::thread::id(); }));
  enum { N = 100 };
  for (int i = 0; i < N; ++i) {
    dd::schedule_to(pool, [&] {
      if (std::this_thread::get_id() == blocked_thread.load())
        ++on_blocked_thread;
      ++done;
    });
  }
  error_if(!wait_until([&] { return done == N; }));
  error_if(on_blocked_thread != 0);
  release = true;
  return error_count;
}

// all threads execute tasks at the same time
TEST(blocking_pool_parallel) {
  enum { N = 8 };
  std::atomic_int running = 0;
  std::atomic_int all_met = 0;
  dd::blocking_pool pool(N);
  for (int i = 0; i < N; ++i) {
    dd::schedule_to(pool, [&] {
      ++running;
      if (wait_until([&] { return running == N; }))
        ++all_met;
    });
  }
  error_if(!wait_until([&] { return all_met == N; }));
  return error_count;
}

// tasks from many threads, coroutines return to the pool many times
TEST(blocking_pool_many_producers) {
  enum { PRODUCERS = 4, TASKS = 10'000, JUMPS = 10 };
  std::atomic_int executed = 0;
  {
    dd::blocking_pool pool(3);
    auto task = [](dd::blocking_pool& pool, std::atomic_int& executed) -> dd::job {
      for (int i = 0; i < JUMPS; ++i)
        if (!co_await dd::jump_on(pool))
          co_return;
      ++executed;
    };
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
      producers.emplace_back([&] {
        for (int i = 0; i < TASKS; ++i)
          task(pool, executed);
      });
    }
    for (auto& t : producers)
      t.join();
    error_if(!wait_until([&] { return executed == PRODUCERS * TASKS; }));
  }
  error_if(executed != PRODUCERS * TASKS);
  return error_count;
}

// destructor waits for the running task and cancels pending
TEST(blocking_pool_destroy) {
  enum { PENDING = 100 };
  std::atomic_bool started = false;
  std::atomic_bool release = false;
  std::atomic_bool running_done = false;
  std::atomic_int executed = 0;
  std::atomic_int cancelled = 0;
  std::thread releaser;
  {
    dd::blocking_pool pool(1);
    dd::schedule_to(pool, [&] {
      started = true;
      while (!release)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      running_done = true;
    });
    error_if(!wait_until([&] { return started.load(); }));
    auto pending = [](dd::blocking_pool& pool, std::atomic_int& executed,
                      std::atomic_int& cancelled) -> dd::job {
      if (co_await dd::jump_on(pool))
        ++executed;
      else
        ++cancelled;
    };
    for (int i = 0; i < PENDING; ++i)
      pending(pool, executed, cancelled);
    // releases the running task after destructor stopped the pool
    releaser = std::thread([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      release = true;
    });
  }
  releaser.join();
  error_if(!running_done);
  error_if(executed != 0);
  error_if(cancelled != PENDING);
  return error_count;
}

int main() {
  size_t ec = 0;
  ec += test_blocking_pool_long_task();
  ec += test_blocking_pool_parallel();
  ec += test_blocking_pool_many_producers();
  ec += test_blocking_pool_destroy();
  ec += test_latch();
  ec += test_thread_pool();
  ec += test_latch_waiters();
  ec += test_latch_destroyed_after_wait();
  ec += test_request_stop();
  ec += test_thread_pool_resource();
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
