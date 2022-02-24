
#ifndef TEST_KEL_COROUTINES_HPP
#define TEST_KEL_COROUTINES_HPP

#include <atomic>
#include <cassert>
#include <coroutine>
#include <iterator>
#include <memory_resource>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_kel_base.hpp"

import kel.coro;
import kel.traits;

namespace kel::test {

struct one {};
struct two {
  using input_type = int;
};
struct three {};
struct four {};

}  // namespace kel::test

namespace kel {

template <>
struct event_traits<named_tag<"Event">> {
  using input_type = int;
};

template <>
struct event_traits<test::three> {
  using input_type = std::vector<std::string>;
};

}  // namespace kel

namespace test {

template <typename Name>
inline constinit kel::event_t<Name> event{};

struct test_selector {
  template <typename Event>
  auto& operator()(std::type_identity<Event>) const noexcept {
    return ::test::event<Event>;
  }
};

}  // namespace test

namespace kel::test {

inline task<size_t> Task(int i) {
  auto& p = co_await this_coro::promise;
  auto handle = co_await this_coro::handle;
  co_return i;
}
inline generator<int> Foo() {
  auto& p = co_await this_coro::promise;
  auto handle = co_await this_coro::handle;
  for (int i = 0;; ++i)
    co_yield co_await Task(i);
}

TEST(Generator) {
  static_assert(std::ranges::input_range<kel::generator<size_t>&&>);
  static_assert(std::input_iterator<kel::generator<std::vector<int>>::iterator>);
  static_assert(std::ranges::output_range<kel::generator<int>, int> &&
                std::ranges::input_range<kel::generator<int>>);
  int i = 1;
  for (auto gen = Foo();
       auto value : gen | ::std::views::take(100) | std::views::filter([](auto v) { return v % 2; })) {
    verify(value == i);
    i += 2;
  }
}

void split_str(std::string_view s, char delimiter, generator<std::string_view> consumator) {
  for (auto& out : consumator) {
    const auto sub_end = s.find(delimiter);
    out = s.substr(0, sub_end);
    if (sub_end != std::string_view::npos)
      s.remove_prefix(sub_end + 1);
    else
      break;
  }
}

TEST(Consumator) {
  split_str("h h h h h", ' ',
            make_consumator<std::string_view>([](std::string_view s) { verify(s == "h"); }));
}
// clang-format off
template<std::ranges::borrowed_range... Ranges>
auto zip(Ranges&&... rs)->generator<decltype(std::tie(std::declval<Ranges>()[0]...))> {
  for (size_t i = 0; ((i < rs.size()) && ...); ++i)
      co_await yield(rs[i]...);
}

TEST(ZIP) {
  std::vector<size_t> vec;
  vec.resize(12, 20);
  std::string sz = "Hello world";
  for (auto [a,b,c] : zip(vec, sz, std::string_view(sz))) {
      verify(a == 20);
  }
}
// clang-format on

// x and y are shown, not yields away. Any lvalue in kel::generator will be yielded like this,
// rvalues will be transformed into generator::value_type and stored until generator resumes
// this solves several problems:
//		better perfomance(no copy) / move
//		making yield_value always noexcept for lvalues(important for coroutines)
//		yielding an references
//		interesting applications on consumer side
inline generator<const int> ViewGenerator() {
  int x = 1;
  int y = 2;
  while (x < 100) {
    co_await yield(++x);
    co_yield y;
  }
}

TEST(ViewGenerator) {
  std::set<const int*> addresses;
  for (auto& i : ViewGenerator())
    addresses.emplace(&i);
  verify(addresses.size() == 2);
}
template <typename MemoryResource>
inline logical_thread_mm<MemoryResource> Multithread(std::allocator_arg_t, MemoryResource,
                                                     std::atomic<int32_t>& value) {
  auto& p = co_await this_coro::promise;
  auto handle = co_await this_coro::handle;
  co_await jump_on(another_thread);
  for (auto i : std::views::iota(0, 100))
    ++value;
}
template <typename MemoryResource = std::allocator<std::byte>>
inline void Moo(std::atomic<int32_t>& value) {
  std::vector<logical_thread_mm<MemoryResource>> workers;
  for (int i = 0; i < 10; ++i)
    workers.emplace_back(Multithread<MemoryResource>(std::allocator_arg, MemoryResource{}, value));
  stop(workers);  // more effective then just dctors for all
}
TEST(LogicalThread) {
  std::atomic<int32_t> i;
  Moo(i);
  verify(i == 1000);  // 10 coroutines * 100 increments
}

kel::logical_thread Bar(bool& requested) {
  auto& promise = co_await this_coro::promise;
  co_await jump_on(another_thread);
  auto token = co_await this_coro::stop_token;
  while (true) {
    std::this_thread::sleep_for(std::chrono::microseconds(5));
    if (token.stop_requested()) {
      requested = true;
      co_return;
    }
  }
}
TEST(CoroutinesIntegral) {
  bool is_requested = false;
  Bar(is_requested);
  verify(is_requested == true);
}

struct statefull_resource {
  size_t sz = 0;
  // sizeof of this thing affects frame size with 2 multiplier bcs its saved in frame + saved for coroutine
  void* allocate(size_t size) {
    sz = size;
    return ::operator new(size);
  }
  void deallocate(void* ptr, size_t size) noexcept {
    assert(sz == size);  // cant throw here(std::terminate)
    ::operator delete(ptr, size);
  }
};

TEST(LogicalThreadMM) {
  std::atomic<int32_t> i;
  Moo<statefull_resource>(i);
  verify(i == 1000);  // 10 coroutines * 100 increments
}

task<size_t, statefull_resource> TaskMM(int i, statefull_resource) {
  auto& p = co_await this_coro::promise;
  co_return i;
}
generator<task<size_t, statefull_resource>, std::allocator<std::byte>> GenMM() {
  for (auto i : std::views::iota(0, 10))
    co_yield TaskMM(i, statefull_resource{0});
}
async_task<size_t> GetResult(auto just_task) {
  co_return co_await just_task;
}
TEST(GenMM) {
  int i = 0;
  for (auto gen = GenMM(); auto& task : gen | std::views::filter([](auto&&) { return true; })) {
    verify(GetResult(std::move(task)).get() == i);
    ++i;
  }
}

TEST(JobMM) {
  auto job_creator = [](std::atomic<int32_t>& value,
                        std::pmr::memory_resource*) -> job_mm<std::pmr::polymorphic_allocator<std::byte>> {
    auto th_id = std::this_thread::get_id();
    co_await jump_on(another_thread);
    verify(th_id != std::this_thread::get_id());
    value.fetch_add(1, std::memory_order::release);
    if (value.load(std::memory_order::relaxed) == 10)
      value.notify_one();
  };
  std::atomic<int32_t> flag = 0;
  for (auto i : std::views::iota(0, 10))
    job_creator(flag, std::pmr::new_delete_resource());
  while (flag.load(std::memory_order::acquire) != 10)
    flag.wait(flag.load(std::memory_order::relaxed));
  verify(flag == 10);
}

bool Foooo(int a, int b, std::function<bool(int&, int)> f, int c) {
  int value = 10;
  // POINT 2: invoke a Foooo from co_await call
  auto result = f(value, a + b + c);
  verify(value == 20);
  // POINT 5: after coroutine suspends we are here
  verify(result == true);
  return result;
}

TEST(Call) {
  // usually it used for async callbacks, but it is possible to use for synchronous callbacks too
  []() -> job {
    // POINT 1: entering
    // may auto or auto&&, value / summ / ret is a references to arguments in Foooo
    auto [value, summ, ret] = co_await this_coro::invoked_in(Foooo, 10, 15, signature<bool(int&, int)>{}, 20);
    verify(value == 10);
    value = 20;
    verify(summ == 45);
    // POINT 3: part of the coroutine(from co_await call to next suspend) becomes callback for Foooo
    ret = true;  // setting return value, because Foooo expects it from callback
    // POINT 4: first coroutine suspend after co_await call
  }();
}

job sub(std::atomic<int>& count) {
  while (true) {
    int i = co_await event<named_tag<"Event">>;
    if (i == 0)
      co_return;
    if (count.fetch_add(i, std::memory_order::relaxed) == 1999999)
      count.notify_one();
  }
}

logical_thread writer(std::atomic<int>& count) {
  co_await jump_on(another_thread);
  for (auto i : std::views::iota(0, 1000)) {
    sub(count);
    co_await quit_if_requested;
  }
}

logical_thread reader() {
  co_await jump_on(another_thread);

  for (;;) {
    event<named_tag<"Event">>.notify_all(this_thread_executor{}, 1);
    co_await quit_if_requested;
  }
}

TEST(ThreadSafety) {
  std::atomic<int> count = 0;
  auto _2 = writer(count);
  auto _4 = reader();
  auto _5 = reader();
  auto _3 = writer(count);
  while (count.load() < 2000000)
    count.wait(count.load());
  stop(_2, _4, _5, _3);
  event<named_tag<"Event">>.notify_all(this_thread_executor{}, 0);
}

async_task<void> waiter_any(uint32_t& count) {
  co_await jump_on(another_thread);
  for (int32_t i : std::views::iota(0, 100000)) {
    auto variant = co_await when_any<one, two, three, four>(::test::test_selector{});
    assert(variant.index() != 0);  // something happens
    count++;
  }
}
async_task<void> waiter_all(uint32_t& count) {
  co_await jump_on(another_thread);
  for (int32_t i : std::views::iota(0, 100000)) {
    auto tuple = co_await when_all<one, two, three, four>(::test::test_selector{});
    assert(std::get<2>(tuple) == std::vector<std::string>(3, "hello world"));
    assert(std::get<1>(tuple) == 5);
    count++;
  }
}
template <typename Event>
logical_thread notifier(auto& pool) {
  co_await jump_on(another_thread);
  while (true) {
    pool.notify_all<Event>();
    co_await quit_if_requested;
  }
}
template <typename Event>
logical_thread notifier(auto& pool, auto input) {
  co_await jump_on(another_thread);
  while (true) {
    pool.notify_all<Event>(input);
    co_await quit_if_requested;
  }
}
TEST(WhenAny) {
  event_pool<this_thread_executor, ::test::test_selector> pool;
  auto _1 = notifier<one>(pool);
  auto _2 = notifier<two>(pool, 5);
  auto _3 = notifier<three>(pool, std::vector<std::string>(3, "hello world"));
  auto _4 = notifier<four>(pool);
  uint32_t count = 0;
  auto anyx = waiter_any(count);
  anyx.wait();
  stop(_1, _2, _3, _4);
  pool.notify_all<one>();
  pool.notify_all<two>(5);
  pool.notify_all<three>(std::vector<std::string>(3, "hello world"));
  pool.notify_all<four>();
  verify(count == 100000);
}
TEST(WhenAll) {
  event_pool<this_thread_executor, ::test::test_selector> pool;
  auto _1 = notifier<one>(pool);
  auto _2 = notifier<two>(pool, 5);
  auto _3 = notifier<three>(pool, std::vector<std::string>(3, "hello world"));
  auto _4 = notifier<four>(pool);
  uint32_t count = 0;
  auto allx = waiter_all(count);
  allx.wait();
  stop(_1, _2, _3, _4);
  pool.notify_all<one>();
  pool.notify_all<two>(5);
  pool.notify_all<three>(std::vector<std::string>(3, "hello world"));
  pool.notify_all<four>();
  verify(count == 100000);
}

async_task<std::string> Afoo() {
  co_await jump_on(another_thread);
  co_return "hello world";
}

TEST(AsyncTasks) {
  std::vector<async_task<std::string>> atasks;
  for (auto i : std::views::iota(0, 1000))
    atasks.emplace_back(Afoo());
  for (auto& t : atasks)
    if (rand() % 1000 > 900)
      verify(t.get() == "hello world");
}

async_task<void> DoVoid() {
  co_return;
}
TEST(VoidAsyncTask) {
  auto task = DoVoid();
  task.wait();
}

task<std::string> DoSmth() {
  co_await jump_on(another_thread);
  co_return "hello from task";
}

async_task<void> TasksUser() {
  std::vector<task<std::string>> vec;
  for (int i = 0; i < 10; ++i)
    vec.emplace_back(DoSmth());
  for (int i = 0; i < 8; ++i) {
    std::string result = co_await vec[i];
    verify(result == "hello from task");
  }
  co_return;
}

channel<std::tuple<int, double, float>> Creator() {
  for (int i = 0; i < 100; ++i) {
    co_await jump_on(another_thread);
    std::this_thread::sleep_for(std::chrono::microseconds(3));
    co_await yield{i, static_cast<double>(i), static_cast<float>(i)};
  }
}

async_task<void> ChannelTester() {
  auto my_stream = Creator();
  int i = 0;
  while (auto* v = co_await my_stream) {
    auto tpl = std::tuple{i, static_cast<double>(i), static_cast<float>(i)};
    verify(*v == tpl);
    ++i;
  }
}

TEST(Channel) {
  auto tester = ChannelTester();
  tester.wait();
}
}  // namespace kel::test

#endif  // !TEST_KEL_COROUTINES_HPP
