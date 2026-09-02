// Purpose: cross-validate M1-M3 policy authority, validate every M4 capacity relationship, and
// produce exact positional AEGISM4P bytes plus their SHA-256 identity.

#include "aegis/runtime/m4_policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::runtime {
namespace {

// ########################################################################
// Local aliases keep stable error construction readable without exporting implementation names.
using model::DomainError;
using model::DomainErrorCode;

// ########################################################################
// Validation metadata names all 26 fields without defining their canonical encoding order.
struct M4PolicyCapacityField {
  std::uint64_t M4PolicyCapacities::*member;
  std::string_view name;
};

// ########################################################################
// This internal projection exists only after the public factory cross-validates sealed authorities.
struct M4PolicyCanonicalInputs {
  model::Sha256Digest configuration_fingerprint;
  model::OrganizationRevision organization_revision;
  model::Sha256Digest runtime_policy_fingerprint;
  model::RiskPolicyRevision risk_policy_revision;
  model::Sha256Digest risk_policy_fingerprint;
  model::Sha256Digest submission_policy_fingerprint;
};

// ########################################################################
// Counts derived from sealed M1-M3 objects define generic lower bounds without fixture authorship.
struct M4PolicyCapacityRequirements {
  std::uint64_t logical_account_count;
  std::uint64_t outbound_oms_capacity;
  std::uint64_t reservation_capacity;
  std::uint64_t inventory_aggregate_cell_count;
};

// ########################################################################

// --------------------------------------------------------
// Fix scalar validation precedence independently of the manually written canonical encoder order.
constexpr std::array<M4PolicyCapacityField, 26U> capacity_fields{{
    {&M4PolicyCapacities::max_private_admissions, "max_private_admissions"},
    {&M4PolicyCapacities::max_reconciliation_admissions, "max_reconciliation_admissions"},
    {&M4PolicyCapacities::max_account_safety_fences, "max_account_safety_fences"},
    {&M4PolicyCapacities::max_private_event_records, "max_private_event_records"},
    {&M4PolicyCapacities::max_event_identity_records, "max_event_identity_records"},
    {&M4PolicyCapacities::max_trade_identity_records, "max_trade_identity_records"},
    {&M4PolicyCapacities::max_exchange_order_mappings, "max_exchange_order_mappings"},
    {&M4PolicyCapacities::max_pending_fill_intervals_per_order,
     "max_pending_fill_intervals_per_order"},
    {&M4PolicyCapacities::max_cancel_attempts, "max_cancel_attempts"},
    {&M4PolicyCapacities::max_inventory_source_rows, "max_inventory_source_rows"},
    {&M4PolicyCapacities::max_inventory_aggregate_cells, "max_inventory_aggregate_cells"},
    {&M4PolicyCapacities::max_unattributed_exposure_rows, "max_unattributed_exposure_rows"},
    {&M4PolicyCapacities::max_account_safety_records, "max_account_safety_records"},
    {&M4PolicyCapacities::max_transition_effects_per_turn, "max_transition_effects_per_turn"},
    {&M4PolicyCapacities::max_order_callbacks_per_turn, "max_order_callbacks_per_turn"},
    {&M4PolicyCapacities::max_private_diagnostics, "max_private_diagnostics"},
    {&M4PolicyCapacities::max_private_audit_records, "max_private_audit_records"},
    {&M4PolicyCapacities::max_journal_records, "max_journal_records"},
    {&M4PolicyCapacities::max_snapshot_records, "max_snapshot_records"},
    {&M4PolicyCapacities::max_reconciliation_batches, "max_reconciliation_batches"},
    {&M4PolicyCapacities::max_reconciliation_rows_per_batch, "max_reconciliation_rows_per_batch"},
    {&M4PolicyCapacities::max_live_catchup_facts, "max_live_catchup_facts"},
    {&M4PolicyCapacities::max_recovery_epochs, "max_recovery_epochs"},
    {&M4PolicyCapacities::max_namespace_registrations, "max_namespace_registrations"},
    {&M4PolicyCapacities::max_recovery_notifications, "max_recovery_notifications"},
    {&M4PolicyCapacities::max_reference_intents, "max_reference_intents"},
}};

// --------------------------------------------------------
// Return one stable combined-policy failure without exposing authored numeric values as text.
[[nodiscard]] model::Result<void>
create_m4_policy_validation_failure_from_field(std::string_view field) {
  return model::Result<void>::create_failure(DomainError::create_at_field(
      DomainErrorCode::InvalidM4Policy, std::string{"m4_policy."} + std::string{field}));
}

// --------------------------------------------------------
// Report whether multiplication can produce one u32-representable result without wrapping.
[[nodiscard]] bool is_product_u32_representable(std::uint64_t left, std::uint64_t right) noexcept {
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  return left == 0U || (right <= maximum / left && left * right <= maximum);
}

// --------------------------------------------------------
// Validate every individual bound followed by the accepted deterministic cross-capacity order.
[[nodiscard]] model::Result<void>
validate_capacities(const M4PolicyCapacities& capacities,
                    const M4PolicyCapacityRequirements& requirements) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject absent or non-u32 values in the exact authored field order.
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  for (const auto& field : capacity_fields) {
    const auto value = capacities.*(field.member);
    if (value == 0U || value > maximum) {
      return create_m4_policy_validation_failure_from_field(std::string{"capacities."} +
                                                            std::string{field.name});
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove fixed owners can represent the complete already-sealed M1-M3 authority surface.
  if (capacities.max_account_safety_fences < requirements.logical_account_count) {
    return create_m4_policy_validation_failure_from_field("capacities.max_account_safety_fences");
  }
  if (capacities.max_exchange_order_mappings < requirements.outbound_oms_capacity) {
    return create_m4_policy_validation_failure_from_field("capacities.max_exchange_order_mappings");
  }
  if (capacities.max_inventory_source_rows < requirements.reservation_capacity) {
    return create_m4_policy_validation_failure_from_field("capacities.max_inventory_source_rows");
  }
  if (capacities.max_inventory_aggregate_cells < requirements.inventory_aggregate_cell_count) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_inventory_aggregate_cells");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A fresh registered namespace is required in addition to every recoverable epoch.
  if (capacities.max_recovery_epochs == maximum ||
      capacities.max_namespace_registrations < capacities.max_recovery_epochs + 1U) {
    return create_m4_policy_validation_failure_from_field("capacities.max_namespace_registrations");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // One new fill plus every retained gap must fit one atomic transition/callback/audit plan.
  if (capacities.max_pending_fill_intervals_per_order == maximum) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_pending_fill_intervals_per_order");
  }
  const auto drain_width = capacities.max_pending_fill_intervals_per_order + 1U;
  if (capacities.max_transition_effects_per_turn < drain_width) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_transition_effects_per_turn");
  }
  if (capacities.max_order_callbacks_per_turn < drain_width) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_order_callbacks_per_turn");
  }
  if (drain_width > maximum - 2U || capacities.max_private_audit_records < drain_width + 2U) {
    return create_m4_policy_validation_failure_from_field("capacities.max_private_audit_records");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Retained primary rows each need one full-capacity effect buffer. Callback-bearing spans reserve
  // at least three physical slots, so at most floor(audit slots / 3) retained Planned rows need
  // reference buffers.
  if (!is_product_u32_representable(capacities.max_private_audit_records,
                                    capacities.max_transition_effects_per_turn)) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_transition_effects_per_turn");
  }
  const auto maximum_planned_callback_rows = capacities.max_private_audit_records / 3U;
  if (!is_product_u32_representable(maximum_planned_callback_rows,
                                    capacities.max_order_callbacks_per_turn)) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_order_callbacks_per_turn");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The two reconciliation/recovery worst-case vectors also use u32 aggregate element counts.
  if (!is_product_u32_representable(capacities.max_reconciliation_batches,
                                    capacities.max_reconciliation_rows_per_batch)) {
    return create_m4_policy_validation_failure_from_field(
        "capacities.max_reconciliation_rows_per_batch");
  }
  if (!is_product_u32_representable(capacities.max_recovery_epochs,
                                    capacities.max_recovery_notifications)) {
    return create_m4_policy_validation_failure_from_field("capacities.max_recovery_notifications");
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// ########################################################################
// This fixed writer implements only the positional AEGISM4P schema-one primitives.
class CanonicalM4PolicyWriter final {
public:

  // --------------------------------------------------------
  // Preallocate the one exact schema size so successful encoding performs no growth surprises.
  CanonicalM4PolicyWriter() { bytes_.reserve(canonical_m4_policy_byte_size); }

  // --------------------------------------------------------
  // Append schema magic as exact ASCII bytes without locale conversion.
  void append_ascii(std::string_view value) {
    for (const char character : value) {
      bytes_.push_back(std::byte{static_cast<unsigned char>(character)});
    }
  }

  // --------------------------------------------------------
  // Append one schema version in unsigned big-endian order.
  void append_u16(std::uint16_t value) {
    bytes_.push_back(std::byte{static_cast<std::uint8_t>((value >> 8U) & 0xffU)});
    bytes_.push_back(std::byte{static_cast<std::uint8_t>(value & 0xffU)});
  }

  // --------------------------------------------------------
  // Append one capacity or revision in unsigned big-endian order.
  // Interesting syntax: the explicit zero check ends the unsigned countdown before subtraction
  // could wrap from zero to the type's maximum value.
  void append_u64(std::uint64_t value) {
    for (unsigned int shift = 56U;; shift -= 8U) {
      bytes_.push_back(std::byte{static_cast<std::uint8_t>((value >> shift) & 0xffU)});
      if (shift == 0U) {
        break;
      }
    }
  }

  // --------------------------------------------------------
  // Append a SHA-256 fingerprint at its accepted fixed 32-byte width.
  void append_digest(const model::Sha256Digest& digest) {
    bytes_.insert(bytes_.end(), digest.begin(), digest.end());
  }

  // --------------------------------------------------------
  // Transfer the completed byte vector only after the caller has appended every field.
  [[nodiscard]] std::vector<std::byte> take_canonical_bytes() && { return std::move(bytes_); }

  // --------------------------------------------------------
  // Hide mutable writer storage from policy callers.
private:
  // The vector is reserved to the exact schema size before any append.
  std::vector<std::byte> bytes_;
};

// ########################################################################

// --------------------------------------------------------
// Encode the exact positional policy after validation has fixed every field and relationship.
[[nodiscard]] model::Result<std::vector<std::byte>>
encode_policy(const M4PolicyCanonicalInputs& inputs, const M4PolicyCapacities& capacities) {
  CanonicalM4PolicyWriter writer;
  writer.append_ascii("AEGISM4P");
  writer.append_u16(canonical_m4_policy_schema_version);
  writer.append_digest(inputs.configuration_fingerprint);
  writer.append_u64(inputs.organization_revision.value());
  writer.append_digest(inputs.runtime_policy_fingerprint);
  writer.append_u64(inputs.risk_policy_revision.value());
  writer.append_digest(inputs.risk_policy_fingerprint);
  writer.append_digest(inputs.submission_policy_fingerprint);
  writer.append_u64(capacities.max_private_admissions);
  writer.append_u64(capacities.max_reconciliation_admissions);
  writer.append_u64(capacities.max_account_safety_fences);
  writer.append_u64(capacities.max_private_event_records);
  writer.append_u64(capacities.max_event_identity_records);
  writer.append_u64(capacities.max_trade_identity_records);
  writer.append_u64(capacities.max_exchange_order_mappings);
  writer.append_u64(capacities.max_pending_fill_intervals_per_order);
  writer.append_u64(capacities.max_cancel_attempts);
  writer.append_u64(capacities.max_inventory_source_rows);
  writer.append_u64(capacities.max_inventory_aggregate_cells);
  writer.append_u64(capacities.max_unattributed_exposure_rows);
  writer.append_u64(capacities.max_account_safety_records);
  writer.append_u64(capacities.max_transition_effects_per_turn);
  writer.append_u64(capacities.max_order_callbacks_per_turn);
  writer.append_u64(capacities.max_private_diagnostics);
  writer.append_u64(capacities.max_private_audit_records);
  writer.append_u64(capacities.max_journal_records);
  writer.append_u64(capacities.max_snapshot_records);
  writer.append_u64(capacities.max_reconciliation_batches);
  writer.append_u64(capacities.max_reconciliation_rows_per_batch);
  writer.append_u64(capacities.max_live_catchup_facts);
  writer.append_u64(capacities.max_recovery_epochs);
  writer.append_u64(capacities.max_namespace_registrations);
  writer.append_u64(capacities.max_recovery_notifications);
  writer.append_u64(capacities.max_reference_intents);
  auto bytes = std::move(writer).take_canonical_bytes();
  if (bytes.size() != canonical_m4_policy_byte_size) {
    return model::Result<std::vector<std::byte>>::create_failure(
        DomainError::create_at_field(DomainErrorCode::EncodingOverflow, "m4_policy"));
  }
  return model::Result<std::vector<std::byte>>::create_success(std::move(bytes));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Render the already-computed digest without changing its identity or hashing it again.
std::string M4PolicyFingerprint::to_hex() const {
  const auto hex = model::sha256_hex_from_digest(bytes_);
  return std::string{hex.begin(), hex.end()};
}

// --------------------------------------------------------
// Derive the complete authority projection and fixed-owner requirements from validated policies.
model::Result<M4Policy> M4Policy::create_m4_policy(
    const configuration::StartupConfiguration& configuration, const RuntimePolicy& runtime_policy,
    const risk::RiskPolicySnapshot& risk_policy,
    const execution::SubmissionPolicy& submission_policy, M4PolicyCapacities capacities) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject a disconnected M1-M3 policy chain before any M4 capacity or byte derivation.
  const auto& configuration_fingerprint = configuration.fingerprint().bytes();
  if (runtime_policy.configuration_fingerprint().bytes() != configuration_fingerprint) {
    return model::Result<M4Policy>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidM4Policy, "m4_policy.runtime_policy_fingerprint"));
  }
  if (risk_policy.configuration_fingerprint().bytes() != configuration_fingerprint ||
      risk_policy.organization_revision() != configuration.organization().revision()) {
    return model::Result<M4Policy>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidM4Policy, "m4_policy.risk_policy_fingerprint"));
  }
  if (submission_policy.configuration_fingerprint() != configuration_fingerprint ||
      submission_policy.runtime_policy_fingerprint() != runtime_policy.fingerprint().bytes() ||
      submission_policy.risk_policy_fingerprint() != risk_policy.fingerprint().bytes() ||
      submission_policy.risk_policy_revision() != risk_policy.revision()) {
    return model::Result<M4Policy>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidM4Policy, "m4_policy.submission_policy_fingerprint"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive encoded authority and capacity requirements only from the validated object graph.
  M4PolicyCanonicalInputs inputs{
      configuration_fingerprint,
      configuration.organization().revision(),
      runtime_policy.fingerprint().bytes(),
      risk_policy.revision(),
      risk_policy.fingerprint().bytes(),
      submission_policy.fingerprint().bytes(),
  };
  M4PolicyCapacityRequirements requirements{
      static_cast<std::uint64_t>(configuration.logical_accounts().size()),
      static_cast<std::uint64_t>(submission_policy.capacities().oms_order_capacity),
      static_cast<std::uint64_t>(submission_policy.capacities().reservation_capacity),
      static_cast<std::uint64_t>(risk_policy.limit_sets().size()),
  };
  const auto validation = validate_capacities(capacities, requirements);
  if (!validation) {
    return model::Result<M4Policy>::create_failure(validation.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive bytes, digest, and root only after both authority and capacity validation succeed.
  auto canonical_bytes = encode_policy(inputs, capacities);
  if (!canonical_bytes) {
    return model::Result<M4Policy>::create_failure(canonical_bytes.error());
  }
  M4PolicyFingerprint fingerprint{model::calculate_sha256_digest(canonical_bytes.value())};
  model::M4RootProvenance root{inputs.configuration_fingerprint,
                               inputs.organization_revision,
                               inputs.runtime_policy_fingerprint,
                               inputs.risk_policy_revision,
                               inputs.risk_policy_fingerprint,
                               inputs.submission_policy_fingerprint,
                               fingerprint.bytes()};
  return model::Result<M4Policy>::create_success(M4Policy{
      capacities, std::move(root), std::move(canonical_bytes).value(), std::move(fingerprint)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
