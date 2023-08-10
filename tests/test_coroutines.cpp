#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif
#include <atomic>
#include <coroutine>
#include <iterator>
#include <memory_resource>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <functional>
#include <iostream>

#include "async_task.hpp"
#include "channel.hpp"
#include "generator.hpp"
#include "job.hpp"
#include "logical_thread.hpp"
#include "task.hpp"
#include "events.hpp"

#define error_if(Cond) error_count += static_cast<bool>((Cond))
#define TEST(NAME) inline size_t TEST##NAME(size_t error_count = 0)

inline size_t some_task(int i) {
  return i;
}
inline dd::generator<int> foo() {
  for (int i = 0;; ++i)
    co_yield some_task(i);
}

TEST(generator) {
  static_assert(std::ranges::input_range<dd::generator<size_t>&&>);
  static_assert(std::input_iterator<dd::generator_iterator<std::vector<int>>>);
  static_assert(std::ranges::input_range<dd::generator<int>>);
  int i = 1;
  dd::generator gen = foo();
  for (auto value : gen | ::std::views::take(100) | std::views::filter([](auto v) { return v % 2; })) {
    error_if(value != i);
    i += 2;
  }
  return error_count;
}

template <std::ranges::borrowed_range... Ranges>
auto zip(Ranges&&... rs) -> dd::generator<decltype(std::tie(std::declval<Ranges>()[0]...))> {
  for (size_t i = 0; ((i < rs.size()) && ...); ++i)
    co_yield std::tie(rs[i]...);
}

TEST(zip_generator) {
  std::vector<size_t> vec;
  vec.resize(12, 20);
  std::string sz = "Hello world";
  for (auto [a, b, c] : zip(vec, sz, std::string_view(sz))) {
    error_if(a != 20);
  }
  return error_count;
}

inline dd::logical_thread multithread(std::atomic<int32_t>& value) {
  auto handle = co_await dd::this_coro::handle;
  (void)handle;
  auto token = co_await dd::this_coro::stop_token;
  (void)token.stop_requested();
  co_await dd::jump_on(dd::new_thread_executor{});
  for (auto i : std::views::iota(0, 100))
    ++value, (void)i;
}

inline void moo(std::atomic<int32_t>& value, std::pmr::memory_resource* m = std::pmr::new_delete_resource()) {
  std::vector<dd::logical_thread> workers;
  {
    auto _ = dd::with_resource(*m);
    for (int i = 0; i < 10; ++i)
      workers.emplace_back(multithread(value));
  }
  stop(workers);  // more effective then just dctors for all
}
TEST(logical_thread) {
  std::atomic<int32_t> i;
  moo(i);
  error_if(i != 1000);  // 10 coroutines * 100 increments
  return error_count;
}

dd::logical_thread bar(bool& requested) {
  auto handle = co_await dd::this_coro::handle;
  (void)handle;
  co_await dd::jump_on(dd::new_thread_executor{});
  auto token = co_await dd::this_coro::stop_token;
  while (true) {
    std::this_thread::sleep_for(std::chrono::microseconds(5));
    if (token.stop_requested()) {
      requested = true;
      co_return;
    }
  }
}
TEST(coroutines_integral) {
  bool is_requested = false;
  bar(is_requested);
  error_if(!is_requested);
  return error_count;
}

struct statefull_resource : std::pmr::memory_resource {
  size_t sz = 0;
  // sizeof of this thing affects frame size with 2 multiplier bcs its saved in frame + saved for coroutine
  void* do_allocate(size_t size, size_t) override {
    sz = size;
    return ::operator new(size);
  }
  void do_deallocate(void* ptr, size_t size, size_t) noexcept override {
    if (sz != size)  // cant throw here(std::terminate)
      (std::cerr << "incorrect size"), std::exit(-111);
    ::operator delete(ptr);
  }
  bool do_is_equal(const memory_resource& _That) const noexcept override {
    return true;
  }
};

TEST(logical_thread_mm) {
  std::atomic<int32_t> i;
  statefull_resource r;
  moo(i, &r);
  error_if(i != 1000);  // 10 coroutines * 100 increments
  return error_count;
}

