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
  CHECK(FirmId::parse("firm.aegis-lab"));
  CHECK(DeskId::parse("desk.digital-assets"));
  CHECK(BotId::parse("bot.deribit-btc-perpetual-reference"));
  CHECK(StrategyId::parse("strategy.deterministic-reference"));
  CHECK(VenueId::parse("deribit"));
  CHECK(LogicalAccountId::parse("account.deribit-testnet-aegis"));
  CHECK(InstrumentId::parse("BTC-USD-PERPETUAL"));
  CHECK(VenueInstrumentId::parse("BTC-PERPETUAL"));
  CHECK(SubscriptionId::parse("subscription.deribit-btc-perpetual-book"));
  CHECK(RouteId::parse("route.deribit-testnet-btc-perpetual"));
  CHECK(VenueAccountId::parse("native-account:1234"));
  CHECK(MarketSourceId::parse("source.deribit-btc-perpetual"));
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
    const auto result = FirmId::parse(value);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidIdentifier);
    CHECK(result.error().context.field == "firm_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin both sides of the maximum accepted organizational identifier length.
  CHECK(FirmId::parse("firm.a.b-c0"));
  CHECK(FirmId::parse("firm." + std::string(59U, 'a')));
  CHECK_FALSE(FirmId::parse("firm." + std::string(60U, 'a')));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Market sources use their own prefixed nominal identity so a venue or adapter token cannot select
// ingress state accidentally.
TEST_CASE("market source identifiers retain a distinct configured-stream grammar", "[model][id]") {
  CHECK(MarketSourceId::parse("source.deribit-btc-perpetual"));
  CHECK_FALSE(MarketSourceId::parse("deribit-btc-perpetual"));
  CHECK_FALSE(MarketSourceId::parse("source.Deribit"));
  CHECK_FALSE(MarketSourceId::parse("source.deribit..btc"));
}

// --------------------------------------------------------
// Venue and normalized-instrument grammars intentionally differ in case while sharing strict dash
// segmentation and fixed byte limits.
TEST_CASE("venue and normalized instrument identifiers enforce case and separators",
          "[model][id]") {
  CHECK(VenueId::parse("deribit-testnet"));
  CHECK_FALSE(VenueId::parse("Deribit"));
  CHECK_FALSE(VenueId::parse("deribit--testnet"));
  CHECK_FALSE(VenueId::parse("deribit.testnet"));
  CHECK_FALSE(VenueId::parse(std::string(33U, 'a')));

  CHECK(InstrumentId::parse("BTC-USD-PERPETUAL"));
  CHECK(InstrumentId::parse("X1-USD"));
  CHECK_FALSE(InstrumentId::parse("BTC_USD"));
  CHECK_FALSE(InstrumentId::parse("btc-usd"));
  CHECK_FALSE(InstrumentId::parse("BTC--USD"));
  CHECK_FALSE(InstrumentId::parse(std::string(65U, 'A')));
}

// --------------------------------------------------------
// Adapter values may contain venue punctuation and spaces, but never controls, embedded NUL, or
// bytes beyond the bounded printable-ASCII contract.
TEST_CASE("adapter identifiers accept only bounded printable ASCII", "[model][id]") {
  CHECK(VenueInstrumentId::parse("BTC-PERPETUAL"));
  CHECK(VenueInstrumentId::parse("venue instrument #1"));
  CHECK_FALSE(VenueInstrumentId::parse(""));
  CHECK_FALSE(VenueInstrumentId::parse(std::string(129U, 'X')));
  CHECK_FALSE(VenueInstrumentId::parse(std::string{"BTC\nPERPETUAL"}));
  CHECK_FALSE(VenueAccountId::parse(std::string{"native\0account", 14U}));
}

// --------------------------------------------------------
// Once accepted, an identifier exposes the exact validated bytes without a mutating escape hatch.
TEST_CASE("valid identifiers expose immutable canonical bytes", "[model][id]") {
  const auto parsed = FirmId::parse("firm.aegis-lab");
  REQUIRE(parsed);
  CHECK(parsed.value().value() == "firm.aegis-lab");

  const auto copied = parsed.value();
  CHECK(copied == parsed.value());
  CHECK(copied.value().data() != parsed.value().value().data());
}

// --------------------------------------------------------

} // namespace
