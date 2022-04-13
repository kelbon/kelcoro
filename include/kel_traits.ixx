
export module kel.traits;

export import<tuple>;
export import<type_traits>;

export import : string_literal;

export namespace kel {

template <typename...>
struct type_list;

template <typename T, T... Values>
struct value_list;

// for number_of_first_v npos equals to "no such type in pack"
inline constexpr size_t npos = size_t(-1);

}  // namespace kel

namespace noexport {

template <size_t I, typename... Args>
struct number_of_impl { // selected only for empty pack Args...
  static constexpr size_t value = kel::npos;  // no such element in pack
};
template <size_t I, typename T, typename... Args>
struct number_of_impl<I, T, T, Args...> {
  static constexpr size_t value = I;
};
template <size_t I, typename T, typename First, typename... Args>
struct number_of_impl<I, T, First, Args...> {
  static constexpr size_t value = number_of_impl<I + 1, T, Args...>::value;
};

// type_of_element
template <int64_t, int64_t, typename...>
struct type_of_element_helper;

template <int64_t index, typename T, typename... Types>
struct type_of_element_helper<index, index, T, Types...> {
  using type = T;
};

template <int64_t index, int64_t current_index, typename First, typename... Types>
struct type_of_element_helper<index, current_index, First, Types...> {
  using type = typename type_of_element_helper<index, current_index + 1, Types...>::type;
};

template <int64_t index, typename... Types>
struct type_of_element {
  static_assert(index < sizeof...(Types), "type_list index out of bounds");
  using type = typename type_of_element_helper<index, 0, Types...>::type;
};

// extract_type_list
template <typename>
struct extract_type_list;

template <template <typename...> typename Template, typename... Types>
struct extract_type_list<Template<Types...>> {
  using type = kel::type_list<Types...>;
};

// merge_type_lists
template <typename...>
struct merge_type_lists;

template <template <typename...> typename T, template <typename...> typename U, typename... A, typename... B>
struct merge_type_lists<T<A...>, U<B...>> {
  using type = kel::type_list<A..., B...>;
};

template <template <typename...> typename T, template <typename...> typename U, typename... A, typename... B,
          typename... Types>
struct merge_type_lists<T<A...>, U<B...>, Types...> {
  using type = typename merge_type_lists<kel::type_list<A..., B...>, Types...>::type;
};

// insert_type_list
template <template <typename...> typename Template, typename TypeList>
struct insert_type_list;

template <template <typename...> typename Template, typename... Types>
struct insert_type_list<Template, kel::type_list<Types...>> {
  using type = Template<Types...>;
};

// remove first arg
template <typename T>
struct remove_first_arg;

template <template <typename...> typename T, typename First, typename... Types>
struct remove_first_arg<T<First, Types...>> {
  using type = T<Types...>;
};

// value_of_element
template <typename, size_t, size_t, auto...>
struct value_of_element_helper;

template <typename T, size_t index, size_t current_index, T First, T... Values>
struct value_of_element_helper<T, index, current_index, First, Values...> {
  static constexpr T value = value_of_element_helper<T, index, current_index + 1ull, Values...>::value;
};
template <typename T, size_t index, T First, T... Values>
struct value_of_element_helper<T, index, index, First, Values...> {
  static constexpr T value = First;
};

// merge_value_lists
template <typename...>
struct merge_value_lists;

template <typename T, T... A, T... B, typename... Types>
struct merge_value_lists<kel::value_list<T, A...>, kel::value_list<T, B...>, Types...> {
  using type = typename merge_value_lists<kel::value_list<T, A..., B...>, Types...>::type;
};
template <typename T, T... A, T... B>  // end of recursion
struct merge_value_lists<kel::value_list<T, A...>, kel::value_list<T, B...>> {
  using type = kel::value_list<T, A..., B...>;
};

// make_value_list
template <typename T, T count, T start_value, T current_index, T step, T... Values>
struct value_sequence {
  using type = typename value_sequence<T, count, start_value, current_index + 1, step, Values...,
                                       start_value + step * sizeof...(Values)>::type;
};
template <typename T, T count, T start_value, T step,
          T... Values>  // specialization for end of resursion because count == index
struct value_sequence<T, count, start_value, count, step, Values...> {
  using type = kel::value_list<T, Values...>;
};

// reverse_type_list
template <typename...>
struct reverse_type_list_helper;

template <typename... Types, int... Indexes>
struct reverse_type_list_helper<kel::value_list<int, Indexes...>, Types...> {
 private:
  using help_type = kel::type_list<Types...>;

