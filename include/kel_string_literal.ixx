
module;
#include <cassert>
export module kel.traits : string_literal;

import<array>;
import<ranges>;

export import<string_view>;

export namespace kel {

// TEMPLATE string_literal and named_tag

struct no_consteval_t {};
constexpr inline auto no_consteval = no_consteval_t{};

// no null terminated!
// unique values, but each type corresponds to many strings(but you still can overload by value of this type)
template <typename Char, size_t N>
struct string_literal {
  using char_type = Char;

  std::array<Char, N> chars;

  // constructing from constexpr expressions

  // TODO - substr и мб конструкторы от итераторов бегин энд
  consteval string_literal(std::array<Char, N> arr) noexcept {
    std::ranges::copy(arr, chars.begin());
  }
  constexpr string_literal(std::array<Char, N> arr, no_consteval_t) noexcept {
    std::ranges::copy(arr, chars.begin());
  }
  consteval string_literal(const Char (&arr)[N + 1]) noexcept {
    std::ranges::copy(arr | std::views::take(N), chars.begin());
  }
  constexpr string_literal(const Char (&arr)[N + 1], no_consteval_t) noexcept {
    std::ranges::copy(arr | std::views::take(N), chars.begin());
  }
  // for using this ctor you need to explicitly specify N template argument

  // disable ctor if N is not deducted now(so no conflict with other ctor)
  template <typename = std::void_t<decltype(N)>>
  consteval string_literal(const Char* const ptr) noexcept {
    std::copy(ptr, ptr + N, chars.begin());
  }
  template <typename = std::void_t<decltype(N)>>
  constexpr string_literal(const Char* const ptr, no_consteval_t) noexcept {
    std::copy(ptr, ptr + N, chars.begin());
  }
  // comparing for using as NTTP

  consteval auto operator<=>(const string_literal&) const noexcept = default;
  template <size_t Y>
  consteval auto operator<=>(const string_literal<Char, Y>&) const noexcept {
    return N <=> Y;
  }
  consteval bool operator==(const string_literal&) const noexcept = default;
  template <size_t Y>
  consteval bool operator==(const string_literal<Char, Y>&) const noexcept {
    return false;
  }

  // using on runtime or in constexpr context
  static constexpr size_t size() noexcept {
    return N;
  };

  constexpr auto begin() const noexcept {
    return chars.begin();
  }
  constexpr auto end() const noexcept {
    return chars.end();
  }

  constexpr Char operator[](size_t index) const noexcept {
    assert(index < N);
    return chars[index];
  }

  template <typename Traits = std::char_traits<Char>>
  consteval std::basic_string_view<char_type, Traits> as_view() const noexcept {
    return {chars.data(), size()};
  }
  template <typename Traits = std::char_traits<Char>>
  constexpr std::basic_string_view<char_type, Traits> as_view(no_consteval_t) const noexcept {
    return {chars.data(), size()};
  }
};

template <typename Char, size_t N>
string_literal(const Char (&)[N]) -> string_literal<Char, N - 1>;
template <typename Char, size_t N>
string_literal(const Char (&)[N], no_consteval_t) -> string_literal<Char, N - 1>;

template <typename Char, size_t N, size_t M>
constexpr string_literal<Char, N + M> operator+(string_literal<Char, N> left,
                                                string_literal<Char, M> right) noexcept {
  Char result_data[N + M]{};
  std::ranges::copy(left, result_data);
  std::ranges::copy(right, result_data + N);
  return {result_data, no_consteval};
}

// one type corresponds to one string! every unique string corresponds to unique type!(supports overloading by
// type/value)
template <string_literal V>
struct named_tag {
  static constexpr auto value = V;

  using char_type = typename decltype(V)::char_type;

  // comparing

  consteval bool operator==(named_tag) const noexcept {
    return true;
  }
  template <string_literal L>
  consteval bool operator==(named_tag<L>) const noexcept {
    return false;
  }
  consteval auto operator<=>(named_tag) const noexcept {
    return std::strong_ordering::equal;
  }
  template <string_literal L>
  consteval auto operator<=>(named_tag<L>) const noexcept {
    return V <=> L;
  }

  // using on runtime or in constexpr context

  template <typename Traits = std::char_traits<char_type>>
  static consteval std::basic_string_view<char_type, Traits> as_view() noexcept {
    return value;
  }
};

// constructing from literals

namespace literals {
template <string_literal L>
consteval auto operator""_sl() noexcept {
  return L;
}
template <string_literal L>
consteval auto operator""_tag() noexcept {
  return named_tag<L>{};
}
}  // namespace literals

// integration with std::format for variadic arguments

template <size_t RepeatCount, string_literal pattern>
constexpr string_literal format_string_n = []<size_t... Is>(std::index_sequence<Is...>) {
  return ((Is, pattern) + ...);
}
(std::make_index_sequence<RepeatCount>{});

}  // namespace kel