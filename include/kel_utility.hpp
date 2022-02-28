
#define WRAP(foo, ...)                                                                                    \
  [__VA_ARGS__](auto&&... x) noexcept(                                                                    \
      noexcept(foo(std::forward<decltype(x)>(x)...))) -> decltype(foo(std::forward<decltype(x)>(x)...)) { \
    return foo(std::forward<decltype(x)>(x)...);                                                          \
  }