 public:
  using type = kel::type_list<typename help_type::template get<Indexes>...>;
};

template <typename... Types>
struct reverse_type_list {
  using type = typename reverse_type_list_helper<
      value_sequence<int, sizeof...(Types), (int)(sizeof...(Types)) - 1, 0, -1>, Types...>::type;
};
template <typename... Types>
struct reverse_type_list<kel::type_list<Types...>> {
  using type = typename reverse_type_list<Types...>::type;
};

// is_implicit_constructible
template <typename T, typename... Args>
struct is_implicit_constructible {
 private:
  template <typename Type>
  static void check(Type&&);

  template <typename... Something>
  static consteval auto func(int) -> decltype(check<T>({std::declval<Something>()...}), bool{}) {
    return true;
  }
  template <typename... Something>
  static consteval bool func(...) {
    return false;
  }

 public:
  static constexpr bool value = func<Args...>(0);
};

// is_instance_of
template <template <typename...> typename Template, typename TypeToCheck>
struct is_instance_of {
 private:
  template <typename>
  struct check : std::false_type {};
  template <typename... Args>
  struct check<Template<Args...>> : std::true_type {};

 public:
  static constexpr inline bool value = check<std::remove_cvref_t<TypeToCheck>>::value;
};
}  // namespace noexport
using namespace noexport;

