#pragma once

#include <deque>
#include <latch>

#include "job.hpp"

// TODO соединить таски в асинхронный стек + контекст каждой, чтобы можно было пройти?.. По каналу уже так
// можно по сути т.е. можно просто для канала сделать эту операцию и автоматически готово! Проход по циклу
// вверх
// + на каждом участке взять контекст! (и иметь контекст...)
namespace dd {

struct task_node {
  task_node* next = nullptr;
  std::coroutine_handle<> task;
};

template <typename T>
concept co_executor = executor<T> && requires(T& w, task_node* node) { w.execute(node); };

// if 'foos' throw exception, std::terminate called
template <memory_resource R>
job execute_sequentially(co_executor auto& executor KELCORO_LIFETIMEBOUND, with_resource<R>, auto... foos) {
  // accepts by value, because tasks need to be alive
  ((co_await executor.execute(), foos()), ...);
}

job execute_sequentially(co_executor auto& executor KELCORO_LIFETIMEBOUND, auto... foos) {
  // accepts by value, because tasks need to be alive
  ((co_await executor.execute(), foos()), ...);
}

// waits all
// if exception thrown, std::terminate called
void execute_parallel(co_executor auto& executor, auto&& f, auto&&... foos) {
  static_assert(sizeof...(foos) > 0);
  std::latch all_done(sizeof...(foos) - 1);
  operation_hash_t hash = 62;
  auto execute_one = [&all_done](auto& task) {
    task();
    all_done.count_down();
  };
  // TODO? собрать исключения(со всех тредов...) и результаты.. и вернуть видимо массив из них или как то
  // так... ну да, видимо нужно возвращать тупл юнионов из ексепшна и результата...
  // но если так делать, то в последовательном исполнении тоже это нужно.. Честно говоря нахуй надо...
  ((++hash, executor.execute(foos, hash)), ...);
  [&]() noexcept { f(); }();  // last one executed on this thread
  all_done.wait();
}
void execute_parallel(co_executor auto&, auto&& f) {
  // TODO? собрать исключения(со всех тредов...) и результаты.. и вернуть видимо массив из них или как то
  // так... ну да, ивдимо нужно возвращать тупл юнионов из ексепшна и результата...
  [&]() noexcept { f(); }();
}

// TODO эту логику так то нужно добавить в jump on
template <typename E>
struct KELCORO_CO_AWAIT_REQUIRED create_task_node_and_execute {
  // invariant: != nullptr
  E& e;
  task_node node;

  static bool await_ready() noexcept {
    return false;
  }
  template <typename P>
  void await_suspend(std::coroutine_handle<P> handle) noexcept {
    // set task before it is attached
    node.task = handle;
    e.execute(&node, calculate_operation_hash(handle));
  }
  static void await_resume() noexcept {
  }
};

// returns new begin
// TODO нормальную очередь на мьютексе и это не нужно тогда
[[nodiscard]] constexpr task_node* reverse(task_node* node) noexcept {
  task_node* prev = nullptr;
  task_node* cur = node;
  while (cur) {
    task_node* next = std::exchange(cur->next, prev);
    prev = std::exchange(cur, next);
  }
  return prev;
}

// TODO сделать combine_event_pools, которое из различных источников типа нетворк ивенты, файл систем ивенты
// таймеры и проч проч поллит и предоставляет как генератор ивентов просто
// TODO !! ПИЗДЕЦ, кажется всё это не рабочая схема вообще. т.к. один тред загрузил топ
// потом второй снял все значения и удалил (вызвал)
// первый продолжил исполнение и пошёл сравнивать одно с другим - получил сегфот (удалённая память)
// TODO написать на мьютексе очередь
struct task_queue {
 private:
  task_node* top = nullptr;
  std::mutex mtx;
  std::condition_variable pushed;

 public:
  // precondition: node != nullptr
  void push(task_node* node) {
    {
      std::lock_guard l(mtx);
      node->next = std::exchange(top, node);
    }
    pushed.notify_one();
  }

