# KelCoro - C++20 coroutine library for C++
* [`How to build?`](#build)

Library consists of
* small common part, usefull for all coroutines, including your own created
  * tags elements_of and by_ref for generator and channel
  * with_resource - easy to use allocation customization
  * enable_memory_resource_support - for your own coroutines
  * [`jump_on(Executor)`](#jump_on)
  * always_done_coroutine
  * etc... see all in 'include/common.hpp'
* coroutines
  * [`generator<T>`](#generatort)
  * [`channel<T>`](#channelt)
  * [`logical_thread`](#logical_thread)
  * [`job`](#job)
  * [`async_task<T>`](#async_taskt)
  * [`task<T>`](#taskt)
* Functions or seems like functions

  * [`stop(Args...)`](#stop)
* Event system
  * [`event`](#eventt)
  * [`when_all`](#when_all)
  * [`when_any`](#when_any)
* Concepts
  * [`co_awaiter`](#co_awaiter)
  * [`co_awaitable`](#co_awaitable)
  * [`executor`](#executor)

# Primitives:

## `generator<T>`
interface:
```C++
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
```
   * produces next/first value when .begin called
   * recursive (see co_yield dd::elements_of(rng))
 * default constructed generator is an empty range

   
 notes:
  * operator* of iterator returns rvalue reference to value, but if you yield 'by_ref(v)' you can change value and generator will observe changes
  * generator ignores fact, that 'destroy' may throw exception from destructor of object in coroutine, it
  will lead to std::terminate
  * if exception was throwed from recursivelly co_yielded generator, then this leaf just skipped and caller
  can continue iterating after catch(requires new .begin call)

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
## `job`
 behaves as always detached logical_thread, but more lightweight, has no methods, because always detached == caller have no guarantees about possibility to manipulate coro
 
Lifetime: same as detached dd::logical_thread
## `async_task<T>`
```C++
interface:
  void wait() const; // blocks until result is ready
  Result get() &&; // waits result and then returns it
  bool empty() const noexcept;
```
If unhandled exception happens in async_task std::terminate is called
Execution starts immediately when async_task created.

Result can be ignored, it is safe.

example:
```C++
task<std::string> DoSmth() {
  co_await jump_on(dd::new_thread_executor{});
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
Literaly same as dd::generator. But 'begin' and operator++ on iterator requires co_await.
So, there are macro co_foreach for easy usage.

example:
```C++
channel<int> ints_creator() {
  for (int i = 0; i < 100; ++i) {
    co_await jump_on(some_executor);
    // any hard working for calculating i
    for (int i = 0; i < 10; ++i)
      co_yield i; // control returns to caller ONLY after co_yield!
    co_yield elements_of(std::vector{1, 2, 3, 4, 5});
  }
}

async_task<void> user() {
  co_foreach(int i, ints_creator())
    use(i);
  }
// if you want to not use macro, then:
// auto c = ints_creator();
// for (auto b = co_await c.begin(); b != c.end(); co_await ++b)
//     use(*b);
}
```

## `task<T>`
interface:
only operator co_await ! This means task may be used only in another coroutine.
It is just a channel between two coroutines for exactly one value(returned by co_return)

task is lazy (starts only when co_awaited)

example: see async_task example =)

# Memory allocations for coroutines
Every coroutine supports allocation customization, just use 'with_resource'

example:
```C++
  my_resource r; // inheritor of std::pmr::memory_resource
  dd::with_resource _(r); // every coroutine created on this thread until '_' dies will use 'r' as memory resource
  foo();
```

# Usefull
## `jump_on`
co_await jump_on(Executor) equals to suspending coroutine and resume it on Executor(with .execute method), for example it can be thread pool
or dd::this_thread_executor(executes all on this thread) / dd::noop_executor etc
## `stop`
more effective way to stop(request_stop + join) for many stopable arguments or range of such type.

# Event system

## `event<T>`
interface:
```C++
  template <executor Executor>
  bool notify_all(Executor&& exe, input_type input);

  // subscribe for not coroutines(allocator for task allocating)
  template <typename Alloc = std::allocator<std::byte>, typename F>
  void set_callback(F f, Alloc alloc = Alloc{});

  // subscribe, but only for coroutines
  auto operator co_await();
```
example:

```C++
inline dd::event<int> e1 = {};

dd::job waiter() {
  while (true) {
    int i = co_await e1;
    foo(i);
  }
}
dd::logical_thread notifier() {
  co_await dd::jump_on(dd::new_thread_executor{});
  dd::stop_token tok = co_await dd::this_coro::stop_token;
  while(true) {
    e1.notify_all(dd::this_thread_executor{}, 1);
    if (tok.stop_requested())
      co_return;
  }
}
```

## `when_all`
```C++
template <typename... Inputs>
co_awaitable auto when_all(event<Inputs>&... events);
```
## `when_any`
```C++
template <typename... Inputs>
co_awaitable auto when_any(event<Inputs>&... events);
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

## `build`
```CMake

include(FetchContent)
FetchContent_Declare(
  kelcoro
  GIT_REPOSITORY https://github.com/kelbon/kelcoro
  GIT_TAG        origin/main
)
FetchContent_MakeAvailable(kelcoro)
target_link_libraries(MyTargetName kelcorolib)

```
or use add_subdirectory

1. Clone this repository into folder with your project 2. Add these lines to it's CMakeLists.txt
```
add_subdirectory(AnyAny)
target_link_libraries(MyTargetName PUBLIC anyanylib)
```

Builds tests/examples

```
git clone https://github.com/kelbon/kelcoro
cd kelcoro
cmake . --preset debug_dev
// also you can use --preset default / debug_dev / release_dev / your own
cmake --build build
```

