# KelCoro - C++20 coroutine library for C++

Module `kel.coro` includes:
* Coroutine types
  * [`generator<T>`](#generatort)
  * [`logical_thread`](#logical_thread)
  * [`job`](#job)
  * [`async_task<T>`](#async_taskt)
  * [`channel<T>`](#channelt)
  * [`task<T>`](#taskt)
* Functions or seems like functions
  * [`jump_on(Executor)`](#jump_on)
  * [`stop(Args...)`](#stop)
  * [`invoked_in(Foo)`](#invoked_in)
  * [`yield`](#yield)
* Event system
  * [`event_t`](#event_t)
  * [`when_all`](#when_all)
  * [`when_any`](#when_any)
* Concepts
  * [`co_awaiter`](#co_awaiter)
  * [`co_awaitable`](#co_awaitable)
  * [`executor`](#executor)

# Class Details
## `generator<T>`
* Calculates first value when.begin() / .next() called
* It is an input AND output range(co_yielded lvalues can be safely changed from consumer side), which means every value from generator will appear only once

Lifetime: frame dies with coroutine object (or special owner iterator if you call.begin() on rvalue generator)

example:
```C++
generator<int> Gen() {
  for (size_t i = 0;; ++i)
      co_yield i;
}

void GeneratorUser() {
  for (auto i : Gen())
      ;// do smth with i
}
```

## `logical_thread`
interface(same as jthread):
```C++
  bool joinable();
  void join(); // block until coroutine is done
  std::stop_source detach();
  handle_type native_handle(); // returns coroutine_handle
  bool request_stop();
  std::stop_token get_token();
  std::stop_source get_stop_source();
```
It is cancellable coroutine, which behavior similar to https://en.cppreference.com/w/cpp/thread/jthread (can be .request_stop(),
automatically requested for stop and joined in destructor or when move assigned etc)

Execution starts immediately when logical_thread created.
If unhandled exception happens in logical_thread std::terminate is called

Lifetime: If not .detach(), then coroutine frame dies with coroutine object, else (if.detach() called) frame dies after co_return (more precisely after final_suspend)

example :
```C++
kel::logical_thread Bar() {
// imagine that already C++46 and networking in the standard
  auto socket = co_await async_connect(endpoint);
  while (true) {
	auto write_info = co_await socket.async_write("Hello world");
	auto read_result = co_await socket.async_read();
	std::cout << read_result;
  }
}
```
## `job`
 behaves as always detached logical_thread, but more lightweight, has no methods, because always detached == caller have no guarantees about possibility to manipulate coro
 
Lifetime: same as detached kel::logical_thread
## `async_task<T>`
```C++
interface:
  void wait() const; // blocks until result is ready
  Result get(); // waits result and then returns it
```
If unhandled exception happens in async_task std::terminate is called
Execution starts immediately when async_task created.

Result can be ignored, it is safe.

example:
```C++
task<std::string> DoSmth() {
  co_await jump_on(another_thread);
  co_return "hello from task";
}

async_task<void> TasksUser() {
  std::vector<task<std::string>> vec;
  for (int i = 0; i < 10; ++i)
    vec.emplace_back(DoSmth());
  for (int i = 0; i < 8; ++i) {
    std::string result = co_await vec[i];
    verify(result == "hello from task");
  }
  co_return;
}
```
# Symmetric transfer between coroutines
What is symmetric transfer? this is when the coroutine does not just suspended, but transfers control to another coroutine. Then, in the future, 
the coroutine will be resumed. Thus, relative to the coroutine, this code looks absolutely synchronous, there is no synchronization and any overhead, 
but in fact, the coroutine to which control was transferred can be suspended and not return control. This allows implementing easy-to-use channels (see examples)

## `channel<T>`
interface:
only operator co_await() ! This means channel may be used only in another coroutine.
co_await channel returns pointer to value co_yielded from channel(nullptr == end), it is pointer to exactly value, so it is two-way channel(consumer can change value via pointer) 

example:
```C++
channel<int> CreateChannel() {
  for (int i = 0; i < 100; ++i) {
    co_await jump_on(another_thread);
    // any hard working for calculating i
    co_yield i; // control returns to caller ONLY after co_yield!
  }
}

async_task<void> ChannelUser() {
  auto my_channel = CreateChannel();
  int i = 0;
  // here we in cycle transfer control to channel,
  // then it calculates value and returns control to us
  while (auto* v = co_await my_channel) {
    verify(*v == i);
    ++i;
  }
}
```

## `task<T>`
interface:
only operator co_await ! This means task may be used only in another coroutine.
It is just a channel between two coroutines for exactly one value(returned by co_return)

task is lazy (starts only when co_awaited)

example: see async_task example =)

# Memory allocations for coroutines
Every coroutine supports allocators, (its last template argument of coroutine), by default it is default constructed, but you can use any memory resource with 
leading allocator convention or trailing allocator convention

example:
```C++
template <typename Resource>
task<int, Resource> SmartTask(std::allocator_arg_t, Resource /* i dont use it, coro frame uses it!*/)
// this task will use Resource for allocations! (it can be allocator, but also can be any with .allocate(size_t) / .deallocate(void*, size_t) methods)
```

# Usefull
## `jump_on`
co_await jump_on(Executor) equals to suspending coroutine and resume it on Executor(with .execute method), for example it can be thread pool
or kel::this_thread_executor(executes all on this thread) / kel::noop_executor etc
## `stop`
more effective way to stop(request_stop + join) for many stopable arguments or range of such type.
## `yield`
`co_yield X`; is equal to `co_await yield(X)`;

But in yield you can pass more then one argument to construct value in place
## `invoked_in`
okay its just magic for functions which want a callback
example:
```C++
// some function which accepts callback
bool Foo(int a, std::function<bool(int&, int)> f, int c) {
  int value = 10;
  // POINT 2: invoke a Foo from co_await call
  auto result = f(value, a + b + c);
  assert(value == 20);
  // POINT 5: after coroutine suspends we are here
  assert(result == true);
  return result;
}
  // usually it used for async callbacks, but it is possible to use for synchronous callbacks too
kel::job Bar() {
  // POINT 1: entering
  // value / summ is a references(YES even with auto) to arguments in Foo,
  // ret is a result of callback(not represented if signature<void(...)>)
  auto [value, summ, ret] = co_await this_coro::invoked_in(Foo, 10, signature<bool(int&, int)>{}, 20);
  assert(value == 10);
  value = 20;
  assert(summ == 45);
  // POINT 3: part of the coroutine(from co_await this_coro::invoked_in to next suspend) becomes callback for Foo
  ret = true; // setting return value, because Foo expects it from callback
  // POINT 4: first coroutine suspend after co_await
}
```
# Event system

## `event_t`
interface:
```C++
// Executor - who will execute tasks. input type by default *nothing*,
// but is can be customized with specialization event_traits<your_type>
  template <executor Executor>
  void notify_all(Executor&& exe, input_type input);

  // subscribe for not coroutines(allocator for task allocating)
  template <typename Alloc = std::allocator<std::byte>, typename F>
  void set_callback(F f, Alloc alloc = Alloc{});

  // subscribe, but only for coroutines
  auto operator co_await();
```
## `when_all`
```C++
template <typename... Events>
auto when_all() ->
*awaiter which will return control to coroutine and inputs for every event when all event happens*
```
## `when_any`
```C++
template <typename... Events>
auto when_any() ->
*awaiter which returns control to coroutine and variant with input(and info what happens) when any of Events happen*
```

example:
```C++
// just an event tags
struct one {};
struct two {};
struct three {};

async_task<void> waiter_all() {
  co_await jump_on(another_thread);
  for (int i : std::views::iota(0, 100)) {
    auto tuple = co_await when_all<one, two, three>(::test::test_selector{});
    std::cout << "All three events happen, i accept a tuple with all inputs!\n";
  }
}

template <typename Event>
logical_thread notifier(auto& pool, auto input) {
  co_await jump_on(another_thread);
  while (true) {
  // copy for all events, so no move
    pool.notify_all<Event>(input);
  // check if caller .request_stop()? then suspend
    co_await quit_if_requested;
  }
}

void WhenAllExample() {
  event_pool<this_thread_executor> pool;
  auto _1 = notifier<one>(pool);
  auto _2 = notifier<two>(pool, 5);
  auto _3 = notifier<three>(pool, std::vector<std::string>(3, "hello world"));
  auto taskl = waiter_all(flag);
  taskl.wait();
  stop(_1, _2, _3); // .request_stop() and .join() for all
  pool.notify_all<one>();
  pool.notify_all<two>(5);
  pool.notify_all<three>(std::vector<std::string>(3, "hello world"));
}
```
# Concepts
## `co_awaiter`
```C++
template <typename T>
concept co_awaiter = requires(T value) {
  { value.await_ready() } -> std::same_as<bool>;
  value.await_resume();
};
```
## `co_awaitable`
```C++
template <typename T>
concept co_awaitable = has_member_co_await<T> || has_global_co_await<T> || co_awaiter<T>;
```
## `executor`
something with .execute([]{}) method

