// Purpose: prove domain failures carry stable structured context through value and void results.

#include "aegis/model/result.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using aegis::model::DomainError;
using aegis::model::DomainErrorCode;
using aegis::model::Result;

static_assert(static_cast<std::uint16_t>(DomainErrorCode::InvalidIdentifier) == 1U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::TraceCapacityExceeded) == 301U);

TEST_CASE("a successful result exposes only its value", "[model][result]") {
  auto result = Result<std::string>::success("accepted");

  REQUIRE(result.has_value());
  CHECK(static_cast<bool>(result));
  CHECK(result.value() == "accepted");
}

TEST_CASE("a failed result preserves field and collection position", "[model][result]") {
  auto result = Result<int>::failure(
      DomainError::at_index(DomainErrorCode::DuplicateIdentifier, "firms", 3U));

  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(static_cast<bool>(result));
  CHECK(result.error().code == DomainErrorCode::DuplicateIdentifier);
  CHECK(result.error().context.field == "firms");
  REQUIRE(result.error().context.collection_index.has_value());
  CHECK(*result.error().context.collection_index == std::size_t{3U});
}

TEST_CASE("void results distinguish completion from failure", "[model][result]") {
  const auto completed = Result<void>::success();
  const auto failed =
      Result<void>::failure(DomainError::at_field(DomainErrorCode::InvalidValue, "route.enabled"));

  CHECK(completed.has_value());
  completed.value();
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().context.field == "route.enabled");
  CHECK_FALSE(failed.error().context.collection_index.has_value());
}

} // namespace
