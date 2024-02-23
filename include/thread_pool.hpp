#pragma once

#include <deque>
#include <latch>
#include <mutex>
#include <condition_variable>

#include "job.hpp"

// TODO соединить таски в асинхронный стек + контекст каждой, чтобы можно было пройти?.. По каналу уже так
// можно по сути т.е. можно просто для канала сделать эту операцию и автоматически готово! Проход по циклу
// вверх
// + на каждом участке взять контекст! (и иметь контекст...)
// TODO сделать combine_event_pools, которое из различных источников типа нетворк ивенты, файл систем ивенты
// таймеры и проч проч поллит и предоставляет как генератор ивентов просто
// TODO any executor (ref), for unifying strand/executor with diffrent template args
// TODO операцию спавн? Чтобы начать корутину, дальше переехать на другой тред, передать управление корутине

namespace dd {

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
// 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │ ...
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

// TODO специальный эксепшн завершающий тред выкидывать, т.е. вместо кучи ифов обойтись одним
// вызовом try / catch получается, но если без исключений, то..?
struct task_node {
  task_node* next = nullptr;
  std::coroutine_handle<> task;
};
// TODO добавить (дебажную? информацию о нагрузке тредпула,
// как минимум просто число исполненных/запушенных задач)
// Под дефайном "метрики" (если не придумаю как применить по другому информацию)
// сделать
// 1. количество исполненных задач
// 2. задание имени потока (на разных ос...) (типа pool # X worker # Y )
//
template <typename T>
concept co_executor = executor<T> && requires(T& w, task_node* node) { w.execute(node); };

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

template <typename E>
struct KELCORO_CO_AWAIT_REQUIRED create_task_node_and_attach_with_operation_hash
    : private create_task_node_and_attach<E> {
 private:
  operation_hash_t hash = 0;

  using base_t = create_task_node_and_attach<E>;

 public:
  create_task_node_and_attach_with_operation_hash(E& e KELCORO_LIFETIMEBOUND, operation_hash_t hash) noexcept
      : create_task_node_and_attach<E>(e), hash(hash) {
  }
  using base_t::await_ready;
  using base_t::await_resume;

  void await_suspend(std::coroutine_handle<> handle) noexcept {
    // set task before it is attached
    this->node.task = handle;
    this->e.attach(&this->node, hash);
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
  [[nodiscard]] task_node* pop_all_not_empty() {
    task_node* nodes;
    {
      std::unique_lock l(mtx);
      while (!first)
        pushed.wait(l);
      nodes = pop_all_nolock();
    }
    assert(!!nodes);
    return nodes;
  }
};

void worker_job(task_queue*) noexcept;

struct worker_t {
  task_queue queue;
  std::thread thread;
  worker_t() : thread(worker_job, &queue) {
  }
};

inline void worker_job(task_queue* queue) noexcept {
  assert(queue);
  task_node* top;
  std::coroutine_handle task;
  while (true) {
    top = queue->pop_all_not_empty();
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
  cancel_tasks(top);
}

}  // namespace dd::noexport

namespace dd {

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

// TODO interface
struct strand {
 private:
  // invariant: != nullptr, ptr for trivial copy/move
  noexport::worker_t* w = nullptr;

  friend struct thread_pool;
  explicit strand(noexport::worker_t& w KELCORO_LIFETIMEBOUND) : w(&w) {
  }

 public:
  // TODO same interface as thread pool
  void execute(task_node* node) noexcept {
    w->queue.push(node);
  }

  co_awaiter auto transition() noexcept {
    return noexport::create_task_node_and_attach<strand>{*this};
  }

  void execute(auto&& foo) {
    execute_sequentially(*this, std::forward<decltype(foo)>(foo));
  }
};

struct thread_pool {
 private:
  // invariant: .size never changed, .size > 0
  std::deque<noexport::worker_t> workers;

 public:
  static size_t default_thread_count() {
    unsigned c = std::thread::hardware_concurrency();
    return c < 2 ? 1 : c - 1;
  }

  explicit thread_pool(size_t thread_count = default_thread_count()) {
    thread_count = std::max<size_t>(1, thread_count);
    bool exception = true;
    scope_exit _([&] {
      if (exception)
        stop();
    });
    for (; thread_count; --thread_count)
      workers.emplace_back();
    exception = false;
  }

  ~thread_pool() {
    stop();
  }

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // precondition: node && node->task
  void attach(task_node* node, operation_hash_t hash) noexcept {
    assert(node && node->task);
    // if destructor started, then it is undefined behavior to push tasks
    // because its data race (between destruction and accessing to 'this' for calling 'execute')
    //
    // But there is special case - workers, which may invoke task, which .execute next task
    // in this case, .stop waits for workers consume dead pill, grab all operations from all queues
    // and cancel them(handle.destroy)
    // So, no one pushes and all what was pushed by tasks executed on workers is now destroyed,
    // no memory leak, profit!

    auto& w = select_worker(hash);
    w.queue.push(node);
  }
  void attach(task_node* node) noexcept {
    return attach(node, calculate_operation_hash(node->task));
  }

  co_awaiter auto transition() noexcept {
    return noexport::create_task_node_and_attach<thread_pool>{*this};
  }
  co_awaiter auto transition(operation_hash_t hash) noexcept {
    return noexport::create_task_node_and_attach_with_operation_hash<thread_pool>{*this, hash};
  }

  [[maybe_unused]] job execute(std::invocable auto foo, operation_hash_t hash) KELCORO_LIFETIMEBOUND {
    co_await transition(hash);
    foo();
  }
  [[maybe_unused]] job execute(std::invocable auto&& foo) {
    return execute(std::forward<decltype(foo)>(foo), calculate_operation_hash(foo));
  }

  // this value dont changed after thread_pool creation
  KELCORO_PURE size_t workers_count() const noexcept {
    return workers.size();
  }
  // TODO std::ranges::range auto workers() noexcept KELCORO_LIFETIMEBOUND {
  // TODO   return workers | std::views::transform(to strand);
  // TODO }

  // TODO? хм, может просто назвать это worker_ref?
  strand get_strand(operation_hash_t op_hash) KELCORO_LIFETIMEBOUND {
    // TODO как-то специально выбирать мб, чтобы равномерно хотя бы было. Ну или тредлокал переменная номерок
    return strand(select_worker(op_hash));
  }

 private:
  noexport::worker_t& select_worker(operation_hash_t op_hash) noexcept {
    // TODO автобалансировка через изменение чиселки и проверку нагрузки (через какую то хрень типа среднее
    //  квадартичное отклонение количества тасок на воркерах и тд). Если точнее - КОЭФФИЦИЕНТ ДЖИННИ!))
    // прибавление просто числа здесь это просто сдвиг нагрузки без смены распределения (который тоже имеет
    // смысл) если на какой то тред попало несколько стрендов (т.к. нагрузка стрендов не сдвигается)
    // Но можно придумать преобразование, которое будет менять распределение нагрузки как-нибудь и подбирать
    // его
    return workers[op_hash % workers.size()];
  }

  // should be called exactly once
  void stop() noexcept {
    task_node pill{.next = nullptr, .task = nullptr};
    for (auto& w : workers)
      w.queue.push(&pill);
    for (auto& w : workers) {
      KELCORO_ASSUME(w.thread.joinable());
      w.thread.join();
    }
    // here all workers stopped, cancel tasks
    for (auto& w : workers) {
      assert(w.queue.mtx.try_lock() && "no one should lock this mutex now!");
      assert((w.queue.mtx.unlock(), true));
      noexport::cancel_tasks(w.queue.pop_all_nolock());
    }
  }
};

template <>
struct KELCORO_CO_AWAIT_REQUIRED jump_on<thread_pool> : noexport::create_task_node_and_attach<thread_pool> {};

template <>
struct KELCORO_CO_AWAIT_REQUIRED jump_on<strand> : noexport::create_task_node_and_attach<strand> {};

}  // namespace dd
