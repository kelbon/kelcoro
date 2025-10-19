# kelcoro - C++20 coroutines library

clang, gcc, msvc
[![](
https://github.com/kelbon/kelcoro/actions/workflows/build_and_test.yaml/badge.svg?branch=main)](
https://github.com/kelbon/kelcoro/actions/workflows/build_and_test.yaml)

* [`build with cmake`](#build)

coroutine types:
  * [`task<T>`](#taskt)
  * [`generator<T>`](#generatort)
  * [`channel<T>`](#channelt)
  * [`job`](#job)
  * [`async_task<T>`](#async_taskt)
  * [`logical_thread`](#logical_thread)

coroutine synchronization primitives:

* [`road`](#road)
* [`gate`](#gate)
* [`latch`](#latch)

algorithms:

* [`when_all`](#when_all)
* [`when_any`](#when_any)
* [`with`](#with)
* [`chain`](#chain)

executors:

* [`any_executor_ref`](#any_executor_ref)
* [`jump_on`](#jump_on)
* [`task_queue`](#task_queue)
* [`worker`](#worker)
* [`strand`](#strand)
* [`thread_pool`](#thread_pool)
* [`noop_executor`](#noop_executor)
* [`this_thread_executor`](#this_thread_executor)
* [`new_thread_executor`](#new_thread_executor)

customizing memory allocation:

* [`with_resource`](#with_resource)
* [`chunk_from`](#chunk_from)
* [`stack_resource`](#stack_resource)
* [`new_delete_resource`](#new_delete_resource)
* [`polymorphic_resource`](#polymorphic_resource)

# coroutine types

## `task<T>`

`task` is a lazy function. Returns one value (or rethrows exception). Lazy == starts when co_await or when start_and_detach() called.

interface:
```cpp
// movable, default constructible type
// Result - what will be returned from coroutine
// Ctx - for advanced usage and customization, see below
template <typename Result, typename Ctx = null_context>
struct task {
  // precondition: !empty()
  // co_await returns Result&& or rethrows exception
  auto operator co_await() &;

  // precondition: !empty()
  // co_await returns Result or rethrows exception
  auto operator co_await() &&;

  // precondition: !empty()
  // co_awaits until task is done, but dont gets value / rethrows exception
  auto wait() noexcept;

  bool empty() const noexcept;
  explicit operator bool() const noexcept { return !empty(); }

  // precondition: empty() || not started yet
  // postcondition: empty(), task result ignored (exception too)
  // returns released task handle
  // if 'stop_at_end' is false, then task will delete itself at end,
  // otherwise handle.destroy() should be called
  handle_type start_and_detach(bool stop_at_end = false);

  // precondition: not started yet
  // postcondiion: empty()
  // example: thread_pool.schedule(mytask().detach())
  handle_type detach() noexcept;

  Ctx* get_context() const noexcept;
};
```

<details>
	<summary>about task contexts (advanced usage) </summary>


```cpp
//  Ctx may be used as second `task` argument for customization
// e.g. may be used to make a custom traces and `call graphs` of coroutines
// or customizing how unhandled exception handled
struct null_context {
  // invoked when task is scheduled to execute, 'owner' is handle of coroutine, which awaits task
  // never invoked with nullptrs
  // note: one task may have only one owner, but one owner can have many child tasks (when_all/when_any)
  template <typename OwnerPromise, typename P>
  static void on_owner_setted(std::coroutine_handle<OwnerPromise>, std::coroutine_handle<P>) noexcept {}
  // may be invoked even without 'on_owner_setted' before, if task is detached
  template <typename P>
  static void on_start(std::coroutine_handle<P>) noexcept {}
  // invoked when task ended with/without exception
  template <typename P>
  static void on_end(std::coroutine_handle<P>) noexcept {}

  // invoked when no one waits for task and exception thrown
  // precondition: std::current_exception() != nullptr
  // Note: if exception thrown memory leak possible (no one will destroy handle)
  // this function MUST NOT destroy handle (UB) and handle must not be destroyed while exception alive
  // Note: passed coroutine handle will became invalid after this call
  template <typename P>
  static void on_ignored_exception(std::coroutine_handle<P>) noexcept {}
};

```

</details>

example with creating and using task:

```C++
dd::task<load_response> load_from_database(database_ptr client, user_id id) {
  request req = make_load_req(id);
  response rsp = co_await client->send_request(req);
  co_return parse_load_rsp(rsp);
}

dd::task<bool> some_foo() {
..
  load_response rsp = co_await send_into_database(client, info);
  co_return rsp.ok().
}

// or same with `wait`
dd::task<bool> some_foo() {
..
  dd::task t = send_into_database(client, info);
  co_await t.wait();
  // all promise not special methods are public interface too
  load_response rsp = t.raw_handle().promise().result_or_rethrow();
  co_return rsp.ok().
}

```

## `generator<T>`
interface:
```C++
template <yieldable Yield>
struct generator {
  // * if .empty(), then begin() == end()
  // * produces next value(often first)
  // iterator invalidated only when generator dies
  iterator begin() & [[clang::lifetimebound]];
  static std::default_sentinel_t end() noexcept;

  // postcondition: empty(), 'for' loop produces 0 values
  constexpr generator() noexcept = default;

  // postconditions:
  // * other.empty()
  // * iterators to 'other' == end()
  constexpr generator(generator&& other) noexcept;
  constexpr generator& operator=(generator&& other) noexcept;

  bool operator==(const generator& other) const noexcept;

  bool empty() const noexcept;
  explicit operator bool() const noexcept {
    return !empty();
  }

  void reset(handle_type handle) noexcept;
  // postcondition: .empty()
  void clear() noexcept;
  // postcondition: .empty()
  // its caller responsibility to correctly destroy handle
  handle_type release() noexcept;

};
```
   * produces next/first value when .begin called
   * recursive (see co_yield dd::elements_of(rng))
 * default constructed generator is an empty range

   
 notes:
  * tag `dd::elements_of(RNG)` accepts range(including other generator) and yields all elements of it more effectively, then for-loop
  * tag `by_ref` allows to yield lvalue referencnes, so caller may get reference from iterator's operator* and change value, so generator will observe changes.
This allows usage generator as in-place output range, which may be very cose


usage as output iterator example:
```C++
dd::generator<int> printer() {
  int i;
  while (true) {
    co_yield dd::by_ref{i};
    print(i);
  }
}

void print_foo(std::vector<int> values) {
  // prints all values
  std::copy(begin(values), end(values), printer().begin().out());
}
```
  * operator* of iterator returns rvalue reference to value, so `for(std::string s : generator())` is effective code(one move from generator)

example:
```C++
dd::generator<int> ints() {
  for (int i = 0; i < 100; ++i)
      co_yield i;
}
dd::generator<int> intsints() {
  co_yield dd::elements_of(ints());
  co_yield dd::elements_of(ints());
}
void use() {
  for (int i : intsints())
      do_smth(i);
}
```

## `channel<T>`
interface:
Literaly same as dd::generator. But 'begin' and operator++ on iterator requires co_await.
So, there are macro co_foreach for easy usage.

example:
```C++
dd::channel<int> ints() {
	co_yield 1;
	co_yield 2;
}
dd::channel<int> ints_creator() {
  for (int i = 0; i < 100; ++i) {
    int value = co_await foo(); // any hard working for calculating `value`
    co_yield value; 					// control returns to caller ONLY after co_yield!
    co_yield dd::elements_of(ints()); 	// elements_of works too
  }
}

dd::task<void> user() {
  co_foreach(int i, ints_creator())
    use(i);
}
// if you want to not use macro, then:
// auto c = ints_creator();
// for (auto b = co_await c.begin(); b != c.end(); co_await ++b)
//     use(*b);

```

## `job`
 coroutine for detached work, starts immediately (not lazy), similar to task\<void\> with always start_and_detach(). Calls std::terminate on unhandled exceptions
 
 mostly used for low-level implementations

example:

```cpp
dd::job periodic_task() {
    mytimer t;
    for (;;) {
	  errc e = co_await t.sleep(5s);
	  if (e == canceled)
        co_return;
	  do_foo();
	}
}

```

## `async_task<T>`

`async_task` is similar to std::future, produces value or rethrows exception, allows to blocking wait it even if co_return called in other thread

Execution starts immediately when async_task created.

Result can be ignored, it is safe.

Mostly used when its required to blocking wait a `task` on other thread

interface:

```C++
template <typename Result>
struct async_task {
  async_task() noexcept = default;
  async_task(async_task&& other) noexcept;

  // postcondition: if !empty(), then coroutine suspended and value produced
  void wait() const noexcept;

  // returns true if 'get' is callable and will return immedially without wait
  bool ready() const noexcept;

  // postcondition: empty()
  void detach() noexcept;

  // precondition: !empty()
  // may be invoked in different thread, but only in one
  std::add_rvalue_reference_t<Result> get() &;

  // precondition: !empty()
  // may be invoked in different thread, but only in one
  Result get() &&;

  bool empty() const noexcept;
};
```

example:

```C++
dd::async_task<int> future_int() {
  co_await dd::jump_on(my_executor);
  auto x = co_await foo();
  co_return bar(x);
}
..
void foo() {
 int x = future_int().get(); // blocking wait
}
```

## `logical_thread`
interface(similar to std::jthread):
```C++
struct logical_thread {
  logical_thread() noexcept = default;
  logical_thread(logical_thread&&) noexcept;
  bool joinable();
  void join(); // block until coroutine is done
  void detach();
  bool stop_possible() const noexcept;
  bool request_stop();
  // co_await dd::this_coro::stop_token will return dd::stop_token for coroutine
};
```
It is cancellable coroutine, which behavior similar to https://en.cppreference.com/w/cpp/thread/jthread (can be .request_stop(),
automatically requested for stop and joined in destructor or when move assigned etc)

Execution starts immediately when logical_thread created.
If unhandled exception happens in logical_thread std::terminate is called

example:
```C++
dd::logical_thread Bar() {
  // imagine that already C++47 and networking in the standard
  auto socket = co_await async_connect(endpoint);
  auto token = co_await dd::this_coro::stop_token; // cancellable coroutine can get stop token associated with it
  while (!token.stop_requested()) {
	auto write_info = co_await socket.async_write("Hello world");
	auto read_result = co_await socket.async_read();
	std::cout << read_result;
  }
}
```

# coroutine synchronization primitives

## `road`

interface:
`road` may be closed or opened. if closed, coroutines may wait until its open. Must be used in one thread

```cpp
struct road {
  // precondition: not closed (use wait_open in loop...)
  void close() noexcept;

  [[nodiscard]] bool closed() const noexcept;

  // notifies all waiters, that gateway is opened
  void open(executor auto& e);

  // co_await returns `bool` == !this->closed()
  // Note: resuming waiters in `open` may lead to .close again
  // this means `wait_open` often need to use in loop.
  // Example:
  //  while (!co_await r.wait_open())
  //    [[unlikely]];
  open_awaiter wait_open() noexcept;
};
```

example:

```cpp
// blocks all writes for 5s
dd::task<void> close_for_5s(road& write_road) {
  // avoid other parallely working close_for_5s, even in single thread!
  while (!co_await r.wait_open())
	[[unlikely]];
  write_road.close();
  co_await my_sleep_for(5s);
  write_road.open(dd::this_thread_executor);
}
..
dd::task<void> writer(road& write_road) {
  // loop for avoiding use closed road if `open` in close_for_5s resumes someone who close road again
  while (!co_await r.wait_open())
     [[unlikely]];
  do_write();
}
```

## `gate`

`gate` tracks the number of running operations and may be used to forbid new operations and await until all operations done. Must be used in one thread

interface:

```cpp
// throwed in `enter` called on closed `gate`
struct gate_closed_exception : std::exception {};

// Note: all methods should be called from one thread
struct gate {
  // if gate is closed, returns false.
  // Otherwise returns true and caller must call 'leave' in future
  bool try_enter() noexcept;

  // throws `gate_closed_exception` if closed
  void enter();

  // must be invoked only after invoking 'try_enter'
  // Note: may resume close awaiters.
  // Sometimes it makes sense to push them into task list instead of executing now
  // e.g. someone who call `leave` uses resources, which they will destroy
  void leave() noexcept;

  struct holder;
  // combines enter + leave on scope exit
  holder hold();

  // now many successfull 'try_enter' calls not finished by 'leave' now
  size_t active_count() const noexcept;

  // postcondition (after co_await): is_closed() == true && `active_count()` == 0
  // Note: is_closed() true even before co_await
  // Note: several coroutines may wait `close`. All close awaiters will be resumed.
  // reopen() will allow calling 'close' again
  gate_close_awaiter close() noexcept;

  bool is_closed() const noexcept;

  // after `reopen` gate may be `entered` or `closed` again
  // precondition: .active_count() == 0 && no one waits for `close`
  void reopen() noexcept;
};

```

## `latch`
same as [std::latch](https://en.cppreference.com/w/cpp/thread/latch.html), but for coroutines

interface:

```cpp
// non movable
struct latch {
  // precondition: count >= 0 and <= max,
  // `e` used when counter reaches 0 for resuming all who waits
  constexpr explicit latch(ptrdiff_t count, any_executor_ref e) noexcept;

  static constexpr ptrdiff_t max() noexcept;

  // decrements the internal counter by n without blocking the caller
  // precondition: n >= 0 && n <= internal counter
  void count_down(std::ptrdiff_t n = 1) noexcept;

  // returns true if the internal counter has reached zero
  // never blocks, 'try_wait' in std::latch
  bool ready() const noexcept;

  // suspends the calling coroutine until the internal counter reaches ​0​.
  // If it is zero already, returns immediately
  co_awaiter auto wait() noexcept;

  // precondition: n >= 0 && n <= internal counter
  // logical equivalent to count_down(n); wait() (but atomicaly, really count down + wait is rata race)
  co_awaiter auto arrive_and_wait(std::ptrdiff_t n = 1) noexcept;

  any_executor_ref get_executor() const noexcept;
};
```

# algorithms

## `when_all`

Waits until all tasks are done

interface:

```cpp
template <typename... Ts, typename Ctx>
auto when_all(task<Ts, Ctx>... tasks) -> task<std::tuple<expected<Ts, std::exception_ptr>...>, Ctx>;

template <typename T, typename Ctx>
auto when_all(std::vector<task<T, Ctx>> tasks) -> task<std::vector<expected<T, std::exception_ptr>>, Ctx>;

```

## `when_any`

Waits and returns value and index of first not failed task. If all tasks failed (exception), last exception returned.

Note: all other tasks do not canceled after first is done. Its caller responsibility to not resources with such tasks

interface:

```cpp

// precondition: all tasks not .empty()
// returns first not failed or last exception if all failed
template <typename... Ts, typename... Ctx>
auto when_any(task<Ts, Ctx>... tasks) -> task<std::variant<expected<Ts, std::exception_ptr>...>, first_type_t<Ctx...>>;

template <typename T>
struct when_any_dyn_result {
  expected<T, std::exception_ptr> value;
  size_t index() const noexcept;
};
// precondition: all tasks not .empty()
// returns value and index of first not failed or last exception if all failed
template <typename T, typename Ctx>
auto when_any(std::vector<task<T, Ctx>> tasks) -> task<when_any_dyn_result<T>, Ctx>;
```

example:

```cpp

dd::task<void> foo1();

dd::task<int> foo2();

dd::task<int> bar() {
   std::variant r = co_await dd::when_any(foo1(), foo2());
   switch (r.index()) {
		case 0:
		// foo1() done first
		break;
		case 1:
		// foo2 done first
		break;
   }
}

```

## `with`

`with` binds arguments to a task so that they will be destroyed after the task is executed

interface:

```cpp
// precondition: !t.empty()
template <typename T, typename Ctx>
task<T, Ctx> with(task<T, Ctx> t, auto...) { co_return co_await t; }
```

example:

```cpp
dd::task<void> foo(data_t*);

..
std::shared_ptr data = std::make_shared<data_t>();
dd::with(foo(data.get()), data).start_and_detach();
```

## `chain`
`chain` is a `then` in more useful interface, co_awaits awaitables one by one in a sequence passing result of previous into next (if next is function), stops on first exception


Note: if value both co_awaitable and invocable - compilation error (ambigious call)

interface:

```cpp
// set of overloads, but for understanding this interface present
// starts always with co_awaitable, then continues with co_awaitable
// OR function, which accepts result of previous co_await and returns co_awaitable
template <typename Ctx = null_context, co_awaitable A, typename Head, typename... Tail>
auto chain(A t, Head foo, Tail... tail) -> task<...> (type deducted)

// helper, ignores result of previous co_await in `chain`
constexpr inline ignore_result_t ignore_result = {};

```

example:

```cpp
dd::task<int> foo(std::string);
dd::task<std::string> bar();

..
int x = co_await dd::chain(bar(), [] (std::string s) { return foo(std::move(s)); });
..
```

example with `ignore_result`:

```cpp

dd::task<response> handle_request(request);
dd::task<bool> validate_rsp(response);

dd::task<void> do_complex_handling(request r) {
  // function guarantees, that validator will be created only after `handle_request`, not now
  auto make_validator = [](response r) { return validate_rsp(std::move(r)); };
  auto assert_schedule_status = [](dd::schedule_status s) {
    assert(!s);
    return std::suspend_never{};
  };
  return dd::chain(dd::jump_on(executor), assert_schedule_status, handle_request(std::move(r)), make_validator,
                   dd::ignore_result);
}

```

# executors

kelcoro defines concept `executor` and struct `task_node`.

Basically `executor` is any type, which supports .attach(task_node*).

This interface greatly simplifies implementation and improves performance, removing allocations and type erasure

<details>
<summary> executor and task node definitions </summary>

```cpp

// task node is a intrusive node for implementation of executors, it may be used in user defined awaiters / executors
struct task_node {
  task_node* next = nullptr;
  std::coroutine_handle<> task = nullptr;
  schedule_errc status = schedule_errc::ok;
};

template <typename T>
concept executor = requires(T& exe, task_node* node) {
  // preconditions for .attach:
  // node && node->task
  // node live until node->task.resume() and will be invalidated immediately after that
  exe.attach(node);
  // effect: schedules node->task to be executed on 'exe'
};

```
</details>

## `jump_on`

```cpp
// ADL customization point, works for all kelcoro executors
// may be overloaded for your executor type, should return awaitable which
// schedules execution of coroutine to 'e'
template <executor E>
auto jump_on(E&& e) noexcept;

// specialization for dd::thread_pool
// Note: selects thread where task will be executed based on operation hash.
// this guarantees, that if same coroutine jumps on same thread pool it will always run on same thread
auto jump_on(thread_pool& tp) noexcept;

```

## `any_executor_ref`

type-erased reference to executor. Also satisfies concept executor

interface:

```cpp
struct any_executor_ref {
  template <executor E>
  constexpr any_executor_ref(E& exe KELCORO_LIFETIMEBOUND)
    requires(!std::same_as<std::remove_volatile_t<E>, any_executor_ref> && !std::is_const_v<E>);

  any_executor_ref(const any_executor_ref&) = default;
  any_executor_ref(any_executor_ref&&) = default;

  // executor interface
  void attach(task_node* node);
};
```

## `task_queue`

thread-safe queue for tasks. Note, that it is also satisfies `executor` concept, but not executes tasks (someone can execute them later)

interface:

```cpp

struct task_queue {
  // precondition: node != nullptr && node is not contained in queue
  void push(task_node* node);

  // attach a whole linked list
  void push_list(task_node* first, task_node* last);
  // finds last
  void push_list(task_node* first);

  task_node* pop_all();

  // blocking, waits until atleast one task pushed
  // postcondition: task_node != nullptr
  task_node* pop_all_not_empty();

  // executor interface
  void attach(task_node* node) noexcept;
};
```

## `worker`
executor, basicaly `task_node` + `std::thread`

## `strand`
executor, basically just reference to `task_node` of concrete `worker`.

Guarantees, that all pushed tasks will be executed on one thread in push order

interface:

```cpp
struct strand {
  explicit strand(worker&);
  explicit strand(task_queue&);
  task_queue& get_queue() const noexcept;

  // executor interface
  void attach(task_node* node) const noexcept;
};
```

## `thread_pool`

executor, not stealing, manages many `worker`s

interface:

```cpp
// distributes tasks among workers
// co_await jump_on(pool) schedules coroutine to thread pool
// note: when thread pool dies, all pending tasks invoked with schedule_errc::cancelled
struct thread_pool {
  static size_t default_thread_count();

  explicit thread_pool(size_t thread_count = default_thread_count(), worker::job_t job = default_worker_job,
                       std::pmr::memory_resource& r = *std::pmr::get_default_resource());

  thread_pool(thread_pool&&) = delete;
  void operator=(thread_pool&&) = delete;

  // executor interface. used by dd::jump_on(pool) internally
  void attach(task_node* node) noexcept;
  
  bool is_worker(std::thread::id id = std::this_thread::get_id());

  // same as dd::schedule_to(pool), but uses pool memory resource to allocate tasks
  void schedule(std::invocable auto&& foo, operation_hash_t hash);
  void schedule(std::invocable auto&& foo);

  std::span<std::thread> workers_range() noexcept;
  std::span<const std::thread> workers_range() const noexcept;

  std::span<task_queue> queues_range() noexcept;
  std::span<const task_queue> queues_range() const noexcept;

  // see `strand` doc
  strand get_strand(operation_hash_t op_hash);
  task_queue& select_queue(operation_hash_t op_hash) noexcept;

  // can be called as many times as you want from any threads
  void request_stop();

  // NOTE: can't be called from workers
  // NOTE: can't be called more than once
  // Wait for the job to complete (after calling `request_stop`)
  void wait_stop() &&;
};

```

## `noop_executor`
executor, does nothing for each .attach(task_node*)

## `this_thread_executor`
executor. .attach(task_node*) invokes task in this thread

## `new_thread_executor`
executor. each .attach(task_node*) creates new std::thread which will execute task. Mostly used for tests etc


# customizing memory allocation

kelcoro uses `memory_resource` concept instead of allocators for coroutines. Requirements for memory resources less than for allocators

<details><summary>concept memory resource</summary>

```cpp
template <typename T>
concept memory_resource = !std::is_reference_v<T> && requires(T value, size_t sz, void* ptr) {
  // align of result must be atleast aligned as dd::coroframe_align()
  // align arg do not required, because standard do not provide interface
  // for passing alignment to promise_type::new
  { value.allocate(sz) } -> std::convertible_to<void*>;
  // if type is trivially destructible it must handle case when `value` lays in memory under `ptr`
  { value.deallocate(ptr, sz) } -> std::same_as<void>;
  requires std::is_nothrow_move_constructible_v<T>;
  requires alignof(T) <= alignof(std::max_align_t);
  requires !(std::is_empty_v<T> && !std::default_initializable<T>);
};

```

</details>


## `with_resource`

All kelcoro coroutines detect memory resource from tag `with_resource` passed as last argument into coroutine.

If you want to use another resource always without changing signature of function, use *_r versions of coroutines, e.g. dd::task_r\<T, YourResource\>. In this case if YourResource is default constructible it will be used when coroutine frame allocated.

mostly used as tag in combination with `chunk_from`

## `chunk_from`
reference to resource for cozy usage, mostly used in combination with `with_resource`

example:

```cpp

using with_my_resource = dd::with_resource<dd::chunk_from<MyResource>>;

dd::task<T> foo(int arg, with_my_resource = get_my_resource());

dd::generator<int> gen(dd::with_stack_resource r) {
  co_yield 5;
}

void bar() {
  dd::stack_resource r;
  // `gen` will use `r` for allocation
  for (int x : gen(r))
    use(x);
}

```

## `stack_resource`
memory resource, implements the most efficient allocation strategy possible for typical case when coroutine waits until another coroutine is done.
.deallocate(ptr) assumes, that `ptr` is last allocated pointer, like on real stack - only last chunk may be deallocated.
This makes possible to effectively allocate memory and reuse it


interface:

```cpp


/*
 assumes, that only last allocated chunk may be deallocated (like real stack)
 its common case for coroutines, but this resource must be used carefully
 to preserve order of deallocations
 all allocated pointers aligned to coroframe_align()
*/
struct stack_resource {
  explicit stack_resource(std::span<byte_t> bytes = {});

  stack_resource(stack_resource&& other) noexcept;
  stack_resource& operator=(stack_resource&& other) noexcept;

  void* allocate(size_t len);

  void deallocate(void* ptr, size_t len) noexcept;

  // works as 'deallocate(ptr, size)' for all allocated chunks
  void reuse_memory() noexcept;
};

```

example:

```cpp
    dd::task<int> foo(dd::with_stack_resource r) {
      co_await bar(r); // pass and wait to preseve allocations order
    }
```

## `new_delete_resource`
default memory resource, uses standard new / delete for allocations

## `polymorphic_resource`
`dd::pmr::polymorphic_resource` is a small wrapper over `std::memory_resource&`, which adapts it for using with coroutines

# `build`

kelcoro has no external dependencies and can be built with CMake easily:

Prefered way with [CPM](https://github.com/cpm-cmake/CPM.cmake):

```CMake
CPMAddPackage(
  NAME KELCORO
  GIT_REPOSITORY https://github.com/kelbon/kelcoro
  GIT_TAG v1.4.1
)
target_link_libraries(MyTargetName kelcorolib)
```

<details>
  <summary>or just fetch content</summary>

```CMake

include(FetchContent)
FetchContent_Declare(
  kelcoro
  GIT_REPOSITORY https://github.com/kelbon/kelcoro
  GIT_TAG        v1.4.1
)
FetchContent_MakeAvailable(kelcoro)
target_link_libraries(MyTargetName kelcorolib)

```

</details>
