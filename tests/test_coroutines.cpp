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
#include <iostream>
#include <array>
#include <algorithm>
#include <latch>

#include "kelcoro/async_task.hpp"
#include "kelcoro/channel.hpp"
#include "kelcoro/generator.hpp"
#include "kelcoro/job.hpp"
#include "kelcoro/logical_thread.hpp"
#include "kelcoro/task.hpp"
#include "kelcoro/events.hpp"
#include "kelcoro/thread_pool.hpp"
#include "kelcoro/algorithm.hpp"

inline dd::thread_pool TP(8);

// clang had bug which breaks all std::views
#if !defined(__clang_major__) || __clang_major__ >= 15

  #define error_if(Cond) error_count += static_cast<bool>((Cond))
  #define TEST(NAME) inline size_t TEST##NAME(size_t error_count = 0)

inline size_t some_task(int i) {
  return i;
}
inline dd::generator<int> foo() {
  for (int i = 0;; ++i)
    co_yield some_task(i);
}
struct r2 : dd::pmr::polymorphic_resource {};

TEST(allocations) {
  using dd::noexport::padding_len;
  static_assert(padding_len<16>(16) == 0);
  static_assert(padding_len<16>(0) == 0);
  static_assert(padding_len<16>(1) == 15);
  static_assert(padding_len<16>(8) == 8);
  static_assert(padding_len<4>(3) == 1);
  static_assert(padding_len<4>(2) == 2);
  static_assert(padding_len<4>(1) == 3);
  static_assert(padding_len<4>(4) == 0);

  #define EXPECT_PROMISE(promise, ... /* coro arg args*/) \
    static_assert(std::is_same_v<std::coroutine_traits<__VA_ARGS__>::promise_type, promise>);
  using namespace dd;
  using r = pmr::polymorphic_resource;
  {
    static constexpr std::array szs{sizeof(dd::generator<int>), sizeof(dd::generator<float>),
                                    sizeof(dd::generator_r<int, r>), sizeof(dd::generator_r<float, r>)};
    static_assert(std::all_of(begin(szs), end(szs), [](auto x) { return x == szs.front(); }));
  }
  {
    static constexpr std::array szs{sizeof(dd::channel<int>), sizeof(dd::channel<float>),
                                    sizeof(dd::channel_r<int, r>), sizeof(dd::channel_r<float, r>)};
    static_assert(std::all_of(begin(szs), end(szs), [](auto x) { return x == szs.front(); }));
  }
  {
    static constexpr std::array szs{sizeof(std::coroutine_traits<dd::generator<int>>::promise_type),
                                    sizeof(std::coroutine_traits<dd::generator<float>>::promise_type),
                                    sizeof(std::coroutine_traits<dd::generator_r<int, r>>::promise_type),
                                    sizeof(std::coroutine_traits<dd::generator_r<float, r>>::promise_type)};
    static_assert(std::all_of(begin(szs), end(szs), [](auto x) { return x == szs.front(); }));
  }
  {
    static constexpr std::array szs{sizeof(std::coroutine_traits<dd::channel<int>>::promise_type),
                                    sizeof(std::coroutine_traits<dd::channel<float>>::promise_type),
                                    sizeof(std::coroutine_traits<dd::channel_r<int, r>>::promise_type),
                                    sizeof(std::coroutine_traits<dd::channel_r<float, r>>::promise_type)};
    static_assert(std::all_of(begin(szs), end(szs), [](auto x) { return x == szs.front(); }));
  }
  {
    r test_resource;
    auto x0 = with_resource{r{}};
    auto x1 = with_resource{test_resource};
    auto x2 = with_resource{std::as_const(test_resource)};
    static_assert(std::is_same_v<decltype(x0), decltype(x1)>);
    static_assert(std::is_same_v<decltype(x1), decltype(x2)>);
    static_assert(std::is_same_v<decltype(x0), with_resource<r>>);
  }
  struct some_resource {
    void* allocate(size_t);
    void deallocate(void*, size_t) noexcept;
  };
  // resource deduction
  {
    using default_promise = generator_promise<int>;
    using expected = resourced_promise<generator_promise<int>, r>;
    using test_t = generator<int>;
  #define TEST_DEDUCTED                                                                         \
    using some_resource_promise = resourced_promise<default_promise, some_resource>;            \
    EXPECT_PROMISE(default_promise, test_t, int, float, double);                                \
    EXPECT_PROMISE(default_promise, test_t);                                                    \
    EXPECT_PROMISE(expected, test_t, with_resource<r>);                                         \
    EXPECT_PROMISE(expected, test_t, with_resource<r2>, with_resource<r>);                      \
    EXPECT_PROMISE(expected, test_t, float, float, double, int, with_resource<r>);              \
    EXPECT_PROMISE(default_promise, test_t, float, float, double, int, with_resource<r>, int);  \
    EXPECT_PROMISE(some_resource_promise, test_t, float, float, double, int, with_resource<r>&, \
                   with_resource<some_resource>);                                               \
    EXPECT_PROMISE(default_promise, test_t, float, dd::with_default_resource)
    TEST_DEDUCTED;
  }
  {
    using default_promise = channel_promise<int>;
    using expected = resourced_promise<channel_promise<int>, r>;
    using test_t = channel<int>;
    TEST_DEDUCTED;
  }
  {
    using default_promise = job_promise;
    using expected = resourced_promise<job_promise, r>;
    using test_t = job;
    TEST_DEDUCTED;
  }
  {
    using default_promise = logical_thread_promise;
    using expected = resourced_promise<logical_thread_promise, r>;
    using test_t = logical_thread;
    TEST_DEDUCTED;
  }
  {
    using default_promise = task_promise<int, null_context>;
    using expected = resourced_promise<task_promise<int, null_context>, r>;
    using test_t = task<int>;
    TEST_DEDUCTED;
  }
  {
    using default_promise = async_task_promise<int>;
    using expected = resourced_promise<async_task_promise<int>, r>;
    using test_t = async_task<int>;
    TEST_DEDUCTED;
  }
  // concrete resource
  {
    using expected = resourced_promise<generator_promise<int>, r>;
    using test_t = generator_r<int, r>;
  #define TEST_CONCRETE                                                 \
    EXPECT_PROMISE(expected, test_t, int, float, double);               \
    EXPECT_PROMISE(expected, test_t, int, with_resource<r>, double);    \
    EXPECT_PROMISE(expected, test_t, with_resource<r>);                 \
    EXPECT_PROMISE(expected, test_t, const volatile with_resource<r>&); \
    EXPECT_PROMISE(expected, test_t);

    TEST_CONCRETE;
  }
  {
    using expected = resourced_promise<channel_promise<int>, r>;
    using test_t = channel_r<int, r>;
    TEST_CONCRETE;
  }
  {
    using expected = resourced_promise<job_promise, r>;
    using test_t = job_r<r>;
    TEST_CONCRETE;
  }
  {
    using expected = resourced_promise<task_promise<int, null_context>, r>;
    using test_t = task_r<int, r>;
    TEST_CONCRETE;
  }
  {
    using expected = resourced_promise<async_task_promise<int>, r>;
    using test_t = async_task_r<int, r>;
    TEST_CONCRETE;
  }
  {
    using expected = resourced_promise<logical_thread_promise, r>;
    using test_t = logical_thread_r<r>;
    TEST_CONCRETE;
  }
  #undef EXPECT_RPOMISE
  #undef TEST_CONCRETE
  #undef TEST_DEDUCTED
  return error_count;
}
TEST(generator) {
  static_assert(std::ranges::input_range<dd::generator<size_t>&&>);
  static_assert(std::input_iterator<dd::generator_iterator<std::vector<int>>>);
  static_assert(std::ranges::input_range<dd::generator<int>>);
  int i = 1;
  dd::generator gen = foo();
  for (auto value : gen | std::views::take(100) | std::views::filter([](auto v) { return v % 2; })) {
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
  std::string_view sz_view = sz;
  for (auto [a, b, c] : zip(vec, sz, sz_view))
    error_if(a != 20);
  return error_count;
}

inline dd::logical_thread multithread(std::atomic<int32_t>& value) {
  auto handle = co_await dd::this_coro::handle;
  (void)handle;
  auto token = co_await dd::this_coro::stop_token;
  (void)token.stop_requested();
  (void)co_await dd::jump_on(TP);
  for (auto i : std::views::iota(0, 100))
    ++value, (void)i;
}

inline void moo(std::atomic<int32_t>& value, std::pmr::memory_resource* m = std::pmr::new_delete_resource()) {
  std::vector<dd::logical_thread> workers;
  {
    // auto _ = dd::with_resource(*m);
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
  (void)co_await dd::jump_on(TP);
  auto token = co_await dd::this_coro::stop_token;
  while (true) {
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
  static_assert(
      std::is_same_v<decltype(handle), std::coroutine_handle<dd::task_promise<size_t, dd::null_context>>>);
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
  for (auto task : gen | std::views::filter([](auto&&) { return true; })) {
    error_if(get_result(std::move(task)).get() != i);
    ++i;
  }
  return error_count;
}

[[gnu::noinline]] void nocache(auto&) {
}

TEST(job_mm) {
  std::atomic<size_t> err_c = 0;
  auto job_creator = [&](std::atomic<int32_t>& value) -> dd::job {
    auto th_id = std::this_thread::get_id();
    nocache(th_id);
    (void)co_await dd::jump_on(TP);
    if (th_id == std::this_thread::get_id())
      ++err_c;
    value.fetch_add(1, std::memory_order::release);
    if (value.load(std::memory_order::acquire) == 10)
      value.notify_one();
  };
  std::atomic<int32_t> flag = 0;
  {
    // auto _ = dd::with_resource(*std::pmr::new_delete_resource());
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
  (void)co_await dd::jump_on(TP);
  dd::stop_token tok = co_await dd::this_coro::stop_token;
  for (auto i : std::views::iota(0, 1000)) {
    (void)i;
    sub(count);
    if (tok.stop_requested())
      co_return;
  }
}

dd::logical_thread reader() {
  (void)co_await dd::jump_on(TP);
  dd::stop_token tok = co_await dd::this_coro::stop_token;
  for (;;) {
    e1.notify_all(dd::this_thread_executor, 1);
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
  e1.notify_all(dd::this_thread_executor, 0);
  return error_count;
}

inline dd::event<void> one;
inline dd::event<int> two;
inline dd::event<std::vector<std::string>> three;
inline dd::event<void> four;

dd::async_task<void> waiter_any(uint32_t& count) {
  (void)co_await dd::jump_on(TP);
  std::mutex m;
  int32_t i = 0;
  while (i < 100000) {
    auto variant = co_await dd::when_any(one, two, three, four);
    std::lock_guard l(m);
    count++;
    ++i;
  }
}
dd::async_task<void> waiter_all(uint32_t& count) {
  (void)co_await dd::jump_on(TP);
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
  co_await dd::jump_on(TP);
  dd::stop_token token = co_await dd::this_coro::stop_token;
  while (true) {
    event.notify_all(dd::this_thread_executor);
    if (token.stop_requested())
      co_return;
  }
}

dd::logical_thread notifier(auto& pool, auto input) {
  co_await dd::jump_on(TP);
  dd::stop_token token = co_await dd::this_coro::stop_token;
  while (true) {
    pool.notify_all(dd::this_thread_executor, input);
    if (token.stop_requested())
      co_return;
  }
}

dd::async_task<std::string> afoo() {
  (void)co_await dd::jump_on(TP);
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
  (void)co_await dd::jump_on(TP);
  co_return "hello from task";
}

TEST(task_start_and_detach) {
  std::atomic_bool flag = false;
  dd::task task = [](std::atomic_bool& flag) -> dd::task<void> {
    (void)co_await dd::jump_on(TP);
    flag.exchange(true);
    (void)co_await dd::jump_on(TP);
    dd::scope_exit e = [&] { flag.notify_one(); };
  }(flag);
  task.start_and_detach();
  error_if(!task.empty());
  flag.wait(false);
  int x = 0;
  dd::task task2 = [](int& x) -> dd::task<void> {
    x = 42;
    co_return;
  }(x);
  error_if(x != 0);
  std::coroutine_handle h = task2.start_and_detach(/*stop_at_end=*/true);
  error_if(!task2.empty());
  error_if(x != 42);
  error_if(!h.done());
  h.destroy();
  return error_count;
}

TEST(task_blocking_wait) {
  error_if(do_smth().get() != "hello from task");
  return error_count;
}

dd::task<std::string> do_throw() {
  (void)co_await dd::jump_on(TP);
  throw 42;
}

TEST(task_with_exception) {
  dd::task t = do_throw();
  try {
    t.get();
  } catch (int x) {
    error_if(x != 42);
    return error_count;
  }
  error_if(true);
  return error_count;
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
    (void)co_await dd::jump_on(TP);
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

dd::async_task<int> small_task() {
  (void)co_await dd::jump_on(TP);
  co_return 1;
}

TEST(detached_tasks) {
  std::vector<dd::async_task<int>> tasks;
  for (int i = 0; i < 3'000; ++i) {
    auto& t = tasks.emplace_back(small_task());
    if (rand() % 2)
      t.detach();
  }
  for (auto& x : tasks) {
    if (x.empty())
      continue;
    error_if(std::move(x).get() != 1);
  }
  return error_count;
}

struct ctx {
  std::vector<std::string>* s = nullptr;
  std::string name;

  template <typename R1, typename R2>
  void on_owner_setted(std::coroutine_handle<dd::task_promise<R1, ctx>> owner,
                       std::coroutine_handle<dd::task_promise<R2, ctx>> task) noexcept {
    s = owner.promise().ctx.s;
  }
  template <typename P1, typename P2>
  void on_owner_setted(std::coroutine_handle<P1>, std::coroutine_handle<P2>) noexcept {
  }
  template <typename TaskPromise>
  void on_start(std::coroutine_handle<TaskPromise> task) noexcept {
    if (s)
      s->push_back(name);
  }
  template <typename TaskPromise>
  void on_end(std::coroutine_handle<TaskPromise>) noexcept {
    if (s && !s->empty())
      s->pop_back();
  }
};

inline std::vector<std::string> locations;

dd::task<std::string, ctx> ctx_user2() {
  auto& ctx = co_await dd::this_coro::context;
  if (ctx.s != &locations || ctx.s->size() != 3)
    throw 42;
  co_return "abc";
}

dd::task<float, ctx> ctx_user() {
  auto& ctx = co_await dd::this_coro::context;
  if (ctx.s != &locations || ctx.s->size() != 2 || ctx.s->front() != "task 1")
    throw 42;
  std::string s = co_await ctx_user2();
  if (s != "abc")
    throw 43;
  co_return 3.14f;
}

dd::task<int, ctx> ctxed_foo() {
  // init my context
  auto& ctx = co_await dd::this_coro::context;
  ctx.s = &locations;
  ctx.s->push_back("task 1");
  float f = co_await ctx_user();
  if (f != 3.14f)
    throw 11;
  co_return 0;
}

TEST(contexted_task) {
  auto h = ctxed_foo().start_and_detach(/*stop_at_end=*/true);
  error_if(h.promise().exception);
  error_if(!locations.empty());
  h.destroy();
  return error_count;
}

// checks that 'return_value' may be used with non movable types
dd::task<std::tuple<int, std::string, std::vector<int>, std::unique_ptr<int>>> complex_ret(int i) {
  switch (i) {
    case 0:
      co_return {5, std::string(""), std::vector<int>{5, 10}, std::unique_ptr<int>(nullptr)};
    case 1: {
      std::tuple<int, std::string, std::vector<int>, std::unique_ptr<int>> v{
          5, std::string(""), std::vector<int>{5, 10}, std::unique_ptr<int>(nullptr)};
      ;
      co_return v;
    }
    default:
      co_return {};
  }
}
TEST(complex_ret) {
  complex_ret(0).start_and_detach();
  complex_ret(1).start_and_detach();
  return error_count;
}

dd::task<int> task_fast_value() {
  co_return 42;
}

dd::task<std::string> task_value() {
  (void)co_await dd::jump_on(TP);
  co_return "hello";
}

dd::task<std::string> task_throw() {
  (void)co_await dd::jump_on(TP);
  throw std::runtime_error("err");
}

dd::task<void> task_fast_throw() {
  throw 4;
  co_return;
}

dd::task<size_t> waiter_of_all() {
  size_t error_count = 0;
  (void)co_await dd::jump_on(TP);
  auto [a, b, c, d] = co_await dd::when_all(task_fast_value(), task_value(), task_throw(), task_fast_throw());
  error_if(!a || *a != 42);
  error_if(!b || *b != "hello");
  error_if(c);
  error_if(d);
  co_return error_count;
}
TEST(when_all_same_ctx) {
  return waiter_of_all().get();
}
TEST(when_all_dynamic) {
  std::vector<dd::task<size_t>> tasks;
  for (int i = 0; i < 30; ++i)
    tasks.push_back(waiter_of_all());
  for (auto& x : dd::when_all(std::move(tasks)).get()) {
    error_if(!x);
    error_count += *x;
  }
  return error_count;
}

TEST(when_any) {
  // basic cases
  {
    auto x = dd::when_any(task_fast_throw()).get();
    error_if(x.index() != 1);
  }
  {
    auto x = dd::when_any(task_fast_value()).get();
    error_if(x.index() != 1);
  }
  // full failure cases
  {
    auto y = dd::when_any(task_fast_throw(), task_fast_throw(), task_fast_throw()).get();
    error_if(y.index() != 3);  // must return last exception
  }
  {
    auto y = dd::when_any(task_fast_throw(), task_throw(), task_fast_throw()).get();
    // may be 2, but its race, dont know
    error_if(y.index() == 0);
  }
  {
    auto y = dd::when_any(task_throw(), task_throw(), task_fast_throw()).get();
    error_if(y.index() == 0);
  }
  {
    auto y = dd::when_any(task_fast_throw(), task_throw(), task_throw()).get();
    error_if(y.index() == 0);
  }
  {
    auto y = dd::when_any(task_throw(), task_throw(), task_throw()).get();
    error_if(y.index() == 0);
  }
  // party failed cases, one pretendent
  {
    auto y = dd::when_any(task_throw(), task_throw(), task_value()).get();
    error_if(y.index() != 3);
  }
  {
    auto y = dd::when_any(task_throw(), task_value(), task_throw()).get();
    error_if(y.index() != 2);
  }
  {
    auto y = dd::when_any(task_value(), task_throw(), task_throw()).get();
    error_if(y.index() != 1);
  }
  {
    auto y = dd::when_any(task_throw(), task_throw(), task_fast_value()).get();
    error_if(y.index() != 3);
  }
  {
    auto y = dd::when_any(task_throw(), task_fast_value(), task_throw()).get();
    error_if(y.index() != 2);
  }
  {
    auto y = dd::when_any(task_value(), task_fast_throw(), task_throw()).get();
    error_if(y.index() != 1);
  }
  // partial fail many pretendenst cases
  {
    auto y = dd::when_any(task_throw(), task_fast_value(), task_value()).get();
    error_if(y.index() != 2);
  }
  {
    auto y = dd::when_any(task_throw(), task_value(), task_fast_value()).get();
    error_if(y.index() == 0);  // dont know order
  }
  {
    auto y = dd::when_any(task_fast_value(), task_value(), task_throw()).get();
    error_if(y.index() != 1);
  }
  // full success cases
  {
    auto y = dd::when_any(task_fast_value(), task_fast_value(), task_fast_value()).get();
    error_if(y.index() != 1);
  }
  {
    auto y = dd::when_any(task_fast_value(), task_fast_value(), task_value()).get();
    error_if(y.index() != 1);
  }
  {
    auto y = dd::when_any(task_value(), task_value(), task_value()).get();
    error_if(y.index() == 0);
  }

  return error_count;
}

dd::task<std::string, ctx> task_throw_cxted() {
  (void)co_await dd::jump_on(TP);
  throw std::runtime_error("err");
}

TEST(when_any_different_ctxts) {
  dd::task t = dd::when_any(task_throw_cxted(), task_value());
  ctx* ctx = t.get_context();
  error_if(!ctx);
  std::variant result = t.get();
  error_if(result.index() != 2);
  return error_count;
}

dd::task<std::string> rvo_task() {
  (void)co_await jump_on(TP);
  std::string& ret = co_await dd::this_coro::return_place;
  ret = "hello world";
  co_return dd::rvo;
}

dd::task<int&> rvo_task_ref(int& x) {
  (void)co_await jump_on(TP);
  int*& ret = co_await dd::this_coro::return_place;
  ret = &x;
  co_return dd::rvo;
}

struct test_mem_resource : std::pmr::memory_resource {
  void* do_allocate(size_t bytes, size_t align) override {
    return std::pmr::new_delete_resource()->allocate(bytes, align);
  }
  void do_deallocate(void* ptr, size_t bytes, size_t align) override {
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, align);
  }
  bool do_is_equal(const memory_resource&) const noexcept override {
    return false;
  }
};

dd::generator<int> gen_with_alloc(std::string val, dd::with_pmr_resource = *std::pmr::new_delete_resource()) {
  for (alignas(64) int i = 0; i < 100; ++i)
    co_yield i;
}

TEST(gen_with_alloc) {
  test_mem_resource res;
  for (int x : gen_with_alloc("hello", res))
    ;
  return error_count;
}

TEST(rvo_tasks) {
  error_if(rvo_task().get() != "hello world");
  int x = 0;
  // TODO simplify, here clang bug (incorrect warning)
  // error_if(&rvo_task_ref(x).get() != &x);
  return error_count;
}

inline std::latch detach_task_latch(1);

dd::task<void> detach_task(std::string& s) {
  s += " world";
  detach_task_latch.count_down();
  co_return;
}

TEST(detached_task) {
  std::string s = "hello";
  TP.schedule(detach_task(s).detach());
  detach_task_latch.wait();
  error_if(s != "hello world");
  return error_count;
}

TEST(expected_e) {
  using et = dd::expected<int, std::exception_ptr>;
  et e;
  error_if(!e);  // default constructed int
  et e2(5);
  error_if(!e2 || *e2 != 5);
  e = e2;
  error_if(!e2 || !e2 || *e2 != *e || *e2 != 5);
  dd::expected e3 = std::move(e2);
  static_assert(std::is_same_v<decltype(e3), et>);  // deduction guide
  error_if(!e2 || !e3 || *e3 != 5);
  e2 = std::move(e3);
  error_if(!e2 || !e3 || *e2 != 5);
  // moved out state also contains value
  return error_count;
}

  #define RUN(TEST_NAME)                             \
    {                                                \
      std::cout << "- " << #TEST_NAME << std::flush; \
      size_t c = TEST##TEST_NAME();                  \
      if (c > 0) {                                   \
        std::cerr << " FAIL " << c << '\n';          \
        ec += c;                                     \
      } else {                                       \
        std::cout << " +" << '\n';                   \
      }                                              \
    }

  #define CHECK_ALIGN(sz, pad, expected) static_assert(::dd::noexport::padding_len<pad>(sz) == expected);

CHECK_ALIGN(0, 8, 0);
CHECK_ALIGN(16, 16, 0);
CHECK_ALIGN(15, 16, 1);
CHECK_ALIGN(15, 8, 1);
CHECK_ALIGN(15, 1, 0);
CHECK_ALIGN(1024, 256, 0);
CHECK_ALIGN(1023, 256, 1);
CHECK_ALIGN(0, 1, 0);
CHECK_ALIGN(1, 1, 0);
CHECK_ALIGN(0, 2, 0);
CHECK_ALIGN(1, 2, 1);
CHECK_ALIGN(0, 8, 0);
CHECK_ALIGN(0, 1, 0);
CHECK_ALIGN(16, 16, 0);
CHECK_ALIGN(1024, 256, 0);
CHECK_ALIGN(15, 16, 1);
CHECK_ALIGN(15, 8, 1);
CHECK_ALIGN(15, 1, 0);
CHECK_ALIGN(1023, 256, 1);
CHECK_ALIGN(54321, 256, 207);
CHECK_ALIGN(12345, 100, 55);
CHECK_ALIGN(100000, 1024, 352);
CHECK_ALIGN(65535, 32768, 1);
CHECK_ALIGN(65535, 65536, 1);
CHECK_ALIGN(999, 11, 2);
CHECK_ALIGN(1999, 33, 14);
CHECK_ALIGN(1048575, 4096, 1);
CHECK_ALIGN(500000, 123, 118);
CHECK_ALIGN(432, 10033, (10033 - 432));
CHECK_ALIGN(5, 63, (63 - 5));

dd::task<void> throw_smth() {
  throw 42;
  co_return;
}

TEST(task_throw) {
  auto t = throw_smth();
  try {
    t.start_and_detach();
    error_if(true);
  } catch (int i) {
    error_if(i != 42);
  } catch (...) {
    error_if(true);
  }
  auto t2 = throw_smth();
  try {
    auto h = t2.start_and_detach(/*stop_at_end=*/true);
    error_if(h.promise().exception == nullptr);
  } catch (...) {
    error_if(true);
  }
  return error_count;
}

int main() {
  // default constructible for empty typpes
  (void)dd::chunk_from<dd::new_delete_resource>{};
  dd::with_resource<statefull_resource> r;
  auto copy(r);
  auto mv(std::move(r));
  auto copy2 = std::as_const(r);
  srand(time(0));
  size_t ec = 0;
  RUN(task_throw);
  RUN(expected_e);
  RUN(generator);
  RUN(zip_generator);
  RUN(logical_thread);
  RUN(coroutines_integral);
  RUN(logical_thread_mm);
  RUN(gen_mm);
  RUN(job_mm);
  RUN(async_tasks);
  RUN(void_async_task);
  RUN(channel);
  RUN(allocations);
  RUN(detached_tasks);
  RUN(task_blocking_wait);
  RUN(task_with_exception);
  RUN(task_start_and_detach);
  RUN(contexted_task);
  RUN(complex_ret);
  RUN(when_all_same_ctx);
  RUN(when_any);
  RUN(when_all_dynamic);
  RUN(when_any_different_ctxts);
  RUN(rvo_tasks);
  RUN(gen_with_alloc);
  RUN(detached_task);
  return ec;
}
#else
int main() {
  return 0;
}
#endif  // clang bug
