// tests for task context location hooks (on_create_location / on_start_location) and dd::trace_context

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "kelcoro/algorithm.hpp"
#include "kelcoro/task.hpp"
#include "kelcoro/thread_pool.hpp"

// ------------------------- tiny test framework -------------------------

[[noreturn]] static void fail(int line, const char* what) {
  std::fprintf(stderr, "test_trace_context.cpp:%d: FAILED: %s\n", line, what);
  std::abort();
}

#define CHECK(...)                \
  if (!bool(__VA_ARGS__)) {       \
    fail(__LINE__, #__VA_ARGS__); \
  }

// compilers choose different points of multi-line constructs
// (gcc: operand / closing brace, clang: `co_await` keyword / function name), so lines are checked by range
[[maybe_unused]] static void check_loc(int line, const dd::source_location& got, int first, int last,
                                       const char* fn) {
  bool ok = int(got.line()) >= first && int(got.line()) <= last &&
            std::string_view(got.file_name()).ends_with("test_trace_context.cpp") &&
            (!fn || std::strstr(got.function_name(), fn));
  if (ok)
    return;
  std::fprintf(stderr,
               "test_trace_context.cpp:%d: FAILED: expected location at lines [%d, %d] in '%s'\n"
               "  got: %s:%u:%u in '%s'\n",
               line, first, last, fn ? fn : "*", got.file_name(), unsigned(got.line()),
               unsigned(got.column()), got.function_name());
  std::abort();
}
#define CHECK_LOC(LOC, FIRST_LINE, LAST_LINE, FN) check_loc(__LINE__, LOC, FIRST_LINE, LAST_LINE, FN)
#define CHECK_UNKNOWN(LOC) CHECK((LOC).line() == 0)
#define NEXT_LINE (__LINE__ + 1)

using test_t = dd::task<void, dd::trace_context>;
#define TEST(NAME) static test_t NAME()
#define RUN(NAME) \
  NAME().get();   \
  std::printf("[ok] %s\n", #NAME)

// ------------------------- location type -------------------------

static_assert(!dd::noexport::has_on_create_location<dd::null_context>);
static_assert(!dd::noexport::has_on_start_location<dd::null_context>);
static_assert(dd::noexport::has_on_create_location<dd::trace_context>);
static_assert(dd::noexport::has_on_start_location<dd::trace_context>);

// default context must not store locations
static_assert(std::is_same_v<dd::task_promise<int, dd::null_context>::loc_t, dd::noexport::nolocation>);

// all locations have same interface
template <typename L>
constexpr bool location_like = requires(const L& l) {
  { L::current() } -> std::same_as<L>;
  { l.line() } -> std::same_as<uint_least32_t>;
  { l.column() } -> std::same_as<uint_least32_t>;
  { l.file_name() } -> std::same_as<const char*>;
  { l.function_name() } -> std::same_as<const char*>;
};
static_assert(location_like<dd::source_location>);
static_assert(location_like<dd::noexport::nolocation>);
static_assert(dd::noexport::nolocation::current().line() == 0);

#if KELCORO_SOURCE_LOCATION_SUPPORTED
static_assert(std::is_same_v<dd::source_location, std::source_location>);
#else
static_assert(std::is_same_v<dd::source_location, dd::noexport::nolocation>);
#endif

// guard against silently disabled locations (e.g. feature-test macro checked before <version>)
#if __cpp_lib_source_location >= 201907L && \
    ((defined(__clang__) && __clang_major__ >= 16) || (!defined(__clang__) && __GNUC__ >= 14))
static_assert(KELCORO_SOURCE_LOCATION_SUPPORTED);
#endif

// ------------------------- helpers -------------------------

using ttask = dd::task<int, dd::trace_context>;

[[maybe_unused]] static const int leaf_line = NEXT_LINE;
ttask leaf(int x) {
  co_return x;
}

static dd::trace_context& ctx(auto& task) {
  return *task.get_context();
}

#if !KELCORO_SOURCE_LOCATION_SUPPORTED

// hooks still compile and are invoked with empty locations, owner chain works
TEST(without_locations) {
  dd::trace_context& self = co_await dd::this_coro::context;
  ttask t = leaf(1);
  CHECK_UNKNOWN(ctx(t).created);
  CHECK(co_await t == 1);
  CHECK_UNKNOWN(ctx(t).started);
  CHECK(ctx(t).owner == &self);
  ttask t2 = leaf(2);
  auto r = t2.start();
  CHECK(t2.empty());
}

int main() {
  std::puts("source location is not supported");
  RUN(without_locations);
}

#else

// ------------------------- created -------------------------

// get_return_object is invoked implicitly, so `created` is location of coroutine definition,
// not the call site
TEST(created) {
  ttask t = leaf(1);
  CHECK_LOC(ctx(t).created, leaf_line, leaf_line + 2, "leaf");
  CHECK_UNKNOWN(ctx(t).started);
  CHECK(ctx(t).owner == nullptr);

  const int lambda_line = NEXT_LINE;
  auto lambda = [](int x) -> ttask { co_return x; };
  auto lt = lambda(1);
  CHECK_LOC(ctx(lt).created, lambda_line, lambda_line, nullptr);

  // when_all task is created in algorithm.hpp
  auto all = dd::when_all(leaf(1));
  CHECK(std::strstr(ctx(all).created.function_name(), "when_all"));
  co_return;
}

// ------------------------- co_await -------------------------

  #define MY_AWAIT(...) co_await __VA_ARGS__

TEST(co_await_forms) {
  dd::trace_context& self = co_await dd::this_coro::context;
  {
    ttask t = leaf(1);
    const int line = NEXT_LINE;
    CHECK(co_await std::move(t) == 1);
    CHECK_LOC(ctx(t).started, line, line, "co_await_forms");
    CHECK(ctx(t).owner == &self);
  }
  {
    ttask t = leaf(2);
    const int line = NEXT_LINE;
    CHECK(co_await t == 2);
    CHECK_LOC(ctx(t).started, line, line, "co_await_forms");
    CHECK(ctx(t).owner == &self);
  }
  {
    ttask t = leaf(3);
    const int line = NEXT_LINE;
    co_await t.wait();
    CHECK_LOC(ctx(t).started, line, line, "co_await_forms");
  }
  {
    // clang-format off
    ttask t = leaf(4);
    const int line = NEXT_LINE;
    int x = co_await
        std::move(t);
    // clang-format on
    CHECK(x == 4);
    CHECK_LOC(ctx(t).started, line, line + 1, "co_await_forms");
  }
  {
    ttask t = leaf(5);
    const int line = NEXT_LINE;
    (void)MY_AWAIT(t);
    CHECK_LOC(ctx(t).started, line, line, "co_await_forms");
  }
}

// ------------------------- start / start_and_detach -------------------------

TEST(start) {
  {
    ttask t = leaf(1);
    auto& c = ctx(t);
    const int line = NEXT_LINE;
    auto r = t.start();
    CHECK_LOC(c.started, line, line, "start");
    CHECK(c.owner == nullptr);
  }
  {
    ttask t = leaf(1);
    auto& c = ctx(t);
    const int line = NEXT_LINE;
    auto h = t.start_and_detach(/*stop_at_end=*/true);
    CHECK_LOC(c.started, line, line, "start");
    h.destroy();
  }
  {
    ttask t = leaf(1);
    auto& c = ctx(t);
    auto custom = dd::source_location::current();
    auto h = t.start_and_detach(true, custom);
    CHECK(c.started.line() == custom.line());
    h.destroy();
  }
  {
    // task destroys itself at end, so it reports location by itself
    static dd::source_location seen;
    auto self_reporting = []() -> ttask {
      seen = (co_await dd::this_coro::context).started;
      co_return 0;
    };
    const int line = NEXT_LINE;
    self_reporting().start_and_detach();
    CHECK_LOC(seen, line, line, "start");
  }
  co_return;
}

// ------------------------- when_all / when_any -------------------------

// children are started by internal `job`, so their `started` is unknown,
// but they are attached to when_all task, which knows where it was awaited
TEST(when_all_variadic) {
  dd::trace_context& self = co_await dd::this_coro::context;
  ttask a = leaf(1), b = leaf(2);
  dd::trace_context* children[] = {&ctx(a), &ctx(b)};
  auto all = dd::when_all(std::move(a), std::move(b));
  const int line = NEXT_LINE;
  auto [x, y] = co_await all;
  CHECK(*x + *y == 3);
  CHECK(ctx(all).owner == &self);
  CHECK_LOC(ctx(all).started, line, line, "when_all_variadic");
  for (dd::trace_context* c : children) {
    CHECK(c->owner == &ctx(all));
    CHECK_UNKNOWN(c->started);
    CHECK_LOC(c->created, leaf_line, leaf_line + 2, "leaf");
  }
}

TEST(when_all_dynamic) {
  dd::trace_context& self = co_await dd::this_coro::context;
  std::vector<ttask> tasks;
  std::vector<dd::trace_context*> children;
  for (int i = 0; i < 5; ++i) {
    tasks.push_back(leaf(i));
    children.push_back(&ctx(tasks.back()));
  }
  auto all = dd::when_all(std::move(tasks));
  const int line = NEXT_LINE;
  auto results = co_await all;
  CHECK(results.size() == 5);
  CHECK(ctx(all).owner == &self);
  CHECK_LOC(ctx(all).started, line, line, "when_all_dynamic");
  for (dd::trace_context* c : children)
    CHECK(c->owner == &ctx(all));
}

// when_any may end before its children, so children are not attached to it (owner would dangle)
TEST(when_any) {
  dd::trace_context& self = co_await dd::this_coro::context;
  auto any = dd::when_any(leaf(1), leaf(2));
  const int line = NEXT_LINE;
  auto result = co_await any;
  CHECK(result.index() == 0);
  CHECK(ctx(any).owner == &self);
  CHECK_LOC(ctx(any).started, line, line, "when_any");
}

// ------------------------- other contexts -------------------------

struct only_create : dd::null_context {
  dd::source_location created;
  void on_create_location(dd::source_location l) noexcept {
    created = l;
  }
};
struct only_start : dd::null_context {
  dd::source_location started;
  void on_start_location(dd::source_location l) noexcept {
    started = l;
  }
};

TEST(other_contexts) {
  {
    const int line = NEXT_LINE;
    auto t = []() -> dd::task<int, only_create> { co_return 1; }();
    CHECK_LOC(t.get_context()->created, line, line, nullptr);
  }
  {
    // awaiting from any context with on_start_location hook records location
    auto outer = []() -> dd::task<void, only_start> {
      ttask t = leaf(1);
      const int line = NEXT_LINE;
      (void)co_await t;
      CHECK_LOC(ctx(t).started, line, line, nullptr);
      CHECK(ctx(t).owner == nullptr);  // owner is not trace_context
    };
    outer().get();
  }
  {
    // null_context does not pay for locations, so nothing recorded
    auto outer = []() -> dd::task<void> {
      ttask t = leaf(1);
      (void)co_await t;
      CHECK_UNKNOWN(ctx(t).started);
    };
    outer().get();
  }
  {
    // trace_context awaiting task without hooks
    CHECK(co_await []() -> dd::task<int> { co_return 2; }() == 2);
  }
}

dd::pmr::task<int, dd::trace_context> pmr_leaf(dd::with_resource<dd::pmr::polymorphic_resource>) {
  co_return 1;
}

// resourced promise is derived from task_promise
TEST(pmr_task) {
  auto outer = []() -> dd::pmr::task<void, dd::trace_context> {
    dd::trace_context& self = co_await dd::this_coro::context;
    auto t = pmr_leaf(dd::with_resource{dd::pmr::polymorphic_resource{}});
    CHECK(std::strstr(ctx(t).created.function_name(), "pmr_leaf"));
    const int line = NEXT_LINE;
    (void)co_await t;
    CHECK_LOC(ctx(t).started, line, line, nullptr);
    CHECK(ctx(t).owner == &self);
  };
  co_await outer();
}

// ------------------------- multithreading -------------------------

// All fields are written before task starts, so children may read whole owner chain
// from different threads concurrently (run under -fsanitize=thread)
ttask mt_child(dd::thread_pool& pool, int expected_line) {
  if (!co_await dd::jump_on(pool))
    co_return 0;
  dd::trace_context& self = co_await dd::this_coro::context;
  CHECK(self.owner && self.owner->owner);
  CHECK_LOC(self.owner->started, expected_line, expected_line, "mt_parent");
  int depth = 0;
  for (dd::trace_context* c = &self; c; c = c->owner)
    ++depth;
  co_return depth;
}

ttask mt_parent(dd::thread_pool& pool, int n) {
  std::vector<ttask> tasks;
  const int line = __LINE__ + 3;
  for (int i = 0; i < n; ++i)
    tasks.push_back(mt_child(pool, line));
  auto results = co_await dd::when_all(std::move(tasks));
  int sum = 0;
  for (auto& r : results)
    sum += *r;
  co_return sum;
}

TEST(multithreaded) {
  dd::thread_pool pool(4);
  for (int i = 0; i < 20; ++i)
    CHECK(mt_parent(pool, 200).get() == 200 * 3);  // child -> when_all -> mt_parent
  pool.request_stop();
  std::move(pool).wait_stop();
  co_return;
}

int main() {
  RUN(created);
  RUN(co_await_forms);
  RUN(start);
  RUN(when_all_variadic);
  RUN(when_all_dynamic);
  RUN(when_any);
  RUN(other_contexts);
  RUN(pmr_task);
  RUN(multithreaded);
}

#endif
