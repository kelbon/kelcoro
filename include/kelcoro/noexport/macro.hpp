#pragma once

#include <cassert>

#define KELCORO_CO_AWAIT_REQUIRED [[nodiscard("forget co_await?")]]

#if defined(__GNUC__) || defined(__clang__)
  #define KELCORO_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
  #define KELCORO_UNREACHABLE __assume(false)
#else
  #define KELCORO_UNREACHABLE assert(false)
#endif

#define KELCORO_ASSUME(expr) \
  if (!(expr))               \
  KELCORO_UNREACHABLE

// for some implementation reasons clang adds noinline on 'await_suspend'
// https://github.com/llvm/llvm-project/issues/64945
// As workaround to not affect performance I explicitly mark 'await_suspend' as always inline
// if no one can observe changes on coroutine frame after 'await_suspend' start until its end(including
// returning)
#ifdef __clang__
  #define KELCORO_ASSUME_NOONE_SEES [[gnu::always_inline]]
#else
  #define KELCORO_ASSUME_NOONE_SEES
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #define KELCORO_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif defined(__clang__)
  #define KELCORO_LIFETIMEBOUND [[clang::lifetimebound]]
#else
  #define KELCORO_LIFETIMEBOUND
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #define KELCORO_PURE
#else
  #define KELCORO_PURE [[gnu::pure]]
#endif

#ifdef _MSC_VER
  #define KELCORO_MSVC_EBO __declspec(empty_bases)
#else
  #define KELCORO_MSVC_EBO
#endif

#ifdef __has_cpp_attribute
  #if __has_cpp_attribute(no_unique_address)
    #define KELCORO_NO_UNIQUE_ADDRESS [[no_unique_address]]
  #elif __has_cpp_attribute(msvc::no_unique_address)
    #define KELCORO_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
  #else
    #define KELCORO_NO_UNIQUE_ADDRESS
  #endif
#else
  #define KELCORO_NO_UNIQUE_ADDRESS
#endif

// used in coroutine promise type to get default behavior
// for all types which not handled by await_transform
#define KELCORO_DEFAULT_AWAIT_TRANSFORM        \
  template <typename T>                        \
  static T&& await_transform(T&& v) noexcept { \
    return (T&&)(v);                           \
  }

#if __cpp_aggregate_paren_init < 201902L
  #define KELCORO_AGGREGATE_PAREN_INIT 0
#else
  #define KELCORO_AGGREGATE_PAREN_INIT 1
#endif
