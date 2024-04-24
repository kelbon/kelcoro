#pragma once

#if !defined(NDEBUG) && !defined(KELCORO_DISABLE_MONITORING) && defined(KELCORO_ENABLE_THREADPOOL_MONITORING)
#define KELCORO_THREADPOOL_MONITORING_IS_ENABLED
#endif

#ifdef KELCORO_THREADPOOL_MONITORING_IS_ENABLED

#include <vector>
#include <atomic>
#include <cassert>

namespace dd {

struct monitoring_t {
  // all values only grow
  std::atomic_size_t pushed = 0;
  std::atomic_size_t finished = 0;
  std::atomic_size_t cancelled = 0;
  std::atomic_size_t strands_count = 0;
  // count of pop_all from queue
  std::atomic_size_t pop_count = 0;
  std::atomic_size_t sleep_count = 0;

  monitoring_t() = default;
  // copied only when one thread
  monitoring_t(const monitoring_t& other)
      : pushed(other.pushed.load(relaxed)),
        finished(other.finished.load(relaxed)),
        cancelled(other.cancelled.load(relaxed)),
        strands_count(other.strands_count.load(relaxed)),
        pop_count(other.pop_count.load(relaxed)),
        sleep_count(other.sleep_count.load(relaxed)) {
  }
  // all calculations approximate
  using enum std::memory_order;

  static size_t average_tasks_popped(size_t pop_count, size_t finished) noexcept {
    if (!pop_count)
      return 0;
    return finished / pop_count;
  }
  static size_t pending_count(size_t pushed, size_t finished) noexcept {
    // order to never produce value < 0
    return pushed - finished;
  }
  static float sleep_percent(size_t pop_count, size_t sleep_count) noexcept {
    assert(pop_count >= sleep_count);
    return static_cast<float>(sleep_count) / pop_count;
  }
  void print(auto&& out) const {
    size_t p = pushed, f = finished, sc = strands_count, pc = pop_count, slc = sleep_count,
           slp = sleep_percent(pc, slc), avr_tp = average_tasks_popped(pc, f), pending = pending_count(p, f),
           cld = cancelled;
    // clang-format off
    out << "pushed:               " << p << '\n';
    out << "finished:             " << f << '\n';
    out << "cancelled:            " << cld << '\n';
    out << "strands_count:        " << sc << '\n';
    out << "pop_count:            " << pc << '\n';
    out << "sleep_count:          " << slc << '\n';
    out << "sleep%:               " << slp << '\n';
    out << "average tasks popped: " << avr_tp << '\n';
    out << "pending count:        " << pending << '\n';
    // clang-format on
  }
};

// only for debug with access from one thread
static inline std::vector<monitoring_t> monitorings;

#define KELCORO_MONITORING(...) __VA_ARGS__
#define KELCORO_MONITORING_INC(x) KELCORO_MONITORING(x.fetch_add(1, ::std::memory_order::relaxed))

}  // namespace dd

#else

#define KELCORO_MONITORING(...)
#define KELCORO_MONITORING_INC(x)

#endif  // !KELCORO_ENABLE_THREADPOOL_MONITORING
