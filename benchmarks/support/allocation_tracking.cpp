// Purpose: own the benchmark executable's sole replaceable allocation operators and thread-local
// scoped counter, never linking this instrumentation into the production library.

#include "support/allocation_tracking.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace aegis_benchmark_support::allocation_tracking {
namespace {

// Allocation tracking is disabled outside an explicitly bracketed workload on each benchmark
// thread, so fixture and Google Benchmark bookkeeping allocations are excluded.
thread_local bool tracking_enabled = false;
thread_local std::uint64_t successful_allocations = 0U;

// --------------------------------------------------------
// Count one successful C++ heap allocation without allocating inside the observation path.
void record_successful_allocation() noexcept {
  if (tracking_enabled && successful_allocations != std::numeric_limits<std::uint64_t>::max()) {
    ++successful_allocations;
  }
}

// --------------------------------------------------------
// Allocate one ordinary block before publishing its successful allocation observation.
[[nodiscard]] void* allocate_tracked_block(std::size_t size) {
  void* const pointer = std::malloc(size == 0U ? 1U : size);
  if (pointer == nullptr) {
    throw std::bad_alloc{};
  }
  record_successful_allocation();
  return pointer;
}

// --------------------------------------------------------
// Honor C++ over-alignment through the platform allocator and count only successful blocks.
[[nodiscard]] void* allocate_tracked_aligned_block(std::size_t size, std::align_val_t alignment) {
  void* pointer = nullptr;
  const auto byte_alignment = static_cast<std::size_t>(alignment);
  if (posix_memalign(&pointer, byte_alignment, size == 0U ? 1U : size) != 0) {
    throw std::bad_alloc{};
  }
  record_successful_allocation();
  return pointer;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reset and enable the calling thread's bounded measurement interval.
void begin_allocation_interval() noexcept {
  successful_allocations = 0U;
  tracking_enabled = true;
}

// --------------------------------------------------------
// Disable observation before returning the interval's exact successful-allocation count.
std::uint64_t finish_allocation_interval() noexcept {
  tracking_enabled = false;
  return successful_allocations;
}

// --------------------------------------------------------

} // namespace aegis_benchmark_support::allocation_tracking

// --------------------------------------------------------
// Interesting syntax: replaceable global allocation overloads let one shared scoped counter observe
// library containers and every benchmark translation unit without duplicate operator definitions.
void* operator new(std::size_t size) {
  return aegis_benchmark_support::allocation_tracking::allocate_tracked_block(size);
}

void* operator new[](std::size_t size) {
  return aegis_benchmark_support::allocation_tracking::allocate_tracked_block(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return aegis_benchmark_support::allocation_tracking::allocate_tracked_aligned_block(size,
                                                                                      alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return aegis_benchmark_support::allocation_tracking::allocate_tracked_aligned_block(size,
                                                                                      alignment);
}

// --------------------------------------------------------
// Pair every replaceable delete form with the malloc-family implementation above.
void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}

// --------------------------------------------------------
