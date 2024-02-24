#pragma once

#include <span>
#include <latch>
#include <mutex>
#include <condition_variable>

#include "job.hpp"

#if !defined(NDEBUG) && !defined(KELCORO_DISABLE_MONITORING)
#define KELCORO_ENABLE_THREADPOOL_MONITORING
#endif

// TODO соединить таски в асинхронный стек + контекст каждой, чтобы можно было пройти?.. По каналу уже так
// можно по сути т.е. можно просто для канала сделать эту операцию и автоматически готово! Проход по циклу
// вверх
// + на каждом участке взять контекст! (и иметь контекст...)
// TODO сделать combine_event_pools, которое из различных источников типа нетворк ивенты, файл систем ивенты
// таймеры и проч проч поллит и предоставляет как генератор ивентов просто
// TODO any executor (ref)
// TODO hmm, нужно использовать ресурс и при аллокации тасок?..
// TODO специаилизированный под корутины ресурсы, который имеет внутри таблицу размер-алигн и фри листы, всё
// это в тредлокалах вероятно
//
// план такой:
// тесты, бенчмарк, дальше улучшать/менять и смотреть на бенч

namespace dd {

#ifdef KELCORO_ENABLE_THREADPOOL_MONITORING

struct monitoring_t {
  // all values only grow
  // TODO хм, убрать атомики вообще, забирать значения только когда воркер остановлен?
  std::atomic_size_t pushed = 0;
  std::atomic_size_t finished = 0;
  std::atomic_size_t strands_count = 0;
  // count of pop_all from queue
  std::atomic_size_t pop_count = 0;
  std::atomic_size_t sleep_count = 0;

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
           slp = sleep_percent(pc, slc), avr_tp = average_tasks_popped(pc, f), pending = pending_count(p, f);
    // clang-format off
    out << "pushed:               " << p;
    out << "finished:             " << f;
    out << "strands_count:        " << sc;
    out << "pop_count:            " << pc;
    out << "sleep_count:          " << slc;
    out << "sleep%:               " << slp;
    out << "average tasks popped: " << avr_tp;
    out << "pending count:        " << pending;
    // clang-format on
  }
};

#define KELCORO_MONITORING(...) __VA_ARGS__
#define KELCORO_MONITORING_INC(x) KELCORO_MONITORING(x.fetch_add(1, ::std::memory_order::relaxed))
#else
#define KELCORO_MONITORING(...)
#define KELCORO_MONITORING_INC(x)
#endif

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

// TODO специальный эксепшн завершающий тред выкидывать, т.е. вместо кучи ифов обойтись одним
// вызовом try / catch получается, но если без исключений, то..?
struct task_node {
  task_node* next = nullptr;
  std::coroutine_handle<> task;
};

template <typename T>
concept co_executor = executor<T> && requires(T& w, task_node* node) {
  w.attach(node);
  { w.transition() } -> co_awaiter;
};

struct thread_pool;

}  // namespace dd

namespace dd::noexport {

static void cancel_tasks(task_node* top) noexcept {
  // cancel tasks
  // there are assumption, that .destroy on handle correctly releases all resources associated with
  // coroutine and will not lead to double .destroy
  // (assume good code)
  while (top) {
    std::coroutine_handle task = top->task;
    assert(task && "dead pill must be consumed by worker");
    top = top->next;
    task.destroy();
  }
}

template <typename E>
struct KELCORO_CO_AWAIT_REQUIRED create_task_node_and_attach {
 protected:
  E& e;
  task_node node;

 public:
  explicit create_task_node_and_attach(E& e) noexcept : e(e) {
  }

  static bool await_ready() noexcept {
    return false;
  }
  template <typename P>
  void await_suspend(std::coroutine_handle<P> handle) noexcept {
    // set task before it is attached
    node.task = handle;
    e.attach(&node, calculate_operation_hash(handle));
  }
  static void await_resume() noexcept {
  }
};

struct alignas(hardware_destructive_interference_size) task_queue {
 private:
  task_node* first = nullptr;
  // if !first, 'last' value unspecified
  // if first, then 'last' present too
  task_node* last = nullptr;
  std::mutex mtx;
  std::condition_variable pushed;

  friend struct dd::thread_pool;

  // precondition: node && node->next
  void push_nolock(task_node* node) noexcept {
    if (first) {
      last->next = node;
      last = node;
    } else {
      first = last = node;
    }
  }
  [[nodiscard]] task_node* pop_all_nolock() noexcept {
    return std::exchange(first, nullptr);
  }

