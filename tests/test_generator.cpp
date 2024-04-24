
#include "kelcoro/generator.hpp"
#include "kelcoro/channel.hpp"
#include "kelcoro/async_task.hpp"
#include "kelcoro/inplace_generator.hpp"

#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <ranges>
#include <sstream>
#include <deque>

using dd::channel;
using dd::elements_of;
using dd::generator;

#define error_if(Cond) error_count += static_cast<bool>((Cond))
#define TEST(NAME) inline size_t TEST##NAME(size_t error_count = 0)
#define CHANNEL_TEST(NAME) inline dd::async_task<size_t> NAME(size_t error_count = 0)
#define CO_TEST(NAME)    \
  TEST(NAME) {           \
    return NAME().get(); \
  }
#define CHAN_OR_GEN template <template <typename> typename G>
#define RANDOM_CONTROL_FLOW                               \
  if constexpr (std::is_same_v<G<int>, dd::channel<int>>) \
    if (flip())                                           \
  (void)co_await dd::jump_on(dd::new_thread_executor)

static bool flip() {
  static thread_local std::mt19937 rng = [] {
    auto seed = std::random_device{}();
    return std::mt19937{seed};
  }();
  return std::bernoulli_distribution(0.5)(rng);
}

static_assert(std::input_iterator<dd::generator_iterator<int>>);
TEST(empty) {
  dd::generator<int> g;
  error_if(!g.empty());
  error_if(g != dd::generator<int>{});
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
  auto g = base_case<dd::generator>();
  for (int i : g) {
    error_if(j != i);
    ++j;
  }
  error_if(g != dd::generator<int>{});
  return error_count;
}
channel<float> base_case_chan() {
  channel<int> c = base_case<channel>();
  co_yield elements_of(c);
  if (c != channel<int>{})
    throw std::runtime_error{"base case chan failed"};
}
CHANNEL_TEST(chan_yield) {
  std::vector<int> v;
  co_foreach(int i, base_case_chan()) v.push_back(i);
  std::vector<int> check(100, 0);
  std::iota(begin(check), end(check), 0);
  error_if(v != check);
  co_return error_count;
}
CO_TEST(chan_yield);

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
  error_if(g != dd::channel<int>{});
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

// clang had bug which breaks all std::views
#if !defined(__clang_major__) || __clang_major__ >= 15

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
#endif  // clang bug

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

dd::generator<int> interrupted_g() {
  auto g = base_case<dd::generator>();
  for (int i : g) {
    co_yield i;
    if (i == 10)
      break;
  }
  co_yield dd::elements_of(g);
}
dd::channel<int> interrupted_c(auto&&) {
  auto g = base_case<dd::channel>();
  co_foreach(int i, g) {
    co_yield i;
    if (i == 10)
      break;
  }
  co_yield dd::elements_of(g);
}
TEST(interrupted) {
  std::vector<int> v;
  for (int i : interrupted_g())
    v.push_back(i);
  std::vector check(100, 0);
  std::iota(begin(check), end(check), 0);
  error_if(v != check);
  return error_count;
}
CHANNEL_TEST(interrupted_channel) {
  std::vector<int> v;
  co_foreach(int i, interrupted_c(dd::with_resource<dd::pmr::polymorphic_resource>{})) v.push_back(i);
  std::vector check(100, 0);
  std::iota(begin(check), end(check), 0);
  error_if(v != check);
  co_return error_count;
}
CO_TEST(interrupted_channel);

channel<int> recursive_interrupted_c() {
  co_yield elements_of(interrupted_c(dd::with_resource<dd::pmr::polymorphic_resource>{}));
}
generator<int> recursive_interrupted_g() {
  co_yield elements_of(interrupted_g());
}

TEST(recursive_interrupted) {
  std::vector<int> v;
  for (int i : recursive_interrupted_g())
    v.push_back(i);
  std::vector check(100, 0);
  std::iota(begin(check), end(check), 0);
  error_if(v != check);
  return error_count;
}
CHANNEL_TEST(recursive_interrupted_channel) {
  std::vector<int> v;
  co_foreach(int i, recursive_interrupted_c()) v.push_back(i);
  std::vector check(100, 0);
  std::iota(begin(check), end(check), 0);
  error_if(v != check);
  co_return error_count;
}
CO_TEST(recursive_interrupted_channel);

