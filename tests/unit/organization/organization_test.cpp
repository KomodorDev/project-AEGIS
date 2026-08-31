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
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view value) {
  auto parsed = Identifier::parse_identifier(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Construct one typed firm registration without repeating parser boilerplate in each test.
[[nodiscard]] organization::Firm create_firm_registration_or_throw(std::string_view value) {
  return organization::Firm{parse_identifier_or_throw<model::FirmId>(value)};
}

// --------------------------------------------------------
// Construct one desk-to-firm ownership edge from readable fixture literals.
[[nodiscard]] organization::Desk create_desk_registration_or_throw(std::string_view desk_value,
                                                                   std::string_view firm_value) {
  return organization::Desk{parse_identifier_or_throw<model::DeskId>(desk_value),
                            parse_identifier_or_throw<model::FirmId>(firm_value)};
}

// --------------------------------------------------------
// Construct one bot-to-desk and bot-to-strategy registration from fixture literals.
[[nodiscard]] organization::BotRegistration
create_bot_registration_or_throw(std::string_view bot_value, std::string_view desk_value,
                                 std::string_view strategy_value) {
  return organization::BotRegistration{
      parse_identifier_or_throw<model::BotId>(bot_value),
      parse_identifier_or_throw<model::DeskId>(desk_value),
      parse_identifier_or_throw<model::StrategyId>(strategy_value)};
}

// --------------------------------------------------------
// The accepted case proves peer roots retain independent ownership after canonical publication.
TEST_CASE("peer firms produce canonical immutable bot attribution", "[organization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Author peer roots in non-canonical order so publication must sort without merging ownership.
  auto result = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(),
      {create_firm_registration_or_throw("firm.zeta-subsidiary"),
       create_firm_registration_or_throw("firm.alpha-subsidiary")},
      {create_desk_registration_or_throw("desk.zeta-desk", "firm.zeta-subsidiary"),
       create_desk_registration_or_throw("desk.alpha-desk", "firm.alpha-subsidiary")},
      {create_bot_registration_or_throw("bot.zeta", "desk.zeta-desk", "strategy.shared"),
       create_bot_registration_or_throw("bot.alpha", "desk.alpha-desk", "strategy.shared")});

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify canonical firm order and immutable revision ownership.
  REQUIRE(result);
  const organization::Organization& configured = result.value();
  REQUIRE(configured.firms().size() == 2U);
  CHECK(configured.firms()[0U].id ==
        parse_identifier_or_throw<model::FirmId>("firm.alpha-subsidiary"));
  CHECK(configured.firms()[1U].id ==
        parse_identifier_or_throw<model::FirmId>("firm.zeta-subsidiary"));
  CHECK(configured.revision() == model::OrganizationRevision::create_initial());

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve one bot through its complete desk, firm, and strategy attribution chain.
  const organization::BotAttribution* attribution =
      configured.find_bot(parse_identifier_or_throw<model::BotId>("bot.zeta"));
  REQUIRE(attribution != nullptr);
  CHECK(attribution->desk_id == parse_identifier_or_throw<model::DeskId>("desk.zeta-desk"));
  CHECK(attribution->firm_id == parse_identifier_or_throw<model::FirmId>("firm.zeta-subsidiary"));
  CHECK(attribution->strategy_id ==
        parse_identifier_or_throw<model::StrategyId>("strategy.shared"));
  CHECK(configured.find_bot(parse_identifier_or_throw<model::BotId>("bot.unknown")) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Rejection cases lock root-to-leaf precedence for missing, duplicate, dangling, and orphaned
// nodes.
TEST_CASE("organization rejects empty registration collections in section order",
          "[organization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Missing roots fail before any descendant collection can be considered.
  const auto no_firms = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(), {}, {}, {});
  REQUIRE_FALSE(no_firms);
  CHECK(no_firms.error() == model::DomainError::create_at_field(
                                model::DomainErrorCode::EmptyCollection, "organization.firms"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Once a root exists, the empty desk layer becomes the first canonical failure.
  const auto no_desks = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(), {create_firm_registration_or_throw("firm.a")},
      {}, {});
  REQUIRE_FALSE(no_desks);
  CHECK(no_desks.error() == model::DomainError::create_at_field(
                                model::DomainErrorCode::EmptyCollection, "organization.desks"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Once roots and desks exist, an empty bot layer becomes the actionable failure.
  const auto no_bots = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(), {create_firm_registration_or_throw("firm.a")},
      {create_desk_registration_or_throw("desk.a", "firm.a")}, {});
  REQUIRE_FALSE(no_bots);
  CHECK(no_bots.error() == model::DomainError::create_at_field(
                               model::DomainErrorCode::EmptyCollection, "organization.bots"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Duplicate IDs are reported at their canonical sorted position rather than their authored order.
TEST_CASE("organization rejects duplicate typed identifiers after canonical sorting",
          "[organization]") {
  const auto result = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(),
      {create_firm_registration_or_throw("firm.a"), create_firm_registration_or_throw("firm.a")},
      {create_desk_registration_or_throw("desk.a", "firm.a")},
      {create_bot_registration_or_throw("bot.a", "desk.a", "strategy.a")});

  REQUIRE_FALSE(result);
  CHECK(result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "organization.firms.id", 1U));
}

// --------------------------------------------------------
// Dangling edges fail from roots toward leaves so descendant errors never mask their cause.
TEST_CASE("organization rejects dangling desk and bot references before descendants",
          "[organization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A desk cannot attribute ownership to a firm absent from the root catalog.
  const auto dangling_firm = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(), {create_firm_registration_or_throw("firm.a")},
      {create_desk_registration_or_throw("desk.a", "firm.missing")},
      {create_bot_registration_or_throw("bot.a", "desk.a", "strategy.a")});
  REQUIRE_FALSE(dangling_firm);
  CHECK(dangling_firm.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DanglingReference,
                                            "organization.desks.firm_id", 0U));

  // ++++++++++++++++++++++++++++++++++++++++
  // A bot cannot attribute ownership through a desk absent from the validated desk catalog.
  const auto dangling_desk = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(), {create_firm_registration_or_throw("firm.a")},
      {create_desk_registration_or_throw("desk.a", "firm.a")},
      {create_bot_registration_or_throw("bot.a", "desk.missing", "strategy.a")});
  REQUIRE_FALSE(dangling_desk);
  CHECK(dangling_desk.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DanglingReference,
                                            "organization.bots.desk_id", 0U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Completeness requires every registered root and intermediate node to own a descendant.
TEST_CASE("every firm needs a desk and every desk needs a bot", "[organization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical sorting makes the orphaned firm failure index deterministic.
  const auto firm_without_desk = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(),
      {create_firm_registration_or_throw("firm.z"), create_firm_registration_or_throw("firm.a")},
      {create_desk_registration_or_throw("desk.a", "firm.a")},
      {create_bot_registration_or_throw("bot.a", "desk.a", "strategy.a")});
  REQUIRE_FALSE(firm_without_desk);
  CHECK(firm_without_desk.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                            "organization.firms.desks", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
  // The same completeness rule rejects a desk that owns no bot.
  const auto desk_without_bot = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(), {create_firm_registration_or_throw("firm.a")},
      {create_desk_registration_or_throw("desk.z", "firm.a"),
       create_desk_registration_or_throw("desk.a", "firm.a")},
      {create_bot_registration_or_throw("bot.a", "desk.a", "strategy.a")});
  REQUIRE_FALSE(desk_without_bot);
  CHECK(desk_without_bot.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                            "organization.desks.bots", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
