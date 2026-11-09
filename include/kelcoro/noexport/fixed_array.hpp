#pragma once

#include <cassert>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace dd::noexport {

template <typename T>
struct fixed_array {
 private:
  T* arr = nullptr;
  size_t n = 0;
  std::pmr::memory_resource* resource = std::pmr::new_delete_resource();

 public:
  static_assert(std::is_same_v<T, std::decay_t<T>>);
  fixed_array() = default;
  fixed_array(fixed_array&& other) noexcept : arr(other.arr), n(other.n), resource(other.resource) {
    other.arr = nullptr;
    other.n = 0;
    other.resource = nullptr;
  }

  fixed_array& operator=(fixed_array&& other) noexcept {
    if (this != &other) {
      std::destroy_at(this);
      std::construct_at(this, std::move(other));
    }
    return *this;
  }
  explicit fixed_array(size_t n, std::pmr::memory_resource& r = *std::pmr::new_delete_resource())
      : n(n), resource(&r) {
    if (n == 0)
      return;
    arr = (T*)resource->allocate(sizeof(T) * n, alignof(T));
    try {
      std::uninitialized_default_construct_n(arr, n);
    } catch (...) {
      resource->deallocate(arr, sizeof(T) * n, alignof(T));
      throw;
    }
  }

  std::pmr::memory_resource* get_resource() const {
    assert(resource);
    return resource;
  }

  T* data() {
    return arr;
  }

  const T* data() const {
    return arr;
  }

  size_t size() const noexcept {
    return n;
  }

  bool empty() const noexcept {
    return size() == 0;
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
    if (arr) {
      std::destroy_n(arr, n);
      // assume noexcept
      resource->deallocate(arr, sizeof(T) * n, alignof(T));
    }
  }
};

}  // namespace dd::noexport
