#pragma once

#include <coroutine>
#include <thread>

#include "noexport/macro.hpp"

namespace dd {

enum struct schedule_errc : int {
  ok,
  cancelled,          // if task awaited with this code, it should not produce new tasks
  timed_out,          // not used now
  executor_overload,  // not used now
};

struct schedule_status {
  schedule_errc what = schedule_errc::ok;

  constexpr explicit operator bool() const noexcept {
    return what == schedule_errc::ok;
  }
};

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

struct any_executor_ref {
 private:
  // if nullptr, then thread pool )
  void (*attach_node)(void*, task_node*);
  void* data;

  template <typename T>
  static void do_attach(void* e, task_node* node) {
    T& exe = *static_cast<T*>(e);
    exe.attach(node);
  }

 public:
  template <executor E>
  constexpr any_executor_ref(E& exe KELCORO_LIFETIMEBOUND)
    requires(!std::same_as<std::remove_volatile_t<E>, any_executor_ref> && !std::is_const_v<E>)
      : attach_node(&do_attach<E>), data(std::addressof(exe)) {
  }

  any_executor_ref(const any_executor_ref&) = default;
  any_executor_ref(any_executor_ref&&) = default;

  void attach(task_node* node) {
    attach_node(data, node);
  }
};

// attaches all tasks from linked list starting from 'top'
void attach_list(executor auto& e, task_node* top) {
  while (top) {
    task_node* next = top->next;
    e.attach(top);
    top = next;
  }
}

// returns new begin
[[nodiscard]] constexpr task_node* reverse_list(task_node* top) noexcept {
  task_node* prev = nullptr;
  while (top) {
    task_node* next = std::exchange(top->next, prev);
    prev = std::exchange(top, next);
  }
  return prev;
}

template <typename E>
struct KELCORO_CO_AWAIT_REQUIRED create_node_and_attach : task_node {
  E& e;
  // creates task node and attaches it
  explicit create_node_and_attach(std::type_identity_t<E>& e) noexcept : e(e) {
  }

  static bool await_ready() noexcept {
    return false;
  }
  void await_suspend(std::coroutine_handle<> handle) noexcept {
    // set task before it is attached
    task = handle;
    e.attach(this);
  }
  [[nodiscard]] schedule_status await_resume() noexcept {
    return schedule_status{status};
  }
};

// ADL customization point, may be overloaded for your executor type, should return awaitable which
// schedules execution of coroutine to 'e'
template <executor E>
KELCORO_CO_AWAIT_REQUIRED constexpr auto jump_on(E&& e KELCORO_LIFETIMEBOUND) noexcept {
  return create_node_and_attach<E>(e);
}

struct noop_executor_t {
  static void attach(task_node*) noexcept {
  }
};

constexpr inline noop_executor_t noop_executor;

struct this_thread_executor_t {
  static void attach(task_node* node) {
    node->task.resume();
  }
};
constexpr inline this_thread_executor_t this_thread_executor;

struct new_thread_executor_t {
  static void attach(task_node* node) {
    std::thread([node]() mutable { node->task.resume(); }).detach();
  }
};

constexpr inline new_thread_executor_t new_thread_executor;

}  // namespace dd
