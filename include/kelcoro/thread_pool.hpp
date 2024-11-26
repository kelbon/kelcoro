#pragma once

#include <memory_resource>
#include <span>
#include <mutex>
#include <condition_variable>

#include "job.hpp"
#include "executor_interface.hpp"
#include "memory_support.hpp"
#include "noexport/fixed_array.hpp"
#include "noexport/macro.hpp"
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
    if (task)
      task.resume();
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
    if (!first_)
      return;
    auto last_ = first_;
    while (last_->next)
      last_ = last_->next;
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
    assert(node && node->task);
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
  co_await deadpill_pusher{queue};
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
  std::thread thread;

  friend struct strand;
  friend struct thread_pool;

 public:
  // the function must process the queue, pop tasks from it,
  // and also be able to process task_node::deadpill
  // for example: default_worker_job
  using job_t = void (*)(task_queue&);

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
    assert(node && node->task);
    queue.push(node);
  }

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
  task_queue* q = nullptr;

 public:
  explicit strand(worker& wo KELCORO_LIFETIMEBOUND) : q(&wo.queue) {
  }
  explicit strand(task_queue& qu KELCORO_LIFETIMEBOUND) : q(&qu) {
  }
  // use co_await jump_on(strand) to schedule coroutine

  void attach(task_node* node) const noexcept {
    q->attach(node);
  }
  task_queue& get_queue() const noexcept {
    return *q;
  }
};

// distributes tasks among workers
// co_await jump_on(pool) schedules coroutine to thread pool
// note: when thread pool dies, all pending tasks invoked with schedule_errc::cancelled
struct thread_pool {
 private:
  noexport::fixed_array<task_queue> queues;    // invariant: queues.size() == threads.size()
  noexport::fixed_array<std::thread> threads;  // invariant: size > 0
 public:
  static size_t default_thread_count() {
    unsigned c = std::thread::hardware_concurrency();
    return c < 2 ? 1 : c - 1;
  }

  explicit thread_pool(size_t thread_count = default_thread_count(), worker::job_t job = default_worker_job,
                       std::pmr::memory_resource& r = *std::pmr::get_default_resource())
      : queues(std::max<size_t>(1, thread_count), r), threads(std::max<size_t>(1, thread_count), r) {
    for (size_t i = 0; i < threads.size(); i++)
      threads[i] = std::thread(job, std::ref(queues[i]));
  }

  ~thread_pool() {
    // You can't just reset it because the queue is inside the worker,
    // and in that case, while the stop was
    // going on, someone could push into the dead queue.
    task_node pill = task_node::deadpill();
    for (size_t i = 0; i < threads.size(); i++) {
      if (threads[i].joinable()) {
        queues[i].push(&pill);
        threads[i].join();
      }
    }
    for (auto& q : queues)
      noexport::cancel_tasks(q.pop_all());
  }

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // use co_await jump_on(pool) to schedule coroutine
  void attach(task_node* node) noexcept {
    task_queue& q = select_queue(calculate_operation_hash(node->task));
    q.attach(node);
  }

  [[nodiscard]] bool is_worker(std::thread::id id = std::this_thread::get_id()) {
    for (auto& t : threads)
      if (t.get_id() == id)
        return true;
    return false;
  }

  // same as schedule_to(pool), but uses pool memory resource to allocate tasks
  void schedule(std::invocable auto&& foo, operation_hash_t hash) {
    task_queue& w = select_queue(hash);
    schedule_to(w, std::forward<decltype(foo)>(foo), with_resource{*queues.get_resource()});
  }
  void schedule(std::invocable auto&& foo) {
    schedule(std::forward<decltype(foo)>(foo), calculate_operation_hash(foo));
  }

  KELCORO_PURE std::span<std::thread> workers_range() noexcept KELCORO_LIFETIMEBOUND {
    return std::span(threads.data(), threads.size());
  }
  KELCORO_PURE std::span<const std::thread> workers_range() const noexcept KELCORO_LIFETIMEBOUND {
    return std::span(threads.data(), threads.size());
  }

  strand get_strand(operation_hash_t op_hash) KELCORO_LIFETIMEBOUND {
    return strand(select_queue(op_hash));
  }
  task_queue& select_queue(operation_hash_t op_hash) noexcept KELCORO_LIFETIMEBOUND {
    return queues[op_hash % queues.size()];
  }
  std::pmr::memory_resource& get_resource() const noexcept {
    return *queues.get_resource();
  }

  // can be called as many times as you want from any threads
  void request_stop() {
    for (auto& q : queues)
      noexport::push_deadpill(q);
  }

  // NOTE: can't be called from workers
  // NOTE: can't be called more than once
  // Wait for the job to complete (after calling `request_stop`)
  void wait_stop() && {
    assert(!is_worker());
    for (auto& t : threads)
      if (t.joinable())
        t.join();
    for (auto& q : queues)
      noexport::cancel_tasks(q.pop_all());
  }
};

// specialization for thread pool uses hash to maximize parallel execution
inline void attach_list(thread_pool& e, task_node* top) {
  operation_hash_t hash = noexport::do_hash(top);
  while (top) {
    task_node* next = top->next;
    e.select_queue(hash).attach(top);
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
    task_queue& q = e.select_queue(calculate_operation_hash(task));
    q.attach(this);
  }
};

KELCORO_CO_AWAIT_REQUIRED inline jump_on_thread_pool jump_on(thread_pool& tp KELCORO_LIFETIMEBOUND) noexcept {
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
