#pragma once

#include <string_view>

namespace aegis {

[[nodiscard]] std::string_view project_name() noexcept;
[[nodiscard]] std::string_view project_version() noexcept;

} // namespace aegis
