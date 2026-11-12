// tests for task exception helpers (has_exception / take_exception / return_exception /
// KELCORO_RETURN_EXCEPTION) and task context customization (ctx.transform, ctx.on_end)

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "kelcoro/algorithm.hpp"
#include "kelcoro/task.hpp"

// ------------------------- tiny test framework -------------------------

[[noreturn]] static void fail(int line, const char* what) {
  std::fprintf(stderr, "test_task_context.cpp:%d: FAILED: %s\n", line, what);
  std::abort();
}

#define CHECK(...)                \
  if (!bool(__VA_ARGS__)) {       \
    fail(__LINE__, #__VA_ARGS__); \
  }

#define TEST(NAME) static void NAME()
#define RUN(NAME) \
  NAME();         \
  std::printf("[ok] %s\n", #NAME)

// returns message of exception in `e` or "" if `e` contains something else
static std::string what(const std::exception_ptr& e) {
  try {
    std::rethrow_exception(e);
  } catch (const std::runtime_error& ex) {
    return ex.what();
  } catch (...) {
    return "";
  }
}

// invokes `f`, returns message of thrown runtime_error or "" if nothing thrown
template <typename F>
static std::string thrown_by(F&& f) {
  try {
    (void)f();
  } catch (const std::runtime_error& ex) {
    return ex.what();
  }
  return "";
}

static std::exception_ptr make_error(const char* msg) {
  return std::make_exception_ptr(std::runtime_error(msg));
}

// ------------------------- exceptions -------------------------

dd::task<int> int_value() {
  co_return 1;
}
dd::task<int> int_throws() {
  throw std::runtime_error("int_throws");
  co_return 0;
}
dd::task<void> void_value() {
  co_return;
}
dd::task<void> void_throws() {
  throw std::runtime_error("void_throws");
  co_return;
}

TEST(exception_accessors) {
  auto check = []() -> dd::task<void> {
    {
      auto t = int_value();
      co_await t.wait();
      CHECK(!t.has_exception());
      CHECK(t.get() == 1);
    }
    {
      auto t = int_throws();
      co_await t.wait();  // does not rethrow
      CHECK(t.has_exception());
      CHECK(what(t.take_exception()) == "int_throws");
      CHECK(!t.has_exception());
    }
    {
      auto t = void_value();
      co_await t.wait();
      CHECK(!t.has_exception());
    }
    {
      auto t = void_throws();
      co_await t.wait();
      CHECK(t.has_exception());
      CHECK(what(t.take_exception()) == "void_throws");
    }
  };
  check().get();
}

// non-void: co_return dd::return_exception(e)
dd::task<int> int_returns_exception(std::exception_ptr e) {
  co_return dd::return_exception(std::move(e));
}

// void: KELCORO_RETURN_EXCEPTION
dd::task<void> void_returns_exception(std::exception_ptr e, bool& after_return) {
  KELCORO_RETURN_EXCEPTION(std::move(e));
  after_return = true;
}

// same with direct co_await
dd::task<void> void_sets_exception(std::exception_ptr e) {
  co_return co_await dd::this_coro::set_exception{std::move(e)};
}

TEST(return_exception) {
  // value of exception_ptr is passed as is, without copy / rethrow
  std::exception_ptr e = make_error("int");
  {
    auto t = int_returns_exception(e);
    CHECK(thrown_by([&] { return t.get(); }) == "int");
  }
  {
    auto check = [](std::exception_ptr e) -> dd::task<void> {
      auto t = int_returns_exception(e);
      co_await t.wait();
      CHECK(t.take_exception() == e);
      CHECK(thrown_by([&] { return int_returns_exception(e).get(); }) == "int");
    };
    check(e).get();
  }
  {
    // co_await rethrows
    auto check = [](std::exception_ptr e) -> dd::task<void> {
      bool thrown = false;
      try {
        (void)co_await int_returns_exception(e);
      } catch (const std::runtime_error&) {
        thrown = true;
      }
      CHECK(thrown);
    };
    check(e).get();
  }
}

TEST(set_exception_void) {
  std::exception_ptr e = make_error("void");
  {
    bool after_return = false;
    auto t = void_returns_exception(e, after_return);
    CHECK(thrown_by([&] {
            t.get();
            return 0;
          }) == "void");
    CHECK(!after_return);  // co_return is part of macro
  }
  {
    auto check = [](std::exception_ptr e) -> dd::task<void> {
      bool after_return = false;
      auto t1 = void_returns_exception(e, after_return);
      co_await t1.wait();
      CHECK(t1.take_exception() == e);
      auto t2 = void_sets_exception(e);
      co_await t2.wait();
      CHECK(t2.take_exception() == e);
      bool thrown = false;
      try {
        co_await void_sets_exception(e);
      } catch (const std::runtime_error&) {
        thrown = true;
      }
      CHECK(thrown);
    };
    check(e).get();
  }
}

// only void tasks may use set_exception, non-void tasks use dd::return_exception
template <typename Promise>
constexpr bool can_set_exception = requires(Promise& p, std::exception_ptr e) {
  p.await_transform(dd::this_coro::set_exception{std::move(e)}).await_ready();
};
static_assert(can_set_exception<dd::task_promise<void, dd::null_context>>);
static_assert(can_set_exception<dd::task_promise<void, dd::trace_context>>);

dd::pmr::task<void> pmr_void_returns_exception(dd::with_resource<dd::pmr::polymorphic_resource>,
                                               std::exception_ptr e) {
  KELCORO_RETURN_EXCEPTION(std::move(e));
}

TEST(set_exception_pmr) {
  std::exception_ptr e = make_error("pmr");
  auto t = pmr_void_returns_exception(dd::with_resource{dd::pmr::polymorphic_resource{}}, e);
  CHECK(thrown_by([&] {
          t.get();
          return 0;
        }) == "pmr");
}

// child error passed up through two levels without rethrowing it
dd::task<void> void_level1(std::exception_ptr e) {
  auto child = void_sets_exception(std::move(e));
  co_await child.wait();
  if (child.has_exception())
    KELCORO_RETURN_EXCEPTION(child.take_exception());
}
dd::task<int> int_level2(std::exception_ptr e) {
  auto child = void_level1(std::move(e));
  co_await child.wait();
  if (child.has_exception())
    co_return dd::return_exception(child.take_exception());
  co_return 0;
}

TEST(propagate_without_rethrow) {
  std::exception_ptr e = make_error("deep");
  auto check = [](std::exception_ptr e) -> dd::task<void> {
    auto t = int_level2(e);
    co_await t.wait();
    CHECK(t.take_exception() == e);  // same exception object at the top
  };
  check(e).get();
}

TEST(set_exception_when_all) {
  std::exception_ptr e = make_error("when_all");
  auto check = [](std::exception_ptr e) -> dd::task<void> {
    auto [ok, err] = co_await dd::when_all(void_value(), void_sets_exception(e));
    CHECK(ok.has_value());
    CHECK(!err.has_value());
    CHECK(err.error() == e);
  };
  check(e).get();
}

// ------------------------- ctx.transform -------------------------

// awaitable without task related things
struct ready_int {
  int value;
  bool await_ready() const noexcept {
    return true;
  }
  void await_suspend(std::coroutine_handle<>) const noexcept {
  }
  int await_resume() const noexcept {
    return value;
  }
};

// wraps ready_int, adds 100 to its result
struct plus_100 {
  ready_int inner;
  bool await_ready() const noexcept {
    return inner.await_ready();
  }
  void await_suspend(std::coroutine_handle<> h) const noexcept {
    inner.await_suspend(h);
  }
  int await_resume() const noexcept {
    return inner.await_resume() + 100;
  }
};

struct counting_ctx : dd::null_context {
  static inline int transformed = 0;

  decltype(auto) transform(auto&& x) noexcept {
    ++transformed;
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, ready_int>)
      return plus_100{x};
    else
      return std::forward<decltype(x)>(x);
  }
};

using ctask = dd::task<int, counting_ctx>;

ctask counted_leaf(int x) {
  co_return x;
}

TEST(transform_applies_to_awaitables) {
  counting_ctx::transformed = 0;
  auto t = []() -> ctask {
    int sum = co_await ready_int{1};  // 101
    sum += co_await counted_leaf(2);  // 2, rvalue task
    auto l = counted_leaf(3);
    sum += co_await l;  // 3, lvalue task
    co_await l.wait();  // done already, still transformed
    co_return sum;
  }();
  CHECK(t.get() == 106);
  CHECK(counting_ctx::transformed == 4);
}

TEST(transform_skips_special_awaitables) {
  counting_ctx::transformed = 0;
  auto t = []() -> ctask {
    counting_ctx& c = co_await dd::this_coro::context;
    (void)c;
    std::coroutine_handle<> h = co_await dd::this_coro::handle;
    (void)h;
    co_return 0;
  }();
  (void)t.get();
  CHECK(counting_ctx::transformed == 0);

  // internal co_awaits of when_all / when_any (they are tasks with same ctx) are not transformed,
  // only when_all / when_any tasks themselves
  counting_ctx::transformed = 0;
  auto t2 = []() -> ctask {
    auto [a, b] = co_await dd::when_all(counted_leaf(1), counted_leaf(2));
    auto any = co_await dd::when_any(counted_leaf(3), counted_leaf(4));
    co_return *a + *b + int(any.index());
  }();
  CHECK(t2.get() == 3);
  CHECK(counting_ctx::transformed == 2);
}

// transform together with location hooks
struct counting_trace_ctx : dd::trace_context {
  static inline int transformed = 0;

  decltype(auto) transform(auto&& x) noexcept {
    ++transformed;
    return std::forward<decltype(x)>(x);
  }
};

#if KELCORO_SOURCE_LOCATION_SUPPORTED
TEST(transform_with_locations) {
  counting_trace_ctx::transformed = 0;
  using task_t = dd::task<int, counting_trace_ctx>;
  auto t = []() -> task_t {
    auto leaf = []() -> task_t { co_return 5; }();
    const int line = __LINE__ + 1;
    int x = co_await leaf;
    CHECK(int(leaf.get_context()->started.line()) == line);
    co_return x;
  }();
  CHECK(t.get() == 5);
  CHECK(counting_trace_ctx::transformed == 1);
}
#endif

struct throwing_ctx : dd::null_context {
  auto transform(ready_int) {
    throw std::runtime_error("transform");
    return ready_int{0};
  }
};

// ------------------------- ctx.on_end -------------------------

// on_end may redirect `who_waits`, e.g. resume owner later on another executor
struct deferred_resume_ctx : dd::null_context {
  static inline std::vector<std::coroutine_handle<>> queue;

  template <typename P>
  static void on_end(std::coroutine_handle<P> h) noexcept {
    auto& who_waits = h.promise().who_waits;
    if (who_waits) {
      queue.push_back(who_waits);
      who_waits = std::noop_coroutine();
    }
  }
};

TEST(on_end_redirects_who_waits) {
  using dtask = dd::task<int, deferred_resume_ctx>;
  deferred_resume_ctx::queue.clear();
  auto parent = []() -> dtask {
    int x = co_await []() -> dtask { co_return 7; }();
    co_return x + 1;
  }();
  auto handle = parent.raw_handle();
  auto r = parent.start();
  // child ended, but parent is not resumed by it
  CHECK(!handle.done());
  CHECK(deferred_resume_ctx::queue.size() == 1);
  // "executor" resumes parent. Parent ends and pushes its own who_waits (noop from .start())
  auto owner = deferred_resume_ctx::queue[0];
  owner.resume();
  CHECK(handle.done());
  CHECK(handle.promise().result() == 8);
  CHECK(deferred_resume_ctx::queue.size() == 2);
}

int main() {
  RUN(exception_accessors);
  RUN(return_exception);
  RUN(set_exception_void);
  RUN(set_exception_pmr);
  RUN(propagate_without_rethrow);
  RUN(set_exception_when_all);
  RUN(transform_applies_to_awaitables);
  RUN(transform_skips_special_awaitables);
#if KELCORO_SOURCE_LOCATION_SUPPORTED
  RUN(transform_with_locations);
#endif
  RUN(on_end_redirects_who_waits);
}
