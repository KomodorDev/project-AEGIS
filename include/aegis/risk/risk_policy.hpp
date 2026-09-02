// Purpose: define and seal the immutable schema-one M3 risk policy, including canonical
// provenance, complete seven-scope limits, and its AEGISRSP fingerprint.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/execution/submission_route.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"
#include "aegis/risk/risk_scope.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::risk {

inline constexpr std::uint16_t canonical_risk_policy_schema_version = 1U;

// ########################################################################
// RiskPolicyFingerprint names the exact AEGISRSP bytes; display hexadecimal is not a second hash.
class RiskPolicyFingerprint final {
public:

  // --------------------------------------------------------
  // Own one already-derived fixed-width risk-policy digest.
  explicit RiskPolicyFingerprint(model::Sha256Digest bytes) noexcept : bytes_{std::move(bytes)} {}

  // --------------------------------------------------------
  // Borrow the fixed-width binary identity of the canonical policy bytes.
  [[nodiscard]] const model::Sha256Digest& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Render the same digest as exactly 64 lowercase hexadecimal characters.
  [[nodiscard]] std::string to_hex() const;

  // --------------------------------------------------------
  // Structural equality compares the complete canonical policy digest.
  friend bool operator==(const RiskPolicyFingerprint&, const RiskPolicyFingerprint&) = default;

  // --------------------------------------------------------
private:
  model::Sha256Digest bytes_;
};

// ########################################################################
// Authoring parameters retain a wide count and heterogeneous scope-subject spelling until the
// complete policy can validate them against sealed owner-local routes.
struct RiskLimitSetParams {
  model::FirmId firm_id;
  RiskScopeKind scope;
  std::string scope_subject;
  model::InstrumentId instrument_id;
  std::string quote_currency;
  model::Quantity maximum_single_order_quantity;
  model::Notional maximum_single_order_quote_notional;
  std::uint64_t maximum_open_order_count;
  model::Notional maximum_gross_reserved_quote_notional;
  model::Quantity maximum_worst_case_position_quantity;
  model::Notional maximum_worst_case_position_quote_notional;
};

// ########################################################################
// RiskLimitSet is one immutable complete semantic key with all six positive M3 limits.
class RiskLimitSet final {
public:

  // --------------------------------------------------------
  // Borrow the authoritative firm component of this complete semantic key.
  [[nodiscard]] const model::FirmId& firm_id() const noexcept { return firm_id_; }

  // --------------------------------------------------------
  // Return the canonical scope kind selected by this limit row.
  [[nodiscard]] RiskScopeKind scope() const noexcept { return scope_; }

  // --------------------------------------------------------
  // Borrow the validated subject spelling required by the selected scope kind.
  [[nodiscard]] std::string_view scope_subject() const noexcept { return scope_subject_; }

  // --------------------------------------------------------
  // Borrow the normalized instrument component shared by all six limits.
  [[nodiscard]] const model::InstrumentId& instrument_id() const noexcept { return instrument_id_; }

  // --------------------------------------------------------
  // Borrow the validated quote-currency component used by notional limits.
  [[nodiscard]] std::string_view quote_currency() const noexcept { return quote_currency_; }

  // --------------------------------------------------------
  // Return the positive per-order quantity ceiling.
  [[nodiscard]] model::Quantity maximum_single_order_quantity() const noexcept {
    return maximum_single_order_quantity_;
  }

  // --------------------------------------------------------
  // Return the positive once-rounded quote-notional ceiling for one order.
  [[nodiscard]] model::Notional maximum_single_order_quote_notional() const noexcept {
    return maximum_single_order_quote_notional_;
  }

  // --------------------------------------------------------
  // Return the positive narrowed count ceiling for open reservations.
  [[nodiscard]] std::uint32_t maximum_open_order_count() const noexcept {
    return maximum_open_order_count_;
  }

  // --------------------------------------------------------
  // Return the positive aggregate reserved quote-notional ceiling.
  [[nodiscard]] model::Notional maximum_gross_reserved_quote_notional() const noexcept {
    return maximum_gross_reserved_quote_notional_;
  }

