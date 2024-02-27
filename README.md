# kelcoro - C++20 coroutines library

clang, gcc, msvc
[![](
https://github.com/kelbon/kelcoro/actions/workflows/build_and_test.yaml/badge.svg?branch=main)](
https://github.com/kelbon/kelcoro/actions/workflows/build_and_test.yaml)

* [`How to build?`](#build)
  
  * [`generator<T>`](#generatort)
  * [`channel<T>`](#channelt)
  * [`logical_thread`](#logical_thread)
  * [`job`](#job)
  * [`async_task<T>`](#async_taskt)
  * [`task<T>`](#taskt)

<details>
	<summary>How to customize allocations?</summary>
Library uses memory resources(see concept `dd::memory_resource`)

By default all coroutines deduct memory resource from arguments

All coroutines have *_r version, for example `generator_r`, such alias will use selected memory resource for allocations (if required),

for example:

```C++
generator<int> g();                                // will use operator new if required
generator<int> g(int, dd::with_resource<Resource>) // will use 'Resource' if allocation required
```
Also all coroutines have `dd::pmr::*` version which can use std::pmr::memory_resource using `dd::pass_resource` or default, if not passed (you can set default polymorphic memory resource globally)
```C++
std::pmr::memory_resource& get_default_resource() noexcept
std::pmr::memory_resource& set_default_resource(std::pmr::memory_resource& r) noexcept;
// next coroutine with 'dd::polymorphic_resource' on this thread will use 'm' for allocation
void pass_resource(std::pmr::memory_resource& m) noexcept;
```
</details>
	
* Functions or seems like functions
  * [`stop(Args...)`](#stop)
  * [`jump_on(Executor)`](#jump_on)

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
  // must be invoked in one thread(one consumer)
  std::add_rvalue_reference_t<Result> get() && noexcept;

  // returns true if call to 'get' will produce UB
  bool empty() const noexcept;
};
```
If unhandled exception happens in async_task std::terminate is called
Execution starts immediately when async_task created.

Result can be ignored, it is safe.

example:

```C++
async_task<int> future_int() {
  co_await dd::jump_on(my_executor);
  auto x = co_await foo();
  co_return bar(x);
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
    co_yield elements_of(std::vector{1, 2, 3, 4, 5}); // accepts range or other channel
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

example:

```C++
dd::task<std::string> foo() {
  co_await dd::jump_on(my_executor);
  std::string result = co_await something();
  co_return std::move(result);  // move here not useless!
}

dd::async_task<std::string> foo_user() {
  // task may be used only in other coroutine
  co_return co_await foo();
}

```

# Usefull

## `jump_on`
co_await jump_on(Executor) equals to suspending coroutine and resume it on Executor(with .attach(node)), for example it can be thread pool
or dd::this_thread_executor(executes all on this thread) / dd::noop_executor etc
## `stop`
more effective way to stop(request_stop + join) for many stopable arguments or range of such type.

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
<details>
  <summary>or use add_subdirectory</summary>


1. clone this repository into folder with your project
   `git clone https://github.com/kelbon/kelcoro`
3. add these lines to it's CMakeLists.txt

```
add_subdirectory(kelcoro)
target_link_libraries(MyTargetName PUBLIC kelcorolib)
```
</details>

Builds tests/examples

```
git clone https://github.com/kelbon/kelcoro
cd kelcoro
cmake . --preset debug_dev
// also you can use --preset default / debug_dev / release_dev / your own
cmake --build build
```