export namespace kel {

// nullopt/nullptr/nullstruct
struct nullstruct {};

template <typename T>
constexpr inline bool is_nullstruct_v = std::is_same_v<nullstruct, std::remove_cv_t<T>>;

// TEMPLATE FUNCTION always_false (for static asserts)

[[nodiscard]] consteval inline bool always_false(auto&&) noexcept {
  return false;
}
template <typename...>
[[nodiscard]] consteval inline bool always_false() noexcept {
  return false;
}

// CONCEPT one_of

template <typename T, typename... Args>
concept one_of = sizeof...(Args) > 0 && ((std::same_as<T, Args>) || ...);

// CONCEPTS about co_await operator

template <typename T>
concept has_member_co_await = requires(T (*value)()) {
  value().operator co_await();
};

template <typename T>
concept has_global_co_await = requires(T (*value)()) {
  operator co_await(value());
};

template <typename T>
concept ambigious_co_await_lookup = has_global_co_await<T> && has_member_co_await<T>;

// CONCEPT co_awaiter

template <typename T>
concept co_awaiter = requires(T value) {
  { value.await_ready() } -> std::same_as<bool>;
  // cant check await_suspend here because:
  // case: value.await_suspend(coroutine_handle<>{}) - may be non convertible to concrete T in signature
  // of await_suspend
  // case: value.await_suspend(nullptr) - may be template signature, compilation error(cant deduct type)
  // another case - signature with requires, impossible to know how to call it
  value.await_resume();
};

// CONCEPT co_awaitable

template <typename T>
concept co_awaitable = has_member_co_await<T> || has_global_co_await<T> || co_awaiter<T>;

// CONCEPT enumeration

template <typename T>
concept enumeration = std::is_enum_v<std::remove_cv_t<T>>;

// TEMPLATE TYPE_LIST

template <int64_t Index, typename... Types>
using type_of_element_t = typename type_of_element<Index, Types...>::type;

template <typename... Types>
struct type_list {
  static constexpr size_t size = sizeof...(Types);
  template <int64_t index>
  using get = typename type_of_element<index, Types...>::type;
};

// TEMPLATE variable number_of_first_v

template <typename T, typename... Args>
inline constexpr size_t number_of_first_v = number_of_impl<0, T, Args...>::value;

// TRAIT extract_type_list

template <typename T>
using extract_type_list_t = typename extract_type_list<T>::type;

// TRAIT MERGE_TYPE_LISTS

template <typename... Types>
using merge_type_lists_t = typename merge_type_lists<Types...>::type;

// TRAIT insert_type_list

template <template <typename...> typename Template, typename TypeList>
using insert_type_list_t = typename insert_type_list<Template, TypeList>::type;

// TRAIT remove_first_arg
template <typename T>
using remove_first_arg_t = typename remove_first_arg<T>::type;

template <typename... Types>
using last_t = typename decltype(((std::type_identity<Types>{}), ...))::type;

template <typename First, typename...>
using first_t = First;

// TEMPLATE value_list

template <size_t index, typename T, T... Values>
struct value_of_element {
  static_assert(index < sizeof...(Values) || sizeof...(Values) == 0, "Index >= arguments count");
  static constexpr T value = value_of_element_helper<T, index, 0ull, Values...>::value;
};
template <typename T, T... Values>
struct value_list {
  static constexpr size_t size = sizeof...(Values);
  template <size_t index>
  static constexpr T get = value_of_element<index, T, Values...>::value;
};

// TRAIT merge_value_lists

template <typename... Types>
using merge_value_lists_t = typename merge_value_lists<Types...>::type;

// TRAIT make_value_list

template <typename T, size_t Count, T StartValue = 0, T Step = 1>
using make_value_list = typename value_sequence<T, Count, StartValue, 0, Step>::type;

// TRAIT make index list

template <size_t... Values>
using index_list = value_list<size_t, Values...>;

template <size_t Count, size_t StartValue = 0, size_t Step = 1>
using make_index_list = make_value_list<size_t, Count, StartValue, Step>;

// TRAIT reverse_type_list

template <typename... Types>
using reverse_type_list_t = typename reverse_type_list<Types...>::type;

// TRAIT is_explicit_constructible

template <typename T, typename... Types>
constexpr inline bool is_implicit_constructible_v = is_implicit_constructible<T, Types...>::value;
template <typename T>
constexpr inline bool is_implicit_default_constructible_v = is_implicit_constructible_v<T>;
template <typename T>
constexpr inline bool is_implicit_copy_constructible_v = is_implicit_constructible_v<T, const T&>;
template <typename T>
constexpr inline bool is_implicit_move_constructible_v = is_implicit_constructible_v<T, T&&>;

// TRAIT generate_unique_type
// clang-format off
template <auto V = []() {} >
using generate_unique_type = decltype(V);
// clang-format on

// TRAIT is_instance_of, works only for templates that NOT accepts non-type template parameters

template <template <typename...> typename Template, typename TypeToCheck>
constexpr inline bool is_instance_of_v = is_instance_of<Template, TypeToCheck>::value;

template <typename T, template <typename...> typename OfWhat>
concept instance_of = is_instance_of_v<OfWhat, T>;

}  // namespace kel

export namespace std {

template <typename... Types>
struct tuple_size<::kel::type_list<Types...>> : std::integral_constant<size_t, sizeof...(Types)> {};

template <size_t Index, typename... Types>
struct tuple_element<Index, ::kel::type_list<Types...>> {
  using type = ::kel::type_of_element_t<Index, Types...>;
};

template <size_t Index, typename... Types>
constexpr auto get(::kel::type_list<Types...>) noexcept {
  return std::type_identity<::kel::type_of_element_t<Index, Types...>>{};
}
template <typename T, T... Values>
struct tuple_size<::kel::value_list<T, Values...>> : std::integral_constant<size_t, sizeof...(Values)> {};

template <size_t Index, typename T, T... Values>
struct tuple_element<Index, ::kel::value_list<T, Values...>> {
  using type = T;
};

template <size_t Index, typename T, T... Values>
constexpr T get(::kel::value_list<T, Values...>) noexcept {
  return ::kel::value_of_element<Index, T, Values...>::value;
}

}  // namespace std