  task_node* try_pop_all() {
    task_node* tasks;
    {
      std::lock_guard l(mtx);
      tasks = std::exchange(top, nullptr);
    }
    return reverse(tasks);
  }

  // waitfree, exactly one atomic operation, but O(N) time
  [[nodiscard]] task_node* pop_all() {
    task_node* nodes;
    {
      std::unique_lock l(mtx);
      pushed.wait(l, [&] { return top != nullptr; });
      nodes = std::exchange(top, nullptr);
    }
    // Хм, получается в пуше нужно исполнять самому треду который пушит, если уже началось разрушение?..
    // хмм, по сути если стренд и пушится новая задача, НА САМОМ ТРЕДЕ, то он её и должен СРАЗУ исполнять?
    // хотя тогда нарушается порядок же потенциально...
    // нужно подумать либо сделать unordered_strand хех, тогда действительно можно в обход очередей и
    // всего начать исполнять на месте если тред тот же самый
    //  В случае если пуш происходит и пушит один из тредов тредпула, ТО... впринципе кажется сейфовым
    // сразу начать исполнение там же, разве нет? Хотя кажется может начаться рекурсия внутри пуша
    // вызов пуша и так далее до бесконечности
    // можно внутри рабочего треда другой интерфейс использовать, хотя нет, это же юзер код...
    // ну и да, можно использовать тредлокалы и туда положить bool, хотя некая хеш таблица с идеальным
    // хешом выглядит лучше вероятно
    // ах да, у меня же есть ещё один сценарий! Я могу напрямую вызывать .destroy хендлов при отмене!
    // блин, а ведь это правильная стратегия так-то, только у меня дедпил и я уже исполнил остальное
    // * новых операций не пушится
    // * дедпил пришла
    // * нужно забрать все оперцаии из очереди и разрушить через .destroy
    // * потом выйти
    // важно чтобы тредпул не оказался на одной из этих корутин, хех. Тогда реально проблема может
    // получится
    // но известно, что деструктор уже запущен, значит дело на другом потоке происходит.... Хм...
    // т.е. либо мы дошли до конца корутины и вызвали деструктор, тогда будить корутину которая окончилась
    // это уб, либо деструктор запущен через .destroy(), но тогда это второй destroy, это уб
    // Может ли такая стратегия привести к двойному вызову destroy в нормальном коде?
    // never blocks forewer, because dead pill sometime will be pushed (and it will be last
    // push)
    assert(!!nodes);
    return reverse(nodes);
  }
};

void worker_job(task_queue*) noexcept;
// let 64 == hardware hashline size
struct alignas(64) worker_t {
  task_queue queue;
  std::thread thread;
  worker_t() : thread(worker_job, &queue) {
  }
};

inline void worker_job(task_queue* queue) noexcept {
  assert(!!queue);
  while (true) {
    task_node* top = queue->pop_all();
    assert(top);
    while (top) {
      // grab task from memory which will be invalidated after call to task
      std::coroutine_handle task = top->task;
      // ++ before invoking a task
      top = top->next;
      if (!task) [[unlikely]]
        goto work_end;  // dead pill
      // if exception thrown, std::terminate called
      task();
    }
  }
work_end:
  // cancel tasks
  // there are assumption, that .destroy on handle correctly releases all resources associated with
  // coroutine and will not lead to double .destroy
  // (assume good code)
  task_node* top = queue->try_pop_all();
  while (top) {
    std::coroutine_handle task = top->task;
    assert(task);
    top = top->next;
    task.destroy();
  }
  // thread pool should stop push
  assert(queue->try_pop_all() == nullptr);
}

// TODO any executor (ref), for unifying strand/executor with diffrent template args
// TODO операцию спавн? Чтобы начать корутину, дальше переехать на другой тред, передать управление корутине
struct strand {
 private:
  // invariant: != nullptr
  worker_t* w = nullptr;

  friend struct thread_pool;
  explicit strand(worker_t& w KELCORO_LIFETIMEBOUND) : w(&w) {
  }

