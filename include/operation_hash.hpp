#pragma once

#include <functional>
#include <coroutine>

namespace dd {

// operation hash helps identify same logical operation
// from different tasks, its used to improve thread pool performance
using operation_hash_t = size_t;

// precondition: opeation can be executed
// e.g. it is not empty coroutine handle or empty std::function
template <typename T>
struct operation_hash {
  static_assert(std::is_same_v<T, std::decay_t<T>>);
  // default version
  operation_hash_t operator()(const T& op) const noexcept {
    return std::hash<const void*>()(std::addressof(op));
  }
};

template <typename P>
struct operation_hash<std::coroutine_handle<P>> {
  operation_hash_t operator()(std::coroutine_handle<P> handle) const noexcept {
    // heuristic #1 - the same coroutine is executed on the same thread
    // and same coroutine produces max 1 task at one time
    return std::hash<const void*>()(handle.address());
  }
};

template <typename O>
operation_hash_t calculate_operation_hash(const O& operation) noexcept {
  return operation_hash<std::decay_t<O>>()(operation);
}

}  // namespace dd
