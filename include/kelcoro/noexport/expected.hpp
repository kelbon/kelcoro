#pragma once

#include <variant>

namespace dd {

template <typename T>
struct unexpected {
  T value;
};

template <typename T>
unexpected(T&&) -> unexpected<std::remove_cvref_t<T>>;

// used only as return of when_all etc
template <typename T, typename E>
struct expected {
  struct void_t {};
  using value_type = std::conditional_t<!std::is_void_v<T>, T, void_t>;
  std::variant<value_type, E> data;

  expected() = default;

  template <typename U = T>
  expected(U&& arg) : data(std::forward<U>(arg)) {
  }
  template <typename U>
  expected(unexpected<U> u) : data(std::move(u.value)) {
  }

  explicit operator bool() const noexcept {
    return has_value();
  }
  [[nodiscard]] bool has_value() const noexcept {
    return std::holds_alternative<value_type>(data);
  }
  value_type& operator*() noexcept
    requires(!std::is_void_v<T>)
  {
    assert(has_value());
    return *std::get_if<0>(&data);
  }
  const value_type& operator*() const noexcept
    requires(!std::is_void_v<T>)
  {
    assert(has_value());
    return *std::get_if<0>(&data);
  }
  // precondition: !has_value()
  E& error() noexcept {
    assert(!has_value());
    return std::get_if<1>(&data);
  }
  // precondition: !has_value()
  const E& error() const noexcept {
    assert(!has_value());
    return std::get_if<1>(&data);
  }
};

}  // namespace dd