 public:
  void execute(task_node* node) noexcept {
    w->queue.push(node);
  }

  co_awaiter auto execute() noexcept {
    return create_task_node_and_execute<strand>{*this};
  }

  void execute(auto&& foo) {
    execute_sequentially(*this, std::forward<decltype(foo)>(foo));
  }
};

struct thread_pool {
 private:
  // invariant: .size never changed, .size > 0
  std::deque<worker_t> workers;

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
  // TODO и все execute сводятся к созданию ноды, считанию хеша, вызову execute от этого...
  // TODO есть ли какой-то смысл в подмножестве тредов, т.е. strand на N тредов, а не 1?

  // precondition: node && node->task
  void execute(task_node* node, operation_hash_t hash) noexcept {
    // хм, получается я нашёл источник рандома - указатели...
    // TODO operation_hash<> специализированный под разные типы задач, например coroutine_handle<void>
    // и тд!!!
    // TODO сверху - возможность ассоциировать таску с какой то операцией (явно передать хеШ)
    // TODO счётчики, чтобы была возможность отслеживать нагрузку на разные воркеры тредпула хотя бы
    // TODO для операций с одного треда имеет смысл иметь увеличивающийся счётчик, чтобы один тред раскладывал
    // операции как раз по разным тредам, т.к. это максимально выгодно (как кажется) (параллельное исполнение)
    // TODO тредлокал счётчик для этого
    // TODO можно сделать некую таблицу из адресов воркеров, чем больше адресов воркера в ней, тем вероятнее
    // его попадание и наоборот, т.е. балансировку за счёт такого странного механизма можно сделать...

    // эвристика №2 - один и тот же тред пушит на один и тот же тред?.. Причём желательно сам на себя,
    // если если это тред тредпула
    // ну вот это кстати хуйня, с одного потока как раз таски должны пойти на разные для скорости обработки
    assert(node && node->task);
    // if destructor started, then it is undefined behavior to push tasks here
    // because its data race (between destruction and accessing to 'this' for calling 'execute')
    //
    // But there is special case - workers, which may invoke task, which .execute next task
    // in this case, workers when get a dead pills grab all operations from queue and cancel them
    // (handle.destroy) and thread_pool waits them in 'stop'
    //
    // So, no one pushes and all what was pushed by tasks executed on workers is now destroyed,
    // no memory leak, profit!

    worker_t& w = select_worker(hash);
    w.queue.push(node);
  }
  void execute(task_node* node) noexcept {
    return execute(node, calculate_operation_hash(node->task));
  }

  co_awaiter auto execute() noexcept {
    return create_task_node_and_execute<thread_pool>{*this};
  }

  void execute(auto&& foo, operation_hash_t hash) {
    [](thread_pool* pool, operation_hash_t hash, auto f) -> job {
      task_node node;
      co_await this_coro::suspend_and([&](std::coroutine_handle<> self_handle) {
        node.task = self_handle;
        pool->execute(&node, hash);
      });
      f();
    }(this, hash, std::forward<decltype(foo)>(foo));
  }
  void execute(auto&& foo) {
    return execute(std::forward<decltype(foo)>(foo), calculate_operation_hash(foo));
  }

  // TODO std::ranges::range auto workers() noexcept KELCORO_LIFETIMEBOUND {
  // TODO   return workers | std::views::transform(to strand);
  // TODO }

  // TODO? хм, может просто назвать это worker_ref?
  strand get_strand(operation_hash_t op_hash = rand()) KELCORO_LIFETIMEBOUND {
    // TODO srand(time(0)) ???
    // TODO как-то специально выбирать мб, чтобы равномерно хотя бы было. Ну или тредлокал переменная номерок
    return strand(select_worker(op_hash));
  }

 private:
  worker_t& select_worker(operation_hash_t op_hash) noexcept {
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
    for (worker_t& w : workers)
      w.queue.push(&pill);
    for (worker_t& w : workers) {
      KELCORO_ASSUME(w.thread.joinable());
      w.thread.join();
    }
  }
};

}  // namespace dd
