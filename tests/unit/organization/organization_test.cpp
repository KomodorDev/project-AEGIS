// Purpose: prove multi-firm bot attribution is complete, immutable, and deterministic.

#include "aegis/organization/organization.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid literals are fixture-authoring defects, so these terse typed builders fail immediately.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view value) {
  auto parsed = Identifier::parse(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in test fixture"};
  }
  return std::move(parsed).value();
}
// --------------------------------------------------------
// Construct one typed firm registration without repeating parser boilerplate in each test.
[[nodiscard]] organization::Firm firm(std::string_view value) {
  return organization::Firm{id<model::FirmId>(value)};
}
// --------------------------------------------------------
// Construct one desk-to-firm ownership edge from readable fixture literals.
[[nodiscard]] organization::Desk desk(std::string_view desk_value, std::string_view firm_value) {
  return organization::Desk{id<model::DeskId>(desk_value), id<model::FirmId>(firm_value)};
}
// --------------------------------------------------------
// Construct one bot-to-desk and bot-to-strategy registration from fixture literals.
[[nodiscard]] organization::BotRegistration
bot(std::string_view bot_value, std::string_view desk_value, std::string_view strategy_value) {
  return organization::BotRegistration{id<model::BotId>(bot_value), id<model::DeskId>(desk_value),
                                       id<model::StrategyId>(strategy_value)};
}
// --------------------------------------------------------
// The accepted case proves peer roots retain independent ownership after canonical publication.
TEST_CASE("peer firms produce canonical immutable bot attribution", "[organization]") {
  // ++++++++++++++++++++++++++++++++++++++++
  // Author peer roots in non-canonical order so publication must sort without merging ownership.
  auto result = organization::Organization::create(
      model::OrganizationRevision::initial(),
      {firm("firm.zeta-subsidiary"), firm("firm.alpha-subsidiary")},
      {desk("desk.zeta-desk", "firm.zeta-subsidiary"),
       desk("desk.alpha-desk", "firm.alpha-subsidiary")},
      {bot("bot.zeta", "desk.zeta-desk", "strategy.shared"),
       bot("bot.alpha", "desk.alpha-desk", "strategy.shared")});
  // ++++++++++++++++++++++++++++++++++++++++
  // Verify canonical firm order and immutable revision ownership.
  REQUIRE(result);
  const organization::Organization& configured = result.value();
  REQUIRE(configured.firms().size() == 2U);
  CHECK(configured.firms()[0U].id == id<model::FirmId>("firm.alpha-subsidiary"));
  CHECK(configured.firms()[1U].id == id<model::FirmId>("firm.zeta-subsidiary"));
  CHECK(configured.revision() == model::OrganizationRevision::initial());
  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve one bot through its complete desk, firm, and strategy attribution chain.
  const organization::BotAttribution* attribution =
      configured.find_bot(id<model::BotId>("bot.zeta"));
  REQUIRE(attribution != nullptr);
  CHECK(attribution->desk_id == id<model::DeskId>("desk.zeta-desk"));
  CHECK(attribution->firm_id == id<model::FirmId>("firm.zeta-subsidiary"));
  CHECK(attribution->strategy_id == id<model::StrategyId>("strategy.shared"));
  CHECK(configured.find_bot(id<model::BotId>("bot.unknown")) == nullptr);
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// Rejection cases lock root-to-leaf precedence for missing, duplicate, dangling, and orphaned
// nodes.
TEST_CASE("organization rejects empty registration collections in section order",
          "[organization]") {
  // ++++++++++++++++++++++++++++++++++++++++
  // Missing roots fail before any descendant collection can be considered.
  const auto no_firms =
      organization::Organization::create(model::OrganizationRevision::initial(), {}, {}, {});
  REQUIRE_FALSE(no_firms);
  CHECK(no_firms.error() == model::DomainError::at_field(model::DomainErrorCode::EmptyCollection,
                                                         "organization.firms"));
  // ++++++++++++++++++++++++++++++++++++++++
  // Once a root exists, the empty desk layer becomes the first canonical failure.
  const auto no_desks = organization::Organization::create(model::OrganizationRevision::initial(),
                                                           {firm("firm.a")}, {}, {});
  REQUIRE_FALSE(no_desks);
  CHECK(no_desks.error() == model::DomainError::at_field(model::DomainErrorCode::EmptyCollection,
                                                         "organization.desks"));
  // ++++++++++++++++++++++++++++++++++++++++
  // Once roots and desks exist, an empty bot layer becomes the actionable failure.
  const auto no_bots = organization::Organization::create(
      model::OrganizationRevision::initial(), {firm("firm.a")}, {desk("desk.a", "firm.a")}, {});
  REQUIRE_FALSE(no_bots);
  CHECK(no_bots.error() ==
        model::DomainError::at_field(model::DomainErrorCode::EmptyCollection, "organization.bots"));
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// Duplicate IDs are reported at their canonical sorted position rather than their authored order.
TEST_CASE("organization rejects duplicate typed identifiers after canonical sorting",
          "[organization]") {
  const auto result = organization::Organization::create(
      model::OrganizationRevision::initial(), {firm("firm.a"), firm("firm.a")},
      {desk("desk.a", "firm.a")}, {bot("bot.a", "desk.a", "strategy.a")});

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                                       "organization.firms.id", 1U));
}
// --------------------------------------------------------
// Dangling edges fail from roots toward leaves so descendant errors never mask their cause.
TEST_CASE("organization rejects dangling desk and bot references before descendants",
          "[organization]") {
  // ++++++++++++++++++++++++++++++++++++++++
  // A desk cannot attribute ownership to a firm absent from the root catalog.
  const auto dangling_firm = organization::Organization::create(
      model::OrganizationRevision::initial(), {firm("firm.a")}, {desk("desk.a", "firm.missing")},
      {bot("bot.a", "desk.a", "strategy.a")});
  REQUIRE_FALSE(dangling_firm);
  CHECK(dangling_firm.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DanglingReference,
                                     "organization.desks.firm_id", 0U));
  // ++++++++++++++++++++++++++++++++++++++++
  // A bot cannot attribute ownership through a desk absent from the validated desk catalog.
  const auto dangling_desk = organization::Organization::create(
      model::OrganizationRevision::initial(), {firm("firm.a")}, {desk("desk.a", "firm.a")},
      {bot("bot.a", "desk.missing", "strategy.a")});
  REQUIRE_FALSE(dangling_desk);
  CHECK(dangling_desk.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DanglingReference,
                                     "organization.bots.desk_id", 0U));
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// Completeness requires every registered root and intermediate node to own a descendant.
TEST_CASE("every firm needs a desk and every desk needs a bot", "[organization]") {
  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical sorting makes the orphaned firm failure index deterministic.
  const auto firm_without_desk = organization::Organization::create(
      model::OrganizationRevision::initial(), {firm("firm.z"), firm("firm.a")},
      {desk("desk.a", "firm.a")}, {bot("bot.a", "desk.a", "strategy.a")});
  REQUIRE_FALSE(firm_without_desk);
  CHECK(firm_without_desk.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                     "organization.firms.desks", 1U));
  // ++++++++++++++++++++++++++++++++++++++++
  // The same completeness rule rejects a desk that owns no bot.
  const auto desk_without_bot = organization::Organization::create(
      model::OrganizationRevision::initial(), {firm("firm.a")},
      {desk("desk.z", "firm.a"), desk("desk.a", "firm.a")}, {bot("bot.a", "desk.a", "strategy.a")});
  REQUIRE_FALSE(desk_without_bot);
  CHECK(desk_without_bot.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                     "organization.desks.bots", 1U));
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------

} // namespace
