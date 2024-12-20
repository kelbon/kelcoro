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
  fixed_array(fixed_array&& rhs) noexcept {
    *this = std::move(rhs);
  }
  fixed_array& operator=(fixed_array&& rhs) noexcept {
    std::swap(arr, rhs.arr);
    std::swap(n, rhs.n);
    std::swap(resource, rhs.resource);
    return *this;
  }
  fixed_array(size_t n, std::pmr::memory_resource& resource = *std::pmr::new_delete_resource()) : n(n) {
    if (n == 0)
      return;
    arr = (T*)resource.allocate(sizeof(T) * n, alignof(T));
    std::uninitialized_default_construct_n(arr, n);
  }

  std::pmr::memory_resource* get_resource() const {
    assert(resource);
    return resource;
  }

  T* data() {
    return arr;
  }

  T* data() const {
    return arr;
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
    if (arr) {
      std::destroy_n(arr, n);
      // assume noexcept
      resource->deallocate(arr, sizeof(T) * n, alignof(T));
    }
  }
};

}  // namespace dd::noexport
