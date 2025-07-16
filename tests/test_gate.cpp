#include "kelcoro/gate.hpp"
#include "kelcoro/async_task.hpp"
#include "kelcoro/job.hpp"
#include <vector>
#include <iostream>
#define error_if(...)                          \
  if ((__VA_ARGS__)) {                         \
    std::cout << "ERROR ON LINE " << __LINE__; \
    std::exit(__LINE__);                       \
  }

inline std::vector<std::coroutine_handle<>> handles;

dd::job gate_user(dd::gate& g) {
  if (g.is_closed())
    co_return;
  auto guard = g.hold();
  while (!g.is_closed()) {
    auto guard2 = g.hold();
    co_await dd::suspend_and_t{[](std::coroutine_handle<> h) { handles.push_back(h); }};
  }
}

dd::async_task<void> gate_holder() {
  dd::gate g;
  error_if(g.is_closed());
  error_if(g.active_count() != 0);
  co_await g.close();
  error_if(!g.is_closed());
  error_if(g.active_count() != 0);
  error_if(g.try_enter());
  g.reopen();
  error_if(g.is_closed());
  error_if(g.active_count() != 0);
  gate_user(g);
  error_if(g.is_closed());
  error_if(g.active_count() != 2);
  gate_user(g);
  error_if(g.is_closed());
  error_if(g.active_count() != 4);
  g.request_close();
  error_if(!g.is_closed());
  for (std::coroutine_handle<> h : handles)
    h.resume();  // cancel operations
  error_if(g.active_count() != 0);
  co_await g.close();
  error_if(!g.is_closed());
  error_if(g.active_count() != 0);
  g.reopen();
  error_if(g.is_closed());
  error_if(!g.try_enter());
  error_if(g.active_count() != 1);
  g.leave();
  error_if(g.active_count() != 0);
}

int test_gate() {
  gate_holder().get();
  return 0;
}

int main() {
  return test_gate();
}
