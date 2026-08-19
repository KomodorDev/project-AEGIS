// Purpose: implement the M0 project-identity API with allocation-free compile-time text.

// Include the public declaration first so compiler diagnostics catch interface mismatches here.
#include "aegis/version.hpp"

// Interesting syntax: __cplusplus is the compiler's active language-level number (202002 means
// C++20).
static_assert(__cplusplus >= 202002L, "AEGIS requires C++20 or newer");

// Implement only symbols declared in the public aegis namespace.
namespace aegis {

// --------------------------------------------------------
// String literals have static lifetime, so returning non-owning string_view objects is safe.
std::string_view project_name() noexcept { return "AEGIS"; }
// --------------------------------------------------------
// AEGIS_PROJECT_VERSION is injected from CMake's project(VERSION ...) during compilation.
std::string_view project_version() noexcept { return AEGIS_PROJECT_VERSION; }
// --------------------------------------------------------

} // namespace aegis
