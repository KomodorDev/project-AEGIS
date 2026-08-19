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

// --------------------------------------------------------
// Persisted numeric assignments must not drift when later milestones add new error categories.
static_assert(static_cast<std::uint16_t>(DomainErrorCode::InvalidIdentifier) == 1U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::TraceCapacityExceeded) == 301U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::ExecutorWrongOwner) == 400U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::ExecutorReentryDetected) == 401U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::ExecutorNotBound) == 402U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::ExecutorClockRegression) == 403U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::ExecutorCounterExhausted) == 404U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::InvalidRuntimePolicy) == 405U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::RuntimeEvidenceExhausted) == 406U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::RuntimeSourceNotConfigured) == 407U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::InvalidMarketEvent) == 500U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::InvalidMarketState) == 501U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketSequenceGap) == 502U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketSequenceConflict) == 503U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketMetadataMismatch) == 504U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketIntegrityFailure) == 505U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketBookInvalid) == 506U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketBookCapacityExceeded) == 507U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketNotReady) == 508U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::MarketCounterExhausted) == 509U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::DiagnosticCapacityExceeded) == 510U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::StrategyNotConfigured) == 600U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::DispatchCapacityExceeded) == 601U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::DispatchReentryDetected) == 602U);
static_assert(static_cast<std::uint16_t>(DomainErrorCode::CallbackCounterExhausted) == 603U);

// --------------------------------------------------------
// Successful results expose their value through both the explicit predicate and contextual boolean
// conversion used by production validation chains.
TEST_CASE("a successful result exposes only its value", "[model][result]") {
  auto result = Result<std::string>::success("accepted");

  REQUIRE(result.has_value());
  CHECK(static_cast<bool>(result));
  CHECK(result.value() == "accepted");
}

// --------------------------------------------------------
// Collection failures must retain stable machine-readable location rather than depending on prose.
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

// --------------------------------------------------------
// Command-style operations use the void specialization without weakening field-only error context.
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

// --------------------------------------------------------

} // namespace
