#include "MemoryBudget.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

namespace {
  constexpr std::size_t mib = 1024 * 1024;
  constexpr auto max_size = std::numeric_limits<std::size_t>::max();
  constexpr std::uintmax_t cap_mb{BLIT_MEMORY_CAP_MB};
  static_assert(cap_mb > 0 && cap_mb <= max_size / mib,
                "BLIT_MEMORY_CAP_MB must be a positive number of MiB that fits in size_t");
  constexpr std::size_t cap_bytes = static_cast<std::size_t>(cap_mb) * mib;
  constexpr std::size_t default_alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  constexpr auto relaxed = std::memory_order_relaxed;

  // Reserve before allocating from the CRT heap. Only successful allocations
  // contribute to the public counters, even if another thread's malloc fails.
  std::atomic<std::size_t> reserved{0};
  std::atomic<std::size_t> live{0};
  std::atomic<std::size_t> peak{0};

  struct alignas(std::max_align_t) Header {
    void *base;
    std::size_t size;
  };

  struct PeakReport {
    ~PeakReport() {
      std::fprintf(stderr, "32blit simulator: peak C++ allocation: %zu bytes\n", peak.load(relaxed));
    }
  } peak_report;

  // Acquiring released reservations orders the prior live decrement before
  // the next allocation can count those bytes again.
  bool reserve(std::size_t size, bool abort_on_cap) noexcept {
    auto used = reserved.load(relaxed);
    do {
      if (size > cap_bytes - used) {
        if (!abort_on_cap)
          return false;
        std::fprintf(stderr, "32blit simulator: memory cap exceeded (BLIT_MEMORY_CAP_MB): "
                             "requested %zu bytes, %zu bytes in use or reserved, cap %zu bytes\n",
                     size, used, cap_bytes);
        std::fflush(stderr);
        std::abort();
      }
    } while (!reserved.compare_exchange_weak(used, used + size, std::memory_order_acquire, relaxed));
    return true;
  }

  void *allocate(std::size_t size, std::size_t alignment, bool abort_on_cap) noexcept {
    if (!reserve(size, abort_on_cap))
      return nullptr;

    const auto payload = size == 0 ? 1 : size;
    const auto header_alignment = alignof(Header);
    // malloc already aligns the header. Reserve only the extra padding needed
    // to put the payload at a stricter alignment, with the header just before it.
    const auto padding = alignment > header_alignment ? alignment - header_alignment : 0;
    const auto overhead = sizeof(Header) + padding;
    void *base = payload <= max_size - overhead ? std::malloc(payload + overhead) : nullptr;
    if (!base) {
      reserved.fetch_sub(size, relaxed);
      return nullptr;
    }

    void *result = static_cast<unsigned char *>(base) + sizeof(Header);
    if (padding) {
      auto space = payload + padding;
      (void)std::align(alignment, payload, result, space);
    }
    ::new (static_cast<unsigned char *>(result) - sizeof(Header)) Header{base, size};

    const auto current = live.fetch_add(size, relaxed) + size;
    auto high = peak.load(relaxed);
    while (current > high && !peak.compare_exchange_weak(high, current, relaxed)) {}
    return result;
  }

  void *allocate_or_throw(std::size_t size, std::size_t alignment) {
    if (auto *result = allocate(size, alignment, true))
      return result;
    throw std::bad_alloc{};
  }

  void release(void *ptr) noexcept {
    if (!ptr)
      return;
    const auto *header = reinterpret_cast<const Header *>(static_cast<unsigned char *>(ptr) - sizeof(Header));
    const auto size = header->size;
    live.fetch_sub(size, relaxed);
    std::free(header->base);
    reserved.fetch_sub(size, std::memory_order_release);
  }
}

namespace blit::simulator {
  std::size_t live_bytes() noexcept { return live.load(relaxed); }
  std::size_t peak_bytes() noexcept { return peak.load(relaxed); }
}

void *operator new(std::size_t size) { return allocate_or_throw(size, default_alignment); }
void *operator new[](std::size_t size) { return allocate_or_throw(size, default_alignment); }
void *operator new(std::size_t size, std::align_val_t align) {
  return allocate_or_throw(size, static_cast<std::size_t>(align));
}
void *operator new[](std::size_t size, std::align_val_t align) {
  return allocate_or_throw(size, static_cast<std::size_t>(align));
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  return allocate(size, default_alignment, false);
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return allocate(size, default_alignment, false);
}
void *operator new(std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept {
  return allocate(size, static_cast<std::size_t>(align), false);
}
void *operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept {
  return allocate(size, static_cast<std::size_t>(align), false);
}

void operator delete(void *ptr) noexcept { release(ptr); }
void operator delete[](void *ptr) noexcept { release(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { release(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { release(ptr); }
void operator delete(void *ptr, std::align_val_t) noexcept { release(ptr); }
void operator delete[](void *ptr, std::align_val_t) noexcept { release(ptr); }
void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept { release(ptr); }
void operator delete[](void *ptr, std::size_t, std::align_val_t) noexcept { release(ptr); }
void operator delete(void *ptr, const std::nothrow_t &) noexcept { release(ptr); }
void operator delete[](void *ptr, const std::nothrow_t &) noexcept { release(ptr); }
void operator delete(void *ptr, std::align_val_t, const std::nothrow_t &) noexcept { release(ptr); }
void operator delete[](void *ptr, std::align_val_t, const std::nothrow_t &) noexcept { release(ptr); }
