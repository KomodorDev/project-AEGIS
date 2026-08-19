// Purpose: expose the minimal public identity API used to prove the M0 library is wired correctly.

// Interesting syntax: #pragma once prevents duplicate declarations when this header is included
// twice.
#pragma once

// string_view returns immutable, non-owning text without allocating memory.
#include <string_view>

// Keep public symbols under the project namespace to avoid collisions with application code.
namespace aegis {
// --------------------------------------------------------
// [[nodiscard]] warns if callers discard the immutable project name; noexcept promises it cannot
// throw.
[[nodiscard]] std::string_view project_name() noexcept;
// --------------------------------------------------------
// Return the immutable project version without allocation or exceptions.
[[nodiscard]] std::string_view project_version() noexcept;
// --------------------------------------------------------
} // namespace aegis
