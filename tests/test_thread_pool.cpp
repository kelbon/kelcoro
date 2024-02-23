#include "thread_pool.hpp"

std::atomic<int> i = 0;
enum { COUNT = 150 };
std::latch l(COUNT);

dd::job foo(dd::thread_pool& p) {
  while (true) {
    auto x = i.fetch_add(1, std::memory_order::acq_rel);
    if (x >= 100'000)
      break;
    co_await p.transition();
  }
  l.count_down();
}

int main() {
  dd::thread_pool p(16);

  for (int i = 0; i < COUNT; ++i) {
    foo(p);
  }
  // TODO есть плагин - профайлер под ввскод? Непонятно где время пропаает
  l.wait();
  if (i.load() < 100'000)
    return -1;
  return 0;
}
