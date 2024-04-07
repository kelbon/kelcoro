#pragma once

#include <memory_resource>
#include <cassert>

#include "noexport/macro.hpp"
#include "operation_hash.hpp"

namespace dd {

template <typename T>
concept memory_resource = (!std::is_reference_v<T>)&&requires(T value, size_t sz, size_t align, void* ptr) {
  { value.allocate(sz, align) } -> std::convertible_to<void*>;
  { value.deallocate(ptr, sz, align) } noexcept -> std::same_as<void>;
  requires std::is_nothrow_move_constructible_v<T>;
  requires alignof(T) <= alignof(std::max_align_t);
  requires !(std::is_empty_v<T> && !std::default_initializable<T>);
};

namespace noexport {

template <typename T>
concept coro = requires { typename T::promise_type; };

}

namespace pmr {

struct polymorphic_resource {
 private:
  std::pmr::memory_resource* passed;

  static auto& default_resource() {
    // never null
    static std::atomic<std::pmr::memory_resource*> r = std::pmr::new_delete_resource();
    return r;
  }
  static auto& passed_resource() {
    thread_local constinit std::pmr::memory_resource* r = nullptr;
    return r;
  }
  friend std::pmr::memory_resource& get_default_resource() noexcept;
  friend std::pmr::memory_resource& set_default_resource(std::pmr::memory_resource&) noexcept;
  friend void pass_resource(std::pmr::memory_resource&) noexcept;

 public:
  polymorphic_resource() noexcept : passed(std::exchange(passed_resource(), nullptr)) {
    if (!passed)
      passed = std::pmr::get_default_resource();
    assert(passed != nullptr);
  }
  polymorphic_resource(std::pmr::memory_resource& m) noexcept : passed(&m) {
  }
  void* allocate(size_t sz, size_t align) {
    return passed->allocate(sz, align);
  }
  void deallocate(void* p, std::size_t sz, std::size_t align) noexcept {
    passed->deallocate(p, sz, align);
  }
};

inline std::pmr::memory_resource& get_default_resource() noexcept {
  return *polymorphic_resource::default_resource().load(std::memory_order_acquire);
}

inline std::pmr::memory_resource& set_default_resource(std::pmr::memory_resource& r) noexcept {
  return *polymorphic_resource::default_resource().exchange(&r, std::memory_order_acq_rel);
}
inline void pass_resource(std::pmr::memory_resource& m) noexcept {
  polymorphic_resource::passed_resource() = &m;
}

}  // namespace pmr

consteval size_t coroframe_align() {
  // Question: what will be if coroutine local contains alignas(64) int i; ?
  // Answer: (quote from std::generator paper)
  //  "Let BAlloc be allocator_traits<A>::template rebind_alloc<U> where U denotes an unspecified type
  // whose size and alignment are both _STDCPP_DEFAULT_NEW_ALIGNMENT__"
  return __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}

// TODO free list with customizable max blocks
//  must be good because coroutines have same sizes,
//  its easy to reuse memory for them
// TODO info about current channel/generator call, is_top, handle, iteration from top to bot/reverse, swap two
//  generators in chain
//  struct co_memory_resource { ... };

// when passed into coroutine coro will allocate/deallocate memory using this resource
template <memory_resource R>
struct with_resource {
  KELCORO_NO_UNIQUE_ADDRESS R resource;
};
template <typename X>
with_resource(X&&) -> with_resource<std::remove_cvref_t<X>>;
with_resource(std::pmr::memory_resource&) -> with_resource<pmr::polymorphic_resource>;

namespace noexport {

template <typename... Args>
struct find_resource_tag : std::type_identity<void> {};
template <typename R, typename... Args>
struct find_resource_tag<with_resource<R>, Args...> : std::type_identity<R> {};
template <typename Head, typename... Tail>
struct find_resource_tag<Head, Tail...> : find_resource_tag<Tail...> {};

}  // namespace noexport

// searches for 'with_resource' in Types and returns first finded or void if no such
template <typename... Types>
using find_resource_tag_t = typename noexport::find_resource_tag<std::remove_cvref_t<Types>...>::type;

namespace noexport {

template <typename... Types>
consteval bool contains_1_resource_tag() {
  return 1 == (!std::is_void_v<find_resource_tag_t<Types>> + ... + 0);
}

constexpr auto only_for_resource(auto&& foo, auto&&... args) {
  static_assert(contains_1_resource_tag<decltype(args)...>());
  auto try_one = [&](auto& x) {
    if constexpr (contains_1_resource_tag<decltype(x)>())
      foo(x.resource);
  };
  (try_one(args), ...);
}

template <size_t RequiredPadding>
constexpr size_t padding_len(size_t sz) noexcept {
  enum { P = RequiredPadding };
  static_assert(P != 0);
  return ((P - sz) % P) % P;
}

}  // namespace noexport

// inheritor(coroutine promise) may be allocated with 'R'
// using 'with_resource' tag or default constructed 'R'
template <memory_resource R>
struct overload_new_delete {
 private:
  enum { frame_align = std::max(coroframe_align(), alignof(R)) };
  static void* do_allocate(size_t frame_sz, R& r) {
    if constexpr (std::is_empty_v<R>)
      return (void*)r.allocate(frame_sz, coroframe_align());
    else {
      const size_t padding = noexport::padding_len<frame_align>(frame_sz);
      const size_t bytes_count = frame_sz + padding + sizeof(R);
      std::byte* p = (std::byte*)r.allocate(bytes_count, frame_align);
      std::byte* resource_place = p + frame_sz + padding;
      std::construct_at((R*)resource_place, std::move(r));
      return p;
    }
  }