 public:
  // precondition: node != nullptr, node is not contained in queue
  void push(task_node* node) {
    node->next = nullptr;
    {
      std::lock_guard l(mtx);
      push_nolock(node);
    }
    pushed.notify_one();
  }

  [[nodiscard]] task_node* pop_all() {
    task_node* tasks;
    {
      std::lock_guard l(mtx);
      tasks = pop_all_nolock();
    }
    return tasks;
  }

  // blocking
  // postcondition: task_node != nullptr
  [[nodiscard]] task_node* pop_all_not_empty(KELCORO_MONITORING(bool& sleeped)) {
    task_node* nodes;
    {
      std::unique_lock l(mtx);
      KELCORO_MONITORING(sleeped = !first);
      while (!first)
        pushed.wait(l);
      nodes = pop_all_nolock();
    }
    assert(!!nodes);
    return nodes;
  }
};

}  // namespace dd::noexport

namespace dd {

// executes tasks on one thread, not movable
// expensive to create
struct worker {
 private:
  noexport::task_queue queue;
  KELCORO_MONITORING(monitoring_t mon);
  std::thread thread;

  friend struct strand;
  friend thread_pool;
  static void worker_job(worker* w) noexcept;

 public:
  worker() : queue(), thread(&worker_job, this) {
  }
  // precondition: node && node->task
  void attach(task_node* node) noexcept {
    assert(node && node->task);
    queue.push(node);
    KELCORO_MONITORING_INC(mon.pushed);
  }

  KELCORO_MONITORING(const monitoring_t& get_moniroting() const noexcept { return mon; })

  // schedules coroutine to be executed on thread pool
  co_awaiter auto transition() noexcept {
    return noexport::create_task_node_and_attach<worker>{*this};
  }

  // creates coroutine with will invoke 'foo' on thread pool
  [[maybe_unused]] job execute(std::invocable auto foo) KELCORO_LIFETIMEBOUND {
    co_await transition();
    foo();
  }
};

// blocks until all 'foos' executed
// if exception thrown, std::terminate called
void execute_parallel(co_executor auto& executor, auto&& f, auto&&... foos) noexcept {
  std::latch all_done(sizeof...(foos));
  operation_hash_t hash = 62;  // because why not 62?)
  auto execute_one = [&](auto& task) {
    return [&all_done, &task]() {
      task();  // terminate on exception
      all_done.count_down();
    };
  };
  // different hash for each fn, max parallel
  ((++hash, executor.execute(execute_one(foos), hash)), ...);
  f();  // last one executed on this thread
  all_done.wait();
}

// executes tasks on one thread
// works as lightweight worker ref
// very cheap to create and copy
struct strand {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  worker* w = nullptr;

  friend thread_pool;

 public:
  explicit strand(worker& wo KELCORO_LIFETIMEBOUND) : w(&wo) {
    KELCORO_MONITORING_INC(w->mon.strands_count);
  }

  // precondition: node && node->task
  void attach(task_node* node) noexcept {
    w->attach(node);
  }

  // schedules coroutine to be executed on thread pool
  co_awaiter auto transition() noexcept {
    return w->transition();
  }

  // creates coroutine with will invoke 'foo' on thread pool
  [[maybe_unused]] job execute(std::invocable auto&& foo) KELCORO_LIFETIMEBOUND {
    return w->execute(std::forward<decltype(foo)>(foo));
  }
};

struct thread_pool {
 private:
  // invariant: .size never changed, .size > 0
  worker* workers;
  size_t workers_size;
  std::pmr::memory_resource* resource;

 public:
  static size_t default_thread_count() {
    unsigned c = std::thread::hardware_concurrency();
    return c < 2 ? 1 : c - 1;
  }

  explicit thread_pool(size_t thread_count = default_thread_count(),
                       std::pmr::memory_resource* r = std::pmr::new_delete_resource());

  ~thread_pool() {
    stop(workers, workers_size);
  }

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // precondition: node && node->task
  void attach(task_node* node, operation_hash_t hash) noexcept {
    // if destructor started, then it is undefined behavior to push tasks
    // because its data race (between destruction and accessing to 'this' for calling 'execute')
    //
    // But there is special case - workers, which may invoke task, which .execute next task
    // in this case, .stop waits for workers consume dead pill, grab all operations from all queues
    // and cancel them(handle.destroy)
    // So, no one pushes and all what was pushed by tasks executed on workers is now destroyed,
    // no memory leak, profit!

    auto& w = select_worker(hash);
    w.attach(node);
  }
  // precondition: node && node->task
  void attach(task_node* node) noexcept {
    return attach(node, calculate_operation_hash(node->task));
  }

