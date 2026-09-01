// Purpose: prove every textual identity obeys its grammar, remains nominally distinct, and copies
// through fixed inline storage without changing canonical bytes.

#include "aegis/model/identifier.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace aegis::model;

// --------------------------------------------------------
// Nominal identifier kinds must remain unrelated at compile time and constructible only through
// their validating factories.
static_assert(!std::is_same_v<FirmId, DeskId>);
static_assert(!std::is_same_v<LogicalAccountId, VenueAccountId>);
static_assert(!std::is_same_v<InstrumentId, VenueInstrumentId>);
static_assert(!std::is_same_v<MarketSourceId, VenueId>);
static_assert(!std::is_convertible_v<FirmId, DeskId>);
static_assert(!std::is_convertible_v<LogicalAccountId, VenueAccountId>);
static_assert(!std::is_constructible_v<FirmId, std::string>);
static_assert(std::is_trivially_copyable_v<FirmId>);
static_assert(std::is_trivially_copyable_v<VenueId>);
static_assert(std::is_trivially_copyable_v<InstrumentId>);
static_assert(std::is_trivially_copyable_v<VenueInstrumentId>);

// --------------------------------------------------------
// The reference vocabulary is a compatibility fixture for every public identifier kind.
TEST_CASE("reference configuration identifiers satisfy their nominal grammars", "[model][id]") {
  CHECK(FirmId::parse_identifier("firm.aegis-lab"));
  CHECK(DeskId::parse_identifier("desk.digital-assets"));
  CHECK(BotId::parse_identifier("bot.deribit-btc-perpetual-reference"));
  CHECK(StrategyId::parse_identifier("strategy.deterministic-reference"));
  CHECK(VenueId::parse_identifier("deribit"));
  CHECK(LogicalAccountId::parse_identifier("account.deribit-testnet-aegis"));
  CHECK(InstrumentId::parse_identifier("BTC-USD-PERPETUAL"));
  CHECK(VenueInstrumentId::parse_identifier("BTC-PERPETUAL"));
  CHECK(SubscriptionId::parse_identifier("subscription.deribit-btc-perpetual-book"));
  CHECK(RouteId::parse_identifier("route.deribit-testnet-btc-perpetual"));
  CHECK(VenueAccountId::parse_identifier("native-account:1234"));
  CHECK(MarketSourceId::parse_identifier("source.deribit-btc-perpetual"));
}

// --------------------------------------------------------
// Organization IDs combine a kind-specific prefix with bounded lowercase slug segments; failures
// must also identify the exact nominal field.
TEST_CASE("organizational identifiers reject wrong prefixes and malformed segments",
          "[model][id]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Exercise every malformed segment shape against the same stable error contract.
  constexpr std::array invalid_firms{
      "",           "aegis-lab",       "desk.aegis-lab",  "firm.",
      "firm.Aegis", "firm.aegis..lab", "firm.aegis--lab", "firm.-aegis",
      "firm.aegis-"};

  for (const std::string_view value : invalid_firms) {
    CAPTURE(value);
    const auto result = FirmId::parse_identifier(value);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidIdentifier);
    CHECK(result.error().context.field == "firm_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin both sides of the maximum accepted organizational identifier length.
  CHECK(FirmId::parse_identifier("firm.a.b-c0"));
  CHECK(FirmId::parse_identifier("firm." + std::string(59U, 'a')));
  CHECK_FALSE(FirmId::parse_identifier("firm." + std::string(60U, 'a')));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Market sources use their own prefixed nominal identity so a venue or adapter token cannot select
// ingress state accidentally.
TEST_CASE("market source identifiers retain a distinct configured-stream grammar", "[model][id]") {
  CHECK(MarketSourceId::parse_identifier("source.deribit-btc-perpetual"));
  CHECK_FALSE(MarketSourceId::parse_identifier("deribit-btc-perpetual"));
  CHECK_FALSE(MarketSourceId::parse_identifier("source.Deribit"));
  CHECK_FALSE(MarketSourceId::parse_identifier("source.deribit..btc"));
}

// --------------------------------------------------------
// Venue and normalized-instrument grammars intentionally differ in case while sharing strict dash
// segmentation and fixed byte limits.
TEST_CASE("venue and normalized instrument identifiers enforce case and separators",
          "[model][id]") {
  CHECK(VenueId::parse_identifier("deribit-testnet"));
  CHECK_FALSE(VenueId::parse_identifier("Deribit"));
  CHECK_FALSE(VenueId::parse_identifier("deribit--testnet"));
  CHECK_FALSE(VenueId::parse_identifier("deribit.testnet"));
  CHECK_FALSE(VenueId::parse_identifier(std::string(33U, 'a')));

  CHECK(InstrumentId::parse_identifier("BTC-USD-PERPETUAL"));
  CHECK(InstrumentId::parse_identifier("X1-USD"));
  CHECK_FALSE(InstrumentId::parse_identifier("BTC_USD"));
  CHECK_FALSE(InstrumentId::parse_identifier("btc-usd"));
  CHECK_FALSE(InstrumentId::parse_identifier("BTC--USD"));
  CHECK_FALSE(InstrumentId::parse_identifier(std::string(65U, 'A')));
}

// --------------------------------------------------------
// Adapter values may contain venue punctuation and spaces, but never controls, embedded NUL, or
// bytes beyond the bounded printable-ASCII contract.
TEST_CASE("adapter identifiers accept only bounded printable ASCII", "[model][id]") {
  CHECK(VenueInstrumentId::parse_identifier("BTC-PERPETUAL"));
  CHECK(VenueInstrumentId::parse_identifier("venue instrument #1"));
  CHECK_FALSE(VenueInstrumentId::parse_identifier(""));
  CHECK_FALSE(VenueInstrumentId::parse_identifier(std::string(129U, 'X')));
  CHECK_FALSE(VenueInstrumentId::parse_identifier(std::string{"BTC\nPERPETUAL"}));
  CHECK_FALSE(VenueAccountId::parse_identifier(std::string{"native\0account", 14U}));
}

// --------------------------------------------------------
// Once accepted, an identifier exposes the exact validated bytes without a mutating escape hatch.
TEST_CASE("valid identifiers expose immutable canonical bytes", "[model][id]") {
  const auto parsed = FirmId::parse_identifier("firm.aegis-lab");
  REQUIRE(parsed);
  CHECK(parsed.value().value() == "firm.aegis-lab");

  const auto copied = parsed.value();
  CHECK(copied == parsed.value());
  CHECK(copied.value().data() != parsed.value().value().data());
}

// --------------------------------------------------------

} // namespace
