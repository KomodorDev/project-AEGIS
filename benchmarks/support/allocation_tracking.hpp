// Purpose: expose benchmark-only scoped C++ allocation counting while keeping the single global
// replacement implementation in one translation unit.

#pragma once

#include <cstdint>

namespace aegis_benchmark_support::allocation_tracking {

// --------------------------------------------------------
// Begin one non-nested allocation interval on the calling benchmark thread.
void begin() noexcept;

// --------------------------------------------------------
// Finish the calling thread's interval and return its successful C++ heap-allocation count.
[[nodiscard]] std::uint64_t finish() noexcept;

// --------------------------------------------------------

} // namespace aegis_benchmark_support::allocation_tracking
