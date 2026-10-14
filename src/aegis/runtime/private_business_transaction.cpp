// Purpose: jointly retain genuine initial business plans and bounded journal/evidence reservations,
// then reject foreign or stale preparations without publishing or applying their proposed effects.

#include "private_business_transaction.hpp"

#include "submission_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Report one precise transaction-preparation failure while preserving the existing error domains.
[[nodiscard]] model::DomainError create_business_transaction_error(
    const char* field,
    model::DomainErrorCode code = model::DomainErrorCode::InvalidPrivateOmsState) {
  return model::DomainError::create_at_field(code, field);
}

// --------------------------------------------------------
// Resolve the genuine configured account by its complete source binding, without inventing a gate.
[[nodiscard]] const PrivateAccountContainment*
find_transaction_account(const PrivateIdentityRetentionState& retention,
                         model::LogicalAccountId account_id, model::VenueId venue_id) noexcept {
  const auto found = std::find_if(retention.accounts.begin(), retention.accounts.end(),
                                  [account_id, venue_id](const PrivateAccountContainment& account) {
                                    return account.binding.logical_account_id == account_id &&
                                           account.binding.venue_id == venue_id;
                                  });
  return found == retention.accounts.end() ? nullptr : &*found;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Transfer the complete optional group and remove every inspection path through the source.
PreparedInitialKnownPrivateBusinessTransaction::PreparedInitialKnownPrivateBusinessTransaction(
    PreparedInitialKnownPrivateBusinessTransaction&& other) noexcept
    : state_{std::move(other.state_)} {
  other.state_.reset();
}

// --------------------------------------------------------
// Release this value's previous unpublished reservations before taking the complete source group.
PreparedInitialKnownPrivateBusinessTransaction&
PreparedInitialKnownPrivateBusinessTransaction::operator=(
    PreparedInitialKnownPrivateBusinessTransaction&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
    other.state_.reset();
  }
  return *this;
}

// --------------------------------------------------------
// Destroy the retained plans and backing leases without appending any business or evidence row.
PreparedInitialKnownPrivateBusinessTransaction::~PreparedInitialKnownPrivateBusinessTransaction() =
    default;

// --------------------------------------------------------
// Seal one complete group after all component preparation has succeeded without allocation.
PreparedInitialKnownPrivateBusinessTransaction::PreparedInitialKnownPrivateBusinessTransaction(
    FirstSeenAuthoritativePrivateIdentityPlan identity,
    InitialKnownPrivateBusinessProposal business, PreparedPrivateBusinessEvidence evidence,
    recovery::PreparedPrivateEventJournalInput journal, OwnerSnapshot owner) noexcept
    : state_{std::in_place, State{std::move(identity), std::move(business), std::move(evidence),
                                  std::move(journal), std::move(owner)}} {
  static_assert(std::is_nothrow_move_constructible_v<State>);
  static_assert(std::is_nothrow_move_assignable_v<State>);
}

// --------------------------------------------------------
// A moved-from value cannot inspect a later scratch reuse or validate any preparation.
bool PreparedInitialKnownPrivateBusinessTransaction::has_preparation() const noexcept {
  return state_.has_value();
}

// --------------------------------------------------------
// Borrow immutable correlation candidates only while this value owns its complete group.
const FirstSeenAuthoritativePrivateIdentityPlan*
PreparedInitialKnownPrivateBusinessTransaction::identity_plan() const noexcept {
  return state_ ? &state_->identity : nullptr;
}

// --------------------------------------------------------
// Preserve read-only economic scratch for the lifetime of this complete preparation.
const InitialKnownPrivateBusinessProposal*
PreparedInitialKnownPrivateBusinessTransaction::business_proposal() const noexcept {
  return state_ ? &state_->business : nullptr;
}

// --------------------------------------------------------
// Borrow complete prospective evidence without exposing its publication or cleanup rights.
const PreparedPrivateBusinessEvidence*
PreparedInitialKnownPrivateBusinessTransaction::business_evidence() const noexcept {
  return state_ ? &state_->evidence : nullptr;
}

