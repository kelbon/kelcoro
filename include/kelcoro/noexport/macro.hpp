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

#if !defined(NDEBUG)
#define KELCORO_ASSUME(expr) assert(expr)
#else
#define KELCORO_ASSUME(expr) \
  if (!(expr))               \
  KELCORO_UNREACHABLE
#endif

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
#else
#define KELCORO_NO_UNIQUE_ADDRESS
#endif
#else
#define KELCORO_NO_UNIQUE_ADDRESS
#endif