dd::task<size_t> task_mm(int i) {
  auto handle = co_await dd::this_coro::handle;
  (void)handle;
  co_return i;
}
dd::generator<dd::task<size_t>> gen_mm() {
  for (auto i : std::views::iota(0, 10))
    co_yield task_mm(i);
}
dd::async_task<size_t> get_result(auto just_task) {
  co_return co_await just_task;
}
TEST(gen_mm) {
  int i = 0;
  auto gen = gen_mm();
  // TODO check working on rvlaue(or somehow forbide it)
  for (auto task : gen | std::views::filter([](auto&&) { return true; })) {
    error_if(get_result(std::move(task)).get() != i);
    ++i;
  }
  return error_count;
}

TEST(job_mm) {
  std::atomic<size_t> err_c = 0;
  auto job_creator = [&](std::atomic<int32_t>& value) -> dd::job {
    auto th_id = std::this_thread::get_id();
    co_await dd::jump_on(dd::new_thread_executor{});
    if (th_id == std::this_thread::get_id())
      ++err_c;
    value.fetch_add(1, std::memory_order::release);
    if (value.load(std::memory_order::acquire) == 10)
      value.notify_one();
  };
  std::atomic<int32_t> flag = 0;
  {
    auto _ = dd::with_resource(*std::pmr::new_delete_resource());
    for (auto i : std::views::iota(0, 10))
      job_creator(flag), (void)i;
  }
  while (flag.load(std::memory_order::acquire) != 10)
    flag.wait(flag.load(std::memory_order::acquire));
  error_if(flag != 10);
  return error_count + err_c.load();
}

inline dd::event<int> e1 = {};

dd::job sub(std::atomic<int>& count) {
  while (true) {
    int i = co_await e1;
    if (i == 0)
      co_return;
    if (count.fetch_add(i, std::memory_order::acq_rel) == 1999999)
      count.notify_one();
  }
}

dd::logical_thread writer(std::atomic<int>& count) {
  co_await dd::jump_on(dd::new_thread_executor{});
  dd::stop_token tok = co_await dd::this_coro::stop_token;
  for (auto i : std::views::iota(0, 1000)) {
    (void)i;
    sub(count);
    if (tok.stop_requested())
      co_return;
  }
}

dd::logical_thread reader() {
  co_await dd::jump_on(dd::new_thread_executor{});
  dd::stop_token tok = co_await dd::this_coro::stop_token;
  for (;;) {
    e1.notify_all(dd::this_thread_executor{}, 1);
    if (tok.stop_requested())
      co_return;
  }
}

TEST(thread_safety) {
  std::atomic<int> count = 0;
  auto _2 = writer(count);
  auto _4 = reader();
  auto _5 = reader();
  auto _3 = writer(count);
  while (count.load() < 2000000)
    count.wait(count.load());
  stop(_2, _4, _5, _3);
  e1.notify_all(dd::this_thread_executor{}, 0);
  return error_count;
}

inline dd::event<void> one;
inline dd::event<int> two;
inline dd::event<std::vector<std::string>> three;
inline dd::event<void> four;

dd::async_task<void> waiter_any(uint32_t& count) {
  co_await dd::jump_on(dd::new_thread_executor{});
  for (int32_t i : std::views::iota(0, 100000)) {
    (void)i;
    auto variant = co_await dd::when_any(one, two, three, four);
    count++;
  }
}
dd::async_task<void> waiter_all(uint32_t& count) {
  co_await dd::jump_on(dd::new_thread_executor{});
  for (int32_t i : std::views::iota(0, 100000)) {
    (void)i;
    auto tuple = co_await dd::when_all(one, two, three, four);
    if (std::get<2>(tuple) != std::vector<std::string>(3, "hello world"))
      throw false;
    if (std::get<1>(tuple) != 5)
      throw false;
    count++;
  }
}

dd::logical_thread notifier(auto& event) {
  co_await dd::jump_on(dd::new_thread_executor{});
  dd::stop_token token = co_await dd::this_coro::stop_token;
  while (true) {
    event.notify_all(dd::this_thread_executor{});
    if (token.stop_requested())
      co_return;
  }
}

