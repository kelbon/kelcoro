#pragma once

#include <iostream>
#include <span>
#include <mutex>
#include <condition_variable>

#include "common.hpp"
#include "job.hpp"
#include "executor_interface.hpp"
#include "kelcoro/noexport/macro.hpp"
#include "noexport/fixed_array.hpp"
#include "noexport/thread_pool_monitoring.hpp"

namespace dd::noexport {

static void cancel_tasks(task_node* top) noexcept {
  // set status and invoke tasks once,
  // assume after getting 'cancelled' status task do not produce new task
  while (top) {
    // if this task is the final one, then the work must be completed.
    // after resume top deleted
    std::coroutine_handle task = top->task;
    top->status = schedule_errc::cancelled;
    top = top->next;
    if (task) {
      task.resume();
    }
  }
}

}  // namespace dd::noexport

namespace dd {

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
    push_list(node, node);
  }

  // attach a whole linked list
  void push_list(task_node* first_, task_node* last_) {
    assert(first_ && last_);
    std::lock_guard l{mtx};
    if (first) {
      last->next = first_;
      last = last_;
    } else {
      first = first_;
      last = last_;
      // notify under lock, because of loop in 'pop_all_not_empty'
      // while (!first) |..missed notify..| wait
      // this may block worker thread forever if no one notify after that
      not_empty.notify_one();
    }
  }

  void push_list(task_node* first_) {
    if (!first_) {
      return;
    }
    auto last_ = first_;
    while (last_->next) {
      last_ = last_->next;
    }
    push_list(first_, last_);
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
  [[nodiscard]] task_node* pop_all_not_empty() {
    task_node* nodes;
    {
      std::unique_lock l(mtx);
      while (!first)
        not_empty.wait(l);
      nodes = pop_all_nolock();
    }
    assert(!!nodes);
    return nodes;
  }

  // precondition: node && node->task && node.status == ok
  // executor interface
  void attach(task_node* node) noexcept {
    assert(node && node->task && node->status == schedule_errc::ok);
    push(node);
  }
};

static_assert(executor<task_queue>);

namespace noexport {

// We push the deadpill, and behind it ourselves,
// in a chain, so that the deadpill is definitely alive.
struct deadpill_pusher {
  task_queue& queue;

  // put deadpill on this awater and push them together.
  task_node pill = task_node::deadpill();
  task_node this_node;

  static bool await_ready() noexcept {
    return false;
  }
  void await_suspend(std::coroutine_handle<> handle) {
    pill.next = &this_node;
    this_node.task = handle;
    // avoiding race between tasks
    queue.push_list(&pill, &this_node);
  }
  void await_resume() noexcept {
    assert(this_node.status == schedule_errc::cancelled);
  }
};

inline dd::job push_deadpill(task_queue& queue) {
  co_await deadpill_pusher(queue);
}

}  // namespace noexport

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

void default_worker_job(task_queue& queue) noexcept;

// executes tasks on one thread, not movable
// expensive to create
struct worker {
 private:
  task_queue queue;
  KELCORO_MONITORING(monitoring_t mon);
  std::thread thread;

  friend struct strand;
  friend struct thread_pool;

 public:
  // the function must process the queue, pop tasks from it,
  // and also be able to process task_node::deadpill
  // for example: default_worker_job
  using job_t = void (&)(task_queue&);

  worker(job_t job = default_worker_job) : queue(), thread(job, std::ref(queue)) {
  }
  worker(worker&&) = delete;
  void operator=(worker&&) = delete;

  ~worker() {
    if (!thread.joinable()) {
      noexport::cancel_tasks(queue.pop_all());
      return;
    }
    auto pill = task_node::deadpill();
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
// note: when thread pool dies, all pending tasks invoked with schedule_errc::cancelled
struct thread_pool {
 private:
  noexport::fixed_array<worker> workers;  // invariant: size > 0
  std::pmr::memory_resource* resource;    // invariant: != 0

 public:
  static size_t default_thread_count() {
    unsigned c = std::thread::hardware_concurrency();
    return c < 2 ? 1 : c - 1;
  }

  explicit thread_pool(size_t thread_count = default_thread_count(),
                       std::pmr::memory_resource& r = *std::pmr::get_default_resource())
      : workers(std::max<size_t>(1, thread_count), r), resource(&r) {
  }

  ~thread_pool() {
    task_node pill = task_node::deadpill();
    for (auto& w : workers) {
      if (w.thread.joinable()) {
        w.queue.push(&pill);
        w.thread.join();
      }
    }
    for (auto& w : workers) {
      noexport::cancel_tasks(w.queue.pop_all());
    }
  }

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // use co_await jump_on(pool) to schedule coroutine
  void attach(task_node* node) noexcept {
    worker& w = select_worker(calculate_operation_hash(node->task));
    w.attach(node);
  }

  [[nodiscard]] bool is_worker(std::thread::id id = std::this_thread::get_id()) {
    for (worker& w : workers) {
      if (w.thread.get_id() == id) {
        return true;
      }
    }
    return false;
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
    return std::span(workers.data(), workers.size());
  }
  KELCORO_PURE std::span<const worker> workers_range() const noexcept KELCORO_LIFETIMEBOUND {
    return std::span(workers.data(), workers.size());
  }

  strand get_strand(operation_hash_t op_hash) KELCORO_LIFETIMEBOUND {
    return strand(select_worker(op_hash));
  }
  worker& select_worker(operation_hash_t op_hash) noexcept KELCORO_LIFETIMEBOUND {
    return workers[op_hash % workers.size()];
  }
  std::pmr::memory_resource& get_resource() const noexcept {
    assert(resource);
    return *resource;
  }

  void request_stop() {
    for (auto& w : workers) {
      noexport::push_deadpill(w.queue);
    }
  }

  // NOTE: can't be called from workers
  // NOTE: can't be called more than once
  // Wait for the job to complete (after calling `request_stop`)
  void wait_stop() {
    assert(!is_worker());
    for (auto& w : workers) {
      if (w.thread.joinable()) {
        w.thread.join();
      }
    }
  }
};

// specialization for thread pool uses hash to maximize parallel execution
inline void attach_list(thread_pool& e, task_node* top) {
  operation_hash_t hash = noexport::do_hash(top);
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

inline void default_worker_job(task_queue& queue) noexcept {
  task_node* top;
  std::coroutine_handle task;
  for (;;) {
    top = queue.pop_all_not_empty();
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
    } while (top);
  }
work_end:
  // after this point .stop in thread pool cancels all pending tasks in queues for all workers
  noexport::cancel_tasks(top);
}

}  // namespace dd

#undef KELCORO_MONITORING
#undef KELCORO_MONITORING_INC
