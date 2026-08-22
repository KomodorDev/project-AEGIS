// Purpose: validate complete M3 risk authority and encode its deterministic positional AEGISRSP
// schema without allowing stale provenance, ambiguous keys, or incomplete route coverage.

#include "aegis/risk/risk_policy.hpp"

#include "aegis/model/domain_error.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::risk {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// ########################################################################
// RiskLimitKey is a startup-only semantic projection used to compare authored rows with the exact
// seven scopes derivable from enabled owner-local routes.
struct RiskLimitKey {
  model::FirmId firm_id;
  RiskScopeKind scope;
  std::string scope_subject;
  model::InstrumentId instrument_id;
  std::string quote_currency;

  // --------------------------------------------------------
  friend bool operator==(const RiskLimitKey&, const RiskLimitKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Map every construction rejection onto ADR-0008's single persisted policy error code.
template <typename Value> [[nodiscard]] model::Result<Value> invalid_policy(std::string field) {
  return model::Result<Value>::failure(
      DomainError::at_field(DomainErrorCode::InvalidRiskPolicy, std::move(field)));
}

// --------------------------------------------------------
// Compare complete semantic keys in their canonical AEGISRSP field order.
[[nodiscard]] bool key_less(const RiskLimitKey& left, const RiskLimitKey& right) noexcept {
  return std::tie(left.firm_id, left.scope, left.scope_subject, left.instrument_id,
                  left.quote_currency) < std::tie(right.firm_id, right.scope, right.scope_subject,
                                                  right.instrument_id, right.quote_currency);
}

// --------------------------------------------------------
// Project an immutable limit row into the same canonical key representation.
[[nodiscard]] RiskLimitKey key_of(const RiskLimitSet& limits) {
  return RiskLimitKey{limits.firm_id(), limits.scope(), std::string{limits.scope_subject()},
                      limits.instrument_id(), std::string{limits.quote_currency()}};
}

// --------------------------------------------------------
// Compare installed metadata revision keys independently from authored collection order.
[[nodiscard]] bool metadata_less(const configuration::InstrumentMetadataRevisionEntry& left,
                                 const configuration::InstrumentMetadataRevisionEntry& right) {
  return std::tie(left.venue_id, left.instrument_id) <
         std::tie(right.venue_id, right.instrument_id);
}

// --------------------------------------------------------
// Compare every economic and venue-filter field against the sealed startup metadata projection.
[[nodiscard]] bool metadata_equal(const model::InstrumentMetadata& left,
                                  const model::InstrumentMetadata& right) noexcept {
  return left.venue_id() == right.venue_id() && left.instrument_id() == right.instrument_id() &&
         left.venue_instrument_id() == right.venue_instrument_id() &&
         left.revision() == right.revision() && left.base_currency() == right.base_currency() &&
         left.quote_currency() == right.quote_currency() &&
         left.settlement_currency() == right.settlement_currency() &&
         left.contract_style() == right.contract_style() &&
         left.quantity_unit() == right.quantity_unit() &&
         left.contract_multiplier_unit() == right.contract_multiplier_unit() &&
         left.price_scale() == right.price_scale() &&
         left.quantity_scale() == right.quantity_scale() && left.tick_size() == right.tick_size() &&
         left.quantity_step() == right.quantity_step() &&
         left.minimum_quantity() == right.minimum_quantity() &&
         left.contract_multiplier() == right.contract_multiplier();
}

// --------------------------------------------------------
// Resolve the exact scope-subject spelling from sealed route and attribution authority.
[[nodiscard]] std::string_view scope_subject(const execution::InstalledSubmissionRoute& installed,
                                             RiskScopeKind scope) noexcept {
  switch (scope) {
  case RiskScopeKind::Bot:
    return installed.attribution().bot_id.value();
  case RiskScopeKind::Desk:
    return installed.attribution().desk_id.value();
  case RiskScopeKind::Firm:
    return installed.attribution().firm_id.value();
  case RiskScopeKind::Account:
    return installed.route().logical_account_id.value();
  case RiskScopeKind::Route:
    return installed.route().id.value();
  case RiskScopeKind::Instrument:
    return installed.metadata().instrument_id().value();
  case RiskScopeKind::Venue:
    return installed.metadata().venue_id().value();
  default:
    return {};
  }
}

// --------------------------------------------------------
// Append one unsigned integer most-significant byte first, independent of host representation.
template <typename Unsigned> void append_unsigned(std::vector<std::byte>& bytes, Unsigned value) {
  static_assert(std::is_unsigned_v<Unsigned>);
  for (std::size_t offset = 0U; offset < sizeof(Unsigned); ++offset) {
    const auto shift = static_cast<unsigned int>((sizeof(Unsigned) - 1U - offset) * 8U);
    bytes.push_back(static_cast<std::byte>((value >> shift) & static_cast<Unsigned>(0xffU)));
  }
}

// --------------------------------------------------------
// Append accepted ASCII behind its schema-one unsigned 16-bit byte length.
void append_string(std::vector<std::byte>& bytes, std::string_view value) {
  append_unsigned(bytes, static_cast<std::uint16_t>(value.size()));
  const auto characters = std::as_bytes(std::span{value.data(), value.size()});
  bytes.insert(bytes.end(), characters.begin(), characters.end());
}

// --------------------------------------------------------
// Encode one canonical signed-decimal tuple without exposing native signed representation.
template <typename Decimal> void append_decimal(std::vector<std::byte>& bytes, Decimal value) {
  append_unsigned(bytes, std::bit_cast<std::uint64_t>(value.coefficient()));
  append_unsigned(bytes, value.scale());
}

// --------------------------------------------------------
// Encode the exact positional AEGISRSP schema whose SHA-256 becomes policy identity.
[[nodiscard]] std::vector<std::byte>
encode_risk_policy(const RiskPolicyParams& params, std::uint8_t notional_scale,
                   const std::vector<configuration::InstrumentMetadataRevisionEntry>& metadata,
                   const std::vector<RiskLimitSet>& limits) {
  std::vector<std::byte> bytes;
  constexpr std::string_view magic{"AEGISRSP"};
  const auto magic_bytes = std::as_bytes(std::span{magic.data(), magic.size()});
  bytes.insert(bytes.end(), magic_bytes.begin(), magic_bytes.end());
  append_unsigned(bytes, canonical_risk_policy_schema_version);
  append_unsigned(bytes, params.revision.value());
  bytes.insert(bytes.end(), params.configuration_fingerprint.bytes().begin(),
               params.configuration_fingerprint.bytes().end());
  append_unsigned(bytes, params.configuration_revision.value());
  append_unsigned(bytes, params.organization_revision.value());
  append_unsigned(bytes, params.route_revision.value());
  append_unsigned(bytes, notional_scale);
  append_unsigned(bytes, static_cast<std::uint8_t>(params.notional_rounding));

  append_unsigned(bytes, static_cast<std::uint32_t>(metadata.size()));
  for (const auto& entry : metadata) {
    append_string(bytes, entry.venue_id.value());
    append_string(bytes, entry.instrument_id.value());
    append_unsigned(bytes, entry.revision.value());
  }

  append_unsigned(bytes, static_cast<std::uint32_t>(limits.size()));
  for (const auto& row : limits) {
    append_string(bytes, row.firm_id().value());
    append_unsigned(bytes, static_cast<std::uint8_t>(row.scope()));
    append_string(bytes, row.scope_subject());
    append_string(bytes, row.instrument_id().value());
    append_string(bytes, row.quote_currency());
    append_decimal(bytes, row.maximum_single_order_quantity());
    append_decimal(bytes, row.maximum_single_order_quote_notional());
    append_unsigned(bytes, row.maximum_open_order_count());
    append_decimal(bytes, row.maximum_gross_reserved_quote_notional());
    append_decimal(bytes, row.maximum_worst_case_position_quantity());
    append_decimal(bytes, row.maximum_worst_case_position_quote_notional());
  }
  return bytes;
}

// --------------------------------------------------------
// Return whether a scope byte is one of the seven assigned M3 values.
[[nodiscard]] bool assigned_scope(RiskScopeKind scope) noexcept {
  return scope >= RiskScopeKind::Bot && scope <= RiskScopeKind::Venue;
}

// --------------------------------------------------------
// Require every decimal limit to be strictly positive before it can authorize exposure.
[[nodiscard]] bool positive_limits(const RiskLimitSetParams& limits) noexcept {
  return limits.maximum_single_order_quantity.coefficient() > 0 &&
         limits.maximum_single_order_quote_notional.coefficient() > 0 &&
         limits.maximum_open_order_count > 0U &&
         limits.maximum_gross_reserved_quote_notional.coefficient() > 0 &&
         limits.maximum_worst_case_position_quantity.coefficient() > 0 &&
         limits.maximum_worst_case_position_quote_notional.coefficient() > 0;
}

// --------------------------------------------------------
// Enforce shared mutable-cell limits independent of authored row ordering.
[[nodiscard]] bool shared_key_limits_agree(const std::vector<RiskLimitSet>& limits) noexcept {
  for (std::size_t left_index = 0U; left_index < limits.size(); ++left_index) {
    const auto& left = limits[left_index];
    for (std::size_t right_index = left_index + 1U; right_index < limits.size(); ++right_index) {
      const auto& right = limits[right_index];
      const bool same_count = left.firm_id() == right.firm_id() && left.scope() == right.scope() &&
                              left.scope_subject() == right.scope_subject();
      if (!same_count) {
        continue;
      }
      if (left.maximum_open_order_count() != right.maximum_open_order_count()) {
        return false;
      }

      const bool same_quantity = left.instrument_id() == right.instrument_id();
      if (same_quantity &&
          (left.maximum_single_order_quantity() != right.maximum_single_order_quantity() ||
           left.maximum_worst_case_position_quantity() !=
               right.maximum_worst_case_position_quantity())) {
        return false;
      }

      const bool same_notional = left.quote_currency() == right.quote_currency();
      if (same_notional && (left.maximum_single_order_quote_notional() !=
                                right.maximum_single_order_quote_notional() ||
                            left.maximum_gross_reserved_quote_notional() !=
                                right.maximum_gross_reserved_quote_notional() ||
                            left.maximum_worst_case_position_quote_notional() !=
                                right.maximum_worst_case_position_quote_notional())) {
        return false;
      }
    }
  }
  return true;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Render exactly 64 lowercase hexadecimal digits for the same fixed digest bytes.
std::string RiskPolicyFingerprint::to_hex() const {
  const auto encoded = model::sha256_hex(bytes_);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Validate all startup authority before publishing any canonical policy or mutable risk capability.
model::Result<RiskPolicySnapshot>
RiskPolicySnapshot::create(RiskPolicyParams params,
                           const configuration::StartupConfiguration& authority,
                           const execution::OwnerLocalRouteCatalog& routes) {
  const auto& provenance = authority.provenance();

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 1: reject stale top-level provenance and any unassigned arithmetic policy.
  if (params.configuration_fingerprint != provenance.fingerprint() ||
      params.configuration_revision != provenance.configuration_revision() ||
      params.organization_revision != provenance.organization_revision() ||
      params.route_revision != provenance.route_revision()) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.provenance");
  }
  if (params.notional_scale > model::FixedPoint::maximum_scale ||
      params.notional_rounding != model::RoundingMode::AwayFromZero) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.notional_decision");
  }
  if (params.metadata_revisions.size() > std::numeric_limits<std::uint32_t>::max() ||
      params.limit_sets.size() > std::numeric_limits<std::uint32_t>::max()) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.collection_size");
  }
  const auto notional_scale = static_cast<std::uint8_t>(params.notional_scale);

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 2: canonicalize metadata references and reject duplicate, stale, or unknown revisions.
  auto metadata = std::move(params.metadata_revisions);
  std::sort(metadata.begin(), metadata.end(), metadata_less);
  for (std::size_t index = 0U; index < metadata.size(); ++index) {
    if (index > 0U && !metadata_less(metadata[index - 1U], metadata[index]) &&
        !metadata_less(metadata[index], metadata[index - 1U])) {
      return invalid_policy<RiskPolicySnapshot>("risk_policy.metadata_duplicate");
    }
    const auto* const accepted = provenance.find_instrument_metadata_revision(
        metadata[index].venue_id, metadata[index].instrument_id);
    if (accepted == nullptr || accepted->value() != metadata[index].revision.value()) {
      return invalid_policy<RiskPolicySnapshot>("risk_policy.metadata_revision");
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 3: prove the installed catalog is the complete sealed route/attribution/metadata
  // projection, then derive the metadata and seven-scope keys needed by enabled routes.
  std::vector<configuration::InstrumentMetadataRevisionEntry> expected_metadata;
  std::vector<RiskLimitKey> expected_keys;
  if (routes.routes().size() != authority.routes().routes().size()) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.route_completeness");
  }
  for (const auto& installed : routes.routes()) {
    const auto* const accepted_route = authority.routes().find(installed.route().id);
    const auto* const accepted_attribution =
        authority.organization().find_bot(installed.route().bot_id);
    const auto* const accepted_metadata = authority.find_instrument_metadata(
        installed.route().venue_id, installed.route().instrument_id);
    if (installed.configuration_fingerprint() != provenance.fingerprint() ||
        installed.configuration_revision() != provenance.configuration_revision() ||
        installed.organization_revision() != provenance.organization_revision() ||
        installed.route_revision() != provenance.route_revision() || accepted_route == nullptr ||
        *accepted_route != installed.route() || accepted_attribution == nullptr ||
        *accepted_attribution != installed.attribution() || accepted_metadata == nullptr ||
        !metadata_equal(*accepted_metadata, installed.metadata())) {
      return invalid_policy<RiskPolicySnapshot>("risk_policy.route_provenance");
    }
    if (!installed.route().is_enabled()) {
      continue;
    }
    expected_metadata.push_back(configuration::InstrumentMetadataRevisionEntry{
        installed.metadata().venue_id(), installed.metadata().instrument_id(),
        installed.metadata().revision()});
    for (std::uint8_t value = static_cast<std::uint8_t>(RiskScopeKind::Bot);
         value <= static_cast<std::uint8_t>(RiskScopeKind::Venue); ++value) {
      const auto scope = static_cast<RiskScopeKind>(value);
      expected_keys.push_back(RiskLimitKey{installed.attribution().firm_id, scope,
                                           std::string{scope_subject(installed, scope)},
                                           installed.metadata().instrument_id(),
                                           std::string{installed.metadata().quote_currency()}});
    }
  }
  std::sort(expected_metadata.begin(), expected_metadata.end(), metadata_less);
  expected_metadata.erase(std::unique(expected_metadata.begin(), expected_metadata.end()),
                          expected_metadata.end());
  std::sort(expected_keys.begin(), expected_keys.end(), key_less);
  expected_keys.erase(std::unique(expected_keys.begin(), expected_keys.end()), expected_keys.end());
  if (metadata != expected_metadata) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.metadata_completeness");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 4: validate and narrow every positive authored row before canonical key sorting.
  std::vector<RiskLimitSet> limits;
  limits.reserve(params.limit_sets.size());
  for (auto& authored : params.limit_sets) {
    if (!assigned_scope(authored.scope) || authored.scope_subject.empty() ||
        authored.scope_subject.size() > std::numeric_limits<std::uint16_t>::max() ||
        authored.quote_currency.empty() ||
        authored.quote_currency.size() > std::numeric_limits<std::uint16_t>::max() ||
        authored.maximum_open_order_count > std::numeric_limits<std::uint32_t>::max() ||
        !positive_limits(authored)) {
      return invalid_policy<RiskPolicySnapshot>("risk_policy.limit_value");
    }
    const auto maximum_open_order_count =
        static_cast<std::uint32_t>(authored.maximum_open_order_count);
    limits.push_back(RiskLimitSet{std::move(authored), maximum_open_order_count});
  }
  std::sort(limits.begin(), limits.end(), [](const RiskLimitSet& left, const RiskLimitSet& right) {
    return key_less(key_of(left), key_of(right));
  });

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 5: require exact route coverage, no extra semantic key, and no ordering-selected limits.
  std::vector<RiskLimitKey> authored_keys;
  authored_keys.reserve(limits.size());
  for (const auto& row : limits) {
    authored_keys.push_back(key_of(row));
  }
  for (std::size_t index = 1U; index < authored_keys.size(); ++index) {
    if (authored_keys[index - 1U] == authored_keys[index]) {
      return invalid_policy<RiskPolicySnapshot>("risk_policy.limit_duplicate");
    }
  }
  if (authored_keys != expected_keys) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.limit_completeness");
  }
  if (!shared_key_limits_agree(limits)) {
    return invalid_policy<RiskPolicySnapshot>("risk_policy.limit_consistency");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 6: encode the sorted positional artifact once and fingerprint exactly those bytes.
  params.metadata_revisions.clear();
  params.limit_sets.clear();
  auto encoded = encode_risk_policy(params, notional_scale, metadata, limits);
  RiskPolicyFingerprint fingerprint{model::sha256(encoded)};
  return model::Result<RiskPolicySnapshot>::success(RiskPolicySnapshot{
      params.revision, std::move(params.configuration_fingerprint), params.configuration_revision,
      params.organization_revision, params.route_revision, notional_scale, params.notional_rounding,
      std::move(metadata), std::move(limits), std::move(encoded), std::move(fingerprint)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolve one canonical complete key without allocating or consulting configuration state.
const RiskLimitSet* RiskPolicySnapshot::find_limit_set(
    const model::FirmId& firm_id, RiskScopeKind scope, std::string_view scope_subject_value,
    const model::InstrumentId& instrument_id, std::string_view quote_currency) const noexcept {
  const auto key = std::tuple{firm_id.value(), scope, scope_subject_value, instrument_id.value(),
                              quote_currency};
  const auto found = std::lower_bound(
      limit_sets_.begin(), limit_sets_.end(), key, [](const RiskLimitSet& row, const auto& target) {
        return std::tuple{row.firm_id().value(), row.scope(), row.scope_subject(),
                          row.instrument_id().value(), row.quote_currency()} < target;
      });
  if (found == limit_sets_.end() || found->firm_id() != firm_id || found->scope() != scope ||
      found->scope_subject() != scope_subject_value || found->instrument_id() != instrument_id ||
      found->quote_currency() != quote_currency) {
    return nullptr;
  }
  return &*found;
}

// --------------------------------------------------------

} // namespace aegis::risk
