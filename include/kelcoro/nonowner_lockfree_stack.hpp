#pragma once

#include <atomic>
#include <concepts>
#include <cassert>

namespace dd {

template <typename T>
concept singly_linked_node = !std::is_const_v<T> && requires(T value) {
  { &T::next } -> std::same_as<T * T::*>;  // 'next' is a field with type T*
};

template <singly_linked_node Node>
struct nonowner_lockfree_stack {
  using node_type = Node;

 protected:
  using enum std::memory_order;

  std::atomic<node_type*> top = nullptr;

 public:
  void push(node_type* value_ptr) noexcept {
    // any other push works with another value_ptr
    assert(!!value_ptr);
    value_ptr->next = top.load(relaxed);
    // after this loop this->top == value_ptr, value_ptr->next == previous value of this->top
    while (!top.compare_exchange_weak(value_ptr->next, value_ptr, acq_rel, acquire)) {
    }
  }

  // returns top of the stack
  [[nodiscard]] node_type* try_pop_all() noexcept {
    return top.exchange(nullptr, acq_rel);
  }
  // other_top must be a top of other stack ( for example from try_pop_all() )
  void push_stack(node_type* other_top) noexcept {
    if (!other_top)
      return;
    auto* last = other_top;
    while (last->next)
      last = last->next;
    last->next = top.load(relaxed);
    while (!top.compare_exchange_weak(last->next, other_top, acq_rel, acquire)) {
    }
  }
};

}  // namespace dd
