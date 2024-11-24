#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "kelcoro/common.hpp"

namespace dd::noexport {

template <typename T>
struct fixed_array {
 private:
  T* arr = nullptr;
  size_t n = 0;
  std::pmr::memory_resource* resourse = std::pmr::new_delete_resource();

 public:
  static_assert(std::is_same_v<T, std::decay_t<T>>);
  static auto default_factory() {
    return [](size_t) { return T{}; };
  }
  fixed_array() = default;
  fixed_array(fixed_array&& rhs) noexcept {
    *this = std::move(rhs);
  }
  fixed_array& operator=(fixed_array&& rhs) noexcept {
    std::swap(arr, rhs.arr);
    std::swap(n, rhs.n);
    std::swap(resourse, rhs.resourse);
    return *this;
  }
  fixed_array(size_t n, std::pmr::memory_resource& resource = *std::pmr::new_delete_resource())
      : fixed_array(n, default_factory(), resource) {
  }
  fixed_array(size_t n, std::invocable<size_t> auto&& factory,
              std::pmr::memory_resource& resource = *std::pmr::new_delete_resource()) noexcept
      : n(n), resourse(&resource) {
    arr = (T*)resource.allocate(sizeof(T) * n, alignof(T));
    size_t constructed = 0;
    bool fail = true;
    scope_exit _{[&] {
      if (fail) {
        std::destroy_n(arr, constructed);
        resourse->deallocate(arr, sizeof(T) * n, alignof(T));
      }
    }};
    for (; constructed < n; constructed++)
      new (arr + constructed) T(factory(constructed));
    fail = false;
  }

  std::pmr::memory_resource* get_resource() const {
    assert(resourse);
    return resourse;
  }

  T* data() {
    return arr;
  }

  T* data() const {
    return arr;
  }

  void reset() noexcept {
    if (!arr)
      return;
    // avoid double destroy if element destructor calls reset again
    auto moved = std::move(*this);
    std::destroy_n(moved.arr, moved.n);
    // assume noexcept
    moved.resourse->deallocate(moved.arr, sizeof(T) * moved.n, alignof(T));
    moved.arr = nullptr;
    moved.n = 0;
  }

  size_t size() const noexcept {
    return n;
  }

  T* begin() noexcept {
    return arr;
  }

  T* end() noexcept {
    return arr + n;
  }

  const T* begin() const noexcept {
    return arr;
  }

  const T* end() const noexcept {
    return arr + n;
  }

  T& operator[](size_t i) noexcept {
    assert(arr);
    assert(i < n);
    return arr[i];
  }

  const T& operator[](size_t i) const noexcept {
    assert(arr);
    assert(i < n);
    return arr[i];
  }

  ~fixed_array() {
    reset();
  }
};

}  // namespace dd::noexport
