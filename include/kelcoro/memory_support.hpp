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

struct std_pmr_resource {
 private:
  std::pmr::memory_resource* mr;

 public:
  std_pmr_resource(std::pmr::memory_resource& r) noexcept : mr(&r) {
  }
  void* allocate(size_t sz, size_t align) {
    return mr->allocate(sz, align);
  }
  void deallocate(void* p, std::size_t sz, std::size_t align) noexcept {
    mr->deallocate(p, sz, align);
  }
};

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

// specialization for implicit ctor
template <>
struct with_resource<pmr::std_pmr_resource> {
  pmr::std_pmr_resource resource;

  with_resource(std::pmr::memory_resource& mr) noexcept : resource(mr) {
  }
  with_resource(pmr::std_pmr_resource mr) noexcept : resource(std::move(mr)) {
  }
};

using with_pmr_resource = with_resource<pmr::std_pmr_resource>;

namespace noexport {

template <typename T>
struct type_identity_special {
  using type = T;

  template <typename U>
  type_identity_special<U> operator=(type_identity_special<U>);
};

// TODO optimize when pack indexing will be possible
// void when Args is empty
template <typename... Args>
using last_type_t = typename std::remove_cvref_t<decltype((type_identity_special<void>{} = ... =
                                                               type_identity_special<Args>{}))>::type;

template <typename T>
struct memory_resource_info : std::false_type {
  using resource_type = void;
};
template <typename R>
struct memory_resource_info<with_resource<R>> : std::true_type {
  using resource_type = R;
};

}  // namespace noexport

template <typename... Args>
using get_memory_resource_info =
    noexport::memory_resource_info<std::remove_cvref_t<noexport::last_type_t<Args...>>>;

template <typename... Args>
constexpr inline bool last_is_memory_resource_tag = get_memory_resource_info<Args...>::value;

template <typename... Args>
using resource_type_t = typename get_memory_resource_info<Args...>::resource_type;

namespace noexport {

template <size_t RequiredPadding>
constexpr size_t padding_len(size_t sz) noexcept {
  enum { P = RequiredPadding };
  static_assert(P != 0);
  return (P - sz % P) % P;
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
    requires(last_is_memory_resource_tag<Args...> && std::is_same_v<R, resource_type_t<Args...>>)
  static void* operator new(std::size_t frame_sz, Args&&... args) {
    static_assert(std::is_same_v<std::remove_cvref_t<noexport::last_type_t<Args...>>, with_resource<R>>);
    // TODO when possible
    // return do_allocate(frame_sz, (args...[sizeof...(Args) - 1]).resource);
    auto voidify = [](auto& x) { return const_cast<void*>((const void volatile*)std::addressof(x)); };
    void* p = (voidify(args), ...);
    return do_allocate(frame_sz, static_cast<with_resource<R>*>(p)->resource);
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

template <typename Coro, ::dd::memory_resource R, typename... Args>
struct coroutine_traits<::dd::resourced<Coro, R>, Args...> {
  using promise_type = ::dd::resourced_promise<typename Coro::promise_type, R>;
};

// enable_resource_deduction always uses last argument if present (memory_resource<R>)
template <typename Coro, typename... Args>
  requires(derived_from<Coro, ::dd::enable_resource_deduction> && dd::last_is_memory_resource_tag<Args...>)
struct coroutine_traits<Coro, Args...> {
  using promise_type = ::dd::resourced_promise<typename Coro::promise_type, ::dd::resource_type_t<Args...>>;
};

}  // namespace std