channel<int> recursive_interrupted_c2() {
  co_yield -1;
  co_yield elements_of(interrupted_c(dd::with_resource<dd::pmr::polymorphic_resource>{}));
  co_yield 100;
}
generator<int> recursive_interrupted_g2() {
  co_yield -1;
  co_yield elements_of(interrupted_g());
  co_yield 100;
}

TEST(recursive_interrupted2) {
  std::vector<int> v;
  for (int i : recursive_interrupted_g2())
    v.push_back(i);
  std::vector check(102, 0);
  std::iota(begin(check), end(check), -1);
  error_if(v != check);
  return error_count;
}
CHANNEL_TEST(recursive_interrupted_channel2) {
  std::vector<int> v;
  co_foreach(int i, recursive_interrupted_c2()) v.push_back(i);
  std::vector check(102, 0);
  std::iota(begin(check), end(check), -1);
  error_if(v != check);
  co_return error_count;
}
CO_TEST(recursive_interrupted_channel2);

CHAN_OR_GEN
G<int> throw_c() {
  for (int i = 0; i < 100; ++i) {
    co_yield i;
    if (i == 10)
      throw std::runtime_error{"10"};
  }
  std::terminate();  // unreachable
}

channel<int> throw_tester() {
  channel c = throw_c<channel>();
  co_foreach(int i, c) {
    co_yield i;
    if (i == 5)
      break;
  }
  co_yield elements_of([&]() -> channel<int> {
    co_yield -1;
    co_yield elements_of(c);
    co_yield -1;
  }());
  for (int i = 11; i < 20; ++i)
    co_yield i;
}

