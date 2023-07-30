
#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif

#include "generator.hpp"

#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>

#define error_if(Cond) error_count += static_cast<bool>((Cond))
#define TEST(NAME) inline size_t TEST##NAME(size_t error_count = 0)

static bool flip() {
  static std::mt19937 rng = [] {
    auto seed = std::random_device{}();
    std::clog << "SEED: " << seed << std::endl;
    return std::mt19937{seed};
  }();
  return std::bernoulli_distribution(0.5)(rng);
}
TEST(empty) {
  dd::generator<int> g;
  error_if(!g.empty());
  for (auto x : g)
    error_if(true);
  dd::generator_iterator<int> it{};
  return error_count;
}
dd::generator<int> g1() {
  for (int i = 0; i < 98; ++i)
    co_yield i;
  const int i = 98;
  co_yield i;
  const int j = 99;
  co_yield std::move(j);
}
TEST(reuse) {
  dd::generator g = g1();
  std::vector<int> v;
  while (!g.empty()) {
    for (int& x : g) {
      if (flip())
        break;
      v.push_back(x);
    }
  }
  std::vector<int> check(100);
  std::iota(begin(check), end(check), 0);
  error_if(check != v);
  return error_count;
}

static dd::generator<int> g2() {
    co_yield dd::elements_of(std::move([]()->dd::generator<int> { co_return; }()));
}
TEST(empty_recursive) {
    for (int x : g2()) error_if(true);
    return error_count;
}

static dd::generator<int> g3(int i) {
  co_yield i;
  if (i != 10)
    co_yield dd::elements_of(g3(i + 1));
}
TEST(recursive) {
  std::vector<int> vec;
  auto g = g3(0);
  for (int i : g)
    vec.push_back(i);
  error_if((vec != std::vector{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  error_if(!g.empty());
  for(int i : g)
    error_if(true);
  return error_count;
}

dd::generator<int> g5(int& i) {
  co_yield dd::elements_of(std::vector(15, -1));
  for (; i < 15; ++i)
    co_yield i;
}

dd::generator<int> g4(int& i) {
  co_yield dd::elements_of(std::vector(15, -1));
  for (int x = i; i < x + 15; ++i)
    co_yield i;
  if (i < 300) {
    co_yield dd::elements_of(g4(i));
  }
}

TEST(reuse_recursive) {
  int i = 0;
  dd::generator g = g4(i);
  std::vector<int> v;
  while (!g.empty()) {
    for (int& x : g) {
      if (flip())
        break;
      v.push_back(x);
    }
  }
  std::vector<int> check(300);
  std::iota(begin(check), end(check), 0);
  std::erase_if(v, [](int x) { return x == -1; });
  error_if(check != v);
  return error_count;
}

dd::generator<std::string> str_g(std::string s) {
  while(s.size() < 10) {
    co_yield s;
    s.push_back('A');
  }
  if (flip())
    co_yield dd::elements_of(str_g(std::move(s)));
}

TEST(string_generator) {
std::string check;
for(std::string& s : str_g(std::string{})) {
  error_if(s != check);
  check.push_back('A');
}
return error_count;
}

int main() {
  return TESTempty() + TESTreuse() + TESTempty_recursive() + TESTrecursive() + TESTreuse_recursive() + TESTstring_generator();
}