  // schedules coroutine to be executed on thread pool
  co_awaiter auto transition() noexcept {
    return noexport::create_task_node_and_attach<thread_pool>{*this};
  }
  co_awaiter auto transition(operation_hash_t hash) noexcept {
    worker& w = select_worker(hash);
    return w.transition();
  }

  // creates coroutine with will invoke 'foo' on thread pool
  [[maybe_unused]] job execute(std::invocable auto&& foo, operation_hash_t hash) KELCORO_LIFETIMEBOUND {
    worker& w = select_worker(hash);
    return w.execute(std::forward<decltype(foo)>(foo));
  }
  [[maybe_unused]] job execute(std::invocable auto&& foo) {
    return execute(std::forward<decltype(foo)>(foo), calculate_operation_hash(foo));
  }

  // this value dont changed after thread_pool creation
  KELCORO_PURE size_t workers_count() const noexcept {
    return workers_size;
  }

  std::span<const worker> workers_range() noexcept KELCORO_LIFETIMEBOUND {
    return std::span(workers, workers_size);
  }

  strand get_strand(operation_hash_t op_hash) KELCORO_LIFETIMEBOUND {
    return strand(select_worker(op_hash));
  }

 private:
  worker& select_worker(operation_hash_t op_hash) noexcept {
    return workers[op_hash % workers_size];
  }

  // should be called exactly once
  void stop(worker* w, size_t count) noexcept;
};

template <>
struct KELCORO_CO_AWAIT_REQUIRED jump_on<thread_pool> : noexport::create_task_node_and_attach<thread_pool> {};

template <>
struct KELCORO_CO_AWAIT_REQUIRED jump_on<strand> : noexport::create_task_node_and_attach<strand> {};

template <>
struct KELCORO_CO_AWAIT_REQUIRED jump_on<worker> : noexport::create_task_node_and_attach<worker> {};

inline void worker::worker_job(worker* w) noexcept {
  assert(w);
  task_node* top;
  std::coroutine_handle task;
  noexport::task_queue* queue = &w->queue;
  KELCORO_MONITORING(bool sleeped);
  while (true) {
    top = queue->pop_all_not_empty(KELCORO_MONITORING(sleeped));
    KELCORO_MONITORING(if (sleeped) KELCORO_MONITORING_INC(w->mon.sleep_count));
    KELCORO_MONITORING_INC(w->mon.pop_count);
    assert(top);
    do {
      // grab task from memory which will be invalidated after task.resume()
      task = top->task;
      // ++ before invoking a task
      top = top->next;
      if (!task) [[unlikely]]
        goto work_end;  // dead pill
      // if exception thrown, std::terminate called
      task.resume();
      KELCORO_MONITORING_INC(w->mon.finished);
    } while (top);
  }
work_end:
  // after this point .stop in thread pool cancels all pending tasks in queues for all workers
  noexport::cancel_tasks(top);
}

inline thread_pool::thread_pool(size_t thread_count, std::pmr::memory_resource* r) {
  resource = r;
  workers_size = std::max<size_t>(1, thread_count);
  workers = (worker*)r->allocate(sizeof(worker) * workers_size, alignof(worker));

  bool exception = true;
  size_t i = 0;
  scope_exit _([&] {
    if (exception)
      stop(workers, i);
  });
  for (; i < workers_size; ++i) {
    worker* ptr = &workers[i];
    new (ptr) worker;
  }
  exception = false;
}

inline void thread_pool::stop(worker* w, size_t count) noexcept {
  task_node pill{.next = nullptr, .task = nullptr};
  std::span workers(w, count);
  for (auto& w : workers)
    w.queue.push(&pill);
  for (auto& w : workers) {
    assert(w.thread.joinable());
    w.thread.join();
  }
  // here all workers stopped, cancel tasks
  for (auto& w : workers) {
    assert(w.queue.mtx.try_lock() && "no one should lock this mutex now!");
    assert((w.queue.mtx.unlock(), true));
    noexport::cancel_tasks(w.queue.pop_all_nolock());
  }
  std::destroy(begin(workers), end(workers));
  resource->deallocate(this->workers, sizeof(worker) * workers_size, alignof(worker));
}

}  // namespace dd

#undef KELCORO_MONITORING
#undef KELCORO_MONITORING_INC
