// Purpose: prove a consumer can link AEGIS and read the configured project identity.

// Include the interface under test before the testing framework.
#include "aegis/version.hpp"

#include <catch2/catch_test_macros.hpp>

// Interesting syntax: Catch2 expands TEST_CASE and CHECK into a discoverable test and assertions.
TEST_CASE("the compiled library reports its project identity", "[baseline]") {
  CHECK(aegis::project_name() == "AEGIS");
  CHECK(aegis::project_version() == "0.1.0");
}
