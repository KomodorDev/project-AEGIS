#include "aegis/version.hpp"

static_assert(__cplusplus >= 202002L, "AEGIS requires C++20 or newer");

namespace aegis {

std::string_view project_name() noexcept { return "AEGIS"; }

std::string_view project_version() noexcept { return AEGIS_PROJECT_VERSION; }

} // namespace aegis
