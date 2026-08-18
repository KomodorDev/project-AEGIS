#include "aegis/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the compiled library reports its project identity", "[baseline]") {
  CHECK(aegis::project_name() == "AEGIS");
  CHECK(aegis::project_version() == "0.1.0");
}
