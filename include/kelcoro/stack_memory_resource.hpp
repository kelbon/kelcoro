#pragma once

#include <span>
#include <utility>

#include "memory_support.hpp"

namespace dd {

/*
 assumes, that only last allocated chunk may be deallocated (like real stack)
 its common case for coroutines, but this resource must be used carefully
 to preserve order of deallocations
 all allocated pointers aligned to coroframe_align()

 example:
    task<int> foo(with_stack_resource r) {
      co_await bar(r); // pass and wait to preseve allocations order
    }
*/
struct stack_resource {
 private:
  using byte_t = unsigned char;

  struct alignas(coroframe_align()) chunk_header {
    // prev chunk state
    byte_t *b, *m, *e;
    chunk_header* freelist_next;

    [[nodiscard]] size_t avail() const noexcept {
      return e - m;
    }
    [[nodiscard]] size_t capacity() const noexcept {
      return e - b;
    }
  };

  struct freelist_t {
    chunk_header* top = nullptr;

    void push(chunk_header* chunk) noexcept {
      assert(chunk);
      chunk->freelist_next = top;
      top = chunk;
    }

    chunk_header* take_atleast(size_t bytes) noexcept {
      size_t sz_before = count();
      chunk_header* prev = nullptr;
      chunk_header* c = top;
      while (c) {
        assert(c->b == c->m);
        if (c->capacity() >= bytes) {
          if (prev)
            prev->freelist_next = c->freelist_next;
          else
            top = c->freelist_next;
          c->freelist_next = nullptr;
          assert(count() == sz_before - 1);
          return c;
        }
        prev = c;
        c = c->freelist_next;
      }
      return nullptr;
    }

    void drop_all() noexcept {
      while (top) {
        chunk_header* next = top->freelist_next;
        delete[] (byte_t*)top;
        top = next;
      }
    }

    size_t count() noexcept {
      chunk_header* c = top;
      size_t sz = 0;
      while (c) {
        ++sz;
        c = c->freelist_next;
      }
      return sz;
    }
  };

  std::span<byte_t> initial_buffer;
  byte_t* b = nullptr;
  byte_t* m = nullptr;
  byte_t* e = nullptr;
  freelist_t freelist;

  chunk_header* prev_chunk() noexcept {
    assert(b != initial_buffer.data());
    byte_t* raw_chunk_begin = (byte_t*)b - sizeof(chunk_header);
    return (chunk_header*)raw_chunk_begin;
  }

  void drop_cur_chunk() {
    assert(b == m);
    assert(b != initial_buffer.data());
    chunk_header* chunk = prev_chunk();
    // replace prev chunk state with its own state
    b = std::exchange(chunk->b, b);
    m = std::exchange(chunk->m, m);
    e = std::exchange(chunk->e, e);
    freelist.push(chunk);
  }

  std::size_t aligned(std::size_t bytes) {
    return bytes + noexport::padding_len<dd::coroframe_align()>(bytes);
  }

 public:
  // precondition: 'bytes' aligned correctly to dd::coroframe_align
  constexpr explicit stack_resource(std::span<byte_t> bytes = {})
      : initial_buffer(bytes), b(bytes.data()), m(b), e(b + bytes.size()) {
    if (!std::is_constant_evaluated())
      assert(((uintptr_t)bytes.data() % dd::coroframe_align()) == 0 && "incorrect align!");
  }

  stack_resource(stack_resource&& other) noexcept
      : initial_buffer(other.initial_buffer), b(other.b), m(other.m), e(other.e), freelist(other.freelist) {
    other.initial_buffer = {};
    other.b = other.e = other.m = nullptr;
    other.freelist.top = nullptr;
  }

  stack_resource& operator=(stack_resource&& other) noexcept {
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
  }

  ~stack_resource() {
    reuse_memory();
    freelist.drop_all();
  }

  [[nodiscard]] void* allocate(size_t len) {
    len = aligned(len);
    if (e - m >= len)
      return std::exchange(m, m + len);
    if (chunk_header* chunk = freelist.take_atleast(len)) {
      assert(chunk->b == chunk->m);
      // store prev chunk in the 'chunk'
      b = std::exchange(chunk->b, b);
      m = std::exchange(chunk->m, m) + len;
      e = std::exchange(chunk->e, e);
    } else {
      // allocate atleast * 2 current size
      const size_t alloc_bytes = (len + (e - b) + initial_buffer.size()) * 2;
      byte_t* new_chunk = new byte_t[sizeof(chunk_header) + alloc_bytes];
      new (new_chunk) chunk_header{b, m, e, nullptr};
      b = new_chunk + sizeof(chunk_header);
      m = b + len;
      e = b + alloc_bytes;
    }
    return b;
  }

  void deallocate(void* ptr, size_t len) noexcept {
    assert(((uintptr_t)ptr % 16) == 0 && "dealloc not allocated memory");
    len = aligned(len);
    // not in current chunk, must be in some of prev
    // in most cases its literaly previous chunk, but its possible
    // if all memory from chunk deallocated, then allocated > chunk size
    // so prev chunk will be empty
    while ((byte_t*)ptr != m - len) {
      assert(ptr <= b || ptr >= e);
      drop_cur_chunk();
    }
    m -= len;
  }

  // works as 'deallocate(ptr, size)' for all allocated chunks
  void reuse_memory() noexcept {
    while (b != initial_buffer.data()) {
      m = b;
      drop_cur_chunk();
    }
  }
};

using with_stack_resource = with_resource<chunk_from<stack_resource>>;

}  // namespace dd
