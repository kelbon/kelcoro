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

  // marker of closed stack, never a real node
  static inline node_type closed_marker{};

  std::atomic<node_type*> top = nullptr;

 public:
  nonowner_lockfree_stack() = default;
  constexpr explicit nonowner_lockfree_stack(bool closed) noexcept : top(closed ? &closed_marker : nullptr) {
  }

  // precondition: stack is not closed (use try_push for closable stack)
  void push(node_type* value_ptr) noexcept {
    // any other push works with another value_ptr
    assert(!!value_ptr);
    value_ptr->next = top.load(relaxed);
    // after this loop this->top == value_ptr, value_ptr->next == previous value of this->top
    while (!top.compare_exchange_weak(value_ptr->next, value_ptr, acq_rel, acquire)) {
    }
  }

  // returns top of the stack
  // precondition: stack is not closed
  [[nodiscard]] node_type* try_pop_all() noexcept {
    return top.exchange(nullptr, acq_rel);
  }

  // same as push, but does not push into closed stack, then returns false
  [[nodiscard]] bool try_push(node_type* value_ptr) noexcept {
    assert(!!value_ptr);
    value_ptr->next = top.load(acquire);
    do {
      if (value_ptr->next == &closed_marker)
        return false;
    } while (!top.compare_exchange_weak(value_ptr->next, value_ptr, acq_rel, acquire));
    return true;
  }

  // pops all nodes and closes the stack: try_push will not push anymore. Returns top of popped nodes
  [[nodiscard]] node_type* close() noexcept {
    node_type* t = top.exchange(&closed_marker, acq_rel);
    return t == &closed_marker ? nullptr : t;
  }

  [[nodiscard]] bool closed() const noexcept {
    return top.load(acquire) == &closed_marker;
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
