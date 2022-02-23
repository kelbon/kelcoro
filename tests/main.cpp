
#include "test_kel_coroutines.hpp"

import<chrono>;

int main() {
  using namespace std::chrono;
  const auto start = steady_clock::now();
  kel::test::test_room::run_all_tests();
  std::cout << duration_cast<milliseconds>(steady_clock::now() - start);
}