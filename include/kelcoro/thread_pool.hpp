#pragma once

#include <span>
#include <mutex>
#include <condition_variable>

#include "job.hpp"
#include "executor_interface.hpp"
#include "noexport/thread_pool_monitoring.hpp"
#include "common.hpp"

namespace dd::noexport {

static auto cancel_tasks(task_node* top) noexcept {
  // set status and invoke tasks once,
  // assume after getting 'cancelled' status task do not produce new tasks
  KELCORO_MONITORING(size_t count = 0;)
  while (top) {
    std::coroutine_handle task = top->task;
    assert(task && "dead pill must be consumed by worker");
    top->status = schedule_errc::cancelled;
    top = top->next;
    task.resume();
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
  std::condition_variable not_empty;
  [[nodiscard]] task_node* pop_all_nolock() noexcept {
    return std::exchange(first, nullptr);
  }

 public:
  // precondition: node != nullptr && node is not contained in queue
  void push(task_node* node) {
    node->next = nullptr;
    std::lock_guard l{mtx};
    if (first) {
      last->next = node;
      last = node;
    } else {
      first = last = node;
      // notify under lock, because of loop in 'pop_all_not_empty'
      // while (!first) |..missed notify..| wait
      // this may block worker thread forever if no one notify after that
      not_empty.notify_one();
    }
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
        not_empty.wait(l);
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
  if (!co_await jump_on(e)) [[unlikely]]
    co_return;
  foo();
}
// same but allocates memory with resource
template <memory_resource R>
[[maybe_unused]] job schedule_to(auto& e KELCORO_LIFETIMEBOUND, auto foo, with_resource<R>) {
  if (!co_await jump_on(e)) [[unlikely]]
    co_return;
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
  friend struct thread_pool;
  static void worker_job(worker* w) noexcept;

 public:
  worker() : queue(), thread(&worker_job, this) {
  }
  worker(worker&&) = delete;
  void operator=(worker&&) = delete;

  ~worker() {
    if (!thread.joinable())
      return;
    task_node pill{.next = nullptr, .task = nullptr};
    queue.push(&pill);
    thread.join();
    noexport::cancel_tasks(queue.pop_all());
  }
  // use co_await jump_on(worker) to schedule coroutine

  // precondition: node && node->task && node.status == ok
  void attach(task_node* node) noexcept {
    assert(node && node->task && node->status == schedule_errc::ok);
    queue.push(node);
    KELCORO_MONITORING_INC(mon.pushed);
  }

  KELCORO_MONITORING(const monitoring_t& get_moniroting() const noexcept { return mon; })

  std::thread::id get_id() const noexcept {
    return thread.get_id();
  }
  // precondition: caller should not break thread, e.g. do not use .join, .detach, .swap
  // but may use it to set thread name or pinning thread to core
  std::thread& get_thread() noexcept {
    return thread;
  }
  const std::thread& get_thread() const noexcept {
    return thread;
  }
};

// executes tasks on one thread
// works as lightweight worker ref
// very cheap to create and copy
struct strand {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  worker* w = nullptr;

 public:
  explicit strand(worker& wo KELCORO_LIFETIMEBOUND) : w(&wo) {
    KELCORO_MONITORING_INC(w->mon.strands_count);
  }
  // use co_await jump_on(strand) to schedule coroutine

  void attach(task_node* node) const noexcept {
    w->attach(node);
  }
  worker& get_worker() const noexcept {
    return *w;
  }
};

// distributes tasks among workers
// co_await jump_on(pool) schedules coroutine to thread pool
// note: when thread pool dies, all pending tasks invoked with errc::cancelled
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
    // because its data race (between destruction and accessing to 'this' for scheduling new task)
    //
    // But there is special case - workers, which may invoke task, which schedules next task
    // in this case, .stop waits for workers consume dead pill, grab all operations from all queues
    // and cancel them (resume with special errc)
    // So, no one pushes and all what was pushed by tasks executed on workers is cancelled,
    // no memory leak, profit!
    stop(workers, workers_size);
  }

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // use co_await jump_on(pool) to schedule coroutine

  void attach(task_node* node) noexcept {
    worker& w = select_worker(calculate_operation_hash(node->task));
    w.attach(node);
  }

  // same as schedule_to(pool), but uses pool memory resource to allocate tasks
  void schedule(std::invocable auto&& foo, operation_hash_t hash) {
    worker& w = select_worker(hash);
    schedule_to(w, std::forward<decltype(foo)>(foo), with_resource{*resource});
  }
  void schedule(std::invocable auto&& foo) {
    schedule(std::forward<decltype(foo)>(foo), calculate_operation_hash(foo));
  }

  KELCORO_PURE std::span<worker> workers_range() noexcept KELCORO_LIFETIMEBOUND {
    return std::span(workers, workers_size);
  }
  KELCORO_PURE std::span<const worker> workers_range() const noexcept KELCORO_LIFETIMEBOUND {
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

// specialization for thread pool uses hash to maximize parallel execution
inline void attach_list(thread_pool& e, task_node* top) {
  operation_hash_t hash = 0;
  while (top) {
    task_node* next = top->next;
    e.select_worker(hash).attach(top);
    ++hash;
    top = next;
  }
}

struct jump_on_thread_pool : private create_node_and_attach<thread_pool> {
  using base_t = create_node_and_attach<thread_pool>;

  explicit jump_on_thread_pool(thread_pool& e) noexcept : base_t(e) {
  }

  using base_t::await_ready;
  using base_t::await_resume;

  template <typename P>
  void await_suspend(std::coroutine_handle<P> handle) noexcept {
    // set task before it is attached
    task = handle;
    worker& w = e.select_worker(calculate_operation_hash(task));
    w.attach(this);
  }
};

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
    KELCORO_MONITORING(w.mon.cancelled +=) noexport::cancel_tasks(w.queue.pop_all());
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