  // --------------------------------------------------------
  // Return the positive directional worst-case position-quantity ceiling.
  [[nodiscard]] model::Quantity maximum_worst_case_position_quantity() const noexcept {
    return maximum_worst_case_position_quantity_;
  }

  // --------------------------------------------------------
  // Return the positive directional worst-case position-notional ceiling.
  [[nodiscard]] model::Notional maximum_worst_case_position_quote_notional() const noexcept {
    return maximum_worst_case_position_quote_notional_;
  }

  // --------------------------------------------------------
  // Structural equality compares the complete semantic key and all six validated limits.
  friend bool operator==(const RiskLimitSet&, const RiskLimitSet&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the complete policy validator may narrow and publish an authored limit row.
  friend class RiskPolicySnapshot;

  // ########################################################################

  // --------------------------------------------------------
  // Publish one fully validated row with its count already narrowed to implementation width.
  explicit RiskLimitSet(RiskLimitSetParams params, std::uint32_t maximum_open_order_count)
      : firm_id_{std::move(params.firm_id)}, scope_{params.scope},
        scope_subject_{std::move(params.scope_subject)},
        instrument_id_{std::move(params.instrument_id)},
        quote_currency_{std::move(params.quote_currency)},
        maximum_single_order_quantity_{params.maximum_single_order_quantity},
        maximum_single_order_quote_notional_{params.maximum_single_order_quote_notional},
        maximum_open_order_count_{maximum_open_order_count},
        maximum_gross_reserved_quote_notional_{params.maximum_gross_reserved_quote_notional},
        maximum_worst_case_position_quantity_{params.maximum_worst_case_position_quantity},
        maximum_worst_case_position_quote_notional_{
            params.maximum_worst_case_position_quote_notional} {}

  // --------------------------------------------------------
  model::FirmId firm_id_;
  RiskScopeKind scope_;
  std::string scope_subject_;
  model::InstrumentId instrument_id_;
  std::string quote_currency_;
  model::Quantity maximum_single_order_quantity_;
  model::Notional maximum_single_order_quote_notional_;
  std::uint32_t maximum_open_order_count_;
  model::Notional maximum_gross_reserved_quote_notional_;
  model::Quantity maximum_worst_case_position_quantity_;
  model::Notional maximum_worst_case_position_quote_notional_;
};

// ########################################################################
// RiskPolicyParams is the complete authored immutable snapshot before provenance and completeness
// checks bind it to the sealed startup authority.
struct RiskPolicyParams {
  model::RiskPolicyRevision revision;
  configuration::ConfigurationFingerprint configuration_fingerprint;
  model::ConfigurationRevision configuration_revision;
  model::OrganizationRevision organization_revision;
  model::RouteRevision route_revision;
  std::uint64_t notional_scale;
  model::RoundingMode notional_rounding;
  std::vector<configuration::InstrumentMetadataRevisionEntry> metadata_revisions;
  std::vector<RiskLimitSetParams> limit_sets;
};

// ########################################################################
// RiskPolicySnapshot validates once, sorts by semantic key, and exposes immutable direct-path
// lookups without consulting StartupConfiguration.
class RiskPolicySnapshot final {
public:

  // --------------------------------------------------------
  // Fail closed unless provenance, metadata, scope subjects, completeness, and shared-key limits
  // exactly match the sealed startup authority and every enabled submission route.
  [[nodiscard]] static model::Result<RiskPolicySnapshot>
  create_risk_policy_snapshot(RiskPolicyParams params,
                              const configuration::StartupConfiguration& authority,
                              const execution::OwnerLocalRouteCatalog& routes);

  // --------------------------------------------------------
  // Return the immutable risk-policy revision encoded into this snapshot.
  [[nodiscard]] model::RiskPolicyRevision revision() const noexcept { return revision_; }

