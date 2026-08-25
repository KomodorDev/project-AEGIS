// Purpose: prove every source-normalization provenance shape comes only from self-owned sealed
// configuration and preserves exact caller-unforgeable attribution.

#include "aegis/runtime/m4_provenance_resolver.hpp"
#include "m4_private_event_fixture.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace aegis;

// ########################################################################
// Public value types expose inspection only: raw callers cannot default/aggregate-construct trusted
// subject provenance or a complete normalized private input.
static_assert(!std::default_initializable<model::M4SubjectProvenance>);
static_assert(!std::is_aggregate_v<model::M4SubjectProvenance>);
static_assert(!std::default_initializable<model::M4Provenance>);
static_assert(!std::is_aggregate_v<model::M4Provenance>);
static_assert(!std::default_initializable<oms::NormalizedPrivateOrderInput>);
static_assert(!std::is_aggregate_v<oms::NormalizedPrivateOrderInput>);

// ########################################################################

// --------------------------------------------------------
// Root-only, account, supported-instrument, and unknown-source profiles expose exact typed absence.
TEST_CASE("M4 provenance resolver publishes every configuration-proved subject shape",
          "[m4][provenance]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& resolver = fixture.resolver();

  // ++++++++++++++++++++++++++++++++++++++++
  // Lineage provenance carries the complete root and no sentinel subject.
  const auto root_only = resolver.create_root_only_provenance();
  REQUIRE(root_only.root() == fixture.authority().m4_policy.root_provenance());
  REQUIRE_FALSE(root_only.subject().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A configured account carries only account, venue, and independently derived firm.
  const auto account =
      resolver.create_configured_account_provenance(fixture.account_id(), fixture.venue_id());
  REQUIRE(account);
  REQUIRE(account.value().subject().has_value());
  const auto& account_subject = *account.value().subject();
  REQUIRE(account_subject.logical_account_id() == fixture.account_id());
  REQUIRE(account_subject.venue_id() == fixture.venue_id());
  REQUIRE(account_subject.firm_id() ==
          test_support::parse_m4_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
  REQUIRE_FALSE(account_subject.desk_id().has_value());
  REQUIRE_FALSE(account_subject.bot_id().has_value());
  REQUIRE_FALSE(account_subject.strategy_id().has_value());
  REQUIRE_FALSE(account_subject.route().has_value());
  REQUIRE_FALSE(account_subject.instrument().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Supported instrument provenance adds the instrument/metadata pair atomically and nothing else.
  const auto instrument = resolver.create_configured_instrument_provenance(
      fixture.account_id(), fixture.venue_id(), fixture.instrument_id());
  REQUIRE(instrument);
  REQUIRE(instrument.value().subject()->instrument().has_value());
  REQUIRE(instrument.value().subject()->instrument()->instrument_id == fixture.instrument_id());
  REQUIRE(instrument.value().subject()->instrument()->metadata_revision ==
          fixture.record().provenance().metadata_revision);
  REQUIRE_FALSE(instrument.value().subject()->route().has_value());
  REQUIRE(account.value() != instrument.value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Authoritative source derivation retains maximal proof but never gains local attribution from raw
// locators or visual similarity.
TEST_CASE("M4 authoritative provenance remains source-limited", "[m4][provenance]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& resolver = fixture.resolver();

  // ++++++++++++++++++++++++++++++++++++++++
  // A supported configured source may retain firm plus instrument/metadata, never local ownership.
  const auto supported = resolver.derive_authoritative_source_provenance(
      fixture.account_id(), fixture.venue_id(), fixture.instrument_id());
  REQUIRE(supported.subject().has_value());
  REQUIRE(supported.subject()->firm_id() ==
          test_support::parse_m4_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
  REQUIRE(supported.subject()->instrument().has_value());
  REQUIRE_FALSE(supported.subject()->desk_id().has_value());
  REQUIRE_FALSE(supported.subject()->bot_id().has_value());
  REQUIRE_FALSE(supported.subject()->strategy_id().has_value());
  REQUIRE_FALSE(supported.subject()->route().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A known account under a contradictory venue proves firm only; it cannot claim instrument scope.
  const auto other_venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("other");
  const auto wrong_venue = resolver.derive_authoritative_source_provenance(
      fixture.account_id(), other_venue, fixture.instrument_id());
  REQUIRE(wrong_venue.subject()->firm_id().has_value());
  REQUIRE_FALSE(wrong_venue.subject()->instrument().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A syntactically valid unknown account retains exact locators and no invented firm/instrument.
  const auto unknown_account = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
      "account.unknown-private-source");
  const auto unknown = resolver.derive_authoritative_source_provenance(
      unknown_account, fixture.venue_id(), fixture.instrument_id());
  REQUIRE(unknown.subject()->logical_account_id() == unknown_account);
  REQUIRE_FALSE(unknown.subject()->firm_id().has_value());
  REQUIRE_FALSE(unknown.subject()->instrument().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Local account factories fail closed when exact sealed account/venue authority is absent.
TEST_CASE("M4 configured provenance rejects missing or contradictory account authority",
          "[m4][provenance]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& resolver = fixture.resolver();
  const auto unknown_account =
      test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>("account.not-configured");
  const auto wrong_venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("other");

  const auto missing =
      resolver.create_configured_account_provenance(unknown_account, fixture.venue_id());
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().context.field == "m4_provenance.logical_account_id");

  const auto contradictory =
      resolver.create_configured_account_provenance(fixture.account_id(), wrong_venue);
  REQUIRE_FALSE(contradictory);
  REQUIRE(contradictory.error().context.field == "m4_provenance.venue_id");

  const auto unsupported = resolver.create_configured_instrument_provenance(
      fixture.account_id(), fixture.venue_id(),
      test_support::parse_m4_identifier_or_throw<model::InstrumentId>("ETH-USD"));
  REQUIRE_FALSE(unsupported);
  REQUIRE(unsupported.error().context.field == "m4_provenance.instrument_id");
}

// --------------------------------------------------------

// --------------------------------------------------------
// Resolver creation rejects a valid configuration that does not match the sealed M4 root.
TEST_CASE("M4 provenance resolver rejects a different sealed configuration", "[m4][provenance]") {
  const auto authority = test_support::create_m4_test_authority_or_throw();
  auto other =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  REQUIRE(other);
  REQUIRE(other.value().fingerprint() != authority.configuration.fingerprint());

  const auto rejected = runtime::M4ProvenanceResolver::create(other.value(), authority.m4_policy);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == model::DomainErrorCode::InvalidPrivateEvent);
  REQUIRE(rejected.error().context.field == "m4_provenance.configuration_fingerprint");
}

// --------------------------------------------------------

// --------------------------------------------------------
// Resolver state remains valid after every caller-owned startup object used to build it disappears.
TEST_CASE("M4 provenance resolver owns its complete normalization authority", "[m4][provenance]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy a sealed configuration into the resolver and destroy the policy-bearing fixture scope.
  auto resolver = [] {
    auto authority = test_support::create_m4_test_authority_or_throw();
    auto created =
        runtime::M4ProvenanceResolver::create(authority.configuration, authority.m4_policy);
    if (!created) {
      throw std::logic_error{"invalid owning M4 resolver fixture"};
    }
    return std::move(created).value();
  }();

  // ++++++++++++++++++++++++++++++++++++++++
  // Later source derivation uses only the resolver-owned immutable configuration projection.
  const auto account = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
      "account.deribit-testnet-aegis");
  const auto venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("deribit");
  const auto instrument =
      test_support::parse_m4_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL");
  const auto provenance =
      resolver.create_configured_instrument_provenance(account, venue, instrument);
  REQUIRE(provenance);
  REQUIRE(provenance.value().subject()->firm_id() ==
          test_support::parse_m4_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
  REQUIRE(provenance.value().subject()->instrument().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
