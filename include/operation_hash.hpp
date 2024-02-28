#pragma once

#include <coroutine>

#include "noexport/macro.hpp"

namespace dd {

// operation hash helps identify same logical operation
// from different tasks, its used to improve thread pool performance
using operation_hash_t = size_t;

namespace noexport {

// copy from one of stdlibcs, because hash for void* on linux gcc/clang really bad

#if UINTPTR_MAX > UINT_LEAST32_MAX
constexpr inline size_t fnv_offset_basis = 14695981039346656037ULL;
constexpr inline size_t fnv_prime = 1099511628211ULL;
#else  // 32 bit or smth like
constexpr inline size_t fnv_offset_basis = 2166136261U;
constexpr inline size_t fnv_prime = 16777619U;
#endif

static size_t do_hash(const void* ptr) noexcept {
  size_t val = fnv_offset_basis;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&ptr);
  for (int i = 0; i < sizeof(void*); ++i) {
    val ^= static_cast<size_t>(bytes[i]);
    val *= fnv_prime;
  }
  return val;
}

struct KELCORO_CO_AWAIT_REQUIRED op_hash_t {
  operation_hash_t hash;

  static constexpr bool await_ready() noexcept {
    return false;
  }
  template <typename P>
  constexpr bool await_suspend(std::coroutine_handle<P> handle) noexcept {
    hash = calculate_operation_hash(handle);
    return false;
  }
  constexpr operation_hash_t await_resume() noexcept {
    return hash;
  }
};

}  // namespace noexport

// precondition: opeation can be executed
// e.g. it is not empty coroutine handle or empty std::function
template <typename T>
struct operation_hash {
  static_assert(std::is_same_v<T, std::decay_t<T>>);
  // default version
  operation_hash_t operator()(const T& op) const noexcept {
    return noexport::do_hash(std::addressof(op));
  }
};

template <typename P>
struct operation_hash<std::coroutine_handle<P>> {
  operation_hash_t operator()(std::coroutine_handle<P> handle) const noexcept {
    // heuristic #1 - the same coroutine is executed on the same thread
    // and same coroutine produces max 1 task at one time
    return noexport::do_hash(handle.address());
  }
};

template <typename O>
[[gnu::pure]] constexpr operation_hash_t calculate_operation_hash(const O& operation) noexcept {
  return operation_hash<std::decay_t<O>>()(operation);
}

namespace this_coro {

constexpr inline noexport::op_hash_t operation_hash = {};

}

}  // namespace dd
