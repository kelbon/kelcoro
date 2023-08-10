
#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif

#include "generator.hpp"
#include "channel.hpp"
#include "async_task.hpp"

#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <ranges>

#define error_if(Cond) error_count += static_cast<bool>((Cond))
#define TEST(NAME) inline size_t TEST##NAME(size_t error_count = 0)
#define CHANNEL_TEST(NAME) inline dd::async_task<size_t> NAME(size_t error_count = 0)
#define CO_TEST(NAME) \
  TEST(NAME) { return NAME().get(); }
#define CHAN_OR_GEN template <template <typename> typename G>
#define RANDOM_CONTROL_FLOW                               \
  if constexpr (std::is_same_v<G<int>, dd::channel<int>>) \
    if (flip())                                           \
  co_await dd::jump_on(dd::new_thread_executor{})

static bool flip() {
  static thread_local std::mt19937 rng = [] {
    auto seed = std::random_device{}();
    std::clog << "SEED: " << seed << std::endl;
    return std::mt19937{seed};
  }();
  return std::bernoulli_distribution(0.5)(rng);
}

static_assert(std::input_iterator<dd::generator_iterator<int>>);
TEST(empty) {
  dd::generator<int> g;
  error_if(!g.empty());
  for (auto x : g)
    error_if(true);
  return error_count;
}
CHAN_OR_GEN
G<int> base_case() {
  (void)co_await dd::this_coro::handle;
  for (int i = 0; i < 100; ++i) {
    RANDOM_CONTROL_FLOW;
    co_yield i;
  }
}
TEST(base) {
  int j = 0;
  for (int i : base_case<dd::generator>()) {
    error_if(j != i);
    ++j;
  }
  return error_count;
}
TEST(base2) {
  std::vector<int> vec;
  auto g = base_case<dd::generator>();
  while (!g.empty()) {
    for (int i : g) {
      vec.push_back(i);
      if (flip())
        break;
    }
  }
  std::vector check(100, 0);
  std::iota(begin(check), end(check), 0);
  error_if(vec != check);
  return error_count;
}
CHANNEL_TEST(base_channel) {
  int j = 0;
  auto c = base_case<dd::channel>();
  co_foreach(int i, c) {
    error_if(j != i);
    ++j;
  }
  co_return error_count;
}
CO_TEST(base_channel);

CHANNEL_TEST(empty_channel) {
  dd::channel<int> g;
  error_if(!g.empty());
  co_foreach(auto&& x, g) error_if(true);
  co_return error_count;
}
CO_TEST(empty_channel);

CHAN_OR_GEN
G<int> g1() {
  for (int i = 0; i < 98; ++i) {
    RANDOM_CONTROL_FLOW;
    co_yield i;
  }
  const int i = 98;
  RANDOM_CONTROL_FLOW;
  co_yield i;
  const int j = 99;
  RANDOM_CONTROL_FLOW;
  co_yield std::move(j);
}
TEST(reuse) {
  dd::generator g = g1<dd::generator>();
  std::vector<int> v;
  while (!g.empty()) {
    for (int&& x : g) {
      v.push_back(x);
      if (flip())
        break;
    }
  }
  std::vector<int> check(100);
  std::iota(begin(check), end(check), 0);
  error_if(check != v);
  return error_count;
}
CHANNEL_TEST(reuse_channel) {
  dd::channel g = g1<dd::channel>();
  std::vector<int> v;
  while (!g.empty()) {
    co_foreach(int x, g) {
      v.push_back(x);
      if (flip())
        break;
    }
  }
  std::vector<int> check(100);
  std::iota(begin(check), end(check), 0);
  error_if(check != v);
  co_return error_count;
}
CO_TEST(reuse_channel);

CHAN_OR_GEN
static G<int> g2() {
  RANDOM_CONTROL_FLOW;
  co_yield dd::elements_of(std::move([]() -> G<int> { co_return; }()));
  RANDOM_CONTROL_FLOW;
}
TEST(empty_recursive) {
  for (int x : g2<dd::generator>())
    error_if(true);
  return error_count;
}

CHANNEL_TEST(empty_recursive_channel) {
  dd::channel c = g2<dd::channel>();
  co_foreach(int&& _, c) error_if(true);
  co_return error_count;
}
CO_TEST(empty_recursive_channel);

