
#include <random>

#include <kelcoro/stack_memory_resource.hpp>
#include <kelcoro/task.hpp>

using byte_t = unsigned char;

static void test_allocator(size_t seed, dd::stack_resource resource) {
  struct chunk_data {
    void* ptr;
    size_t size;
    byte_t filling;

    chunk_data(void* p, size_t sz, byte_t fill) noexcept : ptr(p), size(sz), filling(fill) {
      std::span data((byte_t*)p, sz);
      std::fill(data.begin(), data.end(), fill);
    }

    void invariant_check() {
      byte_t* b = (byte_t*)ptr;
      for (byte_t byte : std::span(b, size)) {
        if (byte != filling)
          throw std::runtime_error("invariant failed");
      }
    }
  };

  std::vector<chunk_data> chunks;
  std::mt19937 gen(seed);

  for (int i = 0; i < 1000; ++i) {
    if (std::bernoulli_distribution(0.1)(gen)) {
      dd::stack_resource tmp;
      std::swap(tmp, resource);
      std::swap(tmp, resource);
    }
    if (chunks.size() && std::bernoulli_distribution(0.5)(gen)) {
      chunks.back().invariant_check();
      resource.deallocate(chunks.back().ptr, chunks.back().size);
      chunks.pop_back();
    } else {
      size_t len = std::uniform_int_distribution<size_t>(0, 500)(gen);
      chunks.emplace_back(resource.allocate(len), len, std::uniform_int_distribution<int>('A', 'Z')(gen));
      assert(((uintptr_t)chunks.back().ptr % 16) == 0);
    }
  }
}

static void do_test_resource() {
  {
    alignas(dd::coroframe_align()) unsigned char bytes[1];
    dd::stack_resource r(bytes);
    for (int seed = 0; seed < 1000; ++seed)
      test_allocator(seed, std::move(r));
  }
  {
    for (int seed = 0; seed < 1000; ++seed)
      test_allocator(seed, dd::stack_resource{});
  }
  {
    alignas(dd::coroframe_align()) unsigned char bytes[123];
    dd::stack_resource r(bytes);
    for (int seed = 0; seed < 1000; ++seed)
      test_allocator(seed, std::move(r));
  }
}

dd::task<int> bar(dd::with_stack_resource r) {
  void* p = r.resource.allocate(15);
  r.resource.deallocate(p, 15);
  co_return 4;
}

dd::task<int> foo(dd::with_stack_resource r) {
  co_return co_await bar(r) + co_await bar(r);
}

int main() {
  dd::stack_resource r;
  std::coroutine_handle<> h = foo(r).start_and_detach(true);
  while (!h.done())
    h.resume();
  h.destroy();
  do_test_resource();

  return 0;
}