 public:
  static void* operator new(size_t frame_sz)
    requires(std::default_initializable<R>)
  {
    R r{};
    return do_allocate(frame_sz, r);
  }
  template <typename... Args>
    requires(std::same_as<find_resource_tag_t<Args...>, R> && noexport::contains_1_resource_tag<Args...>())
  static void* operator new(std::size_t frame_sz, Args&&... args) {
    void* p;
    noexport::only_for_resource([&](auto& resource) { p = do_allocate(frame_sz, resource); }, args...);
    return p;
  }
  static void operator delete(void* ptr, std::size_t frame_sz) noexcept {
    if constexpr (std::is_empty_v<R>) {
      R r{};
      r.deallocate(ptr, frame_sz, coroframe_align());
    } else {
      const size_t padding = noexport::padding_len<frame_align>(frame_sz);
      const size_t bytes_count = frame_sz + padding + sizeof(R);
      R* onframe_resource = (R*)((std::byte*)ptr + frame_sz + padding);
      assert((((uintptr_t)onframe_resource % alignof(R)) == 0));
      R r = std::move(*onframe_resource);
      std::destroy_at(onframe_resource);
      r.deallocate(ptr, bytes_count, frame_align);
    }
  }
};

struct enable_resource_deduction {};

// creates type of coroutine which may be allocated with resource 'R'
// disables resource deduction
// typical usage: aliases like generator_r/channel_r/etc
// for not duplicating code and not changing signature with default constructible resources
// see dd::pmr::generator as example
template <typename Coro, memory_resource R>
struct KELCORO_MSVC_EBO resourced : Coro {
  using resource_type = R;

  using Coro::Coro;
  using Coro::operator=;

  constexpr resourced(auto&&... args)
    requires(std::constructible_from<Coro, decltype(args)...>)
      : Coro(std::forward<decltype(args)>(args)...) {
  }

  constexpr Coro& decay() & noexcept {
    return *this;
  }
  constexpr Coro&& decay() && noexcept {
    return std::move(*this);
  }
  constexpr const Coro& decay() const& noexcept {
    return *this;
  }
  constexpr const Coro&& decay() const&& noexcept {
    return std::move(*this);
  }
};

template <typename Promise, memory_resource R>
struct KELCORO_MSVC_EBO resourced_promise : Promise, overload_new_delete<R> {
  using Promise::Promise;
  using Promise::operator=;

  constexpr resourced_promise(auto&&... args)
    requires(std::constructible_from<Promise, decltype(args)...>)
      : Promise(std::forward<decltype(args)>(args)...) {
  }

  using overload_new_delete<R>::operator new;
  using overload_new_delete<R>::operator delete;

  // assume sizeof and alignof of *this is equal with 'Promise'
  // its formal UB, but its used in reference implementation,
  // standard wording goes wrong
};

template <typename Promise, memory_resource R>
struct operation_hash<std::coroutine_handle<resourced_promise<Promise, R>>> {
  size_t operator()(std::coroutine_handle<resourced_promise<Promise, R>> h) const {
    return operation_hash<std::coroutine_handle<Promise>>()(
        // assume addresses are same (dirty hack for supporting allocators)
        std::coroutine_handle<Promise>::from_address(h.address()));
  }
};

}  // namespace dd

namespace std {

template <::dd::noexport::coro Coro, ::dd::memory_resource R, typename... Args>
struct coroutine_traits<::dd::resourced<Coro, R>, Args...> {
  using promise_type = ::dd::resourced_promise<typename Coro::promise_type, R>;
};

template <::dd::noexport::coro Coro, typename... Args>
  requires derived_from<Coro, ::dd::enable_resource_deduction>
struct coroutine_traits<Coro, Args...> {
 private:
  template <typename Promise>
  static auto deduct_promise() {
    if constexpr (::dd::noexport::contains_1_resource_tag<Args...>())
      return std::type_identity<::dd::resourced_promise<Promise, ::dd::find_resource_tag_t<Args...>>>{};
    else
      return std::type_identity<Promise>{};
  }

 public:
  // if Args contain exactly ONE(1) 'with_resource<X>' it is used, otherwise Coro::promise_type selected
  using promise_type = typename decltype(deduct_promise<typename Coro::promise_type>())::type;
};

}  // namespace std