// --------------------------------------------------------
// Borrow the real reserved record; copying it cannot acquire its exclusive journal-slot lease.
const recovery::JournalRecord*
PreparedInitialKnownPrivateBusinessTransaction::journal_record() const noexcept {
  return state_ ? state_->journal.record() : nullptr;
}

// --------------------------------------------------------
// Acquire genuine component plans, reserve the actual journal position, copy linked evidence, and
// retain all leases together. Every earlier successful acquisition is abandoned on later failure.
model::Result<PreparedInitialKnownPrivateBusinessTransaction>
PrivateOrderReconciler::prepare_initial_known_authoritative_business_transaction(
    const oms::NormalizedPrivateOrderInput& input,
    recovery::JournalReplayProvenance replay_provenance,
    std::optional<model::CallbackOrdinal> first_callback_ordinal,
    std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal) const {
  using Transaction = PreparedInitialKnownPrivateBusinessTransaction;
  using PreparationResult = model::Result<Transaction>;

  // ++++++++++++++++++++++++++++++++++++++++
  // Protect the existing detached evidence prefix before attempting to lease economic scratch.
  if (business_evidence_store_->has_outstanding_preparation()) {
    return PreparationResult::create_failure(
        create_business_transaction_error("private_business_transaction.preparation_busy",
                                          model::DomainErrorCode::PrivateEvidenceExhausted));
  }
  if (owner_->is_runtime_faulted() || retention_.globally_blocked ||
      retention_.emergency_turn_published.load(std::memory_order_acquire)) {
    return PreparationResult::create_failure(
        create_business_transaction_error("private_business_transaction.global_containment",
                                          model::DomainErrorCode::ExecutionNotPermitted));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Obtain complete initial owner-derived identity, lifecycle, economics, and audit requirements.
  auto business = derive_initial_known_authoritative_business_proposal(
      input, business_evidence_store_->next_audit_ordinal());
  if (!business) {
    return PreparationResult::create_failure(std::move(business).error());
  }
  auto identity = derive_first_seen_authoritative_identity_plan(
      oms::PrivateEventIngressSemanticValue::from_normalized_input(input));
  if (!identity) {
    return PreparationResult::create_failure(std::move(identity).error());
  }
  const auto* known = business.value().first_admission_resolution().known_resolution();
  const auto* order = owner_->outbound_oms().find_order(known->order_id);
  const auto* inventory = risk::InventoryLedger::installed_inventory(owner_->reservations());
  const auto* account =
      find_transaction_account(retention_, known->provenance.subject()->logical_account_id(),
                               known->provenance.subject()->venue_id());
  if (order == nullptr || inventory == nullptr || account == nullptr || !business_incarnation_ ||
      inventory->planning_generation() == std::numeric_limits<std::uint64_t>::max()) {
    return PreparationResult::create_failure(
        create_business_transaction_error("private_business_transaction.owner_baseline"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preflight each canonical registry's exact required insertion while preserving suppression of
  // candidate mappings on contained or forbidden transitions. These tables still remain empty.
  if (event_identity_record_count_ >= event_identity_records_.size()) {
    return PreparationResult::create_failure(
        create_business_transaction_error("private_business_transaction.event_capacity",
                                          model::DomainErrorCode::PrivateEventCapacityExceeded));
  }
  if (std::holds_alternative<FirstSeenPrivateTradeIdentityPlan>(identity.value().trade_plan()) &&
      trade_identity_record_count_ >= trade_identity_records_.size()) {
    return PreparationResult::create_failure(
        create_business_transaction_error("private_business_transaction.trade_capacity",
                                          model::DomainErrorCode::PrivateEventCapacityExceeded));
  }
  const auto& correlation =
      std::get<KnownFirstSeenPrivateCorrelationPlan>(identity.value().correlation_plan());
  const auto classification = business.value().oms_transition().classification;
  if (correlation.candidate_mapping &&
      classification != oms::ProposedPrivateOmsClassification::SafetyContained &&
      classification != oms::ProposedPrivateOmsClassification::ForbiddenRejected &&
      exchange_order_mapping_count_ >= exchange_order_mappings_.size()) {
    return PreparationResult::create_failure(
        create_business_transaction_error("private_business_transaction.mapping_capacity",
                                          model::DomainErrorCode::PrivateEventCapacityExceeded));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reserve the real medium's next position before copying evidence, so every linkage names its
  // actual sequence. The closed recovery component constructs the payload from unchanged facts.
  auto journal = recovery::PreparedPrivateEventJournalInput::reserve_private_event_journal_input(
      recovery_lease_, runtime_epoch_id_, input, business.value().first_admission_resolution(),
      replay_provenance, business.value().proposed_audit_span());
  if (!journal) {
    return PreparationResult::create_failure(std::move(journal).error());
  }
  auto evidence = prepare_initial_business_evidence_from_proposal(
      business.value(), identity.value(), replay_provenance, first_callback_ordinal,
      journal.value().record()->sequence(), diagnostic_ordinal);
  if (!evidence) {
    return PreparationResult::create_failure(std::move(evidence).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal complete fixed baselines without exposing independent mutation rights for any component.
  Transaction::OwnerSnapshot snapshot{
      business_incarnation_,
      inventory->planning_generation(),
      order->admission(),
      account->state,
      account->cause_count,
      account->first_quarantine_cause,
      account->fenced_attempt_count,
      retention_.globally_blocked,
      retention_.global_fence,
      event_identity_record_count_,
      trade_identity_record_count_,
      exchange_order_mapping_count_,
      retention_.published_turn_count.load(std::memory_order_acquire),
      retention_.emergency_turn_published.load(std::memory_order_acquire),
      retention_.preparations.event_record_count(),
      retention_.preparations.trade_record_count(),
      retention_.preparations.mapping_candidate_count()};
  return PreparationResult::create_success(
      Transaction{std::move(identity).value(), std::move(business).value(),
                  std::move(evidence).value(), std::move(journal).value(), std::move(snapshot)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate lifetime identity first, then unchanged live state and every linked immutable record.
// This check performs no mutation and never attempts to repair or refresh a stale preparation.
model::Result<void> PrivateOrderReconciler::validate_prepared_initial_known_business_transaction(
    const PreparedInitialKnownPrivateBusinessTransaction& transaction) const {
  using ValidationResult = model::Result<void>;

  // ++++++++++++++++++++++++++++++++++++++++
  // Compare opaque incarnation identity before consulting saved row or reservation identities.
  if (!transaction.state_ || !business_incarnation_ ||
      transaction.state_->owner.incarnation != business_incarnation_) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.owner"));
  }
  const auto& prepared = *transaction.state_;
  const auto& baseline = prepared.owner;
  const auto& business = prepared.business;
  const auto* known = business.first_admission_resolution().known_resolution();
  if (known == nullptr || known->provenance.subject() == std::nullopt) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.resolution"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Re-resolve genuine live storage; changed OMS or any economic owner mutation rejects every
  // preparation, including projection-only proposals that carry no individual economic plan.
  const auto* order = owner_->outbound_oms().find_order(known->order_id);
  const auto* inventory = risk::InventoryLedger::installed_inventory(owner_->reservations());
  const auto* reservation =
      owner_->reservations().find_reservation(baseline.order_admission.reservation_id);
  if (owner_->is_runtime_faulted() || order == nullptr ||
      !owner_->outbound_oms().has_retained_order_record(*order) ||
      order->admission() != baseline.order_admission ||
      order->private_projection() != business.oms_transition().before || inventory == nullptr ||
      inventory->planning_generation() != baseline.inventory_generation ||
      inventory->planning_generation() == std::numeric_limits<std::uint64_t>::max() ||
      reservation == nullptr || *reservation != business.reservation_before()) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.component_baseline",
                                          model::DomainErrorCode::InvalidReservationConversion));
  }
  if (const auto* economics = business.economics_plan()) {
    auto validation = inventory->validate_reservation_inventory_plan(*economics);
    if (!validation) {
      return validation;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve append-only identity/preparation history and every submission-containment baseline.
  const auto* account =
      find_transaction_account(retention_, known->provenance.subject()->logical_account_id(),
                               known->provenance.subject()->venue_id());
  if (account == nullptr || account->state != baseline.account_state ||
      account->cause_count != baseline.account_cause_count ||
      account->first_quarantine_cause != baseline.first_quarantine_cause ||
      account->fenced_attempt_count != baseline.fenced_attempt_count ||
      retention_.globally_blocked != baseline.globally_blocked ||
      retention_.global_fence != baseline.global_fence ||
      event_identity_record_count_ != baseline.event_count ||
      trade_identity_record_count_ != baseline.trade_count ||
      exchange_order_mapping_count_ != baseline.mapping_count ||
      retention_.published_turn_count.load(std::memory_order_acquire) !=
          baseline.retained_turn_count ||
      retention_.emergency_turn_published.load(std::memory_order_acquire) !=
          baseline.emergency_turn_published ||
      retention_.preparations.event_record_count() != baseline.prepared_event_count ||
      retention_.preparations.trade_record_count() != baseline.prepared_trade_count ||
      retention_.preparations.mapping_candidate_count() != baseline.prepared_mapping_count) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.containment_baseline"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the exclusive real-medium reservation independently before trusting its record.
  auto journal_validation = prepared.journal.validate_reservation();
  if (!journal_validation) {
    return journal_validation;
  }
  const auto* record = prepared.journal.record();
  const auto* event = prepared.evidence.private_event_evidence();
  const auto* payload = prepared.evidence.journal_payload();
  const auto* span = prepared.evidence.audit_span();
  if (!prepared.evidence.has_preparation() ||
      !business_evidence_store_->has_outstanding_preparation() || record == nullptr ||
      event == nullptr || payload == nullptr || span == nullptr ||
      *span != business.proposed_audit_span() ||
      prepared.evidence.primary_audit_record_count() !=
          business.evidence_requirements().primary_audit_record_count ||
      prepared.evidence.order_callback_count() !=
          business.evidence_requirements().order_callback_count ||
      event->input() != business.input() ||
      event->ingress_semantic_value() != prepared.identity.ingress_semantic_value() ||
      event->first_admission_resolution() != business.first_admission_resolution() ||
      payload->input() != business.input() ||
      payload->first_admission_resolution() != business.first_admission_resolution() ||
      record->lineage_id() != recovery_lineage_id_ ||
      record->runtime_epoch_id() != std::optional{runtime_epoch_id_} ||
      record->root_provenance() != m4_policy_.root_provenance() ||
      record->subject_provenance() != known->provenance.subject() ||
      record->kind() != recovery::JournalRecordKind::PrivateEventInput ||
      record->audit_span() != std::optional{*span} ||
      record->replay_provenance() != std::optional{event->replay_provenance()} ||
      event->journal_sequence() != std::optional{record->sequence()}) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.evidence_linkage"));
  }
  const auto* journal_payload =
      std::get_if<recovery::PrivateEventInputJournalPayload>(&record->payload());
  if (journal_payload == nullptr || *journal_payload != *payload) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.journal_payload"));
  }
  for (std::uint32_t index = 0U; index < prepared.evidence.primary_audit_record_count(); ++index) {
    const auto* primary = prepared.evidence.primary_audit_record_at(index);
    if (primary == nullptr || primary->journal_sequence() != std::optional{record->sequence()}) {
      return ValidationResult::create_failure(
          create_business_transaction_error("private_business_transaction.primary_linkage"));
    }
  }
  const auto* planned = prepared.evidence.planned_callback_record();
  if ((planned != nullptr) != (prepared.evidence.order_callback_count() != 0U) ||
      (planned != nullptr && planned->journal_sequence() != std::optional{record->sequence()})) {
    return ValidationResult::create_failure(
        create_business_transaction_error("private_business_transaction.callback_linkage"));
  }
  return ValidationResult::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