  // --------------------------------------------------------
  // Borrow the sealed startup-configuration identity validated during creation.
  [[nodiscard]] const configuration::ConfigurationFingerprint&
  configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Return the exact startup-configuration revision bound into policy provenance.
  [[nodiscard]] model::ConfigurationRevision configuration_revision() const noexcept {
    return configuration_revision_;
  }

  // --------------------------------------------------------
  // Return the exact organization revision bound into policy provenance.
  [[nodiscard]] model::OrganizationRevision organization_revision() const noexcept {
    return organization_revision_;
  }

  // --------------------------------------------------------
  // Return the exact route revision against which complete scope coverage was checked.
  [[nodiscard]] model::RouteRevision route_revision() const noexcept { return route_revision_; }

  // --------------------------------------------------------
  // Return the single quote-notional scale used by all policy calculations.
  [[nodiscard]] std::uint8_t notional_scale() const noexcept { return notional_scale_; }

  // --------------------------------------------------------
  // Return the required rounding mode for every quote-notional calculation.
  [[nodiscard]] model::RoundingMode notional_rounding() const noexcept {
    return notional_rounding_;
  }

  // --------------------------------------------------------
  // Borrow metadata revisions in canonical normalized-instrument order.
  [[nodiscard]] const std::vector<configuration::InstrumentMetadataRevisionEntry>&
  metadata_revisions() const noexcept {
    return metadata_revisions_;
  }

  // --------------------------------------------------------
  // Borrow validated limit rows in canonical complete-key order.
  [[nodiscard]] const std::vector<RiskLimitSet>& limit_sets() const noexcept { return limit_sets_; }

  // --------------------------------------------------------
  // Borrow the exact schema-one bytes from which the policy fingerprint was derived.
  [[nodiscard]] const std::vector<std::byte>& canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

  // --------------------------------------------------------
  // Borrow the SHA-256 identity of the complete canonical policy.
  [[nodiscard]] const RiskPolicyFingerprint& fingerprint() const noexcept { return fingerprint_; }

  // --------------------------------------------------------
  // Resolve one complete semantic key through canonical binary search.
  [[nodiscard]] const RiskLimitSet* find_limit_set(const model::FirmId& firm_id,
                                                   RiskScopeKind scope,
                                                   std::string_view scope_subject,
                                                   const model::InstrumentId& instrument_id,
                                                   std::string_view quote_currency) const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only mutually validated provenance, canonical rows, bytes, and fingerprint.
  RiskPolicySnapshot(model::RiskPolicyRevision revision,
                     configuration::ConfigurationFingerprint configuration_fingerprint,
                     model::ConfigurationRevision configuration_revision,
                     model::OrganizationRevision organization_revision,
                     model::RouteRevision route_revision, std::uint8_t notional_scale,
                     model::RoundingMode notional_rounding,
                     std::vector<configuration::InstrumentMetadataRevisionEntry> metadata_revisions,
                     std::vector<RiskLimitSet> limit_sets, std::vector<std::byte> canonical_bytes,
                     RiskPolicyFingerprint fingerprint)
      : revision_{revision}, configuration_fingerprint_{std::move(configuration_fingerprint)},
        configuration_revision_{configuration_revision},
        organization_revision_{organization_revision}, route_revision_{route_revision},
        notional_scale_{notional_scale}, notional_rounding_{notional_rounding},
        metadata_revisions_{std::move(metadata_revisions)}, limit_sets_{std::move(limit_sets)},
        canonical_bytes_{std::move(canonical_bytes)}, fingerprint_{std::move(fingerprint)} {}

  // --------------------------------------------------------
  model::RiskPolicyRevision revision_;
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  model::ConfigurationRevision configuration_revision_;
  model::OrganizationRevision organization_revision_;
  model::RouteRevision route_revision_;
  std::uint8_t notional_scale_;
  model::RoundingMode notional_rounding_;
  std::vector<configuration::InstrumentMetadataRevisionEntry> metadata_revisions_;
  std::vector<RiskLimitSet> limit_sets_;
  std::vector<std::byte> canonical_bytes_;
  RiskPolicyFingerprint fingerprint_;
};

// ########################################################################

} // namespace aegis::risk
