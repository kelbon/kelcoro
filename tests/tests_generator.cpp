
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
    std::clog << seed << std::endl;
    return std::mt19937{seed};
  }();
  return std::bernoulli_distribution(0.5)(rng);
}
TEST(empty) {
  dd::generator<int> g;
  error_if(!g.empty());
  for (auto x : g)
    error_if(true);
  return error_count;
}
dd::generator<int> g1() {
  for (int i = 0; i < 100; ++i)
    co_yield i;
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

//TEST(reuse_recursive) {
//  int i = 0;
//  dd::generator g = g4(i);
//  std::vector<int> v;
//  while (!g.empty()) {
//    for (int& x : g) {
//      if (flip())
//        break;
//      v.push_back(x);
//    }
//  }
//  std::vector<int> check(100);
//  std::iota(begin(check), end(check), 0);
//  error_if(check != v);
//  return error_count;
//}
// TODO test yield const objects(and const &&)
int main() {
  return TESTempty() + TESTreuse() + TESTempty_recursive() + TESTrecursive();
}