CHAN_OR_GEN
static G<int> g10(int i) {
  if (i < 10) {
    RANDOM_CONTROL_FLOW;
    co_yield dd::elements_of(g10<G>(i + 1));
    RANDOM_CONTROL_FLOW;
  }
}
TEST(empty_recursive2) {
  for (int x : g10<dd::generator>(0))
    error_if(true);
  return error_count;
}

CHANNEL_TEST(empty_recursive2_channel) {
  co_foreach(int x, g10<dd::channel>(0)) error_if(true);
  co_return error_count;
}
CO_TEST(empty_recursive2_channel);

CHAN_OR_GEN
static G<int> g3(int i) {
  if (i < 10) {
    RANDOM_CONTROL_FLOW;
    co_yield dd::elements_of(g3<G>(i + 1));
    RANDOM_CONTROL_FLOW;
  }
  RANDOM_CONTROL_FLOW;
  co_yield i;
  RANDOM_CONTROL_FLOW;
}

TEST(recursive) {
  std::vector<int> vec;
  auto g = g3<dd::generator>(0);
  for (int i : g)
    vec.push_back(i);
  error_if((vec != std::vector{10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}));
  error_if(!g.empty());
  for (int i : g)
    error_if(true);
  return error_count;
}
CHANNEL_TEST(recursive_channel) {
  std::vector<int> vec;
  auto g = g3<dd::channel>(0);
  co_foreach(int i, g) vec.push_back(i);
  error_if((vec != std::vector{10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}));
  error_if(!g.empty());
  co_foreach(int i, g) error_if(true);
  co_return error_count;
}
CO_TEST(recursive_channel);

CHAN_OR_GEN
G<int> g5(int& i) {
  co_yield dd::elements_of([]() -> G<int> {
    for (int i = 0; i < 15; ++i) {
      RANDOM_CONTROL_FLOW;
      co_yield -1;
      RANDOM_CONTROL_FLOW;
    }
  }());
  RANDOM_CONTROL_FLOW;
  for (; i < 15; ++i) {
    RANDOM_CONTROL_FLOW;
    co_yield i;
    RANDOM_CONTROL_FLOW;
  }
  RANDOM_CONTROL_FLOW;
}

CHAN_OR_GEN
G<int> g4(int& i) {
  co_yield dd::elements_of([]() -> G<int> {
    for (int i = 1; i < 15; ++i) {
      RANDOM_CONTROL_FLOW;
      co_yield -i;
      RANDOM_CONTROL_FLOW;
    }
  }());
  for (int x = i; i < x + 15; ++i) {
    RANDOM_CONTROL_FLOW;
    co_yield i;
  }
  // make sure it behaves as empty range
  co_yield dd::elements_of(G<int>{});
  RANDOM_CONTROL_FLOW;
  if (i < 300) {
    RANDOM_CONTROL_FLOW;
    co_yield dd::elements_of(g4<G>(i));
    RANDOM_CONTROL_FLOW;
  }
}

TEST(reuse_recursive) {
  int i = 0;
  dd::generator g = g4<dd::generator>(i);
  std::vector<int> v;
  while (!g.empty()) {
    for (int x : g) {
      v.push_back(x);
      if (flip())
        break;
    }
  }
  std::vector<int> check(300);
  std::iota(begin(check), end(check), 0);
  std::erase_if(v, [](int x) { return x < 0; });
  error_if(check != v);
  return error_count;
}

CHANNEL_TEST(reuse_recursive_channel) {
  int i = 0;
  dd::channel g = g4<dd::channel>(i);
  std::vector<int> v;
  while (!g.empty()) {
    co_foreach(int x, g) {
      v.push_back(x);
      if (flip())
        break;
    }
  }
  std::vector<int> check(300);
  std::iota(begin(check), end(check), 0);
  std::erase_if(v, [](int x) { return x < 0; });
  error_if(check != v);
  co_return error_count;
}
CO_TEST(reuse_recursive_channel);

dd::generator<std::string> str_g(std::string s) {
  while (s.size() < 10) {
    co_yield s;
    s.push_back('A');
  }
  if (flip())
    co_yield dd::elements_of(str_g(std::move(s)));
}

TEST(string_generator) {
  std::string check;
  for (std::string&& s : str_g(std::string{})) {
    error_if(s != check);
    check.push_back('A');
  }
  return error_count;
}

dd::generator<bool> g_b() {
  co_yield dd::elements_of(std::vector<bool>{true, true, false, false, true});
}
inline std::coroutine_handle h = nullptr;
TEST(nontrivial_references) {
  std::vector<bool> v;
  for (bool b : g_b())
    v.push_back(b);
  error_if((v != std::vector<bool>{true, true, false, false, true}));
  return error_count;
}

