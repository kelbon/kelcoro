#pragma once

#include <span>
#include <mutex>
#include <condition_variable>

#include "job.hpp"
#include "noexport/thread_pool_monitoring.hpp"

namespace dd {

struct task_node {
  task_node* next = nullptr;
  std::coroutine_handle<> task;
};

struct thread_pool;

}  // namespace dd

namespace dd::noexport {

static auto cancel_tasks(task_node* top) noexcept {
  // there are assumption, that .destroy on handle correctly releases all resources associated with
  // coroutine and will not lead to double .destroy (assume good code)
  KELCORO_MONITORING(size_t count = 0;)
  while (top) {
    std::coroutine_handle task = top->task;
    assert(task && "dead pill must be consumed by worker");
    top = top->next;
    task.destroy();
    KELCORO_MONITORING(++count);
  }
  KELCORO_MONITORING(return count);
}

struct task_queue {
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

// schedules execution of 'foo' to executor 'e'
[[maybe_unused]] job schedule_to(auto& e KELCORO_LIFETIMEBOUND, auto foo) {
  co_await jump_on(e);
  foo();
}
// same but allocates memory with resource
template <memory_resource R>
[[maybe_unused]] job schedule_to(auto& e KELCORO_LIFETIMEBOUND, auto foo, with_resource<R>) {
  co_await jump_on(e);
  foo();
}
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
  worker(worker&&) = delete;
  void operator=(worker&&) = delete;

  ~worker() {
    stop();
  }
  // use co_await jump_on(worker) to schedule coroutine

  // precondition: node && node->task
  void attach(task_node* node) noexcept {
    assert(node && node->task);
    queue.push(node);
    KELCORO_MONITORING_INC(mon.pushed);
  }

  KELCORO_MONITORING(const monitoring_t& get_moniroting() const noexcept { return mon; })

  std::thread::id get_id() const noexcept {
    return thread.get_id();
  }

 private:
  void stop() noexcept {
    if (!thread.joinable())
      return;
    task_node pill{.next = nullptr, .task = nullptr};
    queue.push(&pill);
    thread.join();
  }
};

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
  // use co_await jump_on(strand) to schedule coroutine

  worker& get_worker() const noexcept {
    return *w;
  }
};

// schedules operations to workers
// co_await jump_on(pool) schedules coroutine to thread pool
struct thread_pool {
 private:
  worker* workers;                      // invariant: != 0
  size_t workers_size;                  // invariant: > 0
  std::pmr::memory_resource* resource;  // invariant: != 0

 public:
  static size_t default_thread_count() {
    unsigned c = std::thread::hardware_concurrency();
    return c < 2 ? 1 : c - 1;
  }

  explicit thread_pool(size_t thread_count = default_thread_count(),
                       std::pmr::memory_resource* r = std::pmr::new_delete_resource());

  ~thread_pool() {
    // if destructor started, then it is undefined behavior to push tasks
    // because its data race (between destruction and accessing to 'this' for calling 'execute')
    //
    // But there is special case - workers, which may invoke task, which .execute next task
    // in this case, .stop waits for workers consume dead pill, grab all operations from all queues
    // and cancel them(handle.destroy)
    // So, no one pushes and all what was pushed by tasks executed on workers is now destroyed,
    // no memory leak, profit!
    stop(workers, workers_size);
  }

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // use co_await jump_on(pool) to schedule coroutine

  // same as schedule_to(pool), but uses pool memory resource to allocate tasks
  void schedule(std::invocable auto&& foo, operation_hash_t hash) {
    worker& w = select_worker(hash);
    schedule_to(w, std::forward<decltype(foo)>(foo), with_resource(*resource));
  }
  void schedule(std::invocable auto&& foo) {
    schedule(std::forward<decltype(foo)>(foo), calculate_operation_hash(foo));
  }

  KELCORO_PURE std::span<const worker> workers_range() noexcept KELCORO_LIFETIMEBOUND {
    return std::span(workers, workers_size);
  }

  strand get_strand(operation_hash_t op_hash) KELCORO_LIFETIMEBOUND {
    return strand(select_worker(op_hash));
  }
  worker& select_worker(operation_hash_t op_hash) noexcept KELCORO_LIFETIMEBOUND {
    return workers[op_hash % workers_size];
  }
  std::pmr::memory_resource& get_resource() const noexcept {
    assert(resource);
    return *resource;
  }

 private:
  // should be called exactly once
  void stop(worker* w, size_t count) noexcept;
};

struct jump_on_thread_pool : task_node {
 private:
  thread_pool& tp;

 public:
  explicit jump_on_thread_pool(thread_pool& e) noexcept : tp(e), task_node(nullptr, nullptr) {
  }
  static bool await_ready() noexcept {
    return false;
  }
  template <typename P>
  void await_suspend(std::coroutine_handle<P> handle) noexcept {
    // set task before it is attached
    task = handle;
    worker& w = tp.select_worker(calculate_operation_hash(handle));
    w.attach(this);
  }
  static void await_resume() noexcept {
  }
};

struct jump_on_worker : task_node {
  worker& w;
  // creates task node and attaches it
  explicit jump_on_worker(worker& w) noexcept : task_node(nullptr, nullptr), w(w) {
  }

  static bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle) noexcept {
    // set task before it is attached
    task = handle;
    w.attach(this);
  }
  static void await_resume() noexcept {
  }
};

KELCORO_CO_AWAIT_REQUIRED inline co_awaiter auto jump_on(worker& w KELCORO_LIFETIMEBOUND) noexcept {
  return jump_on_worker(w);
}
KELCORO_CO_AWAIT_REQUIRED inline co_awaiter auto jump_on(strand& s) noexcept {
  return jump_on(s.get_worker());
}
KELCORO_CO_AWAIT_REQUIRED inline co_awaiter auto jump_on(thread_pool& tp KELCORO_LIFETIMEBOUND) noexcept {
  return jump_on_thread_pool(tp);
}

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
  KELCORO_MONITORING(w->mon.cancelled +=) noexport::cancel_tasks(top);
}

inline thread_pool::thread_pool(size_t thread_count, std::pmr::memory_resource* r) {
  resource = r ? r : std::pmr::new_delete_resource();
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
    KELCORO_MONITORING(w.mon.cancelled +=) noexport::cancel_tasks(w.queue.pop_all_nolock());
  }
#ifdef KELCORO_ENABLE_THREADPOOL_MONITORING
  monitorings.clear();
  for (auto& w : workers)
    monitorings.emplace_back(w.get_moniroting());
#endif
  std::destroy(begin(workers), end(workers));
  resource->deallocate(this->workers, sizeof(worker) * workers_size, alignof(worker));
}

}  // namespace dd

#undef KELCORO_MONITORING
#undef KELCORO_MONITORING_INC
