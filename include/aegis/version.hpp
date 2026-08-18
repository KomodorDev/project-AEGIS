// Purpose: expose the minimal public identity API used to prove the M0 library is wired correctly.

// Interesting syntax: #pragma once prevents duplicate declarations when this header is included
// twice.
#pragma once

// string_view returns immutable, non-owning text without allocating memory.
#include <string_view>

// Keep public symbols under the project namespace to avoid collisions with application code.
namespace aegis {

// [[nodiscard]] warns if callers discard the identity; noexcept promises these queries cannot
// throw.
[[nodiscard]] std::string_view project_name() noexcept;
[[nodiscard]] std::string_view project_version() noexcept;

} // namespace aegis