dd::logical_thread notifier(auto& pool, auto input) {
  co_await dd::jump_on(dd::new_thread_executor{});
  dd::stop_token token = co_await dd::this_coro::stop_token;
  while (true) {
    pool.notify_all(dd::this_thread_executor{}, input);
    if (token.stop_requested())
      co_return;
  }
}
TEST(when_any) {
  auto _1 = notifier(one);
  auto _2 = notifier(two, 5);
  auto _3 = notifier(three, std::vector<std::string>(3, "hello world"));
  auto _4 = notifier(four);
  uint32_t count = 0;
  auto anyx = waiter_any(count);
  anyx.wait();
  stop(_1, _2, _3, _4);
  one.notify_all(dd::this_thread_executor{});
  two.notify_all(dd::this_thread_executor{}, 5);
  three.notify_all(dd::this_thread_executor{}, std::vector<std::string>(3, "hello world"));
  four.notify_all(dd::this_thread_executor{});
  error_if(count != 100000);
  return error_count;
}
TEST(when_all) {
  auto _1 = notifier(one);
  auto _2 = notifier(two, 5);
  auto _3 = notifier(three, std::vector<std::string>(3, "hello world"));
  auto _4 = notifier(four);
  uint32_t count = 0;
  auto allx = waiter_all(count);
  allx.wait();
  stop(_1, _2, _3, _4);
  one.notify_all(dd::this_thread_executor{});
  two.notify_all(dd::this_thread_executor{}, 5);
  three.notify_all(dd::this_thread_executor{}, std::vector<std::string>(3, "hello world"));
  four.notify_all(dd::this_thread_executor{});
  error_if(count != 100000);
  return error_count;
}

dd::async_task<std::string> afoo() {
  co_await dd::jump_on(dd::new_thread_executor{});
  co_return "hello world";
}

TEST(async_tasks) {
  std::vector<dd::async_task<std::string>> atasks;
  for (auto i : std::views::iota(0, 1000))
    atasks.emplace_back(afoo()), (void)i;
  for (auto& t : atasks)
    error_if(std::move(t).get() != "hello world");
  return error_count;
}

dd::async_task<void> do_void() {
  co_return;
}
TEST(void_async_task) {
  auto task = do_void();
  task.wait();
  return error_count;
}

dd::task<std::string> do_smth() {
  co_await dd::jump_on(dd::new_thread_executor{});
  co_return "hello from task";
}

dd::async_task<void> tasks_user() {
  std::vector<dd::task<std::string>> vec;
  for (int i = 0; i < 10; ++i)
    vec.emplace_back(do_smth());
  for (int i = 0; i < 8; ++i) {
    std::string result = co_await vec[i];
    if (result != "hello from task")
      throw false;
  }
  co_return;
}

dd::channel<std::tuple<int, double, float>> creator() {
  for (int i = 0; i < 100; ++i) {
    co_await dd::jump_on(dd::new_thread_executor{});
    std::this_thread::sleep_for(std::chrono::microseconds(3));
    co_yield std::tuple{i, static_cast<double>(i), static_cast<float>(i)};
  }
}

dd::async_task<void> channel_tester() {
  auto my_stream = creator();
  int i = 0;
  co_foreach(auto&& v, my_stream) {
    auto tpl = std::tuple{i, static_cast<double>(i), static_cast<float>(i)};
    if (v != tpl)
      throw false;
    ++i;
  }
}

TEST(channel) {
  auto tester = channel_tester();
  tester.wait();
  return error_count;
}

int main() {
  return static_cast<int>(TESTgenerator() + TESTzip_generator() + TESTlogical_thread() +
                          TESTcoroutines_integral() + TESTlogical_thread_mm() + TESTgen_mm() + TESTjob_mm() +
                          TESTthread_safety() + TESTwhen_any() + TESTwhen_all() + TESTasync_tasks() +
                          TESTvoid_async_task() + TESTchannel());
}