CHANNEL_TEST(recursive_throw) {
  std::vector<int> v;
  channel c = throw_tester();
  try {
    co_foreach(int i, c) {
      v.push_back(i);
    }
  } catch (const std::runtime_error& e) {
    error_if(e.what() != std::string("10"));
  }
  std::vector check1 = {0, 1, 2, 3, 4, 5, -1, 6, 7, 8, 9, 10};
  error_if(v != check1);
  co_foreach(int i, c) {
    v.push_back(i);
  }
  std::vector check2 = {0, 1, 2, 3, 4, 5, -1, 6, 7, 8, 9, 10, -1, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  error_if(v != check2);
  co_return error_count;
}
CO_TEST(recursive_throw);

TEST(toplevel_throw) {
  std::vector<int> v;
  try {
    for (auto x : throw_c<generator>())
      v.push_back(x);
  } catch (const std::runtime_error& e) {
    error_if(e.what() != std::string("10"));
    std::vector check{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    error_if(v != check);
    return error_count;
  }
  std::terminate();  // unreachable
}
CHANNEL_TEST(toplevel_throw_channel) {
  std::vector<int> v;
  try {
    co_foreach(auto x, throw_c<channel>()) v.push_back(x);
  } catch (...) {
    std::vector check{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    error_if(v != check);
  }
  co_return error_count;
}
CO_TEST(toplevel_throw_channel);

// clang had bug which breaks all std::views
#if !defined(__clang_major__) || __clang_major__ >= 15

CHAN_OR_GEN
G<int> inp() {
  std::stringstream s;
  co_yield elements_of(std::views::istream<int>(s));
}
TEST(input_rng) {
  for (auto x : inp<generator>()) {
    error_if(true);
  }
  return error_count;
}
CHANNEL_TEST(input_rng_channel) {
  co_foreach(auto x, inp<channel>()) {
    error_if(true);
  }
  co_return error_count;
}
CO_TEST(input_rng_channel);

#endif  // clang bug

struct tester_t : dd::not_movable {
  int i;
  tester_t(int i) : i(i) {
  }
};

CHAN_OR_GEN
G<tester_t> nomove_generator() {
  // must be yielded by ref bcs !borrowed range
  std::deque<tester_t> d;
  d.emplace_back(0);
  d.emplace_back(1);
  d.emplace_back(2);
  co_yield elements_of(std::move(d));
}
TEST(nomove_gen) {
  int i = 0;
  for (tester_t&& x : nomove_generator<generator>())
    error_if(x.i != i++);
  return error_count;
}
CHANNEL_TEST(nomove_gen_channel) {
  int i = 0;
  co_foreach(tester_t && x, nomove_generator<channel>()) error_if(x.i != i++);
  co_return error_count;
}
CO_TEST(nomove_gen_channel);

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

inline auto sz = size_t(-1);
struct new_delete_resource {
  void* allocate(size_t s, size_t a) {
    sz = s;
    (void)a;  // cannot use new with 'align_val_t' on msvc... Its broken
    return malloc(s);
  }
  void deallocate(void* p, size_t s, size_t a) noexcept {
    sz -= s;
    (void)a;
    free(p);
  }
};
dd::generator_r<int, new_delete_resource> new_ints() {
  co_yield 1;
}
dd::generator_r<int, new_delete_resource> new_ints2(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_yield 2;
}
dd::generator_r<float, new_delete_resource> new_ints3() {
  co_yield 3;
}
dd::generator<int> new_ints4() {
  co_yield 4;
}
dd::generator<int> new_ints5(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_yield 5;
}
dd::generator<float> new_ints6() {
  co_yield 6;
}
dd::generator<float> new_ints7(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_yield 7;
}
dd::generator<int> throw_different_things() {
  co_yield elements_of(new_ints());
  co_yield elements_of(new_ints2({}));
  co_yield elements_of(new_ints3());
  co_yield elements_of(new_ints4());
  co_yield elements_of(new_ints5({}));
  co_yield elements_of(new_ints6());
  co_yield elements_of(new_ints7({}));
}
TEST(different_yields) {
  std::vector<int> v;
  for (auto x : throw_different_things())
    v.push_back(x);
  error_if((v != std::vector{1, 2, 3, 4, 5, 6, 7}));
  return error_count;
}
dd::channel_r<int, new_delete_resource> new_ints_c() {
  co_yield 1;
}
dd::channel_r<int, new_delete_resource> new_ints2_c(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_yield 2;
}
dd::channel_r<float, new_delete_resource> new_ints3_c() {
  co_yield 3;
}
dd::channel<int> new_ints4_c() {
  co_yield 4;
}
dd::channel<int> new_ints5_c(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_yield 5;
}
dd::channel<float> new_ints6_c() {
  co_yield 6;
}
dd::channel<float> new_ints7_c(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_yield 7;
}
dd::channel<int> throw_different_things_c() {
  co_yield elements_of(new_ints());
  co_yield elements_of(new_ints2({}));
  co_yield elements_of(new_ints3());
  co_yield elements_of(new_ints4());
  co_yield elements_of(new_ints5({}));
  co_yield elements_of(new_ints6());
  co_yield elements_of(new_ints7({}));
  co_yield elements_of(new_ints_c());
  co_yield elements_of(new_ints2_c({}));
  co_yield elements_of(new_ints3_c());
  co_yield elements_of(new_ints4_c());
  co_yield elements_of(new_ints5_c({}));
  co_yield elements_of(new_ints6_c());
  co_yield elements_of(new_ints7_c({}));
}
CHANNEL_TEST(different_yields_channel) {
  std::vector<int> v;
  co_foreach(auto x, throw_different_things_c()) v.push_back(x);
  error_if((v != std::vector{1, 2, 3, 4, 5, 6, 7, 1, 2, 3, 4, 5, 6, 7}));
  co_return error_count;
}
CO_TEST(different_yields_channel);

dd::generator<int> byref_g(int& res) {
  int i;
  while (true) {
    co_yield dd::by_ref{i};
    res += i;
  }
}

TEST(generator_as_output_range) {
  std::vector v{1, 2, 3, 4, 5};
  int res = 0;
  std::copy(begin(v), end(v), byref_g(res).begin().out());
  error_if(res != (1 + 2 + 3 + 4 + 5));
  return error_count;
}

static_assert(std::output_iterator<dd::generator_output_iterator<int>, int>);

dd::generator<int&> ref_generator() {
  int x = 0;
  for (int i = 0; i < 10; ++i) {
    int prev = x;
    co_yield x;
    if (x != prev + 1)
      std::exit(-6);
  }
}
dd::generator<const int&> recursive_ref_gen() {
  int i = 5;
  co_yield i;  // &
  co_yield 5;  // &&
  const int j = 5;
  co_yield j;             // const &
  co_yield std::move(j);  // const &&
  co_yield dd::elements_of(ref_generator());
}

static_assert(std::input_iterator<dd::generator<int&>::iterator>);
TEST(reference_generators) {
  for (int& x : ref_generator()) {
    ++x;
  }
  dd::generator g = recursive_ref_gen();
  auto b = g.begin();
  error_if(*b != 5);
  for (int i = 0; i < 3; ++i) {
    ++b;
    error_if(*b != 5);
  }
  for (const int& x : g) {
    ++(*const_cast<int*>(&x));
  }
  return error_count;
}
dd::channel<int&> ref_channel() {
  int x = 0;
  for (int i = 0; i < 10; ++i) {
    int prev = x;
    co_yield x;
    if (x != prev + 1)
      std::exit(-6);
  }
}
dd::channel<const int&> recursive_ref_channel() {
  int i = 5;
  co_yield i;  // &
  co_yield 5;  // &&
  const int j = 5;
  co_yield j;             // const &
  co_yield std::move(j);  // const &&
  co_yield dd::elements_of(ref_channel());
}

CHANNEL_TEST(reference_channels) {
  co_foreach(int& x, ref_channel()) {
    ++x;
  }
  dd::channel g = recursive_ref_channel();
  auto b = co_await g.begin();
  error_if(*b != 5);
  for (int i = 0; i < 3; ++i) {
    co_await ++b;
    error_if(*b != 5);
  }
  co_foreach(const int& x, g) {
    ++(*const_cast<int*>(&x));
  }
  co_return error_count;
}
CO_TEST(reference_channels);

dd::inplace_generator<int> inplace_iota(int i, int j) {
  for (int x = i; x < j; ++x)
    co_yield x;
}

TEST(inplace_generator) {
  int x = 0;
  for (int&& i : inplace_iota(0, 150)) {
    error_if(x != i);
    ++x;
  }
  return error_count;
}
TEST(empty_inplace_generator) {
  for (int x : inplace_iota(0, 0)) {
    error_if(true);
  }
  return error_count;
}
TEST(inplace_generator_move) {
  dd::inplace_generator g = inplace_iota(0, 150);
  auto g_moved = std::move(g);
  error_if(!g.empty());
  int x = 0;
  for (int&& i : g_moved) {
    error_if(x != i);
    ++x;
  }
  error_if(!g_moved.empty());
  return error_count;
}

int main() {
  static_assert(::dd::memory_resource<new_delete_resource>);
  (void)flip();  // initalize random
  {
    log_resource r;
    dd::pmr::pass_resource(r);
    auto x = []() -> dd::generator<int> { co_yield 1; };
    auto y_g = x();
    auto y_g_it = y_g.begin();
    if (1 != *y_g_it.operator->())
      std::exit(-5);
    for (auto i : x())
      if (i != 1)
        std::exit(-1);
    auto y = []() -> dd::generator_r<int, new_delete_resource> { co_yield 1; };
    for (auto i : y())
      if (i != 1)
        std::exit(-2);
    auto z = [](dd::with_resource<dd::pmr::polymorphic_resource> = {}) -> generator<int> { co_yield 1; };
    for (auto i : z())
      if (i != 1)
        std::exit(-3);
    auto ze = [](dd::with_resource<new_delete_resource> = {}) -> generator<int> { co_yield 1; };
    for (auto i : ze())
      if (i != 1)
        std::exit(-4);
  }
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
  // clang had bug which breaks all std::views
#if !defined(__clang_major__) || __clang_major__ >= 15
  RUN(ranges_recursive);
  RUN(ranges_base2);
#endif  // clang bug
  RUN(byref_generator);
  RUN(byref_channel);
  RUN(interrupted);
  RUN(interrupted_channel);
  RUN(recursive_interrupted);
  RUN(recursive_interrupted_channel);
  RUN(recursive_interrupted2);
  RUN(recursive_interrupted_channel2);
  RUN(recursive_throw);
#ifndef _WIN32
  // segfault when reading throwed exception .what(), because its somehow on coro frame, when it must not
  // so its already destroyed by generator destructor when cached
  RUN(toplevel_throw);
#endif
  RUN(toplevel_throw_channel);
  // clang had bug which breaks all std::views
#if !defined(__clang_major__) || __clang_major__ >= 15
  RUN(input_rng);
  RUN(input_rng_channel);
#endif  // clang bug
  RUN(nomove_gen);
  RUN(nomove_gen_channel);
  RUN(chan_yield);
  RUN(different_yields);
  RUN(different_yields_channel);
  RUN(generator_as_output_range);
  RUN(reference_generators);
  RUN(reference_channels);
  RUN(inplace_generator);
  RUN(empty_inplace_generator);
  RUN(inplace_generator_move);
  if (sz != 0 && sz != size_t(-1))
    std::exit(146);
  return ec;
}