TEST(ranges_recursive) {
  int i = 0;
  dd::generator g = g4<dd::generator>(i);
  std::vector<int> v;
  for (int x : g | std::views::filter([](int i) { return i >= 0; }))
    v.push_back(x);
  std::vector<int> check(300);
  std::iota(begin(check), end(check), 0);
  error_if(check != v);
  return error_count;
}
TEST(ranges_base2) {
  std::vector<int> vec;
  auto g = base_case<dd::generator>();
  for (int i : g | std::views::transform([](int&& i) { return i * 2; }) |
                   std::views::filter([](int i) { return i >= 0; }))
    vec.push_back(i);
  std::vector check(100, 0);
  std::iota(begin(check), end(check), 0);
  for (int& x : check)
    x *= 2;
  error_if(vec != check);
  return error_count;
}
CHAN_OR_GEN
G<int> byrefg(size_t& error_count) {
  int i = 0;
  RANDOM_CONTROL_FLOW;
  for (int j = 0; j < 100; ++j) {
    error_if(i != j);
    RANDOM_CONTROL_FLOW;
    co_yield dd::by_ref{i};
  }
  RANDOM_CONTROL_FLOW;
}
TEST(byref_generator) {
  for (int&& i : byrefg<dd::generator>(error_count))
    ++i;
  return error_count;
}
CHANNEL_TEST(byref_channel) {
  co_foreach(int&& i, byrefg<dd::channel>(error_count))++ i;
  co_return error_count;
}
CO_TEST(byref_channel);

dd::channel<int> null_terminated_ints() {
  std::vector vec(10, 1);
  for (int i : vec)
    co_yield i;
  co_yield dd::terminator;
  for (int i : vec)
    co_yield i;
  co_yield dd::terminator;
  for (int i : vec)
    co_yield i;
}
CHANNEL_TEST(null_terminated_channel) {
  auto c = null_terminated_ints();
  for (int i = 0; i < 3; ++i) {
    std::vector<int> vec;
    co_foreach(int i, c) vec.push_back(i);
    error_if(vec != std::vector(10, 1));
  }
  co_return error_count;
}
CO_TEST(null_terminated_channel);
// TODO tests with terminator in channel + exceptions etc
// TODO tests когда начал генерировать, приостановился, скинул все остальные элементы как elements_of
// и для генератора и для канала
// TODO test с бросанием пустого инпут ренжа из генератора, например istream_view<int> какое-то
// TODO тесты с исключениями(бросок из рекурсии) и обработку исключений всё таки
// TODO генератор и канал должны использовать один и тот же промис абсолютно
// TODO бросить канал сам из себя, взяв хендл и создав канал внутри канала
struct log_resource : std::pmr::memory_resource {
  size_t allocated = 0;
  // sizeof of this thing affects frame size with 2 multiplier bcs its saved in frame + saved for coroutine
  void* do_allocate(size_t size, size_t a) override {
    allocated += size;
    return ::operator new(size, std::align_val_t{a});
  }
  void do_deallocate(void* ptr, size_t size, size_t a) noexcept override {
    allocated -= size;
    ::operator delete(ptr, std::align_val_t{a});
  }
  bool do_is_equal(const memory_resource&) const noexcept override {
    return true;
  }
  ~log_resource() {
    if (allocated != 0) {
      std::cerr << "memory leak " << allocated << " bytes";
      std::flush(std::cerr);
      std::exit(-146);
    }
  }
};

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
int main() {
  (void)flip();  // initalize random
  log_resource r;
  dd::with_resource _(r);
  dd::scope_exit e = [&] { std::flush(std::cout), std::flush(std::cerr); };
  size_t ec = 0;
  RUN(base_channel);
  RUN(empty_channel);
  RUN(reuse_channel);
  RUN(empty_recursive2_channel);
  RUN(empty_recursive2_channel);
  RUN(recursive_channel);
  RUN(reuse_recursive_channel);
  RUN(base);
  RUN(base2);
  RUN(empty);
  RUN(reuse);
  RUN(empty_recursive);
  RUN(recursive);
  RUN(reuse_recursive);
  RUN(string_generator);
  RUN(nontrivial_references);
  RUN(empty_recursive2);
  RUN(ranges_recursive);
  RUN(ranges_base2);
  RUN(byref_generator);
  RUN(byref_channel);
  RUN(null_terminated_channel);
  return ec;
}
